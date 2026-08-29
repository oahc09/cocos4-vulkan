---
id: kb-blendshape
title: BlendShape 形变动画
category: 3D 渲染模块
engine: Cocos Creator 4.x
updated: 2026-08-22
layer: L2
domain: rendering
module: morph
breakable: true
tags: [blendshape, morph, MAX_MORPH_TARGET_COUNT]
sources: [cocos/rendering/define.ts:694, cocos/3d/assets/morph-rendering.ts:120, cocos/3d/models/morph-model.ts, editor/assets/chunks/builtin/uniforms/cc-morph.chunk, morph-animation.chunk:62, cc-morph.chunk:6, morph-rendering.ts:535, cc-morph.chunk:3, morph-rendering.ts:159]
---

# BlendShape / Morph 规格标准

## 概述

BlendShape（Morph）的形变 target 数在 GPU 路径下受固定尺寸权重 UBO 约束，上限 **60**；超出自动回退 CPU 计算（无硬上限）。

## 极限参数表

| 参数 | 默认 / 上限 | 限制类型 | 说明 |
|---|---|---|---|
| `MAX_MORPH_TARGET_COUNT`（GPU 路径上限） | **60** | 软件 | UBO 权重数组固定 60 |
| 权重 UBO 数组 | `cc_displacementWeights[15]` = 15 vec4 = **60 float** | 数据布局 | — |
| 超过 60 target | 自动回退 **CPU 计算**（无硬上限） | 软件 | — |
| 位移纹理格式 | `RGBA32F`（浮点）或 `RGBA8888`（decode32 打包 ×4） | 硬件能力 | — |
| 位移纹理容量 | `nextPow2(顶点数 × target 数)`，宽高 2 的幂、宽为 4 倍数；上限受 `maxTextureSize` | 硬件+软件 | — |

## 底层限制机制

- GPU 形变把每个 target 的位移写进一张纹理，顶点着色器按 `nVertices * iTarget + vertexIndex` 逐 target 采样并乘权重累加（`morph-animation.chunk:62`）。
- 权重存固定尺寸 UBO：`cc_displacementWeights[15]`（`cc-morph.chunk:6`），因此 GPU 路径最多 60 个 target。
- 路径选择（`morph-rendering.ts:120`）：
  ```ts
  if (preferCpuComputing || subMeshMorph.targets.length > UBOMorphEnum.MAX_MORPH_TARGET_COUNT) {
      // CpuComputing（>60 target 或无 GPU 支持）
  } else {
      // GpuComputing（≤60 target）
  }
  ```
- 位移纹理尺寸由 `createVec4TextureFactory` / `bestSizeToHavePixels` 决定（`morph-rendering.ts:535`）：浮点纹理 1 像素 = 1 vec4；非浮点用 `decode32` 打包，宽度需 ×4。
- 顶点纹理采样依赖 `gl_VertexID`（宏 `CC_MORPH_TARGET_USE_TEXTURE`），WebGL1 不支持，故 WebGL1 走 CPU 或 `CC_MORPH_PRECOMPUTED`。

> ⚠️ 坑：`cc-morph.chunk:3` 声明 `#pragma define-meta CC_MORPH_TARGET_COUNT range([2, 8])`，但运行时该宏被设为 `targets.length`（可达 60，`morph-rendering.ts:159`）。二者不一致，`range([2,8])` 是遗留编辑器元数据提示，**不代表真实上限**。

## 关键源码定位

- `cocos/rendering/define.ts` — `UBOMorphEnum`(L694)、`UBOMorph`(L708)
- `cocos/3d/assets/morph-rendering.ts` — `StdMorphRendering`(L102)、`GpuComputing`(L232)、`CpuComputing`(L347)、`MorphUniforms`(L487)、`createVec4TextureFactory`(L535)
- `cocos/3d/models/morph-model.ts` — 模型入口
- GLSL — `editor/assets/chunks/builtin/uniforms/cc-morph.chunk`、`.../functionalities/morph-animation.chunk`

## 核心宏定义

`CC_USE_MORPH`、`CC_MORPH_TARGET_COUNT`、`CC_MORPH_TARGET_HAS_POSITION/NORMAL/TANGENT`、`CC_MORPH_TARGET_USE_TEXTURE`、`CC_MORPH_PRECOMPUTED`、`CC_DEVICE_SUPPORT_FLOAT_TEXTURE`

## 突破限制方案

1. **>60 target 自动 CPU 回退**：引擎已内置，但每帧 CPU 重算位移并上传纹理，开销随「顶点数 × target 数」线性增长。
2. **扩大 GPU 路径 60 上限**：同步改 `UBOMorphEnum.MAX_MORPH_TARGET_COUNT`、`UBOMorph.LAYOUT` 中 `cc_displacementWeights` 数量、`cc-morph.chunk` 中数组长度三处。
3. **纹理容量突破**：顶点数 × target 数必须 ≤ `maxTextureSize²`；超限时拆分子网格或按 target 分块（多纹理 + 多采样循环）。
4. **性能优化**：预计算位移（`CC_MORPH_PRECOMPUTED`）把「逐 target 循环」移出 VS，仅留一次纹理采样。
