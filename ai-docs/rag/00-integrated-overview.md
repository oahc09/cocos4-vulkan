---
id: kb-integrated-overview
title: 引擎规格标准综合视图
category: 综合
engine: Cocos Creator 4.x
updated: 2026-08-29
layer: L1
domain: meta
module: overview
breakable: false
tags: [overview, integration, spec, index]
sources: [cocos/gfx/base/define.ts:747, cocos/2d/renderer/mesh-buffer.ts:257]
---
# Cocos 引擎规格标准 · 综合整合视图（Integrated Spec Standards）

> 本文由 `ai-docs/rag/` 下 30 个专题条目（01–30）提炼整合而成，提供「分层体系 + 真正不可突破的天花板 + 分类速查 + 交叉约束」的全局视角，供检索增强（RAG）与架构评审使用。所有数值均来自本仓库 `cocos4-vulkan`（Cocos Creator 4.x）源码快照，逐条溯源见各分文件。

## 一、规格体系分层

引擎的全部"规格/极限"可按约束力分为三层。排查性能或扩容问题时，先判定目标参数落在哪一层，再决定可行解法。

| 层级 | 约束力 | 来源 | 能否突破 | 典型参数 |
|---|---|---|---|---|
| **L1 硬件能力** | GPU/驱动/图形 API 决定 | `DeviceCaps`（`cocos/gfx/base/define.ts:747`） | 不可改，仅能降级适应 | 纹理尺寸、UBO 字节、顶点属性数、RT 数、Compute 共享内存 |
| **L2 软件常量** | 引擎宏/常量决定 | 各模块 `MAX_*` / `range()` | 改引擎代码可调 | `MAX_PASS_COUNT=8`、`TERRAIN_MAX_LAYER_COUNT=256`、`MAX_BLOOM_FILTER_PASS_NUM=6`、**`MAX_CAPACITY=1024`**、`maxAtlasCount=5` |
| **L3 性能/平台** | 无硬上限，受带宽/draw/算力约束 | 运行时动态 | 优化而非突破 | 灯光总数、粒子数、骨架 bone/slot、PostProcess 特效数、摄像机/音频 |

> 共性结论：Cocos 4.x 对动态数据（骨骼、Morph、灯光、粒子、2D 顶点）统一采用「小容量 GPU 快路径 + 超限自动降级/动态扩容」策略。真正不可突破的天花板只有 L1 的硬件指标与 L2 中少数写死常量。

## 二、真正不可突破的天花板（True Hard Ceilings）

以下为跨模块共享、修改引擎也无法绕过的硬约束，是规格评审的"红线"。

| 天花板 | 数值 | 约束范围 | 源码 |
|---|---|---|---|
| 2D 顶点寻址上限 | `ELEMENT_INDEX_UINT ? 2^32 : 65536` | 全部 2D 渲染：Spine / DragonBones / TiledMap / UI | `cocos/2d/renderer/mesh-buffer.ts:257` |
| UBO 块大小 | `maxUniformBlockSize`（最小保证 16KB，桌面常 64KB） | 所有材质/全局 uniform | `DeviceCaps`（Vulkan `VKDevice.cpp` 取自 `VkPhysicalDeviceLimits`） |
| UBO 偏移对齐 | `uboOffsetAlignment`（常 256B） | 动态 UBO 写入 | `DeviceCaps` |
| 最大纹理尺寸 | `maxTextureSize` | 所有纹理/RT/图集 | `DeviceCaps` |
| 颜色附件数 | `maxColorAttachments` | 单 RT / 多渲染目标 | `DeviceCaps` |
| 顶点属性槽位 | `maxVertexAttributes` | 顶点布局（含蒙皮/Morph 占用） | `DeviceCaps` |
| 硬件后端 | 无 `gfx-d3d12` | 仅 vulkan/wgpu/metal/gles2/gles3 | `native/cocos/renderer/` |

> 关键事实：本仓库**不含 D3D12 后端**。所有硬件规格统一收敛到 `DeviceCaps`，不存在"D3D12 专属上限"。

## 三、分类整合速查表

### A. 硬件能力（L1）
- 全量字段见 [01-device-caps.md](./01-device-caps.md)；取值由各后端（`webgl2-device.ts` / `VKDevice.cpp` 等）从 `VkPhysicalDeviceLimits` / GL 常量填充。
- 注意：Shader **插值器 Varyings**（`MAX_VARYING_VECTORS`，GLES3 最小 15）是 GL 常量但引擎**未纳入 `DeviceCaps`**，属隐式限制（[15-varyings.md](./15-varyings.md)）。

