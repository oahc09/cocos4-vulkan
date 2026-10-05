# Vulkan 动态 Buffer instance 物理复用审计（M3-M3 Task B，只读）

> 隶属：M3 剩余寿命边界调查。执行通道：Orca Worker。下面是M3-M3修复前代码快照与行号，不代表修复后现状；后续Lead实际取证/修复见末尾。

## 1. 机制摘要

`MemoryUsage::HOST|DEVICE` 的 buffer 按 `instanceSize`（`uboOffsetAlignment` 对齐）×
`backBufferCount` 分配一块连续 HOST|DEVICE 映射内存（VKCommands.cpp:289-299）。
每个 frame-slot 拥有独立的 `[slot*instanceSize, (slot+1)*instanceSize)` 物理范围。
录制 CB 时绑定的 offset 在录制瞬间确定。

## 2. 逐项事实

| 项 | 事实 | file:line |
|---|---|---|
| instanceSize 计算 | `roundUp(size, uboOffsetAlignment)`（= minUniformBufferOffsetAlignment） | VKCommands.cpp:289; VKDevice.cpp:609 |
| 物理 range | `getStartOffset(slot) = instanceSize * slot` | VKGPUObjects.h:309-310 |
| 录制时 offset | descriptor 实例 offset 由 getStartOffset(instanceIndex) 计算；vertex/index 录制时取当前slot | VKGPUObjects.h:1289; VKCommandBuffer.cpp:392,400 |
| 跨 slot 广播 | `BufferHub::record(buf, srcSlot, size, canMemcpy)` 将"srcSlot 的数据需拷到其它 slot"记录到所有非 srcSlot 的 pending map | VKGPUObjects.h:1595-1603 |
| flush 时机 | `CCVKQueue::submit` 内 `gpuBufferHub()->flush(transportHub)` 在 submit 打包前执行 | VKQueue.cpp:61 |
| memcpy 路径 | `canMemcpy=true`：CPU 直接 `memcpy(dst=curSlot*instanceSize, src=srcSlot*instanceSize)` | VKCommands.cpp:1734-1736 |
| GPU copy 路径 | `canMemcpy=false`：`vkCmdCopyBuffer(同 buffer, srcOffset=srcSlot*instanceSize, dstOffset=curSlot*instanceSize)` | VKCommands.cpp:1746-1749 |
| 内存类型 | `VMA_MEMORY_USAGE_CPU_TO_GPU` + MAPPED | VKCommands.cpp:296-297 |
| submit point 登记 | `submitTracked` 在 `curBackBufferIndex` 的 `slotCompletions` 登记 point | VKDevice.cpp:121-122 |
| slot 安全窗口 | `present()` timeline等新slot points；fence等该slot fences，并复核 slotComplete 后 reset | VKDevice.cpp:1158-1178 |

## 3. 关键问题分析

### Q1：延迟录制 CB 在 slot0 取 offset 但实际 slot1 提交时，point 登记在哪个 slot？

**事实**：`submitTracked` 用提交时的 `curBackBufferIndex` 登记 point（VKDevice.cpp:121-122）。
CB 内绑定的 offset 是录制时的 slot0 offset，但 point 在 slot1。
之后回到 slot0 时，现有帧槽等待只检索 slot0 登记的 points/fences；不能仅凭该slot的等待证明这个延迟提交已完成。

**推断风险**：如果 CB 在 slot0 录制（绑定 slot0 offset），延迟到 slot1 提交，GPU 命令
引用的是 slot0 物理范围的数据。slot0 轮转回来时 `present()` 等 slot0 的 fences——但
该 CB 的 point 在 slot1，不在 slot0 的 slotCompletions 中。slot0 的 CPU memcpy 可能
在 GPU 尚未读完该 CB 引用的 slot0 数据时就覆写。

**关键区别**：这与 staging 不同。staging 的 `alloc()` 录制时返回当前游标偏移，且 recording
retain staging view（M3-K lease）。BufferHub 的 instance offset 是直接计算的，无 per-submission
保护。但 CB 的 recording 通过 `retain(buffer)` 持有 buffer 对象（非字节范围）。

