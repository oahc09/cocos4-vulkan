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

// Default-off input-attachment content diagnostic in the real Windows project.
#include "RealProjectSubpass.h"
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
    bool rpContract = false, chain = false;
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
void check(bool ok, const char *label) noexcept {
    if (!ok) {
        ++s.failures;
        CC_LOG_ERROR("project-subpass failure: %s", label);
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
    const auto result = s.createPass(d, i, a, o);
    if (s.creating && result == VK_SUCCESS) {
        auto &p = s.phases[s.phase];
        bool ok = i && o && *o && i->attachmentCount == 2 && i->subpassCount == 2 && i->pSubpasses;
        if (ok) {
            const auto &x = i->pSubpasses[0], &y = i->pSubpasses[1];
            ok = x.colorAttachmentCount == 1 && x.pColorAttachments && x.pColorAttachments[0].attachment == 0 && x.inputAttachmentCount == 0 &&
                 y.colorAttachmentCount == 1 && y.pColorAttachments && y.pColorAttachments[0].attachment == 1 &&
                 y.inputAttachmentCount == 1 && y.pInputAttachments && y.pInputAttachments[0].attachment == 0 &&
                 y.pInputAttachments[0].layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            bool dependency = false;
            if (i->pDependencies)
                for (unsigned j = 0; j < i->dependencyCount; ++j) {
                    const auto &b = i->pDependencies[j];
                    dependency |= b.srcSubpass == 0 && b.dstSubpass == 1 &&
                                  (b.srcStageMask & VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT) && (b.srcAccessMask & VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT) &&
                                  (b.dstStageMask & VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT) && (b.dstAccessMask & VK_ACCESS_INPUT_ATTACHMENT_READ_BIT) &&
                                  (b.dstStageMask & VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT) && (b.dstAccessMask & VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
                }
            ok &= dependency;
        }
        p.rpContract = ok;
        check(ok, "actual driver render-pass input/dependency contract");
        if (o && *o) {
            p.rp = *o;
            remember(Kind::Pass, *o);
        }
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL beginHook(VkCommandBuffer cb, const VkRenderPassBeginInfo *i, VkSubpassContents c) {
    if (!s.begin) {
        check(false, "begin PFN");
        return;
    }
    s.begin(cb, i, c);
    if (s.armed) {
        auto &p = s.phases[s.phase];
        check(i && i->renderPass == p.rp && c == VK_SUBPASS_CONTENTS_INLINE, "actual Classic RP");
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
        check(cb == p.cb && p.subpass == 1, "end identity/subpass");
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
        check(cb == p.cb && point == VK_PIPELINE_BIND_POINT_GRAPHICS, "bind identity");
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
        check(p.subpass < 2 && cb == p.cb && n == 3 && instances == 1 && first == 0 && instance == 0, "actual draw parameters");
        if (p.subpass < 2) {
            check(p.bound == p.pipeline[p.subpass], "actual pipeline handle by subpass");
            if (p.subpass == 1) check(p.descriptor != VK_NULL_HANDLE, "actual input descriptor bound");
            ++p.draws[p.subpass];
        }
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
VKAPI_ATTR VkResult VKAPI_CALL pipelineHook(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *i, const VkAllocationCallbacks *a, VkPipeline *o) {
    if (!s.createPipeline) return VK_ERROR_INITIALIZATION_FAILED;
    const auto r = s.createPipeline(d, c, n, i, a, o);
    if (s.creating && r == VK_SUCCESS && i && o) {
        auto &p = s.phases[s.phase];
        for (unsigned j = 0; j < n; ++j) {
            check(i[j].renderPass == p.rp && i[j].subpass < 2 && o[j] != VK_NULL_HANDLE, "actual Classic subpass pipeline");
            if (i[j].subpass < 2) p.pipeline[i[j].subpass] = o[j];
            ++p.pipelines;
            remember(Kind::Pipeline, o[j]);
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
    HOOK(vkCmdBeginRenderPass, begin, beginHook)
    HOOK(vkCmdEndRenderPass, end, endHook)
    HOOK(vkCmdNextSubpass, next, nextHook) if (vkCmdBeginRendering){HOOK(vkCmdBeginRendering, dynamic, dynamicHook)} HOOK(vkCmdDraw, draw, drawHook) HOOK(vkCmdBindPipeline, bind, bindHook) HOOK(vkCmdBindDescriptorSets, descriptors, descriptorsHook) HOOK(vkCmdCopyImageToBuffer, copy, copyHook)
        HOOK(vkQueueSubmit, submit, submitHook) if (vkQueueSubmit2) { HOOK(vkQueueSubmit2, submit2, submit2Hook) }
    if (vkQueueSubmit2KHR) {
        HOOK(vkQueueSubmit2KHR, submit2KHR, submit2KHRHook)
    }
    HOOK(vkCreateShaderModule, createModule, moduleHook)
    HOOK(vkCreateGraphicsPipelines, createPipeline, pipelineHook)
    HOOK(vkDestroyShaderModule, destroyModule, moduleDestroy)
    HOOK(vkDestroyPipeline, destroyPipeline, pipelineDestroy) HOOK(vkDestroyPipelineLayout, destroyLayout, layoutDestroy)
        HOOK(vkDestroyRenderPass, destroyPass, passDestroy) HOOK(vkDestroyFramebuffer, destroyFramebuffer, framebufferDestroy) HOOK(vkDestroyDescriptorSetLayout, destroyDescriptorLayout, descriptorLayoutDestroy) HOOK(vkDestroyDevice, destroyDevice, deviceDestroy)
#undef HOOK
            s.installed = true;
}
void restore() noexcept {
    if (!s.installed) return;
#define RESTORE(Global, Member) Global = s.Member;
    RESTORE(vkCmdBeginRenderPass, begin)
    RESTORE(vkCmdEndRenderPass, end)
    RESTORE(vkCmdNextSubpass, next) if (s.dynamic){RESTORE(vkCmdBeginRendering, dynamic)} RESTORE(vkCmdDraw, draw) RESTORE(vkCmdBindPipeline, bind) RESTORE(vkCmdBindDescriptorSets, descriptors) RESTORE(vkCmdCopyImageToBuffer, copy)
        RESTORE(vkQueueSubmit, submit) if (s.submit2) { RESTORE(vkQueueSubmit2, submit2) }
    if (s.submit2KHR) {
        RESTORE(vkQueueSubmit2KHR, submit2KHR)
    }
    RESTORE(vkCreateShaderModule, createModule)
    RESTORE(vkCreateGraphicsPipelines, createPipeline)
    RESTORE(vkDestroyShaderModule, destroyModule) RESTORE(vkDestroyPipeline, destroyPipeline) RESTORE(vkDestroyPipelineLayout, destroyLayout)
        RESTORE(vkDestroyRenderPass, destroyPass) RESTORE(vkDestroyFramebuffer, destroyFramebuffer) RESTORE(vkDestroyDescriptorSetLayout, destroyDescriptorLayout) RESTORE(vkDestroyDevice, destroyDevice)
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
    RenderPassInfo rpInfo;
    rpInfo.colorAttachments.resize(2);
    rpInfo.subpasses.resize(2);
    for (unsigned j = 0; j < 2; ++j) {
        auto &c = rpInfo.colorAttachments[j];
        c.format = Format::RGBA8;
        c.loadOp = LoadOp::CLEAR;
        c.storeOp = StoreOp::STORE;
        GeneralBarrierInfo b;
        b.nextAccesses = j ? AccessFlagBit::COLOR_ATTACHMENT_WRITE : AccessFlagBit::FRAGMENT_SHADER_READ_COLOR_INPUT_ATTACHMENT;
        c.barrier = d.getGeneralBarrier(b);
    }
    rpInfo.subpasses[0].colors = {0};
    rpInfo.subpasses[1].inputs = {0};
    rpInfo.subpasses[1].colors = {1};
    auto rp = owned(d.createRenderPass(rpInfo));
    require(p.rpContract && p.rp, "render-pass driver contract");
    auto input = owned(d.createTexture({TextureType::TEX2D, TextureUsageBit::COLOR_ATTACHMENT | TextureUsageBit::INPUT_ATTACHMENT | TextureUsageBit::TRANSFER_SRC, Format::RGBA8, width, height}));
    auto output = owned(d.createTexture({TextureType::TEX2D, TextureUsageBit::COLOR_ATTACHMENT | TextureUsageBit::TRANSFER_SRC, Format::RGBA8, width, height}));
    auto *gi = static_cast<CCVKTexture *>(input.get())->gpuTexture(), *go = static_cast<CCVKTexture *>(output.get())->gpuTexture();
    p.images = {gi->vkImage, go->vkImage};
    p.inputView = static_cast<CCVKTexture *>(input.get())->gpuTextureView()->vkImageView;
    FramebufferInfo fb;
    fb.renderPass = rp.get();
    fb.colorTextures = {input.get(), output.get()};
    auto framebuffer = owned(d.createFramebuffer(fb));
    remember(Kind::Framebuffer, static_cast<CCVKFramebuffer *>(framebuffer.get())->gpuFBO()->vkFramebuffer);
    DescriptorSetLayoutBinding binding;
    binding.binding = 0;
    binding.count = 1;
    binding.descriptorType = DescriptorType::INPUT_ATTACHMENT;
    binding.stageFlags = ShaderStageFlagBit::FRAGMENT;
    DescriptorSetLayoutInfo dlInfo;
    dlInfo.bindings = {binding};
    auto dl = owned(d.createDescriptorSetLayout(dlInfo));
    remember(Kind::DescriptorLayout, static_cast<CCVKDescriptorSetLayout *>(dl.get())->gpuDescriptorSetLayout()->vkDescriptorSetLayout);
    auto layout0 = owned(d.createPipelineLayout(PipelineLayoutInfo{}));
    PipelineLayoutInfo li;
    li.setLayouts = {dl.get()};
    auto layout1 = owned(d.createPipelineLayout(li));
    remember(Kind::Layout, static_cast<CCVKPipelineLayout *>(layout0.get())->gpuPipelineLayout()->vkPipelineLayout);
    remember(Kind::Layout, static_cast<CCVKPipelineLayout *>(layout1.get())->gpuPipelineLayout()->vkPipelineLayout);
    ShaderStage vertex;
    vertex.stage = ShaderStageFlagBit::VERTEX;
    vertex.source = "void main(){int i=gl_VertexIndex;vec2 p=vec2(float((i<<1)&2),float(i&2));gl_Position=vec4(p*2.0-1.0,0.0,1.0);}";
    ShaderStage f0;
    f0.stage = ShaderStageFlagBit::FRAGMENT;
    f0.source = "layout(location=0) out vec4 color;void main(){color=gl_FragCoord.x<" + std::to_string(width / 2) + ".0?vec4(1,0,0,1):vec4(0,1,0,1);}";
    ShaderStage f1;
    f1.stage = ShaderStageFlagBit::FRAGMENT;
    f1.source = "layout(input_attachment_index=0,set=0,binding=0) uniform subpassInput prior;layout(location=0) out vec4 color;void main(){color=subpassLoad(prior).brga;}";
    ShaderInfo si0;
    si0.name = "real-subpass-producer";
    si0.stages = {vertex, f0};
    auto shader0 = owned(d.createShader(si0));
    ShaderInfo si1;
    si1.name = "real-subpass-consumer";
    si1.stages = {vertex, f1};
    UniformInputAttachment u;
    u.name = "prior";
    u.count = 1;
    si1.subpassInputs = {u};
    auto shader1 = owned(d.createShader(si1));
    PipelineStateInfo ps;
    ps.renderPass = rp.get();
    ps.primitive = PrimitiveMode::TRIANGLE_LIST;
    ps.rasterizerState.cullMode = CullMode::NONE;
    ps.depthStencilState.depthTest = ps.depthStencilState.depthWrite = false;
    ps.shader = shader0.get();
    ps.pipelineLayout = layout0.get();
    auto pso0 = owned(d.createPipelineState(ps));
    ps.subpass = 1;
    ps.shader = shader1.get();
    ps.pipelineLayout = layout1.get();
    auto pso1 = owned(d.createPipelineState(ps));
    require(p.pipelines == 2 && p.pipeline[0] && p.pipeline[1] && p.pipeline[0] != p.pipeline[1], "two actual matching subpass pipelines");
    require(static_cast<CCVKPipelineState *>(pso0.get())->gpuPipelineState()->vkDynamicPipeline == VK_NULL_HANDLE && static_cast<CCVKPipelineState *>(pso1.get())->gpuPipelineState()->vkDynamicPipeline == VK_NULL_HANDLE, "multi-subpass Classic variant gate");
    s.creating = false;
    DescriptorSetInfo di;
    di.layout = dl.get();
    auto descriptors = owned(d.createDescriptorSet(di));
    descriptors->bindTexture(0, input.get());
    descriptors->update();
    auto ia = owned(d.createInputAssembler(InputAssemblerInfo{}));
    const uint32_t bytes = width * height * 4;
    auto read0 = owned(d.createBuffer({BufferUsageBit::TRANSFER_DST, MemoryUsageBit::HOST, bytes, 4}));
    auto read1 = owned(d.createBuffer({BufferUsageBit::TRANSFER_DST, MemoryUsageBit::HOST, bytes, 4}));
    auto *gb0 = static_cast<CCVKBuffer *>(read0.get())->gpuBuffer(), *gb1 = static_cast<CCVKBuffer *>(read1.get())->gpuBuffer();
    require(gb0->mappedData && gb1->mappedData, "mapped readbacks");
    CommandBufferInfo ci;
    ci.queue = d.getQueue();
    auto cmd = owned(d.createCommandBuffer(ci));
    const auto errorsBefore = d.gpuContext()->validationErrorCount.load();
    s.armed = true;
    cmd->begin();
    const Color clears[2] = {{0, 0, 1, 1}, {0, 0, 0, 1}};
    cmd->beginRenderPass(rp.get(), framebuffer.get(), {0, 0, width, height}, clears, 1, 0);
    DrawInfo draw;
    draw.vertexCount = 3;
    draw.instanceCount = 1;
    cmd->bindPipelineState(pso0.get());
    cmd->bindInputAssembler(ia.get());
    if (s.mode != "skip-producer") cmd->draw(draw);
    cmd->nextSubpass();
    cmd->bindPipelineState(pso1.get());
    cmd->bindDescriptorSet(0, descriptors.get());
    cmd->draw(draw);
    cmd->endRenderPass();
    cmd->customCommand([device = d.gpuDevice(), a = p.images[0], b = p.images[1], gb0, gb1, width, height](void *value) {
        const auto cb = reinterpret_cast<VkCommandBuffer>(value);
        VkImageMemoryBarrier barriers[2]{};
        for (auto &x : barriers) {
            x.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            x.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            x.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            x.srcQueueFamilyIndex = x.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            x.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        barriers[0].image = a;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[0].srcAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barriers[1].image = b;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barriers[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        cmdPipelineBarrier(device, cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(cb, a, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gb0->vkBuffer, 1, &copy);
        vkCmdCopyImageToBuffer(cb, b, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gb1->vkBuffer, 1, &copy);
        VkBufferMemoryBarrier host[2]{};
        for (auto &x : host) {
            x.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            x.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            x.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            x.srcQueueFamilyIndex = x.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            x.size = VK_WHOLE_SIZE;
        }
        host[0].buffer = gb0->vkBuffer;
        host[1].buffer = gb1->vkBuffer;
        cmdPipelineBarrier(device, cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 2, host, 0, nullptr);
    });
    cmd->end();
    auto *submitted = cmd.get();
    d.getQueue()->submit(&submitted, 1);
    d.waitAllFences();
    s.armed = false;
    p.chain = p.cb && p.begins == 1 && p.ends == 1 && p.next == 1 && !p.dynamic && p.draws[0] == unsigned(s.mode != "skip-producer") && p.draws[1] == 1 && p.copies[0] == 1 && p.copies[1] == 1 && p.submitted == 1 && d.gpuContext()->validationErrorCount.load() == errorsBefore;
    require(p.chain, "actual CB/subpass/draw/copy/submit chain");
    require(vmaInvalidateAllocation(d.gpuDevice()->memoryAllocator, gb0->vmaAllocation, 0, bytes) == VK_SUCCESS && vmaInvalidateAllocation(d.gpuDevice()->memoryAllocator, gb1->vmaAllocation, 0, bytes) == VK_SUCCESS, "invalidate readbacks");
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t source[4] = {uint8_t(x < width / 2 ? 255 : 0), uint8_t(x < width / 2 ? 0 : 255), 0, 255};
            const uint8_t result[4] = {source[2], source[0], source[1], source[3]};
            for (unsigned c = 0; c < 4; ++c) {
                const auto offset = (y * width + x) * 4 + c;
                p.mismatch[0] += gb0->mappedData[offset] != source[c];
                p.mismatch[1] += gb1->mappedData[offset] != result[c];
            }
        }
    check(!p.mismatch[0] && !p.mismatch[1], "producer/consumer full golden");
    CC_LOG_INFO("project-subpass phase: index=%u width=%u height=%u rpContract=%u pipelines=%u begins=%u next=%u ends=%u dynamic=%u producer=%u consumer=%u copies=%u,%u submits=%u chain=%u checkedBytes=%u sourceMismatch=%u outputMismatch=%u", index, width, height, unsigned(p.rpContract), p.pipelines, p.begins, p.next, p.ends, p.dynamic, p.draws[0], p.draws[1], p.copies[0], p.copies[1], p.submitted, unsigned(p.chain), bytes, p.mismatch[0], p.mismatch[1]);
}
class Capture final {
public:
    Capture() {
        if (!present(L"CC_VULKAN_REAL_PROJECT_SUBPASS_CAPTURE")) return;
        const auto *path = std::getenv("CC_VULKAN_REAL_PROJECT_SUBPASS_CAPTURE");
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
            CC_LOG_INFO("RenderDoc subpass capture: started=1 ended=1");
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
        require(d && d->gpuContext() && d->gpuDevice(), "live Vulkan device");
        s.device = d->gpuDevice()->vkDevice;
        s.context = d->gpuContext();
        require(s.context->validationEnabled && s.context->synchronizationValidationEnabled && s.context->bestPracticesValidationEnabled, "Validation triad");
        CC_LOG_INFO("project-subpass validation: ENABLED");
        require(vkGetPhysicalDeviceImageFormatProperties && d->gpuDevice()->createRenderPass2, "format/RenderPass2 PFNs");
        for (auto usage : {VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT}) {
            VkImageFormatProperties props{};
            const auto r = vkGetPhysicalDeviceImageFormatProperties(s.context->physicalDevice, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &props);
            if (r == VK_ERROR_FORMAT_NOT_SUPPORTED || (r == VK_SUCCESS && !(props.sampleCounts & VK_SAMPLE_COUNT_1_BIT))) {
                s.unsupported = true;
                return;
            }
            require(r == VK_SUCCESS && props.maxExtent.width >= 32 && props.maxExtent.height >= 16, "RGBA8 image extent/usage");
        }
        install();
        s.createPass = d->gpuDevice()->createRenderPass2;
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
    s.creating = s.armed = false;
}
} // namespace
bool prepareSubpass() noexcept {
    if (s.prepared) return !s.failures;
    s.prepared = true;
    s.enabled = present(L"CC_VULKAN_REAL_PROJECT_SUBPASS");
    if (!s.enabled) {
        CC_LOG_INFO("project-subpass mode: off enabled=0");
        return true;
    }
    try {
        const auto *mode = std::getenv("CC_VULKAN_REAL_PROJECT_SUBPASS");
        require(mode && *mode, "nonempty mode");
        s.mode = mode;
        require(s.mode == "content" || s.mode == "skip-producer", "known mode");
        require(!present(L"CC_VULKAN_REAL_PROJECT_PREWARM") && !present(L"CC_VULKAN_REAL_PROJECT_CACHE_PATH") && !present(L"CC_VULKAN_REAL_PROJECT_INDIRECT") && !present(L"CC_VULKAN_REAL_PROJECT_RESOURCE_ROOT"), "diagnostic modes exclusive");
        CC_LOG_INFO("project-subpass mode: %s enabled=1", s.mode.c_str());
        return true;
    } catch (...) {
        return false;
    }
}
void startSubpass() noexcept {
    if (!s.enabled || s.failures) return;
    if (s.started) {
        check(false, "start once");
        return;
    }
    if (auto *agent = DeviceAgent::getInstance()) {
        auto *messages = agent->getMessageQueue();
        if (!messages) {
            check(false, "render message queue");
            return;
        }
        ENQUEUE_MESSAGE_0(messages, RealProjectSubpass, { run(); });
        messages->kickAndWait();
    } else
        run();
}
int finishSubpass(int platformResult) noexcept {
    if (!s.enabled) return platformResult;
    if (s.unsupported) {
        restore();
        CC_LOG_INFO("project-subpass completed: UNSUPPORTED");
        return 3;
    }
    check(platformResult == 0 && s.started && s.devices == 1, "normal engine shutdown");
    unsigned dead = 0;
    for (unsigned i = 0; i < s.objects; ++i) {
        dead += s.object[i].destroyed;
        check(s.object[i].destroyed == 1, "diagnostic object retired");
    }
    check(s.objects == 33 && dead == 33 && !s.liveErrors && !s.postErrors, "retirement and Validation through device shutdown");
    check(s.bufferFacades == 6 && s.bufferDrops == 6 && s.imageFacades == 6 && s.imageDrops == 6, "diagnostic allocator facade release");
    CC_LOG_INFO("project-subpass retirement: created=%u destroyed=%u devices=%u bufferFacades=%u bufferDrops=%u imageFacades=%u imageDrops=%u", s.objects, dead, s.devices, s.bufferFacades, s.bufferDrops, s.imageFacades, s.imageDrops);
    CC_LOG_INFO("project-subpass completed: failures=%u liveErrors=%u postErrors=%u outcome=%s", s.failures, s.liveErrors, s.postErrors, s.failures ? "FAILED" : "CLEAN");
    restore();
    return platformResult || s.failures ? 1 : 0;
}
} // namespace cc::vulkan_test