### B. 3D 渲染动态模块（L2/L3）
| 模块 | 上限 | 类型 | 突破方向 | 分文件 |
|---|---|---|---|---|
| 骨骼动画 | uniform 关节硬 256 / 纹理通道 256 / 顶点影响 4 | L2 | 超限切 JointTexture | [02-skeleton.md](./02-skeleton.md) |
| BlendShape | GPU 60 target，>60 CPU 回退 | L2/L3 | 拆分网格或接受 CPU 路径 | [03-blendshape.md](./03-blendshape.md) |
| 光源 | Forward 1/pass，Deferred 10/pass，总灯数无上限 | L3 | 切 Deferred / 聚类剔除 | [04-light.md](./04-light.md) |
| Uniform/UBO | 见 L1 | L1 | 拆 UBO / 用 SSBO | [05-uniform-ubo.md](./05-uniform-ubo.md) |
| 顶点/索引 | Uint16 索引 65535；属性数=`maxVertexAttributes` | L1/L2 | 用 uint32 索引（需 `ELEMENT_INDEX_UINT`） | [06-vertex-index.md](./06-vertex-index.md) |
| 纹理/材质/Pass/RT | 单 SubModel `MAX_PASS_COUNT=8`；RT 数=`maxColorAttachments` | L2 | 拆分 SubModel | [07-texture-material-pass.md](./07-texture-material-pass.md) |
| 粒子 | `capacity` 默认 100，无硬上限 | L3 | 受 buffer 内存约束 | [08-particle.md](./08-particle.md) |
| 地形 | `TERRAIN_MAX_LAYER_COUNT=256` | L2 | 引擎常量 | [09-terrain.md](./09-terrain.md) |
| 阴影 | CSM 4 级；`maxReceived=4` | L2 | 调 `shadowMapSize` | [12-shadow.md](./12-shadow.md) |
| 光照探针/GI | `giSamples`[64,65535] 默认 1024；`bounces`[1,4] 默认 2 | L2 | 改 decorators 范围 | [14-light-probe.md](./14-light-probe.md) |
| 计算着色器 | `maxComputeSharedMemorySize` / 工作组维度 | L1 | `DeviceCaps` | [16-compute.md](./16-compute.md) |
| **GPU Instancing** | 单批 `MAX_CAPACITY=1024`（初始 32、翻倍扩容） | L2 | 改常量 / 依赖自动拆批 | [25-gpu-instancing.md](./25-gpu-instancing.md) |
| **反射探针** | 数量无上限；CUBE 受显存、PLANAR 受重绘开销 | L3 | CUBE 优先 / 降低刷新频率 | [27-reflection-probe.md](./27-reflection-probe.md) |
| **Shader 变体** | 无硬上限，宏组合呈 **2^N 爆炸** | L2 | 预声明 `combinations` / 削减宏数量 | [30-shader-variant.md](./30-shader-variant.md) |

### C. 2D 与特效（L2/L3，统一受 2D 顶点天花板约束）
| 模块 | 上限 | 类型 | 突破方向 | 分文件 |
|---|---|---|---|---|
| Spine/DragonBones | 无 bone/slot 引擎常量；2D 顶点 `2^32`/`65536` | L3/L1 | 拆骨架 / 缓存模式 | [22-spine-dragonbones.md](./22-spine-dragonbones.md) |
| TiledMap | 无层数/瓦片引擎常量；逐层独立 draw | L3 | 减层 / 分块加载 | [23-tiledmap.md](./23-tiledmap.md) |
| 后处理 | 特效数无 `MAX_`；Bloom 级数 6；`shadingScale`∈[0.01,4] | L2/L3 | 关重特效 / 降 `shadingScale` | [24-post-process.md](./24-post-process.md) |
| UI 遮罩嵌套 | 受 stencil 位深约束（典型 ≤8 层） | L1 | 减嵌套 | [13-ui-mask.md](./13-ui-mask.md) |
| 文本 Label 池 | `MAX_LABEL_CANVAS_POOL_SIZE=20` | L2 | 引擎常量 | [18-label-pool.md](./18-label-pool.md) |
| **2D 动态合图** | `maxAtlasCount=5`、`textureSize=2048`、`maxFrameSize=512` | L2 | 调参数 / 改用静态图集 | [28-dynamic-atlas.md](./28-dynamic-atlas.md) |

