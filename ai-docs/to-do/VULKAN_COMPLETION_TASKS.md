# Vulkan 现代化收口任务

目标：完成原 `Cocos_Vulkan_1.1_to_1.3_Modernization_Task_Plan.md` 的必需实现与 DoD，不把阶段性提交、有限 smoke 或编译成功等同最终完成。

更新日期：2026-10-06。当前执行渠道：Lead。状态：已完成 / 进行中 / 待执行 / 等待前置 / 依赖外部条件 / 验证失败 / 暂缓（本阶段排除）。

## 执行约定

- 用户于2026-10-06明确Apple/XR暂不考虑：T11保留为后续任务，移出本阶段验收与T12阻塞条件；状态为暂缓，非已验证通过。原计划与历史证据保持原样。

- 工程验证直接使用 `E:/AIProMax/github/cocos4-vulkan-test-project/native/engine/win64` 与 `build/windows/proj`，目标 Debug `test-cases`。独立 CPU/smoke 仅补充缺口。
- mask Canvas 层不匹配为用户确认的已知测试问题，排除，不修场景、不算 Vulkan 阻塞。UI 输入诊断已结束。
- 保留既有实现与回退边界；FSR/复杂 secondary/XR 等未验证路径默认 Classic 符合计划，不为“全部完成”强开。graphics manifest 扩展/GPL 等可选项不加入硬完成条件。
- 每项完成须记录当前源码、命令/运行配置、实际路径、退出/错误、证据目录与限制。已有有效证据复用，失效或未覆盖部分才重跑。
- 每次只保留一项“进行中”；发现具体红例先修复并回归，再完成该项。M4-B 必须等待 M4-A 的正确性/性能前置，不绕过。
- 不自动 stage/commit/push；不关闭 Validation 或用 suppress 掩盖真实问题。外部设备不足如实保留，不能把目标全部完成标成 PASS。
- 用户于2026-10-05指定顺序：先真实工程cache/prewarm与必需功能闭环，再正确性验证，随后正式性能验收；M4-B继续等待M4-A前置通过。

## 任务状态

