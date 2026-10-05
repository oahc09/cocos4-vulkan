#pragma once

#include "VKSubmitBatch.h"
#include <atomic>
#include <cstdint>
#include <deque>
#include <limits>
#include <vector>

namespace cc {
namespace gfx {

using GpuCompletionValue = uint64_t;
struct GpuCompletionPoint {
    uint64_t domain = 0;
    GpuCompletionValue value = 0;
    bool empty() const { return domain == 0 && value == 0; }
};

// Borrowed Vulkan fences stay owned by their original pool/transport hub.
// Every successful actual submit receives a unique point within its VkQueue.
// External queue synchronization remains the caller's responsibility.
class GpuCompletionTracker final {
public:
    struct Functions {
        // Addresses preserve dynamic Volk dispatch (including validation hooks).
        PFN_vkQueueSubmit *submit = nullptr;
        PFN_vkGetFenceStatus *getFenceStatus = nullptr;
        PFN_vkWaitForFences *waitForFences = nullptr;
        PFN_vkGetSemaphoreCounterValue *getSemaphoreCounterValue = nullptr;
        PFN_vkWaitSemaphores *waitSemaphores = nullptr;
        PFN_vkQueueSubmit2 *submit2 = nullptr;
    };

    GpuCompletionTracker(VkDevice device, VkQueue queue, Functions functions)
    : device_(device), queue_(queue), functions_(functions), domain_(nextDomain_.fetch_add(1)) {}
    GpuCompletionTracker(const GpuCompletionTracker &) = delete;
    GpuCompletionTracker &operator=(const GpuCompletionTracker &) = delete;
    GpuCompletionTracker(GpuCompletionTracker &&) = delete;
    GpuCompletionTracker &operator=(GpuCompletionTracker &&) = delete;

    uint64_t domain() const { return domain_; }
    GpuCompletionValue submittedValue() const { return submitted_; }
    GpuCompletionValue completedValue() const { return completed_; }
    VkResult lastResult() const { return lastResult_; }
    bool usesTimeline() const { return timeline_ != VK_NULL_HANDLE; }
    bool usesSubmit2() const { return submit2_; }
    // Caller must have enabled synchronization2 on this VkDevice. Encoder and
    // completion choices remain independent; configuration is bootstrap-only.
    VkResult configureSubmit2() {
        if (!active_ || !device_ || !queue_ || submitted_ || !functions_.submit2 || !*functions_.submit2) return remember(unavailableResult());
        submit2_ = true; return VK_SUCCESS;
    }

    // The caller has enabled the feature and owns a dedicated, initially-zero
    // timeline semaphore. Only this tracker signals it; no host signals are allowed.
    // Its lifetime must cover all submissions, not just this C++ object's lifetime.
    VkResult configureTimeline(VkSemaphore semaphore, uint64_t maxValueDifference) {
        if (!active_ || !domain_ || !device_ || !queue_ || submitted_ || usesTimeline() ||
            !semaphore || !maxValueDifference || !functions_.getSemaphoreCounterValue ||
            !*functions_.getSemaphoreCounterValue || !functions_.waitSemaphores || !*functions_.waitSemaphores) {
            return remember(unavailableResult());
        }
        uint64_t initialValue = 0;
        remember((*functions_.getSemaphoreCounterValue)(device_, semaphore, &initialValue));
        if (lastResult_ != VK_SUCCESS) return lastResult_;
        if (initialValue != 0) return remember(VK_ERROR_INITIALIZATION_FAILED);
        timeline_ = semaphore;
        maxValueDifference_ = maxValueDifference;
        return VK_SUCCESS;
    }

