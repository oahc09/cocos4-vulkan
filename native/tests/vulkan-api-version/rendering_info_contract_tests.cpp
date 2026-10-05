// Composite VkRenderingInfo owning contract CPU tests (M5-J, rev 2).
// Run contract #18; probe compile-only. Generic input-driven verification;
// fresh/rebuild/growing allocation coverage; pure CPU (no real Vulkan calls).

#if defined(VKRINFO_STANDALONE_PROBE)
    #include "VKRenderingInfoContract.h"
int main() { return 0; }
#else

    #include <cstdio>
    #include <cstdlib>
    #include <cstring>
    #include <new>
    #include <type_traits>
    #include "VKRenderingInfoContract.h"

using cc::gfx::VKRenderingInfoContract;
using Input = VKRenderingInfoContract::Input;
using AttachmentInput = VKRenderingInfoContract::AttachmentInput;

// ---- global allocation counter + single-shot bad_alloc (test-only) ----
static long g_allocCount = 0;
static bool g_counting = false;
static long g_failNth = 0;

void *operator new(size_t size) {
    if (g_counting) {
        ++g_allocCount;
        if (g_failNth > 0 && g_allocCount >= g_failNth) {
            g_failNth = 0;
            throw std::bad_alloc();
        }
    }
    void *p = std::malloc(size ? size : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }

namespace {
unsigned g_failures = 0;
void check(bool ok, const char *w) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", w);
        ++g_failures;
    }
}
struct Guard {
    Guard() {
        g_allocCount = 0;
        g_counting = true;
    }
    ~Guard() {
        g_counting = false;
        g_failNth = 0;
    }
};

const VkImageView CV0 = reinterpret_cast<VkImageView>(static_cast<uintptr_t>(0xA001));
const VkImageView CV1 = reinterpret_cast<VkImageView>(static_cast<uintptr_t>(0xA002));
const VkImageView RV0 = reinterpret_cast<VkImageView>(static_cast<uintptr_t>(0xB001));
const VkImageView DSV = reinterpret_cast<VkImageView>(static_cast<uintptr_t>(0xC001));

VkAttachmentDescription makeDesc(VkFormat format, VkSampleCountFlagBits samples) {
    VkAttachmentDescription d = {};
    d.format = format;
    d.samples = samples;
    d.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    d.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    d.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    d.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    return d;
}
void setArea(Input &in) { in.renderArea = {{0, 0}, {64, 64}}; }

Input makeInput() {
    Input in;
    in.signature.colorFormats = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT};
    in.signature.depthFormat = VK_FORMAT_D24_UNORM_S8_UINT;
    in.signature.stencilFormat = VK_FORMAT_D24_UNORM_S8_UINT;
    in.signature.samples = VK_SAMPLE_COUNT_4_BIT;
    in.signature.viewMask = 0;
    in.colors.resize(2);
    in.colors[0].description = makeDesc(VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_4_BIT);
    in.colors[0].binding.imageView = CV0;
    in.colors[0].binding.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    in.colors[0].binding.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    in.colors[0].binding.resolveImageView = RV0;
    in.colors[0].binding.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    in.colors[0].clear.color.float32[0] = 1.5f;
    in.colors[1].description = makeDesc(VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_4_BIT);
    in.colors[1].description.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    in.colors[1].binding.imageView = CV1;
    in.colors[1].binding.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    in.colors[1].clear.color.float32[3] = 4.5f;
    in.depth = AttachmentInput{};
    in.depth->description = makeDesc(VK_FORMAT_D24_UNORM_S8_UINT, VK_SAMPLE_COUNT_4_BIT);
    in.depth->description.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    in.depth->binding.imageView = DSV;
    in.depth->binding.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    in.depth->binding.resolveMode = VK_RESOLVE_MODE_MIN_BIT;
    in.depth->binding.resolveImageView = RV0;
    in.depth->binding.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    in.depth->clear.depthStencil.depth = 0.75f;
    in.stencil = AttachmentInput{};
    in.stencil->description = makeDesc(VK_FORMAT_D24_UNORM_S8_UINT, VK_SAMPLE_COUNT_4_BIT);
    in.stencil->binding.imageView = DSV;
    in.stencil->binding.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    in.stencil->binding.resolveMode = VK_RESOLVE_MODE_MAX_BIT;
    in.stencil->binding.resolveImageView = RV0;
    in.stencil->binding.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    in.stencil->clear.depthStencil.stencil = 0x42u;
    in.renderArea = {{4, 8}, {320, 200}};
    in.layerCount = 2;
    in.flags = VK_RENDERING_SUSPENDING_BIT;
    return in;
}

