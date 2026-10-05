# Vulkan 资源退休清单 / 提交入口地图（M3-A，只读调查；M3-C 修订）

> 隶属：计划书 §11（M3 Timeline Semaphore + 资源退休）前置调查。
> 性质：**纯代码结构证据（file:line）**，非实现、非 Gate 通过。执行通道：Orca Worker。
> **M3-C 修订（2026-10-02）**：原清单存在真实误述与错位行号，本轮按当前源码逐条核实重写。
> 原调查历史见 PROGRESS §2W；本修订以 M3-B（fence tracker 基线接入后）代码为准。
> **M3-E 后续更新（Lead）见 §7**：§1~§5 的行号及 fence 等待描述是 M3-C 快照，当前 timeline 模式已替换 submission fence；独立资源退休和 CPU 引用保护仍未完成。

## 1. 资源生命周期表

| 资源 | 所有者/分配点 | 当前 reset/回收入口 | 当前完成证据 | 迁移接入点 | 风险/缺项 |
|---|---|---|---|---|---|
| Command Pool / Buffer | `CCVKGPUCommandBufferPool`（VKGPUObjects.h:762）按 queueFamily+slot 分池；`request()/yield()` 复用 | `reset()`（:827）：仅当轮到该 slot 且 used 非空时 `vkResetCommandPool`（:844） | 依赖 present() 中该 slot fence 的 wait | submit → tracker.point；reset 按 point 门控 | CB yield 后 GPU 未执行即 reset 危险；现靠 fence wait 兜底 |
| 用户 Command Buffer | `CCVKCommandBuffer` 录制经 `begin/beginRenderPass/.../end`；submit 路径：`CCVKQueue::submit`（VKQueue.cpp:~61 `prepareWSISubmit` → packageForFlight(early) → pendingQueue CB → packageForFlight(late)）→ tracker 包装的 vkQueueSubmit | submit 时消费 | submit 所附 fence→tracker point | submit 登记 completion point | **已录制未 submit 的 CB**：GPU completion 不负责其 CPU/Vk 引用（descriptor/buffer 无 ownership 计数），上层 TS 保证——这是 M3 后续待处理边界，不是"无需迁移" |
| Dynamic/Uniform/Upload transient allocations | `CCVKGPUBufferHub`（:1446）：`record(buffer, backBufferIndex, size, canMemcpy)` 将修改广播到其它 slot 的 pending map；`flush` 在 VKCommands.cpp 中按 canMemcpy 做 per-slot memcpy 或 vkCmdCopyBuffer。**per-slot 为 buffer.instanceSize 内的偏移复用（CCVKGPUBuffer.instanceSize/getStartOffset），不存在 getBuffer()/curOffset 游标分配** | 换槽时 flush（VKQueue.cpp:~60）更新后自然失效 | 该 slot 上一轮 fence wait 后重写安全 | update 随 submit 打点 | 描述"无专用 transient ring"仍然正确，但机制是 per-slot instance 重写，不是游标 |
| Descriptor Set instances | `CCVKGPUDescriptorSetHub::flush()`（:973）：按 `curBackBufferIndex` 更新 `instances[slot].vkDescriptorSet`；instances 数组按 backBufferCount 预建于 `CCVKGPUDescriptorSet` | 无 reset：per-slot vkDescriptorSet 由 pool 长期持有，flush 只写 | 上一轮该 slot fence | update 随 submit 打点 | Descriptor pool 按 layoutID 由 `CCVKGPUDevice::_descriptorSetPools`（VKGPUObjects.h:566）共享 map；`getDescriptorSetPool/link` 在 VKCommands.cpp；pool 销毁走 RecycleBin |
| Staging Buffer | `CCVKGPUStagingBufferPool`（:865）：CHUNK_SIZE 块链；`alloc()` 返回块内游标偏移 | `reset()`（:911）curOffset=0；`shrinkSize()`（:917）回收全空块 | present() 中 slot fence wait 后 reset+shrink | alloc 登记 retireAfter | curOffset 是 StagingPool 自己的游标（VKGPUObjects.h:898/917），与 BufferHub 无关 |
| Recycle Bin / Transient GPU 对象 | `CCVKGPURecycleBin`（:1186）per slot；collect()（VKGPURecycleBin.cpp:28-90：texture/view/FBO/descriptorSet/buffer） | `clear()`（:91）真 vkDestroy* | present() 中该 slot fence wait 后 clear | clear 按 point 条件 | 现安全窗口=slot 轮转+bulk wait |
| Fence 对象 | `CCVKGPUFencePool`（:562）per slot；`alloc()`（:573） | `reset()`（:590）vkResetFences 后复用 | **M3-B 后经 tracker**：submit → `{domain, value}` point；present 前 `observeFenceCompletion` 真实 getFenceStatus 观测 + slotComplete 门控 | 已接入 baseline tracker | fence 复用安全由 tracker point 单调保证 |
| Acquire semaphore | frame-slot 池（M2-C2）：acquire 专用，仅该 slot 全部 submit fence 完成后 reset | slot 轮转时 reset | slot fence→tracker point | 同上 | 已与 present semaphore 分离 |
| Present binary semaphore | `CCVKGPUWSIGeneration::renderFinished` per image + presentFences | M2-C2/C3 生命周期 | maintenance1: presentFences；纯 core: WaitIdle 回退 | **明确排除**：不由 timeline completion 单独回收 | 与 M2 解耦保持 |
| Transport early/late CB | `CCVKGPUTransportHub`（:1327）2 个常驻 CB；`packageForFlight()` 取出 | 取出后置 null，下次 request 重用 | 随所在 submit 的 tracker point | 同 Command Pool 接入点 | immediate 提交路径自带同步 wait |

