// Offline owning VKRenderingContract CPU contract tests (M5-C, revised).
// No GPU, no loader, no engine. Covers: initial/failure sType correctness,
// self-input rebuild, owning independence from caller mutation/destruction,
// validation rules, secondary flag propagation, and node chaining.

#include <cstdio>
#include <type_traits>
#include <vector>
#include "VKRenderingContract.h"

using cc::gfx::VKRenderingContract;
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

// verify every node field for a given built contract
void verifyFull(const VKRenderingContract &c, const VKRenderingSignature &sig,
                VkRenderingFlags expectedSecFlags) {
    const auto &p = c.pipelineInfo();
    check(p.sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, "pipeline sType");
    check(p.pNext == nullptr, "pipeline pNext");
    check(p.viewMask == sig.viewMask, "pipeline viewMask");
    check(p.colorAttachmentCount == sig.colorFormats.size(), "pipeline colorCount");
    const bool pipelineColorsSafe = p.colorAttachmentCount == sig.colorFormats.size() &&
                                    (sig.colorFormats.empty() ? p.pColorAttachmentFormats == nullptr : p.pColorAttachmentFormats != nullptr);
    check(pipelineColorsSafe, "pipeline color view shape");
    if (pipelineColorsSafe) {
        for (size_t i = 0; i < sig.colorFormats.size(); ++i)
            check(p.pColorAttachmentFormats[i] == sig.colorFormats[i], "pipeline color[i]");
    }
    check(p.depthAttachmentFormat == sig.depthFormat, "pipeline depth");
    check(p.stencilAttachmentFormat == sig.stencilFormat, "pipeline stencil");

    const auto &sec = c.secondaryRenderingInfo();
    check(sec.sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO, "sec sType");
    check(sec.viewMask == sig.viewMask, "sec viewMask");
    check(sec.colorAttachmentCount == sig.colorFormats.size(), "sec colorCount");
    const bool secondaryColorsSafe = sec.colorAttachmentCount == sig.colorFormats.size() &&
                                     (sig.colorFormats.empty() ? sec.pColorAttachmentFormats == nullptr : sec.pColorAttachmentFormats != nullptr);
    check(secondaryColorsSafe, "secondary color view shape");
    if (secondaryColorsSafe) {
        for (size_t i = 0; i < sig.colorFormats.size(); ++i)
            check(sec.pColorAttachmentFormats[i] == sig.colorFormats[i], "sec color[i]");
    }
    check(sec.depthAttachmentFormat == sig.depthFormat, "sec depth");
    check(sec.stencilAttachmentFormat == sig.stencilFormat, "sec stencil");
    check(sec.rasterizationSamples == sig.samples, "sec rasterizationSamples");
    check(sec.flags == expectedSecFlags, "sec flags");

    const auto &base = c.baseInheritanceInfo();
    check(base.sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, "base sType");
    check(base.pNext == &c.secondaryRenderingInfo(), "base pNext chain");
    check(base.renderPass == VK_NULL_HANDLE, "base renderPass null");
    check(base.framebuffer == VK_NULL_HANDLE, "base framebuffer null");
    check(base.subpass == 0, "base subpass 0");
}
void verifyReset(const VKRenderingContract &c) {
    const auto &p = c.pipelineInfo();
    const auto &sec = c.secondaryRenderingInfo();
    const auto &base = c.baseInheritanceInfo();
    check(!c.ready() && c.signature() == VKRenderingSignature{}, "reset: default snapshot and readiness");
    check(p.sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO && p.pNext == nullptr &&
              p.colorAttachmentCount == 0 && p.pColorAttachmentFormats == nullptr && p.viewMask == 0 &&
              p.depthAttachmentFormat == VK_FORMAT_UNDEFINED && p.stencilAttachmentFormat == VK_FORMAT_UNDEFINED,
          "reset: complete pipeline view");
    check(sec.sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO && sec.pNext == nullptr &&
              sec.colorAttachmentCount == 0 && sec.pColorAttachmentFormats == nullptr && sec.viewMask == 0 &&
              sec.flags == 0 && sec.rasterizationSamples == 0 &&
              sec.depthAttachmentFormat == VK_FORMAT_UNDEFINED && sec.stencilAttachmentFormat == VK_FORMAT_UNDEFINED,
          "reset: complete secondary view");
    check(base.sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO && base.pNext == nullptr &&
              base.renderPass == VK_NULL_HANDLE && base.framebuffer == VK_NULL_HANDLE && base.subpass == 0 &&
              base.occlusionQueryEnable == VK_FALSE && base.queryFlags == 0 && base.pipelineStatistics == 0,
          "reset: complete base inheritance");
}
} // namespace

