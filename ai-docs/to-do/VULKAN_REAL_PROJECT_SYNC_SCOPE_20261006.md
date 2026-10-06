# M4-B 本机首批同步范围验收

日期：2026-10-06。执行渠道：Lead。结论：已知 buffer 上传资源化及已知 Dynamic 后续附件访问缩窄，完成真实 Windows 工程有限正确性验收；不等同全平台、全部负载性能或整体验收。

## 实现与回退

- `CCVKGPUBarrierManager::update`：有真实句柄、物理范围、声明消费信息的 buffer，使用独立 whole-physical-buffer 屏障。各资源 access mask 独立；未知消费/范围/句柄保留原 global 回退。没有添加 descriptor 自动消费推导，compute fixture 显式声明其输入消费。原有资源引用保持，不新增 byte-use slot lease。
- Dynamic post-render：候选门控已要求颜色附件及 resolve 的 next access 为 COLOR_WRITE，D/S 附件及 resolve 为 DS_WRITE；分别使用 COLOR_ATTACHMENT_OUTPUT/COLOR_WRITE 与 EARLY|LATE_FRAGMENT_TESTS/DS_WRITE。固定功能 depth resolve 的源 COLOR 阶段/access 保留。layout、subresource、barrier 数量和提交拓扑未变。
- 历史 producer 不明的 discard pre-render 保留保守范围；不凭 layout 猜历史使用。completion signal 的 ALL_COMMANDS 生命周期语义未缩窄。没有新增 waitIdle、submit、功能默认开关或关闭 Validation。
- 图像屏障的同步与访问范围分别由 stage/access 指定，并局限到实际 image/subresource；依据 [Khronos VkImageMemoryBarrier2](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageMemoryBarrier2.html)。本次访问选择来自实际候选门控，不由布局名称单独推导。

## 真实工程红绿证据

主工程为用户指定的 `cocos4-vulkan-test-project`，实际 `build/windows/proj` Debug `test-cases`。每次核心变更后先构建当前源码，再运行；不使用旧 exe 替代新源码。

| 证据 | 结果与限制 |
|---|---|
| 旧 buffer global 红例 | native 1；三波各 global=1/resources=0，完整 golden 正确，仅资源化断言失败 |
| 最新 combined buffer 六案 | legacy、Sync2/Submit2、1.1/fence、未知消费回退 native 0；wrong-input 与 bad-mode native 1，按负例判据通过 |
| 三波上传提交身份 | 每波实际 manager transport CB 与 compute/readback CB 非空且不同；各成功转发一次；256 字节完整 golden、resize/drop 后旧 recording 有效 |
| buffer 退休与判据 | 12/12 管线相关对象实际销毁；live/post/leak 0；23/23 判据夹具通过；不把 VMA 缓存的 global hook 当 buffer 销毁计数 |
| 旧 Dynamic post 宽范围红例 | 四个真实 post，precise=0/broad=4；内容全部正确，精确范围判据拒绝 native 1 |
| post 缩窄八案 | legacy/Sync2 × color2/color4/depth2/depth4，8/8 native 0；每案 posts=4/precise=4/broad=0/failures=0，关联实际 CB 与 owned image 快照 |
| post 内容与退休 | color uniform/edge 独立全字节 golden；depth/color 双 golden 及互差均 0；每案管线 created=4/destroyed=4，Validation/live/post/leak 0；8/8 scope 判据反例拒绝 |

首个 post 诊断误用了 Submit2，超出现有 resolve fixture 的 legacy-submit 观察边界，运行被拒；不当作宽范围红例。随后使用 legacy submit、Sync2 barrier 取得实际宽范围红。脚本第一次修改证据目录字符串未命中，创建已存在目录被拒，没有启动进程或覆盖日志。所有失败原样保留。

证据根（均在 ignored 区）：`native/build/vulkan-modernization/win-visibility-t07-lead/` 下 `red`、`final-combined`、`post-red`、`post-red2`、`post-green`。combined 与 post 两矩阵当时使用同一 exe SHA `ddb9cf0e271c492b024e118bb36d034db638ee5f864b40ff358b93197d6f2c25`。

