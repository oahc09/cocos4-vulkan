# Real Windows project: cache / serial prewarm

This module runs **inside the user's `test-cases.exe`**, not a standalone smoke
binary. It is a default-off functional diagnostic for plan §15.2/§15.3.

## Integration

The current project is already connected:

- Source: `E:/AIProMax/github/cocos4-vulkan-test-project/native/engine/win64`.
- Build: `E:/AIProMax/github/cocos4-vulkan-test-project/build/windows/proj`.
- `Game::init` listens to `EngineStatusChange::ON_START` before `BaseGame::init`.
  Cache configuration and two known compute recipes run before scripts create
  business PSOs. `DeviceAgent` uses its existing render-thread message queue;
  bootstrap waits for that CPU message, without adding a GPU wait or submission.
- `main.cpp` retains the normal platform init/run/shutdown order, then checks
  the diagnostic after the engine has destroyed its device.
- The Windows target compiles this source with the backend's `__TBB_LIB_NAME=tbb`.
  This cache/prewarm bootstrap does not change renderer, scene, resource,
  dependency version or default cache features. Separate Indirect and isolated
  scene-package diagnostics are described below. The existing in-memory driver
  cache remains available as before.

`real-project-integration.patch` preserves the three native project edits in the
engine repository. Do **not** apply it a second time to the already connected
project. A nonmutating check is:

```powershell
git -C E:/AIProMax/github/cocos4-vulkan-test-project apply --reverse --check `
  E:/AIProMax/github/cocos4-vulkan/native/tests/vulkan-real-project/real-project-integration.patch
```

Build the actual project, and stop on a build failure rather than running an old exe:

```powershell
& D:/Creator/3.8.8/resources/tools/cmake/bin/cmake.exe --build `
  E:/AIProMax/github/cocos4-vulkan-test-project/build/windows/proj `
  --config Debug --target test-cases --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'test-cases build failed' }
```

## Explicit runtime opt-in

The functional diagnostic requires the enabled Validation/synchronization/Best
Practices triad of this Debug project. With both variables absent, it only emits
`project-prewarm mode: phase=off enabled=0`, installs no hooks and writes no
diagnostic cache. An empty value, missing counterpart, unknown phase or relative
path is rejected before Vulkan initialization.

| Variable | Meaning |
|---|---|
| `CC_VULKAN_REAL_PROJECT_CACHE_PATH` | Absolute, isolated cache filename; use the project's ASCII paths |
| `CC_VULKAN_REAL_PROJECT_PREWARM` | `cold`, `restored`, `rejected`, or `wrong-key` |

- `cold`: filename must not exist. No automatic deletion or replacement of an
  existing user's cold-test file.
- `restored`: compatible validated envelope/payload must exist. The actual driver
  create input is byte-compared with the validated payload.
- `rejected`: existing corrupted/incompatible input must be rejected. Driver input
  is empty, while normal prewarm and scene rendering continue.
- `wrong-key`: absent cache file; one creator returns a valid but unequal key after
  a legal real pipeline creation. Queue reports 1 Ready / 1 Failed; the scene still
  runs and retires its resources; normal close returns the intentional exit code 1.

Set these in a dedicated child process. For example, this Python fragment leaves
the caller's environment intact and starts an interactive test window:

```python
import os, subprocess
from pathlib import Path
exe = Path('E:/AIProMax/github/cocos4-vulkan-test-project/build/windows/proj/Debug/test-cases.exe')
cache = Path('E:/AIProMax/github/cocos4-vulkan/native/build/vulkan-modernization/my-prewarm-run/cache.bin')
cache.parent.mkdir(parents=True, exist_ok=True)
assert not cache.exists()  # choose a fresh path; do not delete a prior cache
env = os.environ.copy()
for name in ('CC_VULKAN_RENDERING', 'CC_VULKAN_DISABLE_FEATURES', 'CC_VULKAN_PIPELINE_CACHE'):
    env.pop(name, None)
env.update(CC_VULKAN_FORCE_API='1.3', CC_VULKAN_COMPLETION='auto',
           CC_VULKAN_SUBMIT='legacy', CC_VULKAN_BARRIER='legacy',
           CC_VULKAN_REAL_PROJECT_CACHE_PATH=str(cache),
           CC_VULKAN_REAL_PROJECT_PREWARM='cold',
           VK_LAYER_PATH='D:/Software/VulkanSDK/1.4.341.1/Bin')
