# 慢帧对应的 GPU/CPU/wait/in-flight 字段

日期：2026-10-04。Orca Worker 提供只读调查，Lead 核当前源码并完成离线同帧关联；无 build/GPU 重跑/生产或驱动设置改动。

## 当前源码边界

- `completion_benchmark.h:408/415`：用户 CB 的 TOP_OF_PIPE/BOTTOM_OF_PIPE timestamp，围绕 begin/endRenderPass，包含该区间的 GPU 执行，不当纯 clear 指令、整帧 GPU、队列等待或可见 present 时间。
- `completion_benchmark.h:489/505/509/511/512/517`：先 waitAllFences，读取从 `warmup*2` 开始的 `frames*2` query；逐对显式检查 availability，再用 timestampValidBits mask 和 timestampPeriod 换算。CSV 第i帧对应总循环第warmup+i帧；mask不是 query flags 自动提供的。GPU 与主机时钟不是同一时钟，区间不能与 CPU wall 相加。
- `completion_benchmark.h:319/397/398/445/450`：GetThreadTimes kernel+user 的 FILETIME ticks 除10000得 ms；单位100ns不代表实际分辨率。cpuStart先于wall start，CPU读数在afterPresent之后，两个采样范围不完全相同，不以wall−cpu代表睡眠或阻塞。
- `completion_benchmark.h:204/221/396/451`：两 wait PFN hooks 同时累加全局 benchWaits 与 active present scope；main wait_ms 是本帧API span差值，present host_wait_ms 是其中scope内子集，不能重复相加。即使成功立即返回也计一次调用，span不是纯GPU等待时间。
- `VKDevice.cpp:84`：tracker借用PFN变量地址，后续全局hook替换会被真实调用读取。`VKCompletionTracker.h:131` isComplete缓存可短路；未完成point会调用GetFenceStatus或GetSemaphoreCounterValue，并非始终纯CPU轮询，也不是wait API。
- `VKCompletionTracker.h:14`：point是domain/value，两后端统一，point本身没有VkFence指针。`completion_benchmark.h:422/425/433` 的in_flight为本帧submit后、point轮询剔除后、present前的framePoints.size，不是全部submit数或GPU瞬时队列深度。
- `VKDevice.cpp:1271` 和 `VKGPUObjects.h:779/827/1578`：对象计数使用pool allocatedCount（含idle），加transport fence和completion semaphore；benchmark再加当前generation的present fence/renderFinished（:434/435）。不当used-count或整个device所有对象清单。

## 同帧关联的实际验证

Lead ignored `perf-b3-e-lead/frame-metrics.py`：直接读此前两个root各20案的四CSV，核来源/终态/数值/分区，将原top50排名与当前wall重新匹配；补集严格950帧。每案1000帧一一对应，**40案/40000帧、exit0**。
CPU fixtures **13/13 exit0**：4行已知单位/选帧/并列/补集/重复索引/越界/NaN/错误frame/wait子集；保留CPU大于wall的合法观测而不把它伪装成坏CSV。
未round JSON `frame-metrics-analysis.json`、实际两次命令及exit文件/日志、`frame-metrics-selftest.json`位于同ignored目录。旧GPU执行归属仍为Orca Worker，新增读数分析归属Lead。

GPU单位µs；top50按wall选择，GPU P95只对同一集合取nearest rank，补集并非另一次进程。

| case | top50 GPU mean | top50 GPU P95 | 补集950 GPU mean | 全1000帧CPU非零数量 |
|---|---:|---:|---:|---:|
| mixed pair9 fence | 3.923840 | 4.512 | 3.940480 | 7 |
| mixed pair9 timeline | 4.016000 | 4.416 | 3.947688 | 8 |
| control pair3 fence member0 | 6.006400 | 4.704 | 3.955166 | 9 |
| control pair3 fence member1 | 3.951360 | 4.416 | 3.919124 | 7 |
| control pair10 timeline member0 | 3.935360 | 4.512 | 3.931251 | 8 |
| control pair10 timeline member1 | 3.966720 | 4.384 | 3.950922 | 9 |

40进程共40000帧中CPU值仅 **0/15625µs**，317帧非零且全部比该帧wall大。这是当前文件实际分布；实际分辨率/更新机制未知，不能据此分离逐帧running/blocked时间，也不否定累计CPU统计的有限用途。
mixed fence pair9 frame276：wall921.7、driver116.0、GPU区间4.096、帧内wait692.0、present内wait0.9；差值691.1µs位于present scope以外，不能只看present sidecar遗漏该成本。
mixed timeline pair9 frame499：wall1121.1、driver550.1、GPU区间3.680、帧内wait211.7且present wait0；同样存在scope外wait。
control timeline pair10 member1 frame504：wall3192.7、driver3080.7、GPU区间3.872、帧内wait2.0；该例慢帧与长PFN区间对应，但不意味着所有慢帧同因。

## 初稿修正与下一步

Worker F0的timestamp/query/in-flight等多处行号不是当前位置；“polling纯CPU”“point含fence指针”“mask由query flags提供”等说法不采纳。允许测QueuePresent API span，不能称首像素显示延迟。
`VKDevice.cpp:1118/1120` reacquire时存在maintenance1 present fence的显式wait，可能解释部分scope外API wait；旧CSV未记录wait的调用点/句柄，当前只列候选，不把上述两帧直接归到此调用。
下一层可调查正常帧scope外wait的可达调用点，决定是否需harness内按阶段/句柄分类；不改正确性等待、不增加signal-only submit，不以小GPU区间排除全部GPU/呈现瓶颈。
>3%原因/M4-A性能Gate仍未收口，M4-B生产缩窄保持未启动；游戏/真实设备/温控/平台条件仍未齐。
