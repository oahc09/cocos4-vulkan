// Graphics pipeline rendering entry adapter CPU tests (M5-H, rev 2).
// Full-field root fixtures (no memcmp: padding is not semantic); success
// expects ONLY pNext/renderPass/subpass to change. Run contract #16.

#if defined(VKPIPELINERENDER_STANDALONE_PROBE)
    #include "VKPipelineRenderingAdapter.h"
int main() { return 0; }
#else

    #include <cstdio>
    #include <functional>
    #include <type_traits>
    #include "VKPipelineRenderingAdapter.h"

using cc::gfx::applyVKPipelineRenderingContract;
using cc::gfx::VKPipelinePNextChain;
using cc::gfx::VKPipelineRenderingMode;
using cc::gfx::VKRenderingSignature;

namespace {
unsigned g_failures = 0;
void check(bool ok, const char *w) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", w);
        ++g_failures;
    }
}

const VkRenderPass RP = reinterpret_cast<VkRenderPass>(static_cast<uintptr_t>(0x7100));
const VkPipelineLayout LAYOUT = reinterpret_cast<VkPipelineLayout>(static_cast<uintptr_t>(0x7200));
const VkPipeline BASE = reinterpret_cast<VkPipeline>(static_cast<uintptr_t>(0x7400));
const VkRenderPass OLD_RP = reinterpret_cast<VkRenderPass>(static_cast<uintptr_t>(0x7500));

VKRenderingSignature makeSig() {
    VKRenderingSignature s;
    s.colorFormats = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT};
    s.depthFormat = VK_FORMAT_D24_UNORM_S8_UINT;
    s.stencilFormat = VK_FORMAT_D24_UNORM_S8_UINT;
    s.samples = VK_SAMPLE_COUNT_2_BIT;
    s.viewMask = 0x3;
    return s;
}
VkPipelineFragmentShadingRateStateCreateInfoKHR makeFsr() {
    VkPipelineFragmentShadingRateStateCreateInfoKHR f = {};
    f.sType = VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR;
    f.fragmentSize = {2, 4};
    f.combinerOps[0] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR;
    f.combinerOps[1] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MUL_KHR;
    return f;
}

