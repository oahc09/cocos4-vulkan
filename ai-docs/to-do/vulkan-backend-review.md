# Vulkan 后端现状、性能审查与技术演进路线

> 更新日期：2026-10-01
> 范围：当前仓库的 `native/cocos/renderer/gfx-vulkan/`，以及直接相关的 GFX 装饰器、管线和资源管理代码。
> 本文合并原 `vulkan-backend-review.md` 与 `vulkan-evolution-improvements.md`，统一源码基线、问题优先级、现代能力选型和待办。
> 证据边界：已做定向源码核对与官方资料核对；未执行构建、Validation、抓帧、性能测量或平台回归。性能收益是待验证假设，不是实测结论。
> 配套文档：[帧调用链](../codebase-guide/frame-flow.html)、[帧时序](../codebase-guide/frame-sequence.html)、[架构](../codebase-guide/architecture.html)。

## 1. 结论与关键修正

当前后端具备完整的传统 Vulkan 渲染链路：VMA 内存分配、持久映射、按帧槽管理的资源、状态脏检查、上层 PSO 复用与驱动管线缓存均已存在。主要工作是完善交换链异常恢复，测量首次编译和同步开销，再按设备能力逐步引入现代路径。

合并前两份文档中以下判断以本表为准：

| 原判断 | 当前源码事实 | 对优化方向的影响 |
|---|---|---|
| 引擎缺少 PSO 哈希缓存 | `PipelineStateManager::psoHashMap` 已按 pass、render pass、顶点属性、shader 和 subpass 生成 key；Vulkan 后端没有额外的管线对象去重缓存 | 先统计现有缓存命中与绕过路径，再判断是否需要新增后端缓存 |
| `vkCreateGraphicsPipelines` 未传 `VkPipelineCache` | 图形与计算管线都传入 `pipelineCache->getHandle()` | 改进现有缓存持久化、预热和首次创建路径，不重复添加已存在的缓存 |
| `present()` 每帧等待所有在途帧 | 先切换 `curBackBufferIndex`，再等待即将复用槽内的 fence；跨槽等待在 `waitAllFences()` | 已有多帧轮转，timeline 迁移不能仅以“消除全帧等待”为理由 |
| indirect draw 只有桩代码 | `VKCommandBuffer.cpp` 已调用 `vkCmdDrawIndirect` / `vkCmdDrawIndexedIndirect` | 接口支持与 GPU 剔除、GPU 生成参数是不同层次 |
| 调试路径仅使用 `debug_marker` | 实例侧请求 `debug_utils`，命令缓冲已有 utils label 与 marker 回退 | 应核对并统一已有调试路径，而不是从零迁移 |
| 每帧 `resize()` 必然产生堆分配，staging 池只增不减 | vector 在容量足够时可复用；staging 池已有 `shrinkSize()` | 需测量容量变化、分配次数和收缩抖动，不直接认定每帧分配或持续增长 |

## 2. 架构与每帧链路

### 2.1 模块边界

| 层级 | 职责 | 代码入口 |
|---|---|---|
| 场景与帧驱动 | 收集相机、更新场景、调用 acquire / render / present | `native/cocos/core/Root.cpp:397-445` |
| 渲染管线 | Legacy Forward/Deferred 与 Custom RenderGraph 的 pass 编排和命令生成 | `native/cocos/renderer/pipeline/` |
| PSO 管理 | 根据渲染状态请求或复用 `gfx::PipelineState` | `native/cocos/renderer/pipeline/PipelineStateManager.cpp:33-68` |
| GFX 抽象与装饰器 | 跨后端接口、设备线程转发和引擎侧校验 | `native/cocos/renderer/GFXDeviceManager.h:144-165`、`gfx-agent/`、`gfx-validator/` |
| Vulkan 对象封装 | Device、Queue、Swapchain、Pipeline、Descriptor、CommandBuffer | `native/cocos/renderer/gfx-vulkan/VK*.cpp` |
| 底层对象与资源池 | VMA 分配、帧槽、命令池、staging、descriptor hub、延迟回收 | `VKGPUObjects.h`、`VKCommands.cpp` |

`DeviceAgent` 在允许分离设备线程时启用，`DeviceValidator` 由构建与开关控制，并非所有运行配置都包含完整装饰器链。引擎侧 Validator 与 `VK_LAYER_KHRONOS_validation` 是不同机制。

