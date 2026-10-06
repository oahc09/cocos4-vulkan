# Vulkan 现代化整体验收记录

日期：2026-10-05。执行渠道：Lead。结论：**整体验收未通过；真实 Windows 工程已测功能范围通过，最终目标尚未完成。**

## 2026-10-06 最新证据增量（不改写历史61条审查）

- 当前任务状态以 [VULKAN_COMPLETION_TASKS.md](VULKAN_COMPLETION_TASKS.md) 为准。以下旧表中的“真实工程cache未接”“Input/Subpass尚缺”“M4-A本机前置未通过”“M4-B未实现”等，是2026-10-05审查时的状态，不能作为2026-10-06仍未实现的结论。
- T09已在真实工程完成opt-in cache/prewarm、冷暖关联与安全拒绝；T13已补各渲染族的本机最小内容/异常/退休合同；T05有真实工程启动/first-use/冷暖、固定预热与正式P50/P95/P99，T06记录本机M4-A有限接受理由。相关报告及真实原始数据链接见任务列表。
- T07首批已知buffer资源化及已知Dynamic post stage/access缩窄完成本机有限验证。六案buffer合同、八案color/depth resolve、原性能超阈值及重复/控制调查、当前Android编译链接，见 [2026-10-06同步范围报告](VULKAN_REAL_PROJECT_SYNC_SCOPE_20261006.md)。未知历史访问保留安全回退；未声明上传密集或全部Dynamic负载性能收益。
- 仍不能关闭最终DoD：Android真实设备/自然低版GPU/KHR与fallback运行、rotation/background/恢复/生命周期及温控；跨设备golden/实际游戏负载性能。Apple/XR按本阶段范围调整暂缓。当前adb列表为空，Windows有限运行和Android链接不代替这些证据。loader/physical/XR低于最低API等端到端边界，不由forced高能力Windows tier或CPU契约扩称自然设备通过。
- 更多CSM/Compute非倍数resize、Bloom实际滑条交互及未纳入最小矩阵的组合，仍不得从现有有限结果推断通过。已知mask层匹配问题按用户指示不处理；Bloom场景侧JS问题与Vulkan有限内容合同分列。
- 结论仍是**整体验收未通过**。这次更新是旧审查与当前证据的状态对齐，不冒称重新执行了61项跨平台验收，也没有修改原计划或降低完成条件。执行渠道Lead；未stage/commit/push。

验收依据是原计划 §8~20、§22 和 §23 的 61 条 DoD，不降低标准，也不把阶段性提交或任务单中的有限调查完成等同全部通过。本次核对代码差异、原始日志、终态、源码/产物哈希和已有 GPU 内容证据；没有新增 GPU 运行、重建、平台测试或生产修复。历史 Orca Worker 的构建/运行归属仍按 PROGRESS 原记录保留。

## 后续功能闭环更新（2026-10-05，Lead）

- 以下1~6节保留先前61条整体验收审查的时间点、源码/HEAD/产物与归属，不当作本次新exe的重新全验收。当前主HEAD a947a4d0，Vulkan生产核心未因T09接线改变；新Debug exe3859b142…cabc9940由实际测试工程三处接线与新增default-off模块重新构建。
- T09功能合同关闭：17终态进程，冷保存137391payload字节与新进程driver input真实关联；六类坏缓存安全拒绝、live checkpoint恢复、wrong-key/正常资源销毁、OFF/参数拒绝、保存失败安全分别取证。共享judge38/38、24静态ROI diff0；保存失败native0不能算保存成功，正例judge正确拒绝。见PROGRESS§2EE与[模块README](../../native/tests/vulkan-real-project/README.md)。
- D54/D60等正式冷暖性能、FirstPresent/FirstPSO/启动测量与平台条件仍未全部满足，不能因为本次cold/restored功能通过就改为整体PASS。先补T13渲染/异常/销毁恢复正确性，再T05正式性能及>3%调查；T06放行后才T07 M4-B。
- 当前15项任务：已完成8、进行中1（T13）、待执行1（T05）、等待前置3、依赖外部条件2。历史表格中T09/T05状态以[最新清单](VULKAN_COMPLETION_TASKS.md)为准；全特性/自然低版/移动/项目支持苹果-XR仍需证据，最终T12等待前置。
- 新root为`native/build/vulkan-modernization/win-real-prewarm-lead/run-20261005-212151/`，本项源码/exe前后不变，所有实际运行Lead。本轮没有stage/commit/push，未以独立smoke替代真实项目。

