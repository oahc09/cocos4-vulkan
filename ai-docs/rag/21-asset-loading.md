---
id: kb-asset-loading
title: 资源加载并发
category: 资源与管线
engine: Cocos Creator 4.x
updated: 2026-08-22
layer: L2
domain: asset
module: asset-manager
breakable: true
tags: [asset, loading, maxConcurrency]
sources: [cocos/asset/asset-manager/downloader.ts:161, cocos/asset/asset-manager/downloader.ts:171, cocos/asset/asset-manager/asset-manager.ts:511, cocos/asset/asset-manager/shared.ts:71, downloader.ts:503, cocos/asset/asset-manager/downloader.ts:424]
---

# 资源加载并发规格标准

## 概述

`AssetManager` 的下载器对并发加载有两组可配上限：**同时下载数 `maxConcurrency = 15`** 与 **每帧最大请求数 `maxRequestsPerFrame = 15`**；不同管线预设下 `maxRequestsPerFrame` 默认值不同（2 / 20 / 20）。

## 极限参数表

| 参数 | 默认 / 上限 | 限制类型 | 说明 |
|---|---|---|---|
| 同时下载数 `maxConcurrency` | **15**（可配） | 软件（配置项） | `downloader.ts:161` |
| 每帧最大请求数 `maxRequestsPerFrame` | **15**（可配，下载器默认） | 软件（配置项） | `downloader.ts:171` |
| 管线预设 `maxRequestsPerFrame` | 2 / 20 / 20（不同管线） | 软件（预设） | `shared.ts:71,77,83` |
| 最大重试次数 `maxRetryCount` | 可配 | 软件 | `asset-manager.ts:511` |

## 底层限制机制

- 下载队列由 `Downloader` 管理（`downloader.ts`）。入队时按 `while (_queue.length > 0 && _totalNum < maxConcurrency && _totalNumThisPeriod < maxRequestsPerFrame)` 节流（`downloader.ts:503`）：**两个闸门共同约束吞吐**——`maxConcurrency` 限制"同时在途"的请求数，`maxRequestsPerFrame` 限制"单帧新发起"的请求数，避免一帧内暴增请求造成卡顿。
- 两组值均可经 `options` 在 `load` / `preload` 时覆盖（`downloader.ts:424`）：`maxConcurrency` 与 `maxRequestsPerFrame` 取 `options` 传入值或类默认值。
- `shared.ts` 中不同管线（如普通加载 / 远程包 / 子包）预设了不同的 `maxRequestsPerFrame`（2 / 20 / 20），体现"首屏预载更保守、普通加载更激进"的策略差异。
- **无全局硬性资源数上限**：可加载资源总数仅受内存与磁盘缓存约束；并发参数只影响加载速度节奏。

## 关键源码定位

- `cocos/asset/asset-manager/downloader.ts:161` — `public maxConcurrency = 15;`
- `cocos/asset/asset-manager/downloader.ts:171` — `public maxRequestsPerFrame = 15;`
- `cocos/asset/asset-manager/downloader.ts:424,503` — 读 options 与队列节流逻辑
- `cocos/asset/asset-manager/asset-manager.ts:511` — `load` 选项含 `maxConcurrency` / `maxRequestsPerFrame` / `maxRetryCount`
- `cocos/asset/asset-manager/shared.ts:71,77,83` — 各管线 `maxRequestsPerFrame` 预设（2 / 20 / 20）

## 核心宏定义

无宏；由 `Downloader` 实例属性与 `load` 选项控制。

## 突破限制方案

1. **提高加载吞吐**：在 `load` / `preload` 的 `options` 中调大 `maxConcurrency` 与 `maxRequestsPerFrame`（代价：带宽 / 主线程解析压力上升）。
2. **首屏策略**：首屏用保守 `maxRequestsPerFrame`（如预设 2）避免卡顿，进入游戏后放开。
3. **分批预载**：用 `preload` + 自定义并发控制，错峰加载大资源。
4. **本地缓存**：开启缓存（`ccache`）减少重复下载，缓解并发压力。