| ID | 任务与用户得到的结果 | 完成条件 | 状态 | 证据/下一动作 |
|---|---|---|---|---|
| T00 | 核对原计划、现有实现及剩余 DoD | 区分未实现、未验证、可选范围和外部条件 | 已完成 | 计划§12~23、现有验收矩阵、PROGRESS§2DZ；本文件记录执行目标 |
| T01 | 在真实 Windows 工程运行版本与回退矩阵 | 当前 Debug 构建；1.1/1.2/1.3、fence/auto、legacy/Sync2/Submit2、Dynamic opt-in/Classic fallback 各实际路径核验；红例单独记录 | 已完成 | T01R后8配置+fence/Sync2补例9/9，路径/场景/窗口/退出/Validation与泄漏均过；非全平台/性能Gate |
| T01R | 修复真实1.1/fence buffer重复上传WAW | 同一真实工程红→绿；legacy/Sync2及版本回归；不增加waitIdle/submit或关闭Validation | 已完成 | 真实1.1/fence红→绿，9配置同新exe回归；仅VKCommands.cpp补目标range及实例复制依赖 |
| T02 | 处理真实工程 BestPractices 警告 | 按 layout/access、load/store、WSI 分族；真实缺陷修复；无需修改项提供依据及边界，不仅屏蔽输出 | 已完成 | 三族修复实跑及菜单GPU红→绿；两族layout/access按规范分类保留，不宣称warning0；见VULKAN_BEST_PRACTICES_DISPOSITION.md，首次有效内容另列T04 |
| T03 | 完成四档同场景图像对照 | Baseline1.1/Enhanced1.2/Modern1.3/forced fallback 同场景、同尺寸；golden/image diff及阈值；实际命中的 Classic/Dynamic 分开证明 | 已完成 | 当前源码四档+Sync2/Modern完整策略6配置；atlas/RenderTexture静态主体ROI阈值0全部相同，2d3d有限人工核查；真实Dynamic执行仍由T04分证，不把政策ON算draw |
| T04 | 用 GPU 抓帧核对渲染内容与依赖 | RenderDoc/AGI 实际 pass/draw/PSO/descriptor/layout/load-store/resolve/stencil 内容证据；截图不替代抓帧 | 已完成 | 菜单STORE/stencil红→绿，Modern 2d3d/RT真实PSO-descriptor-Barrier2-Submit2与Classic回退已捕获；RT首次内容24帧回放完成，初始内容未定义合同如实保留，见VULKAN_REAL_PROJECT_GPU_CAPTURE.md |
| T05 | 收口性能、冷暖 cache 与 >3% 调查 | 同设备同场景固定warmup，≥1000有效帧×≥5轮、P50/P95/P99；FirstPresent/FirstPSO/启动分开；回归有证据与接受理由 | 已完成（本机有限） | 40正式进程+20同策略控制+4等待身份、4Immediate短测+20隔离编码器正式进程+10同编码器控制；超3%原始数据保留，三slot等待重分布和host变异分开记录；[报告与接受理由](VULKAN_REAL_PROJECT_PERFORMANCE_20261006.md)，非全游戏/平台Gate |
| T06 | 判定 M4-A 语义迁移是否满足前置 | Validation/sync0、图像等价、submit拓扑不变、performance non-regression 的综合记录 | 已完成（本机前置有限接受） | T01~T05/T13实际证据；CPU配对median/P95变化中位数低于3%，GPU10对低于0.3%，控制组复现host变异；§16.7接受理由与限制明确保留，外部平台不借此通过 |
| T07 | 实施 M4-B 同步范围优化 | 明确 producer/consumer、stage/access/layout；全局→资源屏障/宽→精确；真实工程等价回归与性能证据 | 已完成（本机首批有限） | 已知buffer资源化6案/23夹具、Dynamic post缩窄8案/8夹具、同当前exe提交/内容/退休全过；原性能超阈值、同binary控制、同路径20进程直接测量与接受限制完整保留；[报告](VULKAN_REAL_PROJECT_SYNC_SCOPE_20261006.md)。未知pre范围保留安全回退，未证明上传或全部Dynamic负载提速 |
| T08 | 完成真实场景 WSI/资源退休压力验证 | resize/minimize/restore/退出/反复场景切换，资源引用与退休完整；无提前复用、错误和泄漏 | 已完成 | 原Debug工程legacy/Modern两进程×6次RT重建、12resize、6minimize/restore各完整过；12静态ROI均diff0，错误/泄漏0，源码/exe不变；有限压力非全平台退休证明 |
| T09 | 核对 cache/串行预热在真实工程的合同 | opt-in/default OFF、冷/暖数据关联、损坏/不匹配安全拒、创建/销毁正确；不声称driver命中减少create调用 | 已完成 | 原Debug工程ON_START/既有渲染线程实际接线；17进程含冷暖、坏输入、wrong-key、checkpoint恢复、保存失败安全与参数拒绝；38判据夹具/24静态ROI；PROGRESS§2EE与模块README。有限功能合同，非性能Gate |
| T13 | 补齐渲染特性与异常正确性覆盖 | 按§18逐族核对实际实现、实际命中路径、内容结果；版本/PFN/能力拒绝、销毁和恢复独立记录 | 已完成（本机最小矩阵） | shadowMap默认纹理09600已最小修复，真实现代/Classic-fence、窗口恢复与RenderDoc有限内容通过；[证据](VULKAN_SHADOW_FALLBACK_VERIFICATION_20261005.md)。Instancing两实例draw与内容、正常退休也已有[有限证据](VULKAN_INSTANCING_VERIFICATION_20261005.md)；Indirect已有限通过；Bloom八pass阈值内容及Compute前提/dispatch→storage→fullscreen有限通过，见[本轮证据](VULKAN_BLOOM_COMPUTE_VERIFICATION_20261006.md)；Input Attachment/Multi Subpass、color/depth resolve、Secondary、FSR实际内容与WSI S1–S8已补齐；滑条JS问题及完整变体/平台仍单列，不把本机最小矩阵扩称全平台 |
| T14 | 补连续上传与更广负载验证 | buffer/texture多轮上传、内容、重建与退休；Dynamic及上传负载性能分项记录 | 进行中 | 24波buffer/texture三路径+内容负例4/4；buffer6/6、Dynamic resolve8/8回归；五轮×1000上传采样已完成，batch P99+4.294%仍调查，不能标整体性能通过；[证据](VULKAN_UPLOAD_STRESS_VERIFICATION_20261006.md) |
| T10 | 完成 Android/真实低版本 GPU 验收 | 真机渲染/格式/恢复/生命周期/温控；自然1.1/1.2及KHR/fallback；编译链接不算设备运行 | 依赖外部条件 | 现有Android编译有效；2026-10-06当前adb devices列表为空（前次设备shell登录限制保留为历史），需可用授权设备；Windows结果不替代 |
| T11 | 完成计划覆盖的苹果/XR 平台验证 | 后续恢复范围后补对应工件、构建、生命周期与视觉证据 | 暂缓（本阶段排除） | 用户2026-10-06明确暂不考虑；不阻塞本阶段T12，不标通过 |
| T12 | 最终逐条关闭 §23 DoD 与 M0~M7 | 每条必需项有实现/验证/证据；所有未完成项清零后才宣布全完成 | 等待前置 | 2026-10-05历史61条审查结论未通过，不能当最新全验收。2026-10-06已补T09/T13/T05/T06及T07本机有限证据；T14上传慢尾调查/接受理由与T10真实Android设备、自然低版路由及游戏温控条件仍未齐；T11按用户范围调整暂缓且不阻塞本阶段。证据到位后按调整后的范围重判整体 |