## 1. 本次证据复核

- 主工程：`E:/AIProMax/github/cocos4-vulkan-test-project/native/engine/win64`；VS 构建树 `E:/AIProMax/github/cocos4-vulkan-test-project/build/windows/proj`；Debug `test-cases`。
- 当前 HEAD 为 `ee54033fc321cd2b04211a08c41219adfe4acaff`，其后五个生产文件修复仍在工作区。因此验收对象是当前工作区与对应 exe，不是声称该 HEAD 已包含全部修复。
- exe SHA256：`870bb4e9645835abfe89a715d5b6b7c935adcfd94a30b2ab96615f08075ecc05`。
- 当前五源文件及 exe 的六个 SHA 与 `window-resource-stress/terminal.json` 全部一致。当前源码的真实工程构建/重链已有记录，哈希一致后复用，不以旧二进制证明新改动。
- 只读重判六配置和两压力进程的原始日志、配置、实际路径、流程与退出：8/8 满足各自有限判据，Error/VUID/SYNC-HAZARD/游戏 JS 异常为 0，唯一设备泄漏行 0 bytes。当前修改五文件的 `git diff --check` exit 0。
- Android 现有当前源影响构建日志末尾 250/251、251/251 为 engine/probe 链接；这是编译/链接证据，本次未运行 Android 二进制。
- §22 生产代码定向检索未发现所列 1.0 feature/property fallback 调用；仅四处 VMA 函数表的 KHR 字段名指向 1.1 core PFN。不是因为后缀就删除 VMA 字段或 vendored 声明，也不把定向检索称全仓库语义证明。
- 文档检查：61 条原文/顺序、G1~G6、相对引用及 14 项状态计数核对通过。默认文档 `git diff --check` exit 2 来自 PROGRESS 既有 CRLF 与 HEAD 的 LF 差异；未整文件格式化。此项与上述生产五文件 check exit 0 分开记录，不能称默认全仓库 whitespace clean。

本次复核根为 `native/build/vulkan-modernization/win-completion-t01-lead/run-20261005-164243788/`（ignored，原始证据留在本机）。

| 证据 | 已证实的范围 | 限制 |
|---|---|---|
| `ds-owned-terminal.json`、六配置各 `debug-output.log/runtime-summary.json/flow-status.json`、`modern-full-roi.json` | effective 1.1/1.2/1.3、feature-disabled fallback、Legacy/Sync2/Submit2 策略；原工程完整场景/窗口流程 | 同一 RTX 4060 Laptop 的 forced tier，不是自然低版本 GPU；policy ON 不代表实际 Dynamic draw |
| atlas/RenderTexture 同尺寸静态图 | 主体 ROI 828000 像素、阈值 0、六配置均 diff 0 | 不是动画全帧或全部 §18 特性 golden；2d3d 仅有限人工画面核查 |
| `window-resource-stress/terminal.json`、legacy-v3/modern-v3 原始日志与十二 cycle 图 | 每进程六轮 RT 重建、12 resize、6 minimize/restore，完整终态与十二 ROI diff 0 | 有限窗口/资源压力，不证明所有 GPU 的 pending lease、present completion、热稳态 |
| RenderDoc 菜单 STORE/stencil 红→绿 | 同 DS 图像跨相机 CLEAR→LOAD 的 STORE 保留；43 stencil draw 与 30 点 0/1 内容核对 | 不把已知 mask 输入过滤当 Vulkan 缺陷或 clipping 已通过 |
| Modern 2d3d/RT 的实际 API、PSO、descriptor 和 attachment 数据 | Barrier2/Submit2 实际消费；所有已查 draw 的 pipeline RP 与活动 RP 匹配 | 实际 BeginRendering=0；FrameGraph 自动屏障门控产生 Classic 回退 |
| RT 首次进入连续 24 帧 | 首次采样早于该帧相机输出，随后消费前帧内容 | API 未定义初始颜色；六点读零不是跨 GPU 零初始化保证，不能关闭所有首帧内容 Gate |

## 2. M0~M7 验收判定

