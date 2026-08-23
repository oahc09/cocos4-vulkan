# Windows x64 三方库升级评估报告

> 评估时间：2026-07-25  
> 路径：`native/external/win64/CMakeLists.txt`  
> 外部仓库：`cocos/cocos-engine-external` checkout `v4.0.0-4`

---

## 一、高优先级升级（安全驱动，强烈推荐）

### 1. OpenSSL — 1.1.1k → 3.5.7 LTS

| 项目 | 详情 |
|---|---|
| 当前版本 | 1.1.1k (2021-03-25) |
| 目标版本 | 3.5.7 LTS (2026-06-09) |
| 当前文件大小 | `libcrypto-1_1-x64.dll` 3370.5 KB + `libssl-1_1-x64.dll` 667 KB + `.lib` ~1107 KB ≈ **5.1 MB** |
| 预估新版大小 | `libcrypto-3-x64.dll` ~5.2 MB + `libssl-3-x64.dll` ~1.0 MB + `.lib` ~1.5 MB ≈ **7.7 MB (+50%)** |
| 升级收益 | **1.1.1 已 EOL (2023-09)**，大量已知 CVE；3.x 新 Provider 架构、FIPS 支持、TLS 1.3 优化、QUIC 支持 |
| 升级风险 | API 有变化（ENGINE → Provider），需验证 curl / websockets 的 OpenSSL 集成 |
| 下载地址 | https://github.com/openssl/openssl/releases/tag/openssl-3.5.7 |
| 源码 | https://www.openssl.org/source/openssl-3.5.7.tar.gz |
| Windows 预编译 | https://slproweb.com/products/Win32OpenSSL.html |

---

### 2. libcurl — 7.74.0-DEV → 8.21.0

| 项目 | 详情 |
|---|---|
| 当前版本 | 7.74.0-DEV (2020-12) |
| 目标版本 | 8.21.0 (2026-06-24) |
| 当前文件大小 | `libcurl.dll` 473.5 KB + `libcurl.lib` 22.1 KB ≈ **496 KB** |
| 预估新版大小 | `libcurl.dll` ~750 KB + `libcurl.lib` ~30 KB ≈ **780 KB (+57%)** (含 HTTP/3) |
| 升级收益 | 修复 100+ CVE，HTTP/3 (QUIC)，DNS-over-HTTPS，更优 TLS 1.3，连接池改进 |
| 升级风险 | 需同步升级 OpenSSL；部分废弃 API 已移除 |
| 下载地址 | https://curl.se/download.html |
| Windows 官方构建 | https://curl.se/windows/ |
| GitHub | https://github.com/curl/curl/releases/tag/curl-8_21_0 |

---

### 3. libwebsockets — 2.4.2 → 4.5.8

| 项目 | 详情 |
|---|---|
| 当前版本 | 2.4.2 (2017) |
| 目标版本 | 4.5.8 (2026-03-27) |
| 当前文件大小 | `websockets.dll` 196.5 KB + `websockets.lib` 64 KB ≈ **261 KB** |
| 预估新版大小 | `websockets.dll` ~450 KB + `websockets.lib` ~80 KB ≈ **530 KB (+103%)** |
| 升级收益 | 安全修复，HTTP/2 支持，全新事件循环，更好 TLS 集成，MQTT 支持，Secure Streams |
| 升级风险 | API 大幅变更（v2→v4），需重写连接/回调代码 |
| 下载地址 | https://github.com/warmcat/libwebsockets/releases/tag/v4.5.8 |
| 源码 | https://github.com/warmcat/libwebsockets/archive/refs/tags/v4.5.8.tar.gz |

---

### 4. libwebp — ~1.2.x → 1.6.0

