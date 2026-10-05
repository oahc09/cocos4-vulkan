# 真实 Windows 测试工程验证（2026-10-05更新）

结论（2026-10-05最新调查）：**CRT、上传WAW、DS首用与RenderTexture修复的双路径有限证据保留；mask触摸失败现已定位为Canvas层与实际相机visibility不交**。真实工程legacy运行仅临时匹配层后，触摸/stroke及可见刮除出现，恢复原配置再入后重新无响应；未永久修改场景或renderer。BestPractices、工具级stencil内容、整体性能与平台验收仍待；本轮inspector诊断产生11条地址枚举ERROR，不宣称全部日志错误0。修复前历史保留，M0~M7不因此验收。

## mask 输入与 stencil 单变量运行调查（2026-10-05，Lead）

- 新报告 [VULKAN_MASK_INPUT_STENCIL_INVESTIGATION.md](VULKAN_MASK_INPUT_STENCIL_INVESTIGATION.md) 保存源码、实际camera、透明observer、原配置→临时层匹配→恢复再入证据及G1~G6边界。用户指定原Debug工程exe，本轮未重构建，未改测试场景/资源/renderer；实际exe SHA d6c5237a…870a1，不复用§2DY旧exe SHA冒称本轮构建。
- 原Canvas33554432（UI_3D）与全部三实际camera visibility不交，hitTest 34次全false、touch/stroke为0。仅运行中改成524288（UI_2D），真实拖动hitTest 65次全true、touch start/move/end=1/30/1、stroke30；截图蓝遮罩中出现刮开区域。中心ROI原拖动差0、临时匹配差60108像素；恢复7个observer与原层后返回再入，原拖动差又为0。
- Mask真实inverted/type/stage= true/2/6；native StencilManager和Batcher2d的状态链已读。有限视觉效果不等于GPU stencil bits读回或完整stencil正确，未抓RenderDoc/AGI。建议场景监听节点层匹配目标UIcamera；本轮只调查，不永久修复。
- 原路径PID37696运行547.72s后WM_CLOSE exit0；actual timeline1/sync2=submit2=dynamic0、Validation layer加载、VUID-vk/SYNC-HAZARD/游戏JS Uncaught日志0、唯一泄漏0。所有11条ERROR均来自inspector地址枚举，不把它们隐去；BestPractices五族含load/store仍待。诊断CDP启动/首版control异常留档，失败05/06截图不作为有效对照；仅v2层控制为成功样本。
- ignored根 `win-mask-investigation-lead/run-20261005-151510567737/retry/` 含完整raw log/CDP/PNG/恢复/summary。exe、场景和脚本前后SHA一致。本轮无Sync2/平台/性能重跑、不stage/commit。

## RenderTexture访问合同与首次采样修复（2026-10-05，Lead）

- **新红例**：扩展到 `render-texture-sample` 后，出现Classic RP initialLayout不匹配（00900）与colour/DS实际UNDEFINED（09600）。此前2d-rendering-in-3d有限绿例没有覆盖这条链。Worker WIN-SCENE-A只完成2d3d双路径；本节新增场景、修复、构建和运行均由Lead执行。
- **源码与实证**：ForwardStage虚拟颜色输出为COLOR_WRITE→COLOR_WRITE，`FrameGraph::move`替换为imported RenderTexture时未转换输出访问约定；实际图像随后用于shader采样。修复这条约定后仍有首次进入09600：报错时间13:52:42早于resize动作13:52:50，撤销“resize边缘错误”判断。仅在第一次相机RP之前seed太晚，已有消费者可能先采样新图像。
- **最终最小接线**：`GFXTexture::isSwapchainTexture()`提供真实swapchain身份；`FrameGraph::move`仅对imported、非view/非swapchain、单层单mip TEX2D、flags NONE、SAMPLED且非STORAGE的默认颜色写入双端约定，改成FRAGMENT_SHADER_READ_TEXTURE双端。其它访问元组不改；相同COLOR_WRITE双端元组会被转换，不能宣称任意用户自定义元组均被保留。
- **首用布局**：`cmdFuncCCVKCreateFramebuffer`仅在autoBarrier开启且实际RP初始布局为SHADER_READ_ONLY、prev恰为fragment sampled、非DISCARD时，对空访问状态的普通单采样colour图像排入既有early transport batch，UNDEFINED→声明的初始布局，目标SHADER_READ。录制retain texture至完成；不再在texture创建时普遍转换全部SAMPLED图像。view/range、外部图像、swapchain、storage、多mip/层等均排除。
- **DS配套**：现有Classic DS首用seed扩展到autoBarrier开启的离屏FBO，仍核对实际DS initialLayout及普通图像/空访问状态；manual offscreen和其它initialLayout不进入。原上传WAW修复保留，Dynamic分支不使用该seed。未增加ALL_COMMANDS、waitIdle、新submit或关闭Validation。

