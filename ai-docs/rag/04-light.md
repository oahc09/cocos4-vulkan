---
id: kb-light
title: 光源与光照 Light
category: 3D 渲染模块
engine: Cocos Creator 4.x
updated: 2026-08-22
layer: L2
domain: rendering
module: light
breakable: true
tags: [light, forward, deferred, LIGHTS_PER_PASS]
sources: [cocos/rendering/define.ts:549, cocos/rendering/render-additive-light-queue.ts:478, cocos/rendering/scene-culling.ts:53, cocos/render-scene/scene/shadows.ts:330, editor/assets/chunks/builtin/uniforms/cc-forward-light.chunk, cc-forward-light.chunk:3, define.ts:577, render-additive-light-queue.ts:123, define.ts:289, cocos/rendering/custom/define.ts:70, cocos/scene-graph/scene-globals.ts:999]
---

# 光源控制 Light 规格标准

## 概述

Forward 管线每 pass 仅处理 1 个 punctual 光（逐光 forward-add），Deferred/Cluster 每 pass 10 个；**场景灯光总数无硬上限**，由动态 UBO + 逐光 draw 决定，是性能约束而非数量约束。

## 极限参数表

| 参数 | 默认 / 上限 | 限制类型 | 说明 |
|---|---|---|---|
| Forward 每 pass 灯光数 `LIGHTS_PER_PASS` | **1** | 软件 | shader 固定 1 |
| Deferred / Cluster 每 pass 灯光数 | **10** | 软件 | — |
| 场景灯光总数 | **无硬上限**（视锥剔除动态收集，light buffer `nextPow2` 动态扩容） | 性能 | 每光一次 draw |
| 主光（方向光） | **1** 个（存 `UBOCamera`） | 架构约定 | — |
| 阴影投射光上限 `maxReceived` | **4**（可配置） | 软件 | 每帧接收阴影的 punctual 光数 |
| 每光 UBO 尺寸 | 5 × vec4 = **80 字节** | 数据布局 | — |

## 底层限制机制

- Forward 为「主光 base pass + 每 punctual 光 1 次 forward-add pass」：`LIGHTS_PER_PASS` 固定为 1（`cc-forward-light.chunk:3`）。
- 灯光数据写入 `UBOForwardLight`，是**动态 UBO**（`DescriptorType.DYNAMIC_UNIFORM_BUFFER`，`define.ts:577`）：一个大 light buffer 里每光一个 stride（对齐 `uboOffsetAlignment`），通过 dynamic offset 逐光切换（`render-additive-light-queue.ts:478`）。
- 光数量动态：`validPunctualLights` 由视锥剔除从 `spotLights/sphereLights/pointLights/rangedDirLights` 收集（`scene-culling.ts:53`）；light buffer 初始 16 条、超限 `nextPow2` 扩容（`render-additive-light-queue.ts:123,485`）。**因此灯光数无硬上限，纯性能约束（每光多一次 draw）**。
- 主光（1 个方向光）单独走 `UBOCamera.cc_mainLitDir / cc_mainLitColor`（`define.ts:289`）。
- 阴影：`maxReceived = 4` 限制每帧接收阴影的 punctual 光数量（`shadows.ts:330`、`scene-globals.ts:999`）。

## 关键源码定位

- `cocos/rendering/define.ts` — `UBOForwardLightEnum`(L549)、`UBOForwardLight`(L564)、`UBODeferredLight`(L596)
- `cocos/rendering/render-additive-light-queue.ts` — `_lightBufferCount`(L123)、`_updateUBOs`(L478)
- `cocos/rendering/scene-culling.ts:53`、`cocos/rendering/custom/define.ts:70` — 光收集 / 剔除
- `cocos/render-scene/scene/shadows.ts:330`、`cocos/scene-graph/scene-globals.ts:999` — 阴影上限
- GLSL — `editor/assets/chunks/builtin/uniforms/cc-forward-light.chunk`

## 核心宏定义

`CC_PIPELINE_TYPE`（`CC_PIPELINE_TYPE_FORWARD` 等）、`CC_ENABLE_CLUSTERED_LIGHT_CULLING`、`LIGHTS_PER_PASS`

## 突破限制方案

1. **减少 draw 数**：切换 Deferred / Cluster 管线（`LIGHTS_PER_PASS = 10`），一次 pass 处理 10 光。
2. **Forward 下优化灯光数**：缩小 `_lightBufferStride` 对齐浪费、关闭不必要光阴影、烘焙静态光到 lightmap。
3. **提高阴影接收光数**：调大 `ShadowsInfo.maxReceived`（代价：阴影贴图内存与带宽线性增长）。
4. **大规模动态光**：启用 cluster 光剔除（`CC_ENABLE_CLUSTERED_LIGHT_CULLING`）替代逐光 forward-add。
