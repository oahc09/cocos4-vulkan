# Cocos Vulkan engine smoke

This executable links the actual `cocos_engine`, initializes `CCVKDevice`, and
uses `SPIRVUtils` from that engine. It starts no application or window.

Coverage:

- Instance/device/VMA/default-resource creation and teardown with Khronos,
  synchronization and best-practices validation enabled. Missing validation is
  a test failure, not a pass.
- Vulkan 1.1/1.2/1.3 vertex, fragment and compute compilation; expected SPIR-V
  version words are `0x00010300`, `0x00010500`, `0x00010600`.
- Vertex input location compression through glslang reflection.
- Shader module creation for targets admitted by the effective device API.
- Invalid compilation clears previous output.

## Windows

From the repository root, using the installed Visual Studio generator:

```powershell
cmake -S native/tools/simulator/frameworks/runtime-src -B native/build/vulkan-modernization/win64-simulator -G 'Visual Studio 18 2026' -A x64 '-DCC_USE_VULKAN=ON' '-DCC_USE_GLES3=OFF' '-DCMAKE_POLICY_VERSION_MINIMUM=3.5' '-DUSE_PHYSICS_PHYSX=OFF' '-DCC_DEBUG_FORCE=ON' '-DCC_VULKAN_BUILD_TESTS=ON'
cmake --build native/build/vulkan-modernization/win64-simulator --config Release --target vulkan_engine_smoke --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Engine smoke build failed' }
$env:VK_LAYER_PATH = 'D:/Software/VulkanSDK/1.4.341.1/Bin'
$emptyImplicitLayers = Join-Path (Resolve-Path native/build/vulkan-modernization).Path 'empty-implicit-layers'
New-Item -ItemType Directory -Force -Path $emptyImplicitLayers | Out-Null
$env:VK_IMPLICIT_LAYER_PATH = $emptyImplicitLayers
$env:PATH = (Resolve-Path native/external/win64/libs).Path + ';' + (Resolve-Path native/external/win64/libs/v8/Release).Path + ';' + $env:PATH
$engineOutput = Join-Path (Resolve-Path native/build/vulkan-modernization).Path ('engine-spirv-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
& native/build/vulkan-modernization/win64-simulator/vulkan-engine-smoke/Release/vulkan_engine_smoke.exe $engineOutput
if ($LASTEXITCODE -ne 0) { throw 'Engine smoke run failed' }
& native/tests/vulkan-modernization/verify-spirv.ps1 -SkipHarnessRun -OutDir $engineOutput -SpirvVal 'D:/Software/VulkanSDK/1.4.341.1/Bin/spirv-val.exe'
if ($LASTEXITCODE -ne 0) { throw 'Engine SPIR-V validation failed' }
```

Adjust generator and SDK paths to the local installation. `VK_LAYER_PATH` is a
process-local workaround for this host's stale SDK registration.
`VK_IMPLICIT_LAYER_PATH` isolates this test from the host's broken WeGame implicit
layer manifest; it changes this test's layer environment and does not establish
that the unmodified host environment is error-free. A fresh output
directory prevents old artifacts from satisfying a failed run. Source archives
are hash-checked; `CC_SHADER_UPSTREAM_CACHE` may point to a cache containing
`glslang.tar.gz`, `spirv-tools.tar.gz`, and `spirv-headers.tar.gz`.

This test does not draw, dispatch, present, recreate a swapchain, compare images,
measure performance, or substitute for Android/XR device validation. Compiling
lower targets on a newer GPU does not verify a real Vulkan 1.1/1.2 GPU.

## Capability acceptance matrix (M1-D, revised)

One command runs the REAL engine across the whole negotiation matrix and fails
on any deviation:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File native\tests\vulkan-engine-smoke\verify-capabilities.ps1
# exit 0 = every case as expected (9 x exit0 + 2 x clean exit1)
# -Cases <names>  run a subset (unknown names rejected)
# -SelfTest       fixture pseudo-cases prove unrelated-exit1 / NTSTATUS crash
#                 can never be judged PASS (script must exit 0)
```

- Positive cases: default tier + forced 1.1/1.2/1.3 + 1.3 with each debug-disable
  feature + all four disabled. Each case: engine exit 0, `Cocos effective API`
  matches the forced tier, validation ENABLED with `errors=0`, physical-vs-effective
  note logged (a forced tier is NOT a real low-version GPU), and the engine's own
  SPIR-V products validated 9/9 by `verify-spirv.ps1` (three tiers always compile;
  module creation is only attempted at or below the effective tier).
- Negative cases (`neg-force-1.0`, `neg-unknown-disable`): exit **exactly 1**, log
  contains the case-specific rejection reason (`Invalid CC_VULKAN_FORCE_API: 1.0` /
  `Unknown CC_VULKAN_DISABLE_FEATURES entry: bogusToken`) AND
  `FAIL: Cocos Vulkan initialization`. Crashes (NTSTATUS), other exit codes,
  missing logs/reasons, or unrelated programs exiting 1 are FAILURES - a
  non-zero exit alone is NOT acceptance evidence (the initial revision accepted
  any non-zero exit; that judgement was insufficient and has been corrected).
- The binary itself additionally asserts the negotiation contract internally:
  effective EQUALS min(instance, physical) with four-way consistency across
  context apiVersionInfo / context capabilities.version / gpuDevice
  apiVersionInfo / capabilities.version; `enabled == supported && requested` for
  every concrete state (incl. descriptor-indexing members and FSR
  pipeline/attachment); default policy derived from tier + disable hooks
  (BDA/DI/memoryBudget/GPL unrequested); FSR summary verified field-wise as
  ANY-of-sub (never the generic invariant - a GPU supporting only `primitive`
  while policy requests pipeline/attachment would break it); pipeline/attachment
  requested together; completion backend matches feature/debug policy while
  modern render encoders remain OFF; maintenance4 enabled implies
  maxBufferSize > 0. No per-GPU feature availability is hardcoded.
- Environment (PATH / VK_LAYER_PATH / VK_IMPLICIT_LAYER_PATH / CC_VULKAN_*) is
  snapshotted and restored in `finally`; unhandled exceptions are caught, marked
  as failure and the environment is still restored; every case writes to its own
  collision-proof `run-<timestamp>-<pid>` directory.
- Known noise: the default-texture `BestPractices-ImageMemoryBarrier-TransitionUndefinedToReadOnly`
  warning stays; the intentional invalid-GLSL parse error is not a validation error.

## Real Win32 WSI stress harness (M2-D)

One command (builds the `vulkan_wsi_smoke` target in the existing
win64-simulator tree, runs it once with SDK validation + implicit-layer
isolation + engine DLLs on PATH):

```powershell
# single default run
powershell -NoProfile -ExecutionPolicy Bypass -File native\tests\vulkan-engine-smoke\verify-wsi.ps1
# strict-judge fixture self-check (must exit 0 before trusting any run)
powershell -NoProfile -ExecutionPolicy Bypass -File native\tests\vulkan-engine-smoke\verify-wsi.ps1 -SelfTest
# cross-tier matrix: default + forced 1.1/1.2/1.3 + swapchainMaintenance1-off
powershell -NoProfile -ExecutionPolicy Bypass -File native\tests\vulkan-engine-smoke\verify-wsi.ps1 -Matrix
```

## Strict verdict function + matrix + S8 (M2-E)

- `Test-WsiLog` is the ONLY judge (shared by real runs and `-SelfTest`
  fixtures): exit 0 + validation ENABLED + EXACTLY ONE completion marker per
  scenario S1..S8 (duplicates fail) + EXACTLY ONE clean footer
  (`validation errors=0 ... failures=0`) + no SKIPPED/INCOMPLETE + no
  VUID/hazard/FAIL lines. Fixtures cover: missing footer, footer failures=1,
  7x duplicate S1 faking a complete run, NTSTATUS exit, SKIPPED/incomplete,
  and a valid complete sample.
- `-Matrix` runs default / forced 1.1 / 1.2 / 1.3 / swapchainMaintenance1-off
  (each run's debug env is set then restored; forced tiers on a 1.4 physical
  GPU are NOT real low-version GPU evidence; the maint1-off PASS is a runtime
  fallback regression, not a strict retirement Gate).
- S8 (new): offscreen empty CB submit strictly between one frame's present
  and the next acquire + empty present - asserts the post-present cleanup
  queue does not consume the previous present semaphore or a stale index,
  with zero added validation errors (delta asserted).

- What it does: real `CreateWindowExW` test windows (SW_SHOWNOACTIVATE, no
  focus stealing), real `CCVKSwapchain` via the raw backend, minimal clear
  frames (acquire -> beginRenderPass(CLEAR) -> submit -> present), and the
  full lifecycle matrix: 120 stable frames, 30 alternating resizes,
  zero-size suspend/restore, surface destroy/recreate (same HWND), two
  windows with one suspended, all-acquire-failed empty submit + empty
  present, and documented acquire-result injection through the volk function
  pointer (OUT_OF_DATE / SURFACE_LOST injected WITHOUT a real acquire;
  SUBOPTIMAL via a real acquire with a rewritten result).
- Harness config reported at runtime: `enableAutoBarrier(false)` fixed at
  startup (the render pass encodes PRESENT->PRESENT itself; the deduced late
  barrier would duplicate the final transition) - not a mid-run VUID
  workaround. No per-frame waitIdle; recreation/retirement waits submissions plus present fences when the negotiated
  swapchainMaintenance1 feature is enabled. The pure-core WaitIdle retirement
  fallback is conventional, not a normative present-completion proof.
- **STRICT PASS criteria** (revised): exit 0 + validation REQUIRED + all eight
  scenario summaries + 0 assertion failures + 0 validation errors. The run
  budget-stops on a live error threshold (`gpuContext()->validationErrorCount`
  atomic; `getLastValidationErrorCount()` is a post-destroy cache by design)
  and reports remaining scenarios as SKIPPED - an incomplete run is FAIL, not
  a soft pass. The pre-M2-C2 baseline failed early in S1. Lead's M2-C2
  implementation passes all seven scenarios on this Windows GPU with
  swapchainMaintenance1 enabled (see PROGRESS §2P); other tiers/platforms
  and visual/platform coverage remain outside this limited harness. Never mask VUIDs or weaken assertions to pass.
- This is a real-window limited-clear harness: NOT a game visual baseline,
  NOT Android/XR evidence, and not a substitute for the M2 Gate.

Findings from the first full run: see PROGRESS §2O.

M2-C2 debug A/B hook: `CC_VULKAN_DISABLE_FEATURES=swapchainMaintenance1`
keeps feature support visible while using the pure-core WSI path. This hook
is independent of `maintenance4`. Test output is under
`native/build/vulkan-modernization`; the script restores working directory
and its process environment in `finally`.

## Aborted-frame / recreate-failure red cases (M2-F)

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-wsi-lifecycle.ps1
```

- `vulkan_wsi_lifecycle_smoke --case <name>` runs ONE dangerous sequence per
  process (a crash cannot hide the other cases). Strict per-case judge
  (separate from the S1..S8 judge): exit 0 + validation ENABLED + exactly one
  clean `<case> completed: failures=0 liveErrors=0 postErrors=0` marker + no
  VUID/hazard/FAIL lines.
- Cases: `zero-before-submit` (acquire then resize(0,0) then present),
  `surface-before-submit` (acquire then destroySurface/createSurface),
  `resize-after-record` (record stale FBO, resize, submit stale CB),
  `create-out-of-date` (one-shot volk hook on vkCreateSwapchainKHR: REAL
  create succeeds so the old swapchain is genuinely retired, the fresh
  replacement is destroyed, and OUT_OF_DATE is returned; recovery acquire
  must rebuild from a non-retired path with no oldSwapchain-01933).
- Historical M2-F baseline: cases 1-3 crashed (0xC0000005) / emitted
  VUID-vkEndCommandBuffer-00059; case 4 passed. After M2-C3 all four cases
  pass with maintenance1 enabled and disabled on this Windows GPU.
- Cases 5-6 (M2-G): `two-window-abort` (batch acquire, A zero-aborted, B
  keeps its acquire and presents independently; A restored, both frames run;
  B's objects fully destroyed) and `reacquire-stale-record` (stale CB gated
  by acquire-serial with generation UNCHANGED + pure-CPU synthetic
  CCVKGPUWSIUsage counterexample - explicitly not driver evidence).
- `--auto-barrier` (startup-only): false = RP encodes PRESENT->PRESENT;
  true = RP stays COLOR_ATTACHMENT_WRITE and the engine's early/late
  barriers own the PRESENT<->COLOR transitions. The script's `-AutoBarrier`
  / `-MaintenanceOff` switches expand the matrix to 6 x 2 x 2 = 24 runs.
- Historical baseline (after M2-C3, before M2-C4): 22/24 PASS. Remaining red then: two-window-abort
  under autoBarrier=true (both maintenance1 modes) - VUID-vkCmdDraw-None-09600
  and VUID-VkPresentInfoKHR-pImageIndices-01430 on the SURVIVING window.
  Honest core evidence for Lead; stays FAIL until fixed.
- Recovery clears rebuild the FBO when its swapchain generation changes.
  `resize-after-record` also checks the queue's discarded-command counter;
  a clean empty submit cannot masquerade as successful old-generation rendering.
- Abort consumes acquire waits and the target render-finished signal before
  `vkReleaseSwapchainImagesEXT`; without maintenance1 it retires only the
  target surface's generations. This retirement fallback does not prove
  presentation completion as a general core-WSI guarantee. Recorded WSI work
  is accepted only for the same live generation, image and acquire serial.

## Repeated submits with automatic barriers (M2-C4)

`two-window-abort` checks the surviving B frame separately from the A/B
recovery frame. The latter records two independent clears and counts actual
`vkQueueSubmit` calls: exactly two, including `present()`. Every further
submit in an acquired batch restores the COLOR layout and stages its matching
PRESENT transition; already queued pairs are reused. This also covers the
ordinary two-window, two-submit recovery frame, independently of maintenance1.
The historical M2-G failures occurred in this recovery frame, after B's first
surviving frame had completed cleanly. See PROGRESS §2U.

## Completion backend (M3-D/E)

The engine defaults to timeline completion when `timelineSemaphore.enabled` is
true, otherwise fence completion. Debug builds accept
`CC_VULKAN_COMPLETION=auto|fence|timeline`; forcing `fence` preserves feature
enablement, while forcing `timeline` without the enabled feature fails bootstrap.
Unknown values fail explicitly. Set and restore this variable in the calling
shell; the existing scripts inherit it and do not modify it.

Every actual submit contains its binary WSI signals and a single completion
timeline signal. Timeline mode uses no submission fence, including immediate
transport and abort drain. Each slot waits for its actual queue points before
reuse. Present fences/semaphores retain their separate M2 lifetime rules.

Both WSI executables assert the actual backend and completed queue points, and
print `completion backend=... submits=... submission-fences=...`. These counts
exclude presentation fences and measure usage, not allocated object counts or
frame-time improvement. The recovery test still requires exactly two real
submits, preventing extra signal-only submissions from passing unnoticed.

Current limited Windows results: six lifecycle cases x two completion modes x
two automatic-barrier modes x two maintenance1 modes = **48/48 PASS**;
the five-tier WSI matrix and capability matrix also pass. Same effective 1.3
WSI workload: 232 actual submits in both modes, submission-fence counts 0/232.
Android compiles/links only. Recording ownership and staging content leases are
now connected to completion points (below); descriptor contents, split-event
lifetime, complete performance measurements and platform gates remain pending.

## Recording ownership and staging contents (M3-H/K)

`verify-recording-lifetime.ps1` checks six cases: a control submission, delayed
resource destruction, stale command-pool recordings, dropping a pending
recording, reusing the same facade, and executing a secondary recording.
The secondary case uses an empty body with inherited RP/FBO; it verifies
inheritance lifetime and real ExecuteCommands, not a resource used exclusively
by the secondary. Pending-drop requires both framebuffer and image-view
destruction after dropping; facade-rebegin checks survival after the new begin.

```powershell
./native/tests/vulkan-engine-smoke/verify-recording-lifetime.ps1 -SelfTest
./native/tests/vulkan-engine-smoke/verify-recording-lifetime.ps1
./native/tests/vulkan-engine-smoke/verify-staging-lifetime.ps1 -SelfTest
./native/tests/vulkan-engine-smoke/verify-staging-lifetime.ps1
./native/tests/vulkan-engine-smoke/verify-staging-lifetime.ps1 -Completion fence
./native/tests/vulkan-engine-smoke/verify-staging-lifetime.ps1 -Completion fence -ApiVersion 1.1
```

The staging executable has four cases in independent processes:

| Case | Required evidence |
|---|---|
| control | Real upload/readback submission, all 64 bytes equal 0xA5 |
| delayed-staging | CB begins in slot A and allocates staging in B; B rotates and allocates again; no pool reset, no overlapping live range, old recording forwarded, all bytes still 0xA5 |
| reuse-after-drop | Drop unsubmitted recording without a submit; rotate normally; same staging buffer/offset reused; new upload reads back 0x5A |
| reuse-after-completion | Submit and wait for actual completion retirement; rotate normally; same staging buffer/offset reused; new upload reads back 0x5A |

Each allocation view holds its block token. Recordings retain these views;
ended pending recordings retain them independently of the facade; successful
submissions retain recordings until their actual queue completion point.
The pool resets a block cursor only when it owns the last token reference.
Live blocks can append allocations beyond their current cursor. There is no
new submit or waitIdle. Block leases can increase peak memory while recordings
remain pending; existing allocation calls still require single-threaded access.
Current staging uses VMA CPU_ONLY, which requires HOST_VISIBLE and HOST_COHERENT
(vk_mem_alloc.h:488/3902). Readback nevertheless calls vmaInvalidateAllocation
after GPU completion; this is a no-op for coherent memory.

