# Windows x64 三方库升级方案（实测版）

> 评估时间：2026-07-25　实测验证：2026-09-01
> 路径：`native/external/win64/CMakeLists.txt`　外部仓库：`cocos/cocos-engine-external` @ `v4.0.0-4`（commit `c454ca89`）
>
> **本文档是本轮工作的唯一交付物。** 此前的所有引擎源码改动、构建脚本、vcpkg 产物均已回退，仓库处于可编译基线状态。
> 与初版的区别：本版所有版本号、体积、依赖关系、阻塞项都来自**本机实测**，不再使用估算值；实测推翻的旧结论集中在第十节。

---

## 一、结论速览

| 判定 | 内容 |
|---|---|
| **可以安全升级、不改引擎源码** | zlib、libuv、libpng、freetype、sqlite3、SDL2、libwebp（共 7 个，仅需替换二进制 + 保持 CMake 形态）。其中 libwebp 是 ABI 兼容升级，但要付 +342% 静态库体积，且它的 CMake 目标本身有缺陷（第六节） |
| **可升级，需改引擎代码（已验证）** | libcurl —— 3 处，已在本机验证编译+链接全绿，且**与二进制升级无关**（新 API 在现存 7.74 头文件里就有） |
| **可升级，需改引擎代码（未验证）** | libwebsockets 2.4.2 → 4.5.8 —— 4 处断点、集中在 1 个文件；改造语义尚未编译验证 |
| **可升级，但需先决策** | OpenSSL —— vcpkg 基线只能给 3.6.2（短支持线，EOL 2026-11），不是原计划写的 3.5.7 LTS |
| **建议暂缓** | mpg123 —— 唯一无安全驱动的库，其 vcpkg 端口依赖 yasm，而 yasm 1.3.0 在现代 MSVC 下构建失败 |
| **体积涨幅远超预期（静态库）** | `.lib` 实测涨幅显著高于 DLL：libwebp +342%、freetype +76%。成因见第三节（不影响最终产物体积）。**例外：若关闭 `USE_DEBUG_RENDERER`，freetype 是纯体积成本、零收益**——它的 imported target 只在该开关下创建（`win64/CMakeLists.txt:19-26`，源码侧 `native/CMakeLists.txt:2229-2233`），而该开关默认 ON（`:50`） |

一句话：**高/中优先级 11 项里，7 项不动引擎源码，1 项（curl）的代码改动已验证且可独立落地，1 项（lws）要先做一次未验证的语义改写，1 项（OpenSSL）卡在版本线决策，1 项（mpg123）建议放弃。**

但"不改源码"不等于"直接可用"：上面 7 项里的 **zlib 与 libuv 还要先过第五节的两个构建侧阻塞**（vcpkg 的 `zconf.h` 把条件编译改成恒真 → 引擎编译中断；vcpkg 产物名 `z.dll`/`uv.dll` 与导入表不一致 → 静默改名会运行时崩）。这两个是跨库的工具链问题，不是某两个库各自的麻烦，解决一次即对 curl / lws 同时生效。

---

## 二、逐库升级清单（实测版本与体积）

体积单位 KB，`当前 → 升级后`。当前值取 `native/external/win64/libs/` 实文件，升级后值取本机 vcpkg 构建产物实文件。

### 高优先级（安全驱动）

| 库 | 当前 → 目标 | 形态（必须保持） | 体积 | 驱动 |
|---|---|---|---|---|
| **OpenSSL** | 1.1.1k → **3.6.2**（或 3.5.7 LTS，需 overlay） | DLL：`libcrypto`+`libssl` | 5144.4 → 7553.2 (+46.8%) | **1.1.1 已 EOL 2023-09**，本轮最强理由 |
| **libcurl** | 7.74.0-DEV → **8.20.0** | DLL | 495.6 → 696.3 (+40.5%) | 100+ CVE；HTTP/2、连接池、TLS 后端现代化 |
| **libwebsockets** | 2.4.2 → **4.5.8** | DLL | 260.5 → 635.5 (+144.0%) | 2017 年至今 2 个大版本，含安全修复 |
| **libwebp** | **1.2.0** → 1.6.0 | 静态 `webp.lib` | 938.7 → 4153.5 (+342.5%) | CVE-2023-4863 系（1.2.0 在受影响区间内） |

