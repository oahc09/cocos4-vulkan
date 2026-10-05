# 真实 Windows 测试工程验证（2026-10-04）

结论（23:17补证）：**原 Debug 输出目录的 CRT 初始化崩溃已修复；完整 Vulkan 运行验收仍失败**。永久修复 CMake 复制规则后，原路径程序在 Windows 调试模式与普通启动均显示真实画面并正常退出；同步与深度/模板布局错误仍存在。下文隔离与崩溃记录保留为修复前历史，M0~M7 不因此验收。

## CRT 永久修复与原目录回归

- **Orca Worker 实施/构建**：`native/CMakeLists.txt` 删除 external `msvcr/*.dll` glob，新增运行库模块（当前位置 `native/external/win64/windows-runtime-libs.cmake`，2026-10-05 按用户要求从 `native/cmake/` 迁移），用官方 `InstallRequiredSystemLibraries` 自动发现已安装 VC/SDK 的 release/debug 运行库；局部设置 runtime 收集选项，不新增 install 规则。非 CRT/V8 复制逻辑不变。真实工程同源同 cache configure/build1/build2 均 exit0，生成项目不再包含旧 external CRT copy 来源。
- **Lead 复核补充**：明确关闭 DEBUG_LIBRARIES_ONLY，强制同时收集两种 CRT；检查必需 VC/UCRT DLL，阻止官方模块仅警告并跳过缺失库后留下陈旧 app-local 文件。当前原 Debug 目标再构建 exit0。未修改 glslang 初始化、Vulkan renderer、external 二进制或测试工程源码；没有通过关闭 Validation 绕过崩溃。
- 原目录 VC DLL 14.29.30038→**14.44.35211**（本机 VS2022 Redist 自动发现，与编译器14.42兼容）；Debug UCRT 10.0.17763→**10.0.26100.8249**（SDK）。不是硬编码本机路径/版本或只手工替换产物。增量构建不会再复制14.29。
- **Lead Windows debug-event 实际运行**：直接启动原 `build/windows/proj/Debug/test-cases.exe`，约41.28秒，菜单截图，WM_CLOSE正常exit0，无访问违规。模块记录证明实际加载原Debug目录的 `msvcp140[d]`/`vcruntime140[d]` 14.44和SDK `ucrtbased`；release `ucrtbase` 实际取系统目录，不把产物中 DLL 的存在误称加载。不是在 VS IDE 中自动执行 F5。
- **Lead 普通启动**：最终构建后原路径 exe 无调试器运行约30.91秒，真实鼠标进入 `2d-rendering-in-3d` 并核对画面，WM_CLOSE正常exit0，无强制终止。exe SHA仍 `09E8E9F7980EF0260F35CCF7FFF536B09327D1790F551B7BE20885EA3521DCC5`，此次修复只影响部署的运行库；五个关键 renderer 源码 SHA未变。测试工程 tracked diff为空。
- 新证据根：`native/build/vulkan-modernization/win-crt-fix-lead/run-20261004-231415148335/`（最终构建日志/exit、debug-output、runtime-summary、native-summary、verification-summary、startup/native-menu/native-scene PNG）；Worker证据 `win-crt-fix-worker/20261004-run1/`。执行归属分别为 **Orca Worker** 实施/构建与 **Lead** 复核/运行，未暂存/提交。
- 调试运行仍真实记录 **WAW×10、09600×2**，teardown日志泄漏0。这里只解决用户报告的 mutex/CRT 启动崩溃，不将程序退出0当成 Validation clean，也不声称完整平台/性能 Gate。

## 工程与执行渠道

- 用户指定 CMake：`E:/AIProMax/github/cocos4-vulkan-test-project/native/engine/win64`；VS 工程：同仓库 `build/windows/proj`。
- `cfg.cmake` 指向本仓库 `native`，`CC_USE_VULKAN=ON`；Debug 实际编译宏 `CC_VULKAN_VALIDATION=1`，非 Release/旧 smoke 替代。
- **Orca Worker**构建 `test-cases` Debug，exit=0；`cocos_engine.lib` 本次 up-to-date，exe 真实重新链接，19:22→21:15。不能把本次写作全引擎重新编译。
- **Lead**实际启动、崩溃栈采集、运行库隔离、截图、窗口操作与证据核对；未修改生产代码、原项目输出目录、cache、资源、设置或 driver。未暂存/提交。

```powershell
& 'D:/Creator/3.8.8/resources/tools/cmake/bin/cmake.exe' --build `
  'E:/AIProMax/github/cocos4-vulkan-test-project/build/windows/proj' `
  --config Debug --target test-cases --parallel 4
```

## 原目录启动失败与运行库隔离

