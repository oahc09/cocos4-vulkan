// ============================================================================
// WSI lifecycle ABORTED-FRAME / RECREATE-FAILURE harness (M2-F).
//
// Four RED-expected cases run in INDEPENDENT processes (--case <name>) so one
// crash cannot hide the others. Each case drives the REAL engine through a
// dangerous frame-abort sequence that the current core may not yet handle;
// the harness reports honestly (exit 1 / crash = defect evidence for Lead).
//
//   zero-before-submit     : real acquire (no submit) -> resize(0,0) -> present
//   surface-before-submit  : real acquire (no submit) -> destroySurface ->
//                            createSurface(same HWND) -> old-frame present must
//                            not use the retired handle/index -> next frame OK
//   resize-after-record    : acquire -> record old-FBO clear, NO submit ->
//                            real window resize -> submit the stale CB (engine
//                            must safely reject, never execute against old
//                            images) -> present -> new frame renders
//   create-out-of-date      : normal presented frame -> forced resize with a
//                            volk hook on vkCreateSwapchainKHR: call the REAL
//                            create first (so oldSwapchain is genuinely
//                            retired), then destroy the fresh not-yet-acquired
//                            replacement and return VK_ERROR_OUT_OF_DATE_KHR.
//                            Next acquire must rebuild from a non-retired path
//                            without VUID-VkSwapchainCreateInfoKHR-oldSwapchain-01933.
//
// Fixed harness config (same as wsi_smoke): raw backend, no DeviceAgent,
// enableAutoBarrier(false) at startup, DS attachment index 1, device-owned
// general barriers, full destroy+delete teardown before device destroy.
// This is a limited real-clear harness: NOT visual baseline, NOT mobile/XR.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "gfx-vulkan/VKDevice.h"
#include "gfx-vulkan/VKGPUObjects.h"
#include "gfx-vulkan/VKSwapchain.h"
#include "gfx-vulkan/VKQueue.h"
#include "gfx-vulkan/VKFramebuffer.h"
#include "gfx-vulkan/VKWSIState.h"
#include "renderer/GFXDeviceManager.h"
#include "platform/FileUtils.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

int cocos_main(int, const char **) { return 0; }
void cocos_destroy() {}