## T14 分项状态（2026-10-06）

| 子项 | 状态 | 实际完成与边界 |
|---|---|---|
| T14-01 buffer/texture连续上传内容与生命周期 | 已完成（本机有限） | 24波×4次更新，三配置正例+真实错误内容负例4/4；不能扩称mipgen、跨draw采样后再次上传 |
| T14-02 Dynamic与原buffer组合回归 | 已完成（本机有限） | 8个resolve路径+6个buffer合同；执行版本与后续清理增量分开归档 |
| T14-03 正式上传批次采样 | 已完成（有限测量） | 两版各5轮×1000；严格host闭合/实际wait/whole golden/staging有界；不是游戏帧或新平台验收 |
| T14-04 波动调查与接受理由 | 进行中 | 分段/top50已定位等待与提交占比较大；同binary新版五对10/10通过，P95本身−11.9%~+24.1%双向波动；进一步host等待/调度证据与接受理由待补；旧P95差异和P99观察不可未经解释当根因完成 |

原计划§16.7阈值针对Median/P95 Frame Time，不把额外P99批次观察变为自动FAIL条件；整体DoD仍须平台与游戏测量证据。

## T13 逐项正确性清单

仅记录各项有限完成边界，不将独立smoke或策略开关当真实工程内容通过。T13本机最小矩阵完成；XR/Multiview与真实设备矩阵仍归T10/T11。有限组合不扩称所有渲染变体通过。