empty = cache.parent / 'empty-implicit'
empty.mkdir(exist_ok=True)
env['VK_IMPLICIT_LAYER_PATH'] = str(empty)
assert subprocess.run([str(exe)], cwd=exe.parent, env=env).returncode == 0
# Operate the original scenes and close normally. A new process restores the same file.
env['CC_VULKAN_REAL_PROJECT_PREWARM'] = 'restored'
assert subprocess.run([str(exe)], cwd=exe.parent, env=env).returncode == 0
```

Capture OutputDebugString / VS output together with actual exit, exceptions and
exe SHA. The current Lead's debugger collector, own-PID scene controller and raw
files are retained in the evidence directory below. They are not game APIs.

## Shared offline judge

```powershell
python native/tests/vulkan-real-project/verify-prewarm-evidence.py --self-test
python native/tests/vulkan-real-project/verify-prewarm-evidence.py `
  --case-dir <cold-case-directory> --phase cold
python native/tests/vulkan-real-project/verify-prewarm-evidence.py `
  --case-dir <restored-case-directory> --phase restored --expected-initial <cold-saved-payload-bytes>
```

Inputs: `debug-output.log`, `runtime-summary.json` and `config.json` containing
the actual case's expected API/paths. The shared judge requires exact unique
markers/footer, actual test-project exe identity, native exit/exception evidence,
Validation and leak zero, manifest/queue ownership, driver input linkage and
all observed scene pipeline retirements. It rejects malformed/duplicate evidence,
noncanonical numbers, VUID/hazards/JS/errors and **cache-save/retrieval warnings**.
A previously valid file or native exit 0 alone does not prove a successful save.
`--self-test`: 38 executed fixtures; this is parser coverage, not GPU evidence.

## 2026-10-05 functional evidence (Lead)

Root: `native/build/vulkan-modernization/win-real-prewarm-lead/run-20261005-212151/`.
`terminal.json` is the final cross-file result; `source-before/after.json` match.
Exe SHA: `3859b142cf0daba02ce8ee1d5c66106e3990e2080c8d8f0c6708efa6cabc9940`.

- 12 complete real-scene processes: default OFF, cold/restored, checksum damage,
  driver/vendor/device/UUID mismatch, truncation, wrong-key, 1.1/fence and
  live-checkpoint restore. All satisfy their expected exit/queue contract.
- Normal enabled cases: 2 actual compute creations, recipe GPU pipeline/module/
  layout retirements 2/2, 2/2, 1/1; 14 actual business graphics pipelines all use
  the configured cache and all are destroyed. No compute dispatch is asserted.
- Cold payload 137391 bytes → new restored process driver input 137391 bytes.
  A validated **live** checkpoint containing 118866 payload bytes also restores
  in a new process. This uses a before-close copy, not a claimed power-loss test.
- One intentional save-failure process holds only its isolated cache without
  delete sharing: three failed atomic replacements, old SHA unchanged, normal
  scene/retirement and native exit 0; the positive judge correctly returns False.
- Four parameter guards exit 1 before Vulkan init. Total 17 terminal native
  processes; earlier collector failure is excluded, not rewritten as PASS.
- 24 atlas/RenderTexture static ROI comparisons against the current OFF baseline,
  each 828000 pixels at threshold 0, all diff 0. Animated 2d3d screenshots are
  retained but not treated as synchronized golden images.

The first build exposed the target's missing private TBB import definition; it
was fixed and current source rebuilt/relinked. The first OFF coordinator used an
older window helper without `move`, so that incomplete flow is preserved at
`run-20261005-212034/off/`; the new root uses the verified own-PID helper.
An initial generated patch had CRLF mismatch; the final LF patch passes the
nonmutating reverse check. No important cache, scene resource or source was removed.

Functional scope only: no performance/non-regression, full rendering-feature,
Android runtime, platform or overall Gate. Release/OFF formal measurement belongs
to T05; graphics manifest expansion and asynchronous prewarm remain optional.

## T13-02b actual-project Indirect Draw (2026-10-06, Lead)

The cumulative three-file native integration patch also includes
`RealProjectIndirect.cpp`. Default OFF; setting
`CC_VULKAN_REAL_PROJECT_INDIRECT=multi|loop|skip-second` enables two native
indexed/nonindexed phases before the ordinary test menu. Prewarm/cache diagnostic
variables are mutually exclusive. Unknown or genuinely present empty modes fail
before device creation. The temporary multi/loop policy is restored afterwards.

Run the actual Debug `test-cases.exe` with Validation enabled, then close normally.
For an already running VS2022 session, put the opt-in in the target Debugging
Environment field; a PowerShell environment change is not inherited by an existing
Visual Studio process. The example below starts its own interactive test window.
`multi` uses one two-command API call; `loop` uses two one-command calls. Both read
actual packed commands and full 2048-byte red/green pixels. `skip-second` is a
content negative: expected native exit 1, two 512-byte golden mismatches, clean
Validation and complete release; never a positive rendering success.

```powershell
$previousIndirect = Get-Item Env:CC_VULKAN_REAL_PROJECT_INDIRECT -ErrorAction SilentlyContinue
try {
    $env:CC_VULKAN_REAL_PROJECT_INDIRECT = 'multi'
    & D:/Creator/3.8.8/resources/tools/cmake/bin/cmake.exe --build E:/AIProMax/github/cocos4-vulkan-test-project/build/windows/proj --config Debug --target test-cases --parallel 4
    if ($LASTEXITCODE -ne 0) { throw 'Current-source build failed' }
    # Interactive test window: close normally after the menu appears.
    Start-Process -FilePath E:/AIProMax/github/cocos4-vulkan-test-project/build/windows/proj/Debug/test-cases.exe -WorkingDirectory E:/AIProMax/github/cocos4-vulkan-test-project/build/windows/proj/Debug -Wait
} finally {
    if ($null -ne $previousIndirect) { $env:CC_VULKAN_REAL_PROJECT_INDIRECT = $previousIndirect.Value }
    else { Remove-Item Env:CC_VULKAN_REAL_PROJECT_INDIRECT -ErrorAction SilentlyContinue }
}
```

Offline shared judge against the preserved actual native collector artifacts:

```powershell
python native/tests/vulkan-real-project/verify-indirect-evidence.py <case-directory>
python native/tests/vulkan-real-project/verify-indirect-evidence.py <modern-positive-case-directory> --self-test
```

SelfTest requires its sibling `skip-second` actual case and executes 31 fixtures.
Each case contains `config.json`, `runtime-summary.json`, `debug-output.log`.
No missing CSV/golden/native evidence is silently substituted.

Explicit optional `CC_VULKAN_REAL_PROJECT_INDIRECT_CAPTURE=<capture-prefix>` uses
an already injected RenderDoc, official MIT app header at `renderdoc/`, and an
offscreen Start/EndFrameCapture scope. Normal runs never load or install RenderDoc.

Observed pipeline/module/layout/render-pass/framebuffer retirement is separated
from VMA buffer/image facade drops plus final allocator leak zero. Captured draws
are Classic because the normal auto-barrier gate remains; Dynamic policy ON is
not proof of Dynamic execution. No nonzero firstInstance, IndirectCount, compute
dispatch, platform or performance/overall Gate is asserted. Full evidence and
red/green provenance: `ai-docs/to-do/VULKAN_INDIRECT_DRAW_VERIFICATION_20261006.md`.

## Isolated Bloom / Compute scene packages

`RealProjectScenePackage.h` is called before platform startup by the real
project's `main.cpp`. `CC_VULKAN_REAL_PROJECT_RESOURCE_ROOT` absent leaves
FileUtils and the normal Debug resources unchanged. An explicit absolute root
must contain a complete Creator package with `customPipeline=true` and nonempty
effect-layout data. It becomes the only resource search root: missing data must
not silently fall back to the original legacy package. No Vulkan path is forced
by this selector. The cumulative integration patch includes the header call.

The original project's Debug settings use the legacy pipeline. Entering Bloom
or Compute through that package is not evidence of the requested rendering path.
Generate build options in an ignored evidence directory, using the project's
exported Creator build configuration:

```powershell
python native/tests/vulkan-real-project/prepare-scene-pipeline.py --scene bloom `
  --project E:/AIProMax/github/cocos4-vulkan-test-project `
  --exported-config <exported-build-options.json> --output <new-bloom-config.json>
