# 真实 Windows 工程 WSI 补充验证（2026-10-06）

执行渠道：Lead。实际原工程 Debug test-cases，现有 DeviceAgent/CCVKDevice，默认 OFF 的 `CC_VULKAN_REAL_PROJECT_WSI` 入口。没有创建第二个引擎 Device。

当前源码构建 exit0，exe SHA256：`5545c99fe4475bbd52658323e5ffb790002c0c68742c0756d7319a945cb38cf6`。

## 有限结果

- modern：实际 timeline/Sync2/Submit2，native exit0；legacy：effective1.1/fence/legacy，native exit0。
- S1 稳定120帧；S2 30次重建；S3 零尺寸挂起与恢复；S4 实际销毁/重建surface；S5 两个真实Win32窗口/swapchain，A挂起时B继续10帧；S6 全acquire失败/空提交；S7 明确注入OUT_OF_DATE/SURFACE_LOST/SUBOPTIMAL返回值；S8 离屏过程不复活无效image index。
- 每个正例实际4个surface、39个swapchain、179次present、14次双swapchain present；43个目标句柄均恰销毁一次，实际device销毁一次，live/post Validation0、VMA泄漏0。
- skip-surface：实际漏掉重建后先acquire，原渲染状态断言失败；随后真实修复surface并完成清理，native exit1。bad-mode在device创建前拒绝，exit1。
- 共享 `verify-wsi-project.py` 重判两正例、真实漏重建负例及14个单条件判据反例，exit0。

## 证据与边界

证据：`native/build/vulkan-modernization/win-wsi-t13-lead/run-20261006-122929997196/`，逐案原始debug-output/runtime-summary/build日志与terminal.json。

这里证明WSI API、生命周期与恢复，不包含shader draw或像素golden；真实场景视觉证据另见T03/T04/T08。S7是显式结果注入，不能称真实驱动故障或device-loss恢复。原独立wsi_smoke.cpp未改，生产WSI同步未改；默认入口OFF。
