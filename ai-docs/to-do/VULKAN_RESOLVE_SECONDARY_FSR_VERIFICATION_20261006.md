# 真实 Windows 工程 Resolve / Secondary / FSR 有限验收

执行日期：2026-10-06；执行渠道：Lead。T13-06 的下述有限合同完成，整体验收未通过。

## G1：范围、当前代码与构建

- 用户指定 `cocos4-vulkan-test-project/build/windows/proj`，Debug `test-cases`。新增 `RealProjectResolve.*`、Color/Depth 两个实现与 `RealProjectFeatures.*`，运行在该工程既有 DeviceAgent 渲染线程，使用已有 Vulkan device；不创建第二个引擎、不替换 Resources。
- 默认 OFF；与 prewarm/indirect/subpass/alternate-resource-root 互斥；未知或空模式在设备创建前拒绝。累计 `real-project-integration.patch` 记录 main/Game/CMake 接线，原基座 forward check 与当前项目 reverse check 均0。
- Resolve 三文件从既有 standalone MSAA/D32 harness 派生，原 standalone 源不改；移除独立 DeviceManager、入口、SPIRV 全局重初始化，保留真实 shader/PSO/CB/copy/readback 逻辑。运行结束后由实际项目 shutdown 检查退休，而非诊断结束前伪造 postErrors。
- Resolve build1/build2、Features build1 均0。Resolve 正例及对应 RDC 的 EXE SHA256 均 `8f24b8b31b1a21226e4d19e3af190e3f0f3d30196245fa25ae1ce154be18d572`；Features 正例及 RDC 均 `dd253e5726d37b0cf4ad2a8e93f5aa8e8d39e2a9fa0c116bd69aa815e033ae17`。两批分别关联，不把旧运行改记为新二进制。

## G2～G4：实际路径、内容与负控制

| 合同 | 实际原生证据 | 内容判据与结果 |
|---|---|---|
| RGBA8 color 2x/4x resolve | Classic 与引擎 Hybrid Dynamic；四独立录制/submit/copy，uniform/edge 各 owning 快照 | 每分支每 pattern 748B；uniform 红187像素，edge 红/蓝/混合且4x量化；全 golden 与两 pattern 互 diff0 |
| D32 depth-only 2x/4x SAMPLE_ZERO | Classic+Dynamic、近红 z=.25 与远绿 z=.75 LESS，两 draw/两 resolve copy | 每分支 color748B、depth187float；红色与.25 golden 全0；colorDiff/depthDiff 独立0 |
| Secondary | 现代 Submit2/Sync2、另1.1/fence/legacy；真实继承、execute 与子CB draw | 32×16→24×12→32×16，左右红/绿，5248B 全golden；drop secondary/PSO facade 在 submit 前，管线销毁仍0 |
| FSR attachment | 查询真实2×2 sample1支持及16×16 texel范围；R8_UINT rate image；FSR强制Classic资格回退 | 同三波次；rate byte=5，fragment shader读取实际 `gl_ShadingRateEXT`，全5248B为红 |

- 六 Resolve 正例：color2/color4/depth2/depth4、color2-fence、depth2-sync2。三 Features 正例：secondary、secondary-legacy、fsr。native 均0，Validation triad ENABLED，ERROR/VUID/HAZARD/JS、live/post 与唯一 allocator leak 均0。正常进程均自有窗口 WM_CLOSE 后退出，非强杀通过。
- 实际负例：skip-color draws0、wrong-uniform 输出蓝、skip-depth 缺近 draw、skip-execute 未执行 child、fsr-wrong-rate image为0。全部 native1，golden非零；前3个 resolve 负例互 diff仍0，证明互相一致不能替代golden。负例 Validation 仍无VUID/HAZARD。
- 合法低版：FORCE1.1+Classic 返回 UNSUPPORTED/native3，正常shutdown/leak0，未当PASS。
- 历史红：FORCE1.1 又请求 Dynamic，后端 failBootstrap 后测试游戏落入 Empty Device/abort，采集最终强杀60929；原目录保留、不算拒绝成功。随后仅诊断入口增加预检；新实际进程同冲突 native1、设备创建前拒绝，未修改全局 DeviceManager 的失败语义。
- default OFF、两个模块未知模式以及上述冲突预检均实际运行。Depth hook 的 clearValueCount 从旧 standalone 的4改为当前最高CLEAR索引+1=3；源调用依旧两 color clear+depth1.0。编译过程中2处 enum→flags 收缩转换/采集脚本 schema 修订如实保留在本轮日志，不当引擎故障。

## G5：独立 GPU 抓帧

- 四份对应真实EXE的 RenderDoc 1.44 capture/replay：color4、depth4、secondary、fsr，均 Complete。未抓任意菜单帧冒充诊断；注入后显式 API capture 包住既有设备上的诊断，诊断自身不加载 RenderDoc DLL。
- Color：四实际draw、四copy；attachment0真实4sample、attachment1真实1sample；完整 resolved pixels golden/edge量化与模式互相一致。
- Depth：四实际draw、四copy。Classic RP attachment3实际1x D32；Dynamic 的实际 `vkCmdBeginRendering.pDepthAttachment.resolveImageView`，经该view的真实 parentResources 链到独立1x D32图像；resolveMode=1/layout=3。最后实际API事件之后，两图187float全部.25。RenderDoc当前动态 framebuffer 列表不显示 resolve slot，不能凭猜测资源ID补证。
- Secondary：三真实 `vkCmdExecuteCommands`、三个子CB draw、三ClassicRP/Submit2/copy；实际输出全golden。FSR：实际RP shadingRateAttachment=1、texel16×16、rate image全5；实际 fragment SPIR-V BuiltIn ShadingRateKHR 装饰与全红输出同时验证，非仅 pipeline create 成功。

## G6：退休、共享判据与限制

- Resolve：每进程4目标管线各毁一次，跨实际 device shutdown 检查；未把此计数称为所有Vulkan句柄计数。Secondary：现代21/21、旧版18/18；FSR18/18，shader/PSO/layout/RP/FBO各目标句柄一次。Buffer/Texture facade 分别计数，最终VMA leak0另证。有限重建/recording lease，不是物理 device-loss 恢复证明。
- `verify-rendering-features.py` 共用真实与反例判据：9 native正例、4 replay，30单变更/实际负例正确拒绝，exit0。新增的SPIR-V读取最初漏掉 `.spv` 文件装入，judge真实KeyError后补齐；未以失败时输出宣称通过。
- BestPractices advisory保留；derived resolve harness 的 broad依赖是验证合同，不是M4-B优化，没有因本项缩窄生产同步、增加生产submit/wait、关闭Validation。
- 不涵盖 stencil resolve/更多mode、FSR各设备/所有rate、复杂secondary/XR/multiview、自然低版GPU、游戏性能/温控。原计划要求的本机特性有限合同分别完成；T13异常/恢复与T05/平台验收仍须独立记录。

Raw根：`native/build/vulkan-modernization/win-resolve-t13-lead/run-20261006-115613721300/` 与 `win-features-t13-lead/run-20261006-120704819323/`；后者 `terminal-content.json`、源码SHA和共享judge结果。未暂存、提交或推送。

FSR解释依据：[Khronos GLSL扩展](https://github.com/KhronosGroup/GLSL/blob/main/extensions/ext/GLSL_EXT_fragment_shading_rate.txt)、[attachment texel约束](https://docs.vulkan.org/refpages/latest/refpages/source/VkFragmentShadingRateAttachmentInfoKHR.html)。设备实际支持以本轮query与shader读取为证。
