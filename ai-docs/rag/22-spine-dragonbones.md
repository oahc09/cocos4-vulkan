# 22 · Spine / DragonBones 骨骼动画（2D 骨架系统）

## 概述
Spine 与 DragonBones 是 Cocos 4.x 内置的两套 2D 骨骼动画方案，均通过 `sp.Skeleton` / `dragonBones.ArmatureDisplay` 组件接入，渲染走 2D 批渲染管线（`batcher2D`）。引擎层**不设置骨骼数/插槽数的硬上限**，真正的"规格"来自两部分：① 底层运行时（spine-core / dragonbones-js）与资源本身；② 2D 网格缓冲的顶点寻址上限。

## 极限参数表

| 参数 | 默认/上限 | 限制类型 | 说明 |
|---|---|---|---|
| 单骨架骨骼数 (bone) | 无引擎常量上限 | 软件/运行时 | 由 spine-core / dragonbones-js 运行时与导出资源决定 |
| 单骨架插槽数 (slot) | 无引擎常量上限 | 软件/运行时 | 同上 |
| 动画缓存模式 | `REALTIME` / `SHARED_CACHE` / `PRIVATE_CACHE` | 软件 | 三选一，见下方枚举 |
| Spine 帧缓存步长 `FrameTime` | `1/60` 秒（固定） | 软件 | 缓存模式下面板帧按此采样 |
| 2D 批渲染顶点寻址上限 | `ELEMENT_INDEX_UINT ? 2^32 : 65536` | 硬件 | 决定单 2D 缓冲可容纳顶点数 |

## 底层限制机制
- **无 bone/slot 常量**：Cocos 的 `spine/` 与 `dragon-bones/` 目录中不存在 `MAX_BONE` / `MAX_SLOT` 之类的硬编码上限。骨骼/插槽数量完全取决于美术导出的 `.json`/`.skel`/`.dbbin` 资源，以及底层运行时（spine-core 的 `SkeletonData`、dragonbones-js 的 `Armature`）的承载能力。
- **缓存模式决定合批策略**：`REALTIME`（每帧重算骨架、可合批）、`SHARED_CACHE` / `PRIVATE_CACHE`（预烘焙为顶点帧缓存、不可跨实例合批但 CPU 开销低）。缓存模式下顶点在 `skeleton-cache.ts` 中按 `FrameTime` 逐帧烘焙。
- **2D 网格缓冲顶点上限**：所有 2D 渲染（含 Spine/DragonBones）共享 `MeshBuffer`，其可寻址顶点数由索引格式决定——设备支持 `Feature.ELEMENT_INDEX_UINT` 时为 2^32，否则退化为 65536（`mesh-buffer.ts:257`）。这是 Spine/DragonBones 单缓冲的硬天花板。

## 关键源码定位
- 缓存模式枚举：`cocos/spine/skeleton.ts:70-92`（`SpineAnimationCacheMode`，`REALTIME=0/SHARED_CACHE=1/PRIVATE_CACHE=2`）
- 帧缓存步长：`cocos/spine/skeleton-cache.ts:33`（`const FrameTime = 1/60`）、`:133`（`_maxFrameIdex = floor(duration / FrameTime)`）
- DragonBones 缓存模式：`cocos/dragon-bones/ArmatureDisplay.ts:58-95`（`AnimationCacheMode` 同三值）
- 2D 顶点硬上限：`cocos/2d/renderer/mesh-buffer.ts:257`（`vDataCountLimit`）
- 渲染接入：`cocos/spine/assembler/simple.ts`、`cocos/dragon-bones/assembler/simple.ts`

## 核心宏定义
- `SpineAnimationCacheMode` / `dragonBones.ArmatureDisplay.AnimationCacheMode`：`{ REALTIME, SHARED_CACHE, PRIVATE_CACHE }`
- `SpineSkeletonCacheItem.FrameTime = 1/60`（导出只读常量，`skeleton-cache.ts:312`）
- `Feature.ELEMENT_INDEX_UINT`：决定 2D 顶点上限走 2^32 还是 65536

## 突破限制方案
1. **骨骼/插槽过多 → 拆骨架**：单 Spine 文件骨骼数建议控制在数百以内；超大规模角色拆分多个 `Skeleton` 或改用多 Part 拼合。
2. **降低 CPU**：大量静态/循环动画用 `SHARED_CACHE`（同资源多实例共享）或 `PRIVATE_CACHE`（独立烘焙）替代 `REALTIME`，减少逐帧骨骼解算。
3. **顶点超限**：确保目标设备支持 `ELEMENT_INDEX_UINT`（绝大多数桌面/现代移动 GPU 支持），否则单 2D 缓冲顶点被限制在 65536；超大 2D 场景应拆分多个渲染对象/图层。
4. **大数据量**：Spine 资源优先用二进制（`.skel`）格式减小解析与内存开销；纹理图集按 POT 与最大纹理尺寸（见 `07-texture-material-pass.md`）约束规划。
