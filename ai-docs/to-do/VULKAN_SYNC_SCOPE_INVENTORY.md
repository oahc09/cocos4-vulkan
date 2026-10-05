# Vulkan 同步范围现状清单（M4-B0 rev2，只读）

日期：2026-10-04（Orca Worker）。rev2 按 Lead 复核修正：completion 引用、WSI per-submit 路径、虚构依赖声明、函数名。全部为当前源码实读；零构建/零测试；Gate 前不实施缩窄。
官方规范引用（primary 源）：
- Vulkan spec synchronization 章节：https://docs.vulkan.org/spec/latest/chapters/synchronization.html —— submission order 本身不定义执行/内存依赖。
- VkSubmitInfo：https://docs.vulkan.org/refpages/latest/refpages/source/VkSubmitInfo.html —— wait/signal semaphore 语义。
- synchronization examples：https://docs.vulkan.org/guide/latest/synchronization_examples.html —— 常见依赖建模示例。

## 1. Signal / fence lifetime（提交完成语义；计划 §11 豁免 ALL_COMMANDS 的对象是"signal stage"）

| 位置 | 函数/机制 | 事实 |
|---|---|---|
| `VKCompletionTracker.h:114`（`append`/completedBatch 构造） | timeline signal 追加 | `completedBatch.signals.push_back({timeline_, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, next, true})` —— 这是真正的 whole-submission completion signal，属 §11 豁免；fence 分支（非 timeline）整批完成等价语义 |
| `VKSubmitBatch.h:91`（Submit2 signals 编码） | signals2_ 生成 | 每个 signal `stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT`，与 tracker 语义一致 |
| `VKQueue.cpp:129`（`submitTracked`） | 完成点登记/资源退休入口 | fence/timeline 双模，M3/M4-A 生命周期的唯一权威 |

以上 signal/fence lifetime 边界有意宽，不作为 M4-B 缩窄对象；但 Queue 层的 wait stage（下节）不因此自动豁免。

## 2. Semaphore waits（submit wait stage）

| 位置 | 函数/数据源 | 事实 |
|---|---|---|
| `VKQueue.cpp:117`（`submit` 内 resize） | `_gpuQueue->submitStageMasks.resize(waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)` | 这是 submit 的 WAIT stage，不是 completion signal。实际来源：`VKDevice.cpp:1110` acquire 成功后追加 acquire semaphore；`VKQueue.cpp` submit 成功后以 image-owned signals 替换整个 `lastSignaledSemaphores`。acquire/present/abort 也清理或移除成员。两类 producer 已能从控制流区分；如何保留分类并安全选择更窄 consumer stage 尚未设计/验证，不自动适用 §11 豁免。 |
| `VKSubmitBatch.h:85-89`（waits2_） | Submit2 wait 编码 | stageMask 透传 wait.stages |

## 3. Memory dependencies（生产屏障/全局 barrier 实读）

