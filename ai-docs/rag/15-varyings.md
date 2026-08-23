---
id: kb-varyings
title: Shader 插值器 Varyings 规格标准
category: 渲染 / Shader
engine: Cocos Creator 4.x
updated: 2026-08-22
sources: [cocos/gfx/gl-constants.ts:88, cocos/gfx/gl-constants.ts:234, cocos/gfx/gl-constants.ts:401, cocos/gfx/base/define.ts:747]
---

# Shader 插值器 Varyings 规格标准

## 概述

顶点着色器输出到片元着色器的 **varying（插值器）数量受图形 API 上限约束**。引擎未在 `DeviceCaps` 中暴露该值，属"隐式上限"，需在 shader 编写时自查。

## 极限参数表

| 参数 | 典型值 / 保证最小值 | 限制类型 | 说明 |
|---|---|---|---|
| `MAX_VARYING_VECTORS`（GLES3 / WebGL2） | GLES3 保证最小 **15** 向量；桌面 GL 常 32 | 硬件 | GL 常量 `0x8DFC`（`gl-constants.ts:401`） |
| `MAX_VARYING_COMPONENTS` | GLES3 保证最小 **60** 分量 | 硬件 | GL 常量 `0x8B4B` |
| `MAX_VERTEX_OUTPUT_COMPONENTS` | GLES3 保证最小 **64** 分量 | 硬件 | GL 常量 `0x9122`（`gl-constants.ts:234`） |
| 引擎是否纳入 `DeviceCaps` | **否** | — | `define.ts:747` 无对应字段 |

## 底层限制机制

- varying 是 VS → FS 之间光栅化插值传递的变量，其总数受 GPU 的 varying 向量 / 分量上限约束。GLSL 编译 / 链接阶段若超出会报 `linkProgram` 失败。
- **引擎未查询该值**：`DeviceCaps`（`define.ts:747`）只含 uniform / 纹理 / 属性等字段，**没有 varying 上限字段**。因此它在引擎内是"隐式"的——既不会在运行时自动降级，也不会在编译前校验，超量只会在 shader 链接时报 GL 错误。
- 量化口径有向量（`MAX_VARYING_VECTORS`，每向量 4 分量）与分量（`MAX_VARYING_COMPONENTS`）两套，实际可用量以更紧的那个为准。

## 关键源码定位

- `cocos/gfx/gl-constants.ts:88` — `MAX_VARYING_COMPONENTS = 0x8B4B`
- `cocos/gfx/gl-constants.ts:234` — `MAX_VERTEX_OUTPUT_COMPONENTS = 0x9122`
- `cocos/gfx/gl-constants.ts:401` — `MAX_VARYING_VECTORS = 0x8DFC`
- `cocos/gfx/base/define.ts:747` — `DeviceCaps`（确认无 varying 字段）

## 核心宏定义

无引擎宏；varying 由 GLSL `varying` / `out` / `in` 关键字与 GL 常量约束。

## 突破限制方案

1. **压缩 varying**：合并语义相近的 varying（如把多个 float 打包进一个 vec4），减少向量占用。
2. **改用纹理传递**：大数据（如逐顶点置换数据）改由顶点纹理采样（依赖 `maxVertexTextureUnits`），绕开 varying 上限。
3. **拆分 shader / 多 pass**：将输出分散到多个 pass，单 pass 的 varying 负载降低。
4. **运行时自查**：对目标机型通过 `gl.getParameter(gl.MAX_VARYING_VECTORS)` 实测，建立机型兼容表。
