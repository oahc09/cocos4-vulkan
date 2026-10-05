# Vulkan Sync2 / Submit2 迁移入口清单

> M4-A0 只读初稿：Orca Worker，2026-10-03。Lead 在 M4-A Submit2 接入时复核并修正调用数量、遗漏入口和 KHR 门控描述。
> 下列行号对应本次源码快照；实际进度见 VULKAN_MODERNIZATION_PROGRESS.md §2AY。Barrier2 接入及后续验证见 §2BA。

## 1. 原 legacy barrier：15 处活动路径 + 1 处未调用 debug helper

| 文件:行 | 用途 |
|---|---|
| VKCommandBuffer.cpp:187,287 | primary / secondary begin 的 general barrier |
| VKCommandBuffer.cpp:787,837,848,1128 | pipelineBarrier / full 与 split 混合路径 |
| VKSwapchain.cpp:520 | 新 swapchain 初始 color / depth layout |
| VKCommands.cpp:1248 | buffer upload 的 transfer barrier |
| VKCommands.cpp:1374 | buffer-to-texture copy 的目标 layout 转换 |
| VKCommands.cpp:1641 | texture layout barrier |
| VKCommands.cpp:1732 | command helper 汇总 memory / image barriers |
| VKDevice.cpp:1012,1016 | abort acquired image 的 early / late 转换 |
| VKDevice.cpp:1108,1117 | prepareWSISubmit 的 per-submit early / late 转换 |
| VKUtils.cpp:544（fullPipelineBarrier） | CC_DEBUG helper，当前无调用者，也已接统一encoder |

复核命令：`rg -n 'cmdPipelineBarrier\(|vkCmdPipelineBarrier\(' native/cocos/renderer/gfx-vulkan`。
初稿“7处”漏计多个实际调用且其分组计数自相矛盾。Lead §2AY 的15处又漏计未调用的debug helper，完整.cpp迁移点现为16处；旧数字保留为调查历史，不能作为全面覆盖率依据。bundled THSVS header里未调用的便利API仍保留legacy实现，不改第三方代码。

THSVS helper 在 VKCommands.cpp:400,647,1232,1638,1688,1717 使用，产出 legacy stage / access / layout。RenderPass dependency 也消费其中部分结果，不能仅替换函数名或丢弃 layout / queue-family / subresource / dependencyFlags。

## 2. Split 事件链

| 操作 | 文件:行 | 用途 |
|---|---|---|
| vkCmdSetEvent | VKCommandBuffer.cpp:983 | SPLIT_BEGIN，owning event lease |
| vkCmdWaitEvents | VKCommandBuffer.cpp:1098 | matched SPLIT_END |
| vkCmdResetEvent | VKCommandBuffer.cpp:1109,1121 | buffer / texture pair 的 device reset |

M4-A 的第一步保持 legacy Set/Wait/Reset 事件编码及现有寿命保护。
Barrier2 与 legacy event 可以分别处理，不能因保留 event 路径就禁止其他 full barrier 的独立迁移；也不能把同一事件的 legacy / Sync2 set-wait 语义随意混用。具体迁移需按规范与真实 event 回归核对。

## 3. Submit 路由（M4-A 首步已接入统一模型）

| 入口 | 文件:行 | 行为 |
|---|---|---|
| 用户 CCVKQueue::submit | VKQueue.cpp:133 | early + 用户 + late CB；原 acquire wait / per-image signal |
| TransportHub immediate | VKGPUObjects.h:1556 | 单批次提交后 tracker-selected completion wait；不是固定 vkWaitForFences |
| abort drain | VKDevice.cpp:941 | 消费本窗口 acquire wait，保持原提交拓扑 |
| completion 编码 | VKCompletionTracker.h:74 | 同一 VulkanSubmitBatch 选择 legacy / Submit2；专用 timeline signal 同批追加 |

所有入口经 CCVKGPUDevice::submitTracked（VKDevice.cpp:110）。Recording ownership 在真实 driver call 前预分配；成功才发布 domain/value point、关联实际录制槽及提交槽，失败保留用户 pending recording。业务层构造同一个 VulkanSubmitBatch，PFN 分发仅在 tracker encoder 层。

## 4. Capability 与 runtime path 门控

- synchronization2 请求策略仍为 effective minor >=3，并以真实 enable chain / vkCreateDevice 成功结果确定 enabled。
- core 1.3 选择 core PFN；较低 API 的 KHR 分支仅在 feature 和扩展实际 enabled 时才合法。Volk 加载入口不等于 feature / 扩展启用，也不自动为 1.2 开启功能。
- DEBUG `CC_VULKAN_SUBMIT=legacy|submit2`，默认 legacy。submit2 在 synchronization2 未 enabled 时干净拒绝；编码器与 fence / timeline completion 后端独立。
- DEBUG `CC_VULKAN_BARRIER=legacy|sync2` 独立选择useSynchronization2，useSubmit2按上述选择；timeline 在 M3 已接入且通常 ON，不能声称 runtimePaths 全 OFF。
- 1.2 KHR Submit2 分支尚无真实设备运行证据，当前策略也不主动在该档请求 Sync2。

## 5. 后续顺序与边界

Submit2 首步证据见 §2AY；Barrier2 由 owning VulkanBarrierEncoding + cmdPipelineBarrier dispatcher接入，保留原stage/access/layout/dependency topology。两encoder独立A/B，未优化scope。空资源数组的execution-only依赖必须显式生成zero-access memory barrier。未建模pNext不静默丢弃。

M4-A Gate 仍要求 Synchronization Validation=0、render output identical、submit count unchanged、performance non-regression。有限 clear / compute 内容和 mock 契约不能替代游戏画面、移动/XR及性能 Gate。

M4-B 范围优化等待 M4-A Gate；本步骤不缩窄 completion signal stage，不改变 present binary semaphore 寿命，不启用 Dynamic Rendering。
