# Vulkan 后端性能审查与技术演进路线

> 审查对象：`native/cocos/renderer/gfx-vulkan/`（Cocos Native 的 Vulkan 渲染后端）
> 方法：4 个并行源码追踪代理，逐文件定位 `file:line`；关键 P0 结论已用 grep 二次核对。
> 配套文档：`ai-docs/codebase-guide/frame-flow.html`（调用链树）、`frame-sequence.html`、`architecture.html`。
> 演进更新：[vulkan-evolution-improvements.md](./vulkan-evolution-improvements.md)（吸收 1.4 / Roadmap 2026 的分层改进建议，含 descriptor_heap 取代 descriptor_buffer 等关键修正）。

---

## 1. 范围与方法

本次审查覆盖四大性能域，均基于真实源码而非推测：

| 域 | 关键文件 |
|---|---|
| 着色器 / 管线态编译 | `VKShader.cpp`、`VKPipelineState.cpp`、`VKPipelineCache.cpp`、`VKPipelineLayout.cpp`、`VKDescriptorSetLayout.cpp` |
| 内存 / 缓冲 / 描述符分配 | `VKGPUObjects.h`、`VKBuffer.cpp`、`VKTexture.cpp`、`VKDescriptorSet.cpp`、`VKCommands.cpp`、`vk_mem_alloc.h`(VMA) |
| 命令录制 / 提交 | `VKCommandBuffer.cpp`、`VKCommands.cpp`、`VKQueue.cpp`、`VKSwapchain.cpp`、`VKDevice.cpp` |
| 架构 / 扩展性 / 全局停顿 | `VKDevice.cpp`、`GFXDeviceManager.h`（装饰器链）、`gfx-agent/` |

---

## 2. 架构视角：对"高可用 / 负载均衡 / 微服务化"的引擎语境重述

你提出的"高可用性、负载均衡、服务解耦与微服务化拆分"是**分布式业务系统**的范式，对单进程原生引擎并不字面适用。但其*意图*在引擎里都有对应物，建议按如下映射理解，审查与演进都围绕它们展开：

- **高可用性 (HA) → 渲染健壮性与容错**：`VK_ERROR_DEVICE_LOST` 设备丢失恢复、交换链 `OUT_OF_DATE_KHR` 重建状态机、GPU 资源回收安全、崩溃安全的管线缓存。
- **负载均衡 → 多核命令生成负载均衡**：把录制从单一设备线程分散到多个工作线程（二级命令缓冲并行录制），上传走独立 transfer 队列，计算走 async compute 队列重叠。
- **服务解耦 / 微服务化 → 引擎内模块解耦**：现有的**装饰器链 `DeviceValidator ⊃ DeviceAgent ⊃ 真实后端`** 就是"服务边界"；`gfx-base` 抽象 + 多后端（Vulkan/GLES3/WebGPU/Empty）即"可插拔微服务实现"。进一步要隔离的是 render-pass 抽象层与 feature-flag/扩展探测层。
- **可扩展系统架构 → 模块化管线**：Legacy（Forward/Deferred）与 Custom RenderGraph 已并存，应继续向"稳定接口 + 可插拔后端变体"演进。

---

## 3. 性能审查结论（优先级排序）

