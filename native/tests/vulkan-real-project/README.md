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
