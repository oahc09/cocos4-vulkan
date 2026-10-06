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

#include "RealProjectPrewarm.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "base/threading/MessageQueue.h"
#include "gfx-agent/DeviceAgent.h"
#include "gfx-vulkan/VKDevice.h"
#include "gfx-vulkan/VKPipelineCache.h"
#include "gfx-vulkan/VKPipelinePrewarm.h"
#include "gfx-vulkan/VKPipelineState.h"
#include "gfx-vulkan/VKShader.h"

namespace cc::vulkan_test {
namespace {
using namespace gfx;
using Store = VulkanPipelineCachePersistence;
using Queue = VulkanPipelinePrewarmQueue;

struct Module {
    VkShaderModule handle = VK_NULL_HANDLE;
    std::array<uint32_t, 4096> words{};
    size_t count = 0;
    unsigned destroyed = 0;
};
struct Pipeline {
    VkPipeline handle = VK_NULL_HANDLE;
    unsigned destroyed = 0;
    VulkanComputePipelineKey actual;
};
struct ScenePipeline {
    VkPipeline handle = VK_NULL_HANDLE;
    unsigned destroyed = 0;
};
struct State {
    bool prepared = false, enabled = false, started = false, boot = false;
    bool configuring = false, installed = false, loaded = false, inputExact = false;
    bool queuedOwner = false, releasedOwner = false, duplicate = false;
    unsigned failures = 0, modules = 0, computes = 0, layouts = 0;
    unsigned cacheCalls = 0, graphics = 0, cachedGraphics = 0, devicesDestroyed = 0;
    unsigned liveErrors = 0, deviceErrors = 0, layoutDestroyed = 0;
    unsigned attempted = 0, ready = 0, failed = 0;
    size_t initialBytes = 0;
    std::string path, phase;
    Store::Identity identity;
    Store::Bytes expectedInput;
    VkDevice device = VK_NULL_HANDLE;
    VkPipelineCache cache = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipelineLayoutCreateFlags layoutFlags = 0;
    CCVKGPUContext *context = nullptr; // Borrowed only until vkDestroyDevice returns.
    std::thread::id renderThread;
    std::array<Module, 2> module;
    std::array<Pipeline, 2> compute;
    std::array<ScenePipeline, 4096> scene;
    PFN_vkCreatePipelineCache createCache = nullptr;
    PFN_vkCreateShaderModule createModule = nullptr;
    PFN_vkDestroyShaderModule destroyModule = nullptr;
    PFN_vkCreatePipelineLayout createLayout = nullptr;
    PFN_vkDestroyPipelineLayout destroyLayout = nullptr;
    PFN_vkCreateComputePipelines createCompute = nullptr;
    PFN_vkCreateGraphicsPipelines createGraphics = nullptr;
    PFN_vkDestroyPipeline destroyPipeline = nullptr;
    PFN_vkDestroyDevice destroyDevice = nullptr;
} s;

void check(bool ok, const char *label) noexcept {
    if (!ok) {
        ++s.failures;
        CC_LOG_ERROR("project-prewarm failure: %s", label);
    }
}
void require(bool ok, const char *label) {
    check(ok, label);
    if (!ok) throw std::runtime_error(label);
}
void checkThread() noexcept {
    check(std::this_thread::get_id() == s.renderThread, "render-thread affinity");
}

bool keyFromActual(const VkPipelineShaderStageCreateInfo &stage, VkPipelineCreateFlags flags,
                   VkPipelineLayout layout, VulkanComputePipelineKey &out) {
    const Module *module = nullptr;
    for (unsigned n = 0; n < s.modules; ++n)
        if (s.module[n].handle == stage.module) module = &s.module[n];
    if (!module || layout != s.layout || s.layouts != 1) return false;
    VulkanShaderStageKey shader;
    VulkanPipelineLayoutKey layoutKey;
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.flags = s.layoutFlags;
    return VulkanShaderStageKey::build(stage, module->words.data(), module->count, shader) &&
           VulkanPipelineLayoutKey::build(info, {}, layoutKey) &&
           VulkanComputePipelineKey::build(flags, shader, layoutKey, out);
}

VKAPI_ATTR VkResult VKAPI_CALL cacheHook(VkDevice d, const VkPipelineCacheCreateInfo *i,
                                        const VkAllocationCallbacks *a, VkPipelineCache *o) {
    if (!s.createCache) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = s.createCache(d, i, a, o);
    if (s.configuring) {
        checkThread();
        ++s.cacheCalls;
        const bool valid = d == s.device && i && o && result == VK_SUCCESS && *o &&
                           i->sType == VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO && !i->pNext;
        check(valid, "actual cache create successful");
        if (valid) {
            s.cache = *o;
            s.initialBytes = i->initialDataSize;
            s.inputExact = s.expectedInput.size() == i->initialDataSize &&
                           (i->initialDataSize ? i->pInitialData &&
                                                    std::memcmp(i->pInitialData, s.expectedInput.data(), i->initialDataSize) == 0
                                              : !i->pInitialData);
        }
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL moduleHook(VkDevice d, const VkShaderModuleCreateInfo *i,
                                         const VkAllocationCallbacks *a, VkShaderModule *o) {
    if (!s.createModule) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = s.createModule(d, i, a, o);
    if (s.boot) {
        checkThread();
        const bool valid = d == s.device && i && o && result == VK_SUCCESS && *o && !i->pNext &&
                           i->codeSize >= 20 && i->codeSize % 4 == 0 && i->pCode &&
                           i->codeSize / 4 <= s.module[0].words.size() && s.modules < s.module.size();
        check(valid, "bounded actual SPIR-V module");
        if (valid) {
            auto &m = s.module[s.modules++];
            m.handle = *o;
            m.count = i->codeSize / 4;
            std::memcpy(m.words.data(), i->pCode, i->codeSize);
        }
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL layoutHook(VkDevice d, const VkPipelineLayoutCreateInfo *i,
                                         const VkAllocationCallbacks *a, VkPipelineLayout *o) {
    if (!s.createLayout) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = s.createLayout(d, i, a, o);
    if (s.boot) {
        checkThread();
        const bool valid = d == s.device && i && o && result == VK_SUCCESS && *o &&
                           i->sType == VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO && !i->pNext &&
                           !i->setLayoutCount && !i->pushConstantRangeCount && s.layouts == 0;
        check(valid, "one actual empty pipeline layout");
        if (valid) {
            ++s.layouts;
            s.layout = *o;
            s.layoutFlags = i->flags;
        }
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL computeHook(VkDevice d, VkPipelineCache cache, uint32_t count,
                                          const VkComputePipelineCreateInfo *i, const VkAllocationCallbacks *a,
                                          VkPipeline *o) {
    if (!s.createCompute) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = s.createCompute(d, cache, count, i, a, o);
    if (s.boot) {
        checkThread();
        const bool valid = d == s.device && cache == s.cache && count == 1 && i && o &&
                           result == VK_SUCCESS && *o && s.computes < s.compute.size() &&
                           i->sType == VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO && !i->pNext &&
                           !i->basePipelineHandle && !(i->flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT);
        check(valid, "actual compute root/cache");
        if (valid) {
            auto &p = s.compute[s.computes++];
            p.handle = *o;
            try {
                check(keyFromActual(i->stage, i->flags, i->layout, p.actual), "actual compute key independently rebuilt");
            } catch (...) {
                check(false, "compute hook allocation");
            }
        }
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL graphicsHook(VkDevice d, VkPipelineCache cache, uint32_t count,
                                           const VkGraphicsPipelineCreateInfo *i, const VkAllocationCallbacks *a,
                                           VkPipeline *o) {
    if (!s.createGraphics) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = s.createGraphics(d, cache, count, i, a, o);
    checkThread();
    check(!s.boot, "scene pipelines follow prewarm");
    check(d == s.device && result == VK_SUCCESS && i && o, "scene pipeline create succeeds");
    if (o && result == VK_SUCCESS) {
        for (uint32_t n = 0; n < count; ++n) {
            check(o[n] != VK_NULL_HANDLE && s.graphics < s.scene.size(), "bounded scene pipeline evidence");
            if (o[n] && s.graphics < s.scene.size()) {
                s.scene[s.graphics++].handle = o[n];
                if (cache == s.cache) ++s.cachedGraphics;
                else check(false, "scene consumes configured cache");
            }
        }
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL moduleDestroyHook(VkDevice d, VkShaderModule h, const VkAllocationCallbacks *a) {
    if (!s.destroyModule) { check(false, "module destroy PFN"); return; }
    s.destroyModule(d, h, a);
    // Ignore later handle reuse by scene modules; only an outstanding recipe is counted.
    for (unsigned n = 0; n < s.modules; ++n)
        if (s.module[n].handle == h && s.module[n].destroyed == 0) { ++s.module[n].destroyed; break; }
}
VKAPI_ATTR void VKAPI_CALL layoutDestroyHook(VkDevice d, VkPipelineLayout h, const VkAllocationCallbacks *a) {
    if (!s.destroyLayout) { check(false, "layout destroy PFN"); return; }
    s.destroyLayout(d, h, a);
    if (h == s.layout && !s.layoutDestroyed) ++s.layoutDestroyed;
}
VKAPI_ATTR void VKAPI_CALL pipelineDestroyHook(VkDevice d, VkPipeline h, const VkAllocationCallbacks *a) {
    if (!s.destroyPipeline) { check(false, "pipeline destroy PFN"); return; }
    s.destroyPipeline(d, h, a);
    for (unsigned n = 0; n < s.computes; ++n)
        if (s.compute[n].handle == h && !s.compute[n].destroyed) { ++s.compute[n].destroyed; return; }
    for (unsigned n = 0; n < s.graphics; ++n)
        if (s.scene[n].handle == h && !s.scene[n].destroyed) { ++s.scene[n].destroyed; return; }
}
VKAPI_ATTR void VKAPI_CALL deviceDestroyHook(VkDevice d, const VkAllocationCallbacks *a) {
    if (!s.destroyDevice) { check(false, "device destroy PFN"); return; }
    checkThread();
    check(d == s.device && s.devicesDestroyed == 0, "expected device shutdown");
    s.liveErrors = s.context->validationErrorCount.load();
    s.destroyDevice(d, a);
    // Context remains live here; the backend destroys it after this call returns.
    s.deviceErrors = s.context->validationErrorCount.load();
    s.context = nullptr;
    ++s.devicesDestroyed;
}

void install() {
    s.createCache = vkCreatePipelineCache;
    s.createModule = vkCreateShaderModule;
    s.destroyModule = vkDestroyShaderModule;
    s.createLayout = vkCreatePipelineLayout;
    s.destroyLayout = vkDestroyPipelineLayout;
    s.createCompute = vkCreateComputePipelines;
    s.createGraphics = vkCreateGraphicsPipelines;
    s.destroyPipeline = vkDestroyPipeline;
    s.destroyDevice = vkDestroyDevice;
    require(s.createCache && s.createModule && s.destroyModule && s.createLayout && s.destroyLayout &&
                s.createCompute && s.createGraphics && s.destroyPipeline && s.destroyDevice,
            "all diagnostic hook PFNs present");
    vkCreatePipelineCache = cacheHook;
    vkCreateShaderModule = moduleHook;
    vkDestroyShaderModule = moduleDestroyHook;
    vkCreatePipelineLayout = layoutHook;
    vkDestroyPipelineLayout = layoutDestroyHook;
    vkCreateComputePipelines = computeHook;
    vkCreateGraphicsPipelines = graphicsHook;
    vkDestroyPipeline = pipelineDestroyHook;
    vkDestroyDevice = deviceDestroyHook;
    s.installed = true;
}
void restore() noexcept {
    if (!s.installed) return;
    vkCreatePipelineCache = s.createCache;
    vkCreateShaderModule = s.createModule;
    vkDestroyShaderModule = s.destroyModule;
    vkCreatePipelineLayout = s.createLayout;
    vkDestroyPipelineLayout = s.destroyLayout;
    vkCreateComputePipelines = s.createCompute;
    vkCreateGraphicsPipelines = s.createGraphics;
    vkDestroyPipeline = s.destroyPipeline;
    vkDestroyDevice = s.destroyDevice;
    s.installed = false;
}

template <class T> struct DestroyFacade {
    void operator()(T *p) const { if (p) { p->destroy(); delete p; } }
};
template <class T> using Owned = std::unique_ptr<T, DestroyFacade<T>>;
struct Inputs {
    std::array<Owned<Shader>, 2> shaders;
    Owned<PipelineLayout> layout;
};

void runPrewarm(CCVKDevice &d) {
    auto inputs = std::make_shared<Inputs>();
    inputs->layout.reset(d.createPipelineLayout(PipelineLayoutInfo{}));
    require(inputs->layout && s.layouts == 1, "layout factory");
    std::array<VulkanComputePipelineKey, 2> expected;
    for (unsigned n = 0; n < 2; ++n) {
        ShaderInfo info;
        info.name = "real-project-serial-prewarm";
        ShaderStage stage;
        stage.stage = ShaderStageFlagBit::COMPUTE;
        stage.source = n ? "layout(local_size_x=2) in;void main(){}" : "layout(local_size_x=1) in;void main(){}";
        info.stages = {stage};
        inputs->shaders[n].reset(d.createShader(info));
        require(inputs->shaders[n] != nullptr, "shader facade");
        const auto *gpu = static_cast<CCVKShader *>(inputs->shaders[n].get())->gpuShader();
        require(gpu && gpu->gpuStages.size() == 1, "real lazy compute shader");
        VkPipelineShaderStageCreateInfo actual{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        actual.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        actual.module = gpu->gpuStages[0].vkShader;
        actual.pName = "main";
        require(keyFromActual(actual, 0, s.layout, expected[n]), "manifest from real SPIR-V and layout");
    }
    require(expected[0] != expected[1] && !s.computes && !s.graphics, "distinct manifest before any PSO");
    std::array<Owned<PipelineState>, 2> psos;
    Queue queue;
    std::weak_ptr<Inputs> weak = inputs;
    for (unsigned n = 0; n < 2; ++n) {
        Queue::Job job;
        job.label = n ? "compute-local2" : "compute-local1";
        job.expected = expected[n];
        job.create = [&, n, keep = inputs]() -> Queue::CreateResult {
            PipelineStateInfo info;
            info.bindPoint = PipelineBindPoint::COMPUTE;
            info.shader = keep->shaders[n].get();
            info.pipelineLayout = keep->layout.get();
            psos[n].reset(d.createPipelineState(info));
            require(psos[n] != nullptr && s.computes == n + 1, "real factory reaches driver");
            const auto *gpu = static_cast<CCVKPipelineState *>(psos[n].get())->gpuPipelineState();
            const auto &record = s.compute[n];
            require(gpu && gpu->vkPipeline == record.handle && record.actual == expected[n] &&
                        record.actual.hash() == expected[n].hash() && gpu->computeContentKey == record.actual,
                    "manifest, driver, production key agree");
            if (s.phase == "wrong-key" && n == 0) {
                VulkanComputePipelineKey wrong;
                require(VulkanComputePipelineKey::build(record.actual.flags() ^ 1U,
                                                       record.actual.shader(), record.actual.layout(), wrong),
                        "negative key valid");
                return {true, wrong};
            }
            return {true, record.actual};
        };
        require(queue.enqueue(job) == Queue::EnqueueStatus::Added, "enqueue recipe");
        if (n == 0) {
            job.label = "same-key-another-label";
            s.duplicate = queue.enqueue(job) == Queue::EnqueueStatus::Duplicate;
            require(s.duplicate && !s.computes, "duplicate rejected before factory");
        }
    }
    inputs.reset();
    s.queuedOwner = !weak.expired();
    require(s.queuedOwner, "queue closure owns inputs");
    for (unsigned n = 0; n < 2; ++n) {
        const auto report = queue.drain(1);
        require(report.attempted == 1 && !report.busy, "serial drain budget");
        s.attempted += static_cast<unsigned>(report.attempted);
        s.ready += static_cast<unsigned>(report.ready);
        s.failed += static_cast<unsigned>(report.failed);
    }
    require(queue.drain(8).attempted == 0, "finished recipes do not rerun");
    require(s.ready == (s.phase == "wrong-key" ? 1U : 2U) &&
                s.failed == (s.phase == "wrong-key" ? 1U : 0U), "queue exact-key outcome");
    for (auto &p : psos) p.reset();
    require(queue.clear(), "queue clear");
    s.releasedOwner = weak.expired();
    require(s.releasedOwner, "closures release facade inputs");
    CC_LOG_INFO("project-prewarm manifest: keys=2 distinct=1 duplicate=1 beforeCreates=0");
    CC_LOG_INFO("project-prewarm queue: attempted=%u ready=%u failed=%u creates=%u", s.attempted, s.ready, s.failed, s.computes);
    CC_LOG_INFO("project-prewarm ownership: queued=%u released=%u", unsigned(s.queuedOwner), unsigned(s.releasedOwner));
}

void startOnRenderThread() noexcept {
    s.started = true;
    s.renderThread = std::this_thread::get_id();
    try {
        auto *d = CCVKDevice::getInstance();
        require(d && d->gpuDevice() && d->gpuContext() && d->pipelineCache(), "live Vulkan backend");
        s.device = d->gpuDevice()->vkDevice;
        s.context = d->gpuContext();
        require(s.context->validationEnabled && s.context->synchronizationValidationEnabled &&
                    s.context->bestPracticesValidationEnabled, "validation triad enabled");
        CC_LOG_INFO("project-prewarm validation: ENABLED");
        require(!d->pipelineCache()->isDirty(), "cache config before first business PSO");
        s.identity = Store::identity(s.context->physicalDeviceProperties);
        CC_LOG_INFO("project-prewarm identity: driver=%u vendor=%u device=%u", s.identity.driver, s.identity.vendor, s.identity.device);
        const bool exists = std::filesystem::exists(s.path);
        const bool valid = Store::read(s.path, s.identity, s.expectedInput);
        if (s.phase == "restored") require(exists && valid && !s.expectedInput.empty(), "restored source validated");
        else if (s.phase == "rejected") require(exists && !valid && s.expectedInput.empty(), "bad input rejected");
        else require(!exists && s.expectedInput.empty(), "cold path must be absent");
        install();
        s.configuring = true;
        const bool configured = d->configurePipelineCacheForPrewarm(s.path.c_str());
        s.configuring = false;
        require(configured && s.cacheCalls == 1 && s.inputExact && s.cache == d->pipelineCache()->getHandle(),
                "actual configured driver input equals validated payload");
        s.loaded = d->pipelineCache()->loadedFromDisk();
        require(s.loaded == (s.phase == "restored"), "cold/restored/rejected cache path");
        CC_LOG_INFO("project-prewarm cache-input: calls=%u loaded=%u initialBytes=%llu exact=%u", s.cacheCalls,
                    unsigned(s.loaded), static_cast<unsigned long long>(s.initialBytes), unsigned(s.inputExact));
        s.boot = true;
        runPrewarm(*d);
    } catch (const std::exception &e) {
        check(false, e.what());
    } catch (...) {
        check(false, "unexpected diagnostic exception");
    }
    s.configuring = s.boot = false;
}
} // namespace

bool prepare() noexcept {
    if (s.prepared) return s.failures == 0;
    s.prepared = true;
    try {
        const char *path = std::getenv("CC_VULKAN_REAL_PROJECT_CACHE_PATH");
        const char *phase = std::getenv("CC_VULKAN_REAL_PROJECT_PREWARM");
        s.enabled = path != nullptr || phase != nullptr;
        if (!s.enabled) {
            CC_LOG_INFO("project-prewarm mode: phase=off enabled=0");
            return true;
        }
        require(path && *path && phase && *phase, "path and phase must be supplied together");
        s.path = path;
        s.phase = phase;
        require(s.phase == "cold" || s.phase == "restored" || s.phase == "rejected" || s.phase == "wrong-key",
                "known prewarm phase");
        require(std::filesystem::path(s.path).is_absolute(), "isolated absolute cache path");
        CC_LOG_INFO("project-prewarm mode: phase=%s enabled=1", s.phase.c_str());
        return true;
    } catch (...) {
        return false;
    }
}

void start() noexcept {
    if (!s.enabled || s.failures) return;
    if (s.started) { check(false, "start once"); return; }
    // DeviceAgent is already consuming commands when Engine emits ON_START.
    // Route the complete diagnostic through that same queue, then finish before JS bootstraps.
    if (auto *agent = gfx::DeviceAgent::getInstance()) {
        auto *messages = agent->getMessageQueue();
        if (!messages) { check(false, "agent queue exists"); return; }
        ENQUEUE_MESSAGE_0(messages, RealProjectPrewarm, { startOnRenderThread(); });
        messages->kickAndWait();
    } else {
        startOnRenderThread();
    }
}

int finish(int platformResult) noexcept {
    if (!s.enabled) return platformResult;
    try {
        check(platformResult == 0 && s.started && s.devicesDestroyed == 1, "normal platform/device shutdown");
        unsigned computeDestroyed = 0, modulesDestroyed = 0, sceneDestroyed = 0;
        for (unsigned n = 0; n < s.computes; ++n) computeDestroyed += s.compute[n].destroyed;
        for (unsigned n = 0; n < s.modules; ++n) modulesDestroyed += s.module[n].destroyed;
        for (unsigned n = 0; n < s.graphics; ++n) sceneDestroyed += s.scene[n].destroyed;
        check(s.computes == 2 && computeDestroyed == 2 && s.modules == 2 && modulesDestroyed == 2 &&
                  s.layouts == 1 && s.layoutDestroyed == 1, "all recipe GPU resources retired");
        check(s.graphics > 0 && s.cachedGraphics == s.graphics && sceneDestroyed == s.graphics,
              "actual scene pipeline cache consumption and retirement");
        check(s.liveErrors == 0 && s.deviceErrors == 0, "validation through device shutdown");
        Store::Bytes saved;
        const bool validated = Store::read(s.path, s.identity, saved) && !saved.empty();
        check(validated, "saved cache validates after normal device destruction");
        CC_LOG_INFO("project-prewarm scene: graphics=%u cached=%u destroyed=%u", s.graphics, s.cachedGraphics, sceneDestroyed);
        CC_LOG_INFO("project-prewarm saved: bytes=%llu validated=%u", static_cast<unsigned long long>(saved.size()), unsigned(validated));
        CC_LOG_INFO("project-prewarm retirement: pipelines=%u/%u modules=%u/%u layouts=%u/%u devices=%u", s.computes,
                    computeDestroyed, s.modules, modulesDestroyed, s.layouts, s.layoutDestroyed, s.devicesDestroyed);
        const char *outcome = s.failures ? "FAILED" : s.phase == "wrong-key" ? "REJECTED" : "CLEAN";
        CC_LOG_INFO("project-prewarm %s completed: failures=%u liveErrors=%u deviceErrors=%u outcome=%s", s.phase.c_str(),
                    s.failures, s.liveErrors, s.deviceErrors, outcome);
    } catch (...) {
        check(false, "final evidence exception");
    }
    restore();
    return platformResult || s.failures || s.phase == "wrong-key" ? 1 : 0;
}
} // namespace cc::vulkan_test