| 优先级 | 优化点 | 位置 | 影响 |
|---|---|---|---|
| **P0** | 缺少 PSO 哈希缓存（每次 `vkCreate*Pipelines` 同步阻塞） | `VKPipelineState.cpp:43-67` → `VKCommands.cpp:978-1194` | 渲染线程驱动级停顿，Material 重建/复用即重复编译 |
| **P0** | 帧末 `vkWaitForFences(全部栅栏, VK_TRUE)` 每帧阻塞 + 交换链 `OUT_OF_DATE` 未处理 | `VKDevice.cpp:716-717`、`VKSwapchain.cpp:335`、`:649`/`:313`/`:623` | 限制流水线并行深度；OOD 返回 → release UB |
| **P1** | 过度宽泛的全局内存屏障（所有上传合并为单个 `VkMemoryBarrier`） | `VKCommands.cpp:1612`(`BarrierManager::update`)、`1634-1648`；render pass 级 `VKCommandBuffer.cpp:150-153,235-237` | 全管线串行化到最坏 stage 组合，阻塞所有内存 |
| **P1** | 管线缓存 blob 仅进程干净退出时落盘 | `VKPipelineCache.cpp:70-77`(析构)、`98-119`(全量重写) | 崩溃丢失全部预热数据；每次退出整块重写 |
| **P1** | 运行时 GLSL→SPIRV 且无源码去重 | `VKShader.cpp:58-63`、`VKCommands.cpp:870-884` | 相同 GLSL 的多个 `CCVKShader` 各自重编译 |
| **P1** | 描述符集 / 管线布局后端无缓存；描述符池粒度粗（`maxSetsPerPool=10`） | `VKPipelineLayout.cpp:41-56`、`VKDescriptorSetLayout.cpp:40-57`、`VKGPUObjects.h:584/681/632-635` | 大量唯一布局时 Vk 对象与池数量膨胀 |
| **P1** | 单设备线程 + 二级 CB 并行录制路径未用于场景录制 | `GFXDeviceManager.h:144-165`、`VKCommandBuffer.cpp:199`(支持)、`:538-558`(executeCommands) | 多核录制潜力未释放 |
| **P1** | 每帧向量 resize / 描述符脏跟踪用 `unordered_set` | `VKDevice.cpp:619-639`、`VKGPUObjects.h:1101` | 高频路径堆分配与集合 churn |
| **P2** | 命令缓冲池无预热（1 增量增长） | `VKGPUObjects.h:713-750` | 首帧/抖动走 allocate |
| **P2** | 跨 camera 未合并 submit | `VKQueue.cpp:96` | 多次 `vkQueueSubmit` + 各自 fence/信号量开销 |
| **P2** | 动态 offset 变更导致重绑 `_firstDirty..N` 全部集合 | `VKCommandBuffer.cpp:313-318,764-785` | 仅 set2 offset 变也重绑 2..N |
| **P2** | staging 池 `alloc()` O(块数) 线性扫描、增长无软上限 | `VKGPUObjects.h:805,811,842` | 大上传帧主机内存与查找浪费 |
| **P2** | 小动态 UBO 对齐填充（256B × backBuffer）浪费 | `VKCommands.cpp:283-289` | 内存膨胀 |
| **P2** | 纹理冗余屏障（copy 自插 + BarrierManager 再插） | `VKCommands.cpp:1325-1333` + `1660` | 重复屏障 |

---

## 4. 详细优化点

### P0 — 立即处理

**① PSO 哈希缓存（消除主导性渲染线程停顿）**
- 现状：`CCVKPipelineState::doInit`（`VKPipelineState.cpp:43-67`）通过 `cmdFuncCCVKCreateGraphicsPipelineState` 走 `VKCommands.cpp:978-1194` 直接 `vkCreateGraphicsPipelines`，后端**无**按 `(shader+layout+renderpass+fixed-state)` 的哈希缓存，不同对象请求相同 PSO 各自重编译。
- 修复：在 gfx 层加 PSO 缓存（key = 上述签名哈希），命中则复用 `VkPipeline`；理想情况下在**工作线程**异步创建并延迟绑定。这是收益最大的单项改动。

**② 帧末全栅栏等待 + 交换链鲁棒性**
- 现状：`present()` 末尾 `vkWaitForFences(_gpuDevice->vkDevice, fenceCount, gpuFencePool()->data(), VK_TRUE, DEFAULT_TIMEOUT)`（`VKDevice.cpp:716-717`）每帧等待**所有**在途帧栅栏；resize 走 `waitAllFences`（`VKSwapchain.cpp:335`）。`acquire()` 仅 `assert(VK_SUCCESS || VK_SUBOPTIMAL_KHR)`（`VKDevice.cpp:649`），`OUT_OF_DATE_KHR` 完全未处理——release 下返回 OOD 即未定义行为（`VKSwapchain.cpp:313` 仅在尺寸变化或 `lastPresentResult==VK_SUCCESS` 触发重建）。
- 修复：改用**时间线信号量 + 只等最旧帧**的信号量（真正的 N 帧环形流水线，而非等全部栅栏）；把 `OUT_OF_DATE`/`SUBOPTIMAL` 从 `acquire` 与 `present` 两侧都作为**延迟重建状态机**的触发条件，与渲染调用解耦。

