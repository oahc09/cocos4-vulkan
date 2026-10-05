// Secondary CB begin/inheritance adapter CPU tests (M5-I, rev 2).
// Run contract #17; probe variant compile-only. Includes allocation
// red/green contracts: Classic must not copy the signature, and every
// allocation failure inside Dynamic must leave the adapter fully reset.

#if defined(VKSECINH_STANDALONE_PROBE)
    #include "VKSecondaryInheritanceAdapter.h"
int main() { return 0; }
#else

    #include <cstdio>
    #include <cstdlib>
    #include <new>
    #include <type_traits>
    #include "VKSecondaryInheritanceAdapter.h"

using cc::gfx::VKRenderingSignature;
using cc::gfx::VKSecondaryInheritanceAdapter;
using Mode = VKSecondaryInheritanceAdapter::Mode;

// ---- global allocation counter + single-shot bad_alloc (test-only) ----
static long g_allocCount = 0;
static bool g_counting = false;
static long g_failNth = 0; // 0 = off; fail when the counter reaches this

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

const VkRenderPass RP = reinterpret_cast<VkRenderPass>(static_cast<uintptr_t>(0x9100));
const VkFramebuffer FBO = reinterpret_cast<VkFramebuffer>(static_cast<uintptr_t>(0x9200));

VKRenderingSignature makeSig() {
    VKRenderingSignature s;
    s.colorFormats = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT};
    s.depthFormat = VK_FORMAT_D24_UNORM_S8_UINT;
    s.stencilFormat = VK_FORMAT_D24_UNORM_S8_UINT;
    s.samples = VK_SAMPLE_COUNT_2_BIT;
    s.viewMask = 0x3;
    return s;
}
VkCommandBufferBeginInfo makeBegin(VkCommandBufferInheritanceInfo &base) {
    VkCommandBufferBeginInfo b = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    b.pInheritanceInfo = &base;
    return b;
}
VkCommandBufferInheritanceInfo makeBase() {
    VkCommandBufferInheritanceInfo i = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
    i.renderPass = RP;
    i.subpass = 5;
    i.framebuffer = FBO;
    i.occlusionQueryEnable = VK_TRUE;
    i.queryFlags = VK_QUERY_CONTROL_PRECISE_BIT;
    i.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
    return i;
}

// full reset verification: every field of every owned node
void verifyReset(const VKSecondaryInheritanceAdapter &a) {
    check(!a.ready(), "vr: not ready");
    const auto &b = a.beginInfo();
    check(b.sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, "vr: begin sType");
    check(b.pNext == nullptr, "vr: begin pNext null");
    check(b.pInheritanceInfo == nullptr, "vr: begin inh ptr null");
    check(b.flags == 0, "vr: begin flags 0");
    const auto &i = a.inheritanceInfo();
    check(i.sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, "vr: inh sType");
    check(i.pNext == nullptr, "vr: inh pNext null");
    check(i.renderPass == VK_NULL_HANDLE, "vr: inh RP null");
    check(i.framebuffer == VK_NULL_HANDLE, "vr: inh FBO null");
    check(i.subpass == 0, "vr: inh subpass 0");
    check(i.occlusionQueryEnable == 0, "vr: inh occlusion 0");
    check(i.queryFlags == 0, "vr: inh queryFlags 0");
    check(i.pipelineStatistics == 0, "vr: inh stats 0");
    const auto &ct = a.rendering();
    check(!ct.ready(), "vr: contract not ready");
    const auto &pi = ct.pipelineInfo();
    check(pi.sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, "vr: pipe sType");
    check(pi.colorAttachmentCount == 0, "vr: pipe count 0");
    check(pi.pColorAttachmentFormats == nullptr, "vr: pipe ptr null");
    check(pi.viewMask == 0, "vr: pipe viewMask 0");
    check(pi.depthAttachmentFormat == VK_FORMAT_UNDEFINED, "vr: pipe depth");
    check(pi.stencilAttachmentFormat == VK_FORMAT_UNDEFINED, "vr: pipe stencil");
    check(pi.pNext == nullptr, "vr: pipe pNext null");
    const auto &sec = ct.secondaryRenderingInfo();
    check(sec.sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO, "vr: sec sType");
    check(sec.colorAttachmentCount == 0, "vr: sec count 0");
    check(sec.pColorAttachmentFormats == nullptr, "vr: sec ptr null");
    check(sec.viewMask == 0, "vr: sec viewMask 0");
    check(sec.depthAttachmentFormat == VK_FORMAT_UNDEFINED, "vr: sec depth");
    check(sec.stencilAttachmentFormat == VK_FORMAT_UNDEFINED, "vr: sec stencil");
    check(sec.rasterizationSamples == 0, "vr: sec samples 0 (node zeroed)");
    check(sec.flags == 0, "vr: sec flags 0");
    check(sec.pNext == nullptr, "vr: sec pNext null");
    const auto &bi = ct.baseInheritanceInfo();
    check(bi.sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, "vr: base sType");
    check(bi.pNext == nullptr, "vr: base pNext null");
    check(bi.renderPass == VK_NULL_HANDLE, "vr: base RP null");
    check(bi.framebuffer == VK_NULL_HANDLE, "vr: base FBO null");
    check(bi.subpass == 0, "vr: base subpass 0");
}