Both scripts require exit 0, enabled validation, unique clean footer, unique
zero-leak line and complete case evidence. Staging additionally requires all
64 readback bytes, a real successful submission and allocation reuse evidence.
Any VUID/hazard/FAIL/SKIPPED/INCOMPLETE rejects the case. Native hooks are installed
after device initialization and call the real functions. The scripts restore
completion/feature/API/layer/PATH environment variables and their working directory.

Lead verification: 4 staging cases x auto/fence/fence forced 1.1 = 12/12 PASS;
recording 6/6 PASS with strengthened assertions; staging parser 28 fixtures and
recording parser 26 fixtures behave as expected. This is finite Windows data
and lifetime evidence. Forced 1.1 is a tier test on the same modern GPU;
texture staging, descriptor rewriting, split events, captures, performance and
Android/XR runtime still need their own evidence.

## Split events and completion retirement (M3-L)

```powershell
./native/tests/vulkan-engine-smoke/verify-event-lifetime.ps1 -SelfTest
./native/tests/vulkan-engine-smoke/verify-event-lifetime.ps1
./native/tests/vulkan-engine-smoke/verify-event-lifetime.ps1 -Completion fence -Cases @('control-full','split-buffer-pair','unmatched-split-end','event-reuse')
./native/tests/vulkan-engine-smoke/verify-event-lifetime.ps1 -Completion fence -ApiVersion 1.1 -Cases @('control-full','split-buffer-pair','unmatched-split-end','event-reuse')
```

Five independent cases check FULL data dependency, matched buffer split
Set/Wait/device Reset, an unmatched split-end full fallback, event retirement,
and event cache reuse.
FULL/split readback depends on the engine barrier: the custom readback adds no
extra target-buffer barrier that could hide a broken engine dependency. Every
readback checks all 64 bytes after real completion and invalidate.

The held-observation case requires an actual timeline backend. It performs one
diagnostic raw queueWaitIdle to finish the GPU physically, then holds only this
queue's completion-counter observation at the last cached prefix. It proves the
hook was queried and the target batch survived. Facade destruction must issue
neither host ResetEvent nor DestroyEvent; restoring real observations must
retire and destroy the event exactly once. This is a synthetic completion
observation test with real Vulkan objects, not a GPU-in-flight VUID reproduction.
Requesting it with an unsupported backend fails explicitly.

CCVKGPUEvent is an owning device object retained by recording resources,
including event Set/Wait/device Reset. Successful submission batches preserve
these references until completion. The facade cache reuses a device-reset event
only when the facade is its sole owner; active/pending/in-flight recordings
block reuse. Destruction needs no host reset. Missing split-end uses the stated
full barrier, with its source access mask preserved. Producer/consumer submit
ordering for cross-recording split dependencies remains the caller's responsibility.

The event-reuse case ends two separate recordings on two DEVICE buffers before
submitting either. Their events must differ. Each queue->submit consumes one
pending recording, so transparent hooks prove three separate successful submits
for the first two pairs and a third pair recorded after the first two retire.
Per-pair Set/Wait/device Reset identities must match; the third pair reuses one
completed event without an extra allocation. Every submitted point must complete
and its owning batch retire. Facade destruction releases both cached events
exactly once. This case proves identity, execution and reuse; it does not test
buffer data contents.

Orca Worker supplied the fifth native case, three-backend runtime logs and
Android full-engine/probe compilation and final-link evidence (16 CCVKGPUEvent
symbols, no old glslang/OGLCompiler, no device runtime). Lead review corrected
completion observation, transparent hook restoration and parser counterexamples.
The parser now has 33 independent fixtures. The supported matrix is default
five cases and fence/fence forced 1.1 four cases; held is excluded from fence.
Forced 1.1 remains a tier test on a modern GPU. Texture split coverage, actual
GPU-in-flight event stress and Android/XR runtime remain unverified; the whole
modernization plan remains unaccepted for visual/performance/platform gates.

## Descriptor versions retained by recordings (M3-M)

```powershell
./native/tests/vulkan-engine-smoke/verify-descriptor-lifetime.ps1 -SelfTest
./native/tests/vulkan-engine-smoke/verify-descriptor-lifetime.ps1
./native/tests/vulkan-engine-smoke/verify-descriptor-lifetime.ps1 -Completion fence
./native/tests/vulkan-engine-smoke/verify-descriptor-lifetime.ps1 -Completion fence -ApiVersion 1.1
./native/tests/vulkan-engine-smoke/verify-descriptor-lifetime.ps1 -TemplateUpdates
```

Three independent processes use a real engine compute shader, two DEVICE input
buffers (64 bytes each of 0xA5 and 0x5A), a DEVICE output and HOST readbacks:

- `control` records and submits input A, then validates all 64 output bytes.
- `update-after-record` ends an input-A recording, updates the same DescriptorSet
  facade to input B without rotating the frame slot, and submits the old recording.
  Hooks must observe a different written Vulkan set; old output remains 0xA5.
- `update-during-record` dispatches input A, updates to B while still recording,
  and dispatches again without rebinding the facade. Version refresh must emit a
  new Vulkan binding; independent readbacks must contain 0xA5 and 0x5A.

The initial reliable baseline had a clean control and an actual invalid-command
red case: the same VkDescriptorSet was updated and the old VkCommandBuffer still
reached vkQueueSubmit (`VUID-vkQueueSubmit-pCommandBuffers-00070`). Its correct
readback did not make that submission valid. The first test-construction crashes
came from casting validator proxy objects to Vulkan objects; the harness now
creates objects through the real backend. Shader sources omit the #version that
the backend itself prepends.

Production descriptor instances now hold an owning CCVKGPUDescriptorVersion.
A recording retains the version and referenced shader resources. Updating an
externally retained version allocates another set from its layout's existing
pool and writes the new handle. The old version reaches the recycle bin only
after pending recordings are dropped or actual submitted completion points
retire. Unreferenced current versions remain eligible for ordinary in-place
updates. Draw/dispatch checks for a changed version even when the facade is the
same. The harness selects ordinary writes by default; `-TemplateUpdates` selects
the template branch before creating the test layout and requires an actual
non-null descriptor update template. Both modes now have three clean auto cases.
The hub caches its template PFN during device initialization, so post-init
ordinary-write hooks cannot observe template calls. In template mode the
`lastWritten` log field identifies the selected instance handle, rather than a
PFN-hook observation. Real bindings and the two 64-byte GPU readbacks prove
version separation and new descriptor contents without claiming such a hook.

The strict script requires actual bind/dispatch/attempt/success evidence,
64-byte data, unique clean footer/leak lines and enabled validation; 35 parser
fixtures include missing phases, invalid submissions and lost version refresh.
Every VUID/hazard/crash/incomplete case fails. Environment/cwd are restored and
logs have independent millisecond/PID names. Lead's final three-case auto run
passes with validation and leaks zero; existing recording six cases and default
WSI S1~S8 also pass after the core change. Orca Worker then ran all three cases
with fence/effective 1.3 and fence/forced 1.1 (six clean cases, timeline=0), and
rebuilt/linked the full Android engine and probe with eight descriptor-version
symbols and no old shader libraries. Android has compile/link evidence only.
Pending-GPU update stress, descriptor pool reuse, secondary shader bindings and
performance remain separate follow-up evidence. Additional versions
can increase descriptor usage; no speedup or whole-M3 acceptance is claimed.

[Ordinary descriptor update validity rules](https://docs.vulkan.org/refpages/latest/refpages/source/vkUpdateDescriptorSets.html)
explain why retaining the resource alone does not keep a recorded binding valid.

## Dynamic buffer instance completion (M3-M4, Lead)

```powershell
./native/tests/vulkan-engine-smoke/verify-descriptor-lifetime.ps1 -Cases delayed-dynamic-slot
./native/tests/vulkan-engine-smoke/verify-descriptor-lifetime.ps1 -Cases delayed-dynamic-offset -Completion fence
```

These explicit cases reuse the compute setup; the default suite still runs its
original three descriptor cases. A HOST|DEVICE input is written with 0xA5 in its
recording slot. Its CB is submitted from another slot after that other instance
is written with 0x5A. A test-only timeline wait is appended to the real submit,
preserving the tracker's values, binary semaphore positions and pNext chain.
The tracker must observe the submitted point pending. Its domain/value must
reach both the recording and submission slots, and leave an unrelated slot
unmarked (the test rejects globally serializing all slots). A host thread signals the
test gate after one second, bounding a correct slot wait. There is no diagnostic
queueWaitIdle or masked completion observation in these cases.

Empty presents rotate back to the original slot, and an empty user submission
triggers the real BufferHub copy. The initial baseline overwrote the original
instance while the gate remained closed and returned all 64 bytes as 0x5A;
Validation stayed clean. The fixed result is 64/64 bytes of 0xA5 followed by legal
reuse of the original instance for 0x5A. The offset variant uses a real nonzero,
aligned DYNAMIC_STORAGE_BUFFER binding and a 64-byte view; its requested offset
must be observed in vkCmdBindDescriptorSets and the readback must still match.
Both timeline and fence completion backends can run this test on a device with
the timeline feature enabled. Forced 1.1/default feature policy cannot run the
gate; it fails explicitly instead of counting as a passed case.

Recordings track physical buffer instance slots from actual descriptor offsets
(including dynamic offsets), vertex/index/indirect accesses and transfer ranges.
Secondary usages propagate to the submitting primary. Successful submits attach
the point to every affected slot as well as the submission slot. Present, mapped
Buffer.update and CPU BufferHub flush wait the relevant slot's points before
reuse; GPU-only copies retain queue ordering. Persistent buffer content updates
before submission retain their existing semantics; this introduces no immutable
snapshot of every Buffer.update. Raw customCommand buffer accesses require the
caller to provide equivalent lifetime/synchronization information.

CPU-written instances are aligned to both the uniform alignment and
nonCoherentAtomSize, and both engine mapped write paths call vmaFlushAllocation
(VMA makes this a no-op for coherent allocations). The diagnostic case also
flushes its explicit initialization to isolate lifetime from cache visibility;
it reports actual allocation coherence. Runtime coverage here used coherent
memory, so non-coherent hardware correctness remains unverified. Slot waits and
alignment padding can increase stalls/space; no performance benefit is claimed.
Parser fixtures also reject missing gate evidence, completed points, identical
record/submit slots, wrong reuse slots and missing/nonmatching dynamic offsets.

After the dynamic-instance core fix, Orca Worker ran recording lifetime's six
fence/forced-1.1 cases and default WSI S1~S8: strict checks and zero leaks/errors
passed. The full Android engine/probe rebuilt and linked (two range-tracking
symbols, no old shader libraries); this is compile/link evidence without a
device. Lead reviewed the six runtime logs, WSI footer and fresh Android artifact.
The complete M3 benchmark and platform/visual gates remain open.

## Completion clear microbenchmark (M3-M6, Lead)

```powershell
./native/tests/vulkan-engine-smoke/verify-completion-benchmark.ps1 -SelfTest
./native/tests/vulkan-engine-smoke/verify-completion-benchmark.ps1
# M4-A: four encoder pairs x two completion backends, repeated serially:
./native/tests/vulkan-engine-smoke/verify-completion-benchmark.ps1 -EncoderMatrix
# One chosen encoder pair (still compares both completion backends):
./native/tests/vulkan-engine-smoke/verify-completion-benchmark.ps1 -Submit submit2 -Barrier sync2
# A separate validation-OFF build, with CC_DEBUG_FORCE=ON for backend controls:
./native/tests/vulkan-engine-smoke/verify-completion-benchmark.ps1 -BuildDir <OFF-build> -Validation off -EncoderMatrix
# After a complete encoder matrix passes, retain all samples and compare within backend/repeat:
python native/tests/vulkan-engine-smoke/analyze-completion-benchmark.py <artifact-root>
```

The default comparison uses effective Vulkan 1.3, 300 warmup frames, 1000 measured
frames and five repeats per backend, alternating fence/timeline order. Each run
has a unique log and per-frame CSV; summary.csv reports mean, nearest-rank
P50/P95/P99 and maximum. All reported aggregates are independently checked
against CSV. Crashes, errors, incomplete/duplicate footer, missing metadata,
changed device/surface settings, NaN/negative/missing samples and leaks fail.
Forty-two parser fixtures exercise metrics, phases, encoder identity and actual
dispatch proof. Positive fixtures cover both legacy and modern encoders.
`analyze-completion-benchmark.py` writes encoder-comparison.json, retaining
conventional median, nearest-rank P95/P99, population standard deviation and MAD
for all raw metrics/phases. It compares each encoder pair to legacy/legacy in the
same completion backend and repeat, records the dispersion of paired changes,
and flags median/P95 increases above 3% for investigation. It requires all eight
cases in each repeat and matching verified summary labels. Analysis output
explicitly leaves the performance Gate unaccepted; thresholds cannot identify a
cause from this workload alone.
The script restores process environment and cwd, and never changes the build's
validation policy. `-Submit` and `-Barrier` default to legacy and explicitly set
and restore both debug environment switches. `-EncoderMatrix` cannot be combined
with those selectors; it selects all four encoder pairs, rotates the starting
case per repeat and reverses alternate repeats, with no concurrent GPU workload.
summary.csv includes Submit/Barrier labels; each log and CSV filename includes
both. PFN hooks cover legacy/core/KHR submissions and barriers. The measured
window plus warmup must have exactly one actual submit call per frame through
the selected route, positive barrier calls through only the selected route, and
a unique matching encoder marker. Calls before the completed setup and after the frame loop are excluded
from this dispatch proof; lazy initialization during warmup remains counted. Counter-hook overhead is included on both paths.
A successful script exit proves complete, internally consistent data and finite
correctness; it does not assert the plan's performance non-regression Gate.
Validation OFF must use a separate CMake build; ON evidence
and OFF five-repeat evidence now exists (PROGRESS §2AU/AV/AW).

This is a finite color+depth clear microbenchmark. Initialization and final waits
are excluded, every frame has one actual submit, and no per-frame waitIdle or
extra signal-only submit is added. GPU queries measure TOP-to-BOTTOM of the user
clear CB, not presentation or transport CBs; results include explicit availability
and valid-bit wrap handling. Actual present mode, extent, queue timestamp support,
image/slot count, maintenance policy, device and driver are logged. VSync OFF is
requested; the measured run reports actual IMMEDIATE mode. Instrumentation and
WSI pacing remain in frame wall time. GetThreadTimes measures this harness's
frame-flow thread; on this machine its approximately 15.625 ms quantization makes
single-frame CPU percentiles unsuitable for performance conclusions.

Object counts include all engine submission pools, the transport fence, all
queue completion semaphores (including idle queues) and the active WSI generation.
They count actual live allocations, preserving existing pool.size() used-count
semantics. Stable single-window runs have no retired generations. Host waits count
actual vkWaitForFences/vkWaitSemaphores calls and their API wall duration, excluding
status/counter polling. Occupancy polls real per-frame points after submission;
these observations update tracker completion caches and have measurement overhead.
It counts unfinished frames, rather than treating every queue submission as a frame.

Lead's ON five-repeat comparison passed all ten runs (10000 measured frames,
validation/leaks zero). Timeline used 3 fences versus 7 for fence mode and 12
semaphores versus 6; actual waits averaged about 1 versus 2 per frame. Frame-time
ranges overlap and fluctuate, while clear GPU spans are similar. These results do
not establish game speedup or complete M3 acceptance. Game cold/first-use,
profiler/variance attribution, OFF measurements, Android/XR runtime and visual
platform gates remain separate work. See PROGRESS §2AU for raw artifacts.

The benchmark now also emits `.csv.phases.csv` and five phase summaries for
acquire, record, submit, completion observation and present. Every phase sample
is finite/nonnegative, indices are continuous, and the five durations partition
the main frame wall time. The script independently checks phase statistics.
Lead's additional five-repeat ON/OFF data is recorded in §2AW; original raw runs
are preserved. Some OFF rounds exceeded the plan's 3% comparison threshold.
The present segment dominates this limited workload's cost and variation, but
it includes both driver presentation and engine cleanup, so this does not prove
a specific internal cause. The test's completion observer has layer-dependent
cost. Performance acceptance remains pending; no slow samples are discarded.
`dispersion.json` in the recorded profile roots adds conventional median,
population standard deviation and median absolute deviation from raw samples.
Unlike a shipping Release game, the OFF build retains CC_DEBUG_FORCE and the
engine's gfx debug validator. Android compiled the new read-only object getter
into libcocos_engine.a; the probe does not call it, so its final shared object
normally drops that definition during section GC.

## Submit2 semantic migration (M4-A, first step)

DEBUG `CC_VULKAN_SUBMIT=legacy|submit2` selects the submit encoder independently
of `CC_VULKAN_COMPLETION=auto|fence|timeline`. Default is legacy; selecting
Submit2 requires actually enabled synchronization2. Invalid modes and explicit
Submit2 without that feature fail initialization cleanly. Barrier2 remains OFF.
The present policy currently requests synchronization2 only at effective 1.3;
the lower-API KHR branch has not been demonstrated on a real device.

All three engine submit entries construct VulkanSubmitBatch, then the tracker
encodes one VkSubmitInfo or VkSubmitInfo2. Binary values are zero; the dedicated
completion timeline value is appended within the same actual submission, with
ALL_COMMANDS signal scope. Wait masks, command order and submission topology
are preserved. Failure publishes no point and retains pending recording leases.

