// M6-A0 rev2: real-GPU Classic vs Dynamic rendering clear comparison.
// Dynamic path is driven by customCommand calling the REAL vkCmdBeginRendering/
// vkCmdEndRendering, consuming the M5 owning contracts via the unified
// cmdPipelineBarrier entry. Diagnostic cases keep the runtime path OFF.
// --engine-path uses ordinary gfx APIs with the debug Dynamic policy ON.
// Validation counters are read from a LIVE context only; post-destroy counts
// come from the device's cached counter (no use-after-free).

#include "gfx-base/GFXInputAssembler.h"
#include "gfx-base/SPIRVUtils.h"
#include "gfx-vulkan/VKBuffer.h"
#include "gfx-vulkan/VKCommands.h"
#include "gfx-vulkan/VKDevice.h"
#include "gfx-vulkan/VKGPUObjects.h"
#include "gfx-vulkan/VKPipelineRenderingAdapter.h"
#include "gfx-vulkan/VKPipelineState.h"
#include "gfx-vulkan/VKRenderPass.h"
#include "gfx-vulkan/VKRenderingInfoContract.h"
#include "gfx-vulkan/VKRenderingTransitionContract.h"
#include "gfx-vulkan/VKTexture.h"
#include "gfx-vulkan/VKUtils.h"
#include "platform/FileUtils.h"
#include "renderer/GFXDeviceManager.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#if defined(_WIN32)
    #include <windows.h>
#endif

int cocos_main(int, const char **) { return 0; }
void cocos_destroy() {}

// ---- hook state (fixed capacity; overflow is a FAIL, never silent) ----
namespace {
unsigned g_failures = 0;
void check(bool ok, const char *w) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", w);
        ++g_failures;
    }
}
constexpr unsigned CAP = 8;

struct Phase {
    // per-CB evidence chains: only SUCCESS-registered entries
    VkCommandBuffer beginCB[CAP] = {};
    unsigned beginCBCount = 0;    // successful vkBeginCommandBuffer
    unsigned beginRenderPass = 0; // forwarded BeginRenderPass calls
    unsigned endRenderPass = 0;
    unsigned beginRendering = 0; // forwarded BeginRendering calls
    unsigned endRendering = 0;
    VkCommandBuffer renderPassBeginCB[CAP] = {};
    unsigned renderPassBeginCBCount = 0;
    VkCommandBuffer renderPassEndCB[CAP] = {};
    unsigned renderPassEndCBCount = 0;
    VkCommandBuffer renderingBeginCB[CAP] = {};
    unsigned renderingBeginCBCount = 0;
    VkCommandBuffer renderingEndCB[CAP] = {};
    unsigned renderingEndCBCount = 0;
    unsigned copyCalls = 0;
    VkCommandBuffer copyCB[CAP] = {};
    unsigned copyCBCount = 0;
    unsigned bindCalls = 0;
    VkCommandBuffer bindCB[CAP] = {};
    VkPipeline bindPipeline[CAP] = {};
    unsigned bindCBCount = 0;
    unsigned drawCalls = 0;
    VkCommandBuffer drawCB[CAP] = {};
    uint32_t drawVertexCount[CAP] = {};
    uint32_t drawInstanceCount[CAP] = {};
    uint32_t drawFirstVertex[CAP] = {};
    uint32_t drawFirstInstance[CAP] = {};
    VkPipeline drawBoundPipeline[CAP] = {};
    unsigned drawCBCount = 0;
    unsigned submitCalls = 0;
    unsigned submitSuccess = 0;
    VkCommandBuffer submitCB[CAP] = {};
    unsigned submitCBCount = 0;
    bool overflow = false;
};
Phase g_c;

PFN_vkCmdBeginRendering g_realBeginRendering = nullptr;
PFN_vkCmdEndRendering g_realEndRendering = nullptr;
PFN_vkCmdBeginRenderPass g_realBeginRenderPass = nullptr;
PFN_vkCmdEndRenderPass g_realEndRenderPass = nullptr;
PFN_vkCmdCopyImageToBuffer g_realCopyImgToBuf = nullptr;
PFN_vkQueueSubmit g_realSubmit = nullptr;
PFN_vkBeginCommandBuffer g_realBegin = nullptr;

void noteCB(VkCommandBuffer *arr, unsigned &count, VkCommandBuffer cb) {
    if (count < CAP)
        arr[count++] = cb;
    else
        g_c.overflow = true;
}