| 阶段 | 本次判定 | 尚未关闭的条件 |
|---|---|---|
| M0 依赖/基线 | 已实现，有 Windows 构建及 SPIR-V 实测、Android 编译链接证据；完整平台 Gate 未齐 | LOCK 覆盖的平台工件与运行；项目支持 XR 时的对应构建/运行 |
| M1 版本/能力 | 已实现，有 CPU/协商与 forced tier 证据；完整拒绝/平台 Gate 未齐 | loader/PFN 缺失、physical <1.1、XR <1.1 的端到端拒绝；自然低版 GPU/KHR 路由 |
| M2 WSI | Windows 已测压力通过；完整 Gate 未齐 | Android rotation/background/Surface recreate；其他 present mode/设备的退休与恢复证据 |
| M3 Completion/退休 | 模型与寿命修复已有有限契约/运行，当前工程重建流程通过 | 更完整的游戏资源及跨平台退休、present binary ownership 证据；不以无泄漏替代提前复用证明 |
| M4-A Sync2/Submit2 | 语义迁移已有有限正确性证据；前置 Gate 未通过 | 当前真实工程性能 non-regression 和 >3% 分析/接受结论；完整拓扑/平台覆盖 |
| M4-B 范围优化 | 待实现，暂不能启动 | 必须先完成 M4-A，不能用已有 WAW 根因修复或 inline 编码优化冒称整个 M4-B 完成 |
| M5 渲染契约 | signature/pipeline/pNext/secondary adapter/transition/attachment 模型已实现并有有限验证 | 完整特性与平台验证仍缺；未验证 secondary/FSR/XR 保持 Classic 符合 §13.7，不要求盲开 Dynamic |
| M6 Hybrid | 已实现白名单/回退，有独立 Dynamic 内容实测；真实选定场景 Classic 回退正常 | 同真实场景实际命中路径的视觉/CPU/GPU 对照；Mobile TBDR/vendor counters 与平台 Gate |
| M7 Cache/预热 | owning keys、持久化、串行 queue/compute 实验已实现；整体验收未齐 | 用户真实工程的 opt-in 冷/暖关联、安全拒绝、创建/销毁及首次使用测量；graphics manifest 扩展/GPL 是可选，不能虚构为硬阻塞 |

## 3. 原 §23 DoD 逐条状态（61/61 已核对）

“实现/有限验证”表示已有实现或局部证据，**不表示所有目标平台最终 PASS**。“待验收”表示证据未覆盖或当前真实工程未完成；“待实现”表示代码工作确实尚未做。“条件未触发”不算遗漏，不能借此缩窄 completion signal。

