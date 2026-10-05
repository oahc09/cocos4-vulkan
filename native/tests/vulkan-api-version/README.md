# Vulkan API version + capability contract tests

纯数据模型/契约测试（无引擎、无 loader、无渲染行为）：

- `version_tests.cpp`（Lead，M0-D）：Loader 查询失败传播、缺入口=1.0 loader、
  engine max 封顶 1.3、XR 约束、effective=min(instance, physical)
- `capability_tests.cpp`（Worker，M1-A）：计划第 4/5.4/17.2 节语义契约——
  supported/requested/enabled 三态独立；仅在「设备创建成功 && 进入 enable chain &&
  requested && supported」时 enabled；设备创建失败全量禁用；descriptor indexing
  家族逐成员独立；Sync2 未启用时 barrier/Submit2 双拒、启用后两者独立 A/B；
  forceClassicRenderPass 阻断 dynamic rendering；runtime path 请求永不抬高版本模型
- `feature_chain_tests.cpp`（Worker，M1-B）：`VKFeatureChain.h` 查询/启用链构建器契约——
  canonical tier 无重复结构（pNext-02829/02830/06532）；1.1 绝不挂 Vulkan11Features；
  1.3 core 能力不需扩展名；available-but-not-enabled 仅 supported；未请求/不支持位保持
  VK_FALSE（无 aggregate 整体拷贝）；descriptor/FSR 部分成员；创建失败清零；重建无旧
  pNext/位泄漏；properties 链独立；非拷贝/非移动 static_assert。**合成 feature 快照的
  结构/契约测试，非 GPU/Validation 证明**
- `wsi_state_tests.cpp`（Worker，M2-B）：`VKWSIState.h` 纯状态序列契约——六态转换、
  陈旧 index 拒绝、SUBOPTIMAL 延迟重建跨 present 存活、零尺寸暂停/恢复顺序、
  surface 重建→swapchain 重建顺序、TIMEOUT 非 fatal、DEVICE_LOST 不可清除、
  多实例独立、imageCount 缩容、非法 begin/complete/present 不制造 index、失败
  recreate 绝不 READY。**纯状态机测试：不拥有 Vk 对象、不证明 WSI 图像/信号量
  完成或释放，未接入引擎，非 WSI 压力/sem 安全证明**

## 运行（Windows）

```powershell
cmake -S native\tests\vulkan-api-version -B native\build\vulkan-modernization\version-tests -G "Visual Studio 18 2026" -A x64
cmake --build native\build\vulkan-modernization\version-tests --config Release
ctest --test-dir native\build\vulkan-modernization\version-tests -C Release --output-on-failure
# 预期 8/8 PASS（含 Submit1/Submit2 统一提交编码及 Barrier2 转换契约）
```

## Android 头兼容性（仅编译，不运行）

```powershell
cmake -S native\tests\vulkan-api-version -B native\build\vulkan-modernization\version-tests-android -G Ninja `
      "-DCMAKE_TOOLCHAIN_FILE=D:/Android/Sdk/ndk/26.1.10909125/build/cmake/android.toolchain.cmake" `
      "-DCMAKE_MAKE_PROGRAM=C:/ninja/ninja.exe" `
      "-DANDROID_ABI=arm64-v8a" "-DANDROID_PLATFORM=android-26" "-DANDROID_STL=c++_static" `
      "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
cmake --build native\build\vulkan-modernization\version-tests-android
```

## Completion tracker 契约（M3）

`completion_tracker_tests.cpp` 检查 fence 后端的成功/失败提交、真实透传内容、
累计完成、借用 fence 复用、跨 domain、等待超时和不可复活的 DEVICE_LOST。
`timeline_tracker_tests.cpp` 检查 binary 与 timeline 同次 submit、对齐的值数组、
原 pNext 保存、value-difference 限制、counter/wait 路由及独立 fence 占用。
两者均在 mock callback 内读取临时数组，夹具拥有被引用的对象。

`configureTimeline(semaphore, maxValueDifference)` 只接受专用初值为 0 的借用
timeline semaphore，必须在任何成功提交之前配置。调用方负责先启用 feature、
提供真实设备 limit，并在全部 GPU 工作完成后销毁 semaphore/fences/device。
`shutdown()` 仅清理 CPU bookkeeping，不调用 Vulkan，也不证明 GPU 工作完成。
统一模型显式标注 binary / timeline wait及signal值；binary value必须为0。
tracker自己的timeline模式拒绝已有legacy timeline pNext和专用semaphore出现在输入中。
timeline counter 完成不证明 fence signal 完成，也不证明 present 完成。

这些测试只证明 CPU 契约。Android 仅编译/链接，无设备运行；真实引擎接入与
Validation/WSI 证据见 `ai-docs/to-do/VULKAN_MODERNIZATION_PROGRESS.md`。

## 边界

M1-A/M1-B/M2-B 都是数据模型/链构建器/状态机与契约测试准备：不代表引擎已做 capability
协商或 WSI 状态机接入、不代表任何 runtime path 已接入渲染行为、不代表 M1/M2 或平台
Gate 通过。核心集成（真实 vkGetPhysicalDeviceFeatures2/vkCreateDevice/vkAcquireNextImageKHR
接线）由 Lead 负责。

## Submit1 / Submit2 编码契约（M4-A 首步）

`submit_batch_tests.cpp` 使用 owning `VulkanSubmitBatch` 与 mock PFN 验证两种编码的
wait stage、binary=0 / timeline value、command顺序、deviceIndex=0 / deviceMask=1。
signal统一使用ALL_COMMANDS，覆盖整个submission；不引入额外signal-only提交。

覆盖64位stage在legacy中不可表示的拒绝、未建模pNext拒绝、双completion后端、
PFN动态替换、失败不发point/不耗序号/不消费fence占用、DEVICE_LOST不可复活，
以及timeline counter完成不能证明借用fence完成。配置Submit2由调用方先保证
synchronization2真的enabled；单纯PFN非空不足以授权调用。

7/7是CPU契约结果，不是Barrier2迁移、驱动Validation、视觉或性能Gate。

## Barrier2 等价转换契约（M4-A）

`barrier_encoding_tests.cpp` 检查每个 barrier 保留 command-wide src/dst stage、
access、dependencyFlags、layout、buffer offset/size、queue family和image subresource。
TOP/BOTTOM与宽mask先保留，未缩窄范围。没有资源barrier时显式构造access=0的
memory barrier，保留legacy execution-only依赖，不能输出零个barrier丢掉该依赖。

编码器拥有输出数组，不拷贝/移动指针状态；原数组修改不影响已编码数据。
错误sType、count/空数组和未建模pNext拒绝，重建不保留旧结构。
CPU契约不能替代render-pass self dependency、split event、WSI和实际资源内容验证。

## CPU barrier encoding benchmark (M4-A4)

Standalone exe (not a ctest), CPU-only. Measures the fresh-construct cost of
the production `VulkanBarrierEncoding` (construct → build → consume → destruct)
versus a logical legacy data-walk reference over the same inputs.