### 2.2 API 与资源流

```text
Root::frameMoveProcess
  → Device::acquire
    → vkAcquireNextImageKHR（binary semaphore，获取 swapchain image index）
    → 帧槽 descriptor 更新、按配置准备 image barrier
Root::frameMoveEnd
  → PipelineRuntime::render
    → CommandBuffer::begin / vkBeginCommandBuffer
    → vkCmdBeginRenderPass / vkCmdNextSubpass / vkCmdEndRenderPass
    → 绑定 Pipeline、DescriptorSet、顶点/索引缓冲
    → draw / indirect draw / dispatch
    → Queue::submit
      → 上传与 barrier 命令 + 场景命令 + 后置命令
      → vkQueueSubmit（等待/信号 binary semaphore，提交槽内 fence）
  → Device::present / vkQueuePresentKHR（或配置启用的 Swappy 路径）
    → 切换到下一个 curBackBufferIndex
    → 等待该槽 fence
    → reset fence、清理延迟回收、重置并收缩 staging 池
```

`curBackBufferIndex` 是帧槽索引，与 `curImageIndex` / `vkSwapchainIndices` 的交换链图像索引不同。`backBufferCount` 默认值为 3，交换链创建/重建可以扩展槽数。fence、回收箱和 staging 按槽管理；binary semaphore 使用单独的池，不能据此认为已按帧槽完成全部安全性验证。

### 2.3 需要保留的基础

- **VMA 与持久映射**：buffer/image 使用 VMA；HOST 与 HOST|DEVICE buffer 通过 `VMA_ALLOCATION_CREATE_MAPPED_BIT` 建立映射（`VKCommands.cpp:278-292`）。迁移时仍需保持 flush、对齐和在途访问约束。
- **多帧资源管理**：buffer offset、descriptor 实例、命令池与回收操作依赖当前帧槽；调整同步时必须成套保持这些关系。
- **状态脏检查**：pipeline、descriptor、IA、viewport 等已有 `_cur*` 守卫，descriptor 延迟到 draw 绑定（`VKCommandBuffer.cpp:298-346,764-785`）。
- **已有双层管线复用**：上层 PSO map 与后端 `VkPipelineCache` 的作用不同，前者复用对象，后者复用驱动编译数据。
- **提交内批处理**：`CCVKQueue::submit` 将传入的命令缓冲及 transport hub 命令装入一次提交（`VKQueue.cpp:55-96`）。
- **二级 CB 与 indirect/compute 支持**：已有线程关联的命令池、`vkCmdExecuteCommands`、indirect draw 和 dispatch 接口，应在现有路径上扩展。

### 2.4 架构目标的引擎含义

| 目标 | 引擎内对应工作 |
|---|---|
| 高可用性 | 交换链/Surface 异常恢复、安全回收、缓存容错；device lost 恢复需单独设计和验证 |
| 负载均衡 | 根据 CPU/GPU 瓶颈评估并行命令录制、上传调度和 async compute |
| 服务解耦 | 保持 `gfx-base` 接口稳定，隔离能力协商、render-pass 策略与后端实现 |
| 可扩展架构 | Legacy 与 RenderGraph 共存，现代路径按能力启用并保留兼容实现 |

这些目标对应单进程引擎子系统，不需要引入分布式微服务。

## 3. 当前能力与兼容性基线