| 位置 | 函数 | producer/consumer 现有来源 |
|---|---|---|
| `VKDevice.cpp:1032-1054`（`prepareWSISubmit`） | per-submit WSI transitions（非 acquire、非 swapchain 创建）：restore=acquireBarrier（ALL_COMMANDS→ALL_COMMANDS）+ finish=presentBarrier（COLOR_ATTACHMENT_OUTPUT→BOTTOM_OF_PIPE），仅 color；`wsiTransitionsQueued` 防重 | producer/consumer 语义由 acquireBarrier/presentBarrier 常量给定（引擎 WSI 契约），非 per-image tracked；color attach→present 方向已窄，restore 方向宽 |
| `VKCommandBuffer.cpp:442-447`（Dynamic beginRenderPass 前 clear-discard transition） | `_dynamicResolveTransition.build` 输入 `VKRenderingTransitionContract::Input` | before=UNDEFINED+ALL_COMMANDS+MEM_RW（discard 语义，注释声明非缩窄）；during=COLOR_ATTACHMENT_OUTPUT+COLOR_RW；after=ALL_COMMANDS+MEM_RW（renderTarget 后续消费未分类） |
| `VKCommands.cpp:1453`（`cmdFuncCCVKCreateGeneralBarrier`） | GPUGeneralBarrier 元数据 thsvs 展开 | 上层 `GPUGeneralBarrierInfo` 给定 access/layout（M5 契约核对过部分字段） |
| `VKCommands.cpp:1874` | 单 image barrier 提交 | mask 来自上游 thsvs 计算结果 |
| `VKCommands.cpp:1889-1965`（`CCVKGPUBarrierManager::update`） | `ThsvsGlobalBarrier` 合并 transfer→render | producer=`gpuBuffer->transferAccess`（收集后清为 NONE）；consumer=`gpuBuffer->renderAccessTypes`，由 THSVS 展开实际 masks；image 分支读取各 texture 的 transfer/render access。 |
| `VKCommands.cpp:1965` | 同族多 image barrier 批量 | 同上 |
| `VKCommandBuffer.cpp:222-230` | 非 auto-barrier 分支 transfer 写→VS/FS/顶点读（窄 RAW guard） | 已窄；auto 分支走 subpass dependencies |
| `VKCommandBuffer.cpp:503-518` | Dynamic pre/resolvePre/dsPre（M6-A4/A5 编码） | mask 由 `VKBarrierEncoding`/transition contract 输入计算 |
| `VKCommandBuffer.cpp:535-550` | classic subpass dependencies 回放 | 上层 `renderPass->getDependencies()` 给定 |
| `VKSwapchain.cpp:494-524`（`doCreateSwapchain` 内，非每帧路径） | 新 swapchain 链 depth image 初始 barrier→immediate submit；仅 depthStencil 参与该 loop；color 的 `currentAccessTypes.assign(PRESENT)` 是状态赋值，不是 layout transition | 初始化期一次性；与 per-submit 无关（修正初稿误读） |

## 4. 未调查项（只声明未跟踪，不作存在/不存在论断）

- event/split-event 生产使用链：实现主要在 `VKCommandBuffer.cpp`（M3-L 系列修复）；本次未对 event 全调用链做生产范围枚举——未调查。
- descriptor COW 保护 binding snapshot 寿命（M3-M 系列），resource byte-level dependencies 是另一层，当前无追踪——未调查。
- compute→compute / compute→transfer 的执行依赖：submission order 本身不定义依赖（见 spec 链接）；当前未做依赖链追踪——未调查。不推定"隐式保证"或"存在缺口"。
- acquire vs 同帧 signal 的来源已定位；分类随 batch 保存、与 early/user/late CB consumer stage 的对应关系未设计/验证。

## 5. 候选（仅 inference；Gate 前不动生产）

1. `prepareWSISubmit` restore 方向（ALL_COMMANDS→ALL_COMMANDS）：若 acquire producer 可分类（acquire semaphore + queue-owned 假设成立），可收窄；缺失：acquire 侧 producer/access 记录。
2. clear-discard `before`（VKCommandBuffer.cpp:442）：需要 renderTarget lastAccess 记录（transferAccess 先例在 VKCommands.cpp:1908 附近）；缺失：attachment 上一使用追踪。
3. transition `after`（447 行）：按 subpass 后实际 consumer（shader read/present/copy）细分；缺失：attachment 后续使用分类。

## 6. 现有 harness 覆盖映射

WSI（wsi_smoke/wsi_lifecycle_smoke S1~S8）、completion/retirement（recording_lifetime_smoke+verify-completion-benchmark）、dynamic/resolve（dynamic_rendering/msaa_resolve/depth_resolve/dynamic_depth_stencil smokes）、descriptor（descriptor_lifetime_smoke）、staging（staging_lifetime_smoke）、event（event_lifetime_smoke）。注：生产 Hybrid eligibility 排除 WSI 路径，WSI finite 与 offscreen Dynamic 是两证据族。

## 7. Gate 前只读结论

conservative broad（wait stage/restore/transition 前后段）不因 §11 自动豁免，也非本块可动；上述候选按序最小实验（harness 内验证，不改生产）作为 M4-A PASS+平台 Gate 后的 M4-B 实施入口。
