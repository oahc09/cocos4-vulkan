// CPU-only mock contracts. No loader, device, feature negotiation or GPU proof.
#include "VKCompletionTracker.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace {
using cc::gfx::GpuCompletionPoint;
using cc::gfx::GpuCompletionTracker;
unsigned failures = 0;
void check(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
template <class T> T handle(uintptr_t value) { return (T)value; }

struct Driver {
    VkResult submitResult = VK_SUCCESS;
    VkResult counterResult = VK_SUCCESS;
    VkResult waitResult = VK_TIMEOUT;
    VkResult statusResult = VK_NOT_READY;
    uint64_t counter = 0;
    int submits = 0, counters = 0, waits = 0, statuses = 0, replacements = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    uint64_t timeout = 0, waitedValue = 0;
    VkSemaphoreWaitFlags waitFlags = ~VkSemaphoreWaitFlags{0};
    const void *preservedNext = nullptr;
    std::vector<VkSemaphore> waitSemaphores, signals;
    std::vector<VkCommandBuffer> commands;
    std::vector<VkPipelineStageFlags> stages;
    std::vector<uint64_t> waitValues, signalValues;
    int calls() const { return submits + counters + waits + statuses + replacements; }
};
Driver *driver = nullptr;
VKAPI_ATTR VkResult VKAPI_CALL mockSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *infos, VkFence fence) {
    ++driver->submits;
    check(count == 1 && infos, "one actual submit batch");
    if (count != 1 || !infos) return VK_ERROR_INITIALIZATION_FAILED;
    const auto &info = *infos;
    driver->queue = queue;
    driver->fence = fence;
    check(info.pNext != nullptr, "timeline values attached");
    if (!info.pNext) return VK_ERROR_INITIALIZATION_FAILED;
    const auto &values = *static_cast<const VkTimelineSemaphoreSubmitInfo *>(info.pNext);
    check(values.sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, "timeline sType");
    check(values.waitSemaphoreValueCount == info.waitSemaphoreCount &&
              values.signalSemaphoreValueCount == info.signalSemaphoreCount, "values align with all semaphore slots");
    driver->preservedNext = values.pNext;
    driver->waitSemaphores.clear(); driver->stages.clear(); driver->commands.clear();
    driver->waitValues.clear(); driver->signals.clear(); driver->signalValues.clear();
    // Deep copies happen during the call; the adapter's arrays are temporary.
    for (uint32_t i = 0; i < info.waitSemaphoreCount; ++i) {
        driver->waitSemaphores.push_back(info.pWaitSemaphores[i]);
        driver->stages.push_back(info.pWaitDstStageMask[i]);
        driver->waitValues.push_back(values.pWaitSemaphoreValues[i]);
    }
    for (uint32_t i = 0; i < info.commandBufferCount; ++i) driver->commands.push_back(info.pCommandBuffers[i]);
    for (uint32_t i = 0; i < info.signalSemaphoreCount; ++i) {
        driver->signals.push_back(info.pSignalSemaphores[i]);
        driver->signalValues.push_back(values.pSignalSemaphoreValues[i]);
    }
    return driver->submitResult;
}
VKAPI_ATTR VkResult VKAPI_CALL mockCounter(VkDevice device, VkSemaphore semaphore, uint64_t *value) {
    ++driver->counters;
    driver->device = device; driver->semaphore = semaphore;
    *value = driver->counter;
    return driver->counterResult;
}
VKAPI_ATTR VkResult VKAPI_CALL mockWait(VkDevice device, const VkSemaphoreWaitInfo *info, uint64_t timeout) {
    ++driver->waits;
    check(info && info->sType == VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO && info->semaphoreCount == 1,
          "wait has one timeline semaphore");
    driver->device = device; driver->timeout = timeout;
    driver->semaphore = info->pSemaphores[0]; driver->waitedValue = info->pValues[0]; driver->waitFlags = info->flags;
    if (driver->waitResult == VK_SUCCESS) driver->counter = std::max(driver->counter, driver->waitedValue);
    return driver->waitResult;
}
VKAPI_ATTR VkResult VKAPI_CALL mockStatus(VkDevice, VkFence fence) {
    ++driver->statuses; driver->fence = fence;
    return driver->statusResult;
}
VKAPI_ATTR VkResult VKAPI_CALL replacementCounter(VkDevice d, VkSemaphore s, uint64_t *v) {
    ++driver->replacements; return mockCounter(d, s, v);
}
VKAPI_ATTR VkResult VKAPI_CALL replacementWait(VkDevice d, const VkSemaphoreWaitInfo *i, uint64_t t) {
    ++driver->replacements; return mockWait(d, i, t);
}
VKAPI_ATTR VkResult VKAPI_CALL replacementSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *i, VkFence f) {
    ++driver->replacements; return mockSubmit(q, n, i, f);
}

