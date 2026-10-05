# Vulkan WSI 资源生命周期与引用链审计（M2-A，只读）

> 隶属：计划书 §10（M2 Swapchain/WSI 生命周期闭环）前置审计。
> 性质：**纯代码结构证据（file:line），非 Validation/画面证明**；M2 状态机尚未接入。
> 所有行号基于 2026-10-02 工作树（M1-C/D 接入后）。

## 1. Acquire / Present 状态传播

| 位置 | 事实 | 评估 |
|---|---|---|
| `VKDevice.cpp:699-707` | acquire 每帧清 `lastSignaledSemaphores`/三个 vk 向量；`_onAcquire` 回调 | — |
| `VKDevice.cpp:713-717` | `lastPresentResult == VK_NOT_READY` → `checkSwapchainStatus()` 尝试重建；失败 `continue`（该 swapchain 本帧跳过） | 唯一的"跳过"入口，但见 §3 未 acquire 仍渲染问题 |
| `VKDevice.cpp:736-739` | `vkAcquireNextImageKHR` 后仅 `CC_ASSERT(res == SUCCESS || SUBOPTIMAL)`；OUT_OF_DATE/SURFACE_LOST/DEVICE_LOST 仍继续写 `vkSwapchainIndices[i]`、push acquireSemaphore、按 `swapchainImages[staleIndex]` 填 barrier | **根因（Lead 已确认）**：失败返回码无状态传播，失败 index 继续被消费 |
| `VKDevice.cpp:762-805` | present：单一返回值 `res` 广播给**所有** `gpuSwapchains`（:803-805）；无 `VkPresentInfoKHR::pResults` | 多 swapchain 时一个返回值覆盖全部状态（计划 §10.3） |
| `VKDevice.cpp:809-811` | `if (!vkSwapchains.empty())` 空采集不 present；`curBackBufferIndex` **先推进再等 fence** | all-acquire-fail 时 present 为空合法；slot 轮转语义见 §5 |
| `VKSwapchain.cpp:341-344` | 0 尺寸：`lastPresentResult = VK_NOT_READY; return false`（不重建） | 显式 zero 有路径，但见 §4 语义混叠 |
| `VKSwapchain.cpp:336-338` | `-1`（未知）：退回 colorTexture 当前尺寸 | "未知尺寸"与"显式 0"是两个不同输入，见 §4 |

## 2. Semaphore 全部提交路径与所有权

| 位置 | 事实 | 评估 |
|---|---|---|
| `VKGPUObjects.h:545-590` | `CCVKGPUSemaphorePool`：设备级全局池，`alloc()` 顺序复用 | 无 per-image / per-swapchain 所有权 |
| `VKDevice.cpp:740` | 每次 acquire 开头 `_gpuSemaphorePool->reset()` | **acquire 信号量复用无"已被 wait 消费"证明**（计划 §10.5） |
| `VKDevice.cpp:742-743` | acquire 信号量 push 进 `lastSignaledSemaphores` | — |
| `VKQueue.cpp:64-92` | submit：wait=`lastSignaledSemaphores`（stage=COLOR_ATTACHMENT_OUTPUT），signal=池中新分配的**单个** binary semaphore；submit 后 `lastSignaledSemaphores.assign(1, signal)` | renderFinished 为 frame-slot 级而非 per-swapchain-image（计划 §10.4）；同一池与 acquire 复用同一 reset 周期 |
| `VKDevice.cpp:797-799` | present 等待 `lastSignaledSemaphores`（= 上一 submit 的 signal） | submission fence/该 signal **不能证明** presentation engine 已消费（官方 swapchain_semaphore_reuse 指南；计划 §1.5） |
| `VKGPUObjects.h:1302-1316`（checkIn immediate） | transport 立即提交：独立 `vkQueueSubmit` + 同步 `vkWaitForFences` | 无信号量参与；resize 首帧 barrier 即此路径（`VKSwapchain.cpp:399-405`） |
| `VKQueue.cpp:47-56` | 每帧普通提交前 packageForFlight(early/late) 挂接 | present() 也独立 package（`VKDevice.cpp:775-776`） |

