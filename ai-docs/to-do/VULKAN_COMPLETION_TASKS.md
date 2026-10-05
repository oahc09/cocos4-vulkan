# Vulkan 现代化收口任务

目标：完成原 `Cocos_Vulkan_1.1_to_1.3_Modernization_Task_Plan.md` 的必需实现与 DoD，不把阶段性提交、有限 smoke 或编译成功等同最终完成。

更新日期：2026-10-05。当前执行渠道：Lead。状态：已完成 / 进行中 / 待执行 / 等待前置 / 依赖外部条件 / 验证失败。

## 执行约定

- 工程验证直接使用 `E:/AIProMax/github/cocos4-vulkan-test-project/native/engine/win64` 与 `build/windows/proj`，目标 Debug `test-cases`。独立 CPU/smoke 仅补充缺口。
- mask Canvas 层不匹配为用户确认的已知测试问题，排除，不修场景、不算 Vulkan 阻塞。UI 输入诊断已结束。
- 保留既有实现与回退边界；FSR/复杂 secondary/XR 等未验证路径默认 Classic 符合计划，不为“全部完成”强开。graphics manifest 扩展/GPL 等可选项不加入硬完成条件。
- 每项完成须记录当前源码、命令/运行配置、实际路径、退出/错误、证据目录与限制。已有有效证据复用，失效或未覆盖部分才重跑。
- 每次只保留一项“进行中”；发现具体红例先修复并回归，再完成该项。M4-B 必须等待 M4-A 的正确性/性能前置，不绕过。
- 不自动 stage/commit/push；不关闭 Validation 或用 suppress 掩盖真实问题。外部设备不足如实保留，不能把目标全部完成标成 PASS。

## 任务状态

