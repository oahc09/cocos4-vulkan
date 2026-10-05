// Single-attachment Load/Store/Resolve mapping CPU contract tests (M5-E).
// Reads go through the owned info() reference; no Vulkan calls anywhere.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include "VKRenderingAttachmentContract.h"

using cc::gfx::VKRenderingAttachmentContract;
using Role = VKRenderingAttachmentContract::Role;
using Binding = VKRenderingAttachmentContract::Binding;

namespace {
unsigned g_failures = 0;
void check(bool ok, const char *w) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", w);
        ++g_failures;
    }
}

const VkImageView VIEW_A = reinterpret_cast<VkImageView>(static_cast<uintptr_t>(0xA111));
const VkImageView VIEW_R = reinterpret_cast<VkImageView>(static_cast<uintptr_t>(0xB222));
const VkImageView VIEW_X = reinterpret_cast<VkImageView>(static_cast<uintptr_t>(0xC333));

VkAttachmentDescription makeDesc() {
    VkAttachmentDescription d = {};
    d.format = VK_FORMAT_B8G8R8A8_UNORM;
    d.samples = VK_SAMPLE_COUNT_4_BIT;
    d.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;              // color/depth selected
    d.storeOp = VK_ATTACHMENT_STORE_OP_STORE;            // color/depth selected
    d.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;        // stencil selected
    d.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; // stencil selected
    d.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    d.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return d;
}
Binding makeBindingActive() {
    Binding b;
    b.imageView = VIEW_A;
    b.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    return b;
}
VkClearValue makeClearColor() {
    VkClearValue v = {};
    v.color.float32[0] = 1.5f;
    v.color.float32[1] = 2.5f;
    v.color.float32[2] = 3.5f;
    v.color.float32[3] = 4.5f;
    return v;
}
VkClearValue makeClearDepthStencil() {
    VkClearValue v = {};
    v.depthStencil.depth = 0.75f;
    v.depthStencil.stencil = 0x42u;
    return v;
}

