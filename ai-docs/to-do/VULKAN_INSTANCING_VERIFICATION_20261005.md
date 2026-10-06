# 真实 Windows 工程 GPU Instancing 有限验收

2026-10-05，执行渠道：Lead。T13-02a 已完成（有限）；T13、M0～M7 整体验收仍未通过。没有委派 Orca Worker/native subagent，没有新增生产代码、修改场景资源或 Git 提交。

## 工程与源码合同

- 使用用户指定的 `cocos4-vulkan-test-project/build/windows/proj/Debug/test-cases.exe`，场景为 `assets/cases/scene/dynamic-batch/instanced-color.scene`。沿用上一块当前源码 Debug 构建产物，不重复构建：exe SHA256 `6db8457a3187863a3f1531f609891c0246538ccc0a79c2ee1004e427fe9dacf7`。真实构建日志为上一块 `win-render-features-t13-lead/run-20261005-222623337071/build-fixed.log`。
- `batch-tester.ts` 默认 count=20，每批十组、每组十个模型，即生成 2000 个方块；slider 改变 count 并新增/移除节点。每个方块 y 随时间正弦变化。
- `instanced-color.ts` 按每个模型的 y 计算 HSV：`h=(y+1)/2, s=0.5, v=1`，经 `Color` 的 Uint8Clamped 量化与 `Color.toArray` 写 `a_color_instanced`。材质实际 `USE_INSTANCING=true`；自定义 fragment 将实例颜色直接输出。
- 原场景 ExtensionDetection 的“蓝色方块”提示不是此材质的内容合同；蓝色是非 instancing shader 分支。验收使用真实实例属性和输出，不按这条旧提示误判彩色结果。
- `InstancedBuffer` 的 MAX_CAPACITY=1024；世界矩阵三列的 w 保存平移，worldY 位于实例记录 byte28。颜色位于 byte64，每条记录 80 字节。

## 实际执行与退休

所有 raw、脚本、PNG、RDC 和 JSON 位于：

`native/build/vulkan-modernization/win-instancing-t13-lead/run-20261005-231927836181/`

| 终态 case | 实际 API / 路径 | 正常退出 | Error / VUID / HAZARD / JS | 设备泄漏 |
|---|---|---|---|---|
| modern-retirement | effective1.3；timeline/sync2/submit2/dynamicPolicy=1，classicOverride=0 | 0 | 0 | 唯一 0 bytes |
| legacy-retirement | effective1.1；fence/legacy submit/legacy barrier/Classic | 0 | 0 | 唯一 0 bytes |

- 两 case 日志实际加载 `VK_LAYER_KHRONOS_validation`，prewarm 为 OFF。forced1.1 是策略回退测试，不是自然低版本 GPU 证明。
- `legacy-retirement` 留有 2000→1000→3800 的截图证据、resize/minimize/restore 后彩色方块，随后正常退出。`count-up.png` 拍在事件处理途中只显示 1100，最终 `restored.png` 才是 3800，不能以拖动目标坐标代替实际数量。
- modern 初次运行有 2000→1000→3800、resize/minimize/restore 截图和错误0；其关闭被脚本强制结束，因此不当退休通过。`modern-retirement` 是独立同源重复，默认 2000 个方块正常退出；不声称重复案包含全部增长/重建流程。
- 菜单返回再入的初次截图仍是菜单，未取得再次完成加载的画面，本块不勾选 Instancing 再入合同。已有 T08 的 RT 循环不冒充这一特性再入。

## G5 实际帧与内容

`renderdoc-instancing/instancing_frame7187.rdc` 捕获真实 Vulkan，回放 `replay.json` Complete/Pass=true。UI 场景确认后才触发捕获，不把菜单或策略开关当特性执行。

| event | 实际 API | indexCount | instanceCount | divisor / stride | pipeline |
|---|---|---|---|---|---|
| 34 | vkCmdDrawIndexed | 36 | 1024 | 1 / 80 bytes | ResourceId::29958 |
| 38 | vkCmdDrawIndexed | 36 | 976 | 1 / 80 bytes | ResourceId::29958 |

