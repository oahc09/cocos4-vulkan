# PERF-B0 rev2 稳态 >3% 波动诊断（只读调查 + 一次性分析 rev2）

日期：2026-10-04（Orca Worker 调查，Lead 收口）。rev2 修正初稿误报（Lead 源码复核及独立 `lead-independent-pairs.json`，Worker 修订）；Lead 再补齐真实自测与严格结构检查。零构建/GPU 重跑/生产改动。分析脚本与 JSON 在 ignored `native/build/vulkan-modernization/perf-b0-analysis/`。

## 1. rev2 修正（撤销初稿误报）

1. phase_diff 实现 a−b 却标 sync2_minus_legacy，wall 用 b−a，**符号反向**；"sum==wall ±1µs 逐对验证"系误报（初稿无 assert）。rev2 改 signed current−baseline（全对同方向），phase closure 在代码内 assert <1e-6µs（初稿实测 closure 精确成立）。
2. diff 百分比改 100*(current/baseline−1)，保留方向（初稿用 abs/max 丢失方向）。真实值：timeline rep1 med **+27.877%**/P95 **+40.020%**；fence rep3 med **−26.603%**/present **−32.675µs**；fence rep4 med **+33.025%**——与 Lead 数值逐项吻合。
3. 撤销"旧 M3 根 10 对入 JSON"：旧根文件名无 legacy/sync2 label，循环 continue 实际 0 对；rev2 仅保留 OFF inline 10 pair。
4. 撤销"present 80~100% 全部主导"：timeline rep2 ratio≈71.5%、rep4≈5.9%、rep5 present −0.655µs 而 wall +1.625µs（反向抵消）；observe 最大绝对差 0.058µs 非 ≤0.04（小绝对值不等于 observer effect 可忽略）。
5. load 缺文件/行数/frame 断裂/non-finite/phase 不闭合：rev2 全部 assert 非零退出（初稿 continue/silent skip）。
6. GPU query results 读取在采样循环后 waitAllFences 之后（completion_benchmark.h:260 起），**不属于 observe 段**；observe 段=framePoints isComplete 轮询+objectCounts 等，在 afterSubmit→afterObserve。引用已修正。
7. maintenance1 presentFence 在当前 present 源码只附加到 pNext（VKDevice.cpp:1194-1203），**并未显式 wait 该 presentation fence**；:1244 fence pool wait 与 waitSlot 属于 submit 完成等待。二者不混同；driver 行为无证据不写。

## 2. 定量结果（OFF inline 10 pair，signed sync2−legacy，单位 µs）

| rep | backend | wall med diff | wall med % | wall P95 diff | wall P95 % | present mean diff |
|---|---|---|---|---|---|---|
| 1 | fence | +1.100 | +0.942 | +24.100 | +8.716 | +10.657 |
| 1 | timeline | +32.700 | +27.877 | +121.700 | +40.020 | +34.544 |
| 2 | fence | +14.700 | +12.895 | +64.700 | +24.323 | +26.947 |
| 2 | timeline | +0.500 | +0.426 | +48.400 | +16.917 | +5.850 |
| 3 | fence | −43.350 | −26.603 | −61.900 | −17.772 | −32.675 |
| 3 | timeline | +1.000 | +0.876 | −34.700 | −11.176 | −11.437 |
| 4 | fence | +41.050 | +33.025 | +55.800 | +15.984 | +26.993 |
| 4 | timeline | +4.150 | +3.634 | +26.100 | +9.812 | +0.163 |
| 5 | fence | −9.450 | −7.420 | −112.800 | −30.056 | −29.830 |
| 5 | timeline | +4.400 | +3.836 | −14.100 | −5.107 | −0.655 |

- phase closure: sum(5 phase mean diffs) == wall mean diff（每对 <1e-6µs）；present mean 差多数轮为最大绝对差项但有例外及抵消（rep4 timeline present **+0.163µs**，wall **mean +2.772µs**；该轮 wall median 差为 +4.150µs，不能混算），**内部构成未知需分段测量**。
- 前后半段漂移提示（不称显著/因果）：多数轮后半段略低于前半段。

## 3. present 段源码调查（修正后）

