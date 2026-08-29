---
id: kb-texture-material-pass
title: 纹理 / 材质 / Pass / RT
category: 渲染与着色器
engine: Cocos Creator 4.x
updated: 2026-08-22
layer: L2
domain: rendering
module: material
breakable: true
tags: [texture, material, pass, MAX_PASS_COUNT]
sources: [cocos/render-scene/scene/submodel.ts:37, cocos/gfx/base/define.ts:757, native/cocos/renderer/gfx-vulkan/VKDevice.cpp:321, cocos/gfx/webgl2/webgl2-device.ts:191, cocos/asset/assets/material.ts:175, device.ts:420, cocos/gfx/webgl2/webgl2-commands.ts:931]
---

# 纹理 / 材质 / Pass / RT 规格标准

## 概述

纹理尺寸、MRT 数等由 `DeviceCaps` 决定；**单 SubModel 的 Pass 数量有硬上限 `MAX_PASS_COUNT = 8`**（引擎常量，非硬件）。

## 极限参数表

| 参数 | 默认 / 上限 | 限制类型 | 说明 |
|---|---|---|---|
| 2D 纹理最大边长 `maxTextureSize` | WebGL2 `MAX_TEXTURE_SIZE`；Vulkan `maxImageDimension2D` | 硬件 | 触发 `errorID(9100)` 超限报错 |
| CubeMap 最大边长 `maxCubeMapTextureSize` | WebGL2 `MAX_CUBE_MAP_TEXTURE_SIZE` | 硬件 | — |
| 数组纹理层数 `maxArrayTextureLayers` | WebGL2 `MAX_ARRAY_TEXTURE_LAYERS`；Vulkan `maxImageArrayLayers` | 硬件 | — |
| 3D 纹理边长 `max3DTextureSize` | WebGL2 `MAX_3D_TEXTURE_SIZE`；Vulkan `maxImageDimension3D` | 硬件 | — |
| 片元采样纹理单元数 `maxTextureUnits` | WebGL2 `MAX_TEXTURE_IMAGE_UNITS` | 硬件 | 单 shader 可采样上限 |
| 颜色渲染目标（MRT）数 `maxColorRenderTargets` | Vulkan `maxColorAttachments`（桌面常 8） | 硬件 | — |
| **单 SubModel Pass 数 `MAX_PASS_COUNT`** | **8** | 软件（常量） | `submodel.ts:37`，超报 `errorID(12004)` |
| 材质 Pass 数 | 无独立硬常量（受 SubModel 8 限制） | 软件 | `material._passes` 动态数组 |

## 底层限制机制

- **纹理尺寸硬约束**：创建 / 上传纹理时若边长超过 `maxTextureSize`，后端抛 `errorID(9100)`（`webgl2-commands.ts:931` 等多处）。Mipmap 生成还要求 `maxTextureSize >= 2`（`device.ts:420`）。
- **MRT 数**：单帧可同时输出的颜色附件数 = `maxColorAttachments`（Vulkan，桌面 GPU 通常 8）。Deferred 管线依赖多 RT。
- **Pass 数硬上限**：`submodel.ts:37` 定义 `const MAX_PASS_COUNT = 8`，`bindMaterial` 时若 `passLengh > MAX_PASS_COUNT` 报 `errorID(12004, MAX_PASS_COUNT)`。这是**引擎架构约束**（Descriptor Set / 管线状态缓存规模），与硬件无关，但可直接改常量放宽。

## 关键源码定位

- `cocos/render-scene/scene/submodel.ts:37` — `MAX_PASS_COUNT = 8` 及校验（L81）
- `cocos/gfx/base/define.ts:757` — `DeviceCaps.maxColorRenderTargets`
- `native/cocos/renderer/gfx-vulkan/VKDevice.cpp:321` — Vulkan `maxColorAttachments` 填充
- `cocos/gfx/webgl2/webgl2-device.ts:191` — `maxTextureSize` 填充
- `cocos/gfx/webgl2/webgl2-commands.ts:931` — 纹理超限 `errorID(9100)`
- `cocos/asset/assets/material.ts:175` — `material.passes` 访问

## 核心宏定义

无统一宏；MRT 数受管线配置与硬件 `maxColorAttachments` 约束。

## 突破限制方案

1. **放宽 Pass 数**：直接调大 `submodel.ts:37` 的 `MAX_PASS_COUNT`（需同步评估管线状态与 Descriptor Set 规模）。
2. **超大纹理**：拆分为纹理数组（`maxArrayTextureLayers`）或图集（atlas），避免单图超 `maxTextureSize`。
3. **多 RT 极限**：确认目标 GPU 的 `maxColorAttachments`；超量时序列化多个 pass 而非单帧多 RT。
4. **采样单元紧张**：减少单 shader 采样纹理数，或用纹理数组 / bindless（若后端支持）降低 `maxTextureUnits` 压力。