| ID | 原 DoD 条目 | 本次状态与证据边界 |
|---|---|---|
| D01 | Vulkan 1.0 明确不支持。 | 实现/有限验证：版本模型和 force 1.0 负例；实际入口拒绝覆盖仍见 D02~04 |
| D02 | Loader <1.1 安全拒绝。 | 待验收：已有版本策略/CPU 契约，loader 低版或函数缺失的端到端证据未齐 |
| D03 | XR constrained API <1.1 安全拒绝。 | 待验收：策略有实现，真实项目支持的 XR runtime 约束/拒绝取证未齐 |
| D04 | Device <1.1 安全 reject。 | 待验收：物理设备选择已有过滤，低版本 physical 的端到端拒绝未齐 |
| D05 | Engine Max API 固定 1.3。 | 实现/有限验证：1.4 loader/physical 下当前实际 effective 上限 1.3 |
| D06 | Loader / Engine / XR / Instance / Device / Effective 分离。 | 实现/有限验证：版本模型、CPU 契约、当前 API 日志；XR 分支平台验证待补 |
| D07 | `vkEnumerateInstanceVersion` 正确处理返回值与函数缺失。 | 实现/有限验证：模型/入口已有处理；缺失 PFN 的端到端故障输入仍待补 |
| D08 | Vulkan 1.4+ 不自动改变 Backend contract。 | 实现/有限验证：当前 1.4 设备仍按 1.3 contract 协商 |
| D09 | Vulkan 1.1 使用需要的独立 Feature Struct。 | 实现/有限验证：feature chain CPU 契约与 forced 1.1 协商 |
| D10 | Vulkan 1.2+ 可使用 Vulkan11/12 aggregate。 | 实现/有限验证：已有 feature chain 与 1.2 协商 |
| D11 | Vulkan 1.3 使用 Vulkan13 aggregate。 | 实现/有限验证：已有 feature chain 与 1.3 协商 |
| D12 | Aggregate/independent promoted structures 不重复。 | 实现/有限验证：链构建与去重契约；自然低版/平台矩阵待补 |
| D13 | supported/requested/enabled/runtime path 完整。 | 实现/有限验证：能力与集中路径归一化；区分 policy 与实际命令 |
| D14 | Vulkan dependency lock 完成。 | 实现/有限验证：LOCK 的已选范围；其他计划覆盖平台的工件/链接待核齐 |
| D15 | Vulkan 1.3 core dispatch 正确。 | 实现/有限验证：当前设备实际 Barrier2/Submit2；1.1/1.2 不要求 1.3 core PFN |
| D16 | SPIR-V 1.6 实际编译/验证 PASS。 | 实现/有限验证：PROGRESS §2F 三档引擎与 spirv-val 9/9，其他平台合同待补 |
| D17 | OUT_OF_DATE 闭环。 | 实现/有限验证：既有 WSI 状态运行与当前 resize；真实多平台异常流待补 |
| D18 | SUBOPTIMAL 闭环。 | 实现/有限验证：既有 WSI harness；模拟不能替代全部真实平台 |
| D19 | Zero-size suspend/recover。 | 实现/有限验证：既有 WSI 边界与当前 minimize/restore；移动恢复待补 |
| D20 | Surface recreate。 | 实现/有限验证：既有状态测试；Android Surface destroy/recreate 实机待验收 |
| D21 | Present semaphore 按 image 生命周期安全复用。 | 实现/有限验证：per-image/generation 管理及有限 WSI；跨 present mode/平台证据待补 |
| D22 | submission completion 不被错误用于证明 present completion。 | 实现/有限验证：两个生命周期已分开；fallback 严格退休边界不由有限无错结果代替 |
| D23 | Completion Tracker 统一。 | 实现/有限验证：tracker + submitTracked + fence/timeline 路径 |
| D24 | Command Pool / Descriptor / Dynamic Buffer / Staging / Recycle 等资源纳入 retirement。 | 实现/有限验证：recording epoch/lease、descriptor version、buffer range、staging/event 与既有寿命测试；全平台证据待补 |
| D25 | 用于通用资源回收的 completion signal 覆盖整个 submission；Submit2 默认 `VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT`。 | 实现/有限验证：tracker/submit encoder 的 whole-submission signal；不是 M4-B 缩窄对象 |
| D26 | 若 completion signal stage 缩窄，有完整正确性证明与验证。 | 条件未触发：本轮保持 whole-submission signal；不得以优化任务为由盲缩窄 |
| D27 | Present binary semaphore 不由 timeline 单独回收。 | 实现/有限验证：WSI generation 所有权独立，平台退休 Gate 尚未齐 |
| D28 | 无提前复用。 | 待验收：有限寿命/重建用例通过，不足以无范围地证明所有资源/设备路径 |
| D29 | 先完成语义等价迁移。 | 实现/有限验证：两 encoder 与独立 A/B；M4-A 综合 Gate 仍未通过 |
| D30 | 再完成同步范围优化。 | 待实现：M4-B 等待 M4-A；不能把上传 WAW 修复算成完整范围优化 |
| D31 | `synchronization2.enabled == false` 时强制关闭 Sync2 Barrier 与 Submit2。 | 实现/有限验证：集中归一化与当前 feature-disabled fallback |
| D32 | synchronization2 feature 已 enabled 时，Legacy Barrier Encoder 与 Submit2 可以独立 A/B。 | 实现/有限验证：既有 encoder 矩阵，当前实际 Sync2/Submit2 命令补证 |
| D33 | Runtime path 依赖集中归一化，不散落在业务代码。 | 实现/有限验证：能力/运行路径模型；资格检查与能力启用分别记录 |
| D34 | Synchronization Validation = 0。 | 实现/有限验证：当前六配置及两压力原始日志零同步错误，未扩大为全部场景/平台 |
| D35 | Submit topology 无意外变化。 | 实现/有限验证：统一 batch 和已有 harness/有限捕获；真实性能对照需同时核查 |
| D36 | Rendering Signature 完成。 | 实现/有限验证：owning tuple/hash/相等性契约及生产元数据 |
| D37 | `VkPipelineRenderingCreateInfo` 正确。 | 实现/有限验证：adapter/owning payload 契约与独立真实 pipeline 实测 |
| D38 | Dynamic pipeline `renderPass = VK_NULL_HANDLE`。 | 实现/有限验证：adapter 与独立 owned Dynamic driver 创建证据 |
| D39 | Pipeline `pNext` composer 完成。 | 实现/有限验证：typed owning chain/重建/失败/别名契约与 Classic 接线 |
| D40 | Secondary CB inheritance 完成。 | 实现/有限验证：Classic 实际 begin/execute + Dynamic inheritance adapter；未验证 secondary 保持 Classic 符合 §13.7 |
| D41 | Explicit layout/dependency migration 完成。 | 实现/有限验证：transition model/Hybrid 白名单；未入白名单场景继续 Classic，不能称全 pass Dynamic 迁移 |
| D42 | Load/store/resolve 映射完成。 | 实现/有限验证：契约、MSAA/D32 独立内容测试与当前 stencil STORE；所有格式/设备不由本机结果代替 |
| D43 | Eligibility 白名单明确。 | 实现/有限验证：单 subpass 等门控；当前 autoBarrier 场景实际回退 Classic |
| D44 | Classic fallback 始终存在。 | 实现/有限验证：factory failure 独立实验及当前真实工程 feature fallback |
| D45 | FSR/Secondary CB/XR 未验证场景默认 Classic。 | 实现/有限验证：保守白名单符合 §13.7；不是要求强行启用的缺失功能 |
| D46 | Dynamic vs Classic A/B 可强制切换。 | 实现/有限验证：独立 eligible 场景可切换；当前工程不 eligible 的场景 policy ON 仍 Classic |
| D47 | supported/requested/enabled 正确。 | 实现/有限验证：Maintenance4 能力契约与协商；并非必须开启其优化路径 |
| D48 | `maxBufferSize` 纳入 capabilities。 | 实现/有限验证：properties/limits 与创建上限检查 |
| D49 | 关闭能力时 fallback 正常。 | 实现/有限验证：既有 no-m4 与当前能力关闭场景；自然旧版设备待验收 |
| D50 | Golden scene visual PASS。 | 待验收：选定 atlas/RT 静态 ROI 与菜单 stencil 已通过；§18 其他渲染族和首帧定义内容边界未齐 |
| D51 | Baseline/Enhanced/Modern/Fallback 一致。 | 实现/有限验证：当前同 exe 四档/六配置 selected ROI 等价；不是全特性/平台一致 |
| D52 | Validation Error = 0。 | 实现/有限验证：本次原始日志 8/8 零错误；两族已分类 advisory 保留，不宣称 Warning=0 |
| D53 | Cold Start / First Present / First PSO 单独统计。 | 待验收：有限首用 harness 已有；用户真实工程对应测量未完成，main-entry/present API 不等于进程冷启动/可见首像素 |
| D54 | Cold Cache / Warm Cache 分开报告。 | 待验收：既有独立冷/暖实验；真实工程 cache 编译 OFF，未取得关联样本 |
| D55 | Warm-up 条件固定。 | 待验收：独立矩阵已有固定预热；真实场景测量入口/条件尚未完成 |
| D56 | 正式采样 ≥1000 valid frames/轮。 | 待验收：已有独立正式矩阵，不冒称用户真实工程已取得新样本 |
| D57 | ≥5 次重复。 | 待验收：独立矩阵已满足；真实工程相应重复仍待执行 |
| D58 | P50/P95/P99 均记录。 | 待验收：独立数据与有限首用统计保留；真实场景对应统计未完成 |
| D59 | Mobile 温控条件记录。 | 待验收：需要可用授权移动 Vulkan 设备/传感器/运行条件 |
| D60 | Cold/Warm pipeline cache 分开报告。 | 待验收：与 D54 同一证据族；真实工程 opt-in、持久数据关联与安全拒绝待补 |
| D61 | >3% 回归有分析结论。 | 待验收：已有分段/配对/同 backend 诊断，根因或接受理由未收口；>3% 是调查阈值，不是自动失败 |

