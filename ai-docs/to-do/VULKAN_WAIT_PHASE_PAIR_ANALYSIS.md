# Wait phase 配对实测分析（2026-10-04）

GPU执行：**Orca Worker**；共享判据、终态复核与独立只读分析：**Lead**。
本块补齐新 wait 分类证据；不授予 M4-A、M4-B 或平台/性能 Gate。

- 正式命令：`verify-present-profile-matrix.ps1 -CompletionProfile -WaitProfile -PairedBackends`。
- Root：`native/build/vulkan-modernization/paired-wait-phase-matrix/run-20261004-201828150-23380-31f410f3/`。
- OFF树当前目标build0；20新进程、十相邻pair、每例1000帧+300预热，odd/even交替先后。
- 终态Complete/Pass/EnvRestored/SourceUnchanged=true，NativeOk/JudgePassed=20；五CSV每案1001行。
- benchmark SHA `A143539D0C47504249B82BFCECD43B2B11EBB5581C4EE11339EC13E784781110`；recorder SHA `7A6618A2D332AE57508EB0F1A4FE7FDC8210CB41D268AA0BBE2E78A8CB61719A`。当前文件哈希匹配。
- 此矩阵Validation **OFF**、triad实际`NOT VERIFIED`；不把errors=0当作Validation clean证明。
- 实际paths核对fence/timeline；sync2=0、submit2=0、dynamicRendering=0。GPU/driver/surface两成员元数据逐对一致，不能据此排除温控/时钟/后台负载。

## 完整十对：timeline − fence

百分比基准为fence；时间单位µs。全量未round数据见ignored JSON；不将不同分位数差相加。

| Pair | 首成员 | wall median Δ% | wall P95 Δ% | wall mean Δ | 全帧wait mean Δ | acquire present-fence mean Δ | present API mean Δ |
|---|---|---:|---:|---:|---:|---:|---:|
| 1 | fence | -5.11 | -28.70 | -22.824 | +0.766 | +1.406 | -17.000 |
| 2 | timeline | +4.18 | +31.50 | +11.910 | -1.748 | -1.223 | +14.486 |
| 3 | fence | +0.69 | -9.08 | -7.038 | +0.409 | +1.069 | -4.525 |
| 4 | timeline | -0.85 | +4.59 | -2.658 | +0.598 | +1.129 | +0.588 |
| 5 | fence | -1.42 | +11.17 | +5.889 | -0.172 | +0.384 | +6.024 |
| 6 | timeline | -0.51 | -16.00 | -12.698 | +1.174 | +1.108 | -8.978 |
| 7 | fence | -16.21 | -62.26 | -84.519 | -0.764 | +0.096 | -59.255 |
| 8 | timeline | +1.22 | +7.32 | +7.538 | +0.144 | +1.951 | +9.197 |
| 9 | fence | +4.20 | +35.94 | +12.101 | +0.083 | +0.015 | +9.316 |
| 10 | timeline | +1.64 | -2.01 | -0.755 | +0.963 | +1.087 | +0.711 |

4/10对median、9/10对P95绝对变化超过3%；两种先后方向均出现正负变化。这是调查证据，不是显著性、稳定提速或根因证明。
各对五phase mean差与wall mean差闭合，六wait bucket mean差与全帧wait mean差闭合（误差<1e-5µs）。wait子时长已包含在各phase/wall内，不重复相加。

## 新分类给出的范围

- 20000帧中每帧一次成功acquire，均有一次实际句柄/代/index/pending核对成功的acquire present-fence wait；其它acquire fences为0。这是本次稳定generation、maintenance1=1的结果，verifier并未硬编码次数。
- 10000个fence帧各有一次present段fence wait。10000个timeline帧中有178次present段semaphore wait；不硬编码timeline wait=0。
- unknown/record/submit/observe wait调用在本数据中为0；身份异常、wait failures均为0。
- Acquire present-fence等待的是maintenance1 presentation completion；独立于选定的fence/timeline submission completion后端。
- present段分类证明API所在phase与类型，没有输出该段wait的逐call句柄，不能凭CSV进一步指定每次具体slot/调用点。

## 同帧代表例（仅本root）

| Case / frame | wall | present API | 全帧wait | acquire present-fence | present wait |
|---|---:|---:|---:|---:|---:|
| timeline rep3 / 121 | 865.5 | 78.9 | 683.8 | 683.8 | 0.0 |
| fence rep6 / 38 | 769.6 | 72.3 | 610.0 | 609.6 | 0.4 |
| fence rep8 / 347 | 615.7 | 117.8 | 475.6 | 0.4 | 475.2 |
| fence rep7 / 768 | 4472.5 | 4381.8 | 5.8 | 0.6 | 5.2 |

前三例区分了原先“present作用域外wait”与present内wait；最后一例长跨度在真实转发的`vkQueuePresentKHR`调用内，不能分解ICD、GPU、合成器或线程调度的内部原因。
本root的pair7还同时出现record/submit均值差，不能将全部wall差归于单个present或wait段。
短clear-CB GPU timestamp不是整帧/队列/可见呈现耗时；API等待跨度也不是GPU执行耗时。
旧root的frame276/499没有当时的phase身份记录，本次分类不能回填其调用点。

## 可复现证据与剩余

Lead ignored脚本：`perf-b3-f2e-lead/analyze-wait-pairs.py`；`--self-test` **28/28 exit0**，正式读20案/20000帧 **exit0**。
完整JSON：`perf-b3-f2e-lead/wait-pair-analysis.json`，含终态、十对signed统计、全部case的每段统计/call总数、wall及三种wait排序的top10同帧记录。
独立reader核五CSV schema/frame/canonical有限数/计数/所有分区与包含phase范围、完整plan/manifest/judge/build/source/UTC、实际paths/triad。未构建、未GPU重跑、未改raw。
首次reader把validation字面量`+`作为正则造成真实exit1，修正为Escape并加入7个marker检查后通过；失败log/exit单独保留。此为分析脚本错误，非矩阵失败。
>3%根因仍需观测长present API跨度及进程/调度/时钟条件；真实游戏、移动设备、温控与平台Gate未完成。不删wait、不开展M4-B生产同步缩窄来绕过阈值。
