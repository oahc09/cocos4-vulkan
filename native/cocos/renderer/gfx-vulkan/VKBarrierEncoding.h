#pragma once

#include <vulkan/vulkan.h>
#include <array>
#include <vector>

namespace cc {
namespace gfx {

// Semantic port of legacy stage/access masks, not a scope optimization.
// Legacy bit values are shared with the 64-bit Sync2 flags. Keep TOP/BOTTOM
// aliases, layouts, ranges, queue-family indices and dependency flags intact.
// Each barrier receives the ORIGINAL command-wide stage scopes.
//
// Owning storage: common count<=1 barriers live in inline std::array members
// (zero heap allocation); larger counts spill to std::vector. Rebuild clears
// everything; failed builds zero all info pointers/counts.
class VulkanBarrierEncoding final {
public:
    VulkanBarrierEncoding() = default;
    VulkanBarrierEncoding(const VulkanBarrierEncoding &) = delete;
    VulkanBarrierEncoding &operator=(const VulkanBarrierEncoding &) = delete;
    VulkanBarrierEncoding(VulkanBarrierEncoding &&) = delete;
    VulkanBarrierEncoding &operator=(VulkanBarrierEncoding &&) = delete;

    bool build(VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags flags,
               uint32_t memoryCount, const VkMemoryBarrier *memory,
               uint32_t bufferCount, const VkBufferMemoryBarrier *buffers,
               uint32_t imageCount, const VkImageMemoryBarrier *images) {
        // always reset: no stale pointers after failure or rebuild
        info_ = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        memoryVec_.clear(); bufferVec_.clear(); imageVec_.clear();
        memoryUsed_ = bufferUsed_ = imageUsed_ = 0;
        if ((memoryCount && !memory) || (bufferCount && !buffers) || (imageCount && !images)) return fail();
        for (uint32_t i = 0; i < memoryCount; ++i)
            if (memory[i].sType != VK_STRUCTURE_TYPE_MEMORY_BARRIER || memory[i].pNext) return fail();
        for (uint32_t i = 0; i < bufferCount; ++i)
            if (buffers[i].sType != VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER || buffers[i].pNext) return fail();
        for (uint32_t i = 0; i < imageCount; ++i)
            if (images[i].sType != VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER || images[i].pNext) return fail();

        if (!memoryCount && !bufferCount && !imageCount) {
            // execution-only fallback: one memory node with original stages
            auto &b = memoryInline_[0];
            b = {};
            b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            b.srcStageMask = src; b.dstStageMask = dst;
            memoryUsed_ = 1;
        } else {
            if (memoryCount == 1) {
                auto &b = memoryInline_[0];
                b = {};
                b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
                b.srcStageMask = src; b.dstStageMask = dst;
                b.srcAccessMask = memory[0].srcAccessMask;
                b.dstAccessMask = memory[0].dstAccessMask;
                memoryUsed_ = 1;
            } else if (memoryCount > 1) {
                memoryVec_.resize(memoryCount);
                for (uint32_t i = 0; i < memoryCount; ++i) {
                    auto &b = memoryVec_[i];
                    b = {};
                    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
                    b.srcStageMask = src; b.dstStageMask = dst;
                    b.srcAccessMask = memory[i].srcAccessMask;
                    b.dstAccessMask = memory[i].dstAccessMask;
                }
            }
            if (bufferCount == 1) {
                const auto &old = buffers[0];
                auto &b = bufferInline_[0];
                b = {};
                b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                b.srcStageMask = src; b.dstStageMask = dst;
                b.srcAccessMask = old.srcAccessMask; b.dstAccessMask = old.dstAccessMask;
                b.srcQueueFamilyIndex = old.srcQueueFamilyIndex; b.dstQueueFamilyIndex = old.dstQueueFamilyIndex;
                b.buffer = old.buffer; b.offset = old.offset; b.size = old.size;
                bufferUsed_ = 1;
            } else if (bufferCount > 1) {
                bufferVec_.resize(bufferCount);
                for (uint32_t i = 0; i < bufferCount; ++i) {
                    const auto &old = buffers[i];
                    auto &b = bufferVec_[i];
                    b = {};
                    b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                    b.srcStageMask = src; b.dstStageMask = dst;
                    b.srcAccessMask = old.srcAccessMask; b.dstAccessMask = old.dstAccessMask;
                    b.srcQueueFamilyIndex = old.srcQueueFamilyIndex; b.dstQueueFamilyIndex = old.dstQueueFamilyIndex;
                    b.buffer = old.buffer; b.offset = old.offset; b.size = old.size;
                }
            }
            if (imageCount == 1) {
                const auto &old = images[0];
                auto &b = imageInline_[0];
                b = {};
                b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                b.srcStageMask = src; b.dstStageMask = dst;
                b.srcAccessMask = old.srcAccessMask; b.dstAccessMask = old.dstAccessMask;
                b.srcQueueFamilyIndex = old.srcQueueFamilyIndex; b.dstQueueFamilyIndex = old.dstQueueFamilyIndex;
                b.oldLayout = old.oldLayout; b.newLayout = old.newLayout;
                b.image = old.image; b.subresourceRange = old.subresourceRange;
                imageUsed_ = 1;
            } else if (imageCount > 1) {
                imageVec_.resize(imageCount);
                for (uint32_t i = 0; i < imageCount; ++i) {
                    const auto &old = images[i];
                    auto &b = imageVec_[i];
                    b = {};
                    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                    b.srcStageMask = src; b.dstStageMask = dst;
                    b.srcAccessMask = old.srcAccessMask; b.dstAccessMask = old.dstAccessMask;
                    b.srcQueueFamilyIndex = old.srcQueueFamilyIndex; b.dstQueueFamilyIndex = old.dstQueueFamilyIndex;
                    b.oldLayout = old.oldLayout; b.newLayout = old.newLayout;
                    b.image = old.image; b.subresourceRange = old.subresourceRange;
                }
            }
        }

        info_.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        info_.dependencyFlags = flags;
        info_.memoryBarrierCount = memoryUsed_ + static_cast<uint32_t>(memoryVec_.size());
        info_.pMemoryBarriers = info_.memoryBarrierCount
            ? (memoryVec_.empty() ? memoryInline_.data() : memoryVec_.data()) : nullptr;
        info_.bufferMemoryBarrierCount = bufferUsed_ + static_cast<uint32_t>(bufferVec_.size());
        info_.pBufferMemoryBarriers = info_.bufferMemoryBarrierCount
            ? (bufferVec_.empty() ? bufferInline_.data() : bufferVec_.data()) : nullptr;
        info_.imageMemoryBarrierCount = imageUsed_ + static_cast<uint32_t>(imageVec_.size());
        info_.pImageMemoryBarriers = info_.imageMemoryBarrierCount
            ? (imageVec_.empty() ? imageInline_.data() : imageVec_.data()) : nullptr;
        return true;
    }

    const VkDependencyInfo &info() const { return info_; }

private:
    bool fail() {
        info_ = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        memoryVec_.clear(); bufferVec_.clear(); imageVec_.clear();
        memoryUsed_ = bufferUsed_ = imageUsed_ = 0;
        return false;
    }

    VkDependencyInfo info_{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    // inline storage for the common <=1 case (zero heap alloc)
    std::array<VkMemoryBarrier2, 1> memoryInline_{};
    std::array<VkBufferMemoryBarrier2, 1> bufferInline_{};
    std::array<VkImageMemoryBarrier2, 1> imageInline_{};
    uint32_t memoryUsed_ = 0, bufferUsed_ = 0, imageUsed_ = 0;
    // vector fallback for counts > 1
    std::vector<VkMemoryBarrier2> memoryVec_;
    std::vector<VkBufferMemoryBarrier2> bufferVec_;
    std::vector<VkImageMemoryBarrier2> imageVec_;
};
} // namespace gfx
} // namespace cc
