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

#include "RealProjectPerformance.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include "application/ApplicationManager.h"
#include "application/BaseApplication.h"
#include "base/Log.h"
#include "base/threading/MessageQueue.h"
#include "bindings/jswrapper/SeApi.h"
#include "engine/EngineEvents.h"
#include "gfx-agent/DeviceAgent.h"
#include "gfx-vulkan/VKDevice.h"
#include "gfx-vulkan/VKMeasurementProbe.h"
#include "gfx-vulkan/VKPipelineCache.h"
#include "gfx-vulkan/VKQueue.h"
namespace cc::vulkan_test {
namespace {
using namespace gfx;
using Clock = std::chrono::steady_clock;
using Store = VulkanPipelineCachePersistence;
constexpr unsigned LIMIT = 8192;
uint64_t now() { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()); }
uint64_t threadCpu() {
    FILETIME a{}, b{}, k{}, u{};
    if (!GetThreadTimes(GetCurrentThread(), &a, &b, &k, &u)) return 0;
    ULARGE_INTEGER x{}, y{};
    x.LowPart = k.dwLowDateTime;
    x.HighPart = k.dwHighDateTime;
    y.LowPart = u.dwLowDateTime;
    y.HighPart = u.dwHighDateTime;
    return (x.QuadPart + y.QuadPart) * 100;
}
struct Row {
    VkSemaphore acquiredSemaphore = VK_NULL_HANDLE, timelineSemaphore = VK_NULL_HANDLE;
    VkFence submittedFence = VK_NULL_HANDLE;
    uint64_t signalValue = 0, waitValue = 0, timelineWaitNs = 0, fenceWaitNs = 0;
    unsigned slot = 0, slots = 0, image = 0, waitSlot = 0, signals = 0;
    unsigned submitWaits = 0, acquireMatches = 0, timelineWaits = 0, timelineMatches = 0, currentFenceWaits = 0, otherFenceWaits = 0;
    uint64_t mainBegin = 0, mainPeriod = 0, mainActive = 0, mainCpu = 0, present = 0, driver = 0, wait = 0, gpuNs = 0;
    unsigned calls = 0, successes = 0, waitCalls = 0, draws = 0, submits = 0, gpuIntervals = 0;
    uint64_t visibilityCalls = 0, visibilityNs = 0, checkedBuffers = 0, checkedImages = 0;
    uint64_t memoryBarriers = 0, bufferBarriers = 0, imageBarriers = 0;
};
struct Query {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    unsigned frame = LIMIT, index = 0, submitted = 0;
    bool closed = false;
};
struct QueryMap {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    unsigned record = 0;
};
struct State {
    bool probeInstalled = false, trace = false, immediate = false, visibilityTrace = false;
    uint64_t visibilityStart = 0;
    bool visibilityScope = false;
    VulkanMeasurementPresentPolicy previousPresentPolicy = VulkanMeasurementPresentPolicy::Keep;
    bool prepared = false, enabled = false, started = false, installed = false, requested = false, ready = false, closing = false, scope = false, loaded = false;
    std::atomic<unsigned> failures{0};
    unsigned frames = 1000, warmup = 300, fps = 1000, tick = 0, renderFrame = LIMIT, live = 0, post = 0, devices = 0;
    uint64_t entry = 0, previous = 0, beginCpu = 0, presentStart = 0, firstPresent = 0, firstFactory = 0, firstDriver = 0, firstShader = 0, factoryStart = 0, shaderStart = 0;
    uint64_t backendStart = 0, instanceStart = 0, deviceStart = 0, backendNs = 0, instanceNs = 0, deviceNs = 0, cacheNs = 0;
    uint64_t firstMainBegin = 0, firstMainFrame = 0, totalShaderNs = 0, totalFactoryNs = 0, totalDriverNs = 0, maxFactoryNs = 0, maxShaderNs = 0;
    unsigned factories = 0, shaderCompiles = 0, pipelineCalls = 0;
    int displayHz = 0;
    LARGE_INTEGER firstPresentQpc{}, qpcFrequency{};
    Store::Bytes expectedInput;
    bool inputExact = false;
    std::string phase, path;
    size_t initialBytes = 0, savedBytes = 0;
    Store::Identity identity;
    std::array<Row, LIMIT> rows{};
    VkQueryPool queries = VK_NULL_HANDLE;
    uint32_t queryCount = 0, mapCount = 0;
    uint64_t timestampMask = 0;
    double timestampPeriod = 0;
    std::array<Query, 32768> queryRecords{};
    std::array<QueryMap, 128> queryMap{};
    PFN_vkBeginCommandBuffer beginCB = nullptr;
    PFN_vkEndCommandBuffer endCB = nullptr;
    VulkanMeasurementCallback previousProbe = nullptr;
    std::unique_ptr<event::Listener<events::BeforeTick>> before;
    std::unique_ptr<event::Listener<events::AfterTick>> after;
    CCVKGPUContext *context = nullptr;
    PFN_vkQueuePresentKHR present = nullptr;
    PFN_vkWaitForFences fences = nullptr;
    PFN_vkWaitSemaphores semaphores = nullptr;
    PFN_vkCmdDraw draw = nullptr;
    PFN_vkCmdDrawIndexed indexed = nullptr;
    PFN_vkQueueSubmit submit = nullptr;
    PFN_vkQueueSubmit2KHR submit2 = nullptr;
    PFN_vkQueueSubmit2 submit2Core = nullptr;
    PFN_vkCreateGraphicsPipelines graphics = nullptr;
    PFN_vkDestroyDevice destroy = nullptr;
    PFN_vkCreatePipelineCache cache = nullptr;
    PFN_vkCreateSwapchainKHR createSwapchain = nullptr;
    PFN_vkAcquireNextImageKHR acquire = nullptr;
    VkSwapchainKHR acquiredSwapchain = VK_NULL_HANDLE;
    uint32_t acquiredIndex = 0;
    uint32_t width = 0, height = 0, presentMode = 0, images = 0;
} s;
void check(bool b, const char *name) {
    if (!b) {
        ++s.failures;
        CC_LOG_ERROR("project-performance FAIL: %s", name);
    }
}
Row *current() { return s.renderFrame < LIMIT ? &s.rows[s.renderFrame] : nullptr; }
void measurement(VulkanMeasurementKind kind, bool entering, const void *owner) noexcept {
    if (kind == VulkanMeasurementKind::BufferVisibility && !s.visibilityTrace) return;
    const uint64_t value = now();
    auto interval = [&](uint64_t &start, uint64_t &span) {if(entering)start=value;else span=value-start; };
    if (kind == VulkanMeasurementKind::BufferVisibility) {
        if (entering) {
            check(!s.visibilityScope && owner, "non nested visibility measurement");
            s.visibilityScope = true;
            s.visibilityStart = value;
        } else {
            check(s.visibilityScope && owner, "matching visibility measurement");
            if (owner)
                if (auto *r = current()) {
                    const auto &v = *static_cast<const VulkanVisibilityMeasurement *>(owner);
                    ++r->visibilityCalls;
                    r->visibilityNs += value - s.visibilityStart;
                    r->checkedBuffers += v.checkedBuffers;
                    r->checkedImages += v.checkedImages;
                    r->memoryBarriers += v.memoryBarriers;
                    r->bufferBarriers += v.bufferBarriers;
                    r->imageBarriers += v.imageBarriers;
                }
            s.visibilityScope = false;
        }
    } else if (kind == VulkanMeasurementKind::BackendInit)
        interval(s.backendStart, s.backendNs);
    else if (kind == VulkanMeasurementKind::InstanceCreation)
        interval(s.instanceStart, s.instanceNs);
    else if (kind == VulkanMeasurementKind::DeviceCreation)
        interval(s.deviceStart, s.deviceNs);
    else if (kind == VulkanMeasurementKind::Present) {
        if (entering) {
            check(!s.scope, "non nested present");
            s.scope = true;
            s.presentStart = value;
            if (s.trace)
                if (auto *r = current()) {
                    auto *d = CCVKDevice::getInstance()->gpuDevice();
                    r->slot = d->curBackBufferIndex;
                    r->slots = d->backBufferCount;
                }
        } else {
            if (auto *r = current()) r->present += value - s.presentStart;
            s.scope = false;
        }
    } else if (kind == VulkanMeasurementKind::GraphicsFactory) {
        if (entering)
            s.factoryStart = value;
        else {
            auto ns = value - s.factoryStart;
            if (!s.firstFactory) s.firstFactory = ns;
            s.totalFactoryNs += ns;
            s.maxFactoryNs = std::max(s.maxFactoryNs, ns);
            ++s.factories;
        }
    } else if (kind == VulkanMeasurementKind::ShaderCompile) {
        if (entering)
            s.shaderStart = value;
        else {
            auto ns = value - s.shaderStart;
            if (!s.firstShader) s.firstShader = ns;
            s.totalShaderNs += ns;
            s.maxShaderNs = std::max(s.maxShaderNs, ns);
            ++s.shaderCompiles;
        }
    }
}
VKAPI_ATTR VkResult VKAPI_CALL createSwapchain(VkDevice d, const VkSwapchainCreateInfoKHR *i, const VkAllocationCallbacks *a, VkSwapchainKHR *out) {
    auto result = s.createSwapchain(d, i, a, out);
    if (result == VK_SUCCESS && i) {
        s.width = i->imageExtent.width;
        s.height = i->imageExtent.height;
        s.presentMode = i->presentMode;
        s.images = i->minImageCount;
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL acquire(VkDevice d, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, VkFence fence, uint32_t *index) {
    auto result = s.acquire(d, sc, timeout, sem, fence, index);
    if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) && index) {
        s.acquiredSwapchain = sc;
        s.acquiredIndex = *index;
        if (s.trace)
            if (auto *r = current()) {
                r->acquiredSemaphore = sem;
                r->image = *index;
            }
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL present(VkQueue q, const VkPresentInfoKHR *i) {
    const auto b = now();
    const auto result = s.present(q, i);
    const auto e = now();
    if (!s.firstPresent && result == VK_SUCCESS && i && i->swapchainCount == 1 && i->pSwapchains && i->pImageIndices && i->pSwapchains[0] == s.acquiredSwapchain && i->pImageIndices[0] == s.acquiredIndex && (!i->pResults || i->pResults[0] == VK_SUCCESS)) {
        s.firstPresent = e - s.entry;
        QueryPerformanceCounter(&s.firstPresentQpc);
    }
    if (s.scope) {
        auto *r = current();
        if (r) {
            r->driver += e - b;
            ++r->calls;
            bool ok = result == VK_SUCCESS && i && i->swapchainCount == 1 && i->pSwapchains && i->pImageIndices && i->pSwapchains[0] == s.acquiredSwapchain && i->pImageIndices[0] == s.acquiredIndex;
            if (ok && i->pResults) ok = i->pResults[0] == VK_SUCCESS;
            if (ok)
                ++r->successes;
            else
                check(false, "actual present result/payload");
        }
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL fences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t timeout) {
    auto b = now();
    auto result = s.fences(d, n, f, all, timeout);
    if (s.scope)
        if (auto *r = current()) {
            const auto elapsed = now() - b;
            r->wait += elapsed;
            ++r->waitCalls;
            if (s.trace) {
                bool matched = false;
                if (r->submittedFence && f)
                    for (uint32_t k = 0; k < n; ++k) matched |= f[k] == r->submittedFence;
                if (matched)
                    ++r->currentFenceWaits;
                else
                    ++r->otherFenceWaits;
                r->fenceWaitNs += elapsed;
            }
            check(result == VK_SUCCESS, "present fence wait");
        }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL semaphores(VkDevice d, const VkSemaphoreWaitInfo *i, uint64_t timeout) {
    auto b = now();
    auto result = s.semaphores(d, i, timeout);
    if (s.scope)
        if (auto *r = current()) {
            const auto elapsed = now() - b;
            r->wait += elapsed;
            ++r->waitCalls;
            if (s.trace) {
                ++r->timelineWaits;
                r->timelineWaitNs += elapsed;
                r->waitSlot = CCVKDevice::getInstance()->gpuDevice()->curBackBufferIndex;
                const bool shape = i && i->semaphoreCount == 1 && i->pSemaphores && i->pValues;
                if (shape) {
                    r->waitValue = i->pValues[0];
                    r->timelineMatches += r->signals == 1 && i->pSemaphores[0] == r->timelineSemaphore;
                }
                check(shape, "actual single timeline wait payload");
            }
            check(result == VK_SUCCESS, "present semaphore wait");
        }
    return result;
}
VKAPI_ATTR void VKAPI_CALL draw(VkCommandBuffer c, uint32_t a, uint32_t b, uint32_t f, uint32_t g) {
    s.draw(c, a, b, f, g);
    if (auto *r = current()) ++r->draws;
}
VKAPI_ATTR void VKAPI_CALL indexed(VkCommandBuffer c, uint32_t a, uint32_t b, uint32_t f, int32_t g, uint32_t h) {
    s.indexed(c, a, b, f, g, h);
    if (auto *r = current()) ++r->draws;
}
Query *queryFor(VkCommandBuffer cb) {
    for (unsigned i = 0; i < s.mapCount; ++i)
        if (s.queryMap[i].cb == cb) return &s.queryRecords[s.queryMap[i].record];
    return nullptr;
}
VKAPI_ATTR VkResult VKAPI_CALL beginCB(VkCommandBuffer cb, const VkCommandBufferBeginInfo *info) {
    auto result = s.beginCB(cb, info);
    if (result != VK_SUCCESS || !current() || !info || info->pInheritanceInfo || (info->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT)) return result;
    check(s.queryCount < s.queryRecords.size(), "timestamp fixed capacity");
    if (s.queryCount >= s.queryRecords.size()) return result;
    unsigned m = 0;
    for (; m < s.mapCount; ++m)
        if (s.queryMap[m].cb == cb) break;
    if (m == s.mapCount) {
        check(s.mapCount < s.queryMap.size(), "CB map capacity");
        if (s.mapCount >= s.queryMap.size()) return result;
        ++s.mapCount;
    }
    auto &record = s.queryRecords[s.queryCount];
    record = {cb, s.renderFrame, s.queryCount * 2, 0, false};
    s.queryMap[m] = {cb, s.queryCount++};
    vkCmdResetQueryPool(cb, s.queries, record.index, 2);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s.queries, record.index);
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL endCB(VkCommandBuffer cb) {
    auto *r = queryFor(cb);
    if (r && !r->closed) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.queries, r->index + 1);
    auto result = s.endCB(cb);
    if (r && result == VK_SUCCESS) r->closed = true;
    return result;
}
void submittedCB(VkCommandBuffer cb) {
    auto *r = queryFor(cb);
    if (current() && r && r->closed && r->frame == s.renderFrame) {
        ++r->submitted;
        check(r->submitted == 1, "timestamp CB submitted once");
    }
}
void readGpu() {
    if (!s.queries) return;
    auto *d = CCVKDevice::getInstance();
    for (unsigned i = 0; i < s.queryCount; ++i) {
        auto &r = s.queryRecords[i];
        if (!r.submitted) continue;
        uint64_t values[4]{};
        auto result = vkGetQueryPoolResults(d->gpuDevice()->vkDevice, s.queries, r.index, 2, sizeof(values), values, sizeof(uint64_t) * 2, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        check(result == VK_SUCCESS && values[1] == 1 && values[3] == 1, "submitted GPU timestamps available");
        if (result == VK_SUCCESS && values[1] && values[3] && r.frame < LIMIT) {
            auto &row = s.rows[r.frame];
            row.gpuNs += uint64_t(double((values[2] - values[0]) & s.timestampMask) * s.timestampPeriod);
            ++row.gpuIntervals;
        }
    }
    vkDestroyQueryPool(d->gpuDevice()->vkDevice, s.queries, nullptr);
    s.queries = VK_NULL_HANDLE;
}
void signal(Row &r, VkSemaphore semaphore, uint64_t value) {
    if (!value) return;
    ++r.signals;
    r.timelineSemaphore = semaphore;
    r.signalValue = value;
}
void traceSubmit(const VkSubmitInfo &i, VkFence f) {
    if (!s.trace) return;
    auto *r = current();
    if (!r) return;
    r->submittedFence = f;
    r->submitWaits += i.waitSemaphoreCount;
    if (i.pWaitSemaphores)
        for (uint32_t k = 0; k < i.waitSemaphoreCount; ++k) r->acquireMatches += r->acquiredSemaphore && i.pWaitSemaphores[k] == r->acquiredSemaphore;
    auto *node = static_cast<const VkBaseInStructure *>(i.pNext);
    unsigned depth = 0;
    for (; node && depth < 16; node = node->pNext, ++depth)
        if (node->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) {
            const auto *t = reinterpret_cast<const VkTimelineSemaphoreSubmitInfo *>(node);
            const bool shape = t->signalSemaphoreValueCount == i.signalSemaphoreCount && t->pSignalSemaphoreValues && i.pSignalSemaphores;
            check(shape, "actual timeline submit payload");
            if (shape)
                for (uint32_t k = 0; k < i.signalSemaphoreCount; ++k) signal(*r, i.pSignalSemaphores[k], t->pSignalSemaphoreValues[k]);
        }
    check(node == nullptr, "bounded actual submit chain");
}
void traceSubmit2(const VkSubmitInfo2 &i, VkFence f) {
    if (!s.trace) return;
    auto *r = current();
    if (!r) return;
    r->submittedFence = f;
    r->submitWaits += i.waitSemaphoreInfoCount;
    if (i.pWaitSemaphoreInfos)
        for (uint32_t k = 0; k < i.waitSemaphoreInfoCount; ++k) r->acquireMatches += r->acquiredSemaphore && i.pWaitSemaphoreInfos[k].semaphore == r->acquiredSemaphore;
    if (i.pSignalSemaphoreInfos)
        for (uint32_t k = 0; k < i.signalSemaphoreInfoCount; ++k) signal(*r, i.pSignalSemaphoreInfos[k].semaphore, i.pSignalSemaphoreInfos[k].value);
}
VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue q, uint32_t n, const VkSubmitInfo *i, VkFence f) {
    auto result = s.submit(q, n, i, f);
    if (result == VK_SUCCESS) {
        if (auto *r = current()) ++r->submits;
        for (unsigned a = 0; a < n; ++a) {
            traceSubmit(i[a], f);
            for (unsigned b = 0; b < i[a].commandBufferCount; ++b) submittedCB(i[a].pCommandBuffers[b]);
        }
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL submit2(VkQueue q, uint32_t n, const VkSubmitInfo2KHR *i, VkFence f) {
    auto result = s.submit2(q, n, i, f);
    if (result == VK_SUCCESS) {
        if (auto *r = current()) ++r->submits;
        for (unsigned a = 0; a < n; ++a) {
            traceSubmit2(i[a], f);
            for (unsigned b = 0; b < i[a].commandBufferInfoCount; ++b) submittedCB(i[a].pCommandBufferInfos[b].commandBuffer);
        }
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL submit2Core(VkQueue q, uint32_t n, const VkSubmitInfo2 *i, VkFence f) {
    auto result = s.submit2Core(q, n, i, f);
    if (result == VK_SUCCESS) {
        if (auto *r = current()) ++r->submits;
        for (unsigned a = 0; a < n; ++a) {
            traceSubmit2(i[a], f);
            for (unsigned b = 0; b < i[a].commandBufferInfoCount; ++b) submittedCB(i[a].pCommandBufferInfos[b].commandBuffer);
        }
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL graphics(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *i, const VkAllocationCallbacks *a, VkPipeline *out) {
    auto b = now();
    auto result = s.graphics(d, c, n, i, a, out);
    auto elapsed = now() - b;
    s.totalDriverNs += elapsed;
    ++s.pipelineCalls;
    if (!s.firstDriver && result == VK_SUCCESS) s.firstDriver = elapsed;
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL cache(VkDevice d, const VkPipelineCacheCreateInfo *i, const VkAllocationCallbacks *a, VkPipelineCache *out) {
    auto result = s.cache(d, i, a, out);
    if (result == VK_SUCCESS && i) {
        s.initialBytes = i->initialDataSize;
        s.inputExact = i->initialDataSize == s.expectedInput.size() && (s.expectedInput.empty() || (i->pInitialData && std::memcmp(i->pInitialData, s.expectedInput.data(), s.expectedInput.size()) == 0));
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL destroy(VkDevice d, const VkAllocationCallbacks *a) {
    readGpu();
    if (s.context) s.live = s.context->validationErrorCount.load();
    s.destroy(d, a);
    if (s.context) s.post = s.context->validationErrorCount.load();
    s.context = nullptr;
    ++s.devices;
}
void install() {
    auto *d = CCVKDevice::getInstance();
    check(d && d->gpuContext(), "existing device");
    if (!d || !d->gpuContext()) return;
    s.context = d->gpuContext();
    const bool all = s.context->validationEnabled && s.context->synchronizationValidationEnabled && s.context->bestPracticesValidationEnabled;
    const bool none = !s.context->validationEnabled && !s.context->synchronizationValidationEnabled && !s.context->bestPracticesValidationEnabled;
    check(all || none, "validation triad consistent");
    CC_LOG_INFO("project-performance validation: %s", all ? "ENABLED" : (none ? "DISABLED" : "PARTIAL"));
    s.createSwapchain = vkCreateSwapchainKHR;
    s.acquire = vkAcquireNextImageKHR;
    s.beginCB = vkBeginCommandBuffer;
    s.endCB = vkEndCommandBuffer;
    auto family = static_cast<CCVKQueue *>(d->getQueue())->gpuQueue()->queueFamilyIndex;
    const auto bits = s.context->queueFamilyProperties[family].timestampValidBits;
    check(bits > 0 && bits <= 64 && vkCreateQueryPool && vkGetQueryPoolResults && vkCmdResetQueryPool && vkCmdWriteTimestamp, "timestamp support/PFNs");
    if (s.failures) return;
    s.timestampMask = bits == 64 ? ~uint64_t(0) : ((uint64_t(1) << bits) - 1);
    s.timestampPeriod = s.context->physicalDeviceProperties.limits.timestampPeriod;
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = 65536;
    check(vkCreateQueryPool(d->gpuDevice()->vkDevice, &qi, nullptr, &s.queries) == VK_SUCCESS, "timestamp query pool created");
    if (s.failures) return;
    s.present = vkQueuePresentKHR;
    s.fences = vkWaitForFences;
    s.semaphores = vkWaitSemaphores;
    s.draw = vkCmdDraw;
    s.indexed = vkCmdDrawIndexed;
    s.submit = vkQueueSubmit;
    s.submit2 = vkQueueSubmit2KHR;
    s.submit2Core = vkQueueSubmit2;
    s.graphics = vkCreateGraphicsPipelines;
    s.destroy = vkDestroyDevice;
    s.cache = vkCreatePipelineCache;
    check(s.present && s.fences && s.draw && s.indexed && s.submit && s.graphics && s.destroy && s.cache && s.createSwapchain && s.acquire && s.beginCB && s.endCB, "required PFNs");
    if (s.failures) return;
    vkCreateSwapchainKHR = createSwapchain;
    vkAcquireNextImageKHR = acquire;
    vkBeginCommandBuffer = beginCB;
    vkEndCommandBuffer = endCB;
    vkQueuePresentKHR = present;
    vkWaitForFences = fences;
    if (s.semaphores) vkWaitSemaphores = semaphores;
    vkCmdDraw = draw;
    vkCmdDrawIndexed = indexed;
    vkQueueSubmit = submit;
    if (s.submit2) vkQueueSubmit2KHR = submit2;
    if (s.submit2Core) vkQueueSubmit2 = submit2Core;
    vkCreateGraphicsPipelines = graphics;
    vkDestroyDevice = destroy;
    check(getVulkanMeasurementCallback() == measurement, "early measurement callback retained");
    s.installed = true;
    s.identity = Store::identity(s.context->physicalDeviceProperties);
    if (s.phase == "restored") check(Store::read(s.path, s.identity, s.expectedInput) && !s.expectedInput.empty(), "validated restored payload");
    vkCreatePipelineCache = cache;
    auto cacheStart = now();
    const bool configured = d->configurePipelineCacheForPrewarm(s.path.c_str());
    s.cacheNs = now() - cacheStart;
    vkCreatePipelineCache = s.cache;
    check(s.inputExact, "actual driver cache payload exact");
    check(configured, "explicit isolated cache configured");
    if (configured) {
        s.loaded = d->pipelineCache()->loadedFromDisk();
        check(s.phase == "cold" ? !s.loaded && s.initialBytes == 0 : s.loaded && s.initialBytes > 0, "actual cold/restored input");
    }
    CC_LOG_INFO("project-performance cache: phase=%s loaded=%u initialBytes=%zu inputExact=%u", s.phase.c_str(), s.loaded ? 1U : 0U, s.initialBytes, s.inputExact ? 1U : 0U);
}
BOOL CALLBACK closeWindow(HWND h, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h)) {
        char cls[128]{};
        GetClassNameA(h, cls, 128);
        if (std::string(cls) != "ConsoleWindowClass") PostMessageW(h, WM_CLOSE, 0, 0);
    }
    return TRUE;
}
BOOL CALLBACK recordDisplay(HWND h, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h)) {
        char cls[128]{};
        GetClassNameA(h, cls, 128);
        if (std::string(cls) != "ConsoleWindowClass") {
            MONITORINFOEXW m{};
            m.cbSize = sizeof(m);
            DEVMODEW dm{};
            dm.dmSize = sizeof(dm);
            if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &m) && EnumDisplaySettingsW(m.szDevice, ENUM_CURRENT_SETTINGS, &dm)) s.displayHz = int(dm.dmDisplayFrequency);
        }
    }
    return TRUE;
}
void before() {
    if (!s.firstMainBegin) s.firstMainBegin = now();
    if (s.closing) {
        if (auto *a = DeviceAgent::getInstance()) {
            auto *q = a->getMessageQueue();
            ENQUEUE_MESSAGE_0(q, PerformanceStopFrame, { s.renderFrame = LIMIT; });
        } else
            s.renderFrame = LIMIT;
        return;
    }
    auto *js = se::ScriptEngine::getInstance();
    se::Value value;
    if (!s.requested) {
        if (js->evalString("typeof cc !== 'undefined' && !!cc.director && !!cc.director.getScene()", 0, &value) && value.isBoolean() && value.toBoolean()) {
            s.requested = true;
            check(js->evalString("globalThis.__vkPerfReady=false;cc.director.loadScene('atlas-compress',function(e){globalThis.__vkPerfReady=!e;});"), "request actual atlas scene");
        }
        return;
    }
    if (!s.ready) {
        if (js->evalString("globalThis.__vkPerfReady===true && cc.director.getScene().name==='atlas-compress'", 0, &value) && value.isBoolean() && value.toBoolean()) {
            s.ready = true;
            EnumWindows(recordDisplay, 0);
            CC_CURRENT_ENGINE()->setPreferredFramesPerSecond(s.fps);
            CC_LOG_INFO("project-performance scene: name=atlas-compress ready=1 warmup=%u frames=%u", s.warmup, s.frames);
        } else
            return;
    }
    const unsigned i = s.tick;
    check(i < LIMIT, "fixed sample capacity");
    if (i >= LIMIT) return;
    auto t = now();
    auto &r = s.rows[i];
    r.mainBegin = t;
    r.mainPeriod = s.previous ? t - s.previous : 0;
    s.previous = t;
    s.beginCpu = threadCpu();
    if (auto *a = DeviceAgent::getInstance()) {
        auto *q = a->getMessageQueue();
        ENQUEUE_MESSAGE_1(q, PerformanceFrame, i, i, { s.renderFrame = i; });
    } else
        s.renderFrame = i;
}
void after() {
    if (!s.firstMainFrame && s.firstMainBegin) s.firstMainFrame = now() - s.firstMainBegin;
    if (!s.ready || s.closing) return;
    const unsigned i = s.tick;
    if (i >= LIMIT) return;
    auto &r = s.rows[i];
    r.mainActive = now() - r.mainBegin;
    r.mainCpu = threadCpu() - s.beginCpu;
    ++s.tick;
    if (s.tick == s.warmup + s.frames) {
        s.closing = true;
        if (auto *a = DeviceAgent::getInstance()) {
            auto *q = a->getMessageQueue();
            ENQUEUE_MESSAGE_0(q, PerformanceSave, {auto*d=CCVKDevice::getInstance();d->pipelineCache()->saveCache();Store::Bytes bytes;bool valid=Store::read(s.path,s.identity,bytes);check(valid&&!bytes.empty()&&!d->pipelineCache()->isDirty(),"actual validated saved cache");s.savedBytes=bytes.size();s.renderFrame=LIMIT; });
            q->kickAndWait();
        }
        EnumWindows(closeWindow, 0);
    }
}
unsigned number(const char *n, unsigned fallback) {
    auto *v = std::getenv(n);
    if (!v) return fallback;
    std::string t = v;
    if (t.empty() || t.find_first_not_of("0123456789") != std::string::npos) return 0;
    try {
        auto v = std::stoull(t);
        return v < LIMIT ? unsigned(v) : 0;
    } catch (...) {
        return 0;
    }
}
} // namespace
bool preparePerformance() noexcept {
    if (s.prepared) return !s.failures;
    s.prepared = true;
    s.entry = now();
    auto *v = std::getenv("CC_VULKAN_REAL_PROJECT_PERFORMANCE");
    if (!v) return true;
    s.enabled = true;
    s.phase = v;
    if (const auto *mode = std::getenv("CC_VULKAN_REAL_PROJECT_PERF_PRESENT")) {
        s.immediate = std::strcmp(mode, "immediate") == 0;
        check(s.immediate, "known diagnostic present mode");
    }
    if (const auto *trace = std::getenv("CC_VULKAN_REAL_PROJECT_PERF_WAIT_TRACE")) {
        s.trace = std::strcmp(trace, "1") == 0;
        check(s.trace, "known wait trace value");
    }
    if (const auto *trace = std::getenv("CC_VULKAN_REAL_PROJECT_PERF_VISIBILITY_TRACE")) {
        s.visibilityTrace = std::strcmp(trace, "1") == 0;
        check(s.visibilityTrace, "known visibility trace value");
    }
    const auto *api = std::getenv("CC_VULKAN_FORCE_API"), *rendering = std::getenv("CC_VULKAN_RENDERING");
    check(!(api && rendering && (std::string(api) == "1.1" || std::string(api) == "1.2") && std::string(rendering) == "dynamic"), "forced low API conflicts with Dynamic policy");
    check(s.phase == "cold" || s.phase == "restored", "known phase");
    s.fps = number("CC_VULKAN_REAL_PROJECT_PERF_FPS", 1000);
    check(s.fps > 0 && s.fps <= 1000, "bounded FPS");
    s.frames = number("CC_VULKAN_REAL_PROJECT_PERF_FRAMES", 1000);
    s.warmup = number("CC_VULKAN_REAL_PROJECT_PERF_WARMUP", 300);
    check(s.frames > 0 && s.warmup > 0 && s.frames < LIMIT && s.warmup < LIMIT && uint64_t(s.frames) + s.warmup < LIMIT, "bounded positive frame counts");
    auto *path = std::getenv("CC_VULKAN_REAL_PROJECT_PERF_CACHE");
    check(path && *path, "explicit cache path");
    if (path) s.path = path;
    for (auto *n : {"CC_VULKAN_REAL_PROJECT_PREWARM", "CC_VULKAN_REAL_PROJECT_INDIRECT", "CC_VULKAN_REAL_PROJECT_SUBPASS", "CC_VULKAN_REAL_PROJECT_RESOLVE", "CC_VULKAN_REAL_PROJECT_FEATURES", "CC_VULKAN_REAL_PROJECT_WSI", "CC_VULKAN_REAL_PROJECT_RESOURCE_ROOT"}) check(!std::getenv(n), "exclusive diagnostic mode");
    try {
        check(std::filesystem::path(s.path).is_absolute(), "absolute cache path");
        check(std::filesystem::exists(s.path) == (s.phase == "restored"), "cold absence/restored existence");
    } catch (...) {
        check(false, "cache path shape");
    }
    if (!s.failures) {
        QueryPerformanceFrequency(&s.qpcFrequency);
        s.previousProbe = setVulkanMeasurementCallback(measurement);
        if (s.immediate) s.previousPresentPolicy = setVulkanMeasurementPresentPolicy(VulkanMeasurementPresentPolicy::Immediate);
        s.probeInstalled = true;
    }
    return !s.failures;
}
void startPerformance() noexcept {
    if (!s.enabled || s.failures) return;
    try {
        s.started = true;
        if (auto *a = DeviceAgent::getInstance()) {
            auto *q = a->getMessageQueue();
            ENQUEUE_MESSAGE_0(q, PerformanceInstall, { install(); });
            q->kickAndWait();
        } else
            install();
        s.before = std::make_unique<event::Listener<events::BeforeTick>>();
        s.after = std::make_unique<event::Listener<events::AfterTick>>();
        s.before->bind([] { before(); });
        s.after->bind([] { after(); });
    } catch (...) {
        check(false, "setup exception");
    }
}
int finishPerformance(int platformResult) noexcept {
    if (!s.enabled) return platformResult;
    s.before.reset();
    s.after.reset();
    check(s.started && s.closing && s.tick == s.frames + s.warmup, "all requested actual scene frames");
    check(!platformResult && s.devices == 1 && !s.live && !s.post, "actual shutdown validation");
    check(!s.immediate || s.presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR, "requested diagnostic present mode actually selected");
    FILE *f = std::fopen("performance.csv", "wb");
    check(f != nullptr, "CSV open");
    if (f) {
        std::fprintf(f, "frame,main_period_ns,main_active_ns,main_thread_cpu_ns,present_ns,driver_present_ns,present_wait_ns,present_residual_ns,presents,successes,wait_calls,draws,submits,gpu_submitted_cb_ns,gpu_intervals\n");
        for (unsigned j = 0; j < s.frames; ++j) {
            auto &r = s.rows[j + s.warmup];
            check(r.mainPeriod && r.mainActive && r.present && r.calls == 1 && r.successes == 1 && r.draws > 0 && r.submits > 0 && r.gpuIntervals > 0, "actual per-frame completeness");
            check(r.present >= r.driver + r.wait, "present partition closure");
            auto residual = r.present >= r.driver + r.wait ? r.present - r.driver - r.wait : 0;
            std::fprintf(f, "%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%u,%u,%u,%llu,%u\n", j, r.mainPeriod, r.mainActive, r.mainCpu, r.present, r.driver, r.wait, residual, r.calls, r.successes, r.waitCalls, r.draws, r.submits, r.gpuNs, r.gpuIntervals);
        }
        check(std::fflush(f) == 0, "CSV flush");
        check(std::fclose(f) == 0, "CSV close");
    }
    if (s.trace) {
        FILE *trace = std::fopen("performance.wait-identities.csv", "wb");
        check(trace != nullptr, "wait identities CSV open");
        if (trace) {
            std::fprintf(trace, "frame,slot,slots,image,signal_value,signal_count,submit_wait_count,acquire_wait_matches,timeline_wait_calls,timeline_wait_value,timeline_matches,timeline_wait_ns,wait_slot,fence_wait_current,fence_wait_other,fence_wait_ns\n");
            for (unsigned j = 0; j < s.frames; ++j) {
                const auto &r = s.rows[j + s.warmup];
                check(r.signals == 1 && r.signalValue > 0 && r.submitWaits == 1 && r.acquireMatches == 1, "actual acquire-only wait and timeline signal");
                check(r.timelineMatches == r.timelineWaits && r.wait == r.timelineWaitNs + r.fenceWaitNs, "actual timeline identity and wait partition");
                std::fprintf(trace, "%u,%u,%u,%u,%llu,%u,%u,%u,%u,%llu,%u,%llu,%u,%u,%u,%llu\n", j, r.slot, r.slots, r.image, r.signalValue, r.signals, r.submitWaits, r.acquireMatches, r.timelineWaits, r.waitValue, r.timelineMatches, r.timelineWaitNs, r.waitSlot, r.currentFenceWaits, r.otherFenceWaits, r.fenceWaitNs);
            }
            check(std::fflush(trace) == 0, "wait identities CSV flush");
            check(std::fclose(trace) == 0, "wait identities CSV close");
        }
    }
    if (s.visibilityTrace) {
        FILE *trace = std::fopen("performance.visibility.csv", "wb");
        check(trace != nullptr, "visibility CSV open");
        if (trace) {
            std::fprintf(trace, "frame,manager_calls,manager_ns,checked_buffers,checked_images,memory_barriers,buffer_barriers,image_barriers\n");
            for (unsigned j = 0; j < s.frames; ++j) {
                const auto &r = s.rows[j + s.warmup];
                std::fprintf(trace, "%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n", j, r.visibilityCalls, r.visibilityNs, r.checkedBuffers, r.checkedImages, r.memoryBarriers, r.bufferBarriers, r.imageBarriers);
            }
            check(std::fflush(trace) == 0, "visibility CSV flush");
            check(std::fclose(trace) == 0, "visibility CSV close");
        }
    }
    CC_LOG_INFO("project-performance metadata: width=%u height=%u requestedImages=%u presentMode=%u fps=%u displayHz=%d timestampPeriod=%.9g", s.width, s.height, s.images, s.presentMode, s.fps, s.displayHz, s.timestampPeriod);
    CC_LOG_INFO("project-performance first-use: nativeEntryToPresentNs=%llu firstGraphicsFactoryNs=%llu firstShaderCompileNs=%llu firstGraphicsDriverNs=%llu", s.firstPresent, s.firstFactory, s.firstShader, s.firstDriver);
    CC_LOG_INFO("project-performance startup: backendNs=%llu instanceNs=%llu deviceNs=%llu cacheConfigureNs=%llu firstMainFrameNs=%llu totalShaderCompileNs=%llu totalGraphicsFactoryNs=%llu totalGraphicsDriverNs=%llu maxGraphicsFactoryNs=%llu maxShaderCompileNs=%llu factories=%u shaderCompiles=%u pipelineCalls=%u firstPresentQpc=%llu qpcFrequency=%llu", s.backendNs, s.instanceNs, s.deviceNs, s.cacheNs, s.firstMainFrame, s.totalShaderNs, s.totalFactoryNs, s.totalDriverNs, s.maxFactoryNs, s.maxShaderNs, s.factories, s.shaderCompiles, s.pipelineCalls, uint64_t(s.firstPresentQpc.QuadPart), uint64_t(s.qpcFrequency.QuadPart));
    CC_LOG_INFO("project-performance saved: bytes=%zu", s.savedBytes);
    CC_LOG_INFO("project-performance completed: frames=%u failures=%u liveErrors=%u postErrors=%u outcome=%s", s.frames, s.failures.load(), s.live, s.post, s.failures ? "FAILED" : "CLEAN");
    if (s.probeInstalled) {
        if (s.immediate) setVulkanMeasurementPresentPolicy(s.previousPresentPolicy);
        setVulkanMeasurementCallback(s.previousProbe);
        s.probeInstalled = false;
    }
    if (s.installed) {
        vkCreateSwapchainKHR = s.createSwapchain;
        vkAcquireNextImageKHR = s.acquire;
        vkBeginCommandBuffer = s.beginCB;
        vkEndCommandBuffer = s.endCB;
        vkQueuePresentKHR = s.present;
        vkWaitForFences = s.fences;
        if (s.semaphores) vkWaitSemaphores = s.semaphores;
        vkCmdDraw = s.draw;
        vkCmdDrawIndexed = s.indexed;
        vkQueueSubmit = s.submit;
        if (s.submit2) vkQueueSubmit2KHR = s.submit2;
        if (s.submit2Core) vkQueueSubmit2 = s.submit2Core;
        vkCreateGraphicsPipelines = s.graphics;
        vkDestroyDevice = s.destroy;
    }
    return s.failures ? 1 : 0;
}
} // namespace cc::vulkan_test