`CCVKDevice::present`（VKDevice.cpp:1159）在 observe→present 采样区间内实际包含：vkQueuePresentKHR(:1204)、maintenance1 presentFence 附加(:1194-1203，**仅 pNext 不显式 wait**)、fence pool vkWaitForFences(:1244)+observeCompletedFences+waitSlot(:1245-1248)、fence/semaphore pool reset(:1255-1256)、retireRecordings(:1257)、recycleBin clear(:1258)、staging reset/shrink(:1259-1260)、pipeline cache checkpoint(:1261)。bench 现 vkWaitForFences/vkWaitSemaphores hooks 只计 API span；**observe 段在帧内含 framePoints 完成查询及对象计数；GPU query results 在整个采样循环结束、waitAllFences 之后读取，不计入这五段帧 wall 时间**（completion_benchmark.h:218-269）。

## 4. PERF-B1 方案

- **instrumentWholePresent：原五段保持不变**，在 present 段内部增加三分字段：forward-first vkQueuePresentKHR span、present 作用域内 host waits span（vkWaitForFences/vkWaitSemaphores）、remaining host residual=父 present span−两个非重叠子 span（嵌套去重）；residual 只标"未归类"，不宣称 recycle/driver stall。
- PFN 测量不改执行顺序，成功/失败调用均记录 span+result，成功样本要求全 SUCCESS；wait 分类只限 present 作用域内的实际调用，未 observed 即未知不补造；off 分解字段 NA 或未采集非伪 0。
- 固定容量/无热路径分配/RAII/ON-OFF 身份稳定；instrumentation off 开关不改同步。

## 5. 结论

- **已证实**：可加性（phase closure）；present 段 mean 差多数轮绝对值最大但有例外（rep4/rep5 timeline）；>3% 波动根因未归因。
- **未知**：present 内部各子类占比；不同 driver/平台/温控。
- **后续更新**：PERF-B1 已由 Lead 完成仪器化及可信正式矩阵（PROGRESS §2DA~2DC）；下一步是针对新数据与旧 encoder 实验边界继续定位，不能重复把分解测量标为未实施。不授予任何 Gate。

## 6. Lead 收口实测

Worker rev2 的 `--self-test` 实际没有 CLI 分发，运行它仍执行正常分析；自测函数还读错 fixture 名、使用未定义恢复变量且未真正调用多项拒绝检查，故撤销自测覆盖声明。Lead 重写 ignored 分析脚本：显式 `--self-test`，fixture 留在唯一 ignored 目录，不修改全局 ROOT/FRAMES；正常分析先严格验证 CSV schema/1000 行/frame 身份/finite/逐帧闭合，再按未舍入数据计算差值。

Lead 运行自测 **exit 0**：已知方向/分母/mean 可加性，以及缺文件、行数、sidecar 数量、frame、非有限、负值、闭合、schema 八项拒绝均实际执行。正常分析 **exit 0，10 pair**，最大 mean 闭合误差 <4e-14µs；结果与 Lead 独立 JSON 逐字段比对通过。数据有效性接受，>3% 原因仍未定位，未接受性能 Gate。

## 7. PERF-B1 正式测量已完成（Lead，2026-10-04）

新 root present-profile-matrix/run-20261004-085153313-10956-40e952fc：
当前源码两树 build 0；40实际进程（两Validation×两backend×两profile×五轮），
每例1000样本+300warmup，native/judge均通过；四万帧对齐与分区闭合独立复核。
run-status/manifest/judge 与原始日志、CSV 均保留。完整事实见 PROGRESS §2DC。

OFF profiling-on 数据的五轮等权 present PFN span 均值：
fence 199.223µs、timeline 229.244µs；scoped waits 1.719/0.006µs，
residual 4.374/3.233µs。这里是 PFN 调用路径（含 layer/ICD/调用期等待），
不是 GPU 时间或隔离的 vendor driver 成本。ON/OFF必须分列。

profiling ON−OFF逐轮 wall median/P95差仍有明显波动；
这测量 profiling 开关，不是 legacy/Sync2 encoder对照。
新时点数据不能直接证明旧§2BJ encoder波动的原因。
已完成的是可信分段采样；内部耗时归因、>3% Gate仍待调查。
