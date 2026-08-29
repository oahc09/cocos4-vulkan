---
id: kb-gpu-instancing
title: GPU Instancing 实例化渲染
category: 渲染与批处理
engine: Cocos Creator 4.x
updated: 2026-08-29
layer: L2
domain: rendering
module: instancing
breakable: true
tags: [instancing, batch, MAX_CAPACITY, drawcall]
sources: [cocos/rendering/instanced-buffer.ts:57, cocos/rendering/instanced-buffer.ts:58, cocos/rendering/instanced-buffer.ts:111, cocos/rendering/instanced-buffer.ts:136, cocos/gfx/base/define.ts:109, cocos/gfx/base/input-assembler.ts:133, webgl-device.ts:221, webgpu-device.ts:561, particle-batch-model.ts:97, cocos/rendering/instanced-buffer.ts:160]
---

# GPU Instancing 实例化渲染

## 概述
GPU Instancing 把「同网格 + 同材质」的多个对象合并为一次 `drawElementsInstanced` 调用，靠 `instanceCount` 与 per-instance 顶点属性区分实例。这是引擎降低 DrawCall 的核心手段之一，其**单批实例数存在明确硬编码上限 1024**。

## 极限参数表

| 参数 | 默认/上限 | 限制类型 | 说明 |
|---|---|---|---|
| 单 `InstancedBuffer` 最大实例数 | **1024** | 软件（硬编码常量） | 超出后该 buffer 不再接收新实例 |
| 初始实例容量 | 32 | 软件 | 首次分配大小，按需翻倍 |
| 容量扩容策略 | `capacity <<= 1`（32→64→…→1024） | 软件 | 达到 1024 后停止 |
| 设备实例化能力 | `Feature.INSTANCED_ARRAYS` | 硬件 | WebGL1 需 `ANGLE_instanced_arrays` 扩展 |
| per-instance 属性槽位 | 受 `maxVertexAttributes` 约束 | 硬件 | 一个 `mat4` 占 4 个属性槽 |
| 单批超限行为 | 自动拆分到新 `InstancedBuffer` | 软件 | 不丢渲染，但增加 draw call |

## 底层限制机制
- **1024 的来源**：`instanced-buffer.ts:111` 在合并实例时判断 `instance.count >= MAX_CAPACITY` 即 `continue` 跳过该 buffer，改由后续逻辑新建 buffer 承载。这是**纯软件常量**，与硬件无关，目的是控制单批顶点缓冲大小与上传开销。
- **翻倍扩容**：`instanced-buffer.ts:136-138` 当 `instance.count >= instance.capacity` 时执行 `capacity <<= 1` 并重建 `Buffer` 与 `Uint8Array`，故实际内存占用按 2 的幂增长，上限截断在 1024。
- **属性槽位竞争**：per-instance 属性被追加到源 `InputAssembler` 的属性表之后（`instanced-buffer.ts:160-166`）。若实例矩阵以 `mat4` 传入，单个实例属性就消耗 4 个槽位；WebGL2 保证 ≥16 个顶点属性槽，因此复杂材质下槽位可能先于 1024 成为瓶颈。
- **能力检测**：WebGL1 后端需 `ANGLE_instanced_arrays`（`webgl-device.ts:221`），WebGPU 原生支持（`webgpu-device.ts:561`）。若不支持，粒子等模块会检测 `Feature.INSTANCED_ARRAYS` 后回退（`particle-batch-model.ts:97`）。

## 关键源码定位

| 位置 | 内容 |
|---|---|
| `cocos/rendering/instanced-buffer.ts:57` | `const INITIAL_CAPACITY = 32;` |
| `cocos/rendering/instanced-buffer.ts:58` | `const MAX_CAPACITY = 1024;` |
| `cocos/rendering/instanced-buffer.ts:111` | `if (instance.ia.indexBuffer?.objectID !== ... \|\| instance.count >= MAX_CAPACITY) continue;` |
| `cocos/rendering/instanced-buffer.ts:136-138` | 容量翻倍重建缓冲 |
| `cocos/gfx/base/define.ts:109` | `INSTANCED_ARRAYS` Feature 枚举 |
| `cocos/gfx/base/input-assembler.ts:133` | `set instanceCount` |
| `cocos/rendering/instanced-buffer.ts:160-166` | per-instance 属性追加逻辑 |

## 核心宏定义与常量
- `MAX_CAPACITY = 1024` —— 单批实例数硬上限（可改）
- `INITIAL_CAPACITY = 32` —— 初始容量
- `Feature.INSTANCED_ARRAYS` —— 设备实例化能力位

## 突破限制方案
1. **接受自动分拆（推荐）**：超过 1024 时引擎自动新建 buffer，功能无损；只需关注 DrawCall 数是否达预期。用 RenderDoc / 引擎统计面板确认 `_numInstances`。
2. **调整 `MAX_CAPACITY`**：属软件常量，修改 `instanced-buffer.ts:58` 后重编译引擎即可。代价是单批 buffer 内存按 `stride × capacity` 增长，且上传带宽压力上升；不建议超过 4096。
3. **压缩 per-instance 数据**：若受顶点属性槽位限制而非 1024 限制，优先压缩实例数据——用 `vec4` 传输（位置+缩放+四元数旋转）替代 `mat4`，可省 3 个属性槽。
4. **改用 Compute/Indirect Draw**：超大规模（万级）同构对象建议自定义管线走 indirect draw，绕过 `InstancedBuffer` 的 1024 约束。

## 关联条目
- [01 DeviceCaps](./01-device-caps.md) —— `maxVertexAttributes` 是属性槽位的硬件基准
- [08 粒子系统](./08-particle.md) —— 粒子批模型依赖 `INSTANCED_ARRAYS`
- [06 顶点与索引](./06-vertex-index.md) —— 索引格式与顶点寻址上限