| ID | 任务与用户得到的结果 | 完成条件 | 状态 | 证据/下一动作 |
|---|---|---|---|---|
| T00 | 核对原计划、现有实现及剩余 DoD | 区分未实现、未验证、可选范围和外部条件 | 已完成 | 计划§12~23、现有验收矩阵、PROGRESS§2DZ；本文件记录执行目标 |
| T01 | 在真实 Windows 工程运行版本与回退矩阵 | 当前 Debug 构建；1.1/1.2/1.3、fence/auto、legacy/Sync2/Submit2、Dynamic opt-in/Classic fallback 各实际路径核验；红例单独记录 | 已完成 | T01R后8配置+fence/Sync2补例9/9，路径/场景/窗口/退出/Validation与泄漏均过；非全平台/性能Gate |
| T01R | 修复真实1.1/fence buffer重复上传WAW | 同一真实工程红→绿；legacy/Sync2及版本回归；不增加waitIdle/submit或关闭Validation | 已完成 | 真实1.1/fence红→绿，9配置同新exe回归；仅VKCommands.cpp补目标range及实例复制依赖 |
| T02 | 处理真实工程 BestPractices 警告 | 按 layout/access、load/store、WSI 分族；真实缺陷修复；无需修改项提供依据及边界，不仅屏蔽输出 | 已完成 | 三族修复实跑及菜单GPU红→绿；两族layout/access按规范分类保留，不宣称warning0；见VULKAN_BEST_PRACTICES_DISPOSITION.md，首次有效内容另列T04 |
| T03 | 完成四档同场景图像对照 | Baseline1.1/Enhanced1.2/Modern1.3/forced fallback 同场景、同尺寸；golden/image diff及阈值；实际命中的 Classic/Dynamic 分开证明 | 已完成 | 当前源码四档+Sync2/Modern完整策略6配置；atlas/RenderTexture静态主体ROI阈值0全部相同，2d3d有限人工核查；真实Dynamic执行仍由T04分证，不把政策ON算draw |
| T04 | 用 GPU 抓帧核对渲染内容与依赖 | RenderDoc/AGI 实际 pass/draw/PSO/descriptor/layout/load-store/resolve/stencil 内容证据；截图不替代抓帧 | 已完成 | 菜单STORE/stencil红→绿，Modern 2d3d/RT真实PSO-descriptor-Barrier2-Submit2与Classic回退已捕获；RT首次内容24帧回放完成，初始内容未定义合同如实保留，见VULKAN_REAL_PROJECT_GPU_CAPTURE.md |
| T05 | 收口性能、冷暖 cache 与 >3% 调查 | 同设备同场景固定warmup，≥1000有效帧×≥5轮、P50/P95/P99；FirstPresent/FirstPSO/启动分开；回归有证据与接受理由 | 进行中 | 已调查实际工程测量入口：Profiler默认0且无逐帧CSV，现有smoke不替代；下一步补真实场景采样/冷暖cache合同，尚无新1000×5性能结果 |
| T06 | 判定 M4-A 语义迁移是否满足前置 | Validation/sync0、图像等价、submit拓扑不变、performance non-regression 的综合记录 | 等待前置 | T01~T05；>3% 是调查阈值而非自动失败，但不能没有原因/接受理由就放行 |
| T07 | 实施 M4-B 同步范围优化 | 明确 producer/consumer、stage/access/layout；全局→资源屏障/宽→精确；真实工程等价回归与性能证据 | 等待前置 | T06 后才实施；completion signal ALL_COMMANDS 为生命周期语义，不列为缩窄目标 |
| T08 | 完成真实场景 WSI/资源退休压力验证 | resize/minimize/restore/退出/反复场景切换，资源引用与退休完整；无提前复用、错误和泄漏 | 已完成 | 原Debug工程legacy/Modern两进程×6次RT重建、12resize、6minimize/restore各完整过；12静态ROI均diff0，错误/泄漏0，源码/exe不变；有限压力非全平台退休证明 |
| T09 | 核对 cache/串行预热在真实工程的合同 | opt-in/default OFF、冷/暖数据关联、损坏/不匹配安全拒、创建/销毁正确；不声称driver命中减少create调用 | 待执行 | §2CD~CU 实验已完成，补真实工程接线与场景证据；可选graphics manifest不作为硬缺口 |
| T10 | 完成 Android/真实低版本 GPU 验收 | 真机渲染/格式/恢复/生命周期/温控；自然1.1/1.2及KHR/fallback；编译链接不算设备运行 | 依赖外部条件 | 现有Android编译有效；当前adb列出7F72500010301388，但shell提示登录才能继续，尚无法核实API/ABI/Vulkan能力，需可用授权设备 |
| T11 | 完成计划覆盖的苹果/XR 平台验证 | 对应依赖工件、构建、API约束/拒绝、生命周期与视觉证据；无设备不得宣称通过 | 依赖外部条件 | 需要对应平台/SDK/设备；不可仅凭Windows或Android链接替代 |
| T12 | 最终逐条关闭 §23 DoD 与 M0~M7 | 每条必需项有实现/验证/证据；所有未完成项清零后才宣布全完成 | 等待前置 | 2026-10-05整体验收审查已执行，61/61逐条核对，结论未通过；见VULKAN_OVERALL_ACCEPTANCE_20261005.md。T05/T06/T07/T09及外部平台/特性证据仍未齐，不降低标准 |

## 执行记录

### 2026-10-05：整体验收审查完成，最终目标尚未通过

- Lead按原计划M0~M7、§18~20/22/23核对，61条DoD逐条记状态。五个修改生产文件与exe SHA仍匹配最新真实工程证据；只读重判六配置+两压力原始日志8/8满足有限判据。本次没有重跑GPU/新构建或实施生产改动。
- 已测Windows功能/静态ROI/stencil/窗口资源重建证据有效，不能直接扩为所有场景/平台通过。真实场景Dynamic policy ON仍Classic回退；未验证FSR/secondary/XR默认Classic符合原范围，不作为强行启用任务。
- 验收未通过：真实工程性能/首次使用/冷暖cache与>3%结论、M4-A前置、M4-B范围优化、实际cache/prewarm合同、§18全特性及自然低版/移动/项目支持的苹果-XR平台证据仍不齐。T05继续唯一进行中；T12仍等待前置，不把“完成验收报告”写成“产品全部完成”。
- 本轮记录VULKAN_OVERALL_ACCEPTANCE_20261005.md并更新最终矩阵当前结论，历史执行归属及失败记录保留。未暂存/提交/推送。

### 2026-10-05：T00 完成，T01 开始

- 已核对原计划§12.2 M4-B仍未实施、§13.7 Classic保留范围、§15.3串行预热范围、§16.7 >3%规则、§19警告分类与§20/23最终验收。
- 原阶段实现与有限测试保留，不从头重新实现；已知mask输入不继续处理。
- 当前目标仍未全部完成。接下来实际执行T01并把终态写在这里，不能只停在清单。

