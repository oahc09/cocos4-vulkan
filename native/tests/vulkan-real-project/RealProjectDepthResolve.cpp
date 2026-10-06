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

// M6-A5: real D32 depth-only SAMPLE_ZERO MSAA resolve harness.
// Reuses the DS/MSAA harness patterns: per-phase hooks, policy guard, dual
// PSO handles, owned readback snapshots, retirement. Two PSOs (near red
// z=0.25 LESS, far green z=0.75 rejected), blue color clear + depth clear 1.
// Copies BOTH 1x resolve images (color + depth) to host per phase.

#include "RealProjectResolve.h"
#include "RealProjectScopeObservation.h"
#include "base/Log.h"
#include "gfx-base/SPIRVUtils.h"
#include "gfx-vulkan/VKBuffer.h"
#include "gfx-vulkan/VKDevice.h"
#include "gfx-vulkan/VKFramebuffer.h"
#include "gfx-vulkan/VKGPUObjects.h"
#include "gfx-vulkan/VKPipelineState.h"
#include "gfx-vulkan/VKRenderPass.h"
#include "gfx-vulkan/VKTexture.h"
#include "gfx-vulkan/VKUtils.h"
#include "platform/FileUtils.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

#if defined(_WIN32)
    #include <windows.h>
#endif