```powershell
# build (in the existing version-tests tree)
cmake -S native\tests\vulkan-api-version -B native\build\vulkan-modernization\version-tests -G "Visual Studio 18 2026" -A x64
cmake --build native\build\vulkan-modernization\version-tests --config Release --target vulkan_barrier_encoding_benchmark
# run (1M iterations × 9 repeats × 3 scenarios × 2 paths)
& native\build\vulkan-modernization\version-tests\Release\vulkan_barrier_encoding_benchmark.exe native\build\vulkan-modernization\barrier-cpu.csv 1000000 9
# invalid inputs (0, "1junk", negative) are rejected with exit 2
```

### Evidence boundaries

- **CPU-only**: reference is a logical data traversal, NOT the old Vulkan
  driver/API cost. This isolates encoder conversion overhead only.
- **Warmup ≥10000** conversions per path (not timed); formal rounds alternate
  path order (odd: ref→enc, even: enc→ref) to reduce ordering bias.
- **Full hash**: both paths consume the complete logical field set (stages,
  accesses, layouts, queue families, image handles, subresource ranges,
  dependency flags). The three timed scenarios do not cover execution-only
  zero-count or buffer barriers; those remain covered by the separate contracts. Seven preflight checks mutate encoded output (memory dstStage/srcAccess;
  image dstStage/newLayout/dstQueueFamily/handle/levelCount), restore it after
  each change, and require the measured output hash to differ. Normal output
  must match the reference; formal round checksums are paired as well.
- **Raw CSV**: every round saved with actual execution order column; no slow
  round is dropped. steady_clock; ns_per_call = total / iterations.
- **NOT a game/performance gate**: results do not extrapolate to GPU frame
  time or real-game performance. No allocator attribution is claimed from
  these numbers alone.


## Barrier allocation contracts (M4-A5)

`VulkanBarrierEncoding` owns one inline node per barrier type. Counts above one
use an owned vector; no caller array is borrowed and the encoder cannot move or
copy. The execution-only case also uses an inline memory node with zero accesses
and the original stage masks. Every build starts with cleared views; invalid
input leaves zero counts/null pointers and a valid dependency-info sType.

```powershell
cmake --build native/build/vulkan-modernization/version-tests --config Release --target vulkan_barrier_allocation_tests
ctest --test-dir native/build/vulkan-modernization/version-tests -C Release --output-on-failure
```

The independent CPU test replaces new/delete only in its own process and enables
counting inside build scopes. Fresh memory1, buffer1, image1, mixed-single and
execution-only builds require zero heap allocations. Large batches exercise the
heap fallback and verify all three output arrays, ordering, ranges and ownership,
then rebuild through spill -> inline -> invalid -> execution-only recovery.
This changes CPU temporary storage; stage/access/layout scopes and submit count
remain unchanged. Allocation and CPU conversion results do not grant the WSI,
render-output or game-performance Gate. Android compile/link is separate from
running these contracts on a device.

## VKRenderingSignature pure value model (M5-B)

Standalone CPU contract tests (10th target, `vulkan_rendering_signature_tests`).
The signature is a pure-owning value struct with deterministic FNV-1a 64-bit
hash (RFC 9923 basis `0xcbf29ce484222325`, prime `0x100000001b3`). Each field
is mixed as 8 bytes little-endian in fixed order: count → colors → depth →
stencil → samples → viewMask. An explicit `VKRenderingSignatureHash` functor
(NOT a `std::hash` specialization) is provided for `unordered_map`; collision
resolution requires `operator==` on the key.

```powershell
# Windows configure + build + run all 10 contract tests
cmake -S native\tests\vulkan-api-version -B native\build\vulkan-modernization\version-tests -G "Visual Studio 18 2026" -A x64
cmake --build native\build\vulkan-modernization\version-tests --config Release
ctest --test-dir native\build\vulkan-modernization\version-tests -C Release --output-on-failure
# current suite: 11/11 PASS (includes M5-C)

# Android arm64 compile/link (no device run)
cmake -S native\tests\vulkan-api-version -B native\build\vulkan-modernization\version-tests-android -G Ninja `
      "-DCMAKE_TOOLCHAIN_FILE=D:/Android/Sdk/ndk/26.1.10909125/build/cmake/android.toolchain.cmake" `
      "-DCMAKE_MAKE_PROGRAM=C:/ninja/ninja.exe" `
      "-DANDROID_ABI=arm64-v8a" "-DANDROID_PLATFORM=android-26" "-DANDROID_STL=c++_static" `
      "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
cmake --build native\build\vulkan-modernization\version-tests-android
```

### Evidence boundaries

- Pure value model: no Vulkan objects, no loader, no engine dependencies.
- The hash is deterministic but NOT collision-free; equal hash does not
  imply semantic equality — always verify with `operator==`.
- The tuple equality is NOT a Vulkan compatibility check, device-support
  check, or pipeline-creatability assertion.
- Not connected to any runtime path, pipeline, or Dynamic Rendering.
- No persistent hash cache — hash is recomputed on every call. This tuple hash
  is not an on-disk pipeline-cache protocol or a device compatibility identity.

## VKRenderingContract offline pipeline/secondary model (M5-C)

Standalone CPU contract tests (11th target, `vulkan_rendering_contract_tests`).
Builds a `VKRenderingContract` from a `VKRenderingSignature` snapshot: the
class privately owns the signature vector and assembles three Vulkan struct
nodes (`VkPipelineRenderingCreateInfo`, `VkCommandBufferInheritanceRenderingInfo`,
`VkCommandBufferInheritanceInfo`) with correct sType/pNext chaining. No Vulkan
objects are created, no loader is called, no pipeline is actually built.

```powershell
# Windows configure + build + run all 11 contract tests
cmake -S native\tests\vulkan-api-version -B native\build\vulkan-modernization\version-tests -G "Visual Studio 18 2026" -A x64
cmake --build native\build\vulkan-modernization\version-tests --config Release
ctest --test-dir native\build\vulkan-modernization\version-tests -C Release --output-on-failure
# expected: 11/11 PASS

# Android arm64 compile/link (no device run)
cmake -S native\tests\vulkan-api-version -B native\build\vulkan-modernization\version-tests-android -G Ninja `
      "-DCMAKE_TOOLCHAIN_FILE=D:/Android/Sdk/ndk/26.1.10909125/build/cmake/android.toolchain.cmake" `
      "-DCMAKE_MAKE_PROGRAM=C:/ninja/ninja.exe" `
      "-DANDROID_ABI=arm64-v8a" "-DANDROID_PLATFORM=android-26" "-DANDROID_STL=c++_static" `
      "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
cmake --build native\build\vulkan-modernization\version-tests-android
```

### Evidence boundaries

- Offline data assembly only: no VkDevice, no VkPipeline, no vkBeginCommandBuffer.
- The contract does NOT prove a pipeline can be created or a device supports
  the formats — that requires a device adapter (future work).
- Secondary inheritance flags clear `SECONDARY_CONTENTS` but preserve all other
  flags (SUSPEND/RESUME). Actual secondary CB begin with RENDER_PASS_CONTINUE
  is the caller's responsibility (not validated here).
- `samples` is stored in the signature and the `rasterizationSamples` field;
  it is NOT placed in `VkPipelineRenderingCreateInfo` (which has no such
  field) and does NOT override any multisampleState.
- Non-copyable/non-movable owner; rebuild/failure fully clears stale state.
- Pure model: non-zero viewMask accepted but does not imply multiview enabled.

Getter references and format pointers remain valid only while their owner is
alive and until its next build. Self-input rebuild (build(c.signature(), flags))
is supported. Invalid input resets readiness and all published views; a failed
snapshot allocation resets readiness and rethrows the exception. The model does
not check device limits, format features/aspects, or multiview enablement.

