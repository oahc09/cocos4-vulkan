---
id: kb-dynamic-atlas
title: 2D 动态合图 Dynamic Atlas
category: 2D 与 UI
engine: Cocos Creator 4.x
updated: 2026-08-29
layer: L2
domain: ui
module: dynamic-atlas
breakable: true
tags: [dynamic-atlas, batch, drawcall, maxFrameSize]
sources: [cocos/2d/utils/dynamic-atlas/atlas-manager.ts:51, cocos/2d/utils/dynamic-atlas/atlas-manager.ts:52, cocos/2d/utils/dynamic-atlas/atlas-manager.ts:53, cocos/2d/utils/dynamic-atlas/atlas-manager.ts:155, cocos/2d/utils/dynamic-atlas/atlas-manager.ts:170, cocos/2d/utils/dynamic-atlas/atlas-manager.ts:189, cocos/2d/framework/sprite-frame.ts:35]
---

# 2D 动态合图 Dynamic Atlas

## 概述
动态合图在运行时把零散的小 SpriteFrame 自动打包进少数大纹理，让本不相邻的图元共享同一张贴图从而**合并 DrawCall**。它是 2D/UI 性能优化的第一道闸门，其规格由三个可调参数决定：**最多 5 张图集、每张 2048²、单帧最大 512²**。

## 极限参数表

| 参数 | 默认值 | 限制类型 | 说明 |
|---|---|---|---|
| `maxAtlasCount` 图集张数上限 | **5** | 软件（可配） | 超出后不再创建新图集 |
| `textureSize` 单图集尺寸 | **2048** px（正方形） | 软件 + 硬件 | 受 `maxTextureSize` 约束 |
| `maxFrameSize` 单帧最大尺寸 | **512** px | 软件（可配） | 超过此尺寸的图**不参与**动态合图 |
| `enabled` 开关 | `!macro.CLEANUP_IMAGE_CACHE` | 软件（自动推导） | 项目设置间接控制 |
| 理论最大显存占用 | 5 × 2048² × 4B ≈ **80 MB** | 显存 | RGBA8 估算，含 mip 更高 |

## 底层限制机制
- **图集耗尽即降级**：`atlas-manager.ts:155` 只有 `this._atlasIndex < this.maxAtlasCount` 时才 `new Atlas(textureSize, textureSize)`；`atlas-manager.ts:189` 与 `:206` 在 `_atlasIndex >= _maxAtlasCount` 时直接放弃打包。此时碎图回落为独立纹理，**DrawCall 悄然上升**——这是动态合图"失效"的典型根因，且不会报错。
- **大图拒收**：单帧尺寸超过 `maxFrameSize = 512` 的精灵不进入动态图集（避免图集碎片化、利用率骤降）。这类大图应走静态图集或独立纹理。
- **开关联动**：`atlas-manager.ts:170` 把 `enabled` 绑定为 `!macro.CLEANUP_IMAGE_CACHE`。若项目开启了图像缓存清理，动态合图会被自动关闭——排查合图失效时需先确认这一点。
- **合图与合批的关系**：合图解决"贴图不同"导致的批次断裂，但**不同材质/混合模式/层级**仍会断批。合图不是 DrawCall 的万能解。

## 关键源码定位

| 位置 | 内容 |
|---|---|
| `cocos/2d/utils/dynamic-atlas/atlas-manager.ts:51` | `private _maxAtlasCount = 5;` |
| `cocos/2d/utils/dynamic-atlas/atlas-manager.ts:52` | `private _textureSize = 2048;` |
| `cocos/2d/utils/dynamic-atlas/atlas-manager.ts:53` | `private _maxFrameSize = 512;` |
| `cocos/2d/utils/dynamic-atlas/atlas-manager.ts:155-156` | 图集数量未达上限时新建 `Atlas` |
| `cocos/2d/utils/dynamic-atlas/atlas-manager.ts:170` | `this.enabled = !macro.CLEANUP_IMAGE_CACHE;` |
| `cocos/2d/utils/dynamic-atlas/atlas-manager.ts:189` | 图集耗尽时放弃打包 |
| `cocos/2d/framework/sprite-frame.ts:35` | SpriteFrame 引用 `dynamicAtlasManager` |

> 注：`sprite-frame.ts` 位于 `cocos/2d/assets/sprite-frame.ts`（导入路径写作 `../utils/dynamic-atlas/atlas-manager`）。

## 核心宏定义与常量
- `maxAtlasCount = 5` —— 动态图集张数上限
- `textureSize = 2048` —— 单张图集边长
- `maxFrameSize = 512` —— 参与合图的单帧尺寸上限
- `macro.CLEANUP_IMAGE_CACHE` —— 反相控制合图开关

## 突破限制方案
1. **调大 `maxAtlasCount`**：碎图极多时提到 8~10，代价是显存按 `张数 × size² × 4B` 线性增长。移动端需谨慎（5 张 2048² 已约 80 MB）。
2. **调大 `maxFrameSize`**：让中等尺寸图标也进图集；但过大会迅速碎片化、反而降低空间利用率，一般不超过 1024。
3. **优先用静态图集**：对**确定的、稳定的** UI 图标集，制作期打成静态图集（`.plist` / 图集资源）优于运行时动态合图——零运行时开销、无碎片、可控。
4. **大图单独处理**：超过 512 的背景/立绘不指望动态合图，改为独立纹理并按渲染顺序排布，减少状态切换。
5. **关掉 `CLEANUP_IMAGE_CACHE`**：若发现合图整体失效，检查该宏是否被开启导致 `enabled = false`。

## 关联条目
- [13 UI 遮罩嵌套](./13-ui-mask.md) —— 同属 2D 合批影响因素
- [22 Spine / DragonBones](./22-spine-dragonbones.md) —— 2D 顶点与合批公共约束
- [07 纹理 / 材质 / Pass](./07-texture-material-pass.md) —— `maxTextureSize` 是 `textureSize` 的硬件天花板
- [18 Label 画布池](./18-label-pool.md) —— 文本图集相关