struct Fixture {
    Driver state;
    PFN_vkQueueSubmit submitFn = &mockSubmit;
    PFN_vkGetFenceStatus statusFn = &mockStatus;
    PFN_vkGetSemaphoreCounterValue counterFn = &mockCounter;
    PFN_vkWaitSemaphores waitFn = &mockWait;
    VkDevice device = handle<VkDevice>(0x100);
    VkQueue queue = handle<VkQueue>(0x200);
    VkSemaphore timeline = handle<VkSemaphore>(0x300);
    VkFence fence = handle<VkFence>(0x400);
    VkSemaphore waits[2] = {handle<VkSemaphore>(0x501), handle<VkSemaphore>(0x502)};
    VkSemaphore signals[2] = {handle<VkSemaphore>(0x601), handle<VkSemaphore>(0x602)};
    VkPipelineStageFlags stages[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
    VkCommandBuffer command = handle<VkCommandBuffer>(0x700);
    VkProtectedSubmitInfo protectedInfo{VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO};
    GpuCompletionTracker tracker;
    Fixture() : tracker(device, queue, {&submitFn, &statusFn, nullptr, &counterFn, &waitFn}) { driver = &state; }
    void configure(uint64_t limit = UINT64_MAX) {
        check(tracker.configureTimeline(timeline, limit) == VK_SUCCESS && tracker.usesTimeline(), "configure timeline");
    }
    VkSubmitInfo info() const {
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        info.pNext = &protectedInfo;
        info.waitSemaphoreCount = 2; info.pWaitSemaphores = waits; info.pWaitDstStageMask = stages;
        info.commandBufferCount = 1; info.pCommandBuffers = &command;
        info.signalSemaphoreCount = 2; info.pSignalSemaphores = signals;
        return info;
    }
    GpuCompletionPoint submit(VkFence f = VK_NULL_HANDLE) {
        GpuCompletionPoint point;
        check(tracker.submit(info(), f, point) == VK_SUCCESS, "successful timeline submit");
        return point;
    }
};
} // namespace