// full Classic verification
void verifyClassic(const VKSecondaryInheritanceAdapter &a, VkCommandBufferUsageFlags srcFlags,
                   const VkCommandBufferInheritanceInfo &want) {
    check(a.ready(), "vc: ready");
    const auto &b = a.beginInfo();
    check(b.sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, "vc: begin sType");
    check(b.pNext == nullptr, "vc: begin pNext null");
    check(b.flags == (srcFlags | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT), "vc: flags OR");
    check(b.pInheritanceInfo == &a.inheritanceInfo(), "vc: own inheritance ptr");
    const auto &i = a.inheritanceInfo();
    check(i.sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, "vc: inh sType");
    check(i.pNext == nullptr, "vc: no rendering node");
    check(i.renderPass == want.renderPass, "vc: RP kept");
    check(i.subpass == want.subpass, "vc: subpass kept");
    check(i.framebuffer == want.framebuffer, "vc: FBO kept");
    check(i.occlusionQueryEnable == want.occlusionQueryEnable, "vc: occlusion kept");
    check(i.queryFlags == want.queryFlags, "vc: queryFlags kept");
    check(i.pipelineStatistics == want.pipelineStatistics, "vc: stats kept");
    check(!a.rendering().ready(), "vc: contract inactive");
}

// full Dynamic verification (all tuple fields, guarded array read)
void verifyDynamic(const VKSecondaryInheritanceAdapter &a, VkCommandBufferUsageFlags srcFlags,
                   VkRenderingFlags intendedFlags) {
    check(a.ready(), "vd: ready");
    const auto &b = a.beginInfo();
    check(b.sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, "vd: begin sType");
    check(b.pNext == nullptr, "vd: begin pNext null");
    check(b.flags == (srcFlags | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT), "vd: flags OR");
    check(b.pInheritanceInfo == &a.inheritanceInfo(), "vd: own inheritance ptr");
    const auto &i = a.inheritanceInfo();
    check(i.pNext == &a.rendering().secondaryRenderingInfo(), "vd: pNext owned rendering");
    check(i.renderPass == VK_NULL_HANDLE && i.framebuffer == VK_NULL_HANDLE, "vd: RP/FBO null");
    check(i.subpass == 0, "vd: subpass 0");
    check(i.occlusionQueryEnable == VK_TRUE, "vd: occlusion kept");
    check(i.queryFlags == VK_QUERY_CONTROL_PRECISE_BIT, "vd: queryFlags kept");
    check(i.pipelineStatistics == VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT,
          "vd: stats kept");
    const auto &r = a.rendering();
    check(r.ready(), "vd: contract ready");
    const auto &pi = r.pipelineInfo();
    check(pi.sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, "vd: pipe sType");
    check(pi.pNext == nullptr, "vd: pipe pNext null");
    check(pi.viewMask == 0x3, "vd: viewMask");
    check(pi.depthAttachmentFormat == VK_FORMAT_D24_UNORM_S8_UINT, "vd: depth");
    check(pi.stencilAttachmentFormat == VK_FORMAT_D24_UNORM_S8_UINT, "vd: stencil");
    const bool arrOk = pi.colorAttachmentCount == 2 &&
                       r.signature().colorFormats.size() == 2 &&
                       pi.pColorAttachmentFormats != nullptr &&
                       pi.pColorAttachmentFormats == r.signature().colorFormats.data();
    check(arrOk, "vd: color guard");
    if (arrOk) {
        check(pi.pColorAttachmentFormats[0] == VK_FORMAT_B8G8R8A8_UNORM, "vd: c0");
        check(pi.pColorAttachmentFormats[1] == VK_FORMAT_R16G16B16A16_SFLOAT, "vd: c1");
    }
    const auto &sec = r.secondaryRenderingInfo();
    check(sec.sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO, "vd: sec sType");
    check(sec.pNext == nullptr, "vd: sec pNext null");
    check(sec.viewMask == 0x3, "vd: sec viewMask");
    check(sec.rasterizationSamples == VK_SAMPLE_COUNT_2_BIT, "vd: sec samples");
    check(sec.colorAttachmentCount == 2, "vd: sec count");
    check(sec.pColorAttachmentFormats == pi.pColorAttachmentFormats, "vd: sec shares array");
    check(sec.depthAttachmentFormat == VK_FORMAT_D24_UNORM_S8_UINT, "vd: sec depth");
    check(sec.stencilAttachmentFormat == VK_FORMAT_D24_UNORM_S8_UINT, "vd: sec stencil");
    check(sec.flags == (intendedFlags & ~VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT),
          "vd: sec flags");
    check(r.baseInheritanceInfo().sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
          "vd: base sType");
    check(r.baseInheritanceInfo().pNext == &sec, "vd: base chains sec");
}
} // namespace

