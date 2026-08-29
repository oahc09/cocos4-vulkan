---
id: kb-camera
title: 摄像机 Camera
category: 场景与系统
engine: Cocos Creator 4.x
updated: 2026-08-22
layer: L3
domain: scene
module: camera
breakable: true
tags: [camera, render]
sources: [cocos/render-scene/core/render-scene.ts:157, cocos/render-scene/core/render-window.ts:148, cocos/render-scene/core/render-scene.ts:265, cocos/render-scene/core/render-window.ts:350]
---

# 摄像机 Camera 规格标准

## 概述

引擎对**同时存在的摄像机数量没有硬上限**——摄像机以动态数组管理，数量仅受内存与"每帧全量渲染"的性能约束。

## 极限参数表

| 参数 | 默认 / 上限 | 限制类型 | 说明 |
|---|---|---|---|
| 场景内摄像机数量 | **无硬上限**（动态数组） | 性能（内存） | `render-scene.ts:157` `_cameras: Camera[]` |
| 窗口内摄像机数量 | **无硬上限**（动态数组） | 性能 | `render-window.ts:148` `_cameras: Camera[]` |
| 摄像机优先级 `priority` | 整数排序，决定渲染先后 | 软件 | `render-window.ts:350` sort by priority |
| 视口 / 图层 | 受 `Layers`（[11](./11-layers.md)）与视口尺寸约束 | 架构 | — |

## 底层限制机制

- 摄像机由 `RenderScene`（`render-scene.ts:157` `_cameras: Camera[]`）与 `RenderWindow`（`render-window.ts:148` `_cameras: Camera[]`）以**动态数组**管理，通过 `attachCamera` / `detachCamera` 增删（push / splice），**无 `MAX_CAMERA` 常量**。
- **无硬上限的根因**：摄像机是场景对象，数量上限取决于节点树规模与内存；引擎不预设上限。
- **性能约束才是真天花板**：每个启用的摄像机会独立执行一遍完整渲染流程（culling + draw），因此摄像机数量直接线性放大每帧渲染开销。多摄像机（分屏、画中画、RT 采集）时，draw call 与带宽成倍增长。
- 渲染顺序由 `priority` 排序（`render-window.ts:350`），同级按插入顺序。

## 关键源码定位

- `cocos/render-scene/core/render-scene.ts:157` — `private _cameras: Camera[] = []`
- `cocos/render-scene/core/render-scene.ts:265` — `this._cameras.push(cam)`（attach）
- `cocos/render-scene/core/render-window.ts:148` — `protected _cameras: Camera[] = []`
- `cocos/render-scene/core/render-window.ts:350` — `this._cameras.sort((a,b) => a.priority - b.priority)`

## 核心宏定义

无宏；由场景 / 窗口的摄像机数组与 `priority` 控制。

## 突破限制方案

1. **控制激活摄像机数**：非必要不常驻多摄像机；临时采集（如截图）用完即 `detach`。
2. **用视口 / 图层替代多机**：单摄像机通过 `viewport` 分屏或 `Layers` 过滤，减少独立渲染流。
3. **降低副摄像机开销**：副摄像机（画中画 / RT）用小分辨率、关阴影 / 后处理。
4. **对象池复用**：频繁切换的摄像机用对象池管理，避免反复增删数组带来的 GC。
