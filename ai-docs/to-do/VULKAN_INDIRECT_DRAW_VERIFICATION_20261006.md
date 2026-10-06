# T13-02b 真实 Windows 工程 Indirect Draw 有限验证（2026-10-06）

结论：索引/非索引两命令内容、多命令/循环回退、实际上传同步和正常资源退出的有限合同通过。T13整体、性能/平台与最终验收仍未通过。执行渠道为Lead；无委派、无Git提交。

## 工程与改动

- 用户指定源码 `E:/AIProMax/github/cocos4-vulkan-test-project/native/engine/win64`，VS树 `build/windows/proj`，真实Debug `test-cases.exe`；不是独立smoke替代工程。
- 新增默认关闭的 `native/tests/vulkan-real-project/RealProjectIndirect.{h,cpp}`，通过原工程Game启动事件进入既有DeviceAgent队列；普通GFX API创建Buffer/IA/PSO、updateBuffer、draw、submit。诊断期间临时选择multi/loop，之后恢复。没有修改场景、资源或mask输入。
- 原工程只增量接线CMake/Game/main三个原生文件；累计补丁 `real-project-integration.patch` 的非修改reverse-check为0。原工程所有文件当前未跟踪，不能把git diff为空当没有改动。
- 核心修复仅 `VKCommandBuffer.cpp` beginRenderPass上传RAW屏障：补 `INDEX_READ|INDIRECT_COMMAND_READ` 和 `DRAW_INDIRECT`，保留已有Uniform/Vertex消费者；+5/-2。不改submit拓扑、不加waitIdle/宽ALL_COMMANDS、不关闭Validation。不是等待前置的M4-B缩窄优化。

## 真实红→绿与同步六要素

首次真实modern-multi出现 `SYNC-HAZARD-READ-AFTER-WRITE`：`vkCmdCopyBuffer`上传间接VkBuffer后，`vkCmdDrawIndirect`读取未被上传屏障覆盖。该屏障此前只有Uniform/Vertex读，缺少Indirect读。

- producer：同录制中的staging→命令Buffer拷贝；TRANSFER/TRANSFER_WRITE。
- consumer：DRAW_INDIRECT/INDIRECT_COMMAND_READ；索引Buffer为VERTEX_INPUT/INDEX_READ。
- resource：实际56字节Indirect backing，非索引前32字节、索引前40字节；实际12字节索引Buffer。现有fallback采用memory barrier，不伪称已改为逐range屏障。
- queue：同graphics queue；实际Submit2/fence路径分别核验，不需要跨队列所有权迁移。
- layout：Buffer无image layout；独立颜色附件COLOR→TRANSFER_SRC与HOST_READ只服务测试读回。
- dependency：原fallback消费者范围补齐；测试没有在draw前额外加入手工Indirect可见性屏障掩盖生产缺陷。waitAllFences只用于已经提交的读回完成。

首次红日志、错误退出及原诊断误报销毁计数保留。Buffer/Image的VMA destroy函数值在hook前已捕获，初稿把它们当全局hook可观察对象产生假未销毁；改为区分facade释放与直接观察的非VMA句柄。真实上传RAW与这项取证缺陷分开记录。

## Native与GPU内容证据

根目录：`E:/AIProMax/github/cocos4-vulkan/native/build/vulkan-modernization/win-indirect-t13-lead/run-20261006-055716198`；`terminal.json`含10个接受的native终态、每个实际exe SHA、例外说明与最终GPU回放。

| 案例 | 实际策略/内容 | 结果 |
|---|---|---|
| modern-multi-final | effective1.3/timeline/Sync2/Submit2；每种一调用count2 | exit0，两phase命令/图像全匹配 |
| final-modern-loop | 当前最终源码；同现代策略，每种两调用count1 | exit0，全部匹配，Validation/泄漏0 |
| legacy-multi | forced1.1/fence/legacy/Classic，每种一调用count2 | exit0，全部匹配；不是自然低版本设备证明 |
| postformat-legacy-loop | 1.1/fence/legacy，每种两调用count1 | exit0，全部匹配 |
| skip-second | 第二命令vertexCount/indexCount0，Vulkan命令合法 | exit1；两个真实phase各512字节golden差，packed/submit/chain仍完整，live/post0；这是预期拒绝，不是正例绘制PASS |
| default-off | 未设置诊断变量，原场景照常启动/退出 | exit0/error0/leak0，无诊断draw |
| bad-mode/exclusive-mode/empty-block | 未知模式/与prewarm互斥/实际空值Unicode环境块 | 各exit1，设备初始化前拒绝，无draw/submit证据 |
| prewarm-cold-regression | 原工程T09 cold，Indirect关闭 | 既有共享judge PASS、exit0，验证保存payload77514；未重复全部17案/冷暖性能 |