| 子项 | 真实工程任务 | 状态 | 已有证据 / 下一步 |
|---|---|---|---|
| T13-01 | Shadow Map默认纹理初始化与采样 | 已完成（有限） | 实际09600红→绿、现代/Classic-fence、窗口恢复、8个draw默认纹理白色读回及四张shadow output；CSM全族未含在此完成范围 |
| T13-02a | GPU Instancing实例颜色 | 已完成（有限） | 原工程两实际Indexed draw=1024+976实例；2000条HSV颜色、652种输出匹配；现代/Classic-fence正常退出0/error0/leak0。详见[有限证据](VULKAN_INSTANCING_VERIFICATION_20261005.md)，再入/其他变体与平台不含在此范围 |
| T13-02b | Indirect Draw真实工程内容 | 已完成（有限） | 原test-cases原生默认OFF入口；upload RAW红→绿；索引/非索引multi与loop、1.1/fence回退、2048字节完整golden及32/40字节命令一致；缺失第二draw真实负例/资源释放/RenderDoc通过。仅Classic实际执行，详见[Vulkan间接绘制证据](VULKAN_INDIRECT_DRAW_VERIFICATION_20261006.md) |
| T13-03 | Compute场景管线与dispatch内容 | 已完成（有限） | 原test-cases默认OFF资源包入口、RaytracingWeekend真实注册；queue断言/storage descriptor/整块uniform三红→绿；100×150dispatch、32B实际参数、storage→fullscreen全字节一致、96000背景像素/球心golden；现代/Classic-fence正常退出。完整算法、非倍数resize/设备恢复待补，见[证据](VULKAN_BLOOM_COMPUTE_VERIFICATION_20261006.md) |
| T13-04 | Bloom实际pass与阈值交互 | 内容有限通过，交互待验证 | 相同输入/相同八阶段shader、GPU threshold0.05/1.0、Kawase八pass/descriptor链、combine差1440000字节；真实滑条曾触发JS null-window红例，不把包参数对照当交互通过。见[证据](VULKAN_BLOOM_COMPUTE_VERIFICATION_20261006.md) |
| T13-05 | Input Attachment / Multi Subpass | 有限工程合同完成 | 两 subpass 的实际 input descriptor/shader/全字节内容、三尺寸资源重建、缺 producer 负例、双政策退休通过；更多附件组合另验 |
| T13-06 | MSAA / D-S Resolve / Secondary / FSR | 有限工程合同完成 | 原test-cases默认OFF接线；2x/4x color与D32 SAMPLE_ZERO、Secondary真实draw/execute、FSR实际rate内容；9 native正例/4 GPU回放/5内容负例/30判据反例、实际shutdown退休。见[证据](VULKAN_RESOLVE_SECONDARY_FSR_VERIFICATION_20261006.md)，更多组合/设备另验 |
| T13-07 | 版本/PFN/能力拒绝、资源销毁与恢复 | 本机有限合同完成 | 已有窗口资源压力及cache拒绝；按实际命中特性补齐异常，区别实现与验证 |

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

### 2026-10-05：按用户优先级完成T09，开始T13正确性覆盖

- 执行渠道Lead。实际test-cases接入default-off诊断模块：平台启动前拒绝坏参数，ON_START在业务PSO前配置持久化cache，两known compute recipes通过既有DeviceAgent线程串行创建。未改Vulkan生产核心/场景/资源；测试工程三处接线以native/tests/vulkan-real-project/real-project-integration.patch保留，不重复应用。
- 当前源码Debug构建/重链exit0，exe SHA3859b142…cabc9940。初次缺private TBB导入导致链接红，补源文件__TBB_LIB_NAME后重编；首个旧窗口helper缺move的OFF采集未完成，不算通过，失败独立保留。
- 17个终态进程：12完整场景预期正例/键拒绝、1保存失败安全负例、4初始化前参数拒绝。cold保存137391 payload字节，新restored实际driver输入137391字节；checksum/driver/vendor/device/UUID/truncation各安全拒绝并正常渲染。wrong-key实际创建合法PSO后Ready1/Failed1，正常场景退出为预期1；1.1/fence实际路径另测。
- 正常enabled案实际预热GPU pipeline/module/layout销毁2/2、2/2、1/1；14业务graphics均消费同cache并完整销毁。Validation/Error/VUID/HAZARD/JS与设备泄漏0；默认OFF无额外hook/预热/诊断文件IO。共享判据38/38，24组atlas/RT主体ROI阈值0/diff0；源码/exe前后不变。
- 活进程checkpoint快照118866 payload字节在新进程恢复；不冒称断电测试。保存失败案仅锁独立测试文件、失败atomic replace三次、旧SHA不变；native0/CLEAN与旧文件有效不代表本次保存成功，正例judge正确拒绝，安全性单独通过。
- 根：native/build/vulkan-modernization/win-real-prewarm-lead/run-20261005-212151/，terminal.json为本项有限终态。模块README与必需功能状态表记录实现/证据/限制。当前共15任务：已完成8、进行中1（T13）、待执行1（T05）、等待前置3、依赖外部条件2；不是总体完成百分比。
- T13仅开始源码合同调查，尚无新增Compute/Bloom GPU内容结论；当前Debug打包settings.rendering.customPipeline=false，Compute管线前提尚未满足，未改全局设置。Compute场景README要求custom-pipeline名RaytracingWeekend并加载注册脚本；现有addDispatch之后storage输出被fullscreen quad采样，需验证实际dispatch与内容，不能用本轮预热创建替代。Bloom settings启用、slider改threshold，需核实际pass与输出变化。性能随后T05，M4-B仍等T06。未暂存/提交/推送。


