// GpuCompletionTracker contract tests (M3-C).
//
// Pure-mock sequence contracts: no Vulkan loader, no engine, no GPU. The mock
// PFN VARIABLES outlive the trackers; fake handles follow platform pointer
// width. These prove the tracker's state machine, NOT any driver behavior.

#include "VKCompletionTracker.h"

#include <cstdio>
#include <cstring>
#include <type_traits>
#include <vector>

namespace {

unsigned g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// ---- mock Vulkan -------------------------------------------------------------
VkResult g_submitResult = VK_SUCCESS;
VkResult g_fenceStatusResult = VK_NOT_READY;
VkResult g_waitResult = VK_TIMEOUT;
int g_submitCalls = 0;
int g_statusCalls = 0;
int g_waitCalls = 0;
bool g_submitSawNullNext = false;
VkQueue g_submittedQueue = VK_NULL_HANDLE;
VkFence g_submittedFence = VK_NULL_HANDLE;
std::vector<VkSemaphore> g_submittedWaits;
std::vector<VkPipelineStageFlags> g_submittedStages;
std::vector<VkCommandBuffer> g_submittedCommands;
std::vector<VkSemaphore> g_submittedSignals;
VkDevice g_waitDevice = VK_NULL_HANDLE;
VkFence g_waitFence = VK_NULL_HANDLE;

VKAPI_ATTR VkResult VKAPI_CALL mockSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *infos, VkFence fence) {
    ++g_submitCalls;
    if (count == 1 && infos) {
        g_submitSawNullNext = infos->pNext == nullptr;
        g_submittedQueue = queue;
        g_submittedFence = fence;
        g_submittedWaits.assign(infos->pWaitSemaphores, infos->pWaitSemaphores + infos->waitSemaphoreCount);
        g_submittedStages.assign(infos->pWaitDstStageMask, infos->pWaitDstStageMask + infos->waitSemaphoreCount);
        g_submittedCommands.assign(infos->pCommandBuffers, infos->pCommandBuffers + infos->commandBufferCount);
        g_submittedSignals.assign(infos->pSignalSemaphores, infos->pSignalSemaphores + infos->signalSemaphoreCount);
    }
    (void)queue;
    (void)fence;
    return g_submitResult;
}
VKAPI_ATTR VkResult VKAPI_CALL replacementLegacySubmit(VkQueue, uint32_t, const VkSubmitInfo *, VkFence) {
    return VK_SUCCESS; // second function: proves Functions are read BY ADDRESS
}
VKAPI_ATTR VkResult VKAPI_CALL mockStatus(VkDevice, VkFence) {
    ++g_statusCalls;
    return g_fenceStatusResult;
}
VKAPI_ATTR VkResult VKAPI_CALL mockWait(VkDevice device, uint32_t count, const VkFence *fences, VkBool32 all, uint64_t timeout) {
    ++g_waitCalls;
    check(count == 1, "mock wait must receive exactly one fence");
    check(all == VK_TRUE, "mock wait must receive VK_TRUE");
    check(timeout == 9999, "mock wait must pass the caller timeout verbatim");
    g_waitDevice = device;
    g_waitFence = count == 1 && fences ? fences[0] : VK_NULL_HANDLE;
    return g_waitResult;
}

struct MockVulkan {
    // PFN variables (addresses are what the tracker stores)
    PFN_vkQueueSubmit submit = &mockSubmit;
    PFN_vkGetFenceStatus status = &mockStatus;
    PFN_vkWaitForFences wait = &mockWait;
    void reset() {
        submit = &mockSubmit;
        status = &mockStatus;
        wait = &mockWait;
        g_submitResult = VK_SUCCESS;
        g_fenceStatusResult = VK_NOT_READY;
        g_waitResult = VK_TIMEOUT;
        g_submitCalls = g_statusCalls = g_waitCalls = 0;
        g_submitSawNullNext = false;
        g_submittedWaits.clear();
        g_submittedStages.clear();
        g_submittedCommands.clear();
        g_submittedSignals.clear();
    }
};