### 中优先级（性能/维护驱动）

| 库 | 当前 → 目标 | 形态（必须保持） | 体积 | 驱动 |
|---|---|---|---|---|
| **zlib** | 1.2.12 → **1.3.2** | DLL `zlib1.dll` | 98.4 → 106.8 (+8.5%) | CVE-2022-37434；是 curl/lws/png 的共同前置 |
| **libuv** | 1.41.0 → **1.52.1** | DLL | 254.6 → 269.4 (+5.8%) | Windows IOCP 改进；lws 4 的硬依赖 |
| **SQLite** | 3.35.4 → **3.53.0** | DLL | 941.6 → 1135.4 (+20.6%) | 18 个主版本；向后兼容极好 |
| **SDL2** | 2.28.1 → **2.32.10** | DLL | 1319.3 → 1837.5 (+39.3%) | 2.32 系列，比原计划的 2.30.11 更新 |
| **libpng** | 1.6.37 → **1.6.58** | 静态 `libpng16.lib` | 1892.6 → 2321.0 (+22.6%) | 安全修复 + SIMD |
| **FreeType** | 2.10.0 → **2.14.3** | 静态 `freetype.lib` | 3863.7 → 6786.9 (+75.7%) | 多个安全漏洞；COLRv1、CFF2 |

### 不在本轮范围

- **mpg123** 1.25.x → 1.33.x：见第五节阻塞项 4。
- **V8 / PhysX / glslang / TBB / tcmalloc**：低优先级结论不变（API 破坏面大、收益不足以抵偿工作量），本节不重复。
- **libjpeg 9d / libogg / libvorbis / GLEW / OpenAL Soft**：无升级必要；若要音频/图片收益，正确动作是**换库**（libjpeg-turbo）而不是升级 libjpeg。

---

## 三、体积：实测打脸估算

初版报告的"+50% / +18% / +17%"是外推的，实测结果是**静态库普遍涨得比 DLL 凶**：

| 库 | 初版估算 | 实测 | 偏差 |
|---|---|---|---|
| libwebp | +18% | **+342%** | 严重低估 |
| freetype | +17% | **+76%** | 低估 |
| libpng | +6% | **+23%** | 低估 |
| SDL2 | +15% | +39% | 低估 |
| OpenSSL | +50% | +47% | 基本准 |
| libcurl | +57% | +41% | 高估 |
| zlib | +10% | +9% | 准 |

**原因**：`.lib` 是全部目标文件的归档，vcpkg 默认开全特性（freetype 内建 brotli/bzip2/png/harfbuzz 支持、libwebp 附带 demux/mux/sharpyuv 全部对象），而上游 Cocos 手工构建时裁剪过。**链接器只会抽取被引用的目标文件**，所以最终 EXE 增量远小于这里的 `.lib` 增量——但这个数字不能用来吓自己，也不能用来论证"零成本"。

**要控制体积**：给 vcpkg 加 `vcpkg-configuration.json` + overlay port，把 freetype/libwebp 的 `FEATURES` 裁到引擎实际用到的项（引擎只用 `FT_New_Memory_Face`/`FT_Load_Glyph` 等核心 API，以及 `WebPDecode*`）。

---

## 四、依赖关系：PE 导入表实测

不是 changelog 转述，是解析每个 DLL 的导入目录得到的。

```
zlib 1.3.2（Cocos 名 zlib1.dll / vcpkg 名 z.dll，见第五节 2）
   ├── libcurl.dll      ← 还导入 secur32/bcrypt/crypt32/advapi32/iphlpapi：Schannel 后端，不依赖 OpenSSL
   └── websockets.dll   ← 还导入 libcrypto-3-x64.dll, libssl-3-x64.dll, uv.dll
                              OpenSSL + libuv + lws 必须同批构建、头文件配套

libuv 1.52.1（Cocos 名 libuv.dll / vcpkg 名 uv.dll）  ← 独立（导入表新增 dbghelp.dll / ole32.dll，系统库，无需随包分发）
sqlite3.dll            ← 独立
SDL2.dll               ← 独立
freetype.lib (静态)    ← 依赖 zlib / libpng / bzip2 / brotli（都静态并入）
libpng16.lib (静态)    ← 依赖 zlib
webp.lib (静态)        ← 1.6.0 起拆出 libsharpyuv（新目标，现状见第六节 `:105-109`）
```