| 项目 | 详情 |
|---|---|
| 当前版本 | ~1.2.x (ABI 0x0209) |
| 目标版本 | 1.6.0 (2025-06-30) |
| 当前文件大小 | `webp.lib` 931.7 KB + `libwebpmux.dll` 38.5 KB ≈ **977 KB** |
| 预估新版大小 | `webp.lib` ~1100 KB + `libwebpmux.dll` ~50 KB ≈ **1150 KB (+18%)** |
| 升级收益 | **CVE-2023-4863 Critical RCE** 修复！AVX2/SSE2 优化，编解码性能大幅提升 |
| 升级风险 | API 二进制兼容，低风险 |
| 下载地址 | https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0-windows-x64.zip |
| 源码 | https://github.com/webmproject/libwebp/releases/tag/v1.6.0 |

---

## 二、中优先级升级（性能/功能驱动，推荐）

### 5. SQLite — 3.35.4 → 3.53.4

| 项目 | 详情 |
|---|---|
| 当前版本 | 3.35.4 (2021-04-02) |
| 目标版本 | 3.53.4 (2026-07-24) |
| 当前文件大小 | `sqlite3.dll` 879 KB + `sqlite3.lib` 62.6 KB ≈ **942 KB** |
| 预估新版大小 | `sqlite3.dll` ~1400 KB + `sqlite3.lib` ~75 KB ≈ **1475 KB (+57%)** |
| 升级收益 | 18 个主版本：JSONB、数学函数、WAL2、性能大幅优化、安全修复 |
| 升级风险 | 极低，SQLite 向后兼容性优秀 |
| 下载地址 | https://sqlite.org/2026/sqlite-dll-win-x64-3530400.zip |
| 源码 | https://sqlite.org/2026/sqlite-amalgamation-3530400.zip |

---

### 6. zlib — 1.2.12 → 1.3.2

| 项目 | 详情 |
|---|---|
| 当前版本 | 1.2.12 (2022-03-27) |
| 目标版本 | 1.3.2 (2026-02-17) |
| 当前文件大小 | `zlib1.dll` 82 KB + `zlib.lib` 16.4 KB ≈ **98 KB** |
| 预估新版大小 | `zlib1.dll` ~90 KB + `zlib.lib` ~18 KB ≈ **108 KB (+10%)** |
| 升级收益 | CVE-2022-37434 修复，更好的 64-bit 支持，deflate 性能优化 |
| 升级风险 | 极低，API 完全兼容 |
| 下载地址 | https://github.com/madler/zlib/releases/tag/v1.3.2 |
| 源码 | https://zlib.net/zlib-1.3.2.tar.gz |

---

### 7. libuv — 1.41.0 → 1.52.1

| 项目 | 详情 |
|---|---|
| 当前版本 | 1.41.0 (2021) |
| 目标版本 | 1.52.1 (2026-03-06) |
| 当前文件大小 | `libuv.dll` 197.5 KB + `libuv.lib` 57.1 KB ≈ **255 KB** |
| 预估新版大小 | `libuv.dll` ~250 KB + `libuv.lib` ~65 KB ≈ **315 KB (+24%)** |
| 升级收益 | Windows IOCP 优化，io_uring (Linux)，更好的 socket 性能，bug 修复 |
| 升级风险 | 低，API 稳定 |
| 下载地址 | https://github.com/libuv/libuv/releases/tag/v1.52.1 |
| 源码 | https://dist.libuv.org/dist/v1.52.1/libuv-v1.52.1.tar.gz |

---

### 8. FreeType — 2.10.0 → 2.14.3

| 项目 | 详情 |
|---|---|
| 当前版本 | 2.10.0 (2019-03) |
| 目标版本 | 2.14.3 (2026-03-22) |
| 当前文件大小 | `freetype.lib` 3863.7 KB ≈ **3.8 MB** |
| 预估新版大小 | `freetype.lib` ~4500 KB ≈ **4.4 MB (+17%)** |
| 升级收益 | 多个安全漏洞修复，COLRv1 彩色字体，Variable Font 改进，CFF2 渲染优化 |
| 升级风险 | 低，API 基本兼容（仅 USE_DEBUG_RENDERER 时使用） |
| 下载地址 | https://sourceforge.net/projects/freetype/files/freetype2/2.14.3/ |
| 源码 | https://download.savannah.gnu.org/releases/freetype/freetype-2.14.3.tar.xz |

---

### 9. libpng — 1.6.37 → 1.6.58

