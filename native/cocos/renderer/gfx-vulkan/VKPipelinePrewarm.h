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

// VulkanPipelinePrewarmQueue: reusable pure-CPU in-memory manifest + serial
// prewarm queue for known pipeline keys. Experimental interface only; it does
// not call Vulkan, does not create or destroy GPU objects, does not touch the
// filesystem, and does not change any existing runtime default. The real
// engine factory / driver cache / checkpoint integration is a later step.
//
// Semantics:
// - The manifest is a std::vector<Entry>. enqueue() deep-copies the label,
//   the complete key variant, and the owning std::function closure. A job is
//   invalid unless the label is non-empty and <= 256 chars, the expected key
//   is valid (a valueless variant is invalid), and the creator is non-empty.
// - Dedup uses the complete variant type + full tuple equality of the key.
//   Labels are not dedup keys: the same key under a different label is still
//   a Duplicate; different keys with the same label are both kept. Compute
//   and graphics keys never compare equal to each other.
// - Order of enqueue checks: busy rejection first, then validation, then
//   duplicate, then capacity. A full queue still reports Duplicate for an
//   equivalent entry. capacity 0 is legal; any valid new job is then Full.
// - enqueue builds a local owning snapshot first and publishes on success.
//   Any allocation failure maps to OutOfMemory and leaves the manifest
//   (order/state/cursor) completely unchanged. Copy exceptions of Result/Key
//   behave as ordinary value semantics; only enqueue/drain carry noexcept.
// - drain(maxJobs) walks FIFO, attempts each Pending job once. maxJobs
//   counts attempts (failures consume budget). The callback result counts as
//   Ready only when created == true, actual is valid, and actual equals the
//   expected key in full; created == false, an invalid/mismatched actual, or
//   any exception (including bad_alloc) is Failed, then the walk continues.
//   Done/failed jobs never rerun; a later drain continues at the cursor and
//   newly appended jobs join the same wave. There is no automatic retry; a
//   new experiment wave is clear() + re-enqueue.
// - Reentrancy: enqueue during drain is Busy, clear during drain is false,
//   a nested drain returns busy == true with zero counters. The draining
//   flag is restored on every exit. clear() only releases the manifest and
//   closures when not draining.
// - Single-threaded by design: no threads, locks, async, or Vk calls. The
//   in-memory manifest is not cross-run serialization.
// - Entry closures own their captures with normal value-copy semantics; the
//   queue does not extend the lifetime of raw borrowed resources. entries()
//   is a const view; references invalidate on clear, owner destruction, or
//   reallocation from later growth.

#ifndef CC_GFX_VK_PIPELINE_PREWARM_H
#define CC_GFX_VK_PIPELINE_PREWARM_H

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "VKComputePipelineKey.h"
#include "VKGraphicsPipelineKey.h"

namespace cc {
namespace gfx {

class VulkanPipelinePrewarmQueue final {
public:
    using Key = std::variant<VulkanComputePipelineKey, VulkanGraphicsPipelineKey>;

    struct CreateResult {
        bool created = false;
        Key actual{};
    };
    using Creator = std::function<CreateResult()>;

    struct Job {
        std::string label;
        Key expected{};
        Creator create{};
    };

    enum class EnqueueStatus { Added,
                               Duplicate,
                               Invalid,
                               Full,
                               OutOfMemory,
                               Busy };

    enum class JobState { Pending,
                          Ready,
                          Failed };

    struct Entry {
        Job job;
        JobState state = JobState::Pending;
    };

    struct DrainReport {
        size_t attempted = 0;
        size_t ready = 0;
        size_t failed = 0;
        bool busy = false;
    };

    explicit VulkanPipelinePrewarmQueue(size_t capacity = 256) noexcept
    : capacity_(capacity) {}

    VulkanPipelinePrewarmQueue(const VulkanPipelinePrewarmQueue &) = delete;
    VulkanPipelinePrewarmQueue &operator=(const VulkanPipelinePrewarmQueue &) = delete;
    VulkanPipelinePrewarmQueue(VulkanPipelinePrewarmQueue &&) = delete;
    VulkanPipelinePrewarmQueue &operator=(VulkanPipelinePrewarmQueue &&) = delete;

    EnqueueStatus enqueue(const Job &job) noexcept {
        if (draining_) return EnqueueStatus::Busy;

        // Phase 1: validation (no writes to the manifest).
        if (job.label.empty() || job.label.size() > 256u) return EnqueueStatus::Invalid;
        if (!keyValid(job.expected)) return EnqueueStatus::Invalid;
        if (!static_cast<bool>(job.create)) return EnqueueStatus::Invalid;

        // Phase 2: duplicate check on the complete variant + full tuple equality.
        if (containsKey(job.expected)) return EnqueueStatus::Duplicate;

        // Phase 3: capacity. A full queue still reports Duplicate for an
        // equivalent entry (handled above).
        if (entries_.size() >= capacity_) return EnqueueStatus::Full;

        // Phase 4: local owning snapshot, then publish. Any allocation
        // failure leaves the manifest completely unchanged.
        try {
            Entry snapshot;
            snapshot.job.label = job.label;
            snapshot.job.expected = job.expected;
            snapshot.job.create = job.create;
            snapshot.state = JobState::Pending;
            entries_.push_back(std::move(snapshot));
        } catch (...) {
            return EnqueueStatus::OutOfMemory;
        }
        return EnqueueStatus::Added;
    }

    DrainReport drain(size_t maxJobs) noexcept {
        DrainReport report;
        if (draining_) {
            report.busy = true;
            return report;
        }
        draining_ = true;
        size_t attempted = 0, ready = 0, failed = 0;
        for (size_t i = 0; i < entries_.size() && attempted < maxJobs; ++i) {
            Entry &entry = entries_[i];
            if (entry.state != JobState::Pending) continue;
            ++attempted;
            bool ok = false;
            try {
                CreateResult result = entry.job.create();
                if (result.created && keyValid(result.actual) && result.actual == entry.job.expected) {
                    ok = true;
                }
            } catch (...) {
                ok = false;
            }
            if (ok) {
                entry.state = JobState::Ready;
                ++ready;
            } else {
                entry.state = JobState::Failed;
                ++failed;
            }
        }
        draining_ = false;
        report.attempted = attempted;
        report.ready = ready;
        report.failed = failed;
        report.busy = false;
        return report;
    }

    bool clear() noexcept {
        if (draining_) return false;
        entries_.clear();
        return true;
    }

    const std::vector<Entry> &entries() const noexcept { return entries_; }
    size_t size() const noexcept { return entries_.size(); }
    size_t capacity() const noexcept { return capacity_; }

private:
    static bool keyValid(const Key &key) noexcept {
        if (key.valueless_by_exception()) return false;
        if (auto *compute = std::get_if<VulkanComputePipelineKey>(&key)) {
            return compute->valid();
        }
        if (auto *graphics = std::get_if<VulkanGraphicsPipelineKey>(&key)) {
            return graphics->valid();
        }
        return false;
    }

    bool containsKey(const Key &key) const noexcept {
        for (const Entry &entry : entries_) {
            if (entry.job.expected == key) return true;
        }
        return false;
    }

    std::vector<Entry> entries_;
    size_t capacity_;
    bool draining_ = false;
};

} // namespace gfx
} // namespace cc

#endif // CC_GFX_VK_PIPELINE_PREWARM_H
