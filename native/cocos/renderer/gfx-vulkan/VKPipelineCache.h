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

#pragma once

#include "VKGPUObjects.h"
#include "VKPipelineCachePersistence.h"
#include "base/Ptr.h"
#include "base/RefCounted.h"
#include "base/std/container/string.h"
#include "base/std/container/unordered_map.h"
#include "base/std/container/vector.h"

namespace cc::gfx {

#ifndef CC_VULKAN_PIPELINE_CACHE
    #define CC_VULKAN_PIPELINE_CACHE 0
#endif

class CCVKPipelineCache : public RefCounted {
public:
    CCVKPipelineCache();
    ~CCVKPipelineCache() override;

    // Internal backend API; an explicit path allows an isolated cache domain.
    void init(VkDevice dev, const VkPhysicalDeviceProperties &properties,
              bool persistent = CC_USE_PIPELINE_CACHE || CC_VULKAN_PIPELINE_CACHE,
              const ccstd::string &path = {});
    void loadCache();
    void saveCache();
    void checkpoint();

    void setDirty();
    VkPipelineCache getHandle() const;
    bool isDirty() const { return _dirty; }
    bool loadedFromDisk() const { return _loadedFromDisk; }

private:
    VkDevice _device = VK_NULL_HANDLE;
    VkPipelineCache _pipelineCache = VK_NULL_HANDLE;
    ccstd::string _savePath;
    bool _dirty = false;
    bool _persistent = false;
    bool _loadedFromDisk = false;
    VulkanPipelineCachePersistence::Identity _identity;
    std::chrono::steady_clock::time_point _lastCheckpoint;
};

} // namespace cc::gfx
