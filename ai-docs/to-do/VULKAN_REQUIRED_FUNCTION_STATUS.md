# Vulkan 必需功能与验证缺口

更新：2026-10-06，Lead。用户指定顺序：功能闭环 → 正确性验证 → 性能验收 → M4-B（等待 M4-A）。原计划不改，整体验收尚未通过。

| 原计划要求 | 当前功能状态 | 需要继续做什么 |
|---|---|---|
| M0/M1：依赖、1.1下限、1.1/1.2/1.3分层能力与SPIR-V目标 | 已有实现，真实工程版本/回退有限验证已有 | Loader/PFN缺失、低版physical与XR约束异常入口的端到端验证；平台工件验收 |
| M2：WSI/image generation、present与submission完成分离 | 已有实现，真实resize/minimize/restore与资源重建已有 | 真机surface销毁/恢复、自然低版和平台present退休验证；不能拿正常resize替代全部异常 |
| M3：recording/descriptor version/buffer range/staging/event退休 | 已有实现，独立寿命用例及真实场景退出/重建有限验证已有 | 按资源族补真实工程异常/恢复合同；构建或对象保活不等于所有字节依赖已证明 |
| M4-A：Submit2/Barrier2语义编码 | 已有实现，实际抓帧命令及有限图像对照已有 | 正确性、拓扑与性能综合前置验收尚未通过 |
| M4-B：同步范围优化 | **未实施的必需优化** | T06通过后实施T07；上传WAW修复不是整个M4-B，不缩窄通用completion signal |
| M5/M6：rendering tuple/composer、Hybrid白名单及Classic回退 | 已有实现；独立Dynamic内容证据，真实场景资格回退已有 | 按§18渲染族验证实际命中路径，未验证secondary/FSR/XR继续Classic；不要求盲开Dynamic |
| M7：稳定owning key、完整相等判定、driver cache持久化 | 已有实现；实际factory消费key和cache | T09有限功能合同已完成：真实工程冷暖/坏输入/checkpoint恢复/保存失败安全/销毁；性能与跨平台证据不在本项 |
| M7：known manifest → 串行queue → pipeline cache | 已接入真实test-cases；known compute manifest通过既有渲染线程串行创建，业务graphics共享cache | 17进程有限合同见模块README；不要求扩展可选graphics manifest/异步框架，不把创建证明当dispatch内容 |
| §16正式测量入口/冷暖统计/百分位/>3%调查 | 独立实验已有，**真实场景测量入口及正式验收尚未完成** | T09与正确性验证之后推进T05；与渲染功能缺失分开记录 |
| §18、§19、§20与§23覆盖 | 部分已验证，**证据不齐** | T13按特性/异常/生命周期记录；T10/T11需可用授权平台，T12逐条验收 |

## 正确性验证的下一批覆盖

这些是“已有入口但证据未齐”，不能直接写成渲染功能尚未实现，也不能仅凭函数存在判内容通过。

| 特性族 | 当前源码/真实工程候选 | 当前验证边界 |
|---|---|---|
| Shadow Map / Depth Texture | `assets/cases/light/shadow-map/shadowMap.scene`、`shadowCSM.scene`；现有depth/stencil附件与采样路径 | shadowMap真实红→绿：默认采样纹理初始化修复；现代/Classic-fence两配置及抓帧内容通过，见[有限证据](VULKAN_SHADOW_FALLBACK_VERIFICATION_20261005.md)。shadowCSM与全族仍待验证 |
| Post Process | `assets/cases/postprocess/bloom.scene` | 隔离Builtin包真实Kawase八pass、GPU threshold0.05/1.0与同输入内容对照有限通过；滑条曾有JS null-window错误，交互未通过，见[证据](VULKAN_BLOOM_COMPUTE_VERIFICATION_20261006.md) |
| Compute | `assets/cases/GFX/compute-demo/compute-demo.scene`；`VKCommandBuffer.cpp::dispatch` | 原默认包仍legacy；默认OFF隔离资源根已满足RaytracingWeekend注册/材质/effect-layout前提。真实queue断言/storage layout/整块uniform三处功能缺陷修复；实际dispatch/32B参数/storage→fullscreen与有限golden通过，见[证据](VULKAN_BLOOM_COMPUTE_VERIFICATION_20261006.md)。完整算法/非倍数resize/异常恢复仍待，不混同预热创建 |
| GPU Instancing / Indirect | `assets/cases/scene/dynamic-batch/instanced-color.scene`、instanced-skinning；`draw`中的instanceCount/indirect分支 | Instanced-color有限通过：真实Indexed实例数1024+976、2000属性HSV一致、652种输出颜色匹配、现代/Classic-fence正常退出。见[证据](VULKAN_INSTANCING_VERIFICATION_20261005.md)。Indirect已在原工程新增默认OFF合同，索引/非索引multi/loop、实际命令/完整golden及上传RAW红→绿有限通过，见[Vulkan间接绘制证据](VULKAN_INDIRECT_DRAW_VERIFICATION_20261006.md)；skinning/其他变体不在此次范围 |
| Input Attachment / Multi Subpass | `VKUtils.cpp::mapVkDescriptorType`映射INPUT_ATTACHMENT；RP input refs与`nextSubpass`已有 | 需查实际工程是否有可用场景/合同，不编造菜单项或绕过真实工程跑smoke替代 |
| MSAA / Depth Resolve / Secondary / FSR | 原生产Classic/Hybrid门控、resolve与inheritance已有；部分独立用例有效 | 真实工程内容及拒绝边界仍待补；不改变白名单来制造Dynamic通过 |
| 上传 / Stencil / WSI | 真实WAW红→绿、菜单stencil内容、atlas/RT静态ROI及窗口/资源压力已有 | 保留已有证据，不重复调查用户排除的mask Canvas输入问题 |