### P1 — 高价值

**③ 屏障收窄（全局 → 按缓冲/按范围/按 stage 组）**
- `CCVKGPUBarrierManager::update`（`VKCommands.cpp:1612`）把本帧**所有**上传缓冲合并为单个 `VkMemoryBarrier`（`:1634-1648`），以及 `begin/endRenderPass` 每 pass 插入全量 `TRANSFER→VERT/FRAG` 全局屏障（`VKCommandBuffer.cpp:150-153,235-237`）。应改为：每个缓冲独立的 `VkBufferMemoryBarrier`，按真实 `srcAccess/dstAccess` 与作用 stage 组批，避免为只送顶点阶段的上传去阻塞 fragment/compute。

**④ 管线缓存安全持久化 + 预热**
- `saveCache()`（`VKPipelineCache.cpp:98-119`）仅析构时（`:70-77`）写整块 blob，崩溃即丢失；且 `_dirty` 每次新建 PSO 都置位 → 退出必整块重写。改为：每新建 N 个 PSO 增量落盘；启动期预建已知 PSO 填充缓存。

**⑤ 离线 SPIRV + 着色器源码去重**
- `VKShader` 首次使用走运行时 glslang（`VKCommands.cpp:870-884`），相同 GLSL 的多个对象各自重编译且 `vkCreateShaderModule` 各建一份（`VKShader.cpp:58-63`）。改为：发布预编译 `.spv`，按源哈希建后端去重 map。

**⑥ 描述符/布局缓存与池粒度**
- `VkPipelineLayout`/`VkDescriptorSetLayout` 每次新建对象无后端去重（`VKPipelineLayout.cpp:41-56`、`VKDescriptorSetLayout.cpp:40-57`）；描述符池按固定 `maxSetsPerPool=10` 整块分配（`VKGPUObjects.h:681`）。加布局签名缓存；池大小可调/几何增长。

**⑦ 多线程命令生成（二级 CB）**
- 已有按 `thread_local` 的 per-thread 命令池（`VKGPUObjects.h:478`、`VKCommands.cpp:52`，`tbb::concurrent_unordered_map`）与 `executeCommands` 支持（`:538-558`），但主路径用 INLINE 录制。引入 per-pass 二级 CB 录制器，让 cull/record 任务并行产出，提交线程 `executeCommands` 合并。

**⑧ 热路径分配/查找清理**
- `acquire()` 每帧 `vkAcquireBarriers/vkPresentBarriers.resize(...)` 并重新 push（`VKDevice.cpp:619-639`）；描述符脏跟踪用 `unordered_set<CCVKGPUDescriptorSet*>`（`VKGPUObjects.h:1101`）。改为预分配/缓存 barrier 数组，脏跟踪改用扁平 bitset。

### P2 — 增量

命令池预热、跨 camera 合并 submit（`VKQueue.cpp:96` 内部已合并单 submit 的 CB，但多 camera 仍多次 submit）、动态 offset 重绑范围收窄（`VKCommandBuffer.cpp:313-318,764-785`）、staging `alloc` 扫描与增长软上限（`VKGPUObjects.h:805,811,842`）、小 UBO 对齐打包（`VKCommands.cpp:283-289`）、纹理冗余屏障去重（`1325-1333`+`1660`）。

---

## 5. 3–5 年技术演进路线

> 以"稳定接口 + 可插拔后端变体 + GPU 驱动"为主线；与第 2 节的重述一一对应。