WSI smoke hooks actual legacy/core/KHR dispatch after device initialization,
compares those counts with tracked submissions and rejects mixed/missing routes.
The benchmark branch uses its own transparent hooks and explicit encoder
selectors (see the microbenchmark section above). To select
a route for `verify-wsi.ps1`, use its explicit -Submit and -Completion options;
the script restores the process environment. Its forced-1.1/1.2 Matrix requires
legacy submit/barrier encoders, since those tiers currently do not enable Sync2.

Descriptor tests have an explicit `-Submit` option and include actual Submit2
hooks, including the real GPU timeline gate used for delayed dynamic slots:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-descriptor-lifetime.ps1 -SelfTest
powershell -NoProfile -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-descriptor-lifetime.ps1 -Submit submit2 -Completion fence
powershell -NoProfile -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-descriptor-lifetime.ps1 -Submit submit2 -Completion timeline -TemplateUpdates
# PowerShell -Command or an in-process invocation is needed for a multi-item Cases array.
& native/tests/vulkan-engine-smoke/verify-descriptor-lifetime.ps1 -Submit submit2 -Completion timeline -Cases @('delayed-dynamic-slot','delayed-dynamic-offset')
```

The shared log judge requires exactly one selected encoder marker with a positive
actual driver-call count and zero calls through the other route; its 42 fixtures
include missing/duplicate/zero/mixed/wrong-route negatives. Finite compute contents
and clear scenes do not establish identical game output or performance acceptance.
Full M4-A also needs Barrier2 migration and its own A/B regressions.

## Barrier2 semantic migration (M4-A, second step)

DEBUG `CC_VULKAN_BARRIER=legacy|sync2` independently selects barrier encoding.
Default remains legacy. Explicit sync2 requires actually enabled synchronization2;
unknown modes or disabled-feature requests fail initialization. Submit1+Barrier2
and Submit2+legacy barriers are both valid choices. No M4-B scope optimization is
included: stage/access aliases, dependency flags, layouts and ownership ranges
remain intact. Empty legacy execution-only barriers become a zero-access memory
barrier carrying the original stage scopes. Legacy event Set/Wait/Reset stay paired.

The 15 active .cpp barrier sites plus the currently uncalled debug full-barrier
helper now route through cmdPipelineBarrier. THSVS produces the same legacy
masks/layouts and is not rewritten. The uncalled bundled thsvsCmdPipelineBarrier
convenience implementation is retained; current engine code uses its conversion
helpers instead. In-render-pass barriers still require valid original self
dependencies. Barrier2 rejects unmodelled legacy pNext instead of dropping nodes.

WSI tests hook both actual barrier PFNs after initialization and require a positive
selected route count, zero other-route calls, clean teardown and complete S1..S8.
verify-wsi now explicitly selects/restores all three policies:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-wsi.ps1 -SelfTest
powershell -NoProfile -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-wsi.ps1 -Submit legacy -Barrier sync2 -Completion fence
powershell -NoProfile -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-wsi.ps1 -Submit submit2 -Barrier sync2 -Completion timeline
powershell -NoProfile -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-descriptor-lifetime.ps1 -Submit submit2 -Barrier sync2 -Completion timeline -TemplateUpdates
```

For full four-way A/B, run both Submit choices with both Barrier choices and each
completion backend. The forced-1.1/1.2 WSI Matrix must select legacy for both encoders
because current feature policy does not enable Sync2 in those tiers. WSI's 28
single-variable fixtures now enforce actual route counts, exact clean footer,
zero unique leak line and matching completion/fence count; descriptor's 46 fixtures
also enforce the selected barrier policy. The WSI script is UTF-8 with BOM for
Windows PowerShell 5.1. Event and lifecycle hooks now observe actual core Submit2
as well as legacy submit; lower-API KHR hooks/runtime remain separate pending work.

Finite clear/compute/split-event evidence is not complete game visual equivalence,
a performance claim or Android/XR runtime acceptance. Both modern encoders remain
debug opt-in until those gates are satisfied; M4-B has not started.

## Dynamic rendering clear comparison (M6-A0)

`vulkan_dynamic_rendering_smoke` + `verify-dynamic-rendering.ps1`: a real-GPU
diagnostic harness (offscreen, no swapchain). Classic clears via the engine's
`beginRenderPass`; the Dynamic branch is driven through `customCommand`
calling the REAL `vkCmdBeginRendering`/`vkCmdEndRendering`, consuming the M5
owning `VKRenderingInfoContract` and `VKRenderingTransitionContract` edges via
the unified `cmdPipelineBarrier` entry. Both paths transition explicitly and
read back through real `vkCmdCopyImageToBuffer` + HOST barrier + real submit +
`waitAllFences` + `vmaInvalidateAllocation`; every pixel is compared against
an exact golden RGBA(1,0,0,1) and against each other. Real-driver hooks count
BeginRendering/EndRendering/BeginRenderPass/EndRenderPass/copy/submit with
fixed capacity, forward FIRST, and restore via RAII. The global
`runtimePaths.useDynamicRendering` stays OFF (asserted) - this is NOT the
engine beginRenderPass being migrated; no PSO/draw is involved.

```powershell
# from the repository root (builds the current target first)
powershell -ExecutionPolicy Bypass -File native\tests\vulkan-engine-smoke\verify-dynamic-rendering.ps1
# subset / judge fixtures
powershell -ExecutionPolicy Bypass -File native\tests\vulkan-engine-smoke\verify-dynamic-rendering.ps1 -Cases classic
powershell -ExecutionPolicy Bypass -File native\tests\vulkan-engine-smoke\verify-dynamic-rendering.ps1 -SelfTest
```

rev2: validation counters are read from a LIVE context (budget fuse >=10
aborts INCOMPLETE); post-destroy counts come from the device's cached counter
(no use-after-free). Hooks register CB identity only on real SUCCESS; each
phase prints its own chain evidence (successful begin -> RP/Rendering calls ->
copy -> that CB submitted with SUCCESS) and any-capacity overflow is a FAIL.
The script isolates the policy environment (FORCE=1.3, COMPLETION=auto,
SUBMIT=legacy, BARRIER=legacy, disables cleared) so the vkQueueSubmit hook
observes the real path, and SelfTest carries 24 single-condition fixtures
including the four Lead red samples (zero-submits / stage trailing junk /
wrong diff byte count / nonzero validation line).

M6-A1 draw cases (`--case draw-classic|draw-compare`): a real engine Classic
PSO (procedural full-screen triangle via gl_VertexIndex, red fragment output)
is created with the existing RP; `draw-compare` additionally builds a matched
Dynamic variant INSIDE the real `vkCreateGraphicsPipelines` hook using the
M5-H adapter (value-copied createInfo, owned pNext chain, saved real PFN, no
recursion, failures recorded without rewriting the Classic result). Both
branches clear BLUE and must DRAW red; `--skip-draw` is the real negative
control (red versus blue differs in two channels per pixel and must exit 1).
The actual negative readback is 374/748 changed bytes for draw-classic,
or 748/1496 across both buffers for draw-compare. Hooks cover
BindPipeline/Draw/CreateGraphicsPipelines with per-branch CB identity chains
(begin -> RP/Rendering -> bind real pipeline -> draw(3) -> copy -> successful
submit). Diagnostic only: global dynamicRendering stays OFF; no production
hybrid path, no secondary/MSAA/DS/complex visuals; engine behavior unchanged.

Scope: attachment-contract consumption + clear/draw content comparison only.
Not yet: engine eligibility selection, secondary consistency, platform
visual/perf baselines, M5/M6 gates.

Lead review (2026-10-03): rev2 still sampled the cached `postErrors` before
device destruction and only counted RP/Rendering calls without their CB
identities. The current harness samples the cache after `device.destroy()`,
records begin/end CB handles, and verifies each full identity chain before
reading mapped data. Six additional single-condition judge fixtures initially
failed (exit 1) and now pass: null CB, an extra bad phase/validation/stage line,
the wrong submission count and an invalid capability value. SelfTest now has
30 fixtures. Empty `-Cases` entries fail; all evidence lines must be unique,
including malformed extra lines. The forced baseline is explicitly 1.3.
Current-source Classic/compare runs both return zero with live/post errors
zero and identical 748-byte readback. Earlier rev2 results remain historical;
they do not prove that the final device-destruction errors were checked.

M6-A1 Lead review (2026-10-03): the first draw revision appended its 12 new
fixtures after the execution loop, so its claimed 42-fixture result only
executed the previous 30. The loop now runs after all fixtures are assembled:
49 fixtures really execute. Seven new adversarial logs first failed the
SelfTest and now are correctly rejected (Dynamic bound handle mismatch,
extra bad draw evidence, disabled capability, null Dynamic pipeline, wrong
draw CB, wrong format, and missing pipeline signature). The judge ties each
draw CB and bound pipeline to the corresponding render phase and real created
pipeline, requires exactly one draw per phase, and rejects malformed duplicate
evidence. The harness checks the actual Dynamic createInfo tuple before the
driver call and records all four draw arguments. Current-source runs of all
four cases pass with live/post validation errors and byte differences zero;
the actual skip-draw negative exits 1 with 374/748 mismatches in draw-classic.
This still proves a diagnostic variant, not production hybrid selection.

## M6-A2a engine-owned pipeline variants

See above: owned-compare / owned-failure exercise the ENGINE-owned variant
with recording leases, pending-facade destruction evidence, injected partial-
output failure cleanup, and a DISCARD-op eligibility negative. Manual isolated
runs exit 0 CLEAN. The strict judge and Android build are now covered by the
Lead completion below; the earlier manual runs remain historical evidence.

Lead review (2026-10-03): the Dynamic branch now registers its gfx PSO lease
after begin. Before destroying the user PSO, another empty begin/end releases
the facade cache while leaving the earlier tested recording pending. The
distinct pending/fresh CB evidence and zero destruction counts therefore
exercise recording protection; the empty record is discarded without submit.
The draw hook checks the last pipeline bound at the actual draw, and the
creation hook reads the real Dynamic pNext/multisample tuple instead of
printing the PSO metadata. The eligibility create is explicitly scoped and
all creation counts are exact. The injected failure records driver SUCCESS,
returned error, partial output cleanup and RP fallback. Current-source owned
runs still pass with clean live/post counters and one final destruction per
owned handle. The strict script and Android regression were completed next.

The engine graphics factory retains its Classic pipeline and can prepare a
matching Dynamic variant from the same creation state only after the Classic
driver call returns VK_SUCCESS with a non-null output. The initial RP
candidate requires one actual color slot, CLEAR/STORE, sample1, viewMask0,
no depth/stencil/input/resolve/preserve/FSR/dependencies, and explicit
NONE-to-COLOR_ATTACHMENT_WRITE access semantics. This only checks the RP
candidate; FBO, texture and secondary eligibility still belong to later CB
selection. Variant failure preserves Classic, cleans a partial non-null
output, and keeps that RP on Classic. Both successful handles retire through
the existing GPU PSO references and completion-controlled recycle bin.

`owned-compare` temporarily sets the runtime flag only during factory creation,
restores it before recording, and draws the two engine-owned handles through
diagnostic custom commands. `owned-failure` first really creates a variant,
then explicitly injects error -3 while keeping the output, verifying cleanup
and a usable Classic pipeline. This is an injected failure, not a driver OOM.
Its independent DISCARD RP is a real Classic eligibility negative. Device
initialization and the production CB rendering selector remain unchanged.

```powershell
# Current-source build followed by all six independent GPU cases:
powershell -NoProfile -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-dynamic-rendering.ps1
# Owned cases only; parser fixtures run without a GPU:
powershell -NoProfile -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-dynamic-rendering.ps1 -Cases owned-compare,owned-failure
powershell -NoProfile -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-dynamic-rendering.ps1 -SelfTest
# Android arm64 full-engine compile and final-link probe; no device run:
powershell -NoProfile -ExecutionPolicy Bypass -File native/tests/vulkan-android-build/build.ps1
```

The shared judge now checks exact creation/retirement counts, pending/fresh
CB identity, held/created/bound pipeline identity, injected partial cleanup,
real eligibility creation, golden/diff bytes and live/post/leak counters.
It rejects duplicate or malformed extra evidence and evidence from another
case. Hex handles are compared by value despite case, padding or a 0x prefix.
Lead added 33 fixtures to the previous 49: all 82 actually execute. Six new
adversarial samples exposed false acceptance before the parser fixes and are
now rejected. Six real cases, default WSI/capability regressions and Android
compile/link pass. No game visual/performance or mobile-runtime Gate follows
from this finite offscreen evidence; production Hybrid selection is pending.

## M6-A2b initial offscreen Hybrid path

Debug builds accept `CC_VULKAN_RENDERING=classic|dynamic`. The default stays
Classic. Dynamic requires core 1.3 and an enabled feature; unknown values or
unsupported explicit requests fail initialization. The first engine selector
accepts only an inline primary pass with one ordinary offscreen TEX2D color,
CLEAR/STORE, sample1, one mip/layer, initially no depth/stencil/resolve or special flags,
matching formats and valid extent. Secondary, multi-subpass, input/preserve,
FSR, XR/WSI and runtime autoBarrier use Classic.

Owning attachment preparation precedes the explicit pre-transition and real
BeginRendering. EndRendering precedes the explicit post-transition. The
texture access record and GPU recording leases are preserved. Graphics binding
tracks the actual VkPipeline and compares the complete rendering signature.
A pre-bound incompatible PSO chooses Classic; a later incompatible PSO skips
draw while the Dynamic instance still ends normally. These checks follow the
[BeginRendering requirements](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBeginRendering.html)
and [draw pipeline requirements](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDraw.html).

The harness `--engine-path` accepts only owned-compare/owned-failure. With
`CC_VULKAN_RENDERING=dynamic`, these use ordinary gfx begin/bind/draw/end;
custom commands only perform readback. The comparison deliberately pre-binds
Classic before selecting Dynamic, so the draw hook must observe the rebind.
The failure case leaves policy ON and verifies automatic Classic fallback.
An empty procedural InputAssembler is valid without a vertex buffer; the
engine now omits the forbidden zero-count vertex-buffer bind.

Current direct runs require the same layer/DLL environment as the existing
script. Its default six diagnostic cases remain separate. The formal
`verify-dynamic-rendering.ps1 -EnginePath` option is available below.
The harness also accepts `--skip-draw`, `--reject-variant` or
`--reject-signature` for injected negative controls (the latter two only with
engine owned-compare, mutually exclusive). A negative exits 1 because blue
pixels fail the red golden; it must still show paired begin/end, real successful
submission and zero live/post errors. These injections do not simulate a real
driver refusing a pipeline. Finite readback is not a game visual/performance or
mobile/XR Gate.


## M6-A2b formal engine-path judging (`-EnginePath`)

`verify-dynamic-rendering.ps1 -EnginePath` builds the current target and runs
the REAL gfx hybrid path (`CC_VULKAN_RENDERING=dynamic` debug opt-in, restored
after the run) over `owned-compare` + `owned-failure` (the only cases wired to
the engine path), passing `--engine-path` to the exe and forcing
FORCE=1.3/COMPLETION=auto/SUBMIT=legacy/BARRIER=legacy. The judge reuses every
owned strict check and additionally requires runtime=1/caps=1 plus unique
exact `engine-path: gfxRendering=1 gfxDraw=1 runtimeDynamic=1 restored=1` and
per-case `engine-calls:` lines (compare: classic 1/1 dynamic 1/1 draws 2
submits 2/2; failure: classic 1/1 dynamic 0/0 draws 1 submits 1/1).
Diagnostic-mode logs carrying engine-path evidence are rejected.
The judge also rejects non-owned EnginePath cases independently of the CLI,
and counts malformed extra engine evidence even without a space after the colon.

```powershell
# from the repository root
powershell -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-dynamic-rendering.ps1 -EnginePath
powershell -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-dynamic-rendering.ps1 -EnginePath -Cases owned-compare
powershell -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-dynamic-rendering.ps1 -SelfTest   # 102 fixtures, 102 executed
powershell -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-dynamic-rendering.ps1              # default 6 diagnostic cases
```

Boundaries: the engine path is a debug opt-in only (default OFF, offscreen
single-color CLEAR/STORE 1x/viewMask 0, these cases have no D/S/resolve/secondary/FSR); the
three C++ negatives (--skip-draw / --reject-variant / --reject-signature,
clean exit 1 + paired end + Validation 0) were verified by the Lead and are
NOT run by this script; SelfTest judges forged engine-path samples only.

## M6-A3 engine depth/stencil comparison

`vulkan_dynamic_depth_stencil_smoke` uses ordinary gfx RP/bind/draw/end calls
for both branches. The debug policy remains explicitly enabled, with only the
Classic control's begin temporarily choosing Classic. The production selector
now also accepts a single CLEAR/STORE 1x D32_SFLOAT or D32_SFLOAT_S8_UINT
attachment beside the color attachment, with explicit pre/post transitions.
Other D/S formats, LOAD/DISCARD, resolve, autoBarrier, secondary, WSI/XR and
the earlier exclusions stay on Classic. The default rendering policy is OFF.

The harness queries the exact format/usage combination on its real GPU before
temporarily selecting D32/D32S8 in the backend's GFX format mapping. This is
test-only; production format priorities are unchanged (RTX's default combined
format D24S8 therefore retains Classic in this tier).

Case `depth` draws near red at depth 0.25, then far green at 0.75 with LESS.
Case `depth-stencil` additionally writes stencil 7 and attempts a nearer blue
draw requiring stencil 9. Hooks forward actual driver calls, check the real
Classic/Dynamic pipeline signature and depth/stencil states, and require each
draw's exact selected pipeline in order. Each branch copies its color, depth,
and optional stencil plane into a mapped host buffer, waits for completion,
invalidates the VMA allocation, and checks all 187 pixels (red / 0.25 / 7).
The final all-plane comparisons contain 1496 / 1683 bytes. All four / six real
pipeline handles must be destroyed exactly once during retirement.

