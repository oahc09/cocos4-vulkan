/****************************************************************************
 Copyright (c) 2023 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do so,
 subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
****************************************************************************/

#include "VKPipelineCache.h"

#include "gfx-base/GFXUtil.h"

namespace cc::gfx {

CCVKPipelineCache::CCVKPipelineCache() {
    _savePath = getPipelineCacheFolder() + "/pipeline_cache_vk.bin";
}

CCVKPipelineCache::~CCVKPipelineCache() {
    if (_pipelineCache != VK_NULL_HANDLE) {
        saveCache();
        vkDestroyPipelineCache(_device, _pipelineCache, nullptr);
    }
}

void CCVKPipelineCache::init(VkDevice dev, const VkPhysicalDeviceProperties &properties,
                             bool persistent, const ccstd::string &path) {
    CC_ASSERT(_pipelineCache == VK_NULL_HANDLE);
    if (_pipelineCache != VK_NULL_HANDLE) return;
    _device = dev;
    _identity = VulkanPipelineCachePersistence::identity(properties);
    _persistent = persistent;
    if (!path.empty()) _savePath = path;
    _lastCheckpoint = std::chrono::steady_clock::now();
    loadCache();
}

void CCVKPipelineCache::loadCache() {
    if (_pipelineCache != VK_NULL_HANDLE) return;
    VulkanPipelineCachePersistence::Bytes data;
    _loadedFromDisk = false;
    if (_persistent) {
        try {
            VulkanPipelineCachePersistence::read(std::filesystem::u8path(_savePath.c_str()), _identity, data);
        } catch (...) {
            data.clear(); // optional cache failure must not abort device bootstrap
        }
    }
    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.initialDataSize = data.size();
    info.pInitialData = data.empty() ? nullptr : data.data();
    auto result = vkCreatePipelineCache(_device, &info, nullptr, &_pipelineCache);
    if (result != VK_SUCCESS && !data.empty()) {
        // Even validated optional data can be rejected by the implementation.
        // A cold-cache retry does not turn a true device/allocation failure into success.
        _pipelineCache = VK_NULL_HANDLE;
        info.initialDataSize = 0;
        info.pInitialData = nullptr;
        result = vkCreatePipelineCache(_device, &info, nullptr, &_pipelineCache);
        CC_LOG_WARNING("Vulkan pipeline cache rejected; retried empty cache.");
    } else {
        _loadedFromDisk = result == VK_SUCCESS && !data.empty();
    }
    if (result != VK_SUCCESS) _pipelineCache = VK_NULL_HANDLE;
    VK_CHECK(result);
    if (_persistent) CC_LOG_INFO("Vulkan pipeline cache: %s", _loadedFromDisk ? "loaded validated data" : "cold cache");
}

void CCVKPipelineCache::saveCache() {
    if (!_persistent || !_dirty || !_pipelineCache) return;
    VulkanPipelineCachePersistence::Bytes data;
    if (!VulkanPipelineCachePersistence::retrieve(_device, _pipelineCache, vkGetPipelineCacheData, _identity, data)) {
        CC_LOG_WARNING("Vulkan pipeline cache retrieval failed; previous file preserved.");
        return;
    }
    try {
        if (VulkanPipelineCachePersistence::write(std::filesystem::u8path(_savePath.c_str()), _identity, data)) {
            _dirty = false;
        } else {
            CC_LOG_WARNING("Vulkan pipeline cache save failed; previous file preserved.");
        }
    } catch (...) {
        CC_LOG_WARNING("Vulkan pipeline cache path failed; previous file preserved.");
    }
}

void CCVKPipelineCache::checkpoint() {
    if (!_persistent || !_dirty) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - _lastCheckpoint < std::chrono::seconds(30)) return;
    // The rendering thread owns checkpoint/create operations. This is a serial
    // checkpoint, not an asynchronous or concurrently mutable cache API.
    _lastCheckpoint = now;
    saveCache();
}

void CCVKPipelineCache::setDirty() { _dirty = true; }
VkPipelineCache CCVKPipelineCache::getHandle() const { return _pipelineCache; }

} // namespace cc::gfx
