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

// M7-G2: serial compute-only prewarm using the real engine factory/cache.
// No seed PSO, dispatch, background compilation, or performance claim.
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include "gfx-vulkan/VKDevice.h"
#include "gfx-vulkan/VKPipelineCache.h"
#include "gfx-vulkan/VKPipelinePrewarm.h"
#include "gfx-vulkan/VKPipelineState.h"
#include "gfx-vulkan/VKShader.h"
#include "platform/FileUtils.h"
#include "renderer/GFXDeviceManager.h"

int cocos_main(int, const char **) { return 0; }
using namespace cc::gfx;
using PrewarmQueue = VulkanPipelinePrewarmQueue;
using Store = VulkanPipelineCachePersistence;
using Clock = std::chrono::steady_clock;
namespace {
unsigned failures = 0;
bool incomplete = false;
void check(bool ok, const char *label) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", label);
    }
}
void require(bool ok, const char *label) {
    check(ok, label);
    if (!ok) throw std::runtime_error(label);
}
long long elapsed(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
}
struct Module {
    VkShaderModule handle = VK_NULL_HANDLE;
    std::array<uint32_t, 4096> words{};
    size_t count = 0;
    unsigned destroyed = 0;
};
struct Pipeline {
    VkPipeline handle = VK_NULL_HANDLE;
    VkPipelineCache cache = VK_NULL_HANDLE;
    VkPipelineCreateFlags flags = 0;
    VkStructureType stageType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    VkPipelineShaderStageCreateFlags stageFlags = 0;
    VkShaderStageFlagBits stage = VK_SHADER_STAGE_ALL;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::array<char, 64> entry{};
    long long ns = 0;
    unsigned destroyed = 0;
};
struct Evidence {
    std::array<Module, 2> modules;
    std::array<Pipeline, 2> pipelines;
    unsigned moduleCount = 0, layoutCount = 0, pipelineCount = 0, layoutDestroyed = 0;
    bool invalid = false;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipelineLayoutCreateFlags layoutFlags = 0;
    PFN_vkCreateShaderModule moduleCreate = nullptr;
    PFN_vkDestroyShaderModule moduleDestroy = nullptr;
    PFN_vkCreatePipelineLayout layoutCreate = nullptr;
    PFN_vkDestroyPipelineLayout layoutDestroy = nullptr;
    PFN_vkCreateComputePipelines pipelineCreate = nullptr;
    PFN_vkDestroyPipeline pipelineDestroy = nullptr;
} ev;
VKAPI_ATTR VkResult VKAPI_CALL moduleHook(VkDevice d, const VkShaderModuleCreateInfo *i,
                                          const VkAllocationCallbacks *a, VkShaderModule *o) {
    if (!ev.moduleCreate || !i || !o || i->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO ||
        i->pNext || i->flags || !i->pCode || i->codeSize < 20 || i->codeSize % 4 ||
        i->codeSize / 4 > ev.modules[0].words.size() || ev.moduleCount >= ev.modules.size()) {
        ev.invalid = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const auto r = ev.moduleCreate(d, i, a, o);
    if (r != VK_SUCCESS || !*o) {
        ev.invalid = true;
        return r;
    }
    auto &record = ev.modules[ev.moduleCount++];
    record.handle = *o;
    record.count = i->codeSize / 4;
    std::memcpy(record.words.data(), i->pCode, i->codeSize);
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL layoutHook(VkDevice d, const VkPipelineLayoutCreateInfo *i,
                                          const VkAllocationCallbacks *a, VkPipelineLayout *o) {
    if (!ev.layoutCreate || !i || !o || i->sType != VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO ||
        i->pNext || i->setLayoutCount || i->pushConstantRangeCount || ev.layoutCount) {
        ev.invalid = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const auto r = ev.layoutCreate(d, i, a, o);
    if (r != VK_SUCCESS || !*o) {
        ev.invalid = true;
        return r;
    }
    ev.layout = *o;
    ev.layoutFlags = i->flags;
    ++ev.layoutCount;
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL pipelineHook(VkDevice d, VkPipelineCache cache, uint32_t n,
                                            const VkComputePipelineCreateInfo *i, const VkAllocationCallbacks *a, VkPipeline *o) {
    if (!ev.pipelineCreate || n != 1 || !i || !o || ev.pipelineCount >= ev.pipelines.size() ||
        i->sType != VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO || i->pNext ||
        i->basePipelineHandle || (i->flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) ||
        i->stage.sType != VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO || i->stage.pNext ||
        i->stage.pSpecializationInfo || !i->stage.pName || !i->stage.module || !i->layout) {
        ev.invalid = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    size_t length = 0;
    while (length < ev.pipelines[0].entry.size() && i->stage.pName[length]) ++length;
    if (!length || length >= ev.pipelines[0].entry.size()) {
        ev.invalid = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const auto start = Clock::now();
    const auto r = ev.pipelineCreate(d, cache, n, i, a, o);
    const auto ns = elapsed(start);
    if (r != VK_SUCCESS || !*o) {
        ev.invalid = true;
        return r;
    }
    auto &record = ev.pipelines[ev.pipelineCount++];
    record.handle = *o;
    record.cache = cache;
    record.flags = i->flags;
    record.stageType = i->stage.sType;
    record.stageFlags = i->stage.flags;
    record.stage = i->stage.stage;
    record.module = i->stage.module;
    record.layout = i->layout;
    std::memcpy(record.entry.data(), i->stage.pName, length + 1);
    record.ns = ns;
    return r;
}
VKAPI_ATTR void VKAPI_CALL moduleDestroyHook(VkDevice d, VkShaderModule h, const VkAllocationCallbacks *a) {
    if (!ev.moduleDestroy) {
        ev.invalid = true;
        return;
    }
    for (unsigned x = 0; x < ev.moduleCount; ++x)
        if (ev.modules[x].handle == h && ++ev.modules[x].destroyed != 1) ev.invalid = true;
    ev.moduleDestroy(d, h, a);
}
VKAPI_ATTR void VKAPI_CALL layoutDestroyHook(VkDevice d, VkPipelineLayout h, const VkAllocationCallbacks *a) {
    if (!ev.layoutDestroy) {
        ev.invalid = true;
        return;
    }
    if (h == ev.layout && ++ev.layoutDestroyed != 1) ev.invalid = true;
    ev.layoutDestroy(d, h, a);
}
VKAPI_ATTR void VKAPI_CALL pipelineDestroyHook(VkDevice d, VkPipeline h, const VkAllocationCallbacks *a) {
    if (!ev.pipelineDestroy) {
        ev.invalid = true;
        return;
    }
    for (unsigned x = 0; x < ev.pipelineCount; ++x)
        if (ev.pipelines[x].handle == h && ++ev.pipelines[x].destroyed != 1) ev.invalid = true;
    ev.pipelineDestroy(d, h, a);
}
struct Hooks {
    Hooks() {
        ev = {};
        ev.moduleCreate = vkCreateShaderModule;
        ev.moduleDestroy = vkDestroyShaderModule;
        ev.layoutCreate = vkCreatePipelineLayout;
        ev.layoutDestroy = vkDestroyPipelineLayout;
        ev.pipelineCreate = vkCreateComputePipelines;
        ev.pipelineDestroy = vkDestroyPipeline;
        vkCreateShaderModule = moduleHook;
        vkDestroyShaderModule = moduleDestroyHook;
        vkCreatePipelineLayout = layoutHook;
        vkDestroyPipelineLayout = layoutDestroyHook;
        vkCreateComputePipelines = pipelineHook;
        vkDestroyPipeline = pipelineDestroyHook;
    }
    ~Hooks() {
        vkCreateShaderModule = ev.moduleCreate;
        vkDestroyShaderModule = ev.moduleDestroy;
        vkCreatePipelineLayout = ev.layoutCreate;
        vkDestroyPipelineLayout = ev.layoutDestroy;
        vkCreateComputePipelines = ev.pipelineCreate;
        vkDestroyPipeline = ev.pipelineDestroy;
    }
};
struct CacheEvidence {
    PFN_vkCreatePipelineCache real = nullptr;
    VkPipelineCache handle = VK_NULL_HANDLE;
    size_t initialBytes = 0;
    unsigned calls = 0;
    bool payload = false, invalid = false;
} ce;
VKAPI_ATTR VkResult VKAPI_CALL cacheHook(VkDevice d, const VkPipelineCacheCreateInfo *i,
                                         const VkAllocationCallbacks *a, VkPipelineCache *o) {
    if (!ce.real || !i || !o || i->sType != VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO ||
        i->pNext || i->flags || (i->initialDataSize > 0) != (i->pInitialData != nullptr) || ce.calls) {
        ce.invalid = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    ++ce.calls;
    const auto r = ce.real(d, i, a, o);
    if (r != VK_SUCCESS || !*o) {
        ce.invalid = true;
        return r;
    }
    ce.handle = *o;
    ce.initialBytes = i->initialDataSize;
    ce.payload = i->pInitialData != nullptr;
    return r;
}
struct CacheHook {
    CacheHook() {
        ce = {};
        ce.real = vkCreatePipelineCache;
        vkCreatePipelineCache = cacheHook;
    }
    ~CacheHook() { vkCreatePipelineCache = ce.real; }
};
template <class T>
struct DestroyFacade {
    void operator()(T *p) const {
        if (p) {
            p->destroy();
            delete p;
        }
    }
};
template <class T>
using Owned = std::unique_ptr<T, DestroyFacade<T>>;
struct Inputs {
    std::array<Owned<Shader>, 2> shaders;
    Owned<PipelineLayout> layout;
};
const Module *findModule(VkShaderModule h) {
    for (unsigned i = 0; i < ev.moduleCount; ++i)
        if (ev.modules[i].handle == h) return &ev.modules[i];
    return nullptr;
}
bool makeKey(const VkPipelineShaderStageCreateInfo &stage, VkPipelineCreateFlags flags,
             VulkanComputePipelineKey &out) {
    const auto *module = findModule(stage.module);
    if (!module || ev.layoutCount != 1) return false;
    VulkanShaderStageKey shaderKey;
    VulkanPipelineLayoutKey layoutKey;
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.flags = ev.layoutFlags;
    return VulkanShaderStageKey::build(stage, module->words.data(), module->count, shaderKey) &&
           VulkanPipelineLayoutKey::build(layoutInfo, {}, layoutKey) &&
           VulkanComputePipelineKey::build(flags, shaderKey, layoutKey, out);
}
void checkBudget(CCVKDevice &d) {
    if (d.gpuContext()->validationErrorCount.load() >= 10) {
        incomplete = true;
        std::fprintf(stderr, "INCOMPLETE: validation error budget exhausted\n");
        throw std::runtime_error("validation budget");
    }
}
void run(CCVKDevice &d, CCVKPipelineCache &cache, const std::filesystem::path &path, bool wrong, long long configureNs) {
    auto inputs = std::make_shared<Inputs>();
    inputs->layout.reset(d.createPipelineLayout(PipelineLayoutInfo{}));
    require(inputs->layout && ev.layoutCount == 1 && !ev.invalid, "one actual empty layout");
    std::array<VulkanComputePipelineKey, 2> expected;
    for (unsigned i = 0; i < 2; ++i) {
        ShaderInfo info;
        info.name = "m7g2-prewarm-compute";
        ShaderStage stage;
        stage.stage = ShaderStageFlagBit::COMPUTE;
        stage.source = i ? "layout(local_size_x=2) in;void main(){}" : "layout(local_size_x=1) in;void main(){}";
        info.stages = {stage};
        inputs->shaders[i].reset(d.createShader(info));
        require(inputs->shaders[i] != nullptr, "shader facade created");
        const auto *gpu = static_cast<CCVKShader *>(inputs->shaders[i].get())->gpuShader();
        require(gpu && gpu->gpuStages.size() == 1 && gpu->gpuStages[0].type == ShaderStageFlagBit::COMPUTE && !ev.invalid,
                "lazy compute shader initialized under hook");
        // Recipe defines flags=0 / entry=main, module comes from its real shader.
        VkPipelineShaderStageCreateInfo submitted{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        submitted.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        submitted.module = gpu->gpuStages[0].vkShader;
        submitted.pName = "main";
        require(makeKey(submitted, 0, expected[i]), "manifest key from actual SPIR-V/layout and recipe stage");
    }
    require(ev.moduleCount == 2 && expected[0] != expected[1] && ev.pipelineCount == 0, "distinct recipes without seed PSO");
    checkBudget(d);
    PrewarmQueue q;
    std::array<Owned<PipelineState>, 2> psos;
    std::array<long long, 2> factoryNs{};
    std::array<bool, 2> exact{};
    unsigned factoryCalls = 0;
    bool duplicate = false, queued = false, released = false;
    for (unsigned i = 0; i < 2; ++i) {
        PrewarmQueue::Job job;
        job.label = i ? "compute-local2" : "compute-local1";
        job.expected = expected[i];
        job.create = [&, i, keep = inputs]() -> PrewarmQueue::CreateResult {
            ++factoryCalls;
            checkBudget(d);
            PipelineStateInfo info;
            info.bindPoint = PipelineBindPoint::COMPUTE;
            info.shader = keep->shaders[i].get();
            info.pipelineLayout = keep->layout.get();
            const auto start = Clock::now();
            // No allocation between factory return and acquiring ownership.
            psos[i].reset(d.createPipelineState(info));
            factoryNs[i] = elapsed(start);
            if (!psos[i] || ev.invalid || ev.pipelineCount != i + 1) return {};
            const auto *gpu = static_cast<CCVKPipelineState *>(psos[i].get())->gpuPipelineState();
            const auto &record = ev.pipelines[i];
            VkPipelineShaderStageCreateInfo stage{record.stageType};
            stage.flags = record.stageFlags;
            stage.stage = record.stage;
            stage.module = record.module;
            stage.pName = record.entry.data();
            VulkanComputePipelineKey actual;
            exact[i] = makeKey(stage, record.flags, actual) && gpu && record.handle == gpu->vkPipeline &&
                       record.cache == cache.getHandle() && record.layout == ev.layout &&
                       actual == expected[i] && actual.hash() == expected[i].hash() &&
                       actual == gpu->computeContentKey && actual.hash() == gpu->computeContentKey.hash();
            check(exact[i], "actual root / manifest / production full keys and handles match");
            checkBudget(d);
            return {exact[i], (wrong && i == 0) ? PrewarmQueue::Key{} : PrewarmQueue::Key{std::move(actual)}};
        };
        require(q.enqueue(job) == PrewarmQueue::EnqueueStatus::Added, "manifest entry published");
        if (i == 0) {
            job.label = "same-key-other-label";
            duplicate = q.enqueue(job) == PrewarmQueue::EnqueueStatus::Duplicate;
            require(duplicate, "exact key duplicate suppressed");
        }
    }
    std::weak_ptr<Inputs> watch = inputs;
    inputs.reset();
    queued = !watch.expired();
    require(queued, "only queued closures keep inputs alive");
    const auto beforeCreates = ev.pipelineCount;
    require(q.size() == 2 && beforeCreates == 0 && factoryCalls == 0, "manifest published before any factory");
    std::printf("prewarm manifest: entries=%zu modules=%u layouts=%u duplicate=%u beforeCreates=%u keys=DISTINCT\n",
                q.size(), ev.moduleCount, ev.layoutCount, unsigned(duplicate), beforeCreates);
    const auto first = q.drain(1);
    checkBudget(d);
    const auto second = q.drain(1);
    checkBudget(d);
    const auto replay = q.drain(8);
    checkBudget(d);
    require(first.attempted == 1 && first.ready == (wrong ? 0u : 1u) && first.failed == (wrong ? 1u : 0u) && !first.busy &&
                second.attempted == 1 && second.ready == 1 && second.failed == 0 && !second.busy &&
                replay.attempted == 0 && replay.ready == 0 && replay.failed == 0 && !replay.busy && q.size() == 2,
            "FIFO budget and no retry reports");
    check(q.entries()[0].state == (wrong ? PrewarmQueue::JobState::Failed : PrewarmQueue::JobState::Ready) &&
              q.entries()[1].state == PrewarmQueue::JobState::Ready,
          "queue states reflect creator result");
    const auto totalFailed = first.failed + second.failed;
    check(totalFailed == 0, "prewarm rejected creator key"); // Real negative: one failure, not a forced exit flag.
    std::printf("prewarm queue: attempted=%zu ready=%zu failed=%zu replay=%zu busy=%u\n",
                first.attempted + second.attempted, first.ready + second.ready, totalFailed, replay.attempted,
                unsigned(first.busy || second.busy || replay.busy));
    require(factoryCalls == 2 && ev.pipelineCount == 2 && exact[0] && exact[1] && !ev.invalid, "two real exact factory creations");
    std::printf("prewarm factory: calls=%u driverSuccesses=%u cache=EXACT keys=EXACT\n", factoryCalls, ev.pipelineCount);
    cache.saveCache();
    Store::Bytes payload;
    const bool valid = Store::read(path, Store::identity(d.gpuContext()->physicalDeviceProperties), payload);
    require(valid && !payload.empty() && !cache.isDirty(), "saved cache validated and dirty cleared");
    std::printf("prewarm saved: bytes=%zu validated=%u dirty=%u\n", payload.size(), unsigned(valid), unsigned(cache.isDirty()));
    require(q.clear(), "clear queue");
    released = watch.expired();
    require(released, "last closure releases input facades");
    std::printf("prewarm ownership: queued=%u released=%u\n", unsigned(queued), unsigned(released));
    std::printf("prewarm timing ns: configure=%lld factory1=%lld factory2=%lld driver1=%lld driver2=%lld\n",
                configureNs, factoryNs[0], factoryNs[1], ev.pipelines[0].ns, ev.pipelines[1].ns);
    // PSOs retain their GPU inputs until their owning facades are destroyed here.
}
} // namespace
int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::string phase;
    bool wrong = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--phase" && phase.empty() && i + 1 < argc) {
            phase = argv[++i];
            if (phase != "cold" && phase != "restored") {
                std::fprintf(stderr, "FAIL: unknown phase\n");
                return 1;
            }
        } else if (arg == "--wrong-key" && !wrong)
            wrong = true;
        else {
            std::fprintf(stderr, "FAIL: unknown, duplicate or incomplete argument\n");
            return 1;
        }
    }
    if (phase.empty()) {
        std::fprintf(stderr, "FAIL: --phase required\n");
        return 1;
    }
    std::unique_ptr<cc::FileUtils> files;
    Owned<Device> facade;
    try {
        const auto directory = std::filesystem::current_path() / (wrong ? "prewarm-cwd-wrong" : "prewarm-cwd");
        const auto path = directory / "pipeline-prewarm.bin";
        const bool exists = std::filesystem::exists(path);
        if ((phase == "cold" && exists) || (phase == "restored" && !exists)) {
            std::fprintf(stderr, "FAIL: %s cache-file precondition\n", phase.c_str());
            return 1;
        }
        std::filesystem::create_directories(directory);
        std::filesystem::current_path(directory);
        files.reset(cc::createFileUtils());
        require(files != nullptr, "FileUtils created");
        files->setWritablePath(directory.generic_string() + "/");
        facade.reset(DeviceManager::create());
        require(facade && facade->getGfxAPI() == API::VULKAN && CCVKDevice::getInstance(), "Vulkan device initialized");
        auto &d = *CCVKDevice::getInstance();
        require(d.gpuContext() && d.gpuDevice(), "live Vulkan context");
        const bool validation = d.gpuContext()->validationEnabled && d.gpuContext()->bestPracticesValidationEnabled && d.gpuContext()->synchronizationValidationEnabled;
        std::printf("prewarm validation: %s\n", validation ? "ENABLED" : "UNAVAILABLE");
        std::printf("prewarm mode: phase=%s wrongKey=%u\n", phase.c_str(), unsigned(wrong));
        Hooks hooks;
        try {
            require(validation, "validation triad required");
            checkBudget(d);
            long long configureNs;
            {
                CacheHook observe;
                const auto start = Clock::now();
                const bool configured = d.configurePipelineCacheForPrewarm(path.generic_string());
                configureNs = elapsed(start);
                require(configured && ce.calls == 1 && !ce.invalid && ce.handle &&
                            d.pipelineCache() && ce.handle == d.pipelineCache()->getHandle(),
                        "configured cache observed from actual driver");
            }
            auto &cache = *d.pipelineCache();
            const bool loaded = cache.loadedFromDisk();
            require(!cache.isDirty() && loaded == (phase == "restored") &&
                        (phase == "cold" ? (ce.initialBytes == 0 && !ce.payload) : (ce.initialBytes > 0 && ce.payload)),
                    "cold/restored input payload observed");
            std::printf("prewarm cache-input: calls=%u loaded=%u initialBytes=%zu payload=%u\n",
                        ce.calls, unsigned(loaded), ce.initialBytes, unsigned(ce.payload));
            run(d, cache, path, wrong, configureNs);
        } catch (const std::exception &error) {
            check(false, error.what());
        } catch (...) {
            check(false, "unexpected experiment exception");
        }
        const auto live = d.gpuContext()->validationErrorCount.load();
        check(live == 0, "live validation clean before device teardown");
        facade->destroy(); // still inside hook lifetime; keep facade object alive for cached counter
        const auto post = d.getLastValidationErrorCount();
        unsigned destroyedP = 0, destroyedM = 0;
        for (unsigned i = 0; i < ev.pipelineCount; ++i) {
            destroyedP += ev.pipelines[i].destroyed;
            check(ev.pipelines[i].destroyed == 1, "each pipeline destroyed once");
        }
        for (unsigned i = 0; i < ev.moduleCount; ++i) {
            destroyedM += ev.modules[i].destroyed;
            check(ev.modules[i].destroyed == 1, "each module destroyed once");
        }
        check(ev.pipelineCount == 2 && ev.moduleCount == 2 && ev.layoutCount == 1 && ev.layoutDestroyed == 1 && !ev.invalid, "full retirement evidence");
        std::printf("prewarm retirement: pipelines=%u/%u modules=%u/%u layouts=%u/%u\n",
                    destroyedP, ev.pipelineCount, destroyedM, ev.moduleCount, ev.layoutDestroyed, ev.layoutCount);
        check(post == 0, "post-destroy validation clean");
        const bool clean = !failures && !live && !post && !incomplete;
        std::printf("prewarm %s completed: failures=%u liveErrors=%u postErrors=%u outcome=%s\n",
                    phase.c_str(), failures, live, post, clean ? "CLEAN" : "FAILED");
        delete facade.release(); // Delete the actual facade/proxy, without a second destroy call.
        return clean ? 0 : 1;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: setup exception: %s\n", error.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "FAIL: setup exception\n");
        return 1;
    }
}