```powershell
# repository root; configure process-local test policy, restore it on exit
cmake --build native/build/vulkan-modernization/win64-simulator --config Release --target vulkan_dynamic_depth_stencil_smoke
$names = @('PATH','VK_LAYER_PATH','VK_IMPLICIT_LAYER_PATH','CC_VULKAN_FORCE_API','CC_VULKAN_COMPLETION','CC_VULKAN_SUBMIT','CC_VULKAN_BARRIER','CC_VULKAN_RENDERING','CC_VULKAN_DISABLE_FEATURES')
$saved = @{}; foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$exe = (Resolve-Path native/build/vulkan-modernization/win64-simulator/vulkan-engine-smoke/Release/vulkan_dynamic_depth_stencil_smoke.exe).Path
$run = "$PWD/native/build/vulkan-modernization/ds-manual"
New-Item -ItemType Directory -Force -Path $run | Out-Null
try {
    $env:PATH = "$PWD/native/external/win64/libs;$PWD/native/external/win64/libs/v8/Release;" + $env:PATH
    $env:VK_LAYER_PATH = 'D:/Software/VulkanSDK/1.4.341.1/Bin'
    $empty = "$PWD/native/build/vulkan-modernization/empty-implicit-layers"
    New-Item -ItemType Directory -Force -Path $empty | Out-Null
    $env:VK_IMPLICIT_LAYER_PATH = $empty
    $env:CC_VULKAN_FORCE_API = '1.3'; $env:CC_VULKAN_COMPLETION = 'auto'
    $env:CC_VULKAN_SUBMIT = 'legacy'; $env:CC_VULKAN_BARRIER = 'legacy'
    $env:CC_VULKAN_RENDERING = 'dynamic'
    Remove-Item Env:CC_VULKAN_DISABLE_FEATURES -ErrorAction SilentlyContinue
    Push-Location $run
    try {
        & $exe --case depth
        & $exe --case depth-stencil
        # intentional failure: both paths must fail the golden, with Validation 0
        & $exe --case depth-stencil --skip-near
    } finally { Pop-Location }
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
}
```

Exit 0 requires complete CLEAN readback and zero live/post Validation errors;
exit 1 is failure; exit 3 is UNSUPPORTED and never a pass. The negative control
demonstrates that mutual equality alone is insufficient: both paths produce
the same wrong contents while the golden reports 1496 mismatches. Broad
memory ordering retains BestPractices access/layout warnings; synchronization
narrowing is outside this change. The formal PowerShell judge below builds
the current target and checks both cases; raw executable runs alone are not
its acceptance.
Finite NVIDIA rendering evidence and Android core compilation do not establish
AMD rendering, mobile/XR, MSAA/resolve, visual/performance or capture Gates.

## M6-A3 depth/stencil hybrid verification (verify-depth-stencil.ps1)

Formal judge for the single-sample D32/D32S8 production-hybrid harness
(`vulkan_dynamic_depth_stencil_smoke`). Builds the CURRENT target first (a
red build fails immediately; a stale exe is never judged), then runs each
case in its own ignored cwd/log with the policy isolated to
FORCE=1.3/COMPLETION=auto/SUBMIT=legacy/BARRIER=legacy/RENDERING=dynamic and
DISABLE cleared; every environment variable is restored by existence in a
try/finally along with the working directory.

`Test-DepthStencilLog` is the single shared judge for real runs and fixtures:
exit must be 0 (3 = UNSUPPORTED is not PASS), validation triad ENABLED unique,
one exact all-zero CLEAN footer (bad/duplicate/foreign footers never hide),
unique exact `ds-config` / `ds-pipelines` / `ds-compare` / `ds-retirement`
lines with the case-exact format (126/130), stencil (0/1), byte totals
(1496/1683), pipeline counts (2/3) and created==destroyed (4/6); exactly two
`ds-phase` lines (classic rp 1/1 dynamic 0/0, dynamic the reverse, non-zero
CBs, draws 2/3, copies 1/1/0 or 1/1/1, submits 1/1 chain=1) and two
`ds-readback` lines whose THREE golden mismatch fields are all exactly 0 -
mutual path diff 0 alone can never pass (the skip-near negative has
diff bytes=0 while golden mismatches are non-zero). printf handles without
0x are accepted but must be non-zero; whole-line anchoring rejects 01/00
prefix-number tricks. Reserved-prefix totals are counted BEFORE exact
matching, so malformed/no-space duplicates of every evidence prefix and ANY
footer-like line (foreign/numeric/underscore/no-space/truncated case names)
reject the log; timestamped `] FAIL:` lines are also rejected. Two sequential
recordings MAY legally reuse the same VkCommandBuffer handle (the C++ harness
proves phase membership via real per-stage hooks), so distinct CBs are not
required - only non-zero handles. Any VUID/SYNC-HAZARD/FAIL/SKIPPED/INCOMPLETE/UNSUPPORTED or
non-zero validation rejects the run; known BestPractices WARNINGs from the
current coarse barriers are tolerated.

```powershell
# from the repository root
powershell -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-depth-stencil.ps1
powershell -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-depth-stencil.ps1 -Cases depth
powershell -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-depth-stencil.ps1 -SelfTest
```

Boundaries: query != render; the real render evidence covers the RTX device
(the AMD iGPU is query-only in this workspace). G5 capture / G6 mobile XR /
MSAA / resolve are NOT verified; M6 has no overall gate. The C++ `--skip-near`
negative control is not run by this script; its recorded log is used only to
confirm the judge rejects that shape.

Lead review added a bare extra footer (`depth-stencil depth completed:`):
the rev2 judge incorrectly ignored it when no failures/outcome field was
present. Footer counting now includes the reserved prefix or any
`completed:` line, before exact CLEAN matching. The current SelfTest executes
46 fixtures; sequential reuse of a non-zero CB handle remains legal.

## M6-A4 color MSAA resolve preparation

The experimental Hybrid candidate now accepts one RGBA8 draw slot at 2x or
4x with a matching 1x color resolve attachment: attachment indices 0/1,
subpass colors `{0}` and resolves `{1}`, primary CLEAR/STORE and resolve
DISCARD/STORE, explicit NONE -> COLOR_ATTACHMENT_WRITE barriers. D/S,
secondary, input/preserve attachments and other resolve formats remain
outside this first resolve tier. Both views must be ordinary, allocated,
single-layer 2D images of matching extent and format.