- 原 `Debug/test-cases.exe` 无调试器启动：exit=`3221225477`（`0xC0000005`），约1.03秒；两次 Windows debug-event 采集也取得 first/second-chance AV，约5–6秒。调试器采集时间不可作为性能结果。
- 活栈顶在原 exe 目录的 `msvcp140.dll`，中间经 SDK 的 `VkLayer_khronos_validation.dll`、loader，抵达 `VKGPUContext.cpp:327` 的 `vkCreateInstance`。实际是读 NULL。export 最近符号 `Thrd_yield+272` 不等于准确函数名，第三方 DLL 没有完整私有符号。
- 原携带 release/debug CRT 为14.29.30038；工具链为14.42。微软明确要求运行库不低于各组件使用的最新工具链，并记录旧运行库可能导致 mutex NULL dereference：[兼容性说明](https://github.com/microsoft/STL/releases/tag/vs-2022-17.10)。栈与版本支持兼容性问题；未声称已永久修复。
- ignored 副本换为本机已有 release CRT（14.51）和匹配工具链的 Debug CRT（14.42）、SDK Debug UCRT；原目录未替换。exe SHA256 始终为 `09E8E9F7980EF0260F35CCF7FFF536B09327D1790F551B7BE20885EA3521DCC5`。
- 对原/副本全部**1649个资源文件**逐文件 SHA 校验一致。隔离运行初始化崩溃未再出现；这是运行库对照与临时验证环境，不能写作原输出目录运行通过。
- 复制来源可追溯至 `native/CMakeLists.txt:3491` 的 WINDOWS_DLLS glob（含 external `msvcr/*.dll`）；实际 `test-cases.vcxproj:276/285` 明确 copy_if_different 两个14.29 MSVCP DLL到Debug。永久复制规则修复尚未实施，不修改 external 二进制。

## 真实运行与错误

同 exe/资源的隔离运行实际日志：RTX 4060 Laptop，effective API1.3，`timeline=1 sync2=0 submit2=0 dynamicRendering=0 classicOverride=0 maintenance4=0`。本轮只验证此默认路径，不冒称 Submit2/Barrier2/Hybrid 组合全过。

| Lead 运行 | 退出码 | 日志中的错误消息 | 实际证据 |
|---|---:|---|---|
| crt-run2 | 0 | WAW×10、09600×2 | 完整资源下初始化及正常关闭 |
| crt-visible2 | 0 | WAW×10、09600×2 | TestList + `2d-rendering-in-3d`真实画面，约91秒 |
| crt-lifecycle | 0 | WAW×10、09600×4 | 外窗1224×959→900×650，最小化/恢复仍显示 TestList，WM_CLOSE正常退出 |

- `SYNC-HAZARD-WRITE-AFTER-WRITE`：初始化纹理上传时，`vkCmdCopyBufferToImage` 与先前图像布局转换之间无充分依赖。下一定位点：`VKCommands.cpp:1576` 的上传前屏障、`:1692` copy、`:1865` THSVS 编码。这里只记录真实 hazard 与候选路径，尚未确认完整 producer/consumer scope 根因。
- `VUID-vkCmdDraw-None-09600`：提交时 depth/stencil aspect 期望 `DEPTH_STENCIL_ATTACHMENT_OPTIMAL`，实际 `UNDEFINED`；窗口重建后再出现。下一定位点：`VKCommands.cpp:456` RP initialLayout、`VKCommandBuffer.cpp:258` pass 输入及新建/重建 DS 图像首次使用链。尚未把报错的 descriptor 文本当成唯一根因。
- 三个有效隔离运行 teardown 日志均 `Total device memory leaked: 0 bytes.`。错误数是**日志计数**，不是读取活原子计数；重复消息受 validation duplicate limit 限制，不能认为后续完全无错。exit0/画面正常/泄漏0均不能抵消 Validation 失败。
- layer实际加载证据见 `INSTANCE_LAYERS`；SYNC-HAZARD 和 BestPractices 消息证明这两类检查正在工作。没有新增 harness triad/count footer，不能套用旧 smoke 判据。

## 证据与边界

- 构建：ignored `native/build/vulkan-modernization/win-test-project-worker/20261004-f2g-build/`。
- Lead运行根：`native/build/vulkan-modernization/win-test-project-lead/run-20261004-212151443/`。`verification-summary.json`、`native-exit.json`、`diagnostic/crash-analysis.json`、三个有效运行的 `debug-output.log` / `runtime-summary.json`、runtime来源/哈希与资源校验脚本均保留。
- 截图：`testlist.png`、`scene2d3d.png`、`lifecycle-resized.png`、`lifecycle-restored.png`。`mask-graphics.png`实际仍是滚动后的菜单，不作为 mask 场景证据；随后真实鼠标操作进入并核对了标题 `2d-rendering-in-3d`。
- 中间失败也保留：junction创建不支持，首次隔离副本缺资源，不计入有效场景验证；一次 debugger-loop 内同步窗口调用卡住，终止自有采集器后重跑，不误归因引擎。minidump写失败，0字节 `crash.dmp`不当证据，活栈 JSON 有效。
- 五个关键 Vulkan 源码 SHA前后与 Worker 构建记录一致，原 exe未改变，测试项目 tracked diff仍为空；只新增 ignored证据与本记录/PROGRESS。
- 下一步：先处理CRT复制兼容性、纹理上传依赖和DS首次/重建布局，再在**这个真实工程**保持Validation开启复跑。尚未运行其它大量场景、RenderDoc、长期稳定性或性能矩阵。

| Verification Gate | 状态 |
|---|---|
| G1 API合法性 | 未通过：真实布局错误；其余API未全面审计 |
| G2 生命周期 | 有限证据：正常close/日志泄漏0；完整资源寿命未验收 |
| G3 同步 | 未通过：真实WAW hazard |
| G4 Validation | 未通过：实际有ERROR |
| G5 RenderDoc/AGI | 未验证：未抓帧；截图仅证明可见场景 |
| G6 平台回归 | 修复后原部署启动/场景/正常退出通过；此前隔离缩放/恢复有限证据保留，完整平台/其它设备未验收 |
