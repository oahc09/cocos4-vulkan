# Vulkan 后端演进吸收版改进建议

> 版本：演进吸收版（Evolution-Absorbed）
> 日期：2026-07-24
> 定位：在 `vulkan-backend-review.md`（P0/P1/P2 性能审查）基础上，吸收 2020–2026 年 Vulkan 演进，给出更贴合当前生态、带代码锚点的分层改进建议。
> 关联文档：[vulkan-backend-review.md](./vulkan-backend-review.md)

---

## 0. 结论先行

近五年（1.3 → 1.4 → Roadmap 2026）Vulkan 的演进，实质改写了三件事的优化打法：

1. **PSO 编译卡顿** 的现代正解从「加缓存」升级为「`VK_EXT_graphics_pipeline_library` 把编译拆成可链接阶段」（2022）。
2. **描述符体系正在整体换血**：`VK_EXT_descriptor_heap`（Roadmap 2026）意图完全取代描述符集，而 `VK_EXT_descriptor_buffer` **已于 2026 被官方废弃**。
3. **`dynamic_rendering` + `local_read`（1.4 核心）让 render-pass 对象可以彻底退役**，包括 input-attachment 类效果。

叠加本后端已核实的代码缺口，给出 P0→P3 分层建议。关键前提：**引擎已在设备创建时协商到平台最高版本**（RTX 4060 上即 1.4），所以动作是「开 feature bit + 加扩展请求」（`VKDevice.cpp:122-136 / :149 / :233-236`），而非修改版本协商逻辑。

---

## 1. 演进事实基线（已逐条 WebSearch 核实，2026-07）

| 里程碑 | 时间 | 关键内容（与本文相关） |
|---|---|---|
| Vulkan 1.3 | 2022 | `dynamic_rendering`、`synchronization2`、`descriptor indexing`、`buffer_device_address` 纳入核心 |
| **Vulkan 1.4** | **2024-12** | 核心纳入：`dynamic_rendering_local_read`、`synchronization2`、`push descriptors`、`scalar block layouts`、`host image copy`（可选）、`pipeline robustness`（原 `VK_EXT_pipeline_robustness`）、`maintenance6` |
| **Roadmap 2026** | **2026-01-23** | 强制 VRS（引擎已用 `VK_KHR_fragment_shading_rate`）、shader clock、host image copy、compute shader derivatives、swapchain 改进、更高描述符/接口上限；引入 **`VK_EXT_descriptor_heap`** 全新描述符体系 |
| 其他 | — | **Vulkan 1.5 截至 2026-07 不存在**；`VK_EXT_device_generated_commands`（DGC）仍为 EXT（2024-02，需 1.1 + buffer_device_address） |

**两个易踩的坑（已核实）：**
- `VK_EXT_graphics_pipeline_library` **不是 1.4 核心**，仍是 EXT（2022），需显式开启。
- `VK_EXT_descriptor_buffer` **已被 `VK_EXT_descriptor_heap` 废弃**（Khronos registry 标注 "Deprecated by VK_EXT_descriptor_heap"）——bindless 应直接建在 heap 上，不要再走 buffer。

**弃用趋势（deprecation 对齐）：** Khronos 现主动标记废弃：`debug_marker → debug_utils`、`descriptor_buffer → descriptor_heap`。跟随弃用清单即未来-proof。

---

## 2. 现有代码缺口基线（已 grep 核实）

| 现象 | 代码位置 |
|---|---|
| `requestedExtensions` 刻意最小：「only enable the absolute essentials」，仅 swapchain + fragment_shading_rate + 1.1 前兼容补丁 | `VKDevice.cpp:120-136` |
| `requestedVulkan12Features` 仅设 `separateDepthStencilLayouts`，timeline/dynamic_rendering 等 feature 全未开 | `VKDevice.cpp:149` |
| Vulkan11/12 feature 结构体仅在 `minorVersion >= 2` 时链式挂载 | `VKDevice.cpp:233-236` |
| 帧末 `vkWaitForFences` 阻塞整帧 | `VKDevice.cpp:716-717 / :745` |
| 调试构建用旧 `VK_EXT_debug_marker` | `VKDevice.cpp:127` |
| `vkCreateGraphicsPipelines` 无 `VkPipelineCache` | `VKPipelineState.cpp:43-67` → `VKCommands.cpp` |
| VMA 已按 1.2 配置（比后端扩展姿态更现代） | `vk_mem_alloc.h:165` |
| 已自带 barrier 抽象库 | `thsvs_simpler_vulkan_synchronization.h` |
| API 版本协商向上到平台最大（fallback 1.0） | `VKGPUContext.cpp:114-120` |
| 已启用 Swappy 帧节奏 | `CC_SWAPPY_ENABLED` |