int main() {
    static_assert(!std::is_copy_constructible<VKRenderingContract>::value &&
                      !std::is_copy_assignable<VKRenderingContract>::value &&
                      !std::is_move_constructible<VKRenderingContract>::value &&
                      !std::is_move_assignable<VKRenderingContract>::value,
                  "VKRenderingContract must be non-copyable and non-movable");

    // --- 1: initial state: not ready, correct sTypes, all zeroed ---
    {
        VKRenderingContract c;
        check(!c.ready(), "initial: not ready");
        verifyReset(c);
        check(c.pipelineInfo().sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
              "initial: pipeline sType");
        check(c.pipelineInfo().colorAttachmentCount == 0, "initial: pipeline count 0");
        check(c.pipelineInfo().pColorAttachmentFormats == nullptr, "initial: pipeline ptr null");
        check(c.pipelineInfo().depthAttachmentFormat == VK_FORMAT_UNDEFINED, "initial: pipeline depth");
        check(c.secondaryRenderingInfo().sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
              "initial: sec sType");
        check(c.secondaryRenderingInfo().colorAttachmentCount == 0, "initial: sec count 0");
        check(c.secondaryRenderingInfo().flags == 0, "initial: sec flags 0");
        check(c.baseInheritanceInfo().sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
              "initial: base sType");
        check(c.baseInheritanceInfo().pNext == nullptr, "initial: base pNext null");
    }

    // --- 2: successful build: full field verification ---
    {
        VKRenderingContract c;
        check(c.build(makeSig()), "build success");
        check(c.ready(), "ready");
        verifyFull(c, makeSig(), 0);
    }

    // --- 3: failed build fully resets (including sTypes) ---
    {
        VKRenderingContract c;
        check(c.build(makeSig()), "valid first");
        VKRenderingSignature bad = makeSig();
        bad.samples = static_cast<VkSampleCountFlagBits>(0);
        check(!c.build(bad), "bad samples rejected");
        check(!c.ready(), "failed: not ready");
        verifyReset(c);
        check(c.pipelineInfo().sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, "failed: pipeline sType");
        check(c.pipelineInfo().colorAttachmentCount == 0, "failed: pipeline count");
        check(c.pipelineInfo().pColorAttachmentFormats == nullptr, "failed: pipeline ptr");
        check(c.pipelineInfo().depthAttachmentFormat == VK_FORMAT_UNDEFINED, "failed: pipeline depth");
        check(c.secondaryRenderingInfo().colorAttachmentCount == 0, "failed: sec count");
        check(c.secondaryRenderingInfo().flags == 0, "failed: sec flags 0");
        check(c.secondaryRenderingInfo().depthAttachmentFormat == VK_FORMAT_UNDEFINED, "failed: sec depth");
        check(c.baseInheritanceInfo().pNext == nullptr, "failed: base pNext");
    }

    // --- 4: self-input build (c.signature() reference) ---
    {
        VKRenderingContract c;
        const VkRenderingFlags flags = VK_RENDERING_SUSPENDING_BIT;
        check(c.build(makeSig(), flags), "initial build");
        // self-input: rebuild using own signature reference
        check(c.build(c.signature(), flags | VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT),
              "self-input build");
        check(c.ready(), "self-input: ready");
        // verify all fields preserved from the snapshot taken before reset
        check(c.pipelineInfo().colorAttachmentCount == 2, "self-input: color count preserved");
        verifyFull(c, makeSig(), flags);
        check(c.pipelineInfo().depthAttachmentFormat == VK_FORMAT_D24_UNORM_S8_UINT,
              "self-input: depth preserved");
        check(c.secondaryRenderingInfo().rasterizationSamples == VK_SAMPLE_COUNT_2_BIT,
              "self-input: samples preserved");
        check(c.secondaryRenderingInfo().viewMask == 0x3, "self-input: viewMask preserved");
        // SECONDARY_CONTENTS cleared even in self-input path
        check((c.secondaryRenderingInfo().flags & VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT) == 0,
              "self-input: SECONDARY_CONTENTS cleared");
        check((c.secondaryRenderingInfo().flags & VK_RENDERING_SUSPENDING_BIT) != 0,
              "self-input: SUSPEND preserved");
    }

    // --- 5: owning independence: modify the actual input signature ---
    {
        VKRenderingContract c;
        VKRenderingSignature input = makeSig();
        check(c.build(input), "owning build");
        const bool ownsArray = c.pipelineInfo().pColorAttachmentFormats != input.colorFormats.data();
        check(ownsArray, "owning: distinct array before caller resize");
        // mutate the ACTUAL input signature (not a copy)
        input.colorFormats[0] = VK_FORMAT_R32_UINT;
        input.colorFormats.push_back(VK_FORMAT_R16_UINT);
        input.depthFormat = VK_FORMAT_D16_UNORM;
        input.samples = VK_SAMPLE_COUNT_8_BIT;
        if (ownsArray) verifyFull(c, makeSig(), 0);
        check(c.pipelineInfo().colorAttachmentCount == 2, "owning: caller resize not reflected");
        check(c.pipelineInfo().depthAttachmentFormat == VK_FORMAT_D24_UNORM_S8_UINT,
              "owning: caller depth mutation not reflected");
        check(c.secondaryRenderingInfo().rasterizationSamples == VK_SAMPLE_COUNT_2_BIT,
              "owning: caller samples mutation not reflected");
        // payload array address differs from input array
        check(c.pipelineInfo().pColorAttachmentFormats != input.colorFormats.data(),
              "owning: payload array is not the caller's array");
    }

    // --- 6: owning: input destroyed after build ---
    {
        VKRenderingContract c;
        bool ownsArray = false;
        {
            VKRenderingSignature temp = makeSig();
            check(c.build(temp), "scoped build");
            ownsArray = c.pipelineInfo().pColorAttachmentFormats != temp.colorFormats.data();
            check(ownsArray, "scoped: array ownership before input destruction");
        } // temp destroyed here
        check(c.ready(), "input destroyed: still ready");
        check(c.pipelineInfo().colorAttachmentCount == 2, "input destroyed: count intact");
        if (ownsArray) verifyFull(c, makeSig(), 0);
    }

    // --- 7: two owners from same input, rebuild one ---
    {
        VKRenderingSignature shared = makeSig();
        VKRenderingContract a, b;
        check(a.build(shared) && b.build(shared), "two owners build");
        // rebuild a with different data
        VKRenderingSignature different;
        different.depthFormat = VK_FORMAT_D32_SFLOAT;
        check(a.build(different), "rebuild a");
        check(a.pipelineInfo().depthAttachmentFormat == VK_FORMAT_D32_SFLOAT, "a: new depth");
        check(a.pipelineInfo().colorAttachmentCount == 0, "a: empty colors");
        // b unchanged
        check(b.pipelineInfo().depthAttachmentFormat == VK_FORMAT_D24_UNORM_S8_UINT, "b: depth intact");
        check(b.pipelineInfo().colorAttachmentCount == 2, "b: colors intact");
        check(b.ready(), "b: still ready");
        verifyFull(b, shared, 0);
    }

    // --- 8: rebuild cycle: big -> small -> empty -> invalid -> success ---
    {
        VKRenderingContract c;
        VKRenderingSignature big = makeSig();
        big.colorFormats = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM,
                            VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32};
        check(c.build(big), "cycle: big");
        check(c.pipelineInfo().colorAttachmentCount == 5, "cycle: big count 5");
        VKRenderingSignature small;
        small.depthFormat = VK_FORMAT_D16_UNORM;
        small.colorFormats = {VK_FORMAT_R8_UNORM};
        check(c.build(small), "cycle: small");
        verifyFull(c, small, 0);
        VKRenderingSignature empty;
        check(c.build(empty), "cycle: empty");
        check(c.pipelineInfo().colorAttachmentCount == 0, "cycle: empty ok");
        VKRenderingSignature bad;
        bad.samples = static_cast<VkSampleCountFlagBits>(3);
        check(!c.build(bad), "cycle: invalid");
        check(!c.ready(), "cycle: invalid not ready");
        verifyReset(c);
        check(c.build(makeSig()), "cycle: recover");
        check(c.ready(), "cycle: recovered");
        verifyFull(c, makeSig(), 0);
    }

    // --- 9: samples validation ---
    {
        VKRenderingContract c;
        VKRenderingSignature s = makeSig();
        s.samples = static_cast<VkSampleCountFlagBits>(0);
        check(!c.build(s), "samples 0 rejected");
        s.samples = static_cast<VkSampleCountFlagBits>(VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT);
        check(!c.build(s), "samples combined rejected");
    }

    // --- 10: depth/stencil validation ---
    {
        VKRenderingContract c;
        VKRenderingSignature s = makeSig();
        s.depthFormat = VK_FORMAT_D32_SFLOAT;
        s.stencilFormat = VK_FORMAT_S8_UINT;
        check(!c.build(s), "D/S mismatch rejected");
        s.depthFormat = VK_FORMAT_D32_SFLOAT;
        s.stencilFormat = VK_FORMAT_UNDEFINED;
        check(c.build(s), "depth-only accepted");
        s.depthFormat = VK_FORMAT_UNDEFINED;
        s.stencilFormat = VK_FORMAT_S8_UINT;
        check(c.build(s), "stencil-only accepted");
        const VkFormat combined = VK_FORMAT_D24_UNORM_S8_UINT;
        s.depthFormat = combined;
        s.stencilFormat = combined;
        check(c.build(s), "combined same accepted");
    }

    // --- 11: flags: SECONDARY cleared, SUSPEND/RESUME preserved ---
    {
        VKRenderingContract c;
        const VkRenderingFlags flags =
            VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT |
            VK_RENDERING_SUSPENDING_BIT | VK_RENDERING_RESUMING_BIT;
        check(c.build(makeSig(), flags), "flags build");
        check((c.secondaryRenderingInfo().flags & VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT) == 0,
              "flags: SECONDARY cleared");
        check((c.secondaryRenderingInfo().flags & VK_RENDERING_SUSPENDING_BIT) != 0,
              "flags: SUSPEND preserved");
        check((c.secondaryRenderingInfo().flags & VK_RENDERING_RESUMING_BIT) != 0,
              "flags: RESUME preserved");
    }

    // --- 12: secondary all fields consistent with snapshot ---
    {
        VKRenderingContract c;
        VKRenderingSignature sig = makeSig();
        sig.viewMask = 0x7;
        check(c.build(sig, VK_RENDERING_SUSPENDING_BIT), "all-fields build");
        const auto &sec = c.secondaryRenderingInfo();
        check(sec.viewMask == 0x7, "all: sec viewMask");
        check(sec.colorAttachmentCount == sig.colorFormats.size(), "all: sec count");
        check(sec.depthAttachmentFormat == sig.depthFormat, "all: sec depth");
        check(sec.stencilAttachmentFormat == sig.stencilFormat, "all: sec stencil");
        check(sec.rasterizationSamples == sig.samples, "all: sec rasterSamples");
        check(sec.flags == VK_RENDERING_SUSPENDING_BIT, "all: sec flags exact");
    }

    // --- 13: empty colors ---
    {
        VKRenderingContract c;
        VKRenderingSignature s;
        s.depthFormat = VK_FORMAT_D32_SFLOAT;
        check(c.build(s), "empty build");
        check(c.pipelineInfo().colorAttachmentCount == 0, "empty: count 0");
        check(c.pipelineInfo().pColorAttachmentFormats == nullptr, "empty: ptr null");
        check(c.secondaryRenderingInfo().colorAttachmentCount == 0, "empty: sec count 0");
        check(c.secondaryRenderingInfo().pColorAttachmentFormats == nullptr, "empty: sec ptr null");
    }

    // --- 14: pipeline and secondary share same owned array ---
    {
        VKRenderingContract c;
        check(c.build(makeSig()), "shared build");
        check(c.pipelineInfo().pColorAttachmentFormats ==
                  c.secondaryRenderingInfo().pColorAttachmentFormats,
              "pipeline/sec share same owned array");
    }

    // --- 15: public reset() ---
    {
        VKRenderingContract c;
        check(c.build(makeSig()), "reset: prime");
        c.reset();
        check(!c.ready(), "reset: not ready");
        check(c.pipelineInfo().sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, "reset: pipeline sType");
        check(c.pipelineInfo().colorAttachmentCount == 0, "reset: pipeline count");
        check(c.pipelineInfo().pColorAttachmentFormats == nullptr, "reset: pipeline ptr");
        check(c.secondaryRenderingInfo().sType ==
                  VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
              "reset: sec sType");
        check(c.baseInheritanceInfo().sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
              "reset: base sType");
        check(c.baseInheritanceInfo().pNext == nullptr, "reset: base pNext");
    }

    if (g_failures == 0) {
        std::printf("rendering contract tests: all PASS\n");
        return 0;
    }
    std::printf("rendering contract tests: %u FAIL\n", g_failures);
    return 1;
}
