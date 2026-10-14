#!/usr/bin/env python3
from pathlib import Path
import argparse
import hashlib
import json
parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
source = Path(__file__).resolve().parents[3] / 'native/cocos/renderer/gfx-vulkan/VKGPUObjects.h'
text = source.read_text()
start = text.index('class CCVKGPUFencePool final')
end = text.index('\n};', start) + 3
body = text[start:end]
fixture = Path(__file__).with_name('fence_pool_tests.cpp.in').read_text()
args.output.write_text(fixture.replace('// @PRODUCTION_CLASS@', body))
args.output.with_suffix('.json').write_text(json.dumps({'source': 'VKGPUObjects.h', 'class': 'CCVKGPUFencePool', 'sha256': hashlib.sha256(body.encode()).hexdigest()}, indent=2) + '\n')
