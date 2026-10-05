#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <limits>
#include <vector>

namespace cc {
namespace gfx {

// Handles remain owned by the engine's existing resource/recording leases.
// This model owns the submission arrays; single-device masks/indices match
// current device creation. Neither encoder submits extra signal-only work.
struct VulkanSubmitDependency {
    VkSemaphore semaphore = VK_NULL_HANDLE;
    VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    uint64_t value = 0;
    bool timeline = false;
};
struct VulkanSubmitBatch {
    std::vector<VkCommandBuffer> commands;
    std::vector<VulkanSubmitDependency> waits;
    std::vector<VulkanSubmitDependency> signals;
    // Compatibility adapter only: preserves borrowed legacy pNext nodes for
    // existing legacy callers. Submit2 rejects unmodelled nodes instead of
    // silently losing protected/device-group/extension semantics.
    const void *legacyNext = nullptr;

    bool valid() const {
        constexpr auto max = std::numeric_limits<uint32_t>::max();
        if (commands.size() >= max || waits.size() >= max || signals.size() >= max) return false;
        for (auto command : commands) if (!command) return false;
        for (const auto &wait : waits) if (!wait.semaphore || !wait.stages || (!wait.timeline && wait.value)) return false;
        for (const auto &signal : signals) {
            if (!signal.semaphore || signal.stages != VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT || (!signal.timeline && signal.value)) return false;
        }
        return true;
    }
    bool canEncode(bool modern) const {
        if (!valid() || (modern && legacyNext)) return false;
        if (!modern) for (const auto &wait : waits) {
            if (wait.stages > std::numeric_limits<VkPipelineStageFlags>::max()) return false;
        }
        return true;
    }
    bool hasLegacyTimeline() const {
        for (auto *n = static_cast<const VkBaseInStructure *>(legacyNext); n; n = n->pNext) {
            if (n->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) return true;
        }
        return false;
    }
    static bool fromLegacy(const VkSubmitInfo &info, VulkanSubmitBatch &batch) {
        batch = {};
        constexpr auto max = std::numeric_limits<uint32_t>::max();
        if (info.sType != VK_STRUCTURE_TYPE_SUBMIT_INFO || info.waitSemaphoreCount == max ||
            info.commandBufferCount == max || info.signalSemaphoreCount == max ||
            (info.waitSemaphoreCount && (!info.pWaitSemaphores || !info.pWaitDstStageMask)) ||
            (info.commandBufferCount && !info.pCommandBuffers) || (info.signalSemaphoreCount && !info.pSignalSemaphores)) return false;
        batch.legacyNext = info.pNext;
        for (uint32_t i=0; i<info.commandBufferCount; ++i) batch.commands.push_back(info.pCommandBuffers[i]);
        for (uint32_t i=0; i<info.waitSemaphoreCount; ++i) batch.waits.push_back({info.pWaitSemaphores[i], info.pWaitDstStageMask[i]});
        for (uint32_t i=0; i<info.signalSemaphoreCount; ++i) batch.signals.push_back({info.pSignalSemaphores[i]});
        return batch.valid();
    }
};

// Owns every pointer exposed in either VkSubmitInfo. Build all arrays before
// the actual driver call/bookkeeping publication. No copy/move of pointer state.
class VulkanSubmitEncoding final {
public:
    VulkanSubmitEncoding() = default;
    VulkanSubmitEncoding(const VulkanSubmitEncoding &) = delete;
    VulkanSubmitEncoding &operator=(const VulkanSubmitEncoding &) = delete;
    VulkanSubmitEncoding(VulkanSubmitEncoding &&) = delete;
    VulkanSubmitEncoding &operator=(VulkanSubmitEncoding &&) = delete;
    bool build(const VulkanSubmitBatch &batch, bool modern) {
        legacy_ = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; modern_ = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        values_ = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        waits_.clear(); signals_.clear(); stages_.clear(); waitValues_.clear(); signalValues_.clear(); commands_.clear();
        waits2_.clear(); signals2_.clear(); commands2_.clear();
        if (!batch.canEncode(modern)) return false;
        if (modern) {
            for (const auto &wait : batch.waits) {
                VkSemaphoreSubmitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
                info.semaphore=wait.semaphore; info.value=wait.timeline ? wait.value : 0; info.stageMask=wait.stages; info.deviceIndex=0;
                waits2_.push_back(info);
            }
            for (const auto &signal : batch.signals) {
                VkSemaphoreSubmitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
                info.semaphore=signal.semaphore; info.value=signal.timeline ? signal.value : 0;
                info.stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; info.deviceIndex=0;
                signals2_.push_back(info);
            }
            for (auto command : batch.commands) {
                VkCommandBufferSubmitInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
                info.commandBuffer=command; info.deviceMask=1; commands2_.push_back(info);
            }
            modern_.waitSemaphoreInfoCount=static_cast<uint32_t>(waits2_.size()); modern_.pWaitSemaphoreInfos=waits2_.data();
            modern_.commandBufferInfoCount=static_cast<uint32_t>(commands2_.size()); modern_.pCommandBufferInfos=commands2_.data();
            modern_.signalSemaphoreInfoCount=static_cast<uint32_t>(signals2_.size()); modern_.pSignalSemaphoreInfos=signals2_.data();
        } else {
            bool timeline = false;
            for (const auto &wait : batch.waits) {
                waits_.push_back(wait.semaphore); stages_.push_back(static_cast<VkPipelineStageFlags>(wait.stages));
                waitValues_.push_back(wait.timeline ? wait.value : 0); timeline |= wait.timeline;
            }
            for (const auto &signal : batch.signals) {
                signals_.push_back(signal.semaphore); signalValues_.push_back(signal.timeline ? signal.value : 0); timeline |= signal.timeline;
            }
            if (timeline && batch.hasLegacyTimeline()) return false;
            commands_=batch.commands;
            legacy_.waitSemaphoreCount=static_cast<uint32_t>(waits_.size()); legacy_.pWaitSemaphores=waits_.data(); legacy_.pWaitDstStageMask=stages_.data();
            legacy_.commandBufferCount=static_cast<uint32_t>(commands_.size()); legacy_.pCommandBuffers=commands_.data();
            legacy_.signalSemaphoreCount=static_cast<uint32_t>(signals_.size()); legacy_.pSignalSemaphores=signals_.data(); legacy_.pNext=batch.legacyNext;
            if (timeline) {
                values_.waitSemaphoreValueCount=legacy_.waitSemaphoreCount; values_.pWaitSemaphoreValues=waitValues_.data();
                values_.signalSemaphoreValueCount=legacy_.signalSemaphoreCount; values_.pSignalSemaphoreValues=signalValues_.data();
                values_.pNext=batch.legacyNext; legacy_.pNext=&values_;
            }
        }
        return true;
    }
    const VkSubmitInfo &legacy() const { return legacy_; }
    const VkSubmitInfo2 &modern() const { return modern_; }
private:
    VkSubmitInfo legacy_{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    VkSubmitInfo2 modern_{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    VkTimelineSemaphoreSubmitInfo values_{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    std::vector<VkSemaphore> waits_,signals_;
    std::vector<VkCommandBuffer> commands_;
    std::vector<VkPipelineStageFlags> stages_;
    std::vector<uint64_t> waitValues_,signalValues_;
    std::vector<VkSemaphoreSubmitInfo> waits2_,signals2_;
    std::vector<VkCommandBufferSubmitInfo> commands2_;
};
} // namespace gfx
} // namespace cc
