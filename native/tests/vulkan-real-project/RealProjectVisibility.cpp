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

// Default-off diagnostic on the real project's existing Vulkan device.
#include "RealProjectVisibility.h"
#include <array>
#include <chrono>
#include <cstdio>
#include "gfx-vulkan/VKQueue.h"
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "gfx-vulkan/VKTexture.h"
#include "base/threading/MessageQueue.h"
#include "gfx-agent/DeviceAgent.h"
#include "gfx-base/GFXCommandBuffer.h"
#include "gfx-base/GFXQueue.h"
#include "gfx-vulkan/VKBuffer.h"
#include "gfx-vulkan/VKDescriptorSet.h"
#include "gfx-vulkan/VKDescriptorSetLayout.h"
#include "gfx-vulkan/VKDevice.h"
#include "gfx-vulkan/VKPipelineLayout.h"
#include "gfx-vulkan/VKPipelineState.h"
#include "gfx-vulkan/VKShader.h"
#include "gfx-vulkan/VKUtils.h"

namespace cc::vulkan_test {
namespace {
using namespace gfx;
struct Phase {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBuffer managerCB = VK_NULL_HANDLE;
    std::array<VkBuffer, 2> inputs{};
    VkBuffer output = VK_NULL_HANDLE, host = VK_NULL_HANDLE;
    unsigned managerCalls = 0, globals = 0, resources = 0, matched = 0, mask = 0;
    unsigned dispatches = 0, copies = 0, submits = 0, managerSubmits = 0, checked = 0, mismatch = 0, uploads = 0;
};
struct Object {
    uint64_t handle = 0;
    unsigned kind = 0, destroyed = 0;
};
struct Timing { uint64_t batch = 0, upload = 0, manager = 0, gpu = 0; unsigned checked = 0, stagingSources = 0; uint64_t record = 0, submit = 0, completion = 0, waitApi = 0; unsigned waitCalls = 0, waitFailures = 0; };
struct State {
    bool profile = false, waitScope = false;
    uint64_t waitApi = 0; unsigned waitCalls = 0, waitFailures = 0;
    PFN_vkWaitForFences waitFences = nullptr;
    PFN_vkWaitSemaphores waitSemaphores = nullptr;
    bool prepared = false, enabled = false, started = false, installed = false, manager = false, armed = false;
    std::string mode;
    unsigned failures = 0, phase = 0, count = 0, devices = 0, live = 0, post = 0;
    std::array<Phase, 24> phases{};
    std::array<Object, 96> objects{};
    std::array<Timing, 1000> timings{};
    unsigned timingCount = 0, stagingSourceCount = 0;
    std::array<VkBuffer, 128> stagingSources{};
    CCVKGPUContext *context = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandBuffer textureCB = VK_NULL_HANDLE;
    VkImage textureImage = VK_NULL_HANDLE;
    VkBuffer textureHost = VK_NULL_HANDLE;
    unsigned textureCopies = 0, textureSubmits = 0, textureUploads = 0, textureWidth = 0, textureHeight = 0;
    PFN_vkCmdCopyBufferToImage imageUpload = nullptr;
    PFN_vkCmdCopyImageToBuffer imageCopy = nullptr;
    PFN_vkCmdPipelineBarrier barrier = nullptr;
    PFN_vkCmdPipelineBarrier2 barrier2 = nullptr;
    PFN_vkCmdPipelineBarrier2KHR barrier2KHR = nullptr;
    PFN_vkCmdDispatch dispatch = nullptr;
    PFN_vkCmdCopyBuffer copy = nullptr;
    PFN_vkQueueSubmit submit = nullptr;
    PFN_vkQueueSubmit2 submit2 = nullptr;
    PFN_vkQueueSubmit2KHR submit2KHR = nullptr;
    PFN_vkDestroyPipeline destroyPipeline = nullptr;
    PFN_vkDestroyShaderModule destroyModule = nullptr;
    PFN_vkDestroyPipelineLayout destroyLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout destroySetLayout = nullptr;
    PFN_vkDestroyDevice destroyDevice = nullptr;
} s;
void check(bool v, const char *why) noexcept {
    if (!v) {
        ++s.failures;
        CC_LOG_ERROR("FAIL: visibility %s", why);
    }
}
void require(bool v, const char *why) {
    if (!v) {
        check(false, why);
        throw std::runtime_error(why);
    }
}
bool perf() noexcept { return s.mode == "upload-perf" || s.mode == "upload-perf-baseline"; }
uint64_t clockNs() noexcept { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
bool stress() noexcept { return s.mode == "stress" || s.mode == "stress-wrong"; }
bool present(const char *name) { return std::getenv(name) != nullptr; }
template <class T>
struct Drop {
    void operator()(T *p) const {
        if (p) {
            p->destroy();
            delete p;
        }
    }
};
template <class T>
using Owned = std::unique_ptr<T, Drop<T>>;
template <class T>
Owned<T> owned(T *p) {
    require(p != nullptr, "created facade");
    return Owned<T>(p);
}
template <class T>
uint64_t bits(T h) {
    uint64_t v = 0;
    static_assert(sizeof(h) <= sizeof(v));
    std::memcpy(&v, &h, sizeof(h));
    return v;
}
void tracked(uint64_t h, unsigned kind) {
    require(h && s.count < s.objects.size(), "tracked object capacity");
    s.objects[s.count++] = {h, kind, 0};
}
void retired(uint64_t h, unsigned kind) noexcept {
    // Vulkan may reuse a handle after an earlier object has retired.
    for (unsigned i = s.count; i > 0; --i)
        if (s.objects[i - 1].handle == h && s.objects[i - 1].kind == kind && !s.objects[i - 1].destroyed) {
            ++s.objects[i - 1].destroyed;
            return;
        }
}
void resource(VkBuffer b, VkDeviceSize offset, VkDeviceSize size, uint32_t srcQ, uint32_t dstQ,
              VkPipelineStageFlags2 srcStage, VkPipelineStageFlags2 dstStage,
              VkAccessFlags2 srcAccess, VkAccessFlags2 dstAccess) noexcept {
    auto &p = s.phases[s.phase];
    ++p.resources;
    bool target = false;
    for (unsigned i = 0; i < p.inputs.size(); ++i)
        if (b == p.inputs[i]) {
            target = true;
            check(!(p.mask & (1U << i)), "no duplicated input resource");
            p.mask |= 1U << i;
        }
    check(target && offset == 0 && size == VK_WHOLE_SIZE && srcQ == VK_QUEUE_FAMILY_IGNORED && dstQ == VK_QUEUE_FAMILY_IGNORED,
          "resource barrier identity and whole physical buffer");
    check((srcStage & VK_PIPELINE_STAGE_TRANSFER_BIT) && (dstStage & VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) &&
              srcAccess == VK_ACCESS_TRANSFER_WRITE_BIT && dstAccess == (VK_ACCESS_SHADER_READ_BIT | (b == p.inputs[1] ? VK_ACCESS_INDIRECT_COMMAND_READ_BIT : 0)),
          "upload-to-compute masks");
    if (target) ++p.matched;
}
VKAPI_ATTR void VKAPI_CALL barrierHook(VkCommandBuffer cb, VkPipelineStageFlags a, VkPipelineStageFlags b, VkDependencyFlags flags,
                                       uint32_t m, const VkMemoryBarrier *mem, uint32_t n, const VkBufferMemoryBarrier *buffers,
                                       uint32_t i, const VkImageMemoryBarrier *images) {
    if (!s.barrier) {
        check(false, "legacy barrier PFN");
        return;
    }
    s.barrier(cb, a, b, flags, m, mem, n, buffers, i, images);
    if (!s.manager) return;
    auto &p = s.phases[s.phase];
    ++p.managerCalls;
    p.managerCB = cb;
    p.globals += m;
    check(!i && (!n || buffers), "manager expected buffer payload");
    if (buffers)
        for (unsigned j = 0; j < n; ++j) {
            const auto &v = buffers[j];
            resource(v.buffer, v.offset, v.size, v.srcQueueFamilyIndex, v.dstQueueFamilyIndex, a, b, v.srcAccessMask, v.dstAccessMask);
        }
}
void barrier2Record(VkCommandBuffer cb, const VkDependencyInfo *info) noexcept {
    if (!s.manager) return;
    if (!info) {
        check(false, "Sync2 dependency payload");
        return;
    }
    auto &p = s.phases[s.phase];
    ++p.managerCalls;
    p.managerCB = cb;
    p.globals += info->memoryBarrierCount;
    check(!info->imageMemoryBarrierCount && (!info->bufferMemoryBarrierCount || info->pBufferMemoryBarriers), "Sync2 buffer payload");
    if (info->pBufferMemoryBarriers)
        for (unsigned j = 0; j < info->bufferMemoryBarrierCount; ++j) {
            const auto &v = info->pBufferMemoryBarriers[j];
            resource(v.buffer, v.offset, v.size, v.srcQueueFamilyIndex, v.dstQueueFamilyIndex, v.srcStageMask, v.dstStageMask, v.srcAccessMask, v.dstAccessMask);
        }
}
VKAPI_ATTR void VKAPI_CALL barrier2Hook(VkCommandBuffer cb, const VkDependencyInfo *info) {
    if (!s.barrier2) {
        check(false, "core barrier2 PFN");
        return;
    }
    s.barrier2(cb, info);
    barrier2Record(cb, info);
}
VKAPI_ATTR void VKAPI_CALL barrier2KHRHook(VkCommandBuffer cb, const VkDependencyInfo *info) {
    if (!s.barrier2KHR) {
        check(false, "KHR barrier2 PFN");
        return;
    }
    s.barrier2KHR(cb, info);
    barrier2Record(cb, info);
}
VKAPI_ATTR void VKAPI_CALL dispatchHook(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    if (!s.dispatch) {
        check(false, "dispatch PFN");
        return;
    }
    s.dispatch(cb, x, y, z);
    if (s.armed) {
        auto &p = s.phases[s.phase];
        check(cb == p.cb && x && y == 1 && z == 1, "actual dispatch CB");
        ++p.dispatches;
    }
}
VKAPI_ATTR void VKAPI_CALL copyHook(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t count, const VkBufferCopy *regions) {
    if (!s.copy) {
        check(false, "copy PFN");
        return;
    }
    s.copy(cb, src, dst, count, regions);
    auto &phase = s.phases[s.phase];
    if (s.profile && (dst == phase.inputs[0] || dst == phase.inputs[1])) {
        check(src && count == 1 && regions && !regions[0].dstOffset && regions[0].size == 131072, "performance input upload shape");
        ++phase.uploads;
        if (s.profile) {
            bool found = false;
            for (unsigned i = 0; i < s.stagingSourceCount; ++i) found = found || s.stagingSources[i] == src;
            if (!found) {
                if (s.stagingSourceCount >= s.stagingSources.size()) check(false, "staging source capacity");
                else s.stagingSources[s.stagingSourceCount++] = src;
            }
        }
    }
    if (s.armed && src == s.phases[s.phase].output) {
        auto &p = s.phases[s.phase];
        check(cb == p.cb && dst == p.host && count == 1 && regions && !regions[0].srcOffset && !regions[0].dstOffset, "actual readback CB");
        ++p.copies;
    }
}
bool textureRegion(uint32_t count, const VkBufferImageCopy *regions, bool upload = false) noexcept {
    if (count != 1 || !regions) return false;
    const auto &r = regions[0];
    return (upload ? r.bufferRowLength >= s.textureWidth && r.bufferImageHeight == s.textureHeight : !r.bufferRowLength && !r.bufferImageHeight) && !r.imageOffset.x && !r.imageOffset.y && !r.imageOffset.z &&
        r.imageSubresource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && !r.imageSubresource.mipLevel && !r.imageSubresource.baseArrayLayer && r.imageSubresource.layerCount == 1 &&
        r.imageExtent.width == s.textureWidth && r.imageExtent.height == s.textureHeight && r.imageExtent.depth == 1;
}
VKAPI_ATTR void VKAPI_CALL imageUploadHook(VkCommandBuffer cb, VkBuffer buffer, VkImage image, VkImageLayout layout, uint32_t count, const VkBufferImageCopy *regions) {
    if (!s.imageUpload) { check(false, "image upload PFN"); return; }
    s.imageUpload(cb, buffer, image, layout, count, regions);
    if (s.textureImage && image == s.textureImage) {
        check(cb && buffer && layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && textureRegion(count, regions, true), "texture upload shape");
        ++s.textureUploads;
    }
}
VKAPI_ATTR void VKAPI_CALL imageCopyHook(VkCommandBuffer cb, VkImage image, VkImageLayout layout, VkBuffer buffer, uint32_t count, const VkBufferImageCopy *regions) {
    if (!s.imageCopy) { check(false, "image copy PFN"); return; }
    s.imageCopy(cb, image, layout, buffer, count, regions);
    if (s.textureCB && image == s.textureImage) {
        check(cb == s.textureCB && buffer == s.textureHost && layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && textureRegion(count, regions) && !regions[0].bufferOffset, "texture copy identity");
        ++s.textureCopies;
    }
}
void submitted(VkCommandBuffer cb) noexcept {
    if (s.textureCB && cb == s.textureCB) ++s.textureSubmits;
    if (s.armed && cb == s.phases[s.phase].cb) ++s.phases[s.phase].submits;
    if (s.armed && cb == s.phases[s.phase].managerCB) ++s.phases[s.phase].managerSubmits;
}
VKAPI_ATTR VkResult VKAPI_CALL submitHook(VkQueue q, uint32_t n, const VkSubmitInfo *info, VkFence f) {
    if (!s.submit) {
        check(false, "submit PFN");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const auto r = s.submit(q, n, info, f);
    if (r == VK_SUCCESS && info)
        for (unsigned i = 0; i < n; ++i)
            if (info[i].pCommandBuffers)
                for (unsigned j = 0; j < info[i].commandBufferCount; ++j) submitted(info[i].pCommandBuffers[j]);
    return r;
}
void submit2Record(uint32_t n, const VkSubmitInfo2 *info, VkResult r) noexcept {
    if (r == VK_SUCCESS && info)
        for (unsigned i = 0; i < n; ++i)
            if (info[i].pCommandBufferInfos)
                for (unsigned j = 0; j < info[i].commandBufferInfoCount; ++j) submitted(info[i].pCommandBufferInfos[j].commandBuffer);
}
VKAPI_ATTR VkResult VKAPI_CALL submit2Hook(VkQueue q, uint32_t n, const VkSubmitInfo2 *info, VkFence f) {
    if (!s.submit2) return VK_ERROR_INITIALIZATION_FAILED;
    const auto r = s.submit2(q, n, info, f);
    submit2Record(n, info, r);
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL submit2KHRHook(VkQueue q, uint32_t n, const VkSubmitInfo2 *info, VkFence f) {
    if (!s.submit2KHR) return VK_ERROR_INITIALIZATION_FAILED;
    const auto r = s.submit2KHR(q, n, info, f);
    submit2Record(n, info, r);
    return r;
}
#define DESTROY(Name, Type, Member, Kind)                                                 \
    VKAPI_ATTR void VKAPI_CALL Name(VkDevice d, Type h, const VkAllocationCallbacks *a) { \
        if (!s.Member) {                                                                  \
            check(false, "destroy PFN");                                                  \
            return;                                                                       \
        }                                                                                 \
        s.Member(d, h, a);                                                                \
        retired(bits(h), Kind);                                                           \
    }
DESTROY(pipelineDestroy, VkPipeline, destroyPipeline, 0)
DESTROY(moduleDestroy, VkShaderModule, destroyModule, 1)
DESTROY(layoutDestroy, VkPipelineLayout, destroyLayout, 2)
DESTROY(setLayoutDestroy, VkDescriptorSetLayout, destroySetLayout, 3)
#undef DESTROY
VKAPI_ATTR void VKAPI_CALL deviceDestroy(VkDevice d, const VkAllocationCallbacks *a) {
    check(d == s.device && !s.devices, "single actual device shutdown");
    if (s.context) s.live = s.context->validationErrorCount.load();
    s.destroyDevice(d, a);
    if (s.context) s.post = s.context->validationErrorCount.load();
    s.context = nullptr;
    ++s.devices;
}
void waitResult(uint64_t start, VkResult result) noexcept {
    if (!s.waitScope) return;
    s.waitApi += clockNs() - start; ++s.waitCalls; s.waitFailures += result != VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL waitFencesHook(VkDevice d, uint32_t count, const VkFence *fences, VkBool32 all, uint64_t timeout) {
    if (!s.waitFences) { check(false, "wait fences PFN"); return VK_ERROR_INITIALIZATION_FAILED; }
    const auto start = s.waitScope ? clockNs() : 0; const auto result = s.waitFences(d, count, fences, all, timeout); waitResult(start, result); return result;
}
VKAPI_ATTR VkResult VKAPI_CALL waitSemaphoresHook(VkDevice d, const VkSemaphoreWaitInfo *info, uint64_t timeout) {
    if (!s.waitSemaphores) { check(false, "wait semaphores PFN"); return VK_ERROR_INITIALIZATION_FAILED; }
    const auto start = s.waitScope ? clockNs() : 0; const auto result = s.waitSemaphores(d, info, timeout); waitResult(start, result); return result;
}
struct WaitScope {
    explicit WaitScope(bool enabled) { s.waitScope = enabled; s.waitApi = 0; s.waitCalls = s.waitFailures = 0; }
    ~WaitScope() { s.waitScope = false; }
};
void install() {
    require(vkCmdCopyBufferToImage && vkCmdCopyImageToBuffer && vkCmdPipelineBarrier && vkCmdDispatch && vkCmdCopyBuffer && vkQueueSubmit && vkDestroyPipeline && vkDestroyShaderModule && vkDestroyPipelineLayout && vkDestroyDescriptorSetLayout && vkDestroyDevice, "required PFNs");
#define HOOK(Global, Member, Hook) \
    s.Member = Global;             \
    Global = Hook;
    HOOK(vkCmdPipelineBarrier, barrier, barrierHook)
    if (vkWaitForFences) { HOOK(vkWaitForFences, waitFences, waitFencesHook) }
    if (vkWaitSemaphores) { HOOK(vkWaitSemaphores, waitSemaphores, waitSemaphoresHook) }
    HOOK(vkCmdDispatch, dispatch, dispatchHook)
    HOOK(vkCmdCopyBuffer, copy, copyHook)
    HOOK(vkCmdCopyImageToBuffer, imageCopy, imageCopyHook)
    HOOK(vkCmdCopyBufferToImage, imageUpload, imageUploadHook)
    HOOK(vkQueueSubmit, submit, submitHook)
        HOOK(vkDestroyPipeline, destroyPipeline, pipelineDestroy) HOOK(vkDestroyShaderModule, destroyModule, moduleDestroy) HOOK(vkDestroyPipelineLayout, destroyLayout, layoutDestroy) HOOK(vkDestroyDescriptorSetLayout, destroySetLayout, setLayoutDestroy) HOOK(vkDestroyDevice, destroyDevice, deviceDestroy) if (vkCmdPipelineBarrier2) { HOOK(vkCmdPipelineBarrier2, barrier2, barrier2Hook) }
    if (vkCmdPipelineBarrier2KHR) {
        HOOK(vkCmdPipelineBarrier2KHR, barrier2KHR, barrier2KHRHook)
    }
    if (vkQueueSubmit2) {
        HOOK(vkQueueSubmit2, submit2, submit2Hook)
    }
    if (vkQueueSubmit2KHR) {
        HOOK(vkQueueSubmit2KHR, submit2KHR, submit2KHRHook)
    }
#undef HOOK
    s.installed = true;
}
void restore() noexcept {
    if (!s.installed) return;
#define RESTORE(Global, Member) Global = s.Member;
    RESTORE(vkCmdPipelineBarrier, barrier)
    if (s.waitFences) { RESTORE(vkWaitForFences, waitFences) }
    if (s.waitSemaphores) { RESTORE(vkWaitSemaphores, waitSemaphores) }
    RESTORE(vkCmdDispatch, dispatch)
    RESTORE(vkCmdCopyBuffer, copy)
    RESTORE(vkCmdCopyImageToBuffer, imageCopy)
    RESTORE(vkCmdCopyBufferToImage, imageUpload)
    RESTORE(vkQueueSubmit, submit)
        RESTORE(vkDestroyPipeline, destroyPipeline) RESTORE(vkDestroyShaderModule, destroyModule) RESTORE(vkDestroyPipelineLayout, destroyLayout) RESTORE(vkDestroyDescriptorSetLayout, destroySetLayout) RESTORE(vkDestroyDevice, destroyDevice) if (s.barrier2) { RESTORE(vkCmdPipelineBarrier2, barrier2) }
    if (s.barrier2KHR) {
        RESTORE(vkCmdPipelineBarrier2KHR, barrier2KHR)
    }
    if (s.submit2) {
        RESTORE(vkQueueSubmit2, submit2)
    }
    if (s.submit2KHR) {
        RESTORE(vkQueueSubmit2KHR, submit2KHR)
    }
#undef RESTORE
    s.installed = false;
}
struct QueryOwner {
    VkDevice device = VK_NULL_HANDLE; VkQueryPool pool = VK_NULL_HANDLE;
    CCVKDevice *owner = nullptr;
    ~QueryOwner() {
        // Cleanup also covers a failed phase after submission. Never destroy a pending query pool.
        if (pool) { if (owner) owner->waitAllFences(); vkDestroyQueryPool(device, pool, nullptr); }
    }
};
void phase(CCVKDevice &d, unsigned index, unsigned bytes) {
    const bool performance = perf(), stressMode = stress(), baseline = s.mode == "upload-perf-baseline";
    s.phase = index;
    auto &p = s.phases[index];
    require(d.gpuContext()->validationErrorCount.load() < 10, "Validation budget");
    BufferInfo bi;
    bi.usage = BufferUsageBit::STORAGE | BufferUsageBit::TRANSFER_DST | BufferUsageBit::TRANSFER_SRC;
    bi.memUsage = MemoryUsageBit::DEVICE;
    bi.size = bytes;
    bi.stride = 4;
    auto a = owned(d.createBuffer(bi)), b = owned(d.createBuffer(bi)), output = owned(d.createBuffer(bi));
    bi.usage = BufferUsageBit::TRANSFER_DST;
    bi.memUsage = MemoryUsageBit::HOST;
    auto host = owned(d.createBuffer(bi));
    auto *ga = static_cast<CCVKBuffer *>(a.get())->gpuBuffer();
    auto *gb = static_cast<CCVKBuffer *>(b.get())->gpuBuffer();
    auto *go = static_cast<CCVKBuffer *>(output.get())->gpuBuffer();
    auto *gh = static_cast<CCVKBuffer *>(host.get())->gpuBuffer();
    require(ga && gb && go && gh && ga->vkBuffer && gb->vkBuffer && go->vkBuffer && gh->vkBuffer && gh->mappedData, "actual physical buffers");
    p.inputs = {ga->vkBuffer, gb->vkBuffer};
    p.output = go->vkBuffer;
    p.host = gh->vkBuffer;
    ShaderInfo si;
    si.name = "project-visibility";
    si.stages.push_back({ShaderStageFlagBit::COMPUTE, "layout(local_size_x=16) in;\nlayout(set=0,binding=0,std430) readonly buffer A { uint a[]; };\nlayout(set=0,binding=1,std430) readonly buffer B { uint b[]; };\nlayout(set=0,binding=2,std430) writeonly buffer O { uint o[]; };\nvoid main(){uint i=gl_GlobalInvocationID.x; o[i]=a[i]+b[i];}\n"});
    for (unsigned j = 0; j < 3; ++j) {
        UniformStorageBuffer u;
        u.set = 0;
        u.binding = j;
        u.count = 1;
        u.name = j == 0 ? "A" : j == 1 ? "B"
                                       : "O";
        u.memoryAccess = j == 2 ? MemoryAccessBit::WRITE_ONLY : MemoryAccessBit::READ_ONLY;
        si.buffers.push_back(u);
    }
    auto shader = owned(d.createShader(si));
    DescriptorSetLayoutInfo li;
    for (unsigned j = 0; j < 3; ++j) {
        DescriptorSetLayoutBinding v;
        v.binding = j;
        v.count = 1;
        v.descriptorType = DescriptorType::STORAGE_BUFFER;
        v.stageFlags = ShaderStageFlagBit::COMPUTE;
        li.bindings.push_back(v);
    }
    auto layout = owned(d.createDescriptorSetLayout(li));
    PipelineLayoutInfo pli;
    pli.setLayouts = {layout.get()};
    auto pl = owned(d.createPipelineLayout(pli));
    PipelineStateInfo psi;
    psi.shader = shader.get();
    psi.pipelineLayout = pl.get();
    psi.bindPoint = PipelineBindPoint::COMPUTE;
    auto ps = owned(d.createPipelineState(psi));
    auto *gps = static_cast<CCVKPipelineState *>(ps.get())->gpuPipelineState();
    require(gps && gps->vkPipeline, "real compute pipeline");
    tracked(bits(gps->vkPipeline), 0);
    auto *gs = static_cast<CCVKShader *>(shader.get())->gpuShader();
    require(gs && gs->gpuStages.size() == 1, "compute module");
    tracked(bits(gs->gpuStages[0].vkShader), 1);
    tracked(bits(static_cast<CCVKPipelineLayout *>(pl.get())->gpuPipelineLayout()->vkPipelineLayout), 2);
    tracked(bits(static_cast<CCVKDescriptorSetLayout *>(layout.get())->gpuDescriptorSetLayout()->vkDescriptorSetLayout), 3);
    DescriptorSetInfo di;
    di.layout = layout.get();
    auto set = owned(d.createDescriptorSet(di));
    set->bindBuffer(0, a.get());
    set->bindBuffer(1, b.get());
    set->bindBuffer(2, output.get());
    set->update();
    std::vector<uint32_t> av(bytes / 4), bv(bytes / 4);
    std::array<std::vector<uint32_t>, 4> uploadData;
    if (performance) for (auto &data : uploadData) data.resize(bytes / 4);
    QueryOwner query;
    uint64_t timestampMask = 0; double timestampPeriod = 0;
    if (performance) {
        query.owner = &d;
        query.device = d.gpuDevice()->vkDevice;
        const auto family = static_cast<CCVKQueue *>(d.getQueue())->gpuQueue()->queueFamilyIndex;
        const unsigned bits = s.context->queueFamilyProperties[family].timestampValidBits;
        require(bits && bits <= 64 && vkCreateQueryPool && vkDestroyQueryPool && vkGetQueryPoolResults && vkCmdResetQueryPool && vkCmdWriteTimestamp, "upload timestamp support");
        timestampMask = bits == 64 ? ~uint64_t(0) : ((uint64_t(1) << bits) - 1);
        timestampPeriod = s.context->physicalDeviceProperties.limits.timestampPeriod;
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 2;
        require(vkCreateQueryPool(query.device, &qi, nullptr, &query.pool) == VK_SUCCESS, "upload query created");
    }
    for (unsigned cycle = 0; cycle < (performance ? 1300U : 1U); ++cycle) {
    p = Phase{}; p.inputs = {ga->vkBuffer, gb->vkBuffer}; p.output = go->vkBuffer; p.host = gh->vkBuffer;
    require(d.gpuContext()->validationErrorCount.load() < 10, "per-cycle validation budget");
    const unsigned salt = stressMode ? index * 97 : performance ? cycle * 17 : 0;
    for (unsigned j = 0; j < bytes / 4; ++j) {
        av[j] = 0x12340000 + j + salt;
        bv[j] = 0x1000 + 3 * j;
    }
    if (s.mode == "wrong-input" || (s.mode == "stress-wrong" && index == 23)) bv[0] ^= 1;
    // This fixture declares the known read-only compute consumer explicitly.
    // Descriptor buffers without such metadata remain on the conservative path.
    ga->renderAccessTypes = {THSVS_ACCESS_COMPUTE_SHADER_READ_OTHER};
    gb->renderAccessTypes = {THSVS_ACCESS_COMPUTE_SHADER_READ_OTHER, THSVS_ACCESS_INDIRECT_BUFFER};
    if (s.mode == "fallback") ga->renderAccessTypes.clear();
    if (performance) for (unsigned write = 0; write < 4; ++write)
        for (unsigned j = 0; j < bytes / 4; ++j) uploadData[write][j] = 0x12340000 + j + salt + 3 - write;
    const uint64_t batchStart = performance ? clockNs() : 0;
    // Input generation is outside the measured upload span in performance mode.
    for (unsigned write = 0; write < ((stressMode || performance) ? 4U : 1U); ++write) {
        if (!performance) for (unsigned j = 0; j < bytes / 4; ++j) av[j] = 0x12340000 + j + salt + (stressMode ? 3 - write : 0);
        a->update(performance ? uploadData[write].data() : av.data(), bytes);
        b->update(bv.data(), bytes);
    }
    const uint64_t uploadEnd = performance ? clockNs() : 0;
    s.manager = true;
    d.gpuBarrierManager()->update(d.gpuTransportHub());
    s.manager = false;
    const uint64_t managerEnd = performance ? clockNs() : 0;
    const bool fallback = s.mode == "fallback";
    check(p.managerCalls == 1 && p.globals == unsigned(fallback || baseline) && p.resources == (baseline ? 0U : fallback ? 1U : 2U) && p.matched == p.resources && p.mask == (baseline ? 0U : fallback ? 2U : 3U), "known buffers receive resource-scoped visibility");
    CommandBufferInfo ci;
    ci.queue = d.getQueue();
    auto cmd = owned(d.createCommandBuffer(ci));
    cmd->begin();
    cmd->customCommand([&](void *handle) {
        p.cb = static_cast<VkCommandBuffer>(handle);
        if (query.pool) { vkCmdResetQueryPool(p.cb, query.pool, 0, 2); vkCmdWriteTimestamp(p.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, query.pool, 0); }
        // Unknown descriptor consumption requires an explicit user dependency;
        // the fallback test must not pretend that an empty consumer list supplies it.
        if (fallback) {
            VkBufferMemoryBarrier v{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            v.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            v.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            v.srcQueueFamilyIndex = v.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            v.buffer = p.inputs[0];
            v.size = VK_WHOLE_SIZE;
            cmdPipelineBarrier(d.gpuDevice(), p.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &v, 0, nullptr);
        }
    });
    s.armed = true;
    cmd->bindPipelineState(ps.get());
    cmd->bindDescriptorSet(0, set.get());
    DispatchInfo dispatch;
    dispatch.groupCountX = bytes / 64;
    dispatch.groupCountY = dispatch.groupCountZ = 1;
    cmd->dispatch(dispatch);
    cmd->customCommand([&](void *handle) {
        VkBufferMemoryBarrier v{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        v.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        v.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        v.srcQueueFamilyIndex = v.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        v.buffer = p.output;
        v.size = VK_WHOLE_SIZE;
        cmdPipelineBarrier(d.gpuDevice(), static_cast<VkCommandBuffer>(handle), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &v, 0, nullptr);
        VkBufferCopy r{0, 0, bytes};
        vkCmdCopyBuffer(static_cast<VkCommandBuffer>(handle), p.output, p.host, 1, &r);
        v.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        v.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        v.buffer = p.host;
        cmdPipelineBarrier(d.gpuDevice(), static_cast<VkCommandBuffer>(handle), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &v, 0, nullptr);
    });
    if (query.pool) cmd->customCommand([&](void *handle) { vkCmdWriteTimestamp(static_cast<VkCommandBuffer>(handle), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query.pool, 1); });
    cmd->end();
    if (!performance || cycle == 1299) {
    a->resize(bytes * 2);
    require(static_cast<CCVKBuffer *>(a.get())->gpuBuffer()->vkBuffer != p.inputs[0], "resize replaces physical input");
    a.reset();
    b.reset();
    set.reset();
    ps.reset();
    }
    unsigned premature = 0;
    for (unsigned j = index * 4; j < s.count; ++j) premature += s.objects[j].destroyed;
    check(!premature, "recording retains shader/layout/pipeline before submit");
    auto *submit = cmd.get();
    const uint64_t submitStart = performance ? clockNs() : 0;
    d.getQueue()->submit(&submit, 1);
    const uint64_t submitEnd = performance ? clockNs() : 0;
    { WaitScope scope(performance); d.waitAllFences(); }
    const uint64_t batchEnd = performance ? clockNs() : 0;
    s.armed = false;
    require(p.cb && p.managerCB && p.cb != p.managerCB && p.dispatches == 1 && p.copies == 1 && p.submits == 1 && p.managerSubmits == 1, "actual barrier/dispatch/copy/submission identity chains");
    require(vmaInvalidateAllocation(d.gpuDevice()->memoryAllocator, gh->vmaAllocation, 0, bytes) == VK_SUCCESS, "readback invalidate after completion");
    for (unsigned j = 0; j < bytes / 4; ++j) {
        uint32_t actual = 0;
        std::memcpy(&actual, gh->mappedData + j * 4, 4);
        p.mismatch += actual != (0x12340000 + j + salt + 0x1000 + 3 * j);
        p.checked += 4;
    }
    check(!p.mismatch, "compute whole-buffer golden");
    if (query.pool) {
        uint64_t values[4]{};
        require(vkGetQueryPoolResults(query.device, query.pool, 0, 2, sizeof(values), values, 2 * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_SUCCESS && values[1] && values[3], "upload GPU timestamps available");
        if (cycle >= 300) {
            require(s.timingCount < s.timings.size(), "upload sample capacity");
            s.timings[s.timingCount++] = {batchEnd - batchStart, uploadEnd - batchStart, managerEnd - uploadEnd, uint64_t(double((values[2] - values[0]) & timestampMask) * timestampPeriod), p.checked, s.stagingSourceCount, submitStart - managerEnd, submitEnd - submitStart, batchEnd - submitEnd, s.waitApi, s.waitCalls, s.waitFailures};
        }
    }
    if (performance) {
        require(p.uploads == 8 && s.stagingSourceCount <= 4, "eight actual uploads with bounded staging sources");
        CC_LOG_INFO("project-visibility batch: cycle=%u bytes=%u managerCalls=%u global=%u resources=%u matched=%u dispatches=%u copies=%u submits=%u managerSubmits=%u checkedBytes=%u mismatches=%u pendingDestroy=%u resized=%u uploads=%u stagingSources=%u", cycle, bytes, p.managerCalls, p.globals, p.resources, p.matched, p.dispatches, p.copies, p.submits, p.managerSubmits, p.checked, p.mismatch, premature, unsigned(cycle == 1299), p.uploads, s.stagingSourceCount);
        cmd.reset();
        // GPU completion/recording retirement is proven above; reset honors staging leases.
        d.gpuStagingBufferPool()->reset();
    }
    else CC_LOG_INFO("project-visibility phase: index=%u bytes=%u managerCalls=%u global=%u resources=%u matched=%u dispatches=%u copies=%u submits=%u managerSubmits=%u checkedBytes=%u mismatches=%u pendingDestroy=%u resized=1", index, bytes, p.managerCalls, p.globals, p.resources, p.matched, p.dispatches, p.copies, p.submits, p.managerSubmits, p.checked, p.mismatch, premature);
    }
}
void texturePhase(CCVKDevice &d, unsigned index) {
    const unsigned width = index % 2 ? 257 : 129, height = 127, bytes = width * height * 4;
    TextureInfo ti;
    ti.type = TextureType::TEX2D; ti.format = Format::RGBA8; ti.width = width; ti.height = height;
    ti.usage = TextureUsageBit::TRANSFER_DST | TextureUsageBit::TRANSFER_SRC | TextureUsageBit::SAMPLED;
    auto texture = owned(d.createTexture(ti));
    auto *gt = static_cast<CCVKTexture *>(texture.get())->gpuTexture();
    require(gt && gt->vkImage, "texture physical image");
    BufferInfo bi; bi.usage = BufferUsageBit::TRANSFER_DST; bi.memUsage = MemoryUsageBit::HOST; bi.size = bytes;
    auto host = owned(d.createBuffer(bi));
    auto *gh = static_cast<CCVKBuffer *>(host.get())->gpuBuffer();
    require(gh && gh->vkBuffer && gh->mappedData, "texture mapped readback");
    std::vector<uint8_t> pixels(bytes);
    BufferTextureCopy region; region.texExtent = {width, height, 1};
    s.textureImage = gt->vkImage; s.textureWidth = width; s.textureHeight = height; s.textureUploads = 0;
    for (unsigned write = 0; write < 4; ++write) {
        for (unsigned j = 0; j < bytes; ++j) pixels[j] = uint8_t(j * 13 + index * 7 + write);
        if (s.mode == "stress-wrong" && index == 23 && write == 3) pixels[0] ^= 1;
        const uint8_t *data = pixels.data();
        static_cast<Device &>(d).copyBuffersToTexture(&data, texture.get(), &region, 1);
    }
    CommandBufferInfo ci; ci.queue = d.getQueue();
    auto cmd = owned(d.createCommandBuffer(ci));
    s.textureImage = gt->vkImage; s.textureHost = gh->vkBuffer;
    s.textureCopies = s.textureSubmits = 0;
    cmd->begin();
    cmd->customCommand([&](void *handle) {
        s.textureCB = static_cast<VkCommandBuffer>(handle);
        VkImageMemoryBarrier image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        image.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; image.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        image.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; image.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        image.srcQueueFamilyIndex = image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image.image = s.textureImage; image.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        cmdPipelineBarrier(d.gpuDevice(), s.textureCB, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &image);
        VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(s.textureCB, s.textureImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.textureHost, 1, &copy);
        VkBufferMemoryBarrier buffer{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        buffer.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; buffer.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        buffer.srcQueueFamilyIndex = buffer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        buffer.buffer = s.textureHost; buffer.size = VK_WHOLE_SIZE;
        cmdPipelineBarrier(d.gpuDevice(), s.textureCB, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &buffer, 0, nullptr);
    });
    cmd->end();
    auto *submit = cmd.get(); d.getQueue()->submit(&submit, 1); d.waitAllFences();
    require(s.textureCB && s.textureCopies == 1 && s.textureSubmits == 1 && s.textureUploads == 4, "texture copy submitted identity");
    require(vmaInvalidateAllocation(d.gpuDevice()->memoryAllocator, gh->vmaAllocation, 0, bytes) == VK_SUCCESS, "texture invalidate");
    unsigned mismatches = 0;
    for (unsigned j = 0; j < bytes; ++j) mismatches += uint8_t(gh->mappedData[j]) != uint8_t(j * 13 + index * 7 + 3);
    check(!mismatches, "texture whole-image golden");
    CC_LOG_INFO("project-visibility texture: index=%u width=%u height=%u uploads=%u copies=%u submits=%u checkedBytes=%u mismatches=%u", index, width, height, s.textureUploads, s.textureCopies, s.textureSubmits, bytes, mismatches);
    s.textureCB = VK_NULL_HANDLE; s.textureImage = VK_NULL_HANDLE; s.textureHost = VK_NULL_HANDLE;
}
void run() noexcept {
    s.started = true;
    try {
        auto *d = CCVKDevice::getInstance();
        require(d && d->gpuDevice() && d->gpuContext(), "existing real project device");
        s.device = d->gpuDevice()->vkDevice;
        s.context = d->gpuContext();
        const bool enabled = s.context->validationEnabled && s.context->synchronizationValidationEnabled && s.context->bestPracticesValidationEnabled;
        require(enabled || (perf() && !s.context->validationEnabled && !s.context->synchronizationValidationEnabled && !s.context->bestPracticesValidationEnabled), "Validation triad or explicit OFF performance tree");
        CC_LOG_INFO("project-visibility validation: %s", enabled ? "ENABLED" : "DISABLED");
        s.profile = perf();
        install();
        if (perf()) { phase(*d, 0, 131072); } else if (stress()) {
            for (unsigned i = 0; i < 24; ++i) {
                phase(*d, i, i % 2 ? 131072 : 65536);
                texturePhase(*d, i);
            }
        } else {
            phase(*d, 0, 64); phase(*d, 1, 128); phase(*d, 2, 64);
        }
    } catch (const std::exception &e) {
        check(false, e.what());
    } catch (...) {
        check(false, "diagnostic exception");
    }
    s.manager = s.armed = s.profile = s.waitScope = false;
    s.textureCB = VK_NULL_HANDLE; s.textureImage = VK_NULL_HANDLE; s.textureHost = VK_NULL_HANDLE;
}
} // namespace
bool prepareVisibility() noexcept {
    if (s.prepared) return !s.failures;
    s.prepared = true;
    s.enabled = present("CC_VULKAN_REAL_PROJECT_VISIBILITY");
    if (!s.enabled) return true;
    try {
        s.mode = std::getenv("CC_VULKAN_REAL_PROJECT_VISIBILITY");
        require(s.mode == "content" || s.mode == "wrong-input" || s.mode == "fallback" || stress() || perf(), "known mode");
        for (const auto *n : {"CC_VULKAN_REAL_PROJECT_PREWARM", "CC_VULKAN_REAL_PROJECT_INDIRECT", "CC_VULKAN_REAL_PROJECT_SUBPASS", "CC_VULKAN_REAL_PROJECT_RESOLVE", "CC_VULKAN_REAL_PROJECT_FEATURES", "CC_VULKAN_REAL_PROJECT_WSI", "CC_VULKAN_REAL_PROJECT_PERFORMANCE"}) require(!present(n), "exclusive diagnostic");
        CC_LOG_INFO("project-visibility mode: %s enabled=1", s.mode.c_str());
        return true;
    } catch (...) {
        return false;
    }
}
void startVisibility() noexcept {
    if (!s.enabled || s.failures) return;
    check(!s.started, "start once");
    if (auto *a = DeviceAgent::getInstance()) {
        auto *q = a->getMessageQueue();
        if (!q) {
            check(false, "render message queue");
            return;
        }
        ENQUEUE_MESSAGE_0(q, RealProjectVisibility, { run(); });
        q->kickAndWait();
    } else
        run();
}
int finishVisibility(int platformResult) noexcept {
    if (!s.enabled) return platformResult;
    unsigned destroyed = 0;
    for (unsigned i = 0; i < s.count; ++i) {
        check(s.objects[i].destroyed == 1, "exact tracked retirement");
        destroyed += s.objects[i].destroyed;
    }
    check(s.started && s.count == (perf() ? 4U : stress() ? 96U : 12U) && destroyed == s.count && s.devices == 1 && !s.live && !s.post && !platformResult, "actual project shutdown");
    if (perf()) {
        check(s.timingCount == 1000, "1000 upload samples after 300 warmup");
        FILE *file = std::fopen("upload-performance.csv", "wb");
        if (!file) check(false, "upload CSV open");
        else {
            bool good = std::fprintf(file, "sample,batch_ns,upload_ns,manager_ns,compute_cb_gpu_ns,checked_bytes,staging_sources,record_ns,submit_ns,completion_ns,wait_api_ns,wait_calls,wait_failures\n") > 0;
            for (unsigned i = 0; i < s.timingCount; ++i) { const auto &v = s.timings[i]; good = (std::fprintf(file, "%u,%llu,%llu,%llu,%llu,%u,%u,%llu,%llu,%llu,%llu,%u,%u\n", i, static_cast<unsigned long long>(v.batch), static_cast<unsigned long long>(v.upload), static_cast<unsigned long long>(v.manager), static_cast<unsigned long long>(v.gpu), v.checked, v.stagingSources, static_cast<unsigned long long>(v.record), static_cast<unsigned long long>(v.submit), static_cast<unsigned long long>(v.completion), static_cast<unsigned long long>(v.waitApi), v.waitCalls, v.waitFailures) > 0) && good; }
            good = std::fflush(file) == 0 && good; good = std::fclose(file) == 0 && good; check(good, "upload CSV write");
        }
        CC_LOG_INFO("project-visibility samples: warmup=300 count=%u bytes=131072", s.timingCount);
    }
    CC_LOG_INFO("project-visibility retirement: created=%u destroyed=%u devices=%u", s.count, destroyed, s.devices);
    CC_LOG_INFO("project-visibility completed: failures=%u liveErrors=%u postErrors=%u outcome=%s", s.failures, s.live, s.post, s.failures ? "FAILED" : "CLEAN");
    restore();
    return s.failures || platformResult ? 1 : 0;
}
} // namespace cc::vulkan_test