| 项目 | 当前状态 | 位置 / 限定 |
|---|---|---|
| API 版本探测 | loader 支持版本用于 instance；物理设备版本另行查询，XR 可调整 instance 请求 | `VKGPUContext.cpp:114-129,319-323`；不能把 loader 版本等同于设备已启用能力 |
| 设备扩展 | 请求集合较小，包括 swapchain、fragment shading rate、调试及旧版本兼容扩展；Swappy 可追加请求 | `VKDevice.cpp:120-200`；请求后仍会过滤不支持项 |
| feature 开启 | 按支持情况请求部分基础 feature；Vulkan12 结构主要设置 `separateDepthStencilLayouts` | `VKDevice.cpp:138-156,229-236` |
| 管线编译 | GLSL 运行时编译为 SPIR-V，同步创建 shader module / pipeline | `VKCommands.cpp:870-884,963-975,1188-1193` |
| 管线缓存 | 已创建和使用 `VkPipelineCache`；磁盘读写受 `CC_USE_PIPELINE_CACHE` 控制 | `VKPipelineCache.cpp:70-119` |
| 帧同步 | binary semaphore + 按帧槽 fence，槽复用前等待 | `VKDevice.cpp:712-745`、`VKQueue.cpp:80-98` |
| RenderPass / Descriptor | 传统 render pass、framebuffer、descriptor set / pool / layout | `VKCommands.cpp`、`VKCommandBuffer.cpp` |
| 上传与同步 | staging + transport hub + barrier manager，使用旧式 `vkCmdPipelineBarrier` 和 thsvs 抽象 | `VKCommands.cpp:1612-1691` |
| 并行与 GPU 驱动 | 二级 CB、indirect draw、compute 接口已有；本次核对未确认主场景已使用并行录制或 GPU 剔除 | `VKCommandBuffer.cpp:199,464-500,538-558,798-801` |
| 现代路径 | 未发现 dynamic rendering、timeline、graphics pipeline library、descriptor heap 的实际接入 | 后端调用与设备请求的定向检索；volk 中函数声明/加载不等于已使用 |
| 调试 | 已有 `debug_utils` 与 marker 路径；Vulkan Validation 默认被 `FORCE_DISABLE_VALIDATION = 1` 禁用 | `VKGPUContext.cpp:39-40`、`VKCommandBuffer.cpp:250-293` |

VMA 的编译配置不代表应用已开启相应 Vulkan feature；API 版本、扩展、feature、入口函数和设备限制需要联合判断。

## 4. 问题优先级与验证依据

P0 表示优先处理的正确性问题；P1 表示优先测量和改进的路径；P2 表示按 profile 决定的增量优化。下面列出的是代码事实及风险，不代表已确认性能瓶颈。

| 优先级 | 问题 / 候选 | 源码锚点 | 判断与最小验证 |
|---|---|---|---|
| **P0** | acquire 异常与交换链重建触发未闭环 | `VKDevice.cpp:623-654,706-708`、`VKSwapchain.cpp:312-335` | acquire 仅断言成功或 SUBOPTIMAL，未处理 OUT_OF_DATE；复现 resize、最小化和 Surface 重建，检查失败后是否仍使用 image index / 等待未信号 semaphore |
| **P1** | 首次 shader / pipeline 编译尖峰 | `VKCommands.cpp:870-884,974,1192`、`pipeline/PipelineStateManager.cpp:33-68` | 同步创建仍在；统计 PSO 命中、GLSL 编译次数和创建耗时，比较冷/热缓存，确定预热、去重或后台编译收益 |
| **P1** | 管线缓存持久化可靠性 | `VKPipelineCache.cpp:70-77,98-119` | 后端调用点仅在析构，启用磁盘缓存时异常退出会丢失尚未保存数据；验证截断文件、异常退出和驱动变更 |
| **P1** | 上传屏障范围与 stage 合并 | `VKCommands.cpp:1612-1687`、`VKCommandBuffer.cpp:145-153,232-237` | buffer 上传访问合并为全局 barrier；pass 前后屏障受宏和自动 barrier 分支控制；用同步校验和 GPU trace 判断等待范围 |
| **P1** | layout 对象复用与 descriptor 池粒度 | `VKPipelineLayout.cpp:41-56`、`VKDescriptorSetLayout.cpp:40-57`、`VKGPUObjects.h:592-681` | 后端创建路径未见签名去重，默认每池 10 sets；先统计重复布局、池数量和分配耗时 |
| **P1** | 单设备线程录制开销 | `GFXDeviceManager.h:144-165`、`VKCommandBuffer.cpp:199,538-558` | 二级 CB 支持存在；先测录制耗时与任务粒度，评估逐 pass 并行录制 |
| **P2** | 热路径容器与 descriptor 关联维护 | `VKDevice.cpp:619-639`、`VKGPUObjects.h:1101-1108` | vector 有容量复用；`unordered_set` 是资源与 set 的关联集合，不能直接等同于脏位表；按分配和查找 profile 决定容器方案 |
| **P2** | 命令池预热 | `VKGPUObjects.h:713-750` | 空池按 1 个 CB 分配；记录冷启动分配尖峰，按预期用量批量预热 |
| **P2** | 跨提交合并 | `VKQueue.cpp:55-96`、`pipeline/forward/ForwardPipeline.cpp:142` | 单次 submit 已批处理；是否有跨 camera 多次 submit 取决于实际管线，先统计每帧提交次数与依赖 |
| **P2** | 动态 offset 重绑范围 | `VKCommandBuffer.cpp:313-318,764-785` | 按 `_firstDirty` 后续集合绑定；统计频次后评估更精确范围，保持 layout 兼容性与 offset 顺序 |
| **P2** | staging 扫描、增长与收缩 | `VKGPUObjects.h:805-855`、`VKDevice.cpp:722-723` | 分配线性扫描块，存在 `shrinkSize()`；测上传峰值、重新分配频率和预算，评估游标与保留水位 |
| **P2** | 小动态 UBO 对齐空间 | `VKCommands.cpp:283-289` | 填充取决于设备对齐限制与帧槽数，不能固定按 256B；统计碎片后评估 ring 打包 |
| **P2** | 上传纹理屏障重复候选 | `VKCommands.cpp:1325-1333,1660-1687` | 两处屏障可能承担不同依赖；核对资源范围、访问和先后顺序后再去重 |

