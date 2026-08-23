# Cocos Creator 渲染模块极限参数分析

> 分析对象：`cocos4-vulkan` 仓库（Cocos Creator 4.x 引擎源码）
> 分析范围：骨骼动画（Skeleton）、BlendShape（Morph）、光源控制（Light）、Uniform/UBO 容量
> 分析方法：基于 `cocos/`（TS 管线层）、`native/`（C++ 渲染后端）、`editor/assets/chunks/`（GLSL 宏与 UBO 定义）交叉推导

---

## 0. 前置结论（版本与后端事实）

- 本仓库为 **Cocos Creator 4.x**，不是 3.x。渲染后端目录 `native/cocos/renderer/` 下仅存在 `gfx-vulkan / gfx-wgpu / gfx-metal / gfx-gles2 / gfx-gles3` 等后端，**不含 `gfx-d3d12`**（D3D12 后端已从该分支移除）。
- 引擎渲染通过 **3 组 Descriptor Set** 组织资源：`SetIndex.GLOBAL / MATERIAL / LOCAL`（`cocos/rendering/define.ts:164-180`）。
- 所有极限参数最终都收敛到 `DeviceCaps`（`cocos/gfx/base/define.ts:747-777`）这个统一能力表上。

---

## 1. 骨骼动画（Skeleton）

### 1.1 极限参数总结表

| 参数 | 默认/推导上限 | 限制类型 |
|---|---|---|
| `JOINT_UNIFORM_CAPACITY`（uniform 关节数） | **30**（历史兼容默认） | 软件逻辑（可由运行时覆盖） |
| 运行时 uniform 关节数 `maxJoints` | `floor((maxVertexUniformVectors - 保留量) / 3)`，**硬上限 256** | 硬件 + 软件双重 |
| 每关节内存 | 3 × vec4 = 12 float = **48 字节** | 数据布局 |
| 实时关节纹理容量 | `RealTimeJointTexture.WIDTH = 256, HEIGHT = 3` → **256 关节** | 软件逻辑（硬编码） |
| 顶点影响数 | LBS / DQS 均为 **4**（`a_weights` 四维） | 顶点属性布局 |

### 1.2 底层限制机制

蒙皮矩阵有两种存储与采样通道，由宏切换：

1. **Uniform 通道**（`CC_USE_SKINNING`，非纹理）：
   - 关节矩阵以 mat3x4 写入 UBO：`cc_joints[CC_JOINT_UNIFORM_CAPACITY * 3]`（`editor/assets/chunks/builtin/uniforms/cc-skinning.chunk:39-41`）。
   - `UBOSkinning` 尺寸 = 关节数 × 3 vec4 × 4 字节 = 关节数 × 48 字节（`cocos/rendering/define.ts:651-682`）。
   - **瓶颈**：顶点阶段 Uniform 向量总数 `maxVertexUniformVectors` 被「全局 UBO + 本地 UBO」共享，蒙皮 UBO 必须从中划拨，因此关节数受硬件顶点 uniform 上限的直接约束。

2. **纹理通道**（`CC_USE_REAL_TIME_JOINT_TEXTURE` / `CC_USE_BAKED_ANIMATION`）：
   - 关节矩阵编码进 `RGBA32F` 浮点纹理（或 `RGBA8888` + `decode32` 打包，宽 ×4）。
   - VS 通过 `getJointMatrix` / `getJointDQ` 采样（`skinning-animation-lbs.chunk` / `skinning-animation-dqs.chunk`）。
   - **绕过 uniform 上限**，改为受顶点纹理单元数与纹理尺寸约束。

切换逻辑（`cocos/3d/models/skinning-model.ts:139-164`）：

```ts
if (UBOSkinning.JOINT_UNIFORM_CAPACITY < skeleton.joints.length) {
    this._realTimeTextureMode = true;   // 超过 uniform 容量 → 自动降级到关节纹理
}
```

运行时 uniform 关节数的推导（两处，公式略不同）：

- 旧/原生管线 `cocos/root.ts:809-815`：
  ```ts
  const usedUBOVectorCount = Math.max(
      (UBOGlobalEnum.COUNT + UBOCameraEnum.COUNT + UBOShadowEnum.COUNT
       + UBOLocalEnum.COUNT + UBOWorldBound.COUNT) / 4, 100);
  let maxJoints = Math.floor((device.capabilities.maxVertexUniformVectors - usedUBOVectorCount) / 3);
  maxJoints = maxJoints < 256 ? maxJoints : 256;   // 硬上限 256
  localDescriptorSetLayout_ResizeMaxJoints(maxJoints);
  ```