# Substitute --scene compute to select RaytracingWeekend and its startup scene.
# From the real project directory, run the installed Creator resource builder:
& D:/Creator/3.8.8/CocosCreator.exe --project E:/AIProMax/github/cocos4-vulkan-test-project `
  --engine D:/Creator/3.8.8/resources/resources/3d/engine `
  --build 'configPath=<new-bloom-config.json>'
# Creator CLI success is exit 36, not 0. Stop on every other exit.
python native/tests/vulkan-real-project/prepare-scene-pipeline.py --scene bloom `
  --check-package <creator-output-data-directory>
```

Use pure `configPath=...` for this project's installed automation-framework
extension: its CLI parser misreads `stage=build;configPath=...`. Config generation
disables inherited make/run stages. Run Creator from the actual project
directory, not the engine repository's Babel configuration scope. Preserve
failed build logs and quarantine stale programming caches rather than deleting
project assets. An initial package had a null serialized settings component;
fresh-cache rebuilding recovered it. Static preflight explicitly returns
`gpuContentVerified=false` and cannot detect every runtime component failure.

These resource builds use installed Creator/JS 3.8.8. The executable still builds
against this repository's current native Vulkan source; this is a finite mixed
front-end/native test, not a complete engine-version compatibility Gate. The
normal Debug package and project rendering/module settings are not replaced.

