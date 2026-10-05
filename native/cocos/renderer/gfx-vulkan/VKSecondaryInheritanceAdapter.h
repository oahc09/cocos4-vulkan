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

// Secondary command-buffer begin/inheritance adapter (M5-I).
// Builds a begin info + inheritance info pair from a VKRenderingContract.
// Pure CPU: no Vulkan calls; handles are borrowed values. The owner must
// stay alive until vkBeginCommandBuffer returns. Narrow checks only - not a
// VUID set, not proof a driver consumes dynamic inheritance.

#pragma once

#include <vulkan/vulkan_core.h>

#include "VKRenderingContract.h"

namespace cc {
namespace gfx {

class VKSecondaryInheritanceAdapter final {
public:
    enum class Mode { Classic,
                      Dynamic };

    VKSecondaryInheritanceAdapter() { reset_(); }
    ~VKSecondaryInheritanceAdapter() = default;
    VKSecondaryInheritanceAdapter(const VKSecondaryInheritanceAdapter &) = delete;
    VKSecondaryInheritanceAdapter &operator=(const VKSecondaryInheritanceAdapter &) = delete;
    VKSecondaryInheritanceAdapter(VKSecondaryInheritanceAdapter &&) = delete;
    VKSecondaryInheritanceAdapter &operator=(VKSecondaryInheritanceAdapter &&) = delete;

    //  source: begin info template. Must have the correct sType and a NULL
    //    pNext (unknown begin extensions are refused). pInheritanceInfo must
    //    be NULL or exactly &base (a foreign inheritance pointer is refused).
    //  base: inheritance template. Correct sType and NULL pNext required
    //    (unknown inheritance chains refused). Classic: renderPass must be
    //    non-null; every field is copied (RP/subpass/FBO/query fields); sig
    //    and primary flags are ignored (invalid sig never rejects Classic);
    //    no rendering node is attached.
    //  Dynamic: sig must be accepted by the owned contract. inheritance
    //    pNext points at the owned secondaryRenderingInfo; renderPass/FBO
    //    are NULL, subpass 0; base query fields are preserved. Formats/
    //    samples/viewMask/flags come from the owned contract;
    //    SECONDARY_CONTENTS is removed from primary flags; SUSPEND/RESUME
    //    are preserved.
    //  Output begin: flags |= RENDER_PASS_CONTINUE_BIT,
    //    pInheritanceInfo -> owned inheritance; other source fields kept as
    //    the narrow contract above. Self-input (inputs aliasing own state)
    //    is safe: scalars are snapshotted before reset. Reusing this
    //    adapter's own dynamic inheritance/base as a new input is rejected
    //    as an existing chain (same rule as the pipeline composer).
    bool build(Mode mode, const VkCommandBufferBeginInfo &source,
               const VkCommandBufferInheritanceInfo &base, const VKRenderingSignature *sig,
               VkRenderingFlags intendedPrimaryFlags) {
        if (source.sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO) {
            reset_();
            return false;
        }
        if (source.pNext != nullptr) {
            reset_();
            return false;
        }
        if (source.pInheritanceInfo != nullptr && source.pInheritanceInfo != &base) {
            reset_();
            return false;
        }
        if (base.sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO) {
            reset_();
            return false;
        }
        if (base.pNext != nullptr) {
            reset_();
            return false;
        }

        // narrow validation first: mode, Classic RP, Dynamic non-null sig.
        // No signature copy happens before these (Classic must not read or
        // copy the signature at all - zero allocations for an ignored input).
        const bool isClassic = mode == Mode::Classic;
        const bool isDynamic = mode == Mode::Dynamic;
        if (!isClassic && !isDynamic) {
            reset_();
            return false;
        }
        if (isClassic && base.renderPass == VK_NULL_HANDLE) {
            reset_();
            return false;
        }
        if (isDynamic && sig == nullptr) {
            reset_();
            return false;
        }

        // snapshot all inputs before clearing own state (self-input safe:
        // sig may point at the own contract signature). The signature copy
        // itself may throw bad_alloc - on any exception we fully reset and
        // rethrow (no stale ready/chain state survives).
        VKRenderingSignature sigSnap;
        try {
            // snapshot base/source scalars first (cheap, no allocation)
            const VkCommandBufferUsageFlags srcFlags = source.flags;
            const VkRenderPass baseRP = base.renderPass;
            const uint32_t baseSubpass = base.subpass;
            const VkFramebuffer baseFBO = base.framebuffer;
            const VkBool32 baseOcclusion = base.occlusionQueryEnable;
            const VkQueryControlFlags baseQueryFlags = base.queryFlags;
            const VkQueryPipelineStatisticFlags baseStats = base.pipelineStatistics;
            if (isDynamic) sigSnap = *sig; // the only copy; Classic never copies

            reset_();

            if (isClassic) {
                begin_.flags = srcFlags | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
                begin_.pInheritanceInfo = &inheritance_;
                inheritance_.renderPass = baseRP;
                inheritance_.subpass = baseSubpass;
                inheritance_.framebuffer = baseFBO;
                inheritance_.occlusionQueryEnable = baseOcclusion;
                inheritance_.queryFlags = baseQueryFlags;
                inheritance_.pipelineStatistics = baseStats;
            } else {
                if (!rendering_.build(sigSnap, intendedPrimaryFlags)) {
                    reset_();
                    return false;
                }
                begin_.flags = srcFlags | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
                begin_.pInheritanceInfo = &inheritance_;
                inheritance_.pNext = &rendering_.secondaryRenderingInfo();
                inheritance_.renderPass = VK_NULL_HANDLE;
                inheritance_.subpass = 0;
                inheritance_.framebuffer = VK_NULL_HANDLE;
                inheritance_.occlusionQueryEnable = baseOcclusion;
                inheritance_.queryFlags = baseQueryFlags;
                inheritance_.pipelineStatistics = baseStats;
            }
        } catch (...) {
            reset_();
            throw; // bad_alloc propagates; no stale state
        }
        ready_ = true;
        return true;
    }

    void reset() { reset_(); }
    bool ready() const { return ready_; }
    const VkCommandBufferBeginInfo &beginInfo() const { return begin_; }
    const VkCommandBufferInheritanceInfo &inheritanceInfo() const { return inheritance_; }
    const VKRenderingContract &rendering() const { return rendering_; }

private:
    void reset_() {
        ready_ = false;
        begin_ = {};
        begin_.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_.pNext = nullptr;
        begin_.pInheritanceInfo = nullptr;
        inheritance_ = {};
        inheritance_.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
        inheritance_.pNext = nullptr;
        inheritance_.renderPass = VK_NULL_HANDLE;
        inheritance_.framebuffer = VK_NULL_HANDLE;
        rendering_.reset();
    }

    bool ready_ = false;
    VkCommandBufferBeginInfo begin_{};
    VkCommandBufferInheritanceInfo inheritance_{};
    VKRenderingContract rendering_{};
};

} // namespace gfx
} // namespace cc