### 2026-10-05：T01 首个真实红例，新增 T01R

- 当前真实Debug test-cases构建exit0；baseline11实际effective1.1/timeline0，正常退出0但copy-buffer WAW到重复输出上限，不能判PASS。原日志与截图保留 `win-completion-t01-lead/run-20261005-164243788/baseline11/`。
- 暂停扩大矩阵，新增T01R。仅VKCommands.cpp补buffer-specific copy依赖，覆盖上传目标range和instance传播同buffer的read/write；源TRANSFER及已有声明render stages，目标TRANSFER，未使用ALL_COMMANDS/waitIdle、新submit或关闭Validation。是否修复由下一次真实运行决定，不以构建代替。
- 首次hash清单误用了FrameGraph路径，已改到frame-graph目录；该记录脚本错误未修改生产文件，原红例仍有效。

### 2026-10-05：T01/T01R 完成，T02 开始

- Lead实际重编VKCommands.cpp并重链原Debug工程，build exit0。原baseline11 copy-buffer WAW红例保留；新源补buffer-specific copy memory dependency，不增加wait/submit或关闭Validation。
- 原真实工程baseline11-fixed2、enhanced12、modern13-{legacy,sync2,submit2,dynamic,classic,feature-fallback}及modern13-fence-sync2共9独立正例：exit0、ERROR/VUID/SYNC-HAZARD/JS0、唯一leak0；实际effective和路径分别核对。同exe SHA cc2affdb…cdc2，source-after-t01清单运行后不变。baseline11-fixed第一次只停在Cocos启动界面，未导航，不当完整通过。
- 同尺寸9组2d3d/atlas/RenderTexture截图已保存；atlas与RenderTexture主体ROI[12,210,1212,900]各828000像素，相对1.1基座8组diff均0。动画2d3d仅人工可见核查，不把非同步动画当静态golden。Dynamic配置仅政策ON，FrameGraph自动屏障使Hybrid资格回退Classic；实际命令需GPU抓帧证明，未冒称Dynamic draw通过。
- 证据：ignored win-completion-t01-lead/run-20261005-164243788/，matrix-terminal.json、source-after-t01.json、static-roi-comparison.json、scene-review-matrix.png及逐案raw/流程/PNG。只读系统MSI查询找到已安装RenderDoc 1.44的实际路径；未安装软件。
- 进入T02：五族BestPractices仍在，不把错误0称为警告0。当前adb设备已列出但shell要求登录，未尝试认证/绕过，T10仍需可用授权条件。

### 2026-10-05：T02 首批两项警告修复与实跑

- Classic clearValueCount改为最高实际CLEAR附件索引+1；含depth/stencil独立CLEAR，完整owned数组保留给Dynamic。无CLEAR时count0/ptrNULL，不改变load/store。maintenance1启用时在本次vkCreateSwapchainKHR局部链声明单一已选present mode，不在持久模板保留栈指针、不启用模式切换。
- 用户原Debug工程bp-final-build exit0，两文件真实重编/重链；中间编辑曾把混合EOL统一导致diff噪声，已从备份按字节恢复所有非目标区域，最终diff仅22/2和12/1，diff --check干净；最终源再次重编，未拿中间exe替代。
- bp-legacy和bp-sync2两原目录独立进程完整场景/窗口流程均exit0、错误/泄漏0，新exeSHA394a12e9…73b61；两对应警告消失，剩UndefinedToReadOnly/access-layout/StoreDontCareThenLoad三族。atlas与RenderTexture主体相对T01基座diff均0/828000。
- RenderDoc原始API查询脚本首次因无__file__异常进入UI，失败保留；修正硬路径后已有Python API数据。当前SDK Validation配置下正式注入原exe成功，实际Capturing Vulkan；菜单已人工核对（鼠标hover导致按钮填色差，不盲用差阈值通过）。下一步分析真实抓帧load/store，T02未完成。

### 2026-10-05：T02 实际菜单 stencil / LOAD-after-DISCARD 证据

