# Vulkan 1.1→1.3 现代化最终验收矩阵（rev2，只读快照）

日期：2026-10-04（Orca Worker）。rev2 按 Lead 复核修正章节映射（§8=M0、§9=M1、§10=M2、§11=M3、§12=M4）、PROGRESS 证据指针（实查 headings）、WSI/Dynamic 两证据族、First-compute-PSO 事实、平台/性能边界。状态词：代码已实现 / CPU 契约 / Windows 有限运行 / Android 编译链接 / 待运行 / 待实现 / 依赖外部条件。只读，无构建/测试。

## M0 基线（计划 §8）

| 要求 | 状态 | 证据 | 剩余 Gate |
|---|---|---|---|
| 依赖锁定（Headers 1.3.296/volk/VMA/glslang+SPIRV-Tools） | 代码已实现 | VULKAN_DEPENDENCY_LOCK.md；LOCK Chosen 范围内平台 | mac/iOS 新版 glslang 源工件不在本机 Chosen 集（LOCK 实际范围为准），依赖外部条件 |
| 三档 SPIR-V 目标+校验 | Windows 有限运行 | §2F：引擎 SPIRVUtils 三档输出与 spirv-val 9/9；§2B 为旧库 1.3 UNSUPPORTED，不能当升级后证明 | 平台矩阵依赖外部条件 |
| 最小版本模型+VMA 接入 | 代码已实现 | §2D | — |
| loader/分层/校验状态 | Windows 有限运行 | §2C loader smoke、§2D 版本/层配置；capability 契约与真实协商分别见 §2H/I 与 §2J/K | 多 GPU 平台矩阵依赖外部条件 |
| Windows Simulator 本地构建入口 | 代码已实现 | §2DH rev3（Lead）：protobuf 两文件最小适配，CPU probe 1030/1030；simulator 静态库和 SimulatorApp-Win32 应用当前源码各 build exit 0 | 未启动应用，不代替 Vulkan 设备/画面/性能 Gate |

## M1 版本协商（计划 §9）

| 要求 | 状态 | 证据 | 剩余 Gate |
|---|---|---|---|
| feature query/enable（三态能力） | 代码已实现 + Windows 有限运行 | §2J（引擎协商接入）、§2K（验收矩阵+Android 构建回归） | 多 GPU 平台矩阵依赖外部条件 |
| promotion/去重（1.2/1.3 promotion 规则） | 代码已实现 | §2J 协商实现+§2K 矩阵 | 真实低版本 GPU 待运行 |
| 版本拒绝负例（force 1.0/unknown token） | Windows 有限运行 | verify-capabilities neg-*（§2K 链路） | — |

## M2 WSI（计划 §10）

| 要求 | 状态 | 证据 | 剩余 Gate |
|---|---|---|---|
| WSI 资源生命周期/引用链审计及修复 | Windows 有限运行 | §2L 为只读审计；§2P/S/U 为退休、中止帧与 per-submit 转换修复；§2Q/T/V 为有限回归 | 完整平台退休 Gate 待验证；maintenance1-off 通过不替代严格 present-completion 证明 |
| 错误状态（surface lost/out-of-date/子优） | Windows 有限运行 | §2U 多次 submit 自动 WSI 转换 + verify-wsi/verify-wsi-lifecycle S1~S8 | 多屏/DPI 场景依赖外部条件 |
| per-image presentation 语义 | Windows 有限运行 | §2U、wsi_lifecycle_smoke | 真实交换链压力（多 present mode）待运行 |
| WSI 与 Hybrid 范围 | 代码已实现（WSI 保留 Classic） | §2BX 生产 eligibility 排除 WSI；Windows WSI 与 offscreen Dynamic 为两证据族 | WSI Dynamic 属未来范围，当前未接入，不能标成已有路径仅待运行 |

## M3 完成模型/资源退休（计划 §11）

| 要求 | 状态 | 证据 | 剩余 Gate |
|---|---|---|---|
| timeline 全资源 lease（staging/event/descriptor/recording） | Windows 有限运行 | §2AA（timeline 接入）、§2AI/§2AJ（staging lease）、§2AL/§2AM（event）、§2AN~§2AS（descriptor 版本/template/动态实例）、§2AE/§2AG/§2AH（寿命 harness/Android 回归） | 平台矩阵依赖外部条件 |
| fence 1.1 回归 | Windows 有限运行 | descriptor §2AO/AP/AQ，recording §2AG/AS，event §2AM | 真实 1.1 GPU 待运行；forced tier 不替代旧设备 |
| signal whole-submission 语义 | 代码已实现 | VKCompletionTracker.h:114 + VKSubmitBatch.h:91（Submit2 ALL_COMMANDS signal） | —（§11 豁免语义边界） |
| benchmark（frame CSV≥1000×5/百分位/GPU timestamp/ON-OFF） | Windows 有限运行 | §2AU/AW、§2DA~DC；§2DE~DG completion正式40进程×1000帧；§2DI/DJ mixed20案；§2DL/DM同backend20案，完整判据/顺序/时点/独立分区核查 | 固定backend重复运行也有>3%变化，不证明旧mixed差异全是噪声；原因调查/正式平台 Gate 未过 |

