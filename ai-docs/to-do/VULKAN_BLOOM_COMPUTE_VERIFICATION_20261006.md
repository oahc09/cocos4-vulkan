# Bloom 内容与 Compute 场景前提：有限验证

日期：2026-10-06。执行渠道：Lead，无 Orca Worker/native subagent。本轮没有 stage/commit/push。

结论：原 Windows `test-cases` 的隔离资源入口已接通。Bloom 八个实际 pass 与阈值内容对照通过；Compute 场景运行前提及 dispatch→storage→fullscreen 的有限内容链通过。发现并修复三处生产缺陷，分别保留断言、Validation 和内容红例。**T13 全族、性能和平台整体验收未通过。**

## 真实工程与资源前提

- 工程：`E:/AIProMax/github/cocos4-vulkan-test-project/native/engine/win64`；VS 树 `build/windows/proj`，Debug `test-cases`。所有最终正例使用同一当前源码 EXE，SHA256 `15b99cd7ccc27f2651e0fd07a26d43eacbffbe5755d76a04eedc97fb3866eca6`。
- 原 Debug Resources 的 `customPipeline=false`，缺少这两场景需要的 custom builder/effect-layout；仅菜单进入不构成 Bloom/Compute 路径证明。原默认资源包未替换。
- 新 `RealProjectScenePackage.h` 由真实工程 `main.cpp` 在平台启动前调用。环境 `CC_VULKAN_REAL_PROJECT_RESOURCE_ROOT` 缺省时不改 FileUtils；显式根要求绝对路径、完整包、customPipeline=true 和非空 effect-layout，成为唯一搜索根。旧 legacy 包实际请求被设备创建前拒绝，native exit1；wrapper exit0 不冒充 native 成功。
- `prepare-scene-pipeline.py` 生成隔离 Creator options/检查包，静态输出明确 `gpuContentVerified=false`。Compute 注册模块已由真实 main prerequisites 导入，无需伪造组件。Builtin 与 RaytracingWeekend 各使用真实场景/材质。
- Creator 3.8.8 从真实项目 cwd、安装 JS 引擎、纯 `configPath=...` CLI 构建资源。成功退出码为 **36**，不是0；原 `stage=build;configPath=...` 被本项目扩展解析错，仓库 cwd 的 Babel scope 也产生过 CJS/序列化错误，失败日志保留。原 programming cache 移至独立 quarantine，不删除资产；fresh-cache 构建得到完整非 null 的 Builtin settings。
- 资源使用安装版 JS 3.8.8，native 来自当前仓库；这是有限混合前后端验证，不宣称所有4.x/3.8兼容。Creator 生成的 information.json 保存后恢复原 hash；事前设置/三 native 文件清单的最终唯一变化是本轮 main.cpp 入口。Game/CMake 的既有 cache/Indirect 接线保留。未建立所有资产的事前字节清单，不能以设置清单冒称全资产 hash 核验。

## 三处生产修复与红例

| 文件 | 实际问题 | 最小修复及边界 |
|---|---|---|
| `pipeline/custom/NativeExecutor.cpp` | Compute queue 没有 raster 外层 viewport，end queue 的 `size()>1` 在真实场景断言；原 native exit3 | 要求 stack 非空，保留 queue 自己的 push/pop 与既有恢复循环；不关闭断言/制造假 viewport |
| `gfx-vulkan/VKDescriptorSet.cpp` | descriptor hub 按 access flags 将 STORAGE_IMAGE 的 GENERAL 改成 SHADER_READ_ONLY；真实04152/00344红例 | 更新 hub 后按 descriptor type 保持 GENERAL。读写标志不改变 descriptor type；不将所有 texture 强制 GENERAL，不加全局屏障 |
| `pipeline/custom/NativeExecutorDescriptor2.cpp` | `setBuffer()` 的整块 uniform 没被资源收集/构建消费；实际 GPU 球体参数32B全零，Validation干净仍内容错误 | 收集显式 uniform buffer，优先绑定整块；没有显式 buffer 时保留成员常量生成路径与 storage 默认 fallback。实际新32B参数与脚本逐字节一致 |

三个文件合计 +22/-3；每步实际重编相应 CU、重链 engine/test-cases，最后 `compute-uniform-build-exit.txt=0`。未改 shader/场景算法、vendored、通用 Vulkan 能力路由；没有新增 waitIdle/submit/wait 或关闭 Validation。这是正确性修复，不是 M4-B 同步优化。

