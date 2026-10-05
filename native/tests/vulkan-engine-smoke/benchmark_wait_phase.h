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

// Test-only, fixed-capacity CPU recorder. No Vulkan calls or object ownership.
// The caller keeps the generation alive; its token is compared only by identity.
#pragma once

#include <vulkan/vulkan_core.h>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace wait_phase_detail {
constexpr uint32_t kMaxFences = 32;
enum class WaitPhase : uint8_t { Unknown, Acquire, Record, Submit, Observe, Present };
struct PhaseBucket {
    uint64_t fenceCalls = 0;
    uint64_t semaphoreCalls = 0;
    uint64_t failures = 0;
    double milliseconds = 0;
};
struct WaitPhaseSnapshot {
    std::array<PhaseBucket, 6> buckets{};
    uint64_t acquireCalls = 0;
    uint64_t acquireSuccesses = 0;
    uint64_t acquirePresentFenceCalls = 0;
    uint64_t acquireOtherFenceCalls = 0;
    double acquirePresentFenceMilliseconds = 0;
    uint64_t identityFailures = 0;
    bool invalid = false;
    uint32_t fenceCount = 0;
    std::array<VkFence, kMaxFences> fences{};
    std::array<uint8_t, kMaxFences> pending{};
    uint64_t totalFenceCalls() const noexcept {
        uint64_t total = 0;
        for (const auto &bucket : buckets) total += bucket.fenceCalls;
        return total;
    }
    uint64_t totalSemaphoreCalls() const noexcept {
        uint64_t total = 0;
        for (const auto &bucket : buckets) total += bucket.semaphoreCalls;
        return total;
    }
    uint64_t totalFailures() const noexcept {
        uint64_t total = 0;
        for (const auto &bucket : buckets) total += bucket.failures;
        return total;
    }
    double totalMilliseconds() const noexcept {
        double total = 0;
        for (const auto &bucket : buckets) total += bucket.milliseconds;
        return total;
    }
};
} // namespace wait_phase_detail

class BenchWaitPhaseRecorder {
public:
    using Phase = wait_phase_detail::WaitPhase;
    using Snapshot = wait_phase_detail::WaitPhaseSnapshot;
    BenchWaitPhaseRecorder() noexcept = default;
    BenchWaitPhaseRecorder(const BenchWaitPhaseRecorder &) = delete;
    BenchWaitPhaseRecorder &operator=(const BenchWaitPhaseRecorder &) = delete;
    BenchWaitPhaseRecorder(BenchWaitPhaseRecorder &&) = delete;
    BenchWaitPhaseRecorder &operator=(BenchWaitPhaseRecorder &&) = delete;

