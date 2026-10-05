# 真实 Windows 工程 BestPractices 处理记录

日期：2026-10-05；执行渠道：Lead。任务状态以 VULKAN_COMPLETION_TASKS.md 的 T02 为准。Error、同步错误与 BestPractices advisory 分开，不关闭 Validation、不 suppress。

| 警告族 | 调查结论与处理 | 证据/限制 |
|---|---|---|
| ClearValueWithoutLoadOpClear | 修复：Classic clearValueCount只到最高实际CLEAR附件索引；无CLEAR count0/ptrNULL；Dynamic完整owned数组保留 | 原工程新构建bp-legacy/bp-sync2，警告消失，画面静态ROI不变 |
| vkCreateSwapchainKHR-no-VkSwapchainPresentModesCreateInfoKHR-provided | 修复：maintenance1启用时局部pNext声明一个实际选定present mode；不开模式切换、不保存栈指针 | 同两原工程流程警告消失，swapchain resize/restore正常 |
| StoreOpDontCareThenLoadOpLoad | 真实缺陷：每相机独立FrameGraph把跨相机DS当临时资源，先丢弃后LOAD。普通Vulkan目标改为导入RenderWindow持有DS，既有imported规则STORE；不是所有附件强制STORE | 菜单rdc同DS image先CLEAR/DONT_CARE后LOAD，43stencil draw；修复实际构建、六配置原工程运行、菜单GPU抓帧红→绿完成：同DS图像三pass STORE，43stencil draw及30像素采样0/1符合预期 |
| ImageBarrierAccessLayout | 已定位mip生成尾屏障，保留此前TRANSFER_WRITE→TRANSFER_WRITE正确性依赖。当前oldLayout=TRANSFER_SRC描述屏障当时布局，不表示更早的blit写都发生在该布局；source访问范围需要包含此前写操作 | Vulkan access与stage匹配，TRANSFER支持TRANSFER_WRITE；规范没有把srcAccess限制为oldLayout的典型访问。BP启发式建议READ，直接去掉WRITE可能撤销此前WAW修复。未授予M4-B优化/性能Gate |
| ImageMemoryBarrier-TransitionUndefinedToReadOnly | 已定位sampled RenderTexture首建layout seed，用于具体Classic RP initialLayout=SHADER_READ_ONLY；它仅建立布局，不初始化像素。当前有限正常场景Validation clean；保留advisory及明确内容边界 | UNDEFINED允许丢弃，新layout只读并非VUID非法；首次有效采样内容仍须由生产者写入。不能把layout seed当first-frame content golden，T04已回放24首用帧：第一次采样先于相机输出，本机六点RGBA0，后续帧读取前帧模型；API未定义初始颜色，保留先写入才有确定内容合同，不称driver噪声或跨GPU零初始化 |

## 依据与边界

- [VkRenderPassBeginInfo](https://docs.vulkan.org/refpages/latest/refpages/source/VkRenderPassBeginInfo.html)：clearValueCount覆盖最高CLEAR附件下标；不是总附件数量。
- [VkSwapchainPresentModesCreateInfoKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainPresentModesCreateInfoKHR.html)：声明允许的present mode集合；本机vendored EXT别名同义。
- [VkAttachmentDescription](https://docs.vulkan.org/refpages/latest/refpages/source/VkAttachmentDescription.html)：depth与stencil各有load/store；DONT_CARE不保证跨pass内容保留。
- [VkImageMemoryBarrier](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageMemoryBarrier.html)与[synchronization](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-dependencies-chains)：布局转换、源/目标访问范围和对应stage分别核对。历史写的availability不能仅为消除advisory删掉。
- 证据根：native/build/vulkan-modernization/win-completion-t01-lead/run-20261005-164243788/；source-after-ds-import.json、逐案raw/judge、renderdoc-bp2与后续新捕获。原warning/red与中间失败全部保留。
- 不使用LOAD-after-DISCARD的偶然像素保留作为正确性证明；menu stencil真实0/1与Equal测试只是实际操作证据。mask已知层过滤排除，不修改其场景。
- T02处理完成不意味着全部警告为0，也不意味着M4-A性能、T04内容、Android/Apple/XR或整计划完成。
