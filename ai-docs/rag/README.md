# Cocos 引擎规格标准知识库（Engine Spec Standards KB）

> 从 Cocos Creator 4.x 引擎源码中提炼「渲染 / 场景」相关模块的**极限参数、底层限制机制、源码定位与突破方案**，供检索增强（RAG）与日常查阅。

- 📌 **综合整合视图（先读这份）**：[00-integrated-overview.md](./00-integrated-overview.md) — 分层体系、真正不可突破的天花板、分类速查、交叉约束图。
- 📑 **分主题条目**：下方 01–30，逐条标注 `文件:行号`，适合切片检索。
- 🤖 **机器可读索引**：[index.json](./index.json) / [index.csv](./index.csv) — 由 `build-index.js` 生成，供向量库直接导入。

## 版本与后端事实（分析前提）

| 项 | 结论 |
|---|---|
| 引擎版本 | 本仓库 `cocos4-vulkan` = **Cocos Creator 4.x**（非 3.x） |
| 渲染后端 | `native/cocos/renderer/` 仅含 `gfx-vulkan / gfx-wgpu / gfx-metal / gfx-gles2 / gfx-gles3`，**无 `gfx-d3d12`** |
| 能力收敛点 | 所有硬件能力统一收敛到 `DeviceCaps`（`cocos/gfx/base/define.ts:747`） |
| 资源组织 | 3 组 Descriptor Set：`SetIndex.GLOBAL / MATERIAL / LOCAL`（`cocos/rendering/define.ts:164`） |

## 条目索引

| 编号 | 主题 | 关键上限速览 |
|---|---|---|
| [01](./01-device-caps.md) | DeviceCaps 硬件能力表 | 纹理尺寸 / UBO 字节 / 顶点属性 / RT 数等全量硬件上限 |
| [02](./02-skeleton.md) | 骨骼动画 Skeleton | uniform 关节硬上限 **256**，纹理通道 256 关节，顶点影响 4 |
| [03](./03-blendshape.md) | BlendShape / Morph | GPU 路径 **60** target，超 60 自动 CPU 回退 |
| [04](./04-light.md) | 光源控制 Light | Forward **1** 光/pass，Deferred **10** 光/pass，总灯数无硬上限 |
| [05](./05-uniform-ubo.md) | Uniform / UBO 容量 | `maxUniformBlockSize` 16KB~64KB；`uboOffsetAlignment` 常 256B |
| [06](./06-vertex-index.md) | 顶点 / 索引 / 属性 | Uint16 索引上限 **65535**；顶点属性数 = `maxVertexAttributes` |
| [07](./07-texture-material-pass.md) | 纹理 / 材质 / Pass / RT | 单 SubModel `MAX_PASS_COUNT = 8`；RT 数 = `maxColorAttachments` |
| [08](./08-particle.md) | 粒子系统 Particle | `capacity` 默认 100，无硬上限（受 buffer 内存约束） |
| [09](./09-terrain.md) | 地形 Terrain | `TERRAIN_MAX_LAYER_COUNT = 256`；混合层 `TERRAIN_MAX_BLEND_LAYERS` |
| [10](./10-animation.md) | 动画层 Animation | `MAX_ANIMATION_LAYER = 32` |
| [11](./11-layers.md) | 节点层级 Layers | uint32 位掩码，**共 32 层**，用户可用 bit 0–19（20 层） |
| [12](./12-shadow.md) | 阴影 Shadow | CSM **4 级**；`maxReceived=4`；`shadowMapSize` 可配 |
| [13](./13-ui-mask.md) | UI 遮罩嵌套 / 2D 合批 | 嵌套层数受 **stencil 位深**约束（典型 ≤8 层） |
| [14](./14-light-probe.md) | 光照探针 / GI | `giSamples` [64,65535]（默认 1024）；`bounces` [1,4]（默认 2） |
| [15](./15-varyings.md) | Shader 插值器 Varyings | `MAX_VARYING_VECTORS`（GLES3 最小 15），引擎未纳入 `DeviceCaps` |
| [16](./16-compute.md) | 计算着色器 Compute | `maxComputeSharedMemorySize` / 工作组维度，来自 `DeviceCaps` |
| [17](./17-input-touch.md) | 输入 / 触摸 | `MAX_TOUCHES = 5` |
| [18](./18-label-pool.md) | 文本 Label 画布池 | `MAX_LABEL_CANVAS_POOL_SIZE = 20` |
| [19](./19-camera.md) | 摄像机 Camera | **无硬上限**（动态数组，性能约束） |
| [20](./20-audio.md) | 音频 Audio | **无引擎硬上限**（受平台声部数约束） |
| [21](./21-asset-loading.md) | 资源加载并发 | `maxConcurrency=15`、`maxRequestsPerFrame=15`（可配） |
| [22](./22-spine-dragonbones.md) | Spine / DragonBones（2D 骨架） | 无 bone/slot 引擎常量上限；2D 顶点硬上限 `2^32`/`65536` |
| [23](./23-tiledmap.md) | TiledMap 瓦片地图 | 无层数/瓦片引擎常量上限；逐层独立 draw；`LAYER=2` 为 TMX 枚举非层级上限 |
| [24](./24-post-process.md) | 后处理 PostProcess | 特效数无 `MAX_` 上限；Bloom 滤波级数 **6**；`shadingScale` ∈ [0.01,4] |
| [25](./25-gpu-instancing.md) | GPU Instancing 实例化 | 单批 **`MAX_CAPACITY = 1024`**（初始 32、翻倍扩容，超限自动拆批） |
| [26](./26-physics.md) | 物理系统 Physics | `fixedTimeStep=1/60`、`maxSubSteps=1`、碰撞矩阵 **32×32**；无刚体数上限 |
| [27](./27-reflection-probe.md) | 反射探针 ReflectionProbe | `CUBE` / `PLANAR` 两类；数量无硬上限（显存与重绘开销约束） |
| [28](./28-dynamic-atlas.md) | 2D 动态合图 Dynamic Atlas | `maxAtlasCount=5`、`textureSize=2048`、`maxFrameSize=512` |
| [29](./29-video-webview.md) | 视频 / WebView | VideoPlayer 仅支持 **Web / iOS / Android**；数量无引擎上限 |
| [30](./30-shader-variant.md) | Shader 变体与 Program 库 | 变体数无硬上限，宏组合呈 **2^N 爆炸**（编译卡顿 / 内存增长） |

