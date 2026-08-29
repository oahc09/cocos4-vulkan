#!/usr/bin/env node
/**
 * Cocos 引擎规格标准知识库 —— 索引构建工具
 *
 * 功能：
 *   1. 为所有条目补全/统一 YAML frontmatter 元数据（id/title/category/layer/domain/module/breakable/tags/sources）
 *   2. 生成机器可读索引 index.json 与 index.csv，供 RAG / 向量库直接导入
 *
 * 用法:  node build-index.js
 * 说明:  幂等 —— 可重复执行；已有 frontmatter 的条目会保留原有 sources 与 updated。
 */

const fs = require('fs');
const path = require('path');

const KB_DIR = __dirname;
const ENGINE = 'Cocos Creator 4.x';
const DEFAULT_UPDATED = '2026-08-29';

/** 分层说明: L1=硬件能力(不可改) L2=软件常量(改引擎可调) L3=性能/平台约束(无硬上限) */
const META = {
  '00-integrated-overview.md': { id: 'kb-integrated-overview', title: '引擎规格标准综合视图', category: '综合', layer: 'L1', domain: 'meta', module: 'overview', breakable: false, tags: ['overview', 'integration', 'spec', 'index'] },
  '01-device-caps.md': { id: 'kb-device-caps', title: 'DeviceCaps 硬件能力表', category: '全局硬件能力', layer: 'L1', domain: 'rendering', module: 'gfx', breakable: false, tags: ['devicecaps', 'hardware', 'limits'] },
  '02-skeleton.md': { id: 'kb-skeleton', title: '骨骼动画 Skeleton', category: '3D 渲染模块', layer: 'L2', domain: 'rendering', module: 'skeleton', breakable: true, tags: ['skeleton', 'skinning', 'joint', 'CC_JOINT_UNIFORM_CAPACITY'] },
  '03-blendshape.md': { id: 'kb-blendshape', title: 'BlendShape 形变动画', category: '3D 渲染模块', layer: 'L2', domain: 'rendering', module: 'morph', breakable: true, tags: ['blendshape', 'morph', 'MAX_MORPH_TARGET_COUNT'] },
  '04-light.md': { id: 'kb-light', title: '光源与光照 Light', category: '3D 渲染模块', layer: 'L2', domain: 'rendering', module: 'light', breakable: true, tags: ['light', 'forward', 'deferred', 'LIGHTS_PER_PASS'] },
  '05-uniform-ubo.md': { id: 'kb-uniform-ubo', title: 'Uniform 与 UBO 容量', category: '渲染与着色器', layer: 'L1', domain: 'rendering', module: 'ubo', breakable: false, tags: ['uniform', 'ubo', 'maxUniformBlockSize'] },
  '06-vertex-index.md': { id: 'kb-vertex-index', title: '顶点与索引缓冲', category: '渲染与着色器', layer: 'L1', domain: 'rendering', module: 'geometry', breakable: false, tags: ['vertex', 'index', 'uint16', 'uint32'] },
  '07-texture-material-pass.md': { id: 'kb-texture-material-pass', title: '纹理 / 材质 / Pass / RT', category: '渲染与着色器', layer: 'L2', domain: 'rendering', module: 'material', breakable: true, tags: ['texture', 'material', 'pass', 'MAX_PASS_COUNT'] },
  '08-particle.md': { id: 'kb-particle', title: '粒子系统 Particle', category: '3D 渲染模块', layer: 'L3', domain: 'rendering', module: 'particle', breakable: true, tags: ['particle', 'capacity', 'emitter'] },
  '09-terrain.md': { id: 'kb-terrain', title: '地形 Terrain', category: '3D 渲染模块', layer: 'L2', domain: 'rendering', module: 'terrain', breakable: true, tags: ['terrain', 'TERRAIN_MAX_LAYER_COUNT'] },
  '10-animation.md': { id: 'kb-animation', title: '动画层 Animation', category: '动画与骨架', layer: 'L2', domain: 'animation', module: 'animation', breakable: true, tags: ['animation', 'MAX_ANIMATION_LAYER'] },
  '11-layers.md': { id: 'kb-layers', title: '节点层级 Layers', category: '场景与系统', layer: 'L2', domain: 'scene', module: 'layers', breakable: false, tags: ['layers', 'bitmask', 'uint32'] },
  '12-shadow.md': { id: 'kb-shadow', title: '阴影 Shadow', category: '3D 渲染模块', layer: 'L2', domain: 'rendering', module: 'shadow', breakable: true, tags: ['shadow', 'CSM', 'maxReceived'] },
  '13-ui-mask.md': { id: 'kb-ui-mask', title: 'UI 遮罩嵌套', category: '2D 与 UI', layer: 'L1', domain: 'ui', module: 'mask', breakable: false, tags: ['mask', 'stencil', 'ui'] },
  '14-light-probe.md': { id: 'kb-light-probe', title: '光照探针 / GI', category: '3D 渲染模块', layer: 'L2', domain: 'rendering', module: 'gi', breakable: true, tags: ['gi', 'lightprobe', 'giSamples'] },
  '15-varyings.md': { id: 'kb-varyings', title: 'Shader Varyings 插值器', category: '渲染与着色器', layer: 'L1', domain: 'rendering', module: 'varyings', breakable: false, tags: ['varyings', 'MAX_VARYING_VECTORS', 'shader'] },
  '16-compute.md': { id: 'kb-compute', title: '计算着色器 Compute', category: '渲染与着色器', layer: 'L1', domain: 'rendering', module: 'compute', breakable: false, tags: ['compute', 'workgroup', 'shared-memory'] },
  '17-input-touch.md': { id: 'kb-input-touch', title: '输入与触摸', category: '场景与系统', layer: 'L2', domain: 'input', module: 'touch', breakable: false, tags: ['input', 'touch', 'MAX_TOUCHES'] },
  '18-label-pool.md': { id: 'kb-label-pool', title: 'Label 画布池', category: '2D 与 UI', layer: 'L2', domain: 'ui', module: 'label', breakable: true, tags: ['label', 'pool', 'MAX_LABEL_CANVAS_POOL_SIZE'] },
  '19-camera.md': { id: 'kb-camera', title: '摄像机 Camera', category: '场景与系统', layer: 'L3', domain: 'scene', module: 'camera', breakable: true, tags: ['camera', 'render'] },
  '20-audio.md': { id: 'kb-audio', title: '音频 Audio', category: '场景与系统', layer: 'L3', domain: 'audio', module: 'audio', breakable: false, tags: ['audio', 'voice', 'platform'] },
  '21-asset-loading.md': { id: 'kb-asset-loading', title: '资源加载并发', category: '资源与管线', layer: 'L2', domain: 'asset', module: 'asset-manager', breakable: true, tags: ['asset', 'loading', 'maxConcurrency'] },
  '22-spine-dragonbones.md': { id: 'kb-spine-dragonbones', title: 'Spine / DragonBones 骨骼动画', category: '2D 与 UI', layer: 'L2', domain: 'animation', module: 'spine', breakable: true, tags: ['spine', 'dragonbones', 'cache-mode', '2d'] },
  '23-tiledmap.md': { id: 'kb-tiledmap', title: 'TiledMap 瓦片地图', category: '2D 与 UI', layer: 'L3', domain: 'ui', module: 'tiledmap', breakable: true, tags: ['tiledmap', 'tmx', 'layer'] },
  '24-post-process.md': { id: 'kb-post-process', title: '后处理 PostProcess', category: '渲染与着色器', layer: 'L2', domain: 'rendering', module: 'postprocess', breakable: true, tags: ['postprocess', 'bloom', 'shadingScale'] },
  '25-gpu-instancing.md': { id: 'kb-gpu-instancing', title: 'GPU Instancing 实例化渲染', category: '渲染与批处理', layer: 'L2', domain: 'rendering', module: 'instancing', breakable: true, tags: ['instancing', 'batch', 'MAX_CAPACITY', 'drawcall'] },
  '26-physics.md': { id: 'kb-physics', title: '物理系统 Physics（3D/2D）', category: '场景与系统', layer: 'L3', domain: 'physics', module: 'physics', breakable: true, tags: ['physics', 'fixedTimeStep', 'maxSubSteps', 'collisionMatrix'] },
  '27-reflection-probe.md': { id: 'kb-reflection-probe', title: '反射探针 ReflectionProbe', category: '3D 渲染模块', layer: 'L3', domain: 'rendering', module: 'reflection-probe', breakable: true, tags: ['reflection-probe', 'cubemap', 'planar', 'bake'] },
  '28-dynamic-atlas.md': { id: 'kb-dynamic-atlas', title: '2D 动态合图 Dynamic Atlas', category: '2D 与 UI', layer: 'L2', domain: 'ui', module: 'dynamic-atlas', breakable: true, tags: ['dynamic-atlas', 'batch', 'drawcall', 'maxFrameSize'] },
  '29-video-webview.md': { id: 'kb-video-webview', title: '视频 VideoPlayer / WebView', category: '场景与系统', layer: 'L3', domain: 'system', module: 'video-webview', breakable: false, tags: ['video-player', 'webview', 'platform'] },
  '30-shader-variant.md': { id: 'kb-shader-variant', title: 'Shader 变体与 Program 库', category: '渲染与着色器', layer: 'L2', domain: 'rendering', module: 'shader-variant', breakable: true, tags: ['shader', 'variant', 'defines', 'macro', 'compile'] },
};

