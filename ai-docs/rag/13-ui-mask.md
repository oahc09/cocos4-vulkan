---
id: kb-ui-mask
title: UI 遮罩嵌套
category: 2D 与 UI
engine: Cocos Creator 4.x
updated: 2026-08-22
layer: L1
domain: ui
module: mask
breakable: false
tags: [mask, stencil, ui]
sources: [cocos/2d/renderer/stencil-manager.ts:92, cocos/2d/renderer/stencil-manager.ts:183, cocos/2d/renderer/stencil-manager.ts:192, cocos/2d/renderer/stencil-manager.ts:137, cocos/2d/renderer/stencil-manager.ts:209]
---

# UI 遮罩嵌套 / 2D 合批规格标准

## 概述

UI 的 `Mask` / `Mask2D` 通过 **stencil buffer** 实现嵌套裁剪，其**嵌套层数受 stencil buffer 位深约束**（典型 8 位 ⇒ 约 ≤8 层嵌套）。

## 极限参数表

| 参数 | 默认 / 上限 | 限制类型 | 说明 |
|---|---|---|---|
| UI 遮罩嵌套层数 | 受 **stencil buffer 位深**约束（典型 8 位 ⇒ ≤8 层） | 硬件 | `stencil-manager.ts` 按 `1 << stackLength` 分配 bit |
| stencil 写入掩码 `stencilMask` | `0xffff`（16 位） | 软件 | `stencil-manager.ts:92` |
| 单 mask 栈深 | 引擎用 `1 << (maskStack.length-1)` 取 bit，超位深则溢出 | 硬件 | `stencil-manager.ts:183,192` |
| 2D 单批顶点上限 | 65535（同 [06-顶点/索引](./06-vertex-index.md)） | 软件+硬件 | — |

## 底层限制机制

- `StencilManager` 维护 `_maskStack: number[]`，每进入一层 mask 就 push 一个掩码位（`stencil-manager.ts:137`）。
- 取参考值时用 `1 << (this._maskStack.length - 1)`（`stencil-manager.ts:183`），写入掩码用 `1 << this._maskStack.length`（`stencil-manager.ts:192`）。这意味着**每一层 mask 占用 stencil 值的一个 bit**。
- **根因**：stencil buffer 通常为 8 位（部分平台更高），故可表达的独立 mask 层数 ≈ stencil 位深。一旦 `maskStack.length` 超过位深，bit 左移溢出，遮罩关系错乱。
- 合批（2D batching）另受单批顶点 65535 上限约束（见 [06](./06-vertex-index.md)）。

## 关键源码定位

- `cocos/2d/renderer/stencil-manager.ts:92` — `stencilMask: 0xffff`
- `cocos/2d/renderer/stencil-manager.ts:137` — `maskStack.push(mask)`
- `cocos/2d/renderer/stencil-manager.ts:183` — `getStencilRef = 1 << (maskStack.length - 1)`
- `cocos/2d/renderer/stencil-manager.ts:192` — `getWriteMask = 1 << maskStack.length`
- `cocos/2d/renderer/stencil-manager.ts:209` — `maskStack.length`（当前嵌套层数）

## 核心宏定义

无统一宏；由 `StencilManager` 的 mask stack 与 stencil buffer 位深决定。

## 突破限制方案

1. **控制嵌套深度**：UI 结构尽量扁平，避免超过 stencil 位深（典型 8 层）的 mask 嵌套。
2. **改用其他裁剪**：非必须时用 `RectMask2D`（基于 scissor / 几何裁剪）替代 `Mask`（stencil），减少 stencil bit 占用。
3. **提升 stencil 位深**：在支持 16/24 位 stencil 的后端（如部分桌面 GL）可获得更多嵌套层数——属平台能力，引擎不可控。
4. **拆分 UI 层级**：超深结构拆分为多个独立 Canvas / 子树，各自独立 mask stack。
