# 真实 Windows 工程 GPU 抓帧核对

日期：2026-10-05。执行渠道：Lead。工程为用户指定 test-cases Debug；不是独立 smoke。exe SHA256 `870bb4e9645835abfe89a715d5b6b7c935adcfd94a30b2ab96615f08075ecc05`。

## 当前证据

ignored 根：`native/build/vulkan-modernization/win-completion-t01-lead/run-20261005-164243788/`。RenderDoc 1.44 已安装，未下载/更新；child env FORCE=1.3、COMPLETION=auto、BARRIER=sync2、SUBMIT=submit2、RENDERING=dynamic。抓帧会扰动耗时，不作性能样本。独立原进程 Validation/退出/泄漏仍以 ds-owned 六配置为准，回放空 debug message 不替代 Validation triad。

| 场景 | 抓帧 | Classic begin | Dynamic begin | Barrier2 | Submit2 | draw | 实际含义 |
|---|---|---:|---:|---:|---:|---:|---|
| 2d3d | renderdoc-modern-scenes/frame_frame854.rdc | 2 | 0 | 6 | 1 | 63 | Modern编码实际消费；资格回退Classic |
| RenderTexture | renderdoc-modern-rt-v4/frame_frame1395.rdc | 5 | 0 | 12 | 1 | 22 | 真实两张256×256 RT与屏幕 pass，未执行Dynamic |

- 对每次draw从当前Vulkan管线状态读取PSO/活动RP/颜色和DS资源、fragment shader及真正used read-only descriptor（resource/view/格式/范围）；从vkCreateGraphicsPipelines结构独立核对PSO的RP。2d3d六PSO、RT五PSO，所有draw均有创建记录，pipeline RP与活动RP句柄差异0。原始attachment-evidence/chunk-evidence/pipeline-dependency-summary JSON均保留。
- RT两个图像（该稳态capture的ResourceId4815/4885）在屏幕draw event29/25被descriptor采样，随后event51/73开始相机输出；这是前帧RT内容消费的真实顺序，不是同帧producer先于consumer。显式Barrier2与Classic附件initial/final layout、子pass依赖均保留在原始chunk，不能只凭截图判依赖正确。
- 普通FrameGraph DevicePass开启运行时自动屏障；beginDynamicRenderPass明确拒绝autoBarrierEnabled。当前场景政策ON而真实Classic fallback。不得用路径flag=1冒称真实场景Dynamic draw；未来扩资格须证明布局/依赖等价，不能盲删门控。

## 菜单 stencil 的红→绿（同源修复）

- 旧renderdoc-bp2/frame_frame1737.rdc：同DS image1621/view1622，CLEAR/DONT_CARE→LOAD/DONT_CARE，43次stencil启用draw。旧store judge exit1。
- 新renderdoc-ds-owned-v2/frame_frame411.rdc：同DS image333/view336，三个pass都STORE；先CLEAR后LOAD，43次stencil draw，新store judge exit0。
- RenderDoc1.44 VulkanReplay::PickPixel的stencil按floatValue[1]×255读取。五个代表draw、各六点：全0→区域内1/外0→Equal reference1保留→后续camera CLEAR0，30点符合预期。具体face op/mask/reference和像素均在stencil-samples.json，不将RGBA packed bytes误当stencil。
- 未修改mask Canvas/layer/输入。未修改mask的捕获只证明当帧未启用stencil，不能当mask clipping通过；用户确认的触摸过滤问题不属于本次修复。

## 采集可靠性与限制

- 第一Modern采集到菜单/2d3d，但search没有输入，filtered ROI拒绝后停止；第一两份rdc有效，不冒称RT完成。三次补采集失败原始目录保留。最终v4直接操作自有PID实际可见RichEdit子控件，通过WM_GETTEXT验证（跨进程GetWindowText不能替代控件内容），菜单主体exact ROI diff0才选择RT；capture/coordinator exit0、CAPTURED。
- 这些是有限真实场景证据，不包含MSAA resolve/复杂secondary/FSR/XR的该工程GPU抓帧，不替代相应平台Gate。已有独立真实MSAA/D32 resolve内容回归仅补充。
- 首次进入连续24帧回放已完成（renderdoc-rt-first-use/burst-evidence.json complete=true）。978~982未出现RT，983首次两个256×256 RT采样event86/82先于相机输出109/132；每张三点RGBA为0。984及以后中心出现模型颜色、边角灰底0.2/a1，证明该场景消费前帧输出。六个首次采样点不是全图golden，更不是跨GPU零初始化保证。
- 源码 RenderTexture.initialize/_initWindow 与 RenderWindow::initialize建立目标与图像，没有定义初始像素颜色；现有layout seed也没有像素写入。按现有合同保留“必须先写入才有确定内容”的边界，不自动把所有RenderTexture改成透明黑、改相机priority或修改测试资源。若业务需要确定首帧，应另行定义初始化/生产先于消费的合同；本次不能宣称首帧透明黑具备可移植保证。此结论是内容调查结果，不把合法但未定义的初始内容计为Validation错误或已证明无风险。
- T04完成的是有限场景GPU内容/命令/依赖调查；复杂secondary、MSAA及外部平台抓帧仍按T10/T11和最终DoD保留范围，不宣称所有视觉Gate完成。

依据：[VkImageMemoryBarrier](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageMemoryBarrier.html)、[RenderDoc v1.44 Vulkan回放源码](https://github.com/baldurk/renderdoc/blob/v1.44/renderdoc/driver/vulkan/vk_replay.cpp)。
