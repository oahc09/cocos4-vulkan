// Test-only measurement mode, included after wsi_smoke's window/pass helpers.
// No extra submits or per-frame waits. Timestamp queries span the user clear CB,
// not presentation or the engine's transport CBs. Polling completion for occupancy
// can update the tracker's completion cache; its cost is included in both runs.
#pragma once
#include "gfx-vulkan/VKQueue.h"
#include "benchmark_wait_phase.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <vector>

namespace {
struct BenchmarkOptions {
    bool enabled = false;
    bool validation = true;
    uint32_t warmup = 120;
    uint32_t frames = 600;
    std::string csv;
    bool presentProfile = false;
    std::string presentFault;
    bool waitProfile = false;
    std::string waitFault;
};
bool benchmarkOptions(int argc, char **argv, BenchmarkOptions &o) {
    bool seenProfile = false, seenFault = false, seenWaitProfile = false, seenWaitFault = false;
    for (int i = 1; i < argc; ++i) {
        std::string flag = argv[i];
        if (flag == "--benchmark") { o.enabled = true; continue; }
        if (i + 1 == argc) return false;
        std::string value = argv[++i];
        if (flag == "--csv") { o.csv = value; continue; }
        if (flag == "--validation") {
            if (value != "on" && value != "off") return false;
            o.validation = value == "on"; continue;
        }
        if (flag == "--present-profile") {
            if (seenProfile) return false;
            seenProfile = true;
            if (value != "on" && value != "off") return false;
            o.presentProfile = value == "on"; continue;
        }
        if (flag == "--present-profile-fault") {
            if (seenFault) return false;
            seenFault = true;
            if (value != "identity" && value != "none") return false;
            o.presentFault = value; continue;
        }
        if (flag == "--wait-profile") {
            if (seenWaitProfile || (value != "on" && value != "off")) return false;
            seenWaitProfile = true;
            o.waitProfile = value == "on"; continue;
        }
        if (flag == "--wait-profile-fault") {
            if (seenWaitFault || (value != "identity" && value != "none")) return false;
            seenWaitFault = true;
            o.waitFault = value; continue;
        }
        if (flag != "--frames" && flag != "--warmup") return false;
        if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) return false;
        const auto number = std::strtoull(value.c_str(), nullptr, 10);
        if (number < 1 || number > 100000) return false;
        if (flag == "--frames") o.frames = static_cast<uint32_t>(number);
        else o.warmup = static_cast<uint32_t>(number);
    }
    if (o.presentFault == "identity" && (!o.enabled || !o.presentProfile)) return false;
    if (o.waitFault == "identity" && (!o.enabled || !o.waitProfile)) return false;
    return argc == 1 || (o.enabled && !o.csv.empty());
}
using BenchClock = std::chrono::steady_clock;
struct BenchWaitTotals { uint64_t calls = 0; double ms = 0; };
BenchWaitTotals benchWaits;
BenchWaitPhaseRecorder benchWaitPhase;
CCVKGPUSwapchain *benchWaitSwapchain = nullptr;
uint32_t benchWaitDepth = 0;
PFN_vkAcquireNextImageKHR benchRealAcquire = nullptr;
struct WaitFrameScope {
    std::shared_ptr<CCVKGPUWSIGeneration> generationOwner;
    bool entered = false;
    WaitFrameScope(bool enabled, CCVKGPUSwapchain *swapchain, VkDevice device, bool fault) {
        if (!enabled) return;
        if (benchWaitPhase.isActive()) {
            benchWaitPhase.setPhase(static_cast<BenchWaitPhaseRecorder::Phase>(255));
            check(false, "wait frame scopes must not nest");
            return;
        }
        generationOwner = swapchain ? swapchain->imageGeneration : nullptr;
        const bool shape = generationOwner && generationOwner->presentFences.size() == generationOwner->presentPending.size() &&
                           generationOwner->presentFences.size() <= wait_phase_detail::kMaxFences;
        const void *token = shape ? (fault ? static_cast<const void *>(&benchWaitPhase) : generationOwner.get()) : nullptr;
        check(benchWaitPhase.begin(device, swapchain ? swapchain->vkSwapchain : VK_NULL_HANDLE, token,
              shape ? generationOwner->presentFences.data() : nullptr,
              shape ? generationOwner->presentPending.data() : nullptr,
              shape ? static_cast<uint32_t>(generationOwner->presentFences.size()) : 0), "wait frame snapshot shape");
        benchWaitSwapchain = swapchain;
        entered = true;
    }
    ~WaitFrameScope() {
        if (entered) { benchWaitPhase.end(); benchWaitSwapchain = nullptr; }
    }
    WaitFrameScope(const WaitFrameScope &) = delete;
    WaitFrameScope &operator=(const WaitFrameScope &) = delete;
};
struct WaitCallScope {
    const bool active = benchWaitPhase.isActive();
    WaitCallScope() {
        if (active && benchWaitDepth++ != 0) benchWaitPhase.setPhase(static_cast<BenchWaitPhaseRecorder::Phase>(255));
    }
    ~WaitCallScope() { if (active) --benchWaitDepth; }
    WaitCallScope(const WaitCallScope &) = delete;
    WaitCallScope &operator=(const WaitCallScope &) = delete;
};
VKAPI_ATTR VkResult VKAPI_CALL benchAcquireHook(VkDevice device, VkSwapchainKHR swapchain,
        uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *imageIndex) {
    const bool shape = benchRealAcquire && device && swapchain && imageIndex;
    const VkResult result = shape ? benchRealAcquire(device, swapchain, timeout, semaphore, fence, imageIndex)
                                 : VK_ERROR_INITIALIZATION_FAILED;
    if (benchWaitPhase.isActive()) {
        const bool success = result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
        benchWaitPhase.onAcquire(device, swapchain, result, success && imageIndex ? *imageIndex : UINT32_MAX,
                                benchWaitSwapchain ? benchWaitSwapchain->imageGeneration.get() : nullptr);
        if (!shape) benchWaitPhase.setPhase(static_cast<BenchWaitPhaseRecorder::Phase>(255));
    }
    return result;
}
// Actual counters remain enabled in both modes. Only profile-on adds driver clocks.
struct PresentProfile {
    bool active = false, profiling = false, invalid = false, identityOk = false;
    uint64_t calls = 0, successes = 0, driverFailures = 0;
    uint64_t fenceWaitCalls = 0, timelineWaitCalls = 0, waitFailures = 0;
    double driverMs = 0, hostWaitMs = 0;
    // Three existing completion APIs; no new Vulkan calls are introduced.
    uint64_t fenceStatusCalls = 0, fenceNotReady = 0;
    uint64_t counterCalls = 0, resetFenceCalls = 0, queryFailures = 0;
    double completionApiMs = 0;
    VkDevice expectedDevice = VK_NULL_HANDLE;
    VkQueue expectedQueue = VK_NULL_HANDLE;
    VkSwapchainKHR expectedSwapchain = VK_NULL_HANDLE;
    uint32_t expectedIndex = UINT32_MAX, depth = 0;
};
PresentProfile benchPresent;
PFN_vkWaitForFences benchRealFences = nullptr;
PFN_vkWaitSemaphores benchRealSemaphores = nullptr;
PFN_vkQueuePresentKHR benchRealPresent = nullptr;
// Saved dispatchers forward unchanged inputs/results. Trackers borrow the
// addresses of the global PFN variables and therefore observe these hooks.
PFN_vkGetFenceStatus benchRealGetFenceStatus = nullptr;
PFN_vkGetSemaphoreCounterValue benchRealGetCounterValue = nullptr;
PFN_vkResetFences benchRealResetFences = nullptr;
struct CompletionApiTotals {
    uint64_t fenceStatusCalls = 0, counterCalls = 0, resetFenceCalls = 0;
};
CompletionApiTotals benchCompletionTotals;
enum class CompletionApi { FenceStatus, Counter, ResetFences };
struct CompletionApiScope {
    const bool active = benchPresent.active;
    const bool nested = active && benchPresent.depth != 0;
    const CompletionApi api;
    explicit CompletionApiScope(CompletionApi kind, VkDevice device) : api(kind) {
        switch (api) {
            case CompletionApi::FenceStatus:
                ++benchCompletionTotals.fenceStatusCalls;
                if (active) ++benchPresent.fenceStatusCalls;
                break;
            case CompletionApi::Counter:
                ++benchCompletionTotals.counterCalls;
                if (active) ++benchPresent.counterCalls;
                break;
            case CompletionApi::ResetFences:
                ++benchCompletionTotals.resetFenceCalls;
                if (active) ++benchPresent.resetFenceCalls;
                break;
        }
        if (active) {
            ++benchPresent.depth;
            if (nested || !device || device != benchPresent.expectedDevice) benchPresent.invalid = true;
        }
    }
    ~CompletionApiScope() { if (active) --benchPresent.depth; }
    CompletionApiScope(const CompletionApiScope &) = delete;
    CompletionApiScope &operator=(const CompletionApiScope &) = delete;
    template <class Forward>
    VkResult forward(bool valid, Forward invoke) {
        VkResult result = VK_ERROR_INITIALIZATION_FAILED;
        if (valid) {
            if (active && benchPresent.profiling && !nested) {
                const auto start = BenchClock::now();
                result = invoke();
                const auto end = BenchClock::now();
                benchPresent.completionApiMs += std::chrono::duration<double, std::milli>(end - start).count();
            } else result = invoke();
        } else if (active) benchPresent.invalid = true;
        if (active) {
            if (api == CompletionApi::FenceStatus && result == VK_NOT_READY) ++benchPresent.fenceNotReady;
            else if (result != VK_SUCCESS) ++benchPresent.queryFailures;
        }
        return result;
    }
};