const FM_ORDER = ['id', 'title', 'category', 'engine', 'updated', 'layer', 'domain', 'module', 'breakable', 'tags', 'sources'];

function splitFrontmatter (text) {
  if (!text.startsWith('---\n')) return { fm: null, body: text };
  const end = text.indexOf('\n---\n', 4);
  if (end < 0) return { fm: null, body: text };
  const raw = text.slice(4, end);
  const body = text.slice(end + 5);
  const fm = {};
  for (const line of raw.split('\n')) {
    const m = line.match(/^([A-Za-z_][\w-]*):\s*(.*)$/);
    if (!m) continue;
    let v = m[2].trim();
    if (v.startsWith('[') && v.endsWith(']')) {
      v = v.slice(1, -1).split(',').map((s) => s.trim().replace(/^["']|["']$/g, '')).filter(Boolean);
    }
    fm[m[1]] = v;
  }
  return { fm, body };
}

function extractSources (body) {
  const re = /([A-Za-z0-9_\-./]+\.(?:ts|cpp|h|hpp|chunk|json)):(\d+)/g;
  const seen = new Set();
  let m;
  while ((m = re.exec(body)) !== null) seen.add(`${m[1]}:${m[2]}`);
  return [...seen];
}

/** 去重；剔除被完整路径覆盖的短路径（mesh-buffer.ts:257 让位于 cocos/2d/renderer/mesh-buffer.ts:257） */
function dedupeSources (list) {
  const seen = new Set();
  const all = list.filter((s) => {
    if (!s || seen.has(s)) return false;
    seen.add(s);
    return true;
  });
  return all.filter((s) => (s.includes('/') ? true : !all.some((o) => o !== s && o.endsWith(`/${s}`))));
}

function extractSummary (body) {
  // 优先取「## 概述」段；缺失时回退到首个标题下的正文
  let m = body.match(/##\s*概述\s*\n+([\s\S]*?)(?=\n##\s|\n---|\n$)/);
  if (!m) m = body.match(/^#[^\n]*\n+([\s\S]*?)(?=\n#+\s|\n$)/);
  if (!m) return '';
  return m[1].replace(/\s+/g, ' ').trim().slice(0, 160);
}

function renderFrontmatter (fields) {
  const lines = ['---'];
  for (const k of FM_ORDER) {
    if (!(k in fields)) continue;
    const v = fields[k];
    if (Array.isArray(v)) lines.push(v.length ? `${k}: [${v.join(', ')}]` : `${k}: []`);
    else lines.push(`${k}: ${v}`);
  }
  lines.push('---');
  return `${lines.join('\n')}\n`;
}

function csvEscape (v) {
  const s = Array.isArray(v) ? v.join('|') : String(v ?? '');
  return /[",\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
}

function main () {
  const files = fs.readdirSync(KB_DIR)
    .filter((f) => f.endsWith('.md') && f !== 'README.md' && f !== 'index.json')
    .sort();

  const index = [];
  let patched = 0;

  for (const file of files) {
    const full = path.join(KB_DIR, file);
    const original = fs.readFileSync(full, 'utf8');
    const { fm, body } = splitFrontmatter(original);
    const meta = META[file];

    if (!meta) {
      console.warn(`  ! 跳过（无元数据映射）: ${file}`);
      continue;
    }

    const fields = {
      id: meta.id,
      title: meta.title,
      category: meta.category,
      engine: (fm && fm.engine) || ENGINE,
      updated: (fm && fm.updated) || DEFAULT_UPDATED,
      layer: meta.layer,
      domain: meta.domain,
      module: meta.module,
      breakable: meta.breakable,
      tags: meta.tags,
      // 保留已有 sources，并补入正文中出现的引用，去重后截断
      sources: dedupeSources([
        ...((fm && Array.isArray(fm.sources)) ? fm.sources : []),
        ...extractSources(body),
      ]).slice(0, 12),
    };

    const next = renderFrontmatter(fields) + body.replace(/^\n+/, '\n');
    if (next !== original) {
      fs.writeFileSync(full, next, 'utf8');
      patched += 1;
    }

    index.push({
      ...fields,
      file,
      summary: extractSummary(body),
    });
  }

  // 写出机器可读索引
  const jsonPath = path.join(KB_DIR, 'index.json');
  fs.writeFileSync(jsonPath, `${JSON.stringify({
    name: 'cocos-engine-spec-kb',
    engine: ENGINE,
    generated: new Date().toISOString().slice(0, 10),
    entryCount: index.length,
    entries: index,
  }, null, 2)}\n`, 'utf8');

  const cols = ['id', 'file', 'title', 'category', 'layer', 'domain', 'module', 'breakable', 'tags', 'sources', 'summary'];
  const csv = [cols.join(','), ...index.map((e) => cols.map((c) => csvEscape(e[c])).join(','))].join('\n');
  fs.writeFileSync(path.join(KB_DIR, 'index.csv'), `${csv}\n`, 'utf8');

  console.log(`✔ 处理条目: ${index.length} 个，更新 frontmatter: ${patched} 个`);
  console.log(`✔ 已生成: index.json / index.csv`);

  const byLayer = index.reduce((acc, e) => { acc[e.layer] = (acc[e.layer] || 0) + 1; return acc; }, {});
  console.log(`  分层统计: ${JSON.stringify(byLayer)}`);
}

main();
