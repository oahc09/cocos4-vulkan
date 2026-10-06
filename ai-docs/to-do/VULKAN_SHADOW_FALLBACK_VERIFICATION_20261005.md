# 真实工程 Shadow Map 默认纹理红→绿验证（2026-10-05）

本项为 T13 的有限正确性闭环，修复级别 **Minimal Fix**。真实 Windows 工程指定 Debug `test-cases` 当前源码构建及两原生运行通过；不是全特性、全平台或性能验收。

## 根因与修复

- `GlobalDSManager::activate` 创建 2×2 RGBA8、仅 SAMPLED 的默认纹理，`bindTexture(binding, nullptr)` 会将其写入全局/灯光描述符；此前没有内容上传，物理布局仍 UNDEFINED。
- 现代路径首次进入实际 `light : shadowMap` 时，draw 采样图像 `0x1aa00000001aa`，期望 SHADER_READ_ONLY_OPTIMAL、实际 UNDEFINED，触发 `VUID-vkCmdDraw-None-09600`。原生 exit0 不能覆盖 Validation 红例。
- 临时 `CC_VK_TEXTURE_PROBE` 日志证明同一句柄为 width2/height2/Format RGBA8/usage SAMPLED。探针只用于取证，`VKCommands.cpp` 已按事前字节备份原样恢复；最终源码和 exe 不含该探针。
- 唯一生产改动：[GlobalDescriptorSetManager.cpp](../../native/cocos/renderer/pipeline/GlobalDescriptorSetManager.cpp) 增 TRANSFER_DST usage，并在发布描述符前通过现有 `copyBuffersToTexture` 上传四个白色 RGBA8 texel（16字节）。该白色与 ShadowStage 的白色清屏值一致，提供确定的默认采样内容。
- DeviceAgent 普通拷贝消息拥有输入字节/region；CCVK transport `checkIn` 即时执行 staging memcpy，录制 retain texture/view lease 至退休。没有新增 submit/wait，未放宽 Dynamic 资格、关闭 Validation 或给所有新纹理伪造布局。

## G1–G6 有限证据

| 关卡 | 结果与边界 |
|---|---|
| G1 API | 普通新建2×2 RGBA8 SAMPLED/TRANSFER_DST、单mip/layer；buffer region extent2×2×1、默认layerCount1；真实驱动和Validation接受 |
| G2 生命周期 | 两原生进程正常WM_CLOSE、exit0、泄漏0；现代进程菜单返回再入，两个配置均resize/minimize/restore。输入复制与recording lease沿既有路径 |
| G3 同步 | 首上传 UNDEFINED→TRANSFER_DST，dst TRANSFER/TRANSFER_WRITE；上传 producer TRANSFER/TRANSFER_WRITE→采样 consumer FRAGMENT_SHADER/SHADER_READ、最终SHADER_READ_ONLY；沿现有上传和BarrierManager依赖，无额外广域等待 |
| G4 Validation | SDK层真实启用；现代 paths timeline1/sync2=1/submit2=1/dynamicRendering1，强制1.1 Classic/fence paths全0且classicOverride1。两例ERROR/09600/SYNC-HAZARD/JS0、唯一leak0；BestPractices advisories单独保留，不称warning0 |
| G5 RenderDoc | 实际51 draw、10 Classic RP、51 Barrier2、1 Submit2。唯一2×2/format37/usage6 image ResourceId::651，被8个实际draw采样；16字节均255。四张512×512 R32_FLOAT输出均先写后读，深度范围有变化 |
| G6 平台 | 本机Windows RTX窗口恢复有限通过；强制API1.1不替代自然低版本GPU。Android/Apple/XR本轮未运行、仍待平台证据 |

阴影color附件的历史prev为FRAGMENT_READ，depthStore为DISCARD，不满足现有Dynamic白名单；抓帧实际Classic正确保留，不以策略ON宣称BeginRendering命中。没有更改该门控。

## 可复查原始证据

- 本地忽略区根 `native/build/vulkan-modernization/win-render-features-t13-lead/run-20261005-222623337071/`：`modern/`原红、`texture-probe/`句柄实证、`fixed-modern/`、`fixed-legacy/`两绿。`build-fixed-exit.txt`为0且目标实际重编/重链；`native-judgment.json`、`shadow-terminal.json`为有限终态。
- 最终 exe SHA256 `6db8457a3187863a3f1531f609891c0246538ccc0a79c2ee1004e427fe9dacf7`；最终模块与exe哈希在两运行及收口检查中一致。
- `renderdoc-shadow-vulkan/shadow_frame10745.rdc`、`replay.json`、`image-create-chunks.json`：主阴影深度0.7050304413..1.0（21000个不同值），另外三张spot阴影均有独立非恒定深度；生产/采样事件分别26→142、73→174、89→176、106→178。
- 历史失败保留：最初临时探针脚本错误匹配CRLF导致未插入，后按真实LF修正；搜索控件不存在的操作已拒绝、后经当前菜单确认；RenderDoc第一轮连接失败、第二轮Python3.6不支持capture_output、第三轮隔离目录排除了捕获层而超时。最终只放行RenderDoc隐式层后捕获成功；初版replay将格式名/动态策略误作实际pass条件，修正真实format/Classic边界后再读唯一usage6资源并断言完整字节。未把任何失败标为通过。

## 下一项

T13 仍进行中：GPU Instancing/Indirect、Compute管线前提及真实dispatch内容、Bloom实际pass与阈值内容、多subpass/input attachment、MSAA/secondary/FSR的真实工程资格与拒绝覆盖。T09有限功能证据保持其历史源码边界；正式性能随后T05，M4-B继续等M4-A。当前未提交Git。