    // Compatibility entry for isolated legacy tests/callers; the engine's
    // business layer constructs VulkanSubmitBatch directly.
    VkResult submit(const VkSubmitInfo &info, VkFence fence, GpuCompletionPoint &point) {
        point = {};
        if (!active_) return remember(unavailableResult());
        VulkanSubmitBatch batch;
        if (!VulkanSubmitBatch::fromLegacy(info, batch)) return remember(VK_ERROR_INITIALIZATION_FAILED);
        return submit(batch, fence, point);
    }
    VkResult submit(const VulkanSubmitBatch &caller, VkFence fence, GpuCompletionPoint &point) {
        point = {};
        const bool dispatch = submit2_ ? (functions_.submit2 && *functions_.submit2) : (functions_.submit && *functions_.submit);
        if (!active_ || !domain_ || !device_ || !queue_ || (!usesTimeline() && !fence) || !dispatch ||
            submitted_ == std::numeric_limits<GpuCompletionValue>::max()) return remember(unavailableResult());
        if (!caller.valid() || (usesTimeline() && caller.hasLegacyTimeline())) return remember(VK_ERROR_INITIALIZATION_FAILED);
        for (const auto &wait : caller.waits) if (usesTimeline() && wait.semaphore == timeline_) return remember(VK_ERROR_INITIALIZATION_FAILED);
        for (const auto &signal : caller.signals) if (usesTimeline() && signal.semaphore == timeline_) return remember(VK_ERROR_INITIALIZATION_FAILED);
        // Reject unmodelled Submit2 pNext and legacy-unrepresentable stages
        // before a counter/fence query could have observable side effects.
        if (!caller.canEncode(submit2_)) return remember(VK_ERROR_INITIALIZATION_FAILED);
        if (!usesTimeline()) {
            for (const auto &record : pending_) if (record.fence == fence) return remember(unavailableResult());
        } else {
            if (fence) {
                for (const auto &record : timelineFences_) if (record.fence == fence) {
                    const auto result = observeFenceCompletion(fence);
                    if (result != VK_SUCCESS) return result;
                    break;
                }
            }
            if (submitted_ + 1 - completed_ > maxValueDifference_) {
                const auto result = queryTimeline();
                if (result != VK_SUCCESS) return result;
                if (submitted_ + 1 - completed_ > maxValueDifference_) return remember(VK_NOT_READY);
            }
        }
        const auto next = submitted_ + 1;
        VulkanSubmitBatch completedBatch;
        const VulkanSubmitBatch *batch = &caller;
        if (usesTimeline()) {
            completedBatch = caller;
            completedBatch.signals.push_back({timeline_, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, next, true});
            batch = &completedBatch;
        }
        VulkanSubmitEncoding encoding;
        if (!encoding.build(*batch, submit2_)) return remember(VK_ERROR_INITIALIZATION_FAILED);
        if (!usesTimeline()) pending_.push_back({next, fence});
        else if (fence) timelineFences_.push_back({next, fence});
        remember(submit2_ ? (*functions_.submit2)(queue_, 1, &encoding.modern(), fence)
                          : (*functions_.submit)(queue_, 1, &encoding.legacy(), fence));
        if (lastResult_ != VK_SUCCESS) {
            if (!usesTimeline()) pending_.pop_back();
            else if (fence) timelineFences_.pop_back();
            return lastResult_;
        }
        submitted_ = next; point = {domain_, next}; return VK_SUCCESS;
    }

    bool isComplete(GpuCompletionPoint point) {
        if (point.empty()) return true; // no submitted work to retire
        if (!accepts(point)) return false;
        if (point.value <= completed_) return true;
        if (usesTimeline()) return queryTimeline() == VK_SUCCESS && point.value <= completed_;
        const auto *record = find(point.value);
        if (!record || !functions_.getFenceStatus || !*functions_.getFenceStatus) return false;
        remember((*functions_.getFenceStatus)(device_, record->fence));
        if (lastResult_ != VK_SUCCESS) return false; // NOT_READY/device loss are not completion
        completeThrough(point.value);
        return true;
    }

    VkResult wait(GpuCompletionPoint point, uint64_t timeout) {
        if (point.empty()) return VK_SUCCESS;
        if (!accepts(point)) return remember(unavailableResult());
        if (point.value <= completed_) return VK_SUCCESS;
        if (usesTimeline()) {
            if (!functions_.waitSemaphores || !*functions_.waitSemaphores) return remember(unavailableResult());
            VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            info.semaphoreCount = 1;
            info.pSemaphores = &timeline_;
            info.pValues = &point.value;
            remember((*functions_.waitSemaphores)(device_, &info, timeout));
            if (lastResult_ == VK_SUCCESS) completeThrough(point.value);
            return lastResult_;
        }
        const auto *record = find(point.value);
        if (!record || !functions_.waitForFences || !*functions_.waitForFences) {
            return remember(unavailableResult());
        }
        const VkFence fence = record->fence;
        remember((*functions_.waitForFences)(device_, 1, &fence, VK_TRUE, timeout));
        if (lastResult_ == VK_SUCCESS) completeThrough(point.value);
        return lastResult_;
    }

