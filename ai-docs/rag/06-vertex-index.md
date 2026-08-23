---
id: kb-vertex-index
title: 顶点 / 索引 / 属性规格标准
category: 渲染 / 几何
engine: Cocos Creator 4.x
updated: 2026-08-22
sources: [cocos/core/platform/macro.ts:1073, cocos/3d/assets/mesh.ts:511, cocos/2d/renderer/render-data.ts:637, cocos/2d/assembler/graphics/webgl/graphics-assembler.ts:34, cocos/scene-graph/layers.ts:33]
---

# 顶点 / 索引 / 属性规格标准

## 概述

索引缓冲默认以 Uint16 寻址，单批顶点上限 **65535**（索引 0–65535）；顶点属性槽位数由 `DeviceCaps.maxVertexAttributes` 决定。

## 极限参数表

| 参数 | 默认 / 上限 | 限制类型 | 说明 |
|---|---|---|---|
| Uint16 索引寻址顶点数 | **65536**（索引 0–65535） | 硬件+软件 | 默认索引格式 |
| WebGL1 顶点上限 | 65535；启用 `OES_element_index_uint` 后达 2³²−1 | 硬件 | `macro.ts:1073` 注释 |
| Graphics 2D 装配器 `MAX_VERTEX` | **65535** | 软件（常量） | `graphics-assembler.ts:34` |
| Graphics 2D `MAX_INDICES` | **65535 × 2** | 软件（常量） | `graphics-assembler.ts:35` |
| 顶点属性槽位数 `maxVertexAttributes` | GLES3 ≥16；Vulkan = `maxVertexInputAttributes` | 硬件 | `DeviceCaps` |

## 底层限制机制

- **Uint16 上限根因**：索引缓冲默认用 16 位无符号整数，最大值 65535，故单批最多 65536 个顶点（含 0）。引擎在合并顶点缓冲时显式校验：`mesh.ts:511` 与 `render-data.ts:637` 均判断 `vertexCount > 65536` 即超出 Uint16 寻址。
- **WebGL1 的 65535**：WebGL1 原生不支持 32 位索引，引擎通过扩展 `OES_element_index_uint` 突破到 2³²−1（`macro.ts:1073`）。
- **属性槽位**：顶点属性（position/normal/uv/joints/weights…）每个占用一个 attribute location，总数受 `maxVertexAttributes` 约束（通常 16，桌面 Vulkan 可达 32 或更高）。蒙皮需要 `a_position + a_normal + a_joints + a_weights` 等多个槽位，会占用属性预算。

## 关键源码定位

- `cocos/core/platform/macro.ts:1073` — WebGL1 顶点上限与 `OES_element_index_uint` 突破说明
- `cocos/3d/assets/mesh.ts:511` — `vertexCount > 65536` Uint16 校验
- `cocos/2d/renderer/render-data.ts:637` — 2D 合并缓冲 65536 校验
- `cocos/2d/assembler/graphics/webgl/graphics-assembler.ts:34` — `MAX_VERTEX = 65535`、`MAX_INDICES = MAX_VERTEX * 2`
- `cocos/gfx/base/define.ts:751` — `DeviceCaps.maxVertexAttributes`

## 核心宏定义

无单一宏；索引格式在网格导入 / 构建时确定（Uint16 / Uint32）。

## 突破限制方案

1. **32 位索引**：在支持 `OES_element_index_uint`（WebGL1）或原生 32 位索引（WebGL2 / Vulkan）的平台使用 Uint32 索引，单批顶点上限提升到 2³²−1。
2. **分批 / 分网格**：超大网格拆分为多个 ≤65535 顶点的子网格，分别提交。
3. **精简顶点属性**：合并 / 移除冗余 attribute，降低对 `maxVertexAttributes` 的占用（尤其蒙皮模型）。
4. **动态合批上限**：2D `MAX_VERTEX` 为软常量，可在源码放宽，但需同步评估属性与索引缓冲容量。
