// Typed pipeline pNext chain composer CPU contract tests (M5-D, rev 2).
// All node reads go through owned references; head/tail addresses are
// verified against owned node addresses before any payload dereference.

#include <cstdio>
#include <type_traits>
#include <vector>
#include "VKPipelinePNextChain.h"

using cc::gfx::VKPipelinePNextChain;
using cc::gfx::VKRenderingSignature;

namespace {
unsigned g_failures = 0;
void check(bool ok, const char *w) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", w);
        ++g_failures;
    }
}

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
    f.fragmentSize = {2, 4}; // asymmetric
    f.combinerOps[0] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR;
    f.combinerOps[1] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MUL_KHR;
    return f;
}

// Full contract payload check through owned reference. Array access is
// guarded: count, pointer non-null, and pointer == owned signature data
// must all hold before any [i] read.
void verifyContractPayload(const cc::gfx::VKRenderingContract &ct) {
    check(ct.ready(), "vcp: ready");
    const auto &pi = ct.pipelineInfo();
    check(pi.sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, "vcp: pipeline sType");
    check(pi.viewMask == 0x3, "vcp: viewMask");
    check(pi.depthAttachmentFormat == VK_FORMAT_D24_UNORM_S8_UINT, "vcp: depth");
    check(pi.stencilAttachmentFormat == VK_FORMAT_D24_UNORM_S8_UINT, "vcp: stencil");
    const bool arrOk = pi.colorAttachmentCount == 2 &&
                       ct.signature().colorFormats.size() == 2 &&
                       pi.pColorAttachmentFormats != nullptr &&
                       pi.pColorAttachmentFormats == ct.signature().colorFormats.data();
    check(arrOk, "vcp: color array guard");
    if (arrOk) {
        check(pi.pColorAttachmentFormats[0] == VK_FORMAT_B8G8R8A8_UNORM, "vcp: c0");
        check(pi.pColorAttachmentFormats[1] == VK_FORMAT_R16G16B16A16_SFLOAT, "vcp: c1");
    }
    check(pi.pNext == nullptr, "vcp: pipeline tail null");
    // secondary shares the same owned array
    const auto &sec = ct.secondaryRenderingInfo();
    check(sec.sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO, "vcp: sec sType");
    check(sec.viewMask == 0x3, "vcp: sec viewMask");
    check(sec.rasterizationSamples == VK_SAMPLE_COUNT_2_BIT, "vcp: sec samples");
    check(sec.colorAttachmentCount == pi.colorAttachmentCount, "vcp: sec count");
    check(sec.pColorAttachmentFormats == pi.pColorAttachmentFormats, "vcp: sec shares array");
    check(sec.depthAttachmentFormat == VK_FORMAT_D24_UNORM_S8_UINT, "vcp: sec depth");
    check(sec.stencilAttachmentFormat == VK_FORMAT_D24_UNORM_S8_UINT, "vcp: sec stencil");
    check(sec.flags == 0, "vcp: sec flags");
    check(sec.pNext == nullptr, "vcp: sec tail null");
    const auto &base = ct.baseInheritanceInfo();
    check(base.sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, "vcp: base sType");
    check(base.pNext == &sec, "vcp: base chains sec");
    check(base.renderPass == VK_NULL_HANDLE && base.framebuffer == VK_NULL_HANDLE &&
              base.subpass == 0 && base.occlusionQueryEnable == VK_FALSE &&
              base.queryFlags == 0 && base.pipelineStatistics == 0,
          "vcp: base fields");
    check(ct.signature() == makeSig() && ct.samples() == VK_SAMPLE_COUNT_2_BIT,
          "vcp: full signature snapshot");
}

// Inactive contract: published fields zeroed, sTypes preserved.
void verifyContractCleared(const cc::gfx::VKRenderingContract &ct) {
    check(!ct.ready(), "vcx: not ready");
    const auto &pi = ct.pipelineInfo();
    check(pi.sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, "vcx: pipeline sType");
    check(pi.colorAttachmentCount == 0, "vcx: pipeline count");
    check(pi.pColorAttachmentFormats == nullptr, "vcx: pipeline ptr");
    check(pi.viewMask == 0, "vcx: pipeline viewMask");
    check(pi.depthAttachmentFormat == VK_FORMAT_UNDEFINED, "vcx: pipeline depth");
    check(pi.stencilAttachmentFormat == VK_FORMAT_UNDEFINED && pi.pNext == nullptr,
          "vcx: pipeline stencil/tail");
    const auto &sec = ct.secondaryRenderingInfo();
    check(sec.sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO, "vcx: sec sType");
    check(sec.colorAttachmentCount == 0, "vcx: sec count");
    check(sec.pNext == nullptr, "vcx: sec pNext");
    check(sec.flags == 0 && sec.viewMask == 0 && sec.pColorAttachmentFormats == nullptr &&
              sec.depthAttachmentFormat == VK_FORMAT_UNDEFINED &&
              sec.stencilAttachmentFormat == VK_FORMAT_UNDEFINED && sec.rasterizationSamples == 0,
          "vcx: secondary fields");
    const auto &base = ct.baseInheritanceInfo();
    check(base.sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, "vcx: base sType");
    check(base.pNext == nullptr, "vcx: base pNext null");
    check(base.renderPass == VK_NULL_HANDLE && base.framebuffer == VK_NULL_HANDLE &&
              base.subpass == 0 && base.occlusionQueryEnable == VK_FALSE &&
              base.queryFlags == 0 && base.pipelineStatistics == 0,
          "vcx: base fields");
    check(ct.signature() == VKRenderingSignature{} && ct.samples() == VK_SAMPLE_COUNT_1_BIT,
          "vcx: default signature snapshot");
}