### 4.1 交换链恢复

`checkSwapchainStatus()` 已有尺寸变化、零尺寸暂停与重建逻辑。当尺寸相同且 `lastPresentResult == VK_SUCCESS` 时提前返回；这不等于 acquire / present 的所有异常都能主动触发检查。

建议让 acquire、present 和平台 Surface 事件进入统一的“可渲染 / 暂停 / 待重建”状态流程。acquire 失败时跳过该交换链的渲染和等待，避免继续使用无效图像；SUBOPTIMAL 可以按策略继续当前帧并安排重建。重建仍需保证旧资源与在途工作安全，包括依赖尺寸的 texture、view 和 framebuffer。

### 4.2 编译、缓存与布局

先保留上层 PSO map 和驱动缓存，测量命中率以及 key/失效行为。按证据选择 shader 编译结果去重、离线 SPIR-V、PSO 预热或后端对象共享。共享 pipeline / layout 时需明确引用计数、销毁责任和设备归属，后台编译还需协调缓存访问与任务生命周期。

缓存持久化可按新增管线数量、时间或退出事件保存完整快照，采用临时文件与原子替换，处理损坏数据和设备/驱动不兼容。`vkGetPipelineCacheData` 返回 blob，定期保存不能称为 API 提供的“增量落盘”。

### 4.3 同步与并行

屏障优化先识别 producer、consumer、src/dst stage/access，以及 image layout 和 subresource。可以评估按资源范围、stage 组批或 RenderGraph 推导，但独立 buffer barrier 不保证优于全局 barrier；仅换成 synchronization2 也不会自动收窄依赖。

当前已有帧槽轮转。timeline 的候选收益是统一提交完成进度与回收条件，迁移需覆盖信号值分配、等待、错误路径和资源池；WSI acquire / present 保留 binary semaphore。需要等待槽复用时，即使使用 timeline 仍可能阻塞。

并行录制与 async compute 分开评估：前者解决 CPU 录制瓶颈；后者需要可重叠 GPU 工作、多队列同步和资源所有权处理。二级 CB 要明确 inheritance、线程独立池与提交前任务完成关系；小任务拆分可能增加调度和执行成本。

## 5. Vulkan 演进基线与能力选型

### 5.1 版本与扩展事实

| 基线 | 与本后端相关的能力 | 限定 |
|---|---|---|
| Vulkan 1.2（2020） | timeline semaphore、descriptor indexing、buffer device address、scalar block layout | 纳入核心不代表应用已启用每个 feature |
| Vulkan 1.3（2022） | dynamic rendering、synchronization2、pipeline creation cache control | 使用对应 feature 与实际入口，保留旧设备路径 |
| Vulkan 1.4（2024-12） | dynamic rendering local read、push descriptors、pipeline robustness、maintenance6、host image copy | `hostImageCopy` 是可选 feature；不将 1.2/1.3 能力重新描述为首次在 1.4 核心化 |
| Roadmap 2026（2026-01-23） | 提高 VRS、shader clock、host image copy、compute derivatives、presentation 能力与描述符/接口限制的要求 | 面向符合该 milestone 的实现，不能推断所有设备均支持 |
| graphics pipeline library | `VK_EXT_graphics_pipeline_library` 将图形管线拆分为四个库并链接 | 仍需显式扩展、feature 与依赖协商；不是 1.4 核心，也不是仅改一次创建调用 |
| descriptor heap | `VK_EXT_descriptor_heap` 提供新的描述符内存模型，`VK_EXT_descriptor_buffer` 已被其弃用 | 与 Roadmap 2026 同期发布不等于 milestone 强制要求 heap；需检查目标驱动与工具支持 |