---

## 3. 演进能力 × 代码缺口 映射表

| 现代能力（来源） | 解决的代码缺口 | 优先级 |
|---|---|---|
| `VK_EXT_graphics_pipeline_library` + pipeline cache | `vkCreateGraphicsPipelines` 无 `VkPipelineCache` | 🔴 P0 |
| `VK_KHR_timeline_semaphore`（1.2 核心） | 帧末 `vkWaitForFences` 整帧 stall | 🔴 P0 |
| `dynamic_rendering` + `local_read`（1.4 核心） | 必须预编译 `VkRenderPass`/`VkFramebuffer` | 🟡 P1 |
| `synchronization2`（1.3 核心） | 全局 barrier 过度同步 | 🟡 P1 |
| `push descriptors`（1.4 核心） | 高频小 UBO 的 descriptor-set 抖动 | 🟡 P1 |
| `host image copy`（1.4 可选） | 移动端/UMA 流式上传争用（staging pool） | 🟡 P1 |
| `pipeline robustness`（1.4 核心） | 缺失绑定时易崩 | 🟡 P1 |
| `VK_EXT_descriptor_heap`（Roadmap 2026） | bindless 描述符体系（取代已废弃 descriptor_buffer） | 💭 P2 |
| `VK_KHR_present_wait` / `present_id` | 帧节奏/延迟（整合 Swappy） | 💭 P2 |
| `VK_KHR_calibrated_timestamps` + shader clock | Profiler 时间戳对齐 | 💭 P2 |
| `VK_EXT_device_generated_commands`（EXT） | GPU 生成命令、GPU 剔除 | 💭 P3 |
| `mesh shading` / `ray tracing` | 现代几何管线 / 特效展示 | 💭 P3 |
| `VK_EXT_subpass_merge_feedback` | 移动端瓦片 GPU subpass 自动合并 | 💭 P3 |

---

## 4. 分层改进建议

### 🔴 P0 — 立即，低风险（与版本无关或仅开 feature bit）

**P0a. 用 graphics_pipeline_library 重写 PSO 路径，而非「只加 VkPipelineCache」**
- **动作**：启用 `VK_EXT_graphics_pipeline_library`，将管线拆为 `vertex input / pre-rasterization / fragment shader / fragment output` 四段可独立编译、可缓存、可链接的库；配合 `VK_EXT_pipeline_creation_cache_control`，1.4 上用 `VK_PIPELINE_CACHE_CREATE_READ_ONLY` 支持跨进程合并。
- **落地**：`VKPipelineState.cpp:43-67` 改用 library 式 `vkCreateGraphicsPipelines`。
- **收益**：首帧卡顿被拆散、可后台/并行编译，比单纯加 cache 更彻底（对齐 `vulkan-backend-review.md` P0）。

**P0b. timeline_semaphore（仅开 feature bit）**
- **动作**：1.2 已核心；在 `requestedVulkan12Features` 追加 `timelineSemaphore = VK_TRUE`（`VKDevice.cpp:149` 附近），帧末改为「只等待最旧在飞帧的信号值」。
- **收益**：直接消灭 `vkWaitForFences` 整帧 stall（`VKDevice.cpp:716-717/745`）。

### 🟡 P1 — 以 Vulkan 1.4 为核心基线（modernization 主体）

- **dynamic_rendering + local_read**：1.4 核心；`local_read` 让 input-attachment 类效果（SSAO/延迟）也脱离 render pass。前置：先把 render-pass 抽象隔离成 `RenderPassStrategy`（`ClassicRenderPass` / `DynamicRendering`）接口，否则无法 drop-in。
- **synchronization2**：用 `VkPipelineBarrier2` 取代旧 barrier；引擎已带 `thsvs_simpler_vulkan_synchronization.h`，接入成本低，消除 review 标记的全局 barrier 过度同步。
- **push descriptors**：每对象小 UBO 更新避免 descriptor-set 反复分配/回收，与现有 `CCVKGPUDescriptorSetHub` 互补。
- **host image copy**（1.4 可选）：UMA/移动端用 CPU 直接拷图，避免与渲染争用传输队列，缓解 `CCVKGPUStagingBufferPool` 上传毛刺。
- **pipeline robustness**（1.4 核心）：缺失绑定不再直接崩，提升健壮性。