The Dynamic attachment publishes AVERAGE resolve with the real 1x view, and
both source and resolve images receive explicit pre/post transitions. This
follows the non-integer color resolve rules in
[VkRenderingAttachmentInfo](https://docs.vulkan.org/refpages/latest/refpages/source/VkRenderingAttachmentInfo.html).
Runtime rendering remains opt-in and defaults to Classic. The dedicated
draw/edge/readback harness and formal judge below cover finite 2x/4x RTX
content comparisons; compilation alone does not establish that equivalence.

## M6-A4 real color MSAA resolve harness (msaa_resolve_smoke.cpp)

`vulkan_msaa_resolve_smoke` exercises the engine's first MSAA Dynamic hybrid
path on the real GPU: a primary RGBA8 MSAA texture (2x/4x) plus a 1x RGBA8
resolve target, one engine RenderPass with `colors={0} resolves={1}` (main
CLEAR/STORE, resolve DISCARD/STORE, barriers prev NONE -> COLOR_ATTACHMENT_
WRITE), two engine PSOs (uniform full-screen red on blue clear; lower-left
diagonal red triangle). rev2 runs FOUR independent phases - {classic, dynamic}
x {uniform, edge} - each with its own begin/submit/end cycle, own phase
record, and its OWN 748-byte owned snapshot saved immediately after a
successful submit + wait + invalidate (a later phase can never rewrite an
earlier snapshot). BOTH patterns copy the 1x resolve image to host after an
explicit COLOR_ATTACHMENT -> TRANSFER_SRC barrier plus a TRANSFER_WRITE ->
HOST_READ buffer barrier. The uniform snapshot is checked byte-by-byte
against the exact red golden (255,0,0,255); the edge snapshot applies
per-pixel rules (pixel-center margins via (x+.5)/W, g==0, a==255, r+b~=255,
mixed-channel round(255*k/N)+-1 quantization, interior red / exterior blue).
Two mutual compares (uniform and edge separately) must both be 0/748. An
explicit broad ALL_COMMANDS memory barrier is recorded between the two render
passes on the same images (manual caller-side attachment-reuse dependency).
`--skip-draw` and `--wrong-uniform` are real negative controls: all four
phase chains complete and both mutual diffs stay 0, but the uniform golden
fails on actual bytes - proof that a two-path diff alone is insufficient.

```powershell
$repo='E:\AIProMax\github\cocos4-vulkan'
$exe="$repo\native\build\vulkan-modernization\win64-simulator\vulkan-engine-smoke\Release\vulkan_msaa_resolve_smoke.exe"
cmake --build "$repo\native\build\vulkan-modernization\win64-simulator" --config Release --target vulkan_msaa_resolve_smoke
$old=@{}; foreach($n in @('PATH','VK_LAYER_PATH','VK_IMPLICIT_LAYER_PATH','CC_VULKAN_FORCE_API','CC_VULKAN_COMPLETION','CC_VULKAN_SUBMIT','CC_VULKAN_BARRIER','CC_VULKAN_RENDERING','CC_VULKAN_DISABLE_FEATURES')){$old[$n]=[Environment]::GetEnvironmentVariable($n)}
$oldCwd=Get-Location
try {
  $env:PATH="$repo\native\external\win64\libs;$repo\native\external\win64\libs\v8\Release;"+$env:PATH
  $env:VK_LAYER_PATH='D:\Software\VulkanSDK\1.4.341.1\Bin'
  New-Item -ItemType Directory -Force -Path "$repo\native\build\vulkan-modernization\empty-implicit-layers" | Out-Null
  $env:VK_IMPLICIT_LAYER_PATH="$repo\native\build\vulkan-modernization\empty-implicit-layers"
  $env:CC_VULKAN_FORCE_API='1.3'; $env:CC_VULKAN_COMPLETION='auto'; $env:CC_VULKAN_SUBMIT='legacy'
  $env:CC_VULKAN_BARRIER='legacy'; $env:CC_VULKAN_RENDERING='dynamic'
  Remove-Item Env:CC_VULKAN_DISABLE_FEATURES -ErrorAction SilentlyContinue
  $root="$repo\native\build\vulkan-modernization\msaa-run-"+(Get-Date -Format 'yyyyMMdd-HHmmss')
  foreach($case in @('2x','4x')){
    $dir="$root\$case"; New-Item -ItemType Directory -Force -Path $dir | Out-Null; Set-Location $dir
    & $exe --case $case *> "$dir\log.txt"; Write-Host "$case exit=$LASTEXITCODE"
  }
  foreach($neg in @('--skip-draw','--wrong-uniform')){
    $dir="$root\"+$neg.TrimStart('-'); New-Item -ItemType Directory -Force -Path $dir | Out-Null; Set-Location $dir
    & $exe --case 2x $neg *> "$dir\log.txt"; Write-Host "$neg exit=$LASTEXITCODE"
  }
  Write-Host "root=$root"
} finally {
  foreach($n in $old.Keys){ if($null -ne $old[$n]){[Environment]::SetEnvironmentVariable($n,$old[$n])}else{Remove-Item "Env:$n" -ErrorAction SilentlyContinue} }
  Set-Location $oldCwd
}
# expected: 2x exit 0, 4x exit 0, --skip-draw exit 1, --wrong-uniform exit 1
```

Boundaries: evidence is Classic/Dynamic resolve CONTENT equality on this RTX
device only, with independent submissions and full waits. This does not
establish same-CB continuous-pass reuse, a performance improvement or a
mobile/XR platform Gate.

## M6-A4 formal MSAA verification

`verify-msaa.ps1` builds the current target before each real run, isolates
FORCE=1.3 / COMPLETION=auto / SUBMIT=legacy / BARRIER=legacy /
RENDERING=dynamic, and restores process environment and cwd in finally.
Each case gets a unique ignored log/cwd. `Test-MsaaLog` is the shared real
and fixture judge: exit0, one Validation triad ENABLED record, one exact
zero-count CLEAN footer, correct sample/configuration/pipeline/retirement
records, four ordered mode/pattern chains with successful submits, four
independent complete content checks, and two separate zero-difference
comparisons. Uniform must be red187/blue0/mixed0; edge must contain all
three classes and their counts must sum to187. Non-zero reused CB handles
are legal. Missing/extra/malformed records, any VUID/hazard/FAIL/incomplete
line or non-zero leak fail. Known BestPractices warnings are tolerated.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-msaa.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-msaa.ps1 -Cases 2x
powershell -NoProfile -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-msaa.ps1 -SelfTest
```

SelfTest runs 68 CPU fixtures, including clean/crash logs, malformed extra
records, independent golden/compare errors, partial checking, a binary edge
with a complete pixel total, footer prefix tricks and legal handle reuse.
The full negative controls remain raw executable options in the preceding
command block; their actual logs fail the shared judge even if the caller
passes a forged exit0. UNSUPPORTED exit3 never counts as PASS.

Lead added actual checked-byte counters, positive covered/uncovered/mixed
edge assertions, per-phase hook/Validation guards and clean capability /
resource failure paths. These harness changes leave production untouched.


## M6-A5 initial D32 resolve wiring (historical implementation boundary)

The optional engine candidate now includes RGBA8 2x/4x color resolve plus
matching D32 multisample depth and a 1x D32 depth resolve target. It uses the
existing FramebufferInfo.depthStencilResolveTexture and saves Classic's actual
filtered per-subpass resolve modes. Dynamic consumes the saved SAMPLE_ZERO
depth mode; no stencil attachment is published. Combined D/S and other depth
modes keep Classic. Default rendering policy remains Classic.

Depth clear values use colorAttachments.size() rather than fixed index 1.
The source and target receive separate owned transitions, including
COLOR_ATTACHMENT_OUTPUT/COLOR_ATTACHMENT accesses for fixed-function depth
resolve as well as EARLY/LATE D/S accesses. See the
[Khronos resolve synchronization rules](https://docs.vulkan.org/spec/latest/chapters/renderpass.html#renderpass-resolve-operations).

At the initial wiring checkpoint, existing single-sample D/S and color-MSAA
scripts passed and Android compiled/linked, while a dedicated depth-resolve
golden readback was pending. The subsequent harness and revisions below now
provide finite RTX content evidence; the initial regressions alone did not.

## M6-A5 D32 depth-only SAMPLE_ZERO MSAA resolve (verify-depth-resolve.ps1)

`vulkan_depth_resolve_smoke` exercises the engine's first depth-resolve
Dynamic hybrid tier: four attachments (RGBA8 MS color 0, RGBA8 1x resolve 1,
D32 MS depth 2, D32 1x depth resolve 3) with `colors={0} resolves={1}
depthStencil=2 depthStencilResolve=3 depthResolveMode=SAMPLE_ZERO`. Two PSOs
(near red z=0.25 LESS wins; far green z=0.75 rejected), blue color clear +
depth clear 1. Each phase (Classic with policy guard / Dynamic engine path)
draws both PSOs, then copies BOTH 1x resolve images (color 748 bytes red
golden + depth 748 bytes float 0.25 golden) to host after explicit image and
buffer barriers, snapshots the bytes, and reports per-phase goldens plus two
independent mutual compares. Hooks verify per-phase CB identity chains, the
Dynamic depth resolve node (engine-filtered mode from
`depthStencilResolveModes`, real views, DEPTH_STENCIL_ATTACHMENT_OPTIMAL
layouts, depth clear 1.0, no stencil node), and Classic/MSAA pipeline
signatures (color 37 / depth 126 / stencil UNDEFINED / samples 2 or 4).
`--skip-near` is the negative control: both chains complete, mutual diffs
stay 0, but both goldens fail on real bytes (color 374/mode, depth 187/mode).

```powershell
powershell -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-depth-resolve.ps1
powershell -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-depth-resolve.ps1 -Cases 4x
powershell -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-depth-resolve.ps1 -SelfTest
```

The judge requires exact all-zero footers, unique evidence lines with
reserved-prefix counting (malformed/no-space duplicates reject), both phase
chains complete, both readbacks with all-golden-zero, both compares 0/748,
retirement 4/4, leak 0; VUID/HAZARD/FAIL/timestamped FAIL/UNSUPPORTED/
INCOMPLETE all reject. 29 SelfTest fixtures include two literal valid bases
(from the real first RTX runs) and a skip-near real-shape multi-condition
negative. Scope: RTX content equivalence for this tier only; combined D/S,
other modes, MSAA feature/perf/platform gates remain unverified.

rev2: the judge's footer counting now uses a broad reserved-prefix rule (any
`completed:` or `^dsr .* completed` line counts) so bare/truncated/foreign/
no-space footers all reject; FAIL is caught in ANY format including the
engine logger's `[INFO]: FAIL:` and bare timestamped forms; the validation
line is counted by reserved prefix before the exact-ENABLED match (a
DISABLED or duplicated/malformed variant rejects). The C++ harness adds the
missing `vkGetPhysicalDeviceImageFormatProperties` PFN check, classifies
sample-bit gaps as UNSUPPORTED (exit 3) vs unexpected Vk errors (exit 1),
NULL-guards the RP/FBO facade before use, adds NULL payload/count guards to
the hooks (submit infos/pCommandBuffers, create stype/payload), verifies copy
sub-resource origin (offset 0/mip 0/layer 0), computes the golden checked
counts in real loops (not printed constants), and makes the resolve readback
barrier an explicit broad ALL_COMMANDS / MEMORY_READ|WRITE image barrier
covering both classic D/S store writes and the fixed-function resolve's
COLOR_ATTACHMENT writes. Phase chain integrity now includes no-new-failures
and zero live validation errors; a failed chain cannot print chain=1.
SelfTest 38 fixtures (29 rev1 + 4 Lead red samples + 5 additional).


Lead completion of rev3 (Orca Worker quota interruption):
- Exact Classic RP/FBO and Dynamic primary/resolve views, root area and clear
  values are checked. Classic pipeline creation records the real RP identity.
- Each phase observes CB begin, active rendering, draw, rendering end, both
  copies and successful forwarding of that CB in order. Failed assertions or
  live validation errors force chain=0. Legal CB reuse between phases remains
  allowed. All queried resource pointers are checked before dereferencing.
- Empty Cases collections are rejected before building: the previous runner
  could report PASS after executing no cases. Real CLI red/green evidence is
  retained in the ignored build directory. The shared judge still has 38 fixtures.
- Current-source 2x/4x formal runs and four fence/legacy or auto/Sync2 runs pass;
  skip-near preserves both complete chains and mutual diff0 while failing both
  independent goldens. Unknown and incomplete CLI arguments exit 1.

The historical intermediate hazard log is
`dsr-matrix/run-20261003-191636-274-42236/2x/log.txt`. Its two errors identify
layout-transition writes preceding color/depth copy reads. It proves a broken
intermediate dependency, not that the original clean narrow-barrier run was
invalid or that a D/S store was the isolated root cause. Current explicit broad
readback barriers pass; scope optimization and driver-independent causality
are not claimed. No production change was made by these harness revisions.

## M7-A Pipeline Cache recovery (Lead implementation)

`vulkan_pipeline_cache_smoke` creates a real compute PSO using the engine cache
wrapper, saves to its isolated working directory, restores actual driver bytes,
checks shutdown-save, and tests four damaged/foreign files as cold misses. The
get-data failure and cached-create rejection are injected before calling Vulkan;
all other pipeline/cache operations call the real driver. The wrapper's default
persistence remains OFF; production opt-in is `-DCC_VULKAN_PIPELINE_CACHE=ON`.
When enabled, dirty data is saved explicitly, at clean shutdown, and at 30-second
serial checkpoints in present. No concurrent create/save contract or speedup claim.

Lead current-source Windows build/run passed (Validation/live/post=0, leak=0):
`native/build/vulkan-modernization/cache-lead-20261003-203652-375/log.txt`.
Formal verification and Android compile/link were pending at this Lead implementation
time point; M7-A1 rev2 below records their completion. Run this executable
in a new child of the ignored build directory: it deliberately writes damaged
fixtures to `pipeline-cache-suite.bin` in its current working directory. The v2
format rejects legacy/future versions and payloads above 64 MiB. Checksum is for
accidental corruption, not authentication; POSIX directory fsync is not implemented.
M7 hash review, serial prewarm, cold-start measurements and platform Gate remain.

## M7-A pipeline cache persistence (verify-pipeline-cache.ps1)

Formal judge for `vulkan_pipeline_cache_smoke` (v2 48-byte header + 64 MiB
payload cap + driver/vendor/device/UUID + original 32-byte Vulkan header +
FNV1a integrity check, atomic temp-write replace, 30s serial checkpoint,
default OFF via CC_VULKAN_PIPELINE_CACHE). Builds the CURRENT target first;
runs in its own isolated cwd (the exe writes a pipeline-cache-suite.bin);
one strict shared judge requires: unique `cache validation: ENABLED`;
exactly three `cache-stage` evidence lines with exact full-line values
(restored: realPipeline=1 cachedCreates=1 preserved=1 / cold-recovery:
cases=4 coldCreates=5 / rejected: rejected=1 coldCreates=6); one exact
all-zero `cache-suite completed:` footer (malformed/foreign/truncated
footers reject); unique zero memory-leak line; VUID/HAZARD/FAIL (any
format)/SKIPPED/INCOMPLETE/UNSUPPORTED all reject. 24 SelfTest fixtures
(single-condition negatives: missing/duplicate footer, junk/foreign/
truncated/01 footers, per-field nonzero, NTSTATUS, missing/duplicate
validation, stage count wrong/trailing junk, leak nonzero/duplicate,
timestamped FAIL, VUID, SKIPPED, INCOMPLETE).

```powershell
# from the repository root
powershell -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-pipeline-cache.ps1 -SelfTest
powershell -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-pipeline-cache.ps1
```

Boundary: proves the persistence protocol end-to-end (cold start, preserved
corrupt file, cold recovery, corrupt rejection); does not change the default
(CC_VULKAN_PIPELINE_CACHE=OFF) or claim any M6/M7 gate.

Lead review: the garbage-leak fixture now replaces the original leak line instead
of appending another one; UNSUPPORTED has its own negative fixture. Current shared
judge SelfTest is 29 fixtures (28 Worker revision + 1 Lead addition). Android 603-step
engine/probe link and default capabilities passed; no Android device execution.

## Layout content keys (M7-B2)

`vulkan_engine_smoke` creates three real descriptor layouts and three pipeline
layouts. Fixed-capacity PFN hooks forward to the driver first, capture successful
creation parameters/handles, and compare them with the engine's owned metadata.
Equivalent binding permutations must have equal keys; changing binding content
or ordered sets must change equality. The success line is emitted only after all
checks pass:

`LAYOUT_KEYS: sets=3 pipelines=3 driver=SUCCESS canonical=OK ordered=OK`

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-capabilities.ps1 -Cases default
powershell -NoProfile -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-capabilities.ps1 -Cases force-1.1 -SelfTest
```

The script builds current source before running and requires exactly one valid
layout evidence line for every positive case. Six parser fixtures reject missing,
duplicate, malformed, wrong-count and trailing-junk evidence. Key construction
failure leaves invalid metadata and does not block driver creation. Existing pool
IDs and Vulkan calls remain in use; layout deduplication and prewarm are pending.
These host-side creation checks do not prove draw compatibility or performance.

## Final shader content snapshots (M7-C2)

The engine records an owned stage key from the same final SPIR-V words passed to
`vkCreateShaderModule`, after vertex input-location compression. The smoke hook
stays active through lazy GPU shader initialization, forwards to the real driver,
and checks two successful modules against their stored handles and full keys.
Every positive capability case requires one exact success line:

`SHADER_KEYS: stages=2 driver=SUCCESS code=EXACT ownership=OK`

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-capabilities.ps1 -Cases default,force-1.1 -SelfTest
```

The script builds current source first. Six parser fixtures reject missing,
duplicate, malformed, wrong-count and trailing-junk shader evidence. These checks
prove host-side creation and owned metadata, not full PSO deduplication, prewarm,
draw compatibility or performance. Snapshot allocation failure leaves invalid
metadata and preserves the existing Vulkan creation path. Lead built this revision
for Android arm64 on 2026-10-04: both shader contract/probe targets linked, and the
29-step engine build linked the final probe with eight modern shader libraries.
The actual VKCommands dependency block includes VKShaderStageKey.h. Logs are
`m7c3-lead-android-contract-build.log` and `android-m7c3-lead-full.log` under the
ignored modernization build directory. No Android device execution was performed.

## Real compute pipeline content keys (M7-D2)

The existing compute encoder stores a key from its creation flags, final shader
key and ordered layout key. A failed key allocation does not block driver creation.
The smoke creates three actual engine compute PSOs with one real empty layout:
two use the same shader; the third changes local_size_x from 1 to 2. Fixed-capacity
hooks capture two successful shader modules, the actual layout and three successful
compute pipeline creations. Each complete key is rebuilt from those submitted
values, compared with GPU metadata and checked for owned code storage.

`COMPUTE_KEYS: pipelines=3 modules=2 driver=SUCCESS equal=OK distinct=OK ownership=OK`

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-capabilities.ps1 -Cases default,force-1.1 -SelfTest
```

Every positive case requires one exact marker. Eight shared parser fixtures cover
missing/duplicate/malformed evidence, trailing junk, wrong counts, failed driver
status and bad equality. The current default/forced-1.1 run passed with Validation
zero (`m7d2-lead-verify.log`). This is host-side creation evidence, with all three
driver creates retained; no dispatch/readback, deduplication or performance claim.
Full graphics keys and prewarm are pending. Orca Worker built the current Android
engine and final probe on 2026-10-04 (29 actual steps, android-m7d3-worker-full.log).
Lead checked the VKCommands dependency block and eight modern shader libraries
in the final link. This proves compile/link only; no Android device was run.

## Real Classic RenderPass compatibility keys (M7-E2)

The engine records an owned conservative key from its final RenderPass2 input.
The existing two-subpass smoke intercepts the device's cached core/KHR creation
function, forwards to the actual driver and compares its successful handle and
complete input-derived key with GPU metadata. Unknown extensions or allocation
failure leave invalid metadata without blocking the original Vulkan creation.

`CLASSIC_RP_KEY: calls=1 driver=SUCCESS subpasses=2 content=EXACT ownership=OK`

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-capabilities.ps1 -Cases default,force-1.1 -SelfTest
```

The script builds current source first and requires exactly one valid evidence
line. Eight shared parser fixtures cover missing/duplicate/malformed evidence,
trailing junk, wrong counts, bad content and failed driver status. The current
default/forced-1.1 cases passed with Validation zero (m7e2-lead-verify.log), and
default WSI S1-S8 passed (m7e2-lead-wsi.log). Equal keys are conservative sufficient
matches for valid modeled inputs; unequal keys do not prove incompatibility.
These checks prove creation and owned metadata, not draw reuse or performance.
Android compilation of this production wiring is pending.

## Real graphics pipeline content metadata (M7-F3)

The final Classic encoder input is recorded as graphicsContentKey. A successfully
created engine-owned Dynamic variant records its own dynamicContentKey; a failed
or ineligible variant leaves that key invalid. Neither key gates driver creation
or enables CPU deduplication. Extra owning copies add unmeasured creation-time
work and memory; performance acceptance is pending.

The existing two-subpass smoke creates two real Classic PSOs. Its driver-first
hooks record final shader-module bytes, the actual empty layout, RenderPass2
input and each graphics creation input/output. The expected graphics key is
rebuilt from those records, without reading production child keys. It must match
the stored full key and hash, own independent state/code arrays, and differ
between the two subpasses. Every positive capability case requires one exact line:

`GRAPHICS_KEYS: pipelines=2 driver=SUCCESS content=EXACT subpasses=OK ownership=OK`

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File native/tests/vulkan-engine-smoke/verify-capabilities.ps1 -Cases default,force-1.1 -SelfTest
```

Current-source default/forced-1.1 checks passed with Validation zero and matching
SPIR-V 9/9 (m7f3-lead-capabilities.log). Eight shared parser fixtures reject
missing, duplicated, malformed, trailing-junk, wrong-count, failed-driver and
bad-subpass evidence. This is Classic creation and metadata ownership evidence;
it is not a direct Dynamic key comparison, draw reuse, prewarm or performance
proof. Orca Worker verified the production Android engine/probe link (29 steps,
android-m7f3-worker-full.log); this is compile/link evidence only. Lead checked
eight modern shader libraries in the final link. Dynamic direct-key evidence
is added below. The earlier M7-E2 production Android pending note is superseded by
M7-E3's actual engine/probe link recorded in progress §2CO.

## Dynamic input keys and empty fallback (M7-F4)

The owned cases rebuild expected keys from real final module bytes, layout and
RenderPass2 creation records plus each actual graphics creation input. They do
not read production child keys to build the expected value. The engine's full
Classic/Dynamic keys must match; word/code/format storage must be independent.
A failed optional variant must leave the complete default invalid Dynamic key.
Only successful assertions emit the unique evidence line:

`pipeline-keys: classic=EXACT dynamic=EXACT ownership=OK` (owned-compare)

`pipeline-keys: classic=EXACT dynamic=EMPTY ownership=OK` (owned-failure)

```powershell
# Direct PowerShell invocation; each formal run builds current source first.
& ./native/tests/vulkan-engine-smoke/verify-dynamic-rendering.ps1 -SelfTest
& ./native/tests/vulkan-engine-smoke/verify-dynamic-rendering.ps1 -EnginePath
& ./native/tests/vulkan-engine-smoke/verify-dynamic-rendering.ps1
```

The 112 fixtures include ten new single-condition key evidence negatives.
Current EnginePath two-case and default six-case runs passed (m7f4-lead-engine-
path-direct.log and m7f4-lead-diagnostic.log). --corrupt-key is an owned-compare
negative control under the same isolated policy environment: it clears only the
test's Dynamic CPU key after two successful real creates, then must exit 1 with
missing key evidence and FAILED footer. No Vulkan parameters are corrupted;
later drawing is stopped by the assertion failure. These tests prove finite
owned metadata and rendering/fallback behavior, not prewarm or performance.
The Worker F3 log named engine-path actually contains the six diagnostic cases;
the current direct EnginePath evidence is Lead execution, not Worker execution.

## M7-G2 pipeline_prewarm_smoke (Windows)

Real-engine experiment wiring the pure-CPU prewarm queue to the Cocos factory
and the engine pipeline cache, then restoring in a NEW process from the same
working directory (`pipeline-prewarm.bin`).

Build (current source; upstream CMake auto-reconfigures):
```powershell
cmake --build native\build\vulkan-modernization\win64-simulator --config Release `
      --target vulkan_pipeline_prewarm_smoke --parallel 4
```

Run cold and restored as separate processes from the same fresh directory.
The executable creates prewarm-cwd beneath that directory; negative mode uses
prewarm-cwd-wrong. Save/restore environment variables and cwd even on failure:
```powershell
$repo = (Get-Location).Path
$exe = Join-Path $repo 'native/build/vulkan-modernization/win64-simulator/vulkan-engine-smoke/Release/vulkan_pipeline_prewarm_smoke.exe'
$run = Join-Path $repo ('native/build/vulkan-modernization/prewarm-manual-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + $PID)
$names = @('PATH','VK_LAYER_PATH','VK_IMPLICIT_LAYER_PATH','CC_VULKAN_FORCE_API','CC_VULKAN_COMPLETION','CC_VULKAN_SUBMIT','CC_VULKAN_BARRIER','CC_VULKAN_RENDERING','CC_VULKAN_DISABLE_FEATURES')
$saved = @{}
foreach ($name in $names) { $saved[$name] = Get-Item "Env:$name" -ErrorAction SilentlyContinue }
$cwd = Get-Location
try {
    New-Item -ItemType Directory -Path $run | Out-Null
    $implicit = Join-Path $run 'empty-implicit'
    New-Item -ItemType Directory -Path $implicit | Out-Null
    $env:PATH = "$repo/native/external/win64/libs;$repo/native/external/win64/libs/v8/Release;$env:PATH"
    $env:VK_LAYER_PATH = 'D:/Software/VulkanSDK/1.4.341.1/Bin'
    $env:VK_IMPLICIT_LAYER_PATH = $implicit
    $env:CC_VULKAN_FORCE_API = '1.3'
    $env:CC_VULKAN_COMPLETION = 'auto'
    $env:CC_VULKAN_SUBMIT = 'legacy'
    $env:CC_VULKAN_BARRIER = 'legacy'
    Remove-Item Env:CC_VULKAN_RENDERING,Env:CC_VULKAN_DISABLE_FEATURES -ErrorAction SilentlyContinue
    Set-Location $run
    & $exe --phase cold *> cold.log
    if ($LASTEXITCODE -ne 0) { throw 'cold failed' }
    & $exe --phase restored *> restored.log
    if ($LASTEXITCODE -ne 0) { throw 'restore failed' }
    & $exe --phase cold --wrong-key *> wrong.log
    if ($LASTEXITCODE -ne 1) { throw 'wrong-key must reject cleanly' }
} finally {
    Set-Location $cwd
    foreach ($name in $names) {
        if ($null -eq $saved[$name]) { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }
        else { Set-Item "Env:$name" $saved[$name].Value }
    }
}
```

Behavior: cold requires the cache file absent and `loadedFromDisk=false`;
restored requires the file present, a validated non-empty payload, and
`loadedFromDisk=true` (the two compute PSOs are really created again in the
restored process; cache-hit creation savings are NOT claimed). Two shaders
(local_size_x=1/2) + one empty layout produce two distinct expected keys built
from real driver evidence; the queue dedups a third same-key job before any
factory call; drain(1) x2 yields exactly two real compute creates whose hooks,
cache handle, and production metadata keys all cross-check. `--wrong-key` keeps
the real PSO but corrupts the CPU result key so the entry fails and the run
exits 1. Unknown flags/missing values exit 1 cleanly. A unique footer line
`prewarm <phase> completed: failures=N liveErrors=N postErrors=N outcome=...`
is required.

Boundaries: no dispatch/draw, single-threaded, no file manifest beyond the
engine cache blob, raw chrono timing without attribution, not an M7 or
performance gate. The formal verify-prewarm.ps1 and Android wiring are
still pending.

### M7-G2 rev2 (reliability revision)

The initial draft overstated evidence (no cache-create hook, no Store::read
validation, no destroy hooks, bootstrap-time liveErrors in the footer). rev2
replaces all of it:

- Hooks forward-first with pre-driver guards; compute hook records the FULL
  actual root (sType/pNext/flags/layout/baseHandle/baseIndex) + stage info
  (sType/pNext/flags/stage/module/owned entry string/spec rejected) + per-call
  driver ns. vkDestroyPipeline/vkDestroyShaderModule/vkDestroyPipelineLayout
  hooks count destroys of exactly the created handles (exactly once each) and
  live across device.destroy (engine recycle bins flush under observation,
  global records only). The pipeline-cache create hook is installed only
  around configurePipelineCacheForPrewarm and observes the real
  initialDataSize/payload (cold: 0 bytes; restored: >0 with loadedFromDisk=1).
- Actual compute keys are rebuilt from the ACTUAL root + captured module words
  + actual layout record, then cross-checked against the manifest expected key
  (tuple + hash) and the production metadata key — three-way exact.
- PSOs are held by RAII from the first touch (allocation failures cannot leak).
- Queue closures are the unique owners of the input facades: external copies
  are released and observed via weak_ptr (alive during queue lifetime, expired
  after clear).
- Save validation uses VulkanPipelineCachePersistence::read(identity) with a
  non-empty payload (bytes reported are the validated payload size, not
  file_size).
- Unified footer `prewarm <phase> completed: failures=N liveErrors=N
  postErrors=N outcome=CLEAN|FAILED` with live sampled before device teardown
  and post from engine.getLastValidationErrorCount(); success requires
  failures==0 && live==0 && post==0. wrong-key keeps both real PSOs, fails the
  poisoned entry (ready=1 failed=1), uses the same unified footer, and exits 1.
- Precondition rejections before any device creation: cold with an existing
  cache file and restored with a missing file exit 1 without modifying the
  file (SHA-256 verified); duplicate --phase, unknown flags, and missing
  values exit 1 cleanly.

Formal verify-prewarm.ps1 and Android wiring remain pending.

M7-G2 rev3 (Lead): stack-free owning unique_ptr PSOs eliminate the allocation
window and released guard-wrapper leak. Captured stage flags and actual layout
flags participate in independent key reconstruction. Invalid hooks return
before calling the driver; all body exceptions release resources before device
teardown. Wrong-key is reported as one actual failed job and failures=1, using
the same live/post-clean footer. Both created PSOs stay alive until cleanup,
so retirement identities cannot be confused by handle reuse. No prewarm timing
benefit is inferred. Formal verification and Android evidence remain pending.

## M7-G3 verify-prewarm.ps1 (formal acceptance script)

Formal acceptance for the prewarm experiment. No -Cases parameter; only
-SelfTest. The formal mode always rebuilds the CURRENT target first (a red
build refuses to run a stale exe), then executes three independent processes in
a fresh `prewarm-matrix/run-<timestamp>-<pid>` parent cwd:
`--phase cold`, `--phase restored` (same cache file), and
`--phase cold --wrong-key` (separate `prewarm-cwd-wrong` directory).

SelfTest:
```powershell
& .\verify-prewarm.ps1 -SelfTest
```
40 fixtures (3 canonical positives taken from real logs + 37 single-variable
negatives): footer truncation/01/outcome/failures1/live1/post1/duplicate/
foreign-name, exit NTSTATUS/positive-nonzero/negative-0/2, validation
DISABLED/duplicate/missing-value/junk-tail, no-space and unknown prewarm
markers, unknownPhase, paths sync2=1, effective API 1.2, queue attempted=0,
manifest no-duplicate/beforeCreates=1, saved bytes=0/dirty=1, restored
payload=0/initialBytes mismatch, leak nonzero/duplicate/junk-prefix, VUID/
HAZARD/extra FAIL lines, wrong-mode other-FAIL/duplicate-FAIL/footer-failures0.

Formal:
```powershell
& .\verify-prewarm.ps1
```
Expected: build exit 0; cold exit 0 judge PASS (savedBytes parsed);
restored exit 0 judge PASS with initialBytes REQUIRED equal to the cold
process's saved payload (real driver input linked to the previous process);
wrong-key exit 1 judge PASS (sole `FAIL: prewarm rejected creator key`,
failures=1 liveErrors=0 postErrors=0 outcome=FAILED); overall exit 0 only if
all three judge PASS. Environment: SDK validation layer, engine DLLs on PATH,
empty implicit-layer isolation, FORCE 1.3, COMPLETION auto, SUBMIT/BARRIER
legacy, RENDERING/DISABLE cleared; every variable restored in finally.

Boundaries: the cache is a strict opt-in via the device's internal
configurePipelineCacheForPrewarm (default OFF); the restored process still
performs both real compute creates (cache hit does NOT reduce creates and no
such claim is made); compute-only, no dispatch/draw; no performance gate.
Android evidence is compile/link only (modern shader static libs in the final
link command, no shaderc, no device execution).

### M7-G3 parser rev2 (reliability revision)

Lead re-judgement with an AST-extracted Test-PrewarmLog found 7 false-accept
reds; all fixed (red discovery credited to Lead, fixes by Worker):

1. restored -ExpectedInitialBytes [long]999 was never compared (int-only
   check; the formal chain passes SavedBytes as long). Now any provided numeric
   type is compared; bad numerics return False instead of passing.
2. A junk-prefixed leak line passed (regex lacked ^). Leak lines are now wide-
   counted, must be unique, and are anchored as ^[optional timestamp] Total
   device memory leaked: 0 bytes.$
3. Appended no-space 'prewarmvalidation: DISABLED' passed. Every 'prewarm'
   -prefixed line must carry a known class token or be the exact footer; the
   total prewarm-prefixed lines must be exactly 11.
4. A truncated foreign footer ('foo completed failures=1') passed because only
   'completed:' was counted. Any line containing the word 'completed' is wide-
   counted (must be unique) and must match the full footer shape.
5. effective=1.30 passed via substring. The API line is reserved-unique and
   boundary-anchored with effective exactly 1.3 (loader/physical accept other
   valid digits).
6. sync2=01 passed via substring. The paths line is reserved-unique with the
   full boundary-anchored schema timeline=[01] sync2=0 submit2=0
   dynamicRendering=0 classicOverride=0 maintenance4=0.
7. A duplicated paths line passed (Select -First). Reserved markers must be
   unique.

Also: SavedBytes is reset per judge (a failed cold cannot leak stale bytes
into restored), saved/initial sizes parse as canonical positive Int64
(no leading zeros, no overflow), duplicated/empty input returns False, the
formal run now clears/restores CC_VULKAN_RENDERING (not PREFERRED_RENDERING)
and saves/restores every variable by existence, and an env-sentinel run proved
caller RENDERING/PREFERRED values survive.

SelfTest: 42 fixtures declared=executed=passed, exit 0. AST re-judge of the
Lead reds: valid=True, all 7 reds=False (m7g3-worker-parser-judge.json).
Formal: cold 0 / restored 0 / wrong 1, all judge PASS, overall exit 0.

M7-G3 rev3 (Lead): 67 current judge fixtures pass, with full beginning/end
anchors on API/paths/leak evidence and Int64 checks for all five timing fields.
All 42 rev2 fixtures remain; 25 fixtures restore missing coverage and test the
new failures. Formal cold/restored/wrong processes pass independently after
building current source. Caller rendering policy and unrelated sentinel env
are preserved. Android evidence from the Worker remains compile/link only;
no GPU performance, graphics prewarm or platform acceptance is inferred.

## PERF-A1 first-use measurement (Windows, Lead rev3)

T0 is main entry, not process creation. Ten fields measure backend initialization,
cache configuration, one swapchain, first lazy shader compilation, first graphics
factory/driver creation, and first-frame API completion. frame_flow covers the
whole Device::present slot wait/recycling; frame_to_present_api and
main_to_present_api end at the actual vkQueuePresentKHR return point. These spans
do not prove on-screen visibility or a game startup result.

Build current source first and refuse a failed build:

~~~powershell
cmake --build native/build/vulkan-modernization/win64-simulator --config Release --target vulkan_first_use_smoke --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Current first-use target build failed' }
~~~

Raw two-process cold/restored pair with environment restoration:

~~~powershell
$repoRoot = (Get-Location).Path
$runRoot = Join-Path $repoRoot ('native/build/vulkan-modernization/firstuse-raw-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $runRoot | Out-Null
$implicitDir = Join-Path $runRoot 'empty-implicit'
New-Item -ItemType Directory -Path $implicitDir | Out-Null
$names = @('PATH','VK_LAYER_PATH','VK_IMPLICIT_LAYER_PATH','CC_VULKAN_FORCE_API','CC_VULKAN_COMPLETION','CC_VULKAN_SUBMIT','CC_VULKAN_BARRIER','CC_VULKAN_RENDERING','CC_VULKAN_DISABLE_FEATURES')
$saved = @{}
foreach ($name in $names) { $saved[$name] = Get-Item -LiteralPath ('Env:' + $name) -ErrorAction SilentlyContinue }
try {
    $env:PATH = "$repoRoot/native/external/win64/libs;$repoRoot/native/external/win64/libs/v8/Release;" + $env:PATH
    $env:VK_LAYER_PATH = 'D:/Software/VulkanSDK/1.4.341.1/Bin'
    $env:VK_IMPLICIT_LAYER_PATH = $implicitDir
    $env:CC_VULKAN_FORCE_API = '1.3'
    $env:CC_VULKAN_COMPLETION = 'auto'
    $env:CC_VULKAN_SUBMIT = 'legacy'
    $env:CC_VULKAN_BARRIER = 'legacy'
    Remove-Item Env:CC_VULKAN_RENDERING -ErrorAction SilentlyContinue
    Remove-Item Env:CC_VULKAN_DISABLE_FEATURES -ErrorAction SilentlyContinue
    Set-Location -LiteralPath $runRoot
    $exe = Join-Path $repoRoot 'native/build/vulkan-modernization/win64-simulator/vulkan-engine-smoke/Release/vulkan_first_use_smoke.exe'
    & $exe --phase cold --validation on
    if ($LASTEXITCODE -ne 0) { throw 'Cold first-use failed' }
    & $exe --phase restored --validation on
    if ($LASTEXITCODE -ne 0) { throw 'Restored first-use failed' }
} finally {
    Set-Location -LiteralPath $repoRoot
    foreach ($name in $names) {
        if ($null -ne $saved[$name]) { Set-Item -LiteralPath ('Env:' + $name) -Value $saved[$name].Value }
        else { Remove-Item -LiteralPath ('Env:' + $name) -ErrorAction SilentlyContinue }
    }
}
~~~

cold requires absent first-use-cwd/first-use.bin; restored requires that file.
Use a new root for each pair. Actual driver input bytes must match the paired cold
saved payload; restored still performs one real graphics create. CSV has a header
and one row; copy the cold row before restored overwrites the shared child CSV.

--skip-draw skips only draw, retaining actual submit/present, and fails on draw=0.
--skip-submit retains recording/draw and omits frame submit/present, also failing
the identity assertion. Negative controls have separate child directories and
CSV outcome FAILED. They fail from missing evidence, not forced CLI return codes.

The chain ties a successful user CB Begin to binding/drawing the created PSO,
submission of that exact CB, the actual acquire semaphore, and actually signaled
binary semaphores consumed by present. A successor requires observed submit
wait/signal linkage; creation time alone is never dependency proof. Graphics
creation checks actual RP/layout/cache handles. CSV CLEAN is decided only after
teardown and clean live/post counters; the formal judge must also require leak0.

The formal script and cold/restored x Validation ON/OFF x at least 5 independent
processes per combination remain pending. OFF uses the separate build with
CC_VULKAN_VALIDATION=OFF and CC_DEBUG_FORCE=ON, measured separately from ON.
No individual instance/device span, full process cold start, display time,
pixel readback, game golden scene or mobile thermal gate is claimed.

## PERF-A2 verify-first-use.ps1 (formal first-use acceptance script)

Formal acceptance for the PERF-A1 harness. Interface: `-SelfTest`,
`-Validation on|off|both` (default both), `-Repeats` (default 5, values < 5
rejected before any build). No -EngineExe/-Cases/-SkipBuild bypasses: the
CURRENT source targets are always rebuilt first (ON tree win64-simulator, OFF
tree win64-benchmark-off; a red build refuses stale exes).

Direct invocation:
```powershell
& .\verify-first-use.ps1 -SelfTest
& .\verify-first-use.ps1                # both trees, 5 repeats (20 processes)
```

Formal matrix: per validation tree, 5 repeats, each repeat a fresh pair
directory with one cold process and one NEW-process restored process (20
independent positive runs for the default both/5). The cold judge's saved
bytes feed the restored judge's ExpectedInitialBytes (real cross-process
payload linkage); a failed cold never lends stale bytes — the warm pair is
reported unaccepted. Every case writes its own log; cold.csv is snapshotted
immediately after cold (the restored process overwrites first-use.csv);
restored.csv is kept after restored. samples.csv accumulates
validation/repeat/saved-bytes/metric rows, and per-group median/P95/P99 are
printed as DESCRIPTIVE only — no warm-uplift, no 3% gate, no game/display/
process-cold-start claims.

Shared judge Test-FirstUseLog (log + CSV, also used by fixtures): exactly 7
first-use prefixed lines (validation/mode/cache-input/evidence/saved/timing/
footer), each reserved class unique with case-sensitive whole-line exact
matches; unknown/no-space/malformed markers and foreign footers rejected;
exact exit 0 with failures=0 liveErrors=0 postErrors=0 outcome=CLEAN
(NTSTATUS and any other code rejected); unique anchored zero-bytes leak line
(real timestamp prefix tolerated); unique anchored API line with effective
exactly 1.3 and the full paths schema; validation ENABLED/DISABLED per
expectation; cache-input cold/restored exact with restored initialBytes a
canonical positive Int64 equal to the linked cold SavedBytes; evidence all
six 1; saved canonical positive validated=1 dirty=0; one timing line with 10
canonical Int64 fields and ordering sanity (factory>=driver,
combined>=shader+factory, frame_flow>=frame_to_present_api,
main_to_present_api>frame_to_present_api); CSV exactly header+row with
phase/validation/skip-flags/outcome and every metric equal to the log line;
VUID-/HAZARD/FAIL:/SKIPPED/INCOMPLETE/UNSUPPORTED rejected. The SelfTest
carries 38 fixtures (4+1 canonical positives from real schemas plus
single-variable negatives: footer truncation/01/foreign/truncated-word/junk,
validation wrong/duplicate/no-space, unknown marker, evidence draw/submit/
chain zeros, saved zero/dirty/overflow, restored long match/mismatch and int
mismatch, timing missing/leading-zero/junk/overflow/driver>factory,
NTSTATUS, VUID/HAZARD, leak junk/duplicate/nonzero, API 1.30, paths 01/
duplicate, unknown phase expectation), declared=executed=passed enforced.
skip-draw/skip-submit logs are rejected by the judge even if exit 0 were
faked (draw/submit evidence and CLEAN footer requirements).

Environment (SDK validation layer, empty implicit isolation, engine DLLs on
PATH, FORCE 1.3, COMPLETION auto, SUBMIT/BARRIER legacy, RENDERING/DISABLE
cleared) is saved by existence and restored in finally along with cwd. The
--validation off formal runs require the OFF build tree; the formal script
does not rerun Android or any other matrix.

### PERF-A2 rev2 (parser reliability + sampling revision)

Lead AST replay with real run logs found six false-accept reds (red credited
to Lead, fixes by Worker): missing-csv-argument, restored-without-cold-link,
junk-api-prefix, junk-paths-prefix, no-space-paths-duplicate, csv-leading-zero
(perf-a2-lead-parser-red.json; worker replay JSON
perf-a2-worker-parser-replay.json shows valid=True and all six False after the
fix). The CSV is now a MANDATORY judge input (no/empty/missing file fails),
the restored judge REQUIRES a canonical positive ExpectedInitialBytes linked
to the same-pair cold saved bytes (missing/0/negative/mismatch/unparseable all
fail), API/paths use wide reserved counts (junk prefixes, no-space variants
and duplicates all rejected) with anchored exact schemas, and CSV numbers must
be canonical decimal Int64 equal to the log per field. Timing ordering checks
use safe subtraction. Invalid ExpectedPhase/Validation judge inputs return
False instead of throwing. prewarm-era class references were removed.

Sampling revision: samples.csv now carries ONE row per qualified process with
validation/repeat/saved_bytes/phase and all 10 raw timing fields (20 rows for
the default both/5 matrix; grouping is per validation/phase/metric,
descriptive only). A failed cold never lends stale bytes to a warm run: the
warm pair is reported BLOCKED without a qualified sample while overall failure
sticks. The run root is millisecond+PID with reuse refused, per-pair
directories are never reused, and an env sentinel proves PATH (exists) and
CC_VULKAN_RENDERING (existence) restore classes after the run.

SelfTest: 46 fixtures declared=executed=passed (Lead six reds as single-
variable fixtures, per-metric CSV mismatches, restored gate control flow, and
the previous footer/validation/leak/API/paths families), plus gate control-
flow checks. Formal: both trees build 0; 20/20 processes judge PASS with
per-pair inputBytes linkage verified. skip-draw/skip-submit logs remain
rejected by evidence+footer requirements. Still Windows finite first-frame
only: not game/display/process-cold-start, no warm-uplift or 3% gate claims.

### PERF-A2 rev2 (parser reliability + sampling revision)

Lead AST replay with real run logs found six false-accept reds (red credited
to Lead, fixes by Worker): missing-csv-argument, restored-without-cold-link,
junk-api-prefix, junk-paths-prefix, no-space-paths-duplicate, csv-leading-zero
(perf-a2-lead-parser-red.json; worker replay JSON
perf-a2-worker-parser-replay.json shows valid=True and all six False after the
fix). The CSV is now a MANDATORY judge input (no/empty/missing file fails),
the restored judge REQUIRES a canonical positive ExpectedInitialBytes linked
to the same-pair cold saved bytes (missing/0/negative/mismatch/unparseable all
fail), API/paths use wide reserved counts (junk prefixes, no-space variants
and duplicates all rejected) with anchored exact schemas, and CSV numbers must
be canonical decimal Int64 equal to the log per field. Timing ordering checks
use safe subtraction. Invalid ExpectedPhase/Validation judge inputs return
False instead of throwing. prewarm-era class references were removed.

Sampling revision: samples.csv now carries ONE row per qualified process with
validation/repeat/saved_bytes/phase and all 10 raw timing fields (20 rows for
the default both/5 matrix; grouping is per validation/phase/metric,
descriptive only). A failed cold never lends stale bytes to a warm run: the
warm pair is reported BLOCKED without a qualified sample while overall failure
sticks. The run root is millisecond+PID with reuse refused, per-pair
directories are never reused, and an env sentinel proves PATH (exists) and
CC_VULKAN_RENDERING (existence) restore classes after the run.

SelfTest: 46 fixtures declared=executed=passed (Lead six reds as single-
variable fixtures, per-metric CSV mismatches, restored gate control flow, and
the previous footer/validation/leak/API/paths families), plus gate control-
flow checks. Formal: both trees build 0; 20/20 processes judge PASS with
per-pair inputBytes linkage verified. skip-draw/skip-submit logs remain
rejected by evidence+footer requirements. Still Windows finite first-frame
only: not game/display/process-cold-start, no warm-uplift or 3% gate claims.

### PERF-A2 rev3 (anchoring precision)

Lead rev2 AST replay found five reds with REPLACEMENT mutations (red credited
to Lead, fixes by Worker): replacing the single API/paths line with a junk
prefix still passed (the regex's ^ lived inside the optional timestamp group,
so a skipped group let the match start anywhere); ExpectedInitialBytes
'014048' (non-canonical leading zero) was accepted; appended
'Total device memory leaked:4096 bytes.' and 'FIRST-USE saved: ...' variants
slipped through. Fixed: '^' is always the outermost anchor with timestamp
prefixes inside (?:...) groups; wide identification is case-insensitive
(FIRST-USE/no-space/malformed variants captured) while exact matches stay
case-sensitive; ExpectedInitialBytes strings accept only canonical positive
ASCII digits (int/long positives kept; '014048' rejected); the junk fixtures
are REPLACEMENT single mutations that reach the anchoring branch. SelfTest
58/58; AST replay of the five reds all False with valid=True
(perf-a2-worker-rev3-parser-replay.json); formal both/5 20/20 judge PASS
exit 0; summary.csv adds 40 validation/phase/metric groups (n=5, nearest-rank
P95/P99, median as midpoint double for even n — descriptive only).

Lead review supplement: timestamp-prefixed no-space API/paths duplicates are
also rejected; the current SelfTest has 60 fixtures. Lead rejudged all 20
Worker process logs and CSV snapshots and independently recomputed all 40
summary groups (five samples each). This accepts finite measurement integrity,
without granting a game, display-latency, or performance Gate.

## PERF-B1a present profiling (Lead reviewed revision)

Adds `--present-profile on|off` (default off; unknown/missing values rejected
by the executable before GPU initialization). When on, a vkQueuePresentKHR PFN hook transparently records
per-frame driver span, per-swapchain/index identity, and per-result;
vkWaitForFences/vkWaitSemaphores hooks classify scope-active waits into fence
vs timeline categories. A new `.present.csv` sidecar carries one row per
sampled frame with columns
`frame,profile,present_calls,present_successes,fence_wait_calls,timeline_wait_calls,wait_failures,driver_ms,host_wait_ms,host_residual_ms`;
profile off writes NA for the three decomposition fields while retaining real
call counts. All non-negative finite; parent phases.present_ms ==
driver+wait+residual (no double counting); no hot-path allocation.

Verification: ON/OFF × fence/timeline × profile on/off = 8 cases, each 40
frames + 10 warmup, all exit 0 CLEAN with per-frame calls=1 successes=1
waitFailures=0. Old main/phase CSV schema unchanged. The formal
verify-completion-benchmark criteria for the new sidecar are a separate next
step (not implemented here).

### PERF-B1a Worker rev2 report (superseded by Lead review)

Lead review found 8 issues in the initial implementation (red credited to
Lead): dual Clock::now spans in driver hook; presentProfile/profiling never
assigned; benchPresent.active manual true/false not RAII; identityOk per-frame
reset missing; waitFailures/driverFailures not propagated to samples; present
wait calls hardcoded 0; residual placeholder with no finite/nonnegative check;
scope/queue/nesting validation missing.

rev2 fixes: PresentProfile simplified to single set of counters (no frame vs
global duplication); noncopyable PresentScope RAII guard (constructor resets
entire profile including identity, destructor clears active); present hook
records driver span only when profiling on (single Clock pair), profile off
skips inner clocks; identityOk reset per frame via scope constructor reset;
wait failures propagate to sample.frameWaitFailures; driver failures counted
separately; present wait calls read from real counters (not hardcoded 0);
residual computed at sidecar write time from actual phases[4] - driver -
waits with finite/nonnegative strictness; nesting via depth counter (present
hook rejects depth>0); --present-profile-fault identity negative control
causes real identity-check failure (exit 1, failures>0) without touching
driver args. CLI duplicates rejected. Build ON/OFF exit 0; 8/8 combos (2 val
× 2 completion × 2 profile) exit 0 with per-frame calls=1/successes=1/
waitFailures=0/identity=OK; fault negative exit 1 with real
"present identity chain broken" FAIL. CLI negatives (bogus/missing) clean
exit 1. Formal 1000-frame matrix and verify-completion-benchmark criteria
remain pending (Lead next block).

### PERF-B1a Lead takeover verification

The Worker rev2 completeness and eight-case claims are withdrawn: identity was
checked only with profiling on, wait counts were not published, failure gates
were incomplete, and the negative control contained a forced false flag.
The current harness checks actual identity, call/result counts and wait failures
in both modes. Profiling on records finite nonnegative driver/wait/residual
parts; off records NA and retains actual counters. The identity fault changes
only the expected image index. Nested scopes/calls invalidate the sample.
No production synchronization or extra submits/waits were added.

Both current-source builds passed. Eight independent cases (validation on/off,
fence/timeline, profiling on/off) each ran 40 sampled frames plus 10 warmup.
The existing main and phase judges passed all eight; the 320 present sidecar
rows align and partition the present phase with absolute error below 1e-8 ms.
Actual waits are reported, including zero-wait timeline frames.

Evidence: native/build/vulkan-modernization/perf-b1a-lead-20261004-074907724-17608/
(manifest and Lead verification JSON). The real identity fault exits 1;
four CLI negative cases exit 1. Eleven mock scenarios of the actual extracted
present/wait hook source pass separately; these are CPU semantics checks.
A preceding Lead run failed because the original wait PFNs were not saved;
that run is retained and excluded from passing evidence.

The formal sidecar verifier, 1000-frame repeat matrix and >3% investigation
remain pending. This finite clear benchmark does not establish game
performance or a platform Gate.

## PERF-B1b-A verify-present-profile.ps1

Offline verification script for the PERF-B1a present-sidecar harness. Two
modes only (mutually exclusive): `-SelfTest` and `-EvidenceRoot <path>`.
Default (no mode) exits 1. Builds nothing, runs no GPU.

Shared judge Test-PresentProfileEvidence: log + main CSV + phase CSV +
present CSV are all mandatory. Reserved markers (vulkan API/paths, triad,
config, present-profile, footer, leak) use case-insensitive wide counting
(unique) then case-sensitive anchored exact match. API anchored
effective=1.3 (not 1.30); paths full schema. Present sidecar: 10 columns,
frame/profile/calls/successes canonical; profile on decomposition
finite nonneg with partition closure (driver+wait+residual ==
phases.present_ms, tolerance <1e-8 ms) and hostWait>0 implies actual wait
calls > 0; profile off three values exact 'NA'. Main/phase CSV use the
legacy Test-BenchmarkResult metric family (via AST-extracted shared judge).
Footer leak both anchored. VUID-/HAZARD/FAIL:/SKIPPED/INCOMPLETE rejected.
Exit exact 0.

SelfTest 19 fixtures (canonical positive + single-variable negatives:
junk API/paths, foreign/truncated footer, NTSTATUS exit, VUID/HAZARD,
triad partial, backend mismatch, fault nonzero, calls/successes zero,
wait-fail, residual NA on, off numeric not NA, empty log/csv).

EvidenceRoot: reads manifest.json (case/exit/csv/log per entry), rejects
duplicates/missing files, calls the shared judge per case with the
correct validation/backend/profile derived from the case name.

```powershell
& .\verify-present-profile.ps1 -SelfTest
& .\verify-present-profile.ps1 -EvidenceRoot <path-to-run-root>
```

Boundaries: offline only; builds nothing; runs no GPU; formal 1000-frame
matrix is a separate next step (Lead). Windows only.

PERF-B1b-A Lead review: the initial verifier is not accepted. Its 19-fixture
SelfTest passes, but the real eight-case replay returns exit 1 (four validation
OFF cases rejected). The claimed legacy AST judge reuse is absent, and ten
independent corrupted samples pass. See PROGRESS section 2DB for the saved
red evidence. A revised verifier is required before a long matrix.

### PERF-B1b-A Lead replacement (accepted offline scope)

The Worker revision was incomplete; the initial AST-reuse/eight-case/JSON
claims are withdrawn. The replacement installs only the two legacy function
bodies into script scope, then calls both main and five-phase judges. It does
not execute the old runner. The wrapper adds strict marker families, exact
schemas, canonical frame/wait integers, finite nonnegative numbers, actual
backend matching and present partition checks. Validation OFF expects the
compile-time OFF evidence NOT VERIFIED. No scoped wait counts are assumed.

Manifest checking requires exactly the eight distinct legal configurations.
Missing inputs/fields/cases, duplicates and unknown names reject. All outputs
go to a new ignored present-profile-checks directory; source logs remain intact.

Windows PowerShell 5.1: SelfTest 148/148 exit 0; the ten original false-positive
mutations all reject; the real eight-case replay passes with exit 0. Three
invalid CLI invocations exit 1. Current detailed evidence is referenced in
PROGRESS section 2DB. SelfTest uses the saved Lead fixture root
perf-b1a-lead-20261004-074907724-17608 and therefore requires that local artifact.
The existing commands above remain valid. Offline checking does not build,
run GPU work or change environment policy.

The full 1000-frame repeat matrix and its formal run entry are still pending.
No performance improvement or overall Gate is granted by this parser work.

### PERF-B1b-B Worker initial runner (superseded)

```powershell
& .\verify-present-profile-matrix.ps1
```
Builds ON/OFF targets then runs 40 processes (2 validation × 2 completion ×
2 profile × 5 repeats) of 1000 frames + 300 warmup each. Strict CLI
validation; env saved/restored by existence in finally.

### PERF-B1b-B Lead formal runner

The initial raw-only runner is superseded. The current script loads the shared
main/phase/present judges through AST and calls them for every case. It first
builds the required current-source targets and preserves build/native streams.
Repeated configurations run in alternating order, in independent processes and
working directories. Missing cases/data, failed builds, native failures or judge
failures make the overall result fail.

Run from this directory:

~~~powershell
& ./verify-present-profile-matrix.ps1 -SelfTest
& ./verify-present-profile-matrix.ps1
~~~

Defaults: both validation builds, 1000 samples and 300 warmup per process,
five repeats of fence/timeline and profile on/off (40 processes).
Validation, sample/warmup/repeat limits and unknown options reject before build.
SelfTest checks runner completeness/order/failure handling without build/GPU.

Artifacts in a new ignored present-profile-matrix/run-* directory:
plan.json, manifest.json, judge.json, run-status.json, build logs, and each
case's original stdout/stderr, merged log and three CSV files. Environment
existence/value and cwd are restored and checked.

Lead execution: runner SelfTest 11/11; five invalid CLI cases exit 1; actual
default matrix native=0 and judge=True for all 40, complete=True,
envRestored=True. Independent analysis verifies 40000 aligned samples and
both phase partitions. Evidence root:
run-20261004-085153313-10956-40e952fc (PROGRESS section 2DC).

The present PFN span includes layers/ICD/call-time waits; it is not GPU time
or an isolated driver cost. The remaining host residual is unclassified.
Paired profiling on/off results still vary materially. Neither this complete
measurement run nor its descriptive statistics grant a performance Gate.

Profile off retains identity/result/call checks and the existing wait-hook timing.
This comparison isolates added profiling behavior; it is not a no-hooks baseline.

### PERF-B2b-A completion API instrumentation (Lead repair)

The Orca Worker added initial hooks. Lead review found no sidecar writer or
sample fields, no query failure/device/nesting checks, unconditional clocks even
with profiling off, and no new eight-case evidence. The initial completion and
GPU claims are withdrawn. The following describes the repaired current source.

Three hooks transparently forward existing vkGetFenceStatus,
vkGetSemaphoreCounterValue and vkResetFences calls. Trackers borrow addresses of
global PFN variables, so hooks work without rebuilding trackers. Fixed counters
run in both modes; added clocks run only inside a non-nested profile-on present
scope. VkResult and counter output remain unchanged; fence VK_NOT_READY is a
normal observed result, counted separately from failures. Device/shape/overlap
and failure checks participate in the frame verdict. All saved PFNs are restored,
and missing PFNs remain missing when hooks are installed.

The existing main, phase and ten-column present sidecars remain unchanged.
New `<csv>.completion.csv` has exactly nine columns:
`frame,profile,fence_status_calls,fence_not_ready,counter_calls,reset_fence_calls,query_failures,completion_api_ms,host_other_ms`.
Counts are actual scoped calls, not inferred from backend selection. On: finite
nonnegative completion_api + host_other equals the original host_residual within
1e-8 ms. Off: counts remain real and both duration columns are NA. The dispatch
snapshot line counts calls since hook installation through the pre-teardown
checkpoint, including initialization/warmup/outside-present calls; it is not the
sum of sampled-frame counts or a GPU timing measurement.

Build both trees **sequentially**; they share the shader-toolchain child tree:

```powershell
cmake --build native/build/vulkan-modernization/win64-simulator --config Release --target vulkan_wsi_smoke --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'ON build failed' }
cmake --build native/build/vulkan-modernization/win64-benchmark-off --config Release --target vulkan_wsi_smoke --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'OFF build failed' }
```

Raw short runs from the repository root (separate evidence; not formal Gate):

```powershell
$repoRoot = (Get-Location).Path
$benchmarkEvidenceRoot = Join-Path $repoRoot ('native/build/vulkan-modernization/completion-short-' + (Get-Date -Format 'yyyyMMdd-HHmmssfff') + '-' + $PID)
$envNames = @('PATH','VK_LAYER_PATH','VK_IMPLICIT_LAYER_PATH','CC_VULKAN_FORCE_API','CC_VULKAN_COMPLETION','CC_VULKAN_SUBMIT','CC_VULKAN_BARRIER','CC_VULKAN_RENDERING','CC_VULKAN_DISABLE_FEATURES')
$savedEnv = @{}
foreach ($name in $envNames) { $item = Get-Item ('Env:'+$name) -ErrorAction SilentlyContinue; $savedEnv[$name] = @{ Exists=($null -ne $item); Value=$(if($item){$item.Value}else{$null}) } }
try {
    New-Item -ItemType Directory -Path ($benchmarkEvidenceRoot+'/empty-implicit') | Out-Null
    $env:VK_LAYER_PATH = 'D:/Software/VulkanSDK/1.4.341.1/Bin'
    $env:VK_IMPLICIT_LAYER_PATH = $benchmarkEvidenceRoot+'/empty-implicit'
    $env:PATH = "$repoRoot/native/external/win64/libs;$repoRoot/native/external/win64/libs/v8/Release;"+$env:PATH
    $env:CC_VULKAN_FORCE_API='1.3'; $env:CC_VULKAN_SUBMIT='legacy'; $env:CC_VULKAN_BARRIER='legacy'
    Remove-Item Env:CC_VULKAN_RENDERING,Env:CC_VULKAN_DISABLE_FEATURES -ErrorAction SilentlyContinue
    foreach ($val in 'on','off') { foreach ($backend in 'fence','timeline') { foreach ($profile in 'on','off') {
        $tree = if ($val -eq 'on') { 'win64-simulator' } else { 'win64-benchmark-off' }
        $exe = "$repoRoot/native/build/vulkan-modernization/$tree/vulkan-engine-smoke/Release/vulkan_wsi_smoke.exe"
        $env:CC_VULKAN_COMPLETION=$backend
        & $exe --benchmark --validation $val --csv "$benchmarkEvidenceRoot/$val-$backend-$profile.csv" --present-profile $profile --frames 40 --warmup 10
        if ($LASTEXITCODE -ne 0) { throw "Short case failed: $val/$backend/$profile" }
    } } }
} finally {
    foreach ($name in $envNames) { if($savedEnv[$name].Exists){Set-Item ('Env:'+$name) $savedEnv[$name].Value}else{Remove-Item ('Env:'+$name) -ErrorAction SilentlyContinue} }
}
```

Lead evidence root `perf-b2b-a-lead-20261004-094430018`: current-source builds
ON=0, OFF retry=0 (initial concurrent OFF build hit shared-child MSB3061).
Eight short cases: native=0 and unchanged shared judges pass; an independent
Python check verifies 320 new rows, real counters and the extra subpartition.
Identity fault and bogus/missing CLI each exit 1. Profile-on completion API mean
us: ON fence 112.225/timeline 85.603; OFF fence 3.270/timeline 0.295. These are
40-sample descriptive call spans, not cross-process causal results or a Gate.
Source hash `45A28C0878639E37768CDCCA51D7B819903D7615A62BAA6190AB0277B2606CE6`.

Extracted-hook CPU probe: `native/build/vulkan-modernization/perf-b2b-b-lead-tests/`.
`generate.py` verifies an exact source round trip, replacing only the clock alias
with a deterministic counting clock. Current-source configure/build/run each
exit 0: 20 groups, 63 assertions, zero failures. Real `GpuCompletionTracker`
objects created before PFN replacement route fence and timeline queries through
the replacement hooks; cached completions skip the API. Other tests cover
result/output forwarding, NOT_READY, scope/exception reset, nested-query timing,
device identity, null guards, and zero added clock calls outside profiling.
This is CPU mock evidence; the original Worker probe does not compile and its
older executable cannot prove the current source works.

The new >=1000 x >=5 matrix has separate accepted evidence below. The old B1
formal run remains valid for its own source. Production, CMake and synchronization
behavior remain unchanged.

## PERF-B2b-C completion-sidecar verification

`completion-profile-judge.ps1` adds a pure check for the nine-column sidecar.
It requires exact headers, aligned frame/profile rows, canonical UInt64 counts,
NOT_READY <= status calls, zero failures, finite nonnegative durations, and the
completion/host-other partition. Profile off requires `NA`. Whole-run dispatch
counts include setup and warmup and must cover sample totals. Query counts are
not fixed to one per frame. Decimal summation detects overflow without rounding.

From the repository root:

```powershell
& ./native/tests/vulkan-engine-smoke/verify-completion-profile.ps1 -SelfTest
& ./native/tests/vulkan-engine-smoke/verify-completion-profile.ps1 -EvidenceRoot ./native/build/vulkan-modernization/perf-b2b-a-lead-20261004-094430018
& ./native/tests/vulkan-engine-smoke/verify-present-profile-matrix.ps1 -SelfTest -CompletionProfile
& ./native/tests/vulkan-engine-smoke/verify-present-profile-matrix.ps1 -CompletionProfile -Validation both -Frames 1000 -Warmup 300 -Repeats 5
```

The offline replay combines the existing main/phase/present checks with the new
sidecar check; it never builds or runs a GPU process. PS5.1: 65 fixtures passed,
the eight saved short cases passed, and the runner's 15 control checks passed.
Fixture files and verdict JSON are retained in `completion-profile-checks/`.
The earlier Worker prototype did not provide target-branch coverage; its bad
dispatch/NaN/column/count cases are rejected by the new shared function.

`-CompletionProfile` is an opt-in matrix check. It requires the fifth evidence
file and writes a separate `completion-profile-matrix/` root with source hash,
build logs, raw native logs, manifest, judgments and restoration status. Two
build trees share a shader-toolchain child and are built sequentially. This
40-process experiment remains a finite clear workload; passing evidence does
not establish a game performance, display-latency, thermal or platform Gate.

PERF-B2b-D accepted root:
`completion-profile-matrix/run-20261004-113348193-10804-b4e2aa95/`.
Orca Worker `perf-b2b-d-worker/formal-output2.log` records PS5.1, sequential
builds=0, 40/40 native/judge passes and wrapper exit 0. Status confirms source
unchanged and environment restored. Earlier incomplete attempts are preserved.
The Worker's partial report was contradicted by its own terminal log.

Lead independent analysis checks 40,000 rows and all three partitions; maximum
errors are below 9e-16 ms. `lead-completion-analysis.json` retains unrounded data
and paired signed changes. Profile-on completion API mean us (five repeats):
validation ON fence/timeline 22.9682/16.5928; OFF 0.8624/0.1537. These call spans
include layers/ICD/waiting. Current positive median changes of 8.022% (ON rep3)
and 6.496% (OFF rep2) remain unresolved; this closes evidence coverage, not the
performance Gate. See `ai-docs/to-do/VULKAN_COMPLETION_API_VARIANCE_ANALYSIS.md`.

### M0-WIN-SIM-B rev3 simulator build fix (protobuf-lite hash_map)

config.h `_WIN32` adds `_MSC_VER >= 1600` branch using `<unordered_map>/<unordered_set>/std` namespace
with `GOOGLE_PROTOBUF_USE_STD_UNORDERED` marker; old MSVC macros preserved in `#else`.
hash.h selects the existing standard-container wrappers for this Windows branch.
Its default equality preserves C-string comparison by contents. The original
pointer, string and pair hash algorithms, comparator overloads, metadata and
`streq` remain shared; legacy MSVC and ordered-container fallback bodies remain
unchanged. Non-Windows behavior is unchanged by this conditional adaptation.
Legacy compiler/platform builds have not been executed.