- 自定义（Web）管线 `cocos/rendering/custom/web-program-library.ts:914-917`：
  ```ts
  // 兼容旧版：maxVertexUniformVectors == 128 时 maxJoints == 30
  let maxJoints = Math.floor((this.device.capabilities.maxVertexUniformVectors - 38) / 3);
  maxJoints = maxJoints < 256 ? maxJoints : 256;
  UBOSkinning.initLayout(maxJoints);
  ```

### 1.3 关键源码定位

| 项 | 路径 |
|---|---|
| UBO/容量常量 | `cocos/rendering/define.ts`（`JOINT_UNIFORM_CAPACITY` L600、`UBOSkinning` L651、`UBOSkinningTexture` L606、`UBOSkinningAnimation` L628、`localDescriptorSetLayout_ResizeMaxJoints` L688） |
| 蒙皮模型（纹理降级逻辑） | `cocos/3d/models/skinning-model.ts`（`RealTimeJointTexture` L82、`bindSkeleton` L139、`_initRealTimeJointTexture` L301） |
| uniform 关节数推导 | `cocos/root.ts:809`、`cocos/rendering/custom/web-program-library.ts:914` |
| 宏注入 | `cocos/rendering/render-pipeline.ts:733-734`、`cocos/rendering/custom/web-pipeline.ts:1254-1255` |
| GLSL 采样 | `editor/assets/chunks/builtin/uniforms/cc-skinning.chunk`、`.../functionalities/skinning-animation-lbs.chunk`、`.../skinning-animation-dqs.chunk` |

核心宏：`CC_USE_SKINNING`、`CC_USE_REAL_TIME_JOINT_TEXTURE`、`CC_USE_BAKED_ANIMATION`、`CC_JOINT_UNIFORM_CAPACITY`、`CC_DEVICE_SUPPORT_FLOAT_TEXTURE`

### 1.4 突破限制方案

1. **依赖引擎内置降级**：关节数 > uniform 容量时自动切纹理，无需改代码。纹理通道最多 256 关节。
2. **扩大纹理关节数**：调 `RealTimeJointTexture.WIDTH`（当前 256），需同步改 shader 中硬编码 `/ 256.0`、`/ 1024.0` 等除数（`skinning-animation-lbs.chunk:45-46` 等）。
3. **提高 uniform 关节数**：主动调大 `localDescriptorSetLayout_ResizeMaxJoints(n)`，但受 `maxVertexUniformVectors` 天花板约束，且需为全局 UBO 预留空间。
4. **超 256 关节**：改为多纹理 / `texture2DArray` / 存储缓冲（SSBO，`UniformStorageBuffer`）方案，或对远距离模型使用 LOD 减关节。

---

## 2. BlendShape（Morph）

### 2.1 极限参数总结表

| 参数 | 默认/上限 | 限制类型 |
|---|---|---|
| `MAX_MORPH_TARGET_COUNT` | **60**（GPU 形变路径上限） | 软件逻辑（UBO 数组固定 60 权重） |
| 权重 UBO 数组 | `cc_displacementWeights[15]` = 15 vec4 = **60 float** | 数据布局 |
| 超过 60 target | 自动回退 **CPU 计算**（无硬上限） | 软件逻辑 |
| 位移纹理格式 | `RGBA32F`（支持浮点纹理）或 `RGBA8888`（decode32 打包 ×4） | 硬件能力 |
| 位移纹理容量 | `nextPow2(顶点数 × target 数)`，宽高 2 的幂、宽为 4 的倍数；上限受 `maxTextureSize` | 硬件 + 软件 |

### 2.2 底层限制机制

