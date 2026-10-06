# 真实工程连续上传验证（2026-10-06）

执行渠道：Lead。当前范围：Windows本机有限上传正确性；正确性与正式采样已完成，慢尾调查仍未收口，不授予整体性能/平台或整体验收。

## 正确性终态

真实工程 `E:/AIProMax/github/cocos4-vulkan-test-project/build/windows/proj` Debug `test-cases`，当前诊断默认关闭。新增 `CC_VULKAN_REAL_PROJECT_VISIBILITY=stress|stress-wrong`。

- 每案24波，buffer大小64/128KiB交替；两个DEVICE输入各连续更新4次，真实Compute求和、copy到HOST、submit成功/完成后invalidate，完整逐word golden。
- 每波录制后resize输入A，提交前释放输入/set/PSO facade；实际manager transport CB与compute/readback CB分别提交一次，pending typed destroy=0。
- RGBA8纹理129/257×127交替，每张连续4次真实staging上传；真实CopyBufferToImage hook确认4次与形状，ImageToBuffer hook确认对应CB/图像/host/range；完成后invalidate，全图逐字节golden。纹理每波重新创建/销毁，不宣称同一图像跨GPU消费后再次上传或mipgen已覆盖。
- legacy、Sync2/Submit2、forced1.1/fence三正例，以及最后一波buffer/纹理各单值错误的实际负例：4/4 shared judge通过。正例native0，负例native1且只有两golden FAIL；每正例buffer检查2359296B、纹理2353056B，96/96管线/module/layout/set-layout销毁；live/post/leak0。buffer/texture分配的销毁以正常facade清理与最终VMA泄漏分证，不冒称逐个VMA destroy hook。
- 14个独立parser夹具通过；终态Complete/Pass/SourceUnchanged=true。

证据根：`native/build/vulkan-modernization/win-upload-stress-lead/formal2-20261006-1842/`。实际exe SHA256：`5de85421b436027be6df358f26c7e9e15b52c116d800139b2e343cd384d4154d`。源与judge原样归档为 `visibility-at-runtime.cpp` / `judge-at-runtime.py`；后续性能扩展不改记为这一轮已执行。

复现：

```powershell
python native/tests/vulkan-real-project/run-upload-stress.py --root native/build/vulkan-modernization/<新的证据目录>
```

## 过程失败（不覆盖）

1. 首次编译直接访问protected的CCVKDevice接口，C2248；改由公开Device接口调用。
2. 首次诊断纹理仅TRANSFER用途，但引擎会创建image view，真实VUID-04441；fixture补SAMPLED用途，未改生产实现或关闭Validation。预算达到10停止，原案不是通过。
3. 上传shape判据错误要求packed staging行；引擎合法按optimalRowPitchAlignment padding。原24波内容/同步/退休为零错误，但诊断96 FAIL；修正为上传允许rowLength>=width且height正确，readback仍packed与完整内容独立核验。

原证据在 `win-upload-stress-lead/20261006-183356/`、`formal-20261006-1840/`。

## 性能范围与状态

新增 `upload-perf` / `upload-perf-baseline`，同套对象复用1300批次（300预热、1000样本），每批4次×2输入更新→manager→Compute→copy→真实提交完成。clock覆盖上传批次host时长，GPU timestamp仅用户Compute/readback CB的TOP→BOTTOM，未含upload transport/呈现或整个游戏帧。shader/PSO创建在循环外。

首个ON探索进程native0、1000 CSV行、内容/Validation/leak0，但旧phase日志误报每轮resized=1（实际仅末轮resize），不当正式性能证据。该日志保留 `perf-on-probe/`，已改独立batch cycle与实际resized值。正式OFF同路径两版本、5轮交替配对已执行；下列版本分开记录。原始异常必须保留，不能把host批次时长或Compute CB GPU时间当游戏帧率。

## 验证边界

G1/G2/G3/G4：本机有限API、录制lease/目标退休、同步、实际Validation与全内容已验证。G5：本块没有新增RenderDoc回放，不把forwarding hook当RenderDoc；已有其他特性抓帧不自动扩为本块。G6：Windows三配置有限通过，Android运行/自然低版设备未验证；Apple/XR按用户要求暂缓。

## 最终正确性回归与清理修订

- `final-buffer-regression/`：当前扩展后的诊断源码实际build，6/6；`final-rendering-regression/`：8/8（legacy/Sync2×color/depth×2x/4x，四个精确post、内容/提交/退休/Validation判据全过）；`final-stress-regression/`：4/4及14/14判据夹具。三个根的EXE SHA一致为`7ebc4ba0221ffd90eba8b5463ca571e50b59c441b7cb741b6e3f2e0edab9f34f`。属于本机有限组合。
- 此后仅诊断退出安全性两处增量：run结束关闭profile/waitScope并清纹理借用句柄；query pool析构在owner仍活时先waitAllFences，覆盖提交后失败的清理，不增加被测循环内wait/submit或waitIdle。没有故障注入证明query失败分支已执行。
- `cleanup-safety-20261006/`：该修订后真实ON工程新build0；一个1300批次实际进程native0，严格1000样本+全内容+退休/Validation/leak通过。EXE SHA `96ab4fbc21c2de362f1e7959732122b471538c7445c14ac9f5d600acda6d7197`。前述18案属于修订前版本，不改记为修订后重跑。该清理增量之外生产代码未变。