// full active-attachment verification through owned reference
void verifyActive(const VKRenderingAttachmentContract &c, VkImageView view, VkImageLayout layout,
                  VkAttachmentLoadOp wantLoad, VkAttachmentStoreOp wantStore) {
    check(c.ready(), "va: ready");
    const auto &i = c.info();
    check(i.sType == VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, "va: sType");
    check(i.pNext == nullptr, "va: pNext null");
    check(i.imageView == view, "va: view");
    check(i.imageLayout == layout, "va: layout");
    check(i.loadOp == wantLoad, "va: loadOp");
    check(i.storeOp == wantStore, "va: storeOp");
}
void verifyClearColor(const VKRenderingAttachmentContract &c) {
    const auto &i = c.info();
    check(i.clearValue.color.float32[0] == 1.5f && i.clearValue.color.float32[1] == 2.5f &&
              i.clearValue.color.float32[2] == 3.5f && i.clearValue.color.float32[3] == 4.5f,
          "va: clear color 4 elems");
}
void verifyResolve(const VKRenderingAttachmentContract &c, VkResolveModeFlagBits mode) {
    const auto &i = c.info();
    check(i.resolveMode == mode, "va: resolve mode");
    check(i.resolveImageView == VIEW_R, "va: resolve view");
    check(i.resolveImageLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, "va: resolve layout");
}
void verifyNoResolve(const VKRenderingAttachmentContract &c) {
    const auto &i = c.info();
    check(i.resolveMode == VK_RESOLVE_MODE_NONE, "vn: resolve NONE");
    check(i.resolveImageView == VK_NULL_HANDLE, "vn: resolve view null");
    check(i.resolveImageLayout == VK_IMAGE_LAYOUT_UNDEFINED, "vn: resolve layout undefined");
}
// fully cleared / canonical empty
void verifyEmpty(const VKRenderingAttachmentContract &c) {
    check(!c.ready(), "ve: not ready");
    const auto &i = c.info();
    check(i.sType == VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, "ve: sType");
    check(i.pNext == nullptr, "ve: pNext null");
    check(i.imageView == VK_NULL_HANDLE, "ve: view null");
    check(i.imageLayout == VK_IMAGE_LAYOUT_UNDEFINED, "ve: layout undefined");
    check(i.resolveMode == VK_RESOLVE_MODE_NONE, "ve: resolve NONE");
    check(i.resolveImageView == VK_NULL_HANDLE, "ve: resolve view null");
    check(i.resolveImageLayout == VK_IMAGE_LAYOUT_UNDEFINED, "ve: resolve layout undefined");
    check(i.loadOp == VK_ATTACHMENT_LOAD_OP_DONT_CARE, "ve: load DONT_CARE");
    check(i.storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE, "ve: store DONT_CARE");
    check(i.clearValue.color.float32[0] == 0.0f && i.clearValue.color.float32[3] == 0.0f &&
              i.clearValue.depthStencil.depth == 0.0f && i.clearValue.depthStencil.stencil == 0,
          "ve: clear zero");
    const VkClearValue zero{};
    check(std::memcmp(&i.clearValue, &zero, sizeof(zero)) == 0, "ve: all clear bytes zero");
}
// canonical unused (successfully published empty attachment)
void verifyUnused(const VKRenderingAttachmentContract &c) {
    check(c.ready(), "vu: ready");
    const auto &i = c.info();
    check(i.sType == VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, "vu: sType");
    check(i.pNext == nullptr, "vu: pNext null");
    check(i.imageView == VK_NULL_HANDLE, "vu: view null");
    check(i.imageLayout == VK_IMAGE_LAYOUT_UNDEFINED, "vu: layout undefined");
    verifyNoResolve(c);
    check(i.loadOp == VK_ATTACHMENT_LOAD_OP_DONT_CARE, "vu: load DONT_CARE");
    check(i.storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE, "vu: store DONT_CARE");
    check(i.clearValue.color.float32[0] == 0.0f, "vu: clear zero");
    const VkClearValue zero{};
    check(std::memcmp(&i.clearValue, &zero, sizeof(zero)) == 0, "vu: all clear bytes zero");
}
} // namespace