`simulator` is a static library; `SimulatorApp-Win32` is the application. Build:

```powershell
cmake --build native\build\vulkan-modernization\win64-simulator --config Release --target simulator --parallel 4
cmake --build native\build\vulkan-modernization\win64-simulator --config Release --target SimulatorApp-Win32 --parallel 4
```
Both exit 0 on MSVC 19.51 after the Lead rev3 change. No application launch was
performed. The Lead's independent probe passed 1030 checks, including pair keys,
C-string map/set equality, explicit equality policies, forced hash collisions and
all 1000 values after rehash. Logs and probe sources are in ignored
`native/build/vulkan-modernization/m0-win-sim-b-worker/lead-rev3/`.
The Orca Worker rev2 probe had only 12 checks and did not contain the claimed
pair or C-string map checks; its broader coverage claim is withdrawn. Build
success does not establish a platform, visual or performance Gate.

### PERF-B3-B adjacent backend pairs (opt-in)

The existing full matrix already alternates backend order across repeats.
This narrower experiment removes intervening profile/validation cases: only the
`win64-benchmark-off` tree, profile ON, 300 warmup frames and 1000 sample frames.
Ten adjacent pairs run fence then timeline on odd pairs, timeline then fence on
even pairs, providing five pairs in each direction. The default matrix behavior
is unchanged.