| 项目 | 详情 |
|---|---|
| 当前版本 | 1.6.37 (2019-04) |
| 目标版本 | 1.6.58 (2026-04-15) |
| 当前文件大小 | `libpng16.lib` 1892.6 KB ≈ **1.8 MB** |
| 预估新版大小 | `libpng16.lib` ~2000 KB ≈ **2.0 MB (+6%)** |
| 升级收益 | 安全修复，ARM NEON / SSE2 性能优化，APNG 改进 |
| 升级风险 | 极低，1.6.x 系列 ABI 兼容 |
| 下载地址 | https://sourceforge.net/projects/libpng/files/libpng16/1.6.58/ |
| 源码 | https://github.com/pnggroup/libpng/releases/tag/v1.6.58 |

---

### 10. SDL2 — 2.28.1 → 2.30.11（保守方案）

| 项目 | 详情 |
|---|---|
| 当前版本 | 2.28.1 (2023-06) |
| 目标版本 | 2.30.11 (2025-01-01) |
| 当前文件大小 | `SDL2.dll` 1136.5 KB + `SDL2.lib` 179.2 KB + `SDL2main.lib` 3.6 KB ≈ **1.3 MB** |
| 预估新版大小 | `SDL2.dll` ~1300 KB + `SDL2.lib` ~200 KB ≈ **1.5 MB (+15%)** |
| 升级收益 | Windows 11 改进、Vulkan 优化、手柄数据库更新、多个 bug 修复 |
| 升级风险 | 低，SDL2 ABI 稳定 |
| 下载地址 | https://www.libsdl.org/release/SDL2-devel-2.30.11-VC.zip |
| GitHub | https://github.com/libsdl-org/SDL/releases/tag/release-2.30.11 |

---

### 10b. SDL2 → SDL3（激进方案）

| 项目 | 详情 |
|---|---|
| 当前版本 | SDL2 2.28.1 (2023-06) |
| 目标版本 | SDL3 3.4.12 (2026-07-01) |
| 当前文件大小 | `SDL2.dll` 1136.5 KB + `SDL2.lib` 179.2 KB + `SDL2main.lib` 3.6 KB ≈ **1.3 MB** |
| 预估新版大小 | `SDL3.dll` ~1.9 MB + `SDL3.lib` ~250 KB ≈ **2.1 MB (+62%)** |
| 下载地址 | https://www.libsdl.org/release/SDL3-devel-3.4.12-VC.zip |
| GitHub | https://github.com/libsdl-org/SDL/releases |
| 迁移指南 | https://github.com/libsdl-org/SDL/blob/main/docs/README-migration.md |

#### SDL3 相比 SDL2 的核心变化

| 特性 | SDL2 | SDL3 |
|---|---|---|
| GPU API | 无（需自行管理 Vulkan/D3D12） | 内置 `SDL_GPU` 跨平台 GPU 抽象（Vulkan/Metal/D3D12） |
| 窗口管理 | `SDL_CreateWindow` + 整数 ID | 统一 `SDL_Window*` 指针，支持多窗口属性查询 |
| 事件系统 | `SDL_PollEvent` + union | 重构的事件类型，更类型安全 |
| 渲染器 | SDL_Renderer (2D) | 增强的 SDL_Renderer + 全新 SDL_GPU (3D) |
| 命名规范 | 混合风格 (SDL_TRUE/SDL_FALSE) | 统一使用 `bool`，函数命名更一致 |
| 属性系统 | 无 | `SDL_GetProperty` / `SDL_SetProperty` 通用属性系统 |
| 初始化 | `SDL_Init(SDL_INIT_VIDEO)` | 类似，但子系统标志重命名 |
| Main 入口 | `SDL2main.lib` 链接 | 不再需要 SDLmain，通过回调 `SDL_AppInit/SDL_AppIterate/SDL_AppQuit` 或传统 main |
| 平台支持 | Windows/Mac/Linux/Mobile | 同上 + 改进的 Wayland、HDR 显示、Steam Deck 集成 |
| ABI 兼容 | SDL2 2.x 系列内兼容 | SDL3 3.x 系列内 ABI 稳定 |

