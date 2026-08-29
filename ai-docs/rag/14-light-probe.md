---
id: kb-light-probe
title: 光照探针 / GI
category: 3D 渲染模块
engine: Cocos Creator 4.x
updated: 2026-08-22
layer: L2
domain: rendering
module: gi
breakable: true
tags: [gi, lightprobe, giSamples]
sources: [cocos/native-binding/decorators.ts:526, cocos/native-binding/decorators.ts:530, cocos/native-binding/decorators.ts:546, cocos/native-binding/decorators.ts:547, decorators.ts:521, decorators.ts:535, decorators.ts:904, decorators.ts:565, cocos/native-binding/decorators.ts:503, cocos/native-binding/decorators.ts:557]
---

# 光照探针 / GI 规格标准

## 概述

光照探针（Light Probe / GI）的核心可配上限为：GI 采样数 `giSamples` 范围 **[64, 65535]**（默认 1024）、光线反弹数 `bounces` 范围 **[1, 4]**（默认 2）；探针**数量本身无硬上限**（数组存储，受内存约束）。

## 极限参数表

| 参数 | 默认 / 上限 | 限制类型 | 说明 |
|---|---|---|---|
| GI 采样数 `giSamples` | 范围 **[64, 65535]**，默认 **1024** | 软件（range 约束） | `decorators.ts:526,546` |
| 光线反弹数 `bounces` | 范围 **[1, 4]**，默认 **2** | 软件（range 约束） | `decorators.ts:530,547` |
| 探针数量（Probe 数组） | 无硬上限，数组动态增长 | 性能（内存） | `LightProbesData._probes` |
| GI 缩放 `giScale` | 范围 [0, 100] | 软件 | `decorators.ts:521` |
| 减少振铃 `reduceRinging` | 范围 [0, 0.05] | 软件 | `decorators.ts:535` |

## 底层限制机制

- `LightProbeInfo` 挂在 `SceneGlobals` 上（`decorators.ts:904`），其 `giSamples` / `bounces` 通过编辑器 `$.range(...)` 限定可输入区间——这是**编辑器元数据约束**，超出范围无法在 Inspector 设置（但运行时代码层面无强制硬上限）。
- `giSamples` 决定 GI 烘焙 / 实时计算时的射线采样密度，值越大精度越高、耗时越长；`bounces` 决定间接光反弹次数（上限 4 是引擎对性能 / 收敛质量的权衡）。
- 探针数据本身存于 `LightProbesData._probes`（顶点数组）与 `_tetrahedrons`（四面体数组，`decorators.ts:565,567`），**数量无常量上限**，仅受内存与烘焙/更新算力约束。

## 关键源码定位

- `cocos/native-binding/decorators.ts:503-555` — `patch_cc_LightProbeInfo`（giScale/giSamples/bounces/reduceRinging 的 range 与默认值）
- `cocos/native-binding/decorators.ts:526` — `giSamples` range [64, 65535]
- `cocos/native-binding/decorators.ts:530` — `bounces` range [1, 4]
- `cocos/native-binding/decorators.ts:546,547` — 默认值 giSamples=1024、bounces=2
- `cocos/native-binding/decorators.ts:557-570` — `LightProbesData`（`_probes` / `_tetrahedrons` 数组）

## 核心宏定义

无统一宏；由 `LightProbeInfo` 属性与 range 装饰器控制。

## 突破限制方案

1. **提高 GI 质量**：在编辑器将 `giSamples` 调高（≤65535）、`bounces` 调至 4；代价是烘焙/实时更新耗时与内存上升。
2. **探针密度**：按需布置探针，避免在无需 GI 的区域堆探针——数量无上限但每个都占内存与更新开销。
3. **运行时扩展**：代码层面可绕过编辑器 range 直接设更大 `giSamples`（需确认底层算法支持），但非推荐路径。
4. **混合方案**：静态场景用探针烘焙，动态物体用 `maxReceived` 阴影（见 [04](./04-light.md) / [12](./12-shadow.md)）互补。
