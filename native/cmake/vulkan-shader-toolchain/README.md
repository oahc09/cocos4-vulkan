# vulkan-shader-toolchain（M0-E）

锁定版本的 glslang + SPIRV-Tools + SPIRV-Headers **源码构建**独立项目，替代
`native/external` 中不可重建的预编译 shader 库。产物只落在
`native/build/vulkan-modernization/shader-toolchain/`（git 忽略区），不入源目录。

## 锁定版本（Lead 决策 Chosen，未验收 Selected）

| 组件 | commit | tar.gz SHA256 |
|---|---|---|
| glslang | `46ef757e048e760b46601e6e77ae0cb72c97bd2f`（tag vulkan-sdk-1.3.296.0，实测版本 15.0.0） | `ef16e5924e07f49b6280158645051ed5930ee5c5628e573fd2fb49e210aaca71` |
| SPIRV-Tools | `6dcc7e350a0b9871a825414d42329e44b0eb8109`（glslang known_good 钉定） | `e3665378ab0ae8efb327027ee103d10c3c9e05823dea8b28893c00886ffe1a23` |
| SPIRV-Headers | `2a9b6f951c7d6b04b6c21fe1bf3f475b68b84801`（glslang known_good 钉定） | `1698e1373bd6e59a263acef821c4d955c561b991feb6db8199833ef19ffe8a37` |

顺序：SPIRV-Headers → SPIRV-Tools → glslang（`SPIRV-Headers_SOURCE_DIR` 传入，
glslang 复用已存在的 `SPIRV-Tools-opt` target，绝不 find_package 到不匹配版本）。
静态库；上游 tests/CLI/shared/HLSL 关闭；`ENABLE_OPT=ON`（不得禁优化绕过验证）；
MSVC 默认 CRT（/MD /MDd）与引擎一致。

## 复现命令

Windows（VS 2026，含现代 harness 与安装）：

```powershell
cmake -S native\cmake\vulkan-shader-toolchain ^
      -B native\build\vulkan-modernization\shader-toolchain\win64-vs ^
      -G "Visual Studio 18 2026" -A x64 ^
      -DCC_SHADER_UPSTREAM_CACHE=<repo>\native\build\vulkan-modernization\upstream ^
      -DCC_BUILD_SPIRV_HARNESS=ON
cmake --build native\build\vulkan-modernization\shader-toolchain\win64-vs --config Release
cmake --install  native\build\vulkan-modernization\shader-toolchain\win64-vs --config Release ^
      --prefix native\build\vulkan-modernization\shader-toolchain\install\win64
```

Android arm64（NDK 26.1.10909125 / Ninja / android-26 / c++_static，无设备仅编译）：

```powershell
cmake -S native\cmake\vulkan-shader-toolchain ^
      -B native\build\vulkan-modernization\shader-toolchain\android-arm64 -G Ninja ^
      -DCMAKE_TOOLCHAIN_FILE=D:/Android/Sdk/ndk/26.1.10909125/build/cmake/android.toolchain.cmake ^
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_static ^
      -DCMAKE_BUILD_TYPE=Release -DCC_BUILD_SPIRV_HARNESS=OFF ^
      -DCC_SHADER_UPSTREAM_CACHE=<repo>\native\build\vulkan-modernization\upstream
cmake --build native\build\vulkan-modernization\shader-toolchain\android-arm64
```

`CC_SHADER_UPSTREAM_CACHE` 可省略（默认走 codeload 钉 commit 下载，任何机器可复现）；
提供时必须通过同一 URL_HASH 校验。Windows 注意：本地缓存以**绝对路径**直接作为 URL
传入（`file:///E:/…` 会被 CMake 解析成 `/E:/…` 导致失败）。

## 引擎接入要点（Lead）

- 安装树头路径：`glslang/Public/ResourceLimits.h`（新）、`glslang/SPIRV/*`（稳定旧路径）、
  `glslang/build_info.h`、`spirv/unified1/spirv.h`
- 链接顺序：glslang → MachineIndependent → GenericCodeGen → OSDependent → SPIRV →
  SPIRV-Tools-opt → SPIRV-Tools（+ glslang-default-resource-limits）
- 旧链接表中的 `OGLCompiler` 已被上游移除；引擎使用静态目标。SPIRV-Tools 上游仍会同时生成 shared 目标，BUILD_SHARED_LIBS=OFF 不表示其产物完全无 shared。
- 实际安装清单见 install 树 `lib-list.txt`；来源/hash/生成器见 `shader-toolchain-version.txt`