// 16-byte clear comparison (explicitly initialized words, no struct padding)
bool clearEq(const VkClearValue &a, const VkClearValue &b) {
    return std::memcmp(&a, &b, sizeof(uint32_t) * 4) == 0;
}

// generic attachment-node verification driven by the input attachment
void verifyNode(const VkRenderingAttachmentInfo &n, const AttachmentInput &att, bool useStencilOps) {
    check(n.sType == VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, "vn: sType");
    check(n.pNext == nullptr, "vn: pNext null");
    check(n.imageView == att.binding.imageView, "vn: view");
    if (att.binding.imageView == VK_NULL_HANDLE) {
        // canonical unused node: layout normalized to UNDEFINED
        check(n.imageLayout == VK_IMAGE_LAYOUT_UNDEFINED, "vn: unused layout UNDEFINED");
        check(n.resolveMode == VK_RESOLVE_MODE_NONE, "vn: unused resolve NONE");
        check(n.resolveImageView == VK_NULL_HANDLE, "vn: unused resolve view null");
        check(n.resolveImageLayout == VK_IMAGE_LAYOUT_UNDEFINED, "vn: unused resolve layout");
        check(n.loadOp == VK_ATTACHMENT_LOAD_OP_DONT_CARE, "vn: unused load DC");
        check(n.storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE, "vn: unused store DC");
        const VkClearValue zero{};
        check(clearEq(n.clearValue, zero), "vn: unused clear zero");
        return;
    }
    check(n.imageLayout == att.binding.imageLayout, "vn: layout");
    check(n.loadOp == (useStencilOps ? att.description.stencilLoadOp : att.description.loadOp),
          "vn: loadOp");
    check(n.storeOp == (useStencilOps ? att.description.stencilStoreOp : att.description.storeOp),
          "vn: storeOp");
    check(clearEq(n.clearValue, att.clear), "vn: clear 16 bytes");
    if (att.binding.resolveMode == VK_RESOLVE_MODE_NONE) {
        check(n.resolveMode == VK_RESOLVE_MODE_NONE, "vn: resolve NONE");
        check(n.resolveImageView == VK_NULL_HANDLE, "vn: resolve view null");
        check(n.resolveImageLayout == VK_IMAGE_LAYOUT_UNDEFINED, "vn: resolve layout UNDEFINED");
    } else {
        check(n.resolveMode == att.binding.resolveMode, "vn: resolve mode");
        check(n.resolveImageView == att.binding.resolveImageView, "vn: resolve view");
        check(n.resolveImageLayout == att.binding.resolveImageLayout, "vn: resolve layout");
    }
}

