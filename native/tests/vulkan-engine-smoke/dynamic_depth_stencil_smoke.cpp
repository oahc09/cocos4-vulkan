// M6: real engine Classic/Hybrid D32 and D32S8 draws plus color and D/S readback.
// The format-map override is harness-only, follows real image-format queries,
// and is restored before device teardown. No raw rendering/draw substitutes.
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include "gfx-base/GFXInputAssembler.h"
#include "gfx-base/SPIRVUtils.h"
#include "gfx-vulkan/VKBuffer.h"
#include "gfx-vulkan/VKCommands.h"
#include "gfx-vulkan/VKDevice.h"
#include "gfx-vulkan/VKPipelineState.h"
#include "gfx-vulkan/VKRenderPass.h"
#include "gfx-vulkan/VKTexture.h"
#include "gfx-vulkan/VKUtils.h"
#include "platform/FileUtils.h"
#include "renderer/GFXDeviceManager.h"

int cocos_main(int, const char **) { return 0; }
void cocos_destroy() {}

namespace {
using namespace cc;
using namespace cc::gfx;
constexpr uint32_t W = 17, H = 11, PIXELS = W * H, COLOR_BYTES = PIXELS * 4;
unsigned failures = 0;
void check(bool condition, const char *what) {
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
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
    Shader *shaders[3]{};
    PipelineState *psos[3]{};
    Framebuffer *fbos[2]{};
    RenderPass *rp = nullptr;
    Texture *colors[2]{}, *ds[2]{};
    Buffer *hosts[2]{};
    void clear() {
        drop(cmd);
        drop(ia);
        for (auto &p : psos) drop(p);
        for (auto &f : fbos) drop(f);
        drop(rp);
        drop(layout);
        for (auto &s : shaders) drop(s);
        for (auto &t : colors) drop(t);
        for (auto &t : ds) drop(t);
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
    VkFormat color = VK_FORMAT_UNDEFINED, depth = VK_FORMAT_UNDEFINED, stencil = VK_FORMAT_UNDEFINED;
    unsigned destroyed = 0;
};
CreatedPipeline created[6];
unsigned createdCount = 0;
bool expectedStencil = false;
struct Phase {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkImage color = VK_NULL_HANDLE, ds = VK_NULL_HANDLE;
    VkImageView dsView = VK_NULL_HANDLE;
    VkBuffer host = VK_NULL_HANDLE;
    VkPipeline bound = VK_NULL_HANDLE, expectedPipelines[3]{};
    unsigned expectedDraws = 0;
    bool dynamic = false, stencil = false;
    unsigned begins = 0, rpBegins = 0, rpEnds = 0, dynBegins = 0, dynEnds = 0;
    unsigned draws = 0, colorCopies = 0, depthCopies = 0, stencilCopies = 0;
    unsigned submitCalls = 0, submitSuccesses = 0;
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
    check(cb == phase->cb && info && info->colorAttachmentCount == 1 && info->layerCount == 1 && info->viewMask == 0, "Dynamic root/CB");
    if (!info) return;
    check(info->pDepthAttachment && info->pDepthAttachment->imageView == phase->dsView &&
              info->pDepthAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && info->pDepthAttachment->storeOp == VK_ATTACHMENT_STORE_OP_STORE &&
              info->pDepthAttachment->imageLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
              info->pDepthAttachment->clearValue.depthStencil.depth == 1.0F,
          "Dynamic depth node/view/clear/store");
    check((info->pStencilAttachment != nullptr) == phase->stencil, "Dynamic stencil presence");
    if (info->pStencilAttachment) {
        check(info->pStencilAttachment->imageView == phase->dsView && info->pStencilAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR &&
                  info->pStencilAttachment->storeOp == VK_ATTACHMENT_STORE_OP_STORE &&
                  info->pStencilAttachment->imageLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
                  info->pStencilAttachment->clearValue.depthStencil.stencil == 0,
              "Dynamic stencil node/shared view/clear/store");
    }
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
    const bool correctPipeline = phase->draws < phase->expectedDraws &&
                                 phase->expectedPipelines[phase->draws] != VK_NULL_HANDLE &&
                                 phase->expectedPipelines[phase->draws] == phase->bound;
    check(cb == phase->cb && correctPipeline && vertices == 3 && instances == 1 && firstVertex == 0 && firstInstance == 0, "draw CB/exact ordered pipeline/arguments");
    ++phase->draws;
}
VKAPI_ATTR void VKAPI_CALL onCopy(VkCommandBuffer cb, VkImage image, VkImageLayout layout, VkBuffer buffer, uint32_t count, const VkBufferImageCopy *regions) {
    realCopy(cb, image, layout, buffer, count, regions);
    if (!phase) return;
    check(cb == phase->cb && layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && buffer == phase->host && count == 1 && regions, "copy CB/layout/host/count");
    if (!regions || count != 1) return;
    const auto &r = regions[0];
    check(r.imageExtent.width == W && r.imageExtent.height == H && r.imageExtent.depth == 1 &&
              r.imageSubresource.layerCount == 1 && r.bufferRowLength == 0 && r.bufferImageHeight == 0,
          "copy full plane");
    if (r.imageSubresource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT) {
        check(image == phase->color && r.bufferOffset == 0, "color copy identity/offset");
        ++phase->colorCopies;
    } else if (r.imageSubresource.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT) {
        check(image == phase->ds && r.bufferOffset == COLOR_BYTES, "depth copy identity/offset");
        ++phase->depthCopies;
    } else if (r.imageSubresource.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT) {
        check(image == phase->ds && r.bufferOffset == 2 * COLOR_BYTES, "stencil copy identity/offset");
        ++phase->stencilCopies;
    } else
        check(false, "unexpected copy aspect");
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
    check(count == 1 && infos && pipelines && result == VK_SUCCESS && pipelines[0] != VK_NULL_HANDLE && createdCount < 6, "real graphics pipeline creation");
    if (count != 1 || !infos || !pipelines || result != VK_SUCCESS || !pipelines[0] || createdCount >= 6) return result;
    const unsigned testIndex = createdCount / 2;
    const auto *ds = infos[0].pDepthStencilState;
    check(ds && ds->depthTestEnable && ds->depthWriteEnable && ds->depthCompareOp == VK_COMPARE_OP_LESS &&
              bool(ds->stencilTestEnable) == expectedStencil,
          "real pipeline depth/stencil test state");
    if (ds && expectedStencil) {
        for (const auto face : {ds->front, ds->back}) {
            check(face.compareOp == (testIndex == 2 ? VK_COMPARE_OP_EQUAL : VK_COMPARE_OP_ALWAYS) &&
                      face.reference == (testIndex == 2 ? 9U : 7U) &&
                      face.passOp == (testIndex == 0 ? VK_STENCIL_OP_REPLACE : VK_STENCIL_OP_KEEP) &&
                      face.failOp == VK_STENCIL_OP_KEEP && face.depthFailOp == VK_STENCIL_OP_KEEP &&
                      (face.compareMask & 0xffU) == 0xffU && (face.writeMask & 0xffU) == 0xffU,
                  "real pipeline independent stencil operation/reference");
        }
    }
    auto &record = created[createdCount++];
    record.handle = pipelines[0];
    record.dynamic = infos[0].renderPass == VK_NULL_HANDLE;
    if (record.dynamic) {
        const auto *node = static_cast<const VkPipelineRenderingCreateInfo *>(infos[0].pNext);
        check(node && node->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO && !node->pNext &&
                  node->colorAttachmentCount == 1 && node->pColorAttachmentFormats && node->viewMask == 0 &&
                  infos[0].subpass == 0 && infos[0].pMultisampleState->rasterizationSamples == VK_SAMPLE_COUNT_1_BIT,
              "Dynamic pipeline signature chain");
        if (node && node->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO && node->colorAttachmentCount == 1 && node->pColorAttachmentFormats) {
            record.color = node->pColorAttachmentFormats[0];
            record.depth = node->depthAttachmentFormat;
            record.stencil = node->stencilAttachmentFormat;
        }
    } else
        check(!infos[0].pNext && infos[0].subpass == 0, "Classic pipeline root");
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
        vkDestroyPipeline = realDestroy;
    }
};

int run(bool stencil, bool skipNear) {
    expectedStencil = stencil;
    std::unique_ptr<FileUtils> fileUtils(createFileUtils());
    fileUtils->setWritablePath(std::filesystem::current_path().generic_string() + "/");
    std::unique_ptr<Device> device(DeviceManager::create());
    struct DeviceCleanup {
        Device *device;
        ~DeviceCleanup() {
            if (device) device->destroy();
        }
    } deviceCleanup{device.get()};
    if (!device || device->getGfxAPI() != API::VULKAN) {
        check(false, "Cocos Vulkan initialization");
        return 1;
    }
    auto &raw = *CCVKDevice::getInstance();
    auto *context = raw.gpuContext();
    auto *gpu = raw.gpuDevice();
    const bool validation = context->validationEnabled && context->synchronizationValidationEnabled && context->bestPracticesValidationEnabled;
    std::printf("validation + best practices + sync: %s\n", validation ? "ENABLED" : "NOT VERIFIED");
    if (!validation) return 1;
    if (!gpu->capabilities.dynamicRendering.enabled || !gpu->runtimePaths.useDynamicRendering ||
        context->apiVersionInfo.effectiveDeviceApiVersion < VK_API_VERSION_1_3) {
        std::fprintf(stderr, "UNSUPPORTED: requires enabled engine Dynamic policy at effective 1.3\n");
        return 3;
    }
    if (!vkBeginCommandBuffer || !vkCmdBeginRenderPass || !vkCmdEndRenderPass || !vkCmdBeginRendering || !vkCmdEndRendering ||
        !vkCmdBindPipeline || !vkCmdDraw || !vkCmdCopyImageToBuffer || !vkQueueSubmit || !vkCreateGraphicsPipelines || !vkDestroyPipeline) {
        check(false, "required PFNs");
        return 1;
    }
    check(context->validationErrorCount.load() == 0, "clean initialization");
    check(!gpu->runtimePaths.useSubmit2, "legacy submit required for this hook");
    raw.enableAutoBarrier(false);
    const VkFormat dsFormat = stencil ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D32_SFLOAT;
    for (const auto format : {VK_FORMAT_R8G8B8A8_UNORM, dsFormat}) {
        VkImageFormatProperties props{};
        const auto usage = (format == VK_FORMAT_R8G8B8A8_UNORM ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        const VkResult result = vkGetPhysicalDeviceImageFormatProperties(context->physicalDevice, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &props);
        if (result == VK_ERROR_FORMAT_NOT_SUPPORTED) {
            std::fprintf(stderr, "UNSUPPORTED: required format/usage %u\n", unsigned(format));
            return 3;
        }
        check(result == VK_SUCCESS && (props.sampleCounts & VK_SAMPLE_COUNT_1_BIT) && props.maxExtent.width >= W && props.maxExtent.height >= H, "real format/usage/sample/extent query");
    }
    if (failures) return 1;
    Hooks hooks;
    struct MappingGuard {
        CCVKGPUDevice *gpu;
        VkFormat depth, stencil;
        ~MappingGuard() {
            if (gpu) {
                gpu->depthFormat = depth;
                gpu->depthStencilFormat = stencil;
            }
        }
    } mapping{gpu, gpu->depthFormat, gpu->depthStencilFormat};
    gpu->depthFormat = VK_FORMAT_D32_SFLOAT;
    gpu->depthStencilFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
    Objects o;
    o.device = &raw;
    const unsigned pipelineCount = stencil ? 3 : 2;
    const uint32_t hostBytes = 2 * COLOR_BYTES + (stencil ? PIXELS : 0);
    const Format gfxDS = stencil ? Format::DEPTH_STENCIL : Format::DEPTH;
    std::printf("ds-config: format=%u stencil=%u samples=1 pixels=%u hostBytes=%u runtimeDynamic=1\n", unsigned(dsFormat), unsigned(stencil), PIXELS, hostBytes);
    for (unsigned i = 0; i < 2; ++i) {
        TextureInfo texture;
        texture.type = TextureType::TEX2D;
        texture.format = Format::RGBA8;
        texture.width = W;
        texture.height = H;
        texture.usage = TextureUsageBit::COLOR_ATTACHMENT | TextureUsageBit::TRANSFER_SRC;
        o.colors[i] = raw.createTexture(texture);
        texture.format = gfxDS;
        texture.usage = TextureUsageBit::DEPTH_STENCIL_ATTACHMENT | TextureUsageBit::TRANSFER_SRC;
        o.ds[i] = raw.createTexture(texture);
        BufferInfo host;
        host.usage = BufferUsageBit::TRANSFER_DST;
        host.memUsage = MemoryUsageBit::HOST;
        host.size = hostBytes;
        host.stride = 1;
        o.hosts[i] = raw.createBuffer(host);
        check(o.colors[i] && o.ds[i] && o.hosts[i], "texture/host facades");
    }
    GeneralBarrierInfo colorBarrier;
    colorBarrier.nextAccesses = AccessFlagBit::COLOR_ATTACHMENT_WRITE;
    GeneralBarrierInfo dsBarrier;
    dsBarrier.nextAccesses = AccessFlagBit::DEPTH_STENCIL_ATTACHMENT_WRITE;
    RenderPassInfo rp;
    rp.colorAttachments.resize(1);
    rp.colorAttachments[0].format = Format::RGBA8;
    rp.colorAttachments[0].barrier = raw.getGeneralBarrier(colorBarrier);
    rp.depthStencilAttachment.format = gfxDS;
    rp.depthStencilAttachment.barrier = raw.getGeneralBarrier(dsBarrier);
    rp.subpasses.resize(1);
    rp.subpasses[0].colors = {0};
    rp.subpasses[0].depthStencil = 1;
    o.rp = raw.createRenderPass(rp);
    for (unsigned i = 0; i < 2; ++i) {
        FramebufferInfo fb;
        fb.renderPass = o.rp;
        fb.colorTextures = {o.colors[i]};
        fb.depthStencilTexture = o.ds[i];
        o.fbos[i] = raw.createFramebuffer(fb);
    }
    o.layout = raw.createPipelineLayout(PipelineLayoutInfo{});
    o.ia = raw.createInputAssembler(InputAssemblerInfo{});
    SPIRVUtils::getInstance()->destroy();
    SPIRVUtils::getInstance()->initialize(static_cast<int>(VK_VERSION_MINOR(context->apiVersionInfo.effectiveDeviceApiVersion)));
    for (unsigned i = 0; i < pipelineCount; ++i) {
        ShaderInfo shader;
        shader.name = "ds-test-" + std::to_string(i);
        ShaderStage vs;
        vs.stage = ShaderStageFlagBit::VERTEX;
        vs.source = "void main(){vec2 p=vec2(float((gl_VertexIndex<<1)&2),float(gl_VertexIndex&2));gl_Position=vec4(p*2.0-1.0,";
        vs.source += i == 0 ? "0.25" : (i == 1 ? "0.75" : "0.125");
        vs.source += ",1.0);}";
        ShaderStage fs;
        fs.stage = ShaderStageFlagBit::FRAGMENT;
        fs.source = "layout(location=0) out vec4 o;void main(){o=";
        fs.source += i == 0 ? "vec4(1,0,0,1)" : (i == 1 ? "vec4(0,1,0,1)" : "vec4(0,0,1,1)");
        fs.source += ";}";
        shader.stages = {vs, fs};
        o.shaders[i] = raw.createShader(shader);
        PipelineStateInfo pso;
        pso.shader = o.shaders[i];
        pso.pipelineLayout = o.layout;
        pso.renderPass = o.rp;
        pso.rasterizerState.cullMode = CullMode::NONE;
        if (stencil) {
            auto &state = pso.depthStencilState;
            state.stencilTestFront = state.stencilTestBack = 1;
            state.stencilFuncFront = state.stencilFuncBack = i == 2 ? ComparisonFunc::EQUAL : ComparisonFunc::ALWAYS;
            state.stencilRefFront = state.stencilRefBack = i == 2 ? 9 : 7;
            state.stencilPassOpFront = state.stencilPassOpBack = i == 0 ? StencilOp::REPLACE : StencilOp::KEEP;
        }
        o.psos[i] = raw.createPipelineState(pso);
        check(o.psos[i] != nullptr, "PSO facade");
        if (!o.psos[i]) return 1;
        const auto *gp = static_cast<CCVKPipelineState *>(o.psos[i])->gpuPipelineState();
        check(gp && gp->vkPipeline && gp->vkDynamicPipeline && !gp->dynamicPipelineCreationFailed, "real engine dual PSO handles");
        if (createdCount == 2 * (i + 1) && gp) {
            const auto &classic = created[2 * i];
            const auto &dynamic = created[2 * i + 1];
            check(!classic.dynamic && classic.handle == gp->vkPipeline && dynamic.dynamic && dynamic.handle == gp->vkDynamicPipeline &&
                      dynamic.color == VK_FORMAT_R8G8B8A8_UNORM && dynamic.depth == dsFormat && dynamic.stencil == (stencil ? dsFormat : VK_FORMAT_UNDEFINED),
                  "driver output/signature equals saved PSO");
        } else
            check(false, "exactly two driver creates per PSO");
    }
    if (failures) return 1;
    std::printf("ds-pipelines: classic=%u dynamic=%u signatures=OK depthTest=1 depthWrite=1 stencilTest=%u\n",
                pipelineCount, pipelineCount, unsigned(stencil));
    CommandBufferInfo cmdInfo;
    cmdInfo.queue = raw.getQueue();
    o.cmd = raw.createCommandBuffer(cmdInfo);
    check(o.cmd != nullptr, "command buffer");
    if (!o.cmd) return 1;
    Phase phases[2];
    unsigned mismatches[2]{}, difference = 0;
    bool incomplete = false;
    for (unsigned i = 0; i < 2; ++i) {
        if (context->validationErrorCount.load() >= 10) {
            incomplete = true;
            break;
        }
        auto &p = phases[i];
        const unsigned previousFailures = failures;
        p.expectedDraws = pipelineCount - unsigned(skipNear);
        phase = &p;
        p.dynamic = i == 1;
        p.stencil = stencil;
        p.color = static_cast<CCVKTexture *>(o.colors[i])->gpuTexture()->vkImage;
        p.ds = static_cast<CCVKTexture *>(o.ds[i])->gpuTexture()->vkImage;
        p.dsView = static_cast<CCVKTexture *>(o.ds[i])->gpuTextureView()->vkImageView;
        auto *host = static_cast<CCVKBuffer *>(o.hosts[i])->gpuBuffer();
        p.host = host->vkBuffer;
        check(p.color && p.ds && p.dsView && p.host && host->mappedData && host->vmaAllocation, "real phase handles/mapping");
        if (failures != previousFailures) {
            phase = nullptr;
            break;
        }
        for (unsigned j = 0; j < pipelineCount; ++j) {
            if (skipNear && j == 0) continue;
            auto *gp = static_cast<CCVKPipelineState *>(o.psos[j])->gpuPipelineState();
            p.expectedPipelines[j - unsigned(skipNear)] = p.dynamic ? gp->vkDynamicPipeline : gp->vkPipeline;
        }
        o.cmd->begin();
        // Only the Classic control branch temporarily narrows the policy.
        const Color blue{0, 0, 1, 1};
        {
            struct PolicyGuard {
                VulkanRuntimePaths &paths;
                bool saved;
                ~PolicyGuard() { paths.useDynamicRendering = saved; }
            } policy{gpu->runtimePaths, gpu->runtimePaths.useDynamicRendering};
            gpu->runtimePaths.useDynamicRendering = p.dynamic;
            o.cmd->beginRenderPass(o.rp, o.fbos[i], Rect{0, 0, W, H}, &blue, 1.0F, 0, nullptr, 0);
        }
        o.cmd->bindInputAssembler(o.ia);
        DrawInfo draw;
        draw.vertexCount = 3;
        draw.instanceCount = 1;
        for (unsigned j = 0; j < pipelineCount; ++j) {
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
                barrier.srcAccessMask = j == 0 ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                barrier.oldLayout = j == 0 ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = j == 0 ? p.color : p.ds;
                barrier.subresourceRange = {j == 0 ? VK_IMAGE_ASPECT_COLOR_BIT : VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)), 0, 1, 0, 1};
            }
            cmdPipelineBarrier(gpu, cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, images);
            for (unsigned j = 0; j < (stencil ? 3U : 2U); ++j) {
                VkBufferImageCopy region{};
                region.bufferOffset = j * COLOR_BYTES;
                region.imageSubresource = {static_cast<VkImageAspectFlags>(j == 0 ? VK_IMAGE_ASPECT_COLOR_BIT : (j == 1 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT)), 0, 0, 1};
                region.imageExtent = {W, H, 1};
                vkCmdCopyImageToBuffer(cb, j == 0 ? p.color : p.ds, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p.host, 1, &region);
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
        const unsigned expectedDraws = pipelineCount - unsigned(skipNear);
        const bool chain = p.cb && p.begins == 1 && p.rpBegins == unsigned(!p.dynamic) && p.rpEnds == p.rpBegins &&
                           p.dynBegins == unsigned(p.dynamic) && p.dynEnds == p.dynBegins && p.draws == expectedDraws &&
                           p.colorCopies == 1 && p.depthCopies == 1 && p.stencilCopies == unsigned(stencil) &&
                           p.submitCalls == 1 && p.submitSuccesses == 1;
        check(chain, "complete real phase identity chain");
        std::printf("ds-phase: mode=%s cb=%p rp=%u/%u dynamic=%u/%u draws=%u copies=%u/%u/%u submits=%u/%u chain=%u\n",
                    p.dynamic ? "dynamic" : "classic", (void *)p.cb, p.rpBegins, p.rpEnds, p.dynBegins, p.dynEnds,
                    p.draws, p.colorCopies, p.depthCopies, p.stencilCopies, p.submitSuccesses, p.submitCalls, unsigned(chain));
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
        unsigned colorMismatch = 0, depthMismatch = 0, stencilMismatch = 0;
        const uint8_t red[] = {255, 0, 0, 255};
        for (unsigned pixel = 0; pixel < PIXELS; ++pixel) {
            for (unsigned channel = 0; channel < 4; ++channel) colorMismatch += host->mappedData[4 * pixel + channel] != red[channel];
            float depth = 0;
            std::memcpy(&depth, host->mappedData + COLOR_BYTES + 4 * pixel, sizeof(depth));
            depthMismatch += depth != 0.25F;
            if (stencil) stencilMismatch += host->mappedData[2 * COLOR_BYTES + pixel] != 7;
        }
        mismatches[i] = colorMismatch + depthMismatch + stencilMismatch;
        check(mismatches[i] == 0, "red/depth=0.25/stencil=7 golden contents");
        std::printf("ds-readback: mode=%s colorBytes=%u depthPixels=%u stencilPixels=%u colorMismatch=%u depthMismatch=%u stencilMismatch=%u\n",
                    p.dynamic ? "dynamic" : "classic", COLOR_BYTES, PIXELS, stencil ? PIXELS : 0, colorMismatch, depthMismatch, stencilMismatch);
    }
    phase = nullptr;
    if (!incomplete && phases[0].submitSuccesses == 1 && phases[1].submitSuccesses == 1) {
        const auto *a = static_cast<CCVKBuffer *>(o.hosts[0])->gpuBuffer()->mappedData;
        const auto *b = static_cast<CCVKBuffer *>(o.hosts[1])->gpuBuffer()->mappedData;
        for (unsigned i = 0; i < hostBytes; ++i) difference += a[i] != b[i];
        check(difference == 0, "Classic/Dynamic all-plane equality");
        std::printf("ds-compare: bytes=%u differences=%u\n", hostBytes, difference);
    } else {
        incomplete = true;
        std::printf("INCOMPLETE: missing successful two-phase readback\n");
    }
    raw.waitAllFences();
    o.clear();
    o.device = nullptr;
    const unsigned liveErrors = context->validationErrorCount.load();
    gpu->depthFormat = mapping.depth;
    gpu->depthStencilFormat = mapping.stencil;
    mapping.gpu = nullptr;
    // MappingGuard must not access the destroyed device, so reset its ownership.
    device->destroy();
    deviceCleanup.device = nullptr;
    const unsigned postErrors = raw.getLastValidationErrorCount();
    unsigned retired = 0;
    for (unsigned i = 0; i < createdCount; ++i) {
        check(created[i].destroyed == 1, "each real PSO handle retired once");
        retired += created[i].destroyed;
    }
    check(createdCount == 2 * pipelineCount, "exact pipeline create count");
    check(liveErrors == 0 && postErrors == 0, "live/post Validation errors");
    std::printf("ds-retirement: created=%u destroyed=%u\n", createdCount, retired);
    const bool pass = !failures && !incomplete && !liveErrors && !postErrors && !mismatches[0] && !mismatches[1] && !difference;
    std::printf("depth-stencil %s completed: failures=%u liveErrors=%u postErrors=%u mismatches=%u outcome=%s\n",
                stencil ? "depth-stencil" : "depth", failures, liveErrors, postErrors, mismatches[0] + mismatches[1], pass ? "CLEAN" : "FAILED");
    return pass ? 0 : 1;
}
} // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char *name = "depth";
    bool skipNear = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--case") && i + 1 < argc)
            name = argv[++i];
        else if (!std::strcmp(argv[i], "--skip-near"))
            skipNear = true;
        else {
            std::fprintf(stderr, "FAIL: unknown/incomplete argument %s\n", argv[i]);
            return 1;
        }
    }
    if (std::strcmp(name, "depth") && std::strcmp(name, "depth-stencil")) {
        std::fprintf(stderr, "FAIL: unknown case %s\n", name);
        return 1;
    }
    try {
        return run(!std::strcmp(name, "depth-stencil"), skipNear);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: exception %s\n", error.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "FAIL: unexpected exception\n");
        return 1;
    }
}