## 3. Multi-swapchain 向量配对（结构性缺陷）

`VKDevice.cpp:722-729` 三个向量成员规则不同：
- `vkSwapchains`：仅 `vkSwapchain != VK_NULL_HANDLE` 者（:722-724）
- `gpuSwapchains` / `vkSwapchainIndices`：所有未 continue 的 swapchain（:725-729）

`:734-739` 的 acquire 循环以 `i < vkSwapchains.size()` 为界，却索引 `gpuSwapchains[i]` / `vkSwapchainIndices[i]` / `vkAcquireBarriers[i]`。**任何"vkSwapchain 为空但 gpuSwapchain 存在"的成员（surface 已销毁、XR 与窗口混布、重建失败）都会造成索引错位**：acquire 结果写入错误 swapchain、barrier 绑定错误 image。`vkAcquireBarriers.resize(count, ...)`（:711-712）按输入 count 而非 vkSwapchains.size()，含上一帧陈旧元素（resize 不清理已存在元素）。

## 4. 未 acquire 的 swapchain 仍被渲染 / 尺寸语义

- 渲染侧消费点全部按 `curImageIndex` 取图，不检查本帧是否 acquire 成功：
  - `VKCommandBuffer.cpp:111-113, 165-167`：`vkFrameBuffers[swapchain->curImageIndex]`（beginRenderPass/继承）
  - `VKCommandBuffer.cpp:578, 622, 669, 677`：blit/copy 按 `swapchainVkImages[curImageIndex]`
- acquire 失败（§1 第 3 行）后 `curImageIndex` 为陈旧值 → 这些调用点继续"渲染"到上一帧 image。**门控位置实际存在**：`root.ts:775-803` 帧级调用（acquire→render→present）在 TS 侧，native `Device` 接口为 void；真正的 native 门控点是 `CCVKDevice::acquire` 内（跳过成员已实现）+ 上述 CB 绑定点（未实现）。all-acquire-fail 时 offscreen submit 仍合法（present 已判空），但 swapchain 纹理路径会踩陈旧 index。
- 尺寸语义混叠：`VKSwapchain.cpp:325-326` `newWidth = width ? width : currentExtent.width` —— **调用方传 0 表示"未知/查询"与"最小化为 0"两种语义走同一分支**；显式 0 只有当 currentExtent 也为 0 时才触发 NOT_READY（:341）。Android 侧 `doInit/doCreateSurface` 传 `viewSize`（:265-268, :495-497），最小化时 viewSize=0 → 误走 currentExtent 回退。

## 5. Resize 链：旧资源去向与销毁顺序

`VKSwapchain.cpp:305-433`（checkSwapchainStatus 非 XR 分支）：

