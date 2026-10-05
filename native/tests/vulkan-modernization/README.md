# SPIR-V Target Verification Harness（M0-B）

> 隶属：`ai-docs/to-do/Cocos_Vulkan_1.1_to_1.3_Modernization_Task_Plan.md` 第 6/8 节
> （M0：SPIR-V target 真实编译验证）。
> 目的：用引擎**实际链接**的 bundled glslang 静态库与**与引擎一致**的编译流程，
> 实证每个 Vulkan target（1.1/1.2/1.3）能真实产出的 SPIR-V 版本；禁止静默回退冒充通过。

## 目录内容

| 文件 | 说明 |
|---|---|
| `CMakeLists.txt` | 独立 executable 工程，链接 `native/external/<platform>/libs/glslang` 预编译静态库与 `native/external/sources` 头文件；不接入引擎大构建 |
| `spirv_target_harness.cpp` | 编译样例 vertex/fragment/compute shader；复刻 `SPIRVUtils::compileGLSL` 流程（含 `optimizeSize=true`、`glslang::DefaultTBuiltInResource`、parse/link 日志处理）；核对 SPIR-V header version word 并落盘 `.spv` |
| `verify-spirv.ps1` | 验证入口：运行 harness → 独立读 `.spv` header word → 用 Vulkan SDK `spirv-val --target-env vulkanN.N` 验证；非 0 退出全部传播 |
| `build/`、`spirv-out/` | 本地产物（构建树与 `.spv` 输出），已与源码隔离 |

## 复现命令（Windows / VS 2026）

```powershell
cd <repo>\native\tests\vulkan-modernization

# 1. configure（默认指向 ../../external；升级依赖后用 -DCC_GLSLANG_EXTERNAL_ROOT 指向新树复跑）
cmake -S . -B build\win64-vs -G "Visual Studio 18 2026" -A x64

# 2. build
cmake --build build\win64-vs --config Release

# 3. run（单 target）
.\build\win64-vs\Release\spirv_target_harness.exe 1.1 --out-dir spirv-out
.\build\win64-vs\Release\spirv_target_harness.exe 1.2 --out-dir spirv-out
.\build\win64-vs\Release\spirv_target_harness.exe 1.3 --out-dir spirv-out

# 3'. run（全 target 汇总）
.\build\win64-vs\Release\spirv_target_harness.exe --all --out-dir spirv-out

# 4. 一键验证（harness + header word + SDK spirv-val）
powershell -ExecutionPolicy Bypass -File verify-spirv.ps1
```

## 退出码语义（harness 与 verify 脚本一致）

| exit | 含义 |
|---|---|
| 0 | 所有请求 target：编译 PASS + version word 匹配 +（脚本内）spirv-val PASS |
| 1 | 存在 FAIL（parse/link/emit 失败、magic/version word 不匹配、spirv-val 报错） |
| 2 | 存在 UNSUPPORTED：bundled glslang 无法表达该 target（如 11.5.0 之于 Vulkan 1.3/SPV 1.6）。**UNSUPPORTED 不是 PASS**，也不会用回退产物冒充 |

## target → 期望产物（SPIR-V header word 1，小端 uint32）

| target | client | SPV 目标 | 期望 word |
|---|---|---|---|
| 1.1 | `EShTargetVulkan_1_1` | 1.3 | `0x00010300` |
| 1.2 | `EShTargetVulkan_1_2` | 1.5 | `0x00010500` |
| 1.3 | `EShTargetVulkan_1_3` | 1.6 | `0x00010600`（需 glslang ≥ 11.10 的枚举） |

版本可用性判定与引擎 guard 一致：`CC_GLSLANG_VERSION_GREATOR_OR_EQUAL_TO(11,10,0)`
（`SPIRVUtils.cpp:44`）。bundled 11.5.0 头文件只有 `EShTargetVulkan_1_0/1_1/1_2`、
`EShTargetSpv_1_0..1_5`（`external/sources/glslang/Public/ShaderLang.h:166-181`），
因此 1.3 target 只能报告 UNSUPPORTED——引擎路径此时会**静默回退 Vulkan1.2/SPV1.5**
（`SPIRVUtils.cpp:60-98` 的 else 分支），本 harness 明确不回退、不冒充。

## 验证边界（如实声明）

1. 本 harness 验证的是**捆绑 glslang 库的 target-env 编译能力**，不是引擎运行时行为、
   不是 swapchain/pipeline/驱动行为，也不能替代移动 GPU 证据。
2. `spirv-val` 来自本机 Vulkan SDK（版本随 SDK），属**外部对照工具**；捆绑包内的
   SPIRV-Tools 是否能验证某 target 与本验证相互独立。