## 2. 真实提交入口与完成依赖

**M3-B 后 vkQueueSubmit 经 tracker 包装**（VKCompletionTracker.h:45 `functions_.submit`，PFN 地址保留 volk hook）。

| 入口 | 位置 | fence | 完成语义 |
|---|---|---|---|
| `CCVKQueue::submit` | VKQueue.cpp:~61（唯一用户帧入口） | `gpuFencePool()->alloc()` → tracker.submit | tracker point |
| TransportHub immediateSubmission | VKGPUObjects.h:~1374（checkIn(record, true)）+ 同步 `vkWaitForFences` | 自有 fence | 同步阻塞；经 tracker 包装 |
| Device abortAcquiredSwapchain drain | VKDevice.cpp:~749 + :~928 同步 wait | fencePool alloc | 同步阻塞；经 tracker |
| swapchain 初始布局 / copyTextureToBuffers 读回 | VKSwapchain.cpp / VKDevice.cpp `checkIn(..., true)` → immediate 路径（同第 2 行） | 同上 | 同步 |

**frame-slot 依赖链**（present()，VKDevice.cpp:~1046-1051）：
1. `curBackBufferIndex` 推进（:1046）
2. `vkWaitForFences(新 slot 全部 fence)`（:1050-1051）——**等待的是即将复用的那个 slot 的 fence，不是所有在飞 slot**
3. `gpuFencePool()->reset()`
4. `gpuRecycleBin()->clear()`
5. `gpuStagingBufferPool()->reset()/shrinkSize()`

`waitAllFences()` 才跨 slot（resize/destroy/surface 路径）。M3-B 后 reset 前有 `observeFenceCompletion` 真实观测 + slotComplete 门控。

## 3. 迁移硬约束映射（不变，见 §2W 历史；补充）

- completion value 必须代表整个 submission 完成；Submit2 signal 默认 ALL_COMMANDS。
- 未 submit CB 的 CPU/Vk 引用问题**不是 GPU completion 能解决的**——M3 后续需单独处理（上层 ownership 或引用计数）。
- 新 tracker baseline ≠ 所有 pool 的 reset/clear 已全面按 point 门控细粒度 retireAfter——多数 pool 仍依赖 frame-slot 轮转+bulk wait。

## 4. 建议迁移顺序

1. ~~统一接口~~ M3-B 已完成（fence baseline）
2. ~~全部实际 submit 登记~~ M3-B 已完成（三入口均包装）
3. 逐资源按 point 回收：RecycleBin → Staging → BufferHub → CmdPool

## 5. Benchmark 现状（修订）

- 现成可采：vkWaitForFences 调用次数（**非固定 1/frame**：未使用 slot 可能为 0，resize/abort/immediate 有额外 wait）、host wait wall time、pool used count（**不等于 allocated count**，fencePool `_count` 是已分配标记）、CC_PROFILE render thread CPU。
- 缺条件：GPU timestamp 采集、P50/P95/P99 管线、vendor counters。不宣称任何指标实测或性能改善。

## 6. 边界

本清单 §1~§5 为 M3 前置调查与 M3-B 接入后修订，后续状态见 §7；不构成 M3 Gate 通过。

## 7. M3-E timeline 接入后更新（2026-10-02，Lead）

- `VKDevice.cpp:80`：实际 VkQueue 共享 tracker，GPUDevice 拥有专用 timeline semaphore。`:427` 选择 auto/fence/timeline；`:446` 查询真实 maxTimelineSemaphoreValueDifference；`:163/813` 在 device idle 后停用并销毁。
- `VKQueue.cpp:116`、`VKGPUObjects.h:1354/1394`、`VKDevice.cpp` 的 abort drain：timeline 不分配 submission fence；原有三实际 submit 入口携带递增 point。Fence 模式保留原所有权。
- `VKDevice.cpp:1131`：新 slot 的所有实际 queue point 等待完成后，才允许原 recycle clear、staging reset/shrink、acquire semaphore pool reset。`:1163` 全局等待各 queue 最新 point。CommandPool、BufferHub、DescriptorSet 的复用仍依赖相同 slot 轮转完成条件。
- CPU 已录制未 submit 的普通资源引用、每个 Resource 独立 retireAfter、跨 pool 细粒度 point 退休尚未实现；不能用最后一次已提交 point 忽略尚未提交的引用。
- Present binary semaphore 继续使用 M2 的 per-image/reacquire/maintenance1 或纯 core 退休回退规则，不能由 timeline 单独回收。
- 本轮有限 Windows WSI/lifecycle Validation clean；同 effective 1.3 两后端均 232 actual submit，submission fence 使用次数为 timeline 0 / fence 232。仅提供拓扑/使用计数，无完整性能改善结论。完整实测和边界见 PROGRESS §2AA。
