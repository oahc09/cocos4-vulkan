# 相邻、交替 backend 配对实验复核

日期：2026-10-04。GPU 执行渠道 Orca Worker；入口、判据及本分析渠道 Lead。有限 clear workload，profile ON，未授予性能/平台 Gate。

## 证据完整性

采用 `native/build/vulkan-modernization/paired-completion-matrix/run-20261004-124220753-36644-744a5068/`。
Worker `perf-b3-b-worker/execution-output.log` 有唯一当前执行终态 exit 0；OFF 树当前目标 build 0，20/20 native 0 + judge True。
status 的 Complete/Pass/EnvRestored/SourceUnchanged/CompletionProfile/PairedBackends 全 True，10 pair、1000 samples+300 warmup。
benchmark SHA256 为 `45A28C0878639E37768CDCCA51D7B819903D7615A62BAA6190AB0277B2606CE6`，与当前源码相同。

Lead `perf-b3-b-lead/analyze-pairs.py --root <上述root>` 实际 exit 0：独立核 plan/manifest/judge/status、单 OFF build、20 个唯一配置与顺序、UTC 起止/非重叠、有限 process-wait span、每案五文件、精确 header/1000行/frame连续/数值与计数/dispatch/三分区。
十对 GPU/surface 元数据完全相同：RTX4060 Laptop、driver=2584739840、624×441、3 images/3 slots、presentMode=0、maintenance1=1。
2 万行三种最大闭合误差（ms）：wall/五段 `1.1102230246251565e-16`，present/三段 `1.1102230246251565e-16`，residual/completion+other `1.3877787807814457e-17`。
数值/顺序 primitive SelfTest 14/14 exit 0；不把这14项当完整文件拒绝覆盖。原判据与附加判据由 runner 实际逐案消费。

## 每对 timeline − fence（同 pair，顺序紧邻）

单位：百分比为各自统计量 current/baseline−1；均值差为 µs。原始 JSON 为 root 下 `lead-paired-analysis.json`，保留每例 mean/median/P95/P99/population stddev/MAD 和未经 round 的差。

| pair | 先运行 | wall median % | wall P95 % | wall mean Δ | present mean Δ | QueuePresent PFN mean Δ | completion mean Δ |
|---|---|---:|---:|---:|---:|---:|---:|
| 1 | fence | -0.967 | -14.436 | -9.6578 | -7.3795 | -6.4995 | -0.6372 |
| 2 | timeline | +5.092 | +6.788 | +6.4084 | +8.8835 | +9.6309 | -0.6994 |
| 3 | fence | -2.821 | -4.596 | -3.3494 | -5.6166 | -4.3903 | -0.6462 |
| 4 | timeline | -5.356 | -4.102 | -2.2594 | -1.0167 | +1.1058 | -0.6728 |
| 5 | fence | -0.177 | -1.197 | -1.3550 | -2.3095 | -0.5236 | -0.6640 |
| 6 | timeline | +0.835 | +0.781 | +3.0144 | +3.3632 | +5.3953 | -0.7048 |
| 7 | fence | -7.635 | -33.526 | -16.2130 | -13.6040 | -11.9554 | -0.7335 |
| 8 | timeline | -5.832 | -1.982 | +0.6876 | +0.4815 | +0.9811 | -0.6716 |
| 9 | fence | +5.959 | +117.027 | +33.5296 | +27.3148 | +27.5415 | -0.5679 |
| 10 | timeline | -3.331 | +2.254 | -4.0134 | -4.7386 | -3.7936 | -0.6190 |

## 分组与可解释范围

五个 fence-first pair 的 paired wall median 百分比之中位数为 -0.967%，paired P95 百分比之中位数 -4.596%；五个 timeline-first 为 -3.331%/+0.781%。全10对对应 -1.894%/-1.589%，不是 pooled frame 统计或 Gate 判据。
>3% 正向变化在两种先后方向各出现一对（2、9）。不能用固定先后解释全部异常，也不据此证明不存在顺序影响；尚未记录 GPU 时钟/温度/后台负载。
pair9 的 wall P95 为 fence 199.1 µs / timeline 432.1 µs；QueuePresent PFN P95 为 122.4/302.8 µs。调用尾部变宽是描述性事实，不代表 GPU 原因已定。
每对 completion API 均值差均负，但没有抵消所有 PFN/其它段变化；各 P95 或 median 不能当可加分量或用均值差做因果解释。
不把旧40案与本20案跨时点直接比较，也不按总体中位数为负宣布 timeline 获胜。

## 下一步

本机可用 `C:/Windows/system32/nvidia-smi.exe` 已由 Lead 只读发现；可先调查实际支持的温度/时钟/pstate字段，尚未将其与本次样本关联，也未改变驱动设置。
后续区分同 backend 自身的重复波动与 backend 差异，避免再重复完整 ON/OFF/profile 矩阵。任何新 GPU 实验须保留完整判据和测量边界。
计划 §16.7 的 >3% 调查尚未收口，M4-B 生产同步缩窄仍受 M4-A Gate 约束；真实游戏/移动温控/低版GPU/XR/macOS/iOS 条件仍未具备。