// GENERIC full verification: every field derived from the expected Input
void verifyBuilt(const VKRenderingInfoContract &c, const Input &in) {
    check(c.renderingContract().ready(), "vb: owned rendering contract remains ready after publication");
    check(c.renderingContract().signature() == in.signature,
          "vb: owned rendering contract matches the published input signature");
    check(c.ready(), "vb: ready");
    const auto &r = c.info();
    check(r.sType == VK_STRUCTURE_TYPE_RENDERING_INFO, "vb: sType");
    check(r.pNext == nullptr, "vb: pNext null");
    check(r.flags == in.flags, "vb: flags");
    check(r.renderArea.offset.x == in.renderArea.offset.x &&
              r.renderArea.offset.y == in.renderArea.offset.y,
          "vb: area offset");
    check(r.renderArea.extent.width == in.renderArea.extent.width &&
              r.renderArea.extent.height == in.renderArea.extent.height,
          "vb: area extent");
    check(r.layerCount == in.layerCount, "vb: layers");
    check(r.viewMask == in.signature.viewMask, "vb: viewMask");
    // colors: guard count + pointer identity before reading
    const bool cGuard = r.colorAttachmentCount == c.colorInfos().size() &&
                        r.colorAttachmentCount == in.colors.size() &&
                        (in.colors.empty() ? r.pColorAttachments == nullptr
                                           : (r.pColorAttachments != nullptr &&
                                              r.pColorAttachments == c.colorInfos().data()));
    check(cGuard, "vb: color guard");
    if (cGuard) {
        for (size_t i = 0; i < in.colors.size(); ++i)
            verifyNode(c.colorInfos()[i], in.colors[i], false); // Color role
    }
    check(c.hasDepth() == in.depth.has_value(), "vb: depth presence");
    check((r.pDepthAttachment != nullptr) == in.depth.has_value(), "vb: depth ptr presence");
    if (in.depth.has_value()) {
        check(r.pDepthAttachment == &c.depthInfo(), "vb: depth ptr identity");
        verifyNode(c.depthInfo(), *in.depth, false); // Depth role: plain ops
    }
    check(c.hasStencil() == in.stencil.has_value(), "vb: stencil presence");
    check((r.pStencilAttachment != nullptr) == in.stencil.has_value(), "vb: stencil ptr presence");
    if (in.stencil.has_value()) {
        check(r.pStencilAttachment == &c.stencilInfo(), "vb: stencil ptr identity");
        verifyNode(c.stencilInfo(), *in.stencil, true); // Stencil role: stencil ops
    }
    // published input snapshot equals the expected input: full value compare
    check(c.input().colors.size() == in.colors.size(), "vb: snapshot colors size");
    check(c.input().signature == in.signature, "vb: snapshot signature");
    check(c.input().layerCount == in.layerCount && c.input().flags == in.flags, "vb: snapshot scalars");
    check(c.input().renderArea.offset.x == in.renderArea.offset.x &&
              c.input().renderArea.offset.y == in.renderArea.offset.y &&
              c.input().renderArea.extent.width == in.renderArea.extent.width &&
              c.input().renderArea.extent.height == in.renderArea.extent.height,
          "vb: snapshot area");
    if (c.input().colors.size() == in.colors.size()) {
        for (size_t i = 0; i < in.colors.size(); ++i) {
            const auto &got = c.input().colors[i];
            const auto &want = in.colors[i];
            const VkAttachmentDescription &gd = got.description;
            const VkAttachmentDescription &wd = want.description;
            check(gd.flags == wd.flags && gd.format == wd.format && gd.samples == wd.samples &&
                      gd.loadOp == wd.loadOp && gd.storeOp == wd.storeOp &&
                      gd.stencilLoadOp == wd.stencilLoadOp &&
                      gd.stencilStoreOp == wd.stencilStoreOp &&
                      gd.initialLayout == wd.initialLayout && gd.finalLayout == wd.finalLayout,
                  "vb: snapshot desc");
            const auto &gb = got.binding;
            const auto &wb = want.binding;
            check(gb.imageView == wb.imageView && gb.imageLayout == wb.imageLayout &&
                      gb.resolveMode == wb.resolveMode &&
                      gb.resolveImageView == wb.resolveImageView &&
                      gb.resolveImageLayout == wb.resolveImageLayout,
                  "vb: snapshot binding");
            check(clearEq(got.clear, want.clear), "vb: snapshot clear");
        }
    }
    check(c.input().depth.has_value() == in.depth.has_value(), "vb: snapshot depth presence");
    if (in.depth.has_value()) {
        check(c.input().depth->binding.imageView == in.depth->binding.imageView &&
                  c.input().depth->description.format == in.depth->description.format,
              "vb: snapshot depth content");
        check(clearEq(c.input().depth->clear, in.depth->clear), "vb: snapshot depth clear");
    }
    check(c.input().stencil.has_value() == in.stencil.has_value(), "vb: snapshot stencil presence");
    if (in.stencil.has_value()) {
        check(c.input().stencil->binding.imageView == in.stencil->binding.imageView &&
                  c.input().stencil->description.format == in.stencil->description.format,
              "vb: snapshot stencil content");
        check(clearEq(c.input().stencil->clear, in.stencil->clear), "vb: snapshot stencil clear");
    }
}