### 2026-10-05：T13 Shadow Map 默认纹理有限闭环

- 真实shadowMap采样2×2默认纹理UNDEFINED触09600，临时图像句柄探针匹配根因；最终只GlobalDescriptorSetManager.cpp加TRANSFER_DST并普通上传16字节白色内容，探针byte-exact移除。当前源Debug重编/重链exit0。
- 现代timeline/Sync2/Submit2/Dynamic策略与Classic/fence强制1.1两独立原生进程正常退出、错误和泄漏0；两配置resize/minimize/restore、现代菜单返回再入均有截图。抓帧51实际draw，默认image唯一usage6、8次采样、16字节255；4张shadow output非恒定、producer早于consumer。实际阴影Classic资格回退保留。
- 完整范围/原红、探针、两绿与抓帧失败记录见[Shadow有限报告](VULKAN_SHADOW_FALLBACK_VERIFICATION_20261005.md)、PROGRESS§2EF。T13状态仍进行中；先继续GPU Instancing实际draw/内容，再处理Compute/Bloom场景前提与内容，性能和M4-B顺序不变。没有Git提交。


### 2026-10-05：T13-02a Instancing 有限内容验证完成

- 全部Lead执行、原Debug当前exe6db8457a…acf7与输入源码不变，沿用上一块真实构建日志，未宣称本块重建。原modern/legacy关闭被15秒采集器强制结束，两案被judge明确拒绝；90秒独立重跑的modern-retirement/legacy-retirement自身退出0/error0/leak0，原失败不覆盖。
- 现代重复案默认2000；Classic重复案2000→1000→3800+resize/minimize/restore后正常退出。现代增长/窗口恢复截图来自初案，其退休失败不当整流程通过；本块菜单返回再入未完成加载，不勾选。
- RenderDoc实际两Indexed实例draw=1024/976，各36index/divisor1/stride80；2000条属性按同帧worldY重算HSV，容差1e-6/mismatch0；677属性颜色、652种匹配输出、27444像素。实际3 Classic pass/11 Barrier2/1 Submit2，Indirect=0。terminal.json有限Complete/Pass=true。
- 旧T13-02细分为02a有限完成、02b待真实工程合同。T13整体仍唯一进行中，其他15主任务计数不变，T05性能/平台/M4-A→M4-B不放行。报告记录G1～G6范围和回放失败，未改生产/场景资源、未Git提交。

### 2026-10-06：T13-02b Indirect Draw有限合同完成（Lead）

- 真实Windows工程默认关闭诊断接线、7行生产屏障diff的Indirect/Index消费端修复；真实RAW红→绿、10个有限native终态、当前exe GPU回放、31 parser fixtures。矩阵各实际SHA分开，未把旧exe改记当前。
- 多/循环两路径、索引/非索引、完整命令/图像、缺失draw负例、未知/互斥/真实空参数拒绝、非VMA直接退休与VMA最终泄漏0按证据分别记录。捕获实际Classic，未改autoBarrier gate。
- 详见[有限报告](VULKAN_INDIRECT_DRAW_VERIFICATION_20261006.md)与PROGRESS§2EH。T13仍进行中，15主任务计数不变；下一批为Bloom实际pass/内容与Compute场景前提，T05/M4-A/M4-B顺序及外部平台条件不变。未暂存/提交。