| 阶段 | 主题 | 关键举措 | 对应架构意图 |
|---|---|---|---|
| **第 1 年（现在→12mo）** | 性能硬化与鲁棒性 | P0/P1 全部落地：PSO 缓存、屏障收窄、时间线信号量 + 最旧帧等待、OUT_OF_DATE 延迟重建状态机、离线 SPIRV、管线缓存安全持久化 | HA（容错）/ 负载均衡（消除停顿） |
| **第 2 年** | 并行化与现代化提交 | 二级 CB 多线程命令生成、async compute 队列重叠、bindless/描述符索引、`VK_KHR_dynamic_rendering` 削减 render-pass 开销、GPU 驱动 indirect draw（GPU culling） | 负载均衡（多核）/ 服务解耦（feature 层） |
| **第 3 年** | 现代几何与光线 | Mesh Shading（task/mesh）、`VK_KHR_ray_tracing`、buffer device address 支撑 bindless、meshlet/cluster culling | 服务解耦（可插拔管线变体） |
| **第 4–5 年** | 统一 GPU 驱动渲染 | 虚拟几何/cluster 渲染、多 GPU/异构（iGPU+dGPU）负载分配、ML 超分（FSR/DLSS 钩子）、WebGPU 收敛、自适应分辨率/画质 | 高可用（自适应降级）/ 微服务化（跨后端统一抽象） |

**现代 Vulkan 能力现状（路线基线）**：已具备 VMA 子分配、`vk_mem_alloc`、持久映射、back-buffer 池化、状态脏检查、fence/信号量池化——这些是良好基础，演进时**不要破坏**。尚缺：dynamic rendering、buffer device address、timeline semaphores、ray tracing、mesh shading、async compute 专用队列、GPU 驱动 indirect（仅 `VKBuffer.cpp:106` 有 `indexedIndirectCmds` 桩，无 GPU culling/命令生成）。

---

## 6. 已确认的良好实践（保留，勿误改）

- **VMA 子分配**：`vmaCreateBuffer`/`vmaDestroy*` 全量使用，智能专用/子分配（`VKCommands.cpp:148,157,166,279,282,287`）。
- **持久映射**：staging 池与 `HOST`/`HOST|DEVICE` 缓冲创建时一次性映射，靠指针算术复用，无每帧 `vkMap/Unmap`（`VKCommands.cpp:1268,1702`、`278/286`）。
- **状态脏检查扎实**：pipeline/descriptor/IA/viewport 均有 `_cur*` 守卫，描述符集延迟到 draw 合并绑定（`VKCommandBuffer.cpp:298-301,309-312,324-346`）。
- **fence/信号量池化复用**：`gpuFencePool`、`_gpuSemaphorePool` 按 slot 复用，wait 后 `reset()`（`VKDevice.cpp:720,732,643,646`）。
- **单 submit 内 CB 批处理良好**：`CCVKQueue::submit` 内部已合并所有 CB（`:55-92`）。
- **指针而非字符串查找**：热路径 `unordered_map<const CCVKGPUBufferView*/...>`（`VKGPUObjects.h:1105-1108`），无字符串哈希。

---

## 7. 参考坐标（已核对）

- PSO 同步创建：`VKPipelineState.cpp:43-67`，`VKCommands.cpp:978-1194`（无后端哈希缓存 — 已 grep 核对）
- 帧末全栅栏等待：`VKDevice.cpp:716-717`（present）、`:745`（resize waitAllFences）— 已 grep 核对
- 全局内存屏障：`VKCommands.cpp:1634-1635`（BarrierManager 内 `VkMemoryBarrier`）— 已 grep 核对
- 着色器编译：`VKCommands.cpp:870-884`；管线缓存：`VKPipelineCache.cpp:70-77/98-119/79-96`（建于 `VKDevice.cpp:464-465`）
- 屏障管理器：`VKCommands.cpp:1612`；render-pass 全局屏障：`VKCommandBuffer.cpp:150-153,235-237`
- 描述符/布局：`VKPipelineLayout.cpp:41-56`、`VKDescriptorSetLayout.cpp:40-57`、`VKGPUObjects.h:584/681`
- 命令池：`VKGPUObjects.h:713-750`；二级 CB：`VKCommandBuffer.cpp:199,538-558`；提交：`VKQueue.cpp:96`
- 交换链鲁棒性：`VKSwapchain.cpp:313,335,623`；`VKDevice.cpp:649`
- 多线程序号池：`VKGPUObjects.h:478`、`VKCommands.cpp:52`