## VKPipelinePNextChain typed composer (M5-D)

Standalone CPU contract tests (12th target, `vulkan_pipeline_pnext_chain_tests`).
Typed pNext chain composer owning a `VKRenderingContract` and an FSR state node;
assembles deterministic FSR -> Rendering -> NULL chain. No Vulkan calls, no
runtime wiring. Supports exactly two typed nodes; adding new types requires a
new owned slot (no opaque chain borrowing).

```powershell
cmake -S native\tests\vulkan-api-version -B native\build\vulkan-modernization\version-tests -G "Visual Studio 18 2026" -A x64
cmake --build native\build\vulkan-modernization\version-tests --config Release
ctest --test-dir native\build\vulkan-modernization\version-tests -C Release --output-on-failure
# Android
cmake -S native\tests\vulkan-api-version -B native\build\vulkan-modernization\version-tests-android -G Ninja `
      "-DCMAKE_TOOLCHAIN_FILE=D:/Android/Sdk/ndk/26.1.10909125/build/cmake/android.toolchain.cmake" `
      "-DANDROID_ABI=arm64-v8a" "-DANDROID_PLATFORM=android-26" "-DANDROID_STL=c++_static" `
      "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" "-DCMAKE_MAKE_PROGRAM=C:/ninja/ninja.exe"
cmake --build native\build\vulkan-modernization\version-tests-android
```

### Evidence boundaries

- Pure offline model: no VkDevice, no vkCreateGraphicsPipelines, no runtime.
- FSR combiner/extent validation checks enum validity only; NOT a device
  feature-support check.
- Self-input boundaries: own contract signature can always rebuild; FSR-only
  own FSR node (pNext NULL) can rebuild; combined-state own FSR has a
  non-NULL tail chain and is therefore rejected per input contract (reset).
- Pointer lifetime: head/node pointers are valid only while the owner is
  alive and before the next build/reset call.

### Revision notes (rev 2)

- const void* head, no const_cast; FSR pNext takes owned const address.
- verifyChain reads nodes ONLY through owned references (fsrInfo()/contract());
  head and FSR tail are verified as addresses against owned node addresses
  before any payload access; mismatch records FAIL without dereferencing.
- Color array traversal is guarded: count AND owned vector size == expected AND pointer non-null
  AND pointer == owned signature colorFormats.data() must hold first.
- verifyCleared checks inactive FSR node, contract published fields, correct
  sTypes on all three nodes, and none-build (ready=true, head NULL) is
  distinguished from cleared (ready=false).
- Owning tests record bool guards while caller memory is valid; mutation
  (which may realloc the caller vector) and post-destruction reads execute
  only when the guard proves non-borrow.
- Every invalid input (FSR: wrong sType / non-NULL pNext / zero width / zero
  height / bad op0 / bad op1; signature: samples 0 / combined samples bits /
  D-S mismatch) runs an independent prime->verify->bad->verifyCleared->
  recover->verify cycle.
- Two-owner test: distinct owned arrays; after rebuilding one owner the
  surviving owner passes the full verifyChain.

Lead review also removed unchecked head/tail casts in the two owning cases,
completed inactive/secondary/base fields, and checked the signature snapshot.
An isolated null-head mutation returns a clean exit 1 (31 failures); the same
test source with the current header returns exit 0. This checks test rejection
and safe failure handling, without creating Vulkan objects.

## VKRenderingAttachmentContract single-attachment mapping (M5-E)

Standalone CPU contract tests (13th target, `vulkan_rendering_attachment_tests`).
Maps one `VkAttachmentDescription` + caller binding to one
`VkRenderingAttachmentInfo` for the future Dynamic Rendering adapter.
Roles: Color/Depth map `loadOp`/`storeOp`, Stencil maps
`stencilLoadOp`/`stencilStoreOp` (only the selected role's ops are validated).

```powershell
cmake -S native\tests\vulkan-api-version -B native\build\vulkan-modernization\version-tests -G "Visual Studio 18 2026" -A x64
cmake --build native\build\vulkan-modernization\version-tests --config Release
ctest --test-dir native\build\vulkan-modernization\version-tests -C Release --output-on-failure
# Android (compile/link only, no device run)
cmake -S native\tests\vulkan-api-version -B native\build\vulkan-modernization\version-tests-android -G Ninja `
      "-DCMAKE_TOOLCHAIN_FILE=D:/Android/Sdk/ndk/26.1.10909125/build/cmake/android.toolchain.cmake" `
      "-DANDROID_ABI=arm64-v8a" "-DANDROID_PLATFORM=android-26" "-DANDROID_STL=c++_static" `
      "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" "-DCMAKE_MAKE_PROGRAM=C:/ninja/ninja.exe"
cmake --build native\build\vulkan-modernization\version-tests-android
```

### Input scope and boundaries

- Single attachment only: no attachment arrays, no VkRenderingInfo, no layout
  barriers, no engine wiring; runtime stays Classic/OFF.
- VkImageView values are borrowed handles only; this model does not own or
  extend real Vulkan resource lifetimes, and never passes synthetic handles
  to real Vulkan functions.
- Narrow input contract: LOAD/CLEAR/DONT_CARE load, STORE/DONT_CARE store,
  exactly one of NONE/SAMPLE_ZERO/AVERAGE/MIN/MAX resolve (combos and
  unknown values rejected). NOT the full set of Vulkan-legal enums.
- imageLayout is copied as the in-render layout (never guessed from
  initial/final; layout legality is deferred to the real adapter).
- No device/format/aspect/resolve-support/independentResolve/actual GPU sample-count or
  layout-feature validation; no implicit transitions; not a Vulkan VUID
  validator. Deferred to the future device/bundle adapter.
- Unused view (NULL) + NONE publishes a canonical empty attachment and
  ignores the description ops entirely; non-NONE resolve on an unused view
  is rejected (the resolve request would be silently lost).

Lead review moved the unused path ahead of description/clear reads and added
full canonical clear-byte checks, ignored stencil/depth operation tests, and
complete active fields after recovery. Source sample-count shape is checked
only when resolve is requested; actual source/target GPU samples remain the
adapter's responsibility. Windows 13/13 and Android incremental compile/link
pass; these are CPU model checks, without rendering or device validation.

## VKRenderingTransitionContract single-image transition model (M5-F)

Standalone CPU contract tests (14th target, `vulkan_rendering_transition_tests`).
Builds two legacy edges (pre = before->during, post = during->after) from an
explicit three-state input; each edge feeds the existing `VulkanBarrierEncoding`
separately and the Sync2 image node is compared field-by-field after
count/pointer guards.

```powershell
cmake -S native\tests\vulkan-api-version -B native\build\vulkan-modernization\version-tests -G "Visual Studio 18 2026" -A x64
cmake --build native\build\vulkan-modernization\version-tests --config Release
ctest --test-dir native\build\vulkan-modernization\version-tests -C Release --output-on-failure
# Android (compile/link only, no device run)
cmake -S native\tests\vulkan-api-version -B native\build\vulkan-modernization\version-tests-android -G Ninja `
      "-DCMAKE_TOOLCHAIN_FILE=D:/Android/Sdk/ndk/26.1.10909125/build/cmake/android.toolchain.cmake" `
      "-DANDROID_ABI=arm64-v8a" "-DANDROID_PLATFORM=android-26" "-DANDROID_STL=c++_static" `
      "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" "-DCMAKE_MAKE_PROGRAM=C:/ninja/ninja.exe"
