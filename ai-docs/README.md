# AI 文档 · Cocos Native 引擎

本目录用于存放为 **AI 辅助开发** 准备的工程文档，聚焦 `native/` C++ 原生后端的架构梳理，为后续开发（尤其是 Vulkan 渲染方向）打基础。

## 快速上手（推荐入口）

> 想让 AI / 新人快速理解整个 `native/cocos` 代码结构、运行流程、代码风格，**从这里开始**：

| 文档 | 内容 |
| --- | --- |
| [codebase-guide/00-overview.md](./codebase-guide/00-overview.md) | **代码库总览（主索引）**：分层代码结构、启动/每帧运行流程、代码风格约定、全 23 模块地图（含逐模块子文档链接）、依赖图、改代码前检查清单 |

`codebase-guide/` 目录下含 **1 篇主索引（`00-overview.md`）+ 21 篇逐模块子文档**，均可从主索引的模块地图跳转：
base · core · math · application · renderer · scene · 2d · 3d · gi · primitive · ui · physics · audio · network · storage · editor-support · xr · platform · bindings · plugins · profiler。

## 深入内容（已并入模块文档）

原「架构 / 时序图 / GFX / 管线」四篇独立深入文档已拆解并入对应模块文档，避免重复维护：

- **架构总览 + 模块依赖**：见 [00-overview.md](./codebase-guide/00-overview.md)（§架构与依赖关系）。
- **GFX 深入**（抽象接口 / 设备装饰器链 / gfx-agent 多线程 / 校验层 / Vulkan 后端 / FrameGraph）：见 [renderer.md](./codebase-guide/renderer.md) §GFX 图形抽象层 ~ §Vulkan 后端。
- **管线深入**（双管线体系 / 传统 Flow-Stage-Queue / Forward-Deferred-Shadow / 自定义 RenderGraph）：见 [renderer.md](./codebase-guide/renderer.md) §渲染管线体系 ~ §自定义管线 RenderGraph。
- **启动与每帧时序图**：见 [application.md](./codebase-guide/application.md) §时序图 与 [core.md](./codebase-guide/core.md) §每帧流程 (Root::frameMove)。

## 项目速览

- **定位**：Cocos Creator 的原生后端，C++17，跨平台（iOS / Android / macOS / Windows）。
- **渲染**：GFX 图形抽象层 + 多后端（Vulkan / GLES3 / GLES2 / Metal / WebGPU / Empty），本仓库重点为 **Vulkan**（`renderer/gfx-vulkan`）。
- **核心链路**：`ApplicationManager → CocosApplication → Engine → (ScriptEngine / GFX Device / Root) → 渲染管线`。
- **脚本桥接**：`bindings/`（JSB），`jswrapper` 抽象层可切换 v8 / SpiderMonkey / napi / jsvm 后端。

## 关键源码入口

| 关注点 | 文件 |
| --- | --- |
| 应用生命周期 | [application/CocosApplication.cpp](file:///e:/AIProMax/github/cocos4-vulkan/native/cocos/application/CocosApplication.cpp) |
| 引擎主循环 | [engine/Engine.cpp](file:///e:/AIProMax/github/cocos4-vulkan/native/cocos/engine/Engine.cpp) |
| 渲染总控 | [core/Root.h](file:///e:/AIProMax/github/cocos4-vulkan/native/cocos/core/Root.h) / `core/Root.cpp` |
| GFX 后端选择 | [renderer/GFXDeviceManager.h](file:///e:/AIProMax/github/cocos4-vulkan/native/cocos/renderer/GFXDeviceManager.h) |
| GFX 抽象接口 | `renderer/gfx-base/GFXDevice.h` 等 |
| 渲染管线 | `renderer/pipeline/RenderPipeline.h`、`pipeline/custom/` |
| 脚本绑定注册 | `bindings/manual/jsb_module_register.cpp` |
| JS 引擎抽象 | `bindings/jswrapper/SeApi.h` |

## 说明

- 所有图使用 **Mermaid** 语法，支持在 GitHub / VS Code（Markdown Preview Mermaid 插件）/ Qoder 中直接预览。
- 文档基于当前源码梳理，若引擎版本更新，请以源码为准并同步更新本目录。