## 4. 其余必须保留的验收边界

- §18 特性矩阵尚不能整体勾选：Shadow/Input Attachment/Multi Subpass/Post Process/Compute/Instancing/Indirect/FSR/Secondary 等需逐族关联当前目标工程或补充合同证据；不能由 atlas/RT 两种 ROI 推断全覆盖。自然 AMD/Intel/Adreno/Mali 的 rendering 运行证据不齐，格式 query 不能当 draw 验收。
- BestPractices 三族已修，两族分类保留。后者仍含 RT 首用未定义像素的业务合同边界；不是 Validation 错误，也不是首帧颜色已保证。需要确定首帧时，先定义生产先于消费/初始化合同，再安排专项验证。
- 用户确认的 mask Canvas/camera visibility 输入过滤不纳入 Vulkan 修复；不改测试场景以消除本次缺口。
- 本次没有发现应新增修复的新红例。验收未通过主要因为未实现项和证据缺口，不能误写为当前原工程仍有已知 WAW/09600。
- Android 当前源码编译链接已有；设备 shell 需登录，未认证或绕过，Vulkan 能力尚未核实。苹果/XR 按项目实际支持范围核对应 SDK/工件/runtime；没有证据时保留未验证，不以 Windows 或 Android 编译代替。

## 5. 剩余任务与推进次序