- GPU 形变把每个 target 的位移写进一张纹理（`GpuComputing`），顶点着色器按 `nVertices * iTarget + vertexIndex` 逐 target 采样并乘权重累加（`editor/assets/chunks/builtin/functionalities/morph-animation.chunk:62-73`）。
- 权重存在固定尺寸 UBO 中：`cc_displacementWeights[15]`（`editor/assets/chunks/builtin/uniforms/cc-morph.chunk:6-9`），因此 GPU 路径最多 60 个 target。
- 路径选择（`cocos/3d/assets/morph-rendering.ts:120`）：
  ```ts
  if (preferCpuComputing || subMeshMorph.targets.length > UBOMorphEnum.MAX_MORPH_TARGET_COUNT) {
      // CpuComputing（> 60 target 或无 GPU 支持）
  } else {
      // GpuComputing（≤ 60 target）
  }
  ```
- 位移纹理尺寸由 `createVec4TextureFactory` / `bestSizeToHavePixels` 决定（`morph-rendering.ts:535-672`）：浮点纹理 1 像素 = 1 vec4；非浮点用 `decode32` 打包，宽度需 ×4。
- 顶点纹理采样依赖 `gl_VertexID`（`enableVertexId`，宏 `CC_MORPH_TARGET_USE_TEXTURE`），WebGL1 不支持，故 WebGL1 走 CPU 或 `CC_MORPH_PRECOMPUTED`。

> 注：`cc-morph.chunk:3` 声明 `#pragma define-meta CC_MORPH_TARGET_COUNT range([2, 8])`，但运行时该宏被设为 `subMeshMorph.targets.length`（可达 60，`morph-rendering.ts:159`）。二者不一致，`range([2,8])` 属遗留的编辑器元数据提示，不代表真实上限。

### 2.3 关键源码定位

| 项 | 路径 |
|---|---|
| UBO/容量常量 | `cocos/rendering/define.ts`（`UBOMorphEnum` L694、`UBOMorph` L708） |
| 路径选择与纹理生成 | `cocos/3d/assets/morph-rendering.ts`（`StdMorphRendering` L102、`GpuComputing` L232、`CpuComputing` L347、`MorphUniforms` L487、`createVec4TextureFactory` L535） |
| 模型入口 | `cocos/3d/models/morph-model.ts` |
| GLSL | `editor/assets/chunks/builtin/uniforms/cc-morph.chunk`、`.../functionalities/morph-animation.chunk` |

核心宏：`CC_USE_MORPH`、`CC_MORPH_TARGET_COUNT`、`CC_MORPH_TARGET_HAS_POSITION/NORMAL/TANGENT`、`CC_MORPH_TARGET_USE_TEXTURE`、`CC_MORPH_PRECOMPUTED`、`CC_DEVICE_SUPPORT_FLOAT_TEXTURE`

### 2.4 突破限制方案

1. **>60 target 自动 CPU 回退**：引擎已内置，无需改代码，但每帧在 CPU 重算位移并上传纹理，性能开销随「顶点数 × target 数」线性增长。
2. **扩大 GPU 路径 60 上限**：同步修改 `UBOMorphEnum.MAX_MORPH_TARGET_COUNT`、`UBOMorph.LAYOUT` 中 `cc_displacementWeights` 数量、以及 `cc-morph.chunk` 中 `cc_displacementWeights[15]` 数组长度三处。
3. **纹理容量突破**：顶点数 × target 数必须 ≤ `maxTextureSize²`；超限时拆分子网格或按 target 分块（多纹理 + 多采样循环）。
4. **性能优化**：预计算位移（`CC_MORPH_PRECOMPUTED`）把「逐 target 循环」从 VS 移出，仅留一次纹理采样。

---

## 3. 光源控制（Light）

### 3.1 极限参数总结表

| 参数 | 默认/上限 | 限制类型 |
|---|---|---|
| Forward 每 pass 灯光数 `LIGHTS_PER_PASS` | **1** | 软件逻辑 |
| Deferred/Cluster 每 pass 灯光数 | **10** | 软件逻辑 |
| 场景灯光总数 | **无硬上限**（视锥剔除动态收集，light buffer `nextPow2` 动态扩容） | 性能受限 |
| 主光（方向光） | **1** 个（存 `UBOCamera`） | 架构约定 |
| 阴影投射光上限 `maxReceived` | **4**（可配置） | 软件逻辑 |
| 每光 UBO 尺寸 | 5 × vec4 = **80 字节** | 数据布局 |

### 3.2 底层限制机制