VKAPI_ATTR void VKAPI_CALL hookBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *i) {
    if (g_realBeginRendering == nullptr || i == nullptr) {
        g_c.overflow = true;
        return;
    }
    g_realBeginRendering(cb, i); // real forward FIRST
    ++g_c.beginRendering;
    noteCB(g_c.renderingBeginCB, g_c.renderingBeginCBCount, cb);
}
VKAPI_ATTR void VKAPI_CALL hookEndRendering(VkCommandBuffer cb) {
    if (g_realEndRendering == nullptr) {
        g_c.overflow = true;
        return;
    }
    g_realEndRendering(cb);
    ++g_c.endRendering;
    noteCB(g_c.renderingEndCB, g_c.renderingEndCBCount, cb);
}
VKAPI_ATTR void VKAPI_CALL hookBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *i, VkSubpassContents c) {
    if (g_realBeginRenderPass == nullptr || i == nullptr) {
        g_c.overflow = true;
        return;
    }
    g_realBeginRenderPass(cb, i, c);
    ++g_c.beginRenderPass;
    noteCB(g_c.renderPassBeginCB, g_c.renderPassBeginCBCount, cb);
}
VKAPI_ATTR void VKAPI_CALL hookEndRenderPass(VkCommandBuffer cb) {
    if (g_realEndRenderPass == nullptr) {
        g_c.overflow = true;
        return;
    }
    g_realEndRenderPass(cb);
    ++g_c.endRenderPass;
    noteCB(g_c.renderPassEndCB, g_c.renderPassEndCBCount, cb);
}
VKAPI_ATTR void VKAPI_CALL hookCopyImageToBuffer(VkCommandBuffer cb, VkImage i, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy *p) {
    if (g_realCopyImgToBuf == nullptr || p == nullptr || n == 0) {
        g_c.overflow = true;
        return;
    }
    g_realCopyImgToBuf(cb, i, l, b, n, p);
    ++g_c.copyCalls;
    noteCB(g_c.copyCB, g_c.copyCBCount, cb);
}
VKAPI_ATTR VkResult VKAPI_CALL hookSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *i, VkFence f) {
    if (g_realSubmit == nullptr || i == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult r = g_realSubmit(q, n, i, f); // forward FIRST
    ++g_c.submitCalls;
    if (r == VK_SUCCESS) {
        ++g_c.submitSuccess;
        for (uint32_t k = 0; k < n; ++k)
            for (uint32_t m = 0; m < i[k].commandBufferCount; ++m)
                noteCB(g_c.submitCB, g_c.submitCBCount, i[k].pCommandBuffers[m]);
    }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL hookBegin(VkCommandBuffer cb, const VkCommandBufferBeginInfo *p) {
    if (g_realBegin == nullptr || p == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult r = g_realBegin(cb, p); // forward FIRST
    if (r == VK_SUCCESS) noteCB(g_c.beginCB, g_c.beginCBCount, cb);
    return r;
}
bool containsCB(const VkCommandBuffer *entries, unsigned count, VkCommandBuffer cb) {
    if (cb == VK_NULL_HANDLE) return false;
    for (unsigned i = 0; i < count; ++i)
        if (entries[i] == cb) return true;
    return false;
}
bool chainOf(VkCommandBuffer cb, bool dynamic) {
    return !g_c.overflow && containsCB(g_c.beginCB, g_c.beginCBCount, cb) &&
           containsCB(g_c.copyCB, g_c.copyCBCount, cb) &&
           containsCB(g_c.submitCB, g_c.submitCBCount, cb) &&
           (dynamic ? containsCB(g_c.renderingBeginCB, g_c.renderingBeginCBCount, cb) &&
                          containsCB(g_c.renderingEndCB, g_c.renderingEndCBCount, cb)
                    : containsCB(g_c.renderPassBeginCB, g_c.renderPassBeginCBCount, cb) &&
                          containsCB(g_c.renderPassEndCB, g_c.renderPassEndCBCount, cb));
}
// M6-A1: pipeline creation + draw hooks
PFN_vkCmdBindPipeline g_realBindPipeline = nullptr;
PFN_vkCmdDraw g_realDraw = nullptr;
PFN_vkCreateGraphicsPipelines g_realCreateGraphicsPipelines = nullptr;
VkDevice g_hookDevice = VK_NULL_HANDLE;
bool g_variantWanted = false;                     // set for draw-compare
cc::gfx::VKRenderingSignature g_variantSignature; // value snapshot (RP slot 0)
VkPipeline g_dynamicPipeline = VK_NULL_HANDLE;    // harness-owned variant
VkResult g_dynamicCreateResult = VK_NOT_READY;
bool g_injectVariantFailure = false; // owned-failure: error + non-NULL output
// per-handle destroy accounting
PFN_vkDestroyPipeline g_realDestroyPipeline = nullptr;
VkPipeline g_destroyedPipelines[CAP] = {};
unsigned g_destroyedPipelineCount = 0;
unsigned destroyCountOf(VkPipeline pl) {
    unsigned n = 0;
    for (unsigned i = 0; i < g_destroyedPipelineCount; ++i)
        if (g_destroyedPipelines[i] == pl) ++n;
    return n;
}
VKAPI_ATTR void VKAPI_CALL hookDestroyPipeline(VkDevice d, VkPipeline p, const VkAllocationCallbacks *a) {
    if (g_realDestroyPipeline == nullptr || p == VK_NULL_HANDLE) {
        g_c.overflow = true;
        return;
    }
    g_realDestroyPipeline(d, p, a); // forward FIRST
    if (g_destroyedPipelineCount < CAP)
        g_destroyedPipelines[g_destroyedPipelineCount++] = p;
    else
        g_c.overflow = true;
}
VkResult g_classicCreateResult = VK_NOT_READY;
VkPipeline g_classicPipelineFromDriver = VK_NULL_HANDLE;
// M7-F4: independent records of real child creation inputs. Fixed capacity,
// owning keys, and noexcept builders keep exceptions inside C++ code.
struct GraphicsKeySources {
    struct Module {
        VkShaderModule handle = VK_NULL_HANDLE;
        uint32_t words[4096]{};
        size_t count = 0;
    } modules[2];
    struct Pass {
        VkRenderPass handle = VK_NULL_HANDLE;
        cc::gfx::VulkanClassicRenderPassKey key;
    } passes[2];
    cc::gfx::VulkanPipelineLayoutKey layout;
    VkPipelineLayout layoutHandle = VK_NULL_HANDLE;
    unsigned moduleCount = 0, passCount = 0, layoutCount = 0;
    bool invalid = false;
} g_keySources;
PFN_vkCreateShaderModule g_realKeyModule = nullptr;
PFN_vkCreatePipelineLayout g_realKeyLayout = nullptr;
PFN_vkCreateRenderPass2 g_realKeyPass = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL hookKeyModule(VkDevice device, const VkShaderModuleCreateInfo *info,
                                             const VkAllocationCallbacks *alloc, VkShaderModule *out) {
    if (!g_realKeyModule || !info || !out) {
        g_keySources.invalid = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const auto result = g_realKeyModule(device, info, alloc, out);
    if (result != VK_SUCCESS || !*out || !info->pCode || info->codeSize % 4 ||
        info->codeSize < 20 || info->codeSize / 4 > 4096 || g_keySources.moduleCount >= 2) {
        g_keySources.invalid = true;
    } else {
        auto &record = g_keySources.modules[g_keySources.moduleCount++];
        record.handle = *out;
        record.count = info->codeSize / 4;
        std::memcpy(record.words, info->pCode, info->codeSize);
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL hookKeyLayout(VkDevice device, const VkPipelineLayoutCreateInfo *info,
                                             const VkAllocationCallbacks *alloc, VkPipelineLayout *out) {
    if (!g_realKeyLayout || !info || !out) {
        g_keySources.invalid = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const auto result = g_realKeyLayout(device, info, alloc, out);
    if (result != VK_SUCCESS || !*out || ++g_keySources.layoutCount != 1 ||
        info->setLayoutCount || info->pushConstantRangeCount ||
        !cc::gfx::VulkanPipelineLayoutKey::build(*info, {}, g_keySources.layout)) {
        g_keySources.invalid = true;
    } else {
        g_keySources.layoutHandle = *out;
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL hookKeyPass(VkDevice device, const VkRenderPassCreateInfo2 *info,
                                           const VkAllocationCallbacks *alloc, VkRenderPass *out) {
    if (!g_realKeyPass || !info || !out) {
        g_keySources.invalid = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const auto result = g_realKeyPass(device, info, alloc, out);
    if (result != VK_SUCCESS || !*out || g_keySources.passCount >= 2) {
        g_keySources.invalid = true;
    } else {
        auto &record = g_keySources.passes[g_keySources.passCount++];
        record.handle = *out;
        if (!cc::gfx::VulkanClassicRenderPassKey::build(*info, record.key)) g_keySources.invalid = true;
    }
    return result;
}

cc::gfx::RenderPass *createObservedPass(cc::gfx::CCVKDevice &device, const cc::gfx::RenderPassInfo &info) {
    struct Restore {
        cc::gfx::CCVKGPUDevice *gpu;
        explicit Restore(cc::gfx::CCVKGPUDevice *value) : gpu(value) {
            g_realKeyPass = gpu->createRenderPass2;
            gpu->createRenderPass2 = hookKeyPass;
        }
        ~Restore() { gpu->createRenderPass2 = g_realKeyPass; }
    } restore(device.gpuDevice());
    return device.createRenderPass(info);
}

bool buildObservedKey(const VkGraphicsPipelineCreateInfo &info, cc::gfx::VulkanGraphicsPipelineKey &out) noexcept {
    if (g_keySources.invalid || g_keySources.moduleCount != 2 || g_keySources.layoutCount != 1 ||
        info.layout != g_keySources.layoutHandle || info.stageCount != 2 || !info.pStages) return false;
    cc::gfx::VulkanShaderStageKey stages[2];
    const cc::gfx::VulkanShaderStageKey *keys[]{&stages[0], &stages[1]};
    for (unsigned i = 0; i < 2; ++i) {
        const GraphicsKeySources::Module *module = nullptr;
        for (const auto &record : g_keySources.modules)
            if (record.handle == info.pStages[i].module) module = &record;
        if (!module || !cc::gfx::VulkanShaderStageKey::build(info.pStages[i], module->words, module->count, stages[i])) return false;
    }
    const cc::gfx::VulkanClassicRenderPassKey *pass = nullptr;
    if (info.renderPass) {
        for (unsigned i = 0; i < g_keySources.passCount; ++i)
            if (g_keySources.passes[i].handle == info.renderPass) pass = &g_keySources.passes[i].key;
        if (!pass) return false;
    }
    return cc::gfx::VulkanGraphicsPipelineKey::build(info, keys, 2, g_keySources.layout, pass, out);
}
uint32_t g_classicCreateSubpass = 0xFFFFFFFFu;
unsigned g_classicCreateCalls = 0; // MAIN Classic creates only
unsigned g_dynamicCreateCalls = 0; // draw-compare hook-built variants
// M6-A2a classification: main Classic / engine-owned Dynamic / eligibility
// (additional Classic such as the LOAD negative). Fixed capacity, no C++
// exceptions across the C boundary, unknown volume is a FAIL.
struct CreateRecord {
    VkResult result = VK_NOT_READY;
    VkPipeline handle = VK_NULL_HANDLE;
    uint32_t subpass = 0xFFFFFFFFu;
    cc::gfx::VulkanGraphicsPipelineKey key;
    bool keyBuilt = false;
};
CreateRecord g_mainClassic;
unsigned g_mainClassicCreates = 0;
CreateRecord g_engineVariant;
unsigned g_engineVariantCreates = 0;
CreateRecord g_eligibility;
unsigned g_eligibilityCreates = 0;
bool g_collectEligibility = false;
VkResult g_engineVariantReturned = VK_NOT_READY;
// owned-case handle snapshots taken while the facade is alive; read AFTER the
// facade is destroyed so no UAF path can touch the GPU object again
VkPipeline g_ownedClassic = VK_NULL_HANDLE;
VkPipeline g_ownedVariant = VK_NULL_HANDLE;
unsigned g_unknownCreateVolume = 0;
VkFormat g_dynamicColorFormat = VK_FORMAT_UNDEFINED;
VkFormat g_dynamicDepthFormat = VK_FORMAT_UNDEFINED;
VkFormat g_dynamicStencilFormat = VK_FORMAT_UNDEFINED;
VkSampleCountFlagBits g_dynamicSamples = VK_SAMPLE_COUNT_1_BIT;
uint32_t g_dynamicViewMask = 0;

VKAPI_ATTR void VKAPI_CALL hookBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline pl) {
    if (g_realBindPipeline == nullptr || pl == VK_NULL_HANDLE) {
        g_c.overflow = true;
        return;
    }
    g_realBindPipeline(cb, bp, pl); // forward FIRST
    ++g_c.bindCalls;
    if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        if (g_c.bindCBCount < CAP) {
            g_c.bindCB[g_c.bindCBCount] = cb;
            g_c.bindPipeline[g_c.bindCBCount] = pl;
            ++g_c.bindCBCount;
        } else
            g_c.overflow = true;
    }
}
VKAPI_ATTR void VKAPI_CALL hookDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
    if (g_realDraw == nullptr) {
        g_c.overflow = true;
        return;
    }
    g_realDraw(cb, vc, ic, fv, fi); // forward FIRST
    ++g_c.drawCalls;
    if (g_c.drawCBCount < CAP) {
        g_c.drawCB[g_c.drawCBCount] = cb;
        g_c.drawVertexCount[g_c.drawCBCount] = vc;
        g_c.drawInstanceCount[g_c.drawCBCount] = ic;
        g_c.drawFirstVertex[g_c.drawCBCount] = fv;
        g_c.drawFirstInstance[g_c.drawCBCount] = fi;
        for (unsigned i = g_c.bindCBCount; i > 0; --i) {
            if (g_c.bindCB[i - 1] == cb) {
                g_c.drawBoundPipeline[g_c.drawCBCount] = g_c.bindPipeline[i - 1];
                break;
            }
        }
        ++g_c.drawCBCount;
    } else
        g_c.overflow = true;
}
VKAPI_ATTR VkResult VKAPI_CALL hookCreateGraphicsPipelines(VkDevice dev, VkPipelineCache cache,
                                                           uint32_t n,
                                                           const VkGraphicsPipelineCreateInfo *infos,
                                                           const VkAllocationCallbacks *alloc,
                                                           VkPipeline *out) {
    if (g_realCreateGraphicsPipelines == nullptr || infos == nullptr || out == nullptr || n != 1)
        return VK_ERROR_INITIALIZATION_FAILED;
    // real forward FIRST: the Classic result/output must never be rewritten
    const VkResult r = g_realCreateGraphicsPipelines(dev, cache, n, infos, alloc, out);
    // classify FIRST by renderPass: Dynamic creates never pollute the Classic
    if (infos[0].renderPass == VK_NULL_HANDLE) {
        // engine-owned Dynamic variant path (M6-A2a)
        if (g_engineVariantCreates == 0) {
            g_engineVariant.result = r;
            g_engineVariant.handle = out[0];
            g_engineVariant.subpass = infos[0].subpass;
            if (r == VK_SUCCESS && out[0]) g_engineVariant.keyBuilt = buildObservedKey(infos[0], g_engineVariant.key);
            const auto *node = static_cast<const VkPipelineRenderingCreateInfo *>(infos[0].pNext);
            const auto *ms = infos[0].pMultisampleState;
            const bool validNode = node && node->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO &&
                                   node->pNext == nullptr && node->colorAttachmentCount == 1 &&
                                   node->pColorAttachmentFormats && ms &&
                                   ms->sType == VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO &&
                                   g_variantSignature.colorFormats.size() == 1;
            check(validNode, "owned variant actual create-info node shape");
            if (validNode) {
                g_dynamicColorFormat = node->pColorAttachmentFormats[0];
                g_dynamicDepthFormat = node->depthAttachmentFormat;
                g_dynamicStencilFormat = node->stencilAttachmentFormat;
                g_dynamicViewMask = node->viewMask;
                g_dynamicSamples = ms->rasterizationSamples;
                check(g_dynamicColorFormat == g_variantSignature.colorFormats[0] &&
                          g_dynamicDepthFormat == g_variantSignature.depthFormat &&
                          g_dynamicStencilFormat == g_variantSignature.stencilFormat &&
                          g_dynamicViewMask == g_variantSignature.viewMask &&
                          g_dynamicSamples == g_variantSignature.samples && infos[0].subpass == 0,
                      "owned variant actual root matches RP signature");
            }
        } else {
            ++g_unknownCreateVolume;
        }
        ++g_engineVariantCreates;
        g_engineVariantReturned = r;
        if (g_injectVariantFailure && r == VK_SUCCESS && out[0] != VK_NULL_HANDLE) {
            // INJECTED failure (not a real driver OOM): return an error while
            // keeping the real non-NULL output so the core must clean it up
            g_engineVariantReturned = VK_ERROR_INITIALIZATION_FAILED;
            return g_engineVariantReturned;
        }
        return r;
    }
    if (g_mainClassicCreates == 0) {
        // the MAIN Classic PSO under test
        g_mainClassic.result = r;
        g_mainClassic.handle = out[0];
        g_mainClassic.subpass = infos[0].subpass;
        if (r == VK_SUCCESS && out[0]) g_mainClassic.keyBuilt = buildObservedKey(infos[0], g_mainClassic.key);
        ++g_mainClassicCreates;
        ++g_classicCreateCalls; // legacy counter: main classic only
        g_classicCreateResult = r;
        g_classicPipelineFromDriver = out[0];
        g_classicCreateSubpass = infos[0].subpass;
    } else if (g_collectEligibility && g_eligibilityCreates == 0) {
        // The explicitly scoped eligibility PSO, never an arbitrary extra create.
        g_eligibility.result = r;
        g_eligibility.handle = out[0];
        g_eligibility.subpass = infos[0].subpass;
        if (r == VK_SUCCESS && out[0]) g_eligibility.keyBuilt = buildObservedKey(infos[0], g_eligibility.key);
        ++g_eligibilityCreates;
    } else {
        ++g_unknownCreateVolume;
        check(false, "unexpected extra Classic driver creation");
        return r;
    }
    // the MAIN classic create may still be followed by the draw-compare
    // hook-built diagnostic variant below
    if (r != VK_SUCCESS || !g_variantWanted || out[0] == VK_NULL_HANDLE) return r;
    if (infos[0].pNext != nullptr) return r; // unknown chain: no variant
    // diagnostic Dynamic variant built INSIDE the callback while engine state
    // pointers are still valid; the owned chain lives through pipeline creation
    try {
        VkGraphicsPipelineCreateInfo variant = infos[0]; // value copy
        cc::gfx::VKPipelinePNextChain chain;
        if (!cc::gfx::applyVKPipelineRenderingContract(
                variant, chain, cc::gfx::VKPipelineRenderingMode::Dynamic, VK_NULL_HANDLE, 0,
                &g_variantSignature, nullptr)) {
            g_dynamicCreateResult = VK_ERROR_VALIDATION_FAILED_EXT;
            return r;
        }
        const auto &rendering = chain.contract().pipelineInfo();
        if (variant.renderPass != VK_NULL_HANDLE || variant.subpass != 0 ||
            variant.pNext != &rendering || rendering.colorAttachmentCount != 1 ||
            rendering.pColorAttachmentFormats == nullptr ||
            g_variantSignature.colorFormats.size() != 1 ||
            rendering.pColorAttachmentFormats[0] != g_variantSignature.colorFormats[0] ||
            rendering.depthAttachmentFormat != g_variantSignature.depthFormat ||
            rendering.stencilAttachmentFormat != g_variantSignature.stencilFormat ||
            rendering.viewMask != g_variantSignature.viewMask ||
            variant.pMultisampleState->rasterizationSamples != g_variantSignature.samples) {
            check(false, "actual Dynamic pipeline root differs from RP signature");
            g_dynamicCreateResult = VK_ERROR_VALIDATION_FAILED_EXT;
            return r;
        }
        g_dynamicColorFormat = rendering.pColorAttachmentFormats[0];
        g_dynamicDepthFormat = rendering.depthAttachmentFormat;
        g_dynamicStencilFormat = rendering.stencilAttachmentFormat;
        g_dynamicViewMask = rendering.viewMask;
        g_dynamicSamples = variant.pMultisampleState->rasterizationSamples;
        ++g_dynamicCreateCalls;
        const VkResult vr = g_realCreateGraphicsPipelines(dev, cache, 1, &variant, alloc,
                                                          &g_dynamicPipeline);
        g_dynamicCreateResult = vr;
    } catch (...) {
        g_dynamicCreateResult = VK_ERROR_OUT_OF_HOST_MEMORY; // explicit FAIL, no rethrow across C
    }
    return r; // Classic result untouched by the diagnostic variant outcome
}
struct HookRAII {
    HookRAII() {
        g_realKeyModule = vkCreateShaderModule;
        vkCreateShaderModule = hookKeyModule;
        g_realKeyLayout = vkCreatePipelineLayout;
        vkCreatePipelineLayout = hookKeyLayout;
        g_realBeginRendering = vkCmdBeginRendering;
        vkCmdBeginRendering = &hookBeginRendering;
        g_realEndRendering = vkCmdEndRendering;
        vkCmdEndRendering = &hookEndRendering;
        g_realBeginRenderPass = vkCmdBeginRenderPass;
        vkCmdBeginRenderPass = &hookBeginRenderPass;
        g_realEndRenderPass = vkCmdEndRenderPass;
        vkCmdEndRenderPass = &hookEndRenderPass;
        g_realCopyImgToBuf = vkCmdCopyImageToBuffer;
        vkCmdCopyImageToBuffer = &hookCopyImageToBuffer;
        g_realSubmit = vkQueueSubmit;
        vkQueueSubmit = &hookSubmit;
        g_realBegin = vkBeginCommandBuffer;
        vkBeginCommandBuffer = &hookBegin;
        g_realBindPipeline = vkCmdBindPipeline;
        vkCmdBindPipeline = &hookBindPipeline;
        g_realDraw = vkCmdDraw;
        vkCmdDraw = &hookDraw;
        g_realCreateGraphicsPipelines = vkCreateGraphicsPipelines;
        vkCreateGraphicsPipelines = &hookCreateGraphicsPipelines;
        g_realDestroyPipeline = vkDestroyPipeline;
        vkDestroyPipeline = &hookDestroyPipeline;
    }
    ~HookRAII() {
        vkCreateShaderModule = g_realKeyModule;
        vkCreatePipelineLayout = g_realKeyLayout;
        vkCmdBeginRendering = g_realBeginRendering;
        vkCmdEndRendering = g_realEndRendering;
        vkCmdBeginRenderPass = g_realBeginRenderPass;
        vkCmdEndRenderPass = g_realEndRenderPass;
        vkCmdCopyImageToBuffer = g_realCopyImgToBuf;
        vkQueueSubmit = g_realSubmit;
        vkBeginCommandBuffer = g_realBegin;
        vkCmdBindPipeline = g_realBindPipeline;
        vkCmdDraw = g_realDraw;
        vkCreateGraphicsPipelines = g_realCreateGraphicsPipelines;
        vkDestroyPipeline = g_realDestroyPipeline;
    }
};
} // namespace

using namespace cc;
using namespace cc::gfx;

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::string caseName = "classic";
    bool skipDraw = false;
    bool enginePath = false;
    bool rejectVariant = false;
    bool rejectSignature = false;
    bool corruptKey = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--case") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "FAIL: --case requires a value\n");
                return 2;
            }
            caseName = argv[++i];
        } else if (std::strcmp(argv[i], "--skip-draw") == 0) {
            skipDraw = true; // negative control: blue clear must FAIL golden
        } else if (std::strcmp(argv[i], "--engine-path") == 0) {
            enginePath = true;
        } else if (std::strcmp(argv[i], "--reject-variant") == 0) {
            rejectVariant = true;
        } else if (std::strcmp(argv[i], "--reject-signature") == 0) {
            rejectSignature = true;
        } else if (std::strcmp(argv[i], "--corrupt-key") == 0) {
            corruptKey = true; // CPU metadata only, leaves Vulkan objects and calls valid.
        } else {
            std::fprintf(stderr, "FAIL: unknown argument '%s'\n", argv[i]);
            return 2;
        }
    }
    if (caseName != "classic" && caseName != "compare" && caseName != "draw-classic" &&
        caseName != "draw-compare" && caseName != "owned-compare" &&
        caseName != "owned-failure") {
        std::fprintf(stderr, "FAIL: unknown case '%s' (classic|compare|draw-classic|draw-compare|owned-compare|owned-failure)\n",
                     caseName.c_str());
        return 2;
    }
    const bool isDraw = caseName == "draw-classic" || caseName == "draw-compare" ||
                        caseName == "owned-compare" || caseName == "owned-failure";
    const bool isCompare = caseName == "compare" || caseName == "draw-compare" ||
                           caseName == "owned-compare";
    const bool isOwned = caseName == "owned-compare" || caseName == "owned-failure";
    if (enginePath && !isOwned) {
        std::fprintf(stderr, "FAIL: --engine-path requires an owned case\n");
        return 2;
    }
    if ((rejectVariant || rejectSignature) &&
        (!enginePath || caseName != "owned-compare" || (rejectVariant && rejectSignature) || skipDraw)) {
        std::fprintf(stderr, "FAIL: draw rejection requires engine owned-compare and one injection\n");
        return 2;
    }
    if (skipDraw && !isDraw) {
        std::fprintf(stderr, "FAIL: --skip-draw only applies to draw cases\n");
        return 2;
    }
    if (corruptKey && (caseName != "owned-compare" || skipDraw || rejectVariant || rejectSignature)) {
        std::fprintf(stderr, "FAIL: --corrupt-key requires an unmodified owned-compare draw\n");
        return 2;
    }

    const uint32_t W = 17, H = 11;
    const uint32_t BYTES = W * H * 4;
    const auto outDir = std::filesystem::current_path() / ("dyn-render-" + caseName);
    std::filesystem::create_directories(outDir);
    std::unique_ptr<FileUtils> fileUtils(createFileUtils());
    fileUtils->setWritablePath(outDir.generic_string() + "/");

    std::unique_ptr<Device> ownedDevice(DeviceManager::create());
    if (!ownedDevice || ownedDevice->getGfxAPI() != API::VULKAN) {
        std::fprintf(stderr, "FAIL: Cocos Vulkan initialization\n");
        if (ownedDevice) ownedDevice->destroy();
        return 1;
    }
    CCVKDevice &raw = *CCVKDevice::getInstance();
    auto *context = raw.gpuContext(); // LIVE while the device is alive
    const bool validation = context->validationEnabled && context->bestPracticesValidationEnabled &&
                            context->synchronizationValidationEnabled;
    std::printf("validation + best practices + sync: %s\n", validation ? "ENABLED" : "NOT VERIFIED");
    if (!validation) {
        ownedDevice->destroy();
        return 1;
    }
    const uint32_t errorsAtStart = context->validationErrorCount.load(std::memory_order_relaxed);
    check(errorsAtStart == 0, "validation counter must start clean");
    raw.enableAutoBarrier(false);

    // evidence: actual runtime path + capability state
    std::printf("runtime: dynamicRendering=%d capsEnabled=%d effective=%u.%u\n",
                raw.gpuDevice()->runtimePaths.useDynamicRendering ? 1 : 0,
                raw.gpuDevice()->capabilities.dynamicRendering.enabled ? 1 : 0,
                VK_VERSION_MAJOR(context->apiVersionInfo.effectiveDeviceApiVersion),
                VK_VERSION_MINOR(context->apiVersionInfo.effectiveDeviceApiVersion));
    check(raw.gpuDevice()->runtimePaths.useDynamicRendering == enginePath,
          "runtime policy must match engine-path request");
    if ((isCompare || isOwned) &&
        (!raw.gpuDevice()->capabilities.dynamicRendering.enabled || vkCmdBeginRendering == nullptr ||
         vkCmdEndRendering == nullptr)) {
        std::fprintf(stderr, "UNSUPPORTED: dynamicRendering not enabled or PFN null\n");
        ownedDevice->destroy();
        return 3;
    }

    HookRAII hooks; // installed after device init (volk loads during init)

    { // real format feature check
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(context->physicalDevice,
                                            mapVkFormat(Format::RGBA8, raw.gpuDevice()), &props);
        check((props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0,
              "RGBA8 color attachment support");
        check((props.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) != 0,
              "RGBA8 transfer src support");
    }

    Texture *texA = nullptr, *texB = nullptr;
    Buffer *hostA = nullptr, *hostB = nullptr;
    RenderPass *rp = nullptr;
    Framebuffer *fbo = nullptr;
    Framebuffer *fboB = nullptr;
    InputAssembler *drawIA = nullptr;
    CommandBuffer *cmd = nullptr;
    cc::gfx::VKRenderingTransitionContract tPre, tPost;
    cc::gfx::VKRenderingInfoContract infoContract;
    unsigned mismatches = 0;

    // ---- shared cleanup: NULL-safe, engine ownership order ----
    const auto cleanup = [&]() {
        if (cmd) {
            cmd->destroy();
            delete cmd;
            cmd = nullptr;
        }
        if (drawIA) {
            drawIA->destroy();
            delete drawIA;
            drawIA = nullptr;
        }
        if (fboB) {
            fboB->destroy();
            delete fboB;
            fboB = nullptr;
        }
        if (hostB) {
            hostB->destroy();
            delete hostB;
            hostB = nullptr;
        }
        if (hostA) {
            hostA->destroy();
            delete hostA;
            hostA = nullptr;
        }
        if (texB) {
            texB->destroy();
            delete texB;
            texB = nullptr;
        }
        if (texA) {
            texA->destroy();
            delete texA;
            texA = nullptr;
        }
        if (fbo) {
            fbo->destroy();
            delete fbo;
            fbo = nullptr;
        }
        if (rp) {
            rp->destroy();
            delete rp;
            rp = nullptr;
        }
    };

    auto makeTexture = [&](const char *name) {
        TextureInfo info;
        info.type = TextureType::TEX2D;
        info.format = Format::RGBA8;
        info.width = W;
        info.height = H;
        info.usage = TextureUsageBit::COLOR_ATTACHMENT | TextureUsageBit::TRANSFER_SRC;
        Texture *t = raw.createTexture(info);
        check(t != nullptr, name);
        if (t) check(static_cast<CCVKTexture *>(t)->gpuTexture() &&
                         static_cast<CCVKTexture *>(t)->gpuTexture()->vkImage != VK_NULL_HANDLE,
                     "real VkImage handle");
        return t;
    };
    texA = makeTexture("texture A (classic)");
    if (isCompare) texB = makeTexture("texture B (dynamic)");
    BufferInfo hostInfo;
    hostInfo.usage = BufferUsageBit::TRANSFER_DST;
    hostInfo.memUsage = MemoryUsageBit::HOST;
    hostInfo.size = BYTES;
    hostInfo.stride = 4;
    hostA = raw.createBuffer(hostInfo);
    if (isCompare) hostB = raw.createBuffer(hostInfo);
    check(hostA && static_cast<CCVKBuffer *>(hostA)->gpuBuffer() &&
              static_cast<CCVKBuffer *>(hostA)->gpuBuffer()->vkBuffer != VK_NULL_HANDLE &&
              static_cast<CCVKBuffer *>(hostA)->gpuBuffer()->mappedData != nullptr,
          "host buffer A mapped");
    if (isCompare)
        check(hostB && static_cast<CCVKBuffer *>(hostB)->gpuBuffer() &&
                  static_cast<CCVKBuffer *>(hostB)->gpuBuffer()->vkBuffer != VK_NULL_HANDLE &&
                  static_cast<CCVKBuffer *>(hostB)->gpuBuffer()->mappedData != nullptr,
              "host buffer B mapped");

    GeneralBarrierInfo colorInfo;
    colorInfo.prevAccesses = AccessFlagBit::NONE;
    colorInfo.nextAccesses = AccessFlagBit::COLOR_ATTACHMENT_WRITE;
    RenderPassInfo rpInfo;
    rpInfo.colorAttachments.resize(1);
    rpInfo.colorAttachments[0].format = Format::RGBA8;
    rpInfo.colorAttachments[0].loadOp = LoadOp::CLEAR;
    rpInfo.colorAttachments[0].storeOp = StoreOp::STORE;
    rpInfo.colorAttachments[0].barrier = raw.getGeneralBarrier(colorInfo);
    rpInfo.subpasses.resize(1);
    rpInfo.subpasses[0].colors = {0};
    if (g_failures == 0) rp = createObservedPass(raw, rpInfo);
    FramebufferInfo fboInfo;
    fboInfo.renderPass = rp;
    fboInfo.colorTextures = {texA};
    if (rp && texA) fbo = raw.createFramebuffer(fboInfo);
    check(rp && fbo, "classic RP/FBO");
    if (enginePath) {
        drawIA = raw.createInputAssembler(InputAssemblerInfo{});
        check(drawIA != nullptr, "engine procedural input assembler");
        if (isCompare && rp && texB) {
            fboInfo.colorTextures = {texB};
            fboB = raw.createFramebuffer(fboInfo);
            check(fboB != nullptr, "engine Dynamic FBO");
        }
    }

    auto *queue = raw.getQueue();
    check(queue != nullptr, "queue");
    CommandBufferInfo cbInfo;
    cbInfo.queue = queue;
    if (queue) cmd = raw.createCommandBuffer(cbInfo);
    check(cmd != nullptr, "command buffer");

    // draw cases clear BLUE and the fragment must overwrite with RED: a
    // missing draw leaves blue and must fail the golden comparison
    const Color clearColor{isDraw ? 0.0f : 1.0f, 0.0f, isDraw ? 1.0f : 0.0f, 1.0f};
    uint8_t golden[BYTES];
    for (uint32_t i = 0; i < W * H; ++i) {
        golden[i * 4 + 0] = 255;
        golden[i * 4 + 1] = 0;
        golden[i * 4 + 2] = 0;
        golden[i * 4 + 3] = 255;
    }

    const auto budgetFuse = [&]() -> bool {
        return (context->validationErrorCount.load(std::memory_order_relaxed) - errorsAtStart) >= 10;
    };
    const auto readbackBarrier = [&](VkCommandBuffer cb, VkBuffer hostBuf) {
        VkBufferMemoryBarrier after{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        after.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        after.srcQueueFamilyIndex = after.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        after.buffer = hostBuf;
        after.size = BYTES;
        cmdPipelineBarrier(raw.gpuDevice(), cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &after, 0, nullptr);
    };
    const auto copyImage = [&](VkCommandBuffer cb, VkImage img, VkBuffer hostBuf) {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {W, H, 1};
        vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hostBuf, 1, &region);
    };

    // short-lived dynamic-path preparation guard: restores the flag on every
    // path including exceptions (RAII), never leaks the policy ON
    struct DynamicPathGuard {
        cc::gfx::VulkanRuntimePaths &paths;
        const bool saved;
        explicit DynamicPathGuard(cc::gfx::VulkanRuntimePaths &p, bool enabled = true)
        : paths(p), saved(p.useDynamicRendering) { p.useDynamicRendering = enabled; }
        ~DynamicPathGuard() { paths.useDynamicRendering = saved; }
    };

    // ---- M6-A1: real Classic PSO + (draw-compare) Dynamic variant ----
    Shader *drawShader = nullptr;
    PipelineLayout *drawLayout = nullptr;
    PipelineState *drawPSO = nullptr;
    const auto cleanupDrawObjects = [&]() {
        if (g_dynamicPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(raw.gpuDevice()->vkDevice, g_dynamicPipeline, nullptr);
            g_dynamicPipeline = VK_NULL_HANDLE;
        }
        if (drawPSO) {
            drawPSO->destroy();
            delete drawPSO;
            drawPSO = nullptr;
        }
        if (drawLayout) {
            drawLayout->destroy();
            delete drawLayout;
            drawLayout = nullptr;
        }
        if (drawShader) {
            drawShader->destroy();
            delete drawShader;
            drawShader = nullptr;
        }
    };
    if (isDraw) {
        // re-target the shared compiler to the effective API (M5-H lesson)
        const int effMinor = static_cast<int>(
            VK_VERSION_MINOR(context->apiVersionInfo.effectiveDeviceApiVersion));
        const int target = effMinor < 1 ? 1 : (effMinor > 3 ? 3 : effMinor);
        SPIRVUtils::getInstance()->destroy();
        SPIRVUtils::getInstance()->initialize(target);
        ShaderInfo shaderInfo;
        shaderInfo.name = "m6a1-procedural-tri";
        ShaderStage vsStage;
        vsStage.stage = ShaderStageFlagBit::VERTEX;
        vsStage.source =
            "void main(){vec2 p=vec2(float((gl_VertexIndex<<1)&2),"
            "float(gl_VertexIndex&2));gl_Position=vec4(p*2.0-1.0,0.0,1.0);}";
        ShaderStage fsStage;
        fsStage.stage = ShaderStageFlagBit::FRAGMENT;
        fsStage.source = "layout(location=0) out vec4 o;void main(){o=vec4(1.0,0.0,0.0,1.0);}";
        shaderInfo.stages = {vsStage, fsStage};
        drawShader = raw.createShader(shaderInfo);
        drawLayout = raw.createPipelineLayout(PipelineLayoutInfo{});
        check(drawShader && drawLayout, "real shader/layout");
        PipelineStateInfo psoInfo;
        psoInfo.shader = drawShader;
        psoInfo.pipelineLayout = drawLayout;
        psoInfo.renderPass = rp;
        psoInfo.subpass = 0;
        psoInfo.bindPoint = PipelineBindPoint::GRAPHICS;
        psoInfo.depthStencilState.depthTest = 0;
        psoInfo.depthStencilState.depthWrite = 0;
        psoInfo.rasterizerState.cullMode = CullMode::NONE;
        // the engine pipeline already carries dynamic viewport/scissor states
        if (isCompare || isOwned) {
            // signature for the diagnostic variant: the real RP slot tuple
            const auto &rpMeta =
                static_cast<CCVKRenderPass *>(rp)->gpuRenderPass()->renderingMetadata;
            check(rpMeta.size() == 1 && rpMeta[0].valid, "RP metadata for variant signature");
            if (rpMeta.size() == 1 && rpMeta[0].valid) {
                g_variantSignature = rpMeta[0].signature; // value copy
                g_variantWanted = isCompare && !isOwned;  // only diagnostic hook creates a variant
            }
        }
        if (isOwned && caseName == "owned-failure") g_injectVariantFailure = true;
        if (drawShader && drawLayout && rp) {
            if (isOwned) {
                DynamicPathGuard guard(raw.gpuDevice()->runtimePaths);
                drawPSO = raw.createPipelineState(psoInfo);
            } else {
                drawPSO = raw.createPipelineState(psoInfo);
            }
        }
        g_injectVariantFailure = false;
        check(raw.gpuDevice()->runtimePaths.useDynamicRendering == enginePath,
              "runtime policy restored after factory call");
        check(drawPSO != nullptr, "real Classic PSO");
        if (drawPSO) {
            auto *snapGpu = static_cast<CCVKPipelineState *>(drawPSO)->gpuPipelineState();
            if (snapGpu) {
                g_ownedClassic = snapGpu->vkPipeline;
                g_ownedVariant = snapGpu->vkDynamicPipeline;
                if (corruptKey) snapGpu->dynamicContentKey = {};
            }
            const auto matches = [](const cc::gfx::VulkanGraphicsPipelineKey &stored, const CreateRecord &record) {
                if (!record.keyBuilt || record.result != VK_SUCCESS || !record.handle || !stored.valid() ||
                    stored != record.key || stored.hash() != record.key.hash() || stored.words().empty() ||
                    stored.words().data() == record.key.words().data() || stored.stages().size() != 2 || record.key.stages().size() != 2) return false;
                for (unsigned i = 0; i < 2; ++i)
                    if (stored.stages()[i].spirvWords().data() == record.key.stages()[i].spirvWords().data()) return false;
                if (stored.dynamicRendering() && (stored.renderingSignature().colorFormats.empty() ||
                                                  stored.renderingSignature().colorFormats.data() == record.key.renderingSignature().colorFormats.data())) return false;
                return true;
            };
            bool keysOk = snapGpu && matches(snapGpu->graphicsContentKey, g_mainClassic);
            check(keysOk, "actual Classic graphics key and owning input payload");
            if (isOwned && caseName == "owned-compare") {
                keysOk = keysOk && snapGpu && matches(snapGpu->dynamicContentKey, g_engineVariant) &&
                         snapGpu->dynamicContentKey.dynamicRendering() &&
                         snapGpu->graphicsContentKey != snapGpu->dynamicContentKey;
            } else {
                keysOk = keysOk && snapGpu && snapGpu->dynamicContentKey == cc::gfx::VulkanGraphicsPipelineKey{};
            }
            check(keysOk, "actual Dynamic key publication or complete empty fallback");
            if (isOwned && keysOk) {
                std::printf("pipeline-keys: classic=EXACT dynamic=%s ownership=OK\n",
                            caseName == "owned-compare" ? "EXACT" : "EMPTY");
            }
        }
        if (!isOwned && !isCompare && drawPSO) {
            // policy OFF baseline: no variant, no failure flag, no engine
            // Dynamic create was even attempted
            auto *base = static_cast<CCVKPipelineState *>(drawPSO)->gpuPipelineState();
            check(base && base->vkDynamicPipeline == VK_NULL_HANDLE &&
                      !base->dynamicPipelineCreationFailed,
                  "policy OFF baseline: variant NULL, no failure flag");
            check(g_engineVariantCreates == 0, "policy OFF baseline: no engine Dynamic create");
            std::printf("pipeline-baseline: policy=OFF variant=NULL creates=0\n");
        }
        if (drawPSO) {
            auto *gpuPSO = static_cast<CCVKPipelineState *>(drawPSO)->gpuPipelineState();
            check(gpuPSO && gpuPSO->vkPipeline != VK_NULL_HANDLE, "Classic VkPipeline handle");
            check(g_classicCreateResult == VK_SUCCESS, "classic driver create SUCCESS");
            check(g_classicCreateCalls == 1, "exactly one actual Classic driver creation");
            check(gpuPSO && gpuPSO->vkPipeline == g_classicPipelineFromDriver,
                  "engine PSO handle equals driver output");
            check(g_classicCreateSubpass == 0, "classic create subpass 0");
            std::printf("pipeline-classic: result=%d handle=%p subpass=%u\n",
                        (int)g_classicCreateResult, (void *)g_classicPipelineFromDriver,
                        g_classicCreateSubpass);
        }
        {
            auto *ownedPSO = drawPSO ? static_cast<CCVKPipelineState *>(drawPSO)->gpuPipelineState()
                                     : nullptr;
            if (isOwned && ownedPSO) {
                const VkPipeline engClassic = ownedPSO->vkPipeline;
                const VkPipeline engDyn = ownedPSO->vkDynamicPipeline;
                const bool failedFlag = ownedPSO->dynamicPipelineCreationFailed;
                if (caseName == "owned-compare") {
                    check(g_engineVariantCreates == 1, "exactly one engine variant create");
                    check(g_engineVariant.result == VK_SUCCESS && engDyn != VK_NULL_HANDLE,
                          "engine-owned variant created");
                    check(engDyn == g_engineVariant.handle, "variant handle equals driver output");
                    check(engDyn != engClassic && !failedFlag, "distinct handles, no failure flag");
                    std::printf("pipeline-owned: classic=%p dynamic=%p failed=0\n",
                                (void *)engClassic, (void *)engDyn);
                    std::printf("pipeline-dynamic: result=%d handle=%p colors=%u samples=%u\n",
                                (int)g_engineVariant.result, (void *)engDyn,
                                (unsigned)ownedPSO->renderingMetadata.signature.colorFormats.size(),
                                (unsigned)ownedPSO->renderingMetadata.signature.samples);
                    std::printf(
                        "pipeline-signature: color=%u depth=%u stencil=%u samples=%u "
                        "viewMask=%u rpNull=1 subpass=0\n",
                        (unsigned)g_dynamicColorFormat, (unsigned)g_dynamicDepthFormat,
                        (unsigned)g_dynamicStencilFormat, (unsigned)g_dynamicSamples,
                        g_dynamicViewMask);
                } else { // owned-failure: injected error, partial output kept
                    check(g_engineVariantCreates == 1, "one engine variant create attempt");
                    check(g_engineVariant.result == VK_SUCCESS &&
                              g_engineVariant.handle != VK_NULL_HANDLE,
                          "driver really created the partial variant first");
                    check(engDyn == VK_NULL_HANDLE && failedFlag,
                          "injected failure: variant NULL + failure flag");
                    check(destroyCountOf(g_engineVariant.handle) == 1,
                          "partial variant destroyed exactly once");
                    check(engClassic == g_classicPipelineFromDriver, "Classic handle intact");
                    check(ownedPSO->gpuRenderPass->dynamicPipelineCreationFailed &&
                              g_engineVariantReturned == VK_ERROR_INITIALIZATION_FAILED,
                          "injected failure also marks RP fallback");
                    std::printf(
                        "pipeline-owned: classic=%p dynamic=NULL failed=1 partialDestroyed=1\n",
                        (void *)engClassic);
                    std::printf("pipeline-failure: driverResult=%d returned=%d partial=%p partialDestroyed=%u rpFallback=1\n",
                                (int)g_engineVariant.result, (int)g_engineVariantReturned,
                                (void *)g_engineVariant.handle, destroyCountOf(g_engineVariant.handle));
                }
            }
            if (isOwned && caseName == "owned-failure") {
                // eligibility negative: DISCARD-op RP is not a candidate; with the
                // policy ON the factory must not even attempt a Dynamic create
                RenderPassInfo loadRpInfo = rpInfo;
                loadRpInfo.colorAttachments[0].loadOp = LoadOp::DISCARD;
                RenderPass *loadRp = createObservedPass(raw, loadRpInfo);
                check(loadRp != nullptr, "DISCARD RP created");
                PipelineStateInfo loadPso = psoInfo;
                loadPso.renderPass = loadRp;
                PipelineState *loadPSO = nullptr;
                const unsigned variantsBefore = g_engineVariantCreates;
                {
                    DynamicPathGuard guard(raw.gpuDevice()->runtimePaths);
                    g_collectEligibility = true;
                    if (loadRp) loadPSO = raw.createPipelineState(loadPso);
                    g_collectEligibility = false;
                }
                check(loadPSO != nullptr, "DISCARD PSO created");
                auto *loadGpu =
                    loadPSO ? static_cast<CCVKPipelineState *>(loadPSO)->gpuPipelineState() : nullptr;
                check(loadGpu && loadGpu->vkDynamicPipeline == VK_NULL_HANDLE &&
                          !loadGpu->dynamicPipelineCreationFailed,
                      "non-candidate RP: no variant, no failure flag");
                check(g_engineVariantCreates == variantsBefore,
                      "non-candidate RP: no Dynamic create attempted");
                check(loadGpu && loadGpu->gpuRenderPass &&
                          !loadGpu->gpuRenderPass->dynamicPipelineCreationFailed &&
                          !vkIsDynamicRenderPassCandidate(loadGpu->gpuRenderPass.get()) &&
                          g_eligibility.result == VK_SUCCESS &&
                          loadGpu->vkPipeline == g_eligibility.handle &&
                          loadGpu->vkPipeline != VK_NULL_HANDLE,
                      "independent non-candidate RP has a real successful Classic pipeline");
                std::printf("pipeline-eligibility: discardRp variantCreates=%u variant=NULL result=%d handle=%p candidate=0 policy=ON\n",
                            g_engineVariantCreates - variantsBefore, (int)g_eligibility.result,
                            (void *)g_eligibility.handle);
                if (loadPSO) {
                    loadPSO->destroy();
                    delete loadPSO;
                    loadPSO = nullptr;
                }
                if (loadRp) {
                    loadRp->destroy();
                    delete loadRp;
                    loadRp = nullptr;
                }
            }
            if (isCompare && !isOwned) {
                check(g_dynamicCreateCalls == 1, "exactly one actual Dynamic driver creation");
                check(g_dynamicCreateResult == VK_SUCCESS && g_dynamicPipeline != VK_NULL_HANDLE,
                      "diagnostic Dynamic variant created");
                if (g_dynamicCreateResult == VK_SUCCESS && g_dynamicPipeline != VK_NULL_HANDLE)
                    std::printf("pipeline-dynamic: result=%d handle=%p colors=%u samples=%u\n",
                                (int)g_dynamicCreateResult, (void *)g_dynamicPipeline,
                                (unsigned)g_variantSignature.colorFormats.size(),
                                (unsigned)g_variantSignature.samples);
                std::printf("pipeline-signature: color=%u depth=%u stencil=%u samples=%u viewMask=%u rpNull=1 subpass=0\n",
                            (unsigned)g_dynamicColorFormat, (unsigned)g_dynamicDepthFormat,
                            (unsigned)g_dynamicStencilFormat, (unsigned)g_dynamicSamples,
                            g_dynamicViewMask);
            } else {
                check(g_dynamicPipeline == VK_NULL_HANDLE, "draw-classic must not create a variant");
            }
            g_variantWanted = false; // no further engine PSO creates a variant
        }

    } // if (isDraw)

    bool classicPhase = false, dynamicPhase = false;

    if (g_failures == 0 && !budgetFuse()) {
        // ---- Classic branch: engine beginRenderPass + explicit transitions ----
        const VkImage imgA = static_cast<CCVKTexture *>(texA)->gpuTexture()->vkImage;
        const VkBuffer hostBufA = static_cast<CCVKBuffer *>(hostA)->gpuBuffer()->vkBuffer;
        cmd->begin();
        Rect area{0, 0, W, H};
        if (enginePath && isCompare) {
            DynamicPathGuard classicControl(raw.gpuDevice()->runtimePaths, false);
            cmd->beginRenderPass(rp, fbo, area, &clearColor, 1.0F, 0U, nullptr, 0);
        } else {
            // owned-failure leaves policy ON: RP failure must select Classic.
            cmd->beginRenderPass(rp, fbo, area, &clearColor, 1.0F, 0U, nullptr, 0);
        }
        if (isOwned && drawPSO) {
            // real engine recording lease: bindPipelineState retains the whole
            // GPU PSO for the pending recording (M3 recording-owning lease)
            cmd->bindPipelineState(drawPSO);
        }
        if (isDraw && !skipDraw && drawPSO) {
            const VkPipeline classicPipeline =
                static_cast<CCVKPipelineState *>(drawPSO)->gpuPipelineState()->vkPipeline;
            check(classicPipeline != VK_NULL_HANDLE, "classic engine handle for draw");
            if (enginePath) {
                cmd->bindInputAssembler(drawIA);
                DrawInfo draw;
                draw.vertexCount = 3;
                draw.instanceCount = 1;
                cmd->draw(draw);
            } else
                cmd->customCommand([&](void *native) {
                    const VkCommandBuffer cb = static_cast<VkCommandBuffer>(native);
                    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, classicPipeline);
                    const VkViewport vp{0.0f, 0.0f, (float)W, (float)H, 0.0f, 1.0f};
                    vkCmdSetViewport(cb, 0, 1, &vp);
                    const VkRect2D sc{{0, 0}, {W, H}};
                    vkCmdSetScissor(cb, 0, 1, &sc);
                    vkCmdDraw(cb, 3, 1, 0, 0);
                });
        }
        cmd->endRenderPass();
        cmd->customCommand([&](void *native) {
            const VkCommandBuffer cb = static_cast<VkCommandBuffer>(native);
            cc::gfx::VKRenderingTransitionContract::Input tin;
            tin.image = imgA;
            tin.range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            tin.before = {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
            tin.during = {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_ACCESS_TRANSFER_READ_BIT};
            tin.after = tin.during;
            cc::gfx::VKRenderingTransitionContract tc;
            if (!tc.build(tin)) {
                ++g_failures;
                return;
            } // no Vulkan calls on failure
            cmdPipelineBarrier(raw.gpuDevice(), cb, tc.pre().srcStages, tc.pre().dstStages,
                               tc.pre().flags, 0, nullptr, 0, nullptr, 1, &tc.pre().barrier);
            copyImage(cb, imgA, hostBufA);
            readbackBarrier(cb, hostBufA);
        });
        cmd->end();
        if (isOwned && drawPSO && !isCompare) {
            // Rebegin with an empty recording to release the facade's cached
            // PSO. The earlier pending recording must be the owning protection.
            const VkCommandBuffer pendingCB = g_c.copyCBCount ? g_c.copyCB[g_c.copyCBCount - 1] : VK_NULL_HANDLE;
            cmd->begin();
            cmd->end();
            const VkCommandBuffer freshCB = g_c.beginCBCount ? g_c.beginCB[g_c.beginCBCount - 1] : VK_NULL_HANDLE;
            check(pendingCB != VK_NULL_HANDLE && freshCB != VK_NULL_HANDLE && freshCB != pendingCB,
                  "fresh empty recording releases cached PSO without replacing pending CB");
            std::printf("pending-cache: pending=%p fresh=%p distinct=%d\n",
                        (void *)pendingCB, (void *)freshCB, freshCB != pendingCB ? 1 : 0);
            // owned-failure: facade dies after the LAST end(), before the
            // submit; the pending recording lease must keep the classic handle
            drawPSO->destroy();
            delete drawPSO;
            drawPSO = nullptr;
            check(destroyCountOf(g_ownedClassic) == 0, "pending lease keeps classic alive");
            std::printf(
                "pending-owned: facade=destroyed classicDestroyed=%u variantDestroyed=%u "
                "partialDestroyed=%u\n",
                destroyCountOf(g_ownedClassic), destroyCountOf(g_ownedVariant),
                caseName == "owned-failure" ? destroyCountOf(g_engineVariant.handle) : 0);
        }
        if (g_failures == 0 && !budgetFuse()) {
            queue->submit(&cmd, 1);
            raw.waitAllFences();
            classicPhase = g_c.submitSuccess == 1 && g_c.copyCBCount == 1 &&
                           chainOf(g_c.copyCB[0], false);
            std::printf("diag-classic: ok=%d submitOK=%u copyCBs=%u chain=%d\n",
                        classicPhase ? 1 : 0, g_c.submitSuccess, g_c.copyCBCount,
                        (g_c.copyCBCount >= 1 && chainOf(g_c.copyCB[0], false)) ? 1 : 0);
            check(classicPhase, "classic CB identity and successful submission before readback");
        }
    }

    if (isCompare && g_failures == 0 && !budgetFuse()) {
        // ---- Dynamic branch: REAL vkCmdBeginRendering via customCommand ----
        const VkImage imgB = static_cast<CCVKTexture *>(texB)->gpuTexture()->vkImage;
        const auto *gpuViewB = static_cast<CCVKTexture *>(texB)->gpuTextureView();
        const VkImageView viewB = gpuViewB ? gpuViewB->vkImageView : VK_NULL_HANDLE;
        const VkBuffer hostBufB = static_cast<CCVKBuffer *>(hostB)->gpuBuffer()->vkBuffer;
        check(viewB != VK_NULL_HANDLE, "real VkImageView handle");
        const VkFormat vkFmt = mapVkFormat(Format::RGBA8, raw.gpuDevice());
        cmd->begin();
        if (isOwned && drawPSO) cmd->bindPipelineState(drawPSO); // this recording's lease
        if (enginePath) {
            // Pre-pass binding selects Classic. Drawing must rebind the same
            // GPU PSO's Dynamic handle after the pass mode changes.
            const Rect area{0, 0, W, H};
            cmd->beginRenderPass(rp, fboB, area, &clearColor, 1.0F, 0U, nullptr, 0);
            cmd->bindInputAssembler(drawIA);
            if (!skipDraw) {
                DrawInfo draw;
                draw.vertexCount = 3;
                draw.instanceCount = 1;
                auto *gpuPSO = static_cast<CCVKPipelineState *>(drawPSO)->gpuPipelineState();
                struct DrawGuardInjection {
                    CCVKGPUPipelineState *pso;
                    VkPipeline savedPipeline;
                    VkFormat savedFormat;
                    ~DrawGuardInjection() {
                        pso->vkDynamicPipeline = savedPipeline;
                        pso->renderingMetadata.signature.colorFormats[0] = savedFormat;
                    }
                } restore{gpuPSO, gpuPSO->vkDynamicPipeline, gpuPSO->renderingMetadata.signature.colorFormats[0]};
                const unsigned beforeDraws = g_c.drawCalls;
                // Inject only after BeginRendering: these must reject the draw,
                // preserve the real handles, and still end/submit the instance.
                if (rejectVariant) gpuPSO->vkDynamicPipeline = VK_NULL_HANDLE;
                if (rejectSignature) gpuPSO->renderingMetadata.signature.colorFormats[0] = VK_FORMAT_UNDEFINED;
                cmd->draw(draw);
                if (rejectVariant || rejectSignature) {
                    check(g_c.drawCalls == beforeDraws, "incompatible Dynamic draw never forwarded");
                    std::printf("draw-rejection: kind=%s forwarded=%u\n",
                                rejectVariant ? "variant" : "signature", g_c.drawCalls - beforeDraws);
                }
            }
            cmd->endRenderPass();
            cmd->customCommand([&](void *native) {
                const VkCommandBuffer cb = static_cast<VkCommandBuffer>(native);
                VKRenderingTransitionContract::Input tin;
                tin.image = imgB;
                tin.range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                tin.before = {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
                tin.during = {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT};
                tin.after = tin.during;
                VKRenderingTransitionContract copyTransition;
                if (!copyTransition.build(tin)) {
                    check(false, "engine path readback transition");
                    return;
                }
                const auto &edge = copyTransition.pre();
                cmdPipelineBarrier(raw.gpuDevice(), cb, edge.srcStages, edge.dstStages, edge.flags,
                                   0, nullptr, 0, nullptr, 1, &edge.barrier);
                copyImage(cb, imgB, hostBufB);
                readbackBarrier(cb, hostBufB);
            });
        } else
            cmd->customCommand([&](void *native) {
                if (g_failures != 0) return;
                const VkCommandBuffer cb = static_cast<VkCommandBuffer>(native);
                cc::gfx::VKRenderingTransitionContract::Input tin;
                tin.image = imgB;
                tin.range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                // fresh engine texture: never rendered; LOAD_CLEAR discards - the
                // real engine discard semantics for a first-use attachment
                tin.before = {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0};
                tin.during = {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                              VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
                tin.after = {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT};
                if (!tPre.build(tin) || !tPost.build(tin)) {
                    ++g_failures;
                    return;
                }
                cmdPipelineBarrier(raw.gpuDevice(), cb, tPre.pre().srcStages, tPre.pre().dstStages,
                                   tPre.pre().flags, 0, nullptr, 0, nullptr, 1, &tPre.pre().barrier);
                cc::gfx::VKRenderingInfoContract::Input in;
                in.signature.colorFormats = {vkFmt};
                in.signature.samples = VK_SAMPLE_COUNT_1_BIT;
                in.colors.resize(1);
                in.colors[0].description.format = vkFmt;
                in.colors[0].description.samples = VK_SAMPLE_COUNT_1_BIT;
                in.colors[0].description.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                in.colors[0].description.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                in.colors[0].binding.imageView = viewB;
                in.colors[0].binding.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                in.colors[0].clear.color.float32[0] = isDraw ? 0.0f : 1.0f;
                in.colors[0].clear.color.float32[1] = 0.0f;
                in.colors[0].clear.color.float32[2] = isDraw ? 1.0f : 0.0f;
                in.colors[0].clear.color.float32[3] = 1.0f;
                in.layerCount = 1;
                in.renderArea = {{0, 0}, {W, H}};
                if (!infoContract.build(in)) {
                    ++g_failures;
                    return;
                }
                vkCmdBeginRendering(cb, &infoContract.info());
                const VkPipeline dynBind = isOwned ? g_ownedVariant : g_dynamicPipeline;
                if (isDraw && !skipDraw && dynBind != VK_NULL_HANDLE) {
                    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, dynBind);
                    const VkViewport vp{0.0f, 0.0f, (float)W, (float)H, 0.0f, 1.0f};
                    vkCmdSetViewport(cb, 0, 1, &vp);
                    const VkRect2D sc{{0, 0}, {W, H}};
                    vkCmdSetScissor(cb, 0, 1, &sc);
                    vkCmdDraw(cb, 3, 1, 0, 0);
                }
                vkCmdEndRendering(cb);
                cmdPipelineBarrier(raw.gpuDevice(), cb, tPost.post().srcStages, tPost.post().dstStages,
                                   tPost.post().flags, 0, nullptr, 0, nullptr, 1, &tPost.post().barrier);
                copyImage(cb, imgB, hostBufB);
                readbackBarrier(cb, hostBufB);
            });
        cmd->end();
        if (isOwned && drawPSO) {
            const VkCommandBuffer pendingCB = g_c.copyCBCount ? g_c.copyCB[g_c.copyCBCount - 1] : VK_NULL_HANDLE;
            cmd->begin();
            cmd->end();
            const VkCommandBuffer freshCB = g_c.beginCBCount ? g_c.beginCB[g_c.beginCBCount - 1] : VK_NULL_HANDLE;
            check(pendingCB != VK_NULL_HANDLE && freshCB != VK_NULL_HANDLE && freshCB != pendingCB,
                  "fresh empty recording releases cached PSO without replacing pending CB");
            std::printf("pending-cache: pending=%p fresh=%p distinct=%d\n",
                        (void *)pendingCB, (void *)freshCB, freshCB != pendingCB ? 1 : 0);
            // owned-compare: facade dies after the LAST end(), before submit
            drawPSO->destroy();
            delete drawPSO;
            drawPSO = nullptr;
            check(destroyCountOf(g_ownedClassic) == 0, "pending lease keeps classic alive (dyn)");
            check(destroyCountOf(g_ownedVariant) == 0, "pending lease keeps variant alive (dyn)");
            std::printf(
                "pending-owned: facade=destroyed classicDestroyed=0 variantDestroyed=0 "
                "partialDestroyed=0\n");
        }
        if (g_failures == 0 && !budgetFuse()) {
            queue->submit(&cmd, 1);
            raw.waitAllFences();
            dynamicPhase = g_c.submitSuccess == 2 && g_c.copyCBCount == 2 &&
                           chainOf(g_c.copyCB[1], true);
            std::printf("diag-dynamic: ok=%d submitOK=%u copyCBs=%u chain=%d\n",
                        dynamicPhase ? 1 : 0, g_c.submitSuccess, g_c.copyCBCount,
                        (g_c.copyCBCount >= 2 && chainOf(g_c.copyCB[1], true)) ? 1 : 0);
            check(dynamicPhase, "dynamic CB identity and successful submission before readback");
        }
    }

    // ---- readback ONLY after real submit success + completion ----
    if (g_failures == 0 && !budgetFuse() && classicPhase &&
        (!isCompare || dynamicPhase)) {
        const auto invalidateHost = [&](Buffer *b, const char *name) -> bool {
            auto *gpu = static_cast<CCVKBuffer *>(b)->gpuBuffer();
            if (!gpu->mappedData || !gpu->vmaAllocation) {
                check(false, name);
                return false;
            }
            const VkResult r = vmaInvalidateAllocation(raw.gpuDevice()->memoryAllocator,
                                                       gpu->vmaAllocation, 0, VK_WHOLE_SIZE);
            if (r != VK_SUCCESS) {
                check(false, name);
                return false;
            }
            return true;
        };
        if (classicPhase && invalidateHost(hostA, "invalidate A")) {
            const uint8_t *d = static_cast<CCVKBuffer *>(hostA)->gpuBuffer()->mappedData;
            for (uint32_t i = 0; i < BYTES; ++i)
                if (d[i] != golden[i]) ++mismatches;
        }
        if (dynamicPhase && invalidateHost(hostB, "invalidate B")) {
            const uint8_t *d = static_cast<CCVKBuffer *>(hostB)->gpuBuffer()->mappedData;
            for (uint32_t i = 0; i < BYTES; ++i)
                if (d[i] != golden[i]) ++mismatches;
            if (classicPhase) {
                unsigned diff = 0;
                const uint8_t *da = static_cast<CCVKBuffer *>(hostA)->gpuBuffer()->mappedData;
                for (uint32_t i = 0; i < BYTES; ++i)
                    if (da[i] != d[i]) ++diff;
                std::printf("compare: classicVsDynamic diff bytes=%u/%u\n", diff, BYTES);
                check(diff == 0, "classic and dynamic results identical");
            }
        }
        std::printf("%s: size=%ux%u totalBytes=%u mismatches=%u\n", caseName.c_str(), W, H, BYTES,
                    mismatches);
        std::printf("diag: binds=%u draws=%u copyCBs=%u beginCBs=%u submitOK=%u\n", g_c.bindCalls, g_c.drawCalls, g_c.copyCBCount, g_c.beginCBCount, g_c.submitSuccess);
        check(mismatches == 0, "clear matches golden bytes");
    }

    // ---- per-phase CB identity chains (only if both phases really ran) ----
    if (g_failures == 0 && classicPhase && (!isCompare || dynamicPhase)) {
        // classic evidence: the first copy CB must be a fully successful chain
        const bool classicChainFirst = g_c.copyCBCount >= 1 && chainOf(g_c.copyCB[0], false);
        const bool classicChain = classicChainFirst && g_c.beginRenderPass >= 1 && g_c.endRenderPass >= 1;
        std::printf("phase-classic: rp=%u/%u cb=%p chain=%d\n", g_c.beginRenderPass,
                    g_c.endRenderPass, g_c.copyCBCount ? (void *)g_c.copyCB[0] : nullptr,
                    classicChain ? 1 : 0);
        check(classicChain, "classic phase evidence chain");
        if (!isCompare) {
            // dynamic counters must be ACTUAL zeros, printed from real state
            std::printf("phase-dynamic: calls=%u/%u (actual counters)\n", g_c.beginRendering,
                        g_c.endRendering);
            check(g_c.beginRendering == 0 && g_c.endRendering == 0,
                  "classic-only must not call dynamic rendering");
        } else {
            // dynamic chain: the SECOND copy CB is the dynamic recording
            const bool dynChainFirst = g_c.copyCBCount >= 2 && chainOf(g_c.copyCB[1], true);
            const bool dynChain =
                dynChainFirst && g_c.beginRendering == 1 && g_c.endRendering == 1;
            std::printf("phase-dynamic: calls=%u/%u cb=%p chain=%d\n", g_c.beginRendering,
                        g_c.endRendering, g_c.copyCBCount >= 2 ? (void *)g_c.copyCB[1] : nullptr,
                        dynChain ? 1 : 0);
            check(dynChain, "dynamic phase evidence chain");
            check(g_c.copyCB[0] != g_c.copyCB[1], "two independent recordings");
        }
        check(g_c.submitSuccess == (isCompare ? 2u : 1u),
              "every recording really submitted with SUCCESS");
        std::printf("stage: copies=%u submits=%u/%u overflow=%d\n", g_c.copyCalls,
                    g_c.submitSuccess, g_c.submitCalls, g_c.overflow ? 1 : 0);
        check(!g_c.overflow, "no hook capacity overflow");
    }

    // ---- M6-A1 draw evidence: bind/draw chains per branch ----
    if (isDraw && g_failures == 0 && classicPhase && (!isCompare || dynamicPhase) && !skipDraw) {
        const auto drawChain = [&](VkCommandBuffer cb, VkPipeline pl) {
            if (cb == VK_NULL_HANDLE) return false;
            bool began = false, copied = false, submitted = false, bound = false, drew = false;
            for (unsigned i = 0; i < g_c.beginCBCount; ++i) began |= (g_c.beginCB[i] == cb);
            for (unsigned i = 0; i < g_c.copyCBCount; ++i) copied |= (g_c.copyCB[i] == cb);
            for (unsigned i = 0; i < g_c.submitCBCount; ++i) submitted |= (g_c.submitCB[i] == cb);
            for (unsigned i = 0; i < g_c.bindCBCount; ++i)
                bound |= (g_c.bindCB[i] == cb && g_c.bindPipeline[i] == pl);
            for (unsigned i = 0; i < g_c.drawCBCount; ++i)
                drew |= (g_c.drawCB[i] == cb && g_c.drawVertexCount[i] == 3 &&
                         g_c.drawInstanceCount[i] == 1 && g_c.drawFirstVertex[i] == 0 &&
                         g_c.drawFirstInstance[i] == 0 && g_c.drawBoundPipeline[i] == pl);
            return began && copied && submitted && bound && drew;
        };
        const VkPipeline classicPipeline =
            drawPSO ? static_cast<CCVKPipelineState *>(drawPSO)->gpuPipelineState()->vkPipeline
                    : g_ownedClassic;
        const bool classicDraw = g_c.copyCBCount >= 1 &&
                                 drawChain(g_c.copyCB[0], classicPipeline) &&
                                 classicPipeline == g_classicPipelineFromDriver;
        const auto drawCount = [&](VkCommandBuffer cb) {
            unsigned count = 0;
            for (unsigned i = 0; i < g_c.drawCBCount; ++i) count += g_c.drawCB[i] == cb;
            return count;
        };
        check(g_c.drawCalls == (isCompare ? 2u : 1u), "exact per-case draw count");
        std::printf("phase-draw-classic: bind=%p draw=%u cb=%p chain=%d\n",
                    (void *)classicPipeline, g_c.copyCBCount ? drawCount(g_c.copyCB[0]) : 0,
                    g_c.copyCBCount ? (void *)g_c.copyCB[0] : nullptr, classicDraw ? 1 : 0);
        check(classicDraw, "classic draw chain (bind real PSO + draw 3 + copy + submit)");
        if (isCompare) {
            const VkPipeline dynEvidence = isOwned ? g_ownedVariant : g_dynamicPipeline;
            const bool dynDraw = g_c.copyCBCount >= 2 &&
                                 drawChain(g_c.copyCB[1], dynEvidence) &&
                                 dynEvidence != classicPipeline;
            std::printf("phase-draw-dynamic: bind=%p draw=%u cb=%p chain=%d\n",
                        (void *)dynEvidence, g_c.copyCBCount >= 2 ? drawCount(g_c.copyCB[1]) : 0,
                        g_c.copyCBCount >= 2 ? (void *)g_c.copyCB[1] : nullptr, dynDraw ? 1 : 0);
            check(dynDraw, "dynamic draw chain (bind variant + draw 3 + copy + submit)");
        }
    }
    if (skipDraw) {
        // negative control: no draws at all; the blue clear must produce a
        // red/blue differ in two of four channels (FAILED, precise exit 1)
        check(g_c.drawCalls == 0, "skip-draw must record zero draws");
        check(mismatches == W * H * 2 * (isCompare ? 2u : 1u),
              "skip-draw must mismatch red/blue channels in every pixel");
        std::printf("skip-draw: draws=%u mismatches=%u/%u\n", g_c.drawCalls, mismatches,
                    BYTES * (isCompare ? 2u : 1u));
    }

    // ---- validation: LIVE counters before destroy; cached after ----
    if (isDraw) {
        check(g_mainClassicCreates == 1 && g_engineVariantCreates == (isOwned ? 1U : 0U) &&
                  g_eligibilityCreates == (caseName == "owned-failure" ? 1U : 0U) &&
                  g_unknownCreateVolume == 0 &&
                  g_dynamicCreateCalls == (caseName == "draw-compare" ? 1U : 0U),
              "exact classified driver creation counts");
        std::printf("pipeline-creates: main=%u dynamic=%u eligibility=%u unknown=%u diagnostic=%u\n",
                    g_mainClassicCreates, g_engineVariantCreates, g_eligibilityCreates,
                    g_unknownCreateVolume, g_dynamicCreateCalls);
    }
    // draw objects (variant VkPipeline + PSO facade) BEFORE RP/FBO teardown
    if (cmd) {
        cmd->destroy();
        delete cmd;
        cmd = nullptr;
    }
    cleanupDrawObjects(); // NULL-safe on every path
    // cleanup, then the runtime-OFF assertion holds to the very end
    cleanup();
    check(raw.gpuDevice()->runtimePaths.useDynamicRendering == enginePath,
          "runtime policy preserved after cleanup");
    if (enginePath) {
        const bool renderingObserved = classicPhase && (!isCompare || dynamicPhase);
        const bool drawsObserved = !skipDraw && g_c.drawCalls == (isCompare ? 2U : 1U);
        std::printf("engine-path: gfxRendering=%u gfxDraw=%u runtimeDynamic=%u restored=%u\n",
                    renderingObserved ? 1U : 0U, drawsObserved ? 1U : 0U,
                    raw.gpuDevice()->runtimePaths.useDynamicRendering ? 1U : 0U,
                    raw.gpuDevice()->runtimePaths.useDynamicRendering == enginePath ? 1U : 0U);
        std::printf("engine-calls: classic=%u/%u dynamic=%u/%u draws=%u submits=%u/%u\n",
                    g_c.beginRenderPass, g_c.endRenderPass, g_c.beginRendering, g_c.endRendering,
                    g_c.drawCalls, g_c.submitSuccess, g_c.submitCalls);
    }

    const uint32_t liveErrors = context->validationErrorCount.load(std::memory_order_relaxed);
    check(liveErrors == 0, "live validation errors");
    ownedDevice->destroy();
    const uint32_t postErrors = raw.getLastValidationErrorCount();
    check(postErrors == 0, "post-destroy cached validation errors");
    std::printf("validation: liveErrors=%u postErrors=%u\n", liveErrors, postErrors);

    // M6-A2a retirement evidence: every engine-owned handle destroyed once.
    // The destroy hook stays installed through ownedDevice->destroy() above
    // (HookRAII outlives it), so the recycle-bin final clear is observed too.
    if (isOwned && drawPSO == nullptr) { // facade torn down above (pending or cleanup)
        const unsigned classicDestroyed = destroyCountOf(g_ownedClassic);
        unsigned dynDestroyed = 0;
        if (caseName == "owned-compare") dynDestroyed = destroyCountOf(g_ownedVariant);
        const unsigned partialDestroyed =
            caseName == "owned-failure" ? destroyCountOf(g_engineVariant.handle) : 0;
        const unsigned eligibilityDestroyed = destroyCountOf(g_eligibility.handle);
        std::printf(
            "retirement: classicDestroyed=%u dynDestroyed=%u partialDestroyed=%u "
            "eligibilityDestroyed=%u\n",
            classicDestroyed, dynDestroyed, partialDestroyed, eligibilityDestroyed);
        check(classicDestroyed == 1, "engine classic pipeline retired exactly once");
        if (caseName == "owned-compare")
            check(dynDestroyed == 1, "engine dynamic variant retired exactly once");
        if (caseName == "owned-failure") {
            // final re-check AFTER device destroy: the partial variant must
            // total exactly one destroy across the whole lifetime
            check(partialDestroyed == 1, "partial variant destroyed exactly once (final)");
            check(eligibilityDestroyed == 1, "eligibility pipeline destroyed exactly once");
        }
    }

    const bool incomplete = !classicPhase || (isCompare && !dynamicPhase);
    const char *outcome = (g_failures || liveErrors || postErrors || mismatches)
                              ? "FAILED"
                              : (incomplete ? "INCOMPLETE" : "CLEAN");
    std::printf("%s completed: failures=%u liveErrors=%u postErrors=%u mismatches=%u outcome=%s\n",
                caseName.c_str(), g_failures, liveErrors, postErrors, mismatches, outcome);
    return (g_failures || liveErrors || postErrors || mismatches || incomplete) ? 1 : 0;
}
