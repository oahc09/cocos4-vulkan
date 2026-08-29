---
id: kb-uniform-ubo
title: Uniform 与 UBO 容量
category: 渲染与着色器
engine: Cocos Creator 4.x
updated: 2026-08-22
layer: L1
domain: rendering
module: ubo
breakable: false
tags: [uniform, ubo, maxUniformBlockSize]
sources: [cocos/gfx/base/define.ts:747, native/cocos/renderer/gfx-vulkan/VKDevice.cpp:313, cocos/gfx/webgl2/webgl2-device.ts:173, cocos/rendering/define.ts:171, webgpu-device.ts:547, native/cocos/renderer/gfx-vulkan/VKDevice.cpp:311]
---

# Uniform / UBO 容量规格标准

## 概述

引擎所有动态 uniform 数据的硬天花板来自 `DeviceCaps` 的 uniform 相关字段；当单块 UBO 超过 `maxUniformBlockSize`（16KB~64KB）时，唯一出路是改用纹理或 SSBO。

## 极限参数表

| 参数 | 典型值 / 保证最小值 | 限制类型 |
|---|---|---|
| `maxVertexUniformVectors` | Vulkan `maxUniformBufferRange/16`；GLES3 ≥256；桌面 GL 4096 | 硬件 |
| `maxFragmentUniformVectors` | GLES3 ≥224；桌面 GL 4096 | 硬件 |
| `maxUniformBlockSize` | Vulkan `maxUniformBufferRange`（**保证 ≥16384B/16KB**，桌面常 65536B/64KB） | 硬件 |
| `maxUniformBufferBindings` | WebGL1 硬编码 16；WebGPU 12 | 硬件 |
| `maxVertexTextureUnits` | GLES3 ≥16；桌面 GL 16~32 | 硬件 |
| `maxVertexAttributes` | GLES3 ≥16；Vulkan = `maxVertexInputAttributes` | 硬件 |
| `uboOffsetAlignment` | Vulkan `minUniformBufferOffsetAlignment`（**常见 256B**） | 硬件 |

## 底层限制机制

- 能力表来源：
  - Vulkan（`VKDevice.cpp:311`）：`maxVertexUniformVectors = limits.maxUniformBufferRange / 16`、`maxUniformBlockSize = limits.maxUniformBufferRange`、`uboOffsetAlignment = limits.minUniformBufferOffsetAlignment`。
  - WebGL2（`webgl2-device.ts:173`）：查询 GL 参数；iOS 微信 clamp `maxVertexUniformVectors ≤ 256`、Safari clamp ≤ 512（规避 iOS Metal 后端 bug）。
  - WebGPU（`webgpu-device.ts:547`）：`maxUniformBufferBindings = 12` 或 `limits.maxUniformBufferBindingSize`。
- 3 组 Descriptor Set 的 UBO / 采样器数量由 `bindingMappingInfo` 固定（`define.ts:171`）：GLOBAL 4 UBO + 4 sampler、LOCAL 7 UBO + N sampler。
- **关键连锁**：顶点阶段 uniform 向量 = 全局 UBO（`UBOGlobal`+`UBOCamera`+`UBOShadow`+`UBOCSM`）+ 本地 UBO（`UBOLocal`+`UBOSkinning`）共享，这是蒙皮关节数被压缩的根源。

## 引擎内置 UBO 尺寸清单

| UBO | 成员构成 | 尺寸 |
|---|---|---|
| `UBOGlobal` | 5 × vec4 | 80 B |
| `UBOCamera` | 6 × mat4 + 13 × vec4 | 592 B |
| `UBOShadow` | 2 × mat4 + 8 × vec4 | 256 B |
| `UBOCSM` | 4 级 + 1 vec4 | 656 B |
| `UBOLocal` | 2 × mat4 + 6 × vec4 | 224 B |
| `UBOSkinning` | 关节数 × 3 vec4（30 关节 1440B，256 关节 12288B） | 关节数 × 48 B |
| `UBOMorph` | 15 vec4 权重 + 1 vec4 信息 | 256 B |
| `UBOForwardLight` | 5 × vec4 / 光 | 80 B / 光 |

（以上由 `define.ts` 各 `UBO*Enum.SIZE = COUNT * 4` 推导，`COUNT` 单位为 float。）

## 关键源码定位

- `cocos/gfx/base/define.ts:747` — `DeviceCaps` 全字段
- `native/cocos/renderer/gfx-vulkan/VKDevice.cpp:311` — Vulkan 填充
- `cocos/gfx/webgl2/webgl2-device.ts:173` — WebGL2 填充 + iOS clamp
- `cocos/rendering/define.ts:171` — `bindingMappingInfo`（Descriptor Set 布局）

## 核心宏定义

无统一宏；UBO 成员布局由各 `UBO*Enum` 常量决定。

## 突破限制方案

1. **对齐优化**：`uboOffsetAlignment` 常 256B，动态 UBO（如 light buffer）需按 stride 对齐，避免无效填充。
2. **大数据移出 uniform**：关节、morph、cluster 光等超大数据改用**纹理**或**存储缓冲（SSBO，`UniformStorageBuffer` / `maxShaderStorageBlockSize`，通常 128MB 级）**，绕开 `maxUniformBlockSize`。
3. **精简全局 UBO**：减少全局 UBO 成员，为蒙皮 UBO 腾出顶点 uniform 向量预算。
4. **跨平台兜底**：针对 iOS / 微信浏览器遵循引擎已有 256/512 顶点 uniform clamp，必要时主动降级到关节纹理。