最终exe `5a551ccbe0905e7586a7ef05c2ae38721aeaa3f998a324be7835169e64fe42a1`。上述历史矩阵exe另有d51222fb…/a671d1b1…，各summary保留真实SHA；后续变化仅诊断格式化和空环境变量检测，核心屏障未变化。最终源码有当前modern-loop及独立RenderDoc捕获，不能将其他历史exe冒记为当前exe。最终捕获前后四份源SHA一致。

每个正例的索引/非索引实际packed分别40/32字节；每幅32×16 RGBA8完整2048字节左红右绿，mismatch0。当前final-modern-loop与历史modern的直接观察句柄8/8、legacy7/7恰销毁一次；7个Buffer、2个Texture facade各释放一次，完整device shutdown后allocator leak0。**不宣称本轮逐Buffer/Image的vkDestroy观察证明**。

## RenderDoc独立回放

当前最终exe的 `renderdoc-indirect-final/indirect_capture.rdc`：官方1.44 app API显式捕获两个离屏phase，未注入RenderDoc/未设capture路径时完全不请求捕获。使用未修改MIT授权 `renderdoc_app.h`，不自动安装工具/修改注册表。

- 两实际API为 `vkCmdDrawIndirect(count2,stride16)` 与 `vkCmdDrawIndexedIndirect(count2,stride20)`；4个实际子draw，每个3顶点/索引、1实例、同真实PSO。
- 实际命令Buffer为Resource235/251，words分别 `[3,1,0,0,3,1,3,0]` 与 `[3,1,0,0,0,3,1,3,0,0]`；从API结构字段找到源Buffer再读取，非只看CPU期望。
- EID17/47实际输出均2048字节与独立golden完全相同；debug messages0。15个Barrier2 chunk中两处上传消费者srcAccess4096/dstAccess15/dstStages142，包含修复后的Indirect/Index。
- **实际两个BeginRenderPass都是Classic**。原工程autoBarrierEnabled门控保留；Dynamic策略ON不等于Dynamic被driver消费，不借本测试宣称Dynamic Indirect完成。
- `replay-raw.json`、源Buffer和输出bin、`judge-replay.py`与`content-judgment.json`均保留。首轮Windows文件名含ResourceId双冒号写入失败的JSON保留；修正取证文件名后回放通过，不改GPU数据。

## 判据、失败与限制

`verify-indirect-evidence.py`真实与31个fixtures共用：actual exe/native exit/异常、实际API/paths、唯一精确marker/footer、两phase、packed/完整golden、已观察资源退休与allocator leak0。缺失/重复/畸形phase、零submit、packed0、坏golden、01、外名footer、junk leak、NTSTATUS等拒绝；skip-second只接受唯一正确golden失败原因，其他错误拒绝。

初始空值测试通过os.environ赋空串，Windows会删除该变量，实际是OFF；两份日志保留但**排除为empty guard证据**。最后用CreateProcess显式Unicode环境块保留KEY=，空值拒绝真实exit1；诊断查询Win32存在性避免CRT把空值当未设置。clang-format depot_tools shim exit9009保留，改用已安装LLVM后exit0并真实重构建。

有限G1/G2/G3/G4/G5仅覆盖此合同；G6为本机Windows RTX有限证据。firstInstance固定0（不强启未请求的drawIndirectFirstInstance）；非零firstInstance、IndirectCount/GPU生成命令、Dynamic实际draw、自然低版/Android/Apple/XR和性能不在完成范围。T13-02b记“已完成（有限）”，T13整体继续；接下来处理已有Bloom内容及Compute场景前提，不提前实施M4-B。