#### 升级收益

1. **SDL_GPU API** — 统一的跨平台 GPU 抽象层（Vulkan/Metal/D3D12），可简化 Cocos 渲染后端窗口/交换链管理
2. **更好的 Vulkan 集成** — `SDL_Vulkan_GetInstanceExtensions` 等 API 改进，Vulkan surface 创建更简洁
3. **HDR 显示支持** — 原生 HDR 输出、色彩空间管理
4. **改进的窗口管理** — 更好的多显示器/多窗口支持，窗口属性系统
5. **无需 SDLmain** — 移除 `SDL2main.lib` 依赖，启动流程更清晰
6. **Steam Deck / 手柄增强** — 内置 Game Controller 传感器支持、触觉反馈
7. **性能优化** — 事件处理、输入延迟降低

#### 升级风险与工作量

| 风险点 | 说明 |
|---|---|
| **API 不兼容** | SDL3 是全新主版本，大量函数重命名/签名变更 |
| **Cocos 影响范围** | 涉及 `SDLHelper.h/cpp`、`SystemWindow`、`SystemWindowManager` 等平台抽象层（Win32/Linux/Mac/QNX 共 ~10 个文件） |
| **SDL_syswm.h 移除** | SDL3 移除了 `SDL_syswm.h`，改用属性系统获取原生窗口句柄：`SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL)` |
| **事件处理变更** | `SDL_WindowEvent` 结构变化，事件类型枚举重命名 |
| **跨平台同步** | 需同时升级 Linux/Mac/QNX 平台的 SDL 依赖 |
| **SDL2 仍维护** | SDL2 目前仍在维护（2.30.x），不升级 SDL3 不会有安全风险 |

#### Cocos 引擎中 SDL 使用分析

引擎当前通过 `SDLHelper` 封装层使用 SDL，涉及：
- **窗口创建与管理** — `SDL_CreateWindow`、窗口位置/最小化查询
- **原生句柄获取** — `SDL_GetWindowWMInfo` (SDL3 中已移除，需改用属性系统)
- **事件分发** — `SDL_PollEvent`、`SDL_WindowEvent` 处理
- **剪贴板** — `SDL_GetClipboardText` / `SDL_SetClipboardText`
- **光标控制** — `SDL_ShowCursor` / `SDL_SetRelativeMouseMode`
- **文本输入** — `SDL_StopTextInput`

使用范围相对集中（主要在 `platform/` 目录），封装良好，迁移可控。

#### 推荐策略

