# 24 · 后处理 PostProcess

## 概述
后处理系统（`cc.PostProcess` + `PostProcessSetting` 系列）在自定义渲染管线末尾对相机画面施加屏幕特效（Bloom、ToneMapping、FXAA、DOF、HBAO 等）。引擎层**不设特效总数量的硬上限**——特效以 `Map<PostProcessSetting>` 形式挂载；但各特效内部存在具体常数（如 Bloom 降/升采样级数）。核心约束来自 RT 分辨率（`shadingScale`）与逐 pass 的 GPU 开销。

## 极限参数表

| 参数 | 默认/上限 | 限制类型 | 说明 |
|---|---|---|---|
| 单相机挂接特效数 | 无 `MAX_` 常量（Map 动态增长） | 软件 | `settings: Map<PostProcessSetting>`，受内存与可用 Setting 子类约束 |
| 同节点 PostProcess 数 | `disallowMultiple`（1 个） | 软件 | 每个节点仅允许 1 个 `cc.PostProcess` |
| Bloom 滤波级数 `MAX_BLOOM_FILTER_PASS_NUM` | **6** | 软件/常量 | 降采样 6 + 升采样 6 + 合并 1 |
| 后处理 RT 分辨率缩放 `shadingScale` | 范围 `[0.01, 4]`，默认 1 | 软件 | 缩放后处理渲染目标尺寸 |
| 单管线 pass 数 | 无 `MAX_` 常量（Map 动态） | 软件 | `pipelines: Map<string, BasePass[]>` |
| PostProcess 实例数 | 静态数组 `PostProcess.all`，按相机 | 软件 | 通常一相机一实例，无硬上限 |

## 底层限制机制
- **以 Setting 为单位挂载**：`cc.PostProcess` 持有 `settings: Map<PostProcessSetting, PostProcessSetting>`（`post-process.ts:47`）。新增一种特效即实现 `PostProcessSetting` 子类（如 `Bloom`、`Fxaa`、`Dof`）并 `addSetting`。数量无常量上限，仅受"你启用了哪些 Setting"与对应 RT/中间纹理内存限制。
- **`disallowMultiple`**：组件装饰器保证单个节点仅一个 `cc.PostProcess`；多相机场景各自挂组件，`PostProcess.all` 静态数组跟踪全部实例。
- **Bloom 多级常量**：`bloom-pass.ts:12` 定义 `MAX_BLOOM_FILTER_PASS_NUM = 6`，据此推导 `BLOOM_DOWNSAMPLEPASS_INDEX=1`、升采样起点 `=1+6`、合并 pass `=1+12`。级数固定，不可配置。
- **`shadingScale` 分辨率缩放**：`post-process.ts:28` 用 `@range([0.01,4,0.01])` 限定后处理 RT 缩放比例，直接决定后处理阶段的像素吞吐与显存带宽。

## 关键源码定位
- 组件与 settings：`cocos/rendering/post-process/components/post-process.ts:7-69`（`@disallowMultiple`、`:47 settings Map`、`:28 shadingScale range`）
- Bloom 级数常量：`cocos/rendering/post-process/passes/bloom-pass.ts:12-15`
- 管线与 pass 注册：`cocos/rendering/post-process/post-process-builder.ts:27`（`pipelines: Map<string, BasePass[]>`）、`:45-71`（默认/自定义管线添加各 pass）
- 可用特效 Setting：`cocos/rendering/post-process/components/`（Bloom、Fxaa、Dof、ColorGrading、ToneMapping 等）

## 核心宏定义
- `MAX_BLOOM_FILTER_PASS_NUM = 6`（`bloom-pass.ts:12`，降/升采样各 6 级）
- `@disallowMultiple`（装饰器，限制单节点 1 个 `cc.PostProcess`）
- `shadingScale` `@range([0.01, 4, 0.01])`（`post-process.ts:28`）

## 突破限制方案
1. **特效叠加**：无数量上限，可同时挂 Bloom + DOF + FXAA + ToneMapping 等；但每多一个 Setting 即多一组 RT 与 pass，按目标机型做分级（低端机关 HBAO/DOF）。
2. **降带宽**：`shadingScale` 设为 `0.5~0.75` 可显著降低后处理像素开销，是移动端最划算的优化；注意会牺牲后处理清晰度。
3. **Bloom 调优**：级数固定为 6，不可增减；如需更长拖尾可改 Bloom 半径/阈值，而非级数。
4. **RT 尺寸**：后处理中间纹理受最大纹理尺寸（见 `07-texture-material-pass.md`）约束，`shadingScale>1` 时可能超设备上限，需自动回落。
