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
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.
****************************************************************************/

// Offline owning pipeline/secondary rendering contract model (M5-C, revised).
// Pure CPU data assembly -- no Vulkan objects, no loader, no engine.
// Builds VkPipelineRenderingCreateInfo + VkCommandBufferInheritanceRenderingInfo
// + VkCommandBufferInheritanceInfo from a VKRenderingSignature snapshot.
// NOT a device-support or pipeline-creatability check.

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "VKRenderingSignature.h"

namespace cc {
namespace gfx {

class VKRenderingContract final {
public:
    VKRenderingContract() { reset_(); }
    ~VKRenderingContract() = default; // no Vulkan calls
    VKRenderingContract(const VKRenderingContract &) = delete;
    VKRenderingContract &operator=(const VKRenderingContract &) = delete;
    VKRenderingContract(VKRenderingContract &&) = delete;
    VKRenderingContract &operator=(VKRenderingContract &&) = delete;

    // Build from a signature snapshot. Safe for self-input (e.g.
    // build(c.signature(), flags)). Can be called repeatedly (rebuild).
    // primaryFlags: intended VkRenderingInfo::flags for the primary render;
    // the secondary inheritance node clears SECONDARY_CONTENTS but preserves
    // all other flags (SUSPEND/RESUME). Returns false on invalid input
    // (bad samples, mismatched depth/stencil) and fully clears state.
    bool build(const VKRenderingSignature &signature, VkRenderingFlags primaryFlags = 0) {
        // validate scalars BEFORE touching own state
        switch (signature.samples) {
            case VK_SAMPLE_COUNT_1_BIT:
            case VK_SAMPLE_COUNT_2_BIT:
            case VK_SAMPLE_COUNT_4_BIT:
            case VK_SAMPLE_COUNT_8_BIT:
            case VK_SAMPLE_COUNT_16_BIT:
            case VK_SAMPLE_COUNT_32_BIT:
            case VK_SAMPLE_COUNT_64_BIT:
                break;
            default:
                reset_();
                return false; // 0 or combined bits
        }
        // VUID-VkCommandBufferInheritanceRenderingInfo-depthAttachmentFormat-06200
        if (signature.depthFormat != VK_FORMAT_UNDEFINED &&
            signature.stencilFormat != VK_FORMAT_UNDEFINED &&
            signature.depthFormat != signature.stencilFormat) {
            reset_();
            return false;
        }
        if (static_cast<uint64_t>(signature.colorFormats.size()) > UINT32_MAX) {
            reset_();
            return false;
        }

        // snapshot the input BEFORE clearing own state (self-input safe)
        VKRenderingSignature snapshot;
        try {
            snapshot = signature;
        } catch (...) {
            reset_();
            throw; // allocation failure; do not fake success
        }

        reset_();
        signature_ = std::move(snapshot);

        const uint32_t colorCount = static_cast<uint32_t>(signature_.colorFormats.size());
        const VkFormat *colorPtr = colorCount ? signature_.colorFormats.data() : nullptr;

        // pipeline rendering node
        pipeline_.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        pipeline_.pNext = nullptr; // composer comes later (M5+)
        pipeline_.viewMask = signature_.viewMask;
        pipeline_.colorAttachmentCount = colorCount;
        pipeline_.pColorAttachmentFormats = colorPtr;
        pipeline_.depthAttachmentFormat = signature_.depthFormat;
        pipeline_.stencilAttachmentFormat = signature_.stencilFormat;

        // secondary inheritance rendering node
        secondaryRendering_.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO;
        secondaryRendering_.pNext = nullptr;
        secondaryRendering_.flags = primaryFlags & ~VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
        secondaryRendering_.viewMask = signature_.viewMask;
        secondaryRendering_.colorAttachmentCount = colorCount;
        secondaryRendering_.pColorAttachmentFormats = colorPtr;
        secondaryRendering_.depthAttachmentFormat = signature_.depthFormat;
        secondaryRendering_.stencilAttachmentFormat = signature_.stencilFormat;
        secondaryRendering_.rasterizationSamples = signature_.samples;

        // base inheritance node
        baseInheritance_.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
        baseInheritance_.pNext = &secondaryRendering_;
        baseInheritance_.renderPass = VK_NULL_HANDLE;
        baseInheritance_.subpass = 0;
        baseInheritance_.framebuffer = VK_NULL_HANDLE;
        baseInheritance_.occlusionQueryEnable = VK_FALSE;
        baseInheritance_.queryFlags = 0;
        baseInheritance_.pipelineStatistics = 0;

        ready_ = true;
        return true;
    }

    bool ready() const { return ready_; }
    void reset() { reset_(); }
    const VKRenderingSignature &signature() const { return signature_; }
    const VkPipelineRenderingCreateInfo &pipelineInfo() const { return pipeline_; }
    const VkCommandBufferInheritanceRenderingInfo &secondaryRenderingInfo() const { return secondaryRendering_; }
    const VkCommandBufferInheritanceInfo &baseInheritanceInfo() const { return baseInheritance_; }
    VkSampleCountFlagBits samples() const { return signature_.samples; }

private:
    void reset_() {
        ready_ = false;
        signature_ = {};
        pipeline_ = {};
        pipeline_.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        secondaryRendering_ = {};
        secondaryRendering_.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO;
        baseInheritance_ = {};
        baseInheritance_.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
        // pNext/pointers/counters are zero from {} above
    }

    bool ready_ = false;
    VKRenderingSignature signature_{};
    VkPipelineRenderingCreateInfo pipeline_{};
    VkCommandBufferInheritanceRenderingInfo secondaryRendering_{};
    VkCommandBufferInheritanceInfo baseInheritance_{};
};

} // namespace gfx
} // namespace cc
