# Vulkan Pipeline Cache — Key/Prewarm Read-Only Inventory (M7-B0 prep, rev2)

Owner: Orca Worker (read-only doc revision per Lead review; zero code changes).
Line numbers live in the current workspace. Facts vs suggestions vs inference
explicitly separated. Vulkan driver opaque `pipelineCacheUUID` is a driver
cache-file identity input (VKPipelineCachePersistence.h:62) and is NOT a PSO
content hash.

## 1. Six key categories per plan §15.1/15.3 — current state

### 1a. PipelineState / PipelineStateInfo

- Engine-level CPU cache **EXISTS**: `PipelineStateManager::psoHashMap`
  (native/cocos/renderer/pipeline/PipelineStateManager.cpp:33) is an
  `unordered_map<hash_t, IntrusivePtr<PipelineState>>`; key is
  `pass->getHash() ^ renderPass->getHash() ^ ia->getAttributesHash() ^
  shader->getTypedID()`, left-shifted by non-zero subpass
  (PipelineStateManager.cpp:34-38).
- `Pass::getPassHash()` (cocos/scene/Pass.cpp:134-154) hashes: primitive,
  dynamicStates, serialized blend/depth-stencil/rasterizer states, phaseID and
  shaderKey (via ProgramLib::getKey(program, defines)). It does NOT include
  subpass, renderPass hash, or layout — subpass is only in the left-shift.
  Whether the missing subpass full-tuple can collide (e.g. two subpass
  indices hashing to the same shifted value) is an **inference** — a
  synthetic mathematical counterexample is possible but no real-scenario bug
  has been reproduced.
- hash-only map: no full-tuple equality check beyond the XOR key (fact);
  hash collision would return a wrong PSO (code risk, inference).
- TS side: `cocos/rendering/pipeline-state-manager.ts` has `_PSOHashMap`
  with the same four-way XOR key pattern (fact, same design).

### 1b. PipelineLayout

- NO content hash in the engine (fact). `cmdFuncCCVKCreatePipelineLayout`
  (VKCommands.cpp:982) maps layouts on each call; the Vulkan driver's opaque
  pipeline cache is the only optimization. **Inference**: whether this causes
  redundant creates or correctness issues cannot be verified from C++ code.

### 1c. DescriptorSetLayout

- NO content hash; the engine uses monotonically-increasing `layoutID`
  (VKGPUObjects.h:710 `getDescriptorSetPool(layoutID)`) for pool lookups
  (fact). This is an identity, not a content key (fact).

### 1d. RenderingSignature

- EXISTS, content-based, deterministic: `VKRenderingSignature::hash()`
  (VKRenderingSignature.h:66-75) — FNV-1a over all 5 fields; 11 CPU contract
  tests validate (fact).

### 1e. Classic RenderPass compatibility key

- Two hash paths exist (fact):
  1. Member: `RenderPass::computeHash()` (GFXRenderPass.cpp:39-55) —
     `hash_combine` over serialized `ColorAttachment`, `DepthStencilAttachment`,
     `DepthStencilResolveAttachment`, and `_subpasses` via
     `hash_combine(seed, ca)` (GFXDef.cpp operator() overloads).
  2. Static: `RenderPass::computeHash(const RenderPassInfo&)` (GFXRenderPass.cpp:57)
     delegates to `Hasher<RenderPassInfo>` (GFXDef.cpp:107-115), which combines
     attachments, subpasses and dependencies. Attachment hash functions use
     `quickHashTrivialStruct` (GFXDef.cpp:38-58), including barrier addresses;
     the helper checks that its input struct has no implicit padding.
- **Neither path is a stable cross-run semantic compatibility key**:
  the member hash includes barrier pointer hashes (pointer values vary per
  run); attachment hashing in both paths includes pointer values.
  The current hash serves the process-local CPU cache only (fact); it
  cannot directly serve as a persistent pipeline-cache compat key.
