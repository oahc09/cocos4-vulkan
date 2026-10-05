# 连续帧块与同帧慢尾分析

日期：2026-10-04。原 GPU 执行渠道 Orca Worker；本块独立校验、补齐测试和分析渠道 Lead。仅离线读取，未构建或重跑 GPU，未改同步/驱动设置。

## 来源与验证

两个 root 各20案、各20000帧，分开分析：
- mixed：`paired-completion-matrix/run-20261004-124220753-36644-744a5068/`。
- control：`same-backend-control-matrix/run-20261004-131601467-596-6a716108/`。

路径均相对 `native/build/vulkan-modernization/`。Lead `perf-b3-e-lead/block-analysis.py` 实际分析 exit 0，40案/40000帧；直接读取 manifest.Log/Csv，核当前 source hash、status、build、plan/manifest/judge、顺序/UTC/来源路径、全部四 CSV 的 header/帧/数值/计数/三分区及 log/dispatch。
每例十个连续100帧块；记录全部五段和 present 四成分（driver/wait/completion API/host other）的均值与 wall 统计。按 wall 降序、frame 升序破同值，恰取 top50；保留排名和时间顺序、十块计数、同一集合的各段均值和 top10 完整逐帧/闭合证据。
sum(driver)/sum(wall) 为同帧区间总和比，不是各帧比值平均，也不表示 GPU 时间占比。P95 用 nearest rank，不相加分位数，不将 top50 称独立 P95 集合。
实际 CPU SelfTest **25/25**，含60帧并列排序/块计数和4行 CSV 单变量反例。三个原红变异重放：valid 接纳，query_failures=1/NaN/present 分区破坏均拒；额外 completion 分区破坏也拒。没有借此声明完整文件级拒绝覆盖。
原始未 round 结果 `perf-b3-e-lead/lead-block-analysis.json`；`analysis.log`/`analysis-exit.txt`、`selftest.log`、`focused-pairs.json`、`replay.log`/`replay-exit.txt` 及 replay 子目录保留。

## 三对关注样本

单位 µs，均值只针对该例按 wall 选出的同一50帧；不同进程的集合独立。占比为同帧总和比×100。

| 实验/成员 | top50 wall mean | PFN mean | wait mean | completion API mean | host other mean | PFN/wall % |
|---|---:|---:|---:|---:|---:|---:|
| mixed pair9 fence | 357.068 | 167.592 | 18.066 | 1.616 | 2.358 | 46.936 |
| mixed pair9 timeline | 598.368 | 454.640 | 7.192 | 0.644 | 6.104 | 75.980 |
| control pair3 fence member0 | 799.100 | 638.444 | 3.724 | 2.606 | 7.706 | 79.895 |
| control pair3 fence member1 | 328.936 | 205.532 | 9.228 | 1.776 | 3.132 | 62.484 |
| control pair10 timeline member0 | 344.742 | 221.622 | 6.742 | 0.426 | 3.306 | 64.286 |
| control pair10 timeline member1 | 676.166 | 509.520 | 9.014 | 0.554 | 5.350 | 75.354 |

- mixed pair9 timeline 的 top50 有 **21帧在500–599**，该块 wall median/P95 **220.6/610.7**；同例其它块 median **113.05–126.25**。fence top50 十块计数为 `[5,7,5,6,4,7,5,3,3,5]`，未有同样的集中。后者最慢 frame276 wall **921.7**、PFN **116.0**、wait **0.9**，具体其它段仍须逐帧读取，不能全部归于 PFN。
- control pair3 fence member0 的 top50 有 **22帧在500–599**，该块 median/P95 **163.55/881.5**；member1 同块 **103.05/124.8**、top50只有4帧。member0 最慢 frame560 wall **3559.0**、PFN **3443.3**、wait **9.1**，该帧确实主要落在 PFN 区间，不能推广为所有慢帧的原因。
- control pair10 timeline member1 的 top50 有 **25帧在400–599**（15+10），两块 P95 **593.1/714.7**，member0 为 **232.8/218.4**。member1 frame504 wall **3192.7**、PFN **3080.7**；frame843 wall **2403.5**、PFN **1376.5**且 submit **886.1**，同例也有多段变大的帧。

## 初稿与 rev2 复核

Worker 初稿仅核五段闭合，忽略 completion 内容；`--self-test` 是空 stub。Lead 原始三个单变量坏样本均被误接纳。
Worker rev2 的40例选帧、块计数和 top50 主要均值与 Lead 结果逐项吻合，但源码仍无 exact header/canonical calls/两层 present 闭合/完整 status-source-order 校验，无 top10/SelfTest/红重放。Lead 重放仍见 query failure、present 分区破坏和 completion 分区破坏误过，NaN 已被 Worker 修正拒绝。
撤销其 PROGRESS `§2DD rev3` 的“strict 校验/4-part 校验/40+20输出”说明；保留真实实际数据计算的有限价值，验收依据为本块 Lead 补齐的结果。Worker 再次写了冻结 PROGRESS，未视为后续写权限。

## 判断与下一步

部分运行的慢尾集中于有限连续区间，PFN 在多例慢尾占比较大；存在 acquire/submit/其它区间贡献明显的反例。跨进程相同帧号不是相同物理时间，不能将500附近的集中归为周期、GPU升频或后台事件。
PERF-B1/B2 的 present/wait/query 分解已经接入，撤销“待细分仪器化”的过时 pending。下一层可核同帧 GPU clear timestamp、CPU/wait/in-flight 字段及其采集语义；目前没有运行关联温控/频率观测，不能从 API 区间定内部原因。
>3% 调查与 M4-A Gate 仍未通过；不启动 M4-B 生产缩窄。有限 clear 不代表游戏场景/移动温控/平台验收，不宣布整计划完成。