// full reset verification (root, nodes, Input defaults, retained contract)
void verifyReset(const VKRenderingInfoContract &c) {
    check(!c.ready(), "vrz: not ready");
    check(!c.hasDepth() && !c.hasStencil(), "vrz: no D/S");
    const auto &r = c.info();
    check(r.sType == VK_STRUCTURE_TYPE_RENDERING_INFO, "vrz: sType");
    check(r.pNext == nullptr, "vrz: pNext null");
    check(r.flags == 0 && r.layerCount == 0 && r.viewMask == 0, "vrz: root scalars 0");
    check(r.renderArea.offset.x == 0 && r.renderArea.offset.y == 0 &&
              r.renderArea.extent.width == 0 && r.renderArea.extent.height == 0,
          "vrz: area 0");
    check(r.colorAttachmentCount == 0 && r.pColorAttachments == nullptr, "vrz: colors 0");
    check(r.pDepthAttachment == nullptr && r.pStencilAttachment == nullptr, "vrz: D/S ptr null");
    check(c.colorInfos().empty(), "vrz: color vector empty");
    const VkClearValue zero{};
    for (const auto *n : {&c.depthInfo(), &c.stencilInfo()}) {
        check(n->sType == VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, "vrz: node sType");
        check(n->pNext == nullptr, "vrz: node pNext null");
        check(n->imageView == VK_NULL_HANDLE, "vrz: node view null");
        check(n->imageLayout == VK_IMAGE_LAYOUT_UNDEFINED, "vrz: node layout undefined");
        check(n->resolveMode == VK_RESOLVE_MODE_NONE, "vrz: node resolve NONE");
        check(n->resolveImageView == VK_NULL_HANDLE, "vrz: node resolve view null");
        check(n->resolveImageLayout == VK_IMAGE_LAYOUT_UNDEFINED, "vrz: node resolve layout");
        check(n->loadOp == VK_ATTACHMENT_LOAD_OP_DONT_CARE, "vrz: node load DC");
        check(n->storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE, "vrz: node store DC");
        check(clearEq(n->clearValue, zero), "vrz: node clear 0");
    }
    const Input &snapIn = c.input();
    check(snapIn.colors.empty() && !snapIn.depth.has_value() && !snapIn.stencil.has_value(),
          "vrz: input snapshot default");
    check(snapIn.layerCount == 1, "vrz: default Input layerCount is 1");
    check(snapIn.flags == 0, "vrz: default flags 0");
    check(snapIn.renderArea.extent.width == 0, "vrz: default area");
    check(snapIn.signature.colorFormats.empty(), "vrz: default signature colors");
    check(snapIn.signature.depthFormat == VK_FORMAT_UNDEFINED &&
              snapIn.signature.stencilFormat == VK_FORMAT_UNDEFINED,
          "vrz: default signature D/S");
    check(!c.renderingContract().ready(), "vrz: retained contract not ready");
    check(c.renderingContract().pipelineInfo().colorAttachmentCount == 0, "vrz: contract pipe 0");
    check(c.renderingContract().secondaryRenderingInfo().colorAttachmentCount == 0, "vrz: contract sec 0");
    check(c.renderingContract().baseInheritanceInfo().pNext == nullptr, "vrz: contract base pNext");
}
} // namespace

