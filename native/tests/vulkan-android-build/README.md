# Android arm64 cocos_engine 构建 + 最终链接探针（M0-G）

独立入口：包含**完整** `cocos_engine`（`native/CMakeLists.txt`），
`CC_USE_VULKAN=ON / CC_USE_GLES3=OFF`，NDK 交叉编译与真实 Android 模板一致；
`vk_android_probe` 共享库按模板 `templates/cmake/android.cmake` 的形态做**真实最终链接**
（whole-archive `cocos_jni` + `cocos_engine`），并通过导出函数
`cc_vulkan_android_probe(selector)` 真实引用：

- `cc::gfx::CCVKDevice::getInstance`（gfx-vulkan 后端入口）
- `cc::gfx::DeviceManager::create(DeviceInfo)`（完整设备 bootstrap 链：VKGPUContext/volk/swapchain）
- `cc::gfx::SPIRVUtils::compileGLSL`（源码 glslang 工具链）
- `cc::pipeline::bindingMappingInfo`（DeviceManager 依赖的外联符号）

另提供游戏侧 `cocos_main` 定义（`AndroidPlatform.cpp:81` 声明，真实模板由游戏 main 提供）。

**无设备：本入口只做交叉编译与最终链接验证，不运行、不做 Validation/画面验收。**

## 构建命令（Windows 主机）

```powershell
.\build.ps1
# 或手动（注意：Windows PowerShell 5.1 会把未加引号的 -DX=3.5 拆成 -DX=3 与 .5，必须加引号）：
cmake -S native\tests\vulkan-android-build `
      -B native\build\vulkan-modernization\android-engine -G Ninja `
      "-DCMAKE_TOOLCHAIN_FILE=D:/Android/Sdk/ndk/26.1.10909125/build/cmake/android.toolchain.cmake" `
      "-DANDROID_ABI=arm64-v8a" "-DANDROID_PLATFORM=android-26" "-DANDROID_STL=c++_static" `
      "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" `
      "-DCC_SHADER_UPSTREAM_CACHE=<repo>\native\build\vulkan-modernization\upstream" `
      "-DUSE_PHYSICS_PHYSX=OFF"
cmake --build native\build\vulkan-modernization\android-engine --parallel 4
```

产物（全部在 git 忽略区 `native/build/vulkan-modernization/android-engine/`）：

- `libvk_android_probe.so` — 最终链接探针（未 strip，含完整符号表）
- `vulkan-shader-toolchain/{build,install}/` — Android 专属 shader 工具链 child 构建
  （`vulkan-dependencies.cmake` 要求不同平台不共享；child version 文件标 platform=Android）

## 验证命令与边界

- 符号/依赖检查：NDK `llvm-nm` / `llvm-readelf`（探针导出在 dynsym；引擎符号为
  hidden，查静态符号表）；实测计数见 PROGRESS §2G
- 现代 8 个 shader 静态库与 vendored Vulkan-Headers 的混用检查依据 `build.ninja`
  链接命令行与 include 顺序（vendored 在 khronos 之前）
- 上游 SPIRV-Tools 即使 `BUILD_SHARED_LIBS=OFF` 仍生成 `libSPIRV-Tools-shared.so`，
  引擎只消费 8 个静态库（与 Lead M0-F 结论一致）
- 本入口不声明任何运行/Validation/画面 PASS
