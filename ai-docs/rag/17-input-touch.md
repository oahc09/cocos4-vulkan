---
id: kb-input-touch
title: 输入 / 触摸规格标准
category: 输入
engine: Cocos Creator 4.x
updated: 2026-08-22
sources: [cocos/input/types/event/event-touch.ts:45]
---

# 输入 / 触摸规格标准

## 概述

触摸输入的最大同时触点数为硬编码常量 **`MAX_TOUCHES = 5`**，属引擎常量（非硬件，但贴合移动端常见 5 点触控）。

## 极限参数表

| 参数 | 默认 / 上限 | 限制类型 | 说明 |
|---|---|---|---|
| 最大同时触点 `EventTouch.MAX_TOUCHES` | **5** | 软件（常量） | `event-touch.ts:45` |
| 鼠标 / 键盘按键 | 无此类硬上限 | — | 离散事件，按需触发 |

## 底层限制机制

- `EventTouch` 上定义静态常量 `MAX_TOUCHES = 5`（`event-touch.ts:45`），用于约束触摸事件数组中可容纳的触点数量。
- **为何是 5**：移动设备物理触控点通常为 5（少数设备更多），引擎取该值作为合理上界；属软件约定而非图形硬件限制。多点手势（缩放 / 旋转）依赖同时采样的触点，超过 5 的触点不会被引擎层记录。
- 鼠标与键盘事件是离散触发，不存在"同时按键数"的上限概念。

## 关键源码定位

- `cocos/input/types/event/event-touch.ts:45` — `public static MAX_TOUCHES = 5;`

## 核心宏定义

无宏；由 `EventTouch.MAX_TOUCHES` 静态常量控制。

## 突破限制方案

1. **放宽触点数**：直接改 `event-touch.ts:45` 的 `MAX_TOUCHES`（需评估上层手势识别与事件数组容量）。
2. **多指手势**：在 5 点内设计手势逻辑（如双指缩放、三指滑动），避免依赖更多触点。
3. **外接设备**：若需 >5 点（如大屏互动），考虑自定义输入源绕过 `EventTouch` 默认上限。