int main() {
    static_assert(!std::is_copy_constructible<VKRenderingInfoContract>::value &&
                      !std::is_copy_assignable<VKRenderingInfoContract>::value &&
                      !std::is_move_constructible<VKRenderingInfoContract>::value &&
                      !std::is_move_assignable<VKRenderingInfoContract>::value,
                  "non-copy/move");

    // --- 1: initial reset ---
    {
        VKRenderingInfoContract c;
        verifyReset(c);
    }

    // --- 2: full positive via generic helper ---
    {
        VKRenderingInfoContract c;
        Input in = makeInput();
        check(c.build(in), "full build");
        verifyBuilt(c, in);
    }

    // --- 3: color-only with UNUSED middle slot; ignored description ops ---
    {
        VKRenderingInfoContract c;
        Input in;
        in.signature.colorFormats = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_UNDEFINED, VK_FORMAT_R8_UNORM};
        in.signature.samples = VK_SAMPLE_COUNT_1_BIT;
        in.colors.resize(3);
        in.colors[0].description = makeDesc(VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_1_BIT);
        in.colors[0].binding.imageView = CV0;
        in.colors[1].description = makeDesc(VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_1_BIT);
        in.colors[1].description.loadOp = static_cast<VkAttachmentLoadOp>(99);  // ignored
        in.colors[1].description.samples = VK_SAMPLE_COUNT_8_BIT;               // ignored
        in.colors[1].description.storeOp = static_cast<VkAttachmentStoreOp>(7); // ignored
        in.colors[1].binding.imageLayout = VK_IMAGE_LAYOUT_GENERAL;             // ignored
        in.colors[1].binding.resolveImageView = RV0;                            // ignored stale
        in.colors[1].binding.resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;      // ignored stale
        in.colors[1].clear.color.uint32[0] = 0x11223344u;                       // kept in snapshot only
        in.colors[2].description = makeDesc(VK_FORMAT_R8_UNORM, VK_SAMPLE_COUNT_1_BIT);
        in.colors[2].binding.imageView = CV1;
        setArea(in);
        check(c.build(in), "unused slot build");
        verifyBuilt(c, in); // generic: 3 slots, middle canonical unused
    }

    // --- 4: attachmentless (viewMask non-zero, layerCount 0) ---
    {
        VKRenderingInfoContract c;
        Input in;
        in.signature.viewMask = 0x5;
        in.layerCount = 0;
        setArea(in);
        check(c.build(in), "attachmentless build");
        verifyBuilt(c, in);
    }

    // --- 5: depth-only / stencil-only (generic) ---
    {
        VKRenderingInfoContract c;
        Input in;
        in.signature.depthFormat = VK_FORMAT_D32_SFLOAT;
        in.signature.samples = VK_SAMPLE_COUNT_1_BIT;
        in.depth = AttachmentInput{};
        in.depth->description = makeDesc(VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT);
        in.depth->binding.imageView = DSV;
        setArea(in);
        check(c.build(in), "depth-only build");
        verifyBuilt(c, in);
        Input in2;
        in2.signature.stencilFormat = VK_FORMAT_S8_UINT;
        in2.signature.samples = VK_SAMPLE_COUNT_1_BIT;
        in2.stencil = AttachmentInput{};
        in2.stencil->description = makeDesc(VK_FORMAT_S8_UINT, VK_SAMPLE_COUNT_1_BIT);
        in2.stencil->binding.imageView = DSV;
        setArea(in2);
        check(c.build(in2), "stencil-only build");
        verifyBuilt(c, in2);
    }

    // --- 6: allocation failure coverage: fresh / rebuild / growing ---
    {
        const Input big = makeInput();
        Input small;
        small.signature.colorFormats = {VK_FORMAT_R8_UNORM};
        small.signature.samples = VK_SAMPLE_COUNT_1_BIT;
        small.colors.resize(1);
        small.colors[0].description = makeDesc(VK_FORMAT_R8_UNORM, VK_SAMPLE_COUNT_1_BIT);
        small.colors[0].binding.imageView = CV0;
        setArea(small);

        // (a) FRESH: every allocation point of a first build on a new owner
        long freshTotal = 0;
        {
            VKRenderingInfoContract c;
            Guard g;
            const long before = g_allocCount;
            check(c.build(big), "fresh probe");
            freshTotal = g_allocCount - before;
        }
        std::printf("fresh-build allocation points: %ld\n", freshTotal);
        for (long n = 1; n <= freshTotal; ++n) {
            VKRenderingInfoContract c; // fresh owner, no prime
            Guard g;
            g_failNth = n;
            bool threw = false;
            try {
                c.build(big);
            } catch (const std::bad_alloc &) {
                threw = true;
            }
            check(threw, "fresh alloc failure must propagate");
            if (threw) verifyReset(c);
        }

        // (b) REBUILD same shape: capacity may be reused - measure then cover
        long rebuildTotal = 0;
        {
            VKRenderingInfoContract c;
            Guard g;
            check(c.build(big), "rebuild prime");
            const long before = g_allocCount;
            check(c.build(big), "rebuild probe");
            rebuildTotal = g_allocCount - before;
        }
        std::printf("rebuild(same-shape) allocation points: %ld\n", rebuildTotal);
        for (long n = 1; n <= rebuildTotal; ++n) {
            VKRenderingInfoContract c;
            Guard g;
            check(c.build(big), "rebuild prime n");
            const long primeEnd = g_allocCount;
            g_failNth = primeEnd + n;
            bool threw = false;
            try {
                c.build(big);
            } catch (const std::bad_alloc &) {
                threw = true;
            }
            check(threw, "rebuild alloc failure must propagate");
            if (threw) verifyReset(c);
        }

        // (c) GROWING shape: small -> big (vector growth allocations)
        long growTotal = 0;
        {
            VKRenderingInfoContract c;
            Guard g;
            check(c.build(small), "grow prime");
            const long before = g_allocCount;
            check(c.build(big), "grow probe");
            growTotal = g_allocCount - before;
        }
        std::printf("grow(small->big) allocation points: %ld\n", growTotal);
        for (long n = 1; n <= growTotal; ++n) {
            VKRenderingInfoContract c;
            Guard g;
            check(c.build(small), "grow prime n");
            const long primeEnd = g_allocCount;
            g_failNth = primeEnd + n;
            bool threw = false;
            try {
                c.build(big);
            } catch (const std::bad_alloc &) {
                threw = true;
            }
            check(threw, "grow alloc failure must propagate");
            if (threw) verifyReset(c);
        }
    }

    // --- 7: rejections: prime full -> bad -> full reset -> recover ---
    {
        VKRenderingInfoContract c;
        const Input good = makeInput();
        unsigned rejections = 0;
        auto cycle = [&](const char *label, const Input &bad) {
            ++rejections;
            check(c.build(good), label);
            verifyBuilt(c, good);
            check(!c.build(bad), label);
            verifyReset(c);
            check(c.build(good), label);
            verifyBuilt(c, good);
        };
        {
            Input t = good;
            t.colors.resize(1);
            cycle("count mismatch", t);
        }
        {
            Input t = good;
            t.colors[0].description.format = VK_FORMAT_R8_UNORM;
            cycle("active format mismatch", t);
        }
        {
            Input t = good;
            t.colors[1].description.samples = VK_SAMPLE_COUNT_1_BIT;
            cycle("color sample mismatch", t);
        }
        {
            Input t = good;
            t.depth->description.samples = VK_SAMPLE_COUNT_1_BIT;
            cycle("depth description sample mismatch", t);
        }
        {
            Input t = good;
            t.stencil->description.format = VK_FORMAT_D32_SFLOAT;
            cycle("stencil description format mismatch", t);
        }
        {
            Input t = good;
            t.depth->binding.imageView = VK_NULL_HANDLE;
            cycle("null active depth view", t);
        }
        {
            Input t = good;
            t.depth.reset();
            cycle("missing depth", t);
        }
        {
            Input t = good;
            t.stencil.reset();
            cycle("missing stencil", t);
        }
        { // optional present although the signature slot is UNDEFINED
            Input base = good;
            base.signature.depthFormat = VK_FORMAT_UNDEFINED;
            base.depth.reset(); // valid no-depth base; then re-add the optional
            Input t = base;
            t.depth = good.depth; // only this optional is the change
            cycle("optional present when UNDEFINED", t);
        }
        {
            Input t = good;
            t.signature.colorFormats[1] = VK_FORMAT_UNDEFINED;
            // colors[1] stays ACTIVE (CV1) while the signature slot says UNDEFINED
            cycle("active color but sig UNDEFINED", t);
        }
        {
            Input t = good;
            t.colors[0].binding.imageView = VK_NULL_HANDLE;
            t.colors[0].binding.resolveMode = VK_RESOLVE_MODE_NONE;
            t.colors[0].binding.resolveImageView = VK_NULL_HANDLE;
            cycle("unused color but sig non-UNDEFINED", t);
        }
        { // UNUSED slot with non-NONE resolve: single-attachment model rejects
            Input base = good;
            base.signature.colorFormats[0] = VK_FORMAT_UNDEFINED;
            base.colors[0].binding.imageView = VK_NULL_HANDLE;
            base.colors[0].binding.resolveMode = VK_RESOLVE_MODE_NONE;
            base.colors[0].binding.resolveImageView = RV0; // ignored stale value
            base.colors[0].binding.resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
            { // legal base first: UNUSED + NONE
                VKRenderingInfoContract legal;
                check(legal.build(base), "unused+NONE base is legal");
                verifyBuilt(legal, base); // output canonical; snapshot keeps stale values
            }
            Input t = base;
            t.colors[0].binding.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT; // the ONLY change
            cycle("unused slot with non-NONE resolve", t);
        }
        {
            Input t = good;
            t.colors[0].binding.resolveImageView = VK_NULL_HANDLE;
            cycle("missing resolve view", t);
        }
        { // one logical change: sig+all descriptions move to 1x samples so the
            // resolve path with samples==1 reaches the model rejection
            Input t = good;
            t.signature.samples = VK_SAMPLE_COUNT_1_BIT;
            for (auto &cIn : t.colors) cIn.description.samples = VK_SAMPLE_COUNT_1_BIT;
            t.depth->description.samples = VK_SAMPLE_COUNT_1_BIT;
            t.stencil->description.samples = VK_SAMPLE_COUNT_1_BIT;
            cycle("resolve with samples 1", t);
        }
        {
            Input t = good;
            t.stencil->binding.imageView = CV1;
            cycle("D/S different imageView", t);
        }
        {
            Input t = good;
            t.stencil->binding.resolveImageView = CV1;
            cycle("two resolves different view", t);
        }
        {
            Input t = good;
            t.flags = static_cast<VkRenderingFlags>(0x80);
            cycle("unknown flag bit", t);
        }
        {
            Input t = good;
            t.renderArea.offset.x = -1;
            cycle("area offset.x negative", t);
        }
        {
            Input t = good;
            t.renderArea.offset.y = -1;
            cycle("area offset.y negative", t);
        }
        {
            Input t = good;
            t.renderArea.extent.width = 0;
            cycle("area width 0", t);
        }
        {
            Input t = good;
            t.renderArea.extent.height = 0;
            cycle("area height 0", t);
        }
        {
            Input t = good;
            t.layerCount = 0;
            cycle("layer 0 with viewMask 0", t);
        }
        {
            Input t = good;
            t.signature.samples = static_cast<VkSampleCountFlagBits>(0);
            cycle("signature samples 0", t);
        }
        {
            Input t = good;
            t.signature.samples = static_cast<VkSampleCountFlagBits>(VK_SAMPLE_COUNT_1_BIT |
                                                                     VK_SAMPLE_COUNT_2_BIT);
            cycle("signature samples combined", t);
        }
        {
            Input t = good;
            t.signature.depthFormat = VK_FORMAT_D32_SFLOAT;
            t.depth->description.format = VK_FORMAT_D32_SFLOAT;
            cycle("signature D-S format mismatch", t);
        }
        std::printf("rejection cases exercised: %u\n", rejections);
    }

    // --- 8: owning: snapshot non-borrow proof + full mutation + death ---
    {
        VKRenderingInfoContract c;
        bool guard = false;
        {
            Input in = makeInput();
            check(c.build(in), "owning build");
            // the INPUT SNAPSHOT must not borrow caller vectors either
            guard = c.input().colors.size() == in.colors.size() &&
                    c.input().signature.colorFormats.size() ==
                        in.signature.colorFormats.size() &&
                    c.input().colors.data() != in.colors.data() &&
                    c.input().signature.colorFormats.data() !=
                        in.signature.colorFormats.data() &&
                    c.info().pColorAttachments == c.colorInfos().data();
            check(guard, "owning: snapshot + output arrays not borrowed");
            if (guard) {
                // mutate EVERY caller field family
                in.signature.samples = VK_SAMPLE_COUNT_8_BIT;
                in.signature.viewMask = 0x3;
                in.colors.push_back(AttachmentInput{});                  // realloc
                in.signature.colorFormats.push_back(VK_FORMAT_R8_UNORM); // realloc
                in.colors[0].clear.color.float32[0] = 9.9f;
                in.colors[0].binding.imageView = CV1;
                in.colors[0].description.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                in.depth->clear.depthStencil.depth = 0.5f;
                in.stencil->binding.resolveMode = VK_RESOLVE_MODE_MIN_BIT;
                in.renderArea.extent.width = 1;
                in.flags = 0;
            }
        } // caller inputs destroyed
        if (guard) {
            verifyBuilt(c, makeInput()); // full read after destruction
        } else {
            check(false, "owning: guard failed");
        }
    }

    // --- 9: two owners + rebuild sequence, all generic ---
    {
        VKRenderingInfoContract a, b;
        Input in = makeInput();
        check(a.build(in) && b.build(in), "two owners");
        check(a.colorInfos().data() != b.colorInfos().data(), "two: distinct arrays");
        check(a.input().colors.data() != b.input().colors.data(), "two: distinct snapshots");
        Input small;
        small.signature.colorFormats = {VK_FORMAT_R8_UNORM};
        small.signature.samples = VK_SAMPLE_COUNT_1_BIT;
        small.colors.resize(1);
        small.colors[0].description = makeDesc(VK_FORMAT_R8_UNORM, VK_SAMPLE_COUNT_1_BIT);
        small.colors[0].binding.imageView = CV0;
        setArea(small);
        Input empty;
        empty.signature.viewMask = 0x3; // layerCount 0 legal
        empty.layerCount = 0;
        setArea(empty);
        check(a.build(small), "rebuild small");
        verifyBuilt(a, small);
        check(a.build(empty), "rebuild attachmentless");
        verifyBuilt(a, empty);
        Input bad = makeInput();
        bad.layerCount = 0; // viewMask 0 -> reject
        check(!a.build(bad), "rebuild invalid");
        verifyReset(a);
        check(a.build(in), "rebuild recover");
        verifyBuilt(a, in);
        verifyBuilt(b, in); // b fully intact throughout
    }

    // --- 10: self-input alias: build(c.input()) ---
    {
        VKRenderingInfoContract c;
        Input in = makeInput();
        check(c.build(in), "self prime");
        check(c.build(c.input()), "self rebuild");
        verifyBuilt(c, in);
    }

    // --- 11: public reset() ---
    {
        VKRenderingInfoContract c;
        Input in = makeInput();
        check(c.build(in), "reset prime");
        verifyBuilt(c, in);
        c.reset();
        verifyReset(c);
    }

    // --- 12: NONE resolve canonicalizes stale binding values (via owned ref) ---
    {
        VKRenderingInfoContract c;
        Input in;
        in.signature.colorFormats = {VK_FORMAT_B8G8R8A8_UNORM};
        in.signature.samples = VK_SAMPLE_COUNT_1_BIT;
        in.colors.resize(1);
        in.colors[0].description = makeDesc(VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_1_BIT);
        in.colors[0].binding.imageView = CV0;
        in.colors[0].binding.resolveMode = VK_RESOLVE_MODE_NONE;
        in.colors[0].binding.resolveImageView = RV0; // stale, normalized away
        in.colors[0].binding.resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
        setArea(in);
        check(c.build(in), "stale resolve build");
        verifyBuilt(c, in); // generic: resolve NONE => NULL/UNDEFINED expected
        check(!c.colorInfos().empty() && c.colorInfos()[0].resolveImageView == VK_NULL_HANDLE,
              "stale: owned node normalized");
    }

    if (g_failures == 0) {
        std::printf("rendering info contract tests: all PASS\n");
        return 0;
    }
    std::printf("rendering info contract tests: %u FAIL\n", g_failures);
    return 1;
}
#endif
