# Vulkan 非 WSI CPU 录制引用寿命审计（M3-F 初版 → M3-G 修订）

> 隶属：计划书 §11.2。执行通道：Orca Worker。
> **M3-G 修订（2026-10-02）**：初版多处事实错误由 Lead 复核指出，本轮逐条纠正重写。
> 原错误结论已在 PROGRESS §2AB 标注撤销；本版以当前源码为准。

## 1. CB 录制→提交→回收链（修正）

```
begin()                                    VKCommandBuffer.cpp:108-112
  _gpuCommandBuffer->began = true           :110
  recordedBuffers.clear()                   :111
bind*/draw/copy/blit 等                    :174+
  _curGPUPipelineState / _curGPUDescriptorSets / _curGPUInputAssembler 等
  ConstPtr(=IntrusivePtr<const T>) 持有 GPU 对象引用     VKCommandBuffer.h:92-98
end()                                      VKCommandBuffer.cpp:159-171
  _curGPUFBO/_curGPUInputAssembler 置空     :162-163
  【注意】_curGPUPipelineState、_curGPUDescriptorSets、_curGPURenderPass
        在 end() 时【不清空】，仍被 CB 持有
  vkEndCommandBuffer                        :165
  _pendingQueue.push(vkCommandBuffer)       :169
  _pendingWSIUsage.push(recorded)           :170
  pool.yield(_gpuCommandBuffer)             :171
    → VKGPUObjects.h:832-841：将 vkCommandBuffer 放入 usedList 并置
      gpuCommandBuffer->vkCommandBuffer = VK_NULL_HANDLE
    → 【不释放 _gpuCommandBuffer 对象或其全部引用】（IntrusivePtr 仍持有）
```

**修正要点**：
- `doDestroy`（VKCommandBuffer.cpp:95-106）才是释放全部引用的地方（非 end()）
- `ConstPtr<T> = IntrusivePtr<const T>`（GFXDeviceObject.h:68）——拥有引用，非 raw pointer
- yield 仅归还 VkCommandBuffer 句柄到池 usedList，_gpuCommandBuffer C++ 对象仍存活

## 2. 逐类资源引用表（修正版）

| 资源类 | CB 录制时持有 | shutdown 位置（实际存在） | 当前回收条件 | 未覆盖部分 |
|---|---|---|---|---|
| Buffer | IA 的 ConstPtr<CCVKGPUBufferView> → gpuBuffer ConstPtr (VKGPUObjects.h:421,304) | VKBuffer.cpp:94-101 → RecycleBin | slot point wait 后 clear | 未提交 CB 中的句柄 |
| BufferView | 同上 ConstPtr | VKBuffer.cpp:115-118（descriptorHub/IAHub disengage） | 无独立 GPU 句柄回收 | — |
| Texture | FBO 的 gpuColorViews ConstPtr → gpuTexture (VKGPUObjects.h:321) | VKTexture.cpp:135-143 → RecycleBin | 同上 | 非 swapchain 纹理无 epoch 门控 |
| TextureView | 同上 ConstPtr | VKTexture.cpp:151-153 → RecycleBin | 同上 | — |
| Sampler | GPUDescriptor.gpuSampler ConstPtr (VKGPUObjects.h:438)，非 pipeline layout 持有 | 随 GPUDescriptor | 同上 | — |
| DescriptorSet | _curGPUDescriptorSets ConstPtr (VKCommandBuffer.h:93) | VKDescriptorSet.cpp:227-251 → RecycleBin per-slot vkDescriptorSet | 同上 | instance per-slot 重写窗口 |
| PipelineState | _curGPUPipelineState ConstPtr (h:92) | VKPipelineState.cpp:73-74 → RecycleBin | 同上 | — |
| RenderPass | _curGPURenderPass IntrusivePtr (h:98)；**RP 自己拥有 VkRenderPass** | VKRenderPass.cpp:82 → RecycleBin | 同上 | — |
| Framebuffer | _curGPUFBO ConstPtr (h:96)；end() 置空但 IntrusivePtr 由 RP/FBO 关系持有 | VKFramebuffer.cpp:69-70 → RecycleBin | 同上 | — |
| CommandPool/CB | _gpuCommandBuffer IntrusivePtr (h:87)；end() yield 句柄但对象仍被持有 | Pool reset() VKGPUObjects.h:846-855：slot 轮转时 vkResetCommandPool | slot point | **vkResetCommandPool 将 CB 置 initial state（录制内容丢失），非句柄释放**；_pendingQueue 中句柄仍存在但内容已清 |
| Staging | 不直接由 CB 持有 | StagingPool reset/shrink | slot 轮转 | — |

## 3. 未 submit CB 的实际机制（修正）

**vkResetCommandPool 语义**：将该池的 CB 置为 initial state——**录制内容丢失，但句柄仍有效可重新 begin**。风险不是"句柄悬空"，而是"旧 _pendingQueue 条目指向的 CB 已被重置/重录，submit 时提交的是新内容而非原内容"。规范：vkResetCommandPool 置 initial；后续 begin 可复用同句柄。

**execute() 语义修正**（VKCommandBuffer.cpp:587-617）：consume secondary 的 pendingQueue，将句柄录入 primary 的 vkCmdExecuteCommands，**并合并 WSI 元数据到 primary**。不 push 到 primary 的 pendingQueue——secondary 的句柄成为 primary CB 内嵌数据。