### 💭 P2 — Roadmap 2026 / 前瞻（必须 capability-gated）

- **`VK_EXT_descriptor_heap`（取代 descriptor_buffer）**：bindless 直接建在 heap 上，叠在 `CCVKGPUDescriptorSetHub`。**不要再用已废弃的 descriptor_buffer**。
- **`VK_KHR_present_wait` / `present_id`**：精确知道某帧何时上屏，做帧节奏与延迟控制，与已有 Swappy（`CC_SWAPPY_ENABLED`）形成完整 pacing 方案。
- **`VK_KHR_calibrated_timestamps` + Roadmap 2026 shader clock**：GPU/CPU 时间戳对齐，服务 `Profiler::endFrame`，比 `vkGetQueryPoolResults` 更准。

### 💭 P3 — GPU 驱动路线（1.4 + 扩展，长期）

- **`VK_EXT_device_generated_commands`（DGC, EXT）**：GPU 侧生成命令缓冲，做遮挡/视锥剔除，是 indirect draw 的现代继任者；依赖 buffer_device_address + 异步 compute 队列。
- **mesh shading / ray tracing**：现代几何管线与特效（ShadowQuest 类 shader 画廊做技术展示很合适）。
- **`VK_EXT_subpass_merge_feedback`**：移动端瓦片 GPU 自动 subpass 合并，省带宽。

---

## 5. 易忽略点：跟随 deprecation 走

Khronos 现在主动弃用旧 API，对齐即未来-proof：

- **`VK_EXT_debug_marker` → `VK_EXT_debug_utils`**：调试构建当前用前者（`VKDevice.cpp:127`，仅 `CC_DEBUG`）。迁移到 `debug_utils`（实例扩展）支持对象命名、label、range，对 RenderDoc/PIX 调试体验提升明显。
- **`VK_EXT_descriptor_buffer` → `VK_EXT_descriptor_heap`**：bindless 直接上 heap（见 P2）。

---

## 6. 权衡与落地顺序

1. **最小规格风险**：1.4 在部分老移动 GPU 尚未达标（Roadmap 2024 设备 2024+ 才铺开，Roadmap 2026 年底才有 conformant 设备）。保留 1.2 回退路径，**每一项都 capability-gated + 优雅回退**——这正是独立 `GfxVKCapabilities` 能力门控层存在的理由（取代散落的 `minorVersion < 1` 分支）。
2. **建议顺序**：P0（pipeline library+cache、timeline_semaphore）→ P1（1.4 核心五件套）→ P2（Roadmap 2026 前瞻：descriptor_heap、present_wait、calibrated_timestamps）→ P3（DGC、mesh/RT）。每步独立可灰度。
3. **最大杠杆**：P0 两项不依赖版本 bump，今天就能在 1.0 路径上以 EXT 形式落地；其余随目标基线升到 1.4 一次解锁。

---

## 7. 待办清单（可执行）

- [ ] P0a：在 `requestedExtensions`（`VKDevice.cpp:122-136`）加 `VK_EXT_graphics_pipeline_library`；改写 `VKPipelineState.cpp` 用 library + cache。
- [ ] P0b：`requestedVulkan12Features.timelineSemaphore = VK_TRUE`，重写帧末等待（`VKDevice.cpp:716-717/745`）。
- [ ] P1：1.4 核心 feature 开启（dynamic_rendering_local_read、synchronization2、push descriptors、hostImageCopy、pipelineRobustness）+ 隔离 `RenderPassStrategy`。
- [ ] P2：评估并接入 `VK_EXT_descriptor_heap`、`present_wait`、`calibrated_timestamps`（带回退）。
- [ ] 弃用对齐：`debug_marker` → `debug_utils` 迁移。
- [ ] P3：DGC / mesh shading / ray tracing 技术预研（移动端 subpass_merge_feedback）。
