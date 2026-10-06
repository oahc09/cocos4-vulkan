# 真实 Windows 工程性能收口（2026-10-06）

执行渠道：Lead。T05 本机有限测量与调查完成；Lead 接受当前 Windows 路径的 M4-A 前置，允许有界 M4-B 实施。数据判据与该人工判定分开，整体性能/平台 Gate 未通过。

## 当前完成的测量

| 证据目录（均在 native/build/vulkan-modernization 下） | 实际执行 | 终态 |
|---|---|---|
| win-real-performance-matrix/run-20261006-132100 | 同源真实 test-cases Debug，Validation ON/OFF × legacy/modern × 5 轮 × cold/restored；每进程 300 预热、1000 有效帧 | 新构建两树 exit 0；40/40 进程及共享判据通过，源码冻结 |
| win-real-performance-controls/run-20261006-141100 | Validation OFF，两策略各 5 对相同策略 A/B，独立进程、独立空应用 cache；300/1000 | 20/20 通过，复用上一项当前 EXE，源码冻结 |
| win-real-wait-identities/run-20261006-143700 | ON/OFF × legacy/modern，实际提交/等待的 semaphore、timeline 值及 slot 归属；300/1000 | 新构建两树 exit 0；4/4 通过，4000 行归属核对通过 |

以上均为用户指定 Windows 测试工程；实际注册 atlas-compress，800×600、FIFO presentMode=2、显示 60Hz、requestedImages=3。每有效帧真实 3 draw、1 submit、3 个提交 primary CB 的 GPU 时间戳区间。legacy 为 timeline+legacy barriers/submits+Classic；modern 为 timeline+Sync2+Submit2+Dynamic 政策，场景资格回退不被冒称 Dynamic draw。

所有进程均正常退出，错误/live/post/泄漏为零。正式矩阵每对暖进程真实 pipeline-cache initialData 精确等于冷进程验证后的 65694 字节 payload；不声称驱动命中减少 create 次数。源文件、EXE、原始 debug log、启动 QPC、CSV、前后只读 NVIDIA 采样及每案判据均保留。

## 正式统计与尚未通过的条件

正式 `paired-analysis.json` 保留 20 对现代−legacy 的逐轮 signed mean/median/P95/P99、stddev/MAD 与启动指标；12/20 对 wall median 或 P95 正向超过 3%。不只用约 16.67ms 的平均帧周期授予通过。

同策略 `control-analysis.json`：8/10 对绝对 median 或 P95 差超过 3%，证明不存在代码/策略变化时也会出现该量级分位数波动。这不证明迁移没有性能影响，也不证明温控或驱动内部根因。

部分进程出现稳定三帧节奏：OFF legacy 第 3 轮 cold 的 lag-3=0.997267、lag-6=0.996621；按 frame%3 分类的周期均值为约 6.138/40.668/3.160ms。不同进程会进入不同节奏，不能将不同节奏直接解释成单个 draw 的 GPU 耗时变化。

`identity-analysis.json` 的 4000 行实际证明：3 个 slot；present 后等待的是 `(slot+1)%3`；每次实际 timeline wait 都与该帧成功 signal 属于同一 semaphore，目标值为当前 signal−2。每帧提交的唯一等待匹配实际 acquire semaphore。这里没有等待本帧提交的 fence；timeline submit 的 fence 为 NULL。

| 案例 | 1000 帧中 timeline wait 次数 | 每次等待均值 µs | 提交 CB GPU 区间均值 µs |
|---|---:|---:|---:|
| ON legacy | 356 | 43194 | 100.766 |
| ON modern | 670 | 19310.820 | 102.673 |
| OFF legacy | 1000 | 14571.146 | 103.792 |
| OFF modern | 1000 | 14547.494 | 103.080 |

这些身份和耗时把 host 等待定位到正常 slot 复用边界；短 GPU 区间不覆盖 acquire 等待、driver queue、合成器或可见像素，因此不能据此宣称 GPU 空闲、FIFO 是全部根因或整体 non-regression。

## 启动指标与测量限制

每进程保留 launch-to-first-present-API、诊断入口到首个 present、backend、实际 instance/device API、cache 配置、shader 编译/反射/module、graphics factory/driver、首个主线程帧、最大材质创建与编译区间。冷暖各自 n=5；不是 OS/驱动全局冷启动。正常调试采集器开销包括在内，首个 API SUCCESS 不等于首像素可见。

GetThreadTimes 的单位为 100ns，但本机实际分辨率较粗，单帧 0 合法；不使用 wall−CPU 推导休眠。NVIDIA 进程前后 P8/210MHz 与温度读数不是采样过程温控稳定证明。GPU 查询在正常退出完成等待后读取，不增加逐帧 wait/submit/waitIdle。