cmake --build native\build\vulkan-modernization\version-tests-android
```

### Input scope and boundaries

- Single image, single range, three explicit states; source layouts/stages/
  accesses come from the caller. No attachment arrays, no subpass adapter,
  no render graph.
- VkImage is a borrowed handle value: no ownership, no GPU lifetime
  extension, no proof of the real current layout; never passed to real
  Vulkan calls in tests (synthetic handles only).
- Narrow shape checks only (NOT a Vulkan VUID validator): image non-null;
  aspect exactly COLOR / DEPTH / STENCIL / DEPTH|STENCIL (plane aspects
  unsupported); level/layer counts non-zero (VK_REMAINING_* allowed, real
  bounds deferred to device adapter); three stage masks non-zero; during/
  after not UNDEFINED/PREINITIALIZED, during not PRESENT; before UNDEFINED
  allowed (discard intent) and never rewrites stages/accesses.
- Queue families fixed to IGNORED: single-queue scope, no ownership
  transfer; cross-queue is future adapter work.
- Equal layouts never skip a dependency; TOP/BOTTOM are never guessed or
  narrowed; after is never defaulted to PRESENT.
- Not yet migrated: Classic subpassDependency, external global/memory
  dependencies, cross-attachment cases, alias/multi-subpass/queue
  ownership. Resolve vs normal images may each hold one model instance;
  same-view D/S merge/dedup belongs to the future bundle layer.

Lead review added the header's direct initializer_list dependency. A standalone
NDK clang header probe failed before that fix (two missing-initializer_list
errors) and passes afterward. Tests now check all five range fields and both
pNext values in Sync2, the complete input/reset snapshot, and both edges after
rebuild/recovery. Windows 14/14 and Android incremental compile/link pass;
these checks do not establish real image layouts or migrate subpass dependencies.

## VKRenderPassSignatureAdapter classic RP -> signature tuple (M5-G)

CPU adapter tests (15th contract target, `vulkan_render_pass_signature_tests`)
plus a standalone-include probe (`vulkan_render_pass_signature_probe`, the
header must compile on its own; compile-only target, not a run contract).
Attachmentless subpasses are supported: count 0 + NULL attachments with no
references builds an empty tuple (caller samples/viewMask kept); count > 0
with a NULL array is rejected; any actual reference on a 0-attachment
subpass fails the bounds check. `buildVKSubpassRenderingMetadata` extracts a
per-subpass `VKRenderingSignature` from already-mapped Vulkan create-info:
colors keep exact slot order (UNUSED -> UNDEFINED slot, no compression);
depth/stencil classify from the referenced description format across the
seven core D/S formats; reference aspectMask is ignored; samples/viewMask
pass through. The engine (`cmdFuncCCVKCreateRenderPass`) collects this
metadata per subpass into `CCVKGPURenderPass::renderingMetadata` while the
mapped arrays are alive; extraction failure only marks that subpass invalid
and never rejects classic creation.

```powershell
# CPU contract + probe
cmake -S native\tests\vulkan-api-version -B native\build\vulkan-modernization\version-tests -G "Visual Studio 18 2026" -A x64
cmake --build native\build\vulkan-modernization\version-tests --config Release
ctest --test-dir native\build\vulkan-modernization\version-tests -C Release --output-on-failure
# Real classic RP metadata evidence (full engine + WSI harness)
powershell -NoProfile -ExecutionPolicy Bypass -File native\tests\vulkan-engine-smoke\verify-wsi.ps1
# Android (compile/link only, no device run)
cmake -S native\tests\vulkan-api-version -B native\build\vulkan-modernization\version-tests-android -G Ninja `
      "-DCMAKE_TOOLCHAIN_FILE=D:/Android/Sdk/ndk/26.1.10909125/build/cmake/android.toolchain.cmake" `
      "-DANDROID_ABI=arm64-v8a" "-DANDROID_PLATFORM=android-26" "-DANDROID_STL=c++_static" `
      "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" "-DCMAKE_MAKE_PROGRAM=C:/ninja/ninja.exe"
cmake --build native\build\vulkan-modernization\version-tests-android
powershell -NoProfile -ExecutionPolicy Bypass -File native\tests\vulkan-android-build\build.ps1
```

### Input scope and boundaries

- valid means the tuple was extracted: NOT Dynamic Rendering eligibility,
  feature support, pipeline compatibility, or proof Vulkan would accept a
  dynamic pipeline. No hash or cache key changes.
- Input arrays are borrowed only for the build call; caller guarantees
  counts match real lengths. Input/resolve/preserve attachments and pNext
  chains are ignored (not a full subpass validity check).
- Adapter tests with UNUSED slots do NOT mean the existing GFX color-index
  creation path supports INVALID_BINDING color slots; that pre-existing
  limitation is untouched.
- Windows WSI smoke assertions read the real classic RP metadata every
  generation rebuild: real-RP tuple evidence, NOT Dynamic pipeline/draw
  proof. Android covers compile/link only, no device run.

Lead review restored the nonzero-color-count/NULL-color-array rejection case.
The current suite exercises 18 independent rejection/recovery sequences;
15 runtime contracts pass, with the include probe built separately.
Run the commands above from the repository root.

## VKPipelineRenderingAdapter graphics entry (M5-H)

CPU adapter tests (16th runtime target, `vulkan_pipeline_rendering_adapter_tests`)
plus a compile-only probe. `applyVKPipelineRenderingContract` writes only
root.pNext/renderPass/subpass after full success; Classic keeps the classic
RP/subpass with optional FSR, Dynamic requires a compatible multisample state
and nulls the render pass. The engine's graphics PSO creation now composes
its pNext through this adapter in Classic mode; a per-PSO metadata snapshot
copies the render-pass slot tuple. Real-driver PSO evidence runs in
`engine_smoke` behind a volk PFN hook that forwards to the true
`vkCreateGraphicsPipelines` and inspects the actual create infos.

```powershell
# from the repository root
cmake -S native\tests\vulkan-api-version -B native\build\vulkan-modernization\version-tests -G "Visual Studio 18 2026" -A x64
cmake --build native\build\vulkan-modernization\version-tests --config Release
ctest --test-dir native\build\vulkan-modernization\version-tests -C Release --output-on-failure
# real Classic PSO hook evidence (Windows; from the repository root -
# the script builds the current target first; a red build fails fast)
powershell -ExecutionPolicy Bypass -File native\tests\vulkan-engine-smoke\verify-capabilities.ps1 -Cases default
# Android (compile/link only, no device run)
cmake -S native\tests\vulkan-api-version -B native\build\vulkan-modernization\version-tests-android -G Ninja `
      "-DCMAKE_TOOLCHAIN_FILE=D:/Android/Sdk/ndk/26.1.10909125/build/cmake/android.toolchain.cmake" `
      "-DANDROID_ABI=arm64-v8a" "-DANDROID_PLATFORM=android-26" "-DANDROID_STL=c++_static" `
      "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" "-DCMAKE_MAKE_PROGRAM=C:/ninja/ninja.exe"