```powershell
$previousRoot = Get-Item Env:CC_VULKAN_REAL_PROJECT_RESOURCE_ROOT -ErrorAction SilentlyContinue
try {
    $env:CC_VULKAN_REAL_PROJECT_RESOURCE_ROOT = '<absolute-complete-data-directory>'
    & D:/Creator/3.8.8/resources/tools/cmake/bin/cmake.exe --build `
      E:/AIProMax/github/cocos4-vulkan-test-project/build/windows/proj `
      --config Debug --target test-cases --parallel 4
    if ($LASTEXITCODE -ne 0) { throw 'Current-source test-cases build failed' }
    # Interactive window requested for scene inspection; close normally.
    Start-Process -FilePath E:/AIProMax/github/cocos4-vulkan-test-project/build/windows/proj/Debug/test-cases.exe `
      -WorkingDirectory E:/AIProMax/github/cocos4-vulkan-test-project/build/windows/proj/Debug -Wait
} finally {
    if ($null -ne $previousRoot) { $env:CC_VULKAN_REAL_PROJECT_RESOURCE_ROOT = $previousRoot.Value }
    else { Remove-Item Env:CC_VULKAN_REAL_PROJECT_RESOURCE_ROOT -ErrorAction SilentlyContinue }
}
```

Compute's registration module is already imported by the main prerequisite
module; no fabricated scene component is needed. Its declared materials and
effect-layout data must load. Actual dispatch, whole-block buffer contents,
storage-image descriptor/layout and fullscreen sampling remain separate checks.

The finite shared content judge requires replay JSON and actual raw textures,
SPIR-V and bound constant buffers, plus four normal native process terminations:

```powershell
python native/tests/vulkan-real-project/verify-scene-content.py <evidence-root> --self-test
```

Bloom compares two isolated packages differing only in threshold, with identical
pre-Bloom pixels, eight actual passes, descriptor dependencies, GPU uniforms and
different combined outputs. This does not claim successful slider input: the
attempted input produced a separate JS null-window error. Compute checks actual
dispatch dimensions, 32-byte sphere parameters, storage-to-fullscreen identity,
upper-quarter gradient pixels and a sphere-center golden. Other intersections,
non-workgroup-multiple resize, device recovery, performance and platform Gates
are outside this finite content check. The original zero-uniform capture is a
required actual negative for SelfTest; no missing evidence is synthesized.


The actual Compute runs also exposed three production defects: a raster-only
viewport-stack assertion applied to compute queues, storage descriptors changed
to a sampled layout, and explicit whole-block uniform buffers replaced by generated
zero/member data. Their focused fixes and preserved assertion/Validation/content
red examples are documented in
`ai-docs/to-do/VULKAN_BLOOM_COMPUTE_VERIFICATION_20261006.md`.

Input Attachment / Multi Subpass (T13-05)
---------------------------------------

`RealProjectSubpass.cpp` is an opt-in diagnostic inside the actual Windows
`test-cases.exe`. Apply the cumulative `real-project-integration.patch` to the
original project once; do not reapply it to an already integrated checkout.
The entry stays inert without `CC_VULKAN_REAL_PROJECT_SUBPASS`. It is mutually
exclusive with the cache/prewarm, indirect and alternate resource-root modes.

```powershell
& D:/Creator/3.8.8/resources/tools/cmake/bin/cmake.exe --build E:/AIProMax/github/cocos4-vulkan-test-project/build/windows/proj --config Debug --target test-cases --parallel 4
$previous = Get-Item Env:CC_VULKAN_REAL_PROJECT_SUBPASS -ErrorAction SilentlyContinue
try {
    $env:CC_VULKAN_REAL_PROJECT_SUBPASS = 'content'
    # Use the existing SDK Validation environment and close the test window normally.
    $process = Start-Process E:/AIProMax/github/cocos4-vulkan-test-project/build/windows/proj/Debug/test-cases.exe -WorkingDirectory E:/AIProMax/github/cocos4-vulkan-test-project/build/windows/proj/Debug -PassThru
    $process.WaitForExit()
    $process.ExitCode
} finally {
    if ($null -ne $previous) { $env:CC_VULKAN_REAL_PROJECT_SUBPASS = $previous.Value }
    else { Remove-Item Env:CC_VULKAN_REAL_PROJECT_SUBPASS -ErrorAction SilentlyContinue }
}
```

Subpass 0 draws red/green halves; subpass 1 uses `subpassLoad` and writes
green/blue halves. Both images are copied and checked byte-for-byte for
32x16 → 24x12 → 32x16 resource reconstruction. `skip-producer` deliberately
omits the first draw and must return 1 with three golden failures. An unknown
mode returns 1 before Vulkan initialization. Multi-subpass stays Classic even
when the modern dynamic policy is enabled.

The finite evidence judge requires five native results plus a same-executable
RenderDoc replay, actual descriptor/image linkage, SPIR-V and full image bytes:

```powershell
python native/tests/vulkan-real-project/verify-subpass-content.py <evidence-root> --self-test
```

The optional `CC_VULKAN_REAL_PROJECT_SUBPASS_CAPTURE` file prefix requires an
already injected RenderDoc API; the diagnostic does not load it itself. It
captures the three phases explicitly, instead of an unrelated menu frame.
The evidence includes the actual pre-fix terminal-store WAW and missing-producer
negative. BestPractices access-layout advisories remain visible. This check
covers color input attachments, two subpasses and finite resource reconstruction;
it does not certify depth input, MSAA, preserve/aliasing, device loss, mobile
platforms or performance. See `ai-docs/to-do/VULKAN_INPUT_ATTACHMENT_VERIFICATION_20261006.md`.


Resolve / Secondary / Fragment Shading Rate (T13-06)
--------------------------------------------------

The cumulative project patch adds `RealProjectResolve` and `RealProjectFeatures`
to the same `test-cases` device/render thread. Both are default OFF and mutually
exclusive with other diagnostics. No second device or replacement Resources.

`CC_VULKAN_REAL_PROJECT_RESOLVE=color2|color4|depth2|depth4` runs Classic and
engine Dynamic resolve with actual readback; use FORCE_API=1.3, RENDERING=dynamic,
SUBMIT=legacy (the resolve identity hook observes legacy QueueSubmit). BARRIER
can be legacy or sync2; COMPLETION can be auto or fence. `skip-color`,
`wrong-uniform` and `skip-depth` deliberately fail golden checks (exit1), even
when Classic/Dynamic mutual diff remains zero. Valid 1.1+Classic is UNSUPPORTED3,
not PASS; forced 1.1/1.2+Dynamic is rejected before device creation.

`CC_VULKAN_REAL_PROJECT_FEATURES=secondary|fsr` runs three resource sizes.
Secondary records/executes a real child draw and drops user facades before
submission. FSR uses a queried R8_UINT attachment and a shader that reads actual
ShadingRateKHR; its production gate stays Classic. `skip-execute` and
`fsr-wrong-rate` must fail actual pixels. Features supports the observed modern
Submit2/Sync2 policy; secondary also ran forced1.1/fence/legacy. Unsupported
FSR is exit3, not PASS. Unknown/empty/conflicting modes reject before Vulkan.

Optional `CC_VULKAN_REAL_PROJECT_RESOLVE_CAPTURE` and
`CC_VULKAN_REAL_PROJECT_FEATURES_CAPTURE` require an already injected RenderDoc
API and explicitly capture the diagnostic; neither loads a DLL itself.

```powershell
python native/tests/vulkan-real-project/verify-rendering-features.py <resolve-evidence-root> <features-evidence-root> --self-test
```

The offline judge requires actual normal shutdown, pixels, resource links and
SPIR-V, not only creation/exit or mutual diff. Nine native positives, four replay
cases and 30 rejected mutations/actual negatives were checked. Pipeline-only
resolve retirement and typed Secondary/FSR object retirement are distinct from
final VMA leak evidence. More resolve modes/stencil resolve, device loss, natural
low-version GPUs, platform/performance Gates are outside this finite contract.
See `ai-docs/to-do/VULKAN_RESOLVE_SECONDARY_FSR_VERIFICATION_20261006.md`.


Variant failure recovery (T13-07)
--------------------------------

`CC_VULKAN_REAL_PROJECT_FEATURES=secondary-variant-failure` requires the modern
Dynamic policy. A narrowly scoped diagnostic hook forwards the actual optional
variant creation, then returns OUT_OF_HOST_MEMORY with its real partial output.
The production factory must destroy that partial result exactly once, retain a
valid Classic pipeline, mark fallback and execute the real Secondary draw.
Three size waves and normal application shutdown are checked; ordinary Secondary
and FSR regressions ran against the same rebuilt executable. This is injected
API-failure handling, not a real driver OOM or device-loss recovery claim.

```powershell
python native/tests/vulkan-real-project/verify-rendering-features.py <resolve-root> <features-root> --failure-root <failure-root> --self-test
```

The optional failure-root adds three current native cases and four exact
recovery-marker rejection fixtures (34 total for this evidence revision).

Real-project WSI lifecycle (T13-07)
----------------------------------

`CC_VULKAN_REAL_PROJECT_WSI=content` runs the S1--S8 lifecycle on the existing
test-cases device: stable frames, resize, zero extent, surface replacement,
two swapchains, failed acquire and injected WSI result handling. Modern and
forced1.1/fence/legacy have separate evidence. `skip-surface` must fail the
actual missing-surface check and still retire its objects. This diagnostic
checks lifecycle/API identity; it does not replace scene pixel evidence.

```powershell
python native/tests/vulkan-real-project/verify-wsi-project.py <evidence-root> --self-test
```

See `ai-docs/to-do/VULKAN_REAL_PROJECT_WSI_VERIFICATION_20261006.md` for the
finite contract and preserved failures. Default absent is OFF.

Real-project performance sampling (T05)
---------------------------------------

The cumulative project patch also links `RealProjectPerformance`. Its default
is OFF. The runner builds the original Debug target and a separate Debug tree
with Validation compiled OFF, then samples the actual registered atlas-compress
scene. No replacement device, scene Resources, per-frame host query wait or
extra submission is introduced. Each cold/restored pair uses one isolated cache
file and separate processes; restored driver input bytes must match validated
cold output. Existing operating-system/driver caches are not made cold.

```powershell
python native/tests/vulkan-real-project/run-performance-matrix.py --root <new-evidence-root> --repeats 5 --frames 1000 --warmup 300
python native/tests/vulkan-real-project/verify-performance.py <evidence-root>
```

The default matrix is 40 processes: Validation ON/OFF, legacy/classic versus
Sync2/Submit2/Dynamic policy, five rounds and cold/restored. Dynamic policy is
not evidence of Dynamic draw; the scene can correctly fall back to Classic.
The fixed scene requires three draws, one submission and three submitted
primary-command-buffer timestamp intervals per sampled frame. GPU queries are
read after normal shutdown completion, outside the sampled interval.

Raw CSV includes main frame period, main active/thread CPU time, present driver,
scoped waits and unclassified residual, plus submitted GPU intervals. FIFO
display pacing and debugger log capture remain part of this measurement.
GetThreadTimes has coarse actual resolution despite its 100ns unit. Neither
CPU/wall subtraction nor a short submitted-CB interval proves GPU idleness.

Startup markers separate backend, instance/device API calls, cache configuration,
shader compilation/reflection/module work, graphics factory/driver work and
first main frame. Native entry is the diagnostic preparation entry, not WinMain's
first instruction. Collector QPC before CreateProcessW supplies a separate
launch-to-first-present-API interval; neither endpoint proves first visible pixel.
Maximum material factory/shader intervals are retained alongside first-use data.

The shared judge requires exact logs, actual exit/Validation/leak zero, CSV and
launch clock, real runtime paths, cold/warm payload linkage and source/executable
identity. Matrix PASS means valid evidence; it does not grant a performance Gate.
Median or P95 regression above 3% requires profile evidence and an acceptance
reason. NVIDIA pre/post sensor observations are not thermal-control proof.

`CC_VULKAN_REAL_PROJECT_PERF_WAIT_TRACE=1` additionally writes
`performance.wait-identities.csv`: actual acquire-to-submit wait identity,
timeline signal/wait value and next-slot ownership. It is absent by default;
it adds counters, not Vulkan work or synchronization.

`CC_VULKAN_REAL_PROJECT_PERF_PRESENT=immediate` is an independent, explicit
diagnostic creation policy. It requests the existing VsyncMode::OFF selection
before a non-XR swapchain is created; no VkCreateInfo or semaphore is rewritten
by a PFN hook. Default absent preserves the application's policy. The owner
restores the measurement policy after rendering stops. The run must observe
actual presentMode=0; unsupported selection is a failed diagnostic, not PASS.
Immediate data investigates FIFO pacing and is never relabeled as a default
FIFO or visible-pixel benchmark. `encoder` offline expectations require only
Sync2/Submit2 ON with Dynamic OFF/Classic override ON, for M4-A isolation.

Formal-root `--self-test` now selects the actual cold/modern/ON manifest case;
it requires no nonexistent smoke subdirectory. Normal formal judging also
requires a terminal complete/frozen matrix, all expected cold/restored cases
and manifest success, before grouping the raw samples.

## M4-B known-buffer visibility (2026-10-06)

`RealProjectVisibility.cpp` is compiled into the same `test-cases` target and is
inert unless `CC_VULKAN_REAL_PROJECT_VISIBILITY=content|fallback|wrong-input` is
set. It uses the existing device/render thread and three real compute dispatches
with 64/128/64-byte inputs. Each recording keeps the original descriptor buffer
and pipeline alive after buffer resize and user-facade destruction before submit.
Full readback, successful actual submission, and twelve typed pipeline-related
retirements are checked through real device shutdown. These checks do not infer
buffer destruction from a VMA-cached global PFN hook; allocator leak output is
checked independently.

This fixture explicitly declares read-only compute consumption in the production
barrier manager metadata; its second input also declares an indirect consumer to
check that access masks do not leak into the first resource. It does **not** claim
that descriptor-buffer consumer discovery was added. The fallback fixture leaves
one input's consumer metadata unknown, verifies the original global fallback, and
supplies the manual compute dependency that unknown descriptor usage requires.
`wrong-input` executes the full valid chain but changes actual uploaded contents;
all three golden comparisons must fail, independently of comparing two paths.

```powershell
C:/Python313/python.exe native/tests/vulkan-real-project/run-visibility-matrix.py `
  --root native/build/vulkan-modernization/visibility-new-evidence
# Offline re-judge; this does not build or execute the application:
C:/Python313/python.exe native/tests/vulkan-real-project/verify-visibility.py `
  native/build/vulkan-modernization/win-visibility-t07-lead/final-combined --self-test
