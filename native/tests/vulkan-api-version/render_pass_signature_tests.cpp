// Classic RenderPass -> rendering signature tuple adapter CPU tests
// (M5-G, rev 2). Full-tuple verify helpers; attachmentless support.
// The standalone-include probe variant lives behind
// VKRPSIG_STANDALONE_PROBE (compile-only, not a run contract).

#if defined(VKRPSIG_STANDALONE_PROBE)
    #include "VKRenderPassSignatureAdapter.h"
int main() { return 0; }
#else

    #include <cstdio>
    #include <utility>
    #include <vector>
    #include "VKRenderPassSignatureAdapter.h"

using cc::gfx::buildVKSubpassRenderingMetadata;
using cc::gfx::VKRenderingSignature;
using cc::gfx::VKSubpassRenderingMetadata;

namespace {
unsigned g_failures = 0;
void check(bool ok, const char *w) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", w);
        ++g_failures;
    }
}

VkAttachmentDescription2 makeAttachment(VkFormat format) {
    VkAttachmentDescription2 d = {VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
    d.format = format;
    d.samples = VK_SAMPLE_COUNT_1_BIT;
    return d;
}
VkAttachmentReference2 makeRef(uint32_t attachment) {
    return {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, attachment,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT};
}
// caller keeps refs alive so multiple subpasses can coexist
VkSubpassDescription2 makeSubpass(const std::vector<uint32_t> &colors,
                                  std::vector<VkAttachmentReference2> &refs) {
    refs.clear();
    for (uint32_t c : colors) refs.push_back(makeRef(c));
    VkSubpassDescription2 s = {VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
    s.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    s.colorAttachmentCount = static_cast<uint32_t>(colors.size());
    s.pColorAttachments = colors.empty() ? nullptr : refs.data();
    return s;
}

// full tuple verification: valid + colors (size guard then per-format) +
// depth/stencil/samples/viewMask - every field, never a partial peek
void verifyTuple(const VKSubpassRenderingMetadata &m, const VKRenderingSignature &want) {
    check(m.valid, "vt: valid");
    if (m.signature.colorFormats.size() != want.colorFormats.size()) {
        check(false, "vt: color size");
        return;
    }
    for (size_t i = 0; i < want.colorFormats.size(); ++i)
        if (m.signature.colorFormats[i] != want.colorFormats[i]) {
            check(false, "vt: color elem");
            return;
        }
    check(m.signature.depthFormat == want.depthFormat, "vt: depth");
    check(m.signature.stencilFormat == want.stencilFormat, "vt: stencil");
    check(m.signature.samples == want.samples, "vt: samples");
    check(m.signature.viewMask == want.viewMask, "vt: viewMask");
}

// full empty verification: invalid + complete default signature
void verifyEmpty(const VKSubpassRenderingMetadata &m) {
    check(!m.valid, "vE: invalid");
    check(m.signature.colorFormats.empty(), "vE: no colors");
    check(m.signature.depthFormat == VK_FORMAT_UNDEFINED, "vE: depth undefined");
    check(m.signature.stencilFormat == VK_FORMAT_UNDEFINED, "vE: stencil undefined");
    check(m.signature.samples == VK_SAMPLE_COUNT_1_BIT, "vE: samples 1");
    check(m.signature.viewMask == 0, "vE: viewMask 0");
}

VKRenderingSignature wantSig(std::vector<VkFormat> colors, VkFormat d, VkFormat st,
                             VkSampleCountFlagBits smp, uint32_t vm) {
    VKRenderingSignature w;
    w.colorFormats = std::move(colors);
    w.depthFormat = d;
    w.stencilFormat = st;
    w.samples = smp;
    w.viewMask = vm;
    return w;
}
} // namespace

int main() {
    // --- 1: colors asymmetric reorder + unused middle slot kept ---
    {
        std::vector<VkAttachmentDescription2> atts = {
            makeAttachment(VK_FORMAT_B8G8R8A8_UNORM),
            makeAttachment(VK_FORMAT_R16G16B16A16_SFLOAT),
            makeAttachment(VK_FORMAT_R8_UNORM),
        };
        std::vector<VkAttachmentReference2> subRefs;
        VkSubpassDescription2 sub = makeSubpass({2, VK_ATTACHMENT_UNUSED, 0}, subRefs);
        sub.viewMask = 0x5;
        VKSubpassRenderingMetadata m;
        check(buildVKSubpassRenderingMetadata(3, atts.data(), sub, VK_SAMPLE_COUNT_4_BIT, m),
              "reorder build");
        verifyTuple(m, wantSig({VK_FORMAT_R8_UNORM, VK_FORMAT_UNDEFINED, VK_FORMAT_B8G8R8A8_UNORM},
                               VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_4_BIT, 0x5));
    }

    // --- 2: seven D/S classes, full tuple incl stencil, aspect-independent ---
    {
        struct Case {
            VkFormat format;
            VkFormat wantDepth;
            VkFormat wantStencil;
        };
        const Case cases[] = {
            {VK_FORMAT_D16_UNORM, VK_FORMAT_D16_UNORM, VK_FORMAT_UNDEFINED},
            {VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_UNDEFINED},
            {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT, VK_FORMAT_UNDEFINED},
            {VK_FORMAT_S8_UINT, VK_FORMAT_UNDEFINED, VK_FORMAT_S8_UINT},
            {VK_FORMAT_D16_UNORM_S8_UINT, VK_FORMAT_D16_UNORM_S8_UINT, VK_FORMAT_D16_UNORM_S8_UINT},
            {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
            {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT},
        };
        for (const auto &tc : cases) {
            std::vector<VkAttachmentDescription2> atts = {makeAttachment(tc.format)};
            std::vector<VkAttachmentReference2> refs;
            auto sub = makeSubpass({}, refs); // depth/stencil-only: no colors
            static VkAttachmentReference2 dsRef;
            dsRef = makeRef(0);
            dsRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            sub.pDepthStencilAttachment = &dsRef;
            VKSubpassRenderingMetadata m;
            check(buildVKSubpassRenderingMetadata(1, atts.data(), sub, VK_SAMPLE_COUNT_1_BIT, m),
                  "ds class build");
            verifyTuple(m, wantSig({}, tc.wantDepth, tc.wantStencil, VK_SAMPLE_COUNT_1_BIT, 0));
            // aspectMask variations must not change the tuple
            for (VkImageAspectFlags aspect :
                 {static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_STENCIL_BIT),
                  static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT),
                  static_cast<VkImageAspectFlags>(0)}) {
                dsRef.aspectMask = aspect;
                VKSubpassRenderingMetadata m2;
                check(buildVKSubpassRenderingMetadata(1, atts.data(), sub, VK_SAMPLE_COUNT_1_BIT, m2),
                      "ds aspect build");
                verifyTuple(m2, wantSig({}, tc.wantDepth, tc.wantStencil, VK_SAMPLE_COUNT_1_BIT, 0));
            }
            dsRef.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        }
    }

    // --- 3: D/S null pointer and UNUSED, full tuple ---
    {
        std::vector<VkAttachmentDescription2> atts = {makeAttachment(VK_FORMAT_B8G8R8A8_UNORM)};
        std::vector<VkAttachmentReference2> refs;
        auto sub = makeSubpass({0}, refs);
        VKSubpassRenderingMetadata m;
        check(buildVKSubpassRenderingMetadata(1, atts.data(), sub, VK_SAMPLE_COUNT_2_BIT, m),
              "ds null build");
        verifyTuple(m, wantSig({VK_FORMAT_B8G8R8A8_UNORM}, VK_FORMAT_UNDEFINED,
                               VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_2_BIT, 0));
        static VkAttachmentReference2 unusedRef = {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr,
                                                   VK_ATTACHMENT_UNUSED,
                                                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0};
        sub.pDepthStencilAttachment = &unusedRef;
        VKSubpassRenderingMetadata m2;
        check(buildVKSubpassRenderingMetadata(1, atts.data(), sub, VK_SAMPLE_COUNT_2_BIT, m2),
              "ds unused build");
        verifyTuple(m2, wantSig({VK_FORMAT_B8G8R8A8_UNORM}, VK_FORMAT_UNDEFINED,
                                VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_2_BIT, 0));
    }

    // --- 4: attachmentless: count 0 + NULL + no refs, sample/viewMask kept ---
    {
        VkSubpassDescription2 sub = {VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.viewMask = 0x3;
        VKSubpassRenderingMetadata m;
        check(buildVKSubpassRenderingMetadata(0, nullptr, sub, VK_SAMPLE_COUNT_8_BIT, m),
              "attachmentless build");
        verifyTuple(m, wantSig({}, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED,
                               VK_SAMPLE_COUNT_8_BIT, 0x3));
    }

    // --- 5: attachmentless with all-UNUSED color slots keeps slot count ---
    {
        static VkAttachmentReference2 u0 = {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr,
                                            VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL, 0};
        static VkAttachmentReference2 u1 = {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr,
                                            VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL, 0};
        VkAttachmentReference2 refsArr[2] = {u0, u1};
        VkSubpassDescription2 sub = {VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 2;
        sub.pColorAttachments = refsArr;
        sub.viewMask = 0x2;
        VKSubpassRenderingMetadata m;
        check(buildVKSubpassRenderingMetadata(0, nullptr, sub, VK_SAMPLE_COUNT_16_BIT, m),
              "attachmentless unused slots");
        verifyTuple(m, wantSig({VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED}, VK_FORMAT_UNDEFINED,
                               VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_16_BIT, 0x2));
    }

    // --- 6: sample bits 1..64 accepted, viewMask preserved; unknown bit rejected ---
    {
        std::vector<VkAttachmentDescription2> atts = {makeAttachment(VK_FORMAT_B8G8R8A8_UNORM)};
        std::vector<VkAttachmentReference2> refs;
        auto sub = makeSubpass({0}, refs);
        sub.viewMask = 0x9;
        for (VkSampleCountFlagBits s :
             {VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_2_BIT, VK_SAMPLE_COUNT_4_BIT,
              VK_SAMPLE_COUNT_8_BIT, VK_SAMPLE_COUNT_16_BIT, VK_SAMPLE_COUNT_32_BIT,
              VK_SAMPLE_COUNT_64_BIT}) {
            VKSubpassRenderingMetadata m;
            check(buildVKSubpassRenderingMetadata(1, atts.data(), sub, s, m), "sample bit");
            verifyTuple(m, wantSig({VK_FORMAT_B8G8R8A8_UNORM}, VK_FORMAT_UNDEFINED,
                                   VK_FORMAT_UNDEFINED, s, 0x9));
        }
        VKSubpassRenderingMetadata m;
        check(!buildVKSubpassRenderingMetadata(
                  1, atts.data(), sub, static_cast<VkSampleCountFlagBits>(0x80), m),
              "sample 0x80 rejected");
        verifyEmpty(m);
    }

    // --- 7: input/resolve/preserve/pNext changes do not affect tuple ---
    {
        std::vector<VkAttachmentDescription2> atts = {makeAttachment(VK_FORMAT_B8G8R8A8_UNORM),
                                                      makeAttachment(VK_FORMAT_D24_UNORM_S8_UINT)};
        static VkAttachmentReference2 dsRef = {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 1,
                                               VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0};
        static VkAttachmentReference2 inputRefs[1] = {
            {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 0,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT}};
        static VkAttachmentReference2 resolveRefs[1] = {
            {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 0,
             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT}};
        static uint32_t preserveRefs[1] = {1};
        static VkSubpassDescriptionDepthStencilResolve resolveNode = {
            VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
        std::vector<VkAttachmentReference2> refs;
        auto sub = makeSubpass({0}, refs);
        sub.pDepthStencilAttachment = &dsRef;
        VKSubpassRenderingMetadata m;
        check(buildVKSubpassRenderingMetadata(2, atts.data(), sub, VK_SAMPLE_COUNT_1_BIT, m),
              "extras base");
        verifyTuple(m, wantSig({VK_FORMAT_B8G8R8A8_UNORM}, VK_FORMAT_D24_UNORM_S8_UINT,
                               VK_FORMAT_D24_UNORM_S8_UINT, VK_SAMPLE_COUNT_1_BIT, 0));
        // add input/resolve/preserve/pNext: tuple must stay identical
        sub.inputAttachmentCount = 1;
        sub.pInputAttachments = inputRefs;
        sub.pResolveAttachments = resolveRefs;
        sub.preserveAttachmentCount = 1;
        sub.pPreserveAttachments = preserveRefs;
        sub.pNext = &resolveNode;
        VKSubpassRenderingMetadata m2;
        check(buildVKSubpassRenderingMetadata(2, atts.data(), sub, VK_SAMPLE_COUNT_1_BIT, m2),
              "extras build");
        verifyTuple(m2, wantSig({VK_FORMAT_B8G8R8A8_UNORM}, VK_FORMAT_D24_UNORM_S8_UINT,
                                VK_FORMAT_D24_UNORM_S8_UINT, VK_SAMPLE_COUNT_1_BIT, 0));
    }

    // --- 8: two outputs, distinct storage, independent rebuild ---
    {
        std::vector<VkAttachmentDescription2> atts = {makeAttachment(VK_FORMAT_B8G8R8A8_UNORM)};
        std::vector<VkAttachmentReference2> refsA, refsB;
        auto subA = makeSubpass({0}, refsA);
        auto subB = makeSubpass({0}, refsB);
        VKSubpassRenderingMetadata a, b;
        check(buildVKSubpassRenderingMetadata(1, atts.data(), subA, VK_SAMPLE_COUNT_1_BIT, a) &&
                  buildVKSubpassRenderingMetadata(1, atts.data(), subB, VK_SAMPLE_COUNT_1_BIT, b),
              "two outputs");
        check(a.signature.colorFormats.data() != b.signature.colorFormats.data(),
              "two: distinct vector storage");
        // rebuild a with a different full tuple; b keeps its original
        std::vector<VkAttachmentDescription2> atts2 = {makeAttachment(VK_FORMAT_R8_UNORM),
                                                       makeAttachment(VK_FORMAT_S8_UINT)};
        std::vector<VkAttachmentReference2> refsA2;
        auto subA2 = makeSubpass({0}, refsA2);
        static VkAttachmentReference2 dsRef2 = {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 1,
                                                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0};
        subA2.pDepthStencilAttachment = &dsRef2;
        subA2.viewMask = 0x6;
        check(buildVKSubpassRenderingMetadata(2, atts2.data(), subA2, VK_SAMPLE_COUNT_4_BIT, a),
              "two: rebuild a");
        verifyTuple(a, wantSig({VK_FORMAT_R8_UNORM}, VK_FORMAT_UNDEFINED, VK_FORMAT_S8_UINT,
                               VK_SAMPLE_COUNT_4_BIT, 0x6));
        verifyTuple(b, wantSig({VK_FORMAT_B8G8R8A8_UNORM}, VK_FORMAT_UNDEFINED,
                               VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, 0));
    }

    // --- 9: owning: caller mutations + array destruction, full tuple ---
    {
        VKSubpassRenderingMetadata m;
        {
            std::vector<VkAttachmentDescription2> atts = {
                makeAttachment(VK_FORMAT_B8G8R8A8_UNORM),
                makeAttachment(VK_FORMAT_D24_UNORM_S8_UINT)};
            std::vector<VkAttachmentReference2> refs;
            auto sub = makeSubpass({0}, refs);
            static VkAttachmentReference2 dsRef = {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr,
                                                   1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0};
            sub.pDepthStencilAttachment = &dsRef;
            sub.viewMask = 0x7;
            check(buildVKSubpassRenderingMetadata(2, atts.data(), sub, VK_SAMPLE_COUNT_2_BIT, m),
                  "owning build");
            const auto want = wantSig({VK_FORMAT_B8G8R8A8_UNORM}, VK_FORMAT_D24_UNORM_S8_UINT,
                                      VK_FORMAT_D24_UNORM_S8_UINT, VK_SAMPLE_COUNT_2_BIT, 0x7);
            // mutate caller data in scope
            atts[0].format = VK_FORMAT_R32_UINT;
            atts[1].format = VK_FORMAT_S8_UINT;
            dsRef.attachment = 0;
            refs[0].attachment = 1;
            sub.viewMask = 0;
            verifyTuple(m, want); // in-scope unchanged
        } // arrays destroyed
        verifyTuple(m, wantSig({VK_FORMAT_B8G8R8A8_UNORM}, VK_FORMAT_D24_UNORM_S8_UINT,
                               VK_FORMAT_D24_UNORM_S8_UINT, VK_SAMPLE_COUNT_2_BIT, 0x7));
    }

    // --- 10: rebuild sequence big -> small -> attachmentless -> invalid -> recover ---
    {
        std::vector<VkAttachmentDescription2> atts = {
            makeAttachment(VK_FORMAT_B8G8R8A8_UNORM), makeAttachment(VK_FORMAT_R8_UNORM),
            makeAttachment(VK_FORMAT_D24_UNORM_S8_UINT)};
        static VkAttachmentReference2 dsRef = {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 2,
                                               VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0};
        VKSubpassRenderingMetadata m;
        std::vector<VkAttachmentReference2> bigRefs, smallRefs;
        auto big = makeSubpass({0, 1}, bigRefs);
        big.pDepthStencilAttachment = &dsRef;
        big.viewMask = 0x1;
        check(buildVKSubpassRenderingMetadata(3, atts.data(), big, VK_SAMPLE_COUNT_4_BIT, m),
              "seq big");
        verifyTuple(m, wantSig({VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8_UNORM},
                               VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT,
                               VK_SAMPLE_COUNT_4_BIT, 0x1));
        auto small = makeSubpass({1}, smallRefs);
        check(buildVKSubpassRenderingMetadata(3, atts.data(), small, VK_SAMPLE_COUNT_1_BIT, m),
              "seq small");
        verifyTuple(m, wantSig({VK_FORMAT_R8_UNORM}, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED,
                               VK_SAMPLE_COUNT_1_BIT, 0));
        VkSubpassDescription2 none = {VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
        none.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        check(buildVKSubpassRenderingMetadata(0, nullptr, none, VK_SAMPLE_COUNT_1_BIT, m),
              "seq attachmentless");
        verifyTuple(m, wantSig({}, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED,
                               VK_SAMPLE_COUNT_1_BIT, 0));
        std::vector<VkAttachmentReference2> badRefs;
        auto bad = makeSubpass({5}, badRefs); // out of range
        check(!buildVKSubpassRenderingMetadata(3, atts.data(), bad, VK_SAMPLE_COUNT_1_BIT, m),
              "seq bad");
        verifyEmpty(m);
        check(buildVKSubpassRenderingMetadata(3, atts.data(), big, VK_SAMPLE_COUNT_4_BIT, m),
              "seq recover");
        verifyTuple(m, wantSig({VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8_UNORM},
                               VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT,
                               VK_SAMPLE_COUNT_4_BIT, 0x1));
    }

    // --- 11: every rejection: prime full -> bad -> full empty -> recover full ---
    {
        std::vector<VkAttachmentDescription2> atts = {
            makeAttachment(VK_FORMAT_B8G8R8A8_UNORM), makeAttachment(VK_FORMAT_D24_UNORM_S8_UINT)};
        static VkAttachmentReference2 dsRef = {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 1,
                                               VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0};
        std::vector<VkAttachmentReference2> goodRefs, tRefs;
        auto good = makeSubpass({0}, goodRefs);
        good.pDepthStencilAttachment = &dsRef;
        good.viewMask = 0x5;
        const VKRenderingSignature wantGood = wantSig(
            {VK_FORMAT_B8G8R8A8_UNORM}, VK_FORMAT_D24_UNORM_S8_UINT,
            VK_FORMAT_D24_UNORM_S8_UINT, VK_SAMPLE_COUNT_4_BIT, 0x5);
        VKSubpassRenderingMetadata m;
        unsigned rejections = 0;
        auto cycle = [&](const char *label, uint32_t count,
                         const VkAttachmentDescription2 *attsPtr, VkSubpassDescription2 sub,
                         VkSampleCountFlagBits samples) {
            ++rejections;
            check(buildVKSubpassRenderingMetadata(2, atts.data(), good, VK_SAMPLE_COUNT_4_BIT, m),
                  label);
            verifyTuple(m, wantGood);
            check(!buildVKSubpassRenderingMetadata(count, attsPtr, sub, samples, m), label);
            verifyEmpty(m);
            check(buildVKSubpassRenderingMetadata(2, atts.data(), good, VK_SAMPLE_COUNT_4_BIT, m),
                  label);
            verifyTuple(m, wantGood);
        };
        VkSubpassDescription2 t = good;
        cycle("count>0 with NULL", 3, nullptr, good, VK_SAMPLE_COUNT_1_BIT); // newly explicit
        t = good;
        t.pColorAttachments = nullptr;
        cycle("nonzero colors with NULL", 2, atts.data(), t, VK_SAMPLE_COUNT_1_BIT);
        t = makeSubpass({7}, tRefs);
        cycle("color oob", 2, atts.data(), t, VK_SAMPLE_COUNT_1_BIT);
        {
            static VkAttachmentReference2 badRef = {VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
                                                    nullptr, 0,
                                                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0};
            t = makeSubpass({}, tRefs);
            t.colorAttachmentCount = 1;
            t.pColorAttachments = &badRef;
            cycle("color ref sType", 2, atts.data(), t, VK_SAMPLE_COUNT_1_BIT);
        }
        {
            static VkAttachmentReference2 badDs = {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr,
                                                   9, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0};
            t = makeSubpass({0}, tRefs);
            t.pDepthStencilAttachment = &badDs;
            cycle("ds oob", 2, atts.data(), t, VK_SAMPLE_COUNT_1_BIT);
        }
        {
            static VkAttachmentReference2 badDs = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                                   nullptr, 1,
                                                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0};
            t = makeSubpass({0}, tRefs);
            t.pDepthStencilAttachment = &badDs;
            cycle("ds ref sType", 2, atts.data(), t, VK_SAMPLE_COUNT_1_BIT);
        }
        {
            std::vector<VkAttachmentDescription2> badAtts = atts;
            badAtts[0].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            t = makeSubpass({0}, tRefs);
            cycle("color desc sType", 2, badAtts.data(), t, VK_SAMPLE_COUNT_1_BIT);
        }
        {
            std::vector<VkAttachmentDescription2> badAtts = atts;
            badAtts[1].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; // D/S desc wrong sType
            cycle("ds desc sType", 2, badAtts.data(), good, VK_SAMPLE_COUNT_1_BIT);
        }
        {
            std::vector<VkAttachmentDescription2> badAtts = atts;
            badAtts[0].format = VK_FORMAT_UNDEFINED;
            t = makeSubpass({0}, tRefs);
            cycle("color desc undefined", 2, badAtts.data(), t, VK_SAMPLE_COUNT_1_BIT);
        }
        {
            std::vector<VkAttachmentDescription2> badAtts = atts;
            badAtts[1].format = VK_FORMAT_B8G8R8A8_UNORM;
            cycle("ds non-ds format", 2, badAtts.data(), good, VK_SAMPLE_COUNT_1_BIT);
        }
        {
            std::vector<VkAttachmentDescription2> badAtts = atts;
            badAtts[1].format = VK_FORMAT_UNDEFINED;
            cycle("ds undefined", 2, badAtts.data(), good, VK_SAMPLE_COUNT_1_BIT);
        }
        {
            // attachmentless (count 0) with an actual color reference: reject
            std::vector<VkAttachmentReference2> refs;
            t = makeSubpass({0}, refs); // attachment 0 >= count 0
            cycle("attachmentless real color ref", 0, nullptr, t, VK_SAMPLE_COUNT_1_BIT);
        }
        {
            // attachmentless with an actual D/S reference: reject
            static VkAttachmentReference2 ds2 = {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr,
                                                 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0};
            t = makeSubpass({}, tRefs);
            t.pDepthStencilAttachment = &ds2;
            cycle("attachmentless real ds ref", 0, nullptr, t, VK_SAMPLE_COUNT_1_BIT);
        }
        {
            t = good;
            t.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE;
            cycle("subpass sType", 2, atts.data(), t, VK_SAMPLE_COUNT_1_BIT);
        }
        {
            t = good;
            t.pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
            cycle("bindpoint compute", 2, atts.data(), t, VK_SAMPLE_COUNT_1_BIT);
        }
        cycle("samples 0", 2, atts.data(), good, static_cast<VkSampleCountFlagBits>(0));
        cycle("samples combined", 2, atts.data(), good,
              static_cast<VkSampleCountFlagBits>(VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT));
        cycle("samples 0x80", 2, atts.data(), good, static_cast<VkSampleCountFlagBits>(0x80));
        check(rejections == 18, "rejection count reported");
        std::printf("rejection cases exercised: %u\n", rejections);
    }

    // --- 12: public empty default state ---
    {
        VKSubpassRenderingMetadata m;
        verifyEmpty(m);
    }

    if (g_failures == 0) {
        std::printf("render pass signature adapter tests: all PASS\n");
        return 0;
    }
    std::printf("render pass signature adapter tests: %u FAIL\n", g_failures);
    return 1;
}
#endif