| 最终原Debug目录运行（Lead） | 实际sync2 | exit | ERROR / 00900 / 09600 / WAW / HAZARD / JS | leak |
|---|---:|---:|---|---:|
| contract-legacy | 0 | 0 | 全0 | 0 |
| contract-sync2 | 1 | 0 | 全0 | 0 |

- 实际timeline=1、submit2=0、dynamicRendering=0；SDK Validation layer确实加载。两独立进程均执行30个成功窗口动作、保存11张流程截图：2d-rendering-in-3d、atlas-compress、mask页面、搜索并进入render-texture-sample、1224×959→900×650缩放、最小化恢复、返回菜单再入与WM_CLOSE。只控制自有PID窗口；坐标导航前核对真实菜单，不以盲点点击冒称场景成功。
- **视觉读取**：2d3d机器人/3D几何与atlas羊图可见；RenderTexture左clamp单个立方体、右repeat网格可见，缩放/恢复/再入均保持。该检查是截图可见内容核对，非逐像素golden。mask拖动前后中心区域无差、未见Touch回调日志，不能声明刮除/stencil通过；Canvas layer与本场景camera visibility不交为源码候选，未取得运行时因果证明、未改测试项目资源强行通过。
- **构建证据**：用户指定VS树的当前Debug `test-cases`构建exit0，VKCommands.cpp确实重编、engine lib/exe重链；两运行exeSHA256同为 `9a7b5c9dbf289f316aa2132e16a8fc313da44d7104b731fc6e78e9a89c9c0bc0`。约192.6/286.1秒是debug-event采集时长，不能比较性能。成功构建后仅GFXTexture.h/FrameGraph.cpp各移除一个行尾CR，无代码token或行号改变；`format-only-record.json`证明规范化源字节与构建源一致，不冒称最终两处原始字节重新编译。最终运行前后六个源码SHA一致。
- **警告/边界**：两日志仍有BestPractices的UndefinedToReadOnly、ImageBarrierAccessLayout、ClearValueWithoutLoadOpClear、StoreOpDontCareThenLoadOpLoad及swapchain present-mode警告，错误0不等于警告0。计数来自完整捕获日志而非活原子。仍沿用existing currentAccessTypes录制状态，未实现新的逐subresource/未提交录制layout tracker；无RenderDoc、Android或性能矩阵/平台Gate。
- **最终证据**：`native/build/vulkan-modernization/win-scene-a-lead/run-20261005-124945523743/`，`contract-build-{stdout,stderr,exit}`、`contract-legacy/`和`contract-sync2/`各raw/actions/PNG、`contract-analyze.py contract` exit0、`contract-verification-summary.json`、`contract-additional-evidence.json`与前后source SHA。未改测试工程源码/资源/cache、未stage/commit。

本轮中间证据全部保留，以下不作为最终通过证明：