```powershell
& ./native/tests/vulkan-engine-smoke/verify-present-profile-matrix.ps1 -CompletionProfile -PairedBackends -SelfTest
& ./native/tests/vulkan-engine-smoke/verify-present-profile-matrix.ps1 -CompletionProfile -PairedBackends
```

`-Pairs` accepts even counts from 10 through 20. Explicit ON/both validation,
`-Repeats`, missing `-CompletionProfile`, fewer than 1000 samples or fewer than
300 warmup frames are rejected before building. The OFF target is rebuilt once;
each process must pass the existing log/main/phase/present judge and mandatory
completion sidecar judge. Actual backend/encoder paths and validation status
come from the log.

Artifacts are in `paired-completion-matrix/run-.../`: plan, ordered manifest,
judge, status, native streams and all four CSVs. Manifest records pair, position,
direction, UTC start/end and monotonic process-wait milliseconds (host launch
and wait span, excluding subsequent stream processing). Environment/cwd and
benchmark source-hash checks remain required. Clock rollback or malformed
timing/order metadata prevents a complete result.

Lead PS5.1 control SelfTests passed 15 existing + 24 pair checks; seven CLI
negatives exited 1 before build/GPU. GPU execution and numerical analysis are
separate subsequent steps. This experiment can investigate order-related
variation; it cannot establish temperature/clock control, explain an old
outlier merely because it disappears, or grant a performance/platform Gate.