    // Called BEFORE an owning pool resets/reuses its fences. Query actual status,
    // so an incorrect reset attempt cannot manufacture completed points.
    VkResult observeFenceCompletion(VkFence fence) {
        if (usesTimeline()) {
            for (auto it = timelineFences_.begin(); it != timelineFences_.end(); ++it) {
                if (it->fence != fence) continue;
                if (!active_ || !functions_.getFenceStatus || !*functions_.getFenceStatus) return remember(unavailableResult());
                remember((*functions_.getFenceStatus)(device_, fence));
                if (lastResult_ == VK_SUCCESS) {
                    // A completed fence also covers the batch's semaphore signals.
                    completeThrough(it->value);
                    timelineFences_.erase(it);
                }
                return lastResult_;
            }
            return VK_SUCCESS;
        }
        GpuCompletionValue value = 0;
        for (const auto &record : pending_) {
            if (record.fence == fence) { value = record.value; break; }
        }
        if (!value) return VK_SUCCESS; // already cached or another queue's fence
        if (!active_ || !functions_.getFenceStatus || !*functions_.getFenceStatus) {
            return remember(unavailableResult());
        }
        remember((*functions_.getFenceStatus)(device_, fence));
        if (lastResult_ == VK_SUCCESS) completeThrough(value);
        return lastResult_;
    }

    // CPU bookkeeping only. The owner must separately wait for GPU completion
    // before destroying any borrowed semaphore/fence/device.
    void shutdown() { active_ = false; pending_.clear(); timelineFences_.clear(); }

private:
    struct Record { GpuCompletionValue value; VkFence fence; };
    VkResult queryTimeline() {
        if (!active_ || !functions_.getSemaphoreCounterValue || !*functions_.getSemaphoreCounterValue) {
            return remember(unavailableResult());
        }
        uint64_t value = 0;
        remember((*functions_.getSemaphoreCounterValue)(device_, timeline_, &value));
        if (lastResult_ != VK_SUCCESS) return lastResult_;
        // A value beyond our own submits violates exclusive signal ownership.
        if (value > submitted_ || value < completed_) return remember(VK_ERROR_INITIALIZATION_FAILED);
        completeThrough(value);
        return VK_SUCCESS;
    }
    VkResult unavailableResult() const {
        return terminalResult_ != VK_SUCCESS ? terminalResult_ : VK_ERROR_INITIALIZATION_FAILED;
    }
    VkResult remember(VkResult result) {
        lastResult_ = result;
        if (result == VK_ERROR_DEVICE_LOST) {
            terminalResult_ = result;
            active_ = false;
        }
        return result;
    }
    bool accepts(GpuCompletionPoint point) const {
        return active_ && point.domain == domain_ && point.value && point.value <= submitted_;
    }
    const Record *find(GpuCompletionValue value) const {
        for (const auto &record : pending_) if (record.value == value) return &record;
        return nullptr;
    }
    void completeThrough(GpuCompletionValue value) {
        // A completed fence covers prior commands in this queue's submission
        // order. This does not prove presentation-engine semaphore consumption.
        completed_ = value > completed_ ? value : completed_;
        while (!pending_.empty() && pending_.front().value <= completed_) pending_.pop_front();
    }

    inline static std::atomic<uint64_t> nextDomain_{1};
    VkDevice device_;
    VkQueue queue_;
    Functions functions_;
    uint64_t domain_;
    GpuCompletionValue submitted_ = 0;
    GpuCompletionValue completed_ = 0;
    VkResult lastResult_ = VK_SUCCESS;
    VkResult terminalResult_ = VK_SUCCESS;
    bool active_ = true;
    bool submit2_ = false;
    std::deque<Record> pending_;
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    uint64_t maxValueDifference_ = 0;
    std::deque<Record> timelineFences_;
};

} // namespace gfx
} // namespace cc