## M4 同步（计划 §12）

| 要求 | 状态 | 证据 | 剩余 Gate |
|---|---|---|---|
| Submit2+Sync2 开关分离（legacy/Sync2 独立 A/B） | 代码已实现 + Windows 有限运行 | §2AY（统一提交模型）、§2BA（Barrier2 等价）、§2BC（A/B 微基准）、§2AZ（Android 编译回归） | 平台矩阵依赖外部条件 |
| Barrier2 inline 存储 | 代码已实现 | §2BH/§2BI（inline+真实引擎回归） | — |
| 编码器未完成项收口 | Windows 有限运行 | §2BJ（未完成报告+Lead 接管验证） | — |
| completion signal ALL_COMMANDS 豁免 | 代码已实现（语义边界） | 计划 §11+§2CV 清单 §1 | 不适用（排除优化对象） |
| M4-B 同步范围缩窄 | 待实现 | VULKAN_SYNC_SCOPE_INVENTORY.md rev2（候选+缺失信息） | 前置 M4-A PASS+平台 Gate |

## M5 渲染契约（计划 §13）

| 要求 | 状态 | 证据 | 剩余 Gate |
|---|---|---|---|
| RenderingContracts 族（signature/contract/pnext/attachment/secondary/transition/RP signature/pipeline rendering/info） | CPU 契约 + Windows 有限运行（真实 hook 证据） | §2BL~§2BT；CPU owning/映射证明与 §2BR/BS 的 Classic driver 证据分别记录 | Dynamic secondary 实际录制未接入；完整平台 Gate 待验证 |
| secondary Classic 支持 | 代码已实现 | §2BS（secondary begin/inheritance 录制 hook） | — |
| Classic fallback | Windows 有限运行 | §2BX、verify-dynamic-rendering EnginePath owned-failure：变体创建失败后 Classic 实际 draw/readback 及 partial 退休 | 游戏场景与其他设备 fallback 待运行 |
| 生产 Hybrid eligibility（debug opt-in，排除 WSI） | 代码已实现 | §2BV~§2BX；环境变量为 CC_VULKAN_RENDERING=dynamic（G3 rev2 已修正 env 访问） | 真实场景扩展待运行 |

## M6 平台视觉（计划 §14）

| 要求 | 状态 | 证据 | 剩余 Gate |
|---|---|---|---|
| color MSAA resolve harness | Windows 有限运行 | §2CA + verify-msaa(68) | 移动 GPU 格式矩阵依赖外部条件 |
| depth-only D32 resolve / DS 变体 | Windows 有限运行 | verify-depth-resolve(38)/verify-depth-stencil(46)/dynamic_depth_stencil_smoke | 同上 |
| 格式能力探针 | Windows 有限运行（query-only） | §2BY rev2：RTX/AMD 格式、sampleCounts、D/S resolve mode 查询；不创建 device/图像 | 不能替代各设备实际 rendering；D/S combined resolve 仍待实现/验证 |

## M7 cache/prewarm（计划 §15）

| 要求 | 状态 | 证据 | 剩余 Gate |
|---|---|---|---|
| PipelineCache 持久化+opt-in 配置（默认 OFF） | 代码已实现 + Windows 有限运行 | §2CD/§2CE；verify-pipeline-cache restored/cold-recovery/rejected 三阶段 | 平台 Gate |
| compute PSO owning key+生产接线 | 代码已实现 | §2CK/§2CL | — |
| Classic RP key/graphics key 完整组合 | 代码已实现 | §2CM~§2CP | — |
| PrewarmQueue 串行接口（§15.3 第一阶段=串行 compute 实验，已有限完成） | CPU 契约 | §2CS | graphics manifest 扩展为可选后续（非本阶段硬 Gate） |
| 真实 factory→cache→新进程 restore | Windows 有限运行 | §2CT/§2CU rev3（Lead 收口）；verify-prewarm.ps1 67 fixtures+三进程 0/0/1（Lead 执行） | 跨平台 restore 依赖外部条件 |
| Android 编译链接 | Android 编译链接 | §2CU Worker 40 步实测（现代 8 库/旧 external 0） | 设备运行依赖外部条件 |

## §23 DoD 汇总族