cmake --build native\build\vulkan-modernization\version-tests-android
```

Positive cases require the unique `CLASSIC_PSO:` evidence marker (exactly one
per log); verify-capabilities.ps1 builds the current target before running
and refuses a stale exe. 18 related build targets total: 16 runtime
contracts + 2 compile-only probes.

### Boundaries

- Runtime stays Classic: Dynamic mode is CPU-contract only, never selected by
  a runtime switch; M4-B not started; metadata.valid is not Dynamic
  eligibility. No PSO cache/hash changes.
- Root state pointers/handles stay owned by the caller; the chain owner must
  live until vkCreateGraphicsPipelines returns; root.pNext is not kept after.
- Narrow checks only: not device support, not a VUID set.
- The engine_smoke evidence is real Classic PSO creation (forwarded driver
  hook): NOT Dynamic Rendering, draw, or visual proof. Two subpasses target
  differently formatted color attachments, selected after querying the real
  device's COLOR_ATTACHMENT_BIT support, to detect wrong-slot metadata copies.

## VKSecondaryInheritanceAdapter secondary begin/inheritance (M5-I)

CPU adapter tests (17th runtime target, `vulkan_secondary_inheritance_adapter_tests`)
plus a compile-only probe. The adapter owns the begin info, inheritance info,
and a VKRenderingContract: Classic copies all inheritance fields (RP/subpass/
FBO/queries) with no rendering node and ignores the signature; Dynamic builds
the contract and chains its secondary rendering info with SECONDARY_CONTENTS
stripped from primary flags. The engine's secondary `begin(renderPass, ...)`
path now routes through this adapter in Classic mode; the primary path keeps
its locals. Real Classic secondary evidence runs in `recording_lifetime_smoke`
(secondary-lifetime case) behind a volk `vkBeginCommandBuffer` hook that
forwards to the real driver and records the actual inheritance values; the
unique `CLASSIC_SECONDARY_INHERITANCE:` marker (printed only after the
real primary submit + completion, with non-NULL real handles) is required by
verify-recording-lifetime.ps1 (SelfTest fixtures: missing/duplicate/
malformed/NULL-handle/wrong-result). The secondary records the current
engine query values (occlusion off, zero query flags/statistics); the core
wiring keeps `&& !_skipRenderPass`, so WSI-failure paths still use the old
local inheritance info.

```powershell
# from the repository root
cmake -S native\tests\vulkan-api-version -B native\build\vulkan-modernization\version-tests -G "Visual Studio 18 2026" -A x64
cmake --build native\build\vulkan-modernization\version-tests --config Release
ctest --test-dir native\build\vulkan-modernization\version-tests -C Release --output-on-failure
# real Classic secondary evidence (Windows; the script builds the target first)
powershell -ExecutionPolicy Bypass -File native\tests\vulkan-engine-smoke\verify-recording-lifetime.ps1 -Cases secondary-lifetime -Completion auto
powershell -ExecutionPolicy Bypass -File native\tests\vulkan-engine-smoke\verify-recording-lifetime.ps1 -Cases secondary-lifetime -Completion fence
powershell -ExecutionPolicy Bypass -File native\tests\vulkan-engine-smoke\verify-recording-lifetime.ps1 -Cases secondary-lifetime -Completion auto -ApiVersion 1.1
# Android (compile/link only, no device run)
cmake -S native\tests\vulkan-api-version -B native\build\vulkan-modernization\version-tests-android -G Ninja `
      "-DCMAKE_TOOLCHAIN_FILE=D:/Android/Sdk/ndk/26.1.10909125/build/cmake/android.toolchain.cmake" `
      "-DANDROID_ABI=arm64-v8a" "-DANDROID_PLATFORM=android-26" "-DANDROID_STL=c++_static" `
      "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" "-DCMAKE_MAKE_PROGRAM=C:/ninja/ninja.exe"
cmake --build native\build\vulkan-modernization\version-tests-android
```

### Boundaries

- Runtime stays Classic: Dynamic mode is CPU-contract only. The secondary-
  lifetime case is an EMPTY secondary: it proves inheritance/execute lifetime
  and real driver begin/execute, NOT secondary draw commands and NOT that a
  driver consumes dynamic inheritance (base.renderPass non-null means
  VkCommandBufferInheritanceRenderingInfo is ignored per spec).
- 20 related build targets: 17 runtime contracts + 3 compile-only probes.
- Handles are borrowed values; the adapter owner must live until
  vkBeginCommandBuffer returns.

## VKRenderingInfoContract composite owning info (M5-J)

CPU contract tests (18th runtime target, `vulkan_rendering_info_contract_tests`)
plus a compile-only probe. Integrates N color attachments plus optional
depth/stencil into a stable owning `VkRenderingInfo` for the future M6 wiring:
colors keep strict slot order (UNUSED -> UNDEFINED, canonical unused node);
depth maps loadOp/storeOp, stencil maps the stencil ops, each with its own
clear value; D/S share one imageView (VUID 06085) and, when both resolve,
one resolveImageView (VUID 06086) while keeping independent modes/ops; flags
accept only SECONDARY_CONTENTS/SUSPEND/RESUME this round; viewMask==0
requires layerCount>0 (VUID 06069 mirror). Reuses the single-attachment
contract per slot and the owned VKRenderingContract for signature validation.

```powershell
# from the repository root (pure CPU, no GPU/driver involvement)
cmake -S native\tests\vulkan-api-version -B native\build\vulkan-modernization\version-tests -G "Visual Studio 18 2026" -A x64
cmake --build native\build\vulkan-modernization\version-tests --config Release
ctest --test-dir native\build\vulkan-modernization\version-tests -C Release --output-on-failure
# Android (compile/link only, no device run)
cmake -S native\tests\vulkan-api-version -B native\build\vulkan-modernization\version-tests-android -G Ninja `
      "-DCMAKE_TOOLCHAIN_FILE=D:/Android/Sdk/ndk/26.1.10909125/build/cmake/android.toolchain.cmake" `
      "-DANDROID_ABI=arm64-v8a" "-DANDROID_PLATFORM=android-26" "-DANDROID_STL=c++_static" `
      "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" "-DCMAKE_MAKE_PROGRAM=C:/ninja/ninja.exe"
