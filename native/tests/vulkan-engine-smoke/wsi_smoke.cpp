// ============================================================================
// Real Win32 WSI stress harness (M2-D).
//
// Real Cocos/Vulkan window flow (acquire -> clear -> submit -> present) and
// lifecycle events on ACTUAL swapchains/windows, with Khronos + Best
// Practices + Synchronization validation REQUIRED. A missing layer is a
// failure, not a pass. This is NOT a visual baseline (no shader/pipeline/
// draw) and NOT Android/XR evidence.
//
// Scenarios:
//   S1  >=120 stable frames (index validity around acquire/present)
//   S2  30 alternating resizes (generation-tracked FBO rebuild)
//   S3  explicit resize(0,0) suspend -> restore
//   S4  destroySurface -> createSurface (same HWND) recovery
//   S5  two windows/swapchains, one suspended while the other keeps pairing
//   S6  all-failed acquire: empty command-buffer submit legal, present empty
//   S7  acquire-result injection via the volk function pointer (documented;
//       OUT_OF_DATE/SURFACE_LOST injected WITHOUT a real acquire, SUBOPTIMAL
//       via a real acquire with a rewritten result)
//
// Synchronization policy: NO per-frame waitIdle. Only scenario transitions
// may rely on the engine's own acquire-time recreation (which waits fences).
// Those waits are submission fences and do NOT prove present completion.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "gfx-base/SPIRVUtils.h"
#include "gfx-base/GFXDef.h"
#include "gfx-vulkan/VKDevice.h"
#include "gfx-vulkan/VKGPUObjects.h"
#include "gfx-vulkan/VKRenderPass.h"
#include "gfx-vulkan/VKSwapchain.h"
#include "gfx-vulkan/VKUtils.h"
#include "gfx-vulkan/VKWSIState.h"
#include "renderer/GFXDeviceManager.h"
#include "platform/FileUtils.h"

#include <cstdio>
#include <filesystem>
#include <string>

// The platform object links its application hooks, but this test starts no app.
int cocos_main(int, const char **) { return 0; }
void cocos_destroy() {}

namespace {

unsigned g_failures = 0;
bool g_stopScenarios = false; // set on error-budget overrun

void fail(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "FAIL: ");
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    va_end(args);
    ++g_failures;
}

void check(bool ok, const char *what) {
    if (!ok) fail("%s", what);
}

// ---------------------------------------------------------------- TestWindow
struct TestWindow {
    HWND hwnd = nullptr;
    static const wchar_t *className() { return L"CocosWsiSmoke"; }

