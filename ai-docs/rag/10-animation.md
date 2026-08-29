---
id: kb-animation
title: 动画层 Animation
category: 动画与骨架
engine: Cocos Creator 4.x
updated: 2026-08-22
layer: L2
domain: animation
module: animation
breakable: true
tags: [animation, MAX_ANIMATION_LAYER]
sources: [cocos/3d/skeletal-animation/limits.ts:26, cocos/animation/marionette/graph-eval.ts:62, cocos/animation/marionette/graph-eval.ts:31]
---

# 动画层 Animation 规格标准

## 概述

Marionette 动画图的**动画层数有硬上限 `MAX_ANIMATION_LAYER = 32`**（引擎常量），超限在求值阶段直接报错。

## 极限参数表

| 参数 | 默认 / 上限 | 限制类型 | 说明 |
|---|---|---|---|
| 动画层数 `MAX_ANIMATION_LAYER` | **32** | 软件（常量） | `limits.ts:26` |
| 实际图层数 | 超过 32 报错 | 软件 | `graph-eval.ts:62` 校验 |

## 底层限制机制

- `MAX_ANIMATION_LAYER = 32` 定义在 `cocos/3d/skeletal-animation/limits.ts:26`，是动画图（Animation Graph）可挂载的最大 layer 数。
- 动画图求值时（`graph-eval.ts:62`）若 `graph.layers.length >= MAX_ANIMATION_LAYER` 则抛出错误，提示 `Allowed: 32, actual: N`。
- 该上限与骨骼蒙皮、渲染无关，是**动画状态机 / 混合栈的架构约束**——层数越多，逐帧混合计算量线性增长。

## 关键源码定位

- `cocos/3d/skeletal-animation/limits.ts:26` — `export const MAX_ANIMATION_LAYER = 32`
- `cocos/animation/marionette/graph-eval.ts:31,62` — 引入常量与层数校验

## 核心宏定义

无宏；由 `limits.ts` 常量控制。

## 突破限制方案

1. **放宽常量**：直接改 `limits.ts:26` 的 `MAX_ANIMATION_LAYER`（需评估逐帧混合性能与内存）。
2. **子图 / 嵌套**：将复杂逻辑拆分为多个动画控制器组件，规避单层 32 上限。
3. **精简层数**：合并语义相近的 layer，用 mask / blend 权重替代独立层。
