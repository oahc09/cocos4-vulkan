---
id: kb-label-pool
title: 文本 Label 画布池规格标准
category: 2D / UI
engine: Cocos Creator 4.x
updated: 2026-08-22
sources: [cocos/core/platform/macro.ts:1134, cocos/2d/assembler/label/font-utils.ts:70]
---

# 文本 Label 画布池规格标准

## 概述

基于画布的文本（Canvas / system font 标签）渲染使用**对象池**，池大小上限为 **`MAX_LABEL_CANVAS_POOL_SIZE = 20`**，属引擎常量。

## 极限参数表

| 参数 | 默认 / 上限 | 限制类型 | 说明 |
|---|---|---|---|
| Label 画布池大小 `MAX_LABEL_CANVAS_POOL_SIZE` | **20** | 软件（常量） | `macro.ts:1134` |
| 池满行为 | 复用最旧画布（`pool.length >= SIZE` 时不再新增） | 软件 | `font-utils.ts:70` |
| 受影响标签类型 | Canvas / system font 文本（非 BMFont / char 模式） | 架构 | — |

## 底层限制机制

- `macro.ts:1134` 定义 `MAX_LABEL_CANVAS_POOL_SIZE: 20`，作为 Label 文本渲染时复用离屏画布（canvas）的对象池容量。
- `font-utils.ts:70` 在取画布时判断 `if (this.pool.length >= macro.MAX_LABEL_CANVAS_POOL_SIZE)`——池满后**不再创建新画布**，而是复用池中已有画布（最旧的被回收）。
- **为何是 20**：离屏 canvas 是显存 / 内存占用大户，固定池上限避免大量不同字号 / 样式的文本无限创建画布导致内存膨胀。代价是超出 20 种不同渲染参数的标签会共享/复用画布，可能引发轻微闪烁或重绘。

## 关键源码定位

- `cocos/core/platform/macro.ts:1050,1134` — `MAX_LABEL_CANVAS_POOL_SIZE` 声明与默认值 20
- `cocos/2d/assembler/label/font-utils.ts:70` — 画布池取用与容量判断

## 核心宏定义

`macro.MAX_LABEL_CANVAS_POOL_SIZE`（引擎宏，非 shader 宏）

## 突破限制方案

1. **放宽池大小**：改 `macro.ts:1134` 的 `MAX_LABEL_CANVAS_POOL_SIZE`（代价：显存 / 内存占用上升）。
2. **复用文本样式**：尽量统一同屏标签的字号 / 描边 / 阴影等参数，使其命中同一画布，减少池压力。
3. **改用 BMFont / char 模式**：位图字体不走 canvas 池，从根本规避该上限（适合大量静态文本）。
4. **合批文本**：使用 `LabelAtlas` / 缓存纹理，减少独立 canvas 数量。