```

The runner refuses an existing evidence directory, builds current Debug sources
before running six bounded cases, clears inherited diagnostic policies before
applying each configuration, and requires both actual runtime paths and shutdown
evidence. The native cases cover legacy, Sync2/Submit2, 1.1/fence, unknown-consumer
fallback, wrong contents, and malformed mode. Evidence records actor `Lead`.
Known-buffer barriers cover the whole physical allocation including frame
instances; they retain the GPU resource without adding a new byte-use slot lease.
Unknown consumer metadata/bounds/handles preserve the old global path. No new
wait, submission, capability enablement or default feature policy is introduced.
This is finite Windows correctness evidence, not a platform or performance Gate.

The current phase schema requires both `submits=1` for the compute/readback CB
and `managerSubmits=1` for the distinct upload/barrier transport CB. Earlier
`green` evidence used the previous schema; do not reinterpret it as current
submission proof. The six final cases and 23 judge fixtures pass with the same
current executable used for the post-render matrix.

## M4-B Dynamic post-render scopes

```powershell
C:/Python313/python.exe native/tests/vulkan-real-project/run-scope-matrix.py `
  --root native/build/vulkan-modernization/post-scopes-new-evidence
```

This fresh-build runner uses the actual Debug project and eight finite cases:
legacy/Sync2 barriers with color/depth resolve at 2x/4x. Its existing resolve
fixture supports legacy submit, so the runner explicitly selects that path.
`CC_VULKAN_REAL_PROJECT_SCOPES=post` installs forward-first observation only.
Each Dynamic phase registers owned image-handle snapshots and the actual CB;
post barriers must match each physical attachment exactly once with precise
destination masks. Golden readback, actual submission and shutdown retirement
remain mandatory. Eight single-mutation scope evidence fixtures must fail.