### 2026-10-06：Bloom 内容与 Compute 场景前提有限闭环

- 全部由Lead执行。原工程默认legacy包不满足custom builder；新增默认OFF完整资源根选择器与隔离Creator构建入口，保留原Resources/项目rendering设置。Creator使用安装JS3.8.8与当前native，混合版本有限验证，不授予引擎兼容/平台Gate。
- 三生产文件最小修复：Compute queue viewport非空合同、STORAGE_IMAGE descriptor保持GENERAL、显式setBuffer整块uniform优先绑定；断言/Validation/实际全零UBO内容红例均保留。当前源码重编重链0，同EXE15b99cd7…eca6的四正常进程全部exit0/error0/leak0，3最终RDC与EXE SHA关联。
- Bloom实际八pass阈值对照，pre-Bloom输入1920000B完全相同，combine差1440000B；Compute实际32B参数、100×150dispatch/local8×4、1920000B storage到fullscreen垂直翻行全字节一致、96000背景像素/球心golden通过。共享内容判据含旧实际全零UBO等5负例exit0。实际render pass均Classic，现代政策ON不冒称Dynamic执行。
- Bloom滑条的JS null-window红例保留为交互待验证，未处理已知mask层问题；不把输入错误0、shader创建或package preflight当内容证据。T13-03有限完成、T13-04内容有限通过/交互待验证；15主任务计数不变，T13仍唯一进行中。
- 详细证据见VULKAN_BLOOM_COMPUTE_VERIFICATION_20261006.md、PROGRESS§2EI，raw=win-bloom-compute-t13-lead/run-20261006-092224859505。下一步建立Input Attachment/Multi Subpass真实工程合同；MSAA/D-S/Secondary/FSR/异常恢复、性能/M4-A→M4-B/外部平台继续各自等待，不升级整体验收。未stage/commit/push。


### 2026-10-06：T13-05 Input Attachment / Multi Subpass 有限工程合同完成

- Lead 执行：原 Windows Debug 工程默认OFF诊断、真实 input descriptor+两个 subpass+六实际draw、三尺寸资源波次、两图10496B/native全golden、同exe RenderDoc全像素/descriptor/SPIR-V独立核验。
- 自动 dependency 遗漏末 input subpass STORE 写 scope，旧真实 WAW native1→最小VKCommands修复→modern及1.1/fence/legacy均native0/error0/live+post0/leak0；对象33/33一次退休。skip producer实际draw0而golden非零/native1，默认OFF与未知mode拒绝均实跑；共享judge14反例全拒。
- 详见VULKAN_INPUT_ATTACHMENT_VERIFICATION_20261006.md、PROGRESS§2EJ；raw win-subpass-t13-lead/run-20261006-105018258858。BestPractices advisory保留；未认证深度input/MSAA/preserve/更多subpasses/device recovery。T13整体仍进行中，15主任务计数不变；下一步T13-06，性能/M4-A→M4-B/平台条件不变。未stage/commit/push。


### 2026-10-06：T13-06 有限工程合同完成

- 用户真实Debug工程，两批当前源码构建及各自EXE关联的9 native内容正例/4 RenderDoc replay；color/D32完整golden、secondary execute与recording lease、FSR rate image+实际builtin读取通过。5实际缺失内容负例native1，正常shutdown计数与VMA leak分证；共享judge30反例拒绝、exit0。
- 旧低版+Dynamic bootstrap/Empty abort强杀60929为红保留；合法1.1/Classic UNSUPPORTED3，新增诊断入口预检冲突native1。不是全局DeviceManager恢复修复。两EXE SHA与具体范围见[VULKAN_RESOLVE_SECONDARY_FSR_VERIFICATION_20261006.md](VULKAN_RESOLVE_SECONDARY_FSR_VERIFICATION_20261006.md)。
- T13-06有限完成；T13整体仍进行中，继续异常/恢复合同。T05性能与M4-A→M4-B、Android/Apple/XR条件不放行。无stage/commit/push；BestPractices advisory保留。