之后将 scopes 参数比较改为无分配的 `strcmp`，避免 `noexcept` 前置检查构造 string；重新构建并测正常 Sync2/color2、未知值、缺少 resolve 前提。正常 native0，两拒绝在 backend 初始化前 native1，记录于 `preflight-final/disposition.json`。原“空值拒绝”夹具失败：Windows `SetEnvironmentVariable` 将空值作为删除变量，实际 child value 为 None/ERROR_ENVVAR_NOT_FOUND。该运行只作为默认OFF/native0内容证据，不称空值拒绝通过，不抹去原测试预期错误。

共享 scope judge 又补强 triad 必须唯一且 ENABLED，离线重判8案均通过、3个 triad 反例拒绝；原运行时 judge 精确字节已留 `post-green/judge-at-runtime.py`，补强重判是离线执行，不冒称新的GPU运行。

## 性能调查与有限接受理由

原前后五轮 cold/restored 共20进程（每进程300预热+1000有效帧）数据合同通过，但 CPU median 配对变化中位数 cold +5.48%、restored +4.59%，超过3%，没有立即接受。原 baseline 使用不同 exe 名称，属于混杂因素。

同一旧 binary 的不同名称五对控制，CPU median 变化为 +1.05%、+9.34%、−7.84%、+8.15%、−7.31%。双向结果不能证明名称效应或驱动/温控根因。

随后两版都加入相同显式测量，使用完全相同 exe 路径/名称与运行库，再执行五轮 cold/restored 共20进程、20000有效帧；所有 native/共享判据通过，源码冻结，当前 exe 精确恢复。差异源码仅位于 manager update。

| 同路径五对变化的中位数 | cold | restored |
|---|---:|---:|
| CPU frame median | −2.49% | −6.36% |
| CPU frame P95 | −1.67% | −2.33% |
| GPU submitted-CB median | −0.066% | 0.000% |
| GPU submitted-CB P95 | −0.031% | +0.031% |

每个缓存状态仍各有一轮正向 CPU median/P95 超过3%，最大约+10.30%，原值及 P99 保留。GPU 每对 median/P95 变化绝对值均低于0.3%。manager 自身 median 增量的五对中位数 cold 400ns/restored 500ns，不能用其百分比替代 frame 指标。

直接测量证明全部20000帧的 checked buffer 为0、实际生成 memory/buffer/image barrier 为0；每帧只检查三个静态图像。该稳态负载没有执行上传资源化分支，所以不声称上传优化提速；也不能把原 CPU 差异归因到未执行的分支。新同路径结果没有复现持续正向 frame 回归，但不是统计等价性或全部 host 波动根因证明。

Lead 据此有限接受已测静态 Windows 帧周期的 non-regression，并保留未归因的单轮变异。上传密集负载、全部 Dynamic 负载及自然低版设备的性能不由此通过；post 缩窄没有单独被该 Classic 稳态采样执行，只有上述内容/同步/拓扑证据。本机首批实现完成，不授予整体性能 Gate。

raw、CSV、P50/P95/P99、signed 配对与 metadata：`performance/paired-analysis.json`、`basename-controls/control-analysis.json`、`visibility-profile/same-path-formal/paired-analysis.json`。采样期间无并行构建或其它测试 GPU workload。

## Android 与仍需的条件

当前源码 `native/tests/vulkan-android-build/build.ps1 -Jobs 4` exit 0，共16步；包含 `VKCommands.cpp`/`VKCommandBuffer.cpp` 等真实编译，`libcocos_engine.a` 和 `libvk_android_probe.so` 最终链接成功。日志 `android-t07-build.log`。既有编译警告保留，没有冒称 warning 0。

`adb devices -l` exit 0，设备列表为空。Android runtime/自然低版 GPU/恢复/温控，以及计划适用的 Apple/XR 构建和设备运行仍依赖可用验证环境。T12 整体验收不能关闭。未 stage/commit/push。