### D. 场景 / 系统类（L3 为主）
| 模块 | 上限 | 类型 | 分文件 |
|---|---|---|---|
| 动画层 | `MAX_ANIMATION_LAYER=32` | L2 | [10-animation.md](./10-animation.md) |
| 节点层级 Layers | uint32 位掩码，共 32 层，用户可用 bit 0–19（20 层） | L2 | [11-layers.md](./11-layers.md) |
| 输入/触摸 | `MAX_TOUCHES=5` | L2 | [17-input-touch.md](./17-input-touch.md) |
| 摄像机 | 无硬上限（动态数组，性能约束） | L3 | [19-camera.md](./19-camera.md) |
| 音频 | 无引擎硬上限（受平台声部数约束） | L3 | [20-audio.md](./20-audio.md) |
| 资源加载并发 | `maxConcurrency=15`、`maxRequestsPerFrame=15`（可配） | L2 | [21-asset-loading.md](./21-asset-loading.md) |
| **物理系统** | 无刚体数上限；`fixedTimeStep=1/60`、`maxSubSteps=1`、碰撞矩阵 32×32 | L2/L3 | [26-physics.md](./26-physics.md) |
| **视频 / WebView** | 无数量上限；VideoPlayer 仅支持 Web / iOS / Android | L3 | [29-video-webview.md](./29-video-webview.md) |

## 四、交叉约束图（共享天花板）

```
DeviceCaps (define.ts:747)  ──► 纹理尺寸 / UBO / 顶点属性 / RT / Compute
        │
        ├── 2D 渲染统一顶点上限 (mesh-buffer.ts:257)
        │        ├── Spine / DragonBones (22)
        │        ├── TiledMap (23)
        │        └── UI / Label (13, 18)
        │
        ├── MAX_PASS_COUNT=8 (07) ──► 材质 SubModel 拆分
        │
        ├── MAX_CAPACITY=1024 (25) ──► 实例化批次自动拆分
        │
        ├── Shader 变体 2^N (30) ──► 运行时编译卡顿 / 内存增长
        │
        └── UBOSkinning / UBO 容量 (02,05) ──► 骨骼关节数推导

动态降级链：
  骨骼关节超限 → JointTexture 路径
  Morph > 60   → CPU 回退
  灯光总数     → 逐光 forward-add（draw 数 = 性能上限）
  粒子/2D 顶点 → 缓冲动态扩容，直到 L1 天花板
  实例数 > 1024 → 自动新建 InstancedBuffer（draw 数上升，功能无损）
  动态图集耗尽  → 碎图回落独立纹理（DrawCall 上升，无报错）
  物理低帧率    → maxSubSteps=1 时时间膨胀（物理变慢而非追赶）
```

## 五、RAG 元数据与机器可读索引（已完成）

全部 31 个条目（00 综合视图 + 01–30 专题）已统一回填 YAML frontmatter，`index.json` / `index.csv` 由 `build-index.js` 自动生成，可直接导入向量库。

### 元数据 Schema

| 字段 | 取值 | 用途 |
|---|---|---|
| `id` | `kb-<module>` | 稳定主键，切片时作为 doc_id |
| `title` | 中文标题 | 检索展示 |
| `category` | 全局硬件能力 / 3D 渲染模块 / 2D 与 UI / 渲染与着色器 / 场景与系统 / 资源与管线 / 动画与骨架 / 综合 | 一级分类 |
| `layer` | `L1`（硬件） / `L2`（软件常量） / `L3`（性能平台） | 决定解法路径：L1 降级适应、L2 改引擎、L3 优化 |
| `domain` | `rendering` / `ui` / `animation` / `physics` / `audio` / `input` / `asset` / `scene` / `system` / `meta` | 领域过滤 |
| `module` | `gfx` / `skeleton` / `morph` / `light` / `ubo` / `instancing` / `spine` / `postprocess` 等 | 精确定位 |
| `breakable` | `true` / `false` | 是否可突破（硬过滤条件） |
| `tags` | 关键词数组 | 向量库召回增强 |
| `sources` | `文件:行号` 数组 | 溯源与代码跳转 |

### 分层统计（当前 31 条）

| 层级 | 条目数 | 含义 |
|---|---|---|
| L1 | 7 | 硬件能力，不可改 |
| L2 | 17 | 软件常量，改引擎可调 |
| L3 | 7 | 性能/平台约束，优化为主 |

### 使用方式

```bash
# 重新生成 frontmatter 与索引（幂等，可重复执行）
node ai-docs/rag/build-index.js
```

产出 `index.json`（含 `summary` 摘要，适合向量库直接切片）与 `index.csv`（含 `tags` / `sources`，适合表格化检索与人工核对）。新增条目时，在 `build-index.js` 的 `META` 表中补一行后重跑即可。