int main() {
    static_assert(!std::is_copy_constructible<VKRenderingAttachmentContract>::value &&
                      !std::is_copy_assignable<VKRenderingAttachmentContract>::value &&
                      !std::is_move_constructible<VKRenderingAttachmentContract>::value &&
                      !std::is_move_assignable<VKRenderingAttachmentContract>::value,
                  "non-copy/move");

    // --- 1: initial empty ---
    {
        VKRenderingAttachmentContract c;
        verifyEmpty(c);
    }

    // --- 2: color basic + asymmetric 4-element clear ---
    {
        VKRenderingAttachmentContract c;
        auto d = makeDesc();
        auto b = makeBindingActive();
        auto cl = makeClearColor();
        check(c.build(Role::Color, d, b, cl), "color build");
        verifyActive(c, VIEW_A, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE);
        verifyClearColor(c);
        verifyNoResolve(c);
    }

    // --- 3: depth role maps loadOp/storeOp; stencil ops irrelevant ---
    {
        VKRenderingAttachmentContract c;
        auto d = makeDesc();
        auto b = makeBindingActive();
        d.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        d.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        d.stencilLoadOp = static_cast<VkAttachmentLoadOp>(77); // garbage, must not matter
        d.stencilStoreOp = static_cast<VkAttachmentStoreOp>(77);
        auto cl = makeClearDepthStencil();
        b.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        check(c.build(Role::Depth, d, b, cl), "depth build");
        verifyActive(c, VIEW_A, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                     VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE);
        check(c.info().clearValue.depthStencil.depth == 0.75f, "depth: clear depth");
        check(c.info().clearValue.depthStencil.stencil == 0x42u, "depth: clear stencil");
        verifyNoResolve(c);
    }

    // --- 4: stencil role maps stencilLoadOp/stencilStoreOp ---
    {
        VKRenderingAttachmentContract c;
        auto d = makeDesc();
        auto b = makeBindingActive();
        auto cl = makeClearDepthStencil();
        d.loadOp = static_cast<VkAttachmentLoadOp>(77); // unselected depth ops
        d.storeOp = static_cast<VkAttachmentStoreOp>(77);
        b.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        check(c.build(Role::Stencil, d, b, cl), "stencil build");
        verifyActive(c, VIEW_A, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                     VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_DONT_CARE);
        check(c.info().clearValue.depthStencil.depth == 0.75f, "stencil: clear depth kept");
        check(c.info().clearValue.depthStencil.stencil == 0x42u, "stencil: clear stencil kept");
        verifyNoResolve(c);
    }

    // --- 5: caller mutation after build leaves output unchanged ---
    {
        VKRenderingAttachmentContract c;
        auto d = makeDesc();
        auto b = makeBindingActive();
        auto cl = makeClearColor();
        check(c.build(Role::Color, d, b, cl), "mut build");
        d.loadOp = static_cast<VkAttachmentLoadOp>(99);
        d.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.imageView = VIEW_X;
        b.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        cl.color.float32[0] = 9.9f;
        verifyActive(c, VIEW_A, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE);
        verifyClearColor(c);
        verifyNoResolve(c);
    }

    // --- 6: self-clear alias: rebuild using own published clearValue ---
    {
        VKRenderingAttachmentContract c;
        auto d = makeDesc();
        auto b = makeBindingActive();
        auto cl = makeClearColor();
        check(c.build(Role::Color, d, b, cl), "alias init");
        check(c.build(Role::Color, d, b, c.info().clearValue), "alias rebuild");
        verifyActive(c, VIEW_A, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE);
        verifyClearColor(c);
    }

    // --- 7: resolve four modes passthrough ---
    {
        VKRenderingAttachmentContract c;
        auto d = makeDesc();
        auto cl = makeClearColor();
        const VkResolveModeFlagBits modes[] = {VK_RESOLVE_MODE_SAMPLE_ZERO_BIT,
                                               VK_RESOLVE_MODE_AVERAGE_BIT,
                                               VK_RESOLVE_MODE_MIN_BIT,
                                               VK_RESOLVE_MODE_MAX_BIT};
        for (VkResolveModeFlagBits m : modes) {
            Binding b = makeBindingActive();
            b.resolveMode = m;
            b.resolveImageView = VIEW_R;
            b.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            check(c.build(Role::Color, d, b, cl), "resolve mode build");
            verifyActive(c, VIEW_A, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE);
            verifyResolve(c, m);
            verifyClearColor(c);
        }
    }

    // --- 8: NONE normalizes stale resolve values in binding ---
    {
        VKRenderingAttachmentContract c;
        auto d = makeDesc();
        auto cl = makeClearColor();
        Binding b = makeBindingActive();
        b.resolveMode = VK_RESOLVE_MODE_NONE;
        b.resolveImageView = VIEW_R; // stale, must be normalized away
        b.resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
        check(c.build(Role::Color, d, b, cl), "none normalize");
        verifyActive(c, VIEW_A, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE);
        verifyNoResolve(c);
    }

    // --- 9: same view depth+stencil instances, resolve and ops independent ---
    {
        VKRenderingAttachmentContract depth, stencil;
        auto d = makeDesc();
        auto cl = makeClearDepthStencil();
        Binding bd = makeBindingActive();
        bd.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        bd.resolveMode = VK_RESOLVE_MODE_MAX_BIT;
        bd.resolveImageView = VIEW_R;
        bd.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        check(depth.build(Role::Depth, d, bd, cl), "ds depth");
        Binding bs = makeBindingActive(); // no resolve
        bs.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        check(stencil.build(Role::Stencil, d, bs, cl), "ds stencil");
        verifyActive(depth, VIEW_A, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                     VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE);
        verifyActive(stencil, VIEW_A, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                     VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_DONT_CARE);
        check(depth.info().resolveMode == VK_RESOLVE_MODE_MAX_BIT, "ds depth resolve");
        check(depth.info().resolveImageView == VIEW_R, "ds depth resolve view");
        check(depth.info().resolveImageLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
              "ds depth resolve layout");
        verifyNoResolve(stencil);
        check(depth.info().loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR, "ds depth load");
        check(stencil.info().loadOp == VK_ATTACHMENT_LOAD_OP_LOAD, "ds stencil load");
        check(depth.info().clearValue.depthStencil.depth == 0.75f &&
                  depth.info().clearValue.depthStencil.stencil == 0x42u &&
                  stencil.info().clearValue.depthStencil.depth == 0.75f &&
                  stencil.info().clearValue.depthStencil.stencil == 0x42u,
              "ds independent clear values");
    }

    // --- 10: DONT_CARE store still carries resolve ---
    {
        VKRenderingAttachmentContract c;
        auto d = makeDesc();
        d.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        auto cl = makeClearColor();
        Binding b = makeBindingActive();
        b.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        b.resolveImageView = VIEW_R;
        b.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        check(c.build(Role::Color, d, b, cl), "dc store resolve");
        check(c.info().storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE, "dc: store DONT_CARE");
        verifyActive(c, VIEW_A, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE);
        verifyClearColor(c);
        verifyResolve(c, VK_RESOLVE_MODE_AVERAGE_BIT);
    }

    // --- 11: initial/final layout changes never touch in-render layout ---
    {
        VKRenderingAttachmentContract c;
        auto d = makeDesc();
        d.initialLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        d.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        auto b = makeBindingActive();
        auto cl = makeClearColor();
        b.imageLayout = VK_IMAGE_LAYOUT_GENERAL; // offscreen GENERAL preserved
        check(c.build(Role::Color, d, b, cl), "layout build");
        verifyActive(c, VIEW_A, VK_IMAGE_LAYOUT_GENERAL,
                     VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE);
        verifyClearColor(c);
        verifyNoResolve(c);
    }

    // --- 12: unused imageView NULL + NONE succeeds canonical, ignores ops ---
    {
        VKRenderingAttachmentContract c;
        auto d = makeDesc();
        d.loadOp = static_cast<VkAttachmentLoadOp>(88); // ignored for unused
        d.storeOp = static_cast<VkAttachmentStoreOp>(88);
        d.stencilLoadOp = static_cast<VkAttachmentLoadOp>(88);
        d.stencilStoreOp = static_cast<VkAttachmentStoreOp>(88);
        auto cl = makeClearColor();
        Binding b; // all defaults: view NULL, mode NONE
        check(c.build(Role::Color, d, b, cl), "unused build");
        verifyUnused(c);
    }

    // --- 13: rebuild cycle resolved -> NONE -> unused -> invalid -> recover ---
    {
        VKRenderingAttachmentContract c;
        auto d = makeDesc();
        auto cl = makeClearColor();
        Binding br = makeBindingActive();
        br.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        br.resolveImageView = VIEW_R;
        br.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        check(c.build(Role::Color, d, br, cl), "cyc resolved");
        verifyClearColor(c);
        verifyActive(c, VIEW_A, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE);
        verifyResolve(c, VK_RESOLVE_MODE_SAMPLE_ZERO_BIT);
        check(c.build(Role::Color, d, makeBindingActive(), cl), "cyc none");
        verifyActive(c, VIEW_A, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE);
        verifyClearColor(c);
        verifyNoResolve(c);
        check(c.build(Role::Color, d, Binding{}, cl), "cyc unused");
        verifyUnused(c);
        auto bad = makeDesc();
        bad.samples = static_cast<VkSampleCountFlagBits>(0);
        Binding bb = makeBindingActive();
        bb.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        bb.resolveImageView = VIEW_R;
        check(!c.build(Role::Color, bad, bb, cl), "cyc invalid");
        verifyEmpty(c);
        check(c.build(Role::Color, d, br, cl), "cyc recover");
        verifyActive(c, VIEW_A, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE);
        verifyClearColor(c);
        verifyResolve(c, VK_RESOLVE_MODE_SAMPLE_ZERO_BIT);
    }

    // --- 14: every rejection: prime resolved -> bad -> verifyEmpty -> recover ---
    {
        VKRenderingAttachmentContract c;
        auto d = makeDesc();
        auto cl = makeClearColor();
        Binding prime = makeBindingActive();
        prime.resolveMode = VK_RESOLVE_MODE_MIN_BIT;
        prime.resolveImageView = VIEW_R;
        prime.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        auto cycle = [&](const char *label, Role role, VkAttachmentDescription dd, Binding bb) {
            check(c.build(Role::Color, d, prime, cl), label);
            verifyActive(c, VIEW_A, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE);
            verifyResolve(c, VK_RESOLVE_MODE_MIN_BIT);
            verifyClearColor(c);
            check(!c.build(role, dd, bb, cl), label);
            verifyEmpty(c);
            check(c.build(Role::Color, d, prime, cl), label);
            verifyActive(c, VIEW_A, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE);
            verifyClearColor(c);
            verifyResolve(c, VK_RESOLVE_MODE_MIN_BIT);
        };
        // unknown role
        cycle("role unknown", static_cast<Role>(99), d, prime);
        // selected load op unknown (color)
        auto t = d;
        t.loadOp = static_cast<VkAttachmentLoadOp>(99);
        cycle("load unknown color", Role::Color, t, prime);
        // selected load op unknown (stencil reads stencilLoadOp)
        t = d;
        t.stencilLoadOp = static_cast<VkAttachmentLoadOp>(99);
        cycle("load unknown stencil", Role::Stencil, t, prime);
        // selected store op unknown
        t = d;
        t.storeOp = static_cast<VkAttachmentStoreOp>(7);
        cycle("store unknown", Role::Depth, t, prime);
        // selected stencil store unknown
        t = d;
        t.stencilStoreOp = static_cast<VkAttachmentStoreOp>(7);
        cycle("stencil store unknown", Role::Stencil, t, prime);
        // unknown resolve mode
        auto bp = prime;
        bp.resolveMode = static_cast<VkResolveModeFlagBits>(0x40);
        cycle("mode unknown", Role::Color, d, bp);
        // combined resolve mode
        bp = prime;
        bp.resolveMode = static_cast<VkResolveModeFlagBits>(VK_RESOLVE_MODE_AVERAGE_BIT |
                                                            VK_RESOLVE_MODE_MIN_BIT);
        cycle("mode combined", Role::Color, d, bp);
        // non-NONE missing resolve view
        bp = prime;
        bp.resolveImageView = VK_NULL_HANDLE;
        cycle("missing resolve view", Role::Color, d, bp);
        // samples 0 with resolve
        t = d;
        t.samples = static_cast<VkSampleCountFlagBits>(0);
        cycle("samples 0", Role::Color, t, prime);
        // combined samples with resolve
        t = d;
        t.samples = static_cast<VkSampleCountFlagBits>(VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT);
        cycle("samples combined", Role::Color, t, prime);
        // samples 1 with resolve
        t = d;
        t.samples = VK_SAMPLE_COUNT_1_BIT;
        cycle("samples 1", Role::Color, t, prime);
        // unused view + non-NONE resolve
        bp = prime;
        bp.imageView = VK_NULL_HANDLE;
        cycle("unused non-NONE", Role::Color, d, bp);
    }

    // --- 15: public reset() ---
    {
        VKRenderingAttachmentContract c;
        auto d = makeDesc();
        auto b = makeBindingActive();
        auto cl = makeClearColor();
        check(c.build(Role::Color, d, b, cl), "reset prime");
        c.reset();
        verifyEmpty(c);
    }

    if (g_failures == 0) {
        std::printf("rendering attachment tests: all PASS\n");
        return 0;
    }
    std::printf("rendering attachment tests: %u FAIL\n", g_failures);
    return 1;
}
