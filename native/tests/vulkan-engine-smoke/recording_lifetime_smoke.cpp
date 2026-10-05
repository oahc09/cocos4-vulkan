// Recording-lifetime forensic harness (M3-G, Task B).
//
// Three cases, each in an independent process (--case <name>), all offscreen
// (no swapchain/window). Real engine, real device. Thin volk hooks observe
// actual vkDestroy*/vkQueueSubmit/vkBeginCommandBuffer/vkResetCommandPool
// calls without altering behavior.
//
//   control            : normal offscreen clear -> submit -> present -> destroy
//   delayed-resource   : record clear -> end, DON'T submit -> destroy FBO+tex
//                        -> rotate slots -> ALWAYS attempt submit old CB.
//                        Valid: (a) target retained by owning lease + clean
//                        submit, or (b) target destroyed + original CB entry
//                        rejected. FAIL: target destroyed AND original CB
//                        forwarded to driver (stale VkFramebuffer usage).
//   pool-reset         : record CB -> end -> lazy pool reset via slot
//                        rotation + re-request -> submit original pending
//                        entry. The reset recording must NOT be forwarded
//                        by the original entry (CPU semantic assertion even
//                        if Validation is clean).
//
// ALL cases are strict contract tests: exit 1 on any failure/error.
// Current engine: control PASSES; delayed-resource and pool-reset are
// EXPECTED RED (engine defects, to be fixed by Lead).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "gfx-vulkan/VKDevice.h"
#include "gfx-vulkan/VKFramebuffer.h"
#include "gfx-vulkan/VKGPUObjects.h"
#include "gfx-vulkan/VKRenderPass.h"
#include "renderer/GFXDeviceManager.h"
#include "platform/FileUtils.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

int cocos_main(int, const char **) { return 0; }
void cocos_destroy() {}