Only candidate-declared next attachment writes are narrowed. Unknown prior
discard uses keep conservative pre dependencies; completion signals keep whole
submission scope. `CC_VULKAN_REAL_PROJECT_PERF_VISIBILITY_TRACE=1` optionally
writes `performance.visibility.csv` with frame, manager calls/ns, checked
buffer/image counts and generated global/buffer/image barrier counts. Absent
preserves the original performance CSV and reads no visibility clocks.
The measured static scene has no steady-state buffer uploads; its performance
data proves no upload speedup. See the dated scope report for the original
over-3% samples, same-binary controls, same-path repeat results and limitations.


## Continuous upload stress and synchronized batch measurement

The actual Windows project remains the verification target. These diagnostic
modes are OFF by default and do not change the normal game policy.

```powershell
python native/tests/vulkan-real-project/run-upload-stress.py --root native/build/vulkan-modernization/<fresh-stress-root>
python native/tests/vulkan-real-project/run-upload-performance.py --root native/build/vulkan-modernization/<fresh-performance-root> --baseline-source native/build/vulkan-modernization/win-visibility-t07-lead/visibility-profile/VKCommands-profile-baseline.cpp
python native/tests/vulkan-real-project/run-upload-controls.py --root native/build/vulkan-modernization/<fresh-controls-root> --fresh-build
```

