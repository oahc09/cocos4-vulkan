---
id: kb-terrain
title: 地形 Terrain 规格标准
category: 场景 / 地形
engine: Cocos Creator 4.x
updated: 2026-08-22
sources: [cocos/terrain/terrain-asset.ts:30, cocos/terrain/terrain.ts:1265, cocos/terrain/terrain-asset.ts:TERRAIN_MAX_BLEND_LAYERS]
---

# 地形 Terrain 规格标准

## 概述

地形的可绘制图层数有硬上限 `TERRAIN_MAX_LAYER_COUNT = 256`，混合（blend）层数另有独立上限，二者均为引擎常量。

## 极限参数表

| 参数 | 默认 / 上限 | 限制类型 | 说明 |
|---|---|---|---|
| 地形最大图层数 `TERRAIN_MAX_LAYER_COUNT` | **256** | 软件（常量） | `terrain-asset.ts:30` |
| 地形最大混合层数 `TERRAIN_MAX_BLEND_LAYERS` | 常量（见 `terrain-asset.ts`） | 软件 | 单次可混合的纹理层数 |
| 高度场范围 `TERRAIN_HEIGHT_FMIN/FMAX` | 常量 | 软件 | 高度量化范围 |

## 底层限制机制

- 图层（layer）指地形可引用的贴图材质槽位总数，`TERRAIN_MAX_LAYER_COUNT = 256` 决定地形资产中可登记的最大图层数（`terrain.ts:1265` 遍历所有图层）。
- 混合层数（`TERRAIN_MAX_BLEND_LAYERS`）限制单个地形块（tile）渲染时实际参与混合的图层数，影响 shader 中的采样循环展开规模——这是性能与 shader 复杂度的权衡点。
- 高度数据以固定 bit 量化存储，`TERRAIN_HEIGHT_FMIN/FMAX` 决定可表示的高度范围。

## 关键源码定位

- `cocos/terrain/terrain-asset.ts:30` — `export const TERRAIN_MAX_LAYER_COUNT = 256`
- `cocos/terrain/terrain-asset.ts` — `TERRAIN_MAX_BLEND_LAYERS`、`TERRAIN_HEIGHT_FMIN/FMAX`、`TERRAIN_DATA_VERSION5`
- `cocos/terrain/terrain.ts:1265` — `for i < TERRAIN_MAX_LAYER_COUNT` 遍历图层

## 核心宏定义

无宏；由 `terrain-asset.ts` 的导出常量控制。

## 突破限制方案

1. **放宽图层 / 混合层常量**：直接改 `TERRAIN_MAX_LAYER_COUNT` / `TERRAIN_MAX_BLEND_LAYERS`（需同步评估 shader 采样循环与内存）。
2. **虚拟纹理 / 流送**：超大量图层改用虚拟纹理（virtual texture）或运行时贴图流送，避免全部图层常驻。
3. **分块混合**：单 tile 仅混合视域内少数图层，降低 `TERRAIN_MAX_BLEND_LAYERS` 实际压力。