3. SDK glslangValidator（16.2.0）可用于对照演示“升级后的 1.3 目标长什么样”，
   但那是 **SDK 路径而非引擎路径**，不能记入引擎依赖验证结果：
   ```powershell
   # 仅对照，不算引擎验证（sample.comp 在本目录，产物 sdk-contrast.comp.spv 已被 .gitignore 忽略）
   & "$env:VULKAN_SDK\Bin\glslangValidator.exe" -V -S comp --target-env vulkan1.3 -o sdk-contrast.comp.spv sample.comp
   & "$env:VULKAN_SDK\Bin\spirv-val.exe" --target-env vulkan1.3 sdk-contrast.comp.spv
   ```
4. 样例 shader 为本 harness 自带（vertex：UBO MVP 变换；fragment：采样+实际输出；
   compute：SSBO 读→写）。带 `#version 450`，target-env 由
   `setEnvClient/setEnvTarget` 决定，与引擎一致。
5. 每次常规验证写入独立 `spirv-out/run-<时间戳>/` 子目录，仅校验本次产物，
   旧产物无法掩盖本次未产生的输出；`-SkipHarnessRun` 模式则要求指定目录内
   **所选 target 矩阵完整**（缺目录/缺文件即 FAIL）。

## 验证脚本负例（M0-C 实测，2026-10-02）

| 场景 | 命令 | 预期 exit | 实测 |
|---|---|---|---|
| 默认全档（11.5 基线） | `verify-spirv.ps1` | 2（1.3 UNSUPPORTED） | 2 |
| 仅 1.1/1.2 | `verify-spirv.ps1 -Targets 1.1,1.2` | 0 | 0 |
| 未知 target | `verify-spirv.ps1 -Targets 1.4` | 1（拒绝） | 1 |
| 缺产物文件 | `-SkipHarnessRun -Targets 1.1 -OutDir <dir缺compute.spv>` | 1 | 1 |
| 目录不存在 | `-SkipHarnessRun -Targets 1.1,1.2 -OutDir <不存在>` | 1 | 1 |

## 升级后复用

依赖成套升级（Headers/Volk/VMA/glslang/SPIRV-Tools/Validation）后：

```powershell
cmake -S . -B build\win64-vs-upgraded -G "Visual Studio 18 2026" -A x64 `
      -DCC_GLSLANG_EXTERNAL_ROOT=<指向升级后的 external 根>
cmake --build build\win64-vs-upgraded --config Release
.\build\win64-vs-upgraded\Release\spirv_target_harness.exe --all --out-dir spirv-out-upgraded
powershell -ExecutionPolicy Bypass -File verify-spirv.ps1 -HarnessExe .\build\win64-vs-upgraded\Release\spirv_target_harness.exe -OutDir spirv-out-upgraded
```

验收要求（对齐计划第 8 节）：三个 target 全部真实 PASS（exit 0），1.3 的 version word
必须 `0x00010600`；任何 UNSUPPORTED/回退都不得记录为通过。

## Loader smoke（M0-C）

`vulkan_loader_smoke`（`loader_smoke.cpp`）独立验证 vendored Vulkan-Headers
（vulkan-sdk-1.3.296.0）+ 再生成的 volk（header 296）：

```powershell
cmake --build build\win64-vs --config Release --target vulkan_loader_smoke
# 本机注册表 layer 路径失效（指向不存在的 C:\VulkanSDK），需显式指定 layer 目录：
$env:VK_LAYER_PATH = 'D:\Software\VulkanSDK\1.4.341.1\Bin'
.\build\win64-vs\Release\vulkan_loader_smoke.exe
```

行为：分别以 apiVersion 1.1/1.2/1.3 创建 headless instance，取
`effective = min(instance, physicalApiVersion)`，按 effective 要求对应 core 入口
（1.3 档必须加载 Lead 指定的六项 Modern 入口）；设备不启用任何现代 feature，
**只检查入口加载、不调用未启用 feature 的命令**；KHRONOS_validation 可用则启用并
计 error，不可用则显式报告 NOT VERIFIED。本机实测（RTX 4060，physical 1.4.351）：
三档 ALL LOADED、validation errors=0、exit 0；未设 VK_LAYER_PATH 时 layer 不可见
（注册表指向失效的 C:\VulkanSDK 路径），报告 NOT VERIFIED。

## 现代源码工具链 harness（M0-E）

`spirv_target_harness.cpp` 同一源文件通过 `__has_include` 双适配：旧捆绑 11.5
（`glslang/StandAlone/ResourceLimits.h` + `DefaultTBuiltInResource`）与新源码
工具链（`glslang/Public/ResourceLimits.h` + 全局 `GetDefaultResources()`；
构建树顶层 `SPIRV/GlslangToSpv.h`；`spirv/unified1/spirv.h`）。无 stub、无假资源。

源码工具链由 `native/cmake/vulkan-shader-toolchain/` 独立项目构建（锁定的官方
commit + SHA256，见该目录 CMakeLists），可选 `CC_BUILD_SPIRV_HARNESS=ON` 产出
链接源码工具链 target 的同名 harness（绝不链旧 external 库）：

```powershell
cmake -S native\cmake\vulkan-shader-toolchain -B native\build\vulkan-modernization\shader-toolchain\win64-vs `
      -G "Visual Studio 18 2026" -A x64 `
      -DCC_SHADER_UPSTREAM_CACHE=<repo>\native\build\vulkan-modernization\upstream `
      -DCC_BUILD_SPIRV_HARNESS=ON
cmake --build native\build\vulkan-modernization\shader-toolchain\win64-vs --config Release
cmake --install  native\build\vulkan-modernization\shader-toolchain\win64-vs --config Release `
      --prefix native\build\vulkan-modernization\shader-toolchain\install\win64
