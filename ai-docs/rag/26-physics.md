---
id: kb-physics
title: 物理系统 Physics（3D/2D）
category: 场景与系统
engine: Cocos Creator 4.x
updated: 2026-08-29
layer: L3
domain: physics
module: physics
breakable: true
tags: [physics, fixedTimeStep, maxSubSteps, collisionMatrix]
sources: [cocos/physics/framework/physics-system.ts:329, cocos/physics/framework/physics-system.ts:331, cocos/physics/framework/physics-system.ts:334, cocos/physics/framework/physics-system.ts:315, cocos/physics/framework/physics-system.ts:366, cocos/physics/framework/collision-matrix.ts:27, cocos/physics/spec/i-physics-world.ts:43, collision-matrix.ts:31, cannon-world.ts:181, cocos/physics/framework/physics-system.ts:392]
---

# 物理系统 Physics（3D/2D）

## 概述
Cocos 4.x 的物理层是**可插拔后端**架构：3D 支持 `cannon` / `bullet` / `physx`，2D 走 `box2d`。引擎层**不设刚体数/碰撞体数的硬编码上限**，真正的"规格"是三个可调时间步参数与 32×32 碰撞矩阵。物理瓶颈几乎总是 CPU 与帧率耦合问题，而非数量上限。

## 极限参数表

| 参数 | 默认值 | 限制类型 | 说明 |
|---|---|---|---|
| `fixedTimeStep` 固定步长 | `1.0 / 60.0` 秒 | 软件（可配） | 物理推进的固定时间片 |
| `maxSubSteps` 最大子步数 | **1** | 软件（可配） | 单帧最多推进几个固定步 |
| `sleepThreshold` 休眠阈值 | `0.1` | 软件（可配） | 速度低于此值持续一段时间后休眠 |
| 碰撞分组维度 | **32 × 32** | 软件（结构设计） | `CollisionMatrix` 按 32 位遍历 |
| 刚体 / 碰撞体数量 | 无引擎常量上限 | 性能约束 | 取决于后端与 CPU |
| 物理后端 | cannon / bullet / physx | 构建期选择 | 三选一，不可运行时切换 |

## 底层限制机制
- **固定时间步累加器**：`physics-system.ts:366-372` 用 `_accumulator` 累积真实帧间隔，只要 `_subStepCount < _maxSubSteps` 且累积量 ≥ `fixedTimeStep` 就推进一次物理。**默认 `maxSubSteps = 1` 意味着单帧最多推进 1 步**——当渲染帧率低于 60 FPS 时，物理会「变慢」（时间膨胀）而非追赶，这是最容易被误判为"物理卡顿"的根因。
- **碰撞矩阵是 32 位分组**：`collision-matrix.ts:31,51` 循环 32 次构造矩阵，配合物理分组（32 组）做位掩码过滤。`PhysicsSystem.instance.collisionMatrix` 默认构造为 `new CollisionMatrix(1)`（`physics-system.ts:315`），实际运行时由项目配置注入。
- **休眠优化**：`sleepThreshold = 0.1`，刚体速度低于阈值一段时间后进入休眠，从求解器活跃集中移除。这是控制大量刚体 CPU 开销的主要手段。
- **后端差异**：`cannon` 为纯 JS 实现（`cannon-world.ts:181` 直接透传 `step(deltaTime, timeSinceLastCalled, maxSubStep)`），`physx` / `bullet` 走 WASM/原生，承载规模差异显著。

## 关键源码定位

| 位置 | 内容 |
|---|---|
| `cocos/physics/framework/physics-system.ts:329` | `private _maxSubSteps = 1;` |
| `cocos/physics/framework/physics-system.ts:331` | `private _fixedTimeStep = 1.0 / 60.0;` |
| `cocos/physics/framework/physics-system.ts:334` | `private _sleepThreshold = 0.1;` |
| `cocos/physics/framework/physics-system.ts:315` | `collisionMatrix: ICollisionMatrix = new CollisionMatrix(1)` |
| `cocos/physics/framework/physics-system.ts:366-372` | 固定步累加器主循环 |
| `cocos/physics/framework/physics-system.ts:392-395` | 从 `SettingsCategory.PHYSICS` 覆盖默认值 |
| `cocos/physics/framework/collision-matrix.ts:27-51` | 32 组碰撞矩阵构造 |
| `cocos/physics/spec/i-physics-world.ts:43` | `step(fixedTimeStep, timeSinceLastCalled?, maxSubSteps?)` 接口 |

## 核心宏定义与常量
- `maxSubSteps`（默认 1）—— 单帧物理最大子步
- `fixedTimeStep`（默认 1/60）—— 物理固定步长
- `sleepThreshold`（默认 0.1）—— 休眠速度阈值
- `CollisionMatrix` 32 维 —— 碰撞分组位矩阵

## 突破限制方案
1. **提高 `maxSubSteps`（首选）**：帧率不稳场景把 1 提到 3~5，避免时间膨胀；代价是峰值 CPU 上升，且低帧率下可能出现"物理追赶"抖动。建议配合上限保护。
2. **调整 `fixedTimeStep`**：高速小物体穿透时调小（如 1/120）提高精度，代价是同时间推进次数翻倍；反之大场景可放大到 1/30 省 CPU。
3. **用碰撞矩阵收敛碰撞对**：32 组位掩码精确配置，把互不交互的对象（如 UI 与地面装饰）分组隔离，收益通常大于调参数。
4. **启用/放宽休眠**：调高 `sleepThreshold` 让更多静态物体快速休眠；对持续运动的物体显式 `allowSleep = false` 避免误休眠。
5. **换后端**：对象规模上千时 `cannon`（纯 JS）会成为瓶颈，构建期切到 `physx` / `bullet`（原生/WASM）。

## 关联条目
- [19 摄像机](./19-camera.md) —— 同属"无硬上限、性能约束"类规格
- [16 计算着色器](./16-compute.md) —— 大规模粒子/物体的 GPU 替代路径
- [01 DeviceCaps](./01-device-caps.md) —— 硬件能力基准（物理为纯 CPU，不直接依赖）