cmake --build native\build\vulkan-modernization\version-tests-android
```

### Boundaries

- Official input rules mirrored narrowly:
  https://docs.vulkan.org/refpages/latest/refpages/source/VkRenderingInfo.html
  (VUID 06069 layerCount, 06085 D/S same imageView, 06086 same resolve view).
  This narrow model is NOT proof a device/driver accepts any VkRenderingInfo.
- VkImageView handles are borrowed values: no GPU lifetime extension, no
  current-layout proof, no transitions (those stay with
  VKRenderingTransitionContract). No FSR/deviceGroup/localRead opaque chains.
- "absent attachment == UNDEFINED signature format" is a first-batch narrow
  policy, not a claim that Vulkan forbids other legal unused combinations.
- Pointer validity: published pointers are valid while the owner lives and
  until the next build/reset. 22 related build targets: 18 runtime contracts
  + 4 compile-only probes.

### M7-A Pipeline Cache persistence contracts

`vulkan_pipeline_cache_tests` is a CPU/mock-driver + real-filesystem test (no GPU).
Run with the existing Windows configuration:

```powershell
cmake -S native/tests/vulkan-api-version -B native/build/vulkan-modernization/version-tests -G "Visual Studio 18 2026" -A x64
cmake --build native/build/vulkan-modernization/version-tests --config Release --target vulkan_pipeline_cache_tests
ctest --test-dir native/build/vulkan-modernization/version-tests -C Release --output-on-failure
```

It covers the v2 envelope, compatibility, malformed length/checksum/Vulkan header,
query errors and bounded INCOMPLETE retries, actual Unicode file replacement,
Windows rename failure preserving the old file, and rejection of oversized files
before allocation. Scratch files live in a unique child of the test working directory;
the test removes only its own files. Current suite has 19 runtime contracts + 4
compile probes. The checksum detects accidental corruption; it is not authentication.
Atomic replacement is not a POSIX power-loss durability guarantee. Driver acceptance,
engine integration, cold-start speed and platform Gate require separate evidence.

## VKLayoutKeys owning layout key models (M7-B1)

CPU value models (20th runtime target, `vulkan_layout_key_tests` + compile-only
probe). `VulkanDescriptorSetLayoutKey` sorts bindings by number (canonical),
preserves flags, rejects duplicates/immutable samplers/count>4096/bad sType;
`VulkanPipelineLayoutKey` deep-copies ordered DS children + push constant
ranges, rejects set count mismatch/NULL/child invalid. Both use FNV-1a 64-bit
hashing with domain separation (DS=1, Pipeline=2). No GPU calls, no device-
support proof; binding shapes are application-model limits, not Vulkan VUIDs;
immutable samplers conservatively rejected (future sampler-content key would
need its own tier). Used by `std::unordered_map` with custom hasher.

Hash schema uses little-endian uint32 words and FNV-1a 64-bit:
DS = domain 1, schema 1, valid, flags, binding count, then each canonical
binding's number/type/count/stages. PL = domain 2, schema 1, valid, flags,
ordered set count, each child's complete DS stream, push count, then each
push range's stages/offset/size. Invalid defaults are hashed with valid=0;
domains distinguish streams but do not eliminate collisions. Tests force
collisions for both key types and use full equality for lookup.

Lead's true `build(info, key.setLayouts(), key)` red case is retained.
Failure injection measures only each builder invocation; output is primed
before counting. Actual tested fixture allocation points: DS=1, nested
PL=4, each injected once including aliased children; false return must leave
default invalid metadata and allow a subsequent successful rebuild.

The engine now stores layout keys as metadata from the actual creation
parameters. A key failure does not gate Vulkan creation; pool IDs and driver
calls retain their existing behavior. No layout dedup/prewarm is implemented.

```powershell
# from the repository root
cmake -S native/tests/vulkan-api-version -B native/build/vulkan-modernization/version-tests -G "Visual Studio 18 2026" -A x64
cmake --build native/build/vulkan-modernization/version-tests --config Release
ctest --test-dir native/build/vulkan-modernization/version-tests -C Release --output-on-failure
# Android (compile/link only, no device run)
cmake -S native/tests/vulkan-api-version -B native/build/vulkan-modernization/version-tests-android -G Ninja `
      "-DCMAKE_TOOLCHAIN_FILE=D:/Android/Sdk/ndk/26.1.10909125/build/cmake/android.toolchain.cmake" `
      "-DANDROID_ABI=arm64-v8a" "-DANDROID_PLATFORM=android-26" "-DANDROID_STL=c++_static" `
      "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" "-DCMAKE_MAKE_PROGRAM=C:/ninja/ninja.exe"
cmake --build native/build/vulkan-modernization/version-tests-android
```

## Shader stage content key (M7-C1)

`VKShaderStageKey.h` owns the stage, pipeline-stage flags, entry point bytes and
final SPIR-V words. It stores no Vulkan handle or borrowed pointer. Equality
compares the complete tuple. FNV-1a mixes LE uint32 domain=3/schema=1/valid/flags/
stage/entry byte count, entry bytes without NUL, LE word count, and each LE word.
Module handle changes do not change the key.

The builder accepts one traditional graphics/compute stage, a nonempty entry
point of at most 1024 bytes and 5..16M words starting with SPIR-V magic. These
are application shape limits, not device support or a SPIR-V validation pass.
Non-null pNext/specialization inputs are conservatively rejected until modeled.
Inputs must be readable; entry scanning stops after the limit. Both name/code
can alias the output's getters: a complete local snapshot precedes publishing.
Failure, including bad_alloc, returns false and clears all output fields.

```powershell
cmake -S native/tests/vulkan-api-version -B native/build/vulkan-modernization/version-tests -G "Visual Studio 18 2026" -A x64
cmake --build native/build/vulkan-modernization/version-tests --config Release --target vulkan_shader_stage_key_tests vulkan_shader_stage_key_probe
ctest --test-dir native/build/vulkan-modernization/version-tests -C Release --output-on-failure
```

Current Windows suite has 21 runtime contracts; probes are compile-only.
The new test checks two independent known vectors, true double self-alias,
separate owning arrays after caller destruction, forced hash collisions and
actual failure injection at both long-name/code allocation points. The earlier
tests that required a noexcept builder to throw were corrected; capacity reuse
was not the cause. Android uses the NDK configure command in the layout-key
section, then builds `vulkan_shader_stage_key_tests vulkan_shader_stage_key_probe`;
Android evidence is compile/link only.

## Compute pipeline content key (M7-D1)

`VulkanComputePipelineKey` owns pipeline flags, a valid COMPUTE shader key and a
valid pipeline-layout key. Flags are opaque values; accepting a key does not
validate a Vulkan pipeline. Equality compares both complete child keys. FNV-1a
mixes LE uint32 domain=4/schema=1/valid/flags, then LE uint64 shader hash and LE
uint64 layout hash. Child-hash collisions still require full content equality.
No module/layout/pipeline handles or object IDs are stored.

Inputs may be the output's own child getters: validation precedes local copies,
and publishing occurs after both copies succeed. Invalid inputs and bad_alloc
return false and clear the entire output. Lead independently computed vectors:
default `0xa4a776ca92d79e50`; flags=0, COMPUTE/main with words
`[0x07230203,0x00010000,0,1,0]` and a built empty layout `0xf9f23734ca303383`.

```powershell
cmake -S native/tests/vulkan-api-version -B native/build/vulkan-modernization/version-tests -G "Visual Studio 18 2026" -A x64
cmake --build native/build/vulkan-modernization/version-tests --config Release --target vulkan_compute_pipeline_key_tests vulkan_compute_pipeline_key_probe
ctest --test-dir native/build/vulkan-modernization/version-tests -C Release --output-on-failure
# Compile/link only on Android; no device execution.
cmake -S native/tests/vulkan-api-version -B native/build/vulkan-modernization/version-tests-android -G Ninja `
      "-DCMAKE_TOOLCHAIN_FILE=D:/Android/Sdk/ndk/26.1.10909125/build/cmake/android.toolchain.cmake" `
      "-DANDROID_ABI=arm64-v8a" "-DANDROID_PLATFORM=android-26" "-DANDROID_STL=c++_static" `
      "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" "-DCMAKE_MAKE_PROGRAM=C:/ninja/ninja.exe"