### Q2：recording leases 保护对象还是字节？

**事实**：`CCVKGPURecording::retain(T*)` 持有 `ConstPtr<T>`（拥有引用，VKGPUObjects.h:406-411）。
对 buffer：`retain(gpuBuffer)` 保持 CCVKGPUBuffer C++ 对象存活→阻止 shutdown→阻止
RecycleBin collect。但 **不阻止 BufferHub::flush 的 memcpy/vkCmdCopyBuffer 覆写 instance 内容**。

**结论**：lease 保护**对象句柄**（VkBuffer 不被销毁），不保护**字节内容**。

### Q3：CPU_TO_GPU 是否保证 HOST_COHERENT？

**事实**：`VMA_MEMORY_USAGE_CPU_TO_GPU` 让 VMA 选择 `HOST_VISIBLE|HOST_COHERENT` 偏好的内存类型。
VMA 不强制 COHERENT——如果设备只有 non-coherent HOST_VISIBLE 堆，VMA 可能选 non-coherent。
**引擎 VKCommands.cpp/VKBuffer.cpp/VKDevice.cpp 的动态 mapped 写路径未发现 `nonCoherentAtomSize` 门控或 `vmaFlushAllocation` 调用**；vendored VMA 本身包含这些定义/实现，不能说整个仓库不存在。直接写位于 VKCommands.cpp:1296-1300，Hub memcpy 位于 :1734-1736；VMA CPU_TO_GPU 的要求/偏好实现见 vk_mem_alloc.h:3904 起。
`instanceSize` 对齐用 `minUniformBufferOffsetAlignment`，**不是** `nonCoherentAtomSize`。

**推断**：实际选中的 memory type 为 COHERENT 时，省略 flush 不构成这项缓存问题；这不证明跨slot复用安全。选中 non-coherent 类型时需要 flush；atom扩展的访问范围也需纳入复用约束。需查询 allocation 的实际 memory properties，不能从设备平台或 BAR 存在推定安全。

[Vulkan flush 规则](https://docs.vulkan.org/refpages/latest/refpages/source/vkFlushMappedMemoryRanges.html)
说明 non-coherent 写入的可见性及 atom 范围；[COHERENT 定义](https://docs.vulkan.org/refpages/latest/refpages/source/VkMemoryPropertyFlagBits.html)只免除相应缓存管理，不提供GPU完成证明。

### Q4：仓库是否真有独立 transient ring？

**事实**：**没有**。"transient" 数据（dynamic uniform/upload）通过 `MemoryUsage::HOST|DEVICE`
的 per-slot instance 机制实现，不是独立 ring allocator。`CCVKGPUBufferHub` 只做跨 slot
内容同步（record→flush 广播），不是分配器。唯一的游标分配是 `CCVKGPUStagingBufferPool`
（M3-K 已加 block lease）。

## 4. 边界

- 以上为纯代码结构分析，未运行跨 slot 延迟提交压力测试
- Buffer.update() 在 submit 前修改普通持久数据是合法语义，不是提前复用缺陷
- 不提议全面 COW 改造（未经验证）

Lead 复核补记：修正 descriptor/vertex offset、slot等待和retain行号；撤销“典型桌面安全”及“整个仓库无VMA定义”的泛化。以上仍为代码推断，模板fence1.1三例的独立日志已核实。

## 后续：Lead M3-M4实际取证与修复

原跨slot风险由真实GPU gate复现：point未完成且gate未signal时，slot0被Hub覆写为5A，旧CB最终读回64/64错误，Validation0也不能判PASS。Lead随后记录实际buffer范围的instance slot、递归secondary，成功submit把同一point关联到引用/提交slot；两后端及mapped写/Hub memcpy复用前等待。已补VMA flush和ubo/atom双重stride对齐。普通/template及gate/nonzero dynamic offset的有限运行已绿，详见PROGRESS §2AR。原“无flush/只关联提交slot”事实不再描述当前实现；仍无独立transient ring。非coherent硬件、性能/平台Gate尚待。
