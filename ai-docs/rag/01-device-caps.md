---
id: kb-device-caps
title: DeviceCaps 硬件能力表
category: 全局硬件能力
engine: Cocos Creator 4.x
updated: 2026-08-22
layer: L1
domain: rendering
module: gfx
breakable: false
tags: [devicecaps, hardware, limits]
sources: [cocos/gfx/base/define.ts:747, cocos/gfx/webgl2/webgl2-device.ts:171, native/cocos/renderer/gfx-vulkan/VKDevice.cpp:311, cocos/gfx/webgpu/webgpu-device.ts:547, gfx-vulkan/VKDevice.cpp:311, webgl2-device.ts:178, cocos/gfx/base/device.ts:420]
---

# DeviceCaps 硬件能力表

## 概述

`DeviceCaps` 是引擎从图形 API 查询后归一化的**硬件能力总表**，所有模块（骨骼、光照、纹理、UBO 等）的上限最终都收敛到这里。它不区分模块，是整份知识库的"天花板"基准。

## 极限参数表

| 字段 | 含义 | 典型值 / 保证最小值 | 来源（后端填充） | 限制类型 |
|---|---|---|---|---|
| `maxVertexAttributes` | 最大顶点属性槽位数 | WebGL2 ≥16；Vulkan = `limits.maxVertexInputAttributes` | webgl2:172 / vulkan:312 | 硬件 |
| `maxVertexUniformVectors` | 顶点阶段最大 uniform 向量数 | Vulkan = `maxUniformBufferRange/16`；WebGL2 ≥256（iOS 微信 clamp ≤256、Safari ≤512） | webgl2:173 / vulkan:313 | 硬件 |
| `maxFragmentUniformVectors` | 片元阶段最大 uniform 向量数 | Vulkan = `maxUniformBufferRange/16`；WebGL2 ≥224 | webgl2:186 / vulkan:314 | 硬件 |
| `maxTextureUnits` | 片元可采样纹理单元数 | WebGL2 = `MAX_TEXTURE_IMAGE_UNITS` | webgl2:187 | 硬件 |
| `maxVertexTextureUnits` | 顶点可采样纹理单元数 | WebGL2 = `MAX_VERTEX_TEXTURE_IMAGE_UNITS` | webgl2:188 | 硬件 |
| `maxImageUnits` | 图像单元数（计算/存储） | 由后端查询 | — | 硬件 |
| `maxUniformBufferBindings` | 可绑定的 UBO 数量 | WebGL2 = `MAX_UNIFORM_BUFFER_BINDINGS`；WebGPU = 12 | webgl2:189 | 硬件 |
| `maxUniformBlockSize` | 单个 UBO 最大字节 | Vulkan = `maxUniformBufferRange`（**保证 ≥16384B/16KB**，桌面常 65536B/64KB）；WebGL2 = `MAX_UNIFORM_BLOCK_SIZE` | webgl2:190 / vulkan:316 | 硬件 |
| `maxShaderStorageBufferBindings` | 可绑定的 SSBO 数量 | Vulkan = `maxDescriptorSetStorageBuffers` | vulkan:318 | 硬件 |
| `maxShaderStorageBlockSize` | 单个 SSBO 最大字节 | Vulkan = `maxStorageBufferRange`（**常达 128MB 级**） | vulkan:317 | 硬件 |
| `maxTextureSize` | 2D 纹理最大边长 | WebGL2 = `MAX_TEXTURE_SIZE`；Vulkan = `maxImageDimension2D` | webgl2:191 / vulkan:322 | 硬件 |
| `maxCubeMapTextureSize` | CubeMap 最大边长 | WebGL2 = `MAX_CUBE_MAP_TEXTURE_SIZE` | webgl2:192 | 硬件 |
| `maxArrayTextureLayers` | 数组纹理最大层数 | WebGL2 = `MAX_ARRAY_TEXTURE_LAYERS`；Vulkan = `maxImageArrayLayers` | webgl2:193 / vulkan:324 | 硬件 |
| `max3DTextureSize` | 3D 纹理最大边长 | WebGL2 = `MAX_3D_TEXTURE_SIZE`；Vulkan = `maxImageDimension3D` | webgl2:194 / vulkan:325 | 硬件 |
| `maxColorRenderTargets` | 单帧最大颜色附件（MRT）数 | Vulkan = `maxColorAttachments`（桌面常 8） | vulkan:321 | 硬件 |
| `uboOffsetAlignment` | 动态 UBO offset 对齐字节 | Vulkan = `minUniformBufferOffsetAlignment`（**常见 256B**） | webgl2:195 / vulkan:326 | 硬件 |
| `maxComputeSharedMemorySize` | 计算着色器共享内存上限 | Vulkan = `maxComputeSharedMemorySize` | — | 硬件 |
| `maxComputeWorkGroupInvocations` | 单工作组最大调用数 | Vulkan = `maxComputeWorkGroupInvocations` | — | 硬件 |
| `maxComputeWorkGroupSize / Count` | 工作组维度上限（X/Y/Z） | Vulkan = `maxComputeWorkGroupSize/Count` | — | 硬件 |
| `supportQuery` / `supportVariableRateShading` / `supportSubPassShading` | 能力开关布尔量 | 由后端查询 | — | 硬件 |
| `clipSpaceMinZ` / `screenSpaceSignY` / `clipSpaceSignY` | 裁剪空间/屏幕空间坐标约定 | 后端决定（如 WebGL clipSpaceMinZ=-1，Vulkan=0） | — | 硬件 |

