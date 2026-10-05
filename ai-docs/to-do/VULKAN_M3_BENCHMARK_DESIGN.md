# Vulkan M3 benchmark 指标与实测边界

> M3-M5：Orca Worker 调查；2026-10-03 Lead 复核修订。原稿中的 timestamp 未查询、每帧固定等待一次、present semaphore 等于 backBufferCount 和提交数差等于在飞帧数均撤销。M3 未整体验收。

## 1. 已有数据与新增入口

| 指标 | 实际数据源 / 口径 |
|---|---|
| Host wait count/frame | 测试透明 hook `vkWaitForFences` / `vkWaitSemaphores`，计实际 API 调用次数，不是等待对象数。`present()` 依据 fenceCount 及实际 slot point 等待，次数不是固定 1；不将 status/counter 查询计作 wait。 |
| Host wait duration/frame | 上述两个 API 调用内的 steady_clock wall duration。acquire/present 阻塞另含在帧 wall 时间内。 |
| Fence object count | Pool.size() 保留 used count；新 allocatedCount() 返回实际创建且尚未销毁的池对象。设备只读 completionObjectCounts() 汇总全部槽池与 transport。测试再加当前 WSI generation 的 present fences。 |
| Semaphore object count | 全部槽 acquire 池、全部专用 queue completion semaphore，以及当前 WSI generation 的 renderFinished。后者由实际 swapchain image count 决定，不能用只增不减的设备 backBufferCount 顶替。 |
| Render Thread CPU | 本 harness 是单线程 frame flow，不启动实际游戏 render thread；GetThreadTimes 读取 kernel+user 的线程 CPU 时间。Windows 本机约 15.625 ms 粒度，单帧 P95/P99 不适合性能判断，保留原样样本和批次均值。CC_PROFILE 存在，但本次不声称已接入其导出接口。 |
| P50/P95/P99 | 每帧 wall/CPU/wait/GPU 数据 CSV，nearest-rank 百分位；脚本重新计算全部均值/百分位/最大值并核对日志。 |
| GPU frame time | 现有 VKGPUContext 的 vkGetPhysicalDeviceProperties 已保存全部 limits，包括 timestampPeriod（VKGPUContext.cpp:393/415）；queueFamilyProperties 已保存 timestampValidBits（:466-469）。新增真实 timestamp query pair 测量用户 clear CB 的 TOP→BOTTOM 时间，不包括 presentation/其他 transport CB。完成后读 64-bit+availability 并按有效位处理 wrap。 |
| frames-in-flight occupancy | 每帧真实 queue completion point，统计提交后尚未完成的帧 point；不能用 submittedValue-completedValue 代替，因为那是 submission 数。测试轮询会更新 tracker cache，其成本和影响包含在两模式中。 |

## 2. 测量模式

`vulkan_wsi_smoke --benchmark --frames 1000 --warmup 300 --validation on --csv <absolute-path>` 与正常 S1~S8 模式独立。初始化/window/FBO/query pool 在预热前；收尾等待和 CSV 输出在采样后。每帧恰好一次实际 submit，无新增 signal-only submit 或逐帧 waitIdle。

`verify-completion-benchmark.ps1` 默认固定 effective 1.3、预热 300、1000 个采样帧、五轮 fence/timeline 交替顺序，输出独立日志/逐帧 CSV/summary.csv。Validation OFF 必须使用单独配置 `CC_VULKAN_VALIDATION=OFF` 且 `CC_DEBUG_FORCE=ON` 的构建树；脚本不会改变引擎 validation 策略。记录实际设备/driver/timestamp 精度、client extent、image/slot 数、presentMode、maintenance1，并要求所有对比轮身份/呈现配置一致。

统计对象范围为 engine completion owners 与本测试当前 WSI generation，不包含外部对象或任意已退休 generation；稳定单窗口测量不重建。六个 queue 的专用 completion semaphore 即使空闲也计入。不要把 submission-fence 使用次数当对象数。

## 3. 实测状态与边界

Lead 的 Validation ON 五轮×两后端已运行通过（10000 个正式采样帧，所有 footer errors=0/failures=0、显存 0）。结果详见 PROGRESS §2AU；Validation OFF 五轮由 Orca Worker完成并经Lead复核（§2AV/AW）；实际游戏 cold/first-use 指标、完整 profiler/vendor counter、移动设备温控/画面/平台 Gate 尚待。

有限 color+depth clear、instrumentation、immediate present 的微基准不能作为真实游戏性能结论。线程 CPU 计时粒度过粗，轮间帧 wall 波动也不足以支持稳定提速结论。保留原始数据；不把对象数/调用次数下降宣传为 FPS 提升。

依据：[timestamp 写入规则](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdWriteTimestamp.html)、[结果与 availability 布局](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetQueryPoolResults.html)。

## 4. 分段调查补记（Lead，§2AW）

首次OFF数据部分median/P95差值超过3%，如实保留。新增五段wall计时与 `.csv.phases.csv`；脚本核对每段样本、frame闭合与统计，28夹具通过。ON/OFF各五轮分段数据保留，dispersion.json补常规median/总体stddev/MAD。present段为最大耗时及波动来源，但该段包含引擎收尾与实际WSI API，尚不能把根因归给其中某一项。观察点查询的instrumentation成本也随层状态变化。此轮没有性能接受结论，M3及平台/真实场景Gate仍待。

Android新getter已编入libcocos_engine.a；probe不调用该只读接口，最终.so正常GC，并不含该符号。OFF保留CC_DEBUG_FORCE=ON与gfx debug validator，与shipping Release条件不同。