## 条目统一结构

每篇主题文档遵循以下 6 段式，便于切片检索：

1. **概述** — 一句话定位
2. **极限参数表** — 参数名 / 默认·上限 / 限制类型 / 说明
3. **底层限制机制** — 为什么是这个上限（内存 / 渲染原理）
4. **关键源码定位** — 文件路径:行号 + 变量/宏
5. **核心宏定义** — 主导行为的 shader 宏
6. **突破限制方案** — 实际工程中的扩容 / 优化手段

## 限制类型图例

- **硬件**：由 GPU / 驱动 / 图形 API 决定，引擎层不可改（如 `maxTextureSize`）。
- **软件**：由引擎常量 / 宏决定，可改代码调整（如 `MAX_PASS_COUNT`）。
- **性能**：无硬上限，但受带宽 / draw 数 / 算力约束（如场景灯光总数）。

## 维护说明

- 所有数值均来自本仓库当前源码快照，标注 `文件:行号` 以便回溯。
- 引擎升级后，优先复核 `DeviceCaps`（`define.ts:747`）与各 `MAX_*` 常量。
- 新增条目请复用本 README 的 6 段式结构与索引表。

### 元数据与索引工具

每篇条目头部均为统一 YAML frontmatter（`id` / `title` / `category` / `layer` / `domain` / `module` / `breakable` / `tags` / `sources`），由 [`build-index.js`](./build-index.js) 维护：

```bash
node ai-docs/rag/build-index.js   # 幂等：统一 frontmatter + 生成 index.json / index.csv
```

新增条目时：① 按 6 段式写 `.md`；② 在 `build-index.js` 的 `META` 表补一行元数据；③ 重跑脚本；④ 在本 README 索引表追加一行。

分层约定：`L1` 硬件能力（不可改）｜`L2` 软件常量（改引擎可调）｜`L3` 性能 / 平台约束（优化为主）。当前分布：**L1 = 7、L2 = 17、L3 = 7**。
