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
// PERF-A1 rev3 (Lead): main-entry -> first successful present API return.
// Not display time, process creation, or individual instance/device spans.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include "gfx-base/SPIRVUtils.h"
#include "gfx-vulkan/VKDevice.h"
#include "gfx-vulkan/VKGPUObjects.h"
#include "gfx-vulkan/VKPipelineCache.h"
#include "gfx-vulkan/VKPipelineCachePersistence.h"
#include "gfx-vulkan/VKPipelineLayout.h"
#include "gfx-vulkan/VKPipelineState.h"
#include "gfx-vulkan/VKRenderPass.h"
#include "gfx-vulkan/VKShader.h"
#include "gfx-vulkan/VKSwapchain.h"
#include "platform/FileUtils.h"
#include "renderer/GFXDeviceManager.h"
using namespace cc;
using namespace cc::gfx;
int cocos_main(int, const char **) { return 0; }
namespace {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
long long ns(Time a, Time b) { return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count(); }
unsigned failures = 0;
bool incomplete = false;
void need(bool yes, const char *why) {
    if (!yes) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", why);
        throw std::runtime_error("reported failure");
    }
}
void budget(CCVKDevice &d) {
    if (d.gpuContext()->validationErrorCount.load() >= 10) {
        incomplete = true;
        std::printf("first-use INCOMPLETE: live validation budget\n");
        need(false, "validation error budget");
    }
}
template <class T>
struct Destroy {
    void operator()(T *p) const noexcept {
        if (p) {
            p->destroy();
            delete p;
        }
    }
};
template <class T>
using Owned = std::unique_ptr<T, Destroy<T>>;
struct Observation {
    bool invalid = false, frame = false, captureBegin = false;
    unsigned swaps = 0, acquireCalls = 0, begins = 0, binds = 0, draws = 0, graphicsCalls = 0, graphicsSuccess = 0, frameSubmits = 0, presents = 0;
    VkSwapchainKHR swap = VK_NULL_HANDLE, acquiredSwap = VK_NULL_HANDLE;
    uint32_t image = UINT32_MAX;
    VkResult acquireResult = VK_NOT_READY;
    VkSemaphore acquireSemaphore = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE, bound = VK_NULL_HANDLE;
    VkRenderPass rp = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipelineCache cache = VK_NULL_HANDLE;
    VkQueue frameQueue = VK_NULL_HANDLE;
    bool acquiredWaited = false, semaphoreChain = false, presentIdentity = false;
    std::array<VkSemaphore, 16> reachable{};
    size_t reachableCount = 0;
    Time acquireTime{}, submitTime{}, presentTime{};
    long long driverNs = 0;
    bool contains(VkSemaphore sem) const {
        if (!sem) return false;
        for (size_t i = 0; i < reachableCount; ++i)
            if (reachable[i] == sem) return true;
        return false;
    }
    void add(VkSemaphore sem) {
        if (!sem) {
            invalid = true;
            return;
        }
        if (contains(sem)) return;
        if (reachableCount == reachable.size()) {
            invalid = true;
            return;
        }
        reachable[reachableCount++] = sem;
    }
} ev;
struct Dispatch {
    PFN_vkCreateSwapchainKHR swap = nullptr;
    PFN_vkAcquireNextImageKHR acquire = nullptr;
    PFN_vkBeginCommandBuffer begin = nullptr;
    PFN_vkCmdBindPipeline bind = nullptr;
    PFN_vkCmdDraw draw = nullptr;
    PFN_vkCreateGraphicsPipelines graphics = nullptr;
    PFN_vkQueueSubmit submit = nullptr;
    PFN_vkQueuePresentKHR present = nullptr;
} real;
VKAPI_ATTR VkResult VKAPI_CALL seeSwap(VkDevice d, const VkSwapchainCreateInfoKHR *i, const VkAllocationCallbacks *a, VkSwapchainKHR *out) {
    if (!real.swap || !i || !out || i->sType != VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR) {
        ev.invalid = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    auto r = real.swap(d, i, a, out);
    ++ev.swaps;
    if (r == VK_SUCCESS && *out && ev.swaps == 1)
        ev.swap = *out;
    else
        ev.invalid = true;
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL seeAcquire(VkDevice d, VkSwapchainKHR s, uint64_t timeout, VkSemaphore sem, VkFence f, uint32_t *index) {
    if (!real.acquire || !s || !index) {
        ev.invalid = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    auto r = real.acquire(d, s, timeout, sem, f, index);
    auto returned = Clock::now();
    ++ev.acquireCalls;
    ev.acquireResult = r;
    if (r == VK_SUCCESS && ev.acquireCalls == 1) {
        ev.acquiredSwap = s;
        ev.image = *index;
        ev.acquireSemaphore = sem;
        ev.acquireTime = returned;
    } else
        ev.invalid = true;
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL seeBegin(VkCommandBuffer cb, const VkCommandBufferBeginInfo *i) {
    if (!real.begin || !cb || !i || i->sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO) {
        ev.invalid = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    auto r = real.begin(cb, i);
    if (ev.captureBegin) {
        ++ev.begins;
        if (r == VK_SUCCESS && ev.begins == 1)
            ev.cb = cb;
        else
            ev.invalid = true;
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL seeBind(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipeline p) {
    if (!real.bind || !cb || !p) {
        ev.invalid = true;
        return;
    }
    real.bind(cb, point, p);
    if (ev.frame && cb == ev.cb) {
        ++ev.binds;
        ev.bound = point == VK_PIPELINE_BIND_POINT_GRAPHICS ? p : VK_NULL_HANDLE;
        if (ev.bound != ev.pipeline) ev.invalid = true;
    }
}
VKAPI_ATTR void VKAPI_CALL seeDraw(VkCommandBuffer cb, uint32_t vertices, uint32_t instances, uint32_t firstVertex, uint32_t firstInstance) {
    if (!real.draw || !cb) {
        ev.invalid = true;
        return;
    }
    real.draw(cb, vertices, instances, firstVertex, firstInstance);
    if (ev.frame) {
        ++ev.draws;
        if (cb != ev.cb || !ev.cb || ev.bound != ev.pipeline || vertices != 3 || instances != 1 || firstVertex || firstInstance) ev.invalid = true;
    }
}
VKAPI_ATTR VkResult VKAPI_CALL seeGraphics(VkDevice d, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo *i, const VkAllocationCallbacks *a, VkPipeline *out) {
    if (!real.graphics || n != 1 || !i || !out || i->sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO) {
        ev.invalid = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    auto start = Clock::now();
    auto r = real.graphics(d, cache, n, i, a, out);
    auto returned = Clock::now();
    ++ev.graphicsCalls;
    if (r == VK_SUCCESS && *out && ev.graphicsCalls == 1) {
        ++ev.graphicsSuccess;
        ev.pipeline = *out;
        ev.rp = i->renderPass;
        ev.layout = i->layout;
        ev.cache = cache;
        ev.driverNs = ns(start, returned);
        if (i->stageCount != 2 || i->subpass != 0 || i->pNext) ev.invalid = true;
    } else
        ev.invalid = true;
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL seeSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *infos, VkFence f) {
    if (!real.submit || !q || !n || n > 8 || !infos) {
        ev.invalid = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    unsigned occurrences = 0;
    bool waitsOnFrame = false, acquiredWait = false;
    for (uint32_t s = 0; s < n; ++s) {
        const auto &i = infos[s];
        if (i.sType != VK_STRUCTURE_TYPE_SUBMIT_INFO || i.commandBufferCount > 16 || i.waitSemaphoreCount > 16 || i.signalSemaphoreCount > 16 ||
            (i.commandBufferCount && !i.pCommandBuffers) || (i.waitSemaphoreCount && (!i.pWaitSemaphores || !i.pWaitDstStageMask)) ||
            (i.signalSemaphoreCount && !i.pSignalSemaphores)) {
            ev.invalid = true;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (i.pNext) {
            const auto *t = static_cast<const VkTimelineSemaphoreSubmitInfo *>(i.pNext);
            if (t->sType != VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO || t->pNext ||
                t->waitSemaphoreValueCount != i.waitSemaphoreCount || t->signalSemaphoreValueCount != i.signalSemaphoreCount ||
                (t->waitSemaphoreValueCount && !t->pWaitSemaphoreValues) || (t->signalSemaphoreValueCount && !t->pSignalSemaphoreValues)) {
                ev.invalid = true;
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }
        for (uint32_t c = 0; c < i.commandBufferCount; ++c)
            if (ev.cb && i.pCommandBuffers[c] == ev.cb && ev.frame) ++occurrences;
        for (uint32_t w = 0; w < i.waitSemaphoreCount; ++w) {
            waitsOnFrame |= ev.contains(i.pWaitSemaphores[w]);
            acquiredWait |= ev.acquireSemaphore && i.pWaitSemaphores[w] == ev.acquireSemaphore;
        }
    }
    auto r = real.submit(q, n, infos, f);
    auto returned = Clock::now();
    if (r != VK_SUCCESS || !ev.frame) return r;
    if (occurrences) {
        ++ev.frameSubmits;
        if (occurrences != 1 || ev.frameSubmits != 1 || !acquiredWait) ev.invalid = true;
        ev.acquiredWaited = acquiredWait;
        ev.frameQueue = q;
        ev.submitTime = returned;
    }
    // Reach a later binary signal ONLY via its actual wait on a prior signal.
    // Semaphore creation timing cannot establish this dependency.
    if ((occurrences || waitsOnFrame) && (!ev.frameQueue || q == ev.frameQueue)) {
        for (uint32_t s = 0; s < n; ++s) {
            const auto &i = infos[s];
            const auto *t = static_cast<const VkTimelineSemaphoreSubmitInfo *>(i.pNext);
            for (uint32_t v = 0; v < i.signalSemaphoreCount; ++v)
                if (!t || t->pSignalSemaphoreValues[v] == 0) ev.add(i.pSignalSemaphores[v]);
        }
    }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL seePresent(VkQueue q, const VkPresentInfoKHR *i) {
    if (!real.present || !q || !i || i->sType != VK_STRUCTURE_TYPE_PRESENT_INFO_KHR || i->swapchainCount != 1 || !i->pSwapchains ||
        !i->pImageIndices || i->waitSemaphoreCount != 1 || !i->pWaitSemaphores) {
        ev.invalid = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    auto r = real.present(q, i);
    auto returned = Clock::now();
    if (ev.frame) {
        ++ev.presents;
        ev.presentIdentity = q == ev.frameQueue && i->pSwapchains[0] == ev.acquiredSwap && i->pImageIndices[0] == ev.image;
        ev.semaphoreChain = ev.contains(i->pWaitSemaphores[0]);
        if (r == VK_SUCCESS && (!i->pResults || i->pResults[0] == VK_SUCCESS) && ev.presentIdentity && ev.semaphoreChain && ev.frameSubmits == 1 && ev.presents == 1)
            ev.presentTime = returned;
        else
            ev.invalid = true;
    }
    return r;
}
struct Hooks {
    Hooks() {
        ev = {};
        real = {vkCreateSwapchainKHR, vkAcquireNextImageKHR, vkBeginCommandBuffer, vkCmdBindPipeline, vkCmdDraw, vkCreateGraphicsPipelines, vkQueueSubmit, vkQueuePresentKHR};
        need(real.swap && real.acquire && real.begin && real.bind && real.draw && real.graphics && real.submit && real.present, "all observation dispatch loaded");
        vkCreateSwapchainKHR = seeSwap;
        vkAcquireNextImageKHR = seeAcquire;
        vkBeginCommandBuffer = seeBegin;
        vkCmdBindPipeline = seeBind;
        vkCmdDraw = seeDraw;
        vkCreateGraphicsPipelines = seeGraphics;
        vkQueueSubmit = seeSubmit;
        vkQueuePresentKHR = seePresent;
    }
    ~Hooks() {
        vkCreateSwapchainKHR = real.swap;
        vkAcquireNextImageKHR = real.acquire;
        vkBeginCommandBuffer = real.begin;
        vkCmdBindPipeline = real.bind;
        vkCmdDraw = real.draw;
        vkCreateGraphicsPipelines = real.graphics;
        vkQueueSubmit = real.submit;
        vkQueuePresentKHR = real.present;
    }
};
struct CacheObservation {
    PFN_vkCreatePipelineCache real = nullptr;
    unsigned calls = 0;
    size_t bytes = 0;
    bool payload = false, invalid = false;
    VkPipelineCache handle = VK_NULL_HANDLE;
} ce;
VKAPI_ATTR VkResult VKAPI_CALL seeCache(VkDevice d, const VkPipelineCacheCreateInfo *i, const VkAllocationCallbacks *a, VkPipelineCache *out) {
    if (!ce.real || !i || !out || i->sType != VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO || (i->initialDataSize && !i->pInitialData)) {
        ce.invalid = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    auto r = ce.real(d, i, a, out);
    ++ce.calls;
    if (r == VK_SUCCESS && *out && ce.calls == 1) {
        ce.bytes = i->initialDataSize;
        ce.payload = i->pInitialData != nullptr;
        ce.handle = *out;
    } else
        ce.invalid = true;
    return r;
}
struct CacheHook {
    CacheHook() {
        ce = {};
        ce.real = vkCreatePipelineCache;
        need(ce.real != nullptr, "cache dispatch");
        vkCreatePipelineCache = seeCache;
    }
    ~CacheHook() { vkCreatePipelineCache = ce.real; }
};
struct Window {
    HWND handle = nullptr;
    Window() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"CocosFirstUseRev3";
        need(RegisterClassExW(&wc) != 0, "register window class");
        handle = CreateWindowExW(0, wc.lpszClassName, L"Vulkan first-use", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 640, 480, nullptr, nullptr, wc.hInstance, nullptr);
        need(handle != nullptr, "create window");
        ShowWindow(handle, SW_SHOWNOACTIVATE);
        UpdateWindow(handle);
    }
    ~Window() {
        if (handle) DestroyWindow(handle);
    }
    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;
};
struct Report {
    long long backend = 0, configure = 0, swapchain = 0, shader = 0, factory = 0, driver = 0, combined = 0, frameApi = 0, frameFlow = 0, mainApi = 0;
    unsigned begins = 0, graphics = 0, draws = 0, submits = 0, presents = 0;
    bool identity = false, valid = false, loaded = false, payload = false;
    size_t initialBytes = 0, savedBytes = 0;
};
void run(CCVKDevice &d, Time t0, const std::filesystem::path &path, const std::string &phase, bool skipDraw, bool skipSubmit, Report &report) {
    auto &caps = d.gpuDevice()->runtimePaths;
    need(!caps.useSubmit2 && !caps.useSynchronization2 && !caps.useDynamicRendering && !caps.forceClassicRenderPass, "legacy submit/barrier and default Classic policy required");
    budget(d);
    {
        CacheHook hook;
        auto start = Clock::now();
        bool configured = d.configurePipelineCacheForPrewarm(path.generic_string());
        report.configure = ns(start, Clock::now());
        need(configured && !ce.invalid && ce.calls == 1 && d.pipelineCache() && ce.handle == d.pipelineCache()->getHandle(), "one configured cache observed from driver");
    }
    auto &cache = *d.pipelineCache();
    report.loaded = cache.loadedFromDisk();
    report.initialBytes = ce.bytes;
    report.payload = ce.payload;
    need(!cache.isDirty() && report.loaded == (phase == "restored") && (phase == "cold" ? (ce.bytes == 0 && !ce.payload) : (ce.bytes > 0 && ce.payload)), "cache phase/input");
    std::printf("first-use cache-input: calls=1 loaded=%u initialBytes=%zu payload=%u\n", unsigned(report.loaded), report.initialBytes, unsigned(report.payload));
    Window window;
    RECT client{};
    need(GetClientRect(window.handle, &client) != 0 && client.right > client.left && client.bottom > client.top, "nonzero client extent");
    SwapchainInfo si;
    si.windowHandle = window.handle;
    si.width = client.right - client.left;
    si.height = client.bottom - client.top;
    si.vsyncMode = VsyncMode::OFF;
    d.enableAutoBarrier(false);
    auto scStart = Clock::now();
    Owned<Swapchain> sc(static_cast<Device &>(d).createSwapchain(si));
    report.swapchain = ns(scStart, Clock::now());
    need(sc != nullptr, "swapchain facade");
    auto *gs = static_cast<CCVKSwapchain *>(sc.get())->gpuSwapchain();
    need(gs && gs->vkSwapchain && ev.swaps == 1 && ev.swap == gs->vkSwapchain && !ev.invalid, "one real swapchain create");
    auto *color = sc->getColorTexture(), *depth = sc->getDepthStencilTexture();
    need(color && depth, "swapchain attachments");
    budget(d);
    GeneralBarrierInfo bi;
    bi.prevAccesses = AccessFlagBit::PRESENT;
    bi.nextAccesses = AccessFlagBit::PRESENT;
    RenderPassInfo ri;
    ri.colorAttachments.resize(1);
    ri.colorAttachments[0].format = color->getFormat();
    ri.colorAttachments[0].loadOp = LoadOp::CLEAR;
    ri.colorAttachments[0].storeOp = StoreOp::STORE;
    ri.colorAttachments[0].barrier = d.getGeneralBarrier(bi);
    ri.depthStencilAttachment.format = depth->getFormat();
    ri.depthStencilAttachment.depthLoadOp = LoadOp::CLEAR;
    ri.depthStencilAttachment.depthStoreOp = StoreOp::STORE;
    Owned<RenderPass> rp(d.createRenderPass(ri));
    need(rp != nullptr, "render pass");
    FramebufferInfo fi;
    fi.renderPass = rp.get();
    fi.colorTextures = {color};
    fi.depthStencilTexture = depth;
    Owned<Framebuffer> fb(d.createFramebuffer(fi));
    need(fb != nullptr, "framebuffer");
    struct CompilerGuard {
        ~CompilerGuard() { SPIRVUtils::getInstance()->destroy(); }
    } compiler;
    SPIRVUtils::getInstance()->destroy();
    SPIRVUtils::getInstance()->initialize(3);
    ShaderInfo shi;
    shi.name = "first-graphics-rev3";
    ShaderStage vs;
    vs.stage = ShaderStageFlagBit::VERTEX;
    vs.source = "void main(){vec2 p=vec2(float((gl_VertexIndex<<1)&2),float(gl_VertexIndex&2));gl_Position=vec4(p*2.0-1.0,0.0,1.0);}";
    ShaderStage fs;
    fs.stage = ShaderStageFlagBit::FRAGMENT;
    fs.source = "layout(location=0) out vec4 o;void main(){o=vec4(1,0,0,1);}";
    shi.stages = {vs, fs};
    Owned<Shader> shader(d.createShader(shi));
    need(shader != nullptr, "shader facade");
    Owned<PipelineLayout> layout(d.createPipelineLayout(PipelineLayoutInfo{}));
    need(layout != nullptr, "pipeline layout");
    auto shaderStart = Clock::now();
    auto *gpuShader = static_cast<CCVKShader *>(shader.get())->gpuShader();
    report.shader = ns(shaderStart, Clock::now());
    need(gpuShader && gpuShader->initialized, "first shader compile");
    PipelineStateInfo pi;
    pi.shader = shader.get();
    pi.pipelineLayout = layout.get();
    pi.renderPass = rp.get();
    pi.bindPoint = PipelineBindPoint::GRAPHICS;
    pi.depthStencilState.depthTest = false;
    pi.depthStencilState.depthWrite = false;
    pi.rasterizerState.cullMode = CullMode::NONE;
    auto factoryStart = Clock::now();
    Owned<PipelineState> pso(d.createPipelineState(pi));
    auto factoryEnd = Clock::now();
    report.factory = ns(factoryStart, factoryEnd);
    report.combined = ns(shaderStart, factoryEnd);
    report.driver = ev.driverNs;
    need(pso != nullptr, "graphics PSO facade");
    auto *gp = static_cast<CCVKPipelineState *>(pso.get())->gpuPipelineState();
    need(gp && gp->vkPipeline && gp->vkPipeline == ev.pipeline && ev.graphicsCalls == 1 && ev.graphicsSuccess == 1 &&
             ev.rp == static_cast<CCVKRenderPass *>(rp.get())->gpuRenderPass()->vkRenderPass &&
             ev.layout == static_cast<CCVKPipelineLayout *>(layout.get())->gpuPipelineLayout()->vkPipelineLayout && ev.cache == cache.getHandle() && !ev.invalid,
         "driver/PSO/render-pass/layout/cache identity");
    Owned<InputAssembler> ia(d.createInputAssembler(InputAssemblerInfo{}));
    need(ia != nullptr, "procedural input assembler");
    CommandBufferInfo ci;
    ci.queue = d.getQueue();
    need(ci.queue != nullptr, "queue");
    Owned<CommandBuffer> cb(d.createCommandBuffer(ci));
    need(cb != nullptr, "command buffer");
    budget(d);
    auto frameStart = Clock::now();
    Swapchain *scPointer = sc.get();
    d.acquire(&scPointer, 1);
    gs = static_cast<CCVKSwapchain *>(sc.get())->gpuSwapchain();
    need(gs && gs->wsi.hasRenderableImage() && ev.acquireCalls == 1 && ev.acquireResult == VK_SUCCESS && ev.acquiredSwap == ev.swap &&
             gs->vkSwapchain == ev.acquiredSwap && ev.image == gs->curImageIndex && !ev.invalid,
         "single successful first acquire");
    ev.frame = true;
    ev.captureBegin = true;
    cb->begin();
    ev.captureBegin = false;
    need(ev.begins == 1 && ev.cb, "successful user CB begin");
    const Color clear{0, 0, 0, 1};
    cb->beginRenderPass(rp.get(), fb.get(), {0, 0, color->getWidth(), color->getHeight()}, &clear, 1, 0);
    cb->bindInputAssembler(ia.get());
    cb->bindPipelineState(pso.get());
    DrawInfo di;
    di.vertexCount = 3;
    di.instanceCount = 1;
    if (!skipDraw) cb->draw(di); // Negative control skips ONLY the original draw.
    cb->endRenderPass();
    cb->end();
    if (!skipSubmit) {
        CommandBuffer *cbPointer = cb.get();
        d.getQueue()->submit(&cbPointer, 1);
        d.present();
    }
    auto flowEnd = Clock::now();
    budget(d);
    report.begins = ev.begins;
    report.graphics = ev.graphicsSuccess;
    report.draws = ev.draws;
    report.submits = ev.frameSubmits;
    report.presents = ev.presents;
    report.identity = !ev.invalid && ev.begins == 1 && ev.binds == 1 && ev.draws == 1 && ev.frameSubmits == 1 && ev.acquiredWaited && ev.presents == 1 && ev.presentIdentity && ev.semaphoreChain && ev.presentTime != Time{};
    report.frameFlow = ns(frameStart, flowEnd);
    if (ev.presentTime != Time{}) {
        report.frameApi = ns(frameStart, ev.presentTime);
        report.mainApi = ns(t0, ev.presentTime);
    }
    std::printf("first-use evidence: begin=%u graphics=%u draw=%u submit=%u present=%u chain=%u\n", report.begins, report.graphics, report.draws, report.submits, report.presents, unsigned(report.identity));
    ev.frame = false; // teardown/abort submissions are outside the measured first frame
    need(report.identity, "first frame draw/submit/present identity chain");
    report.valid = true;
    d.waitAllFences();
    cache.saveCache();
    VulkanPipelineCachePersistence::Bytes bytes;
    need(!cache.isDirty() && VulkanPipelineCachePersistence::read(path, VulkanPipelineCachePersistence::identity(d.gpuContext()->physicalDeviceProperties), bytes) && !bytes.empty(), "saved validated pipeline cache");
    report.savedBytes = bytes.size();
    std::printf("first-use saved: bytes=%zu validated=1 dirty=0\n", report.savedBytes);
    budget(d);
}
void csv(const std::filesystem::path &path, const std::string &phase, bool validation, bool skipDraw, bool skipSubmit, const Report &r, bool clean) {
    std::FILE *f = nullptr;
    if (fopen_s(&f, path.string().c_str(), "w") != 0 || !f) {
        ++failures;
        std::fprintf(stderr, "FAIL: CSV open\n");
        return;
    }
    int a = std::fprintf(f, "phase,validation,skip_draw,skip_submit,outcome,backend_init_ns,cache_config_ns,swapchain_create_ns,shader_compile_ns,pso_factory_ns,pso_driver_ns,combined_graphics_ns,frame_to_present_api_ns,frame_flow_ns,main_to_present_api_ns\n");
    int b = std::fprintf(f, "%s,%s,%u,%u,%s,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n", phase.c_str(), validation ? "on" : "off", unsigned(skipDraw), unsigned(skipSubmit), clean ? "CLEAN" : "FAILED",
                         r.backend, r.configure, r.swapchain, r.shader, r.factory, r.driver, r.combined, r.frameApi, r.frameFlow, r.mainApi);
    int c = std::fflush(f), e = std::fclose(f);
    if (a < 0 || b < 0 || c || e) {
        ++failures;
        std::fprintf(stderr, "FAIL: CSV write/flush/close\n");
    }
}
} // namespace
int main(int argc, char **argv) {
    auto t0 = Clock::now();
    std::string phase, validation = "on";
    bool skipDraw = false, skipSubmit = false, seenPhase = false, seenValidation = false;
    try {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--phase" || a == "--validation") {
                bool &seen = a == "--phase" ? seenPhase : seenValidation;
                need(!seen && i + 1 < argc, "duplicate/missing CLI value");
                seen = true;
                auto &v = a == "--phase" ? phase : validation;
                v = argv[++i];
                need(a == "--phase" ? (v == "cold" || v == "restored") : (v == "on" || v == "off"), "invalid CLI value");
            } else if (a == "--skip-draw") {
                need(!skipDraw, "duplicate skip-draw");
                skipDraw = true;
            } else if (a == "--skip-submit") {
                need(!skipSubmit, "duplicate skip-submit");
                skipSubmit = true;
            } else
                need(false, "unknown CLI argument");
        }
        need(seenPhase && !(skipDraw && skipSubmit), "phase required; one negative control per process");
    } catch (...) {
        return 1;
    }
    bool wantValidation = validation == "on";
    std::filesystem::path directory, path;
    std::unique_ptr<FileUtils> files;
    Owned<Device> facade;
    Report report;
    uint32_t live = 0, post = 0;
    bool finished = false;
    try {
        directory = std::filesystem::current_path() / (skipDraw ? "first-use-cwd-skipdraw" : skipSubmit ? "first-use-cwd-skip"
                                                                                                        : "first-use-cwd");
        path = directory / "first-use.bin";
        need(phase == "cold" ? !std::filesystem::exists(path) : std::filesystem::exists(path), "cache-file phase precondition");
        std::filesystem::create_directories(directory);
        files.reset(createFileUtils());
        need(files != nullptr, "FileUtils");
        files->setWritablePath(directory.generic_string() + "/");
        auto start = Clock::now();
        facade.reset(DeviceManager::create());
        report.backend = ns(start, Clock::now());
        need(facade && facade->getGfxAPI() == API::VULKAN && CCVKDevice::getInstance(), "Vulkan backend initialization");
        auto &d = *CCVKDevice::getInstance();
        need(d.gpuDevice() && d.gpuContext(), "live context/device");
        auto *ctx = d.gpuContext();
        need(ctx->apiVersionInfo.effectiveDeviceApiVersion == VK_API_VERSION_1_3, "effective API exactly 1.3");
        bool enabled = ctx->validationEnabled && ctx->bestPracticesValidationEnabled && ctx->synchronizationValidationEnabled;
        bool disabled = !ctx->validationEnabled && !ctx->bestPracticesValidationEnabled && !ctx->synchronizationValidationEnabled;
        std::printf("first-use validation: %s\n", enabled ? "ENABLED" : disabled ? "DISABLED"
                                                                                 : "PARTIAL");
        std::printf("first-use mode: phase=%s validation=%s skipDraw=%u skipSubmit=%u\n", phase.c_str(), validation.c_str(), unsigned(skipDraw), unsigned(skipSubmit));
        Hooks hooks;
        try {
            need(wantValidation ? enabled : disabled, "actual validation matches requested mode");
            run(d, t0, path, phase, skipDraw, skipSubmit, report);
            finished = true;
        } catch (const std::exception &ex) {
            if (std::string(ex.what()) != "reported failure") {
                ++failures;
                std::fprintf(stderr, "FAIL: body exception %s\n", ex.what());
            }
        } catch (...) {
            ++failures;
            std::fprintf(stderr, "FAIL: unknown body exception\n");
        }
        live = d.gpuContext()->validationErrorCount.load();
        if (live) {
            ++failures;
            std::fprintf(stderr, "FAIL: live validation errors\n");
        }
        facade->destroy();
        post = d.getLastValidationErrorCount();
        if (post) {
            ++failures;
            std::fprintf(stderr, "FAIL: post-destroy validation errors\n");
        }
        delete facade.release();
        if (ev.invalid || ce.invalid) {
            ++failures;
            std::fprintf(stderr, "FAIL: invalid observation\n");
        }
    } catch (const std::exception &ex) {
        if (std::string(ex.what()) != "reported failure") {
            ++failures;
            std::fprintf(stderr, "FAIL: setup exception %s\n", ex.what());
        }
        facade.reset();
    } catch (...) {
        ++failures;
        std::fprintf(stderr, "FAIL: unknown setup exception\n");
        facade.reset();
    }
    bool ready = finished && report.valid && report.savedBytes && !failures && !live && !post && !incomplete;
    std::printf("first-use timing ns: backend_init=%lld cache_config=%lld swapchain_create=%lld shader_compile=%lld pso_factory=%lld pso_driver=%lld combined_graphics=%lld frame_to_present_api=%lld frame_flow=%lld main_to_present_api=%lld\n",
                report.backend, report.configure, report.swapchain, report.shader, report.factory, report.driver, report.combined, report.frameApi, report.frameFlow, report.mainApi);
    if (!directory.empty()) csv(directory / "first-use.csv", phase, wantValidation, skipDraw, skipSubmit, report, ready);
    bool clean = ready && !failures;
    std::printf("first-use %s completed: failures=%u liveErrors=%u postErrors=%u outcome=%s\n", phase.c_str(), failures, live, post, clean ? "CLEAN" : "FAILED");
    return clean ? 0 : 1;
}