版本归属依据：[Vulkan Versions & Porting Guide](https://docs.vulkan.org/guide/latest/versions.html)、[Core Revisions](https://docs.vulkan.org/spec/latest/appendices/versions.html)。Roadmap 依据：[官方发布说明](https://www.khronos.org/blog/vulkan-introduces-roadmap-2026-and-new-descriptor-heap-extension)、[Roadmap Milestones](https://github.khronos.org/Vulkan-Site/spec/latest/appendices/roadmap.html)。扩展依据：[graphics pipeline library](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_graphics_pipeline_library.html)、[descriptor buffer 弃用状态](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_descriptor_buffer.html)。

### 5.2 现代能力与项目需求的对应

下表是技术候选，不以扩展新旧替代性能证据。现有实现为默认兼容路径。

| 候选能力 | 目标与适用条件 | 接入范围 / 重新评估条件 |
|---|---|---|
| graphics pipeline library | 已确认管线变体编译是 CPU 卡顿来源，且设备支持 | 拆分 vertex input、pre-rasterization、fragment shader、fragment output，管理链接与缓存；若驱动编译/链接收益不足则保留单体路径 |
| timeline semaphore | 提交与回收复杂度增长，需要统一完成进度 | Device feature、Queue 信号值、等待与回收池一起迁移；无法减少复杂度或收益不足时继续用 fence |
| synchronization2 | 需要更清晰地表达 barrier 与提交依赖 | 调整同步抽象与 `vkCmdPipelineBarrier2` 等调用；若 scope 不变，不承诺性能改善 |
| dynamic rendering + local read | 需要简化 render-pass 对象管理，目标设备满足能力 | 评估 `ClassicRenderPass` / `DynamicRendering` 策略；映射 subpass/input attachment、resolve 和 PSO 兼容性；移动端带宽或 tile 行为回退时保留经典路径 |
| push descriptors | 少量、频繁变化的绑定存在可测 descriptor 开销 | 核对支持类型、数量与现有动态 UBO 语义；并非直接替换所有 descriptor set |
| host image copy | 上传的 CPU/GPU 调度成本高，且设备/格式/layout 支持 | 评估 HOST_TRANSFER usage、主机拷贝与同步；CPU 成本或吞吐变差时回退 staging |
| pipeline robustness | 需要明确控制支持范围内的越界访问行为 | 不能把未合法绑定的 descriptor 自动变成合法对象；与 binding 校验、null descriptor 等能力分开处理 |
| descriptor indexing / descriptor heap | 资源绑定规模要求 bindless，且目标设备能力明确 | indexing 可作为 descriptor set 路径；heap 需新的分配、写入、shader 映射与在途生命周期；不强制所有平台直接上 heap |
| present ID / wait、Swappy | 需要测量或控制呈现进度和延迟 | 评估 `present_id` / `present_wait` 及支持时的 `present_id2` / `present_wait2`，与 Swappy 配置联合验证；避免重复 pacing |
| calibrated timestamps、shader clock | 需要 CPU/GPU 时域关联和 shader 内部计时 | calibrated timestamps 用于时域校准，query pool 仍用于 GPU 区间计时；不能笼统称为“替代 query 且更准” |
| async compute / transfer 调度 | workload 存在可重叠工作，队列能力允许 | 提交依赖、ownership、带宽与并行资源预算一起评估；额外同步超过重叠收益时保留同队列 |
| device generated commands（DGC） | GPU 剔除/命令生成已成熟，CPU 提交仍是瓶颈 | 检查 EXT feature 与依赖；不要求必须有独立 async compute 队列，也不替代所有 indirect draw |
| mesh shading / ray tracing | 产品需要相关几何能力或特效，设备覆盖满足要求 | 新 shader、资源、管线与工具链；保留传统几何/光栅路径 |
| subpass merge feedback | 移动端需要观测 subpass 合并效果 | 该扩展提供合并反馈，不会自行完成 subpass 重组；结合反馈与带宽实测调整经典路径 |

### 5.3 能力层与兼容策略

建议形成统一的 `GfxVKCapabilities` 能力视图（拟新增，当前并不存在），收敛版本、扩展、feature、限制与入口函数判断。协商顺序为：

```text
查询 instance / physical device 版本
→ 枚举扩展并查询相关 feature / properties
→ 选择可用策略，满足依赖后请求 feature 与扩展
→ 创建设备并加载所需入口
→ 记录最终启用能力，上层使用同一视图
```

维持现有兼容范围，不能未经目标设备评估就统一要求 Vulkan 1.4，也不能以“设备报告高版本”为依据直接启用全部功能。Header、volk、shader 编译工具链的支持情况需与运行期能力联合核对。

调试方面先利用已有 `debug_utils` 命令标签与 marker 回退，核对对象命名和开关一致性。启用 Vulkan Validation 时需检查 `VKGPUContext.cpp` 的禁用宏；不能将引擎侧 `DeviceValidator` 当作 Vulkan Validation clean 的证据。

## 6. 实施顺序与 3–5 年路线

### 6.1 近期顺序

1. **正确性闭环**：处理 acquire / present / Surface 状态与异常重建，验证资源和同步对象复用。
2. **建立性能基线**：测 shader/PSO 编译、缓存命中、CPU 录制、GPU barrier、提交与帧槽等待，区分 CPU / GPU / bandwidth / synchronization 瓶颈。
3. **改进现有路径**：根据基线实施缓存安全保存与预热、编译去重、屏障调整、布局/池优化及热路径清理。
4. **独立评估现代路径**：按需求接入同步、管线库、并行录制或 dynamic rendering，每项具备能力判断、回退与对比测量。
5. **GPU 驱动扩展**：先利用现有 indirect/compute 接口实现剔除与参数生成，再判断是否需要 DGC、mesh 或 RT。

### 6.2 长期候选路线

阶段相对获批实施之日起计算，表示依赖顺序与研究方向，不是交付承诺。

| 阶段 | 主题 | 关键工作 | 进入下一阶段的依据 |
|---|---|---|---|
| 第 1 年（0–12 个月） | 正确性与性能基线 | 交换链恢复、同步/生命周期验证、缓存可靠性、编译预热、屏障与资源池改进 | 目标设备运行稳定，瓶颈与收益有数据 |
| 第 2 年 | 并行录制与现代路径 | 二级 CB、timeline/sync2、dynamic rendering 策略、bindless、GPU culling；按 workload 评估 async compute | 新旧路径能回退，CPU/GPU/带宽指标满足目标 |
| 第 3 年 | 现代几何与光线 | buffer device address、meshlet/cluster、DGC、mesh shading、ray tracing 的产品化评估 | 设备覆盖、内容生产和工具链可支撑 |
| 第 4–5 年 | GPU 驱动与跨后端演进 | 虚拟几何、超分接口（FSR/DLSS 等）、自适应分辨率/画质、跨 WebGPU 抽象协调；按产品需要研究多 GPU/异构 | 产品需求与维护收益足以承担复杂度 |

## 7. 待办与验收

### P0：正确性

- [ ] acquire 的 OUT_OF_DATE、Surface 失效和其他失败返回不再继续消费图像索引/等待未信号 semaphore；present 与平台事件能触发统一恢复。
- [ ] 重建覆盖零尺寸、最小化恢复、resize、Android pause/resume 和 Surface 重建；相关 texture/view/framebuffer 同步更新。
- [ ] 定向验证 fence、binary semaphore、command pool、descriptor、staging 与延迟回收的在途生命周期。

### P1：基线与现有实现改进

- [ ] 记录 PSO cache 命中、shader 编译次数、pipeline 创建耗时、帧槽等待、录制与 submit 次数；完成冷/热启动对比。
- [ ] 确认 `CC_USE_PIPELINE_CACHE` 配置，验证缓存损坏/驱动变更场景，评估周期快照、原子保存与预热。
- [ ] 根据编译开销决定 shader 去重、离线 SPIR-V、后台编译或后端对象共享，明确失效和销毁责任。
- [ ] 在启用的 barrier 分支下做同步校验和 GPU trace，按依赖范围调整；确认视觉正确且无性能回退。
- [ ] 统计 layout 重复、descriptor 池大小和录制负载，按证据决定缓存、池增长与二级 CB 拆分。

### P2：增量优化与现代能力评估

- [ ] 按 profile 评估容器、命令池预热、跨提交合并、动态 offset 重绑、staging 水位、小 UBO 打包及屏障去重。
- [ ] 建立统一能力视图，检查 Vulkan 版本、Header/volk、feature、扩展依赖与入口函数。
- [ ] 为 pipeline library、timeline/sync2、dynamic rendering/local read 分别设计完整对象/同步路径、回退与验收；不只追加 feature bit。
- [ ] 评估 push descriptors、host image copy、robustness、descriptor indexing/heap、present pacing 与计时，按设备和 workload 选择。
- [ ] 核对已有 `debug_utils`、marker 回退、对象命名与 Validation 开关。

### P3：长期路线

- [ ] 利用现有 indirect/compute 建立 GPU 剔除与参数生成，再评估 DGC、独立队列、mesh shading、ray tracing。
- [ ] 移动端记录 subpass merge feedback 与带宽，比较经典 subpass 和现代路径。
- [ ] 根据产品需要评估虚拟几何、超分、跨后端抽象、自适应画质与多 GPU。

每项验收先满足功能、同步与生命周期正确性，再比较 CPU/GPU 帧耗时、尖峰、缓存命中与内存占用。新增现代路径必须同时测试可用与回退场景，不能仅以“Validation 无报错”判定完成。

## 8. 代码坐标与验证状态

除明确写出完整路径外，下表文件位于 `native/cocos/renderer/gfx-vulkan/`。行号对应本次核对的工作区，后续改动后以符号和调用关系重新定位。

| 主题 | 代码坐标 |
|---|---|
| 上层 PSO 缓存 | `native/cocos/renderer/pipeline/PipelineStateManager.cpp:33-68` |
| 后端 pipeline 与驱动缓存 | `VKPipelineState.cpp:43-67`、`VKCommands.cpp:971-975,1189-1193` |
| 缓存初始化/持久化 | `VKDevice.cpp:464-465`、`VKPipelineCache.cpp:70-119` |
| 帧槽轮转与跨槽等待 | `VKDevice.cpp:712-745`、`VKGPUObjects.h:452-453` |
| acquire/present 与重建 | `VKDevice.cpp:623-654,706-708`、`VKSwapchain.cpp:268-355` |
| shader 编译 | `VKShader.cpp:58-63`、`VKCommands.cpp:870-884` |
| 上传/pass 同步 | `VKCommands.cpp:1612-1691`、`VKCommandBuffer.cpp:145-153,232-237` |
| descriptor/layout 创建与池 | `VKCommands.cpp:903,952`、`VKGPUObjects.h:592-681` |
| 命令池与二级 CB | `VKCommands.cpp:52`、`VKGPUObjects.h:713-750`、`VKCommandBuffer.cpp:199,538-558` |
| 提交、indirect 与 compute | `VKQueue.cpp:55-98`、`VKCommandBuffer.cpp:464-500,798-801` |
| staging 与 descriptor 关联 | `VKGPUObjects.h:805-855,1101-1108` |
| feature/扩展、调试开关 | `VKDevice.cpp:120-236`、`VKGPUContext.cpp:39-40,114-129,202-210`、`VKCommandBuffer.cpp:250-293` |

本次文档整理的 Verification Gate 状态：

| 关卡 | 状态 |
|---|---|
| G1 API 合法性 | 未完整验证；核对了版本归属与部分扩展依据，未逐项检查实际调用的全部 VUID |
| G2 生命周期 | 未完整验证；核对帧槽与重建代码，未执行销毁、重建和在途复用测试 |
| G3 同步 | 未完整验证；核对 fence/barrier/semaphore 路径，未证明全部资源依赖正确 |
| G4 Validation | 未验证；未运行 Vulkan Validation / synchronization validation |
| G5 RenderDoc / AGI | 未验证；未抓帧、测 GPU counter 或 trace |
| G6 平台回归 | 未验证；未运行桌面或 Android 场景 |

本文是审查与实施候选清单，不宣称已修复问题或已获得性能收益。
