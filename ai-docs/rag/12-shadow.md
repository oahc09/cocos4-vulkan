---
id: kb-shadow
title: 阴影 Shadow 规格标准
category: 渲染 / 阴影
engine: Cocos Creator 4.x
updated: 2026-08-22
sources: [cocos/render-scene/scene/shadows.ts:73, cocos/render-scene/scene/shadows.ts:330, cocos/rendering/define.ts:355, cocos/scene-graph/scene-globals.ts:999]
---

# 阴影 Shadow 规格标准

## 概述

阴影系统的关键上限为：级联阴影（CSM）**固定 4 级**、每帧接收阴影的 punctual 光数 `maxReceived = 4`、阴影贴图尺寸可配（受 `maxTextureSize` 约束）。

## 极限参数表

| 参数 | 默认 / 上限 | 限制类型 | 说明 |
|---|---|---|---|
| CSM 级联级数 `CSM_LEVEL_COUNT` | **4** | 软件（常量） | `define.ts:356`，UBO 固定 4 级 |
| 每帧接收阴影的 punctual 光数 `maxReceived` | **4**（可配置） | 软件 | `shadows.ts:330` |
| 阴影贴图尺寸 `shadowMapSize` | 可配（常见 1024/2048/4096），受 `maxTextureSize` 约束 | 软件+硬件 | `shadows.ts:376` |
| 阴影类型 `ShadowType` | `Planar` / `ShadowMap` / `NONE` | 软件（枚举） | `shadows.ts:73` |
| 单个阴影帧缓冲 | 1 张 ShadowMap（或 CSM atlas） | 架构 | — |

## 底层限制机制

- **CSM 级数固定 4**：`UBOCSMEnum.CSM_LEVEL_COUNT = 4`（`define.ts:355`），CSM 的视图方向、atlas、view-proj、split 信息均按 4 级展开写入 `UBOCSM`（656 B）。这是 UBO 布局硬编码，改级数需同步调整 `UBOCSMEnum` 全部 offset 与 `UBOCSM.LAYOUT`。
- **`maxReceived = 4`**：限制同一帧内对某个物体产生阴影的 punctual 光数量（`shadows.ts:330`），由 `ShadowsInfo.maxReceived` 配置（默认 4）。超出部分的光不投阴影，避免阴影贴图数量爆炸。
- **ShadowMap 尺寸**：在 `shadows.ts:376` 处从 `ShadowsInfo.shadowMapSize` 读取并 resize 帧缓冲；上限受 `DeviceCaps.maxTextureSize` 限制（超过会触发纹理超限报错）。
- **阴影类型**：`ShadowType.Planar`（平面投影）与 `ShadowType.ShadowMap`（深度贴图）二选一；`NONE` 关闭。

## 关键源码定位

- `cocos/rendering/define.ts:355-401` — `UBOCSMEnum`（CSM_LEVEL_COUNT=4）、`UBOCSM` 布局
- `cocos/render-scene/scene/shadows.ts:73` — `ShadowType` 枚举
- `cocos/render-scene/scene/shadows.ts:330` — `maxReceived = 4`
- `cocos/render-scene/scene/shadows.ts:376` — `shadowMapSize` resize
- `cocos/scene-graph/scene-globals.ts:999` — `ShadowsInfo.maxReceived` 配置入口

## 核心宏定义

`CC_SHADOW_TYPE`（阴影类型切换）、`CC_ENABLE_CASCADED_SHADOW_MAP`（CSM 开关，若有）

## 突破限制方案

1. **更多阴影光**：调大 `ShadowsInfo.maxReceived`（代价：阴影贴图内存与绘制次数线性增长）。
2. **更高阴影精度**：增大 `shadowMapSize`（受 `maxTextureSize` 约束），或用 CSM 4 级提升远距离质量。
3. **改 CSM 级数**：需同步改 `UBOCSMEnum` 的 `CSM_LEVEL_COUNT` 及全部 offset / `UBOCSM.LAYOUT`（工作量大，且 UBO 体积增大）。
4. **静态阴影烘焙**：将不动的光烘焙进 lightmap，规避实时阴影的 `maxReceived` 与贴图开销。