cmake --build native/build/vulkan-modernization/version-tests-android --target vulkan_compute_pipeline_key_tests vulkan_compute_pipeline_key_probe
```

The current Windows suite has 22 runtime contracts; probes are compile-only.
The long-name/two-set/push-range fixture measures six builder allocation points
on this MSVC build, injects each once with aliased inputs, and checks full reset
and recovery. Tests also cover caller destruction, complete rebuilds, independent
SPIR-V/binding/push mutations and forced map collisions. The engine now records
this key for its existing compute encoder; driver proof is documented in the
engine-smoke README. Graphics keys, CPU deduplication, manifest/prewarm and Gates
are pending. Orca Worker built both Android targets on 2026-10-04 (four actual
compile/link steps, m7d3-worker-android-contract-build.log); no device execution.

## Conservative Classic RenderPass compatibility key (M7-E1)

`VulkanClassicRenderPassKey` owns a uint32 value stream from RenderPass2 inputs.
It records root flags, attachment flags/formats/samples, subpass graphs and
reference indices/aspects, preserve slots, view masks, ordinary dependencies
and correlation masks. Attachment load/store and initial/final layouts, and
reference layouts, are omitted. NULL/UNUSED depth references are normalized.
Known D/S resolve and FSR attachment nodes are encoded in a fixed order;
unknown extension structures, duplicate nodes and cycles are rejected.

This is a conservative sufficient key for valid modeled inputs, not a complete
canonicalization of all [Vulkan compatibility rules](https://docs.vulkan.org/spec/latest/chapters/renderpass.html#renderpass-compatibility).
Numbering, array lengths and resolve modes are retained even where more reuse
would be allowed; unequal keys do not prove incompatibility. Build accepts
application shapes, not device support or complete Vulkan validity. Caps are
256 attachments/subpasses and 4096 dependencies. No Vulkan calls or pointers
are stored. Hash uses FNV-1a LE u32 domain=5/schema=1/valid/wordCount/each word;
full stream equality decides lookup. Default hash is `0xf26d36715c6423d1`.

```powershell
cmake -S native/tests/vulkan-api-version -B native/build/vulkan-modernization/version-tests -G "Visual Studio 18 2026" -A x64
cmake --build native/build/vulkan-modernization/version-tests --config Release --target vulkan_classic_render_pass_key_tests vulkan_classic_render_pass_key_probe
ctest --test-dir native/build/vulkan-modernization/version-tests -C Release --output-on-failure
# Use the NDK configure command in the compute-key section, then:
cmake --build native/build/vulkan-modernization/version-tests-android --target vulkan_classic_render_pass_key_tests vulkan_classic_render_pass_key_probe
```

Current Windows suite: 23 runtime tests, probes compile-only. Tests include an
independent complete word/hash vector, ignored versus retained single-field
changes, multi-subpass/ordered color/preserve/correlation cases, owning lifetimes,
fixed extension order, reject/reset/recovery, forced collisions and all eight
measured fixture allocation failures. Orca Worker built both Android targets on
2026-10-04 (four actual compile/link steps, m7e1-worker-android-build.log); no
device execution. M7-E2 now records this key from the real RenderPass2 input;
driver evidence and remaining integration boundaries are in the smoke README.

## M7-F1 VulkanGraphicsRasterKey

Header-only CPU key (`cc::gfx::VulkanGraphicsRasterKey`) snapshotting
rasterization / multisample / optional depth-stencil state of a graphics
pipeline as an owning `std::vector<uint32_t>` word stream. Pure data; not wired
to the engine runtime.

### Field order (schema 1, hash domain 6)

- Raster (11 words): `flags`, `depthClampEnable`, `rasterizerDiscardEnable`,
  `polygonMode`, `cullMode`, `frontFace`, `depthBiasEnable`,
  `depthBiasConstantFactor`, `depthBiasClamp`, `depthBiasSlopeFactor`,
  `lineWidth`
- Multisample (7 + maskWordCount words): `flags`, `rasterizationSamples`,
  `sampleShadingEnable`, `minSampleShading`, `maskWordCount`,
  `mask[maskWordCount]`, `alphaToCoverageEnable`, `alphaToOneEnable`
- DepthStencil (23 words if present, else 1 presence word `0`):
  presence(`1`), `flags`, `depthTestEnable`, `depthWriteEnable`,
  `depthCompareOp`, `depthBoundsTestEnable`, `stencilTestEnable`,
  `front` (7: failOp, passOp, depthFailOp, compareOp, compareMask, writeMask,
  reference), `back` (same 7), `minDepthBounds`, `maxDepthBounds`

### Fixed conventions

- Floats are bit-copied to `uint32_t` via `memcpy` (static_assert size 4);
  positive and negative zero stay distinct.
- `maskWordCount = ceil(rasterizationSamples / 32)`.
- `pSampleMask == nullptr` is normalized to that many `0xffffffff` words —
  equivalent to an explicit all-ones mask.
- `rasterizationSamples` must be a power of two in `[1, 64]`; `0`, `128`, or
  non-power-of-two values are rejected.
- Each `sType` is checked; any non-null `pNext` is rejected.
- FNV-1a 64-bit hash over the LE byte stream: basis `14695981039346656037`,
  prime `1099511628211`. Each `uint32_t` word contributes 4 bytes lowest-first;
  every byte is xor-ed into the low 8 bits of the accumulator then multiplied
  by the prime. Metadata words are domain (`6`), schema (`1`), valid flag,
  word count; then every content word. Hash is an accelerator only; full word
  equality gates key equivalence.
- Conservative snapshot: all scalars are recorded even if dynamic state would
  ignore some, so false equal is impossible; false miss is permitted.
- `static build()` is `noexcept`: any failure (including `bad_alloc`) fully
  resets `out` to default invalid state and returns `false`.
- Copy construction / copy assignment are ordinary value operations: an
  allocation failure propagates `bad_alloc` to the caller. These two
  allocation semantics are intentionally different.
- `VkBool32` fields are stored as their raw `uint32_t` values; no `?:`
  normalization is applied. `1` and `2` remain distinct.
- `reserve` is exact: `11 + 7 + maskWordCount + (DS ? 23 : 1)`.

### Windows commands

```powershell
cmake -S native/tests/vulkan-api-version `
      -B native/build/vulkan-modernization/version-tests `
      -G "Visual Studio 18 2026" -A x64 `
      "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
cmake --build native/build/vulkan-modernization/version-tests --config Release `
      --target vulkan_graphics_raster_key_tests vulkan_graphics_raster_key_probe
ctest --test-dir native/build/vulkan-modernization/version-tests -C Release
```

### Android commands (compile/link only; no device)

```powershell
cmake -S native/tests/vulkan-api-version `
      -B native/build/vulkan-modernization/version-tests-android -G Ninja `
      "-DCMAKE_TOOLCHAIN_FILE=D:/Android/Sdk/ndk/26.1.10909125/build/cmake/android.toolchain.cmake" `
      "-DANDROID_ABI=arm64-v8a" `
      "-DANDROID_PLATFORM=android-26" `
      "-DANDROID_STL=c++_static" `
      "-DCMAKE_BUILD_TYPE=Release" `
      "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" `
      "-DCMAKE_MAKE_PROGRAM=C:/ninja/ninja.exe"
cmake --build native/build/vulkan-modernization/version-tests-android `
      --target vulkan_graphics_raster_key_tests vulkan_graphics_raster_key_probe `
      --parallel 4
