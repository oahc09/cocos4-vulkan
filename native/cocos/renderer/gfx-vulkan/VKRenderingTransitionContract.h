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

// Single-image pre-render/post-render explicit state and dependency model
// (M5-F). Builds two legacy edges (before->during, during->after) that a
// future adapter can hand to the existing VulkanBarrierEncoding entry.
// Pure CPU value model: no Vulkan calls, no allocations, no runtime wiring.
// VkImage is a borrowed handle value; this class does not own the resource,
// extend GPU lifetimes, or prove the real current layout.

#pragma once

#include <initializer_list>

#include <vulkan/vulkan_core.h>

namespace cc {
namespace gfx {

class VKRenderingTransitionContract final {
public:
    struct Use {
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkPipelineStageFlags stages = 0;
        VkAccessFlags accesses = 0;
    };
    struct Input {
        VkImage image = VK_NULL_HANDLE;
        VkImageSubresourceRange range = {};
        Use before;
        Use during;
        Use after;
        VkDependencyFlags preFlags = 0;
        VkDependencyFlags postFlags = 0;
    };
    struct Edge {
        VkPipelineStageFlags srcStages = 0;
        VkPipelineStageFlags dstStages = 0;
        VkDependencyFlags flags = 0;
        VkImageMemoryBarrier barrier = {};
    };

    VKRenderingTransitionContract() { reset_(); }
    ~VKRenderingTransitionContract() = default;
    VKRenderingTransitionContract(const VKRenderingTransitionContract &) = delete;
    VKRenderingTransitionContract &operator=(const VKRenderingTransitionContract &) = delete;
    VKRenderingTransitionContract(VKRenderingTransitionContract &&) = delete;
    VKRenderingTransitionContract &operator=(VKRenderingTransitionContract &&) = delete;

    // Narrow shape checks only (NOT a Vulkan VUID validator):
    //  - image non-null; aspect exactly COLOR, DEPTH, STENCIL or
    //    DEPTH|STENCIL (0/unknown/mixed COLOR+D/S rejected; plane aspects
    //    unsupported in this version); level/layer counts non-zero
    //    (VK_REMAINING_* allowed; real bounds checked by device adapter).
    //  - all three stage masks non-zero (legacy encoding compatibility).
    //  - transition destinations during/after must not be
    //    UNDEFINED/PREINITIALIZED; during not PRESENT (after PRESENT is
    //    allowed). before UNDEFINED is allowed (discard-content intent) and
    //    never rewrites before.stages/accesses.
    // Both edges are always produced; a dependency is never skipped on
    // equal layouts, and TOP/BOTTOM stages are never guessed or narrowed.
    // Queue families are fixed to IGNORED (single-queue scope; no
    // ownership transfer).
    bool build(const Input &input) {
        if (input.image == VK_NULL_HANDLE) {
            reset_();
            return false;
        }
        if (!isValidAspect_(input.range.aspectMask)) {
            reset_();
            return false;
        }
        if (input.range.levelCount == 0 || input.range.layerCount == 0) {
            reset_();
            return false;
        }
        if (input.before.stages == 0 || input.during.stages == 0 || input.after.stages == 0) {
            reset_();
            return false;
        }
        if (!isTransitionDestination_(input.during.layout) || input.during.layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
            reset_();
            return false;
        }
        if (!isTransitionDestination_(input.after.layout)) {
            reset_();
            return false;
        }

        // snapshot all inputs BEFORE clearing own state (self-input safe:
        // build(c.input()) reads this->input_ only before reset)
        const Input snap = input;
        reset_();

        input_ = snap;

        pre_.srcStages = snap.before.stages;
        pre_.dstStages = snap.during.stages;
        pre_.flags = snap.preFlags;
        pre_.barrier.srcAccessMask = snap.before.accesses;
        pre_.barrier.dstAccessMask = snap.during.accesses;
        pre_.barrier.oldLayout = snap.before.layout;
        pre_.barrier.newLayout = snap.during.layout;

        post_.srcStages = snap.during.stages;
        post_.dstStages = snap.after.stages;
        post_.flags = snap.postFlags;
        post_.barrier.srcAccessMask = snap.during.accesses;
        post_.barrier.dstAccessMask = snap.after.accesses;
        post_.barrier.oldLayout = snap.during.layout;
        post_.barrier.newLayout = snap.after.layout;

        for (Edge *e : {&pre_, &post_}) {
            e->barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            e->barrier.pNext = nullptr;
            e->barrier.image = snap.image;
            e->barrier.subresourceRange = snap.range;
            e->barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            e->barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        }
        ready_ = true;
        return true;
    }

    void reset() { reset_(); }
    bool ready() const { return ready_; }
    const Input &input() const { return input_; }
    const Edge &pre() const { return pre_; }
    const Edge &post() const { return post_; }

private:
    static bool isValidAspect_(VkImageAspectFlags aspect) {
        // this version: exactly COLOR, DEPTH, STENCIL, or DEPTH|STENCIL
        const VkImageAspectFlags legal =
            VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        if (aspect == 0 || (aspect & ~legal) != 0) return false;
        const bool hasColor = (aspect & VK_IMAGE_ASPECT_COLOR_BIT) != 0;
        const bool hasDepthStencil =
            (aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
        return !(hasColor && hasDepthStencil); // mixed COLOR + D/S rejected
    }
    static bool isTransitionDestination_(VkImageLayout layout) {
        return layout != VK_IMAGE_LAYOUT_UNDEFINED &&
               layout != VK_IMAGE_LAYOUT_PREINITIALIZED;
    }

    void reset_() {
        ready_ = false;
        input_ = {};
        pre_ = {};
        post_ = {};
        for (Edge *e : {&pre_, &post_}) {
            e->barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            e->barrier.pNext = nullptr;
            e->barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            e->barrier.newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            e->barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            e->barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        }
    }

    bool ready_ = false;
    Input input_{};
    Edge pre_{};
    Edge post_{};
};

} // namespace gfx
} // namespace cc