正式测试的 29 份精确源码已按 SHA 归档至 `win-performance-t05-lead/measured-source-snapshot/`；之后仅新增等待身份诊断和独立呈现模式实验，旧数据不冒称由新源码运行。两个生产文件的增量 EOL 整理有 `eol-cleanup.json` 证明标准化后内容与测量源码一致。

## 下一步及验收边界

独立、显式 opt-in 的 Immediate 呈现模式短测已在 `win-real-immediate-smoke/run-20261006-continue` 完成：ON/OFF × legacy/modern 四案均 actual presentMode=0、exit/error/leak0；40有效帧/10预热仅用于接口与路径核验，不作为正式统计。使用正常 swapchain 模式选择和 pNext 构建；默认与 XR 策略保留，不改 submit、barrier、slot 数或生命周期 signal。

`win-real-immediate-encoder/run-20261006-continue` 已完成 OFF、legacy/encoder ×5轮×cold/restored 的20进程，300预热/1000有效帧，20/20通过且源码冻结。两策略均 Classic，encoder 仅打开Sync2/Submit2，避免将 Dynamic 双PSO创建成本当作 M4-A 本身。全部实际 presentMode=0，该组20000帧 timeline wait=0。它用于调查 FIFO 对测量的影响，不替代正常 FIFO 游戏验收。

| 缓存条件 | 五轮配对变化的中位数：CPU frame median | CPU frame P95 | GPU提交区间 median | GPU提交区间 P95 |
|---|---:|---:|---:|---:|
| cold | +0.836% | +0.195% | +0.033% | −0.031% |
| restored | −1.315% | −0.816% | +0.099% | −0.062% |

这是逐轮 paired-percent 的中位数，不是将5000帧合并后重新定义阈值。10个GPU配对的 median/P95 正向最大变化均小于0.3%。原始CPU单轮仍保留：cold第1轮 median +5.72%/P95 +3.24%，cold第5轮 +17.93%/+13.50%，restored第5轮 P95 +3.29%。因此没有声称所有单轮都低于3%。

补充 `win-real-immediate-controls/run-20261006-continue`：相同encoder策略、相同cold条件、独立进程A/B的5对共10进程，10/10通过且源码冻结。第3对CPU median/P95为 +5.62%/+11.53%，第4对 +3.65%/+5.81%；2/5对超过阈值，无代码或策略切换。共有32次短timeline wait（总计0.6157ms）分布于前三个进程，其余为零，不能将“20进程实验无wait”扩大到所有Immediate运行。

离线分析曾错误要求无wait的帧也有wait-slot证据，修正为仅校验实际wait的next-slot/signal−2；原始数据未改变，红日志保留。控制分析也撤销“Immediate必定零wait”的假设，按真实计数记录。

## M4-A 前置接受结论（本机、有限）

原计划§16.7规定超过3%必须调查、给出profile证据和接受理由，不是自动失败。本轮接受理由为：

1. 正常FIFO的大幅逐帧分位变化主要涉及已实测的三slot completion等待重分布；同策略对照亦可复现，不能把它直接归为迁移开销。等待归属和模式干预提供直接证据，不将可见像素时间或driver内部根因当作已测量。
2. 隔离M4-A的Immediate五轮中，CPU median/P95的配对变化中位数均低于3%，GPU全部10对变化低于0.3%；相同编码器控制组也出现较大的CPU分位波动。未发现重复、持续的编码器相关吞吐下降。单轮17.93%的host变化及未解释的driver/调度内部因素仍是测量限制，不声称已证明全部抖动根因。
3. T01~T04/T13的本机路径/内容合同以及本轮40000正式帧和20000隔离帧保持实际submit=1/draw=3/GPU区间=3，错误和泄漏零；语义迁移没有用额外wait/submit掩盖问题。

据此，Lead对当前Windows实际工程/该静态场景的M4-A non-regression作有限接受，允许进入T07。不是统计等价性证明、所有游戏/平台性能承诺或全计划验收；不得删除原始超阈值记录，不以该接受放行Android/Apple/XR条件。M4-B修改必须继续做内容、Validation、生命周期及同条件性能回归，若产生真实回归则停止并修复。

M4-B只在上述本机前置接受后实施；不削弱 completion signal 的 ALL_COMMANDS 生命周期语义。Android 可用授权设备、Apple/XR 和移动温控仍各自保留外部条件，Windows 结果不替代。

G1/G2/G3/G4：本机有限合同与实际路径已核验；G5：既有真实场景抓帧及本报告有限测量，不是性能 Gate；G6：Windows 有限回归，Android/Apple/XR 实机未完成。总体未验收。