| 条目族 | 状态 | 证据 | 剩余 Gate |
|---|---|---|---|
| API baseline 拒绝/六版本模型 | 代码已实现 + Windows 有限运行 | §2D/§2J/§2K：CPU 版本契约与 forced 1.0 拒绝 | 实际缺 PFN/loader<1.1/physical<1.1/XR constrained<1.1 的端到端拒绝未齐；forced 1.0 不等价 |
| feature query/enable+promotion | 代码已实现 + CPU 契约 | §2H/I/J/K | 真实低版本 GPU/KHR 路由及平台矩阵待运行 |
| 依赖锁定/core dispatch/SPIR-V 1.6 | Windows 有限运行 + Android 编译链接 | LOCK、§2C/D/F/G | mac/iOS/XR 工件与运行未齐 |
| WSI 错误状态/per-image presentation | Windows 有限运行 | §2P/S/U/Q/T/V | 真实其他 present mode/移动/XR；maintenance1-off 回退不替代严格退休证明 |
| Completion tracker/recording/pool/staging/event/descriptor/dynamic buffer 退休 | Windows 有限运行 | §2AA~§2AS；整批 signal 和 presentation binary 生命周期分开 | 完整游戏资源范围与真实平台寿命 Gate 未齐 |
| Submit2/Sync2 分离+signal 语义/拓扑 | 代码已实现 + Windows 有限运行 | §2AY~§2BJ；有限案例 Validation=0 | 完整视觉/性能/平台 Gate 未过；同步范围缩窄待实现 |
| RenderingContracts/Classic secondary | CPU 契约 + Windows 有限运行 | §2BL~§2BT | Dynamic secondary driver 消费与复杂 rendering 场景未齐 |
| Hybrid eligibility/Classic fallback | Windows 有限运行（debug opt-in） | §2BX、owned-failure 的真实 Classic draw/readback | WSI/FSR/secondary/XR 默认 Classic；游戏场景 A/B 未齐 |
| maintenance4 能力与大小限制 | 代码已实现 + CPU 契约 + Windows 有限运行 | VKCommands.cpp:304 上限检查、capability_tests、§2K no-m4 | 可选 useMaintenance4 优化仍 OFF；真实旧设备 fallback 待运行 |
| cache 恢复+serial prewarm | Windows 有限运行 + Android 编译链接 | §2CD~§2CU | 跨平台实际恢复与冷启动场景待运行 |
| Correctness/golden scene/四档一致 | Windows 有限运行 | MSAA §2CA、D/S §2BZ/CC、Dynamic owned 内容对照 | finite pixel golden 不等价游戏 golden scene/capture；四档平台一致未齐 |
| 性能/冷暖 cache/预热/百分位 | Windows 有限运行 | ≥1000×5 与 ON/OFF 分段 CSV（§2AU/AW/BC/BJ）；§2DC 实际 profiling 矩阵40进程/4万帧、native与主表/五段/sidecar共享判据全部通过；prewarm cold/restored factory1/driver1 原始 ns；§2CX/CY 有限 graphics 首用：ON/OFF×cold/restored×5=20 进程、十项时长与 40 组描述统计 | >3% 稳态波动未归因；main-entry→present API 返回不等于整进程 ColdStart/首像素显示，有限 graphics PSO 不等于游戏场景；instance/device 原始时长未拆分；移动温控待设备 |
| mac/iOS/XR | 依赖外部条件 | LOCK 平台范围与编译记录 | 不以 Win/Android arm64 编译证明其他平台 |

## 本机可继续的具体验证

1. 有限首帧测量已实施（§2CX/CY，60 夹具、20 进程复核）；继续稳态 >3% 波动诊断与游戏场景测量，不能把已有有限数据称为未实施，也不能替代真实场景 Gate。
2. >3% 波动定位继续：§2DG、§2DJ、§2DM正式矩阵已核查；mixed pair2/9 wall median +5.092%/+5.959%，固定timeline pair10 +4.638%/P95 +85.644%。§2DN/DO已完成40案/4万帧连续块、top50/top10及同帧GPU/CPU/wait/in-flight关联；部分慢帧有scope外wait，CPU数据不适合逐帧阻塞推断。backend变更非波动必要条件，不能因此否定真实backend效应。下一步核正常帧wait调用点/句柄分类，运行关联时钟/温控未采集；PFN内部原因和旧encoder波动未归因，不把API span当纯GPU或用mean解释分位数。
3. M4-B0 候选仅保留设计调查；计划 §12.2 的 M4-A 前置 Gate 未过，生产缩窄不得启动。

## 需要真实外部设备/场景

1. Android 真机（cache 命中/温控/格式矩阵）。
2. 真实低版本 GPU（自然 1.1/1.2 行为）。
3. mac/iOS/XR 构建工件。
4. 游戏场景视觉 capture（不等价 finite clear/smoke）。

以上未完成前不宣布整计划完成。本文件只读，不构成验收结论。
