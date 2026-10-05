// Exercise the actual Cocos backend and shader compiler, not a substitute renderer.
#include "gfx-base/SPIRVUtils.h"
#include "gfx-vulkan/VKDescriptorSetLayout.h"
#include "gfx-vulkan/VKDevice.h"
#include "gfx-vulkan/VKGPUObjects.h"
#include "gfx-vulkan/VKPipelineLayout.h"
#include "gfx-vulkan/VKPipelineState.h"
#include "gfx-vulkan/VKRenderPass.h"
#include "gfx-vulkan/VKShader.h"
#include "gfx-vulkan/VKUtils.h"
#include "platform/FileUtils.h"
#include "renderer/GFXDeviceManager.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// The platform object links its application hooks, but this test starts no app/window.
int cocos_main(int, const char **) { return 0; }
void cocos_destroy() {}

namespace {
using namespace cc::gfx;
struct ClassicRenderPassEvidence {
    PFN_vkCreateRenderPass2 create = nullptr;
    VulkanClassicRenderPassKey key;
    VkRenderPass handle = VK_NULL_HANDLE;
    uint32_t calls = 0;
    bool invalid = false;
} g_renderPassEvidence;

VKAPI_ATTR VkResult VKAPI_CALL observeRenderPass(VkDevice device, const VkRenderPassCreateInfo2 *info,
                                                 const VkAllocationCallbacks *allocator, VkRenderPass *output) {
    auto &e = g_renderPassEvidence;
    if (!e.create || !info || !output) {
        e.invalid = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkResult result = e.create(device, info, allocator, output);
    ++e.calls;
    if (result != VK_SUCCESS || !*output || e.calls != 1 ||
        !VulkanClassicRenderPassKey::build(*info, e.key)) {
        e.invalid = true;
        return result;
    }
    e.handle = *output;
    return result;
}

bool verifyRenderPassKey(RenderPass *pass) {
    const auto *gpu = pass ? static_cast<CCVKRenderPass *>(pass)->gpuRenderPass() : nullptr;
    const auto &e = g_renderPassEvidence;
    const bool ok = gpu && !e.invalid && e.calls == 1 && e.handle != VK_NULL_HANDLE &&
                    gpu->vkRenderPass == e.handle && e.key.valid() && e.key.subpassCount() == 2 &&
                    !e.key.words().empty() && gpu->compatibilityKey == e.key &&
                    gpu->compatibilityKey.hash() == e.key.hash() &&
                    gpu->compatibilityKey.words().data() != e.key.words().data();
    if (ok)
        std::printf("CLASSIC_RP_KEY: calls=1 driver=SUCCESS subpasses=2 content=EXACT ownership=OK\n");
    else
        std::fprintf(stderr, "FAIL: real Classic RenderPass compatibility key evidence\n");
    return ok;
}

struct ShaderModuleEvidence {
    struct Module {
        VkShaderModule handle = VK_NULL_HANDLE;
        uint32_t words[4096]{};
        size_t count = 0;
    } modules[2];
    PFN_vkCreateShaderModule create = nullptr;
    uint32_t count = 0;
    bool invalid = false;
} g_shaderEvidence;

VKAPI_ATTR VkResult VKAPI_CALL observeShaderModule(VkDevice device, const VkShaderModuleCreateInfo *info,
                                                   const VkAllocationCallbacks *allocator, VkShaderModule *output) {
    auto &e = g_shaderEvidence;
    if (!e.create) {
        e.invalid = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkResult result = e.create(device, info, allocator, output);
    if (result != VK_SUCCESS || !info || !output || !*output ||
        info->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO || info->pNext || info->flags ||
        !info->pCode || info->codeSize % sizeof(uint32_t) || info->codeSize / sizeof(uint32_t) > 4096 || e.count >= 2) {
        e.invalid = true;
        return result;
    }
    auto &record = e.modules[e.count++];
    record.handle = *output;
    record.count = info->codeSize / sizeof(uint32_t);
    for (size_t i = 0; i < record.count; ++i) record.words[i] = info->pCode[i];
    return result;
}

bool verifyShaderKeys(Shader *shader) {
    auto *gpu = shader ? static_cast<CCVKShader *>(shader)->gpuShader() : nullptr;
    bool ok = gpu && gpu->gpuStages.size() == 2 && !g_shaderEvidence.invalid && g_shaderEvidence.count == 2;
    if (ok) {
        const VkShaderStageFlagBits stages[] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
        for (unsigned i = 0; i < 2; ++i) {
            const auto &record = g_shaderEvidence.modules[i];
            const auto &stored = gpu->gpuStages[i];
            VkPipelineShaderStageCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            info.stage = stages[i];
            info.module = record.handle;
            info.pName = "main";
            VulkanShaderStageKey expected;
            ok = VulkanShaderStageKey::build(info, record.words, record.count, expected) && ok;
            ok = ok && stored.vkShader == record.handle && stored.contentKey == expected &&
                 stored.contentKey.hash() == expected.hash() && stored.contentKey.spirvWords().data() != record.words;
        }
    }
    if (ok)
        std::printf("SHADER_KEYS: stages=2 driver=SUCCESS code=EXACT ownership=OK\n");
    else
        std::fprintf(stderr, "FAIL: real shader content key evidence\n");
    return ok;
}

struct GraphicsLayoutEvidence {
    PFN_vkCreatePipelineLayout create = nullptr;
    VulkanPipelineLayoutKey key;
    VkPipelineLayout handle = VK_NULL_HANDLE;
    unsigned calls = 0;
    bool invalid = false;
} g_graphicsLayoutEvidence;

VKAPI_ATTR VkResult VKAPI_CALL observeGraphicsLayout(VkDevice device, const VkPipelineLayoutCreateInfo *info,
                                                     const VkAllocationCallbacks *allocator, VkPipelineLayout *output) {
    auto &e = g_graphicsLayoutEvidence;
    if (!e.create || !info || !output) {
        e.invalid = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkResult result = e.create(device, info, allocator, output);
    ++e.calls;
    if (result != VK_SUCCESS || !*output || e.calls != 1 || info->setLayoutCount ||
        info->pushConstantRangeCount || !VulkanPipelineLayoutKey::build(*info, {}, e.key)) {
        e.invalid = true;
    } else {
        e.handle = *output;
    }
    return result;
}

// Rebuild only from actual shader-module bytes and layout/render-pass creation
// records. Do not borrow the production PSO's key or its child metadata.
bool buildObservedGraphicsKey(const VkGraphicsPipelineCreateInfo &info, VulkanGraphicsPipelineKey &out) noexcept {
    if (info.stageCount != 2 || !info.pStages || g_shaderEvidence.invalid || g_shaderEvidence.count != 2 ||
        g_graphicsLayoutEvidence.invalid || g_graphicsLayoutEvidence.calls != 1 ||
        info.layout != g_graphicsLayoutEvidence.handle || g_renderPassEvidence.invalid ||
        info.renderPass != g_renderPassEvidence.handle) return false;
    VulkanShaderStageKey stages[2];
    const VulkanShaderStageKey *keys[]{&stages[0], &stages[1]};
    for (unsigned i = 0; i < 2; ++i) {
        const ShaderModuleEvidence::Module *module = nullptr;
        for (const auto &record : g_shaderEvidence.modules)
            if (record.handle == info.pStages[i].module) module = &record;
        if (!module || !VulkanShaderStageKey::build(info.pStages[i], module->words, module->count, stages[i])) return false;
    }
    return VulkanGraphicsPipelineKey::build(info, keys, 2, g_graphicsLayoutEvidence.key,
                                            &g_renderPassEvidence.key, out);
}

struct LayoutEvidence {
    struct Set {
        VkDescriptorSetLayout handle = VK_NULL_HANDLE;
        VkDescriptorSetLayoutCreateFlags flags = 0;
        VkDescriptorSetLayoutBinding bindings[2]{};
        uint32_t count = 0;
    } sets[3];
    struct Pipeline {
        VkPipelineLayout handle = VK_NULL_HANDLE;
        VkDescriptorSetLayout layouts[2]{};
        uint32_t count = 0;
    } pipelines[3];
    PFN_vkCreateDescriptorSetLayout createSet = nullptr;
    PFN_vkCreatePipelineLayout createPipeline = nullptr;
    uint32_t setCount = 0, pipelineCount = 0;
    bool invalid = false;
} g_layoutEvidence;

VKAPI_ATTR VkResult VKAPI_CALL observeSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *info,
                                                const VkAllocationCallbacks *allocator, VkDescriptorSetLayout *output) {
    auto &e = g_layoutEvidence;
    if (!e.createSet) {
        e.invalid = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkResult result = e.createSet(device, info, allocator, output);
    if (result != VK_SUCCESS || !info || !output || !*output || info->pNext || info->bindingCount != 2 ||
        !info->pBindings || e.setCount >= 3) {
        e.invalid = true;
        return result;
    }
    auto &record = e.sets[e.setCount++];
    record.handle = *output;
    record.flags = info->flags;
    record.count = info->bindingCount;
    for (uint32_t i = 0; i < 2; ++i) {
        if (info->pBindings[i].pImmutableSamplers) e.invalid = true;
        record.bindings[i] = info->pBindings[i];
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL observePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo *info,
                                                     const VkAllocationCallbacks *allocator, VkPipelineLayout *output) {
    auto &e = g_layoutEvidence;
    if (!e.createPipeline) {
        e.invalid = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkResult result = e.createPipeline(device, info, allocator, output);
    if (result != VK_SUCCESS || !info || !output || !*output || info->pNext || info->flags ||
        info->pushConstantRangeCount || info->setLayoutCount != 2 || !info->pSetLayouts || e.pipelineCount >= 3) {
        e.invalid = true;
        return result;
    }
    auto &record = e.pipelines[e.pipelineCount++];
    record.handle = *output;
    record.count = info->setLayoutCount;
    for (uint32_t i = 0; i < 2; ++i) record.layouts[i] = info->pSetLayouts[i];
    return result;
}

bool verifyLayoutKeys(CCVKDevice &device) {
    DescriptorSetLayout *sets[3]{};
    PipelineLayout *pipelines[3]{};
    bool ok = true;
    {
        struct RestoreHooks {
            RestoreHooks() {
                g_layoutEvidence = {};
                g_layoutEvidence.createSet = vkCreateDescriptorSetLayout;
                g_layoutEvidence.createPipeline = vkCreatePipelineLayout;
                vkCreateDescriptorSetLayout = observeSetLayout;
                vkCreatePipelineLayout = observePipelineLayout;
            }
            ~RestoreHooks() {
                vkCreateDescriptorSetLayout = g_layoutEvidence.createSet;
                vkCreatePipelineLayout = g_layoutEvidence.createPipeline;
            }
        } hooks;
        try {
            DescriptorSetLayoutBinding uniform;
            uniform.binding = 0;
            uniform.descriptorType = DescriptorType::UNIFORM_BUFFER;
            uniform.count = 1;
            uniform.stageFlags = ShaderStageFlagBit::VERTEX;
            DescriptorSetLayoutBinding storage;
            storage.binding = 1;
            storage.descriptorType = DescriptorType::STORAGE_BUFFER;
            storage.count = 2;
            storage.stageFlags = ShaderStageFlagBit::COMPUTE;
            DescriptorSetLayoutInfo info;
            info.bindings = {uniform, storage};
            sets[0] = device.createDescriptorSetLayout(info);
            info.bindings = {storage, uniform};
            sets[1] = device.createDescriptorSetLayout(info);
            storage.count = 3;
            info.bindings = {uniform, storage};
            sets[2] = device.createDescriptorSetLayout(info);
            for (auto *set : sets)
                if (!set) ok = false;
            if (ok) {
                const unsigned ordering[3][2] = {{0, 2}, {1, 2}, {2, 0}};
                for (unsigned i = 0; i < 3; ++i) {
                    PipelineLayoutInfo layout;
                    layout.setLayouts = {sets[ordering[i][0]], sets[ordering[i][1]]};
                    pipelines[i] = device.createPipelineLayout(layout);
                    if (!pipelines[i]) ok = false;
                }
                const auto &e = g_layoutEvidence;
                ok = ok && !e.invalid && e.setCount == 3 && e.pipelineCount == 3;
                VulkanDescriptorSetLayoutKey expectedSets[3];
                if (ok) {
                    for (unsigned i = 0; i < 3; ++i) {
                        VkDescriptorSetLayoutCreateInfo submitted{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
                        submitted.flags = e.sets[i].flags;
                        submitted.bindingCount = e.sets[i].count;
                        submitted.pBindings = e.sets[i].bindings;
                        const auto *gpu = static_cast<CCVKDescriptorSetLayout *>(sets[i])->gpuDescriptorSetLayout();
                        ok = VulkanDescriptorSetLayoutKey::build(submitted, expectedSets[i]) && ok;
                        ok = ok && gpu && gpu->vkDescriptorSetLayout == e.sets[i].handle && gpu->contentKey == expectedSets[i];
                    }
                    for (unsigned i = 0; i < 3; ++i) {
                        const auto &record = e.pipelines[i];
                        VkPipelineLayoutCreateInfo submitted{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
                        submitted.setLayoutCount = record.count;
                        submitted.pSetLayouts = record.layouts;
                        VulkanPipelineLayoutKey expected;
                        ok = VulkanPipelineLayoutKey::build(submitted,
                                                            {expectedSets[ordering[i][0]], expectedSets[ordering[i][1]]}, expected) &&
                             ok;
                        const auto *gpu = static_cast<CCVKPipelineLayout *>(pipelines[i])->gpuPipelineLayout();
                        ok = ok && gpu && gpu->vkPipelineLayout == record.handle && gpu->contentKey == expected;
                        for (unsigned j = 0; j < 2; ++j) ok = ok && record.layouts[j] == e.sets[ordering[i][j]].handle;
                    }
                    const auto &a = static_cast<CCVKPipelineLayout *>(pipelines[0])->gpuPipelineLayout()->contentKey;
                    const auto &b = static_cast<CCVKPipelineLayout *>(pipelines[1])->gpuPipelineLayout()->contentKey;
                    const auto &c = static_cast<CCVKPipelineLayout *>(pipelines[2])->gpuPipelineLayout()->contentKey;
                    ok = ok && expectedSets[0] == expectedSets[1] && expectedSets[0] != expectedSets[2] && a == b && a != c;
                }
            }
        } catch (...) {
            ok = false;
        }
    }
    for (auto *pipeline : pipelines)
        if (pipeline) {
            pipeline->destroy();
            delete pipeline;
        }
    for (auto *set : sets)
        if (set) {
            set->destroy();
            delete set;
        }
    if (ok)
        std::printf("LAYOUT_KEYS: sets=3 pipelines=3 driver=SUCCESS canonical=OK ordered=OK\n");
    else
        std::fprintf(stderr, "FAIL: real layout content key evidence\n");
    return ok;
}

struct ComputeEvidence {
    struct Pipeline {
        VkPipeline handle = VK_NULL_HANDLE;
        VkPipelineCreateFlags flags = 0;
        VkPipelineShaderStageCreateFlags stageFlags = 0;
        VkShaderStageFlagBits stage = VK_SHADER_STAGE_COMPUTE_BIT;
        VkShaderModule module = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        char entry[16]{};
    } pipelines[3];
    PFN_vkCreateComputePipelines create = nullptr;
    PFN_vkCreatePipelineLayout createLayout = nullptr;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipelineLayoutCreateFlags layoutFlags = 0;
    unsigned count = 0, layouts = 0;
    bool invalid = false;
} g_computeEvidence;

VKAPI_ATTR VkResult VKAPI_CALL observeComputeLayout(VkDevice device, const VkPipelineLayoutCreateInfo *info,
                                                    const VkAllocationCallbacks *allocator, VkPipelineLayout *output) {
    auto &e = g_computeEvidence;
    if (!e.createLayout || !info || !output) {
        e.invalid = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const auto result = e.createLayout(device, info, allocator, output);
    if (result != VK_SUCCESS || !*output || info->sType != VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO ||
        info->pNext || info->setLayoutCount || info->pushConstantRangeCount || e.layouts++) {
        e.invalid = true;
        return result;
    }
    e.layout = *output;
    e.layoutFlags = info->flags;
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL observeComputePipeline(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                      const VkComputePipelineCreateInfo *info,
                                                      const VkAllocationCallbacks *allocator, VkPipeline *output) {
    auto &e = g_computeEvidence;
    if (!e.create || !count || !info || !output) {
        e.invalid = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const auto result = e.create(device, cache, count, info, allocator, output);
    if (result != VK_SUCCESS || count != 1 || !output[0] || e.count >= 3 ||
        info->sType != VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO || info->pNext || info->basePipelineHandle ||
        info->stage.sType != VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO || info->stage.pNext ||
        info->stage.pSpecializationInfo || !info->stage.pName || !info->stage.module || !info->layout) {
        e.invalid = true;
        return result;
    }
    auto &record = e.pipelines[e.count++];
    record.handle = output[0];
    record.flags = info->flags;
    record.stageFlags = info->stage.flags;
    record.stage = info->stage.stage;
    record.module = info->stage.module;
    record.layout = info->layout;
    unsigned i = 0;
    for (; i + 1 < sizeof(record.entry) && info->stage.pName[i]; ++i) record.entry[i] = info->stage.pName[i];
    if (info->stage.pName[i]) e.invalid = true;
    return result;
}

bool verifyComputeKeys(CCVKDevice &device) {
    Shader *shaders[2]{};
    PipelineLayout *layout = nullptr;
    PipelineState *pipelines[3]{};
    bool ok = true;
    const auto beforeErrors = device.gpuContext()->validationErrorCount.load();
    {
        struct RestoreComputeHooks {
            RestoreComputeHooks() {
                g_computeEvidence = {};
                g_shaderEvidence = {};
                g_computeEvidence.create = vkCreateComputePipelines;
                g_computeEvidence.createLayout = vkCreatePipelineLayout;
                g_shaderEvidence.create = vkCreateShaderModule;
                vkCreateComputePipelines = observeComputePipeline;
                vkCreatePipelineLayout = observeComputeLayout;
                vkCreateShaderModule = observeShaderModule;
            }
            ~RestoreComputeHooks() {
                vkCreateComputePipelines = g_computeEvidence.create;
                vkCreatePipelineLayout = g_computeEvidence.createLayout;
                vkCreateShaderModule = g_shaderEvidence.create;
            }
        } hooks;
        try {
            ShaderInfo info;
            info.name = "m7d2-compute-key";
            ShaderStage stage;
            stage.stage = ShaderStageFlagBit::COMPUTE;
            stage.source = "layout(local_size_x=1) in;void main(){}";
            info.stages = {stage};
            shaders[0] = device.createShader(info);
            info.stages[0].source = "layout(local_size_x=2) in;void main(){}";
            shaders[1] = device.createShader(info);
            layout = device.createPipelineLayout(PipelineLayoutInfo{});
            ok = shaders[0] && shaders[1] && layout;
            if (ok) {
                for (unsigned i = 0; i < 3; ++i) {
                    PipelineStateInfo pso;
                    pso.bindPoint = PipelineBindPoint::COMPUTE;
                    pso.shader = shaders[i == 2 ? 1 : 0];
                    pso.pipelineLayout = layout;
                    pipelines[i] = device.createPipelineState(pso);
                    if (!pipelines[i]) ok = false;
                }
                ok = ok && !g_computeEvidence.invalid && g_computeEvidence.count == 3 && g_computeEvidence.layouts == 1 &&
                     !g_shaderEvidence.invalid && g_shaderEvidence.count == 2;
            }
            if (ok) {
                VkPipelineLayoutCreateInfo submitted{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
                submitted.flags = g_computeEvidence.layoutFlags;
                VulkanPipelineLayoutKey expectedLayout;
                ok = VulkanPipelineLayoutKey::build(submitted, {}, expectedLayout);
                const auto *gpuLayout = static_cast<CCVKPipelineLayout *>(layout)->gpuPipelineLayout();
                ok = ok && gpuLayout && gpuLayout->vkPipelineLayout == g_computeEvidence.layout &&
                     gpuLayout->contentKey == expectedLayout;
                for (unsigned i = 0; i < 3 && ok; ++i) {
                    const auto &record = g_computeEvidence.pipelines[i];
                    const auto &code = g_shaderEvidence.modules[i == 2 ? 1 : 0];
                    const auto *gpu = static_cast<CCVKPipelineState *>(pipelines[i])->gpuPipelineState();
                    VkPipelineShaderStageCreateInfo submittedStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
                    submittedStage.flags = record.stageFlags;
                    submittedStage.stage = record.stage;
                    submittedStage.module = record.module;
                    submittedStage.pName = record.entry;
                    VulkanShaderStageKey expectedShader;
                    VulkanComputePipelineKey expected;
                    ok = VulkanShaderStageKey::build(submittedStage, code.words, code.count, expectedShader) &&
                         VulkanComputePipelineKey::build(record.flags, expectedShader, expectedLayout, expected) &&
                         record.module == code.handle && record.layout == g_computeEvidence.layout && gpu &&
                         gpu->vkPipeline == record.handle && gpu->computeContentKey == expected &&
                         gpu->computeContentKey.hash() == expected.hash() &&
                         gpu->computeContentKey.shader().spirvWords().data() != code.words;
                }
                if (ok) {
                    const auto &a = static_cast<CCVKPipelineState *>(pipelines[0])->gpuPipelineState()->computeContentKey;
                    const auto &b = static_cast<CCVKPipelineState *>(pipelines[1])->gpuPipelineState()->computeContentKey;
                    const auto &c = static_cast<CCVKPipelineState *>(pipelines[2])->gpuPipelineState()->computeContentKey;
                    ok = a == b && a != c && a.hash() != c.hash();
                }
            }
        } catch (...) {
            ok = false;
        }
    }
    for (auto *pipeline : pipelines)
        if (pipeline) {
            pipeline->destroy();
            delete pipeline;
        }
    if (layout) {
        layout->destroy();
        delete layout;
    }
    for (auto *shader : shaders)
        if (shader) {
            shader->destroy();
            delete shader;
        }
    ok = ok && beforeErrors == 0 && device.gpuContext()->validationErrorCount.load() == beforeErrors;
    if (ok)
        std::printf("COMPUTE_KEYS: pipelines=3 modules=2 driver=SUCCESS equal=OK distinct=OK ownership=OK\n");
    else
        std::fprintf(stderr, "FAIL: real compute pipeline content key evidence\n");
    return ok;
}
} // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    using namespace cc::gfx;
    const std::filesystem::path output = argc > 1 ? argv[1] : "engine-spirv-out";
    std::filesystem::create_directories(output);
    std::unique_ptr<cc::FileUtils> fileUtils(cc::createFileUtils());
    fileUtils->setWritablePath(output.generic_string() + "/");
    std::fprintf(stderr, "Creating Cocos Vulkan device via DeviceManager\n");
    std::unique_ptr<Device> ownedDevice(DeviceManager::create());
    if (!ownedDevice || ownedDevice->getGfxAPI() != API::VULKAN) {
        std::fprintf(stderr, "FAIL: Cocos Vulkan initialization\n");
        // DeviceManager can return a successfully initialized fallback backend.
        // It has the same explicit destroy-before-delete contract as Vulkan.
        if (ownedDevice) ownedDevice->destroy();
        return 1;
    }
    CCVKDevice &device = *CCVKDevice::getInstance();
    auto *context = device.gpuContext();
    const bool validation = context->validationEnabled && context->bestPracticesValidationEnabled &&
                            context->synchronizationValidationEnabled;
    std::printf("Cocos effective API: %u.%u; validation + best practices + sync: %s\n",
                VK_VERSION_MAJOR(context->apiVersionInfo.effectiveDeviceApiVersion),
                VK_VERSION_MINOR(context->apiVersionInfo.effectiveDeviceApiVersion),
                validation ? "ENABLED" : "NOT VERIFIED");
    bool failed = !validation;

    // =========================================================================
    // Capability negotiation contract (M1-D). Assertions derive the EXPECTED
    // policy from the effective tier + debug disable hooks (mirroring
    // VKDevice.cpp requestFeature lines); nothing assumes this GPU supports
    // any specific optional feature.
    // =========================================================================
    {
        const auto &info = context->apiVersionInfo;
        const uint32_t effective = info.effectiveDeviceApiVersion;
        const int minor = static_cast<int>(VK_VERSION_MINOR(effective));
        const auto &caps = device.gpuDevice()->capabilities;
        const auto &paths = device.gpuDevice()->runtimePaths;

        // mirror the engine's debug hooks exactly like the core: parsed only
        // in CC_DEBUG builds
        struct Disabled {
            bool timeline = false, sync2 = false, dynamic = false, m4 = false;
        } dis;
#if CC_DEBUG > 0
        if (const char *list = std::getenv("CC_VULKAN_DISABLE_FEATURES")) {
            std::string token;
            std::stringstream stream(list);
            while (std::getline(stream, token, ',')) {
                if (token == "timelineSemaphore")
                    dis.timeline = true;
                else if (token == "synchronization2")
                    dis.sync2 = true;
                else if (token == "dynamicRendering")
                    dis.dynamic = true;
                else if (token == "maintenance4")
                    dis.m4 = true;
            }
        }
#endif

        // -- version model consistency -------------------------------------
        const auto capFail = [&failed](bool ok, const char *what) {
            if (!ok) {
                std::fprintf(stderr, "FAIL(capability): %s\n", what);
                failed = true;
            }
        };
        const uint32_t minVersion = info.physicalDeviceApiVersion < info.instanceApiVersion
                                        ? info.physicalDeviceApiVersion
                                        : info.instanceApiVersion;
        capFail(effective >= VK_API_VERSION_1_1, "effective API below 1.1 baseline");
        capFail(effective == minVersion, "effective must EQUAL min(instance, physical)");
        capFail(device.gpuDevice()->apiVersionInfo.effectiveDeviceApiVersion == effective,
                "gpu-device effective version differs from context");
        capFail(caps.version.effectiveDeviceApiVersion == effective,
                "capabilities.version.effective differs from context");
        capFail(context->capabilities.version.effectiveDeviceApiVersion == effective,
                "context capabilities.version.effective differs from context apiVersionInfo");

        // -- enabled == supported && requested (creation succeeded) ---------
        const auto stateOk = [&caps, &capFail](const VulkanCapabilityState &s, const char *name) {
            if (s.enabled != (s.supported && s.requested)) {
                std::fprintf(stderr, "FAIL(capability): %s enabled=%d supported=%d requested=%d\n",
                             name, (int)s.enabled, (int)s.supported, (int)s.requested);
                capFail(false, "enabled must equal supported&&requested after successful creation");
            }
        };
        stateOk(caps.timelineSemaphore, "timelineSemaphore");
        stateOk(caps.bufferDeviceAddress, "bufferDeviceAddress");
        stateOk(caps.separateDepthStencilLayouts, "separateDepthStencilLayouts");
        stateOk(caps.synchronization2, "synchronization2");
        stateOk(caps.dynamicRendering, "dynamicRendering");
        stateOk(caps.maintenance4, "maintenance4");
        // NOTE: the FSR family SUMMARY must not be checked via the generic
        // invariant: a GPU may support only `primitive` while the policy
        // requests only pipeline/attachment - then summary.supported and
        // summary.requested are legally true while summary.enabled is false.
        // The summary is verified field-wise against ANY-of-sub instead.
        stateOk(caps.memoryBudget, "memoryBudget");
        stateOk(caps.graphicsPipelineLibrary, "graphicsPipelineLibrary");
        stateOk(caps.fragmentShadingRateDetail.pipelineFragmentShadingRate, "fsr.pipeline");
        stateOk(caps.fragmentShadingRateDetail.attachmentFragmentShadingRate, "fsr.attachment");
        const VulkanCapabilityState *di[] = {
            &caps.descriptorIndexing.shaderUniformBufferArrayNonUniformIndexing,
            &caps.descriptorIndexing.shaderSampledImageArrayNonUniformIndexing,
            &caps.descriptorIndexing.shaderStorageBufferArrayNonUniformIndexing,
            &caps.descriptorIndexing.shaderStorageImageArrayNonUniformIndexing,
            &caps.descriptorIndexing.descriptorBindingUniformBufferUpdateAfterBind,
            &caps.descriptorIndexing.descriptorBindingSampledImageUpdateAfterBind,
            &caps.descriptorIndexing.descriptorBindingStorageImageUpdateAfterBind,
            &caps.descriptorIndexing.descriptorBindingStorageBufferUpdateAfterBind,
            &caps.descriptorIndexing.descriptorBindingPartiallyBound,
            &caps.descriptorIndexing.descriptorBindingVariableDescriptorCount,
            &caps.descriptorIndexing.runtimeDescriptorArray,
        };
        for (const auto *s : di) {
            stateOk(*s, "descriptorIndexing member");
        }

        // -- default policy (VKDevice.cpp: requestFeature lines + hooks) ----
        capFail(caps.timelineSemaphore.requested == (minor >= 2 && !dis.timeline),
                "timeline policy mismatch");
        capFail(caps.separateDepthStencilLayouts.requested == (minor >= 2),
                "separateDepthStencil policy mismatch");
        capFail(caps.synchronization2.requested == (minor >= 3 && !dis.sync2),
                "synchronization2 policy mismatch");
        capFail(caps.dynamicRendering.requested == (minor >= 3 && !dis.dynamic),
                "dynamicRendering policy mismatch");
        capFail(caps.maintenance4.requested == (minor >= 3 && !dis.m4),
                "maintenance4 policy mismatch");
        capFail(!caps.bufferDeviceAddress.requested, "BDA must default to unrequested");
        capFail(!caps.memoryBudget.requested, "memoryBudget must default to unrequested");
        capFail(!caps.graphicsPipelineLibrary.requested, "GPL must default to unrequested");
        for (const auto *s : di) {
            capFail(!s->requested, "descriptor indexing must default to unrequested");
            capFail(!s->enabled, "unrequested descriptor indexing member must stay disabled");
        }
        // FSR: primitive is never requested by policy; pipeline/attachment are
        // requested together (same real-extension closure); the summary fields
        // each equal ANY of the corresponding sub-states (never the generic
        // enabled==supported&&requested invariant).
        capFail(!caps.fragmentShadingRateDetail.primitiveFragmentShadingRate.requested,
                "fsr.primitive must not be requested by default policy");
        capFail(!caps.fragmentShadingRateDetail.primitiveFragmentShadingRate.enabled,
                "fsr.primitive must not be enabled");
        capFail(caps.fragmentShadingRateDetail.pipelineFragmentShadingRate.requested ==
                    caps.fragmentShadingRateDetail.attachmentFragmentShadingRate.requested,
                "fsr pipeline/attachment must be requested together");
        const bool anySup = caps.fragmentShadingRateDetail.pipelineFragmentShadingRate.supported ||
                            caps.fragmentShadingRateDetail.primitiveFragmentShadingRate.supported ||
                            caps.fragmentShadingRateDetail.attachmentFragmentShadingRate.supported;
        const bool anyReq = caps.fragmentShadingRateDetail.pipelineFragmentShadingRate.requested ||
                            caps.fragmentShadingRateDetail.primitiveFragmentShadingRate.requested ||
                            caps.fragmentShadingRateDetail.attachmentFragmentShadingRate.requested;
        const bool anyEn = caps.fragmentShadingRateDetail.pipelineFragmentShadingRate.enabled ||
                           caps.fragmentShadingRateDetail.primitiveFragmentShadingRate.enabled ||
                           caps.fragmentShadingRateDetail.attachmentFragmentShadingRate.enabled;
        capFail(caps.fragmentShadingRate.supported == anySup, "fsr summary supported must equal any(sub)");
        capFail(caps.fragmentShadingRate.requested == anyReq, "fsr summary requested must equal any(sub)");
        capFail(caps.fragmentShadingRate.enabled == anyEn, "fsr summary enabled must equal any(sub)");

        // M3 enables completion only; independently forced fence remains valid.
        bool expectedTimeline = caps.timelineSemaphore.enabled;
#if CC_DEBUG > 0
        if (const char *mode = std::getenv("CC_VULKAN_COMPLETION")) {
            if (std::string(mode) == "fence")
                expectedTimeline = false;
            else if (std::string(mode) == "timeline")
                expectedTimeline = true;
        }
#endif
        capFail(paths.useTimelineSemaphore == expectedTimeline, "completion backend policy mismatch");
        for (const auto &entry : device.gpuDevice()->completionTrackers) {
            capFail(entry.second && entry.second->usesTimeline() == expectedTimeline, "actual tracker backend differs from policy");
        }
        bool expectedSubmit2 = false;
#if CC_DEBUG > 0
        if (const char *mode = std::getenv("CC_VULKAN_SUBMIT")) expectedSubmit2 = std::string(mode) == "submit2";
#endif
        capFail(paths.useSubmit2 == expectedSubmit2 && (!expectedSubmit2 || caps.synchronization2.enabled), "submit encoder policy mismatch");
        for (const auto &entry : device.gpuDevice()->completionTrackers) {
            capFail(entry.second && entry.second->usesSubmit2() == expectedSubmit2, "actual submit encoder differs from policy");
        }
        bool expectedBarrier2 = false;
#if CC_DEBUG > 0
        if (const char *mode = std::getenv("CC_VULKAN_BARRIER")) expectedBarrier2 = std::string(mode) == "sync2";
#endif
        capFail(paths.useSynchronization2 == expectedBarrier2 && (!expectedBarrier2 || caps.synchronization2.enabled), "barrier encoder policy mismatch");
        capFail(!paths.useDynamicRendering && !paths.forceClassicRenderPass && !paths.useMaintenance4,
                "render/maintenance optimizations remain OFF during semantic migration");

        // -- maintenance4 correctness limit ----------------------------------
        if (caps.maintenance4.enabled) {
            capFail(caps.maxBufferSize > 0, "maintenance4 enabled requires maxBufferSize > 0");
        }

        std::printf("CAPS: effective=1.%d physical=%u.%u instance=%u.%u (forced tier does NOT emulate a low-version GPU)\n",
                    minor,
                    VK_VERSION_MAJOR(info.physicalDeviceApiVersion), VK_VERSION_MINOR(info.physicalDeviceApiVersion),
                    VK_VERSION_MAJOR(info.instanceApiVersion), VK_VERSION_MINOR(info.instanceApiVersion));
        std::printf("CAPS: timeline=%d/%d/%d sdsl=%d/%d/%d sync2=%d/%d/%d dynamic=%d/%d/%d m4=%d/%d/%d maxBufferSize=%llu\n",
                    (int)caps.timelineSemaphore.supported, (int)caps.timelineSemaphore.requested, (int)caps.timelineSemaphore.enabled,
                    (int)caps.separateDepthStencilLayouts.supported, (int)caps.separateDepthStencilLayouts.requested, (int)caps.separateDepthStencilLayouts.enabled,
                    (int)caps.synchronization2.supported, (int)caps.synchronization2.requested, (int)caps.synchronization2.enabled,
                    (int)caps.dynamicRendering.supported, (int)caps.dynamicRendering.requested, (int)caps.dynamicRendering.enabled,
                    (int)caps.maintenance4.supported, (int)caps.maintenance4.requested, (int)caps.maintenance4.enabled,
                    (unsigned long long)caps.maxBufferSize);
    }

    struct ShaderCase {
        const char *name;
        ShaderStageFlagBit stage;
        const char *source;
    };
    const ShaderCase cases[]{
        {"vertex", ShaderStageFlagBit::VERTEX,
         "#version 450\nlayout(location=5) in vec2 p;\n"
         "layout(set=0,binding=0) uniform Params { mat4 mvp; };\n"
         "void main(){gl_Position=mvp*vec4(p,0,1);}\n"},
        {"fragment", ShaderStageFlagBit::FRAGMENT,
         "#version 450\nlayout(location=0) out vec4 color;\n"
         "layout(set=0,binding=1) uniform sampler2D tex;\n"
         "void main(){color=texture(tex,vec2(0.5));}\n"},
        {"compute", ShaderStageFlagBit::COMPUTE,
         "#version 450\nlayout(local_size_x=8) in;\n"
         "layout(set=0,binding=0,std430) buffer Data { uint values[]; };\n"
         "void main(){values[gl_GlobalInvocationID.x]+=1;}\n"},
    };
    auto *compiler = SPIRVUtils::getInstance();
    for (int minor : {1, 2, 3}) {
        compiler->destroy();
        compiler->initialize(minor);
        const auto directory = output / ("vk1." + std::to_string(minor));
        std::filesystem::create_directories(directory);
        const uint32_t expectedVersion = minor == 1 ? 0x00010300U : minor == 2 ? 0x00010500U
                                                                               : 0x00010600U;
        for (const auto &sample : cases) {
            compiler->compileGLSL(sample.stage, sample.source);
            if (sample.stage == ShaderStageFlagBit::VERTEX) {
                Attribute attribute;
                attribute.name = "p";
                attribute.format = Format::RG32F;
                attribute.location = 5;
                AttributeList attributes{attribute};
                compiler->compressInputLocations(attributes);
                if (attributes[0].location != 0) {
                    std::fprintf(stderr, "FAIL: engine vertex location compression\n");
                    failed = true;
                }
            }
            const size_t byteCount = compiler->getOutputSize();
            const uint32_t *words = compiler->getOutputData();
            if (byteCount < 20 || !words || words[0] != 0x07230203U || words[1] != expectedVersion) {
                std::fprintf(stderr, "FAIL: engine compiler 1.%d %s\n", minor, sample.name);
                failed = true;
                continue;
            }
            std::ofstream file(directory / (std::string(sample.name) + ".spv"), std::ios::binary);
            file.write(reinterpret_cast<const char *>(words), static_cast<std::streamsize>(byteCount));
            if (!file) {
                failed = true;
            }
            // Module creation is valid only when the actual device admits this target.
            if (context->apiVersionInfo.effectiveDeviceApiVersion >= VK_MAKE_VERSION(1, minor, 0)) {
                VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                info.codeSize = byteCount;
                info.pCode = words;
                VkShaderModule module = VK_NULL_HANDLE;
                const VkResult result = vkCreateShaderModule(device.gpuDevice()->vkDevice, &info, nullptr, &module);
                if (result != VK_SUCCESS || module == VK_NULL_HANDLE) {
                    std::fprintf(stderr, "FAIL: VkShaderModule 1.%d %s, result=%d\n", minor, sample.name, result);
                    failed = true;
                } else {
                    vkDestroyShaderModule(device.gpuDevice()->vkDevice, module, nullptr);
                }
            }
            std::printf("Engine SPIR-V 1.%d %s: 0x%08x\n", minor, sample.name, words[1]);
        }
    }
    compiler->compileGLSL(ShaderStageFlagBit::VERTEX, "#version 450\nvoid main(){invalid syntax;}\n");
    AttributeList invalidAttributes;
    compiler->compressInputLocations(invalidAttributes);
    if (compiler->getOutputSize() != 0) {
        std::fprintf(stderr, "FAIL: failed compilation retained stale SPIR-V\n");
        failed = true;
    }
    // =========================================================================
    // M5-H: real Classic PSO creation evidence via a PFN hook on volk's
    // global vkCreateGraphicsPipelines. The wrapper forwards to the real
    // driver (hook proof, not call-logging) and records each createInfo.
    // Success marker "CLASSIC_PSO: ..." prints only when every real
    // assertion held. Fixed array (2 calls expected); no exceptions cross
    // the C PFN boundary.
    // =========================================================================
    {
        struct PsoCall {
            VkRenderPass rp;
            uint32_t subpass;
            VkSampleCountFlagBits samples;
            const void *pNext;
            VkResult result;
            VkPipeline pipeline;
            VulkanGraphicsPipelineKey key;
            bool keyBuilt = false;
        };
        static PsoCall g_calls[8];
        static uint32_t g_callCount = 0;
        static PFN_vkCreateGraphicsPipelines g_real = nullptr;
        g_callCount = 0;
        for (auto &call : g_calls) call = {};
        g_real = vkCreateGraphicsPipelines;
        vkCreateGraphicsPipelines = [](VkDevice dev, VkPipelineCache cache, uint32_t n,
                                       const VkGraphicsPipelineCreateInfo *infos,
                                       const VkAllocationCallbacks *alloc, VkPipeline *out) -> VkResult {
            if (g_real == nullptr || infos == nullptr || out == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
            // forward FIRST: the proof is a real driver-side creation
            const VkResult r = g_real(dev, cache, n, infos, alloc, out);
            for (uint32_t i = 0; i < n && g_callCount < 8; ++i) {
                auto &record = g_calls[g_callCount++];
                record = {infos[i].renderPass, infos[i].subpass,
                          infos[i].pMultisampleState
                              ? infos[i].pMultisampleState->rasterizationSamples
                              : VK_SAMPLE_COUNT_1_BIT,
                          infos[i].pNext, r, out[i]};
                if (r == VK_SUCCESS && out[i]) record.keyBuilt = buildObservedGraphicsKey(infos[i], record.key);
            }
            if (n > 8) g_callCount = 8; // unexpected volume: flagged as FAIL below
            return r;
        };
        struct HookRestore {
            ~HookRestore() { vkCreateGraphicsPipelines = g_real; }
        } hookRestore;

        // pick two different formats the real device supports for color
        // attachment usage (checked via real format properties, not assumed)
        const Format candidates[] = {Format::RGBA8, Format::RGBA16F, Format::BGRA8,
                                     Format::RGBA32F};
        Format chosen[2] = {Format::UNKNOWN, Format::UNKNOWN};
        {
            uint32_t found = 0;
            for (Format f : candidates) {
                if (found >= 2) break;
                VkFormatProperties props{};
                vkGetPhysicalDeviceFormatProperties(context->physicalDevice,
                                                    mapVkFormat(f, device.gpuDevice()), &props);
                if ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0) {
                    chosen[found++] = f;
                }
            }
            if (found < 2 || chosen[0] == chosen[1]) {
                std::fprintf(stderr, "FAIL: fewer than two distinct color-attachment formats\n");
                failed = true;
            }
        }
        if (chosen[0] != Format::UNKNOWN && chosen[1] != Format::UNKNOWN) {
            // re-target the shared compiler to the EFFECTIVE device API: the
            // SPIR-V loop above leaves it at minor 3 (SPIR-V 1.6), which
            // violates VUID-08737 on forced 1.1/1.2 tiers
            const int effMinor = static_cast<int>(VK_VERSION_MINOR(context->apiVersionInfo.effectiveDeviceApiVersion));
            const int target = effMinor < 1 ? 1 : (effMinor > 3 ? 3 : effMinor);
            SPIRVUtils::getInstance()->destroy();
            SPIRVUtils::getInstance()->initialize(target);

            // GLSL text: the engine prepends "#version 450\n" itself
            ShaderInfo shaderInfo;
            shaderInfo.name = "m5h-pso-evidence";
            ShaderStage vsStage;
            vsStage.stage = ShaderStageFlagBit::VERTEX;
            vsStage.source = "void main(){gl_Position=vec4(0.0,0.0,0.0,1.0);}";
            ShaderStage fsStage;
            fsStage.stage = ShaderStageFlagBit::FRAGMENT;
            fsStage.source = "layout(location=0) out vec4 o;void main(){o=vec4(1.0);}";
            shaderInfo.stages = {vsStage, fsStage};
            Shader *shader = nullptr;
            {
                struct RestoreShaderHook {
                    RestoreShaderHook() {
                        g_shaderEvidence = {};
                        g_shaderEvidence.create = vkCreateShaderModule;
                        vkCreateShaderModule = observeShaderModule;
                    }
                    ~RestoreShaderHook() { vkCreateShaderModule = g_shaderEvidence.create; }
                } restoreShaderHook;
                shader = device.createShader(shaderInfo);
                // gpuShader() compiles lazily; keep the hook alive through verification.
                if (!verifyShaderKeys(shader)) failed = true;
            }
            PipelineLayout *layout = nullptr;
            {
                struct RestoreGraphicsLayoutHook {
                    RestoreGraphicsLayoutHook() {
                        g_graphicsLayoutEvidence = {};
                        g_graphicsLayoutEvidence.create = vkCreatePipelineLayout;
                        vkCreatePipelineLayout = observeGraphicsLayout;
                    }
                    ~RestoreGraphicsLayoutHook() { vkCreatePipelineLayout = g_graphicsLayoutEvidence.create; }
                } hook;
                layout = device.createPipelineLayout(PipelineLayoutInfo{});
            }
            // two subpasses, each referencing its own differently-formatted color
            RenderPassInfo rpInfo;
            rpInfo.colorAttachments.resize(2);
            for (uint32_t i = 0; i < 2; ++i) {
                rpInfo.colorAttachments[i].format = chosen[i];
                rpInfo.colorAttachments[i].loadOp = LoadOp::CLEAR;
                rpInfo.colorAttachments[i].storeOp = StoreOp::STORE;
            }
            rpInfo.subpasses.resize(2);
            rpInfo.subpasses[0].colors = {0};
            rpInfo.subpasses[1].colors = {1};

            RenderPass *rp = nullptr;
            PipelineState *psos[2] = {nullptr, nullptr};
            bool localFail = false;
            if (shader && layout) {
                {
                    // RenderPass2 uses the device's cached core/KHR PFN, not Volk's global variable.
                    struct RestoreRenderPassHook {
                        CCVKGPUDevice *gpu;
                        explicit RestoreRenderPassHook(CCVKGPUDevice *value) : gpu(value) {
                            g_renderPassEvidence = {};
                            g_renderPassEvidence.create = gpu->createRenderPass2;
                            gpu->createRenderPass2 = observeRenderPass;
                        }
                        ~RestoreRenderPassHook() { gpu->createRenderPass2 = g_renderPassEvidence.create; }
                    } restoreRenderPassHook(device.gpuDevice());
                    rp = device.createRenderPass(rpInfo);
                    if (!verifyRenderPassKey(rp)) localFail = true;
                }
                if (rp) {
                    auto *gpuRP = static_cast<CCVKRenderPass *>(rp)->gpuRenderPass();
                    if (gpuRP && gpuRP->renderingMetadata.size() == 2 &&
                        gpuRP->renderingMetadata[0].valid && gpuRP->renderingMetadata[1].valid) {
                        for (uint32_t i = 0; i < 2; ++i) {
                            PipelineStateInfo psoInfo;
                            psoInfo.shader = shader;
                            psoInfo.pipelineLayout = layout;
                            psoInfo.renderPass = rp;
                            psoInfo.subpass = i;
                            psoInfo.bindPoint = PipelineBindPoint::GRAPHICS;
                            psoInfo.depthStencilState.depthTest = 0;
                            psoInfo.depthStencilState.depthWrite = 0;
                            psos[i] = device.createPipelineState(psoInfo);
                        }
                    } else {
                        std::fprintf(stderr, "FAIL: RP metadata missing/invalid\n");
                        localFail = true;
                    }
                } else {
                    std::fprintf(stderr, "FAIL: createRenderPass returned null\n");
                    localFail = true;
                }
            } else {
                std::fprintf(stderr, "FAIL: shader/layout creation returned null\n");
                localFail = true;
            }

            // hook + metadata assertions
            bool callsOk = false;
            bool metadataOk = false;
            bool keysOk = false;
            uint32_t succeeded = 0, forwarded = 0;
            if (!localFail && psos[0] && psos[1]) {
                callsOk = (g_callCount == 2);
                if (!callsOk) {
                    std::fprintf(stderr, "FAIL: PSO hook saw %u calls (want 2)\n", g_callCount);
                } else {
                    auto *gpuRP = static_cast<CCVKRenderPass *>(rp)->gpuRenderPass();
                    for (uint32_t i = 0; i < 2; ++i) {
                        const auto &call = g_calls[i];
                        if (call.result == VK_SUCCESS) ++succeeded;
                        if (call.result == VK_SUCCESS && call.pipeline != VK_NULL_HANDLE) ++forwarded;
                        if (call.result != VK_SUCCESS || call.pipeline == VK_NULL_HANDLE ||
                            call.rp != gpuRP->vkRenderPass || call.subpass != i ||
                            call.samples != VK_SAMPLE_COUNT_1_BIT || call.pNext != nullptr) {
                            callsOk = false;
                            std::fprintf(stderr, "FAIL: PSO[%u] hook assertion (result=%d)\n", i,
                                         call.result);
                        }
                    }
                }
                metadataOk = true;
                keysOk = true;
                auto *gpuRP = static_cast<CCVKRenderPass *>(rp)->gpuRenderPass();
                for (uint32_t i = 0; i < 2 && metadataOk; ++i) {
                    auto *gpuPSO = static_cast<CCVKPipelineState *>(psos[i])->gpuPipelineState();
                    if (!gpuPSO) {
                        metadataOk = false;
                        keysOk = false;
                        break;
                    }
                    if (gpuPSO->vkPipeline == VK_NULL_HANDLE || g_callCount != 2 ||
                        gpuPSO->vkPipeline != g_calls[i].pipeline) {
                        metadataOk = false;
                        std::fprintf(stderr, "FAIL: PSO[%u] saved handle differs from driver output\n", i);
                        break;
                    }
                    std::printf("CLASSIC_PSO_HANDLE: subpass=%u driver=%p stored=%p format=%u\n", i,
                                reinterpret_cast<void *>(g_calls[i].pipeline),
                                reinterpret_cast<void *>(gpuPSO->vkPipeline),
                                static_cast<unsigned>(mapVkFormat(chosen[i], device.gpuDevice())));
                    const auto &slotMeta = gpuRP->renderingMetadata[i];
                    const auto &storedKey = gpuPSO->graphicsContentKey;
                    const auto &observedKey = g_calls[i].key;
                    keysOk = keysOk && g_calls[i].keyBuilt && storedKey.valid() &&
                             !storedKey.dynamicRendering() && storedKey == observedKey &&
                             storedKey.hash() == observedKey.hash() && !storedKey.words().empty() &&
                             storedKey.words().data() != observedKey.words().data() &&
                             storedKey.stages().size() == 2 && observedKey.stages().size() == 2 &&
                             !gpuPSO->dynamicContentKey.valid();
                    if (storedKey.stages().size() == 2 && observedKey.stages().size() == 2) {
                        for (unsigned j = 0; j < 2; ++j)
                            keysOk = keysOk && storedKey.stages()[j].spirvWords().data() != observedKey.stages()[j].spirvWords().data();
                    }
                    const auto &psoMeta = gpuPSO->renderingMetadata;
                    const bool slotGuard = i < gpuRP->renderingMetadata.size() &&
                                           psoMeta.signature.colorFormats.size() == 1 &&
                                           slotMeta.signature.colorFormats.size() == 1;
                    const VkFormat wantColor = mapVkFormat(chosen[i], device.gpuDevice());
                    if (!slotGuard || !psoMeta.valid || !slotMeta.valid ||
                        psoMeta.signature.colorFormats[0] != wantColor ||
                        psoMeta.signature.colorFormats[0] != slotMeta.signature.colorFormats[0] ||
                        psoMeta.signature.depthFormat != slotMeta.signature.depthFormat ||
                        psoMeta.signature.stencilFormat != slotMeta.signature.stencilFormat ||
                        psoMeta.signature.samples != slotMeta.signature.samples ||
                        psoMeta.signature.viewMask != slotMeta.signature.viewMask) {
                        metadataOk = false;
                    }
                }
                if (!metadataOk) {
                    std::fprintf(stderr, "FAIL: PSO metadata mismatch vs RP slot\n");
                }
            }
            if (callsOk && metadataOk && succeeded == 2 && forwarded == 2) {
                std::printf("CLASSIC_PSO: calls=2 forwarded=2 succeeded=2 subpasses=0,1 metadata=OK\n");
            } else {
                failed = true;
            }
            if (keysOk && callsOk && metadataOk && g_calls[0].key != g_calls[1].key &&
                g_calls[0].key.hash() != g_calls[1].key.hash()) {
                std::printf("GRAPHICS_KEYS: pipelines=2 driver=SUCCESS content=EXACT subpasses=OK ownership=OK\n");
            } else {
                std::fprintf(stderr, "FAIL: real graphics pipeline content key evidence\n");
                failed = true;
            }

            // cleanup on every path
            for (auto *pso : psos) {
                if (pso) {
                    pso->destroy();
                    delete pso;
                }
            }
            if (rp) {
                rp->destroy();
                delete rp;
            }
            if (layout) {
                layout->destroy();
                delete layout;
            }
            if (shader) {
                shader->destroy();
                delete shader;
            }
        }
    }

    if (!verifyLayoutKeys(device)) failed = true;
    if (!verifyComputeKeys(device)) failed = true;
    ownedDevice->destroy();
    const uint32_t errors = device.getLastValidationErrorCount();
    std::printf("Cocos Vulkan initialize/shader/destroy: validation errors=%u\n", errors);
    return failed || errors ? 1 : 0;
}
