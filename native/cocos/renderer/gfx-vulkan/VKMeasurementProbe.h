/****************************************************************************
 Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

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

#include "VKStd.h"
#include <cstdint>

namespace cc::gfx {
enum class VulkanMeasurementKind { Present,
                                   GraphicsFactory,
                                   ShaderCompile,
                                   BackendInit,
                                   InstanceCreation,
                                   DeviceCreation,
                                   BufferVisibility };
// Stack-owned metadata for the synchronous optional callback. No Vulkan calls.
struct VulkanVisibilityMeasurement {
    uint32_t checkedBuffers = 0, checkedImages = 0;
    uint32_t memoryBarriers = 0, bufferBarriers = 0, imageBarriers = 0;
};
using VulkanMeasurementCallback = void (*)(VulkanMeasurementKind, bool entering, const void *owner) noexcept;

// Optional diagnostics only: null by default, no clock reads or Vulkan calls.
// The caller owns callback lifetime and restores it after rendering has stopped.
CC_VULKAN_API VulkanMeasurementCallback setVulkanMeasurementCallback(VulkanMeasurementCallback callback) noexcept;
CC_VULKAN_API VulkanMeasurementCallback getVulkanMeasurementCallback() noexcept;

// Explicit diagnostic creation policy; never changes an existing swapchain.
// Keep is the default. Restore after rendering stops, like the callback above.
enum class VulkanMeasurementPresentPolicy { Keep,
                                            Immediate };
CC_VULKAN_API VulkanMeasurementPresentPolicy setVulkanMeasurementPresentPolicy(VulkanMeasurementPresentPolicy policy) noexcept;
CC_VULKAN_API VulkanMeasurementPresentPolicy getVulkanMeasurementPresentPolicy() noexcept;

class VulkanMeasurementScope final {
public:
    VulkanMeasurementScope(VulkanMeasurementKind kind, const void *owner, bool enabled = true) noexcept
    : callback_(enabled ? getVulkanMeasurementCallback() : nullptr), kind_(kind), owner_(owner) {
        if (callback_) callback_(kind_, true, owner_);
    }
    ~VulkanMeasurementScope() {
        if (callback_) callback_(kind_, false, owner_);
    }
    VulkanMeasurementScope(const VulkanMeasurementScope &) = delete;
    VulkanMeasurementScope &operator=(const VulkanMeasurementScope &) = delete;

private:
    VulkanMeasurementCallback callback_;
    VulkanMeasurementKind kind_;
    const void *owner_;
};
} // namespace cc::gfx
