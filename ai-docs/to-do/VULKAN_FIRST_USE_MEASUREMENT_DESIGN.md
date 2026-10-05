# 首次帧/首次呈现计时入口设计（PERF-A0，只读调查）

日期：2026-10-04（Orca Worker）。零构建/零测试/零生产改动；供 Lead 实施独立 Win32 harness。所有 file:line+函数名来自当前源码实读；未证明处标"未调查"。

## 1. 入口链（wsi_smoke.cpp 实际调用顺序）

| # | 阶段 | 当前源码位置 |
|---|---|---|
| 1 | main 入口/CLI 解析 | `wsi_smoke.cpp:343` `main`（BenchmarkOptions :344） |
| 2 | FileUtils/output 目录 | wsi_smoke.cpp:356-359 |
| 3 | DeviceManager::create（含 VkInstance/VkDevice/物理枚举） | wsi_smoke.cpp:362；VkInstance：VKGPUContext.cpp:327 `vkCreateInstance`；VkDevice：VKDevice.cpp:448 `vkCreateDevice`（`CCVKDevice::doInit` 内） |
| 4 | window 创建 | wsi_smoke.cpp:87（SW_SHOWNOACTIVATE） |
| 5 | createSwapchain | wsi_smoke.cpp:410 `backend->createSwapchain`；VKSwapchain.cpp:442 `vkCreateSwapchainKHR`（recreate 同函数） |
| 6 | 首个 acquire | wsi_smoke.cpp:448（S1 循环首迭代 `backend->acquire`） |
| 7 | record/submit | wsi_smoke.cpp:455 `renderFrame`→`backend->submit`（VKQueue.cpp `CCVKQueue::submit`） |
| 8 | 首次真实 vkQueuePresentKHR SUCCESS | wsi_smoke.cpp:458 `backend->present()`→VKDevice.cpp:1159 `CCVKDevice::present`→:1204 `vkCCPresentFunc(...)`（vkQueuePresentKHR，或 Swappy 启用时 SwappyVk_queuePresent :1182） |

其它参与点：prepareWSISubmit（VKDevice.cpp:1032，per-submit restore/finish）；CC_PROFILE(CCVKDevicePresent) 钩子已存在于 present 入口（VKDevice.cpp:1161）。

## 2. 现有 warmup 排除的首次样本（不声称已测进程冷启动）

- completion_benchmark.h:20 默认 `warmup=120`、:171 `total=warmup+frames`：预热帧不入正式样本。正式脚本实际传入 300，不应把默认 120 当已运行配置；首次 acquire/submit/present 未单列。
- wsi_smoke S1（wsi_smoke.cpp:441-465）120 帧稳定流用于状态断言，非计时样本；预算 budgetCheck 周期性中断（:464）。
- pipeline_prewarm_smoke factory1/driver1 ns 是首个 compute PSO 创建的有限计时（无 warmup 剔除，但仅 compute、无窗口/present）。
- 结论：进程级冷启动（main→首 present API return）当前**未被任何 harness 采样**——未调查项即本设计目标。

## 3. 语义边界：API return ≠ 显示完成

vkQueuePresentKHR SUCCESS（VKDevice.cpp:1204）只是 present API 成功返回，不证明像素上屏/扫描输出时间。本块不提供显示时刻测量；不将“尚未实现测量”扩大为进程绝对无法观测。maintenance1 presentFence 是退休/操作完成机制，不作为首像素可见证明。

## 4. 独立 harness timepoint 定义（steady_clock ns，单 Win32 进程）

| TP | 名称 | 采样点 |
|---|---|---|
| T0 | main-entry | main 第一行（CLI 解析前） |
| T1 | backend-init-start | DeviceManager::create 调用前 |
| T2 | backend-ready | DeviceManager::create 成功返回并确认 Vulkan 后 |
| T3 | swapchain-ready | vkCreateSwapchainKHR hook 成功返回后 |
| T4 | first-acquire | 首个 acquire（vkAcquireNextImageKHR hook SUCCESS）返回后 |
| T5 | first-frame-submit | 包含本帧实际 VkCommandBuffer 的 submit hook SUCCESS 返回后（排除创建期 immediate submit） |
| T6 | first-present-api | 首个 vkQueuePresentKHR hook SUCCESS 返回后 |

## 5. hook 办法与身份链

- **Lead 复核更正**：VKGPUContext.cpp:115 的 volkInitialize 会重新加载全局入口，:337 volkLoadInstanceOnly 与 VKDevice.cpp:459 volkLoadDevice 继续覆盖 dispatch。初始化前直接替换 PFN 不能证明 instance/device 创建跨度；首版仅测完整 backend-init，单独 VkInstance/VkDevice 时长保持未完成，后续需独立的可靠采样入口。初始化后安装 swapchain/acquire/BeginCB/graphics-create/submit/present hooks，先真实转发、成功才记录，RAII 恢复。
- 身份链记录：swapchain handle（T3 返回）→ acquire 输出 image index（T4）→ command buffers/submit batch 计数（T5）→ presentInfo.pSwapchains/pImageIndices（T6）比对一致；固定容量（各 1~8）、溢出/NULL/意外 sType 记 invalid。
- 首版一次有效帧：acquire/present 非 SUCCESS 则失败，不发布完成样本，不靠重试替换首个样本。即便 SUBOPTIMAL 有有效图像也先正确清理并报告本次未完成。

## 6. FirstPSO 复用与 graphics 扩展

- 现状：prewarm harness 的 factory1/driver1（pipeline_prewarm_smoke 输出行 `prewarm timing ns`）是 compute 首建的有限 cold/restored 原始 ns，可直接复用为 compute-first-PSO 证据；**不得**由 driver cache 命中声称 create 次数下降（restored 仍两次真实 create，§2CT 边界）。
- graphics 最小路径参考 engine_smoke 中真实图形管线案例。VKShader.cpp 的 createShader facade 本身不编译；首次 gpuShader() 调用才触发 initGpuShader。若显式先调用 gpuShader()，该跨度单列 shader-compile，随后 factory/driver PSO 跨度不包含该编译；另报告从 shader-compile-start 到 factory-end 的 combined-first-graphics 跨度，不能虚称编译落在已结束的 PSO 区间。

## 7. 下一实施小块清单（Lead 实施，本块不实现）

1. 新 harness `first_use_smoke.cpp`（Win32）：修订后的 T0~T6 + graphics-first-use 子段；初始化后 hooks + forward-first。不修改引擎运行行为。
2. 正式脚本 verify-first-use.ps1：cold/restored × Validation ON/OFF 各至少 5 次独立进程（至少 20 正例，负例另列）；cold 目录必须无缓存，restored 使用对应 cold 保存的 validated payload。保留原始 ns/CSV，判据要求真实帧身份链与精确退出码。
3. 判据边界：main-entry=T0 起点；Validation ON/OFF 分列不合并；有限 clear/graphics triangle 不替代游戏 FirstPresent；真实移动温控/首像素可见不做声明（依赖外部条件）。

## 8. 未调查/未知

- Swappy 启用路径（CC_SWAPPY_ENABLED）下 T6 语义变化——未调查。
- presentFence（maintenance1）等待作为"present 完成"代理的可用性——未调查。
- 驱动内部成本不能仅用当前 CPU 区间拆分归因；单独 VkInstance/VkDevice 时长、整进程启动前段、graphics shader-variant hitch 等仍未实现。

以上修订归属 Lead（2026-10-04）；Worker 初版“初始化前六 hook/五进程足够/编译必属于 PSO factory 段”的设计结论撤销，未进行构建运行。
