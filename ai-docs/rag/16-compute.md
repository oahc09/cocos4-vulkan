---
id: kb-compute
title: 计算着色器 Compute 规格标准
category: 渲染 / 计算
engine: Cocos Creator 4.x
updated: 2026-08-22
sources: [cocos/gfx/base/define.ts:767, native/cocos/renderer/gfx-vulkan/VKDevice.cpp:311, cocos/gfx/webgpu/webgpu-device.ts:547]
---

# 计算着色器 Compute 规格标准

## 概述

计算着色器的关键上限（共享内存、工作组调用数、工作组维度）均由 `DeviceCaps` 的 compute 字段承载，数据来自 Vulkan / WebGPU 的 `VkPhysicalDeviceLimits` / `GPUAdapter.limits`。

## 极限参数表

| 字段 | 含义 | 来源 | 限制类型 |
|---|---|---|---|
| `maxComputeSharedMemorySize` | 单工作组共享内存上限 | Vulkan `limits.maxComputeSharedMemorySize` | 硬件 |
| `maxComputeWorkGroupInvocations` | 单工作组最大调用数 | Vulkan `limits.maxComputeWorkGroupInvocations` | 硬件 |
| `maxComputeWorkGroupSize` | 工作组维度上限（X/Y/Z） | Vulkan `limits.maxComputeWorkGroupSize` | 硬件 |
| `maxComputeWorkGroupCount` | 可调度工作组数上限（X/Y/Z） | Vulkan `limits.maxComputeWorkGroupCount` | 硬件 |

## 底层限制机制

- 这些字段是 `DeviceCaps` 的一部分（`define.ts:767-770`），由后端在设备初始化时填充：Vulkan 后端直接搬运 `VkPhysicalDeviceLimits`（见 `VKDevice.cpp` 同段的 uniform / 纹理字段）；WebGPU 后端从 `GPUAdapter.limits` 读取（`webgpu-device.ts:547`）。
- **为何是硬件约束**：工作组大小、共享内存（shared/local memory）容量是 GPU 微架构的固定规格（如桌面 GPU 共享内存常 32KB~48KB，工作组最大调用数常 256/1024），引擎无法突破。
- 计算管线（`PassType.COMPUTE`，`define.ts:709`）的 dispatch 必须在这些上限内，否则提交失败或结果错误。

## 关键源码定位

- `cocos/gfx/base/define.ts:767-770` — `DeviceCaps` 的 compute 四个字段
- `native/cocos/renderer/gfx-vulkan/VKDevice.cpp:311` 区段 — Vulkan 填充（与 uniform/纹理同段）
- `cocos/gfx/webgpu/webgpu-device.ts:547` — WebGPU 填充
- `cocos/gfx/base/define.ts:709` — `PassType.COMPUTE`

## 核心宏定义

无宏；由 `device.capabilities.maxCompute*` 在运行时读取。

## 突破限制方案

1. **shared memory 优化**：将 `maxComputeSharedMemorySize` 视为预算上限，避免单工作组申请超额共享内存。
2. **工作组尺寸**：dispatch 时按 `maxComputeWorkGroupSize/Count` 拆分任务，避免单维越界。
3. **多 pass 计算**：单工作组调用数（`maxComputeWorkGroupInvocations`）不足时，拆分为多 dispatch 或分块（tiling）处理大数组。
4. **能力查询分支**：运行时读 `capabilities` 决定计算任务的分块粒度，保证跨 GPU 兼容。
