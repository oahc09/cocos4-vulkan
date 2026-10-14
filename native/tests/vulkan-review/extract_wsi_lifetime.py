#!/usr/bin/env python3
"""Compile actual production control flow with injected driver results, no GPU."""
from pathlib import Path
import argparse
import hashlib
import json

parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--manifest', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[3]
src = root / 'native/cocos/renderer/gfx-vulkan'
methods = {
    'VKSwapchain.cpp': [
        'VkResult CCVKGPUWSISurface::retireGenerations(',
        'VkResult CCVKGPUWSIGeneration::waitForPresentCompletion(',
        'void CCVKGPUWSIGeneration::releaseVulkanObjects()',
        'CCVKGPUWSIGeneration::~CCVKGPUWSIGeneration()',
        'void CCVKSwapchain::doDestroy()',
        'VkResult CCVKSwapchain::destroySwapchain(',
        'void CCVKSwapchain::doDestroySurface()',
    ],
    'VKDevice.cpp': [
        'void CCVKDevice::acquire(',
        'bool CCVKDevice::hasPendingWSIAbort()',
        'VkResult CCVKDevice::retryPendingWSIAborts()',
        'VkResult CCVKDevice::abortAcquiredSwapchain(',
        'VkResult CCVKDevice::retireSwapchain(',
        'VkResult CCVKDevice::retireDeferredSwapchains()',
        'VkResult CCVKDevice::waitAllFences(',
    ],
    'VKQueue.cpp': [
        'void CCVKQueue::submit(',
        'VkResult CCVKQueue::submitChecked(',
    ],
}
body, manifest = [], []
for name, signatures in methods.items():
    source = (src / name).read_text()
    for signature in signatures:
        start = source.index(signature)
        index = source.index('{', start) + 1
        depth = 1
        while depth:
            depth += (source[index] == '{') - (source[index] == '}')
            index += 1
        method = source[start:index]
        body.append(method)
        manifest.append({'source': name, 'signature': signature, 'sha256': hashlib.sha256(method.encode()).hexdigest()})
fixture = Path(__file__).with_name('wsi_lifetime_tests.cpp.in').read_text()
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(fixture.replace('// @PRODUCTION_METHODS@', '\n\n'.join(body)))
if args.manifest:
    args.manifest.write_text(json.dumps(manifest, indent=2) + '\n')
