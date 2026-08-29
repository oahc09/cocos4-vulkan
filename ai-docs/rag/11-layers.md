---
id: kb-layers
title: 节点层级 Layers
category: 场景与系统
engine: Cocos Creator 4.x
updated: 2026-08-22
layer: L2
domain: scene
module: layers
breakable: false
tags: [layers, bitmask, uint32]
sources: [cocos/scene-graph/layers.ts:33, cocos/scene-graph/layers.ts:34, cocos/scene-graph/layers.ts:66, layers.ts:45, cocos/scene-graph/layers.ts:72]
---

# 节点层级 Layers 规格标准

## 概述

节点的 `layer` 以 **uint32 位掩码**存储，**总共 32 层**（bit 0–31）；其中 **bit 0–19 为用户可用**（20 个用户层），bit 20–31 为系统保留。

## 极限参数表

| 参数 | 默认 / 上限 | 限制类型 | 说明 |
|---|---|---|---|
| 层级总数 | **32 层**（bit 0–31，uint32 掩码） | 硬件+软件 | `Node.layer` 为 32 位整数 |
| 用户可用层 | **bit 0–19（20 个用户层）** | 软件（约定） | `layers.ts:33` 注释 |
| 系统保留层 | bit 20–31（IGNORE_RAYCAST/GIZMOS/EDITOR/UI_3D/SCENE_GIZMO/UI_2D/PROFILER/DEFAULT 等） | 软件（约定） | `layers.ts:34` |
| `ALL` 掩码 | `0xffffffff`（全部 32 层） | 软件 | `layers.ts:45` |

## 底层限制机制

- `Layers` 用 32 位整数作位掩码：`Node.layer` 的每个 bit 表示一层，节点可同时属于多层（包含式 / 排除式检测）。
- `layerList`（`layers.ts:34`）定义内置层：用户层占用 bit 0–19；bit 20–31 被引擎内部占用（射线检测、Gizmos、编辑器、UI、Profiler、DEFAULT 等）。源码注释明确「users can use 0~19 bits, 20~31 are system preserve bits」。
- 因是 uint32，**总层数固定为 32**，无法突破；用户只能在 20 个可用 bit 内自定义命名层。

## 关键源码定位

- `cocos/scene-graph/layers.ts:33` — 注释：用户可用 0~19 bit，20~31 系统保留
- `cocos/scene-graph/layers.ts:34-46` — `layerList`（内置层与 bit 位）
- `cocos/scene-graph/layers.ts:66` — `Layers.Enum = Enum(layerList)`
- `cocos/scene-graph/layers.ts:72` — `Layers.BitMask = BitMask({...})`

## 核心宏定义

无宏；由 `Layers.Enum` / `Layers.BitMask` 枚举与位运算（`1 << bit`）驱动。

## 突破限制方案

1. **复用位 / 动态层**：在 20 个用户 bit 内合理规划层语义，用包含式 / 排除式掩码组合替代"新增层"。
2. **分组管理**：用节点树 / 分组标签（而非 layer）承载超出 20 层的分类需求。
3. **注意保留位**：切勿占用 bit 20–31，否则与引擎内部层（UI / 射线 / Gizmos）冲突。
4. **layer 不可突破 32**：若业务需要 >32 分类，必须改用自定义分组系统，而非扩展 `Layers`。