    bool begin(VkDevice device, VkSwapchainKHR swapchain, const void *generationToken,
               const VkFence *fences, const uint8_t *pending, uint32_t count) noexcept {
        if (active_) {
            snapshot_.invalid = true;
            return false;
        }
        reset();
        if (!device || !swapchain || !generationToken || count > wait_phase_detail::kMaxFences ||
            (count != 0 && (!fences || !pending))) return reject_();
        for (uint32_t i = 0; i < count; ++i) {
            if (!fences[i] || pending[i] > 1) return reject_();
            for (uint32_t j = 0; j < i; ++j) {
                if (fences[i] == fences[j]) return reject_();
            }
        }
        device_ = device;
        swapchain_ = swapchain;
        token_ = generationToken;
        snapshot_.fenceCount = count;
        for (uint32_t i = 0; i < count; ++i) {
            snapshot_.fences[i] = fences[i];
            snapshot_.pending[i] = pending[i];
        }
        active_ = true;
        return true;
    }
    void reset() noexcept {
        snapshot_ = Snapshot{};
        device_ = VK_NULL_HANDLE;
        swapchain_ = VK_NULL_HANDLE;
        token_ = nullptr;
        acquiredOnce_ = false;
        driverImageIndex_ = std::numeric_limits<uint32_t>::max();
        phase_ = Phase::Unknown;
        active_ = false;
    }
    bool setPhase(Phase phase) noexcept {
        if (!active_) return false;
        if (static_cast<size_t>(phase) >= snapshot_.buckets.size()) return reject_();
        phase_ = phase;
        return true;
    }
    // Returns the actual acquire result's success category, independently of
    // instrumentation validity. No actual Vulkan argument or result is changed.
    bool onAcquire(VkDevice device, VkSwapchainKHR swapchain, VkResult result,
                   uint32_t imageIndex, const void *liveGenerationToken) noexcept {
        if (!active_) return false;
        if (phase_ != Phase::Acquire) return reject_();
        ++snapshot_.acquireCalls;
        bool identity = device == device_ && swapchain == swapchain_ && liveGenerationToken == token_;
        const bool success = result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
        if (success) {
            ++snapshot_.acquireSuccesses;
            if (acquiredOnce_) {
                identity = false;
            } else {
                acquiredOnce_ = true;
                driverImageIndex_ = imageIndex;
            }
            if (snapshot_.fenceCount != 0 && imageIndex >= snapshot_.fenceCount) identity = false;
        }
        if (!identity) failIdentity_();
        return success;
    }
    bool recordFenceWait(VkDevice device, uint32_t count, const VkFence *fences,
                         VkResult result, double milliseconds, const void *liveGenerationToken,
                         uint32_t engineImageIndex) noexcept {
        if (!active_) return false;
        if (!device || count == 0 || !fences || !validDuration_(milliseconds)) return reject_();
        for (uint32_t i = 0; i < count; ++i) {
            if (!fences[i]) return reject_();
        }
        auto &bucket = snapshot_.buckets[static_cast<size_t>(phase_)];
        ++bucket.fenceCalls;
        bucket.milliseconds += milliseconds;
        if (result != VK_SUCCESS) ++bucket.failures;
        bool identity = device == device_;
        if (phase_ == Phase::Acquire) {
            bool presentFence = false;
            if (count == 1) {
                for (uint32_t i = 0; i < snapshot_.fenceCount; ++i) {
                    if (fences[0] == snapshot_.fences[i]) {
                        presentFence = true;
                        break;
                    }
                }
            }
            if (presentFence) {
                ++snapshot_.acquirePresentFenceCalls;
                snapshot_.acquirePresentFenceMilliseconds += milliseconds;
                const bool indexValid = acquiredOnce_ && driverImageIndex_ < snapshot_.fenceCount;
                identity = identity && liveGenerationToken == token_ && indexValid &&
                           engineImageIndex == driverImageIndex_;
                if (indexValid) {
                    identity = identity && fences[0] == snapshot_.fences[driverImageIndex_] &&
                               snapshot_.pending[driverImageIndex_] == 1;
                }
            } else {
                ++snapshot_.acquireOtherFenceCalls;
            }
        }
        if (!identity) failIdentity_();
        return true;
    }
    bool recordSemaphoreWait(VkDevice device, VkResult result, double milliseconds) noexcept {
        if (!active_) return false;
        if (!device || !validDuration_(milliseconds)) return reject_();
        auto &bucket = snapshot_.buckets[static_cast<size_t>(phase_)];
        ++bucket.semaphoreCalls;
        bucket.milliseconds += milliseconds;
        if (result != VK_SUCCESS) ++bucket.failures;
        if (device != device_) failIdentity_();
        return true;
    }
    void end() noexcept {
        active_ = false;
        phase_ = Phase::Unknown;
    }
    bool isActive() const noexcept { return active_; }
    bool isInvalid() const noexcept { return snapshot_.invalid; }
    const Snapshot &snapshot() const noexcept { return snapshot_; }
    Phase currentPhase() const noexcept { return phase_; }

private:
    bool reject_() noexcept {
        snapshot_.invalid = true;
        return false;
    }
    void failIdentity_() noexcept {
        ++snapshot_.identityFailures;
        snapshot_.invalid = true;
    }
    bool validDuration_(double milliseconds) const noexcept {
        return std::isfinite(milliseconds) && milliseconds >= 0 &&
               std::isfinite(snapshot_.totalMilliseconds() + milliseconds);
    }
    VkDevice device_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    const void *token_ = nullptr;
    uint32_t driverImageIndex_ = std::numeric_limits<uint32_t>::max();
    bool acquiredOnce_ = false;
    bool active_ = false;
    Phase phase_ = Phase::Unknown;
    Snapshot snapshot_{};
};