STORAGE_IMAGE 的 GENERAL 依据：[Khronos VUID-04152](https://docs.vulkan.org/spec/latest/chapters/descriptorsets.html#VUID-VkWriteDescriptorSet-descriptorType-04152)。绑定 descriptor 合法不自动保证图像内容；第三条独立内容红例正是例证。

## 最终运行与 GPU 内容

四个正常进程均 native exit0、ERROR/VUID/HAZARD/JS0、正常 WM_CLOSE、唯一 allocator leak0，实际加载 Khronos Validation DLL；当前 Debug 编译启用 Validation 及其同步/BestPractices请求。正常原工程没有诊断 triad marker，不捏造此 marker。BestPractices advisory 仍保留，不声称 warning0。

| 正例 | 实际路径 / 有限结果 |
|---|---|
| compute-modern-final | timeline1/Sync2=1/Submit2=1/Dynamic policy1，真实 Compute 场景加载与正常退休 |
| compute-legacy-final | fence/timeline0/Sync2=0/Submit2=0/Classic override1，同场景正常退出；此案没有另抓完整内容帧，不扩称跨后端逐像素对照 |
| bloom-current-final | 原 threshold0.8 隔离 clean 包，现代路径，场景加载/运行/正常退出；无输入尝试 |
| default-current-final | 原默认 legacy 包、resource selector/prewarm OFF 正常运行与退出 |

三个最终 RenderDoc 捕获记录相同 EXE SHA。回放均 Complete、messages0，实际命令与图像如下；捕获正常关闭不代替上述独立 native 退休日志。

### Bloom

- 使用相同 clean 包的两个副本，仅序列化 threshold 从0.8改为 **0.05 / 1.0**，intensity1、iterations3、相机/材质保持一致。两个 pre-Bloom RGBA8 图800×600、1,920,000字节完全相同；八阶段 SPIR-V 也逐字节相同。
- 实际 Kawase dual 链：prefilter400×300 → down200×150 → down100×75 → down50×37 → up100×75 → up200×150 → up400×300 → combine800×600。GPU 纹理 descriptor producer/consumer 与实际输出资源逐项关联；GPU BloomUBO 的 threshold 与包一致。
- 低阈值 prefilter 有 RGB 内容，高阈值 RGB 全零；高阈值 combine 与原输入全字节相同。两 combine 有 **1,440,000 个不同字节**，后续 copy 解码通道后与 combine 完全相同。不是仅比较截图/调用计数。
- 每帧16 Indexed draw/11 Classic RP/78 Barrier2/1 Submit2，BeginRendering0。Dynamic policy ON 不意味着实际 Dynamic pass；资格门控保留。
- **滑条未验证成功**：此前真实输入触发 `windowManager.getWindow(event.windowId)==null` 的 JS getViewSize 错误；对应 red 日志保留，不把输入后的截图当阈值变化证明。本次包参数对照只关闭有限渲染内容，不关闭交互合同，也不修用户排除的 mask 层问题。

### Compute

- 一次实际 dispatch **100×150×1**，localSize8×4×1，写800×600 RGBA8 storage；实际 descriptor/layout 合法。一次下游 Indexed fullscreen draw 采样该资源。
- 实际绑定32B球体参数 `(0,0,-1,0.5,0,-100.5,-1,100)`；不是 shader创建、CPU值或 production key 自证。
- storage完整1,920,000字节与 fullscreen 输出在通道归一后**逐字节相同（垂直翻行）**。上部120行共96,000个背景像素满足场景射线背景 golden，容差1；球心实际 `(127,127,255,255)` 满足 `(128,128,255,255)` 容差1。
- 实际1dispatch/1Indexed draw/1Classic RP/9Barrier2/1Submit2，BeginRendering0。回放不能证明所有GPU/显示合成器内容或性能。
- 未覆盖全部 ray intersection、非workgroup倍数 resize（原 shader 未做边界检查）、设备丢失恢复/再入、完整资源族退休或平台。存储/全屏一致也不能独立证明所有射线算法正确，所以另保留有限背景/球心 golden。

## 共享判据、证据与后续

- `verify-scene-content.py <root> --self-test` 实跑 exit0：实际三帧 raw textures、bound CB、SPIR-V/descriptor 链及四个 native 终态共同判定；5个反例全部拒绝，包含实际修复前全零 UBO 捕获，及无dispatch/错threshold/断纹理依赖。
- raw根：`native/build/vulkan-modernization/win-bloom-compute-t13-lead/run-20261006-092224859505/`。保留 build、Creator/包 mutation、脚本、正常/红例 debug日志、PNG、RDC、inspect/raw、项目设置恢复清单。`content-judge.json` 记录本次成功结果，`terminal.json` 只代表这块有限终态。
- 首次 inspector 用过旧 RenderDoc CB API，API异常 JSON/脚本保留后改为1.44 `GetConstantBlock`；早期模板 ROOT 替换破坏环境名且 wrapper0、无CAPTURED，不计通过。部分早期 API修订 wrapper日志被覆盖，其旧 JSON/脚本已保留；最终三案独立目录。
- T13-03标有限内容完成、T13-04标有限内容通过/交互待验证；T13整体仍进行中。下一族是 Input Attachment/Multi Subpass 的真实工程合同，MSAA/D-S resolve/Secondary/FSR与异常/恢复仍分别待补。
- 性能正式采样/>3%、M4-A前置→M4-B、自然低版/Android/苹果-XR/温控仍未收口；本块没有重跑 Android/独立全量测试/性能矩阵，不授予任何整体 Gate。

复现入口与环境恢复说明见 [真实工程模块 README](../../native/tests/vulkan-real-project/README.md)。任务状态以 [收口任务列表](VULKAN_COMPLETION_TASKS.md) 为准。