Orca Worker subsequently executed the formal 20-process run at
`paired-completion-matrix/run-20261004-124220753-36644-744a5068/`: current OFF
build 0, 20/20 native 0 and shared judges PASS, source/environment checks PASS.
Lead independently validated 20000 rows and all timing/order metadata; paired
analysis is `lead-paired-analysis.json` in that root. Pairs 2 and 9 still exceed
the 3% investigation threshold in both wall median and P95, in opposite first
backend orders. See `ai-docs/to-do/VULKAN_PAIRED_BACKEND_VARIANCE_ANALYSIS.md`.
The run establishes evidence completeness, not the cause of the remaining
variation or a performance Gate.

### PERF-B3-D same-backend controls (opt-in)

```powershell
& ./native/tests/vulkan-engine-smoke/verify-present-profile-matrix.ps1 -CompletionProfile -PairedBackends -SameBackendControls -SelfTest
& ./native/tests/vulkan-engine-smoke/verify-present-profile-matrix.ps1 -CompletionProfile -PairedBackends -SameBackendControls
```

The same OFF/profile-ON/1000-sample/300-warmup protocol now generates five
fence/fence pairs and five timeline/timeline pairs, interleaved by pair type.
Each pair has two unique member names and identical requested backend; its
comparison is **second minus first**, not timeline minus fence. All old and
completion judges, current-source build and restoration checks remain required.
The run family is `same-backend-control-matrix`, with an explicit
`SameBackendControls=true` status flag. Omitting `-PairedBackends` is rejected.

Lead CPU control tests passed 15 existing + 30 pair tests, including acceptance
of the control shape and rejection of mixed/control confusion, duplicate member
names and a changed backend within a control pair. The new GPU experiment has
not yet run at this entry's creation. It measures repeated-run variation at
fixed backend; even large same-backend differences do not prove the cause of an
earlier backend comparison.

GPU observations are kept separate: Worker obtained three current NVIDIA
query logs; its claimed summary JSON and timing proof were absent. Lead then
captured three query-only observations with Windows Stopwatch and async stream
reads (host spans 109.4714/69.3430/73.5310 ms, all exits 0; P8/40 C/210 MHz).
Evidence is ignored `perf-b3-b-lead/gpu-observation.json` and raw streams.
No clock/power settings were changed, no matrix samples are associated with
these observations, and no per-frame telemetry is added by the control mode.

Orca Worker subsequently ran all 20 control processes at
`same-backend-control-matrix/run-20261004-131601467-596-6a716108/`: current OFF
build 0, 20/20 native exits 0 and shared judges PASS, source/environment checks
PASS. Lead independently verified the 20000 samples and ordered manifest;
the unrounded result is `lead-control-analysis.json` in that root. Comparison
is second minus first at fixed backend. Timeline pair 10 changed wall median
by +4.638% and P95 by +85.644%; fence pair 3 changed them by -12.868%/-57.017%.
Backend changes are therefore not required for this variation; this does not
establish that an earlier mixed-backend difference is entirely noise or that
backend has no effect. See
`ai-docs/to-do/VULKAN_SAME_BACKEND_VARIANCE_ANALYSIS.md`. Further diagnosis uses
the existing frame data before repeating GPU runs; the >3% investigation and
performance/platform Gate remain open.

### PERF-B3-E offline block and same-frame tail analysis

Lead completed the offline check of the mixed and same-backend roots (40 cases,
40000 samples). Evidence is ignored `perf-b3-e-lead/lead-block-analysis.json`:
ten contiguous 100-frame blocks per case, top50 ranked by wall time with frame
number as tie-breaker, and top10 complete same-frame decompositions. The four
present components are driver, host wait, completion API and host other;
residual is not counted a second time. The reader checks source/status/order,
all four CSVs, results/dispatch and all partitions. CPU fixtures passed 25/25,
and the original malformed CSV samples were rejected while valid data passed.

Worker rev2 selected frames and main tail means match the independent result,
but its incomplete validation and missing SelfTest/top10 are superseded by the
Lead check. No additional build or GPU workload ran. Some tails cluster in a
short frame interval, while other slow frames include acquire/submit costs.
See `ai-docs/to-do/VULKAN_SLOW_FRAME_CONCENTRATION_ANALYSIS.md`. These interval
observations do not identify GPU/driver/thermal causes or grant a performance
Gate. Existing present/completion instrumentation is already implemented.

The subsequent same-frame main-CSV check is recorded in
`ai-docs/to-do/VULKAN_FRAME_METRIC_SEMANTICS.md` (Lead: 13 CPU fixtures,
40 cases/40000 joins). GPU query results align with sample index after excluding
warmup; their TOP/BOTTOM clear-CB interval is not whole-frame GPU time. CPU
FILETIME units do not establish actual resolution: these runs contain only
0/15.625-ms CPU samples, so wall minus CPU is not a per-frame blocking estimate.
Frame-wide wait API spans include present-scope waits and must not be added to
them. Some slow frames have substantial waits outside present; the saved CSVs
do not identify their call sites/handles. No correctness wait was removed.

### PERF-B3-F2: explicit wait phases (test harness only)

The benchmark has a separate opt-in `--wait-profile on|off` (default `off`).
Build the current `vulkan_wsi_smoke` target first, use the existing SDK/PATH and
policy isolation described above (`FORCE_API=1.3`, completion `fence` or
`timeline`, submit/barrier `legacy`, rendering unset), then run:

```powershell
& ./vulkan_wsi_smoke.exe --benchmark --csv frames.csv --validation on --frames 40 --warmup 10 --present-profile on --wait-profile on
```

Profile-on writes `frames.csv.wait-phases.csv`: six buckets (`unknown`,
`acquire`, `record`, `submit`, `observe`, `present`), each with fence/semaphore
calls, non-success results and API milliseconds; independent acquire evidence,
present-fence categories, validity and totals follow. Its 34 columns supplement
the unchanged main/phases/present/completion schemas. Profile-off does not write
this new file. Shared frame totals and the existing present-scope subset must
match. Durations reuse the existing wait hooks' clocks; no new wait or submit is
added. Instrumentation itself still has CPU and storage cost.

The acquired image index comes from the actual forwarded
`vkAcquireNextImageKHR` result. A frame scope holds its generation alive and
copies up to 32 present fences/pending flags. An acquire-phase single-fence
wait is classified by its actual fence handle; matching also requires a real
successful acquire, the same generation, matching driver/engine image indices
and a previously pending present fence. `--wait-profile-fault identity` requires
profile-on and corrupts only the expected generation token; actual Vulkan
arguments/results remain unchanged and the run must fail.

Lead CPU verification: 13 groups, 4781 executed assertions, ctest 27/27; the
original eight regression assertions also pass after the Lead fix. Current-source
Win32 validation-on smoke: six positive processes (240 measured frames), four
expected negative processes, and 15 single-field CSV rejection fixtures. Four
positive processes supply 160 classified frames. Maintenance1-off produces zero
acquire present-fence calls; no correctness synchronization was removed. Logs
and the offline judgment are retained under
`native/build/vulkan-modernization/wait-phase-smoke/run-20261004-191609836-23436/`.
This finite smoke does not attribute earlier slow frames, replace the formal
1000-frame paired experiment, or grant any performance/platform Gate. Previous
formal present/completion sidecars remain completed evidence for their captured
source version. The formal wait sidecar verifier is described below.

The dedicated Lead forwarding probe now passes 12 groups/279 assertions. It
extracts the hook/scope block verbatim and substitutes only a counted clock alias;
mock Vulkan functions receive the exact caller arguments and preserve actual
results/output. Failed acquires cannot publish an image index; profile-off,
six phase buckets, generation/index faults and CPU owner lifetime are covered.
It revealed two failures in nested frame scopes: a rejected inner scope closed
the parent recorder. The guard was fixed and the same assertions passed. CPU
clock values are simulated; the fake generation verifies CPU ownership only.
After that change, the six positive/four negative Win32 runs were repeated with
the current source under
`native/build/vulkan-modernization/wait-phase-smoke/run-20261004-194839798-51948/`;
240 measured frames and 160 classified frames pass the same offline checks.
CPU extraction, red/green logs and summary are retained in
`native/build/vulkan-modernization/perf-b3-f2c-lead/`.

### PERF-B3-F2e formal wait-phase sidecar checks

`wait-phase-judge.ps1` supplies the pure `Test-WaitPhaseSidecar` used by the
matrix runner. With `-WaitProfile -CompletionProfile`, every case must pass
the existing main/phase/present/completion checks and the new 34-column file.
Missing files, noncanonical/overflowing counts, nonfinite durations, unknown
phase waits, failed acquires/waits, invalid identity and fault markers fail.
Six phase totals must equal main CSV waits; acquire fence categories must
close; present counters and, when present profiling is on, wait time must match
the existing present sidecar. Duration closure tolerance is less than 1e-8 ms.
No assumption is made that timeline waits or other acquire fences are zero.
Present-profile off retains numeric wait-phase durations and old present `NA`s.

```powershell
& ./native/tests/vulkan-engine-smoke/verify-present-profile-matrix.ps1 -SelfTest -CompletionProfile -WaitProfile -PairedBackends
# Current-source OFF build, ten adjacent pairs with alternating first backend:
& ./native/tests/vulkan-engine-smoke/verify-present-profile-matrix.ps1 -CompletionProfile -WaitProfile -PairedBackends
```

The self-test runs 89 shared-judge fixtures, six new boundary checks, 15 existing
runner checks and 30 pair-plan checks without build/GPU work. The formal command
builds the current target and requests 1000 frames plus 300 warmup frames per
process. It writes a unique `paired-wait-phase-matrix` root, preserves five CSVs
per case, and checks benchmark/recorder source hashes and environment restoration.
Omitting `-WaitProfile` preserves the previous command arguments and judge path.
This opt-in measures API wait categories; it does not establish a GPU/driver
cause, remove synchronization or grant the >3% performance Gate. Formal execution
and analysis are recorded separately in PROGRESS.

The formal OFF paired run completed with 20/20 native processes and shared
verdicts passing, 1000 measured frames plus 300 warmup frames each. GPU execution
was by **Orca Worker**; **Lead** checked the terminal records and independently
read all five CSVs for 20000 classified frames. Evidence root:
`native/build/vulkan-modernization/paired-wait-phase-matrix/run-20261004-201828150-23380-31f410f3/`.
Validation is compiled off (`NOT VERIFIED`), so this run provides instrumentation
and statistical evidence rather than new validation-layer coverage. The current
data distinguish acquire present-fence waits from present-scope waits, while
some long frames spend most of their time inside the real present API call.
See [the wait phase analysis](../../../ai-docs/to-do/VULKAN_WAIT_PHASE_PAIR_ANALYSIS.md)
for all ten signed pairs and same-frame examples. The >3% investigation remains open.