// Chain verification. Reads ONLY through owned references; head/tail are
// verified as addresses against owned node addresses. On address mismatch
// only a FAIL is recorded - no dereference of unverified pointers.
void verifyChain(const VKPipelinePNextChain &c, bool wantFsr, bool wantRendering) {
    check(c.ready(), "vc: ready"); // true after any successful build, incl. none
    check(c.hasFsr() == wantFsr, "vc: hasFsr");
    check(c.hasRendering() == wantRendering, "vc: hasRendering");

    if (!wantFsr && !wantRendering) {
        check(c.head() == nullptr, "vc: none head null");
        // inactive FSR node must be zeroed with correct sType
        const auto &f = c.fsrInfo();
        check(f.sType == VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR,
              "vc: none FSR sType");
        check(f.pNext == nullptr, "vc: none FSR pNext null");
        check(f.fragmentSize.width == 0 && f.fragmentSize.height == 0, "vc: none FSR extent 0");
        check(f.combinerOps[0] == 0 && f.combinerOps[1] == 0, "vc: none FSR ops 0");
        verifyContractCleared(c.contract());
        return;
    }

    // verify head address matches expected owned node (address-only check)
    if (wantFsr) {
        check(c.head() == &c.fsrInfo(), "vc: head is owned FSR address");
    } else {
        check(c.head() == &c.contract().pipelineInfo(), "vc: head is owned rendering address");
    }

    const auto &f = c.fsrInfo();
    if (wantFsr) {
        check(f.sType == VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR,
              "vc: FSR sType");
        check(f.fragmentSize.width == 2 && f.fragmentSize.height == 4, "vc: FSR extent 2x4");
        check(f.combinerOps[0] == VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR, "vc: FSR op0 MIN");
        check(f.combinerOps[1] == VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MUL_KHR, "vc: FSR op1 MUL");
        if (wantRendering) {
            check(f.pNext == &c.contract().pipelineInfo(), "vc: FSR tail is owned rendering");
            verifyContractPayload(c.contract());
        } else {
            check(f.pNext == nullptr, "vc: FSR-only tail null");
            verifyContractCleared(c.contract());
        }
    } else {
        // rendering-only: FSR node must be zeroed/inactive
        check(f.sType == VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR,
              "vc: r-only FSR sType");
        check(f.pNext == nullptr, "vc: r-only FSR pNext null");
        check(f.fragmentSize.width == 0 && f.fragmentSize.height == 0, "vc: r-only FSR extent 0");
        check(f.combinerOps[0] == 0 && f.combinerOps[1] == 0, "vc: r-only FSR ops 0");
        verifyContractPayload(c.contract());
    }
}

// Fully cleared: ready=false, all nodes zeroed, sTypes preserved.
void verifyCleared(const VKPipelinePNextChain &c) {
    check(!c.ready(), "vx: not ready");
    check(!c.hasFsr() && !c.hasRendering(), "vx: flags cleared");
    check(c.head() == nullptr, "vx: head null");
    const auto &f = c.fsrInfo();
    check(f.sType == VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR, "vx: FSR sType");
    check(f.pNext == nullptr, "vx: FSR pNext null");
    check(f.fragmentSize.width == 0 && f.fragmentSize.height == 0, "vx: FSR extent 0");
    check(f.combinerOps[0] == 0 && f.combinerOps[1] == 0, "vx: FSR ops 0");
    verifyContractCleared(c.contract());
}
} // namespace