**两个可以直接改变升级计划的实测结论：**

1. **curl 和 OpenSSL 完全解耦。** 基线 `libcurl.dll` 和新版 `libcurl.dll` 的导入表里都**没有 libcrypto/libssl**。curl 走 Windows Schannel。所以"必须先升 OpenSSL 才能升 curl"是错的——curl 真正的前置只有 zlib。curl + zlib 可以独立成为第一批。
2. **OpenSSL 升级的唯一硬约束来自 lws**，因为 `websockets.dll` 确实导入 `libcrypto`/`libssl`。也就是说：想升 lws 就必须同批升 OpenSSL + libuv；不升 lws 的话，OpenSSL 可以单独处理。

---

## 五、落地前必须解决的 4 个阻塞项

按发现顺序，每条都有可复现证据。

### 1. vcpkg 的 zlib 头文件会直接打断引擎构建（已定位根因）

**现象**：替换 zlib 1.3.2 头文件后，`native/external/sources/unzip/unzip.cpp` 报 **59 个 `error C2491`**（`definition of dllimport function not allowed`），`cocos_engine` 目标编译中断。

**根因（实测比对两份 `zconf.h`）**：vcpkg 的 zlib 端口把上游的条件编译改成了恒真。

```c
/* 上游 / Cocos 现用 zlib 1.2.12：zconf.h:337 */
#  ifdef ZLIB_DLL          /* 引擎不定义 ZLIB_DLL → ZEXTERN 就是普通 extern，unzip.cpp 能编译 */

/* vcpkg zlib 1.3.2：zconf.h:348 */
#  if  1                   /* 无条件 dllimport → unzip.cpp 里这些函数的定义全部 C2491 */
```

引擎自身不带 `ZLIB_DLL`，且当前基线正是"普通 `extern` 声明 + 链接 `zlib.lib` 导入库"这一组合在工作，所以这个 patch 对 Cocos 是纯粹的破坏。

**解法（推荐，成本最低）**：**只取 vcpkg 的二进制，不取它的 zlib 头文件**。头文件用 zlib 上游 1.3.2 发布包里的（与基线同源，仅版本号不同）。备选：harvest 后对 `zconf.h` 做一次定向补丁，把 `#  if 1` 还原成 `#  ifdef ZLIB_DLL`。

### 2. DLL 文件名不匹配 —— 会导致运行时"找不到模块"

**现象（实测导入表）**：vcpkg 产出的 zlib/libuv DLL 名字和 Cocos 布局不同，且**导入方按 vcpkg 的名字写死了**：

| 组件 | Cocos 布局 | vcpkg 实际 | 谁在按这个名字找 |
|---|---|---|---|
| zlib | `zlib1.dll` | `z.dll` | `libcurl.dll`、`websockets.dll` |
| libuv | `libuv.dll` | `uv.dll` | `websockets.dll` |

直接把 `z.dll` 改名成 `zlib1.dll` 放进包里，链接能过、**运行时会崩**（loader 按导入表里的字符串找 `z.dll`）。

**解法（三选一，按侵入性排序）**：
- **A. 让 vcpkg 产出正确名字**：overlay port 里设 `OUTPUT_NAME zlib1 / libuv`（改 CMake target property，不动导入表）。这是唯一"名字自洽"的做法。
- **B. 按 vcpkg 名字发布**：包内放 `z.dll`/`uv.dll`，同时改 `win64/CMakeLists.txt` 的 `IMPORTED_LOCATION` 和删除旧名文件。缺点是与 mac/linux/openharmony 的命名习惯不一致。
- **C. harvest 后 patch 导入表字符串**：可行但属于二进制手术，不建议。