- RenderDoc 1.44 实际回放原工程。renderdoc-bp2/frame_frame1737.rdc：同 command buffer 的三个 Classic begin 顺序；前两个 framebuffer 的DS view均ResourceId1622，对应image1621。RP1623 CLEAR→depth/stencil DONT_CARE，RP1627 LOAD→DONT_CARE；第二pass有43次stencil-enabled draw，真实内容被使用。不是仅凭警告推断。原抓帧/完整chunk与菜单-load-after-discard-red.json保留。
- 首份2d3d抓帧两个DS pass都是CLEAR，不能证明该红例；调查时一次控制台字符串无条件写了“causal tuple verified”，该判断撤销。因果链以本次菜单实际tuple为依据。
- 未改变用户确认的mask输入问题。原mask无修改抓帧有13draw、stencilTest均false，仅证明该帧没有stencil测试；不能当mask clipping内容通过。第二采集运行在RenderTexture搜索时因键盘焦点不在自有进程安全停止；菜单/未修改mask两份rdc有效，不把缺失第三场景记成完成。
- ForwardPipeline每个相机compile/execute/reset；ForwardStage此前每次新建临时DS，跨相机LOAD没有持久归属。新增FrameGraph借用gfx::Texture的import入口，普通Vulkan、shadingScale1、尺寸/格式匹配、X1/单层单mip/无view与external标记时导入RenderWindow持有DS；现有imported规则产生STORE，图生命周期不释放该外部图像。其他目标保持原路径。当前正在真实工程重编，尚未宣称修复通过。

### 2026-10-05：T02 完成（三族修复、两族分类），T03开始

- 最终Debug新build exit0，FrameGraph/ForwardStage真实编译；追加明确CC_USE_XR/AR排除后ForwardStage再编译/engine及test-cases再链。exeSHA870bb4e9…ecc05。legacy/Sync2-v2/1.1-fence/1.2/forced-fallback五进程均全流程exit0、Error/VUID/SYNC/JS0、唯一leak0、source不变；各实际路径见ds-owned-terminal.json。atlas/RenderTexture主体ROI五档×两图全部diff0/828000。
- StoreDontCareThenLoad警告全部消失。新RenderDoc菜单frame_frame411.rdc：同image333/view336，三个pass均depth+stencil STORE，先CLEAR后LOAD链有43stencil draw；共享store judge旧红exit1/新绿exit0。PickPixel按RenderDoc1.44 Vulkan实现green*255读取stencil：先0→区域内1/外0→Equal reference1保持→后续camera CLEAR0，全30个采样符合预期。采集mask未改层，仍不宣称mask clipping。
- 最初Sync2因光标悬停首按钮使菜单ROI差73750，窗口正常退出、未导航；保留不计完整通过。runner先将光标移到已识别空白区后新Sync2-v2全过。harness修改一次IndentationError未执行写入/启动，日志保留，修正后执行。首份新抓帧等待确认超时未生成rdc；新独立v2协调器对已人工核对菜单ROI exact-match即时确认，capture exit0/CAPTURED两份rdc，原失败保留。
- 两剩余advisory分别为mip尾历史TRANSFER_WRITE availability与RenderTexture首建READ_ONLY布局；依据/首次内容边界见分类文档，未删正确性依赖/未屏蔽。T02完成仅表示逐族处理，不代表Warning0或所有内容/性能/平台通过。
- 开始T03四档selected静态场景对照。补当前源码Modern完整策略（Sync2+Submit2+Dynamic opt-in）真实流程，不能仅复用新DS修复前旧exe证据。Android源码影响编译仍在运行，未冒称通过。

### 2026-10-05：T03有限四档静态对照完成，T04开始

- 同最终exe870bb4e9…ecc05的1.1/fence、1.2/auto、1.3 legacy、1.3 sync2、feature-disabled fallback与Modern完整策略共6当前源码进程均exit0/Error-VUID-SYNC-JS0/leak0；Modern完整策略actual paths timeline/sync2/submit2/dynamicRendering=1，最后一位只是政策，不是命令执行证明。
- 四档selected静态场景atlas/RenderTexture同1224×959窗口，主体ROI[12,210,1212,900]、828000像素、阈值0，每配置两图diff0；Modern完整策略也独立diff0。PNG不包括变化FPS文字区；2d3d动画已有截图人工检查，非同步动画未作为全帧golden。有限对照不替代复杂视觉/性能/所有平台DoD，资格fallback/真正Dynamic PFN由T04检查。
- Android build.ps1 -Jobs2完成exit0，251步当前源码含ForwardStage、VKCommandBuffer/VKCommands/VKSwapchain真实编译，libcocos_engine.a及libvk_android_probe.so链接成功（android-ds-import.log）。只是源码影响编译补证，不是Android runtime；Boost旧deprecated-builtins警告保留。
- T04已有同源菜单STORE/stencil内容证据；下一步同Modern策略的真实2d3d/RenderTexture捕获、PSO/descriptor/layout/内容与Classic资格回退逐项核对。T04唯一进行中，T05性能未开始。