**对象 destroy 与 owning 引用**：用户调用 `Texture::destroy()` → GPU 对象 shutdown → RecycleBin collect。但如果 CB 的 ConstPtr 仍持有该 GPU 对象（如 _curGPUPipelineState），IntrusivePtr 引用计数>0 → GPU 对象 C++ 对象不析构 → RecycleBin 条目不写入。**只有当最后一个 owning 引用释放后 shutdown 才真正执行**。这意味着：
- 用户 destroy 时若有 CB 正在录制持有该对象，shutdown 被延迟到 CB 的 ConstPtr 释放（通常在下一次 begin() 置空时）
- 一旦 shutdown 执行（最后一个引用释放），VkImage 等进入 RecycleBin 等 slot point

**yield 的 slot hash**：yield 用 `getHash(queueFamilyIndex)` 查池（VKGPUObjects.h:834），但 CB 可能跨 slot 录制/提交。推断：如果 CB 在 slot A begin、slot B end/yield，会按当前 slot 归还到该 queueFamily 的池（hash 含 slot，可能与 begin 时的池不同），reset 只检查 curBackBufferIndex——跨 slot 的池交互是否正确需复现验证（标注：推断，非实测）。

## 4. 可复现序列（修正版，设计未运行）

**序列 1（纯 offscreen，无 swapchain）**：
1. createTexture(T) + createFramebuffer(FBO) + createRenderPass(RP)
2. CB.begin → beginRenderPass(RP, FBO, clear) → endRenderPass → end
3. **不 submit**
4. FBO.destroy + delete → RP GPU shutdown → Texture GPU shutdown → 各进 RecycleBin
   （前提：无其它 owning 引用；CB 的 _curGPUFBO 已在 end() 置空）
5. device.present() 空帧 ×backBufferCount → slot 轮转 → RecycleBin.clear() → 真销毁
6. 尝试 submit 旧 CB（_pendingQueue.front()）
→ **风险**：VkImage/VkImageView 已被 vkDestroyImage/vkDestroyImageView；旧 CB 内 driver 已绑定的句柄变 invalid

**序列 2（pool reset）**：
1. CB_A.begin → 录制 → end（_pendingQueue 有条目）
2. device.present() 轮转并在其它 slot 正常 request，再回原 slot request → 惰性 pool.reset() vkResetCommandPool
3. CB_A.begin（新帧）→ pool.request 可能拿回同一 VkCommandBuffer 句柄
4. 录制新内容 → end → _pendingQueue 现有 [旧条目(句柄X), 新条目(可能同句柄X)]
→ **风险**：submit 时旧条目提交了新内容（语义错误但 Vulkan 可能合法因句柄已重新录制）

**推断标注**：两序列的实际危险取决于驱动对已销毁句柄的行为和引擎上层是否每帧 submit 清空 pendingQueue。序列 2 中的句柄复用可能导致 Vulkan 不报 VUID 但语义不正确——需 CPU trace 断言。

## 5. 迁移建议（修正版，撤销旧建议）

**撤销**：初版建议 untyped raw-pointer set + retireAfter 是错误的——raw pointer 不是 lease 可能悬空；point 必须含 domain 不能仅 value；未提交工作必须有明确丢弃/销毁路径。

**修正建议**：
1. **CPU 录制 lease**：CB 级增强——在 bindPipeline/bindDescriptorSets/bindInputAssembler/beginRenderPass 时通过已有 ConstPtr 机制（非额外 raw set）记录引用的对象。submit 成功后产生 retireAfter point（含 domain）。CB 明确丢弃时释放 lease；begin 新录制不应擅自释放仍在 pendingQueue 中的旧录制 lease。
2. **pool epoch**：CB 池增代次计数；begin/end 时记录 pool epoch；reset 递增 epoch；submit 检查 epoch 匹配拒绝过期条目。
3. **成功 submit 后绑定 point**：非 WSI 资源的 retireAfter 在 tracker.submit 成功后、从该 batch CB 的 lease 记录写入。失败 submit 不发点。
4. **present binary semaphore 不纳入**（硬约束）。
5. **风险**：offscreen 永不 submit 的 lease 持有 → 需明确丢弃路径（begin 覆盖或显式 cancel）；跨 slot/跨 queue 的 domain 判断。

## 6. 边界

初版/Worker 修订的未复现原因推断作废：其 hook 在设备初始化前安装，被 volkLoadDevice 覆盖，零计数无法证明 owning 引用保护。Lead 2026-10-02 修订接线后，control 实测 exit0 / Validation0 / submitObserved=1；delayed-resource 真销毁 VkFramebuffer 与 view，旧 CB submit 触发 VUID-vkQueueSubmit-pCommandBuffers-00070 并 exit -1073741819；pool-reset 实测重置1次、旧/新句柄相同，原 pending 条目仍转发，trace 断言 exit1（Validation0）。详见 PROGRESS §2AD。两项核心风险尚未修复；M3 逐资源 point 回收未完成。

## 7. M3-H 实现补记（2026-10-02，Lead）

本清单前面的源码行号/迁移建议对应 M3-G 历史快照；当前实现已增加 per-recording owning refs、pool epoch、secondary dependency、按实际 tracker domain/value 持有 submission batch。详见 VULKAN_MODERNIZATION_PROGRESS.md §2AF。原两红例现在在 timeline/fence 两后端均干净通过。未提交记录销毁、重 begin 和 secondary 的定向测试仍待补；split-event、descriptor 内容重写、staging 子范围复用/custom 外部句柄未因此自动安全。M3 没有整体验收。