## 底层限制机制

- **为什么是这些值**：除 `maxUniformBufferRange/16` 这类推导项外，绝大多数字段是图形 API（`gl.getParameter` / `VkPhysicalDeviceLimits`）的**直接镜像**。Vulkan 后端用 `gfx-vulkan/VKDevice.cpp:311-326` 逐字段搬运 `VkPhysicalDeviceLimits`；WebGL2 后端用 `webgl2-device.ts:171-195` 逐字段 `glGetParameter`。
- **`maxVertexUniformVectors` 的除以 16**：一个 uniform 向量 = 4 个 float = 16 字节。Vulkan 只暴露字节上限 `maxUniformBufferRange`，故用 `/16` 换算成向量数，与 WebGL 的 `MAX_*_UNIFORM_VECTORS` 语义对齐。
- **iOS clamp 的来由**：`webgl2-device.ts:178-184` 因微信浏览器 / iOS Safari 的 Metal 后端 bug，将顶点 uniform 向量上限分别 clamp 到 256 / 512。**这是平台兼容性兜底，不是硬件真上限**，却会真实压低骨骼可调关节数。
- **`uboOffsetAlignment` 的连锁影响**：动态 UBO（如逐光的 light buffer）的 stride 必须按此值对齐，对齐浪费直接决定批量数据的实际占用。

## 关键源码定位

- `cocos/gfx/base/define.ts:747-777` — `DeviceCaps` 类定义（全部字段与默认值）。
- `cocos/gfx/webgl2/webgl2-device.ts:171-195` — WebGL2 后端填充逻辑。
- `native/cocos/renderer/gfx-vulkan/VKDevice.cpp:311-326` — Vulkan 后端填充逻辑。
- `cocos/gfx/webgpu/webgpu-device.ts:547-552` — WebGPU 后端填充（如 `maxUniformBufferBindings = 12`）。
- `cocos/gfx/base/device.ts:420,432` — `maxTextureSize >= 2` 用于判定是否可生成 mipmap。

## 核心宏定义

无直接控制宏；后端在设备初始化时一次性写入 `DeviceCaps`，运行时通过 `device.capabilities.xxx` 读取。

## 突破限制方案

1. **超大数据搬离 UBO**：当 `maxUniformBlockSize`（16KB~64KB）不够时，改用**纹理**或**存储缓冲 SSBO**（`maxShaderStorageBlockSize` 通常 128MB 级）承载矩阵数组、灯光列表等大数据。
2. **对齐省内存**：动态 UBO 按 `uboOffsetAlignment`（常 256B）规划 stride，避免无效填充。
3. **跨平台兜底**：针对 iOS / 微信浏览器遵循引擎已有的 256/512 顶点 uniform clamp，必要时主动降级（如关节改走纹理通道）。
4. **能力查询驱动分支**：运行时读 `capabilities` 决定特性开关（如 `supportVariableRateShading`），而非硬编码。