namespace {

unsigned g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

struct TestWindow {
    HWND hwnd = nullptr;
    static void registerClass() {
        static bool done = false;
        if (done) return;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = L"CocosWsiLifeSmoke";
        RegisterClassExW(&wc);
        done = true;
    }
    static TestWindow create(int w, int h) {
        registerClass();
        TestWindow win;
        win.hwnd = CreateWindowExW(0, L"CocosWsiLifeSmoke", L"wsi-life-smoke", WS_OVERLAPPEDWINDOW,
                                   CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                                   nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
        ShowWindow(win.hwnd, SW_SHOWNOACTIVATE);
        return win;
    }
    void resize(int w, int h) {
        SetWindowPos(hwnd, nullptr, 0, 0, w, h, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER);
    }
    void destroy() {
        if (hwnd) { DestroyWindow(hwnd); hwnd = nullptr; }
    }
};

// ---- vkCreateSwapchainKHR injection (case 4) -------------------------------
// Calls the REAL create first so the oldSwapchain is genuinely retired by the
// driver; on success destroys the fresh (not-yet-acquired, no views/FBOs)
// replacement and returns VK_ERROR_OUT_OF_DATE_KHR. Never fakes success.
PFN_vkCreateSwapchainKHR g_realCreateSwapchain = nullptr;
int g_createHookCalls = 0;
int g_createRealSuccesses = 0;
VkSwapchainKHR g_lastOldSwapchain = VK_NULL_HANDLE;
bool g_createHookArmed = false;

VKAPI_ATTR VkResult VKAPI_CALL hookedCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
                                                        const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain) {
    ++g_createHookCalls;
    g_lastOldSwapchain = pCreateInfo ? pCreateInfo->oldSwapchain : VK_NULL_HANDLE;
    const VkResult real = g_realCreateSwapchain(device, pCreateInfo, pAllocator, pSwapchain);
    if (real == VK_SUCCESS) {
        ++g_createRealSuccesses;
        if (g_createHookArmed) {
            g_createHookArmed = false; // one-shot
            // destroy the fresh replacement, then report failure: the engine
            // must now treat this swapchain generation as failed while the
            // OLD swapchain has really been retired by this successful create.
            VkSwapchainKHR doomed = *pSwapchain;
            vkDestroySwapchainKHR(device, doomed, pAllocator);
            *pSwapchain = VK_NULL_HANDLE;
            return VK_ERROR_OUT_OF_DATE_KHR;
        }
    }
    return real;
}

// Count actual driver submits during the independent A/B recovery draws.
PFN_vkQueueSubmit g_realQueueSubmit = nullptr;
PFN_vkQueueSubmit2 g_realQueueSubmit2 = nullptr;
unsigned g_queueSubmitCalls = 0;
VKAPI_ATTR VkResult VKAPI_CALL countedQueueSubmit(VkQueue queue, uint32_t count,
                                                const VkSubmitInfo *submits, VkFence fence) {
    const auto result=g_realQueueSubmit(queue,count,submits,fence);
    if(result==VK_SUCCESS)++g_queueSubmitCalls;
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL countedQueueSubmit2(VkQueue queue,uint32_t count,const VkSubmitInfo2 *submits,VkFence fence) {
    const auto result=g_realQueueSubmit2(queue,count,submits,fence);
    if(result==VK_SUCCESS)++g_queueSubmitCalls;
    return result;
}
struct QueueSubmitCounter {
    QueueSubmitCounter() {
        g_queueSubmitCalls = 0;
        g_realQueueSubmit = vkQueueSubmit;
        vkQueueSubmit = &countedQueueSubmit;
        g_realQueueSubmit2=vkQueueSubmit2;
        if(g_realQueueSubmit2)vkQueueSubmit2=&countedQueueSubmit2;
    }
    ~QueueSubmitCounter() { vkQueueSubmit = g_realQueueSubmit; vkQueueSubmit2=g_realQueueSubmit2; g_realQueueSubmit = nullptr; g_realQueueSubmit2=nullptr; }
};

struct CreateSwapchainHook {
    CreateSwapchainHook() {
        g_realCreateSwapchain = vkCreateSwapchainKHR;
        g_createHookArmed = true;
        vkCreateSwapchainKHR = &hookedCreateSwapchainKHR;
    }
    ~CreateSwapchainHook() {
        vkCreateSwapchainKHR = g_realCreateSwapchain;
        g_realCreateSwapchain = nullptr;
    }
};

} // namespace

using namespace cc;
using namespace cc::gfx;

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::string caseName;
    bool autoBarrier = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--case") == 0 && i + 1 < argc) {
            caseName = argv[++i];
        } else if (std::strcmp(argv[i], "--auto-barrier") == 0) {
            autoBarrier = true;
        }
    }
    if (caseName.empty()) {
        std::fprintf(stderr, "usage: %s --case <zero-before-submit|surface-before-submit|resize-after-record|create-out-of-date|two-window-abort|reacquire-stale-record> [--auto-barrier]\n", argv[0]);
        return 2;
    }
    const bool valid = caseName == "zero-before-submit" || caseName == "surface-before-submit" ||
                       caseName == "resize-after-record" || caseName == "create-out-of-date" ||
                       caseName == "two-window-abort" || caseName == "reacquire-stale-record";
    if (!valid) {
        std::fprintf(stderr, "unknown case '%s'\n", caseName.c_str());
        return 2;
    }

    const auto outDir = std::filesystem::current_path() / ("wsi-life-" + caseName);
    std::filesystem::create_directories(outDir);
    std::unique_ptr<FileUtils> fileUtils(createFileUtils());
    fileUtils->setWritablePath(outDir.generic_string() + "/");

    std::fprintf(stderr, "Creating Cocos Vulkan device via DeviceManager\n");
    std::unique_ptr<Device> ownedDevice(DeviceManager::create());
    if (!ownedDevice || ownedDevice->getGfxAPI() != API::VULKAN) {
        std::fprintf(stderr, "FAIL: Cocos Vulkan initialization\n");
        if (ownedDevice) ownedDevice->destroy();
        return 1;
    }
    CCVKDevice &raw = *CCVKDevice::getInstance();
    Device *backend = &raw;
    auto *context = raw.gpuContext();
    const bool validation = context->validationEnabled && context->bestPracticesValidationEnabled &&
                            context->synchronizationValidationEnabled;
    std::printf("validation + best practices + sync: %s\n", validation ? "ENABLED" : "NOT VERIFIED");
    if (!validation) {
        std::fprintf(stderr, "FAIL: validation layers REQUIRED\n");
        ownedDevice->destroy();
        return 1;
    }
    // FIXED at startup, reported once; never changed mid-run to mask VUIDs.
    //  - autoBarrier=false: the render pass itself encodes PRESENT->PRESENT.
    //  - autoBarrier=true : the RP stays COLOR_ATTACHMENT_WRITE on both sides
    //    and the engine's early/late barriers own PRESENT<->COLOR transitions.
    backend->enableAutoBarrier(autoBarrier);
    std::printf("harness config: enableAutoBarrier(%s); RP color barrier %s\n",
                autoBarrier ? "true" : "false",
                autoBarrier ? "COLOR_ATTACHMENT_WRITE->COLOR_ATTACHMENT_WRITE" : "PRESENT->PRESENT");
    auto *queue = backend->getQueue();
    CommandBufferInfo cbInfo;
    cbInfo.queue = queue;
    CommandBuffer *cmd = backend->createCommandBuffer(cbInfo);

    const auto errorCount = [&raw] {
        return raw.gpuContext()->validationErrorCount.load(std::memory_order_relaxed);
    };

    TestWindow window = TestWindow::create(640, 480);
    SwapchainInfo info;
    info.windowHandle = window.hwnd;
    info.width = 640;
    info.height = 480;
    info.vsyncMode = VsyncMode::OFF;
    Swapchain *sc = backend->createSwapchain(info);
    check(sc != nullptr, "createSwapchain");

    // device-cached backend barrier; DS attachment index 1
    GeneralBarrierInfo colorInfo;
    if (autoBarrier) {
        colorInfo.prevAccesses = AccessFlagBit::COLOR_ATTACHMENT_WRITE;
        colorInfo.nextAccesses = AccessFlagBit::COLOR_ATTACHMENT_WRITE;
    } else {
        colorInfo.prevAccesses = AccessFlagBit::PRESENT;
        colorInfo.nextAccesses = AccessFlagBit::PRESENT;
    }
    RenderPassInfo rpInfo;
    rpInfo.colorAttachments.resize(1);
    rpInfo.colorAttachments[0].format = sc->getColorTexture()->getFormat();
    rpInfo.colorAttachments[0].loadOp = LoadOp::CLEAR;
    rpInfo.colorAttachments[0].storeOp = StoreOp::STORE;
    rpInfo.colorAttachments[0].barrier = backend->getGeneralBarrier(colorInfo);
    rpInfo.depthStencilAttachment.format = sc->getDepthStencilTexture()->getFormat();
    rpInfo.depthStencilAttachment.barrier = nullptr;
    rpInfo.subpasses.resize(1);
    rpInfo.subpasses[0].colors = {0};
    rpInfo.subpasses[0].depthStencil = 1;
    RenderPass *rp = backend->createRenderPass(rpInfo);
    FramebufferInfo fboInfo;
    fboInfo.renderPass = rp;
    fboInfo.colorTextures = {sc->getColorTexture()};
    fboInfo.depthStencilTexture = sc->getDepthStencilTexture();
    Framebuffer *fbo = backend->createFramebuffer(fboInfo);

    uint32_t fboGeneration = static_cast<CCVKSwapchain *>(sc)->gpuSwapchain()->generation;
    const Color clearColor{0.5f, 0.3f, 0.2f, 1.0f};
    const auto renderFrame = [&]() {
        const auto *current = static_cast<CCVKSwapchain *>(sc)->gpuSwapchain();
        if (fboGeneration != current->generation) {
            fbo->destroy(); delete fbo;
            fboInfo.colorTextures = {sc->getColorTexture()};
            fboInfo.depthStencilTexture = sc->getDepthStencilTexture();
            fbo = backend->createFramebuffer(fboInfo);
            fboGeneration = current->generation;
        }
        check(static_cast<CCVKFramebuffer *>(fbo)->gpuFBO()->swapchainGeneration == current->generation,
              "fresh clear uses current framebuffer generation");
        cmd->begin();
        Rect area{0, 0, sc->getColorTexture()->getWidth(), sc->getColorTexture()->getHeight()};
        cmd->beginRenderPass(rp, fbo, area, &clearColor, 1.0F, 0U);
        cmd->endRenderPass();
        cmd->end();
        queue->submit(reinterpret_cast<CommandBuffer **>(&cmd), 1);
    };
    const auto gpu = [&]() { return static_cast<CCVKSwapchain *>(sc)->gpuSwapchain(); };

    const uint32_t errorsAtStart = errorCount();
    bool completedMarker = false;

    if (caseName == "zero-before-submit") {
        backend->acquire(&sc, 1);
        check(gpu()->wsi.hasRenderableImage(), "case1: acquired, renderable before abort");
        // ABORT: resize to zero WITHOUT submitting the frame
        sc->resize(0, 0, SurfaceTransform::IDENTITY);
        backend->present(); // engine must skip presenting the aborted frame
        check(!gpu()->wsi.hasRenderableImage(), "case1: not renderable after zero resize + present");
        // restore and run a fresh normal frame
        sc->resize(640, 480, SurfaceTransform::IDENTITY);
        backend->acquire(&sc, 1);
        check(gpu()->wsi.hasRenderableImage(), "case1: renderable after restore");
        if (gpu()->wsi.hasRenderableImage()) renderFrame();
        backend->present();
        completedMarker = true;
    } else if (caseName == "surface-before-submit") {
        backend->acquire(&sc, 1);
        check(gpu()->wsi.hasRenderableImage(), "case2: acquired, renderable before surface loss");
        const uint32_t oldGeneration = gpu()->generation;
        // ABORT: destroy the surface WITHOUT submitting
        sc->destroySurface();
        check(gpu()->wsi.state() == VKSwapchainState::SURFACE_LOST, "case2: SURFACE_LOST recorded");
        backend->present(); // old-frame present must not use the retired surface/handle
        check(!gpu()->wsi.hasRenderableImage(), "case2: not renderable after surface loss");
        // rebuild surface on the SAME HWND and run a fresh frame
        sc->createSurface(window.hwnd);
        backend->acquire(&sc, 1);
        check(gpu()->wsi.hasRenderableImage(), "case2: renderable after surface recreate");
        if (gpu()->wsi.hasRenderableImage()) renderFrame();
        backend->present();
        std::printf("case2 generations: old=%u new=%u\n", oldGeneration, gpu()->generation);
        completedMarker = true;
    } else if (caseName == "resize-after-record") {
        backend->acquire(&sc, 1);
        check(gpu()->wsi.hasRenderableImage(), "case3: renderable before record");
        const uint32_t oldGeneration = gpu()->generation;
        // record against the CURRENT (old) FBO but DO NOT submit yet
        cmd->begin();
        Rect area{0, 0, sc->getColorTexture()->getWidth(), sc->getColorTexture()->getHeight()};
        cmd->beginRenderPass(rp, fbo, area, &clearColor, 1.0F, 0U);
        cmd->endRenderPass();
        cmd->end();
        // resize the real window and swapchain while the CB holds stale state
        window.resize(1024, 768);
        sc->resize(1024, 768, SurfaceTransform::IDENTITY);
        // submit the stale CB: the engine must safely gate it (stale
        // generation), never execute it against retired images
        const auto discardedBefore = static_cast<CCVKQueue *>(queue)->gpuQueue()->discardedWSICommandBuffers;
        queue->submit(reinterpret_cast<CommandBuffer **>(&cmd), 1);
        check(static_cast<CCVKQueue *>(queue)->gpuQueue()->discardedWSICommandBuffers == discardedBefore + 1,
              "case3: stale recorded command buffer rejected before Vulkan submission");
        backend->present();
        // fresh frame on the new generation must render+present normally
        backend->acquire(&sc, 1);
        check(gpu()->wsi.hasRenderableImage(), "case3: renderable after resize");
        if (gpu()->wsi.hasRenderableImage()) renderFrame();
        backend->present();
        std::printf("case3 generations: old=%u new=%u\n", oldGeneration, gpu()->generation);
        completedMarker = true;
    } else if (caseName == "create-out-of-date") {
        // one normal presented frame first
        backend->acquire(&sc, 1);
        check(gpu()->wsi.hasRenderableImage(), "case4: baseline frame renderable");
        if (gpu()->wsi.hasRenderableImage()) renderFrame();
        backend->present();
        const uint32_t oldGeneration = gpu()->generation;

        {   // one-shot hook: real create succeeds (old really retired),
            // fresh replacement destroyed, OUT_OF_DATE returned to the engine.
            // Resize the REAL window first so the surface currentExtent changes
            // and the engine actually attempts a swapchain recreate.
            window.resize(800, 600);
            CreateSwapchainHook hook;
            sc->resize(800, 600, SurfaceTransform::IDENTITY); // triggers recreate
            std::printf("case4 hook: calls=%d realSuccesses=%d oldHandle=%p\n",
                        g_createHookCalls, g_createRealSuccesses, (void *)g_lastOldSwapchain);
            check(g_createHookCalls >= 1, "case4: hook was invoked");
            check(g_createRealSuccesses >= 1, "case4: real vkCreateSwapchainKHR succeeded (old truly retired)");
        }
        // engine must be non-renderable right after the failed recreate
        check(!gpu()->wsi.hasRenderableImage(), "case4: not renderable after injected create failure");
        // next acquire must rebuild from a NON-retired path and present cleanly
        backend->acquire(&sc, 1);
        check(gpu()->wsi.hasRenderableImage(), "case4: renderable after recovery acquire");
        if (gpu()->wsi.hasRenderableImage()) renderFrame();
        backend->present();
        std::printf("case4 generations: old=%u new=%u\n", oldGeneration, gpu()->generation);
        completedMarker = true;
    } else if (caseName == "two-window-abort") {
        // ---- two real windows / swapchains; abort A without submitting,
        //      B must keep its original acquire and present independently.
        TestWindow windowB = TestWindow::create(512, 512);
        SwapchainInfo infoB;
        infoB.windowHandle = windowB.hwnd;
        infoB.width = 512;
        infoB.height = 512;
        infoB.vsyncMode = VsyncMode::OFF;
        Swapchain *scB = backend->createSwapchain(infoB);
        check(scB != nullptr, "case5: createSwapchain B");
        const auto gpuB = [&]() { return static_cast<CCVKSwapchain *>(scB)->gpuSwapchain(); };

        // B's own current-generation RP/FBO (rebuilt on generation change)
        GeneralBarrierInfo colorInfoB;
        if (autoBarrier) {
            colorInfoB.prevAccesses = AccessFlagBit::COLOR_ATTACHMENT_WRITE;
            colorInfoB.nextAccesses = AccessFlagBit::COLOR_ATTACHMENT_WRITE;
        } else {
            colorInfoB.prevAccesses = AccessFlagBit::PRESENT;
            colorInfoB.nextAccesses = AccessFlagBit::PRESENT;
        }
        RenderPassInfo rpInfoB;
        rpInfoB.colorAttachments.resize(1);
        rpInfoB.colorAttachments[0].format = scB->getColorTexture()->getFormat();
        rpInfoB.colorAttachments[0].loadOp = LoadOp::CLEAR;
        rpInfoB.colorAttachments[0].storeOp = StoreOp::STORE;
        rpInfoB.colorAttachments[0].barrier = backend->getGeneralBarrier(colorInfoB);
        rpInfoB.depthStencilAttachment.format = scB->getDepthStencilTexture()->getFormat();
        rpInfoB.depthStencilAttachment.barrier = nullptr;
        rpInfoB.subpasses.resize(1);
        rpInfoB.subpasses[0].colors = {0};
        rpInfoB.subpasses[0].depthStencil = 1;
        RenderPass *rpB = backend->createRenderPass(rpInfoB);
        FramebufferInfo fboInfoB;
        fboInfoB.renderPass = rpB;
        fboInfoB.colorTextures = {scB->getColorTexture()};
        fboInfoB.depthStencilTexture = scB->getDepthStencilTexture();
        Framebuffer *fboB = backend->createFramebuffer(fboInfoB);
        uint32_t fboGenB = gpuB()->generation;

        Swapchain *both[2] = {sc, scB};
        backend->acquire(both, 2);
        check(gpu()->wsi.hasRenderableImage(), "case5: A renderable after batch acquire");
        check(gpuB()->wsi.hasRenderableImage(), "case5: B renderable after batch acquire");
        const uint32_t bImageIndex = gpuB()->curImageIndex;
        const uint32_t bGeneration = gpuB()->generation;

        // ABORT A without submitting any user CB
        sc->resize(0, 0, SurfaceTransform::IDENTITY);
        check(!gpu()->wsi.hasRenderableImage(), "case5: A not renderable after zero resize");
        check(gpuB()->wsi.hasRenderableImage(), "case5: B keeps its acquire through A's abort");
        check(gpuB()->curImageIndex == bImageIndex, "case5: B image index unchanged");
        check(gpuB()->generation == bGeneration, "case5: B generation unchanged");

        // record and submit B's clear for real (destroy+delete below, not empty submit)
        if (gpuB()->generation != fboGenB) {
            fboB->destroy(); delete fboB;
            fboInfoB.colorTextures = {scB->getColorTexture()};
            fboInfoB.depthStencilTexture = scB->getDepthStencilTexture();
            fboB = backend->createFramebuffer(fboInfoB);
            fboGenB = gpuB()->generation;
        }
        cmd->begin();
        Rect areaB{0, 0, scB->getColorTexture()->getWidth(), scB->getColorTexture()->getHeight()};
        cmd->beginRenderPass(rpB, fboB, areaB, &clearColor, 1.0F, 0U);
        cmd->endRenderPass();
        cmd->end();
        queue->submit(reinterpret_cast<CommandBuffer **>(&cmd), 1);
        backend->present();
        check(gpuB()->lastPresentResult == VK_SUCCESS, "case5: B present SUCCESS after A abort");
        check(!gpu()->wsi.hasRenderableImage(), "case5: A stays non-renderable through B's present");
        check(errorCount() == errorsAtStart, "case5: surviving B clear/present has zero validation errors");
        std::printf("case5 survivor frame completed: deltaErrors=%u\n", errorCount() - errorsAtStart);

        // restore A; both windows run a fresh normal clear/present
        sc->resize(640, 480, SurfaceTransform::IDENTITY);
        backend->acquire(both, 2);
        check(gpu()->wsi.hasRenderableImage(), "case5: A renderable after restore");
        check(gpuB()->wsi.hasRenderableImage(), "case5: B renderable after restore");
        { QueueSubmitCounter submitCounter;
        if (gpu()->wsi.hasRenderableImage()) renderFrame();
        if (gpuB()->wsi.hasRenderableImage()) {
            if (gpuB()->generation != fboGenB) {
                fboB->destroy(); delete fboB;
                fboInfoB.colorTextures = {scB->getColorTexture()};
                fboInfoB.depthStencilTexture = scB->getDepthStencilTexture();
                fboB = backend->createFramebuffer(fboInfoB);
                fboGenB = gpuB()->generation;
            }
            cmd->begin();
            Rect areaB2{0, 0, scB->getColorTexture()->getWidth(), scB->getColorTexture()->getHeight()};
            cmd->beginRenderPass(rpB, fboB, areaB2, &clearColor, 1.0F, 0U);
            cmd->endRenderPass();
            cmd->end();
            queue->submit(reinterpret_cast<CommandBuffer **>(&cmd), 1);
        }
        backend->present();
        check(g_queueSubmitCalls == 2, "case5: two independent draws retain exactly two actual submits");
        std::printf("case5 recovery frame completed: submits=%u deltaErrors=%u\n",
                    g_queueSubmitCalls, errorCount() - errorsAtStart);
        }
        check(errorCount() == errorsAtStart, "case5: restored A/B independent submits have zero validation errors");

        // proper destroy+delete for B's objects (never just an empty submit)
        fboB->destroy(); delete fboB;
        rpB->destroy(); delete rpB;
        scB->destroy(); delete scB;
        windowB.destroy();
        completedMarker = true;
    } else if (caseName == "reacquire-stale-record") {
        // ---- record a clear, then a REAL re-acquire (no resize) aborts the
        //      previous frame; the stale CB must be gated by acquire-serial
        //      (generation may be UNCHANGED - proves it is not generation-only).
        backend->acquire(&sc, 1);
        check(gpu()->wsi.hasRenderableImage(), "case6: first acquire renderable");
        const uint32_t oldIndex = gpu()->curImageIndex;
        const uint32_t oldGeneration = gpu()->generation;
        const uint64_t oldSerial = gpu()->imageGeneration->acquireSerials[oldIndex];

        // record a real clear but DO NOT submit
        cmd->begin();
        Rect area{0, 0, sc->getColorTexture()->getWidth(), sc->getColorTexture()->getHeight()};
        cmd->beginRenderPass(rp, fbo, area, &clearColor, 1.0F, 0U);
        cmd->endRenderPass();
        cmd->end();

        // re-acquire the same swapchain: aborts the unsubmitted frame
        backend->acquire(&sc, 1);
        check(gpu()->wsi.hasRenderableImage(), "case6: re-acquire renderable");
        const uint32_t newIndex = gpu()->curImageIndex;
        const uint32_t newGeneration = gpu()->generation;
        const uint64_t newSerial = gpu()->imageGeneration->acquireSerials[newIndex];
        std::printf("case6 acquire: oldIndex=%u newIndex=%u oldGen=%u newGen=%u oldSerial=%llu newSerial=%llu\n",
                    oldIndex, newIndex, oldGeneration, newGeneration,
                    static_cast<unsigned long long>(oldSerial), static_cast<unsigned long long>(newSerial));
        if (oldGeneration == newGeneration && oldIndex == newIndex) {
            check(newSerial > oldSerial, "case6: same-generation same-image reacquire advances serial");
        }

        // submitting the stale CB must be gated (discarded +1) even when the
        // generation is UNCHANGED and the image index happens to repeat.
        const auto discardedBefore = static_cast<CCVKQueue *>(queue)->gpuQueue()->discardedWSICommandBuffers;
        queue->submit(reinterpret_cast<CommandBuffer **>(&cmd), 1);
        check(static_cast<CCVKQueue *>(queue)->gpuQueue()->discardedWSICommandBuffers == discardedBefore + 1,
              "case6: stale command buffer rejected by acquire-serial gate");
        backend->present();

        // fresh legal CB on the current generation renders/presents cleanly
        backend->acquire(&sc, 1);
        check(gpu()->wsi.hasRenderableImage(), "case6: renderable for fresh frame");
        if (gpu()->wsi.hasRenderableImage()) renderFrame();
        backend->present();

        // ---- pure-CPU synthetic counterexample (no Vulkan calls, no GPU
        //      state touched; explicitly NOT driver evidence): the same
        //      generation + same image index but a DIFFERENT acquire serial
        //      must invalidate a CCVKGPUWSIUsage snapshot.
        {
            auto synth = std::make_shared<CCVKGPUWSIGeneration>(); // all Vk handles null
            synth->imageAcquired = {true};
            synth->acquireSerials = {7};
            CCVKGPUWSIUsage recorded{synth, 0, 7};
            check(recorded.valid(), "case6 synthetic: current serial valid");
            synth->acquireSerials[0] = 8;
            check(!recorded.valid(), "case6 synthetic: reacquire invalidates the old same-image record");
            CCVKGPUWSIUsage fresh{synth, 0, 8};
            check(fresh.valid(), "case6 synthetic: fresh serial becomes valid");
            synth->retired = true;
            check(!fresh.valid(), "case6 synthetic: retired generation invalidates even matching serial");
        }
        completedMarker = true;
    }

    // ---------------------------------------------------------------- teardown
    raw.waitAllFences();
    const auto *completionDevice = raw.gpuDevice();
    const bool timelineCompletion = completionDevice->runtimePaths.useTimelineSemaphore;
    check(completionDevice->trackedSubmitCount > 0, "lifecycle reached real tracked submits");
    check(timelineCompletion ? completionDevice->submissionFenceCount == 0 : completionDevice->submissionFenceCount == completionDevice->trackedSubmitCount,
          "timeline submits use no submission fence; fence backend tracks every submit");
    for (const auto &entry : completionDevice->completionTrackers) {
        check(entry.second->usesTimeline() == timelineCompletion, "all queues use selected completion backend");
        check(entry.second->completedValue() == entry.second->submittedValue(), "wait-all observes latest actual point per queue");
    }
    std::printf("completion backend=%s submits=%llu submission-fences=%llu\n",
                timelineCompletion ? "timeline" : "fence",
                static_cast<unsigned long long>(completionDevice->trackedSubmitCount),
                static_cast<unsigned long long>(completionDevice->submissionFenceCount));
    const uint32_t liveErrors = errorCount() - errorsAtStart;
    if (fbo) { fbo->destroy(); delete fbo; fbo = nullptr; }
    if (rp) { rp->destroy(); delete rp; rp = nullptr; }
    if (sc) { sc->destroy(); delete sc; sc = nullptr; }
    if (cmd) { cmd->destroy(); delete cmd; cmd = nullptr; }
    window.destroy();
    UnregisterClassW(L"CocosWsiLifeSmoke", GetModuleHandle(nullptr));
    ownedDevice->destroy();
    const uint32_t postErrors = raw.getLastValidationErrorCount();

    std::printf("%s completed: failures=%u liveErrors=%u postErrors=%u\n",
                caseName.c_str(), g_failures, liveErrors, postErrors);
    if (!completedMarker) std::printf("WARNING: case did not reach its completion marker\n");
    return (g_failures || liveErrors || postErrors || !completedMarker) ? 1 : 0;
}