- Forward 管线为「主光 base pass + 每 punctual 光 1 次 forward-add pass」：shader 里 `LIGHTS_PER_PASS` 固定为 1（`editor/assets/chunks/builtin/uniforms/cc-forward-light.chunk:3-7`）。
- 灯光数据写入 `UBOForwardLight`，是**动态 UBO**（`DescriptorType.DYNAMIC_UNIFORM_BUFFER`，`define.ts:577-584`）：一个大 light buffer 里每光一个 stride（对齐 `uboOffsetAlignment`），通过 dynamic offset 逐光切换（`cocos/rendering/render-additive-light-queue.ts:478-631`）。
- 光数量动态：`validPunctualLights` 由视锥剔除从 `spotLights/sphereLights/pointLights/rangedDirLights` 收集（`cocos/rendering/scene-culling.ts:53-105`），light buffer 初始 16 条、超限 `nextPow2` 扩容（`render-additive-light-queue.ts:123,485-493`）。**因此灯光数无硬上限，纯性能约束（每光多一次 draw）**。
- 主光（1 个方向光）单独走 `UBOCamera.cc_mainLitDir / cc_mainLitColor`（`define.ts:289-290`）。
- 阴影：`maxReceived = 4` 限制每帧接收阴影的 punctual 光数量（`cocos/render-scene/scene/shadows.ts:330`、`cocos/scene-graph/scene-globals.ts:999`）。

### 3.3 关键源码定位

| 项 | 路径 |
|---|---|
| 灯光 UBO | `cocos/rendering/define.ts`（`UBOForwardLightEnum` L549、`UBOForwardLight` L564、`UBODeferredLight` L596） |
| 叠加光队列 | `cocos/rendering/render-additive-light-queue.ts`（`_lightBufferCount` L123、`_updateUBOs` L478） |
| 光收集/剔除 | `cocos/rendering/scene-culling.ts:53`、`cocos/rendering/custom/define.ts:70` |
| 阴影上限 | `cocos/render-scene/scene/shadows.ts:330`、`cocos/scene-graph/scene-globals.ts:999` |
| GLSL | `editor/assets/chunks/builtin/uniforms/cc-forward-light.chunk` |

核心宏：`CC_PIPELINE_TYPE`（`CC_PIPELINE_TYPE_FORWARD` 等）、`CC_ENABLE_CLUSTERED_LIGHT_CULLING`、`LIGHTS_PER_PASS`

### 3.4 突破限制方案

1. **减少 draw 数**：切换 Deferred/Cluster 管线（`LIGHTS_PER_PASS = 10`），一次 pass 处理 10 光。
2. **Forward 下优化灯光数**：缩小 `_lightBufferStride` 对齐浪费、关闭不必要光阴影、烘焙静态光到 lightmap。
3. **提高阴影接收光数**：调大 `ShadowsInfo.maxReceived`（代价：阴影贴图内存与带宽线性增长）。
4. **大规模动态光**：启用 cluster 光剔除（`CC_ENABLE_CLUSTERED_LIGHT_CULLING`）替代逐光 forward-add。

---

## 4. Uniform / UBO 容量

### 4.1 极限参数总结表（DeviceCaps）

| 参数 | 典型值 / 保证最小值 | 限制类型 |
|---|---|---|
| `maxVertexUniformVectors` | Vulkan：`maxUniformBufferRange / 16`；GLES3 最小 256；桌面 GL 4096 | 硬件 |
| `maxFragmentUniformVectors` | GLES3 最小 224；桌面 GL 4096 | 硬件 |
| `maxUniformBlockSize` | Vulkan：`maxUniformBufferRange`（**保证最小 16384 B / 16KB**，桌面通常 65536 B / 64KB） | 硬件 |
| `maxUniformBufferBindings` | WebGL1 硬编码 16；WebGPU 12 | 硬件 |
| `maxVertexTextureUnits` | GLES3 最小 16；桌面 GL 16-32 | 硬件 |
| `maxVertexAttributes` | GLES3 最小 16；Vulkan `maxVertexInputAttributes` | 硬件 |
| `uboOffsetAlignment` | Vulkan `minUniformBufferOffsetAlignment`（常见 256 B） | 硬件 |

### 4.2 底层限制机制