int main() {
    { // Configuration failures cannot select a partially initialized backend.
        Fixture f;
        check(!f.tracker.usesTimeline(), "default remains fence");
        check(f.tracker.configureTimeline(VK_NULL_HANDLE, 1) != VK_SUCCESS && f.state.calls() == 0, "null semaphore rejected");
        check(f.tracker.configureTimeline(f.timeline, 0) != VK_SUCCESS && f.state.calls() == 0, "zero limit rejected");
        f.counterFn = nullptr;
        check(f.tracker.configureTimeline(f.timeline, 1) != VK_SUCCESS && f.state.calls() == 0, "null counter dispatch rejected");
        f.counterFn = &mockCounter;
        f.waitFn = nullptr;
        check(f.tracker.configureTimeline(f.timeline, 1) != VK_SUCCESS && f.state.calls() == 0, "null wait dispatch rejected");
        f.waitFn = &mockWait;
        f.state.counterResult = VK_ERROR_OUT_OF_HOST_MEMORY;
        check(f.tracker.configureTimeline(f.timeline, 1) == VK_ERROR_OUT_OF_HOST_MEMORY && !f.tracker.usesTimeline(), "query error leaves fence mode");
        f.state.counterResult = VK_SUCCESS; f.state.counter = 1;
        check(f.tracker.configureTimeline(f.timeline, 1) != VK_SUCCESS && !f.tracker.usesTimeline(), "nonzero initial counter rejected");
        f.state.counter = 0; f.configure();
        const int before = f.state.calls();
        check(f.tracker.configureTimeline(f.timeline, 1) != VK_SUCCESS && f.state.calls() == before, "reconfiguration rejected");
    }
    {
        Fixture f;
        // Use the fence backend with an empty batch, without this mock's timeline parser.
        PFN_vkQueueSubmit legacy = +[](VkQueue, uint32_t, const VkSubmitInfo *, VkFence) -> VkResult { return VK_SUCCESS; };
        GpuCompletionTracker old(f.device, f.queue, {&legacy, &f.statusFn, nullptr});
        GpuCompletionPoint p;
        check(old.submit(VkSubmitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO}, f.fence, p) == VK_SUCCESS, "legacy work submitted");
        check(old.configureTimeline(f.timeline, 1) != VK_SUCCESS && !old.usesTimeline(), "late backend switch rejected");
    }
    { // Preserve binary waits/signals, CB and original chain in the actual call.
        Fixture f; f.configure();
        auto p = f.submit(f.fence);
        check(f.state.submits == 1 && p.value == 1 && p.domain == f.tracker.domain(), "exactly one submit publishes point");
        check(f.state.queue == f.queue && f.state.fence == f.fence && f.state.preservedNext == &f.protectedInfo, "queue/fence/pNext preserved");
        check(f.state.waitSemaphores == std::vector<VkSemaphore>{f.waits[0], f.waits[1]} &&
                  f.state.stages == std::vector<VkPipelineStageFlags>{f.stages[0], f.stages[1]} &&
                  f.state.commands == std::vector<VkCommandBuffer>{f.command}, "binary waits/stages/CB preserved");
        check(f.state.signals == std::vector<VkSemaphore>{f.signals[0], f.signals[1], f.timeline} &&
                  f.state.signalValues == std::vector<uint64_t>{0, 0, 1} &&
                  f.state.waitValues == std::vector<uint64_t>{0, 0}, "append aligned timeline value to binary signals");
    }
    {
        Fixture f; f.configure();
        GpuCompletionPoint p;
        check(f.tracker.submit(VkSubmitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO}, VK_NULL_HANDLE, p) == VK_SUCCESS, "empty caller batch accepts null fence");
        check(f.state.waitValues.empty() && f.state.commands.empty() && f.state.signals == std::vector<VkSemaphore>{f.timeline} &&
                  f.state.signalValues == std::vector<uint64_t>{1}, "empty batch still tracked in the same actual submit");
        f.state.submitResult = VK_ERROR_OUT_OF_DEVICE_MEMORY; p = {12, 34};
        check(f.tracker.submit(f.info(), f.fence, p) == VK_ERROR_OUT_OF_DEVICE_MEMORY && p.empty() &&
                  f.tracker.submittedValue() == 1, "failure does not publish or consume value");
        f.state.submitResult = VK_SUCCESS;
        check(f.submit(f.fence).value == 2, "failed borrowed fence record rolled back and retry consecutive");
    }
    {
        Fixture f; f.configure(); auto p1 = f.submit(); auto p2 = f.submit();
        f.state.counter = 1;
        check(!f.tracker.isComplete(p2) && f.tracker.completedValue() == 1, "counter below point does not complete it");
        f.state.counter = 2;
        check(f.tracker.isComplete(p2) && f.tracker.isComplete(p1), "high point covers low point");
        const int count = f.state.counters;
        f.state.counter = 0;
        check(f.tracker.isComplete(p1) && f.tracker.completedValue() == 2 && f.state.counters == count, "cached completion skips counter and never regresses");
        f.state.counter = 2; auto p3 = f.submit();
        f.state.counter = 4;
        check(!f.tracker.isComplete(p3) && f.tracker.completedValue() == 2, "future counter cannot manufacture completion");
        f.state.counter = 1;
        check(!f.tracker.isComplete(p3) && f.tracker.completedValue() == 2, "counter regression rejected");
    }
    {
        Fixture f; f.configure(); auto p = f.submit();
        check(f.tracker.wait(p, 12345) == VK_TIMEOUT && f.tracker.completedValue() == 0, "timeout does not complete");
        check(f.state.device == f.device && f.state.semaphore == f.timeline && f.state.waitedValue == p.value &&
                  f.state.timeout == 12345 && f.state.waitFlags == 0, "timeline wait forwards value/timeout and wait-all flags");
        f.state.waitResult = VK_SUCCESS;
        check(f.tracker.wait(p, 12345) == VK_SUCCESS && f.tracker.completedValue() == 1, "successful wait completes");
        int before = f.state.calls();
        check(f.tracker.wait(p, 1) == VK_SUCCESS && f.state.calls() == before, "cached wait skips driver");
        GpuCompletionTracker other(f.device, handle<VkQueue>(0x999), {});
        check(!f.tracker.isComplete({other.domain(), 1}) && f.tracker.wait({other.domain(), 1}, 0) != VK_SUCCESS, "foreign domain rejected");
        check(!f.tracker.isComplete({f.tracker.domain(), 2}) && !f.tracker.isComplete({0, 1}) &&
                  !f.tracker.isComplete({f.tracker.domain(), 0}) && f.state.calls() == before, "future/half-empty do not call driver");
        check(f.tracker.isComplete({}) && f.tracker.wait({}, 0) == VK_SUCCESS, "empty means no work");
    }
    {
        Fixture f; f.configure(1); auto p = f.submit();
        GpuCompletionPoint next{99, 99};
        check(f.tracker.submit(f.info(), VK_NULL_HANDLE, next) == VK_NOT_READY && next.empty() &&
                  f.state.submits == 1 && f.tracker.submittedValue() == 1, "limit forbids signal too far ahead without consuming a value");
        f.state.counter = p.value;
        check(f.submit().value == 2 && f.state.submits == 2, "counter advancement permits next signal");
    }
    { // Timeline signal and fence signal have separate completion observations.
        Fixture f; f.configure(); auto p = f.submit(f.fence);
        f.state.counter = 1;
        check(f.tracker.isComplete(p), "timeline commands complete");
        GpuCompletionPoint next;
        check(f.tracker.submit(f.info(), f.fence, next) == VK_NOT_READY && next.empty() && f.state.submits == 1,
              "counter completion does not permit pending fence reuse");
        check(f.tracker.observeFenceCompletion(f.fence) == VK_NOT_READY, "actual fence still pending");
        f.state.statusResult = VK_SUCCESS;
        check(f.tracker.observeFenceCompletion(f.fence) == VK_SUCCESS, "actual fence completion releases handle");
        check(f.submit(f.fence).value == 2, "fence handle reusable only after actual completion");
        int before = f.state.calls();
        check(f.tracker.observeFenceCompletion(handle<VkFence>(0x888)) == VK_SUCCESS && f.state.calls() == before, "unknown fence no-op");
    }
    {
        Fixture f; f.configure(); f.submit();
        const int before = f.state.submits;
        VkTimelineSemaphoreSubmitInfo existing{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        auto info = f.info(); auto protectedInfo = f.protectedInfo; protectedInfo.pNext = &existing; info.pNext = &protectedInfo;
        GpuCompletionPoint p;
        check(f.tracker.submit(info, VK_NULL_HANDLE, p) != VK_SUCCESS && p.empty(), "existing timeline node rejected even below another pNext");
        info = f.info(); info.pWaitDstStageMask = nullptr;
        check(f.tracker.submit(info, VK_NULL_HANDLE, p) != VK_SUCCESS, "missing wait stages rejected");
        info = f.info(); info.pSignalSemaphores = nullptr;
        check(f.tracker.submit(info, VK_NULL_HANDLE, p) != VK_SUCCESS, "missing signal array rejected");
        info = f.info(); info.pCommandBuffers = nullptr;
        check(f.tracker.submit(info, VK_NULL_HANDLE, p) != VK_SUCCESS, "missing command array rejected");
        info = f.info(); info.pWaitSemaphores = nullptr;
        check(f.tracker.submit(info, VK_NULL_HANDLE, p) != VK_SUCCESS, "missing wait array rejected");
        info = f.info(); info.signalSemaphoreCount = UINT32_MAX;
        check(f.tracker.submit(info, VK_NULL_HANDLE, p) != VK_SUCCESS, "signal count overflow rejected before reading array");
        info = f.info(); VkSemaphore self = f.timeline; info.pSignalSemaphores = &self; info.signalSemaphoreCount = 1;
        check(f.tracker.submit(info, VK_NULL_HANDLE, p) != VK_SUCCESS, "self signal duplication rejected");
        info = f.info(); info.pWaitSemaphores = &self; info.waitSemaphoreCount = 1;
        check(f.tracker.submit(info, VK_NULL_HANDLE, p) != VK_SUCCESS && f.state.submits == before, "self wait rejected with no extra submissions");
    }
    {
        Fixture f; f.configure(); auto p = f.submit();
        f.counterFn = &replacementCounter; f.state.counter = 1;
        check(f.tracker.isComplete(p) && f.state.replacements == 1, "counter PFN read by address");
        f.submitFn = &replacementSubmit; p = f.submit();
        check(f.state.replacements == 2, "submit PFN read by address");
        f.waitFn = &replacementWait; f.state.waitResult = VK_SUCCESS;
        check(f.tracker.wait(p, 5) == VK_SUCCESS && f.state.replacements == 3, "wait PFN read by address");
        p = f.submit(); f.counterFn = nullptr;
        check(!f.tracker.isComplete(p), "removed counter PFN rejected safely");
        f.waitFn = nullptr;
        check(f.tracker.wait(p, 5) != VK_SUCCESS, "removed wait PFN rejected safely");
    }
    for (int entry = 0; entry < 4; ++entry) {
        Fixture f; f.configure(); auto p = f.submit(f.fence);
        if (entry == 0) { f.state.submitResult = VK_ERROR_DEVICE_LOST; GpuCompletionPoint out; check(f.tracker.submit(f.info(), VK_NULL_HANDLE, out) == VK_ERROR_DEVICE_LOST, "submit device loss"); }
        if (entry == 1) { f.state.counterResult = VK_ERROR_DEVICE_LOST; check(!f.tracker.isComplete(p), "counter device loss"); }
        if (entry == 2) { f.state.waitResult = VK_ERROR_DEVICE_LOST; check(f.tracker.wait(p, 0) == VK_ERROR_DEVICE_LOST, "wait device loss"); }
        if (entry == 3) { f.state.statusResult = VK_ERROR_DEVICE_LOST; check(f.tracker.observeFenceCompletion(f.fence) == VK_ERROR_DEVICE_LOST, "fence device loss"); }
        f.state.submitResult = f.state.counterResult = f.state.waitResult = f.state.statusResult = VK_SUCCESS; f.state.counter = 1;
        const int before = f.state.calls(); GpuCompletionPoint out;
        check(!f.tracker.isComplete(p) && f.tracker.wait(p, 0) == VK_ERROR_DEVICE_LOST &&
                  f.tracker.submit(f.info(), VK_NULL_HANDLE, out) == VK_ERROR_DEVICE_LOST && out.empty() &&
                  f.state.calls() == before, "device loss stays terminal even when driver recovers");
    }
    {
        Fixture f; f.state.counterResult = VK_ERROR_DEVICE_LOST;
        check(f.tracker.configureTimeline(f.timeline, 1) == VK_ERROR_DEVICE_LOST, "configuration device loss");
        f.state.counterResult = VK_SUCCESS;
        const int before = f.state.calls();
        check(f.tracker.configureTimeline(f.timeline, 1) == VK_ERROR_DEVICE_LOST && f.state.calls() == before, "configuration cannot revive device loss");
    }
    {
        Fixture f; f.configure(); auto p = f.submit(f.fence); int before = f.state.calls();
        f.tracker.shutdown(); GpuCompletionPoint out;
        check(!f.tracker.isComplete(p) && f.tracker.wait(p, 0) != VK_SUCCESS &&
                  f.tracker.submit(f.info(), VK_NULL_HANDLE, out) != VK_SUCCESS &&
                  f.tracker.observeFenceCompletion(f.fence) == VK_SUCCESS && f.state.calls() == before,
              "shutdown clears bookkeeping, calls no Vulkan and proves no GPU completion");
    }
    std::printf("timeline tracker contracts: failures=%u\n", failures);
    return failures ? 1 : 0;
}