# 现代 harness 三档验证（期望 exit 0）
powershell -ExecutionPolicy Bypass -File verify-spirv.ps1 `
      -HarnessExe ..\..\build\vulkan-modernization\shader-toolchain\win64-vs\Release\spirv_target_harness.exe `
      -OutDir spirv-out-modern
# Android arm64 交叉编译（无设备，不宣称运行 PASS）
cmake -S native\cmake\vulkan-shader-toolchain -B native\build\vulkan-modernization\shader-toolchain\android-arm64 `
      -G Ninja -DCMAKE_TOOLCHAIN_FILE=D:/Android/Sdk/ndk/26.1.10909125/build/cmake/android.toolchain.cmake `
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_static `
      -DCMAKE_BUILD_TYPE=Release -DCC_BUILD_SPIRV_HARNESS=OFF -DCC_SHADER_UPSTREAM_CACHE=<repo>\native\build\vulkan-modernization\upstream
cmake --build native\build\vulkan-modernization\shader-toolchain\android-arm64
```

本机实测（2026-10-02）：现代 harness `--all` exit 0（1.1→0x00010300、1.2→0x00010500、
**1.3→0x00010600**，各 3 shader，spirv-val 匹配全 PASS）；旧基线 harness 仍 1.3
UNSUPPORTED（exit 2，退化测试保留）；Android arm64 全库构建 exit 0。

## M6 rendering-format capability probe (query-only)

`vulkan_rendering_format_probe` (new target in this CMakeLists) links ONLY
the vendored Vulkan-Headers 1.3.296 + engine-tree volk.c, creates an instance
at apiVersion 1.3 (loader shortfall exits UNSUPPORTED, never PASS), enumerates
ALL physical devices, and prints for each: identity + apiVersion, real
VkPhysicalDeviceProperties2 + DepthStencilResolveProperties (physical >= 1.2
only), framebuffer color/depth/stencil sample-count limits, the fixed format
matrix (RGBA8_UNORM with COLOR_ATTACHMENT and COLOR_ATTACHMENT|TRANSFER_SRC;
D16/D32/D24S8/D32S8/S8 with DEPTH_STENCIL_ATTACHMENT) via
vkGetPhysicalDeviceFormatProperties and per-format
vkGetPhysicalDeviceImageFormatProperties (2D/optimal/flags 0).
VK_ERROR_FORMAT_NOT_SUPPORTED is data (NA); any OTHER non-SUCCESS image-format
result fails the whole probe (no QUERY_COMPLETE, exit 1). The real loader
version is queried first (vkEnumerateInstanceVersion, absent = 1.0): <1.3
exits UNSUPPORTED with code 3. Every D/S format is also probed with
DEPTH_STENCIL_ATTACHMENT|TRANSFER_SRC (labels `+DS+XFER`) - this is what
decides later depth/stencil plane readback viability. No logical device,
queue, image, or draw is created; this proves QUERY results only - not any
Dynamic pipeline/rendering/resolve execution, and the instance tier is not
treated as a low-version GPU proof. Exit codes: 0 = QUERY_COMPLETE,
1 = failure (incl. unexpected query results / bad CLI argument), 3 = loader
or instance UNSUPPORTED. `--self-test` is a CPU-only judge test (no loader/GPU): NA is
accepted, OUT_OF_HOST_MEMORY / DEVICE_LOST are rejected, and three distinct
sample flags format distinctly (1 / 2 / 4).

```powershell
# from the repository root
cmake -S native/tests/vulkan-modernization -B native/build/vulkan-modernization/mod-tests -G "Visual Studio 18 2026" -A x64
cmake --build native/build/vulkan-modernization/mod-tests --config Release --target vulkan_rendering_format_probe
native/build/vulkan-modernization/mod-tests/Release/vulkan_rendering_format_probe.exe
native/build/vulkan-modernization/mod-tests/Release/vulkan_rendering_format_probe.exe --self-test
```

rev2: per-call formatting (no shared static buffer aliasing across multiple
fields in one printf line - rev1 printed aliased duplicate values where two
fields coincidentally matched); unexpected image-format results now fail the
run; loader version gate + PFN checks; `+DS+XFER` queries added; hex values
printed with real %x.

Lead review: check the creation dispatcher before use; retain an independent
instance cleanup dispatcher so missing PFNs and allocation exceptions never
call a null destroy function. The absent version-query fallback is actually
Vulkan 1.0. Unexpected exceptions fail with exit 1.
