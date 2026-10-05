# mask-graphics 输入与 stencil 调查（2026-10-05，Lead）

结论：原场景收不到触摸的直接原因已通过真实运行单变量对照确认：监听节点 Canvas 的 UI_3D 层不在任何实际渲染相机的 visibility 中，`UITransform.hitTest` 全部返回 false。仅临时将运行中 Canvas 改为 UI_2D 后，真实鼠标拖动触发 touch/stroke，并出现可见刮除效果。**这是诊断对照，不是永久修复，也不是完整 stencil/GPU 内容验收。**

## 工程与证据

- 使用用户指定 `E:/AIProMax/github/cocos4-vulkan-test-project/build/windows/proj/Debug/test-cases.exe`，未使用独立 smoke 替代；未修改测试工程源码、场景、资源、cache 或 renderer。
- 执行渠道：Lead。所有 observer、CDP、窗口操作、运行、分析与本文记录均由 Lead 执行。本轮无 Orca Worker 新任务。
- ignored 证据根：`native/build/vulkan-modernization/win-mask-investigation-lead/run-20261005-151510567737/retry/`；最终汇总 `investigation-summary.json`。
- 实际 exe SHA256：`d6c5237a2c93bb622f14ac616d16bd9362fcc8bd4b00f358db4aee11ff0870a1`，不同于 §2DY 历史 exe；本轮未构建，不声称自己重编译。exe、NewComponent.ts 与 mask-graphics.scene 前后 SHA 全一致。
- 实际路径 timeline=1、sync2=0、submit2=0、dynamicRendering=0，加载 SDK `VK_LAYER_KHRONOS_validation`。本轮没有 Sync2 对照或性能采样。

## 输入证据链

1. 测试工程 `assets/cases/2D/mask-graphics/NewComponent.ts:9-11` 在所属 Canvas 注册 TOUCH_START/MOVE/END；move 回调 :33-35 依次 lineTo/stroke/moveTo。
2. 场景序列化 `mask-graphics.scene:307` Canvas layer=33554432（UI_3D），Mask 子节点 :459 layer=524288（UI_2D）；真实运行值相同。
3. `cocos/2d/framework/ui-transform.ts:460-471` 先按 camera visibility、swapchain window、systemWindowId 过滤；`mask-runtime-before.json` 保存实际打包运行函数源码，与该过滤一致。
4. 实际 Canvas 的三个 camera：Main Camera visibility=1822425087，Camera=524288，UICamera_backRoot=8650752。三者均与 Canvas layer 不交，均为 systemWindowId=1 且有 swapchain；排除仅凭序列化主相机猜测。
5. 透明 observer 转发原参数/返回值，只记录 hitTest、节点触摸与 stroke；通过自有 PID 的实际鼠标拖动，不直接调用场景 touch/stroke 回调。

| 对照 | Canvas layer | hitTest true / false | touch start / move / end | stroke |
|---|---:|---:|---:|---:|
| 原配置 | 33554432 | 0 / 34 | 0 / 0 / 0 | 0 |
| 仅临时层匹配 | 524288 | 65 / 0 | 1 / 30 / 1 | 30 |

- 原配置：`original-drag.json` 与 `03-mask-before.png` / `04-original-drag.png`。
- 临时对照：`layer-control-v2.json`、`layer-matched-drag-v2.json` 与 `07-layer-only.png` / `08-layer-matched-drag.png`。UI 相机本身未改。
- 截图中心 ROI=(200,250,1000,850)：原拖动改变0像素，临时对照改变60108像素；人工读取确认蓝色遮罩中出现圆头横向刮开区域，可见灰色背景。像素差只证明有限区域变化，不是逐像素 stencil golden。
- 恢复 Canvas 原层及7个 observer/监听器，原 hitTest/stroke/input touch/input mouse 函数与诊断全局恢复检查均通过（`restored-observers.json`）。真实返回菜单再入场景、原配置再次拖动，`10-reentered-original.png` / `11-reentered-drag.png` 同 ROI 仍0差；`reentered-original.json` 再次证明原层与 camera 关系。

