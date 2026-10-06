# Input Attachment / Multi Subpass 有限验证（2026-10-06）

执行渠道：Lead；未委派，未 stage/commit/push。T13-05 有限工程合同完成，T13 整体仍进行中。

## 工程与功能

使用用户指定工程 `E:/AIProMax/github/cocos4-vulkan-test-project/build/windows/proj` 的 Debug `test-cases`。新 `RealProjectSubpass` 默认 OFF，接入原 main/Game/CMake，不替换项目资源或渲染配置。新增源码、README、累计 integration patch 和共享离线内容判据。

生产原有 input-attachment descriptor、`subpassInputs` 与 `nextSubpass()` 路径是真实执行入口。两 subpass、RGBA8/X1：第一个画红/绿左右半图，第二个 `subpassLoad(prior).brga` 输出绿/蓝。两个 PSO 分别匹配 subpass 0/1，consumer descriptor 指向 producer image。现代政策 ON 时仍拒绝 Dynamic 变体，实际 Classic 绘制。

资源按 32×16→24×12→32×16 三次创建/提交/等待/读回/销毁。每案 source+output 共检查 10496 字节；这是离屏资源尺寸重建，不冒称窗口 resize、设备恢复或全部场景内容。

## 真实同步红→绿

首版 build1=0、modern native=1/errorLogMessages=10，原 log 保留。`vkCmdNextSubpass` 的 COLOR→INPUT 布局转换写入与末 subpass 的 color STORE 写入无序；Validation 明确要求 destination `COLOR_ATTACHMENT_OUTPUT/COLOR_ATTACHMENT_WRITE`。原 auto dependency 0→1 只有 FRAGMENT_SHADER/INPUT_ATTACHMENT_READ。

`VKCommands.cpp` 的自动依赖生成新增 12 行/替换 1 行：对于最后使用为 INPUT 的 color 附件，STORE 或 DONT_CARE 仍有 store 写入，将上述 color stage/access 合并到进入该 subpass 的依赖。保留 fragment input read、BY_REGION、显式 dependency 分支、load/store/layout 与 Dynamic gate；无 ALL_COMMANDS、waitIdle 或 Validation 屏蔽。

依据：[Vulkan Render Pass Store Operations](https://docs.vulkan.org/spec/latest/chapters/renderpass.html#renderpass-store-operations)：color store 发生于 COLOR_ATTACHMENT_OUTPUT，并在最后使用附件的 subpass 执行；DONT_CARE 同样是写访问。测试 hook 在真实 driver 创建成功后核验包含新增 STORE scope 的依赖。

build2=0，`VKCommands.cpp` 与新诊断真实重编、engine/exe 重链。最终 EXE SHA256：`bd23beefcff0e109d9f77eca914255f092d43be9c425ad41e8671fe62e13dd70`。

## 原生五案与退休

| 案例 | 实际政策/行为 | native exit | 结果 |
|---|---|---:|---|
| modern-green | effective 1.3，timeline/Sync2/Submit2/dynamic policy=1；诊断实际 Classic | 0 | 三 phase golden=0，Validation live/post=0，error=0，leak=0 |
| legacy-green | effective 1.1，fence/legacy barrier/legacy submit/Classic | 0 | 同样三 phase 与正常退休通过 |
| skip-producer-green | 实际 producer draws=0，consumer=1，copy/submit/chain 完整 | 1 | source/output mismatch 各 1024/576/1024；failures=3，Validation=0，leak=0 |
| default-off-green | 无 opt-in，不创建诊断资源/安装 hooks | 0 | 唯一 off marker，原工程正常关闭、error/leak=0 |
| bad-mode-green | opt-in=bogus | 1 | known-mode 拒绝，未出现 Vulkan 初始化或 phase |

每个完整诊断案最终 device destroy 后记录 33 created=33 destroyed：12 ShaderModule、6 Pipeline、6 PipelineLayout、3 RP、3 FBO、3 DescriptorSetLayout，逐对象恰一次。6 Buffer 与 6 Texture facade 释放分别计数；底层 VMA 资源以最终 allocator leak=0 证明，不伪造独立 vkDestroy 计数。活 context 仅在 device destroy 前后记录 live/post，随后清借用指针。

skip-producer 是实际缺失绘制负例，不以 CLI 布尔直接伪造失败：真实 readback golden 触发三次失败；Debug logger 每次另有源位置 ERROR，故 errorLogMessages=6，Validation live/post 仍 0。

## RenderDoc 与共享判据

同 EXE 显式抓取三 phase：`subpass_capture.rdc`。实际 6 draw、3 BeginRenderPass、3 NextSubpass、3 EndRenderPass、6 CopyImageToBuffer、3 QueueSubmit2、0 BeginRendering。六 draw 的 actual subpass=0/1、同对 RP、不同 PSO；consumer input descriptor 的 ResourceId 等于 producer output。导出真实 SPIR-V 有 SubpassData image 和 OpImageRead；导出的输入与六张输出全字节 golden 均符合预期。

共享 `verify-subpass-content.py` 判原生与回放；`--self-test` 实际 exit0，5 native 案/6 draw/14 拒绝测试。包括真实 pre-fix WAW、缺 phase/提交/退休/Validation、非零 golden、重复 footer、回放不完整/少 draw/全零输出、错误 input link/subpass/NextSubpass 数。

BestPractices access-layout advisory 仍在：读回 source 的 barrier 显式包含原 render-pass STORE 的 COLOR_ATTACHMENT_WRITE availability，而该 source 末 layout 是 SHADER_READ_ONLY。RenderDoc 保留三条相关 Medium 消息。没有删正确性依赖以消除 advisory；本块不称 Warning=0。

## 证据与边界

raw root：`native/build/vulkan-modernization/win-subpass-t13-lead/run-20261006-105018258858/`。包含 before/source、build1/2、原 modern 红、五 green 独立 raw/summary、renderdoc capture/replay/像素/SPIR-V、judge-final 与 terminal.json。integration patch 在实际工程 `git apply --reverse --check` exit0；初次生成混合 EOL 失败已修为字节保真，未覆盖旧日志。

- G1 API/能力：RGBA8 两 usage 真实 image-format query、actual RP/PSO/descriptor 与 Classic gate；未知模式在设备前拒绝。
- G2 生命周期：三个新资源波次、actual submit/wait、33 对象恰销毁一次、facade drop 与 allocator leak 分开。
- G3 同步：layout transition→STORE 真实 WAW 红→绿；COLOR write→INPUT read 与 TRANSFER→HOST 依赖核验。
- G4 Validation：现代与 1.1/fence 原生 live/post/error=0；负例 golden 非零而 Validation=0；advisory 明示。
- G5 抓帧：同 EXE 实际 subpass/descriptor/shader/全像素六 draw 独立回放。
- G6 回归/平台：有限 Windows 双政策和默认 OFF；未重复 Android/无关全量矩阵。不涵盖 depth/stencil input、MSAA input、更多 subpasses、preserve/alias/feedback、secondary、设备丢失、移动温控或性能 Gate。

下一项仍按 T13-06 渲染特性与异常恢复逐个完成；T05 性能、M4-A→M4-B 前置、外部平台条件不变。