    static void registerClass() {
        static bool done = false;
        if (done) return;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = className();
        RegisterClassExW(&wc);
        done = true;
    }
    static TestWindow create(int w, int h) {
        registerClass();
        TestWindow win;
        // SW_SHOWNOACTIVATE: presentable without stealing user focus
        win.hwnd = CreateWindowExW(0, className(), L"wsi-smoke", WS_OVERLAPPEDWINDOW,
                                   CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                                   nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
        ShowWindow(win.hwnd, SW_SHOWNOACTIVATE);
        return win;
    }
    void resize(int w, int h) {
        SetWindowPos(hwnd, nullptr, 0, 0, w, h,
                     SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER);
    }
    void destroy() {
        if (hwnd) {
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
    }
};

// ------------------------------------------------- acquire-result injection
// Reassigns volk's global function pointer (documented result injection, NOT
// a driver error). OUT_OF_DATE/SURFACE_LOST are returned WITHOUT calling the
// real acquire (no semaphore signaled, no index touched). SUBOPTIMAL calls
// the real acquire first (safe semaphore signal) and rewrites the result.
PFN_vkAcquireNextImageKHR g_realAcquire = nullptr;
VkSwapchainKHR g_injectTarget = VK_NULL_HANDLE;
VkResult g_injectResult = VK_SUCCESS;
int g_injectRemaining = 0;

VKAPI_ATTR VkResult VKAPI_CALL hookedAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                         uint64_t timeout, VkSemaphore semaphore,
                                                         VkFence fence, uint32_t *pImageIndex) {
    if (swapchain == g_injectTarget && g_injectRemaining > 0) {
        --g_injectRemaining;
        if (g_injectResult == VK_SUBOPTIMAL_KHR) {
            const VkResult real = g_realAcquire(device, swapchain, timeout, semaphore, fence, pImageIndex);
            return real == VK_SUCCESS ? VK_SUBOPTIMAL_KHR : real;
        }
        return g_injectResult; // OUT_OF_DATE / SURFACE_LOST: no real acquire
    }
    return g_realAcquire(device, swapchain, timeout, semaphore, fence, pImageIndex);
}

struct AcquireHook {
    AcquireHook(VkSwapchainKHR target, VkResult result, int count) {
        g_realAcquire = vkAcquireNextImageKHR;
        g_injectTarget = target;
        g_injectResult = result;
        g_injectRemaining = count;
        vkAcquireNextImageKHR = &hookedAcquireNextImageKHR;
    }
    ~AcquireHook() {
        vkAcquireNextImageKHR = g_realAcquire;
        g_realAcquire = nullptr;
        g_injectTarget = VK_NULL_HANDLE;
        g_injectRemaining = 0;
    }
};

// Observe actual Vulkan dispatch after device init. The trackers hold PFN
// addresses, so hooks installed here see both user and transport/drain submits.
PFN_vkQueueSubmit realSubmit1 = nullptr;
PFN_vkQueueSubmit2 realSubmit2 = nullptr, realSubmit2KHR = nullptr;
uint64_t observedLegacy=0, observedModern=0;
VKAPI_ATTR VkResult VKAPI_CALL observeSubmit1(VkQueue q,uint32_t n,const VkSubmitInfo *s,VkFence f) {
    const auto r=realSubmit1(q,n,s,f); if(r==VK_SUCCESS) ++observedLegacy; return r;
}
VKAPI_ATTR VkResult VKAPI_CALL observeSubmit2(VkQueue q,uint32_t n,const VkSubmitInfo2 *s,VkFence f) {
    const auto r=realSubmit2(q,n,s,f); if(r==VK_SUCCESS) ++observedModern; return r;
}
VKAPI_ATTR VkResult VKAPI_CALL observeSubmit2KHR(VkQueue q,uint32_t n,const VkSubmitInfo2 *s,VkFence f) {
    const auto r=realSubmit2KHR(q,n,s,f); if(r==VK_SUCCESS) ++observedModern; return r;
}
PFN_vkCmdPipelineBarrier realBarrier1 = nullptr;
PFN_vkCmdPipelineBarrier2 realBarrier2 = nullptr, realBarrier2KHR = nullptr;
uint64_t observedBarrier1=0, observedBarrier2=0;
VKAPI_ATTR void VKAPI_CALL observeBarrier1(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags flags,
    uint32_t memoryCount, const VkMemoryBarrier *memory, uint32_t bufferCount, const VkBufferMemoryBarrier *buffers,
    uint32_t imageCount, const VkImageMemoryBarrier *images) {
    realBarrier1(cb,src,dst,flags,memoryCount,memory,bufferCount,buffers,imageCount,images); ++observedBarrier1;
}
VKAPI_ATTR void VKAPI_CALL observeBarrier2(VkCommandBuffer cb,const VkDependencyInfo *info) {
    realBarrier2(cb,info); ++observedBarrier2;
}
VKAPI_ATTR void VKAPI_CALL observeBarrier2KHR(VkCommandBuffer cb,const VkDependencyInfo *info) {
    realBarrier2KHR(cb,info); ++observedBarrier2;
}
struct SubmitObservation {
    SubmitObservation() {
        realSubmit1=vkQueueSubmit;realSubmit2=vkQueueSubmit2;realSubmit2KHR=vkQueueSubmit2KHR;
        observedLegacy=observedModern=0;
        realBarrier1=vkCmdPipelineBarrier;realBarrier2=vkCmdPipelineBarrier2;realBarrier2KHR=vkCmdPipelineBarrier2KHR;
        observedBarrier1=observedBarrier2=0;
        vkCmdPipelineBarrier=observeBarrier1;
        if(realBarrier2) vkCmdPipelineBarrier2=observeBarrier2;
        if(realBarrier2KHR) vkCmdPipelineBarrier2KHR=observeBarrier2KHR;
        vkQueueSubmit=observeSubmit1;
        if(realSubmit2) vkQueueSubmit2=observeSubmit2;
        if(realSubmit2KHR) vkQueueSubmit2KHR=observeSubmit2KHR;
    }
    ~SubmitObservation() {
        vkQueueSubmit=realSubmit1;vkQueueSubmit2=realSubmit2;vkQueueSubmit2KHR=realSubmit2KHR;
        vkCmdPipelineBarrier=realBarrier1;vkCmdPipelineBarrier2=realBarrier2;vkCmdPipelineBarrier2KHR=realBarrier2KHR;
    }
};

// ------------------------------------------------------------------ helpers
using namespace cc;
using namespace cc::gfx;

struct PassSet {
    RenderPass *rp = nullptr;
    Framebuffer *fbo = nullptr;
    uint32_t generation = 0xFFFFFFFFu;
};

// destroy + DELETE the wrapper objects and null the pointers (idempotent).
// The CCVK wrapper destroy() only drops GPU ownership; the C++ object must be
// deleted too, and CCVKCommandBuffer can hold an IntrusivePtr to the GPU
// render pass, so wrappers must be released before the device is destroyed.
inline void destroyPassSet(PassSet &set) {
    if (set.fbo) {
        set.fbo->destroy();
        delete set.fbo;
        set.fbo = nullptr;
    }
    if (set.rp) {
        set.rp->destroy();
        delete set.rp;
        set.rp = nullptr;
    }
    set.generation = 0xFFFFFFFFu;
}

void buildPass(Device *backend, Swapchain *sc, PassSet &set) {
    auto *color = sc->getColorTexture();
    auto *depth = sc->getDepthStencilTexture();
    const uint32_t generation = static_cast<CCVKSwapchain *>(sc)->gpuSwapchain()->generation;
    if (set.rp && set.generation == generation) return;
    destroyPassSet(set); // release old wrapper objects before creating new ones

    GeneralBarrierInfo colorInfo;
    colorInfo.prevAccesses = AccessFlagBit::PRESENT;  // image waits in PRESENT after acquire
    colorInfo.nextAccesses = AccessFlagBit::PRESENT;  // finalLayout = PRESENT for the queue

    RenderPassInfo info;
    info.colorAttachments.resize(1);
    info.colorAttachments[0].format = color->getFormat();
    info.colorAttachments[0].loadOp = LoadOp::CLEAR;
    info.colorAttachments[0].storeOp = StoreOp::STORE;
    // device-cached backend barrier (CCVKGeneralBarrier): a plain
    // gfx::GeneralBarrier lacks the gpuBarrier() state the Vulkan
    // render-pass builder reads.
    info.colorAttachments[0].barrier = backend->getGeneralBarrier(colorInfo);
    info.depthStencilAttachment.format = depth->getFormat();
    info.depthStencilAttachment.depthLoadOp = LoadOp::CLEAR;
    info.depthStencilAttachment.depthStoreOp = StoreOp::STORE;
    info.depthStencilAttachment.stencilLoadOp = LoadOp::CLEAR;
    info.depthStencilAttachment.stencilStoreOp = StoreOp::STORE;
    info.depthStencilAttachment.barrier = nullptr; // device default DS barrier
    info.subpasses.resize(1);
    info.subpasses[0].colors = {0};
    info.subpasses[0].depthStencil = 1; // attachment INDEX of the DS attachment (0 = color)
    set.rp = backend->createRenderPass(info);

    // M5-G: verify per-subpass metadata extracted from the real classic RP.
    // Evidence of real-RP tuple extraction only; NOT Dynamic pipeline proof.
    {
        auto *gpuRP = static_cast<CCVKRenderPass *>(set.rp)->gpuRenderPass();
        if (gpuRP->renderingMetadata.empty() ||
            gpuRP->renderingMetadata.size() != gpuRP->subpasses.size() ||
            gpuRP->sampleCounts.empty() || gpuRP->sampleCounts.size() != gpuRP->subpasses.size()) {
            ++g_failures;
            std::fprintf(stderr, "FAIL: metadata/sampleCounts size mismatch\n");
        } else {
            const auto &meta = gpuRP->renderingMetadata[0];
            if (!meta.valid) {
                ++g_failures;
                std::fprintf(stderr, "FAIL: subpass 0 metadata invalid\n");
            } else {
                const auto wantColor = mapVkFormat(color->getFormat(), CCVKDevice::getInstance()->gpuDevice());
                if (meta.signature.colorFormats.size() != 1 ||
                    meta.signature.colorFormats[0] != wantColor) {
                    ++g_failures;
                    std::fprintf(stderr, "FAIL: metadata color format mismatch\n");
                }
                const VkFormat wantDs = mapVkFormat(depth->getFormat(), CCVKDevice::getInstance()->gpuDevice());
                const FormatInfo &fi = GFX_FORMAT_INFOS[static_cast<uint32_t>(depth->getFormat())];
                if (meta.signature.depthFormat != (fi.hasDepth ? wantDs : VK_FORMAT_UNDEFINED) ||
                    meta.signature.stencilFormat != (fi.hasStencil ? wantDs : VK_FORMAT_UNDEFINED)) {
                    ++g_failures;
                    std::fprintf(stderr, "FAIL: metadata DS format mismatch\n");
                }
                if (meta.signature.samples != gpuRP->sampleCounts[0]) {
                    ++g_failures;
                    std::fprintf(stderr, "FAIL: metadata samples mismatch\n");
                }
                if (meta.signature.viewMask != 0) {
                    ++g_failures;
                    std::fprintf(stderr, "FAIL: metadata viewMask nonzero\n");
                }
            }
        }
    }

    // M5-G rev: one-time real-RP evidence for the attachmentless fix: a
    // default RenderPassInfo creates one default subpass with no attachments;
    // its metadata must be valid with a fully empty tuple (samples 1,
    // viewMask 0). Destroyed immediately; no record/draw/present to it.
    {
        static bool attachmentlessChecked = false;
        if (!attachmentlessChecked) {
            attachmentlessChecked = true;
            RenderPassInfo emptyInfo;
            RenderPass *emptyRp = backend->createRenderPass(emptyInfo);
            if (emptyRp == nullptr) {
                ++g_failures;
                std::fprintf(stderr, "FAIL: attachmentless RP create returned null\n");
            } else {
                auto *gpuRP = static_cast<CCVKRenderPass *>(emptyRp)->gpuRenderPass();
                if (gpuRP->renderingMetadata.size() != 1 || gpuRP->sampleCounts.size() != 1) {
                    ++g_failures;
                    std::fprintf(stderr, "FAIL: attachmentless metadata size\n");
                } else {
                    const auto &meta = gpuRP->renderingMetadata[0];
                    if (!meta.valid || !meta.signature.colorFormats.empty() ||
                        meta.signature.depthFormat != VK_FORMAT_UNDEFINED ||
                        meta.signature.stencilFormat != VK_FORMAT_UNDEFINED ||
                        meta.signature.samples != VK_SAMPLE_COUNT_1_BIT ||
                        meta.signature.viewMask != 0 || gpuRP->vkRenderPass == VK_NULL_HANDLE) {
                        ++g_failures;
                        std::fprintf(stderr, "FAIL: attachmentless tuple not empty/valid\n");
                    }
                }
                emptyRp->destroy();
                delete emptyRp;
            }
        }
    }

    FramebufferInfo fboInfo;
    fboInfo.renderPass = set.rp;
    fboInfo.colorTextures = {color};
    fboInfo.depthStencilTexture = depth;
    set.fbo = backend->createFramebuffer(fboInfo);
    set.generation = generation;
}

CCVKGPUSwapchain *gs(Swapchain *sc) {
    return static_cast<CCVKSwapchain *>(sc)->gpuSwapchain();
}

} // namespace