// full root fixture: every state pointer + non-trivial scalars live
struct RootState {
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineTessellationStateCreateInfo ts;
    VkPipelineViewportStateCreateInfo vs;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendStateCreateInfo cb;
    VkPipelineDynamicStateCreateInfo dy;
    VkPipelineShaderStageCreateInfo stages[2];
    VkGraphicsPipelineCreateInfo make() {
        ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_2_BIT;
        vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ts = {VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
        vs = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        VkGraphicsPipelineCreateInfo r = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        r.flags = 0x11;   // non-zero
        r.stageCount = 2; // non-zero
        r.pStages = stages;
        r.pVertexInputState = &vi;
        r.pInputAssemblyState = &ia;
        r.pTessellationState = &ts;
        r.pViewportState = &vs;
        r.pRasterizationState = &rs;
        r.pMultisampleState = &ms;
        r.pDepthStencilState = &ds;
        r.pColorBlendState = &cb;
        r.pDynamicState = &dy;
        r.layout = LAYOUT;
        r.renderPass = OLD_RP; // pre-existing: overwritten on success only
        r.subpass = 7;         // asymmetric sentinel
        r.basePipelineHandle = BASE;
        r.basePipelineIndex = 9;
        return r;
    }
};

// field-by-field comparison of every root field (no memcmp: padding)
void verifyRoot(const VkGraphicsPipelineCreateInfo &a, const VkGraphicsPipelineCreateInfo &b,
                const char *w) {
    check(a.sType == b.sType, w);
    check(a.flags == b.flags, w);
    check(a.stageCount == b.stageCount, w);
    check(a.pStages == b.pStages, w);
    check(a.pVertexInputState == b.pVertexInputState, w);
    check(a.pInputAssemblyState == b.pInputAssemblyState, w);
    check(a.pTessellationState == b.pTessellationState, w);
    check(a.pViewportState == b.pViewportState, w);
    check(a.pRasterizationState == b.pRasterizationState, w);
    check(a.pMultisampleState == b.pMultisampleState, w);
    check(a.pDepthStencilState == b.pDepthStencilState, w);
    check(a.pColorBlendState == b.pColorBlendState, w);
    check(a.pDynamicState == b.pDynamicState, w);
    check(a.layout == b.layout, w);
    check(a.renderPass == b.renderPass, w);
    check(a.subpass == b.subpass, w);
    check(a.basePipelineHandle == b.basePipelineHandle, w);
    check(a.basePipelineIndex == b.basePipelineIndex, w);
    check(a.pNext == b.pNext, w);
}

void verifyChainCleared(const VKPipelinePNextChain &c) {
    check(!c.ready(), "vx: not ready");
    check(!c.hasFsr() && !c.hasRendering(), "vx: flags");
    check(c.head() == nullptr, "vx: head null");
    const auto &f = c.fsrInfo();
    check(f.sType == VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR, "vx: fsr sType");
    check(f.pNext == nullptr, "vx: fsr pNext null");
    check(f.fragmentSize.width == 0 && f.fragmentSize.height == 0, "vx: fsr extent 0");
    check(f.combinerOps[0] == 0 && f.combinerOps[1] == 0, "vx: fsr ops 0");
    const auto &ct = c.contract();
    check(!ct.ready(), "vx: contract not ready");
    check(ct.pipelineInfo().colorAttachmentCount == 0, "vx: contract count 0");
    check(ct.pipelineInfo().pColorAttachmentFormats == nullptr, "vx: contract ptr null");
    check(ct.pipelineInfo().depthAttachmentFormat == VK_FORMAT_UNDEFINED, "vx: contract depth");
    check(ct.pipelineInfo().stencilAttachmentFormat == VK_FORMAT_UNDEFINED, "vx: contract stencil");
    check(ct.pipelineInfo().pNext == nullptr, "vx: contract pipeline pNext");
    check(ct.secondaryRenderingInfo().sType ==
              VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
          "vx: contract sec sType");
    check(ct.secondaryRenderingInfo().colorAttachmentCount == 0, "vx: contract sec count");
    check(ct.secondaryRenderingInfo().pNext == nullptr, "vx: contract sec pNext");
    check(ct.baseInheritanceInfo().sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
          "vx: contract base sType");
    check(ct.baseInheritanceInfo().pNext == nullptr, "vx: contract base pNext");
}

// full rendering payload via owned references; array guarded by vector size
void verifyRenderingPayload(const VKPipelinePNextChain &c) {
    const auto &ct = c.contract();
    check(ct.ready(), "vr: ready");
    const auto &pi = ct.pipelineInfo();
    check(pi.sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, "vr: sType");
    check(pi.pNext == nullptr, "vr: pNext null");
    check(pi.viewMask == 0x3, "vr: viewMask");
    check(pi.depthAttachmentFormat == VK_FORMAT_D24_UNORM_S8_UINT, "vr: depth");
    check(pi.stencilAttachmentFormat == VK_FORMAT_D24_UNORM_S8_UINT, "vr: stencil");
    const bool arrOk = pi.colorAttachmentCount == 2 &&
                       ct.signature().colorFormats.size() == 2 &&
                       pi.pColorAttachmentFormats != nullptr &&
                       pi.pColorAttachmentFormats == ct.signature().colorFormats.data();
    check(arrOk, "vr: color guard");
    if (arrOk) {
        check(pi.pColorAttachmentFormats[0] == VK_FORMAT_B8G8R8A8_UNORM, "vr: c0");
        check(pi.pColorAttachmentFormats[1] == VK_FORMAT_R16G16B16A16_SFLOAT, "vr: c1");
    }
    const auto &sec = ct.secondaryRenderingInfo();
    check(sec.sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO, "vr: sec sType");
    check(sec.viewMask == 0x3, "vr: sec viewMask");
    check(sec.rasterizationSamples == VK_SAMPLE_COUNT_2_BIT, "vr: sec samples");
    check(sec.colorAttachmentCount == 2, "vr: sec count");
    check(sec.pColorAttachmentFormats == pi.pColorAttachmentFormats, "vr: sec shares array");
    check(sec.depthAttachmentFormat == VK_FORMAT_D24_UNORM_S8_UINT, "vr: sec depth");
    check(sec.stencilAttachmentFormat == VK_FORMAT_D24_UNORM_S8_UINT, "vr: sec stencil");
    check(sec.pNext == nullptr, "vr: sec pNext null");
    check(ct.baseInheritanceInfo().pNext == &sec, "vr: base chains sec");
}
void verifyFsrPayload(const VKPipelinePNextChain &c) {
    const auto &f = c.fsrInfo();
    check(f.sType == VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR, "vf: sType");
    check(f.pNext == nullptr || f.pNext == &c.contract().pipelineInfo(), "vf: pNext");
    check(f.fragmentSize.width == 2 && f.fragmentSize.height == 4, "vf: extent");
    check(f.combinerOps[0] == VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR, "vf: op0");
    check(f.combinerOps[1] == VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MUL_KHR, "vf: op1");
}
} // namespace

