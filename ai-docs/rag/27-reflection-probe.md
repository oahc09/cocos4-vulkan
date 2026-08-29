---
id: kb-reflection-probe
title: 反射探针 ReflectionProbe
category: 3D 渲染模块
engine: Cocos Creator 4.x
updated: 2026-08-29
layer: L3
domain: rendering
module: reflection-probe
breakable: true
tags: [reflection-probe, cubemap, planar, bake]
sources: [cocos/3d/reflection-probe/reflection-probe-enum.ts:29, cocos/3d/reflection-probe/reflection-probe-manager.ts:38, cocos/3d/reflection-probe/reflection-probe-manager.ts:413, cocos/3d/reflection-probe/reflection-probe-manager.ts:543, cocos/3d/framework/mesh-renderer.ts:163, reflection-probe-manager.ts:254, cocos/3d/reflection-probe/reflection-probe-manager.ts:98, cocos/3d/reflection-probe/reflection-probe-manager.ts:294, cocos/3d/reflection-probe/reflection-probe-manager.ts:401, cocos/3d/reflection-probe/reflection-probe-manager.ts:519]
---

# 反射探针 ReflectionProbe

## 概述
反射探针为场景提供局部反射信息，分两种实现：**CUBE**（烘焙立方体贴图，运行时采样）与 **PLANAR**（平面反射，实时重渲染）。反射探针**没有数量硬上限**，但每增加一个 CUBE 探针就多一张 cubemap 显存与一次烘焙开销，PLANAR 探针则每次刷新是一次完整场景重绘——这是它真正的成本天花板。

## 极限参数表

| 参数 | 取值 / 上限 | 限制类型 | 说明 |
|---|---|---|---|
| 探针类型 `ProbeType` | `CUBE` / `PLANAR` | 软件 | 二选一，决定烘焙还是实时 |
| 模型反射模式 `ReflectionProbeType` | `NONE` / `BAKED_CUBEMAP` / `PLANAR_REFLECTION` | 软件 | 挂在 MeshRenderer 上 |
| 探针数量 | 无引擎常量上限 | 性能 / 显存 | CUBE 受显存，PLANAR 受重绘开销 |
| cubemap mip 使用 | `probe.cubemap.mipmapLevel`（未烘焙时降级为 1.0） | 软件 | 按粗糙度选 mip 模拟模糊 |
| 探针默认剔除层 | 排除 `UI_2D` / `UI_3D` / `GIZMOS` / `EDITOR` | 软件 | `REFLECTION_PROBE_DEFAULT_MASK` |
| 混合（blending）支持 | 盒体相交混合 | 软件 | `_isUsedBlending(model)` |

## 底层限制机制
- **CUBE 探针**：烘焙期对 6 个面各渲染一次场景，结果写入 cubemap；运行时按模型世界包围盒与探针盒体求交（`reflection-probe-manager.ts:543` 的 `aabbWithAABB`），命中则采样。粗糙度通过 mip 层级近似——`reflection-probe-manager.ts:413` 写入 `probe.cubemap.mipmapLevel + mipAndUseRGBE`，若探针尚未烘焙则退化为 `1.0 + mipAndUseRGBE`（即无有效模糊）。
- **PLANAR 探针**：每次刷新是对场景的一次完整重渲染（镜像相机），成本随场景复杂度线性上升，多个 PLANAR 探针叠加会直接表现为帧率塌陷。
- **层掩码剔除**：`REFLECTION_PROBE_DEFAULT_MASK`（`reflection-probe-manager.ts:38`）把 UI / Gizmo / 编辑器对象排除在探针捕获之外，避免自反射与污染。
- **混合判断**：`_isUsedBlending(model)` 决定模型是否参与多探针混合，混合会触发 cubemap 刷新（`reflection-probe-manager.ts:254` 注释说明烘焙结束需刷新）。

## 关键源码定位

| 位置 | 内容 |
|---|---|
| `cocos/3d/reflection-probe/reflection-probe-enum.ts:29` | `enum ReflectionProbeType { NONE, BAKED_CUBEMAP, PLANAR_REFLECTION }` |
| `cocos/3d/reflection-probe/reflection-probe-manager.ts:38` | `REFLECTION_PROBE_DEFAULT_MASK` 剔除层定义 |
| `cocos/3d/reflection-probe/reflection-probe-manager.ts:98-101` | 按 `ReflectionProbeType` 分流处理 |
| `cocos/3d/reflection-probe/reflection-probe-manager.ts:294-295` | PLANAR 探针与模型层判定 |
| `cocos/3d/reflection-probe/reflection-probe-manager.ts:401-413` | CUBE 探针参数打包（含 mipmapLevel） |
| `cocos/3d/reflection-probe/reflection-probe-manager.ts:519-521` | 模型反射类型判定 |
| `cocos/3d/reflection-probe/reflection-probe-manager.ts:543` | `aabbWithAABB` 探针盒体求交 |
| `cocos/3d/framework/mesh-renderer.ts:163,276-297` | `_reflectionProbeType` / `bakeToReflectionProbe` 属性 |

## 核心宏定义与常量
- `ReflectionProbeType` —— `NONE` / `BAKED_CUBEMAP` / `PLANAR_REFLECTION`
- `ProbeType` —— `CUBE` / `PLANAR`
- `REFLECTION_PROBE_DEFAULT_MASK` —— 默认剔除 UI / Gizmo / 编辑器层

## 突破限制方案
1. **CUBE 优先于 PLANAR**：除镜面/水面等强需求外一律用烘焙 CUBE，把实时重绘成本转为一次性烘焙成本。
2. **控制 PLANAR 刷新频率**：PLANAR 探针按需 `needRefresh` 触发，避免每帧刷新；可脚本降频（如每 N 帧一次）。
3. **缩小探针盒体**：`aabbWithAABB` 求交意味着盒体越小命中模型越少，混合计算越轻。
4. **烘焙前置校验**：未烘焙的 CUBE 探针 mip 退化为 1.0（无粗糙度模糊），表现为"反射死板"——排查时先确认 `probe.validate()` 与 cubemap 是否已生成。
5. **显存预算**：CUBE 探针数是 `6 × size² × 4 bytes × mip 链` 的显存开销，按目标平台显存倒推上限，不要依赖引擎限制。

## 关联条目
- [14 光照探针 / GI](./14-light-probe.md) —— 同为烘焙型 GI，机制互补
- [12 阴影 Shadow](./12-shadow.md) —— 同属"无硬上限、成本约束"类
- [07 纹理 / 材质 / Pass](./07-texture-material-pass.md) —— cubemap 尺寸受纹理上限约束
