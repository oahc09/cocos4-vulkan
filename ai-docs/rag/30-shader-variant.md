---
id: kb-shader-variant
title: Shader 变体与 Program 库
category: 渲染与着色器
engine: Cocos Creator 4.x
updated: 2026-08-29
layer: L2
domain: rendering
module: shader-variant
breakable: true
tags: [shader, variant, defines, macro, compile]
sources: [cocos/rendering/custom/web-program-library.ts:1099, cocos/rendering/custom/web-program-library.ts:1100, cocos/rendering/custom/web-program-library.ts:1116, cocos/rendering/custom/web-program-library.ts:1121, cocos/rendering/custom/web-program-library.ts:1127, cocos/rendering/custom/web-program-library.ts:1157, cocos/rendering/custom/web-program-library.ts:1055, cocos/rendering/custom/web-program-library.ts:1120, cocos/rendering/custom/web-program-library.ts:1149, cocos/rendering/custom/web-program-library.ts:1309]
---

# Shader 变体与 Program 库

## 概述
引擎不会为每个材质编译一份 shader，而是**按宏组合（defines）缓存复用**：每一组唯一的宏组合对应一个"变体（variant）"与一个已编译的 `ProgramProxy`。该机制**没有数量硬上限**，但变体数随宏数量呈 **2^N 组合爆炸**，表现为**运行时编译卡顿**与**内存/显存持续增长**——这是本项目最隐蔽的性能陷阱之一。

## 极限参数表

| 参数 | 取值 / 上限 | 限制类型 | 说明 |
|---|---|---|---|
| 变体数量 | 无硬编码上限 | 内存 / 编译时间 | N 个二值宏 → 最多 2^N 个变体 |
| 变体缓存键 | `getVariantKey(programInfo, defines)` | 软件 | 字符串 key，存于 `Map` |
| 缓存容器 | `group.programProxies: Map<key, host>` | 软件 | 每 phase 一个 `ProgramGroup` |
| 宏来源 | 材质宏 + 管线宏 `pipeline.macros` + 全局宏 | 软件 | 运行时 `Object.assign` 合并 |
| 预编译组合 | `effect.combinations`（effect 文件声明） | 软件 | 声明后启动期预编译 |
| 属性激活 | `getActiveAttributes(programInfo, attributes, defines)` | 软件 | 宏决定哪些顶点属性参与 |

## 底层限制机制
- **变体生成流程**：`getProgramVariant`（`web-program-library.ts:1099`）先把管线宏合并进 defines（`:1100`），再用 `getVariantKey` 生成 key（`:1116`），然后查 `programProxies` 缓存（`:1120-1121`）。**未命中即现场编译**：`prepareDefines` 展开宏数组（`:1127`）→ 编译 → `programHosts.set(key, host)` 写回缓存（`:1157`）。首次出现某组合的那一帧会产生可感知的卡顿。
- **组合爆炸**：若一个 effect 有 10 个二值宏，理论变体数 1024；20 个则超百万。引擎不会限制，只会不断编译与缓存，内存随之线性上涨。
- **预编译通道**：`web-program-library.ts:1055-1061` 读取 `effect.combinations`，用 `getCombinationDefines` 展开并逐一调用 `getProgramVariant`——这是把运行时卡顿前移到启动期的官方手段。
- **属性按宏激活**：`getActiveAttributes`（`:1149`）依据 defines 决定实际使用的顶点属性集合，因此变体不同，其 `InputAssembler` 的属性布局也可能不同，进一步影响合批（属性布局不一致无法合并）。

## 关键源码定位

| 位置 | 内容 |
|---|---|
| `cocos/rendering/custom/web-program-library.ts:1099` | `getProgramVariant(device, phaseID, name, defines, key)` |
| `cocos/rendering/custom/web-program-library.ts:1100` | `Object.assign(defines, this.pipeline?.macros)` 合并管线宏 |
| `cocos/rendering/custom/web-program-library.ts:1116` | `key = getVariantKey(programInfo, defines)` |
| `cocos/rendering/custom/web-program-library.ts:1120-1121` | `programHosts = group.programProxies; programHost.get(key)` |
| `cocos/rendering/custom/web-program-library.ts:1127` | `prepareDefines(defines, programInfo.defines)` |
| `cocos/rendering/custom/web-program-library.ts:1149` | `getActiveAttributes(...)` 按宏激活属性 |
| `cocos/rendering/custom/web-program-library.ts:1157` | `programHosts.set(key, host)` 写入缓存 |
| `cocos/rendering/custom/web-program-library.ts:1055-1061` | `combinations` 预编译分支 |
| `cocos/rendering/custom/web-program-library.ts:1309` | `phases: Map<number, ProgramGroup>` |
| `cocos/render-scene/core/program-utils.ts` | `getVariantKey` / `prepareDefines` / `getCombinationDefines` 实现 |

## 核心宏定义与常量
- `getVariantKey(programInfo, defines)` —— 变体唯一键生成
- `prepareDefines(defines, programInfo.defines)` —— 宏展开为编译期定义数组
- `effect.combinations` —— effect 文件中声明的预编译组合
- `pipeline.macros` —— 全局管线宏，会并入每个变体

## 突破限制方案
1. **声明 `combinations` 预编译（首选）**：在 `.effect` 中声明常用组合，启动期一次性编译，消除运行时首次卡顿。代价是启动时间变长，需权衡。
2. **削减宏数量**：合并强相关宏、删除冗余分支。每减少 1 个二值宏，理论变体数减半——这是收益最高的优化。
3. **宏分支 → uniform 分支**：对性能不敏感的分支改用 uniform `if`，避免生成新变体（代价是 shader 内有分支开销，需实测）。
4. **收敛全局宏**：`pipeline.macros` 会被并入**所有**变体，一个多余的全局宏会让全项目变体数翻倍，改动前必须评估影响面。
5. **变体审计**：在联调期统计实际生成的 key 数量（可临时在 `programHosts.set` 处埋点），与理论 2^N 对比，定位"意外开启"的宏。

## 关联条目
- [15 Shader Varyings](./15-varyings.md) —— 同为 shader 侧的隐式上限
- [05 Uniform / UBO](./05-uniform-ubo.md) —— 宏分支与 uniform 分支的取舍
- [25 GPU Instancing](./25-gpu-instancing.md) —— 属性布局差异会破坏合批
- [07 纹理 / 材质 / Pass](./07-texture-material-pass.md) —— 材质是宏的主要载体