| 步骤 | 位置 | 事实 | 风险 |
|---|---|---|---|
| 1 | :351 | `waitAllFences()`（全部 fence pool） | fence 只覆盖 submission，**不覆盖 presentation engine 对旧 swapchain image 的消费** |
| 2 | :353-354 | `VK_CHECK(vkCreateSwapchainKHR)`，oldSwapchain 传入 createInfo | 重建失败（OUT_OF_DATE 等）→ VK_CHECK 中止，无 RECREATE_PENDING 状态 |
| 3 | :355 | 立即 `destroySwapchain()` 销毁旧 vkSwapchain | 旧 image 可能仍在 presentation 中使用；destroySwapchain（:447-464）同时清 `swapchainImages` 并复位两个 index |
| 4 | :357-361 | `updateBackBufferCount(imageCount)`；`VKDevice.cpp:841-842` **只增不减** | 缩容时 slot 池保持旧大小（良性），但 `curBackBufferIndex` 取模新值，slot↔池 映射不收缩 |
| 5 | :363-365 | color/depth 纹理强制 width=1 后 `resize(newW,newH)` | 旧 depthStencil `swapchainVkImages`（自建分配）经 texture 重建 → 旧 view/framebuffer 进 recycleBin |
| 6 | :366-405 | 为全部新 image 发 PRESENT/DS 写布局 barrier（transport 立即提交） | — |
| 7 | :349 | `++_generation` | 下游缓存失效：`GFXTexture.cpp:52`（纹理 hash）、`NativePipeline.cpp:252-260`、`NativeResourceGraph.cpp:205-207`（TS 侧渲染管线按 generation 重建 render target） |
| 8 | view/framebuffer 延迟销毁 | `VKFramebuffer.cpp:69-71`、`VKGPUObjects.h` shutdown → `gpuRecycleBin()->collect()`；bin 在 `present()` fence wait 后 `clear()`（`VKDevice.cpp:818-819`；`VKGPURecycleBin.cpp:56+`） | **旧 vkSwapchain 先销毁（步骤3）而其 views/framebuffers 后经 bin 延迟销毁**——bin 持有的是 view/FBO 句柄，view 生命周期覆盖旧 image 句柄被销毁的窗口；靠步骤1 的 waitAllFences 兜底 submission，presentation 消费依旧无观测 |
| 9 | CB 引用 | `VKCommandBuffer.cpp:136` `_pendingQueue.push`（begin 后入队，submit 时消费） | resize 发生在 acquire（帧首）→ 此刻 pendingQueue 已空（上帧已提交）；帧中 resize 无路径 |

## 6. Surface 创建/销毁与平台层

| 位置 | 事实 | 缺口 |
|---|---|---|
| `GFXSwapchain.cpp:38-42` | `SwapchainInfo.windowHandle` 必填；`createSurface()`（:97-99）更新 `_windowHandle` 再 `doCreateSurface` | windowHandle 可直接传 HWND，无需引擎窗口对象（§8 harness 依据） |
| `VKSwapchain.cpp:487-501` | `doDestroySurface`：waitAllFences → destroySwapchain → destroySurface → NOT_READY；`doCreateSurface`：createVkSurface + checkSwapchainStatus | **doCreateSurface 不重查 queueFamilyPresentables / surfaceFormats / presentModes / compositeAlpha**（这些只在 doInit :80-235 查询）→ surface 重建后 present 支持与格式选择基于旧 surface 数据 |
| `VKSwapchain.cpp:452-463`（destroySwapchain） | Swappy destroy（Android）+ vkDestroySwapchainKHR + index 复位 | — |
| `VKSwapchain.cpp:407-425`（Swappy 接入） | 每次 resize 重新 init/interval/window | per-swapchain 正确；`VKDevice.cpp:779`（SwappyVk_setQueueFamilyIndex）为全局队列级 |
| Win32 窗口 | `SystemWindow.cpp:77`（WM_CLOSE）等；窗口尺寸事件由 TS 层转 `swapchain->resize()` | 0/未知尺寸语义见 §4 |

## 7. XR 独立路径（保持独立，不套 vkAcquire）

- `VKSwapchain.cpp:454-456`：XR 无 VkSurface；images 来自 runtime（:311-322，left 全量 + right 全量拼接，典型 6 张）；`curBackBufferIndex=0` + `updateBackBufferCount(总量)`。
- acquire：`VKDevice.cpp:719-721` `xr->doGFXDeviceAcquire` 直接给 `curImageIndex`（无 vkAcquireNextImageKHR、无信号量）；present：`VKDevice.cpp:766, 801, 822-824` `isGFXDeviceNeedsPresent` 门控 + `postGFXDevicePresent` 归还 runtime。
- `destroySwapchain` 对 XR 无操作（vkSwapchain 为空，:447 判空）→ runtime 拥有图像，引擎不销毁 ✓。
- XR 与窗口混布时会触发 §3 的向量错位（vkSwapchains 空 + gpuSwapchains 非空）。

## 8. Win32 WSI 压力 harness 可行性（未实现，仅入口审计）

