# Vulkan Rendering Contract 前置清单（M5-A0，只读）

> 隶属：计划 §13（M5 Rendering Signature + Pipeline / Secondary CB 契约）。
> 执行通道：Orca Worker（2026-10-03）。纯源码只读，无实现。
> Lead同日复核修订：纠正secondary寿命、samples归属、格式/布局来源与近似行号，补pipeline key及subpass pNext风险。

## 六项映射

### 1. Rendering Signature

| 项 | 现状 | 缺口 |
|---|---|---|
| 数据结构 | 不存在；当前通过 `CCVKGPURenderPass`（VKGPUObjects.h:135-157）隐式表达：colorAttachments + depthStencil(+Resolve) + subpasses + dependencies + sampleCounts | 需新增纯数据 struct（计划 §13.1）：colorFormats/depthFormat/stencilFormat/samples/viewMask |
| 格式来源 | `ColorAttachment.format`（GFXDef-common.h:1317）+ depth/stencil format（:1331）；`mapVkFormat`实际映射（VKCommands.cpp:435,449；VKUtils.cpp:98-99处理设备depth格式） | 从目标subpass的colors顺序提取实际VkFormat，保留unused位置；depth/stencil分别按实际format aspect填写。FBO view用于实际附件匹配验证，不能把目标pipeline契约等同当前FBO |
| samples | `CCVKGPURenderPass::sampleCounts` per subpass（VKGPUObjects.h:149），由颜色/depth输出samples累积（VKCommands.cpp:497,508,530） | signature记录实际rasterization samples，不把resolve单采样计入；viewMask当前SubpassInfo未建模（GFXDef-common.h:1344） |

### 2. PipelineRenderingCreateInfo 适配

| 项 | 现状 | 缺口 |
|---|---|---|
| 使用 | 当前pipeline绑定renderPass/subpass（VKCommands.cpp:1215-1216），调用vkCreateGraphicsPipelines（:1222） | Dynamic分支设renderPass=NULL，并链入VkPipelineRenderingCreateInfo的formats/viewMask；该结构无samples字段，samples仍由multisampleState.rasterizationSamples提供（:1131） |
| 接入点 | graphics pipeline创建与multisampleState（VKCommands.cpp:1130,1203-1223） | 后续adapter先准备，不立即增加runtime A/B；Classic保持默认 |

### 3. 统一 pipeline pNext composer

| 项 | 现状 | 缺口 |
|---|---|---|
| 现有 pNext | 当前pipeline FSR节点直接赋值（VKCommands.cpp:1203-1209） | 统一composer管理RenderingInfo+ShadingRate，保持节点拥有关系，防止覆盖/重复/悬空 |
| 覆盖风险 | 若未来加第二个 pNext（如 DynamicRendering）会覆盖 shadingRateInfo | composer 保证互不覆盖 |

另有独立的**当前subpass链覆盖风险**：VKCommands.cpp:594先设depth resolve节点、:601又设FSR节点；两条件同时成立时后者覆盖前者（源码事实，未做该组合GPU取证）。这不是pipeline composer本身能修复的链；需单独处理/定向验证，不能称Classic所有组合均完整验证。

### 4. 显式 pre/post layout 依赖

| 项 | 现状 | 缺口 |
|---|---|---|
| Classic 路径 | attachment initial/finalLayout（VKCommands.cpp:432-457）、attachment生命区间导出的external dependency（:714-750）；CB记录末尾更新currentAccessTypes（VKCommandBuffer.cpp:275,279） | 保留已有行为，单独识别自动WSI early/late转换，避免重复 |
| Dynamic 路径 | rendering adapter不存在；M4的cmdPipelineBarrier可选择legacy/Sync2 encoder | signature只描述格式/采样，不包含完整同步范围；pre/post模型还需现有barrier/prev-next accesses/布局/用途。offscreen后可供shader/transfer等消费；仅呈现资源按WSI策略进入PRESENT，不能统一render后PRESENT，也不能只支持Barrier2 |

### 5. Load/Store/Resolve 映射

| 项 | 现状 | 缺口 |
|---|---|---|
| Load/Store | color字段（GFXDef-common.h:1319-1320）映射（VKCommands.cpp:438-439）；depth/stencil分别映射（:452-455），clearValues来自CB（VKCommandBuffer.cpp:217-226） | 映射至VkRenderingAttachmentInfo的loadOp/storeOp/clearValue及实际view/layout；不能把两aspect操作合并 |
| Resolve | color references（VKCommands.cpp:499-503,553-555）；depth/stencil modes来自SubpassInfo（GFXDef-common.h:1353-1354），依据设备supported/independentResolve属性调整（VKCommands.cpp:572-594） | Dynamic需resolveMode/view/layout及多采样/单采样关系验证；现有FBO分别持source与depth resolve view（VKGPUObjects.h:345-347），不能忽略模式限制或把未测组合宣称通过 |

### 6. Secondary CB inheritance

| 项 | 现状 | 缺口 |
|---|---|---|
| Legacy inheritance | VkCommandBufferInheritanceInfo（VKCommandBuffer.cpp:129-152）填renderPass/subpass/framebuffer，begin设RENDER_PASS_CONTINUE | Dynamic保留base inheritance但renderPass=NULL，pNext追加InheritanceRenderingInfo；formats/viewMask/samples与primary匹配，flags单独按继承规则处理（不能照抄primary的SECONDARY_CONTENTS位） |
| 引用寿命 | :132/:137明确retain renderPass/FBO；CCVKGPURecording.resources是owning ConstPtr（VKGPUObjects.h:403,430） | 裸Vk句柄字段不等于没有寿命保护；延续M3 recording lease，未来secondary还需拥有formats数组并参与primary引用传播 |

## Pipeline key事实

Native PipelineStateManager.cpp:40-46以pass/renderPass/IA/shader hash及subpass构造key；GFXRenderPass.cpp:42-50的hash含附件/subpass。TS cocos/rendering/pipeline-state-manager.ts:35-41也包含renderPass.hash。未来加入signature须考虑Classic/Dynamic路径区分及hash碰撞后完整相等校验；本轮不修改现有key。

## Multi-subpass/Input/Preserve/Multiview/XR 首批保留 Classic 的原因

- 计划§13.7明确首批保留multi-subpass/input/preserve/未验证secondary/FSR/复杂multiview-XR。当前subpass模型有inputs/colors/resolves/preserves（GFXDef-common.h:1344-1354），迁移不能只替换begin/end；本轮不调查或宣称新扩展已支持local-read。
- multiview需独立feature/limits/viewMask与XR契约；现有SubpassInfo无viewMask，不能从XR多窗口推断multiview已具备。FSR及secondary需另外完成Dynamic路径取证。

## 最小实现切块建议

1. 仅新增VKRenderingSignature.h纯owning数据及精确相等/hash契约测试，不含Vk句柄/运行时/pNext自引用；目标subpass/设备format映射adapter留下一块。
2. 核验color顺序与unused slots、depth/stencil分别、samples/viewMask、copy独立及碰撞后的相等判断，不用hash当兼容性证明。
3. 暂不把pipeline runtime A/B纳入最小切块；六契约准备与M4-A Gate完成后再决定开启路径。

规范依据：[pipeline formats结构](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineRenderingCreateInfo.html)、[secondary inheritance结构](https://docs.vulkan.org/refpages/latest/refpages/source/VkCommandBufferInheritanceRenderingInfo.html)、[ExecuteCommands继承flags规则06026](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdExecuteCommands.html)。

不宣称 M5 已实施/验收。