using cc::gfx::GpuCompletionTracker;
using cc::gfx::GpuCompletionPoint;

GpuCompletionTracker::Functions fns(MockVulkan &m) {
    return {&m.submit, &m.status, &m.wait};
}

// Shared fixture: objects OWN the semaphores/CB/stage so lifetimes cover all
// tests (the old basicInfo returned addresses of stack locals).
struct SubmitFixture {
    VkSemaphore waitSem = (VkSemaphore)(void *)(uintptr_t)0x11;
    VkSemaphore signalSem = (VkSemaphore)(void *)(uintptr_t)0x22;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkCommandBuffer cb = (VkCommandBuffer)(void *)(uintptr_t)0x33;

    VkSubmitInfo info() const {
        VkSubmitInfo i{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        i.waitSemaphoreCount = 1;
        i.pWaitSemaphores = &waitSem;
        i.pWaitDstStageMask = &stage;
        i.commandBufferCount = 1;
        i.pCommandBuffers = &cb;
        i.signalSemaphoreCount = 1;
        i.pSignalSemaphores = &signalSem;
        return i;
    }
};
static SubmitFixture g_fixture; // static: outlives every tracker and test

} // namespace

int main() {
    static_assert(!std::is_copy_constructible<GpuCompletionTracker>::value &&
                      !std::is_copy_assignable<GpuCompletionTracker>::value &&
                      !std::is_move_constructible<GpuCompletionTracker>::value &&
                      !std::is_move_assignable<GpuCompletionTracker>::value,
                  "tracker must be non-copyable and non-movable");

    MockVulkan mock;
    auto fakeDevice = (VkDevice)(void *)(uintptr_t)0xD00D;
    auto queueA = (VkQueue)(void *)(uintptr_t)0xA000;
    auto queueB = (VkQueue)(void *)(uintptr_t)0xB000;
    VkFence fence1 = (VkFence)(void *)(uintptr_t)0xF001;
    VkFence fence2 = (VkFence)(void *)(uintptr_t)0xF002;
    VkSubmitInfo info = g_fixture.info();
    GpuCompletionPoint point{};

    // --- 1: successful submit calls the real driver once, publishes a point --
    {
        mock.reset();
        GpuCompletionTracker t(fakeDevice, queueA, fns(mock));
        check(t.submit(info, fence1, point) == VK_SUCCESS, "t1: submit success");
        check(g_submitCalls == 1, "t1: exactly one driver submit call");
        check(point.domain == t.domain() && point.value == 1, "t1: point {domain,1}");
        check(t.submittedValue() == 1, "t1: submitted counter");
        check(g_submittedQueue == queueA && g_submittedFence == fence1 && g_submitSawNullNext,
              "t1: queue/fence/pNext forwarded");
        check(g_submittedWaits == std::vector<VkSemaphore>{g_fixture.waitSem} &&
                  g_submittedStages == std::vector<VkPipelineStageFlags>{g_fixture.stage} &&
                  g_submittedCommands == std::vector<VkCommandBuffer>{g_fixture.cb} &&
                  g_submittedSignals == std::vector<VkSemaphore>{g_fixture.signalSem},
              "t1: wait/stage/command/signal contents forwarded");
        // a pre-set non-empty point must be cleared on failure (checked in t2)
    }

    // --- 2: failed submit publishes nothing; retry keeps values contiguous --
    {
        mock.reset();
        GpuCompletionTracker t(fakeDevice, queueA, fns(mock));
        g_submitResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        point = {123, 456}; // stale non-empty output must be cleared on failure
        check(t.submit(info, fence1, point) == VK_ERROR_OUT_OF_DEVICE_MEMORY, "t2: submit fails");
        check(point.empty(), "t2: failed submit clears the output point");
        check(t.submittedValue() == 0, "t2: no value consumed");
        g_submitResult = VK_SUCCESS;
        check(t.submit(info, fence1, point) == VK_SUCCESS && point.value == 1,
              "t2: retry success keeps value contiguous at 1");
    }

    // --- 3: duplicate pending fence rejected WITHOUT a driver call ----------
    {
        mock.reset();
        GpuCompletionTracker t(fakeDevice, queueA, fns(mock));
        check(t.submit(info, fence1, point) == VK_SUCCESS, "t3: first submit");
        point = {9, 9};
        check(t.submit(info, fence1, point) != VK_SUCCESS, "t3: duplicate pending fence rejected");
        check(point.empty(), "t3: rejected submit clears output point");
        check(g_submitCalls == 1, "t3: no extra driver call for the rejection");
        // NOT_READY observation leaves it pending
        GpuCompletionPoint p1{t.domain(), 1};
        mock.reset();
        g_fenceStatusResult = VK_NOT_READY;
        check(!t.isComplete(p1), "t3: NOT_READY does not complete");
        // after a SUCCESSFUL observation the fence handle may be reused
        g_fenceStatusResult = VK_SUCCESS;
        check(t.isComplete(p1), "t3: SUCCESS observation completes");
        check(t.submit(info, fence1, point) == VK_SUCCESS && point.value == 2,
              "t3: fence handle reusable after completion, next value 2");
        // point completion must NOT be resurrected by handle state going backwards
        g_fenceStatusResult = VK_NOT_READY;
        check(t.isComplete(p1), "t3: completed point stays complete regardless of later status");
        GpuCompletionPoint p2{t.domain(), 2};
        check(!t.isComplete(p2), "t3: new point not complete while its fence is NOT_READY");
    }

    // --- 4: wait semantics: TIMEOUT/NOT_READY do not complete; SUCCESS does --
    {
        mock.reset();
        GpuCompletionTracker t(fakeDevice, queueA, fns(mock));
        check(t.submit(info, fence1, point) == VK_SUCCESS, "t4: submit");
        GpuCompletionPoint p1{t.domain(), 1};
        g_waitResult = VK_TIMEOUT;
        check(t.wait(p1, 9999) == VK_TIMEOUT, "t4: wait TIMEOUT propagates");
        check(g_statusCalls == 0 && g_waitCalls >= 1, "t4: observation via wait only (no status calls)");
        check(g_waitDevice == fakeDevice && g_waitFence == fence1, "t4: wait uses the submitted fence and owning device");
        check(t.completedValue() == 0, "t4: TIMEOUT does not advance completion");
        g_waitResult = VK_SUCCESS;
        check(t.wait(p1, 9999) == VK_SUCCESS, "t4: wait SUCCESS completes");
        check(t.completedValue() == 1, "t4: completion cached");
        const int callsAfter = g_waitCalls;
        check(t.wait(p1, 9999) == VK_SUCCESS && g_waitCalls == callsAfter,
              "t4: cached completion skips the driver (no stale-fence query)");
    }

    // --- 5: same-queue accumulation: higher completion covers earlier work --
    {
        mock.reset();
        GpuCompletionTracker t(fakeDevice, queueA, fns(mock));
        check(t.submit(info, fence1, point) == VK_SUCCESS, "t5: submit 1");
        GpuCompletionPoint p1{t.domain(), 1};
        check(t.submit(info, fence2, point) == VK_SUCCESS, "t5: submit 2");
        GpuCompletionPoint p2{t.domain(), 2};
        g_fenceStatusResult = VK_SUCCESS;
        check(t.isComplete(p2), "t5: completing point 2");
        check(t.isComplete(p1) && g_statusCalls <= 1,
              "t5: point 1 covered by accumulated completion without a second query");
        check(t.completedValue() == 2, "t5: accumulated completion never regresses");
    }

    // --- 6: two trackers / two domains never cross-complete ------------------
    {
        mock.reset();
        GpuCompletionTracker a(fakeDevice, queueA, fns(mock));
        GpuCompletionTracker b(fakeDevice, queueB, fns(mock));
        check(a.domain() != b.domain(), "t6: distinct domains");
        check(a.submit(info, fence1, point) == VK_SUCCESS && point.value == 1, "t6: a submit");
        GpuCompletionPoint pa = point;
        GpuCompletionPoint pb{b.domain(), 1}; // same numeric value, other domain
        g_fenceStatusResult = VK_SUCCESS;
        check(a.isComplete(pa), "t6: a completes its own point");
        const int statusBefore = g_statusCalls;
        check(!b.isComplete(pa), "t6: foreign-domain point rejected");
        check(!b.isComplete(pb), "t6: own unsubmitted point rejected");
        check(g_statusCalls == statusBefore, "t6: foreign domain never queries the driver");
        check(!b.isComplete({b.domain() + 1000, 1}), "t6: unknown domain rejected");
        GpuCompletionPoint future{a.domain(), 99};
        check(!a.isComplete(future), "t6: future point rejected");
        check(g_statusCalls == statusBefore, "t6: future point never queries the driver");
    }

    // --- 7: empty {0,0} completes; half-empty is invalid ----------------------
    {
        mock.reset();
        GpuCompletionTracker t(fakeDevice, queueA, fns(mock));
        GpuCompletionPoint none{};
        check(t.isComplete(none), "t7: empty point complete (no work)");
        check(t.wait(none, 9999) == VK_SUCCESS && g_waitCalls == 0, "t7: empty point waits on nothing");
        GpuCompletionPoint half{t.domain(), 0};
        check(!t.isComplete(half), "t7: value 0 in own domain rejected");
        GpuCompletionPoint half2{0, 5};
        check(!t.isComplete(half2), "t7: domain 0 with value rejected");
        check(!t.isComplete({t.domain() + 7, 0}), "t7: other-domain zero-value rejected");
    }

    // --- 8: DEVICE_LOST via each entry point terminates the tracker ----------
    {
        struct Case { const char *name; void (*arm)(); };
        const Case cases[] = {
            {"submit", [] { g_submitResult = VK_ERROR_DEVICE_LOST; }},
            {"status", [] { g_fenceStatusResult = VK_ERROR_DEVICE_LOST; }},
            {"wait", [] { g_waitResult = VK_ERROR_DEVICE_LOST; }},
        };
        for (const Case &c : cases) {
            mock.reset();
            GpuCompletionTracker t(fakeDevice, queueA, fns(mock));
            check(t.submit(info, fence1, point) == VK_SUCCESS, "t8: baseline submit");
            GpuCompletionPoint p{t.domain(), 1};
            c.arm();
            if (std::strcmp(c.name, "submit") == 0) {
                GpuCompletionPoint unused;
                check(t.submit(info, fence2, unused) == VK_ERROR_DEVICE_LOST, "t8: submit device lost");
            } else if (std::strcmp(c.name, "status") == 0) {
                check(!t.isComplete(p), "t8: status device lost");
            } else {
                check(t.wait(p, 9999) == VK_ERROR_DEVICE_LOST, "t8: wait device lost");
            }
            // mock switches back to SUCCESS: tracker must stay terminal
            mock.reset();
            g_fenceStatusResult = VK_SUCCESS;
            g_waitResult = VK_SUCCESS;
            const int submits = g_submitCalls;
            const int status = g_statusCalls;
            GpuCompletionPoint unused;
            check(t.submit(info, fence2, unused) != VK_SUCCESS && g_submitCalls == submits,
                  "t8: terminal tracker submits nothing");
            check(!t.isComplete(p) && g_statusCalls == status,
                  "t8: terminal tracker queries nothing and completes nothing");
            check(t.wait(p, 9999) != VK_SUCCESS, "t8: terminal tracker waits nothing");
        }
    }

    // --- 9: shutdown semantics -------------------------------------------------
    {
        mock.reset();
        GpuCompletionTracker t(fakeDevice, queueA, fns(mock));
        check(t.submit(info, fence1, point) == VK_SUCCESS, "t9: submit before shutdown");
        GpuCompletionPoint p{t.domain(), 1};
        t.shutdown();
        const int calls = g_submitCalls + g_statusCalls + g_waitCalls;
        check(!t.isComplete(p), "t9: shutdown rejects non-empty points");
        check(g_submitCalls + g_statusCalls + g_waitCalls == calls, "t9: shutdown never touches the driver");
        check(t.wait(p, 9999) != VK_SUCCESS, "t9: shutdown rejects waits");
        GpuCompletionPoint none{};
        check(t.isComplete(none), "t9: empty point still complete after shutdown");
        GpuCompletionPoint unused;
        check(t.submit(info, fence2, unused) != VK_SUCCESS, "t9: shutdown rejects submits");
    }

    // --- 10: Functions are read BY ADDRESS (dynamic dispatch preserved) -------
    {
        mock.reset();
        g_submitCalls = 0;
        GpuCompletionTracker t(fakeDevice, queueA, fns(mock));
        check(t.submit(info, fence1, point) == VK_SUCCESS && g_submitCalls == 1,
              "t10: first call went through mockSubmit");
        mock.submit = &replacementLegacySubmit; // swap the VARIABLE after construction
        const int before = g_submitCalls;
        check(t.submit(info, fence2, point) == VK_SUCCESS && g_submitCalls == before,
              "t10: next call must go through the NEW function (address semantics)");
    }

    // --- 11: invalid construction / null dispatch rejected without driver -----
    {
        mock.reset();
        GpuCompletionTracker nullFns(fakeDevice, queueA, GpuCompletionTracker::Functions{});
        GpuCompletionPoint unused;
        check(nullFns.submit(info, fence1, unused) != VK_SUCCESS && g_submitCalls == 0,
              "t11: null dispatch never calls the driver");
        GpuCompletionTracker noFence(fakeDevice, queueA, fns(mock));
        check(noFence.submit(info, VK_NULL_HANDLE, unused) != VK_SUCCESS && g_submitCalls == 0,
              "t11: null fence rejected");
        GpuCompletionTracker noDevice(VK_NULL_HANDLE, queueA, fns(mock));
        check(noDevice.submit(info, fence1, unused) != VK_SUCCESS && g_submitCalls == 0,
              "t11: null device rejected");
        GpuCompletionTracker noQueue(fakeDevice, VK_NULL_HANDLE, fns(mock));
        check(noQueue.submit(info, fence1, unused) != VK_SUCCESS && g_submitCalls == 0,
              "t11: null queue rejected");
    }

    // --- 12: observeFenceCompletion gates pool reset by real status -----------
    {
        mock.reset();
        GpuCompletionTracker t(fakeDevice, queueA, fns(mock));
        check(t.submit(info, fence1, point) == VK_SUCCESS, "t12: submit");
        GpuCompletionPoint p{t.domain(), 1};
        g_fenceStatusResult = VK_NOT_READY;
        check(t.observeFenceCompletion(fence1) == VK_NOT_READY, "t12: NOT_READY observation propagates");
        check(!t.isComplete(p), "t12: NOT_READY observation keeps pending");
        g_fenceStatusResult = VK_SUCCESS;
        t.observeFenceCompletion(fence1);
        check(t.isComplete(p), "t12: SUCCESS observation caches completion");
        // unknown fence: no driver access, VK_SUCCESS no-op
        const int calls = g_statusCalls;
        check(t.observeFenceCompletion(fence2) == VK_SUCCESS && g_statusCalls == calls,
              "t12: unknown fence is a no-op without driver access");
    }

    if (g_failures == 0) {
        std::printf("completion tracker contract tests: all PASS\n");
        return 0;
    }
    std::printf("completion tracker contract tests: %u FAIL\n", g_failures);
    return 1;
}