- 现成接入点：`native/CMakeLists.txt:3846-3848`（`CC_VULKAN_BUILD_TESTS` 已挂 vulkan-engine-smoke 子目录），同模式可加 `wsi_stress` 目标；构建沿用 win64-simulator 树。
- 无需引擎窗口对象：`SwapchainInfo.windowHandle` 直收 HWND（`GFXSwapchain.cpp:38-42`），`CCVKSwapchain::doInit` 的 `CC_GET_SYSTEM_WINDOW(_windowId)` 仅 Android 分支（`VKSwapchain.cpp:264-268`）。
- 最小调用序列（全部现有 API）：
  1. `CreateWindowExW`（WS_OVERLAPPEDWINDOW，无需消息泵即可 present）
  2. `DeviceManager::create()`（返回 DeviceAgent，代理 CCVKDevice）
  3. `device->createSwapchain(SwapchainInfo{windowHandle=hwnd, width, height, vsyncMode})`
  4. 帧循环：`device->acquire(&sc,1)` → `device->createCommandBuffer()` → beginRenderPass(swapchain colorTexture 的 framebuffer 简单 clear) → `queue->submit` → `device->present()`
  5. 压力项：`SetWindowPos` resize / `ShowWindow(SW_MINIMIZE)`（zero-size）/ `DestroyWindow`+`createSurface`（surface recreate）/ `SetWindowLong` 等
- 可行结论：**能**在 vulkan_engine_smoke 旁以独立 exe 做 Win32 真实 WSI 压力；Android/XR 无设备 → pending。
- 注意：SwapchainInfo 还需 `windowId`（默认 0 即可，Win32 不消费）与 vsync/transform 字段（`GFXDef-common.h` SwapchainInfo）。

## 9. 最高优先 5 个断点（建议实施顺序）

1. **acquire/present 状态机落地**（根因，§1）：`VKDevice.cpp:736-739` 失败码传播 + NOT_READY/OUT_OF_DATE/SUBOPTIMAL/zero-size 分支；`VKSwapchain.cpp:341-344` 已有 NOT_READY 骨架可挂状态。
2. **多 swapchain 向量配对修复**（§3，结构性）：`VKDevice.cpp:722-739` 三向量同构化（或 per-swapchain 结构体数组）；barriers 同步重建。
3. **per-swapchain-image renderFinished semaphore**（§2，计划 §10.4）：替换 frame-slot 级单信号量；安全复用条件=再次 acquire+acquire 同步入 wait 链；presentation 完成观测在纯 core WSI 下无显式 fence → fallback 边界必须写明（vkQueueWaitIdle/超时启发不得称为完成证明；EXT_swapchain_maintenance1 显式 fence 属能力协商项，本轮禁止直接用）。
4. **旧 swapchain 退休顺序**（§5 步骤 1-3）：resize 时旧 vkSwapchain 立即销毁改为退休队列（延迟到可观测 idle 或下一帧 acquire 全失败后），或至少记录边界；`doCreateSurface` 补齐 presentables/formats/presentModes 重查（§6）。
5. **尺寸语义分离**（§4）：`checkSwapchainStatus(width,height)` 增加"未知"显式哨兵（如 -1 已有路径 :336-338）与"显式 0"（直接 SUSPENDED_ZERO_SIZE）区分；TS/root 侧传参路径需同步（root.ts:775 之上）。

## 10. 根因 vs 推断标注

- 根因（代码直接证据）：§1 assert-only、§3 向量规则差异、§4 宽度 0 回退、§5 步骤 3 立即销毁、§6 doCreateSurface 不重查。
- 推断（需运行/Validation 证实）：§2 presentation 消费未被 fence 覆盖的实际竞态窗口、§5 旧 image presentation 中销毁的实际表现、§9-3 各 fallback 的实际必要性。
- 纯代码结构 ≠ Validation/画面证据；本审计未运行任何 WSI 压力场景。