### 2026-10-05：T04 Modern真实命令与RT前帧消费核对

- 2d3d实际2 Classic pass/63draw/6 Barrier2/1Submit2；RenderTexture实际5 Classic pass/22draw/12Barrier2/1Submit2；Dynamic begin均0。所有实际draw PSO有创建记录且RP匹配，used descriptor及布局/依赖JSON保留。FrameGraph自动屏障门控的Classic回退是源码与抓帧事实，不借policyON冒称Dynamic渲染。
- RenderTexture两个图像当前帧先被屏幕采样、后由RT相机写入；正在回放真实首次进入的24连续帧，布局seed不能等于像素初始化。T04仍进行中，不以稳态PNG收口首帧内容。
- 采集搜索按图像guard停止的失败保留；最终仅操作自有可见RichEdit并以WM_GETTEXT+filtered ROI确认，v4采集exit0/CAPTURED。没有修改场景/输入/资源，已知mask输入不处理。

### 2026-10-05：T04有限GPU调查完成，T08独立压力项开始

- 连续24帧回放complete=true，首次RT出现于983：屏幕消费者先于同帧相机producer，首次六点为RGBA0；984以后读取前帧灰底/模型。已有API没有首次像素值保证，layout seed不初始化像素。保留生产先于消费/初始化的应用内容合同，不扩大为Vulkan全RT透明黑初始化，不修改场景。该有限调查不是所有GPU确定首帧或全部复杂视觉Gate，详见GPU抓帧报告。
- T04关闭调查项；T08不依赖性能结果，先执行现有源码两策略资源/窗口压力，不改变同步逻辑。首次stress legacy只因旧window helper不支持move在启动菜单前停止，未当通过，原日志保留；新的legacy-v2/modern-v2用已验证helper且独立目录运行。T08唯一进行中，T05仍待执行。

### 2026-10-05：T08有限真实资源压力完成，T05性能收口开始

- 原Debug工程当前exe870bb4e9…ecc05，legacy-v3与modern-v3各完整主场景流程+六轮RT返回/销毁/重建、交替12次resize/6次minimize-restore，两native退出0、ERROR/VUID/SYNC/JS0、唯一leak0。Modern实际timeline/sync2/submit2/dynamicPolicy1（实际渲染资格仍按T04 Classic）。
- 12次cycle图1224×959，静态主体ROI[12,210,1212,900]相对同源1.1基座阈值0、全部diff0；terminal.json汇总两run各自flow/judge/stress/visual，五生产文件与exe SHA重查不变。执行渠道Lead；没有拿Orca或旧run替代。证据 window-resource-stress/{legacy-v3,modern-v3}/。
- 初版legacy helper不支持move，第二版legacy-v2在第四轮触及原采集器240秒WM_CLOSE上限，native正常退出0/错误0/leak0但缺剩余循环，不当压力PASS。第三版独立新目录将采集期限600秒，场景/循环数与判据不变，两完整终态才关闭T08。原失败日志保留。
- 此压力验证是有限真实场景，不声称所有GPU pending lease/平台present完成/温控证明；已有独立寿命契约只补充，T10/T11/T12仍保留对应验收。
- 进入T05。源码实读Profiler.h默认CC_USE_PROFILER0，Profiler类只有private聚合块与interval显示，没有现成逐帧导出；真实工程缓存CC_VULKAN_PIPELINE_CACHE=OFF，Device代码没有该名字的环境opt-in，不能凭设置env就声称冷暖持久cache测试。下一步需真实工程测量入口（默认OFF/显式opt-in），然后300warmup+1000有效帧×至少5轮、ON/OFF分开、冷暖关联/首次指标；不得使用RenderDoc时长或平均FPS授予Gate。尚未完成入口/正式采样与>3%收口，T05唯一进行中。