### 2026-10-06：T13-07 管线创建失败回退有限补证

- 新增真实工程默认OFF `secondary-variant-failure`。仅诊断目标Dynamic工厂：先真实driver成功创建、记录真实部分handle，再由hook返回OUT_OF_HOST_MEMORY；不是驱动真实OOM、不是device-loss。生产既有分支立即destroy部分结果、Classic handle保持有效、两fallback flag为真；每波一次实际注入与一次立即销毁，三波完整inheritance/execute/draw/copy/submit/golden通过。
- 当前源Debug重编/重链build1=0；variant-failure与Secondary/FSR普通回归三个独立进程均native0/error0/live+post0/leak0，21/21、21/21、18/18实际shutdown目标计数一次。共享judge34反例拒绝、exit0，新增4个单变量回退marker坏值。
- raw win-failure-t13-lead/run-20261006-122017619188/，terminal.json与前版源码备份保留；本项未新增生产同步/等待或改全局capabilities。T13-07部分合同完成，下一步真实工程SurfaceRecreate/MultipleSwapchain。T05及M4-A→M4-B仍按顺序。未stage/commit/push。

### 2026-10-06：T13-07 WSI 有限补证

真实surface重建/双swapchain/挂起恢复已由原工程现有device S1–S8验证，现代与1.1-fence错误/泄漏0，43/43目标退休；漏重建实际负例exit1。见[VULKAN_REAL_PROJECT_WSI_VERIFICATION_20261006.md](VULKAN_REAL_PROJECT_WSI_VERIFICATION_20261006.md)。device-loss与外部设备恢复仍未覆盖。

### 2026-10-06：§18 本机最小矩阵闭合，T05开始（Lead）

| §18族 | 有限证据 |
|---|---|
| Basic Forward / Depth Texture / Stencil | T03/T04实际原场景图像与GPU回放；stencil首次内容红→绿 |
| Shadow Map | T13-01默认纹理采样及shadow output |
| MSAA / Depth Resolve | T13-06真实2x/4x颜色与D32 SAMPLE_ZERO |
| Input Attachment / Multi Subpass | T13-05两subpass实际输入descriptor与全字节golden |
| Post Process / Compute | T13-04 Bloom八pass阈值内容、T13-03真实dispatch→storage→fullscreen |
| GPU Instancing / Indirect Draw | T13-02a/02b实际draw、数据与像素结果 |
| Fragment Shading Rate / Secondary | T13-06实际rate值5与shader读取、真实secondary draw/execute |
| Texture Upload / Buffer Upload | T01R与真实上传WAW修复/版本回归 |
| Swapchain Resize / Surface Recreate / Multiple Swapchains | T08场景窗口压力、T13-07既有device WSI S1–S8 |
| XR / Multiview（如支持） | T11外部条件，未验证 |

异常与退休：既有版本/PFN/能力CPU合同、T01实际回退、T09坏cache拒绝、T13-07真实partial variant合成失败清理/Classic恢复及WSI结果注入分别有证据。未宣称真实device-loss恢复；原计划最小矩阵不要求所有故障类型遍历。Bloom滑条JS异常仍单独记录，内容证据不伪作滑条通过；它不扩为本次Vulkan最小内容矩阵必需项。

T05真实测量入口正在实施：原Debug工程atlas-compress、主线程Before/AfterTick、队列帧标记、实际present与等待、提交CB GPU时间戳、cold/restored cache输入关联。40帧探索正例已过；正式1000×5尚未运行，不宣称性能Gate。T06/T07继续等待。