Replace angle-bracket root names with new paths. Baseline source is a local
historical evidence artifact, not a repository input; supply the preserved old
BarrierManager implementation when reproducing elsewhere. The performance
runner substitutes only that function, builds both OFF versions, restores
source bytes in finally, and runs both images at the same actual EXE path.
It restores the current EXE afterwards. Run sequentially, without other GPU
workloads/builds. `--build-evidence <root>` can reuse an exact frozen current
artifact; it is mutually exclusive with `--fresh-build`.

Stress executes 24 alternating-size buffer and RGBA8 texture waves, four
uploads per input, whole readback goldens, resize/drop pending leases and
shutdown retirement. It includes legacy, Sync2/Submit2, forced1.1/fence and a
real corrupt-content negative. It does not cover mipgen or a texture uploaded
again after being sampled by a draw.

Performance keeps objects outside 1300 batches (300 warmup/1000 samples),
prepares input before timing and resets the staging pool only after completion
outside timing. Batch CSV has 13 exact columns: sample, batch_ns, upload_ns,
manager_ns, compute_cb_gpu_ns, checked_bytes, staging_sources, record_ns,
submit_ns, completion_ns, wait_api_ns, wait_calls, wait_failures. Five host
parts close to batch; wait API is a subset of completion. Whole content and
actual forwarding identity are mandatory. The GPU interval covers only the
user compute/readback CB, not upload transport, the whole frame or display.
Query-pool cleanup waits for tracked completion outside timing; run-end clears
borrowed observation handles before ordinary scenes continue. No query-failure
fault injection has been executed for that cleanup branch.

See `ai-docs/to-do/VULKAN_UPLOAD_STRESS_VERIFICATION_20261006.md` for rejected
measurement assumptions and version-specific evidence. Old 7-column samples
must use their archived judge. Endpoint NVIDIA sensors do not prove thermal
stability. Data/Validation PASS does not grant the performance or platform Gate.