```

### Boundary

This component is CPU-only; it is not consumed by the engine runtime and is
not wired into graphics PSO creation, prewarm, or any performance gate. Android
evidence is compile/link only (no device execution).

Lead review completed the missing full asymmetric 42-word stream, all 40
scalar mutations, nine independently primed rejection/recovery cycles and
owned caller destruction/rebuild checks. Array reads use guards. The measured
fresh build allocation point is injected exactly once; absence of an injection
fails the test. Logs: m7f1-lead-final-{build,test,ctest}.log (24/24 at that time)
and m7f1-lead-final-android-build.log (both targets compiled/linked).

## Complete graphics content model (M7-F2, CPU only)

`VulkanGraphicsPipelineKey` owns ordered shader keys, a layout key, the raster
component, a Classic compatibility key or a Dynamic rendering signature, and
the remaining fixed-function value stream. The builder takes the actual
`VkGraphicsPipelineCreateInfo`, pointers to its stages' already-built content
keys, the layout key, and an optional Classic key. Callers must supply the keys
belonging to the objects behind the handles. Handles themselves are excluded.

The stream preserves root mode/flags/subpass/baseIndex; vertex flags, ordered
bindings and attributes; assembly; optional tessellation; dynamic flags/list;
viewport flags/counts and static viewport/scissor arrays; optional blend flags,
logic state, attachment equations/masks and constants; optional FSR extent and
combiner operations. Dynamic viewport/scissor arrays are ignored per
[Vulkan viewport rules](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineViewportStateCreateInfo.html).
Other ignored scalars may remain and cause conservative misses.

Root extensions support Rendering and FSR only (typed, unique, at most two
nodes). Unknown extensions, derivatives/library pipelines, unsupported shapes
or invalid child keys return false and reset every child. This targets the
existing complete traditional encoder, not the full Vulkan feature space or
device validity. Application caps: five stages, arrays 256, dynamic states 64.
Mandatory vertex/assembly/viewport/raster/multisample blocks may reject otherwise
valid Vulkan configurations. Build is noexcept and snapshots before publishing,
including genuine aliases of the output's child getters and entry-point strings.

FNV-1a hashes LE u32 domain=7/schema=1/valid/stageCount, each stage's LE u64
hash, then layout/raster/Classic/signature LE u64 hashes, followed by wordCount
and each LE u32 state word. Complete child and stream equality resolves collisions.
Independent vectors: default `0x791321a567999f23`, fixture Classic
`0xcee3d48d38ab88ca`, Dynamic `0xe5a4c3ca20117c6f`. The fixture also checks a
hand-specified 46-word stream. No model code calls Vulkan.

```powershell
cmake -S native/tests/vulkan-api-version -B native/build/vulkan-modernization/version-tests -G "Visual Studio 18 2026" -A x64
cmake --build native/build/vulkan-modernization/version-tests --config Release --target vulkan_graphics_pipeline_key_tests vulkan_graphics_pipeline_key_probe
ctest --test-dir native/build/vulkan-modernization/version-tests -C Release --output-on-failure
# Use the quoted NDK configure command above, then compile/link only:
cmake --build native/build/vulkan-modernization/version-tests-android --target vulkan_graphics_pipeline_key_tests vulkan_graphics_pipeline_key_probe
```

Current suite: 25 runtime contracts; probes are compile-only. Lead review fixed
Worker test gaps: failed builds now prime the actual output, the blend-factor
baseline restores ZERO, tess patch count has its own baseline, compute/duplicate
stage failures use matching shader keys, scope destruction uses the surviving
owner, and Dynamic self-input compares the entire tuple with aliased formats and
entry-point strings. Additional checks cover static viewport/scissor fields,
actual NULL ignored arrays, canonical valid pNext order and complete recovery.
Classic/Dynamic allocation counts remain 13/14, with an injection required at
every measured point. The earlier Worker claim of complete independent coverage
was too broad; test command success remains valid as a historical fact.

The engine now records Classic input metadata and successful Dynamic variant
metadata (M7-F3). Real Classic creation proof is documented in the engine-smoke
README. The CPU suite alone does not prove a GPU, visual or performance Gate;
serial prewarm, complete dynamic-field families, unknown-extension support and
Android device execution remain outside this suite's evidence.

## M7-G1 VulkanPipelinePrewarmQueue

Header-only pure-CPU prewarm queue (`cc::gfx::VulkanPipelinePrewarmQueue`) for
known pipeline keys. Experimental interface only: no Vulkan calls, no GPU
object creation/destruction, no filesystem access, no change to any existing
runtime default, not cross-run serialization. The real engine factory / driver
cache / checkpoint integration is a later step (not part of this block).

### Commands

Windows:
```powershell
cmake -S native/tests/vulkan-api-version `
      -B native/build/vulkan-modernization/version-tests `
      -G "Visual Studio 18 2026" -A x64 `
      "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
cmake --build native/build/vulkan-modernization/version-tests --config Release `
      --target vulkan_pipeline_prewarm_tests vulkan_pipeline_prewarm_probe
ctest --test-dir native/build/vulkan-modernization/version-tests -C Release
```

Android (compile/link only, no device execution):
```powershell
cmake -S native/tests/vulkan-api-version `
      -B native/build/vulkan-modernization/version-tests-android -G Ninja `
      "-DCMAKE_TOOLCHAIN_FILE=D:/Android/Sdk/ndk/26.1.10909125/build/cmake/android.toolchain.cmake" `
      "-DANDROID_ABI=arm64-v8a" `
      "-DANDROID_PLATFORM=android-26" `
      "-DANDROID_STL=c++_static" `
      "-DCMAKE_BUILD_TYPE=Release" `
      "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" `
      "-DCMAKE_MAKE_PROGRAM=C:/ninja/ninja.exe"
cmake --build native/build/vulkan-modernization/version-tests-android `
      --target vulkan_pipeline_prewarm_tests vulkan_pipeline_prewarm_probe --parallel 4
```

### Semantics and boundaries

- Manifest dedup uses the complete variant type plus full tuple equality of
  the key; labels never participate. Compute and graphics keys never mix.
- enqueue: busy rejection, then validation (non-empty label <= 256, valid
  expected key, non-empty creator), then duplicate, then capacity. A local
  owning snapshot is published on success; any allocation failure maps to
  OutOfMemory and leaves the manifest unchanged. Copy exceptions of
  Result/Key are ordinary value semantics; only enqueue/drain are noexcept.
- Closure lifetime: the queue holds an owning std::function with normal
  value-copy semantics; it does not extend the lifetime of raw borrowed
  captures. The closure owns its recipe/resources.
- drain: FIFO, one attempt per Pending job; maxJobs counts attempts and
  failures consume budget; Ready requires created==true, valid actual, and
  full equality with expected; everything else (including callback throws and
  bad_alloc) is Failed and the walk continues. No automatic retry; a new wave
  is clear() + re-enqueue. Reentrant enqueue during drain is Busy, clear is
  false, nested drain reports busy with zero counters.
- Single-threaded: no threads, locks, or async anywhere.

M7-G1 Lead 复核：队列完整 tuple 需在调用方销毁后继续核对；closure 的外部 Job/shared_ptr 在观察前释放，clear 后才检验 weak_ptr 失效。FIFO 使用 callback 实际调用轨迹；created=false 与有效匹配 actual 独立验证，同类型不同 tuple 也拒绝。当前契约 26/26，不代表 GPU 预热完成。
