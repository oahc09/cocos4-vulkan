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

// M6-A4: real color MSAA resolve harness (Classic vs Dynamic hybrid path).
// Reuses the DS harness object RAII / fixed-capacity hooks / per-phase CB
// identity chains / real PSO / readback / retirement patterns. Two cases
// (2x, 4x): primary RGBA8 MSAA texture + 1x resolve target, engine RP with
// colors{0} resolves{1}, engine-drawn full-screen triangle (red on blue for
// uniform / diagonal partial for coverage), resolve read back to host and
// compared Classic vs Dynamic. --skip-draw is the negative control.

#include "RealProjectResolve.h"
#include "RealProjectScopeObservation.h"
#include "base/Log.h"
#include "gfx-base/SPIRVUtils.h"
#include "gfx-vulkan/VKBuffer.h"
#include "gfx-vulkan/VKDevice.h"
#include "gfx-vulkan/VKGPUObjects.h"
#include "gfx-vulkan/VKPipelineState.h"
#include "gfx-vulkan/VKRenderPass.h"
#include "gfx-vulkan/VKTexture.h"
#include "platform/FileUtils.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

#if defined(_WIN32)
    #include <windows.h>
#endif

namespace cc::vulkan_test::color_resolve {
using namespace cc;
using namespace cc::gfx;
constexpr uint32_t W = 17, H = 11, PIXELS = W * H, COLOR_BYTES = PIXELS * 4;
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
    Texture *primaries[2]{}, *resolves[2]{};
    Buffer *hosts[2]{};
    void clear() {
        drop(cmd);
        drop(ia);
        for (auto &p : psos) drop(p);
        for (auto &f : fbos) drop(f);
        drop(rp);
        drop(layout);
        for (auto &s : shaders) drop(s);
        for (auto &t : primaries) drop(t);
        for (auto &t : resolves) drop(t);
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
    VkFormat color = VK_FORMAT_UNDEFINED;
    unsigned samples = 0;
    unsigned destroyed = 0;
};
CreatedPipeline created[4];
unsigned createdCount = 0;
VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_2_BIT;
struct Phase {
    bool dynamic = false;
    bool edge = false; // pattern: uniform(false) / edge(true)
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkImage resolve = VK_NULL_HANDLE;
    VkImageView msaaView = VK_NULL_HANDLE, resolveView = VK_NULL_HANDLE;
    VkBuffer host = VK_NULL_HANDLE;
    VkPipeline bound = VK_NULL_HANDLE, expectedPipeline = VK_NULL_HANDLE;
    unsigned begins = 0, rpBegins = 0, rpEnds = 0, dynBegins = 0, dynEnds = 0;
    unsigned draws = 0, copies = 0, submitCalls = 0, submitSuccesses = 0;
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
    const VkResult result = realBegin(cb, info);
    if (phase && result == VK_SUCCESS) {
        phase->cb = cb;
        ++phase->begins;
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL onRPBegin(VkCommandBuffer cb, const VkRenderPassBeginInfo *info, VkSubpassContents contents) {
    realRPBegin(cb, info, contents);
    if (phase) {
        check(cb == phase->cb && info->renderPass != VK_NULL_HANDLE, "Classic begin CB/RP");
        ++phase->rpBegins;
    }
}
VKAPI_ATTR void VKAPI_CALL onRPEnd(VkCommandBuffer cb) {
    realRPEnd(cb);
    if (phase) {
        check(cb == phase->cb, "Classic end CB");
        ++phase->rpEnds;
    }
}
VKAPI_ATTR void VKAPI_CALL onDynBegin(VkCommandBuffer cb, const VkRenderingInfo *info) {
    realDynBegin(cb, info);
    if (!phase) return;
    ++phase->dynBegins;
    check(cb == phase->cb && info && info->colorAttachmentCount == 1 && info->layerCount == 1 && info->viewMask == 0,
          "Dynamic root/CB");
    if (!info) return;
    const auto *color = info->pColorAttachments;
    check(color && color->imageView == phase->msaaView && color->resolveMode == VK_RESOLVE_MODE_AVERAGE_BIT &&
              color->resolveImageView == phase->resolveView &&
              color->imageLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
              color->resolveImageLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
              color->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && color->storeOp == VK_ATTACHMENT_STORE_OP_STORE,
          "Dynamic MSAA resolve node");
    check(info->pDepthAttachment == nullptr && info->pStencilAttachment == nullptr, "Dynamic no D/S nodes");
}
VKAPI_ATTR void VKAPI_CALL onDynEnd(VkCommandBuffer cb) {
    realDynEnd(cb);
    if (phase) {
        check(cb == phase->cb, "Dynamic end CB");
        ++phase->dynEnds;
    }
}
VKAPI_ATTR void VKAPI_CALL onBind(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipeline pipeline) {
    realBind(cb, point, pipeline);
    if (phase && point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        check(cb == phase->cb, "bind CB");
        phase->bound = pipeline;
    }
}
VKAPI_ATTR void VKAPI_CALL onDraw(VkCommandBuffer cb, uint32_t vertices, uint32_t instances, uint32_t firstVertex, uint32_t firstInstance) {
    realDraw(cb, vertices, instances, firstVertex, firstInstance);
    if (!phase) return;
    check(cb == phase->cb && phase->expectedPipeline != VK_NULL_HANDLE && phase->expectedPipeline == phase->bound &&
              vertices == 3 && instances == 1 && firstVertex == 0 && firstInstance == 0,
          "draw CB/pipeline/arguments");
    ++phase->draws;
}
VKAPI_ATTR void VKAPI_CALL onCopy(VkCommandBuffer cb, VkImage image, VkImageLayout layout, VkBuffer buffer, uint32_t count, const VkBufferImageCopy *regions) {
    realCopy(cb, image, layout, buffer, count, regions);
    if (!phase) return;
    check(cb == phase->cb && layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && buffer == phase->host && count == 1 && regions,
          "copy CB/layout/host/count");
    if (!regions || count != 1) return;
    const auto &r = regions[0];
    check(r.imageExtent.width == W && r.imageExtent.height == H && r.imageExtent.depth == 1 &&
              r.imageSubresource.layerCount == 1 && r.imageSubresource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT &&
              r.bufferOffset == 0 && r.bufferRowLength == 0 && r.bufferImageHeight == 0 && image == phase->resolve,
          "resolve-image full-plane copy");
    ++phase->copies;
}
VKAPI_ATTR VkResult VKAPI_CALL onSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *infos, VkFence fence) {
    const VkResult result = realSubmit(queue, count, infos, fence);
    if (!phase) return result;
    ++phase->submitCalls;
    if (result == VK_SUCCESS) {
        unsigned forwarded = 0;
        for (uint32_t i = 0; i < count; ++i)
            for (uint32_t j = 0; j < infos[i].commandBufferCount; ++j)
                if (infos[i].pCommandBuffers[j] == phase->cb) ++forwarded;
        check(phase->cb && forwarded == 1, "successful submit forwards this phase CB exactly once");
        if (forwarded == 1) ++phase->submitSuccesses;
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL onCreate(VkDevice device, VkPipelineCache cache, uint32_t count, const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *alloc, VkPipeline *pipelines) {
    const VkResult result = realCreate(device, cache, count, infos, alloc, pipelines);
    check(count == 1 && infos && pipelines && result == VK_SUCCESS && pipelines[0] != VK_NULL_HANDLE && createdCount < 4,
          "real graphics pipeline creation");
    if (count != 1 || !infos || !pipelines || result != VK_SUCCESS || !pipelines[0] || createdCount >= 4) return result;
    auto &record = created[createdCount++];
    record.handle = pipelines[0];
    record.dynamic = infos[0].renderPass == VK_NULL_HANDLE;
    record.samples = infos[0].pMultisampleState ? infos[0].pMultisampleState->rasterizationSamples : 0;
    if (record.dynamic) {
        const auto *node = static_cast<const VkPipelineRenderingCreateInfo *>(infos[0].pNext);
        check(node && node->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO && !node->pNext &&
                  node->colorAttachmentCount == 1 && node->pColorAttachmentFormats &&
                  node->pColorAttachmentFormats[0] == VK_FORMAT_R8G8B8A8_UNORM &&
                  node->depthAttachmentFormat == VK_FORMAT_UNDEFINED && node->stencilAttachmentFormat == VK_FORMAT_UNDEFINED &&
                  node->viewMask == 0 && infos[0].subpass == 0 && record.samples == msaaSamples,
              "Dynamic pipeline MSAA signature");
        if (node && node->pColorAttachmentFormats && node->colorAttachmentCount == 1)
            record.color = node->pColorAttachmentFormats[0];
    } else
        check(!infos[0].pNext && infos[0].subpass == 0 && record.samples == msaaSamples, "Classic pipeline MSAA root");
    return result;
}
VKAPI_ATTR void VKAPI_CALL onDestroy(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks *alloc) {
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

int run(VkSampleCountFlagBits samples, bool skipDraw, bool wrongUniform) {
    msaaSamples = samples;
    auto &raw = *CCVKDevice::getInstance();
    auto *context = raw.gpuContext();
    auto *gpu = raw.gpuDevice();
    const bool validation = context->validationEnabled && context->synchronizationValidationEnabled && context->bestPracticesValidationEnabled;
    CC_LOG_INFO("validation + best practices + sync: %s\n", validation ? "ENABLED" : "NOT VERIFIED");
    if (!validation) return 1;
    if (!gpu->capabilities.dynamicRendering.enabled || !gpu->runtimePaths.useDynamicRendering ||
        context->apiVersionInfo.effectiveDeviceApiVersion < VK_API_VERSION_1_3) {
        CC_LOG_ERROR("UNSUPPORTED: requires enabled engine Dynamic policy at effective 1.3\n");
        return 3;
    }
    if (!vkBeginCommandBuffer || !vkCmdBeginRenderPass || !vkCmdEndRenderPass || !vkCmdBeginRendering || !vkCmdEndRendering ||
        !vkCmdBindPipeline || !vkCmdDraw || !vkCmdCopyImageToBuffer || !vkQueueSubmit || !vkCreateGraphicsPipelines || !vkDestroyPipeline ||
        !vkGetPhysicalDeviceImageFormatProperties) {
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
    // real capability queries: MSAA primary usage and 1x resolve usage
    for (int i = 0; i < 2; ++i) {
        const auto usage = i == 0 ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                  : VkImageUsageFlags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        const auto want = i == 0 ? samples : VK_SAMPLE_COUNT_1_BIT;
        VkImageFormatProperties props{};
        const VkResult result = vkGetPhysicalDeviceImageFormatProperties(context->physicalDevice, VK_FORMAT_R8G8B8A8_UNORM,
                                                                         VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &props);
        if (result == VK_ERROR_FORMAT_NOT_SUPPORTED) {
            CC_LOG_ERROR("UNSUPPORTED: RGBA8 usage %d\n", i);
            return 3;
        }
        if (result != VK_SUCCESS) {
            check(false, "real RGBA8 image-format query failed");
            return 1;
        }
        if (!(props.sampleCounts & want) || props.maxExtent.width < W || props.maxExtent.height < H) {
            CC_LOG_ERROR("UNSUPPORTED: RGBA8 usage %d sample/extent\n", i);
            return 3;
        }
    }
    if (failures) return 1;
    Hooks hooks;
    Objects o;
    o.device = &raw;
    CC_LOG_INFO("msaa-config: samples=%u resolve=1 pixels=%u hostBytes=%u runtimeDynamic=1\n",
                unsigned(samples), PIXELS, COLOR_BYTES);
    for (unsigned i = 0; i < 2; ++i) {
        TextureInfo primary;
        primary.type = TextureType::TEX2D;
        primary.format = Format::RGBA8;
        primary.width = W;
        primary.height = H;
        primary.usage = TextureUsageBit::COLOR_ATTACHMENT;
        primary.samples = samples == VK_SAMPLE_COUNT_2_BIT ? SampleCount::X2 : SampleCount::X4;
        o.primaries[i] = raw.createTexture(primary);
        TextureInfo resolve;
        resolve.type = TextureType::TEX2D;
        resolve.format = Format::RGBA8;
        resolve.width = W;
        resolve.height = H;
        resolve.usage = TextureUsageBit::COLOR_ATTACHMENT | TextureUsageBit::TRANSFER_SRC;
        o.resolves[i] = raw.createTexture(resolve);
        BufferInfo host;
        host.usage = BufferUsageBit::TRANSFER_DST;
        host.memUsage = MemoryUsageBit::HOST;
        host.size = COLOR_BYTES;
        host.stride = 1;
        o.hosts[i] = raw.createBuffer(host);
        check(o.primaries[i] && o.resolves[i] && o.hosts[i], "texture/host facades");
    }
    if (failures) return 1;
    GeneralBarrierInfo colorBarrier;
    colorBarrier.nextAccesses = AccessFlagBit::COLOR_ATTACHMENT_WRITE;
    RenderPassInfo rp;
    rp.colorAttachments.resize(2);
    rp.colorAttachments[0].format = Format::RGBA8;
    rp.colorAttachments[0].sampleCount = samples == VK_SAMPLE_COUNT_2_BIT ? SampleCount::X2 : SampleCount::X4;
    rp.colorAttachments[0].loadOp = LoadOp::CLEAR;
    rp.colorAttachments[0].storeOp = StoreOp::STORE;
    rp.colorAttachments[0].barrier = raw.getGeneralBarrier(colorBarrier);
    rp.colorAttachments[1].format = Format::RGBA8;
    rp.colorAttachments[1].loadOp = LoadOp::DISCARD;
    rp.colorAttachments[1].storeOp = StoreOp::STORE;
    rp.colorAttachments[1].barrier = raw.getGeneralBarrier(colorBarrier);
    rp.subpasses.resize(1);
    rp.subpasses[0].colors = {0};
    rp.subpasses[0].resolves = {1};
    o.rp = raw.createRenderPass(rp);
    check(o.rp != nullptr, "render pass");
    if (!o.rp) return 1;
    for (unsigned i = 0; i < 2; ++i) {
        FramebufferInfo fb;
        fb.renderPass = o.rp;
        fb.colorTextures = {o.primaries[i], o.resolves[i]};
        o.fbos[i] = raw.createFramebuffer(fb);
        check(o.fbos[i] != nullptr, "framebuffer");
    }
    if (failures) return 1;
    o.layout = raw.createPipelineLayout(PipelineLayoutInfo{});
    o.ia = raw.createInputAssembler(InputAssemblerInfo{});
    check(o.layout && o.ia, "layout/input assembler facades");
    if (!o.layout || !o.ia) return 1;
    // two patterns: 0 = uniform full-screen red (blue clear), 1 = lower-left diagonal red triangle
    for (unsigned i = 0; i < 2; ++i) {
        ShaderInfo shader;
        shader.name = "msaa-" + std::to_string(i);
        ShaderStage vs;
        vs.stage = ShaderStageFlagBit::VERTEX;
        vs.source = i == 0 ? "void main(){vec2 p=vec2(float((gl_VertexIndex<<1)&2),float(gl_VertexIndex&2));"
                             "gl_Position=vec4(p*2.0-1.0,0.0,1.0);}"
                           : "void main(){vec2 p[3]=vec2[3](vec2(-1.0,-1.0),vec2(1.0,-1.0),vec2(-1.0,1.0));"
                             "gl_Position=vec4(p[gl_VertexIndex],0.0,1.0);}";
        ShaderStage fs;
        fs.stage = ShaderStageFlagBit::FRAGMENT;
        // A real negative control keeps the draw/submit counts and diagonal
        // output intact, but turns only the uniform fragment output blue.
        fs.source = i == 0 && wrongUniform ? "layout(location=0) out vec4 o;void main(){o=vec4(0.0,0.0,1.0,1.0);}"
                                           : "layout(location=0) out vec4 o;void main(){o=vec4(1.0,0.0,0.0,1.0);}";
        shader.stages = {vs, fs};
        o.shaders[i] = raw.createShader(shader);
        check(o.shaders[i] != nullptr, "shader facade");
        if (!o.shaders[i]) return 1;
        PipelineStateInfo pso;
        pso.shader = o.shaders[i];
        pso.pipelineLayout = o.layout;
        pso.renderPass = o.rp;
        pso.rasterizerState.cullMode = CullMode::NONE;
        o.psos[i] = raw.createPipelineState(pso);
        check(o.psos[i] != nullptr, "PSO facade");
        if (!o.psos[i]) return 1;
        const auto *gp = static_cast<CCVKPipelineState *>(o.psos[i])->gpuPipelineState();
        check(gp && gp->vkPipeline && gp->vkDynamicPipeline && !gp->dynamicPipelineCreationFailed, "real dual PSO handles");
        if (createdCount == 2 * (i + 1) && gp) {
            const auto &classic = created[2 * i];
            const auto &dynamic = created[2 * i + 1];
            check(!classic.dynamic && classic.handle == gp->vkPipeline && classic.samples == unsigned(samples) &&
                      dynamic.dynamic && dynamic.handle == gp->vkDynamicPipeline &&
                      dynamic.color == VK_FORMAT_R8G8B8A8_UNORM && dynamic.samples == unsigned(samples),
                  "driver output/MSAA signature equals saved PSO");
        } else
            check(false, "exactly two driver creates per PSO");
    }
    if (failures) return 1;
    CC_LOG_INFO("msaa-pipelines: classic=2 dynamic=2 signatures=OK samples=%u\n", unsigned(samples));
    CommandBufferInfo cmdInfo;
    cmdInfo.queue = raw.getQueue();
    o.cmd = raw.createCommandBuffer(cmdInfo);
    check(o.cmd != nullptr, "command buffer");
    if (!o.cmd) return 1;
    // four independent phase records: {classic,dynamic} x {uniform,edge}
    Phase phases[4];
    std::array<std::array<uint8_t, COLOR_BYTES>, 4> snapshots{};
    bool snapshotValid[4]{};
    unsigned uniformMismatch[2]{}, edgeMismatch[2]{}, uniformDiff = 0, edgeDiff = 0;
    bool incomplete = false;
    for (unsigned phaseIdx = 0; phaseIdx < 4; ++phaseIdx) {
        auto &p = phases[phaseIdx];
        p.dynamic = phaseIdx / 2 == 1;
        p.edge = phaseIdx % 2 == 1;
        const unsigned modeIdx = phaseIdx / 2; // 0 classic, 1 dynamic
        const unsigned textureIdx = modeIdx;   // classic uses FBO 0, dynamic FBO 1
        if (context->validationErrorCount.load() >= 10) {
            incomplete = true;
            break;
        }
        const unsigned failuresBefore = failures; // local guard only for setup
        phase = &p;
        p.resolve = static_cast<CCVKTexture *>(o.resolves[textureIdx])->gpuTexture()->vkImage;
        p.msaaView = static_cast<CCVKTexture *>(o.primaries[textureIdx])->gpuTextureView()->vkImageView;
        p.resolveView = static_cast<CCVKTexture *>(o.resolves[textureIdx])->gpuTextureView()->vkImageView;
        auto *host = static_cast<CCVKBuffer *>(o.hosts[textureIdx])->gpuBuffer();
        p.host = host->vkBuffer;
        check(p.resolve && p.msaaView && p.resolveView && p.host && host->mappedData && host->vmaAllocation,
              "real phase handles/mapping");
        if (failures != failuresBefore) {
            phase = nullptr;
            break;
        }
        auto *gp = static_cast<CCVKPipelineState *>(o.psos[p.edge ? 1 : 0])->gpuPipelineState();
        p.expectedPipeline = p.dynamic ? gp->vkDynamicPipeline : gp->vkPipeline;
        o.cmd->begin();
        if (p.dynamic && RealProjectScopeObservation::enabled()) {
            const std::array<VkImage, 2> targets{static_cast<CCVKTexture *>(o.primaries[textureIdx])->gpuTexture()->vkImage, p.resolve};
            o.cmd->customCommand([targets](void *native) {
                RealProjectScopeObservation::expect(static_cast<VkCommandBuffer>(native), targets.data(), unsigned(targets.size()));
            });
        }
        const Color clears[2] = {{0, 0, 1, 1}, {0, 0, 1, 1}}; // engine reads both attachments
        {
            struct PolicyGuard {
                VulkanRuntimePaths &paths;
                bool saved;
                ~PolicyGuard() { paths.useDynamicRendering = saved; }
            } policy{gpu->runtimePaths, gpu->runtimePaths.useDynamicRendering};
            gpu->runtimePaths.useDynamicRendering = p.dynamic;
            o.cmd->beginRenderPass(o.rp, o.fbos[textureIdx], Rect{0, 0, W, H}, clears, 1.0F, 0, nullptr, 0);
        }
        o.cmd->bindInputAssembler(o.ia);
        o.cmd->bindPipelineState(o.psos[p.edge ? 1 : 0]);
        if (!skipDraw) {
            DrawInfo draw;
            draw.vertexCount = 3;
            draw.instanceCount = 1;
            o.cmd->draw(draw);
        }
        o.cmd->endRenderPass();
        o.cmd->customCommand([&](void *native) {
            const auto cb = static_cast<VkCommandBuffer>(native);
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = p.resolve;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            cmdPipelineBarrier(gpu, cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, 0, nullptr, 0, nullptr, 1, &barrier);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {W, H, 1};
            vkCmdCopyImageToBuffer(cb, p.resolve, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p.host, 1, &region);
            VkBufferMemoryBarrier hostBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            hostBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            hostBarrier.srcQueueFamilyIndex = hostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            hostBarrier.buffer = p.host;
            hostBarrier.size = VK_WHOLE_SIZE;
            cmdPipelineBarrier(gpu, cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                               0, 0, nullptr, 1, &hostBarrier, 0, nullptr);
        });
        // explicit caller-side attachment-reuse dependency between two render
        // passes on the same images (manual test dependency, not M4-B work)
        if (!p.edge) {
            o.cmd->customCommand([&](void *native) {
                const auto cb = static_cast<VkCommandBuffer>(native);
                VkMemoryBarrier broad{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                broad.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                broad.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                cmdPipelineBarrier(gpu, cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   0, 1, &broad, 0, nullptr, 0, nullptr);
            });
        }
        o.cmd->end();
        CommandBuffer *commands[] = {o.cmd};
        raw.getQueue()->submit(commands, 1);
        raw.waitAllFences();
        const bool chain = failures == failuresBefore && context->validationErrorCount.load() == 0 && p.cb && p.begins == 1 &&
                           p.rpBegins == unsigned(!p.dynamic) && p.rpEnds == p.rpBegins &&
                           p.dynBegins == unsigned(p.dynamic) && p.dynEnds == p.dynBegins &&
                           p.draws == unsigned(!skipDraw) && p.copies == 1 && p.submitCalls == 1 && p.submitSuccesses == 1;
        check(chain, "complete real phase identity chain");
        CC_LOG_INFO("msaa-phase: mode=%s pattern=%s cb=%p rp=%u/%u dynamic=%u/%u draws=%u copies=%u submits=%u/%u chain=%u\n",
                    p.dynamic ? "dynamic" : "classic", p.edge ? "edge" : "uniform", (void *)p.cb,
                    p.rpBegins, p.rpEnds, p.dynBegins, p.dynEnds, p.draws, p.copies,
                    p.submitSuccesses, p.submitCalls, unsigned(chain));
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
        // own the 748 bytes BEFORE any later phase can rewrite this host buffer
        std::memcpy(snapshots[phaseIdx].data(), host->mappedData, COLOR_BYTES);
        snapshotValid[phaseIdx] = true;
        // content analysis from the owned snapshot
        unsigned red = 0, blue = 0, mixed = 0, mismatch = 0, checkedBytes = 0;
        if (!p.edge) {
            // uniform golden: every byte must be exactly red RGBA(255,0,0,255)
            for (unsigned byte = 0; byte < COLOR_BYTES; ++byte) {
                mismatch += snapshots[phaseIdx][byte] != (byte % 4 == 0 ? 255 : (byte % 4 == 3 ? 255 : 0));
                ++checkedBytes;
            }
            for (unsigned pixel = 0; pixel < PIXELS; ++pixel) {
                const uint8_t *px = &snapshots[phaseIdx][4 * pixel];
                if (px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 255)
                    ++red;
                else if (px[0] == 0 && px[2] == 255)
                    ++blue;
                else
                    ++mixed;
            }
            uniformMismatch[modeIdx] = mismatch;
        } else {
            // edge: per-pixel rules with pixel-center margins and MSAA quantization
            const unsigned sampleCount = samples == VK_SAMPLE_COUNT_2_BIT ? 2 : 4;
            const float margin = 2.0F / W + 2.0F / H;
            for (unsigned pixel = 0; pixel < PIXELS; ++pixel) {
                const uint32_t x = pixel % W, y = pixel / W;
                const uint8_t *px = &snapshots[phaseIdx][4 * pixel];
                const unsigned r = px[0], g = px[1], b = px[2], a = px[3];
                checkedBytes += 4;
                if (g != 0 || a != 255 || r + b < 254 || r + b > 256) {
                    ++mismatch;
                    continue;
                }
                if (r == 255 && b == 0)
                    ++red;
                else if (r == 0 && b == 255)
                    ++blue;
                else {
                    ++mixed;
                    bool plausible = false;
                    for (unsigned k = 1; k < sampleCount; ++k) {
                        const unsigned avg = (255U * k + sampleCount / 2) / sampleCount;
                        const unsigned floorVal = (255U * k) / sampleCount;
                        if (r >= floorVal - 1 && r <= avg + 1) plausible = true;
                    }
                    if (!plausible) ++mismatch;
                }
                const float cx = (x + 0.5F) / W, cy = (y + 0.5F) / H; // pixel center
                const float dist = cx + cy;                           // diagonal at 1.0
                if (dist < 1.0F - margin) {
                    if (r < 254) ++mismatch; // interior must be red-ish
                } else if (dist > 1.0F + margin) {
                    if (r > 1) ++mismatch; // exterior must be blue-ish
                }
            }
            edgeMismatch[modeIdx] = mismatch;
            check(red > 0 && blue > 0 && mixed > 0 && red + blue + mixed == PIXELS,
                  "edge contains covered, uncovered and mixed pixels");
        }
        check(mismatch == 0 && checkedBytes == COLOR_BYTES, "complete pattern golden/content checks");
        CC_LOG_INFO("msaa-readback: mode=%s pattern=%s bytes=%u checkedBytes=%u red=%u blue=%u mixed=%u mismatch=%u\n",
                    p.dynamic ? "dynamic" : "classic", p.edge ? "edge" : "uniform", COLOR_BYTES,
                    checkedBytes, red, blue, mixed, mismatch);
    }
    // mutual comparisons from the OWNED snapshots (uniform and edge separately)
    if (snapshotValid[0] && snapshotValid[2]) {
        for (unsigned byte = 0; byte < COLOR_BYTES; ++byte)
            uniformDiff += snapshots[0][byte] != snapshots[2][byte];
        check(uniformDiff == 0, "Classic/Dynamic uniform resolve equality");
    } else
        incomplete = true;
    if (snapshotValid[1] && snapshotValid[3]) {
        for (unsigned byte = 0; byte < COLOR_BYTES; ++byte)
            edgeDiff += snapshots[1][byte] != snapshots[3][byte];
        check(edgeDiff == 0, "Classic/Dynamic edge resolve equality");
    } else
        incomplete = true;
    CC_LOG_INFO("msaa-compare: pattern=uniform bytes=%u differences=%u\n", COLOR_BYTES, uniformDiff);
    CC_LOG_INFO("msaa-compare: pattern=edge bytes=%u differences=%u\n", COLOR_BYTES, edgeDiff);
    if (incomplete) CC_LOG_INFO("INCOMPLETE: missing successful phase/readback\n");
    phase = nullptr;
    raw.waitAllFences();
    o.clear();
    o.device = nullptr;
    const unsigned liveErrors = context->validationErrorCount.load();
    const unsigned postErrors = 0; // final shutdown errors are checked in finish, not synthesized here.
    const unsigned totalMismatch = uniformMismatch[0] + uniformMismatch[1] + edgeMismatch[0] + edgeMismatch[1];
    const bool pass = !failures && !incomplete && !liveErrors && !postErrors && !totalMismatch && !uniformDiff && !edgeDiff;
    CC_LOG_INFO("msaa %ux readback: failures=%u liveErrors=%u postErrors=%u mismatches=%u outcome=%s\n",
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
    CC_LOG_INFO("project-resolve retirement: kind=color created=%u destroyed=%u devices=%u liveErrors=%u postErrors=%u", createdCount, retired, deviceDestroys, finalLiveErrors, finalPostErrors);
    return platformResult || diagnosticResult || failures ? 1 : 0;
}
} // namespace cc::vulkan_test::color_resolve
namespace cc::vulkan_test {
int runColorResolve(unsigned samples, bool skip, bool wrong) {
    using namespace color_resolve;
    auto *d = gfx::CCVKDevice::getInstance();
    if (!d || !d->gpuContext() || !d->gpuDevice()) return 1;
    originalDestroyDevice = vkDestroyDevice;
    if (!originalDestroyDevice) return 1;
    liveContext = d->gpuContext();
    observing = true;
    vkDestroyDevice = observeDeviceDestroy;
    try {
        return run(static_cast<VkSampleCountFlagBits>(samples), skip, wrong);
    } catch (const std::exception &e) {
        check(false, e.what());
        return 1;
    } catch (...) {
        check(false, "diagnostic exception");
        return 1;
    }
}
int finishColorResolve(int platformResult, int diagnosticResult) { return color_resolve::finish(platformResult, diagnosticResult); }
} // namespace cc::vulkan_test
