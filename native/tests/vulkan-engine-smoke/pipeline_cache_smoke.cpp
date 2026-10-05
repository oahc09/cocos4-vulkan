// M7-A: real driver consumption of the engine cache wrapper, isolated files.
#include "gfx-base/SPIRVUtils.h"
#include "gfx-vulkan/VKDevice.h"
#include "gfx-vulkan/VKPipelineCache.h"
#include "platform/FileUtils.h"
#include "renderer/GFXDeviceManager.h"

#include <cstdio>
#include <filesystem>
#include <memory>

int cocos_main(int, const char **) { return 0; }
void cocos_destroy() {}

using namespace cc::gfx;
using Store = VulkanPipelineCachePersistence;
static int failures = 0;
static unsigned cachedCreates = 0, coldCreates = 0, rejected = 0;
static bool rejectCached = false;
static PFN_vkCreatePipelineCache realCreate = nullptr;
static PFN_vkGetPipelineCacheData realGet = nullptr;
static void check(bool ok, const char *message) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}
static VKAPI_ATTR VkResult VKAPI_CALL createHook(VkDevice device, const VkPipelineCacheCreateInfo *info,
                                                 const VkAllocationCallbacks *allocator, VkPipelineCache *cache) {
    if (!realCreate || !info || !cache) {
        check(false, "invalid cache create hook input");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (info->initialDataSize) {
        ++cachedCreates;
        if (rejectCached) {
            ++rejected;
            *cache = VK_NULL_HANDLE;
            return VK_ERROR_UNKNOWN;
        }
    } else {
        ++coldCreates;
        check(info->pInitialData == nullptr, "cold cache uses null payload");
    }
    return realCreate(device, info, allocator, cache);
}
static VKAPI_ATTR VkResult VKAPI_CALL failGet(VkDevice, VkPipelineCache, size_t *, void *) {
    return VK_ERROR_OUT_OF_HOST_MEMORY; // injected BEFORE driver, not a device failure
}
struct Hooks {
    Hooks() {
        realCreate = vkCreatePipelineCache;
        realGet = vkGetPipelineCacheData;
        vkCreatePipelineCache = createHook;
    }
    ~Hooks() {
        vkCreatePipelineCache = realCreate;
        vkGetPipelineCacheData = realGet;
    }
};

int main(int argc, char **) {
    if (argc != 1) {
        std::fprintf(stderr, "FAIL: no arguments accepted\n");
        return 1;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::unique_ptr<cc::FileUtils> files(cc::createFileUtils());
    const auto path = std::filesystem::current_path() / "pipeline-cache-suite.bin";
    files->setWritablePath(std::filesystem::current_path().generic_string() + "/");
    std::unique_ptr<Device> facade(DeviceManager::create());
    if (!facade || facade->getGfxAPI() != API::VULKAN) {
        if (facade) facade->destroy();
        std::fprintf(stderr, "FAIL: Vulkan initialization\n");
        return 1;
    }
    auto &engine = *CCVKDevice::getInstance();
    auto *context = engine.gpuContext();
    const auto properties = context->physicalDeviceProperties;
    const auto identity = Store::identity(properties);
    const auto device = engine.gpuDevice()->vkDevice;
    const bool validation = context->validationEnabled && context->bestPracticesValidationEnabled && context->synchronizationValidationEnabled;
    std::printf("cache validation: %s\n", validation ? "ENABLED" : "UNAVAILABLE");
    check(validation, "validation triad required");
    check(vkCreatePipelineCache && vkGetPipelineCacheData && vkCreateShaderModule && vkCreatePipelineLayout &&
              vkCreateComputePipelines && vkDestroyPipeline && vkDestroyShaderModule && vkDestroyPipelineLayout,
          "required pipeline entrypoints");

    VkShaderModule shader = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    Store::Bytes original;
    const ccstd::string filename = path.generic_string().c_str();
    if (!failures) {
        auto *compiler = SPIRVUtils::getInstance();
        compiler->compileGLSL(ShaderStageFlagBit::COMPUTE, "#version 450\nlayout(local_size_x=1) in; void main() {}\n");
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = compiler->getOutputSize();
        info.pCode = compiler->getOutputData();
        check(info.codeSize > 0, "compute shader compiled");
        if (info.codeSize) check(vkCreateShaderModule(device, &info, nullptr, &shader) == VK_SUCCESS && shader, "real shader module");
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout) == VK_SUCCESS && layout, "real empty pipeline layout");
    }
    if (!failures) {
        Hooks hooks;
        // First cold creation + real compute PSO populates the local engine cache.
        {
            CCVKPipelineCache cache;
            cache.init(device, properties, true, filename);
            check(cache.getHandle() && !cache.loadedFromDisk(), "isolated initial cold cache");
            if (cache.getHandle()) {
                VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                info.stage.module = shader;
                info.stage.pName = "main";
                info.layout = layout;
                VkPipeline pipeline = VK_NULL_HANDLE;
                check(vkCreateComputePipelines(device, cache.getHandle(), 1, &info, nullptr, &pipeline) == VK_SUCCESS && pipeline,
                      "real compute pipeline with local cache");
                if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
                cache.setDirty();
                cache.saveCache();
                check(!cache.isDirty() && Store::read(path, identity, original), "checkpoint published validated driver data");
                vkGetPipelineCacheData = failGet;
                cache.setDirty();
                cache.saveCache();
                Store::Bytes after;
                check(cache.isDirty() && Store::read(path, identity, after) && after == original,
                      "retrieval failure preserves old file and dirty state");
                vkGetPipelineCacheData = realGet;
                cache.saveCache();
                check(!cache.isDirty(), "retry successfully clears dirty state");
            }
        }
        check(coldCreates == 1, "exact cold creation count");
        {
            CCVKPipelineCache cache;
            cache.init(device, properties, true, filename);
            check(cache.getHandle() && cache.loadedFromDisk() && cachedCreates == 1, "actual driver received restored payload");
        }
        std::printf("cache-stage restored: realPipeline=1 cachedCreates=%u preserved=1\n", cachedCreates);
        // Check shutdown save, with no explicit save in this scope.
        {
            CCVKPipelineCache cache;
            cache.init(device, properties, true, filename);
            cache.setDirty();
        }
        check(Store::read(path, identity, original), "shutdown save leaves valid payload");

        for (unsigned kind = 0; kind < 4; ++kind) {
            Store::Bytes damaged;
            if (!Store::encode(original, identity, damaged)) {
                check(false, "valid fixture required before corruption mutation");
                break;
            }
            if (kind == 0) damaged.back() ^= 1;
            if (kind == 1) damaged.resize(10);
            if (kind == 2) Store::write32(damaged.data() + 8, identity.driver + 1);
            if (kind == 3) Store::write32(damaged.data() + 4, 99);
            {
                std::ofstream out(path, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char *>(damaged.data()), static_cast<std::streamsize>(damaged.size()));
                check(bool(out), "write actual damaged fixture");
            }
            const auto before = cachedCreates;
            CCVKPipelineCache cache;
            cache.init(device, properties, true, filename);
            check(cache.getHandle() && !cache.loadedFromDisk() && cachedCreates == before, "corrupt/foreign cache never sent to driver");
        }
        std::printf("cache-stage cold-recovery: cases=4 coldCreates=%u\n", coldCreates);
        check(Store::write(path, identity, original), "restore valid driver cache for rejection test");
        rejectCached = true;
        {
            CCVKPipelineCache cache;
            cache.init(device, properties, true, filename);
            check(cache.getHandle() && !cache.loadedFromDisk() && rejected == 1 && coldCreates == 6, "driver rejection retried once with empty data");
        }
        rejectCached = false;
        std::printf("cache-stage rejected: rejected=%u coldCreates=%u\n", rejected, coldCreates);
    }
    if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
    if (shader) vkDestroyShaderModule(device, shader, nullptr);
    const auto live = context->validationErrorCount.load();
    facade->destroy();
    const auto post = engine.getLastValidationErrorCount();
    std::printf("cache-suite completed: failures=%d liveErrors=%u postErrors=%u\n", failures, live, post);
    return failures || live || post ? 1 : 0;
}
