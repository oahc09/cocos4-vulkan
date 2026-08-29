---
id: kb-audio
title: 音频 Audio
category: 场景与系统
engine: Cocos Creator 4.x
updated: 2026-08-22
layer: L3
domain: audio
module: audio
breakable: false
tags: [audio, voice, platform]
sources: [cocos/audio/audio-source.ts, cocos/audio/index.ts:27, audio/index.ts:27]
---

# 音频 Audio 规格标准

## 概述

引擎层对**同时播放的音频声部（AudioSource）数量没有硬编码上限**。可播放的并发声部数受**平台音频后端 / 硬件声部数**约束（如 Web Audio 的 voice 数、原生音频设备的并发通道）。

## 极限参数表

| 参数 | 默认 / 上限 | 限制类型 | 说明 |
|---|---|---|---|
| 并发 `AudioSource` 数 | **无引擎硬上限** | 平台/硬件（性能） | `AudioSource` 为组件，数组管理 |
| 单实例播放 `playOneShot` | 受平台声部池约束 | 平台 | 每次 shot 占用一个声部 |
| 音频格式支持 | 平台解码器决定（mp3/ogg/wav…） | 平台 | — |

## 底层限制机制

- `AudioSource` 是挂在节点上的音频组件（`audio/index.ts:27` 导出），引擎以组件实例方式管理，**没有 `MAX_VOICE` / `maxAudioSource` 类常量**——即引擎不在代码层限制同时存在的 `AudioSource` 数量。
- **真正的上限在平台层**：Web 端走 Web Audio API，可同时发声的 voice 数由浏览器 / 音频硬件决定（通常数十个，过量会抢占或静音）；原生端受操作系统音频会话的并发通道限制。
- `playOneShot` 等一次性播放会在底层占用一个声部直至播放结束，频繁调用会快速逼近平台声部上限，表现为部分音效被丢弃。

## 关键源码定位

- `cocos/audio/index.ts:27` — `import { AudioSource } from './audio-source'`
- `cocos/audio/audio-source.ts` — `AudioSource` 组件（播放 / 暂停 / 循环 / 音量）
- （无 `MAX_*` 常量：搜索引擎 `cocos/audio` 无 voice 上限定义）

## 核心宏定义

无统一宏；由 `AudioSource` 组件与平台音频后端决定。

## 突破限制方案

1. **控制并发声部**：音频密集场景主动限制同时 `play` 的 `AudioSource` 数，避免命中平台声部上限。
2. **对象池 / 复用**：对短音效复用有限数量的 `AudioSource`，用 `playOneShot` 替代常驻实例。
3. **优先级 / 淘汰**：自管音频优先级，超限时停掉最不重要 / 最远的音源。
4. **降采样 / 压缩**：降低音频码率与声道数，减少单声部资源占用与解码压力。
