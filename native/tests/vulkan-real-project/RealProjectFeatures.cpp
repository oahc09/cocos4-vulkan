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

// Default-off Secondary/FSR content and variant-failure diagnostic in the real Windows project.
#include "RealProjectFeatures.h"
#include <windows.h>
#include "renderdoc/renderdoc_app.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "base/threading/MessageQueue.h"
#include "gfx-agent/DeviceAgent.h"
#include "gfx-base/GFXCommandBuffer.h"
#include "gfx-base/GFXQueue.h"
#include "gfx-vulkan/VKBuffer.h"
#include "gfx-vulkan/VKDescriptorSet.h"
#include "gfx-vulkan/VKDescriptorSetLayout.h"
#include "gfx-vulkan/VKDevice.h"
#include "gfx-vulkan/VKFramebuffer.h"
#include "gfx-vulkan/VKPipelineLayout.h"
#include "gfx-vulkan/VKPipelineState.h"
#include "gfx-vulkan/VKRenderPass.h"
#include "gfx-vulkan/VKShader.h"
#include "gfx-vulkan/VKTexture.h"
#include "gfx-vulkan/VKUtils.h"

namespace cc::vulkan_test {
namespace {
using namespace gfx;
enum class Kind { Module,
                  Pipeline,
                  Layout,
                  Pass,
                  Framebuffer,
                  DescriptorLayout };
struct Object {
    Kind kind{};
    uint64_t handle = 0;
    unsigned destroyed = 0;
};
struct Phase {
    uint32_t width = 0, height = 0, subpass = 0;
    unsigned begins = 0, ends = 0, next = 0, dynamic = 0, submitted = 0, pipelines = 0;
    std::array<unsigned, 2> draws{}, copies{}, mismatch{};
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkRenderPass rp = VK_NULL_HANDLE;
    std::array<VkPipeline, 2> pipeline{};
    VkPipeline bound = VK_NULL_HANDLE;
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
    std::array<VkImage, 2> images{};
    VkImageView inputView = VK_NULL_HANDLE;
    bool rpContract = false, chain = false, inheritance = false, fsrNode = false;
    VkCommandBuffer secondary = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    unsigned executes = 0;
    unsigned injected = 0;
};
struct State {
    bool prepared = false, enabled = false, started = false, installed = false, armed = false, creating = false, unsupported = false;
    unsigned failures = 0, phase = 0, objects = 0, devices = 0, liveErrors = 0, postErrors = 0;
    unsigned bufferFacades = 0, imageFacades = 0, bufferDrops = 0, imageDrops = 0;
    std::string mode;
    VkDevice device = VK_NULL_HANDLE;
    CCVKGPUContext *context = nullptr;
    std::array<Object, 64> object{};
    std::array<Phase, 3> phases{};
    PFN_vkCreateRenderPass2 createPass = nullptr;
    PFN_vkBeginCommandBuffer beginCB = nullptr;
    PFN_vkCmdExecuteCommands execute = nullptr;
    PFN_vkCmdBeginRenderPass begin = nullptr;
    PFN_vkCmdEndRenderPass end = nullptr;
    PFN_vkCmdNextSubpass next = nullptr;
    PFN_vkCmdBeginRendering dynamic = nullptr;
    PFN_vkCmdDraw draw = nullptr;
    PFN_vkCmdBindPipeline bind = nullptr;
    PFN_vkCmdBindDescriptorSets descriptors = nullptr;
    PFN_vkCmdCopyImageToBuffer copy = nullptr;
    PFN_vkQueueSubmit submit = nullptr;
    PFN_vkQueueSubmit2 submit2 = nullptr;
    PFN_vkQueueSubmit2KHR submit2KHR = nullptr;
    PFN_vkCreateShaderModule createModule = nullptr;
    PFN_vkCreateGraphicsPipelines createPipeline = nullptr;
    PFN_vkDestroyShaderModule destroyModule = nullptr;
    PFN_vkDestroyPipeline destroyPipeline = nullptr;
    PFN_vkDestroyPipelineLayout destroyLayout = nullptr;
    PFN_vkDestroyRenderPass destroyPass = nullptr;
    PFN_vkDestroyFramebuffer destroyFramebuffer = nullptr;
    PFN_vkDestroyDescriptorSetLayout destroyDescriptorLayout = nullptr;
    PFN_vkDestroyDevice destroyDevice = nullptr;
} s;
bool isFsr() { return s.mode == "fsr" || s.mode == "fsr-wrong-rate"; }
bool isVariantFailure() { return s.mode == "secondary-variant-failure"; }
void check(bool ok, const char *label) noexcept {
    if (!ok) {
        ++s.failures;
        CC_LOG_ERROR("project-features failure: %s", label);
    }
}
void require(bool ok, const char *label) {
    check(ok, label);
    if (!ok) throw std::runtime_error(label);
}
bool present(const wchar_t *name) noexcept {
    SetLastError(ERROR_SUCCESS);
    return GetEnvironmentVariableW(name, nullptr, 0) != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
}
template <class T>
uint64_t bits(T value) noexcept {
    uint64_t out = 0;
    static_assert(sizeof(T) <= sizeof(out));
    std::memcpy(&out, &value, sizeof(T));
    return out;
}
template <class T>
void remember(Kind kind, T value) noexcept {
    const auto h = bits(value);
    if (!h || s.objects == s.object.size()) {
        check(false, "bounded nonnull object");
        return;
    }
    for (unsigned i = 0; i < s.objects; ++i)
        if (s.object[i].kind == kind && s.object[i].handle == h) {
            check(false, "unique object handle");
            return;
        }
    s.object[s.objects++] = {kind, h, 0};
}
template <class T>
void destroyed(Kind kind, T value) noexcept {
    for (unsigned i = 0; i < s.objects; ++i)
        if (s.object[i].kind == kind && s.object[i].handle == bits(value)) {
            check(++s.object[i].destroyed == 1, "destroy exactly once");
            return;
        }
}
VKAPI_ATTR VkResult VKAPI_CALL passHook(VkDevice d, const VkRenderPassCreateInfo2 *i, const VkAllocationCallbacks *a, VkRenderPass *o) {
    if (!s.createPass) return VK_ERROR_INITIALIZATION_FAILED;
    const auto r = s.createPass(d, i, a, o);
    if (s.creating && r == VK_SUCCESS) {
        auto &p = s.phases[s.phase];
        bool ok = i && o && *o && i->subpassCount == 1 && i->pSubpasses && i->pSubpasses[0].colorAttachmentCount == 1 && i->pSubpasses[0].pColorAttachments && i->pSubpasses[0].pColorAttachments[0].attachment == 0;
        if (ok && isFsr()) {
            const auto *f = static_cast<const VkFragmentShadingRateAttachmentInfoKHR *>(i->pSubpasses[0].pNext);
            ok = f && f->sType == VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR && f->pFragmentShadingRateAttachment && f->pFragmentShadingRateAttachment->attachment == 1 && f->shadingRateAttachmentTexelSize.width == 16 && f->shadingRateAttachmentTexelSize.height == 16;
        }
        p.rpContract = ok;
        check(ok, "actual RP contract");
        if (o && *o) {
            p.rp = *o;
            remember(Kind::Pass, *o);
        }
    }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL beginCBHook(VkCommandBuffer cb, const VkCommandBufferBeginInfo *i) {
    if (!s.beginCB) return VK_ERROR_INITIALIZATION_FAILED;
    const auto r = s.beginCB(cb, i);
    if (s.armed && r == VK_SUCCESS && i && i->pInheritanceInfo && (i->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT)) {
        auto &p = s.phases[s.phase];
        const auto &h = *i->pInheritanceInfo;
        p.secondary = cb;
        p.inheritance = h.renderPass == p.rp && h.framebuffer == p.framebuffer && h.subpass == 0 && !h.pNext && !h.occlusionQueryEnable && !h.queryFlags && !h.pipelineStatistics;
        check(p.inheritance, "real secondary inheritance");
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL executeHook(VkCommandBuffer cb, uint32_t count, const VkCommandBuffer *commands) {
    if (!s.execute) {
        check(false, "execute PFN");
        return;
    }
    s.execute(cb, count, commands);
    if (s.armed) {
        auto &p = s.phases[s.phase];
        check(cb == p.cb && count == 1 && commands && commands[0] == p.secondary, "actual secondary execute identity");
        ++p.executes;
    }
}
VKAPI_ATTR void VKAPI_CALL beginHook(VkCommandBuffer cb, const VkRenderPassBeginInfo *i, VkSubpassContents c) {
    if (!s.begin) {
        check(false, "begin PFN");
        return;
    }
    s.begin(cb, i, c);
    if (s.armed) {
        auto &p = s.phases[s.phase];
        check(i && i->renderPass == p.rp && c == (isFsr() ? VK_SUBPASS_CONTENTS_INLINE : VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS), "actual Classic RP");
        p.cb = cb;
        ++p.begins;
        p.subpass = 0;
    }
}
VKAPI_ATTR void VKAPI_CALL endHook(VkCommandBuffer cb) {
    if (!s.end) {
        check(false, "end PFN");
        return;
    }
    s.end(cb);
    if (s.armed) {
        auto &p = s.phases[s.phase];
        check(cb == p.cb && p.subpass == 0, "end identity/subpass");
        ++p.ends;
    }
}
VKAPI_ATTR void VKAPI_CALL nextHook(VkCommandBuffer cb, VkSubpassContents c) {
    if (!s.next) {
        check(false, "next PFN");
        return;
    }
    s.next(cb, c);
    if (s.armed) {
        auto &p = s.phases[s.phase];
        check(cb == p.cb && c == VK_SUBPASS_CONTENTS_INLINE && p.subpass == 0, "next identity");
        ++p.next;
        p.subpass = 1;
    }
}
VKAPI_ATTR void VKAPI_CALL dynamicHook(VkCommandBuffer cb, const VkRenderingInfo *i) {
    if (!s.dynamic) {
        check(false, "dynamic PFN");
        return;
    }
    s.dynamic(cb, i);
    if (s.armed) ++s.phases[s.phase].dynamic;
}
VKAPI_ATTR void VKAPI_CALL bindHook(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipeline pipeline) {
    if (!s.bind) {
        check(false, "bind PFN");
        return;
    }
    s.bind(cb, point, pipeline);
    if (s.armed) {
        auto &p = s.phases[s.phase];
        check(cb == (isFsr() ? p.cb : p.secondary) && point == VK_PIPELINE_BIND_POINT_GRAPHICS, "bind identity");
        p.bound = pipeline;
    }
}
VKAPI_ATTR void VKAPI_CALL descriptorsHook(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t first, uint32_t count, const VkDescriptorSet *sets, uint32_t offsets, const uint32_t *values) {
    if (!s.descriptors) {
        check(false, "descriptor bind PFN");
        return;
    }
    s.descriptors(cb, point, layout, first, count, sets, offsets, values);
    if (s.armed && first == 0 && count && sets) {
        auto &p = s.phases[s.phase];
        check(cb == p.cb && point == VK_PIPELINE_BIND_POINT_GRAPHICS, "descriptor identity");
        p.descriptor = sets[0];
    }
}
VKAPI_ATTR void VKAPI_CALL drawHook(VkCommandBuffer cb, uint32_t n, uint32_t instances, uint32_t first, uint32_t instance) {
    if (!s.draw) {
        check(false, "draw PFN");
        return;
    }
    s.draw(cb, n, instances, first, instance);
    if (s.armed) {
        auto &p = s.phases[s.phase];
        check(cb == (isFsr() ? p.cb : p.secondary) && n == 3 && instances == 1 && !first && !instance && p.bound == p.pipeline[0], "actual feature draw identity/Classic PSO");
        ++p.draws[0];
    }
}
VKAPI_ATTR void VKAPI_CALL copyHook(VkCommandBuffer cb, VkImage image, VkImageLayout layout, VkBuffer buffer, uint32_t count, const VkBufferImageCopy *regions) {
    if (!s.copy) {
        check(false, "copy PFN");
        return;
    }
    s.copy(cb, image, layout, buffer, count, regions);
    if (s.armed) {
        auto &p = s.phases[s.phase];
        check(cb == p.cb && count == 1 && regions && layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, "copy identity");
        if (regions) check(regions[0].imageExtent.width == p.width && regions[0].imageExtent.height == p.height && regions[0].bufferOffset == 0 && regions[0].imageSubresource.mipLevel == 0 && regions[0].imageSubresource.baseArrayLayer == 0, "copy range");
        unsigned matched = 0;
        for (unsigned j = 0; j < 2; ++j)
            if (image == p.images[j]) {
                ++p.copies[j];
                ++matched;
            }
        check(matched == 1, "copy diagnostic image");
    }
}
void forwarded(VkCommandBuffer cb) noexcept {
    if (s.armed && cb == s.phases[s.phase].cb) ++s.phases[s.phase].submitted;
}
VKAPI_ATTR VkResult VKAPI_CALL submitHook(VkQueue q, uint32_t count, const VkSubmitInfo *info, VkFence fence) {
    if (!s.submit) return VK_ERROR_INITIALIZATION_FAILED;
    const auto r = s.submit(q, count, info, fence);
    if (s.armed && r == VK_SUCCESS && info)
        for (unsigned a = 0; a < count; ++a)
            if (info[a].pCommandBuffers)
                for (unsigned b = 0; b < info[a].commandBufferCount; ++b) forwarded(info[a].pCommandBuffers[b]);
    return r;
}
VkResult submit2Forward(PFN_vkQueueSubmit2 fn, VkQueue q, uint32_t count, const VkSubmitInfo2 *info, VkFence fence) {
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    const auto r = fn(q, count, info, fence);
    if (s.armed && r == VK_SUCCESS && info)
        for (unsigned a = 0; a < count; ++a)
            if (info[a].pCommandBufferInfos)
                for (unsigned b = 0; b < info[a].commandBufferInfoCount; ++b) forwarded(info[a].pCommandBufferInfos[b].commandBuffer);
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL submit2Hook(VkQueue q, uint32_t n, const VkSubmitInfo2 *i, VkFence f) { return submit2Forward(s.submit2, q, n, i, f); }
VKAPI_ATTR VkResult VKAPI_CALL submit2KHRHook(VkQueue q, uint32_t n, const VkSubmitInfo2 *i, VkFence f) { return submit2Forward(s.submit2KHR, q, n, i, f); }
VKAPI_ATTR VkResult VKAPI_CALL moduleHook(VkDevice d, const VkShaderModuleCreateInfo *i, const VkAllocationCallbacks *a, VkShaderModule *o) {
    if (!s.createModule) return VK_ERROR_INITIALIZATION_FAILED;
    const auto r = s.createModule(d, i, a, o);
    if (s.creating && r == VK_SUCCESS && o) remember(Kind::Module, *o);
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL pipelineHook(VkDevice d, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo *i, const VkAllocationCallbacks *a, VkPipeline *o) {
    if (!s.createPipeline) return VK_ERROR_INITIALIZATION_FAILED;
    const auto r = s.createPipeline(d, cache, n, i, a, o);
    if (s.creating && r == VK_SUCCESS && i && o) {
        auto &p = s.phases[s.phase];
        for (unsigned j = 0; j < n; ++j) {
            check(o[j] && (!i[j].renderPass || i[j].renderPass == p.rp) && i[j].subpass == 0, "real feature pipeline");
            p.pipeline[i[j].renderPass ? 0 : 1] = o[j];
            ++p.pipelines;
            remember(Kind::Pipeline, o[j]);
            if (isFsr()) {
                const auto *f = static_cast<const VkPipelineFragmentShadingRateStateCreateInfoKHR *>(i[j].pNext);
                p.fsrNode = f && f->sType == VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR && f->fragmentSize.width == 1 && f->fragmentSize.height == 1 && f->combinerOps[0] == VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR && f->combinerOps[1] == VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR;
                check(i[j].renderPass && p.fsrNode, "Classic FSR composer driver node");
            }
        }
        // Diagnostic-only fault: a successful real driver creation provides the
        // partial output; report OOM only to the optional Dynamic factory call.
        // The production factory must destroy that output and preserve Classic.
        if (isVariantFailure() && n == 1 && !i[0].renderPass && o[0]) {
            ++p.injected;
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }
    return r;
}

#define DESTROY(Name, Handle, Member, K)                                                    \
    VKAPI_ATTR void VKAPI_CALL Name(VkDevice d, Handle h, const VkAllocationCallbacks *a) { \
        if (!s.Member) {                                                                    \
            check(false, "destroy PFN");                                                    \
            return;                                                                         \
        }                                                                                   \
        s.Member(d, h, a);                                                                  \
        destroyed(Kind::K, h);                                                              \
    }
DESTROY(moduleDestroy, VkShaderModule, destroyModule, Module)
DESTROY(pipelineDestroy, VkPipeline, destroyPipeline, Pipeline)
DESTROY(layoutDestroy, VkPipelineLayout, destroyLayout, Layout)
DESTROY(passDestroy, VkRenderPass, destroyPass, Pass)
DESTROY(framebufferDestroy, VkFramebuffer, destroyFramebuffer, Framebuffer)
DESTROY(descriptorLayoutDestroy, VkDescriptorSetLayout, destroyDescriptorLayout, DescriptorLayout)
#undef DESTROY
VKAPI_ATTR void VKAPI_CALL deviceDestroy(VkDevice d, const VkAllocationCallbacks *a) {
    if (!s.destroyDevice) {
        check(false, "device destroy PFN");
        return;
    }
    check(d == s.device && !s.devices, "expected device shutdown");
    if (s.context) s.liveErrors = s.context->validationErrorCount.load();
    s.destroyDevice(d, a);
    if (s.context) s.postErrors = s.context->validationErrorCount.load();
    s.context = nullptr;
    ++s.devices;
}
void install() {
#define HOOK(Global, Member, Function) \
    s.Member = Global;                 \
    Global = Function;
    // All required pointers are checked before replacing any global entry.
    require(vkCmdBeginRenderPass && vkCmdEndRenderPass && vkCmdNextSubpass && vkCmdDraw && vkCmdBindPipeline && vkCmdBindDescriptorSets && vkCmdCopyImageToBuffer && vkQueueSubmit && vkCreateShaderModule && vkCreateGraphicsPipelines && vkDestroyDevice && vkDestroyShaderModule && vkDestroyPipeline && vkDestroyPipelineLayout && vkDestroyRenderPass && vkDestroyFramebuffer && vkDestroyDescriptorSetLayout, "required hook PFNs");
    require(vkBeginCommandBuffer && vkCmdExecuteCommands, "secondary PFNs");
    HOOK(vkBeginCommandBuffer, beginCB, beginCBHook)
    HOOK(vkCmdExecuteCommands, execute, executeHook)
    HOOK(vkCmdBeginRenderPass, begin, beginHook)
    HOOK(vkCmdEndRenderPass, end, endHook)
    HOOK(vkCmdNextSubpass, next, nextHook)
    if (vkCmdBeginRendering) {
        HOOK(vkCmdBeginRendering, dynamic, dynamicHook)
    }
    HOOK(vkCmdDraw, draw, drawHook)
    HOOK(vkCmdBindPipeline, bind, bindHook)
    HOOK(vkCmdBindDescriptorSets, descriptors, descriptorsHook) HOOK(vkCmdCopyImageToBuffer, copy, copyHook)
        HOOK(vkQueueSubmit, submit, submitHook) if (vkQueueSubmit2) { HOOK(vkQueueSubmit2, submit2, submit2Hook) }
    if (vkQueueSubmit2KHR) {
        HOOK(vkQueueSubmit2KHR, submit2KHR, submit2KHRHook)
    }
    HOOK(vkCreateShaderModule, createModule, moduleHook)
    HOOK(vkCreateGraphicsPipelines, createPipeline, pipelineHook)
    HOOK(vkDestroyShaderModule, destroyModule, moduleDestroy)
    HOOK(vkDestroyPipeline, destroyPipeline, pipelineDestroy)
    HOOK(vkDestroyPipelineLayout, destroyLayout, layoutDestroy)
    HOOK(vkDestroyRenderPass, destroyPass, passDestroy)
    HOOK(vkDestroyFramebuffer, destroyFramebuffer, framebufferDestroy)
    HOOK(vkDestroyDescriptorSetLayout, destroyDescriptorLayout, descriptorLayoutDestroy) HOOK(vkDestroyDevice, destroyDevice, deviceDestroy)
#undef HOOK
        s.installed = true;
}
void restore() noexcept {
    if (!s.installed) return;
#define RESTORE(Global, Member) Global = s.Member;
    RESTORE(vkBeginCommandBuffer, beginCB)
    RESTORE(vkCmdExecuteCommands, execute)
    RESTORE(vkCmdBeginRenderPass, begin)
    RESTORE(vkCmdEndRenderPass, end)
    RESTORE(vkCmdNextSubpass, next)
    if (s.dynamic) {
        RESTORE(vkCmdBeginRendering, dynamic)
    }
    RESTORE(vkCmdDraw, draw)
    RESTORE(vkCmdBindPipeline, bind)
    RESTORE(vkCmdBindDescriptorSets, descriptors) RESTORE(vkCmdCopyImageToBuffer, copy)
        RESTORE(vkQueueSubmit, submit) if (s.submit2) { RESTORE(vkQueueSubmit2, submit2) }
    if (s.submit2KHR) {
        RESTORE(vkQueueSubmit2KHR, submit2KHR)
    }
    RESTORE(vkCreateShaderModule, createModule)
    RESTORE(vkCreateGraphicsPipelines, createPipeline)
    RESTORE(vkDestroyShaderModule, destroyModule)
    RESTORE(vkDestroyPipeline, destroyPipeline)
    RESTORE(vkDestroyPipelineLayout, destroyLayout)
    RESTORE(vkDestroyRenderPass, destroyPass)
    RESTORE(vkDestroyFramebuffer, destroyFramebuffer) RESTORE(vkDestroyDescriptorSetLayout, destroyDescriptorLayout) RESTORE(vkDestroyDevice, destroyDevice)
#undef RESTORE
        s.installed = false;
}
template <class T>
struct Drop {
    void operator()(T *p) const {
        if (p) {
            if constexpr (std::is_same_v<T, Buffer>) ++s.bufferDrops;
            if constexpr (std::is_same_v<T, Texture>) ++s.imageDrops;
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
    if constexpr (std::is_same_v<T, Buffer>) ++s.bufferFacades;
    if constexpr (std::is_same_v<T, Texture>) ++s.imageFacades;
    return Owned<T>(p);
}

void phase(CCVKDevice &d, unsigned index, uint32_t width, uint32_t height) {
    require(d.gpuContext()->validationErrorCount.load() < 10, "validation error budget");
    s.phase = index;
    auto &p = s.phases[index];
    p.width = width;
    p.height = height;
    s.creating = true;
    GeneralBarrierInfo colorBarrier;
    colorBarrier.nextAccesses = AccessFlagBit::COLOR_ATTACHMENT_WRITE;
    RenderPassInfo ri;
    ri.colorAttachments.resize(isFsr() ? 2 : 1);
    ri.colorAttachments[0].format = Format::RGBA8;
    ri.colorAttachments[0].loadOp = LoadOp::CLEAR;
    ri.colorAttachments[0].storeOp = StoreOp::STORE;
    ri.colorAttachments[0].barrier = d.getGeneralBarrier(colorBarrier);
    ri.subpasses.resize(1);
    ri.subpasses[0].colors = {0};
    if (isFsr()) {
        GeneralBarrierInfo b;
        b.prevAccesses = b.nextAccesses = AccessFlagBit::SHADING_RATE;
        ri.colorAttachments[1].format = Format::R8UI;
        ri.colorAttachments[1].loadOp = LoadOp::LOAD;
        ri.colorAttachments[1].storeOp = StoreOp::DISCARD;
        ri.colorAttachments[1].barrier = d.getGeneralBarrier(b);
        ri.subpasses[0].shadingRate = 1;
    }
    auto rp = owned(d.createRenderPass(ri));
    require(p.rpContract && p.rp, "driver renderpass contract");
    auto texture = owned(d.createTexture({TextureType::TEX2D, TextureUsageBit::COLOR_ATTACHMENT | TextureUsageBit::TRANSFER_SRC, Format::RGBA8, width, height}));
    p.images[0] = static_cast<CCVKTexture *>(texture.get())->gpuTexture()->vkImage;
    Owned<Texture> rate;
    Owned<Buffer> rateHost;
    const uint32_t rateWidth = (width + 15) / 16, rateHeight = (height + 15) / 16;
    if (isFsr()) {
        rate = owned(d.createTexture({TextureType::TEX2D, TextureUsageBit::SHADING_RATE | TextureUsageBit::TRANSFER_DST, Format::R8UI, rateWidth, rateHeight}));
        rateHost = owned(d.createBuffer({BufferUsageBit::TRANSFER_SRC, MemoryUsageBit::HOST, rateWidth * rateHeight, 1}));
        auto *g = static_cast<CCVKBuffer *>(rateHost.get())->gpuBuffer();
        require(g && g->mappedData, "mapped shading-rate upload");
        std::memset(g->mappedData, s.mode == "fsr-wrong-rate" ? 0 : 5, rateWidth * rateHeight);
        require(vmaFlushAllocation(d.gpuDevice()->memoryAllocator, g->vmaAllocation, 0, rateWidth * rateHeight) == VK_SUCCESS, "rate upload flush");
    }
    FramebufferInfo fi;
    fi.renderPass = rp.get();
    fi.colorTextures = {texture.get()};
    if (rate) fi.colorTextures.push_back(rate.get());
    auto fb = owned(d.createFramebuffer(fi));
    p.framebuffer = static_cast<CCVKFramebuffer *>(fb.get())->gpuFBO()->vkFramebuffer;
    remember(Kind::Framebuffer, p.framebuffer);
    auto layout = owned(d.createPipelineLayout(PipelineLayoutInfo{}));
    remember(Kind::Layout, static_cast<CCVKPipelineLayout *>(layout.get())->gpuPipelineLayout()->vkPipelineLayout);
    ShaderStage vs;
    vs.stage = ShaderStageFlagBit::VERTEX;
    vs.source = "void main(){vec2 p=vec2(float((gl_VertexIndex<<1)&2),float(gl_VertexIndex&2));gl_Position=vec4(p*2.0-1.0,0.0,1.0);}";
    ShaderStage fs;
    fs.stage = ShaderStageFlagBit::FRAGMENT;
    fs.source = isFsr() ? "#extension GL_EXT_fragment_shading_rate : require\nlayout(location=0) out vec4 o;void main(){o=gl_ShadingRateEXT==5?vec4(1,0,0,1):vec4(0,0,1,1);}" : "layout(location=0) out vec4 o;void main(){o=gl_FragCoord.x<" + std::to_string(width / 2) + ".0?vec4(1,0,0,1):vec4(0,1,0,1);}";
    ShaderInfo si;
    si.name = isFsr() ? "real-FSR-actual-rate" : "real-secondary-content";
    si.stages = {vs, fs};
    auto shader = owned(d.createShader(si));
    PipelineStateInfo pi;
    pi.shader = shader.get();
    pi.pipelineLayout = layout.get();
    pi.renderPass = rp.get();
    pi.primitive = PrimitiveMode::TRIANGLE_LIST;
    pi.rasterizerState.cullMode = CullMode::NONE;
    pi.depthStencilState.depthTest = pi.depthStencilState.depthWrite = false;
    auto pso = owned(d.createPipelineState(pi));
    require(p.pipeline[0] && p.pipelines == (isFsr() ? 1 : unsigned(1 + (d.gpuDevice()->runtimePaths.useDynamicRendering && !d.gpuDevice()->runtimePaths.forceClassicRenderPass && d.gpuContext()->apiVersionInfo.effectiveDeviceApiVersion >= VK_API_VERSION_1_3))), "matching Classic and optional Dynamic handles");
    if (isFsr()) require(!p.pipeline[1] && p.fsrNode, "FSR Classic gate");
    if (isVariantFailure()) {
        auto *gpu = static_cast<CCVKPipelineState *>(pso.get())->gpuPipelineState();
        require(p.injected == 1 && p.pipelines == 2 && gpu &&
                    gpu->vkPipeline == p.pipeline[0] && !gpu->vkDynamicPipeline &&
                    gpu->dynamicPipelineCreationFailed && gpu->gpuRenderPass->dynamicPipelineCreationFailed,
                "failed variant preserves Classic and marks fallback");
        unsigned partialDestroyed = 0;
        for (unsigned i = 0; i < s.objects; ++i)
            if (s.object[i].kind == Kind::Pipeline && s.object[i].handle == bits(p.pipeline[1])) partialDestroyed += s.object[i].destroyed;
        require(partialDestroyed == 1, "failed never-submitted variant retired immediately once");
        CC_LOG_INFO("project-features recovery: index=%u injections=%u partialDestroyed=%u variantNull=%u classicValid=%u", index, p.injected, partialDestroyed, unsigned(!gpu->vkDynamicPipeline), unsigned(gpu->vkPipeline == p.pipeline[0]));
    }
    s.creating = false;
    auto ia = owned(d.createInputAssembler(InputAssemblerInfo{}));
    auto host = owned(d.createBuffer({BufferUsageBit::TRANSFER_DST, MemoryUsageBit::HOST, width * height * 4, 4}));
    auto *g = static_cast<CCVKBuffer *>(host.get())->gpuBuffer();
    require(g && g->mappedData, "mapped feature readback");
    CommandBufferInfo ci;
    ci.queue = d.getQueue();
    auto primary = owned(d.createCommandBuffer(ci));
    Owned<CommandBuffer> secondary;
    s.armed = true;
    const auto before = d.gpuContext()->validationErrorCount.load();
    DrawInfo draw;
    draw.vertexCount = 3;
    draw.instanceCount = 1;
    if (!isFsr()) {
        ci.type = CommandBufferType::SECONDARY;
        secondary = owned(d.createCommandBuffer(ci));
        secondary->begin(rp.get(), 0, fb.get());
        secondary->setViewport({0, 0, width, height, 0, 1});
        secondary->setScissor({0, 0, width, height});
        secondary->bindPipelineState(pso.get());
        secondary->bindInputAssembler(ia.get());
        secondary->draw(draw);
        secondary->end();
    }
    primary->begin();
    if (isFsr()) primary->customCommand([&](void *native) {
        auto cb = static_cast<VkCommandBuffer>(native);
        auto *image = static_cast<CCVKTexture *>(rate.get())->gpuTexture();
        auto *upload = static_cast<CCVKBuffer *>(rateHost.get())->gpuBuffer();
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image->vkImage;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        cmdPipelineBarrier(d.gpuDevice(), cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {rateWidth, rateHeight, 1};
        vkCmdCopyBufferToImage(cb, upload->vkBuffer, image->vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
        cmdPipelineBarrier(d.gpuDevice(), cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR, 0, 0, nullptr, 0, nullptr, 1, &b);
    });
    const Color clear[2] = {{0, 0, 1, 1}, {0, 0, 0, 0}};
    CommandBuffer *child = secondary.get();
    primary->beginRenderPass(rp.get(), fb.get(), {0, 0, width, height}, clear, 1, 0, child ? &child : nullptr, child ? 1 : 0);
    if (isFsr()) {
        primary->bindPipelineState(pso.get());
        primary->bindInputAssembler(ia.get());
        primary->draw(draw);
    } else if (s.mode != "skip-execute")
        primary->execute(&child, 1);
    primary->endRenderPass();
    primary->customCommand([&](void *native) {auto cb=static_cast<VkCommandBuffer>(native);VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;b.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=p.images[0];b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};cmdPipelineBarrier(d.gpuDevice(),cb,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={width,height,1};vkCmdCopyImageToBuffer(cb,p.images[0],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,g->vkBuffer,1,&copy);VkBufferMemoryBarrier h{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};h.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;h.dstAccessMask=VK_ACCESS_HOST_READ_BIT;h.srcQueueFamilyIndex=h.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;h.buffer=g->vkBuffer;h.size=VK_WHOLE_SIZE;cmdPipelineBarrier(d.gpuDevice(),cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&h,0,nullptr); });
    primary->end();
    secondary.reset();
    pso.reset();
    unsigned premature = 0;
    for (unsigned i = 0; i < s.objects; ++i)
        if (s.object[i].kind == Kind::Pipeline && (s.object[i].handle == bits(p.pipeline[0]) || (!isVariantFailure() && s.object[i].handle == bits(p.pipeline[1])))) premature += s.object[i].destroyed;
    require(!premature, "recording lease retains PSO after facade drop before submit");
    CommandBuffer *submit = primary.get();
    d.getQueue()->submit(&submit, 1);
    d.waitAllFences();
    s.armed = false;
    p.chain = p.cb && p.begins == 1 && p.ends == 1 && !p.dynamic && p.draws[0] == 1 && p.copies[0] == 1 && p.submitted == 1 && d.gpuContext()->validationErrorCount.load() == before && (isFsr() ? p.fsrNode : (p.inheritance && p.executes == unsigned(s.mode != "skip-execute")));
    require(p.chain, "complete actual feature CB chain");
    require(vmaInvalidateAllocation(d.gpuDevice()->memoryAllocator, g->vmaAllocation, 0, width * height * 4) == VK_SUCCESS, "feature readback invalidate");
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            const uint8_t pixel[4] = {uint8_t(isFsr() || x < width / 2 ? 255 : 0), uint8_t(!isFsr() && x >= width / 2 ? 255 : 0), 0, 255};
            for (unsigned c = 0; c < 4; ++c) p.mismatch[0] += g->mappedData[(y * width + x) * 4 + c] != pixel[c];
        }
    check(!p.mismatch[0], "feature whole-image golden");
    CC_LOG_INFO("project-features phase: index=%u width=%u height=%u rpContract=%u pipelines=%u inheritance=%u executes=%u fsrNode=%u begins=%u ends=%u dynamic=%u draws=%u copies=%u submits=%u chain=%u checkedBytes=%u mismatch=%u pendingDestroy=%u", index, width, height, unsigned(p.rpContract), p.pipelines, unsigned(p.inheritance), p.executes, unsigned(p.fsrNode), p.begins, p.ends, p.dynamic, p.draws[0], p.copies[0], p.submitted, unsigned(p.chain), width * height * 4, p.mismatch[0], premature);
}
class Capture final {
public:
    Capture() {
        if (!present(L"CC_VULKAN_REAL_PROJECT_FEATURES_CAPTURE")) return;
        const auto *path = std::getenv("CC_VULKAN_REAL_PROJECT_FEATURES_CAPTURE");
        require(path && *path, "capture path");
        const auto module = GetModuleHandleW(L"renderdoc.dll");
        const auto get = module ? reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI")) : nullptr;
        require(get && get(eRENDERDOC_API_Version_1_4_1, reinterpret_cast<void **>(&api_)) == 1 && api_ && !api_->IsFrameCapturing(), "injected capture API");
        api_->SetCaptureFilePathTemplate(path);
        api_->StartFrameCapture(nullptr, nullptr);
        active_ = api_->IsFrameCapturing() == 1;
        require(active_, "capture started");
    }
    ~Capture() {
        if (active_) api_->DiscardFrameCapture(nullptr, nullptr);
    }
    void finish() {
        if (active_) {
            const auto ok = api_->EndFrameCapture(nullptr, nullptr);
            active_ = false;
            require(ok == 1, "capture ended");
            CC_LOG_INFO("RenderDoc features capture: started=1 ended=1");
        }
    }
    Capture(const Capture &) = delete;
    Capture &operator=(const Capture &) = delete;

private:
    RENDERDOC_API_1_4_1 *api_ = nullptr;
    bool active_ = false;
};

void run() noexcept {
    s.started = true;
    try {
        auto *d = CCVKDevice::getInstance();
        require(d && d->gpuDevice() && d->gpuContext(), "live project Vulkan device");
        s.device = d->gpuDevice()->vkDevice;
        s.context = d->gpuContext();
        require(s.context->validationEnabled && s.context->synchronizationValidationEnabled && s.context->bestPracticesValidationEnabled, "Validation triad");
        CC_LOG_INFO("project-features validation: ENABLED");
        if (isFsr()) {
            const auto &caps = d->gpuDevice()->capabilities;
            if (!caps.fragmentShadingRateDetail.attachmentFragmentShadingRate.enabled || !d->getCapabilities().supportVariableRateShading) {
                s.unsupported = true;
                return;
            }
            auto rates = reinterpret_cast<PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR>(vkGetInstanceProcAddr(s.context->vkInstance, "vkGetPhysicalDeviceFragmentShadingRatesKHR"));
            if (!vkGetPhysicalDeviceProperties2 || !rates) {
                s.unsupported = true;
                return;
            }
            VkPhysicalDeviceFragmentShadingRatePropertiesKHR fsr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR};
            VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            props.pNext = &fsr;
            vkGetPhysicalDeviceProperties2(s.context->physicalDevice, &props);
            if (fsr.minFragmentShadingRateAttachmentTexelSize.width > 16 || fsr.minFragmentShadingRateAttachmentTexelSize.height > 16 || fsr.maxFragmentShadingRateAttachmentTexelSize.width < 16 || fsr.maxFragmentShadingRateAttachmentTexelSize.height < 16) {
                s.unsupported = true;
                return;
            }
            uint32_t count = 0;
            require(rates(s.context->physicalDevice, &count, nullptr) == VK_SUCCESS, "FSR count query");
            if (!count || count > 64) {
                s.unsupported = true;
                return;
            }
            std::array<VkPhysicalDeviceFragmentShadingRateKHR, 64> supported{};
            for (auto &v : supported) v.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR;
            require(rates(s.context->physicalDevice, &count, supported.data()) == VK_SUCCESS, "FSR rates query");
            bool rate2 = false;
            for (unsigned i = 0; i < count; ++i) rate2 |= supported[i].fragmentSize.width == 2 && supported[i].fragmentSize.height == 2 && (supported[i].sampleCounts & VK_SAMPLE_COUNT_1_BIT);
            if (!rate2) {
                s.unsupported = true;
                return;
            }
            VkImageFormatProperties imageProps{};
            auto result = vkGetPhysicalDeviceImageFormatProperties(s.context->physicalDevice, VK_FORMAT_R8_UINT, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &imageProps);
            if (result == VK_ERROR_FORMAT_NOT_SUPPORTED) {
                s.unsupported = true;
                return;
            }
            require(result == VK_SUCCESS && (imageProps.sampleCounts & VK_SAMPLE_COUNT_1_BIT), "FSR image format query");
            CC_LOG_INFO("project-features FSR: attachmentEnabled=1 rate2x2=1 texel=16,16 format=R8_UINT");
        }
        const bool autoBarrier = d->autoBarrierEnabled();
        struct AutoRestore {
            CCVKDevice &d;
            bool value;
            ~AutoRestore() { d.enableAutoBarrier(value); }
        } autoRestore{*d, autoBarrier};
        d->enableAutoBarrier(false);
        install();
        s.createPass = d->gpuDevice()->createRenderPass2;
        require(s.createPass, "RP2 PFN");
        d->gpuDevice()->createRenderPass2 = passHook;
        struct PassRestore {
            CCVKGPUDevice *d;
            ~PassRestore() { d->createRenderPass2 = s.createPass; }
        } passRestore{d->gpuDevice()};
        Capture capture;
        phase(*d, 0, 32, 16);
        phase(*d, 1, 24, 12);
        phase(*d, 2, 32, 16);
        capture.finish();
    } catch (const std::exception &e) {
        check(false, e.what());
    } catch (...) {
        check(false, "diagnostic exception");
    }
    s.armed = s.creating = false;
}
} // namespace
bool prepareFeatures() noexcept {
    if (s.prepared) return !s.failures;
    s.prepared = true;
    s.enabled = present(L"CC_VULKAN_REAL_PROJECT_FEATURES");
    if (!s.enabled) {
        CC_LOG_INFO("project-features mode: off enabled=0");
        return true;
    }
    try {
        const auto *value = std::getenv("CC_VULKAN_REAL_PROJECT_FEATURES");
        require(value && *value, "nonempty mode");
        s.mode = value;
        require(s.mode == "secondary" || s.mode == "secondary-variant-failure" || s.mode == "skip-execute" || s.mode == "fsr" || s.mode == "fsr-wrong-rate", "known mode");
        for (const auto *n : {L"CC_VULKAN_REAL_PROJECT_PREWARM", L"CC_VULKAN_REAL_PROJECT_CACHE_PATH", L"CC_VULKAN_REAL_PROJECT_INDIRECT", L"CC_VULKAN_REAL_PROJECT_SUBPASS", L"CC_VULKAN_REAL_PROJECT_RESOURCE_ROOT", L"CC_VULKAN_REAL_PROJECT_RESOLVE"}) require(!present(n), "exclusive diagnostic modes");
        const auto *forcedApi = std::getenv("CC_VULKAN_FORCE_API");
        const auto *rendering = std::getenv("CC_VULKAN_RENDERING");
        require(!(forcedApi && rendering && (std::string(forcedApi) == "1.1" || std::string(forcedApi) == "1.2") && std::string(rendering) == "dynamic"), "forced low API conflicts with Dynamic policy");
        CC_LOG_INFO("project-features mode: %s enabled=1", s.mode.c_str());
        return true;
    } catch (...) {
        return false;
    }
}
void startFeatures() noexcept {
    if (!s.enabled || s.failures) return;
    if (s.started) {
        check(false, "start once");
        return;
    }
    if (auto *a = DeviceAgent::getInstance()) {
        auto *q = a->getMessageQueue();
        if (!q) {
            check(false, "render message queue");
            return;
        }
        ENQUEUE_MESSAGE_0(q, RealProjectFeatures, { run(); });
        q->kickAndWait();
    } else
        run();
}
int finishFeatures(int platformResult) noexcept {
    if (!s.enabled) return platformResult;
    if (s.unsupported) {
        restore();
        CC_LOG_INFO("project-features completed: UNSUPPORTED");
        return 3;
    }
    unsigned expectedPipelines = 0;
    for (const auto &p : s.phases) expectedPipelines += p.pipelines;
    unsigned destroyedCount = 0;
    for (unsigned i = 0; i < s.objects; ++i) {
        check(s.object[i].destroyed == 1, "tracked object retired exactly once");
        destroyedCount += s.object[i].destroyed;
    }
    const unsigned expectedFacades = isFsr() ? 6 : 3;
    check(s.started && s.devices == 1 && !platformResult && s.objects == 15 + expectedPipelines && destroyedCount == s.objects, "actual project retirement");
    check(s.bufferFacades == expectedFacades && s.bufferDrops == expectedFacades && s.imageFacades == expectedFacades && s.imageDrops == expectedFacades, "actual allocator facade release");
    check(!s.liveErrors && !s.postErrors, "live/post Validation through device destroy");
    CC_LOG_INFO("project-features retirement: created=%u destroyed=%u devices=%u bufferFacades=%u bufferDrops=%u imageFacades=%u imageDrops=%u", s.objects, destroyedCount, s.devices, s.bufferFacades, s.bufferDrops, s.imageFacades, s.imageDrops);
    CC_LOG_INFO("project-features completed: failures=%u liveErrors=%u postErrors=%u outcome=%s", s.failures, s.liveErrors, s.postErrors, s.failures ? "FAILED" : "CLEAN");
    restore();
    return platformResult || s.failures ? 1 : 0;
}
} // namespace cc::vulkan_test