#include "completion_benchmark.h"

int main(int argc, char **argv) {
    BenchmarkOptions benchmark;
    if (!benchmarkOptions(argc, argv, benchmark)) {
        std::fprintf(stderr, "FAIL: invalid benchmark arguments\n");
        return 1;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    // NOTE: Device::isSupportDetachDeviceThread is protected in this engine
    // revision and cannot be set from a test; the harness runs single-threaded
    // frame flow anyway (no render thread is started without an application).

    const auto outDir = std::filesystem::current_path() / "wsi-smoke-out";
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
    Device *backend = &raw; // raw backend: public Device overloads, no wrapper proxies
    auto *context = raw.gpuContext();
    const bool validation = context->validationEnabled && context->bestPracticesValidationEnabled &&
                            context->synchronizationValidationEnabled;
    std::printf("validation + best practices + sync: %s\n", validation ? "ENABLED" : "NOT VERIFIED");
    if (benchmark.enabled) {
        if (validation != benchmark.validation || (!benchmark.validation && context->validationEnabled)) {
            std::fprintf(stderr, "FAIL: benchmark validation setting does not match build/runtime\n");
            ownedDevice->destroy(); return 1;
        }
        backend->enableAutoBarrier(false);
        return runCompletionBenchmark(ownedDevice, benchmark);
    }
    if (!validation) {
        std::fprintf(stderr, "FAIL: validation layers are REQUIRED for this harness\n");
        ownedDevice->destroy();
        return 1;
    }

    // Auto-barrier (deduced acquire/present barriers) disabled ONCE here: the
    // render pass below already encodes PRESENT -> PRESENT transitions, and
    // the deduced late barrier would duplicate the final transition. This is
    // a fixed harness configuration, not a mid-run VUID workaround.
    backend->enableAutoBarrier(false);
    std::printf("harness config: enableAutoBarrier(false); RP color barrier PRESENT->PRESENT\n");

    SubmitObservation submitObservation;
    const auto initialSubmits = raw.gpuDevice()->trackedSubmitCount;
    auto *queue = backend->getQueue();
    CommandBufferInfo cbInfo;
    cbInfo.queue = queue; // CCVKCommandBuffer::doInit dereferences the queue
    CommandBuffer *cmd = backend->createCommandBuffer(cbInfo);

    TestWindow windowA = TestWindow::create(640, 480);
    if (!windowA.hwnd) { fail("CreateWindowExW A"); }

    SwapchainInfo infoA;
    infoA.windowHandle = windowA.hwnd;
    infoA.width = 640;
    infoA.height = 480;
    infoA.vsyncMode = VsyncMode::OFF;
    Swapchain *scA = backend->createSwapchain(infoA);
    if (!scA) fail("createSwapchain A");

    PassSet passA;
    buildPass(backend, scA, passA);

    const Color clearColor{0.12f, 0.20f, 0.32f, 1.0f};
    const auto renderFrame = [&](Swapchain *sc, PassSet &set) {
        buildPass(backend, sc, set); // generation-tracked proactive rebuild
        auto *color = sc->getColorTexture();
        cmd->begin();
        Rect area{0, 0, color->getWidth(), color->getHeight()};
        cmd->beginRenderPass(set.rp, set.fbo, area, &clearColor, 1.0F, 0U);
        cmd->endRenderPass();
        cmd->end();
        queue->submit(reinterpret_cast<CommandBuffer **>(&cmd), 1);
    };

    // LIVE per-frame counter (design: getLastValidationErrorCount() is a
    // post-destroy cache and reads 0 while the device is alive).
    const uint32_t errorBudget = 10; // stop scenarios instead of flooding VUID output
    const auto errorCount = [&raw] {
        return raw.gpuContext()->validationErrorCount.load(std::memory_order_relaxed);
    };
    bool budgetStopped = false;
    const char *skippedAfter = nullptr;
    const auto budgetCheck = [&](const char *scenario) {
        if (g_stopScenarios || budgetStopped) return;
        if (errorCount() > errorBudget) {
            fail("validation error budget exceeded (%u) during/after %s; remaining scenarios SKIPPED",
                 errorCount(), scenario);
            budgetStopped = true;
            g_stopScenarios = true;
            skippedAfter = scenario;
        }
    };
    const char *currentScenario = "startup";

    // ---------------------------------------------------------------- S1: stable frames
    if (!g_stopScenarios) {
        uint32_t presented = 0;
        for (uint32_t f = 0; f < 120 && !g_stopScenarios; ++f) {
            backend->acquire(&scA, 1);
            const CCVKGPUSwapchain *gpu = gs(scA);
            check(gpu->wsi.hasRenderableImage(), "S1: renderable index after acquire");
            check(gpu->curImageIndex < gpu->swapchainImages.size(), "S1: index in range");
            if (gpu->wsi.hasRenderableImage()) {
                renderFrame(scA, passA);
                backend->present();
                ++presented;
                check(!gs(scA)->wsi.hasRenderableImage(), "S1: index invalid after present");
            } else {
                backend->present(); // empty present path
            }
            if (f % 30 == 29) budgetCheck("S1");
        }
        std::printf("S1 stable frames: presented=%u/120\n", presented);
        check(presented >= 110, "S1: at least 110 of 120 frames presented");
    }

    // ---------------------------------------------------------------- S2: resizes
    if (!g_stopScenarios) {
        const uint32_t sizes[][2] = {{800, 600}, {640, 480}, {1024, 768}, {512, 512}};
        uint32_t lastGeneration = gs(scA)->generation;
        uint32_t resized = 0;
        for (int i = 0; i < 30 && !g_stopScenarios; ++i) {
            const auto &size = sizes[i % 4];
            windowA.resize(size[0], size[1]);
            static_cast<Swapchain *>(scA)->resize(size[0], size[1], SurfaceTransform::IDENTITY);
            backend->acquire(&scA, 1);
            const CCVKGPUSwapchain *gpu = gs(scA);
            check(gpu->wsi.hasRenderableImage(), "S2: renderable after resize");
            if (gpu->wsi.hasRenderableImage()) {
                renderFrame(scA, passA); // rebuilds FBO if generation changed
                backend->present();
                ++resized;
            } else {
                backend->present();
            }
            if (gpu->generation != lastGeneration) {
                lastGeneration = gpu->generation;
            }
            if (i % 10 == 9) budgetCheck("S2");
        }
        std::printf("S2 resizes: presented=%u/30, final generation=%u\n", resized, gs(scA)->generation);
        check(resized >= 25, "S2: at least 25 of 30 resized frames presented");
    }

    // ---------------------------------------------------------------- S3: zero-size suspend
    if (!g_stopScenarios) {
        scA->resize(0, 0, SurfaceTransform::IDENTITY);
        const CCVKGPUSwapchain *gpu = gs(scA);
        check(gpu->wsi.state() == VKSwapchainState::SUSPENDED_ZERO_SIZE, "S3: suspended state");
        check(!gpu->wsi.hasRenderableImage(), "S3: no renderable index while suspended");
        for (int f = 0; f < 5; ++f) {
            backend->acquire(&scA, 1);
            check(!gs(scA)->wsi.hasRenderableImage(), "S3: acquire skipped while suspended");
            backend->present(); // empty present, must not present
        }
        scA->resize(640, 480, SurfaceTransform::IDENTITY);
        backend->acquire(&scA, 1);
        check(gs(scA)->wsi.hasRenderableImage(), "S3: renderable after restore");
        if (gs(scA)->wsi.hasRenderableImage()) {
            renderFrame(scA, passA);
            backend->present();
        }
        budgetCheck("S3");
        std::printf("S3 zero-size suspend/restore done\n");
    }

    // ---------------------------------------------------------------- S4: surface destroy/create
    if (!g_stopScenarios) {
        scA->destroySurface();
        check(gs(scA)->wsi.state() == VKSwapchainState::SURFACE_LOST, "S4: SURFACE_LOST after destroySurface");
        backend->acquire(&scA, 1);
        check(!gs(scA)->wsi.hasRenderableImage(), "S4: acquire skipped while lost");
        backend->present();
        scA->createSurface(windowA.hwnd);
        backend->acquire(&scA, 1);
        check(gs(scA)->wsi.hasRenderableImage(), "S4: renderable after surface recreate");
        if (gs(scA)->wsi.hasRenderableImage()) {
            renderFrame(scA, passA);
            backend->present();
        }
        budgetCheck("S4");
        std::printf("S4 surface destroy/recreate done, generation=%u\n", gs(scA)->generation);
    }

    // ---------------------------------------------------------------- S5: two windows
    Swapchain *scB = nullptr;
    TestWindow windowB;
    PassSet passB;
    if (!g_stopScenarios) {
        windowB = TestWindow::create(512, 512);
        SwapchainInfo infoB;
        infoB.windowHandle = windowB.hwnd;
        infoB.width = 512;
        infoB.height = 512;
        infoB.vsyncMode = VsyncMode::OFF;
        scB = backend->createSwapchain(infoB);
        check(scB != nullptr, "S5: createSwapchain B");

        Swapchain *both[2] = {scA, scB};
        for (int f = 0; f < 10 && !g_stopScenarios; ++f) {
            backend->acquire(both, 2);
            const bool aOK = gs(scA)->wsi.hasRenderableImage();
            const bool bOK = gs(scB)->wsi.hasRenderableImage();
            check(aOK && bOK, "S5: both renderable");
            if (aOK) renderFrame(scA, passA);
            if (bOK) renderFrame(scB, passB);
            backend->present();
        }
        // suspend A; B must keep pairing
        scA->resize(0, 0, SurfaceTransform::IDENTITY);
        uint32_t bPresented = 0;
        for (int f = 0; f < 10 && !g_stopScenarios; ++f) {
            backend->acquire(both, 2);
            check(!gs(scA)->wsi.hasRenderableImage(), "S5: A skipped while zero");
            const bool bOK = gs(scB)->wsi.hasRenderableImage();
            check(bOK, "S5: B still renderable while A suspended");
            if (bOK) {
                renderFrame(scB, passB);
                ++bPresented;
            }
            backend->present();
            check(gs(scB)->lastPresentResult == VK_SUCCESS, "S5: B present result SUCCESS");
        }
        std::printf("S5 two-window: B presented=%u/10 while A suspended\n", bPresented);
        check(bPresented >= 9, "S5: B kept presenting");
        budgetCheck("S5");
    }

    // ---------------------------------------------------------------- S6: all failed -> empty submit legal
    if (!g_stopScenarios && scB) {
        scB->resize(0, 0, SurfaceTransform::IDENTITY);
        Swapchain *both[2] = {scA, scB};
        backend->acquire(both, 2);
        check(!gs(scA)->wsi.hasRenderableImage() && !gs(scB)->wsi.hasRenderableImage(),
              "S6: both suspended, nothing renderable");
        // empty command buffer submit is legal without a swapchain acquire
        cmd->begin();
        cmd->end();
        queue->submit(reinterpret_cast<CommandBuffer **>(&cmd), 1);
        backend->present(); // empty present, engine must skip vkQueuePresentKHR
        std::printf("S6 all-failed acquire: empty submit + empty present done\n");
        budgetCheck("S6");
    }

    // ---------------------------------------------------------------- S7: result injection
    if (!g_stopScenarios && scB) {
        scB->resize(512, 512, SurfaceTransform::IDENTITY);
        // restore A too
        scA->resize(640, 480, SurfaceTransform::IDENTITY);
        Swapchain *both[2] = {scA, scB};

        // (a) OUT_OF_DATE for A, injected without a real acquire
        {
            AcquireHook hook(gs(scA)->vkSwapchain, VK_ERROR_OUT_OF_DATE_KHR, 1);
            backend->acquire(both, 2);
            check(!gs(scA)->wsi.hasRenderableImage(), "S7a: A not renderable after injected OUT_OF_DATE");
            check(gs(scA)->wsi.needsSwapchainRecreate(), "S7a: A needs swapchain recreate");
            const bool bOK = gs(scB)->wsi.hasRenderableImage();
            check(bOK, "S7a: B independent of A's failure");
            if (bOK) renderFrame(scB, passB);
            backend->present();
        }
        // A recovers on the next acquire (engine recreates from OUT_OF_DATE)
        backend->acquire(both, 2);
        if (gs(scA)->wsi.hasRenderableImage()) {
            renderFrame(scA, passA);
        } else {
            std::printf("S7a note: A did not recover on the immediately following acquire\n");
        }
        backend->present();

        // (b) SURFACE_LOST for A; test drives explicit surface recreate (M2-C1
        // does not rebuild the native surface automatically)
        {
            AcquireHook hook(gs(scA)->vkSwapchain, VK_ERROR_SURFACE_LOST_KHR, 1);
            backend->acquire(both, 2);
            check(!gs(scA)->wsi.hasRenderableImage(), "S7b: A not renderable after injected SURFACE_LOST");
            check(gs(scA)->wsi.state() == VKSwapchainState::SURFACE_LOST, "S7b: A SURFACE_LOST state");
            check(gs(scB)->wsi.hasRenderableImage(), "S7b: B independent");
            if (gs(scB)->wsi.hasRenderableImage()) renderFrame(scB, passB);
            backend->present();
        }
        scA->destroySurface();
        scA->createSurface(windowA.hwnd);
        backend->acquire(both, 2);
        check(gs(scA)->wsi.hasRenderableImage(), "S7b: A renderable after explicit surface recreate");
        if (gs(scA)->wsi.hasRenderableImage()) {
            renderFrame(scA, passA);
        }
        backend->present();
        budgetCheck("S7b");

        // (c) SUBOPTIMAL for A via a REAL acquire with a rewritten result
        {
            AcquireHook hook(gs(scA)->vkSwapchain, VK_SUBOPTIMAL_KHR, 1);
            backend->acquire(both, 2);
            check(gs(scA)->wsi.hasRenderableImage(), "S7c: A renderable under SUBOPTIMAL");
            check(gs(scA)->wsi.state() == VKSwapchainState::SUBOPTIMAL, "S7c: A SUBOPTIMAL state");
            if (gs(scA)->wsi.hasRenderableImage()) {
                renderFrame(scA, passA);
            }
            if (gs(scB)->wsi.hasRenderableImage()) renderFrame(scB, passB);
            backend->present();
        }
        const auto postState = gs(scA)->wsi.state();
        std::printf("S7c post-present state: %d (deferred recreate expected)\n", static_cast<int>(postState));
        check(!gs(scA)->wsi.hasRenderableImage(), "S7c: index consumed by present");
        // next acquire should eventually recreate; observe honestly
        backend->acquire(both, 2);
        std::printf("S7c next acquire: A renderable=%d state=%d\n",
                    (int)gs(scA)->wsi.hasRenderableImage(), (int)gs(scA)->wsi.state());
        backend->present();
        budgetCheck("S7");
        std::printf("S7 result injection: completed\n");
    }

    // ---------------------------------------------------------------- S8: offscreen submit between present and next acquire
    // Regression for the post-present cleanup queue: an offscreen empty
    // command-buffer submission between one frame's present and the next
    // acquire must not consume the previous frame's present semaphore or a
    // stale/INVALID image index, and must stay validation-clean.
    if (!g_stopScenarios && scB) {
        const uint32_t errorsBeforeS8 = errorCount();
        backend->acquire(&scA, 1);
        check(gs(scA)->wsi.hasRenderableImage(), "S8: normal frame renderable");
        if (gs(scA)->wsi.hasRenderableImage()) {
            renderFrame(scA, passA);
        }
        backend->present();
        check(!gs(scA)->wsi.hasRenderableImage(), "S8: index consumed by present");

        // offscreen EMPTY submit strictly between present and next acquire
        cmd->begin();
        cmd->end();
        queue->submit(reinterpret_cast<CommandBuffer **>(&cmd), 1);
        check(!gs(scA)->wsi.hasRenderableImage(), "S8: offscreen submit did not resurrect the index");

        backend->present(); // empty present: nothing to present, nothing consumed
        check(!gs(scA)->wsi.hasRenderableImage(), "S8: empty present stays index-free");

        const uint32_t delta = errorCount() - errorsBeforeS8;
        check(delta == 0, "S8: sequence must add zero validation errors");
        std::printf("S8 offscreen-between-frames: completed (delta=%u)\n", delta);
        budgetCheck("S8");
    }

    // ---------------------------------------------------------------- teardown
    raw.waitAllFences();
    const auto *gpu = raw.gpuDevice();
    const bool timelineCompletion = gpu->runtimePaths.useTimelineSemaphore;
    check(timelineCompletion ? gpu->submissionFenceCount == 0 : gpu->submissionFenceCount == gpu->trackedSubmitCount,
          "selected completion backend controls actual submission fences");
    for (const auto &entry : gpu->completionTrackers) {
        check(entry.second->usesTimeline() == timelineCompletion, "actual tracker matches completion policy");
        check(entry.second->completedValue() == entry.second->submittedValue(), "all actual queue points completed");
    }
    std::printf("completion backend=%s submits=%llu submission-fences=%llu\n",
                timelineCompletion ? "timeline" : "fence",
                static_cast<unsigned long long>(gpu->trackedSubmitCount),
                static_cast<unsigned long long>(gpu->submissionFenceCount));
    const bool submit2 = gpu->runtimePaths.useSubmit2;
    check(observedLegacy + observedModern == gpu->trackedSubmitCount - initialSubmits, "actual Vulkan dispatch count matches tracked submits");
    check(submit2 ? (observedLegacy == 0 && observedModern > 0) : (observedModern == 0 && observedLegacy > 0), "actual Vulkan submit encoder matches selected policy");
    for (const auto &entry : gpu->completionTrackers) check(entry.second->usesSubmit2() == submit2, "all trackers select the same submit encoder");
    std::printf("submit encoder=%s observed-legacy=%llu observed-submit2=%llu\n", submit2 ? "submit2" : "legacy",
                static_cast<unsigned long long>(observedLegacy), static_cast<unsigned long long>(observedModern));
    const bool barrier2 = gpu->runtimePaths.useSynchronization2;
    check(barrier2 ? (observedBarrier1==0 && observedBarrier2>0) : (observedBarrier2==0 && observedBarrier1>0), "actual WSI barrier encoder matches selected policy");
    std::printf("barrier encoder=%s observed-legacy=%llu observed-sync2=%llu\n",barrier2?"sync2":"legacy",
                static_cast<unsigned long long>(observedBarrier1),static_cast<unsigned long long>(observedBarrier2));
    const uint32_t errorsBeforeTeardown = errorCount();
    destroyPassSet(passB);
    destroyPassSet(passA);
    if (scB) { scB->destroy(); delete scB; scB = nullptr; }
    if (scA) { scA->destroy(); delete scA; scA = nullptr; }
    cmd->destroy();
    delete cmd;
    cmd = nullptr;
    windowB.destroy();
    windowA.destroy();
    UnregisterClassW(TestWindow::className(), GetModuleHandle(nullptr));

    ownedDevice->destroy();
    const uint32_t totalErrors = raw.getLastValidationErrorCount();
    std::printf("WSI smoke: validation errors=%u (pre-teardown %u), failures=%u\n",
                totalErrors, errorsBeforeTeardown, g_failures);
    if (budgetStopped) std::printf("SCENARIOS SKIPPED after error budget [INCOMPLETE]\n");
    return (g_failures || totalErrors || budgetStopped) ? 1 : 0;
}
