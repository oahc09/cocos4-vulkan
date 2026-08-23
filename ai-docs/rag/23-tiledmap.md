# 23 · TiledMap 瓦片地图

## 概述
TiledMap 组件（`cc.TiledMap`）解析 TMX（或 CSV/JSON）地图并在 2D 批渲染管线中绘制。引擎层**不设地图层数、瓦片数、地图尺寸的硬上限**，其规格由 TMX 内容、2D 网格缓冲顶点上限与逐层 draw call 共同决定。`tiled-types.ts` 中的 `LAYER = 2` 是 TMX 地图类型枚举成员，**不是层数量上限**。

## 极限参数表

| 参数 | 默认/上限 | 限制类型 | 说明 |
|---|---|---|---|
| 地图层数 (layer) | 无引擎常量上限 | 软件 | 由 TMX 文件决定，每层独立一个 draw batch |
| 单瓦片层顶点数 | ≤ 2D 缓冲上限 (`2^32`/`65536`) | 硬件 | 单层超量自动分段（renderData 按 `vertexCount/4` 计 quad） |
| 地图尺寸 / 瓦片总数 | 无引擎常量上限 | 软件/内存 | 受 TMX 内容与设备内存约束 |
| 2D 批渲染顶点寻址上限 | `ELEMENT_INDEX_UINT ? 2^32 : 65536` | 硬件 | 与 Spine/UI 共享同一 2D 网格缓冲 |

## 底层限制机制
- **逐层独立批**：每个 `TiledLayer` 作为独立渲染对象，经 `batcher2D` 单独提交（`tiled-layer.ts`、`assembler/simple.ts:74` 取 `director.root.batcher2D`）。层数越多 = draw call 越多，是 TiledMap 性能的主要约束。
- **顶点缓冲共享**：2D 渲染统一使用动态 `MeshBuffer`，顶点寻址上限由索引格式决定（支持 `ELEMENT_INDEX_UINT` 时为 2^32，否则 65536，`mesh-buffer.ts:257`）。单层瓦片数过大时引擎按 `vertexCount/4` 的 quad 切分渲染。
- **`LAYER = 2` 的误读**：`tiled-types.ts:102` 的 `LAYER = 2` 属于 `TMXMap` 的枚举（地图类型标识），与"可叠加层数"无关，切勿当作层上限。

## 关键源码定位
- 渲染接入与逐层提交：`cocos/tiledmap/assembler/simple.ts`、`cocos/tiledmap/tiled-layer.ts:432`（取首渲染相机）、`:1534`（`quadCount = vertexCount/4`）
- 类型枚举（非层级上限）：`cocos/tiledmap/tiled-types.ts:98-102`（`LAYER = 2`）
- 2D 顶点硬上限：`cocos/2d/renderer/mesh-buffer.ts:257`
- TMX 解析：`cocos/tiledmap/tmx-xml-parser.ts`、`tiled-map-asset.ts`

## 核心宏定义
- `TileMap.Enum.TMXMap.LAYER = 2`（TMX 地图类型枚举，非层级上限）
- `Feature.ELEMENT_INDEX_UINT`：决定 2D 顶点寻址上限

## 突破限制方案
1. **减少 draw call**：合并静态层、使用 `group` 图层、降低地图层数；对超大地图采用分块加载（只渲染视口内瓦片）。
2. **顶点超限防护**：确保目标设备支持 `ELEMENT_INDEX_UINT`，否则单层顶点被限在 65536；超大单层按瓦片块拆分或降低单层瓦片密度。
3. **内存优化**：优先使用 CSV/Base64 压缩 TMX、压缩图集纹理；地图尺寸受最大纹理尺寸（见 `07-texture-material-pass.md`）约束时，将图集拆分。
4. **性能排查**：TiledMap 卡顿优先看 `batcher2D` 的 draw call 数（每层 + 1），而非骨骼/粒子类指标。