> 我上一轮用的就是被否掉的第 4 种做法（静默改名），这也是为什么"编译通过"并不等于"能跑"——这一条当时没被任何编译期检查抓到。

### 3. libwebsockets v2.4.2 → v4.5.8：4 处断点（集中在 1 个文件）

审计方法：取基线头文件 `native/external/win64/include/websockets/`（实测 `lws_config.h:29` = 2.4.2）与目标 `v4.5.8` tag 头文件做机器比对；从引擎三个网络文件抽取全部 123 个去重 `lws_*`/`LWS_*` 符号 + 引擎实际赋值的结构体字段。

**结构体字段全部存活，无需改动：**

| 结构体 | 引擎赋值字段数 | v4.5.8 缺失 |
|---|---|---|
| `lws_context_creation_info` | 11 | 无 |
| `lws_client_connect_info` | 12 | 无 |
| `lws_protocols` | 4 | 无 |

曾被怀疑、实测确认**不要动**：`info.uid`/`info.gid`、`info.iface`、`info.ssl_ca_filepath`、`protocol.id`、`connectInfo.ssl_connection` —— v4.5.8 全部保留。80+ 个 `LWS_CALLBACK_*` 只有 1 个失效。

**真实断点：4 个符号**

| # | 符号 | 位置 | v4 状态 | 处理 |
|---|---|---|---|---|
| 1 | `LWS_CALLBACK_OPENSSL_CONTEXT_REQUIRES_PRIVATE_KEY` | `WebSocketServer.cpp:690` | 已移除 | 删 2 行（该 case 体是空 `break;`，对 v2 也安全） |
| 2 | `lws_libuv_stop(ctx)` | `WebSocketServer.cpp:174` | 已移除 | 语义改写 |
| 3 | `lws_uv_initloop(ctx, loop, us)` | `WebSocketServer.cpp:237` | 已移除 | 语义改写 |
| 4 | `lws_libuv_run(ctx, flag)` | `WebSocketServer.cpp:254` | 已移除 | 语义改写 |

`LWS_SERVER_OPTION_LIBUV`（`lws-context-vhost.h:75`）与 `lws_uv_getloop()`（`lws-service.h:187`）在 v4 **仍存活** —— 即 v4 部分保留 libuv 集成，只删了"建 loop / 跑 loop / 停 loop"三件套。#2/#3/#4 需要把服务端事件循环改为 `lws_service()` 轮询驱动，属于语义改写，不是机械改名。

**关键约束**：`WebSocketServer.cpp` 全文 770 行**没有任何 `#if`**（只有 `#include`/`#define`，见 `:25-36, 96, 105, 113, 121`），因此这 4 处在 `USE_WEBSOCKET_SERVER=ON` 时无条件参与编译，**无法靠条件编译绕过**。该 target 由 `native/CMakeLists.txt:1089-1093` 编译，CI 的 win64 job 显式开启该开关（`.github/workflows/native-compile-platforms.yml:56`），不是可忽略的边缘配置。

**好消息**：客户端 `WebSocket-libwebsockets.cpp` 的 libuv 调用受 `WS_ENABLE_LIBUV` 保护（`:73-77`，仅 `CC_PLATFORM_ANDROID` 为 1），**win64 客户端路径零改动**。（`:1311` 的 `"lws_callback_wsi_destroy"` 是字符串字面量，不是调用。）

**执行纪律**：**不要在拿到 v4.5.8 头文件+导入库之前改这 4 处**，否则仓库直接编译不过——v2.4.2 头文件里没有 v4 的替代 API。改造范围因此天然被切成"二进制先行、代码跟上"两步，中间存在一个不可编译的窗口，需在同一提交内闭合。

### 4. mpg123：vcpkg 端口链在 yasm 上

`mpg123:x64-windows` 依赖 host 工具 `yasm 1.3.0`（端口依赖标记 `platform='windows & !mingw'`），yasm 已停止维护、在现代 MSVC 下 `BUILD_FAILED`。它是本轮唯一没有安全驱动的库（只是 MP3 解码性能），**建议直接放弃**。若确实需要：overlay 一个安装官方 `yasm-1.3.0-win64.exe` 的 yasm 端口，或从别处取 mpg123 二进制。

