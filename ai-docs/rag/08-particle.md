---
id: kb-particle
title: 粒子系统 Particle
category: 3D 渲染模块
engine: Cocos Creator 4.x
updated: 2026-08-22
layer: L3
domain: rendering
module: particle
breakable: true
tags: [particle, capacity, emitter]
sources: [cocos/particle/particle-system.ts:78, cocos/particle/particle-system.ts:803, cocos/particle/renderer/particle-system-renderer-cpu.ts, cocos/particle/renderer/particle-system-renderer-gpu.ts, cocos/particle/particle-system.ts:84]
---

# 粒子系统 Particle 规格标准

## 概述

粒子系统的 `capacity`（最大粒子容量）是**可配置软上限**，默认 100，引擎**无硬编码硬上限**，实际受粒子缓冲显存 / 内存约束。

## 极限参数表

| 参数 | 默认 / 上限 | 限制类型 | 说明 |
|---|---|---|---|
| `capacity`（最大粒子数） | 默认 **100** | 软件（配置项） | `particle-system.ts:803` 初值 |
| 实际可容纳粒子数 | 无引擎硬上限，受 buffer 容量约束 | 性能 | 调用 `model.setCapacity(val)` |
| 渲染模式 | CPU / GPU 两路 | 软件 | `particle-system-renderer-cpu/gpu` |

## 底层限制机制

- `capacity` 是 `ParticleSystem` 的公开属性（`particle-system.ts:84`），setter 中 `Math.floor(val > 0 ? val : 0)` 后调用 `this.processor.model.setCapacity(this._capacity)`（L91）向渲染模型下发容量。
- **无硬上限根因**：粒子数据存于 CPU / GPU 粒子缓冲（ring buffer / VBO），容量由 buffer 分配大小决定，理论上只受显存 / 内存与单帧更新算力约束。引擎既未定义 `MAX_PARTICLE` 类常量，也未在 setter 中做上限校验。
- GPU 粒子路径（`particle-system-renderer-gpu.ts`）容量还受粒子纹理尺寸（隐含受 `maxTextureSize`）与每粒子属性 stride 影响。

## 关键源码定位

- `cocos/particle/particle-system.ts:78` — `capacity` 属性注释（最大粒子容量）
- `cocos/particle/particle-system.ts:84-91` — `get/set capacity` 与 `setCapacity` 下发
- `cocos/particle/particle-system.ts:803` — `_capacity = 100` 初值
- `cocos/particle/renderer/particle-system-renderer-cpu.ts` / `...-gpu.ts` — 两路渲染器
- `cocos/particle/models/particle-batch-model.ts` — 批量渲染模型

## 核心宏定义

无统一上限宏；容量完全由组件属性驱动。

## 突破限制方案

1. **直接调大 `capacity`**：在属性面板或脚本设更大值即可，引擎自动扩容粒子缓冲。
2. **GPU 粒子优先**：大数据量用 GPU 粒子（`particle-system-renderer-gpu`）卸载 CPU 计算。
3. **内存 / 带宽监控**：超大 `capacity` 受粒子纹理尺寸（`maxTextureSize`）与每帧上传带宽约束，需实测机型上限。
4. **分系统拆分**：单系统容量过高时，拆分为多个粒子系统并联，规避单缓冲过大。