int main() {
    static_assert(!std::is_copy_constructible<VKPipelinePNextChain>::value &&
                      !std::is_move_constructible<VKPipelinePNextChain>::value,
                  "chain still non-copy/move");

    // --- 1..4: four modes; success changes ONLY pNext/renderPass/subpass ---
    {
        RootState st;
        VKRenderingSignature sig = makeSig();
        auto fsr = makeFsr();
        struct ModeCase {
            const char *name;
            VKPipelineRenderingMode mode;
            const VKRenderingSignature *sig;
            const VkPipelineFragmentShadingRateStateCreateInfoKHR *fsr;
        };
        const ModeCase modes[] = {
            {"classic-none", VKPipelineRenderingMode::Classic, nullptr, nullptr},
            {"classic-fsr", VKPipelineRenderingMode::Classic, nullptr, &fsr},
            {"dynamic", VKPipelineRenderingMode::Dynamic, &sig, nullptr},
            {"dynamic-fsr", VKPipelineRenderingMode::Dynamic, &sig, &fsr},
        };
        for (const auto &mc : modes) {
            VKPipelinePNextChain chain;
            auto root = st.make();
            const auto before = root;
            check(applyVKPipelineRenderingContract(root, chain, mc.mode, RP, 3, mc.sig, mc.fsr),
                  mc.name);
            // build the expected root: ONLY the 3 fields differ
            auto expected = before;
            expected.pNext = chain.head();
            if (mc.mode == VKPipelineRenderingMode::Classic) {
                expected.renderPass = RP;
                expected.subpass = 3;
            } else {
                expected.renderPass = VK_NULL_HANDLE;
                expected.subpass = 0;
            }
            verifyRoot(root, expected, mc.name);
            // chain state per mode
            if (mc.mode == VKPipelineRenderingMode::Classic) {
                check(!chain.hasRendering(), mc.name);
                if (mc.fsr)
                    check(root.pNext == &chain.fsrInfo(), mc.name);
                else
                    check(root.pNext == nullptr, mc.name);
            } else {
                check(chain.hasRendering(), mc.name);
                verifyRenderingPayload(chain);
                if (mc.fsr) {
                    check(root.pNext == &chain.fsrInfo(), mc.name);
                    check(chain.fsrInfo().pNext == &chain.contract().pipelineInfo(), mc.name);
                } else {
                    check(root.pNext == &chain.contract().pipelineInfo(), mc.name);
                }
            }
            if (mc.fsr) verifyFsrPayload(chain);
        }
    }

    // --- 5: Classic with an invalid signature still succeeds (unused input) ---
    {
        RootState st;
        VKPipelinePNextChain chain;
        auto root = st.make();
        VKRenderingSignature bad = makeSig();
        bad.samples = static_cast<VkSampleCountFlagBits>(0); // invalid
        check(applyVKPipelineRenderingContract(root, chain, VKPipelineRenderingMode::Classic, RP, 2,
                                               &bad, nullptr),
              "classic invalid sig ok");
        check(root.renderPass == RP && root.subpass == 2 && root.pNext == nullptr,
              "classic invalid sig fields");
        VKRenderingSignature bad2 = makeSig();
        bad2.depthFormat = VK_FORMAT_D32_SFLOAT; // D/S mismatch
        bad2.stencilFormat = VK_FORMAT_S8_UINT;
        check(applyVKPipelineRenderingContract(root, chain, VKPipelineRenderingMode::Classic, RP, 2,
                                               &bad2, nullptr),
              "classic D-S-mismatch sig ok");
    }

    // --- 6: rejections: prime -> bad (all root fields eq, chain cleared) -> recover ---
    {
        RootState st;
        VKPipelinePNextChain chain;
        VKRenderingSignature sig = makeSig();
        auto fsr = makeFsr();
        unsigned rejections = 0;
        auto primeRecover = [&](const char *label) {
            auto r = st.make();
            check(applyVKPipelineRenderingContract(r, chain, VKPipelineRenderingMode::Dynamic, RP, 0,
                                                   &sig, &fsr),
                  label);
            verifyRenderingPayload(chain);
            verifyFsrPayload(chain);
        };
        auto cycle = [&](const char *label, VkGraphicsPipelineCreateInfo root,
                         VKPipelineRenderingMode mode, VkRenderPass rp,
                         const VKRenderingSignature *sg,
                         const VkPipelineFragmentShadingRateStateCreateInfoKHR *f,
                         const std::function<void()> &corrupt = nullptr) {
            ++rejections;
            primeRecover(label);
            if (corrupt) corrupt();
            const auto snap = root;
            check(!applyVKPipelineRenderingContract(root, chain, mode, rp, 0, sg, f), label);
            verifyRoot(root, snap, label); // every field unchanged
            verifyChainCleared(chain);
            primeRecover(label);
        };
        {
            auto r = st.make();
            r.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            cycle("root sType", r, VKPipelineRenderingMode::Classic, RP, nullptr, nullptr);
        }
        {
            static int dummy = 0;
            auto r = st.make();
            r.pNext = &dummy;
            cycle("root pNext nonnull", r, VKPipelineRenderingMode::Classic, RP, nullptr, nullptr);
        }
        {
            auto r = st.make();
            cycle("mode invalid", r, static_cast<VKPipelineRenderingMode>(99), RP, nullptr, nullptr);
        }
        {
            auto r = st.make();
            cycle("classic rp null", r, VKPipelineRenderingMode::Classic, VK_NULL_HANDLE, nullptr, nullptr);
        }
        {
            auto r = st.make();
            cycle("dyn sig null", r, VKPipelineRenderingMode::Dynamic, RP, nullptr, nullptr);
        }
        // Dynamic invalid signature reaching the COMPOSER (not the mismatch
        // guard): bad.samples 0 while ms is ALSO 0 so the guard passes
        {
            auto r = st.make();
            VKRenderingSignature bad = makeSig();
            bad.samples = static_cast<VkSampleCountFlagBits>(0);
            cycle("dyn sig samples0 composer", r, VKPipelineRenderingMode::Dynamic, RP, &bad, nullptr,
                  [&st]() { st.ms.rasterizationSamples = static_cast<VkSampleCountFlagBits>(0); });
        }
        // D/S mismatch signature reaches the composer the same way
        {
            auto r = st.make();
            VKRenderingSignature bad = makeSig();
            bad.depthFormat = VK_FORMAT_D32_SFLOAT;
            bad.stencilFormat = VK_FORMAT_S8_UINT;
            cycle("dyn sig DS mismatch composer", r, VKPipelineRenderingMode::Dynamic, RP, &bad,
                  nullptr);
        }
        {
            auto r = st.make();
            r.pMultisampleState = nullptr;
            cycle("dyn ms null", r, VKPipelineRenderingMode::Dynamic, RP, &sig, nullptr);
        }
        {
            auto r = st.make();
            cycle("dyn ms sType", r, VKPipelineRenderingMode::Dynamic, RP, &sig, nullptr,
                  [&st]() { st.ms.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; });
        }
        {
            auto r = st.make();
            cycle("dyn samples mismatch", r, VKPipelineRenderingMode::Dynamic, RP, &sig, nullptr,
                  [&st]() { st.ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT; });
        }
        {
            auto r = st.make();
            auto badFsr = makeFsr();
            badFsr.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            cycle("classic bad fsr", r, VKPipelineRenderingMode::Classic, RP, nullptr, &badFsr);
        }
        {
            auto r = st.make();
            auto badFsr = makeFsr();
            badFsr.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            cycle("dyn bad fsr", r, VKPipelineRenderingMode::Dynamic, RP, &sig, &badFsr);
        }
        check(rejections == 12, "rejection count");
        std::printf("rejection cases exercised: %u\n", rejections);
    }

    // --- 7: combined own FSR (non-NULL pNext) rejected by the COMPOSER ---
    {
        RootState st;
        VKPipelinePNextChain chain;
        VKRenderingSignature sig = makeSig();
        auto fsr = makeFsr();
        auto prime = st.make();
        check(applyVKPipelineRenderingContract(prime, chain, VKPipelineRenderingMode::Dynamic, RP, 0,
                                               &sig, &fsr),
              "own fsr prime");
        // the chain's own FSR now carries a non-NULL tail (to rendering)
        check(chain.fsrInfo().pNext == &chain.contract().pipelineInfo(),
              "own fsr pNext really nonnull");
        auto fresh = st.make(); // fresh root: the root.pNext guard is NOT the rejecter
        const auto snap = fresh;
        check(!applyVKPipelineRenderingContract(fresh, chain, VKPipelineRenderingMode::Dynamic, RP,
                                                0, &sig, &chain.fsrInfo()),
              "own fsr rejected by composer");
        verifyRoot(fresh, snap, "own fsr root unchanged");
        verifyChainCleared(chain);
        // recover
        auto recover = st.make();
        check(applyVKPipelineRenderingContract(recover, chain, VKPipelineRenderingMode::Dynamic, RP,
                                               0, &sig, &fsr),
              "own fsr recover");
        verifyRenderingPayload(chain);
        verifyFsrPayload(chain);
    }

    // --- 8: mode switching, full payload each step, fresh roots ---
    {
        RootState st;
        VKPipelinePNextChain chain;
        VKRenderingSignature sig = makeSig();
        auto fsr = makeFsr();
        {
            auto r = st.make();
            check(applyVKPipelineRenderingContract(r, chain, VKPipelineRenderingMode::Dynamic, RP, 0,
                                                   &sig, &fsr),
                  "sw dyn");
            check(r.pNext == &chain.fsrInfo() && r.renderPass == VK_NULL_HANDLE && r.subpass == 0,
                  "sw dyn fields");
            verifyRenderingPayload(chain);
            verifyFsrPayload(chain);
        }
        {
            auto r = st.make();
            check(applyVKPipelineRenderingContract(r, chain, VKPipelineRenderingMode::Classic, RP, 4,
                                                   nullptr, nullptr),
                  "sw classic");
            check(r.pNext == nullptr && r.renderPass == RP && r.subpass == 4, "sw classic fields");
            check(!chain.hasFsr() && !chain.hasRendering(), "sw: no stale nodes");
            check(chain.ready() && chain.head() == nullptr,
                  "sw: classic-none is a valid empty build, not cleared");
        }
        {
            auto r = st.make();
            check(applyVKPipelineRenderingContract(r, chain, VKPipelineRenderingMode::Classic, RP, 0,
                                                   nullptr, &fsr),
                  "sw classic fsr");
            check(r.pNext == &chain.fsrInfo(), "sw: classic fsr head");
            check(chain.fsrInfo().pNext == nullptr, "sw: classic fsr tail null");
            check(chain.hasFsr() && !chain.hasRendering(), "sw: classic fsr flags");
            verifyFsrPayload(chain);
        }
    }

    // --- 9: owning: guard, mutation, destruction, full payload after ---
    {
        RootState st;
        VKPipelinePNextChain chain;
        auto root = st.make();
        bool guard = false;
        {
            VKRenderingSignature sig = makeSig();
            auto fsr = makeFsr();
            check(applyVKPipelineRenderingContract(root, chain, VKPipelineRenderingMode::Dynamic,
                                                   RP, 0, &sig, &fsr),
                  "owning apply");
            const auto &pi = chain.contract().pipelineInfo();
            guard = root.pNext == &chain.fsrInfo() &&
                    pi.pColorAttachmentFormats ==
                        chain.contract().signature().colorFormats.data() &&
                    pi.pColorAttachmentFormats != sig.colorFormats.data();
            check(guard, "owning: non-borrow proven");
            if (guard) {
                sig.colorFormats[0] = VK_FORMAT_R32_UINT;
                sig.samples = VK_SAMPLE_COUNT_4_BIT;
                fsr.fragmentSize = {8, 8};
            }
        } // inputs destroyed
        if (guard) {
            verifyRenderingPayload(chain);
            verifyFsrPayload(chain);
        } else {
            check(false, "owning: guard failed");
        }
    }

    // --- 10: self-input: own signature; own FSR in FSR-only mode ---
    {
        RootState st;
        VKPipelinePNextChain chain;
        VKRenderingSignature sig = makeSig();
        auto fsr = makeFsr();
        {
            auto r = st.make();
            check(applyVKPipelineRenderingContract(r, chain, VKPipelineRenderingMode::Dynamic, RP, 0,
                                                   &sig, &fsr),
                  "self prime");
            auto r2 = st.make();
            check(applyVKPipelineRenderingContract(r2, chain, VKPipelineRenderingMode::Dynamic, RP, 0,
                                                   &chain.contract().signature(), nullptr),
                  "self sig rebuild");
            check(r2.pNext == &chain.contract().pipelineInfo(), "self: head is rendering");
            verifyRenderingPayload(chain);
        }
        { // FSR-only self rebuild: chain.fsrInfo().pNext is NULL there
            VKPipelinePNextChain c2;
            auto r = st.make();
            check(applyVKPipelineRenderingContract(r, c2, VKPipelineRenderingMode::Classic, RP, 0,
                                                   nullptr, &fsr),
                  "self-fsr prime");
            check(c2.fsrInfo().pNext == nullptr, "self-fsr: own tail really null");
            auto r2 = st.make();
            check(applyVKPipelineRenderingContract(r2, c2, VKPipelineRenderingMode::Classic, RP, 0,
                                                   nullptr, &c2.fsrInfo()),
                  "self-fsr rebuild");
            check(r2.pNext == &c2.fsrInfo(), "self-fsr: head is fsr");
            verifyFsrPayload(c2);
        }
    }

    if (g_failures == 0) {
        std::printf("pipeline rendering adapter tests: all PASS\n");
        return 0;
    }
    std::printf("pipeline rendering adapter tests: %u FAIL\n", g_failures);
    return 1;
}
#endif