## 正式采样版本与撤销的误归因

所有正式测量均实际测试工程OFF树，同一个Debug/test-cases.exe路径、文件名、相同诊断与输入，只替换BarrierManager::update实现。每版先build当前源码，构建失败拒旧exe；前后五轮交替、各300预热+1000有效批次、十独立进程。原生产源码精确恢复及当前exe恢复均有终态记录。原始数据保留，未将host或GPU异时钟相加。

| 数据根（均在win-upload-stress-lead） | 实际结果 / 限制 |
|---|---|
| performance-20261006-1850 | 10/10运行通过，batch P50配对中位数−23.39%；撤销该“优化收益”：循环内baseline模式字符串比较更多、输入生成计入且staging池增长，方法存在混杂 |
| bounded-performance-20261006-1855 | cached模式/输入生成移到计时外/完成后按lease安全reset staging池；10/10，batch P50−0.182%、P95+5.471%、P99+2.777%；全部样本实际staging来源只有1个VkBuffer，未把旧23%当收益 |
| bounded-controls-20261006-1900 | 同一7列协议binary五对10/10；P95自对照有−20.11%等大变化，不能因此证明原正向回归已解释 |
| scoped-performance-20261006-1910 | 13列协议、五CPU分段与wait API透明观察；10/10、14判据夹具、10000有效样本；每行五段和严格等于batch，wait不超过completion、失败0、staging来源1 |

最新13列协议配对中位数（current/baseline−1；先逐轮统计再中位数）：

| 指标 | P50 | P95 | P99 |
|---|---:|---:|---:|
| batch host | −1.097% | −1.319% | **+4.294%** |
| manager host | −26.623% | −24.296% | −14.286% |
| user compute/readback CB GPU | +0.078% | −0.364% | +0.177% |
| completion host | +0.966% | −1.473% | **+6.385%** |
| wait API | +0.362% | −1.919% | **+5.909%** |

这是有限同步上传批次，不是游戏帧/FirstPresent；GPU timestamp不包含upload transport、全队列等待与显示。原计划§16.7强制调查的是同GPU/Scene/条件下Median或P95 Frame Time回归>3%，不是P99自动FAIL。本块是同步批次而非Frame Time：P99+4.294%作为额外慢尾观察保留，不擅自新增正式拒收阈值；历史批次P95+5.471%与最新结果的差异仍需解释。运行合同PASS不是整体性能Gate PASS。manager局部下降也不能当整帧提速。

## 慢尾定位与剩余项

`cleanup-safety-20261006/analyze-tail.py`以最新13列原始十案重用严格judge，逐案1000行、每100行分块及最慢50行；每案五段闭合、top50块计数总和50，两个单变量损坏样本拒绝，实际exit0，完整未round JSON留存。

十案top50 aggregate中manager占0.21%~1.12%；completion占36.88%~90.87%，实际wait API占31.91%~89.75%，submit占4.12%~47.75%。占比是实际host阶段定位，不是driver/GPU/温控因果证明，不能用分位数相加解释P99。当前等待覆盖固定批次提交完成，不等于present completion。

仍待：>3%历史波动的进一步host等待/调度归因与接受理由、真实Android/自然低版GPU/游戏温控；新负载RenderDoc回放未做。Apple/XR用户已暂缓。不修改功能默认策略，不处理已知mask输入测试问题。

## 新版同binary自对照终态

`scoped-controls-20261006-1930/`：`run-upload-controls.py --fresh-build`先构建实际OFF工程当前源码，build0；同EXE/同路径/同mode五对十进程均native0、judge PASS，每进程1300批次/1000有效样本，最终Complete/Pass/SourceUnchanged/ExeUnchanged=true。实际EXE SHA `874124345022075d57036675b59ce8f063b1c19902c4a8b8a65144ab15b9acae`；cpp/judge/collector/runner原样存档。属于清理修订后的新版，不能改记为前后版本比较。逐pair固定second−first，奇偶执行顺序交替，不能把每个second当时间上后来一次。

| pair | host P50 | host P95 | host P99 |
|---|---:|---:|---:|
| 1 | +1.550% | +11.219% | +2.311% |
| 2 | −1.180% | −11.262% | −12.359% |
| 3 | +5.349% | +24.134% | +8.622% |
| 4 | −12.011% | −11.912% | −12.482% |
| 5 | −2.945% | +9.211% | +2.369% |

同binary batch P95波动远大于最新前后配对中位数；该观察反对“所有尾部变化都是新barrier导致”的简单归因，但不证明GPU、驱动、调度或温控中的哪项是根因，也不取消历史坏样本。compute/readback CB GPU配对中位数P50−0.104%/P95−0.461%/P99−0.135%，只针对该区间。endpoint传感器不是逐批次证据。

本轮已完成：连续上传有限正确性、组合回归、正式批次数据、分段闭合、慢尾定位和同binary控制。尚未完成：整体性能因果/接受结论、真实平台DoD。没有关闭Validation、增加生产wait/submit、改默认策略或提交git。BestPractices全部告警清零也不在本轮通过声明内。

清理修订后的实际日志追加11个离线判据检查均通过（1正+10单条件负例，含wait失败、wait越界、无调用却有wait时长、staging无界、closure、非canonical/UInt64溢出、NaN、缺行）；`cleanup-parser-fixtures.json`保留。它们是坏证据拒绝测试，不冒称实际Vulkan API故障注入。
