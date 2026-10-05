// Event/split-barrier lifetime harness. Five independent cases.
// offscreen, no swapchain. Real engine, hooks observe vkCmdSetEvent/
// vkCmdWaitEvents/vkCmdResetEvent/vkCreateEvent/vkDestroyEvent transparently.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "gfx-vulkan/VKBuffer.h"
#include "gfx-vulkan/VKCommandBuffer.h"
#include "gfx-vulkan/VKDevice.h"
#include "gfx-vulkan/VKQueue.h"
#include "gfx-vulkan/VKGPUObjects.h"
#include "renderer/GFXDeviceManager.h"
#include "platform/FileUtils.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

int cocos_main(int, const char **) { return 0; }
void cocos_destroy() {}
namespace {
unsigned g_failures = 0;
void check(bool ok, const char *w) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", w); ++g_failures; } }

PFN_vkCmdSetEvent g_realSetEvent = nullptr;
PFN_vkCmdWaitEvents g_realWaitEvents = nullptr;
PFN_vkCmdResetEvent g_realResetEvent = nullptr;
PFN_vkResetEvent g_realHostReset = nullptr;
PFN_vkCreateEvent g_realCreateEvent = nullptr;
PFN_vkDestroyEvent g_realDestroyEvent = nullptr;
PFN_vkGetSemaphoreCounterValue g_realCounter = nullptr;
PFN_vkBeginCommandBuffer g_realBeginCB = nullptr;
PFN_vkQueueSubmit g_realSubmit = nullptr;
PFN_vkQueueSubmit2 g_realSubmit2 = nullptr;
std::vector<VkCommandBuffer> g_submittedCBs;
unsigned g_successfulSubmits = 0;
VkCommandBuffer g_lastBegunCB = VK_NULL_HANDLE;

int g_setEventCalls = 0, g_waitEventsCalls = 0, g_resetEventCalls = 0;
int g_createEventCalls = 0, g_destroyEventCalls = 0, g_hostResetCalls = 0;
std::vector<VkEvent> g_setEvents, g_waitEvents, g_deviceResetEvents, g_destroyedEvents, g_createdEvents;

VKAPI_ATTR void VKAPI_CALL hSetEvent(VkCommandBuffer cb, VkEvent e, VkPipelineStageFlags s) {
    ++g_setEventCalls; g_setEvents.push_back(e); g_realSetEvent(cb, e, s);
}
VKAPI_ATTR void VKAPI_CALL hWaitEvents(VkCommandBuffer cb, uint32_t n, const VkEvent *pE, VkPipelineStageFlags s, VkPipelineStageFlags d, uint32_t mbm, const VkMemoryBarrier *pM, uint32_t bmc, const VkBufferMemoryBarrier *pB, uint32_t imc, const VkImageMemoryBarrier *pI) {
    ++g_waitEventsCalls;
    for (uint32_t i = 0; i < n; ++i) g_waitEvents.push_back(pE[i]);
    g_realWaitEvents(cb, n, pE, s, d, mbm, pM, bmc, pB, imc, pI);
}
VKAPI_ATTR void VKAPI_CALL hResetEvent(VkCommandBuffer cb, VkEvent e, VkPipelineStageFlags s) {
    ++g_resetEventCalls; g_deviceResetEvents.push_back(e); g_realResetEvent(cb, e, s);
}
VKAPI_ATTR VkResult VKAPI_CALL hHostReset(VkDevice d, VkEvent e) {
    ++g_hostResetCalls;
    return g_realHostReset(d, e);
}
VKAPI_ATTR VkResult VKAPI_CALL hCreateEvent(VkDevice d, const VkEventCreateInfo *i, const VkAllocationCallbacks *a, VkEvent *e) {
    ++g_createEventCalls; VkResult r = g_realCreateEvent(d, i, a, e);
    if (r == VK_SUCCESS) g_createdEvents.push_back(*e); return r;
}
VKAPI_ATTR void VKAPI_CALL hDestroyEvent(VkDevice d, VkEvent e, const VkAllocationCallbacks *a) {
    ++g_destroyEventCalls; g_destroyedEvents.push_back(e); g_realDestroyEvent(d, e, a);
}
VkSemaphore g_heldTargetSem = VK_NULL_HANDLE;
bool g_heldArmed = false;
uint64_t g_heldPrefix = 0;
unsigned g_maskedQueries = 0;
VKAPI_ATTR VkResult VKAPI_CALL hCounter(VkDevice d, VkSemaphore s, uint64_t *v) {
    if (g_heldArmed && s == g_heldTargetSem) { ++g_maskedQueries; *v = g_heldPrefix; return VK_SUCCESS; }
    return g_realCounter(d, s, v);
}
VKAPI_ATTR VkResult VKAPI_CALL hBeginCB(VkCommandBuffer cb, const VkCommandBufferBeginInfo *p) {
    const auto result = g_realBeginCB(cb, p);
    if (result == VK_SUCCESS) g_lastBegunCB = cb;
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL hSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *infos, VkFence fence) {
    const auto result = g_realSubmit(q, n, infos, fence);
    if (result == VK_SUCCESS) {
        ++g_successfulSubmits;
        for (uint32_t i = 0; i < n; ++i)
            for (uint32_t j = 0; j < infos[i].commandBufferCount; ++j)
                g_submittedCBs.push_back(infos[i].pCommandBuffers[j]);
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL hSubmit2(VkQueue q,uint32_t n,const VkSubmitInfo2 *infos,VkFence fence) {
    const auto result=g_realSubmit2(q,n,infos,fence);
    if(result==VK_SUCCESS){
        ++g_successfulSubmits;
        for(uint32_t i=0;i<n;++i)for(uint32_t j=0;j<infos[i].commandBufferInfoCount;++j)
            g_submittedCBs.push_back(infos[i].pCommandBufferInfos[j].commandBuffer);
    }
    return result;
}
struct HookRAII {
    HookRAII() {
        g_realSetEvent = vkCmdSetEvent; vkCmdSetEvent = &hSetEvent;
        g_realWaitEvents = vkCmdWaitEvents; vkCmdWaitEvents = &hWaitEvents;
        g_realResetEvent = vkCmdResetEvent; vkCmdResetEvent = &hResetEvent;
        g_realHostReset = vkResetEvent; vkResetEvent = &hHostReset;
        g_realCreateEvent = vkCreateEvent; vkCreateEvent = &hCreateEvent;
        g_realDestroyEvent = vkDestroyEvent; vkDestroyEvent = &hDestroyEvent;
        g_realCounter = vkGetSemaphoreCounterValue;
        g_realBeginCB = vkBeginCommandBuffer; vkBeginCommandBuffer = &hBeginCB;
        g_realSubmit = vkQueueSubmit; vkQueueSubmit = &hSubmit;
        g_realSubmit2=vkQueueSubmit2;if(g_realSubmit2)vkQueueSubmit2=&hSubmit2;
    }
    ~HookRAII() {
        vkCmdSetEvent = g_realSetEvent; vkCmdWaitEvents = g_realWaitEvents;
        vkResetEvent = g_realHostReset;
        vkCmdResetEvent = g_realResetEvent; vkCreateEvent = g_realCreateEvent;
        vkDestroyEvent = g_realDestroyEvent; vkGetSemaphoreCounterValue = g_realCounter;
        vkBeginCommandBuffer = g_realBeginCB; vkQueueSubmit = g_realSubmit;
        vkQueueSubmit2=g_realSubmit2;
        g_heldArmed = false;
    }
    static void reset() {
        g_setEventCalls = g_waitEventsCalls = g_resetEventCalls = 0;
        g_createEventCalls = g_destroyEventCalls = g_hostResetCalls = 0;
        g_setEvents.clear(); g_waitEvents.clear(); g_deviceResetEvents.clear();
        g_destroyedEvents.clear(); g_createdEvents.clear();
        g_lastBegunCB = VK_NULL_HANDLE; g_submittedCBs.clear(); g_successfulSubmits = 0;
    }
};
} // namespace
using namespace cc; using namespace cc::gfx;
static constexpr uint32_t kSz = 64;
static constexpr uint8_t kPat = 0xA5;

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0); std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::string cn;
    for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], "--case") == 0 && i + 1 < argc) cn = argv[++i];
    if (cn.empty() || (cn != "control-full" && cn != "split-buffer-pair" && cn != "held-observation" && cn != "unmatched-split-end" && cn != "event-reuse")) {
        std::fprintf(stderr, "usage: %s --case <control-full|split-buffer-pair|held-observation|unmatched-split-end|event-reuse>\n", argv[0]); return 2;
    }
    auto outDir = std::filesystem::current_path() / ("event-life-" + cn);
    std::filesystem::create_directories(outDir);
    std::unique_ptr<FileUtils> fu(createFileUtils());
    fu->setWritablePath(outDir.generic_string() + "/");
    std::fprintf(stderr, "Creating device\n");
    std::unique_ptr<Device> od(DeviceManager::create());
    if (!od || od->getGfxAPI() != API::VULKAN) { std::fprintf(stderr, "FAIL: init\n"); if (od) od->destroy(); return 1; }
    CCVKDevice &raw = *CCVKDevice::getInstance();
    Device *backend = &raw;
    auto *ctx = raw.gpuContext();
    bool val = ctx->validationEnabled && ctx->bestPracticesValidationEnabled && ctx->synchronizationValidationEnabled;
    std::printf("validation + best practices + sync: %s\n", val ? "ENABLED" : "NOT VERIFIED");
    if (!val) { od->destroy(); return 1; }
    backend->enableAutoBarrier(false);
    auto *queue = backend->getQueue();
    auto errCnt = [&raw] { return raw.gpuContext()->validationErrorCount.load(std::memory_order_relaxed); };
    HookRAII hooks;

    BufferInfo bi; bi.usage = BufferUsageBit::TRANSFER_SRC | BufferUsageBit::TRANSFER_DST;
    bi.memUsage = MemoryUsageBit::DEVICE; bi.size = kSz; bi.stride = 4;
    Buffer *bufA = backend->createBuffer(bi);
    BufferInfo hi; hi.usage = BufferUsageBit::TRANSFER_DST; hi.memUsage = MemoryUsageBit::HOST;
    hi.size = kSz; hi.stride = 4;
    Buffer *host = backend->createBuffer(hi);
    uint8_t data[kSz]; memset(data, kPat, kSz);
    CommandBufferInfo cbInfo; cbInfo.queue = queue;
    CommandBuffer *cmd = backend->createCommandBuffer(cbInfo);
    Buffer *bufB = nullptr; // scoped for event-reuse case (goto-safety)

    // Device-owned FULL barrier (TRANSFER_WRITE → TRANSFER_READ)
    BufferBarrierInfo fullInfo;
    fullInfo.prevAccesses = AccessFlagBit::TRANSFER_WRITE;
    fullInfo.nextAccesses = AccessFlagBit::TRANSFER_READ;
    fullInfo.type = BarrierType::FULL; fullInfo.offset = 0; fullInfo.size = kSz;
    BufferBarrier *fullBarrier = backend->getBufferBarrier(fullInfo);
    // SPLIT_BEGIN / SPLIT_END pair
    BufferBarrierInfo beginInfo = fullInfo; beginInfo.type = BarrierType::SPLIT_BEGIN;
    BufferBarrierInfo endInfo = fullInfo; endInfo.type = BarrierType::SPLIT_END;
    BufferBarrier *beginBarrier = backend->getBufferBarrier(beginInfo);
    BufferBarrier *endBarrier = backend->getBufferBarrier(endInfo);

    const uint32_t e0 = errCnt();
    std::string outcome = "UNSET";

    // The engine barrier supplies A dependency; custom commands only copy and make HOST writes visible.
    auto recordReadbackCC = [&](void *vkCB) {
        auto cb = static_cast<VkCommandBuffer>(vkCB);
        auto aVk = static_cast<CCVKBuffer *>(bufA)->gpuBuffer()->vkBuffer;
        auto hVk = static_cast<CCVKBuffer *>(host)->gpuBuffer()->vkBuffer;
        VkBufferCopy cp{0, 0, kSz};
        vkCmdCopyBuffer(cb, aVk, hVk, 1, &cp);
        VkBufferMemoryBarrier bH{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        bH.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bH.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        bH.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bH.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bH.buffer = hVk; bH.offset = 0; bH.size = kSz;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bH, 0, nullptr);
    };

    if (cn == "control-full" || cn == "unmatched-split-end") {
        HookRAII::reset();
        cmd->begin();
        cmd->updateBuffer(bufA, data, kSz);
        BufferBarrier *bs[] = {cn == "unmatched-split-end" ? endBarrier : fullBarrier};
        Buffer *bufs[] = {bufA};
        cmd->pipelineBarrier(nullptr, bs, bufs, 1, nullptr, nullptr, 0);
        cmd->customCommand(recordReadbackCC);
        cmd->end();
        queue->submit(reinterpret_cast<CommandBuffer **>(&cmd), 1);
        backend->present();
        raw.waitAllFences();
        auto *hGPU = static_cast<CCVKBuffer *>(host)->gpuBuffer();
        check(hGPU->instanceSize == 0 && hGPU->mappedData, "readback must have one mapped instance");
        check(vmaInvalidateAllocation(raw.gpuDevice()->memoryAllocator, hGPU->vmaAllocation, 0, kSz) == VK_SUCCESS, "readback invalidate after completion");
        const uint8_t *m = hGPU->mappedData;
        int mm = 0;
        for (uint32_t i = 0; i < kSz; ++i) if (m[i] != kPat) ++mm;
        std::printf("control: mismatches=%d/%u first=0x%02x\n", mm, kSz, m[0]);
        check(mm == 0, "control: 64-byte 0xA5 verification");
        check(g_setEventCalls == 0 && g_waitEventsCalls == 0 && g_resetEventCalls == 0, "FULL or unmatched split-end must not use events");
        outcome = mm == 0 ? "CLEAN" : "MISMATCH";
    } else if (cn == "split-buffer-pair") {
        HookRAII::reset();
        cmd->begin();
        cmd->updateBuffer(bufA, data, kSz);
        // SPLIT_BEGIN then SPLIT_END on same buffer
        BufferBarrier *bArr[] = {beginBarrier};
        Buffer *bBufs[] = {bufA};
        cmd->pipelineBarrier(nullptr, bArr, bBufs, 1, nullptr, nullptr, 0);
        BufferBarrier *eArr[] = {endBarrier};
        cmd->pipelineBarrier(nullptr, eArr, bBufs, 1, nullptr, nullptr, 0);
        cmd->customCommand(recordReadbackCC);
        cmd->end();
        queue->submit(reinterpret_cast<CommandBuffer **>(&cmd), 1);
        backend->present();
        raw.waitAllFences();
        auto *hGPU = static_cast<CCVKBuffer *>(host)->gpuBuffer();
        check(hGPU->instanceSize == 0 && hGPU->mappedData, "readback must have one mapped instance");
        check(vmaInvalidateAllocation(raw.gpuDevice()->memoryAllocator, hGPU->vmaAllocation, 0, kSz) == VK_SUCCESS, "readback invalidate after completion");
        const uint8_t *m = hGPU->mappedData;
        int mm = 0;
        for (uint32_t i = 0; i < kSz; ++i) if (m[i] != kPat) ++mm;
        std::printf("split: setEvent=%d waitEvents=%d resetEvent=%d createEvent=%d destroyEvent=%d\n",
                    g_setEventCalls, g_waitEventsCalls, g_resetEventCalls, g_createEventCalls, g_destroyEventCalls);
        std::printf("split: mismatches=%d/%u first=0x%02x\n", mm, kSz, m[0]);
        check(g_setEventCalls > 0, "split: vkCmdSetEvent must be called (SPLIT_BEGIN)");
        check(g_waitEventsCalls > 0, "split: vkCmdWaitEvents must be called (SPLIT_END)");
        check(g_resetEventCalls == 1, "split: matched event must receive one device reset");
        // matched pair: same VkEvent in set, wait and device reset
        bool matched = false;
        for (auto e : g_setEvents) for (auto w : g_waitEvents) if (e == w && std::find(g_deviceResetEvents.begin(), g_deviceResetEvents.end(), e) != g_deviceResetEvents.end()) { matched = true; break; }
        check(matched, "split: SPLIT_BEGIN/SPLIT_END must share the same VkEvent");
        check(mm == 0, "split: 64-byte 0xA5 verification");
        if (g_waitEventsCalls == 0) {
            std::printf("split: NOTE: known bug: buffer split path has inverted find comparison\n");
            std::printf("split: (VKCommandBuffer.cpp:1011 'missed' reversed), falls back to FULL barrier\n");
        }
        outcome = (g_setEventCalls > 0 && g_waitEventsCalls > 0 && matched && mm == 0) ? "SPLIT-OK" : "SPLIT-FALLBACK-BUG";
    } else if (cn == "held-observation") {
        HookRAII::reset();
        // verify timeline backend is active
        auto *gq = static_cast<CCVKQueue *>(queue)->gpuQueue();
        check(gq->completionTracker && gq->completionTracker->usesTimeline(),
              "held: timeline backend must be active");
        if (!gq->completionTracker || !gq->completionTracker->usesTimeline()) {
            outcome = "NO-TIMELINE"; goto teardown;
        }
        // record upload + SPLIT_BEGIN only (SetEvent is legal without GPU wait)
        {
            cmd->begin();
            cmd->updateBuffer(bufA, data, kSz);
            BufferBarrier *bArr[] = {beginBarrier};
            Buffer *bBufs[] = {bufA};
            cmd->pipelineBarrier(nullptr, bArr, bBufs, 1, nullptr, nullptr, 0);
            cmd->end();
        }
        // real submit
        queue->submit(reinterpret_cast<CommandBuffer **>(&cmd), 1);
        check(g_setEventCalls > 0, "held: SPLIT_BEGIN recorded vkCmdSetEvent");
        // Physically finish this queue once without advancing engine completion.
        check(vkQueueWaitIdle(gq->vkQueue) == VK_SUCCESS, "held: diagnostic physical queue wait must succeed");
        const auto point = gq->lastCompletion;
        g_heldPrefix = gq->completionTracker->completedValue();
        check(point.value > g_heldPrefix, "held: current point must remain unobserved");
        const auto owner = raw.gpuDevice()->completionSemaphores.find(gq->vkQueue);
        check(owner != raw.gpuDevice()->completionSemaphores.end(), "held: real dedicated completion semaphore must exist");
        if (owner == raw.gpuDevice()->completionSemaphores.end()) { outcome = "NO-TIMELINE"; goto teardown; }
        g_heldTargetSem = owner->second;
        const auto batchesBefore = raw.gpuDevice()->recordingSubmissions.size();
        const auto targetBatchExists = [&]() {
            return std::any_of(raw.gpuDevice()->recordingSubmissions.begin(), raw.gpuDevice()->recordingSubmissions.end(),
                [&](const auto &batch) { return batch->point.domain == point.domain && batch->point.value == point.value; });
        };
        check(batchesBefore > 0 && targetBatchExists(), "held: engine must still own this submitted recording");
        g_heldArmed = true; vkGetSemaphoreCounterValue = &hCounter;
        std::printf("held: synthetic held completion observation; GPU already idle prefix=%llu point=%llu\n",
                    (unsigned long long)g_heldPrefix, (unsigned long long)point.value);
        raw.gpuDevice()->retireRecordings();
        check(g_maskedQueries > 0, "held: target semaphore counter hook must actually be queried");
        check(targetBatchExists(), "held: target batch must survive unobserved completion");
        cmd->destroy(); delete cmd; cmd = nullptr;
        std::printf("held: after CB destroy: hostReset=%d destroyEvent=%d queries=%u\n",
                    g_hostResetCalls, g_destroyEventCalls, g_maskedQueries);
        check(g_hostResetCalls == 0, "held: facade destruction must not host-reset a submitted event");
        check(g_destroyEventCalls == 0, "held: event must remain owned until completion retirement");
        g_heldArmed = false; vkGetSemaphoreCounterValue = g_realCounter;
        raw.waitAllFences();
        std::printf("held: after real retirement: destroyEvent=%d hostReset=%d\n", g_destroyEventCalls, g_hostResetCalls);
        check(raw.gpuDevice()->recordingSubmissions.empty(), "held: real completion must release the batch");
        check(g_createEventCalls == 1 && g_destroyEventCalls == 1, "held: exactly one event must be created and eventually destroyed");
        check(g_hostResetCalls == 0, "held: no host reset is required to destroy an event");
        outcome = "HELD-OBSERVED";
    } else if (cn == "event-reuse") {
        HookRAII::reset();
        bufB = backend->createBuffer(bi);
        check(bufB != nullptr, "reuse: create bufB");

        auto *gpuQueue = static_cast<CCVKQueue *>(queue)->gpuQueue();
        auto completionRetired = [&](const GpuCompletionPoint &point) {
            const bool owned = std::any_of(raw.gpuDevice()->recordingSubmissions.begin(),
                raw.gpuDevice()->recordingSubmissions.end(), [&](const auto &batch) {
                    return batch->point.domain == point.domain && batch->point.value == point.value;
                });
            return !point.empty() && gpuQueue->completionTracker->isComplete(point) && !owned;
        };
        auto pairMatches = [&](size_t index, VkEvent event) {
            return event != VK_NULL_HANDLE && g_setEvents.size() > index &&
                g_waitEvents.size() > index && g_deviceResetEvents.size() > index &&
                g_setEvents[index] == event && g_waitEvents[index] == event &&
                g_deviceResetEvents[index] == event;
        };

        auto recordPair = [&](Buffer *target) -> VkCommandBuffer {
            g_lastBegunCB = VK_NULL_HANDLE;
            cmd->begin();
            const VkCommandBuffer cb = g_lastBegunCB;
            check(cb != VK_NULL_HANDLE, "reuse: real command buffer begin must succeed");
            BufferBarrier *bA[] = {beginBarrier};
            Buffer *bB[] = {target};
            cmd->pipelineBarrier(nullptr, bA, bB, 1, nullptr, nullptr, 0);
            BufferBarrier *eA[] = {endBarrier};
            cmd->pipelineBarrier(nullptr, eA, bB, 1, nullptr, nullptr, 0);
            cmd->end();
            return cb;
        };

        // === record pair 1 and pair 2 ===
        VkCommandBuffer cb1 = recordPair(bufA);
        VkEvent ev1 = g_setEvents.empty() ? VK_NULL_HANDLE : g_setEvents[0];
        VkCommandBuffer cb2 = recordPair(bufB);
        VkEvent ev2 = g_setEvents.size() > 1 ? g_setEvents[1] : VK_NULL_HANDLE;
        std::printf("reuse: pair1 cb=%p event=%p pair2 cb=%p event=%p created=%d\n",
                    (void*)cb1, (void*)ev1, (void*)cb2, (void*)ev2, g_createEventCalls);
        check(g_createEventCalls == 2, "reuse: exactly two events created for two pairs");
        const bool distinct = ev1 != VK_NULL_HANDLE && ev2 != VK_NULL_HANDLE && ev1 != ev2;
        check(distinct, "reuse: two pairs must use two different VkEvents");
        const bool firstTwoMatched = pairMatches(0, ev1) && pairMatches(1, ev2);
        check(firstTwoMatched, "reuse: each pending pair must set/wait/device-reset its own event");
        check(g_successfulSubmits == 0, "reuse: recording phase must not submit");

        // === submit #1: must forward pair1 (first pending) ===
        const auto beforePoint = gpuQueue->lastCompletion;
        const size_t submittedBefore = g_submittedCBs.size();
        queue->submit(reinterpret_cast<CommandBuffer **>(&cmd), 1);
        const auto point1 = gpuQueue->lastCompletion;
        check(!point1.empty() && point1.value > beforePoint.value, "reuse: first successful submit must publish a new point");
        backend->present();
        bool fwd1 = std::find(g_submittedCBs.begin() + submittedBefore, g_submittedCBs.end(), cb1) != g_submittedCBs.end();
        bool fwd2early = std::find(g_submittedCBs.begin() + submittedBefore, g_submittedCBs.end(), cb2) != g_submittedCBs.end();
        std::printf("reuse: submit1: forwarded cb1=%d cb2=%d submits=%d\n",
                    fwd1, fwd2early, g_successfulSubmits);
        check(fwd1, "reuse: submit1 must forward pair1 CB");
        check(!fwd2early, "reuse: submit1 must NOT forward pair2 yet (one pending per submit)");

        // === submit #2: must forward pair2 ===
        const size_t before2 = g_submittedCBs.size();
        queue->submit(reinterpret_cast<CommandBuffer **>(&cmd), 1);
        const auto point2 = gpuQueue->lastCompletion;
        check(point2.domain == point1.domain && point2.value > point1.value, "reuse: second submit must publish the next point");
        backend->present();
        raw.waitAllFences();
        const bool firstTwoCompleted = completionRetired(point1) && completionRetired(point2);
        check(firstTwoCompleted && g_destroyEventCalls == 0, "reuse: first two points must retire while cached events survive");
        bool fwd2 = std::find(g_submittedCBs.begin() + before2, g_submittedCBs.end(), cb2) != g_submittedCBs.end();
        std::printf("reuse: submit2: forwarded cb2=%d\n", fwd2);
        check(fwd2, "reuse: submit2 must forward pair2 CB");
        std::printf("reuse: after pair1+2: set=%d wait=%d reset=%d create=%d\n",
                    g_setEventCalls, g_waitEventsCalls, g_resetEventCalls, g_createEventCalls);
        check(g_setEventCalls == 2 && g_waitEventsCalls == 2 && g_resetEventCalls == 2,
              "reuse: two submitted pairs produce set/wait/reset=2");

        // === record pair 3: must REUSE a completed event ===
        const int createdBefore3 = g_createEventCalls;
        VkCommandBuffer cb3 = recordPair(bufA);
        std::printf("reuse: pair3: cb=%p create=%d (was %d)\n", (void*)cb3, g_createEventCalls, createdBefore3);
        check(g_createEventCalls == createdBefore3,
              "reuse: third pair must REUSE a completed event (no extra vkCreateEvent)");
        VkEvent ev3 = g_setEvents.size() > 2 ? g_setEvents[2] : VK_NULL_HANDLE;
        const bool reused = ev3 != VK_NULL_HANDLE && (ev3 == ev1 || ev3 == ev2);
        check(reused, "reuse: third pair must reuse one of the two original events");
        const bool matched = firstTwoMatched && pairMatches(2, ev3);
        check(matched, "reuse: reused event must also match set/wait/device-reset");
        std::printf("reuse: identities distinct=%d matched=%d reused=%d\n", distinct, matched, reused);

        // === submit #3: must forward pair3 CB ===
        const size_t before3 = g_submittedCBs.size();
        queue->submit(reinterpret_cast<CommandBuffer **>(&cmd), 1);
        const auto point3 = gpuQueue->lastCompletion;
        check(point3.domain == point2.domain && point3.value > point2.value, "reuse: third submit must publish the next point");
        backend->present();
        raw.waitAllFences();
        const bool completed = firstTwoCompleted && completionRetired(point3);
        check(completed && g_successfulSubmits == 3, "reuse: all three actual submissions must complete and retire");
        bool fwd3 = std::find(g_submittedCBs.begin() + before3, g_submittedCBs.end(), cb3) != g_submittedCBs.end();
        std::printf("reuse: submit3: forwarded cb3=%d\n", fwd3);
        check(fwd3, "reuse: submit3 must forward pair3 CB");

        std::printf("reuse: final: set=%d wait=%d reset=%d create=%d destroy=%d hostReset=%d\n",
                    g_setEventCalls, g_waitEventsCalls, g_resetEventCalls,
                    g_createEventCalls, g_destroyEventCalls, g_hostResetCalls);
        check(g_setEventCalls == 3 && g_waitEventsCalls == 3 && g_resetEventCalls == 3,
              "reuse: three submitted pairs produce set/wait/reset=3");
        check(g_createEventCalls == 2, "reuse: exactly two events created total");
        std::printf("reuse: submissions first=%d second=%d third=%d completed=%d\n",
                    fwd1, fwd2, fwd3, completed);
        outcome = "REUSE-OK";
    }

teardown:
    g_heldArmed = false; vkGetSemaphoreCounterValue = g_realCounter;
    raw.waitAllFences();
    const uint32_t le = errCnt() - e0;
    // barriers are device-cached (getBufferBarrier) — do not delete
    if (cmd) { cmd->destroy(); delete cmd; }
    if (bufB) { bufB->destroy(); delete bufB; }
    if (host) { host->destroy(); delete host; }
    if (bufA) { bufA->destroy(); delete bufA; }
    od->destroy();
    const uint32_t pe = raw.getLastValidationErrorCount();
    check(g_createEventCalls == g_destroyEventCalls, "all successfully created test events must be destroyed exactly once");
    check(g_hostResetCalls == 0, "event retirement must never introduce a host reset");
    std::printf("event destruction: created=%d destroyed=%d hostReset=%d\n", g_createEventCalls, g_destroyEventCalls, g_hostResetCalls);
    std::printf("%s completed: failures=%u liveErrors=%u postErrors=%u outcome=%s\n",
                cn.c_str(), g_failures, le, pe, outcome.c_str());
    return (g_failures || le || pe) ? 1 : 0;
}