---

## 六、`native/external/win64/CMakeLists.txt` 自身的缺陷

这些与升级无关，但会在升级时暴露出来，应一并修正：

| 位置 | 问题 | 后果 | 修正 |
|---|---|---|---|
| `:35-39` `crypto`、`:77-81` `ssl`、`:83-88` `uv` | 声明为 `STATIC IMPORTED` + 只给 `IMPORTED_LOCATION` | 实测这三个 `.lib` 是 **DLL 导入库**（内含 DLL 名 + `__imp_` 符号 8794/990/580 个）。运行时 DLL 目前**仅靠** `native/CMakeLists.txt` 里 `file(GLOB win64/libs/*.dll)` 被顺带拷走才侥幸可用 | 改 `SHARED IMPORTED` + `IMPORTED_IMPLIB` + `IMPORTED_LOCATION` |
| `:105-109` `webp` | 只声明 `webp.lib` | 磁盘上有 `libwebpmux.dll/.lib` 却**没有任何 target**；1.6.0 起还拆出 `libsharpyuv.lib`、`libwebpdemux.lib` | 按需补 target，或明确 mux 是否真的被用 |
| `native/CMakeLists.txt` 的 `file(GLOB .../*.dll)` | 缺 `CONFIGURE_DEPENDS` | **改了 DLL 文件名后，不重新 configure 就不会进包**。这是升级时最容易被漏掉的一步 | 加 `CONFIGURE_DEPENDS`，或改用显式列表 |
| 允许式同步 | — | 只增不删会让**旧 OpenSSL 1.1 的两个 DLL 与 3.x 同时进包**，等于没消除 EOL 风险 | 升级必须显式删除 `libcrypto-1_1-x64.dll` / `libssl-1_1-x64.dll` |

---

## 七、引擎侧代码改动（唯一需要进引擎仓库的部分）

### 已验证：`native/cocos/network/Downloader-curl.cpp` 3 处

在**未替换任何二进制**的基线上验证通过（增量编译 + 链接 0 error / 0 warning，`CocosGame.exe` 成功重链）。用到的新 API 在现存 7.74 头文件里就已存在，因此**与二进制升级完全解耦，可以单独合入**：

1. 删除死代码宏 `CC_CURL_POLL_TIMEOUT_MS`（原 `:47-49`）。
2. `CURLINFO_CONTENT_LENGTH_DOWNLOAD` → `CURLINFO_CONTENT_LENGTH_DOWNLOAD_T`（`:381` 附近，配 `curl_off_t`）。8.x 里前者已废弃。
3. 用 `curl_multi_poll()` 取代 `curl_multi_fdset()` + `select()` 等待循环（原 `:422` 附近，含 `int rc`）。同时消除了 fdset 的 `FD_SETSIZE` 上限与"无 fd 就绪"空转。

### 待做：`WebSocketServer.cpp` 4 处

见第五节第 3 条。注意 `#2/#3/#4` 改写后需与 libuv 1.41→1.52 升级**同批做运行时验证**（该文件直接使用 `uv_loop_t`/`uv_close`/`uv_async_t`，见 `:236,255,376`）。

---

## 八、复现配方

### 落地方式的硬约束

`native/external/` **不是**引擎仓库跟踪的内容，它是 `github.com/cocos/cocos-engine-external` 的独立 clone（由 `native/external-config.json` 钉版本），且在 `.gitignore:24` 里。两个后果：

1. 升级后的二进制**无法提交进引擎仓库**，能提交的只有"配方 + 引擎侧代码改动"。
2. **`native/utils/download-deps.js` 在 clone 前会 `emptyDir(native/external)`，所以 `gulp init` 会静默抹掉任何本地覆盖**，把仓库打回出厂的 EOL 库。任何覆盖方案都必须把这一步设计进去（钩子挂在 `gulp init` 之后，或干脆 fork 外部仓库并改 `external-config.json` 指向自己的 commit——后者才是长期正确的形态）。

### vcpkg 构建参数