- 两 draw 实际使用 `R32G32B32A32_FLOAT` 的 `a_color_instanced`，perInstance=true；各自实例缓冲完整读回（不是仅检查 draw 计数）。firstInstance=0。
- 全部 2000 条记录用同帧 worldY 独立重算场景 HSV 与 Color 量化：mismatches=0，最大误差 `2.9685450542160652e-08`，最终容差 `1e-6`。完整逐实例 expected/actual 保留于 `instance-rows.json` 和两个实例缓冲 `.bin`。
- 677 种实际实例属性颜色；第二个 3D draw 后、UI 绘制前的 800×600 `B8G8R8A8_UNORM` 输出中有 27444 个像素精确匹配实例颜色，匹配 652 种不同颜色。遮挡等原因不要求每个实例都有可见像素。完整输出 `instanced-output.bin` 与颜色 histogram 保留。
- 实际全帧 11 个 Indexed draw、3 个 BeginRenderPass、0 个 BeginRendering、11 个 PipelineBarrier2、1 个 QueueSubmit2。Dynamic 策略开启但本帧实际是 Classic，资格回退保留。回放 debug messages 没有 VUID/HAZARD。
- **Indirect draw 实际 0 次**，因此不能将本块提升为 Indirect Draw 验证。`instanced-skinning`、全部 instancing 变体与其他平台也不包含在此次完成范围。

## 失败记录与判据

- 初次 modern/legacy 的关闭等待只有15秒，均触及脚本 TerminateProcess，exit=60929，未产生唯一泄漏0行；`judge.py` 明确拒绝这两案。错误0不等于正常退出。
- 新建独立目录，仅将采集器 WM_CLOSE 后的等待从15秒改为90秒，保持程序、场景与 Vulkan 策略不变；两个重复案最终自行退出0。未增加 GPU waitIdle/submit、改变引擎关闭或放宽错误/泄漏判据。
- 首个回放入口未产生 JSON，停止的仅为本块自有 `qrenderdoc --python inspect.py` 进程；保留 `inspect-initial.py`、停止记录和非零 exit。改成显式证据根目录后检查/完整回放成功，不对未捕获的异常类型作因果断言。
- 最终 `terminal.json` Complete/Pass=true，所记录引擎源、exe、六个场景输入的 SHA256 前后一致。共享原生判据同时检查实际路径、Validation、shader、正常退出与唯一 leak0；旧强制结束两案仍是 False。

## 门槛与下一步

| 门槛 | 本块状态 | 范围 |
|---|---|---|
| G1 API | 有限通过 | 实际 indexed instancing、属性/步长/divisor、有效 shader |
| G2 寿命 | 有限通过 | 同源现代2000、Classic增长到3800的正常退出/泄漏0；不包含全部 pending 异常与再入 |
| G3 同步 | 有限通过 | 该帧 Barrier2/Submit2、实际属性/输出与 native sync错误0；不是 M4-B 优化验收 |
| G4 Validation | 有限通过 | 两正常退出原生案；BestPractices advisory 保留，不宣称 warning0 |
| G5 GPU 内容 | 有限通过 | 2000 条同帧属性公式、两实际实例 draw、652种匹配输出颜色 |
| G6 平台 | Windows有限通过，其余待验 | RTX4060 Laptop；没有 Android/自然低版/Apple/XR 运行 |

T13-02b Indirect 仍待建立真实工程验证入口：当前 assets 的显式 `indirectBuffer/drawIndirect/dispatchIndirect/BufferUsageBit.INDIRECT` 搜索未找到调用，生产 `VKCommandBuffer::draw` 的间接分支已有，不把缺测试入口称为功能不存在。其后继续 Compute/Bloom/其他渲染族，T05正式性能与 M4-A→M4-B 顺序不变。