## stencil 内容与结论边界

- 真实 Mask type=2（GRAPHICS_STENCIL）、inverted=true、enabled=true，Graphics stencilStage=6（ENTER_LEVEL_INVERTED）、lineWidth=100。
- `native/cocos/2d/renderer/StencilManager.cpp:145-158`：CLEAR_INVERTED 使用 NEVER/REPLACE，ENTER_LEVEL_INVERTED 使用 NEVER/ZERO；:132-138 内容阶段 ENABLED 使用 EQUAL 与当前层 reference/readMask。
- `native/cocos/2d/renderer/Batcher2d.cpp:555-557` 将 stencil stage 生成的 DepthStencilState/hash 带入批次。该部分是源码链证据，不冒称 PFN 捕获到本次 GPU stencil 参数。
- 临时层匹配后的真实绘制呈现刮除效果，支持本场景已有 stencil 路径可产生非空效果；不能宣称整个 Vulkan stencil 实现、嵌套/非反向遮罩、精确 stencil bits 或所有平台均正确。未抓 RenderDoc/AGI、未读回 GPU stencil buffer。
- 建议最小永久修复：在测试场景中将监听 Canvas 设为与目标 UI 相机匹配的 UI_2D，或将监听移到可命中的 UI 节点。不要为此扩大所有 camera visibility 或先修改 Vulkan stencil；永久场景修改后需重导出并用同一真实工程验证。本轮按“调查”范围未实施。

## 退出与未清零事项

- PID37696 正常 WM_CLOSE、exit0、547.72秒（诊断时间，非性能）；日志 VUID-vk、SYNC-HAZARD、游戏 JS Uncaught 均0，唯一 device memory leak=0。这是日志观测，不是活 validation 原子总数证明。
- **ERROR并非0**：有11条 `failed to get addresses Bad file descriptor`。输出点确认在 V8 inspector `inspector_socket_server.cpp:121` 地址枚举；该代码调用 uv_interface_addresses 后检查 errno，不以此证明具体系统错误根因，也未修它。所有 ERROR 行均为该消息。
- BestPractices 五族仍在：UndefinedToReadOnly、ImageBarrierAccessLayout、swapchain present-modes、ClearValueWithoutLoadOpClear、StoreOpDontCareThenLoadOpLoad。多族达到重复输出上限，不能以后续不输出推定警告消失；load/store 警告与 stencil 附件实际保留需求仍需单独调查。
- 首次启动遇 inspector 端口占用/无法连接及非目标场景，exit1，保留为未完成运行；首次脚本命名 inspect.py 遮蔽 Python 标准库，改名后才成功。启动期 scene 为 null 的 CDP 查询失败，随后查询确认 TestList。
- 第一版 layer-control 错用 global cc.input 导致 CDP TypeError，未改变 Canvas 层；`05/06` 图片与 `layer-matched-drag.json` 不作为匹配成功证据。修正为 System.import('cc') 后的 v2 才是有效单变量对照。CDP 诊断异常不冒称为游戏回调异常或忽略不记。

## Verification Gate

- G1 API：未全面验证；本轮无 Vulkan API 改动，有限运行未报 VUID。
- G2 生命周期：已验证有限恢复/返回再入/正常退出及泄漏0；GPU 全对象退休未独立取证。
- G3 同步：已验证有限运行无 SYNC-HAZARD；未改同步，不代表完整同步验收。
- G4 Validation：已验证 layer 加载及本轮日志无 Vulkan ERROR；BestPractices 仍在，全部日志 ERROR 非0。
- G5 RenderDoc/AGI：未验证；需抓帧核 stencil 附件、draw 与 PSO，才能给工具级内容结论。
- G6 平台：仅真实 Windows legacy 有限证据；本轮未覆盖 resize/minimize、Sync2、Android/苹果/XR，不授予平台或性能 Gate。