namespace cc::vulkan_test::depth_resolve {
using namespace cc;
using namespace cc::gfx;
constexpr uint32_t W = 17, H = 11, PIXELS = W * H, PLANE_BYTES = PIXELS * 4;
unsigned failures = 0;
void check(bool condition, const char *what) {
    if (!condition) {
        ++failures;
        CC_LOG_ERROR("FAIL: %s\n", what);
    }
}
template <class T>
void drop(T *&object) {
    if (object) {
        object->destroy();
        delete object;
        object = nullptr;
    }
}
struct Objects {
    CCVKDevice *device = nullptr;
    CommandBuffer *cmd = nullptr;
    InputAssembler *ia = nullptr;
    PipelineLayout *layout = nullptr;
    Shader *shaders[2]{};
    PipelineState *psos[2]{};
    Framebuffer *fbos[2]{};
    RenderPass *rp = nullptr;
    Texture *msColors[2]{}, *colorResolves[2]{}, *msDepths[2]{}, *depthResolves[2]{};
    Buffer *hosts[2]{};
    void clear() {
        drop(cmd);
        drop(ia);
        for (auto &p : psos) drop(p);
        for (auto &f : fbos) drop(f);
        drop(rp);
        drop(layout);
        for (auto &s : shaders) drop(s);
        for (auto &t : msColors) drop(t);
        for (auto &t : colorResolves) drop(t);
        for (auto &t : msDepths) drop(t);
        for (auto &t : depthResolves) drop(t);
        for (auto &b : hosts) drop(b);
    }
    ~Objects() {
        if (device) device->waitAllFences();
        clear();
    }
};
struct CreatedPipeline {
    VkPipeline handle = VK_NULL_HANDLE;
    bool dynamic = false;
    unsigned samples = 0;
    VkRenderPass renderPass = VK_NULL_HANDLE; // non-NULL for Classic
    unsigned destroyed = 0;
};
CreatedPipeline created[4];
unsigned createdCount = 0;
VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_2_BIT;
struct Phase {
    bool dynamic = false;
    bool skipNear = false;
    VkRenderPass expectedRP = VK_NULL_HANDLE;
    VkFramebuffer expectedFBO = VK_NULL_HANDLE;
    VkImageView msColorView = VK_NULL_HANDLE;
    // 0=uninit, 1=cb-begun, 2=pass-active, 3=pass-ended, 4=submitted
    int state = 0;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkImageView colorResolveView = VK_NULL_HANDLE, depthResolveView = VK_NULL_HANDLE, msDepthView = VK_NULL_HANDLE;
    VkImage colorResolve = VK_NULL_HANDLE, depthResolve = VK_NULL_HANDLE;
    VkBuffer host = VK_NULL_HANDLE;
    VkPipeline bound = VK_NULL_HANDLE, expectedPipelines[2]{};
    unsigned expectedDraws = 0;
    unsigned begins = 0, rpBegins = 0, rpEnds = 0, dynBegins = 0, dynEnds = 0;
    unsigned draws = 0, colorCopies = 0, depthCopies = 0, submitCalls = 0, submitSuccesses = 0;
    unsigned actualDepthResolveMode = 0, actualStencilResolveMode = 0;
};
Phase *phase = nullptr;
PFN_vkBeginCommandBuffer realBegin;
PFN_vkCmdBeginRenderPass realRPBegin;
PFN_vkCmdEndRenderPass realRPEnd;
PFN_vkCmdBeginRendering realDynBegin;
PFN_vkCmdEndRendering realDynEnd;
PFN_vkCmdBindPipeline realBind;
PFN_vkCmdDraw realDraw;
PFN_vkCmdCopyImageToBuffer realCopy;
PFN_vkQueueSubmit realSubmit;
PFN_vkCreateGraphicsPipelines realCreate;
PFN_vkDestroyPipeline realDestroy;

VKAPI_ATTR VkResult VKAPI_CALL onBegin(VkCommandBuffer cb, const VkCommandBufferBeginInfo *info) {
    if (realBegin == nullptr) {
        check(false, "Begin PFN null");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (info == nullptr || cb == VK_NULL_HANDLE || info->sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO) {
        check(false, "Begin payload null");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkResult result = realBegin(cb, info);
    if (phase && result == VK_SUCCESS) {
        check(phase->state == 0, "phase command buffer begins exactly once");
        phase->cb = cb;
        phase->state = 1;
        ++phase->begins;
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL onRPBegin(VkCommandBuffer cb, const VkRenderPassBeginInfo *info, VkSubpassContents contents) {
    if (realRPBegin == nullptr) {
        check(false, "RPBegin PFN null");
        return;
    }
    if (info == nullptr) {
        check(false, "RPBegin payload null");
        return;
    }
    realRPBegin(cb, info, contents);
    if (phase) {
        check(cb == phase->cb, "Classic begin CB");
        check(info->sType == VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO && info->pNext == nullptr,
              "Classic begin sType/pNext");
        check(info->renderPass == phase->expectedRP, "Classic begin renderPass identity");
        check(info->framebuffer == phase->expectedFBO, "Classic begin framebuffer identity");
        check(contents == VK_SUBPASS_CONTENTS_INLINE && info->clearValueCount == 3 && info->pClearValues &&
                  info->pClearValues[2].depthStencil.depth == 1.0F,
              "Classic inline pass and actual depth clear at attachment 2");
        check(info->renderArea.offset.x == 0 && info->renderArea.offset.y == 0 &&
                  info->renderArea.extent.width == W && info->renderArea.extent.height == H,
              "Classic begin area 0,0 17x11");
        check(phase->state == 1, "RP begin after CB begin, once");
        ++phase->rpBegins;
        phase->state = 2;
    }
}
VKAPI_ATTR void VKAPI_CALL onRPEnd(VkCommandBuffer cb) {
    if (realRPEnd == nullptr) {
        check(false, "RPEnd PFN null");
        return;
    }
    realRPEnd(cb);
    if (phase) {
        check(cb == phase->cb && phase->state == 2, "Classic end CB/pass-active");
        ++phase->rpEnds;
        phase->state = 3;
    }
}
VKAPI_ATTR void VKAPI_CALL onDynBegin(VkCommandBuffer cb, const VkRenderingInfo *info) {
    if (realDynBegin == nullptr) {
        check(false, "DynBegin PFN null");
        return;
    }
    if (info == nullptr) {
        check(false, "DynBegin payload null");
        return;
    }
    realDynBegin(cb, info);
    if (!phase) return;
    ++phase->dynBegins;
    check(cb == phase->cb, "Dynamic begin CB");
    check(phase->state == 1, "Dynamic begin after CB begin, once");
    check(info->sType == VK_STRUCTURE_TYPE_RENDERING_INFO && info->pNext == nullptr && info->flags == 0 &&
              info->colorAttachmentCount == 1 && info->layerCount == 1 && info->viewMask == 0,
          "Dynamic root sType/pNext/flags/counts");
    check(info->renderArea.offset.x == 0 && info->renderArea.offset.y == 0 &&
              info->renderArea.extent.width == W && info->renderArea.extent.height == H,
          "Dynamic renderArea 0,0 17x11");
    if (info->colorAttachmentCount != 1) return;
    const auto *color = info->pColorAttachments;
    check(color && color->imageView == phase->msColorView && color->resolveMode == VK_RESOLVE_MODE_AVERAGE_BIT &&
              color->resolveImageView == phase->colorResolveView &&
              color->imageLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
              color->resolveImageLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
              color->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && color->storeOp == VK_ATTACHMENT_STORE_OP_STORE &&
              color->clearValue.color.float32[2] == 1.0F && color->clearValue.color.float32[3] == 1.0F &&
              color->clearValue.color.float32[0] == 0.0F && color->clearValue.color.float32[1] == 0.0F,
          "Dynamic color MSAA resolve node + blue clear");
    const auto *depth = info->pDepthAttachment;
    check(depth && depth->imageView == phase->msDepthView && depth->resolveMode == phase->actualDepthResolveMode &&
              depth->resolveImageView == phase->depthResolveView &&
              depth->imageLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
              depth->resolveImageLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
              depth->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && depth->storeOp == VK_ATTACHMENT_STORE_OP_STORE &&
              depth->clearValue.depthStencil.depth == 1.0F,
          "Dynamic depth SAMPLE_ZERO resolve node");
    check(info->pStencilAttachment == nullptr, "Dynamic has no stencil node (depth-only)");
    phase->state = 2;
}
VKAPI_ATTR void VKAPI_CALL onDynEnd(VkCommandBuffer cb) {
    if (realDynEnd == nullptr) {
        check(false, "DynEnd PFN null");
        return;
    }
    realDynEnd(cb);
    if (phase) {
        check(cb == phase->cb && phase->state == 2, "Dynamic end CB/pass-active");
        ++phase->dynEnds;
        phase->state = 3;
    }
}
VKAPI_ATTR void VKAPI_CALL onBind(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipeline pipeline) {
    if (realBind == nullptr) {
        check(false, "Bind PFN null");
        return;
    }
    realBind(cb, point, pipeline);
    if (phase && point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        check(cb == phase->cb && phase->state == 2, "bind CB/active-pass");
        phase->bound = pipeline;
    }
}
VKAPI_ATTR void VKAPI_CALL onDraw(VkCommandBuffer cb, uint32_t vertices, uint32_t instances, uint32_t firstVertex, uint32_t firstInstance) {
    if (realDraw == nullptr) {
        check(false, "Draw PFN null");
        return;
    }
    realDraw(cb, vertices, instances, firstVertex, firstInstance);
    if (!phase) return;
    check(phase->state == 2, "draw in active pass");
    const bool correctPipeline = phase->draws < phase->expectedDraws &&
                                 phase->expectedPipelines[phase->draws] != VK_NULL_HANDLE &&
                                 phase->expectedPipelines[phase->draws] == phase->bound;
    check(cb == phase->cb && correctPipeline && vertices == 3 && instances == 1 && firstVertex == 0 && firstInstance == 0,
          "draw CB/exact ordered pipeline/arguments");
    ++phase->draws;
}
VKAPI_ATTR void VKAPI_CALL onCopy(VkCommandBuffer cb, VkImage image, VkImageLayout layout, VkBuffer buffer, uint32_t count, const VkBufferImageCopy *regions) {
    if (realCopy == nullptr) {
        check(false, "Copy PFN null");
        return;
    }
    if (regions == nullptr && count > 0) {
        check(false, "Copy regions null");
        return;
    }
    realCopy(cb, image, layout, buffer, count, regions);
    if (!phase) return;
    check(phase->state == 3, "copy after pass end");
    check(cb == phase->cb && layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && buffer == phase->host && count == 1 && regions,
          "copy CB/layout/host/count");
    if (!regions || count != 1) return;
    const auto &r = regions[0];
    check(r.imageExtent.width == W && r.imageExtent.height == H && r.imageExtent.depth == 1 &&
              r.imageSubresource.layerCount == 1 && r.bufferRowLength == 0 && r.bufferImageHeight == 0 &&
              r.imageOffset.x == 0 && r.imageOffset.y == 0 && r.imageOffset.z == 0 &&
              r.imageSubresource.mipLevel == 0 && r.imageSubresource.baseArrayLayer == 0,
          "copy full plane from origin mip0 layer0");
    if (r.imageSubresource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT) {
        check(image == phase->colorResolve && r.bufferOffset == 0, "color resolve copy identity/offset");
        ++phase->colorCopies;
    } else if (r.imageSubresource.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT) {
        check(image == phase->depthResolve && r.bufferOffset == PLANE_BYTES, "depth resolve copy identity/offset");
        ++phase->depthCopies;
    } else
        check(false, "unexpected copy aspect");
}
VKAPI_ATTR VkResult VKAPI_CALL onSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *infos, VkFence fence) {
    if (realSubmit == nullptr) {
        check(false, "Submit PFN null");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (infos == nullptr && count > 0) {
        check(false, "Submit infos null");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (infos[i].sType != VK_STRUCTURE_TYPE_SUBMIT_INFO ||
            (infos[i].commandBufferCount && !infos[i].pCommandBuffers)) {
            check(false, "Submit payload structure/command array");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    if (phase) check(phase->state == 3 && phase->colorCopies == 1 && phase->depthCopies == 1,
                     "submit follows this pass and both copies");
    const VkResult result = realSubmit(queue, count, infos, fence);
    if (!phase) return result;
    ++phase->submitCalls;
    if (result == VK_SUCCESS && infos && count > 0) {
        unsigned forwarded = 0;
        for (uint32_t i = 0; i < count; ++i)
            if (infos[i].pCommandBuffers)
                for (uint32_t j = 0; j < infos[i].commandBufferCount; ++j)
                    if (infos[i].pCommandBuffers[j] == phase->cb) ++forwarded;
        check(phase->cb && forwarded == 1, "successful submit forwards this phase CB exactly once");
        if (forwarded == 1) {
            ++phase->submitSuccesses;
            phase->state = 4;
        }
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL onCreate(VkDevice device, VkPipelineCache cache, uint32_t count, const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *alloc, VkPipeline *pipelines) {
    if (realCreate == nullptr || infos == nullptr || pipelines == nullptr || count != 1 ||
        infos[0].sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO) {
        check(false, "create hook NULL payload/count guard");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkResult result = realCreate(device, cache, count, infos, alloc, pipelines);
    check(infos[0].sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO && result == VK_SUCCESS &&
              pipelines[0] != VK_NULL_HANDLE && createdCount < 4,
          "real graphics pipeline creation");
    if (count != 1 || !infos || !pipelines || result != VK_SUCCESS || !pipelines[0] || createdCount >= 4) return result;
    auto &record = created[createdCount++];
    record.handle = pipelines[0];
    record.dynamic = infos[0].renderPass == VK_NULL_HANDLE;
    record.renderPass = infos[0].renderPass;
    record.samples = infos[0].pMultisampleState ? infos[0].pMultisampleState->rasterizationSamples : 0;
    const auto *ds = infos[0].pDepthStencilState;
    check(ds && ds->depthTestEnable && ds->depthWriteEnable && ds->depthCompareOp == VK_COMPARE_OP_LESS && !ds->stencilTestEnable,
          "depth-only LESS pipeline state");
    if (record.dynamic) {
        const auto *node = static_cast<const VkPipelineRenderingCreateInfo *>(infos[0].pNext);
        check(node && node->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO && !node->pNext &&
                  node->colorAttachmentCount == 1 && node->pColorAttachmentFormats &&
                  node->pColorAttachmentFormats[0] == VK_FORMAT_R8G8B8A8_UNORM &&
                  node->depthAttachmentFormat == VK_FORMAT_D32_SFLOAT && node->stencilAttachmentFormat == VK_FORMAT_UNDEFINED &&
                  node->viewMask == 0 && infos[0].subpass == 0 && record.samples == msaaSamples,
              "Dynamic depth-only MSAA signature");
    } else
        check(!infos[0].pNext && infos[0].subpass == 0 && record.samples == msaaSamples, "Classic MSAA root");
    return result;
}
VKAPI_ATTR void VKAPI_CALL onDestroy(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks *alloc) {
    if (!realDestroy) {
        check(false, "Destroy pipeline PFN null");
        return;
    }
    realDestroy(device, pipeline, alloc);
    for (unsigned i = 0; i < createdCount; ++i)
        if (created[i].handle == pipeline) ++created[i].destroyed;
}
struct Hooks {
    Hooks() {
        realBegin = vkBeginCommandBuffer;
        realRPBegin = vkCmdBeginRenderPass;
        realRPEnd = vkCmdEndRenderPass;
        realDynBegin = vkCmdBeginRendering;
        realDynEnd = vkCmdEndRendering;
        realBind = vkCmdBindPipeline;
        realDraw = vkCmdDraw;
        realCopy = vkCmdCopyImageToBuffer;
        realSubmit = vkQueueSubmit;
        realCreate = vkCreateGraphicsPipelines;
        realDestroy = vkDestroyPipeline;
        vkBeginCommandBuffer = onBegin;
        vkCmdBeginRenderPass = onRPBegin;
        vkCmdEndRenderPass = onRPEnd;
        vkCmdBeginRendering = onDynBegin;
        vkCmdEndRendering = onDynEnd;
        vkCmdBindPipeline = onBind;
        vkCmdDraw = onDraw;
        vkCmdCopyImageToBuffer = onCopy;
        vkQueueSubmit = onSubmit;
        vkCreateGraphicsPipelines = onCreate;
        vkDestroyPipeline = onDestroy;
    }
    ~Hooks() {
        phase = nullptr;
        vkBeginCommandBuffer = realBegin;
        vkCmdBeginRenderPass = realRPBegin;
        vkCmdEndRenderPass = realRPEnd;
        vkCmdBeginRendering = realDynBegin;
        vkCmdEndRendering = realDynEnd;
        vkCmdBindPipeline = realBind;
        vkCmdDraw = realDraw;
        vkCmdCopyImageToBuffer = realCopy;
        vkQueueSubmit = realSubmit;
        vkCreateGraphicsPipelines = realCreate;
        // Keep only target pipeline retirement observation until engine device shutdown.
    }
};

int run(VkSampleCountFlagBits samples, bool skipNear) {
    msaaSamples = samples;
    auto &raw = *CCVKDevice::getInstance();
    auto *context = raw.gpuContext();
    auto *gpu = raw.gpuDevice();
    if (!context || !gpu) {
        check(false, "Vulkan device/context GPU objects");
        return 1;
    }
    const bool validation = context->validationEnabled && context->synchronizationValidationEnabled && context->bestPracticesValidationEnabled;
    CC_LOG_INFO("validation + best practices + sync: %s\n", validation ? "ENABLED" : "NOT VERIFIED");
    if (!validation) return 1;
    if (!gpu->capabilities.dynamicRendering.enabled || !gpu->runtimePaths.useDynamicRendering ||
        context->apiVersionInfo.effectiveDeviceApiVersion < VK_API_VERSION_1_3) {
        CC_LOG_ERROR("UNSUPPORTED: requires enabled engine Dynamic policy at effective 1.3\n");
        return 3;
    }
    const auto &dsrProps = context->physicalDeviceDepthStencilResolveProperties;
    if ((dsrProps.supportedDepthResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT) == 0) {
        CC_LOG_ERROR("UNSUPPORTED: depth SAMPLE_ZERO resolve not supported (depthModes=0x%x)\n",
                     unsigned(dsrProps.supportedDepthResolveModes));
        return 3;
    }
    if (!vkBeginCommandBuffer || !vkCmdBeginRenderPass || !vkCmdEndRenderPass || !vkCmdBeginRendering || !vkCmdEndRendering ||
        !vkCmdBindPipeline || !vkCmdDraw || !vkCmdCopyImageToBuffer || !vkQueueSubmit || !vkCreateGraphicsPipelines ||
        !vkDestroyPipeline || !vkGetPhysicalDeviceImageFormatProperties) {
        check(false, "required PFNs");
        return 1;
    }
    check(context->validationErrorCount.load() == 0, "clean initialization");
    check(!gpu->runtimePaths.useSubmit2, "legacy submit required for this hook");
    const bool previousAutoBarrier = raw.autoBarrierEnabled();
    struct AutoBarrierRestore {
        CCVKDevice &d;
        bool value;
        ~AutoBarrierRestore() { d.enableAutoBarrier(value); }
    } autoRestore{raw, previousAutoBarrier};
    raw.enableAutoBarrier(false);
    // real format/usage/sample queries: RGBA8 MS color, RGBA8 1x resolve,
    // D32 MS depth, D32 1x depth resolve (DS|TRANSFER_SRC)
    const struct {
        VkFormat fmt;
        VkImageUsageFlags usage;
        VkSampleCountFlags want;
        const char *tag;
    } queries[] = {
        {VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VkSampleCountFlags(samples), "RGBA8 MS color"},
        {VK_FORMAT_R8G8B8A8_UNORM, VkImageUsageFlags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT), VK_SAMPLE_COUNT_1_BIT, "RGBA8 1x resolve"},
        {VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VkSampleCountFlags(samples), "D32 MS depth"},
        {VK_FORMAT_D32_SFLOAT, VkImageUsageFlags(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT), VK_SAMPLE_COUNT_1_BIT, "D32 1x resolve"},
    };
    for (const auto &q : queries) {
        VkImageFormatProperties props{};
        const VkResult result = vkGetPhysicalDeviceImageFormatProperties(context->physicalDevice, q.fmt, VK_IMAGE_TYPE_2D,
                                                                         VK_IMAGE_TILING_OPTIMAL, q.usage, 0, &props);
        if (result == VK_ERROR_FORMAT_NOT_SUPPORTED) {
            CC_LOG_ERROR("UNSUPPORTED: %s format/usage\n", q.tag);
            return 3;
        }
        if (result != VK_SUCCESS) {
            check(false, "unexpected image-format query error");
            return 1;
        }
        if ((props.sampleCounts & q.want) == 0) {
            CC_LOG_ERROR("UNSUPPORTED: %s sample bit %u (has 0x%x)\n", q.tag,
                         unsigned(q.want), unsigned(props.sampleCounts));
            return 3;
        }
        if (props.maxExtent.width < W || props.maxExtent.height < H) {
            check(false, "image extent too small");
            return 1;
        }
    }
    if (failures) return 1;
    Hooks hooks;
    Objects o;
    o.device = &raw;
    const SampleCount gfxSamples = samples == VK_SAMPLE_COUNT_2_BIT ? SampleCount::X2 : SampleCount::X4;
    const uint32_t hostBytes = 2 * PLANE_BYTES;
    CC_LOG_INFO("dsr-config: samples=%u depthFormat=126 resolveMode=SAMPLE_ZERO pixels=%u hostBytes=%u runtimeDynamic=1\n",
                unsigned(samples), PIXELS, hostBytes);
    for (unsigned i = 0; i < 2; ++i) {
        TextureInfo t;
        t.type = TextureType::TEX2D;
        t.width = W;
        t.height = H;
        t.format = Format::RGBA8;
        t.usage = TextureUsageBit::COLOR_ATTACHMENT;
        t.samples = gfxSamples;
        o.msColors[i] = raw.createTexture(t);
        t.usage = TextureUsageBit::COLOR_ATTACHMENT | TextureUsageBit::TRANSFER_SRC;
        t.samples = SampleCount::X1;
        o.colorResolves[i] = raw.createTexture(t);
        t.format = Format::DEPTH;
        t.usage = TextureUsageBit::DEPTH_STENCIL_ATTACHMENT;
        t.samples = gfxSamples;
        o.msDepths[i] = raw.createTexture(t);
        t.usage = TextureUsageBit::DEPTH_STENCIL_ATTACHMENT | TextureUsageBit::TRANSFER_SRC;
        t.samples = SampleCount::X1;
        o.depthResolves[i] = raw.createTexture(t);
        BufferInfo host;
        host.usage = BufferUsageBit::TRANSFER_DST;
        host.memUsage = MemoryUsageBit::HOST;
        host.size = hostBytes;
        host.stride = 1;
        o.hosts[i] = raw.createBuffer(host);
        check(o.msColors[i] && o.colorResolves[i] && o.msDepths[i] && o.depthResolves[i] && o.hosts[i], "texture/host facades");
        if (failures) return 1;
        for (auto *t : {o.msColors[i], o.colorResolves[i], o.msDepths[i], o.depthResolves[i]}) {
            check(static_cast<CCVKTexture *>(t)->gpuTexture() && static_cast<CCVKTexture *>(t)->gpuTextureView(),
                  "gpu texture/view handles");
        }
        check(static_cast<CCVKBuffer *>(o.hosts[i])->gpuBuffer() &&
                  static_cast<CCVKBuffer *>(o.hosts[i])->gpuBuffer()->vkBuffer != VK_NULL_HANDLE &&
                  static_cast<CCVKBuffer *>(o.hosts[i])->gpuBuffer()->mappedData &&
                  static_cast<CCVKBuffer *>(o.hosts[i])->gpuBuffer()->vmaAllocation,
              "host GPU buffer handles/mapping");
        if (failures) return 1;
    }
    GeneralBarrierInfo colorBarrier;
    colorBarrier.nextAccesses = AccessFlagBit::COLOR_ATTACHMENT_WRITE;
    GeneralBarrierInfo dsBarrier;
    dsBarrier.nextAccesses = AccessFlagBit::DEPTH_STENCIL_ATTACHMENT_WRITE;
    RenderPassInfo rp;
    rp.colorAttachments.resize(2);
    rp.colorAttachments[0].format = Format::RGBA8;
    rp.colorAttachments[0].sampleCount = gfxSamples;
    rp.colorAttachments[0].loadOp = LoadOp::CLEAR;
    rp.colorAttachments[0].storeOp = StoreOp::STORE;
    rp.colorAttachments[0].barrier = raw.getGeneralBarrier(colorBarrier);
    rp.colorAttachments[1].format = Format::RGBA8;
    rp.colorAttachments[1].loadOp = LoadOp::DISCARD;
    rp.colorAttachments[1].storeOp = StoreOp::STORE;
    rp.colorAttachments[1].barrier = raw.getGeneralBarrier(colorBarrier);
    rp.depthStencilAttachment.format = Format::DEPTH;
    rp.depthStencilAttachment.sampleCount = gfxSamples;
    rp.depthStencilAttachment.depthLoadOp = LoadOp::CLEAR;
    rp.depthStencilAttachment.depthStoreOp = StoreOp::STORE;
    rp.depthStencilAttachment.barrier = raw.getGeneralBarrier(dsBarrier);
    rp.depthStencilResolveAttachment.format = Format::DEPTH;
    rp.depthStencilResolveAttachment.depthLoadOp = LoadOp::DISCARD;
    rp.depthStencilResolveAttachment.depthStoreOp = StoreOp::STORE;
    rp.depthStencilResolveAttachment.barrier = raw.getGeneralBarrier(dsBarrier);
    rp.subpasses.resize(1);
    rp.subpasses[0].colors = {0};
    rp.subpasses[0].resolves = {1};
    rp.subpasses[0].depthStencil = 2;
    rp.subpasses[0].depthStencilResolve = 3;
    rp.subpasses[0].depthResolveMode = ResolveMode::SAMPLE_ZERO;
    o.rp = raw.createRenderPass(rp);
    check(o.rp != nullptr, "render pass");
    if (!o.rp) return 1;
    auto *gpuRP = static_cast<CCVKRenderPass *>(o.rp)->gpuRenderPass();
    if (!gpuRP) {
        check(false, "gpuRenderPass null");
        return 1;
    }
    unsigned actualDepth = 0, actualStencil = 0;
    if (gpuRP && gpuRP->depthStencilResolveModes.size() == 1) {
        actualDepth = unsigned(gpuRP->depthStencilResolveModes[0].depth);
        actualStencil = unsigned(gpuRP->depthStencilResolveModes[0].stencil);
    }
    check(gpuRP && actualDepth == unsigned(VK_RESOLVE_MODE_SAMPLE_ZERO_BIT),
          "Classic-filtered depth resolve mode is SAMPLE_ZERO");
    CC_LOG_INFO("dsr-modes: depth=%u stencil=%u (Classic filtered values)\n", actualDepth, actualStencil);
    for (unsigned i = 0; i < 2; ++i) {
        FramebufferInfo fb;
        fb.renderPass = o.rp;
        fb.colorTextures = {o.msColors[i], o.colorResolves[i]};
        fb.depthStencilTexture = o.msDepths[i];
        fb.depthStencilResolveTexture = o.depthResolves[i];
        o.fbos[i] = raw.createFramebuffer(fb);
        check(o.fbos[i] != nullptr, "framebuffer");
        if (failures) return 1;
    }
    o.layout = raw.createPipelineLayout(PipelineLayoutInfo{});
    if (!o.layout) {
        check(false, "layout null");
        return 1;
    }
    o.ia = raw.createInputAssembler(InputAssemblerInfo{});
    if (!o.ia) {
        check(false, "IA null");
        return 1;
    }
    // PSO 0: near red z=0.25; PSO 1: far green z=0.75 (LESS rejects it)
    for (unsigned i = 0; i < 2; ++i) {
        ShaderInfo shader;
        shader.name = "dsr-" + std::to_string(i);
        ShaderStage vs;
        vs.stage = ShaderStageFlagBit::VERTEX;
        vs.source =
            "void main(){vec2 p=vec2(float((gl_VertexIndex<<1)&2),float(gl_VertexIndex&2));"
            "gl_Position=vec4(p*2.0-1.0,";
        vs.source += i == 0 ? "0.25" : "0.75";
        vs.source += ",1.0);}";
        ShaderStage fs;
        fs.stage = ShaderStageFlagBit::FRAGMENT;
        fs.source = "layout(location=0) out vec4 o;void main(){o=";
        fs.source += i == 0 ? "vec4(1,0,0,1)" : "vec4(0,1,0,1)";
        fs.source += ";}";
        shader.stages = {vs, fs};
        o.shaders[i] = raw.createShader(shader);
        if (!o.shaders[i]) {
            check(false, "shader facade null");
            return 1;
        }
        PipelineStateInfo pso;
        pso.shader = o.shaders[i];
        pso.pipelineLayout = o.layout;
        pso.renderPass = o.rp;
        pso.rasterizerState.cullMode = CullMode::NONE;
        pso.depthStencilState.depthTest = 1;
        pso.depthStencilState.depthWrite = 1;
        pso.depthStencilState.depthFunc = ComparisonFunc::LESS;
        o.psos[i] = raw.createPipelineState(pso);
        check(o.psos[i] != nullptr, "PSO facade");
        if (!o.psos[i]) return 1;
        const auto *gp = static_cast<CCVKPipelineState *>(o.psos[i])->gpuPipelineState();
        check(gp && gp->vkPipeline && gp->vkDynamicPipeline && !gp->dynamicPipelineCreationFailed, "real dual PSO handles");
        if (createdCount == 2 * (i + 1) && gp) {
            const auto &classic = created[2 * i];
            const auto &dynamic = created[2 * i + 1];
            check(!classic.dynamic && classic.handle == gp->vkPipeline && classic.samples == unsigned(samples) &&
                      classic.renderPass == gpuRP->vkRenderPass &&
                      dynamic.dynamic && dynamic.renderPass == VK_NULL_HANDLE &&
                      dynamic.handle == gp->vkDynamicPipeline && dynamic.samples == unsigned(samples),
                  "driver output/MSAA signature/RP identity equals saved PSO");
        } else
            check(false, "exactly two driver creates per PSO");
    }
    if (failures) return 1;
    CC_LOG_INFO("dsr-pipelines: classic=2 dynamic=2 signatures=OK samples=%u\n", unsigned(samples));
    CommandBufferInfo cmdInfo;
    cmdInfo.queue = raw.getQueue();
    o.cmd = raw.createCommandBuffer(cmdInfo);
    check(o.cmd != nullptr, "command buffer");
    if (!o.cmd) return 1;
    Phase phases[2];
    unsigned colorMismatch[2]{}, depthMismatch[2]{};
    unsigned colorDiff = 0, depthDiff = 0;
    bool incomplete = false;
    uint8_t snapshots[2][2 * PLANE_BYTES];
    for (unsigned i = 0; i < 2; ++i) {
        if (context->validationErrorCount.load() >= 10) {
            incomplete = true;
            break;
        }
        auto &p = phases[i];
        p.dynamic = i == 1;
        p.skipNear = skipNear;
        p.actualDepthResolveMode = actualDepth;
        p.actualStencilResolveMode = actualStencil;
        phase = &p;
        p.colorResolve = static_cast<CCVKTexture *>(o.colorResolves[i])->gpuTexture()->vkImage;
        p.depthResolve = static_cast<CCVKTexture *>(o.depthResolves[i])->gpuTexture()->vkImage;
        p.colorResolveView = static_cast<CCVKTexture *>(o.colorResolves[i])->gpuTextureView()->vkImageView;
        p.depthResolveView = static_cast<CCVKTexture *>(o.depthResolves[i])->gpuTextureView()->vkImageView;
        p.msDepthView = static_cast<CCVKTexture *>(o.msDepths[i])->gpuTextureView()->vkImageView;
        p.msColorView = static_cast<CCVKTexture *>(o.msColors[i])->gpuTextureView()->vkImageView;
        p.expectedRP = gpuRP->vkRenderPass;
        {
            auto *gfbo = static_cast<CCVKFramebuffer *>(o.fbos[i])->gpuFBO();
            check(gfbo, "gpu FBO null");
            if (!gfbo) return 1;
            p.expectedFBO = gfbo->vkFramebuffer;
        }
        auto *host = static_cast<CCVKBuffer *>(o.hosts[i])->gpuBuffer();
        p.host = host->vkBuffer;
        check(p.expectedRP && p.expectedFBO && p.msColorView && p.colorResolve && p.depthResolve && p.colorResolveView && p.depthResolveView && p.msDepthView && p.host &&
                  host->mappedData && host->vmaAllocation,
              "real phase handles/mapping");
        if (failures) {
            phase = nullptr;
            break;
        }
        p.expectedDraws = skipNear ? 1 : 2;
        for (unsigned j = 0; j < 2; ++j) {
            if (skipNear && j == 0) continue;
            auto *gp = static_cast<CCVKPipelineState *>(o.psos[j])->gpuPipelineState();
            p.expectedPipelines[skipNear ? 0 : j] = p.dynamic ? gp->vkDynamicPipeline : gp->vkPipeline;
        }
        const unsigned failuresBefore = failures;
        o.cmd->begin();
        if (p.dynamic && RealProjectScopeObservation::enabled()) {
            const std::array<VkImage, 4> targets{static_cast<CCVKTexture *>(o.msColors[i])->gpuTexture()->vkImage, p.colorResolve,
                                             static_cast<CCVKTexture *>(o.msDepths[i])->gpuTexture()->vkImage, p.depthResolve};
            o.cmd->customCommand([targets](void *native) {
                RealProjectScopeObservation::expect(static_cast<VkCommandBuffer>(native), targets.data(), unsigned(targets.size()));
            });
        }
        const Color blue{0, 0, 1, 1};
        const Color clears[2] = {blue, blue}; // engine reads both color attachments
        {
            struct PolicyGuard {
                VulkanRuntimePaths &paths;
                bool saved;
                ~PolicyGuard() { paths.useDynamicRendering = saved; }
            } policy{gpu->runtimePaths, gpu->runtimePaths.useDynamicRendering};
            gpu->runtimePaths.useDynamicRendering = p.dynamic;
            o.cmd->beginRenderPass(o.rp, o.fbos[i], Rect{0, 0, W, H}, clears, 1.0F, 0, nullptr, 0);
        }
        o.cmd->bindInputAssembler(o.ia);
        DrawInfo draw;
        draw.vertexCount = 3;
        draw.instanceCount = 1;
        for (unsigned j = 0; j < 2; ++j) {
            if (skipNear && j == 0) continue;
            o.cmd->bindPipelineState(o.psos[j]);
            o.cmd->draw(draw);
        }
        o.cmd->endRenderPass();
        o.cmd->customCommand([&](void *native) {
            const auto cb = static_cast<VkCommandBuffer>(native);
            VkImageMemoryBarrier images[2]{};
            for (unsigned j = 0; j < 2; ++j) {
                auto &barrier = images[j];
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                // broad manual barrier: covers classic D/S store writes AND
                // the fixed-function resolve writes (COLOR_ATTACHMENT access)
                barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                barrier.oldLayout = j == 0 ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = j == 0 ? p.colorResolve : p.depthResolve;
                barrier.subresourceRange = {VkImageAspectFlags(j == 0 ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT), 0, 1, 0, 1};
            }
            cmdPipelineBarrier(gpu, cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 2, images);
            for (unsigned j = 0; j < 2; ++j) {
                VkBufferImageCopy region{};
                region.bufferOffset = j * PLANE_BYTES;
                region.imageSubresource = {VkImageAspectFlags(j == 0 ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT), 0, 0, 1};
                region.imageExtent = {W, H, 1};
                vkCmdCopyImageToBuffer(cb, j == 0 ? p.colorResolve : p.depthResolve, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p.host, 1, &region);
            }
            VkBufferMemoryBarrier hostBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            hostBarrier.srcQueueFamilyIndex = hostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            hostBarrier.buffer = p.host;
            hostBarrier.size = VK_WHOLE_SIZE;
            cmdPipelineBarrier(gpu, cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hostBarrier, 0, nullptr);
        });
        o.cmd->end();
        CommandBuffer *commands[] = {o.cmd};
        raw.getQueue()->submit(commands, 1);
        raw.waitAllFences();
        const unsigned liveNow = context->validationErrorCount.load();
        const bool chain = p.cb && p.state == 4 && p.begins == 1 && p.rpBegins == unsigned(!p.dynamic) && p.rpEnds == p.rpBegins &&
                           p.dynBegins == unsigned(p.dynamic) && p.dynEnds == p.dynBegins &&
                           p.draws == p.expectedDraws && p.colorCopies == 1 && p.depthCopies == 1 &&
                           p.submitCalls == 1 && p.submitSuccesses == 1 &&
                           failures == failuresBefore && liveNow == 0;
        check(chain, "complete real phase identity chain");
        CC_LOG_INFO("dsr-phase: mode=%s cb=%p rp=%u/%u dynamic=%u/%u draws=%u copies=%u/%u submits=%u/%u chain=%u\n",
                    p.dynamic ? "dynamic" : "classic", (void *)p.cb, p.rpBegins, p.rpEnds, p.dynBegins, p.dynEnds,
                    p.draws, p.colorCopies, p.depthCopies, p.submitSuccesses, p.submitCalls, unsigned(chain));
        phase = nullptr;
        if (!chain || context->validationErrorCount.load() >= 10) {
            incomplete = true;
            break;
        }
        const VkResult invalidated = vmaInvalidateAllocation(gpu->memoryAllocator, host->vmaAllocation, 0, VK_WHOLE_SIZE);
        check(invalidated == VK_SUCCESS, "invalidate real mapped readback");
        if (invalidated != VK_SUCCESS) {
            incomplete = true;
            break;
        }
        std::memcpy(snapshots[i], host->mappedData, hostBytes);
        // color plane: golden red everywhere (near-red wins, far-green rejected)
        const uint8_t red[] = {255, 0, 0, 255};
        unsigned colorChecked = 0;
        for (unsigned byte = 0; byte < PLANE_BYTES; ++byte) {
            ++colorChecked;
            colorMismatch[i] += snapshots[i][byte] != red[byte % 4];
        }
        unsigned depthChecked = 0;
        for (unsigned pixel = 0; pixel < PIXELS; ++pixel) {
            float depth = 0;
            std::memcpy(&depth, snapshots[i] + PLANE_BYTES + 4 * pixel, sizeof(depth));
            ++depthChecked;
            depthMismatch[i] += depth != 0.25F;
        }
        check(colorChecked == PLANE_BYTES && depthChecked == PIXELS, "golden check covered every byte/pixel");
        CC_LOG_INFO("dsr-readback: mode=%s colorCheckedBytes=%u depthCheckedPixels=%u colorMismatch=%u depthMismatch=%u\n",
                    p.dynamic ? "dynamic" : "classic", colorChecked, depthChecked, colorMismatch[i], depthMismatch[i]);
    }
    phase = nullptr;
    if (!incomplete && phases[0].submitSuccesses == 1 && phases[1].submitSuccesses == 1) {
        for (unsigned byte = 0; byte < hostBytes; ++byte) {
            const bool colorByte = byte < PLANE_BYTES;
            colorDiff += colorByte && snapshots[0][byte] != snapshots[1][byte];
            depthDiff += !colorByte && snapshots[0][byte] != snapshots[1][byte];
        }
        check(colorDiff == 0 && depthDiff == 0, "Classic/Dynamic both-resolve equality");
        CC_LOG_INFO("dsr-compare: colorBytes=%u colorDiff=%u depthBytes=%u depthDiff=%u\n",
                    PLANE_BYTES, colorDiff, PLANE_BYTES, depthDiff);
    } else {
        incomplete = true;
        CC_LOG_INFO("INCOMPLETE: missing successful two-phase readback\n");
    }
    raw.waitAllFences();
    o.clear();
    o.device = nullptr;
    const unsigned liveErrors = context->validationErrorCount.load();
    const unsigned postErrors = 0; // final shutdown errors are checked in finish, not synthesized here.
    const unsigned totalMismatch = colorMismatch[0] + colorMismatch[1] + depthMismatch[0] + depthMismatch[1];
    const bool pass = !failures && !incomplete && !liveErrors && !postErrors && !totalMismatch && !colorDiff && !depthDiff;
    CC_LOG_INFO("dsr %ux readback: failures=%u liveErrors=%u postErrors=%u mismatches=%u outcome=%s\n",
                samples == VK_SAMPLE_COUNT_2_BIT ? 2 : 4, failures, liveErrors, postErrors, totalMismatch,
                pass ? "CLEAN" : "FAILED");
    return pass ? 0 : 1;
}
CCVKGPUContext *liveContext = nullptr;
PFN_vkDestroyDevice originalDestroyDevice = nullptr;
unsigned finalLiveErrors = 0, finalPostErrors = 0, deviceDestroys = 0;
bool observing = false;
VKAPI_ATTR void VKAPI_CALL observeDeviceDestroy(VkDevice d, const VkAllocationCallbacks *a) {
    if (!originalDestroyDevice) {
        check(false, "device destroy PFN");
        return;
    }
    if (liveContext) finalLiveErrors = liveContext->validationErrorCount.load();
    originalDestroyDevice(d, a);
    if (liveContext) finalPostErrors = liveContext->validationErrorCount.load();
    liveContext = nullptr;
    ++deviceDestroys;
}
int finish(int platformResult, int diagnosticResult) {
    if (observing) {
        vkDestroyDevice = originalDestroyDevice;
        if (realDestroy) vkDestroyPipeline = realDestroy;
        observing = false;
    }
    if (diagnosticResult == 3) return 3;
    unsigned retired = 0;
    for (unsigned i = 0; i < createdCount; ++i) {
        check(created[i].destroyed == 1, "real-project PSO retired once");
        retired += created[i].destroyed;
    }
    check(createdCount == 4 && retired == 4 && deviceDestroys == 1, "actual real-project shutdown retirement");
    check(!finalLiveErrors && !finalPostErrors, "real-project live/post Validation");
    CC_LOG_INFO("project-resolve retirement: kind=depth created=%u destroyed=%u devices=%u liveErrors=%u postErrors=%u", createdCount, retired, deviceDestroys, finalLiveErrors, finalPostErrors);
    return platformResult || diagnosticResult || failures ? 1 : 0;
}
} // namespace cc::vulkan_test::depth_resolve
namespace cc::vulkan_test {
int runDepthResolve(unsigned samples, bool skip) {
    using namespace depth_resolve;
    auto *d = gfx::CCVKDevice::getInstance();
    if (!d || !d->gpuContext() || !d->gpuDevice()) return 1;
    originalDestroyDevice = vkDestroyDevice;
    if (!originalDestroyDevice) return 1;
    liveContext = d->gpuContext();
    observing = true;
    vkDestroyDevice = observeDeviceDestroy;
    try {
        return run(static_cast<VkSampleCountFlagBits>(samples), skip);
    } catch (const std::exception &e) {
        check(false, e.what());
        return 1;
    } catch (...) {
        check(false, "diagnostic exception");
        return 1;
    }
}
int finishDepthResolve(int platformResult, int diagnosticResult) { return depth_resolve::finish(platformResult, diagnosticResult); }
} // namespace cc::vulkan_test
