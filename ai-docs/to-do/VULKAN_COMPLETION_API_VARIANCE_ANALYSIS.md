# Completion API 分段与正式矩阵复核

日期：2026-10-04。Orca Worker 执行矩阵；Lead 复核与离线分析。有限 clear workload，未授予性能或平台 Gate。

## 证据与执行归属

- 采用 `native/build/vulkan-modernization/completion-profile-matrix/run-20261004-113348193-10804-b4e2aa95/`。
- Worker 的 `perf-b2b-d-worker/formal-output2.log` 记录 PS5.1、两树 build 0、40/40 native 0 + judge True、环境恢复以及 wrapper exit 0。Worker“部分完成/这是 Lead 自跑”的报告与该终态日志不符，按日志纠正归属。
- 先前两个 root 缺 run-status，保留为未完成尝试；另一个 `112917319` root 有完整状态，但执行归属未核定。本分析仅使用可关联 Worker 终态日志的 `113348193` root。
- 源码 SHA256 为 `45A28C0878639E37768CDCCA51D7B819903D7615A62BAA6190AB0277B2606CE6`，运行前后相同；两树顺序构建。
- 每例 1000 samples + 300 warmup，validation ON/OFF × fence/timeline × profile ON/OFF × 5 repeats，共 40 进程、40000 样本。
- 现有 main/phase/present 判据与附加 completion 判据逐例通过；不是只凭 native exit 0。run-status 的 Complete/Pass/EnvRestored/SourceUnchanged/CompletionProfile 均 true。

## 独立数据检查

Lead 执行 `python native/build/vulkan-modernization/perf-b2b-d-worker/lead-analyze.py`，exit 0。分析工具及日志保存在 ignored 区，输出 root 内 `lead-completion-analysis.json`。

实际检查 plan/manifest/judge/status 的 40 个唯一案例顺序与配置、两 build 证据、每例五文件、精确 CSV header/1000 行、frame 0..999、有限非负数、canonical UInt64、NOT_READY≤status、queryFailures=0、整期 dispatch≥样本计数、OFF 时长 NA。profile ON 的十对设备/surface 元数据完整相等。

三种逐帧绝对闭合最大误差（ms）：wall/五阶段 `8.881784197001252e-16`；present/driver+wait+residual `2.220446049250313e-16`；residual/completion+other `2.7755575615628914e-17`。没有把中位数或 P95 作为可加量。

数字解析/已知统计/方向的 primitive SelfTest 实际 17/17 exit 0；完整行验证来自上述实际矩阵，不宣称这 17 项覆盖完整文件拒绝分支。PS5.1 的独立拒绝样本由正式 verifier 的 65 夹具承担。

## Profile ON 五轮等权均值（µs）

| validation / backend | present | QueuePresent PFN | scoped host waits | completion API | host other |
|---|---:|---:|---:|---:|---:|
| ON / fence | 157.1138 | 129.8375 | 2.2956 | 22.9682 | 2.0124 |
| ON / timeline | 150.8599 | 132.2877 | 0.0254 | 16.5928 | 1.9539 |
| OFF / fence | 107.5286 | 104.6477 | 1.1315 | 0.8624 | 0.8871 |
| OFF / timeline | 101.7893 | 100.0015 | 0.9003 | 0.1537 | 0.7338 |

此处 PFN span 是实际转发调用的整个跨度，包含层、ICD、调用期间等待等成本，不能改称纯 GPU 时间。completion API 是 present 作用域内 GetFenceStatus/GetSemaphoreCounterValue/ResetFences 的实际调用跨度。host other 仍为未分类 CPU/销毁/checkpoint 等剩余，不指定根因。显示值已 round，JSON 使用未 round 值校验闭合。

## 同 validation、同 repeat：timeline − fence

| validation / repeat | wall median % | wall P95 % | present mean Δµs | PFN mean Δµs | completion mean Δµs |
|---|---:|---:|---:|---:|---:|
| ON / 1 | -1.696 | +18.061 | +10.4660 | +18.5997 | -5.9309 |
| ON / 2 | -1.137 | -12.366 | -21.4255 | -12.0486 | -7.0236 |
| ON / 3 | +8.022 | +21.640 | +16.6492 | +22.4522 | -3.7996 |
| ON / 4 | -3.906 | -6.977 | -19.7246 | -8.9456 | -8.2777 |
| ON / 5 | -2.152 | -14.990 | -17.2348 | -7.8068 | -6.8452 |
| OFF / 1 | -1.697 | -16.125 | -10.1612 | -8.9326 | -0.6791 |
| OFF / 2 | +6.496 | -0.047 | +8.5287 | +9.0093 | -0.6363 |
| OFF / 3 | -3.126 | -20.793 | -13.4703 | -12.4683 | -0.7281 |
| OFF / 4 | -3.089 | -17.238 | -7.1255 | -5.6422 | -0.7589 |
| OFF / 5 | -3.133 | -4.655 | -6.4683 | -5.1971 | -0.7408 |

>3% 正向变化仍有 ON repeat3/OFF repeat2，ON P95 repeat1/3 亦需调查。两后端的当前 completion API 均值差均为负，但 present 差并非总为负；不能将 API 均值差直接当成 wall median/P95 差的因果解释。OFF repeat2 的 present 均值差可加地分为 PFN +9.0093、wait +0.2159、completion -0.6363、other -0.0602 µs，内部 PFN 原因仍未知。

## 边界与下一步

本块关闭新仪器的正式证据完整性缺口，并把原 residual 分出了 completion API 与 host other。没有宣布 timeline 获胜、validation 因果、encoder Gate 或整计划完成。旧 encoder 数据不能与此新运行跨时点合并归因。

后续先调查可用真实平台/游戏场景与受控重复的 PFN 波动；M4-B 生产同步缩窄仍受原 M4-A Gate 约束。移动温控、旧 GPU、Android/XR/macOS/iOS 运行及游戏视觉验证需要相应环境。