VKAPI_ATTR VkResult VKAPI_CALL benchGetFenceStatus(VkDevice d, VkFence fence) {
    CompletionApiScope scope(CompletionApi::FenceStatus, d);
    return scope.forward(benchRealGetFenceStatus && d && fence,
                         [&] { return benchRealGetFenceStatus(d, fence); });
}
VKAPI_ATTR VkResult VKAPI_CALL benchGetCounterValue(VkDevice d, VkSemaphore semaphore, uint64_t *value) {
    CompletionApiScope scope(CompletionApi::Counter, d);
    return scope.forward(benchRealGetCounterValue && d && semaphore && value,
                         [&] { return benchRealGetCounterValue(d, semaphore, value); });
}
VKAPI_ATTR VkResult VKAPI_CALL benchResetFences(VkDevice d, uint32_t count, const VkFence *fences) {
    CompletionApiScope scope(CompletionApi::ResetFences, d);
    return scope.forward(benchRealResetFences && d && count && fences,
                         [&] { return benchRealResetFences(d, count, fences); });
}
PFN_vkCreateSwapchainKHR benchRealCreate = nullptr;
VkPresentModeKHR benchPresentMode = VK_PRESENT_MODE_MAX_ENUM_KHR;
struct PresentScope {
    bool entered = false;
    PresentScope(bool profile, VkQueue queue, VkSwapchainKHR sc, uint32_t index,
                 VkDevice device = VK_NULL_HANDLE) {
        if (benchPresent.active) { benchPresent.invalid = true; return; }
        benchPresent = PresentProfile{};
        benchPresent.active = true;
        benchPresent.profiling = profile;
        benchPresent.expectedQueue = queue;
        benchPresent.expectedSwapchain = sc;
        benchPresent.expectedIndex = index;
        benchPresent.expectedDevice = device;
        entered = true;
    }
    ~PresentScope() { if (entered) benchPresent.active = false; }
    PresentScope(const PresentScope &) = delete;
    PresentScope &operator=(const PresentScope &) = delete;
};
VKAPI_ATTR VkResult VKAPI_CALL benchPresentHook(VkQueue q, const VkPresentInfoKHR *info) {
    const bool active = benchPresent.active;
    const bool nested = active && benchPresent.depth != 0;
    if (active) { ++benchPresent.calls; ++benchPresent.depth;
        if (nested) benchPresent.invalid = true; }
    const bool shape = q && info && info->sType == VK_STRUCTURE_TYPE_PRESENT_INFO_KHR &&
        info->swapchainCount && info->pSwapchains && info->pImageIndices &&
        (!info->waitSemaphoreCount || info->pWaitSemaphores);
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    if (benchRealPresent && shape) {
        if (active && benchPresent.profiling && !nested) {
            const auto start = BenchClock::now();
            result = benchRealPresent(q, info);
            const auto end = BenchClock::now();
            benchPresent.driverMs += std::chrono::duration<double, std::milli>(end - start).count();
        } else result = benchRealPresent(q, info);
    }
    if (active) { --benchPresent.depth;
        bool success = shape && result == VK_SUCCESS;
        if (success && info->pResults)
            for (uint32_t i = 0; i < info->swapchainCount; ++i)
                if (info->pResults[i] != VK_SUCCESS) success = false;
        if (success) ++benchPresent.successes;
        else ++benchPresent.driverFailures;
        benchPresent.identityOk = shape && info->swapchainCount == 1 &&
            q == benchPresent.expectedQueue && info->pSwapchains[0] == benchPresent.expectedSwapchain &&
            info->pImageIndices[0] == benchPresent.expectedIndex;
        if (!benchRealPresent || !shape) benchPresent.invalid = true;
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL benchScopedWaitFences(VkDevice d, uint32_t n,
        const VkFence *f, VkBool32 all, uint64_t timeout) {
    WaitCallScope waitCall;
    const bool active = benchPresent.active;
    const bool nested = active && benchPresent.depth != 0;
    if (active) { ++benchPresent.depth; ++benchPresent.fenceWaitCalls;
        if (nested) benchPresent.invalid = true; }
    const auto start = BenchClock::now();
    const auto result = benchRealFences && d && n && f ?
        benchRealFences(d, n, f, all, timeout) : VK_ERROR_INITIALIZATION_FAILED;
    const auto end = BenchClock::now();
    const double ms = std::chrono::duration<double, std::milli>(end - start).count();
    ++benchWaits.calls; benchWaits.ms += ms;
    if (waitCall.active) benchWaitPhase.recordFenceWait(d, n, f, result, ms,
        benchWaitSwapchain ? benchWaitSwapchain->imageGeneration.get() : nullptr,
        benchWaitSwapchain ? benchWaitSwapchain->curImageIndex : UINT32_MAX);
    if (active) { --benchPresent.depth;
        if (result != VK_SUCCESS) ++benchPresent.waitFailures;
        if (benchPresent.profiling && !nested) benchPresent.hostWaitMs += ms; }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL benchScopedWaitSemaphores(VkDevice d,
        const VkSemaphoreWaitInfo *info, uint64_t timeout) {
    WaitCallScope waitCall;
    const bool active = benchPresent.active;
    const bool nested = active && benchPresent.depth != 0;
    if (active) { ++benchPresent.depth; ++benchPresent.timelineWaitCalls;
        if (nested) benchPresent.invalid = true; }
    const auto start = BenchClock::now();
    const bool shape = d && info && info->sType == VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO &&
        info->semaphoreCount && info->pSemaphores && info->pValues;
    const auto result = benchRealSemaphores && shape ?
        benchRealSemaphores(d, info, timeout) : VK_ERROR_INITIALIZATION_FAILED;
    const auto end = BenchClock::now();
    const double ms = std::chrono::duration<double, std::milli>(end - start).count();
    ++benchWaits.calls; benchWaits.ms += ms;
    if (waitCall.active) {
        benchWaitPhase.recordSemaphoreWait(d, result, ms);
        if (!shape || !benchRealSemaphores) benchWaitPhase.setPhase(static_cast<BenchWaitPhaseRecorder::Phase>(255));
    }
    if (active) { --benchPresent.depth;
        if (result != VK_SUCCESS) ++benchPresent.waitFailures;
        if (benchPresent.profiling && !nested) benchPresent.hostWaitMs += ms; }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL benchCreateSwapchain(VkDevice d, const VkSwapchainCreateInfoKHR *info,
                                                   const VkAllocationCallbacks *a, VkSwapchainKHR *sc) {
    auto result = benchRealCreate(d, info, a, sc);
    if (result == VK_SUCCESS) benchPresentMode = info->presentMode;
    return result;
}
struct BenchDispatchTotals {
    uint64_t legacySubmits = 0, modernSubmits = 0, legacyBarriers = 0, modernBarriers = 0;
};
BenchDispatchTotals benchDispatch;
PFN_vkQueueSubmit benchRealSubmit = nullptr;
PFN_vkQueueSubmit2 benchRealSubmit2 = nullptr;
PFN_vkQueueSubmit2KHR benchRealSubmit2KHR = nullptr;
PFN_vkCmdPipelineBarrier benchRealBarrier = nullptr;
PFN_vkCmdPipelineBarrier2 benchRealBarrier2 = nullptr;
PFN_vkCmdPipelineBarrier2KHR benchRealBarrier2KHR = nullptr;
VKAPI_ATTR VkResult VKAPI_CALL benchSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *info, VkFence f) {
    ++benchDispatch.legacySubmits;
    return benchRealSubmit(q, n, info, f);
}
VKAPI_ATTR VkResult VKAPI_CALL benchSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *info, VkFence f) {
    ++benchDispatch.modernSubmits;
    return benchRealSubmit2(q, n, info, f);
}
VKAPI_ATTR VkResult VKAPI_CALL benchSubmit2KHR(VkQueue q, uint32_t n, const VkSubmitInfo2 *info, VkFence f) {
    ++benchDispatch.modernSubmits;
    return benchRealSubmit2KHR(q, n, info, f);
}
VKAPI_ATTR void VKAPI_CALL benchBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
    VkDependencyFlags flags, uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b,
    uint32_t ni, const VkImageMemoryBarrier *i) {
    ++benchDispatch.legacyBarriers;
    benchRealBarrier(cb, src, dst, flags, nm, m, nb, b, ni, i);
}
VKAPI_ATTR void VKAPI_CALL benchBarrier2(VkCommandBuffer cb, const VkDependencyInfo *info) {
    ++benchDispatch.modernBarriers;
    benchRealBarrier2(cb, info);
}
VKAPI_ATTR void VKAPI_CALL benchBarrier2KHR(VkCommandBuffer cb, const VkDependencyInfo *info) {
    ++benchDispatch.modernBarriers;
    benchRealBarrier2KHR(cb, info);
}
struct BenchHooks {
    bool acquireInstalled = false;
    explicit BenchHooks(bool waitProfile = false) {
        benchRealAcquire = vkAcquireNextImageKHR;
        if (waitProfile) { vkAcquireNextImageKHR = benchAcquireHook; acquireInstalled = true; }
        benchRealFences = vkWaitForFences;
        benchRealSemaphores = vkWaitSemaphores;
        benchRealPresent = vkQueuePresentKHR;
        benchRealCreate = vkCreateSwapchainKHR;
        benchRealSubmit = vkQueueSubmit; benchRealSubmit2 = vkQueueSubmit2; benchRealSubmit2KHR = vkQueueSubmit2KHR;
        benchRealBarrier = vkCmdPipelineBarrier; benchRealBarrier2 = vkCmdPipelineBarrier2; benchRealBarrier2KHR = vkCmdPipelineBarrier2KHR;
        vkQueueSubmit = benchSubmit;
        if (benchRealSubmit2) vkQueueSubmit2 = benchSubmit2;
        if (benchRealSubmit2KHR) vkQueueSubmit2KHR = benchSubmit2KHR;
        vkCmdPipelineBarrier = benchBarrier;
        if (benchRealBarrier2) vkCmdPipelineBarrier2 = benchBarrier2;
        if (benchRealBarrier2KHR) vkCmdPipelineBarrier2KHR = benchBarrier2KHR;
        vkWaitForFences = benchScopedWaitFences;
        if (benchRealSemaphores) { vkWaitSemaphores = benchScopedWaitSemaphores; }
        else vkWaitSemaphores = nullptr;
        if (benchRealPresent) vkQueuePresentKHR = benchPresentHook;
        benchRealGetFenceStatus = vkGetFenceStatus;
        benchRealGetCounterValue = vkGetSemaphoreCounterValue;
        benchRealResetFences = vkResetFences;
        if (benchRealGetFenceStatus) vkGetFenceStatus = benchGetFenceStatus;
        if (benchRealGetCounterValue) vkGetSemaphoreCounterValue = benchGetCounterValue;
        if (benchRealResetFences) vkResetFences = benchResetFences;
        vkCreateSwapchainKHR = benchCreateSwapchain;
    }
    ~BenchHooks() {
        if (acquireInstalled) vkAcquireNextImageKHR = benchRealAcquire;
        vkWaitForFences = benchRealFences; vkWaitSemaphores = benchRealSemaphores;
        vkQueuePresentKHR = benchRealPresent;
        vkGetFenceStatus = benchRealGetFenceStatus;
        vkGetSemaphoreCounterValue = benchRealGetCounterValue;
        vkResetFences = benchRealResetFences;
        vkCreateSwapchainKHR = benchRealCreate;
        vkQueueSubmit = benchRealSubmit; vkQueueSubmit2 = benchRealSubmit2; vkQueueSubmit2KHR = benchRealSubmit2KHR;
        vkCmdPipelineBarrier = benchRealBarrier; vkCmdPipelineBarrier2 = benchRealBarrier2; vkCmdPipelineBarrier2KHR = benchRealBarrier2KHR;
    }
};
uint64_t benchThreadTicks() {
    FILETIME create{}, exit{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &create, &exit, &kernel, &user)) {
        fail("GetThreadTimes"); return 0;
    }
    auto ticks = [](FILETIME value) { return (uint64_t(value.dwHighDateTime) << 32U) | value.dwLowDateTime; };
    return ticks(kernel) + ticks(user); // 100 ns; OS granularity may yield zero per frame.
}
struct BenchSample {
    double wallMs = 0, cpuMs = 0, waitMs = 0, gpuMs = 0;
    std::array<double, 5> phases{};
    uint64_t waits = 0;
    uint32_t fences = 0, semaphores = 0, inFlight = 0;
    // present profiling (fixed scalars; residual = parent - driver - waits)
    uint64_t frameCalls = 0, frameSuccesses = 0, frameWaitFailures = 0;
    uint64_t fenceWaitCalls = 0, timelineWaitCalls = 0;
    double frameDriverMs = 0, frameHostWaitMs = 0, frameResidualMs = 0;
    bool frameIdentityOk = false;
    uint64_t fenceStatusCalls = 0, fenceNotReady = 0, counterCalls = 0;
    uint64_t resetFenceCalls = 0, queryFailures = 0;
    double completionApiMs = 0, hostOtherMs = 0;
};
double benchPercentile(std::vector<double> values, double p) {
    std::sort(values.begin(), values.end());
    return values[static_cast<size_t>(std::ceil(p * values.size())) - 1];
}
int runCompletionBenchmark(std::unique_ptr<Device> &owner, const BenchmarkOptions &o) {
    auto &raw = *CCVKDevice::getInstance();
    Device *device = &raw;
    auto *context = raw.gpuContext();
    auto *gpu = raw.gpuDevice();
    auto *queue = device->getQueue();
    auto *gpuQueue = static_cast<CCVKQueue *>(queue)->gpuQueue();
    const bool timeline = gpuQueue->completionTracker->usesTimeline();
    const uint32_t bits = context->queueFamilyProperties[gpuQueue->queueFamilyIndex].timestampValidBits;
    const double period = context->physicalDeviceProperties.limits.timestampPeriod;
    if (!bits || bits > 64 || !(period > 0)) {
        fail("GPU timestamps unavailable; benchmark incomplete"); owner->destroy(); return 1;
    }
    BenchHooks hooks(o.waitProfile);
    TestWindow window = TestWindow::create(640, 480);
    check(window.hwnd != nullptr, "benchmark window");
    SwapchainInfo info;
    info.windowHandle = window.hwnd; info.width = 640; info.height = 480; info.vsyncMode = VsyncMode::OFF;
    auto *sc = device->createSwapchain(info);
    check(sc != nullptr, "benchmark swapchain");
    if (!sc) { window.destroy(); owner->destroy(); return 1; }
    PassSet pass;
    buildPass(device, sc, pass);
    CommandBufferInfo ci; ci.queue = queue;
    auto *cmd = device->createCommandBuffer(ci);
    const uint32_t total = o.warmup + o.frames;
    VkQueryPool queries = VK_NULL_HANDLE;
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = total * 2;
    check(vkCreateQueryPool(gpu->vkDevice, &qi, nullptr, &queries) == VK_SUCCESS, "timestamp pool creation");
    std::vector<BenchSample> samples;
    std::vector<BenchWaitPhaseRecorder::Snapshot> waitSamples;
    if (o.waitProfile) waitSamples.reserve(o.frames);
    std::vector<GpuCompletionPoint> framePoints;
    const uint64_t submitsBefore = gpu->trackedSubmitCount;
    const auto dispatchBefore = benchDispatch;
    const bool submit2 = gpu->runtimePaths.useSubmit2;
    const bool barrier2 = gpu->runtimePaths.useSynchronization2;
    std::printf("benchmark encoders: submit=%s barrier=%s\n", submit2 ? "submit2" : "legacy", barrier2 ? "sync2" : "legacy");
    const uint32_t generation = gs(sc)->generation;
    const Color color{0.12F, 0.20F, 0.32F, 1.F};
    std::printf("benchmark config: backend=%s validation=%s warmup=%u frames=%u effective=%u.%u\n",
                timeline ? "timeline" : "fence", o.validation ? "on" : "off", o.warmup, o.frames,
                VK_API_VERSION_MAJOR(gpu->apiVersionInfo.effectiveDeviceApiVersion), VK_API_VERSION_MINOR(gpu->apiVersionInfo.effectiveDeviceApiVersion));
    std::printf("benchmark device: name=%s driver=%u timestampBits=%u timestampPeriod=%.9f\n",
                context->physicalDeviceProperties.deviceName, context->physicalDeviceProperties.driverVersion, bits, period);
    std::printf("benchmark surface: width=%u height=%u images=%zu slots=%u presentMode=%d maintenance1=%d\n",
                sc->getColorTexture()->getWidth(), sc->getColorTexture()->getHeight(), gs(sc)->swapchainImages.size(),
                gpu->backBufferCount, int(benchPresentMode), int(gpu->capabilities.swapchainMaintenance1.enabled));
    std::printf("benchmark present-profile: mode=%s fault=%s\n",
                o.presentProfile ? "on" : "off", o.presentFault == "identity" ? "identity" : "none");
    if (o.waitProfile) std::printf("benchmark wait-profile: mode=on fault=%s capacity=32\n", o.waitFault == "identity" ? "identity" : "none");
    std::printf("benchmark scope: clear-CB GPU timestamps; frame-flow-thread GetThreadTimes; API waits; point polling included\n");
    for (uint32_t f = 0; f < total && !g_failures && queries; ++f) {
        WaitFrameScope waitFrame(o.waitProfile, gs(sc), gpu->vkDevice, o.waitFault == "identity");
        if (o.waitProfile && !benchWaitPhase.isActive()) break;
        const auto waitBefore = benchWaits;
        const uint64_t cpuStart = benchThreadTicks();
        const auto start = BenchClock::now();
        if (o.waitProfile) benchWaitPhase.setPhase(BenchWaitPhaseRecorder::Phase::Acquire);
        device->acquire(&sc, 1);
        check(gs(sc)->wsi.hasRenderableImage(), "benchmark acquire");
        check(gs(sc)->generation == generation, "benchmark generation must remain stable");
        if (!gs(sc)->wsi.hasRenderableImage()) break;
        const auto afterAcquire = BenchClock::now();
        if (o.waitProfile) benchWaitPhase.setPhase(BenchWaitPhaseRecorder::Phase::Record);
        cmd->begin();
        cmd->customCommand([=](void *native) {
            auto cb = static_cast<VkCommandBuffer>(native);
            vkCmdResetQueryPool(cb, queries, f * 2, 2);
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, f * 2);
        });
        auto *texture = sc->getColorTexture();
        const Rect area{0, 0, texture->getWidth(), texture->getHeight()};
        cmd->beginRenderPass(pass.rp, pass.fbo, area, &color, 1.F, 0);
        cmd->endRenderPass();
        cmd->customCommand([=](void *native) {
            vkCmdWriteTimestamp(static_cast<VkCommandBuffer>(native), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, f * 2 + 1);
        });
        cmd->end();
        const auto oldPoint = gpuQueue->lastCompletion;
        const auto afterRecord = BenchClock::now();
        if (o.waitProfile) benchWaitPhase.setPhase(BenchWaitPhaseRecorder::Phase::Submit);
        queue->submit(&cmd, 1);
        const auto point = gpuQueue->lastCompletion;
        check(point.value > oldPoint.value, "benchmark frame actually submitted");
        const auto afterSubmit = BenchClock::now();
        if (o.waitProfile) benchWaitPhase.setPhase(BenchWaitPhaseRecorder::Phase::Observe);
        framePoints.push_back(point);
        // Count actual frame points, not submittedValue-completedValue (which
        // would count transport/drain submissions as frames).
        framePoints.erase(std::remove_if(framePoints.begin(), framePoints.end(), [&](GpuCompletionPoint p) {
            return gpuQueue->completionTracker->isComplete(p);
        }), framePoints.end());
        const auto objects = raw.completionObjectCounts();
        const auto generationOwner = gs(sc)->imageGeneration;
        BenchSample sample;
        sample.inFlight = static_cast<uint32_t>(framePoints.size());
        sample.fences = objects.fences + static_cast<uint32_t>(generationOwner->presentFences.size());
        sample.semaphores = objects.semaphores + static_cast<uint32_t>(generationOwner->renderFinished.size());
        const auto afterObserve = BenchClock::now();
        if (o.waitProfile) benchWaitPhase.setPhase(BenchWaitPhaseRecorder::Phase::Present);
        const VkSwapchainKHR expectedSwapchain = gs(sc)->vkSwapchain;
        uint32_t expectedIndex = gs(sc)->curImageIndex;
        // Corrupt only the expectation; the actual call and predicate are unchanged.
        if (o.presentFault == "identity") expectedIndex ^= 1U;
        {
            PresentScope scope(o.presentProfile, gpuQueue->vkQueue, expectedSwapchain, expectedIndex, gpu->vkDevice);
            device->present();
        }
        const auto afterPresent = BenchClock::now();
        auto elapsed = [](auto begin, auto end) { return std::chrono::duration<double, std::milli>(end-begin).count(); };
        sample.phases = {elapsed(start,afterAcquire), elapsed(afterAcquire,afterRecord), elapsed(afterRecord,afterSubmit),
                         elapsed(afterSubmit,afterObserve), elapsed(afterObserve,afterPresent)};
        sample.wallMs = elapsed(start,afterPresent);
        sample.cpuMs = double(benchThreadTicks() - cpuStart) / 10000.;
        sample.waitMs = benchWaits.ms - waitBefore.ms;
        sample.waits = benchWaits.calls - waitBefore.calls;
        if (o.waitProfile) {
            const auto &wait = benchWaitPhase.snapshot();
            check(!wait.invalid && wait.identityFailures == 0 && benchWaitDepth == 0,
                  "wait phase identity and non-overlap");
            check(wait.acquireCalls == 1 && wait.acquireSuccesses == 1, "wait profile one real acquire");
            check(wait.totalFailures() == 0 && wait.totalFenceCalls() + wait.totalSemaphoreCalls() == sample.waits &&
                  std::abs(wait.totalMilliseconds() - sample.waitMs) < 1e-8, "wait phases close over actual frame waits");
            check(wait.acquirePresentFenceCalls + wait.acquireOtherFenceCalls == wait.buckets[1].fenceCalls,
                  "acquire wait fence category closure");
            check(wait.buckets[0].fenceCalls == 0 && wait.buckets[0].semaphoreCalls == 0, "no unclassified frame waits");
        }
        check(context->validationErrorCount.load() == 0, "benchmark live validation errors");
        sample.frameCalls = benchPresent.calls;
        sample.frameSuccesses = benchPresent.successes;
        sample.frameDriverMs = benchPresent.driverMs;
        sample.frameHostWaitMs = benchPresent.hostWaitMs;
        sample.frameWaitFailures = benchPresent.waitFailures + benchPresent.driverFailures;
        sample.frameIdentityOk = benchPresent.identityOk;
        sample.fenceWaitCalls = benchPresent.fenceWaitCalls;
        sample.timelineWaitCalls = benchPresent.timelineWaitCalls;
        sample.fenceStatusCalls = benchPresent.fenceStatusCalls;
        sample.fenceNotReady = benchPresent.fenceNotReady;
        sample.counterCalls = benchPresent.counterCalls;
        sample.resetFenceCalls = benchPresent.resetFenceCalls;
        sample.queryFailures = benchPresent.queryFailures;
        sample.completionApiMs = benchPresent.completionApiMs;
        check(benchPresent.calls == 1 && benchPresent.successes == 1 &&
              benchPresent.driverFailures == 0, "present exactly one successful driver call");
        check(benchPresent.identityOk, "present identity chain broken");
        check(benchPresent.waitFailures == 0, "present scope waits succeeded");
        check(sample.queryFailures == 0 && sample.fenceNotReady <= sample.fenceStatusCalls,
              "present completion API results and counters valid");
        check(!benchPresent.invalid && benchPresent.depth == 0, "present scope shape and non-overlap");
        if (o.waitProfile) {
            const auto &wait = benchWaitPhase.snapshot().buckets[5];
            check(wait.fenceCalls == sample.fenceWaitCalls && wait.semaphoreCalls == sample.timelineWaitCalls &&
                  wait.failures == benchPresent.waitFailures && (!o.presentProfile || std::abs(wait.milliseconds - sample.frameHostWaitMs) < 1e-8),
                  "wait phase present matches existing scope counters");
        }
        if (o.presentProfile) {
            sample.frameResidualMs = sample.phases[4] - sample.frameDriverMs - sample.frameHostWaitMs;
            check(std::isfinite(sample.frameDriverMs) && sample.frameDriverMs >= 0 &&
                  std::isfinite(sample.frameHostWaitMs) && sample.frameHostWaitMs >= 0 &&
                  std::isfinite(sample.frameResidualMs) && sample.frameResidualMs >= 0,
                  "present finite nonnegative decomposition");
            sample.hostOtherMs = sample.frameResidualMs - sample.completionApiMs;
            check(std::isfinite(sample.completionApiMs) && sample.completionApiMs >= 0 &&
                  std::isfinite(sample.hostOtherMs) && sample.hostOtherMs >= 0 &&
                  std::abs(sample.completionApiMs + sample.hostOtherMs - sample.frameResidualMs) < 1e-8,
                  "present completion API subpartition finite and closed");
        }
        if (f >= o.warmup) {
            samples.push_back(sample);
            if (o.waitProfile) waitSamples.push_back(benchWaitPhase.snapshot());
        }
    }
    raw.waitAllFences(); // outside measured frames; all raw query references complete.
    check(gpu->trackedSubmitCount - submitsBefore == total, "benchmark exactly one submit per frame");
    const uint64_t legacySubmits = benchDispatch.legacySubmits - dispatchBefore.legacySubmits;
    const uint64_t modernSubmits = benchDispatch.modernSubmits - dispatchBefore.modernSubmits;
    const uint64_t legacyBarriers = benchDispatch.legacyBarriers - dispatchBefore.legacyBarriers;
    const uint64_t modernBarriers = benchDispatch.modernBarriers - dispatchBefore.modernBarriers;
    check((submit2 ? modernSubmits : legacySubmits) == total && (submit2 ? legacySubmits : modernSubmits) == 0,
          "benchmark selected submit encoder forwarded exactly one driver call per frame");
    check((barrier2 ? modernBarriers : legacyBarriers) > 0 && (barrier2 ? legacyBarriers : modernBarriers) == 0,
          "benchmark selected barrier encoder actually used exclusively");
    std::printf("benchmark dispatch: frames=%u legacySubmits=%llu modernSubmits=%llu legacyBarriers=%llu modernBarriers=%llu\n",
                total, static_cast<unsigned long long>(legacySubmits), static_cast<unsigned long long>(modernSubmits),
                static_cast<unsigned long long>(legacyBarriers), static_cast<unsigned long long>(modernBarriers));
    struct QueryResult { uint64_t value, available; };
    std::vector<QueryResult> results(o.frames * 2);
    if (queries && samples.size() == o.frames) {
        const auto result = vkGetQueryPoolResults(gpu->vkDevice, queries, o.warmup * 2, o.frames * 2,
                    results.size() * sizeof(QueryResult), results.data(), sizeof(QueryResult),
                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        check(result == VK_SUCCESS, "timestamps available after actual completion");
        const uint64_t mask = bits == 64 ? UINT64_MAX : (uint64_t(1) << bits) - 1;
        for (size_t i = 0; i < samples.size(); ++i) {
            check(results[i*2].available && results[i*2+1].available, "timestamp pair availability");
            samples[i].gpuMs = double((results[i*2+1].value - results[i*2].value) & mask) * period / 1e6;
        }
    } else fail("benchmark incomplete: sampled=%zu expected=%u", samples.size(), o.frames);
    std::ofstream csv(o.csv);
    check(bool(csv), "benchmark CSV file opened");
    csv << "frame,wall_ms,cpu_ms,wait_calls,wait_ms,fence_objects,semaphore_objects,gpu_clear_ms,in_flight\n" << std::fixed << std::setprecision(9);
    std::ofstream phaseCSV(o.csv + ".phases.csv");
    check(bool(phaseCSV), "benchmark phase CSV file opened");
    phaseCSV << "frame,acquire_ms,record_ms,submit_ms,observe_ms,present_ms\n" << std::fixed << std::setprecision(9);
    std::array<std::vector<double>, 5> phases;
    std::vector<double> wall, cpu, waits, waitMs, fences, semaphores, gpuMs, occupancy;
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto &s = samples[i];
        csv << i << ',' << s.wallMs << ',' << s.cpuMs << ',' << s.waits << ',' << s.waitMs << ',' << s.fences << ',' << s.semaphores << ',' << s.gpuMs << ',' << s.inFlight << '\n';
        phaseCSV << i;
        double phaseSum = 0;
        for (size_t k=0; k<s.phases.size(); ++k) {
            phaseCSV << ',' << s.phases[k]; phases[k].push_back(s.phases[k]); phaseSum += s.phases[k];
        }
        phaseCSV << '\n';
        check(std::abs(phaseSum - s.wallMs) < 0.00000001, "benchmark phases partition frame wall time");
        wall.push_back(s.wallMs); cpu.push_back(s.cpuMs); waits.push_back(double(s.waits)); waitMs.push_back(s.waitMs);
        fences.push_back(s.fences); semaphores.push_back(s.semaphores); gpuMs.push_back(s.gpuMs); occupancy.push_back(s.inFlight);
    }
    csv.close(); check(bool(csv), "benchmark CSV write completed");
    phaseCSV.close(); check(bool(phaseCSV), "benchmark phase CSV write completed");
    auto summary = [](const char *name, const std::vector<double> &v, const char *prefix = "metric") {
        if (v.empty()) return;
        double sum = 0; for (double x : v) sum += x;
        std::printf("benchmark %s: %s mean=%.9f p50=%.9f p95=%.9f p99=%.9f max=%.9f\n",
                    prefix, name, sum/v.size(), benchPercentile(v,.50), benchPercentile(v,.95), benchPercentile(v,.99), *std::max_element(v.begin(),v.end()));
    };
    summary("wall_ms", wall); summary("cpu_ms", cpu); summary("wait_calls", waits); summary("wait_ms", waitMs);
    summary("fence_objects", fences); summary("semaphore_objects", semaphores); summary("gpu_clear_ms", gpuMs); summary("in_flight", occupancy);
    const char *phaseNames[] = {"acquire_ms","record_ms","submit_ms","observe_ms","present_ms"};
    for (size_t k=0; k<phases.size(); ++k) summary(phaseNames[k], phases[k], "phase");
    // present sidecar (one header + frames rows; profile off -> NA)
    {
        std::ofstream pcsv(o.csv + ".present.csv");
        check(bool(pcsv), "present sidecar opened");
        pcsv << "frame,profile,present_calls,present_successes,fence_wait_calls,timeline_wait_calls,wait_failures,driver_ms,host_wait_ms,host_residual_ms\n" << std::fixed << std::setprecision(9);
        for (size_t i = 0; i < samples.size(); ++i) {
            const auto &smp = samples[i];
            pcsv << i << "," << (o.presentProfile ? "on" : "off") << "," << smp.frameCalls << "," << smp.frameSuccesses << "," << smp.fenceWaitCalls << "," << smp.timelineWaitCalls << "," << smp.frameWaitFailures << ",";
            if (o.presentProfile)
                pcsv << smp.frameDriverMs << "," << smp.frameHostWaitMs << "," << smp.frameResidualMs << "\n";
            else
                pcsv << "NA,NA,NA\n";
        }
        pcsv.close(); check(bool(pcsv), "present sidecar write completed");
    }
    {
        std::ofstream completionCSV(o.csv + ".completion.csv");
        check(bool(completionCSV), "completion API sidecar opened");
        completionCSV << "frame,profile,fence_status_calls,fence_not_ready,counter_calls,reset_fence_calls,query_failures,completion_api_ms,host_other_ms\n"
                      << std::fixed << std::setprecision(9);
        for (size_t i = 0; i < samples.size(); ++i) {
            const auto &s = samples[i];
            completionCSV << i << ',' << (o.presentProfile ? "on" : "off") << ','
                          << s.fenceStatusCalls << ',' << s.fenceNotReady << ',' << s.counterCalls << ','
                          << s.resetFenceCalls << ',' << s.queryFailures << ',';
            if (o.presentProfile) completionCSV << s.completionApiMs << ',' << s.hostOtherMs << '\n';
            else completionCSV << "NA,NA\n";
        }
        completionCSV.close(); check(bool(completionCSV), "completion API sidecar write completed");
    }
    if (o.waitProfile) {
        check(waitSamples.size() == samples.size(), "wait phase sample alignment");
        std::ofstream waitCSV(o.csv + ".wait-phases.csv");
        check(bool(waitCSV), "wait phases sidecar opened");
        const char *names[] = {"unknown", "acquire", "record", "submit", "observe", "present"};
        waitCSV << "frame";
        for (const char *name : names)
            waitCSV << ',' << name << "_fence_calls," << name << "_semaphore_calls," << name << "_failures," << name << "_wait_ms";
        waitCSV << ",acquire_calls,acquire_successes,acquire_present_fence_calls,acquire_other_fence_calls,acquire_present_fence_ms,identity_failures,invalid,total_wait_calls,total_wait_ms\n"
                << std::fixed << std::setprecision(9);
        for (size_t i = 0; i < waitSamples.size(); ++i) {
            const auto &wait = waitSamples[i];
            waitCSV << i;
            for (const auto &bucket : wait.buckets)
                waitCSV << ',' << bucket.fenceCalls << ',' << bucket.semaphoreCalls << ',' << bucket.failures << ',' << bucket.milliseconds;
            waitCSV << ',' << wait.acquireCalls << ',' << wait.acquireSuccesses << ',' << wait.acquirePresentFenceCalls << ','
                    << wait.acquireOtherFenceCalls << ',' << wait.acquirePresentFenceMilliseconds << ',' << wait.identityFailures << ','
                    << int(wait.invalid) << ',' << wait.totalFenceCalls() + wait.totalSemaphoreCalls() << ',' << wait.totalMilliseconds() << '\n';
        }
        waitCSV.close(); check(bool(waitCSV), "wait phases sidecar write completed");
    }
    std::printf("completion-profile-dispatch: whole-run fenceStatus=%llu counter=%llu resetFence=%llu\n",
                static_cast<unsigned long long>(benchCompletionTotals.fenceStatusCalls),
                static_cast<unsigned long long>(benchCompletionTotals.counterCalls),
                static_cast<unsigned long long>(benchCompletionTotals.resetFenceCalls));
    if (queries) vkDestroyQueryPool(gpu->vkDevice, queries, nullptr);
    destroyPassSet(pass); sc->destroy(); delete sc; cmd->destroy(); delete cmd;
    window.destroy(); UnregisterClassW(TestWindow::className(), GetModuleHandle(nullptr));
    owner->destroy();
    const auto errors = raw.getLastValidationErrorCount();
    std::printf("completion benchmark completed: frames=%zu failures=%u errors=%u\n", samples.size(), g_failures, errors);
    return g_failures || errors || samples.size() != o.frames ? 1 : 0;
}
} // namespace