int main() {
    static_assert(!std::is_copy_constructible<VKPipelinePNextChain>::value &&
                      !std::is_copy_assignable<VKPipelinePNextChain>::value &&
                      !std::is_move_constructible<VKPipelinePNextChain>::value &&
                      !std::is_move_assignable<VKPipelinePNextChain>::value,
                  "non-copy/move");

    // --- 1: initial cleared ---
    {
        VKPipelinePNextChain c;
        verifyCleared(c);
    }

    // --- 2: none combo: ready=true, head null, inactive nodes zeroed ---
    {
        VKPipelinePNextChain c;
        check(c.build(), "none");
        verifyChain(c, false, false);
    }

    // --- 3: rendering only ---
    {
        VKPipelinePNextChain c;
        auto s = makeSig();
        check(c.build(&s), "r-only");
        verifyChain(c, false, true);
    }

    // --- 4: FSR only ---
    {
        VKPipelinePNextChain c;
        auto f = makeFsr();
        check(c.build(nullptr, &f), "f-only");
        verifyChain(c, true, false);
    }

    // --- 5: both ---
    {
        VKPipelinePNextChain c;
        auto s = makeSig();
        auto f = makeFsr();
        check(c.build(&s, &f), "both");
        verifyChain(c, true, true);
    }

    // --- 6: owning: guards recorded while inputs alive; mutation only if guard holds ---
    {
        VKPipelinePNextChain c;
        VKRenderingSignature input = makeSig();
        VkPipelineFragmentShadingRateStateCreateInfoKHR fsr = makeFsr();
        check(c.build(&input, &fsr), "owning build");
        // record guards while caller memory is valid
        const bool fsrOwnAddr = c.head() == &c.fsrInfo() && &c.fsrInfo() != &fsr;
        const auto &rendering = c.contract().pipelineInfo();
        const bool arrOwnAddr = c.fsrInfo().pNext == &rendering &&
                                c.contract().ready() &&
                                rendering.colorAttachmentCount == 2 &&
                                c.contract().signature().colorFormats.size() == 2 &&
                                rendering.pColorAttachmentFormats != nullptr &&
                                rendering.pColorAttachmentFormats ==
                                    c.contract().signature().colorFormats.data() &&
                                rendering.pColorAttachmentFormats != input.colorFormats.data();
        check(fsrOwnAddr, "owning: FSR address distinct");
        check(arrOwnAddr, "owning: color array address distinct");
        // only mutate caller inputs if ownership proven (mutation may free memory)
        if (fsrOwnAddr && arrOwnAddr) {
            input.colorFormats[0] = VK_FORMAT_R32_UINT;
            input.colorFormats.push_back(VK_FORMAT_R16_UINT); // may realloc caller array
            input.depthFormat = VK_FORMAT_D16_UNORM;
            fsr.fragmentSize = {8, 8};
            fsr.combinerOps[0] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MAX_KHR;
            verifyChain(c, true, true); // full verification after mutation
        }
    }

    // --- 7: scoped destruction: non-borrow proof inside scope, read after only if guard ---
    {
        VKPipelinePNextChain c;
        bool nonBorrow = false;
        {
            VKRenderingSignature input = makeSig();
            auto fsr = makeFsr();
            check(c.build(&input, &fsr), "scoped build");
            const auto &r = c.contract().pipelineInfo();
            nonBorrow = c.head() == &c.fsrInfo() && &c.fsrInfo() != &fsr &&
                        c.fsrInfo().pNext == &r && c.contract().ready() &&
                        r.colorAttachmentCount == 2 &&
                        c.contract().signature().colorFormats.size() == 2 &&
                        r.pColorAttachmentFormats != nullptr &&
                        r.pColorAttachmentFormats == c.contract().signature().colorFormats.data() &&
                        r.pColorAttachmentFormats != input.colorFormats.data();
            check(nonBorrow, "scoped: owned array proven inside scope");
        } // inputs destroyed
        if (nonBorrow)
            verifyChain(c, true, true); // full content read after destruction
        else
            check(false, "scoped: guard failed, cannot read");
    }

    // --- 8: two owners: rebuild one, full verify on both; array addresses differ ---
    {
        VKPipelinePNextChain a, b;
        auto s = makeSig();
        auto f = makeFsr();
        check(a.build(&s, &f) && b.build(&s, &f), "two owners");
        check(a.contract().pipelineInfo().pColorAttachmentFormats !=
                  b.contract().pipelineInfo().pColorAttachmentFormats,
              "two owners: distinct arrays");
        // rebuild a as FSR-only
        check(a.build(nullptr, &f), "rebuild a");
        verifyChain(a, true, false);
        verifyChain(b, true, true); // b fully intact
    }

    // --- 9: six FSR invalid conditions, each an independent cycle ---
    {
        VKPipelinePNextChain c;
        auto s = makeSig();
        auto f = makeFsr();
        auto badFsrCycle = [&](const char *label,
                               void (*mutate)(VkPipelineFragmentShadingRateStateCreateInfoKHR &, void *),
                               void *ctx) {
            check(c.build(&s, &f), label); // prime valid both
            verifyChain(c, true, true);
            auto bad = makeFsr();
            mutate(bad, ctx);
            check(!c.build(&s, &bad), label); // rejected
            verifyCleared(c);
            check(c.build(&s, &f), label); // recover valid both
            verifyChain(c, true, true);
        };
        badFsrCycle("fsr wrong sType", [](VkPipelineFragmentShadingRateStateCreateInfoKHR &v, void *) { v.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER; }, nullptr);
        badFsrCycle("fsr nonnull pNext", [](VkPipelineFragmentShadingRateStateCreateInfoKHR &v, void *ctx) { v.pNext = ctx; }, &f);
        badFsrCycle("fsr width 0", [](VkPipelineFragmentShadingRateStateCreateInfoKHR &v, void *) { v.fragmentSize = {0, 4}; }, nullptr);
        badFsrCycle("fsr height 0", [](VkPipelineFragmentShadingRateStateCreateInfoKHR &v, void *) { v.fragmentSize = {2, 0}; }, nullptr);
        badFsrCycle("fsr bad op0", [](VkPipelineFragmentShadingRateStateCreateInfoKHR &v, void *) { v.combinerOps[0] = static_cast<VkFragmentShadingRateCombinerOpKHR>(99); }, nullptr);
        badFsrCycle("fsr bad op1", [](VkPipelineFragmentShadingRateStateCreateInfoKHR &v, void *) { v.combinerOps[1] = static_cast<VkFragmentShadingRateCombinerOpKHR>(-1); }, nullptr);
    }

    // --- 10: three signature invalid conditions, each independent ---
    {
        VKPipelinePNextChain c;
        auto s = makeSig();
        auto f = makeFsr();
        auto badSigCycle = [&](const char *label,
                               void (*mutate)(VKRenderingSignature &)) {
            check(c.build(&s, &f), label);
            verifyChain(c, true, true);
            auto bad = makeSig();
            mutate(bad);
            check(!c.build(&bad, &f), label);
            verifyCleared(c);
            check(c.build(&s, &f), label);
            verifyChain(c, true, true);
        };
        badSigCycle("sig samples 0", [](VKRenderingSignature &v) { v.samples = static_cast<VkSampleCountFlagBits>(0); });
        badSigCycle("sig combined samples", [](VKRenderingSignature &v) { v.samples = static_cast<VkSampleCountFlagBits>(VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT); });
        badSigCycle("sig D-S mismatch", [](VKRenderingSignature &v) {
          v.depthFormat = VK_FORMAT_D32_SFLOAT; v.stencilFormat = VK_FORMAT_S8_UINT; });
    }

    // --- 11: self-input: own signature ---
    {
        VKPipelinePNextChain c;
        auto s = makeSig();
        auto f = makeFsr();
        check(c.build(&s, &f), "self: init");
        check(c.build(&c.contract().signature(), &f), "self: sig rebuild");
        verifyChain(c, true, true);
    }

    // --- 12: self-input: own FSR in FSR-only mode (pNext is null) ---
    {
        VKPipelinePNextChain c;
        auto f = makeFsr();
        check(c.build(nullptr, &f), "self-fsr: init");
        check(c.build(nullptr, &c.fsrInfo()), "self-fsr: rebuild");
        verifyChain(c, true, false);
    }

    // --- 13: combined own FSR has non-NULL tail, must be rejected ---
    {
        VKPipelinePNextChain c;
        auto s = makeSig();
        auto f = makeFsr();
        check(c.build(&s, &f), "combined: init");
        check(!c.build(&s, &c.fsrInfo()), "combined own FSR rejected");
        verifyCleared(c);
    }

    // --- 14: rebuild cycle with full verification ---
    {
        VKPipelinePNextChain c;
        auto s = makeSig();
        auto f = makeFsr();
        check(c.build(&s, &f), "cyc: both");
        verifyChain(c, true, true);
        check(c.build(nullptr, &f), "cyc: fsr");
        verifyChain(c, true, false);
        check(c.build(&s, nullptr), "cyc: r");
        verifyChain(c, false, true);
        check(c.build(), "cyc: none");
        verifyChain(c, false, false);
        auto bad = makeSig();
        bad.samples = static_cast<VkSampleCountFlagBits>(0);
        check(!c.build(&bad, nullptr), "cyc: invalid");
        verifyCleared(c);
        check(c.build(&s, &f), "cyc: recover");
        verifyChain(c, true, true);
    }

    // --- 15: public reset() ---
    {
        VKPipelinePNextChain c;
        auto s = makeSig();
        auto f = makeFsr();
        check(c.build(&s, &f), "reset: prime");
        verifyChain(c, true, true);
        c.reset();
        verifyCleared(c);
    }

    if (g_failures == 0) {
        std::printf("pipeline pnext chain tests: all PASS\n");
        return 0;
    }
    std::printf("pipeline pnext chain tests: %u FAIL\n", g_failures);
    return 1;
}