- **baseline**：`builtin-baseline = cf9b6f1a47898a888f9e86e346a9adfa68357380`（registry 快照 2026-05-03）。没有 baseline，vcpkg 会直接拒绝 `version>=` 写法。
- **两个 triplet 分开装，且各自独立的 install root**：
  - `x64-windows`：zlib、curl、openssl、libuv、libwebsockets、sdl2、sqlite3
  - `x64-windows-static-md`：zlib、libpng、freetype、libwebp
  
  即"动态的保持动态、静态的保持静态"，与出厂形态一致。**注意**：`x64-windows-static`（/MT）会因 CRT 不匹配触发 `LNK2038`，不能用；`static-md`（/MD + 静态库）才是静态项的正确选择。
- **必须钉住 MSVC 工具集**：`VCPKG_VISUAL_STUDIO_PATH` → VS 2022 BuildTools（MSVC 14.44.35207，与引擎基线一致）。不钉的话 vcpkg 会静默选机器上最新的 VS 实例（本机是 MSVC 14.51 / VS 18），产出的库和引擎不同工具集。
- **curl 保持 Schannel 后端**（与出厂一致，见第四节）。
- **一个 install root 不要给两个 manifest 复用**：vcpkg manifest 模式把 `installed/vcpkg/status` 视为独占，第二次 install 会把第一棵树的包当"不再需要"剪掉。这一点我踩过，代价是约 40 分钟重编。

### 实测版本落点（vcpkg 包数据库确认）

`zlib 1.3.2` / `curl 8.20.0` / `openssl 3.6.2` / `libuv 1.52.1` / `libwebsockets 4.5.8` / `sdl2 2.32.10` / `sqlite3 3.53.0` / `libpng 1.6.58` / `freetype 2.14.3` / `libwebp 1.6.0`；传递依赖 `brotli 1.2.0`、`bzip2 1.0.8`、`pthreads 3.0.0`。

**OpenSSL 的版本决策**：3.6.2 是短支持线（EOL 2026-11），3.5.7 LTS（EOL 2030-04）在该 registry baseline 里不存在。两者都消除了 1.1.1 的 EOL 暴露——这是真正的驱动。若要走 LTS，需要 overlay port 或 bump baseline，只改这一行，不影响其他库。

### 构建环境踩坑记录（避免重复消耗）

- **Strawberry Perl 便携 zip 用 `cmake -E tar x` 解压会失败**（libarchive `Can't restore time`，卡在异常 mtime）。绕法：用容错解压器预先解到 `downloads/tools/perl/5.42.2.1/`，vcpkg 的 `paths_to_search` 命中后就不再自行解压。
- 便携版 Perl 没有 `Text::Template`，但**不是阻塞项**——OpenSSL 在 `external/perl/` 下自带副本。
- winget 报告"已安装"不代表真装上（本机 NASM 显示找到但未落地，`perl --version` 退出码 20）。装完必须实测可执行。
- 不要用 `-vcpkg-root`/`--manifest-path`/`--no-print-humans` 这类猜的参数；`--manifest-path` 无效，用 cwd 自动发现或 `VCPKG_MANIFEST_DIR`。

---

## 九、建议的执行顺序（每步都能独立回退）

```
第 0 步（先做，与升级无关）
  └── 合入 Downloader-curl.cpp 的 3 处改动 —— 已在基线验证，零风险，先把确定性收益拿走

第 1 步  zlib 1.3.2（二进制 + 上游头文件）
  └── 关卡：unzip.cpp 无 C2491；HTTP 下载、本地资源解压正常
  └── 注意：必须同时解决第五节 1（头文件）与 2（z.dll 命名）

第 2 步  curl 8.20.0                （依赖 zlib，独立于 OpenSSL）
         sqlite3 3.53.0             （无依赖）
         SDL2 2.32.10               （无依赖）—— 验证 Vulkan surface 创建与窗口事件

第 3 步  libpng 1.6.58 + freetype 2.14.3   （静态项，依赖第 1 步的 zlib）

第 4 步  OpenSSL 3.6.2 + libuv 1.52.1      （确定版本线后再做）

第 5 步  libwebsockets 4.5.8 + WebSocketServer.cpp 4 处改写   （依赖第 4 步，同批）
  └── 关卡：USE_WEBSOCKET_SERVER=ON 下 CI win64 job 必须绿；服务端连/断/关停路径手工验证

第 6 步  libwebp 1.6.0 + 补齐 webpmux/sharpyuv 的 target 判定

并行推进  fork cocos-engine-external 放产物 + 改 external-config.json
          （这是唯一能扛过 `gulp init` 的形态，别把它留到最后）
```