| 方案 | 建议 |
|---|---|
| **短期 (Phase 3)** | 先升级到 SDL2 2.30.11，零风险获得 bug 修复 |
| **中期 (Phase 4+)** | 评估 SDL3 迁移，重点验证 SDL_GPU 是否能简化 Vulkan 交换链管理；创建 `SDLHelper` v3 适配层 |
| **迁移路径** | 利用官方 [sdl2-compat](https://github.com/libsdl-org/sdl2-compat) 兼容层做渐进迁移，或直接重写 `SDLHelper`（代码量 ~500 行，可控） |

---

### 11. mpg123 — ~1.25.x → 1.33.6

| 项目 | 详情 |
|---|---|
| 当前版本 | ~1.25.x (API version 46) |
| 目标版本 | 1.33.6 (2026) |
| 当前文件大小 | `libmpg123-0.dll` 417.4 KB + `libmpg123-0.lib` 34.5 KB ≈ **452 KB** |
| 预估新版大小 | `libmpg123-0.dll` ~500 KB + `.lib` ~40 KB ≈ **540 KB (+19%)** |
| 升级收益 | SIMD 优化，解码精度改进，性能提升 |
| 升级风险 | 低，API 二进制兼容 |
| 下载地址 | https://www.mpg123.de/download.shtml |
| 源码 | https://www.mpg123.de/download/mpg123-1.33.6.tar.bz2 |

---

### 12. OpenAL Soft — 旧版 → 1.25.2

| 项目 | 详情 |
|---|---|
| 当前版本 | 未知具体版本（估计 1.19~1.21） |
| 目标版本 | 1.25.2 (2026) |
| 当前文件大小 | `OpenAL32.dll` 872.5 KB + `OpenAL32.lib` 36.4 KB ≈ **909 KB** |
| 预估新版大小 | `OpenAL32.dll` ~1200 KB + `OpenAL32.lib` ~45 KB ≈ **1.2 MB (+37%)** |
| 升级收益 | C++17 重写，ALC_SOFT_system_events、AL_EXT_debug 扩展，HRTF 改进 |
| 升级风险 | 低，OpenAL API 标准化 |
| 下载地址 | https://openal-soft.org/openal-releases/openal-soft-1.25.2-bin.zip |
| GitHub | https://github.com/kcat/openal-soft/releases/tag/1.25.2 |

---

## 三、低优先级升级（收益有限或风险较高）

### 13. V8 — 11.6.189.22 → 最新版

| 项目 | 详情 |
|---|---|
| 当前版本 | 11.6.189.22 (Chrome 116 时代) |
| 目标版本 | ~13.x+ (Chrome 最新) |
| 当前文件大小 | Release: `v8.dll` 28064.5 KB + 其他 ~1.9 MB ≈ **29.5 MB**；Debug: `v8.dll` 57.6 MB |
| 预估新版大小 | Release: ~32 MB；Debug: ~65 MB **(+10~15%)** |
| 升级收益 | 新 ES 特性、WASM 改进、JIT 性能优化、Maglev/Turboshaft 编译器 |
| 升级风险 | **高** — API 频繁变动，需要适配 ScriptEngine 绑定层 |
| 下载地址 | 需自行编译：https://v8.dev/docs/source-code |
| 构建指南 | https://v8.dev/docs/build |

---

### 14. PhysX — 4.1.2 → 5.5.1

| 项目 | 详情 |
|---|---|
| 当前版本 | 4.1.2 |
| 目标版本 | 5.5.1 |
| 当前文件大小 | Release 合计 ~63.5 MB；Debug 合计 ~136.8 MB ≈ **200 MB** |
| 预估新版大小 | Release ~70 MB；Debug ~150 MB **(+10%)** |
| 升级收益 | Unified Solver、GPU 加速物理、Soft Body、新 Joint 类型 |
| 升级风险 | **极高** — API 不兼容，Cocos 物理绑定层需完全重写；部分场景有 ~10% 性能回退 |
| 下载地址 | https://github.com/NVIDIA-Omniverse/PhysX/releases |
| 源码 | https://github.com/NVIDIA-Omniverse/PhysX |

---

### 15. glslang + SPIRV-Tools — 旧版 → Vulkan SDK 1.4.x

| 项目 | 详情 |
|---|---|
| 当前版本 | 未知（与引擎初始版本绑定） |
| 目标版本 | Vulkan SDK 1.4.341 对应版本 |
| 当前文件大小 | Release 合计 ~34.8 MB；Debug 合计 ~169.9 MB |
| 预估新版大小 | Release ~38 MB；Debug ~180 MB **(+8%)** |
| 升级收益 | 新 GLSL/HLSL 特性，shader 编译优化，更好的 Vulkan 1.3/1.4 支持 |
| 升级风险 | 中等，建议跟随 Vulkan SDK 版本统一升级 |
| 下载地址 | https://github.com/KhronosGroup/glslang/releases |
| Vulkan SDK | https://vulkan.lunarg.com/sdk/home |

---

### 16. TBB — 旧版 → oneTBB 2022.0+

| 项目 | 详情 |
|---|---|
| 当前版本 | 未知（Legacy TBB） |
| 目标版本 | oneTBB 2022.0+ |
| 当前文件大小 | `tbb.dll` 162 KB + `tbb.lib` 98.9 KB + malloc 相关 ~94 KB ≈ **355 KB** |
| 预估新版大小 | `tbb12.dll` ~250 KB + `.lib` ~120 KB ≈ **450 KB (+27%)** |
| 升级收益 | 更好的调度器、task_group 改进、NUMA 感知 |
| 升级风险 | 中等 — 部分废弃 API（task_scheduler_init）需替换；Cocos 使用范围有限（flow_graph + concurrent_unordered_map） |
| 下载地址 | https://github.com/uxlfoundation/oneTBB/releases |

---

### 17. tcmalloc (gperftools) — 旧版 → 2.17.2

| 项目 | 详情 |
|---|---|
| 当前版本 | 未知 |
| 目标版本 | 2.17.2 |
| 当前文件大小 | `libtcmalloc_minimal.dll` 300.5 KB + `.lib` 139.1 KB ≈ **440 KB** |
| 预估新版大小 | ~350 KB + ~150 KB ≈ **500 KB (+14%)** |
| 升级收益 | 内存碎片优化、更好的多线程性能、64-bit 改进 |
| 升级风险 | 低 |
| 下载地址 | https://github.com/gperftools/gperftools/releases/tag/gperftools-2.17.2 |

---

## 四、无需升级

| 库 | 当前文件大小 | 说明 |
|---|---|---|
| **libjpeg** 9d | `jpeg.lib` 1797.9 KB | IJG 更新极少，无安全问题；如需性能可考虑替换为 libjpeg-turbo |
| **libogg** | `ogg.dll` 29 KB + `.lib` 14.4 KB | 多年未更新 (1.3.5)，无升级必要 |
| **libvorbis** | `vorbis.dll` 712 KB + `vorbisfile.dll` 36 KB | 多年未更新 (1.3.7)，无升级必要 |
| **GLEW** | `glew32.dll` 413 KB + `.lib` 684.9 KB | 项目接近停滞 (2.3.1)，Vulkan 为主后端后重要性下降 |
| **dbgcore/dbghelp/dbgeng** | 系统调试库 ~8.4 MB | 随 Windows SDK 版本走 |

---

## 五、汇总：文件大小变化估算

| 类别 | 当前合计 | 升级后估计 | 变化 |
|---|---|---|---|
| 高优先级 4 项 | ~6.8 MB | ~10.2 MB | **+3.4 MB (+50%)** |
| 中优先级 8 项 | ~9.2 MB | ~11.5 MB | **+2.3 MB (+25%)** |
| 低优先级 5 项 (含 V8/PhysX) | ~230 MB | ~253 MB | **+23 MB (+10%)** |
| **总计（全部升级）** | **~246 MB** | **~275 MB** | **+29 MB (+12%)** |

> 注：文件大小为估算值，实际大小取决于编译选项（静态/动态、Debug/Release、优化级别）。

---

## 六、推荐升级顺序

```
Phase 1 (安全紧急):
  ├── OpenSSL 1.1.1k → 3.5.7 LTS
  ├── libwebp → 1.6.0 (CVE-2023-4863)
  └── libcurl → 8.21.0 (依赖 OpenSSL 升级)

Phase 2 (基础设施):
  ├── zlib → 1.3.2
  ├── libpng → 1.6.58 (依赖 zlib)
  ├── SQLite → 3.53.4
  └── libuv → 1.52.1

Phase 3 (功能增强):
  ├── SDL2 → 2.30.11
  ├── libwebsockets → 4.5.8 (依赖 OpenSSL)
  ├── FreeType → 2.14.3
  ├── mpg123 → 1.33.6
  └── OpenAL Soft → 1.25.2

Phase 4 (长期规划):
  ├── glslang/SPIRV-Tools (跟随 Vulkan SDK)
  ├── TBB → oneTBB
  ├── tcmalloc → 2.17.2
  ├── V8 引擎升级
  └── PhysX 5 迁移
```

---

## 七、注意事项

1. **依赖链**：OpenSSL → libcurl / libwebsockets 必须同步升级
2. **zlib → libpng**：libpng 依赖 zlib，先升 zlib
3. **跨平台一致性**：win64 升级后需同步验证 mac / openharmony / android 平台的外部库
4. **编译选项**：建议统一使用 MSVC 2022 (v143)，Release 使用 /O2 + /GL，与引擎编译保持一致
5. **测试覆盖**：每批升级后运行完整引擎测试套件，特别关注网络、音频、图片解码、物理模块
