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
#include <array>
#include <stdexcept>
#include "base/Log.h"
#include "gfx-vulkan/VKUtils.h"

namespace cc::vulkan_test {
// Optional forward-first observation. It changes no Vulkan payload or policy.
// The resolve diagnostic's post-render barriers preserve the attachment layout;
// its copy barriers change layout and are intentionally outside this contract.
class RealProjectScopeObservation final {
public:
    explicit RealProjectScopeObservation(bool enabled) : enabled_(enabled) {
        if (!enabled_) return;
        if (active_ || !::vkCmdPipelineBarrier) throw std::runtime_error("post scope hook PFNs");
        active_ = this;
        barrier_ = ::vkCmdPipelineBarrier;
        barrier2_ = ::vkCmdPipelineBarrier2;
        barrier2KHR_ = ::vkCmdPipelineBarrier2KHR;
        ::vkCmdPipelineBarrier = legacy;
        if (barrier2_) ::vkCmdPipelineBarrier2 = core;
        if (barrier2KHR_) ::vkCmdPipelineBarrier2KHR = khr;
    }
    ~RealProjectScopeObservation() {
        if (!enabled_ || active_ != this) return;
        ::vkCmdPipelineBarrier = barrier_;
        ::vkCmdPipelineBarrier2 = barrier2_;
        ::vkCmdPipelineBarrier2KHR = barrier2KHR_;
        active_ = nullptr;
    }
    bool finish() noexcept {
        if (!enabled_) return true;
        const bool ok = posts_ == 4 && precise_ == 4 && mask_ == ((1U << count_) - 1) && count_ && !broad_ && !failures_;
        if (!ok) ++failures_;
        CC_LOG_INFO("project-resolve scopes: posts=%u precise=%u broad=%u failures=%u", posts_, precise_, broad_, failures_);
        return ok;
    }
    static bool enabled() noexcept { return active_ != nullptr; }
    static void expect(VkCommandBuffer cb, const VkImage *images, unsigned count) noexcept {
        auto *s = active_;
        if (!s) return;
        if (!cb || !images || !count || count > s->images_.size() || (s->count_ && s->mask_ != ((1U << s->count_) - 1))) {
            ++s->failures_;
            return;
        }
        s->cb_ = cb;
        s->count_ = count;
        s->mask_ = 0;
        for (unsigned i = 0; i < count; ++i) {
            s->images_[i] = images[i];
            if (!images[i]) ++s->failures_;
            for (unsigned j = 0; j < i; ++j)
                if (images[j] == images[i]) ++s->failures_;
        }
    }
    RealProjectScopeObservation(const RealProjectScopeObservation &) = delete;
    RealProjectScopeObservation &operator=(const RealProjectScopeObservation &) = delete;

private:
    void record(VkCommandBuffer cb, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                VkPipelineStageFlags2 src, VkPipelineStageFlags2 dst,
                VkAccessFlags2 srcAccess, VkAccessFlags2 dstAccess,
                const VkImageSubresourceRange &range, uint32_t srcQ, uint32_t dstQ) noexcept {
        if (oldLayout != newLayout || !(src & (VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT))) return;
        if (oldLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && oldLayout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) return;
        ++posts_;
        unsigned match = count_;
        for (unsigned i = 0; i < count_; ++i)
            if (images_[i] == image) match = i;
        const bool identity = cb == cb_ && match < count_ && !(mask_ & (1U << match));
        if (match < count_) mask_ |= 1U << match;
        const auto stage = oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ? VkPipelineStageFlags2(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT) : VkPipelineStageFlags2(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
        const auto access = oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ? VkAccessFlags2(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT) : VkAccessFlags2(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        const bool shape = image && range.levelCount == 1 && range.layerCount == 1 && !range.baseMipLevel && !range.baseArrayLayer && srcQ == VK_QUEUE_FAMILY_IGNORED && dstQ == VK_QUEUE_FAMILY_IGNORED;
        if (identity && dst == stage && dstAccess == access && shape && srcAccess)
            ++precise_;
        else
            ++failures_;
        if ((src | dst) & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT || (srcAccess | dstAccess) & (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)) ++broad_;
    }
    static VKAPI_ATTR void VKAPI_CALL legacy(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags flags,
                                             uint32_t m, const VkMemoryBarrier *mem, uint32_t b, const VkBufferMemoryBarrier *buf,
                                             uint32_t n, const VkImageMemoryBarrier *images) {
        auto *s = active_;
        if (!s || !s->barrier_) return;
        s->barrier_(cb, src, dst, flags, m, mem, b, buf, n, images);
        if (images)
            for (unsigned i = 0; i < n; ++i) {
                const auto &v = images[i];
                s->record(cb, v.image, v.oldLayout, v.newLayout, src, dst, v.srcAccessMask, v.dstAccessMask, v.subresourceRange, v.srcQueueFamilyIndex, v.dstQueueFamilyIndex);
            }
    }
    static void modern(VkCommandBuffer cb, const VkDependencyInfo *info) noexcept {
        auto *s = active_;
        if (!s || !info) return;
        if (info->pImageMemoryBarriers)
            for (unsigned i = 0; i < info->imageMemoryBarrierCount; ++i) {
                const auto &v = info->pImageMemoryBarriers[i];
                s->record(cb, v.image, v.oldLayout, v.newLayout, v.srcStageMask, v.dstStageMask, v.srcAccessMask, v.dstAccessMask, v.subresourceRange, v.srcQueueFamilyIndex, v.dstQueueFamilyIndex);
            }
    }
    static VKAPI_ATTR void VKAPI_CALL core(VkCommandBuffer cb, const VkDependencyInfo *info) {
        auto *s = active_;
        if (!s || !s->barrier2_) return;
        s->barrier2_(cb, info);
        modern(cb, info);
    }
    static VKAPI_ATTR void VKAPI_CALL khr(VkCommandBuffer cb, const VkDependencyInfo *info) {
        auto *s = active_;
        if (!s || !s->barrier2KHR_) return;
        s->barrier2KHR_(cb, info);
        modern(cb, info);
    }
    inline static RealProjectScopeObservation *active_ = nullptr;
    bool enabled_ = false;
    unsigned posts_ = 0, precise_ = 0, broad_ = 0, failures_ = 0;
    VkCommandBuffer cb_ = VK_NULL_HANDLE;
    std::array<VkImage, 4> images_{};
    unsigned count_ = 0, mask_ = 0;
    PFN_vkCmdPipelineBarrier barrier_ = nullptr;
    PFN_vkCmdPipelineBarrier2 barrier2_ = nullptr;
    PFN_vkCmdPipelineBarrier2KHR barrier2KHR_ = nullptr;
};
} // namespace cc::vulkan_test