每步之后要做的验证：网络（HTTPS 下载、WebSocket 服务端/客户端）、图片（png/jpg/webp）、字体、音频、SQLite 存档。注意 **CMake 需重新 configure**（第六节 `file(GLOB)` 那条）。

---

## 十、本文档纠正的旧结论

| 旧结论 | 实测 |
|---|---|
| "curl 必须先升 OpenSSL，两者同批" | **错**。基线和新版 `libcurl.dll` 都不导入 OpenSSL，走 Schannel；curl 的前置是 zlib |
| SDL2 目标版本 2.30.11 | **2.32.10**（2.32 系列已发布，旧计划成文于其之前） |
| libwebp 当前 "~1.2.x (ABI 0x0209)" | 二进制内对象路径串实测为 **`libwebp-1.2.0`**；而随包**头文件**声明 decoder 0x0209 / encoder 0x020f / mux 0x0108，属于 1.3.x 级别 —— 上游外部仓库里**头文件与静态库不同版本**，这本身是个待上报的问题 |
| 各库体积涨幅 +6%~+57% | 见第三节，静态库实际 +23%~+342% |
| 只有 crypto/ssl/uv 需要 STATIC→SHARED 修正 | 属实（已用导入库特征验证），但**同一个文件里 webp target 不完整、GLOB 缺 `CONFIGURE_DEPENDS`、旧 DLL 不会被删**这三条同样致命 |
| `native/external/versions.txt` 可作版本依据 | **不可信**：它声明 linux websockets 为 4.3.0，而 `native/external/linux/include/libwebsockets/lws_config.h:29` 实测 2.4.2。版本一律从头文件/二进制实测 |
| lws v2→v4 "需重写约 185 行连接/回调代码" | **缩小为 1 个文件 4 处**，且客户端路径在 win64 零改动 |

---

## 十一、尚未验证 / 需要下一轮接手时注意

诚实标注边界，避免下一轮把这些当已证事实：

1. **lws v4 的 4 处改写从未真正编译过。** 上一轮构建在 `unzip.cpp` 就中断了，`cocos_engine` 根本没走到 `WebSocketServer.cpp`。所以"0 个 lws 错误"**不是好消息**，只是没测到。第五节 3 的 delta 是**头文件层面**的结论（可信），**运行时语义**（改成 `lws_service` 后服务端连/断/关停是否正确）完全未验证。
2. **第 1、2 两个阻塞项修好后是否还有下一个，未知。** 上一轮从未跑通"全部 11 个库同时替换"的完整构建。分批推进（第九节）正是为了把未知风险的暴露面缩小。
3. **静态/动态混用的运行时一致性**未压测：`static-md` 的 libpng/freetype 内嵌各自的私有 zlib 对象，与共享的 `zlib1.dll` 同进程共存，只要两侧 ABI 结构体不跨界传递就安全——引擎实际是各用各的，但需要跑一遍确认。
4. **OpenSSL 3.x 的 `libcrypto-3-x64.dll` 命名**与 Cocos 的 DLL 拷贝 glob 兼容，但任何硬编码了 `libcrypto-1_1` 字样的脚本/安装包规则（含编辑器侧、打包模板）需要一并搜杀。
5. **跨平台一致性**未处理：本轮只覆盖 win64。mac/linux/openharmony 的外部库仍是旧版；同一份 CVE 在其他平台依旧存在，win64 单平台升级不等于产品安全。
6. 上游 `cocos-engine-external` 未被改动/未提 PR。真正"正向收益"要落到用户手上，只有 fork 或被上游接受这一条路。
