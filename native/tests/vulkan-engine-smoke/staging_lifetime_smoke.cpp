// Real staging-content and allocation-reuse tests. Hooks are installed after
// volk device loading. Buffer ownership stays with this harness; recording and
// GPU completion protect staging allocations through the engine's block lease.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "gfx-vulkan/VKBuffer.h"
#include "gfx-vulkan/VKCommandBuffer.h"
#include "gfx-vulkan/VKDevice.h"
#include "gfx-vulkan/VKGPUObjects.h"
#include "renderer/GFXDeviceManager.h"
#include "platform/FileUtils.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

int cocos_main(int, const char **) { return 0; }
void cocos_destroy() {}

namespace {
unsigned failures = 0;
void check(bool ok, const char *reason) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", reason); ++failures; }
}
struct CopyEvent {
    VkCommandBuffer command;
    VkBuffer source, destination;
    VkDeviceSize offset, size;
};
std::vector<CopyEvent> copies;
std::vector<VkCommandBuffer> submitted;
VkCommandBuffer lastBegun = VK_NULL_HANDLE;
unsigned resets = 0;
PFN_vkCmdCopyBuffer realCopy = nullptr;
PFN_vkBeginCommandBuffer realBegin = nullptr;
PFN_vkQueueSubmit realSubmit = nullptr;
PFN_vkResetCommandPool realReset = nullptr;
VKAPI_ATTR void VKAPI_CALL copyHook(VkCommandBuffer cb, VkBuffer src, VkBuffer dst,
                                   uint32_t count, const VkBufferCopy *regions) {
    for (uint32_t i = 0; i < count; ++i) copies.push_back({cb, src, dst, regions[i].srcOffset, regions[i].size});
    realCopy(cb, src, dst, count, regions);
}
VKAPI_ATTR VkResult VKAPI_CALL beginHook(VkCommandBuffer cb, const VkCommandBufferBeginInfo *info) {
    const auto result = realBegin(cb, info);
    if (result == VK_SUCCESS) lastBegun = cb;
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL submitHook(VkQueue queue, uint32_t count, const VkSubmitInfo *info, VkFence fence) {
    const auto result = realSubmit(queue, count, info, fence);
    if (result == VK_SUCCESS) {
        for (uint32_t i = 0; i < count; ++i)
            for (uint32_t j = 0; j < info[i].commandBufferCount; ++j) submitted.push_back(info[i].pCommandBuffers[j]);
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL resetHook(VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags flags) {
    ++resets;
    return realReset(device, pool, flags);
}
struct Hooks {
    Hooks() {
        realCopy = vkCmdCopyBuffer; vkCmdCopyBuffer = copyHook;
        realBegin = vkBeginCommandBuffer; vkBeginCommandBuffer = beginHook;
        realSubmit = vkQueueSubmit; vkQueueSubmit = submitHook;
        realReset = vkResetCommandPool; vkResetCommandPool = resetHook;
    }
    ~Hooks() {
        vkCmdCopyBuffer = realCopy; vkBeginCommandBuffer = realBegin;
        vkQueueSubmit = realSubmit; vkResetCommandPool = realReset;
    }
};
bool forwarded(VkCommandBuffer cb) { return std::find(submitted.begin(), submitted.end(), cb) != submitted.end(); }
CopyEvent uploadTo(VkBuffer destination) {
    std::vector<CopyEvent> matches;
    for (const auto &copy : copies) if (copy.destination == destination) matches.push_back(copy);
    check(matches.size() == 1, "exactly one upload copy must target the requested device buffer");
    if (matches.empty()) return {};
    check(matches[0].size == 64, "upload copy must cover all 64 bytes");
    return matches[0];
}
} // namespace

using namespace cc;
using namespace cc::gfx;
constexpr uint32_t DATA_SIZE = 64;

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::string name;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--case") == 0 && i + 1 < argc) name = argv[++i];
    if (name != "control" && name != "delayed-staging" && name != "reuse-after-drop" && name != "reuse-after-completion") {
        std::fprintf(stderr, "usage: %s --case <control|delayed-staging|reuse-after-drop|reuse-after-completion>\n", argv[0]);
        return 2;
    }
    const auto directory = std::filesystem::current_path() / ("staging-life-" + name);
    std::filesystem::create_directories(directory);
    std::unique_ptr<FileUtils> fileUtils(createFileUtils());
    fileUtils->setWritablePath(directory.generic_string() + "/");
    std::unique_ptr<Device> device(DeviceManager::create());
    if (!device || device->getGfxAPI() != API::VULKAN) {
        std::fprintf(stderr, "FAIL: Cocos Vulkan initialization\n");
        if (device) device->destroy();
        return 1;
    }
    auto &raw = *CCVKDevice::getInstance();
    auto *context = raw.gpuContext();
    const bool validation = context->validationEnabled && context->bestPracticesValidationEnabled && context->synchronizationValidationEnabled;
    std::printf("validation + best practices + sync: %s\n", validation ? "ENABLED" : "NOT VERIFIED");
    if (!validation) { device->destroy(); return 1; }
    raw.enableAutoBarrier(false);
    const auto errorsBefore = context->validationErrorCount.load(std::memory_order_relaxed);
    Hooks hooks;
    auto makeBuffer = [&](MemoryUsage memory) {
        BufferInfo info;
        info.usage = BufferUsageBit::TRANSFER_SRC | BufferUsageBit::TRANSFER_DST;
        info.memUsage = memory;
        info.size = DATA_SIZE; info.stride = 4;
        auto *result = raw.createBuffer(info);
        check(result && static_cast<CCVKBuffer *>(result)->gpuBuffer()->instanceSize == 0,
              "test buffers must have a single backing instance");
        return result;
    };
    Buffer *a = makeBuffer(MemoryUsageBit::DEVICE);
    Buffer *b = makeBuffer(MemoryUsageBit::DEVICE);
    Buffer *host = makeBuffer(MemoryUsageBit::HOST);
    auto *hostGPU = static_cast<CCVKBuffer *>(host)->gpuBuffer();
    check(hostGPU->mappedData != nullptr, "HOST readback must be mapped");
    auto *queue = raw.getQueue();
    auto makeCommand = [&]() {
        CommandBufferInfo info; info.queue = queue;
        return raw.createCommandBuffer(info);
    };
    CommandBuffer *cmd = makeCommand();
    uint8_t patternA[DATA_SIZE], patternB[DATA_SIZE];
    std::memset(patternA, 0xA5, sizeof(patternA)); std::memset(patternB, 0x5A, sizeof(patternB));
    auto body = [&](CommandBuffer *recording, Buffer *target, const void *data, bool readback) {
        recording->updateBuffer(target, data, DATA_SIZE);
        if (!readback) return;
        // External customCommand resources remain alive until the final wait.
        const VkBuffer targetHandle = static_cast<CCVKBuffer *>(target)->gpuBuffer()->vkBuffer;
        const VkBuffer hostHandle = hostGPU->vkBuffer;
        recording->customCommand([=](void *native) {
            auto cb = static_cast<VkCommandBuffer>(native);
            VkBufferMemoryBarrier before{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            before.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            before.srcQueueFamilyIndex = before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            before.buffer = targetHandle; before.size = DATA_SIZE;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 1, &before, 0, nullptr);
            VkBufferCopy copy{0, 0, DATA_SIZE};
            vkCmdCopyBuffer(cb, targetHandle, hostHandle, 1, &copy);
            VkBufferMemoryBarrier after{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; after.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            after.srcQueueFamilyIndex = after.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            after.buffer = hostHandle; after.size = DATA_SIZE;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                 0, 0, nullptr, 1, &after, 0, nullptr);
        });
    };
    const uint32_t originalSlot = raw.gpuDevice()->curBackBufferIndex;
    auto rotateTo = [&](uint32_t slot) {
        unsigned rotations = 0;
        do { raw.present(); ++rotations; }
        while (raw.gpuDevice()->curBackBufferIndex != slot && rotations <= raw.gpuDevice()->backBufferCount);
        check(raw.gpuDevice()->curBackBufferIndex == slot, "rotation must return to the staging allocation slot");
        return rotations;
    };
    cmd->begin();
    const VkCommandBuffer original = lastBegun;
    uint8_t expected = 0xA5;
    std::string outcome = "CLEAN";
    if (name == "delayed-staging") raw.present();
    const uint32_t stagingSlot = raw.gpuDevice()->curBackBufferIndex;
    body(cmd, a, patternA, name != "reuse-after-drop");
    cmd->end();
    const auto oldRange = uploadTo(static_cast<CCVKBuffer *>(a)->gpuBuffer()->vkBuffer);
    VkCommandBuffer finalRecording = original;
    if (name == "delayed-staging") {
        check(stagingSlot != originalSlot, "delayed upload must use a different slot from the recording pool");
        const auto rotations = rotateTo(stagingSlot);
        auto *replacement = makeCommand();
        replacement->begin(); body(replacement, b, patternB, false); replacement->end();
        queue->submit(&replacement, 1);
        raw.present();
        replacement->destroy(); delete replacement;
        const auto newRange = uploadTo(static_cast<CCVKBuffer *>(b)->gpuBuffer()->vkBuffer);
        const bool sameBuffer = oldRange.source == newRange.source;
        const bool overlap = sameBuffer && oldRange.offset < newRange.offset + newRange.size && newRange.offset < oldRange.offset + oldRange.size;
        std::printf("staging range: case=%s sameBuffer=%d overlap=%d oldOffset=%llu newOffset=%llu\n",
                    name.c_str(), sameBuffer, overlap, (unsigned long long)oldRange.offset, (unsigned long long)newRange.offset);
        std::printf("staging isolation: rotations=%u commandPoolResets=%u\n", rotations, resets);
        check(resets == 0, "delayed case must isolate staging reuse from all command-pool resets");
        check(!overlap, "live recording staging ranges must not overlap a later allocation");
        outcome = "PRESERVED";
    } else if (name == "reuse-after-drop" || name == "reuse-after-completion") {
        if (name == "reuse-after-completion") {
            queue->submit(&cmd, 1); raw.waitAllFences();
            check(forwarded(original), "completion reuse must first submit the original recording");
            check(raw.gpuDevice()->recordingSubmissions.empty(), "completed batches must release their recording leases");
        }
        cmd->destroy(); delete cmd; cmd = nullptr;
        if (name == "reuse-after-drop") {
            check(submitted.empty(), "dropping a pending recording must not submit its commands");
            std::printf("staging drop: beforeReplacementSubmit=%zu\n", submitted.size());
        }
        rotateTo(stagingSlot);
        cmd = makeCommand(); cmd->begin(); finalRecording = lastBegun;
        body(cmd, b, patternB, true); cmd->end();
        const auto newRange = uploadTo(static_cast<CCVKBuffer *>(b)->gpuBuffer()->vkBuffer);
        const bool sameBuffer = oldRange.source == newRange.source;
        const bool sameOffset = oldRange.offset == newRange.offset;
        check(sameBuffer && sameOffset, "released staging allocation must reuse the original buffer and offset");
        std::printf("staging reuse: case=%s sameBuffer=%d sameOffset=%d\n", name.c_str(), sameBuffer, sameOffset);
        expected = 0x5A; outcome = "REUSED";
    }
    queue->submit(&cmd, 1);
    raw.waitAllFences();
    const bool finalForwarded = forwarded(finalRecording);
    check(finalForwarded, "the readback recording must reach the real successful queue submission");
    std::printf("staging submit: case=%s forwarded=%d\n", name.c_str(), finalForwarded);
    const auto invalidate = vmaInvalidateAllocation(raw.gpuDevice()->memoryAllocator, hostGPU->vmaAllocation, 0, DATA_SIZE);
    check(invalidate == VK_SUCCESS, "readback invalidate must succeed after actual GPU completion");
    unsigned mismatches = 0;
    if (hostGPU->mappedData) {
        for (uint32_t i = 0; i < DATA_SIZE; ++i) if (hostGPU->mappedData[i] != expected) ++mismatches;
    } else mismatches = DATA_SIZE;
    std::printf("staging data: case=%s bytes=%u mismatches=%u first=0x%02x expected=0x%02x\n",
                name.c_str(), DATA_SIZE, mismatches, hostGPU->mappedData ? hostGPU->mappedData[0] : 0, expected);
    check(mismatches == 0, "all 64 readback bytes must match the recorded upload pattern");
    check(copies.size() >= 2, "real staging upload and readback copies must both be observed");
    const auto liveErrors = raw.gpuContext()->validationErrorCount.load(std::memory_order_relaxed) - errorsBefore;
    cmd->destroy(); delete cmd;
    host->destroy(); delete host; a->destroy(); delete a; b->destroy(); delete b;
    device->destroy();
    const auto postErrors = raw.getLastValidationErrorCount();
    std::printf("%s completed: failures=%u liveErrors=%u postErrors=%u outcome=%s\n",
                name.c_str(), failures, liveErrors, postErrors, outcome.c_str());
    return failures || liveErrors || postErrors ? 1 : 0;
}