异常覆盖包括版本/PFN/能力拒绝、cache数据拒绝与保存失败、资源销毁/重建、WSI失败路径；每项分别记录“实现”“执行”“内容结果”与外部条件。Android/自然低版、苹果/XR及温控不足不伪装成实现缺失，也不能标平台PASS。

## T09有限功能闭环证据

- [接线与复现说明](../../native/tests/vulkan-real-project/README.md)、[任务状态](VULKAN_COMPLETION_TASKS.md)、PROGRESS§2EE；本地raw为`native/build/vulkan-modernization/win-real-prewarm-lead/run-20261005-212151/`。
- 当前真实Debug源重编/重链，17终态进程与共享判据38/38；冷暖driver input关联、六类坏缓存拒绝、checkpoint恢复、wrong-key资源退休、默认OFF、参数拒绝与保存失败安全均分别记录。24静态ROI diff0。
- 未实施的必需优化仍是M4-B（等待M4-A）；真实场景正式测量入口尚未完成。其余特性有实现与部分证据，但T13全覆盖/平台证明不齐，不能混称全部未实现或全部通过。

## 2026-10-06 T13-02b 最新有限证据（Lead）

原Windows测试工程的[Indirect Draw合同](VULKAN_INDIRECT_DRAW_VERIFICATION_20261006.md)有限通过：真实上传RAW红→绿；索引/非索引multi与loop、32/40字节实际命令和两2048字节golden；缺失draw负例、参数拒绝、退出/Validation/泄漏与当前exe RenderDoc。历史“Indirect实际0次/待工程入口”对应Instancing捕获时点，现已独立补证。实际Classic门控保留，非Dynamic/IndirectCount/平台/性能完成声明；T13与整体验收仍待，15主任务计数不变，该时点下一批为Bloom/Compute场景前提，现见下节新证据。


## 2026-10-06 T13-03/04 新发现功能缺陷与有限内容证据（Lead）

旧表的“已有入口、内容待验证”是当时状态，不意味着内部每条功能已正确实现。本轮真实Compute发现并修复queue viewport断言、storage descriptor错误layout，以及整块uniform被生成零buffer替换；不能把后两条分别用Validation无错/对象存在掩盖。三个生产文件+22/-3，无新增submit/wait/关Validation，不是M4-B优化。

实际原Debug工程四当前EXE进程exit0/error0/leak0；三当前EXE RenderDoc捕获证明Bloom阈值内容及Compute dispatch→storage→fullscreen有限链路，共享judge5反例（含真实旧零UBO捕获）通过。[详细证据](VULKAN_BLOOM_COMPUTE_VERIFICATION_20261006.md)分列默认legacy资源与隔离custom包、混合JS/native版本、原项目设置恢复、Classic实际路径和滑条输入红例。

T13-03有限内容完成，T13-04有限内容通过/交互待验证；下一批Input Attachment/Multi Subpass及其余特性/异常/恢复继续。当前尚未实施必需优化M4-B和真实场景正式测量入口，以及全覆盖/平台证据缺口保持；不宣称只有优化待做或整体验收通过。
