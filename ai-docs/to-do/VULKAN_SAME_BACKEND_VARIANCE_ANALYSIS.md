# 同 backend 重复运行波动复核

日期：2026-10-04。GPU 执行渠道 Orca Worker，入口/判据/独立分析渠道 Lead。有限 clear、Validation OFF、profile ON；未授予性能或平台 Gate。

## 完整证据

采用 `native/build/vulkan-modernization/same-backend-control-matrix/run-20261004-131601467-596-6a716108/`。
执行日志为 `perf-b3-d-worker/execution-output.log`，当前 OFF 目标 build 0，wrapper exit 0，20/20 native 0 + judge PASS。
status 的 Complete/Pass/EnvRestored/SourceUnchanged/CompletionProfile/PairedBackends/SameBackendControls 全 True。
10 对、20 进程，各 1000 samples + 300 warmup；奇数对 fence/fence、偶数对 timeline/timeline，各五对。
当前 benchmark SHA256：`45A28C0878639E37768CDCCA51D7B819903D7615A62BAA6190AB0277B2606CE6`。

Lead `perf-b3-b-lead/analyze-pairs.py --root <上述root>` 实际 exit 0，验证 plan/manifest/judge/status、单 OFF build、唯一 case、实际顺序/UTC/非重叠/process-wait、每案五文件及 20000 行数值/计数/dispatch/分区。
每案包含 native.log 和四个 CSV；四个 CSV 各 1001 行（header + 1000 帧），不要求 native.log 也有 1001 行。
三种最大闭合误差（ms）：wall/五段 `2.220446049250313e-16`，present/三段 `2.220446049250313e-16`，residual/completion+other `6.938893903907228e-18`。
十对设备/surface 元数据一致：RTX4060 Laptop、driver=2584739840、624×441、3 images/slots、presentMode=0、maintenance1=1。
完整未 round 统计在 root 的 `lead-control-analysis.json`，执行输出在 `perf-b3-b-lead/same-backend-analysis.log`。

## 同 backend 第二次 − 第一次

百分比的分母为同对第一次的同一统计量；均值差单位 µs。每案保留 mean/median/P95/P99/population stddev/MAD。P95 用 nearest rank。

| pair | backend | wall median % | wall P95 % | wall mean Δ | present mean Δ | QueuePresent PFN mean Δ | completion API mean Δ |
|---|---|---:|---:|---:|---:|---:|---:|
| 1 | fence | -4.766 | -12.010 | -0.9468 | -0.9171 | -0.1587 | -0.1716 |
| 2 | timeline | +0.972 | +21.117 | +10.0867 | +10.1904 | +10.6018 | +0.0162 |
| 3 | fence | -12.868 | -57.017 | -54.1584 | -46.2601 | -45.2137 | -0.2690 |
| 4 | timeline | +0.356 | +4.324 | +2.1687 | +0.6309 | +1.2241 | -0.0011 |
| 5 | fence | -3.240 | -17.589 | -11.4322 | -12.7373 | -12.8442 | -0.0225 |
| 6 | timeline | -3.864 | +8.674 | +0.3020 | -1.6010 | -1.1598 | -0.0062 |
| 7 | fence | -2.834 | -3.348 | -5.0426 | -8.1552 | -8.0461 | +0.0083 |
| 8 | timeline | -1.771 | -2.475 | -3.9270 | -7.6975 | -7.6193 | -0.0190 |
| 9 | fence | -0.089 | -3.139 | -2.7595 | -4.7951 | -4.3668 | -0.0297 |
| 10 | timeline | +4.638 | +85.644 | +38.4969 | +32.8528 | +32.2112 | +0.0467 |

## 判断与限制

固定 backend 也能出现 >3% 的正向变化：timeline 对照第10对 median +4.638%、P95 +85.644%；第2/4/6对 P95 分别 +21.117%/+4.324%/+8.674%。fence 第3对则向下 -12.868%/-57.017%。变更 backend 不是发生这类波动的必要条件。
五个 fence 对的 paired median%/P95% 之中位数为 -3.240%/-12.010%；五个 timeline 对为 +0.356%/+8.674%。这是对级描述统计，不是 pooled frame 结果、显著性检验或 Gate 判据。
不能据此认定此前 mixed-backend 差异全是噪声，或排除 backend 的真实影响。两实验跨时点，未记录运行时 GPU 时钟/温度/后台负载。
第10对 present 均值差 +32.8528 µs 中，PFN 均值差 +32.2112 µs；这描述调用区间成本，不揭示 ICD/合成器/GPU 内部原因。
第6对 median 下降、P95 上升、wall 均值差仅 +0.3020 µs，说明分布变化与均值相抵消；不得用均值分区解释分位数或相加各 P95。

## 下一步

利用已有 mixed/control 原始帧做连续时间块与慢帧分析：按 wall 排名选定同一批帧，再读取这些帧的 PFN/wait/query/其它段，检查集中时段与分区。先不重复 GPU 矩阵，也不将慢帧对应性当因果证明。
后续该离线块已由 Lead 完成（40案/40000帧、25项CPU测试及原红重放），见 `VULKAN_SLOW_FRAME_CONCENTRATION_ANALYSIS.md` 和 PROGRESS §2DN；不把本节原建议继续列为未实施。
计划 §16.7 >3% 调查仍未收口；M4-B 生产同步缩窄受 M4-A Gate 约束。真实游戏/Android 运行/低版本 GPU/移动温控/mac/iOS/XR 条件仍未齐。