namespace {

unsigned g_failures = 0;
void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

// ---- thin observation hooks (transparent pass-through) ----------------------
PFN_vkDestroyFramebuffer g_realDestroyFramebuffer = nullptr;
PFN_vkDestroyImage g_realDestroyImage = nullptr;
PFN_vkDestroyImageView g_realDestroyImageView = nullptr;
PFN_vkQueueSubmit g_realQueueSubmit = nullptr;
PFN_vkBeginCommandBuffer g_realBeginCmdBuf = nullptr;
PFN_vkResetCommandPool g_realResetCmdPool = nullptr;
PFN_vkCmdExecuteCommands g_realCmdExecute = nullptr;

int g_destroyFramebufferCalls = 0;
int g_destroyImageCalls = 0;
int g_destroyImageViewCalls = 0;
int g_submitCalls = 0;
int g_beginCmdBufCalls = 0;
int g_resetCmdPoolCalls = 0;
int g_executeCmdCalls = 0;
VkCommandBuffer g_lastBegunCB = VK_NULL_HANDLE;
std::vector<VkCommandBuffer> g_lastSubmitCBs;
std::vector<VkFramebuffer> g_destroyedFramebuffers;
std::vector<VkImage> g_destroyedImages;

VKAPI_ATTR void VKAPI_CALL hookDestroyFramebuffer(VkDevice d, VkFramebuffer f, const VkAllocationCallbacks *a) {
    ++g_destroyFramebufferCalls; g_destroyedFramebuffers.push_back(f);
    g_realDestroyFramebuffer(d, f, a);
}
VKAPI_ATTR void VKAPI_CALL hookDestroyImage(VkDevice d, VkImage i, const VkAllocationCallbacks *a) {
    ++g_destroyImageCalls; g_destroyedImages.push_back(i);
    g_realDestroyImage(d, i, a);
}
VKAPI_ATTR void VKAPI_CALL hookDestroyImageView(VkDevice d, VkImageView v, const VkAllocationCallbacks *a) {
    ++g_destroyImageViewCalls; g_realDestroyImageView(d, v, a);
}
VKAPI_ATTR VkResult VKAPI_CALL hookQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *pInfos, VkFence f) {
    ++g_submitCalls;
    g_lastSubmitCBs.clear();
    if (pInfos) for (uint32_t i = 0; i < n; ++i)
        for (uint32_t j = 0; j < pInfos[i].commandBufferCount; ++j)
            g_lastSubmitCBs.push_back(pInfos[i].pCommandBuffers[j]);
    return g_realQueueSubmit(q, n, pInfos, f);
}
// M5-I: fixed (no allocation) record of the last begin's inheritance values.
// Scalars are copied synchronously; no stack pointers are retained.
struct BeginRecord {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkStructureType sTypeValue = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    VkResult result = VK_NOT_READY;
    VkCommandBufferUsageFlags flags = 0;
    bool hasInheritance = false;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    uint32_t subpass = 0;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    const void *inheritancePNext = nullptr;
    VkBool32 occlusion = 0;
    VkQueryControlFlags queryFlags = 0;
    VkQueryPipelineStatisticFlags stats = 0;
};
BeginRecord g_lastBegin;
VkCommandBuffer g_lastExecutedSecondary = VK_NULL_HANDLE;
static PFN_vkBeginCommandBuffer g_beginDispatcher = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL hookBeginCmdBuf(VkCommandBuffer cb, const VkCommandBufferBeginInfo *p) {
    if (g_beginDispatcher == nullptr || p == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    ++g_beginCmdBufCalls;
    g_lastBegunCB = cb;
    g_lastBegin = BeginRecord{};
    g_lastBegin.cb = cb;
    g_lastBegin.sTypeValue = p->sType;
    {
        g_lastBegin.flags = p->flags;
        if (p->pInheritanceInfo != nullptr) {
            const auto &inh = *p->pInheritanceInfo;
            g_lastBegin.hasInheritance = true;
            g_lastBegin.renderPass = inh.renderPass;
            g_lastBegin.subpass = inh.subpass;
            g_lastBegin.framebuffer = inh.framebuffer;
            g_lastBegin.inheritancePNext = inh.pNext;
            g_lastBegin.occlusion = inh.occlusionQueryEnable;
            g_lastBegin.queryFlags = inh.queryFlags;
            g_lastBegin.stats = inh.pipelineStatistics;
        }
    }
    const VkResult r = g_beginDispatcher(cb, p);
    g_lastBegin.result = r;
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL hookResetCmdPool(VkDevice d, VkCommandPool p, VkCommandPoolResetFlags f) {
    ++g_resetCmdPoolCalls;
    return g_realResetCmdPool(d, p, f);
}
VKAPI_ATTR void VKAPI_CALL hookCmdExecute(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer *p) {
    if (g_realCmdExecute == nullptr) return;
    g_realCmdExecute(cb, n, p); // real forward FIRST (void: the call is the evidence)
    ++g_executeCmdCalls;
    if (n > 0 && p != nullptr) g_lastExecutedSecondary = p[0];
}

struct HookRAII {
    HookRAII() {
        g_realDestroyFramebuffer = vkDestroyFramebuffer; vkDestroyFramebuffer = &hookDestroyFramebuffer;
        g_realDestroyImage = vkDestroyImage; vkDestroyImage = &hookDestroyImage;
        g_realDestroyImageView = vkDestroyImageView; vkDestroyImageView = &hookDestroyImageView;
        g_realQueueSubmit = vkQueueSubmit; vkQueueSubmit = &hookQueueSubmit;
        g_realBeginCmdBuf = vkBeginCommandBuffer; g_beginDispatcher = g_realBeginCmdBuf; vkBeginCommandBuffer = &hookBeginCmdBuf;
        g_realResetCmdPool = vkResetCommandPool; vkResetCommandPool = &hookResetCmdPool;
        g_realCmdExecute = vkCmdExecuteCommands; vkCmdExecuteCommands = &hookCmdExecute;
    }
    ~HookRAII() {
        vkDestroyFramebuffer = g_realDestroyFramebuffer;
        vkDestroyImage = g_realDestroyImage;
        vkDestroyImageView = g_realDestroyImageView;
        vkQueueSubmit = g_realQueueSubmit;
        vkBeginCommandBuffer = g_realBeginCmdBuf;
        vkResetCommandPool = g_realResetCmdPool;
        vkCmdExecuteCommands = g_realCmdExecute;
    }
    static void reset() {
        g_destroyFramebufferCalls = g_destroyImageCalls = g_destroyImageViewCalls = 0;
        g_submitCalls = g_beginCmdBufCalls = g_resetCmdPoolCalls = g_executeCmdCalls = 0;
        g_lastSubmitCBs.clear(); g_destroyedFramebuffers.clear(); g_destroyedImages.clear();
        g_lastBegin = BeginRecord{};
        g_lastExecutedSecondary = VK_NULL_HANDLE;
    }
};

} // namespace

using namespace cc;
using namespace cc::gfx;

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::string caseName;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--case") == 0 && i + 1 < argc) caseName = argv[++i];
    if (caseName.empty() || (caseName != "control" && caseName != "delayed-resource" && caseName != "pool-reset" &&
                               caseName != "pending-drop" && caseName != "facade-rebegin" && caseName != "secondary-lifetime")) {
        std::fprintf(stderr, "usage: %s --case <control|delayed-resource|pool-reset|pending-drop|facade-rebegin|secondary-lifetime>\n", argv[0]);
        return 2;
    }

    const auto outDir = std::filesystem::current_path() / ("rec-life-" + caseName);
    std::filesystem::create_directories(outDir);
    std::unique_ptr<FileUtils> fileUtils(createFileUtils());
    fileUtils->setWritablePath(outDir.generic_string() + "/");

    std::fprintf(stderr, "Creating Cocos Vulkan device (offscreen, no swapchain)\n");
    std::unique_ptr<Device> ownedDevice(DeviceManager::create());
    if (!ownedDevice || ownedDevice->getGfxAPI() != API::VULKAN) {
        std::fprintf(stderr, "FAIL: device init\n");
        if (ownedDevice) ownedDevice->destroy();
        return 1;
    }
    CCVKDevice &raw = *CCVKDevice::getInstance();
    // volkLoadDevice during initialization replaces the global dispatch values.
    // Install only after initialization; tracker stores their addresses.
    HookRAII hooks;
    Device *backend = &raw;
    auto *context = raw.gpuContext();
    const bool validation = context->validationEnabled && context->bestPracticesValidationEnabled &&
                            context->synchronizationValidationEnabled;
    std::printf("validation + best practices + sync: %s\n", validation ? "ENABLED" : "NOT VERIFIED");
    if (!validation) { std::fprintf(stderr, "FAIL: validation REQUIRED\n"); ownedDevice->destroy(); return 1; }
    backend->enableAutoBarrier(false);
    std::printf("harness config: enableAutoBarrier(false); offscreen RP barrier COLOR_ATTACHMENT_WRITE\n");
    auto *queue = backend->getQueue();

    const auto errorCount = [&raw] {
        return raw.gpuContext()->validationErrorCount.load(std::memory_order_relaxed);
    };

    // --- create offscreen texture / RP / FBO (no swapchain) ---
    TextureInfo texInfo;
    texInfo.type = TextureType::TEX2D;
    texInfo.format = Format::RGBA8;
    texInfo.width = 256;
    texInfo.height = 256;
    texInfo.usage = TextureUsageBit::COLOR_ATTACHMENT;
    texInfo.flags = static_cast<TextureFlagBit>(0); // default flags (MANAGED clashes with windows.h)
    Texture *tex = backend->createTexture(texInfo);
    check(tex != nullptr, "createTexture");

    GeneralBarrierInfo colorInfo;
    colorInfo.prevAccesses = AccessFlagBit::NONE;
    colorInfo.nextAccesses = AccessFlagBit::COLOR_ATTACHMENT_WRITE;
    RenderPassInfo rpInfo;
    rpInfo.colorAttachments.resize(1);
    rpInfo.colorAttachments[0].format = Format::RGBA8;
    rpInfo.colorAttachments[0].loadOp = LoadOp::CLEAR;
    rpInfo.colorAttachments[0].storeOp = StoreOp::STORE;
    rpInfo.colorAttachments[0].barrier = backend->getGeneralBarrier(colorInfo);
    rpInfo.subpasses.resize(1);
    rpInfo.subpasses[0].colors = {0};
    RenderPass *rp = backend->createRenderPass(rpInfo);
    check(rp != nullptr, "createRenderPass");

    FramebufferInfo fboInfo;
    fboInfo.renderPass = rp;
    fboInfo.colorTextures = {tex};
    Framebuffer *fbo = backend->createFramebuffer(fboInfo);
    check(fbo != nullptr, "createFramebuffer");

    CommandBufferInfo cbInfo;
    cbInfo.queue = queue;
    CommandBuffer *cmd = backend->createCommandBuffer(cbInfo);
    const Color clearColor{0.8f, 0.6f, 0.4f, 1.0f};
    const auto recordClear = [&]() {
        cmd->begin();
        Rect area{0, 0, 256, 256};
        cmd->beginRenderPass(rp, fbo, area, &clearColor, 1.0F, 0U);
        cmd->endRenderPass();
        cmd->end();
    };

    const uint32_t errorsAtStart = errorCount();
    const uint32_t backBufferCount = raw.gpuDevice()->backBufferCount;
    std::string outcome = "UNSET";

    if (caseName == "control") {
        HookRAII::reset();
        recordClear();
        queue->submit(reinterpret_cast<CommandBuffer **>(&cmd), 1);
        backend->present(); // slot rotation, fence/timeline wait, recycle
        check(g_beginCmdBufCalls > 0 && g_submitCalls > 0, "control hooks observe real recording and submit");
        std::printf("control: submitObserved=%d\n", g_submitCalls);
        outcome = "PASS-CLEAN"; // validated by liveErrors==0 postErrors==0
    } else if (caseName == "delayed-resource") {
        HookRAII::reset();
        recordClear();       // recorded, NOT submitted
        const uint32_t submitsBefore = g_submitCalls;
        check(submitsBefore == 0, "delayed recording has not submitted");
        const VkCommandBuffer originalCB = g_lastBegunCB;
        fbo->destroy(); delete fbo; fbo = nullptr;
        rp->destroy(); delete rp; rp = nullptr;
        tex->destroy(); delete tex; tex = nullptr;
        for (uint32_t i = 0; i <= backBufferCount; ++i) backend->present();
        const bool targetDestroyed = g_destroyFramebufferCalls > 0 || g_destroyImageViewCalls > 0;
        std::printf("delayed: after rotation destroyFB=%d destroyImg=%d destroyView=%d targetDestroyed=%d\n",
                    g_destroyFramebufferCalls, g_destroyImageCalls, g_destroyImageViewCalls, targetDestroyed);
        const uint32_t before = g_submitCalls;
        queue->submit(&cmd, 1);
        const bool originalForwarded = std::find(g_lastSubmitCBs.begin(), g_lastSubmitCBs.end(), originalCB) != g_lastSubmitCBs.end();
        std::printf("delayed: submit attempt: calls %u->%d originalForwarded=%d\n", before, g_submitCalls, originalForwarded);
        check(!(targetDestroyed && originalForwarded), "destroyed offscreen recording must not reach driver");
        check(originalForwarded || targetDestroyed, "retained recording must actually submit");
        backend->present();
        raw.waitAllFences();
        outcome = targetDestroyed ? "REJECTED-RECORDING" : "PROTECTED-LEASE";
    } else if (caseName == "pool-reset") {
        HookRAII::reset();
        // CB #1: record and end, don't submit
        recordClear();
        const VkCommandBuffer originalCB = g_lastBegunCB;
        const uint32_t originalSlot = raw.gpuDevice()->curBackBufferIndex;
        // Pool reset is lazy on request after a slot change. Make a normal
        // request in another slot before returning to the original slot.
        backend->present();
        CommandBufferInfo otherInfo; otherInfo.queue = queue;
        CommandBuffer *other = backend->createCommandBuffer(otherInfo);
        other->begin(); other->end();
        const uint32_t resetsBefore = g_resetCmdPoolCalls;
        while (raw.gpuDevice()->curBackBufferIndex != originalSlot) backend->present();
        // CB #2: begin/end another CB (may get same handle from pool)
        CommandBufferInfo cb2Info; cb2Info.queue = queue;
        CommandBuffer *cmd2 = backend->createCommandBuffer(cb2Info);
        cmd2->begin();
        const VkCommandBuffer replacementCB = g_lastBegunCB;
        cmd2->end();
        // Submit only the original pending entry. Submitting both aliases in
        // one batch would introduce an additional duplicate-CB violation.
        const uint32_t submitsBefore = g_submitCalls;
        queue->submit(&cmd, 1);
        std::printf("pool-reset: submit calls %d->%d, CBs in last submit: %zu\n",
                    submitsBefore, g_submitCalls, g_lastSubmitCBs.size());
        const bool originalForwarded = std::find(g_lastSubmitCBs.begin(), g_lastSubmitCBs.end(), originalCB) != g_lastSubmitCBs.end();
        std::printf("pool-reset: slot=%u resets=%d original=%p replacement=%p forwarded=%d\n",
                    originalSlot, g_resetCmdPoolCalls - resetsBefore,
                    static_cast<void *>(originalCB), static_cast<void *>(replacementCB), originalForwarded);
        check(g_resetCmdPoolCalls > resetsBefore,
              "pool-reset: vkResetCommandPool must be observed in this scenario");
        if (g_resetCmdPoolCalls > resetsBefore) {
            outcome = "POOL-RESET-OBSERVED";
            check(!originalForwarded, "reset recording must not be forwarded by the original pending entry");
        }
        // present to complete
        backend->present();
        raw.waitAllFences();
        if (cmd2) { cmd2->destroy(); delete cmd2; }
        if (other) { other->destroy(); delete other; }
    } else if (caseName == "pending-drop") {
        HookRAII::reset();
        recordClear();       // recorded, NOT submitted
        // destroy user-visible objects while recording is pending
        fbo->destroy(); delete fbo; fbo = nullptr;
        rp->destroy(); delete rp; rp = nullptr;
        tex->destroy(); delete tex; tex = nullptr;
        // owning lease must prevent GPU destruction while pending
        check(g_destroyFramebufferCalls == 0 && g_destroyImageViewCalls == 0,
              "pending-drop: FB/view must NOT be destroyed while recording holds them");
        std::printf("pending-drop: after user destroy (pending): destroyFB=%d destroyView=%d\n",
                    g_destroyFramebufferCalls, g_destroyImageViewCalls);
        // explicitly discard the pending recording via CB destroy
        cmd->destroy(); delete cmd; cmd = nullptr;
        // rotate full slots to let recycle bins clear
        for (uint32_t i = 0; i <= backBufferCount; ++i) backend->present();
        const bool destroyedAfter = g_destroyFramebufferCalls > 0 && g_destroyImageViewCalls > 0;
        check(g_destroyFramebufferCalls > 0, "pending-drop: framebuffer must be destroyed after the recording is dropped");
        check(g_destroyImageViewCalls > 0, "pending-drop: image view must be destroyed after the recording is dropped");
        std::printf("pending-drop: after rotation: destroyFB=%d destroyView=%d destroyed=%d\n",
                    g_destroyFramebufferCalls, g_destroyImageViewCalls, destroyedAfter);
        check(g_submitCalls == 0, "pending-drop: no stale CB sent to queue");
        outcome = "DROPPED-AND-DESTROYED";
    } else if (caseName == "facade-rebegin") {
        HookRAII::reset();
        // record old FBO clear, don't submit
        recordClear();
        const VkCommandBuffer originalCB = g_lastBegunCB;
        // destroy old user objects (recording lease protects GPU objects)
        fbo->destroy(); delete fbo; fbo = nullptr;
        rp->destroy(); delete rp; rp = nullptr;
        tex->destroy(); delete tex; tex = nullptr;
        check(g_destroyFramebufferCalls == 0, "facade: FB NOT destroyed while old recording pending");
        // same CB: begin/end a NEW empty recording (no rotation/reset)
        cmd->begin();
        const VkCommandBuffer newCB = g_lastBegunCB;
        cmd->end();
        std::printf("facade: original=%p new=%p (same-handle=%d)\n",
                    (void*)originalCB, (void*)newCB, originalCB == newCB);
        check(g_destroyFramebufferCalls == 0 && g_destroyImageViewCalls == 0,
              "facade: old FB/view must survive the same facade beginning a new recording");
        // first submit must forward the ORIGINAL handle (old pending entry)
        g_lastSubmitCBs.clear();
        queue->submit(&cmd, 1);
        const bool origFwd = std::find(g_lastSubmitCBs.begin(), g_lastSubmitCBs.end(), originalCB) != g_lastSubmitCBs.end();
        std::printf("facade: first submit: CBs=%zu originalForwarded=%d\n", g_lastSubmitCBs.size(), origFwd);
        check(origFwd, "facade: first submit must consume the OLD pending entry");
        backend->present();
        // second submit must forward the NEW handle
        g_lastSubmitCBs.clear();
        queue->submit(&cmd, 1);
        const bool newFwd = std::find(g_lastSubmitCBs.begin(), g_lastSubmitCBs.end(), newCB) != g_lastSubmitCBs.end();
        std::printf("facade: second submit: CBs=%zu newForwarded=%d\n", g_lastSubmitCBs.size(), newFwd);
        check(newFwd, "facade: second submit must consume the NEW pending entry");
        backend->present();
        raw.waitAllFences();
        outcome = "FACADE-OK";
    } else if (caseName == "secondary-lifetime") {
        const unsigned secondaryFailuresBefore = g_failures;
        HookRAII::reset();
        // Empty real secondary with inherited RP/FBO. This checks inheritance
        // lifetime and ExecuteCommands, not a resource used only by the secondary.
        CommandBufferInfo secInfo;
        secInfo.type = CommandBufferType::SECONDARY;
        secInfo.queue = queue;
        CommandBuffer *secondary = backend->createCommandBuffer(secInfo);
        secondary->begin(rp, 0, fbo);
        secondary->end();
        check(g_beginCmdBufCalls > 0, "secondary: begin observed");
        // M5-I: real Classic secondary inheritance assertions from the hook
        bool beginEvidenceOk = false;
        BeginRecord beginRecord{};
        const BeginRecord secBegin = g_lastBegin; // the SECONDARY's own begin record
        const VkCommandBuffer secHandle = g_lastBegin.cb; // real secondary VkCB
        check(secHandle != VK_NULL_HANDLE, "secondary: real VkCommandBuffer handle");
        check(secBegin.sTypeValue == VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
              "secondary: begin sType recorded");
        {
            const auto *gpuRP = static_cast<CCVKRenderPass *>(rp)->gpuRenderPass();
            const auto *gpuFBO = static_cast<CCVKFramebuffer *>(fbo)->gpuFBO();
            const bool beginOk = g_lastBegin.result == VK_SUCCESS;
            const bool continueBit =
                (g_lastBegin.flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) != 0;
            const bool rpOk = g_lastBegin.renderPass == gpuRP->vkRenderPass;
            const bool subpassOk = g_lastBegin.subpass == 0;
            const bool fboOk = gpuFBO->isOffscreen
                                   ? g_lastBegin.framebuffer == gpuFBO->vkFramebuffer && gpuFBO->vkFramebuffer != VK_NULL_HANDLE
                                   : g_lastBegin.framebuffer == VK_NULL_HANDLE;
            const bool noDynamicPNext = g_lastBegin.inheritancePNext == nullptr;
            const bool queriesZero = g_lastBegin.occlusion == 0 && g_lastBegin.queryFlags == 0 &&
                                     g_lastBegin.stats == 0;
            check(beginOk, "secondary: driver begin SUCCESS");
            check(continueBit, "secondary: CONTINUE flag");
            check(rpOk, "secondary: real RP handle inherited");
            check(subpassOk, "secondary: subpass 0");
            check(fboOk, "secondary: real FBO handle inherited");
            check(noDynamicPNext, "secondary: no dynamic inheritance pNext");
            check(queriesZero, "secondary: query fields zero");
            beginEvidenceOk = beginOk && continueBit && rpOk && subpassOk && fboOk &&
                             noDynamicPNext && queriesZero;
            beginRecord = g_lastBegin; // primary begin later overwrites g_lastBegin
        }
        // primary: begin -> beginRenderPass(with secondary) -> execute -> endRP -> end
        cmd->begin();
        Rect area{0, 0, 256, 256};
        cmd->beginRenderPass(rp, fbo, area, &clearColor, 1.0F, 0U,
                             reinterpret_cast<CommandBuffer **>(&secondary), 1);
        cmd->execute(reinterpret_cast<CommandBuffer **>(&secondary), 1);
        check(g_executeCmdCalls > 0, "secondary: vkCmdExecuteCommands observed");
        {
            check(g_lastExecutedSecondary == secHandle,
                  "secondary: real handle forwarded to vkCmdExecuteCommands");
            // unique success marker: only from real hook assertions
        }
        cmd->endRenderPass();
        cmd->end();
        const VkCommandBuffer priHandle = g_lastBegunCB;
        // destroy secondary user CB + user objects (primary recording protects)
        secondary->destroy(); delete secondary; secondary = nullptr;
        fbo->destroy(); delete fbo; fbo = nullptr;
        rp->destroy(); delete rp; rp = nullptr;
        tex->destroy(); delete tex; tex = nullptr;
        check(g_destroyFramebufferCalls == 0, "secondary: FB NOT destroyed while primary holds reference");
        std::printf("secondary: FB NOT destroyed (destroyFB=%d destroyView=%d)\n",
                    g_destroyFramebufferCalls, g_destroyImageViewCalls);
        // rotate then submit primary
        for (uint32_t i = 0; i < backBufferCount; ++i) backend->present();
        g_lastSubmitCBs.clear();
        queue->submit(&cmd, 1);
        const bool priFwd = std::find(g_lastSubmitCBs.begin(), g_lastSubmitCBs.end(), priHandle) != g_lastSubmitCBs.end();
        std::printf("secondary: primary=%p forwarded=%d execute=%d\n",
                    (void*)priHandle, priFwd, g_executeCmdCalls);
        check(priFwd, "secondary: primary CB forwarded");
        backend->present();
        raw.waitAllFences();
        // M5-I rev2: marker only AFTER real primary submit + completion, with
        // real non-NULL handles and a real driver begin SUCCESS.
        if (priFwd && g_failures == secondaryFailuresBefore && errorCount() == errorsAtStart &&
            beginEvidenceOk && g_lastExecutedSecondary == secHandle && secHandle != VK_NULL_HANDLE &&
            g_lastExecutedSecondary != VK_NULL_HANDLE && beginRecord.renderPass != VK_NULL_HANDLE &&
            beginRecord.result == VK_SUCCESS) {
            std::printf("CLASSIC_SECONDARY_INHERITANCE: rp=0x%p fbo=0x%p subpass=0 continue=1 "
                        "dynamicPNext=0 executed=0x%p result=%d\n",
                        (void *)beginRecord.renderPass, (void *)beginRecord.framebuffer,
                        (void *)g_lastExecutedSecondary, (int)beginRecord.result);
        }
        outcome = "SECONDARY-OK";
    }

    // ---- teardown ----
    raw.waitAllFences();
    const uint32_t liveErrors = errorCount() - errorsAtStart;
    if (fbo) { fbo->destroy(); delete fbo; }
    if (rp) { rp->destroy(); delete rp; }
    if (tex) { tex->destroy(); delete tex; }
    if (cmd) { cmd->destroy(); delete cmd; }
    ownedDevice->destroy();
    const uint32_t postErrors = raw.getLastValidationErrorCount();

    std::printf("%s completed: failures=%u liveErrors=%u postErrors=%u outcome=%s\n",
                caseName.c_str(), g_failures, liveErrors, postErrors, outcome.c_str());
    // ALL cases: strict contract (failures or errors -> exit 1)
    return (g_failures || liveErrors || postErrors) ? 1 : 0;
}