| 次序 | 任务 | 关闭条件 |
|---|---|---|
| 1 | T05 真实工程性能/首次使用测量；T09 cache/串行预热合同取证 | 显式 opt-in/default OFF、当前源构建；固定场景/尺寸/pacing；冷暖关联；≥1000 valid frames×≥5、百分位；安全拒绝与创建/销毁；>3% 给出分析/接受结论 |
| 2 | T06 M4-A 综合验收 | 正确性/内容、拓扑、性能 non-regression 有完整证据，才能放行 M4-B |
| 3 | T07 M4-B 同步范围优化 | 对每项候选明确 producer/consumer/stage/access/layout；真实工程等价及性能回归；保留 completion signal 生命周期语义 |
| 4 | T10/T11 与 §18 覆盖补齐 | 自然低版 GPU/Android 真机恢复/格式/TBDR/温控；项目支持的苹果/XR；按特性记录覆盖与合法 Classic 回退 |
| 5 | T12 再次整体验收 | 61 条 DoD、M0~M7 与 §18~20 全部必需条件具备终态证据，条件项说明适用范围；不能仅清零表格状态 |

当前任务清单仍为已完成 7、进行中 1、待执行 1、等待前置 3、依赖外部条件 2。T12 本次完成的是一次验收审查，结果为未通过；最终产品完成状态继续“等待前置”。

## 6. Verification Gate

| Gate | 本次状态 |
|---|---|
| G1 API 合法性 | 已验证（有限）：当前实现差异/能力协商与已测实际 driver 调用；全特性/自然低版与外部平台未验证 |
| G2 生命周期 | 已验证（有限）：既有寿命合同、当前两进程资源/窗口六轮压力；所有平台/资源退休未验证 |
| G3 同步 | 已验证（有限）：上传/复制依赖与 current logs/capture；M4-B 与全平台同步优化未验证 |
| G4 Validation | 已验证（有限）：六配置+两压力原始日志零错误，实际 layer 加载；全特性/平台未验证，两 advisory 非零 |
| G5 RenderDoc / AGI | 已验证（有限）：实际菜单 stencil、Modern 2d3d/RT；AGI/复杂特性/可移植首帧 golden 未验证 |
| G6 平台回归 | Windows 已验证（有限窗口/资源）；Android 仅编译链接，真实低版/移动/苹果/XR 运行未验证 |

相关记录：[任务清单](VULKAN_COMPLETION_TASKS.md)、[最终验收矩阵](VULKAN_FINAL_ACCEPTANCE_MATRIX.md)、[真实工程验证](VULKAN_WINDOWS_TEST_PROJECT_VERIFICATION.md)、[GPU 抓帧](VULKAN_REAL_PROJECT_GPU_CAPTURE.md)、[警告分类](VULKAN_BEST_PRACTICES_DISPOSITION.md)。本次不暂存、提交或推送。


## 2026-10-05 T13-02a 有限补证，整体验收结论不变

Lead使用原Windows测试工程完成实例颜色验证：2000实例真实Indexed draw及同帧HSV属性、652种输出匹配颜色，现代/Classic-fence正常退出/错误与泄漏0。详见[报告](VULKAN_INSTANCING_VERIFICATION_20261005.md)。这是已有Instancing实现的有限验证补证；Indirect实际0次、其他渲染特性与异常/平台/性能仍待验证，不能以本块关闭全§18或T12。15主任务总数与状态计数不变。

## 2026-10-06 T13-02b 最新有限证据（Lead）

原Windows测试工程的[Indirect Draw合同](VULKAN_INDIRECT_DRAW_VERIFICATION_20261006.md)有限通过：真实上传RAW红→绿；索引/非索引multi与loop、32/40字节实际命令和两2048字节golden；缺失draw负例、参数拒绝、退出/Validation/泄漏与当前exe RenderDoc。历史“Indirect实际0次/待工程入口”对应Instancing捕获时点，现已独立补证。实际Classic门控保留，非Dynamic/IndirectCount/平台/性能完成声明；T13与整体验收仍待，15主任务计数不变，下一批Bloom/Compute场景前提。