- Vulkan classic render-pass compatibility requires compatible attachment
  formats and sample counts; load/store operations and layouts do not need
  to be identical. [Official compatibility rules](https://docs.vulkan.org/spec/latest/chapters/renderpass.html#renderpass-compatibility);
  the current hash is stricter than required for compatibility.

### 1f. Dynamic compatibility

- M6 candidate `vkIsDynamicRenderPassCandidate` (VKCommands.cpp:1021-1102)
  + `pipelineMetadata.signature` (set at VKCommands.cpp:1066) already encode
  formats/samples/viewMask + narrow RP shape. Per the Vulkan spec,
  `VkGraphicsPipelineCreateInfo` with `renderPass=NULL` and a
  `VkPipelineRenderingCreateInfo` pNext uses attachment formats/viewMask,
  together with multisample state, for the current narrow rendering interface
  (this is not a complete PSO content key; shader, layout, fixed and dynamic
  states also matter). Official spec:
  [VkGraphicsPipelineCreateInfo](https://docs.vulkan.org/refpages/latest/refpages/source/VkGraphicsPipelineCreateInfo.html).
  **RP topology (subpass count, dependencies) is NOT a default missing item**
  (Lead correction). Extending
  the key for future features (FSR/localRead/secondary) is a scope boundary,
  not a current deficiency.

## 2. Serial prewarm entry path (code facts)

```
Device::createPipelineState(PipelineStateInfo)   [GFXDevice virtual]
  → CCVKPipelineState::doInit(info)               [VKPipelineState.cpp]
    → cmdFuncCCVKCreateGraphicsPipelineState      [VKCommands.cpp:1020]
      → dereferences Shader*/PipelineLayout*/RenderPass* raw pointers
      → M5-H metadata copy; M6-A2a optional Dynamic variant creation
      → vkCreateGraphicsPipelines(pipelineCache->getHandle())
    → GPU PSO stored (vkPipeline/vkDynamicPipeline)
  → caller receives PipelineState facade
```
Synchronous; the Vulkan pipeline cache may serve the create internally
(opaque driver behavior, not a CPU-level dedup).

## 3. Lifetime constraint (corrected per Lead)

- `Shader`, `PipelineLayout`, `RenderPass`, `PipelineState` facades inherit
  both `GFXObject` (typedID) and `RefCounted` (reference counting). The
  ConstPtr/IntrusivePtr GPU objects (`_gpuPipelineState`,
  `_gpuRenderPass`, etc.) are ref-counted independently.
- Raw facade inputs must remain alive **until the synchronous
  `createPipelineState` call returns** (the factory dereferences them);
  after that, the GPU PSO is held by IntrusivePtr inside
  `CCVKGPUPipelineState` and the recording lease (`_gpuCommandBuffer->retain`)
  protects it until GPU completion. **Inference withdrawn**: the earlier claim
  that "the pipeline cache has internal staging requiring input facades until
  submit" had no evidence and is retracted.

## 4. Minimal known-PSO-manifest prewarm experiment (suggestion, not implementation)

1. Harness owns all input facades + value infos (or scopes them); serially
   calls `createPipelineState` for each known PSO → real GPU pipelines are
   created and stored in the engine PSO facades.
2. **Assertions must be scoped**: `createdCount` and `destroyed==1` per handle
   can only be asserted when the Dynamic path is explicitly ON, the RP is a
   verified candidate, and the variant succeeded. Classic-only or compute
   pipelines must not hard-assert 4.
3. Prewarm itself requires NO GPU submit (pipeline creation is host-side);
   to validate draw contents, add a bounded content test separately.
4. Call `pipelineCache->saveCache()` explicitly (not `checkpoint()` which is
   gated by the 30-second timer and requires `persistent=true`, which is
   default OFF — the experiment must actually enable it via CMake ON tree or
   an explicit local wrapper with `persistent=true`).
5. Destroy all PSOs → assert each `destroyed==1`.
6. **Same-device PSO re-creation does NOT automatically reload the cache
   file** — a new device or cache instance is needed, or an independent
   process; assert `loadedFromDisk == true` and inspect the actual
   create-info payload (e.g. pipelineCache initialData size > 0) to prove
   recovery. Do NOT infer prewarm success from a reduced `coldCreates` count
   (the driver's opaque cache may or may not hit independently).
7. CPU exact-key dedup (PipelineStateManager) and driver opaque cache
   optimization are separate systems; do not conflate their evidence.

## 5. Summary table

| Item | Status | Reference |
|---|---|---|
| PipelineStateManager CPU hash | EXISTS (XOR of 4 keys, left-shift) | PipelineStateManager.cpp:34-38 |
| Pass::getPassHash | EXISTS (primitive/dyn/blend/DS/RS/shaderKey) | Pass.cpp:134-154 |
| RenderingSignature hash | EXISTS (FNV-1a 5 fields) | VKRenderingSignature.h:66 |
| Classic RP member hash | EXISTS (incl. barrier ptr) | GFXRenderPass.cpp:39-55 |
| Classic RP static hash | EXISTS (aggregates attachments/subpasses/dependencies; attachment hash includes ptr) | GFXDef.cpp:38-58,107-115 |
| PipelineLayout hash | MISSING | fact |
| DescriptorSetLayout hash | MISSING (layoutID only) | fact |
| Dynamic compat key | EXISTS (formats/samples/viewMask + narrow shape) | VKCommands.cpp:1021-1102 |
| Vulkan driver cache | EXISTS (M7-A, default OFF) | VKPipelineCache.h:41-71 |
| PSO manifest / prewarm queue | MISSING (needs design) | suggestion |