| 中间目录/方案 | 实际结果与撤销说明 |
|---|---|
| legacy / green-legacy / green-sync2 | 原布局合同或仅RP前restore仍00900/09600，日志ERROR40；先前中途“00900已消失”的文字已撤销 |
| probe | 临时固定日志探针确认访问模型与真实采样layout不一致，ERROR40；生产探针已移除 |
| attempt3 | CLEAR→UNDEFINED方案构建0，未运行，因LOAD/partial CLEAR合同风险撤销 |
| final-legacy | 仅看SAMPLED usage误包含swapchain，真实00897/09600红；增加真实swapchain与view排除 |
| guard-legacy | 00900/00897消失但首次进入仍colour09600×2（logger重复后ERROR4）；不是resize造成 |
| firstuse-legacy / firstuse-sync2 | 创建期广泛colour seed两次有限绿；最终审查收窄至具体FBO合同，原绿不替代最终源验证 |

首次捕获其它前台窗口的图不能用作场景证据，窗口工具已改为确认自有PID后操作；初始化黑屏/Cocos splash不当场景完成证据。firstuse-legacy中途暂停导航后补完剩余动作的事实保留，最终contract两案均使用完整30动作流程。

## DS 首次使用/重建修复与独立红绿（2026-10-05）

- **实际对象链**：framebuffer颜色来自swapchain，但DS可为普通纹理（非swapchain、独立vkImage）；新建/resize后的普通图像为UNDEFINED。Classic RP要求非UNDEFINED的initialLayout时，实际附件必须处于该layout：[VUID-vkCmdBeginRenderPass-initialLayout-00900](https://docs.vulkan.org/spec/latest/chapters/renderpass.html#VUID-vkCmdBeginRenderPass-initialLayout-00900)。本次遗漏发生于该混合FBO链，不能归因于真正的swapchain DS：后者使用swapchainVkImages并已有创建期转换。
- **Orca Worker 初稿/修订边界**：初稿删除swapchain访问状态并增加无条件DS_OPTIMAL seed；“状态改变RP initialLayout/不匹配时隐式转换被跳过”的根因解释撤销。RP initialLayout来自RP barrier而非texture.currentAccessTypes。初稿sync2目录raw实际sync2=0，声明撤销；rev2脚本恢复设置env后取得真正sync2=1及有限零错误运行，但窗口恢复与部分证据未完成。临时探针已移除，不以无法复核的历史探针文字替代证据。Worker撤回自己的swapchain hunk用了git checkout，非批准的编辑方式；最终VKSwapchain与里程碑逐字节一致，未保留该改动。
- **Lead 最终最小修复**：保留Worker发现的首用显式转换思路；RP创建时保存实际 `depthStencilInitialLayout`（包含DISCARD处理），在Classic分支、非offscreen FBO、普通非外部DS图像、访问状态为空且RP确需DS_ATTACHMENT_OPTIMAL时，发UNDEFINED→该初始layout、TOP_OF_PIPE→EARLY|LATE_FRAGMENT_TESTS、目标DS_READ|WRITE屏障。范围取实际view的mip/layer，联合DS aspect保持layout规则。其它initialLayout、合法UNDEFINED、真正swapchain DS、外部图像及Dynamic分支不进入此修复，原合同不改。
- **Lead 因果红例**：保存Worker补丁原字节并确认除seed外只有行尾差异；移除seed、保留此前WAW修复和原swapchain代码，真实Debug build0/run0但09600×2、WAW0。最终修复后重新build0，VKCommandBuffer/VKCommands及metadata消费者真实重编，engine lib/exe重链。无关42行行尾转换已恢复，未覆盖其它逻辑改动。

| Lead 最终原目录运行 | 实际paths | exit | ERROR / 09600 / WAW / SYNC-HAZARD / JS | leak |
|---|---|---:|---|---:|
| legacy | timeline1/sync2=0/submit2=0/dynamic0 | 0 | 全0 | 0 |
| sync2 | timeline1/sync2=1/submit2=0/dynamic0 | 0 | 全0 | 0 |

- 两次实际进入并读取 `2d-rendering-in-3d` 画面，缩放1224×959→900×650、最小化/恢复后仍显示场景；再恢复尺寸返回菜单进入 `mask-graphics`。mask截图有真实标题/“请刮开”，鼠标drag后画面无可确认变化，**仅证明进入和无错误，不声明刮除/stencil内容正确**。菜单/场景/resize/restore/返回菜单/mask PNG均保留。约97.5/93.7秒为调试采集运行时间，不作为性能数据。
- 实际SDK Validation模块与INSTANCE_LAYERS均在，两完整log无ERROR/VUID错误/SYNC-HAZARD/uncaught、无AV、唯一泄漏0，WM_CLOSE退出0。当前两运行exeSHA=`a17fab992de893492d7111e2e83b5cea377331f90898bf847c08fe656db3cbbc`；红例不同SHA。无旧exe代新、无关闭Validation或插入waitIdle/ALL_COMMANDS。
- Lead证据：`native/build/vulkan-modernization/win-ds-fix-lead/run-20261005-113230226739/`，红/最终build日志与exit、三独立runtime/raw、源SHA/截图/`analyze.py`、`verification-summary.json`（分析exit0）。准备脚本第一轮因空白行匹配断言停止且未改源；失败目录保留，第二轮校验无其它逻辑改动后才执行对照。
- 当前实现沿用existing texture.currentAccessTypes录制期状态，并非新建逐subresource/逐pending layout tracker；未扩展未提交录制的全局状态模型。G1/G2/G3/G4仅本次对象链、运行日志及窗口流程有限验证；G5未抓帧，G6仅本机Windows/RTX、非完整平台验收。未跑Android/全场景/性能矩阵，未stage/commit。下一步仍用这个真实工程覆盖更多场景和真实性能测量。

## 纹理上传 WAW 修复与真实工程复跑（2026-10-05）

- **Orca Worker 实施**：仅修改 `VKCommands.cpp/.h`。上传前图像屏障显式补充后续 `TRANSFER_WRITE` 可见性；mipgen尾部转换使用 `TRANSFER`→`TRANSFER`、写访问→写访问的图像屏障，范围为已转至 `TRANSFER_SRC` 的0..mipLevels-2。单mip不提交零范围屏障。保留THSVS、现有stage范围及两种编码路径；未加入ALL_COMMANDS、waitIdle或关闭Validation。
- **Lead 源码复核**：THSVS `thsvsGetVulkanImageMemoryBarrier` 仅在srcAccessMask非0时输出dstAccessMask。空/只读prev因此可能遗漏布局转换写对随后copy的可见性；转换写自动available仍需要目标访问的visibility。mipgen范围还有此前copy/blit写，需同时处理其availability；TRANSFER阶段覆盖copy/blit。依据：[Vulkan布局转换规范](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-image-layout-transitions)。未把未采集的运行时prev枚举冒称为已观测事实。
- **Lead 当前源码构建**：指定 `build/windows/proj` 的 `test-cases` Debug目标exit0，VKCommands.cpp等真实重编、engine lib/exe重链。两次运行均为原Debug目录exe，SHA256=`655bd0c21380ae498202a9d0fdacf03420569c2d1c17960a2ed11e12ce32d64b`；不是旧exe或隔离副本。

| Lead 独立运行 | 实际路径 | exit | WAW / SYNC-HAZARD消息 | DS09600消息 | 画面/窗口证据 |
|---|---|---:|---:|---:|---|
| legacy | timeline1/sync2=0/submit2=0/dynamic0 | 0 | 0 / 0 | 4 | 菜单、2d-rendering-in-3d、缩放后截图；自动正常关闭 |
| sync2 | timeline1/sync2=1/submit2=0/dynamic0 | 0 | 0 / 0 | 4 | 同场景、900×650窗口、最小化恢复后画面；WM_CLOSE正常关闭 |

- 两日志均实际加载 `VK_LAYER_KHRONOS_validation`，无uncaught exception、无AV、teardown唯一泄漏0。DS两aspect错误首次出现，缩放重建后再次出现；表中为精确消息计数而非活Validation计数。G3纹理上传子问题得到有限真实回归证据；G1/G4完整验收依然FAIL。
- Lead证据根：`native/build/vulkan-modernization/win-upload-waw-lead/run-20261005-105350229036/`；build-exit、两独立debug-output/runtime-summary、菜单/scene/窗口PNG与 `analyze.py`/`verification-summary.json` 均保留，分析exit0。legacy恢复命令到达时65秒自动关闭已结束，不当恢复成功证据；sync2恢复截图已独立读取确认。
- Worker日志在 `win-upload-waw-worker/20261005-run1/`：中间仅改mipgen的红例仍WAW10；最终日志WAW0但盲点导航造成JS异常，且只保留一个覆盖后的raw log，其sync2路径声明不能由该log单独证明。复制工具遗留Actor=Lead不改变实际执行渠道Orca Worker。场景/双路径结论采用本节Lead新进程独立证据。
- 未运行RenderDoc、Android、全部场景或性能矩阵。下一项为DS首次/重建布局；用户本地settings及临时文件保持原样，未暂存/提交本轮修复。

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

## 修复前真实运行与错误（历史）

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
| G1 API合法性 | 本次DS初始layout链有限验证；全API/全部场景未验收 |
| G2 生命周期 | 有限验证：正常close/缩放恢复/日志泄漏0；完整资源寿命未验收 |
| G3 同步 | 本次上传与DS首用回归通过：legacy/Sync2 hazard均0；全场景同步未验收 |
| G4 Validation | 本次两当前源码运行ERROR0；原始日志证据，非全场景/平台Validation验收 |
| G5 RenderDoc/AGI | 未验证：未抓帧；截图仅证明可见场景 |
| G6 平台回归 | 修复后原部署启动/场景/正常退出通过；此前隔离缩放/恢复有限证据保留，完整平台/其它设备未验收 |

## 2026-10-05 任务收口更新（Lead）

- 原Windows工程Debug矩阵与buffer WAW红→绿完成；最终DS归属修复源码又在六配置（含1.1/1.2/Modern完整策略/forced fallback）exit0/error0/leak0。BestPractices三族修复、两族带合同边界保留。atlas/RT静态ROI阈值0/diff0，有限动画人工核对。Android当前源码251步编译链接0，非真机运行。
- 正式状态与证据见 [VULKAN_COMPLETION_TASKS.md](VULKAN_COMPLETION_TASKS.md) / PROGRESS§2EA；本文旧段落保留对应历史时点。用户确认mask层问题排除，不继续输入修复；此前场景层改动不作为本轮Vulkan验收输入。
- 当前有限工程证据不等于整计划完成；自然低版本/Android/Apple/XR、>3%性能与M4-B Gate仍待。

- 当前G5有限GPU调查已完成：菜单同DS STORE/stencil0/1红→绿，Modern 2d3d/RT实际Submit2+Barrier2，所有draw PSO/RP与used descriptor核对；真实begin都是Classic fallback。RT24连续首用帧证明前帧内容消费，首次未写入像素没有可移植值保证，不靠本机RGBA0宣称零初始化。详见[VULKAN_REAL_PROJECT_GPU_CAPTURE.md](VULKAN_REAL_PROJECT_GPU_CAPTURE.md)。旧表“未抓帧”仅对应历史时点，不是当前状态。
- 正在原工程新增两策略×六轮资源重建/resize/minimize/restore压力；首份helper缺move和240秒截止失败保留，均未冒称完整通过。正式T08终态以任务清单记录为准。

### 最新有限资源压力终态（2026-10-05，Lead）

- 当前源码legacy-v3/modern-v3六轮RT重建与窗口流程均完整exit0/judge PASS/Error-VUID-SYNC-JS0/leak0，12张cycle静态主体diff0；源/exe不变。证据为win-completion-t01-lead/run-20261005-164243788/window-resource-stress/terminal.json；旧helper/期限失败保留，不混用终态。
- T08有限真实场景压力关闭；T05真实工程性能入口/正式采样尚未完成，M4-B前置与外部平台仍待，不能把本节当最终Vulkan完成声明。
