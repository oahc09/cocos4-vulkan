# Vulkan review regressions

These tests require C++17, Python 3, and CMake 3.15 or newer. They do not require
the Cocos external libraries, a Vulkan loader, a window system, or a GPU.

```sh
cmake -S native/tests/vulkan-review -B build/vulkan-review -DCMAKE_BUILD_TYPE=Release
cmake --build build/vulkan-review --parallel 2
ctest --test-dir build/vulkan-review --output-on-failure
node --test .github/workflows/get-native-external-version.test.js
```

The lifecycle fixture compiles **unchanged extracted production methods** from
`VKDevice.cpp`, `VKSwapchain.cpp`, and `VKQueue.cpp` with small engine seams and
injected Vulkan results. The actual completion tracker and WSI state headers
are used. Extraction manifests contain normalized-source SHA-256 values. This
checks production control flow rather than a separate hand-written lifecycle
model. It is still CPU fault injection, not a complete engine or real WSI test.

All lifecycle tests force `CC_DEBUG=0` and `NDEBUG`; expectations use always-on
checks. The 28 scenarios include:

- Submission OOM, finite wait timeout, and wait OOM: no premature release
- Retry of the exact submitted drain point; no second wait on a consumed binary semaphore
- Pending presentation fences, partial completion, and core-WSI wait failure
- Public swapchain destruction transferring the only remaining owning lease
- Surface-only destruction retried by the device's normal lifecycle
- Real acquire-loop failure exit, present-fence wait/reset rejection
- Transport recordings and replacement semaphores retained across failed submits
- Timeline and fence completion backends, multi-window pairing
- Surface-loss release falling back to retirement instead of endless release retries
- Device loss handled as teardown, without advancing normal completion counters
- Lost submits waiting for idle **before** destroying possibly submitted handles;
  a failed idle preserves owners until a later valid teardown wait
- Explicit final-shutdown timeout propagation and individually checked present fences

A separate executable extracts the entire production fence-pool class and
checks that cancelling a failed submission removes only its unsubmitted tail
fence from active waits while retaining reusable capacity. The CMake fixture
executes the actual external-root selection and Windows runtime blocks with
both a local external tree and the built-in fallback, including spaces in paths.
It does not claim to emulate Windows toolchain discovery.

The GitHub Actions workflow also runs the existing 27 CPU contracts in Debug
and Release. Native platform CI and real-window engine tests remain separately
necessary for integration coverage.
