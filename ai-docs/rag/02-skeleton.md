---
id: kb-skeleton
title: 骨骼动画 Skeleton
category: 3D 渲染模块
engine: Cocos Creator 4.x
updated: 2026-08-22
layer: L2
domain: rendering
module: skeleton
breakable: true
tags: [skeleton, skinning, joint, CC_JOINT_UNIFORM_CAPACITY]
sources: [cocos/rendering/define.ts:600, cocos/3d/models/skinning-model.ts:139, cocos/root.ts:809, cocos/rendering/custom/web-program-library.ts:914, editor/assets/chunks/builtin/uniforms/cc-skinning.chunk, cc-skinning.chunk:39, skinning-animation-lbs.chunk:45]
---

# 骨骼动画 Skeleton 规格标准

## 概述

骨骼蒙皮的关节数上限由「顶点阶段 uniform 向量预算」与「关节纹理尺寸」双重决定，引擎采用"uniform 通道 + 纹理通道自动降级"策略。

## 极限参数表

| 参数 | 默认 / 推导上限 | 限制类型 | 说明 |
|---|---|---|---|
| `JOINT_UNIFORM_CAPACITY`（历史兼容默认） | 30 | 软件 | 可被运行时覆盖 |
| 运行时 uniform 关节数 `maxJoints` | `floor((maxVertexUniformVectors − 保留量) / 3)`，**硬上限 256** | 硬件+软件 | 顶点 uniform 预算决定 |
| 每关节内存 | 3 × vec4 = 12 float = **48 字节** | 数据布局 | `mat3x4` 写入 UBO |
| 实时关节纹理容量 | `RealTimeJointTexture.WIDTH=256, HEIGHT=3` → **256 关节** | 软件（硬编码） | 纹理通道 |
| 顶点影响数 | LBS / DQS 均为 **4**（`a_weights` 四维） | 顶点属性布局 | — |

## 底层限制机制

蒙皮矩阵有两条存储 / 采样通道，由宏切换：

1. **Uniform 通道**（`CC_USE_SKINNING`，非纹理）：关节矩阵以 `mat3x4` 写入 `cc_joints[CC_JOINT_UNIFORM_CAPACITY * 3]`（`cc-skinning.chunk:39`）。`UBOSkinning` 尺寸 = 关节数 × 48 字节。**瓶颈是顶点阶段 uniform 向量总数 `maxVertexUniformVectors`**——它被「全局 UBO + 本地 UBO」共享，蒙皮 UBO 必须从中划拨，因此关节数受硬件顶点 uniform 上限直接约束。
2. **纹理通道**（`CC_USE_REAL_TIME_JOINT_TEXTURE` / `CC_USE_BAKED_ANIMATION`）：关节矩阵编码进 `RGBA32F` 浮点纹理，VS 通过 `getJointMatrix` / `getJointDQ` 采样，**绕过 uniform 上限**，改受顶点纹理单元数与纹理尺寸约束。

自动降级（`skinning-model.ts:139`）：

```ts
if (UBOSkinning.JOINT_UNIFORM_CAPACITY < skeleton.joints.length) {
    this._realTimeTextureMode = true;   // 超 uniform 容量 → 自动切关节纹理
}
```

运行时 `maxJoints` 推导存在**两处公式、结果不同**：
- 旧 / 原生管线 `root.ts:809`：预留 `max(..., 100)` 向量。
- 自定义（Web）管线 `web-program-library.ts:914`：只减 `38`。

二者均最终 clamp 到 `maxJoints < 256 ? maxJoints : 256`。

## 关键源码定位

- `cocos/rendering/define.ts` — `JOINT_UNIFORM_CAPACITY`(L600)、`UBOSkinning`(L651)、`UBOSkinningTexture`(L606)、`localDescriptorSetLayout_ResizeMaxJoints`(L688)
- `cocos/3d/models/skinning-model.ts` — `RealTimeJointTexture`(L82)、`bindSkeleton`(L139)、`_initRealTimeJointTexture`(L301)
- `cocos/root.ts:809` 与 `cocos/rendering/custom/web-program-library.ts:914` — 两套 `maxJoints` 推导
- GLSL — `editor/assets/chunks/builtin/uniforms/cc-skinning.chunk`、`.../functionalities/skinning-animation-lbs.chunk`、`.../skinning-animation-dqs.chunk`

## 核心宏定义

`CC_USE_SKINNING`、`CC_USE_REAL_TIME_JOINT_TEXTURE`、`CC_USE_BAKED_ANIMATION`、`CC_JOINT_UNIFORM_CAPACITY`、`CC_DEVICE_SUPPORT_FLOAT_TEXTURE`

## 突破限制方案

1. **依赖内置降级**：关节数 > uniform 容量自动切纹理，无需改代码（纹理通道最多 256 关节）。
2. **扩大纹理关节数**：调 `RealTimeJointTexture.WIDTH`（当前 256），需同步改 shader 中硬编码 `/256.0`、`/1024.0` 等除数（`skinning-animation-lbs.chunk:45`）。
3. **提高 uniform 关节数**：主动调大 `localDescriptorSetLayout_ResizeMaxJoints(n)`，受 `maxVertexUniformVectors` 天花板约束，需为全局 UBO 预留空间。
4. **超 256 关节**：改用多纹理 / `texture2DArray` / 存储缓冲（SSBO）方案，或对远模型 LOD 减关节。