- 能力表来源：
  - Vulkan（`native/cocos/renderer/gfx-vulkan/VKDevice.cpp:311-326`）：`maxVertexUniformVectors = limits.maxUniformBufferRange / 16`、`maxUniformBlockSize = limits.maxUniformBufferRange`、`uboOffsetAlignment = limits.minUniformBufferOffsetAlignment`。
  - WebGL2（`cocos/gfx/webgl2/webgl2-device.ts:173-195`）：查询 GL 参数；iOS 微信浏览器 clamp `maxVertexUniformVectors ≤ 256`、Safari clamp ≤ 512（规避 iOS Metal 后端 bug）。
  - WebGPU（`cocos/gfx/webgpu/webgpu-device.ts:547-549`）：`maxUniformBufferBindings = 12` 或 `limits.maxUniformBufferBindingSize`。
- 3 组 Descriptor Set 的 UBO/采样器数量由 `bindingMappingInfo` 固定（`define.ts:171-180`）：GLOBAL 4 UBO + 4 sampler、LOCAL 7 UBO + N sampler。
- **关键连锁**：顶点阶段 uniform 向量 = 全局 UBO（`UBOGlobal`+`UBOCamera`+`UBOShadow`+`UBOCSM`）+ 本地 UBO（`UBOLocal`+`UBOSkinning`）共享，这是蒙皮关节数被压缩的根源。

### 4.3 引擎内置 UBO 尺寸清单

| UBO | 成员构成 | 尺寸 |
|---|---|---|
| `UBOGlobal` | 5 × vec4 | 80 B |
| `UBOCamera` | 6 × mat4 + 13 × vec4 | 592 B |
| `UBOShadow` | 2 × mat4 + 8 × vec4 | 256 B |
| `UBOCSM` | 4 级（3 vec4 + 1 vec4 + 1 mat4 + 2 vec4） + 1 vec4 | 656 B |
| `UBOLocal` | 2 × mat4 + 6 × vec4 | 224 B |
| `UBOSkinning` | 关节数 × 3 vec4（默认 30 关节 1440 B，最大 256 关节 12288 B） | 关节数 × 48 B |
| `UBOMorph` | 15 vec4 权重 + 1 vec4 信息 | 256 B |
| `UBOForwardLight` | 5 × vec4 / 光 | 80 B / 光 |

（以上由 `define.ts` 各 `UBO*Enum.SIZE = COUNT * 4` 推导，`COUNT` 单位为 float。）

### 4.4 突破限制方案

1. **对齐优化**：`uboOffsetAlignment` 常为 256 B，动态 UBO（如 light buffer）需按 stride 对齐，避免无效填充。
2. **大数据移出 uniform**：关节、morph、cluster 光等超大数据改用**纹理**或**存储缓冲（SSBO，`UniformStorageBuffer` / `maxShaderStorageBlockSize`，通常 128MB 级）**，绕开 `maxUniformBlockSize`（16KB~64KB）。
3. **精简全局 UBO**：减少全局 UBO 成员，为蒙皮 UBO 腾出顶点 uniform 向量预算。
4. **跨平台兜底**：针对 iOS/微信浏览器，遵循引擎已有的 256/512 顶点 uniform clamp，必要时主动降级到关节纹理。

---

## 5. 汇总：硬性硬件限制 vs 软件逻辑限制

| 模块 | 硬性硬件限制 | 软件逻辑限制 |
|---|---|---|
| 骨骼动画 | `maxVertexUniformVectors`、顶点纹理单元数、`maxTextureSize` | uniform 关节数（推导 + 256 上限）、纹理宽 256、顶点影响 4 |
| BlendShape | `maxTextureSize`、浮点纹理支持 | GPU 60 target、`CC_MORPH_TARGET_COUNT` 数组 15 vec4 |
| 光源 | 每帧带宽（draw 数）、阴影贴图内存 | Forward 1 光/pass、Deferred 10 光/pass、`maxReceived=4` |
| Uniform/UBO | `maxUniformBlockSize`（16KB~64KB）、`maxUniformBufferBindings`、`uboOffsetAlignment` | Descriptor Set 布局、各 UBO 固定成员布局 |

**共性结论**：Cocos 4.x 对「关节 / morph / 灯光」均采用「小容量 GPU 快路径 + 超大容量自动降级」策略——uniform 关节超限切纹理、morph 超 60 切 CPU、灯光靠动态 UBO + 逐光 pass 无限扩展。真正的硬天花板只有 `DeviceCaps` 里的 uniform 字节数、纹理尺寸与带宽。