int main() {
    static_assert(!std::is_copy_constructible<VKSecondaryInheritanceAdapter>::value &&
                      !std::is_copy_assignable<VKSecondaryInheritanceAdapter>::value &&
                      !std::is_move_constructible<VKSecondaryInheritanceAdapter>::value &&
                      !std::is_move_assignable<VKSecondaryInheritanceAdapter>::value,
                  "non-copy/move");

    // --- 1: initial reset state ---
    {
        VKSecondaryInheritanceAdapter a;
        verifyReset(a);
    }

    // --- 2: Classic full-field copy; invalid/NULL sig never rejects ---
    {
        VKSecondaryInheritanceAdapter a;
        auto base = makeBase();
        auto src = makeBegin(base);
        VKRenderingSignature bad = makeSig();
        bad.samples = static_cast<VkSampleCountFlagBits>(0);
        check(a.build(Mode::Classic, src, base, &bad, 0xFFFFFFFF), "classic invalid sig");
        verifyClassic(a, src.flags, base);
        auto base2 = makeBase();
        base2.framebuffer = VK_NULL_HANDLE;
        auto src2 = makeBegin(base2);
        check(a.build(Mode::Classic, src2, base2, nullptr, 0), "classic null FBO/sig");
        verifyClassic(a, src2.flags, base2);
        VkCommandBufferBeginInfo src3 = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        src3.flags = 0;
        check(a.build(Mode::Classic, src3, base2, nullptr, 0), "classic null source-inh");
        check(a.beginInfo().flags == VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT,
              "classic null src-inh flags");
        check(a.inheritanceInfo().framebuffer == VK_NULL_HANDLE, "classic null src-inh FBO");
    }

    // --- 3: Classic must not read/copy the signature: ZERO allocations ---
    {
        VKSecondaryInheritanceAdapter a;
        auto base = makeBase();
        auto src = makeBegin(base);
        VKRenderingSignature sig = makeSig(); // valid, non-null
        Guard g;
        const long before = g_allocCount;
        const bool ok = a.build(Mode::Classic, src, base, &sig, 0);
        const long allocs = g_allocCount - before;
        check(ok, "classic zero-alloc build");
        check(allocs == 0, "classic must not allocate for an ignored signature");
        verifyClassic(a, src.flags, base);
    }

    // --- 4: Dynamic full tuple + flags; SUSPEND/RESUME kept ---
    {
        VKSecondaryInheritanceAdapter a;
        auto base = makeBase();
        auto src = makeBegin(base);
        VKRenderingSignature sig = makeSig();
        const VkRenderingFlags intended = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT |
                                          VK_RENDERING_SUSPENDING_BIT | VK_RENDERING_RESUMING_BIT;
        check(a.build(Mode::Dynamic, src, base, &sig, intended), "dynamic build");
        verifyDynamic(a, src.flags, intended);
        check((a.rendering().secondaryRenderingInfo().flags & VK_RENDERING_SUSPENDING_BIT) != 0,
              "dyn: SUSPEND kept");
        check((a.rendering().secondaryRenderingInfo().flags & VK_RENDERING_RESUMING_BIT) != 0,
              "dyn: RESUME kept");
    }

    // --- 5: Dynamic empty colors / UNUSED slot retained ---
    {
        VKSecondaryInheritanceAdapter a;
        auto base = makeBase();
        auto src = makeBegin(base);
        VKRenderingSignature sig; // empty colors, UNDEFINED D/S
        check(a.build(Mode::Dynamic, src, base, &sig, 0), "dynamic empty");
        const auto &r = a.rendering();
        check(r.pipelineInfo().colorAttachmentCount == 0, "de: count 0");
        check(r.pipelineInfo().pColorAttachmentFormats == nullptr, "de: ptr null");
        check(r.pipelineInfo().depthAttachmentFormat == VK_FORMAT_UNDEFINED, "de: depth");
        check(r.pipelineInfo().stencilAttachmentFormat == VK_FORMAT_UNDEFINED, "de: stencil");
        check(r.secondaryRenderingInfo().flags == 0, "de: flags 0");
        VKRenderingSignature sig2;
        sig2.colorFormats = {VK_FORMAT_R8_UNORM, VK_FORMAT_UNDEFINED, VK_FORMAT_B8G8R8A8_UNORM};
        check(a.build(Mode::Dynamic, src, base, &sig2, 0), "dynamic unused slot");
        const auto &pi = a.rendering().pipelineInfo();
        check(pi.colorAttachmentCount == 3, "du: 3 slots kept");
        if (pi.colorAttachmentCount == 3 && pi.pColorAttachmentFormats != nullptr) {
            check(pi.pColorAttachmentFormats[1] == VK_FORMAT_UNDEFINED, "du: middle slot UNDEFINED");
        }
    }

    // --- 6: allocation failure paths: every Dynamic alloc point resets ---
    {
        // count the real allocation points of a Dynamic build first
        long total = 0;
        {
            VKSecondaryInheritanceAdapter a;
            auto base = makeBase();
            auto src = makeBegin(base);
            VKRenderingSignature sig = makeSig();
            Guard g;
            const long before = g_allocCount;
            check(a.build(Mode::Dynamic, src, base, &sig, 0), "alloc count probe");
            total = g_allocCount - before;
        }
        check(total >= 1, "dynamic build must allocate at least once");
        std::printf("dynamic build allocation points: %ld\n", total);
        for (long n = 1; n <= total; ++n) {
            VKSecondaryInheritanceAdapter a;
            auto base = makeBase();
            auto src = makeBegin(base);
            VKRenderingSignature sig = makeSig();
            Guard g;
            // prime a valid build first so stale state would be visible
            check(a.build(Mode::Dynamic, src, base, &sig, 0), "alloc prime");
            verifyDynamic(a, src.flags, 0);
            g_failNth = g_allocCount + n; // fail the n-th allocation of THIS build
            bool threw = false;
            try {
                a.build(Mode::Dynamic, src, base, &sig, 0);
            } catch (const std::bad_alloc &) {
                threw = true;
            }
            if (!threw) {
                check(false, "alloc failure must propagate as bad_alloc");
            } else {
                verifyReset(a); // exception must fully reset, old state gone
            }
            std::printf("alloc fail point %ld/%ld: threw=%d reset-verified\n", n, total,
                        threw ? 1 : 0);
        }
    }

    // --- 7: rejections: prime -> bad -> full reset -> recover (true branches) ---
    {
        VKSecondaryInheritanceAdapter a;
        VKRenderingSignature sig = makeSig();
        unsigned rejections = 0;
        auto primeRecover = [&](const char *l) {
            auto b = makeBase();
            auto s = makeBegin(b);
            check(a.build(Mode::Dynamic, s, b, &sig, 0), l);
            verifyDynamic(a, s.flags, 0);
        };
        // NOTE: base is taken BY REFERENCE so &base matches s.pInheritanceInfo;
        // each case asserts the pointer precondition (except the foreign case)
        auto cycle = [&](const char *l, Mode mode, const VkCommandBufferBeginInfo &s,
                         const VkCommandBufferInheritanceInfo &b,
                         const VKRenderingSignature *sg) {
            ++rejections;
            check(s.pInheritanceInfo == &b || s.pInheritanceInfo == nullptr,
                  "precondition: source points at the same base (or null)");
            primeRecover(l);
            check(!a.build(mode, s, b, sg, 0), l);
            verifyReset(a);
            primeRecover(l);
        };
        {
            auto b = makeBase();
            auto s = makeBegin(b);
            s.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            cycle("source sType", Mode::Classic, s, b, nullptr);
        }
        {
            static int d = 0;
            auto b = makeBase();
            auto s = makeBegin(b);
            s.pNext = &d;
            cycle("source pNext", Mode::Classic, s, b, nullptr);
        }
        {
            auto b = makeBase();
            auto b2 = makeBase();
            auto s = makeBegin(b);
            s.pInheritanceInfo = &b2; // the ONLY foreign-pointer case
            check(s.pInheritanceInfo != &b, "foreign case precondition");
            ++rejections;
            primeRecover("foreign pInheritanceInfo");
            check(!a.build(Mode::Classic, s, b, nullptr, 0), "foreign pInheritanceInfo");
            verifyReset(a);
            primeRecover("foreign pInheritanceInfo");
        }
        {
            auto b = makeBase();
            auto s = makeBegin(b);
            b.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            cycle("base sType", Mode::Classic, s, b, nullptr);
        }
        {
            static int d = 0;
            auto b = makeBase();
            auto s = makeBegin(b);
            b.pNext = &d;
            cycle("base pNext", Mode::Classic, s, b, nullptr);
        }
        {
            auto b = makeBase();
            b.renderPass = VK_NULL_HANDLE;
            auto s = makeBegin(b);
            cycle("classic RP null", Mode::Classic, s, b, nullptr);
        }
        {
            auto b = makeBase();
            auto s = makeBegin(b);
            cycle("dynamic sig null", Mode::Dynamic, s, b, nullptr);
        }
        {
            auto b = makeBase();
            auto s = makeBegin(b);
            VKRenderingSignature bad = makeSig();
            bad.samples = static_cast<VkSampleCountFlagBits>(0);
            cycle("dynamic sig samples0", Mode::Dynamic, s, b, &bad);
        }
        {
            auto b = makeBase();
            auto s = makeBegin(b);
            VKRenderingSignature bad = makeSig();
            bad.samples = static_cast<VkSampleCountFlagBits>(VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT);
            cycle("dynamic sig samples combined", Mode::Dynamic, s, b, &bad);
        }
        {
            auto b = makeBase();
            auto s = makeBegin(b);
            VKRenderingSignature bad = makeSig();
            bad.depthFormat = VK_FORMAT_D32_SFLOAT;
            bad.stencilFormat = VK_FORMAT_S8_UINT;
            cycle("dynamic sig DS mismatch", Mode::Dynamic, s, b, &bad);
        }
        {
            auto b = makeBase();
            auto s = makeBegin(b);
            cycle("mode invalid", static_cast<Mode>(99), s, b, &sig);
        }
        check(rejections == 11, "rejection count");
        std::printf("rejection cases exercised: %u\n", rejections);
    }

    // --- 8: own dynamic inheritance as new input: rejected (existing chain) ---
    {
        VKSecondaryInheritanceAdapter a;
        auto base = makeBase();
        auto src = makeBegin(base);
        VKRenderingSignature sig = makeSig();
        check(a.build(Mode::Dynamic, src, base, &sig, 0), "own dyn prime");
        verifyDynamic(a, src.flags, 0);
        check(a.inheritanceInfo().pNext != nullptr, "own dyn pNext really nonnull");
        // TRUE aliasing: pass own begin/inheritance references directly
        check(!a.build(Mode::Dynamic, a.beginInfo(), a.inheritanceInfo(), &sig, 0),
              "own dyn chain rejected");
        verifyReset(a);
        auto b3 = makeBase();
        auto s3 = makeBegin(b3);
        check(a.build(Mode::Dynamic, s3, b3, &sig, 0), "own dyn recover");
        verifyDynamic(a, s3.flags, 0);
    }

    // --- 9: Classic self-rebuild via TRUE aliasing is legal ---
    {
        VKSecondaryInheritanceAdapter a;
        auto base = makeBase();
        auto src = makeBegin(base);
        check(a.build(Mode::Classic, src, base, nullptr, 0), "own classic prime");
        verifyClassic(a, src.flags, base);
        // pass OWN begin/inheritance references (pInheritanceInfo == own inh)
        check(a.build(Mode::Classic, a.beginInfo(), a.inheritanceInfo(), nullptr, 0),
              "own classic alias rebuild legal");
        verifyClassic(a, src.flags | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT, base);
    }

    // --- 10: owning: array non-borrow proof, realloc, scoped death ---
    {
        VKSecondaryInheritanceAdapter a;
        bool guard = false;
        {
            auto base = makeBase();
            auto src = makeBegin(base);
            VKRenderingSignature sig = makeSig();
            check(a.build(Mode::Dynamic, src, base, &sig, 0), "owning dyn");
            const auto &pi = a.rendering().pipelineInfo();
            guard = pi.pColorAttachmentFormats != nullptr &&
                    pi.pColorAttachmentFormats ==
                        a.rendering().signature().colorFormats.data() &&
                    pi.pColorAttachmentFormats != sig.colorFormats.data();
            check(guard, "owning: array address differs from caller");
            if (guard) {
                sig.colorFormats.push_back(VK_FORMAT_R8_UNORM); // may realloc caller array
                sig.colorFormats[0] = VK_FORMAT_R32_UINT;
                base.renderPass = VK_NULL_HANDLE;
            }
        } // caller inputs destroyed
        if (guard) {
            verifyDynamic(a, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, 0);
        } else {
            check(false, "owning: guard failed");
        }
    }

    // --- 11: two owners: distinct arrays, rebuild independence ---
    {
        VKSecondaryInheritanceAdapter x, y;
        auto base = makeBase();
        auto src = makeBegin(base);
        VKRenderingSignature sig = makeSig();
        check(x.build(Mode::Dynamic, src, base, &sig, 0) && y.build(Mode::Dynamic, src, base, &sig, 0),
              "two dyn");
        check(x.rendering().pipelineInfo().pColorAttachmentFormats !=
                  y.rendering().pipelineInfo().pColorAttachmentFormats,
              "two: distinct owned arrays");
        check(x.build(Mode::Classic, src, base, nullptr, 0), "two: x classic");
        check(x.inheritanceInfo().pNext == nullptr, "two: x no stale node");
        check(!x.rendering().ready(), "two: x contract cleared");
        verifyDynamic(y, src.flags, 0); // y fully intact
    }

    // --- 12: self-input: signature aliasing own contract ---
    {
        VKSecondaryInheritanceAdapter a;
        auto base = makeBase();
        auto src = makeBegin(base);
        VKRenderingSignature sig = makeSig();
        check(a.build(Mode::Dynamic, src, base, &sig, 0), "self sig prime");
        auto b2 = makeBase();
        auto s2 = makeBegin(b2);
        check(a.build(Mode::Dynamic, s2, b2, &a.rendering().signature(), 0), "self sig rebuild");
        verifyDynamic(a, s2.flags, 0);
    }

    // --- 13: public reset() after a valid build ---
    {
        VKSecondaryInheritanceAdapter a;
        auto base = makeBase();
        auto src = makeBegin(base);
        VKRenderingSignature sig = makeSig();
        check(a.build(Mode::Dynamic, src, base, &sig, 0), "reset prime");
        verifyDynamic(a, src.flags, 0);
        a.reset();
        verifyReset(a);
    }

    if (g_failures == 0) {
        std::printf("secondary inheritance adapter tests: all PASS\n");
        return 0;
    }
    std::printf("secondary inheritance adapter tests: %u FAIL\n", g_failures);
    return 1;
}
#endif
