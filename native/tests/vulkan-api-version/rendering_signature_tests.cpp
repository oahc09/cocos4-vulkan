// Pure-owning VKRenderingSignature CPU contract tests (M5-B, revised).
// No GPU, no loader, no engine. Verifies deterministic equality, hash with
// a known FNV-1a vector, per-field hash sensitivity, copy/move semantics,
// default values, and unordered_map collision handling.

#include <cstdio>
#include <unordered_map>
#include <utility>
#include "VKRenderingSignature.h"

using cc::gfx::VKRenderingSignature;
using cc::gfx::VKRenderingSignatureHash;

namespace {
unsigned g_failures = 0;
void check(bool ok, const char *w) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", w);
        ++g_failures;
    }
}

VKRenderingSignature makeSample() {
    VKRenderingSignature s;
    s.colorFormats = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT};
    s.depthFormat = VK_FORMAT_D32_SFLOAT;
    s.stencilFormat = VK_FORMAT_S8_UINT;
    s.samples = VK_SAMPLE_COUNT_2_BIT;
    s.viewMask = 0x3;
    return s;
}
} // namespace

int main() {
    // --- 1: known default hash vector (FNV-1a 64-bit, RFC 9923) ---
    // Default signature mixes u64 fields: count=0, depth=0, stencil=0,
    // samples=1, viewMask=0 → 40 bytes little-endian → FNV-1a.
    // Expected: 0xf1d88844dde14404 (Lead-verified via Python struct.pack).
    {
        VKRenderingSignature def;
        uint64_t h = def.hash();
        std::printf("default hash: 0x%016llx\n", (unsigned long long)h);
        check(h == 0xf1d88844dde14404ULL, "known default hash vector mismatch");
    }

    // --- 2: defaults are explicitly correct ---
    {
        VKRenderingSignature def;
        check(def.colorFormats.empty(), "default colorFormats must be empty");
        check(def.depthFormat == VK_FORMAT_UNDEFINED, "default depthFormat UNDEFINED");
        check(def.stencilFormat == VK_FORMAT_UNDEFINED, "default stencilFormat UNDEFINED");
        check(def.samples == VK_SAMPLE_COUNT_1_BIT, "default samples 1");
        check(def.viewMask == 0, "default viewMask 0");
    }

    // --- 3: color reorder / count / unused-UNDEFINED ---
    {
        VKRenderingSignature a, b;
        a.colorFormats = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT};
        b.colorFormats = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_B8G8R8A8_UNORM};
        check(a != b, "color reorder != ");
    }
    {
        VKRenderingSignature a, b;
        a.colorFormats = {VK_FORMAT_B8G8R8A8_UNORM};
        b.colorFormats = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
        check(a != b, "color count !=");
    }
    {
        VKRenderingSignature a, b;
        b.colorFormats = {VK_FORMAT_UNDEFINED};
        check(a != b, "empty vs one-UNDEFINED !=");
    }

    // --- 4: depth/stencil/samples/viewMask each differ ---
    {
        VKRenderingSignature a = makeSample(), b = makeSample();
        b.depthFormat = VK_FORMAT_D24_UNORM_S8_UINT;
        check(a != b, "depth !=");
    }
    {
        VKRenderingSignature a = makeSample(), b = makeSample();
        b.stencilFormat = VK_FORMAT_UNDEFINED;
        check(a != b, "stencil !=");
    }
    {
        VKRenderingSignature a = makeSample(), b = makeSample();
        b.samples = VK_SAMPLE_COUNT_4_BIT;
        check(a != b, "samples !=");
    }
    {
        VKRenderingSignature a = makeSample(), b = makeSample();
        b.viewMask = 0x1;
        check(a != b, "viewMask !=");
    }

    // --- 5: per-field hash sensitivity (single mutation each) ---
    {
        VKRenderingSignature base = makeSample();
        // color value
        {
            VKRenderingSignature m = base;
            m.colorFormats[0] = VK_FORMAT_R8G8B8A8_UNORM;
            check(m != base && m.hash() != base.hash(), "hash sensitive: color value");
        }
        // color order
        {
            VKRenderingSignature m = base;
            std::swap(m.colorFormats[0], m.colorFormats[1]);
            check(m != base && m.hash() != base.hash(), "hash sensitive: color order");
        }
        // color count
        {
            VKRenderingSignature m = base;
            m.colorFormats.push_back(VK_FORMAT_R8_UNORM);
            check(m != base && m.hash() != base.hash(), "hash sensitive: color count");
        }
        // UNDEFINED position
        {
            VKRenderingSignature m = base;
            m.colorFormats[1] = VK_FORMAT_UNDEFINED;
            check(m != base && m.hash() != base.hash(), "hash sensitive: UNDEFINED position");
        }
        // depth
        {
            VKRenderingSignature m = base;
            m.depthFormat = VK_FORMAT_D16_UNORM;
            check(m != base && m.hash() != base.hash(), "hash sensitive: depth");
        }
        // stencil
        {
            VKRenderingSignature m = base;
            m.stencilFormat = VK_FORMAT_UNDEFINED;
            check(m != base && m.hash() != base.hash(), "hash sensitive: stencil");
        }
        // samples
        {
            VKRenderingSignature m = base;
            m.samples = VK_SAMPLE_COUNT_8_BIT;
            check(m != base && m.hash() != base.hash(), "hash sensitive: samples");
        }
        // viewMask
        {
            VKRenderingSignature m = base;
            m.viewMask = 0x7;
            check(m != base && m.hash() != base.hash(), "hash sensitive: viewMask");
        }
    }

    // --- 6: copy independence (vector not shared) ---
    {
        VKRenderingSignature a = makeSample();
        VKRenderingSignature b = a; // copy construct
        a.colorFormats[0] = VK_FORMAT_R8G8B8A8_UNORM;
        check(b.colorFormats[0] == VK_FORMAT_B8G8R8A8_UNORM, "copy: vector not shared");
        check(a != b, "copy: mutated original differs");
        // copy assignment
        VKRenderingSignature c;
        c = a;
        a.colorFormats.push_back(VK_FORMAT_R8_UNORM);
        check(c.colorFormats.size() == 2, "copy assign: vector independent");
    }

    // --- 7: move preserves full tuple and hash ---
    {
        VKRenderingSignature a = makeSample();
        VKRenderingSignature b = a;
        VKRenderingSignature c = std::move(b);
        check(c == a && c.hash() == a.hash(), "move: full tuple and hash preserved");
    }

    // --- 8: equal signatures have equal hash ---
    {
        VKRenderingSignature a = makeSample(), b = a;
        check(a == b && a.hash() == b.hash(), "equal sigs equal hash");
    }

    // --- 9: collision map (constant hasher) ---
    {
        struct ConstHash {
            size_t operator()(const VKRenderingSignature &) const { return 42; }
        };
        std::unordered_map<VKRenderingSignature, int, ConstHash> m;
        VKRenderingSignature s1 = makeSample();
        VKRenderingSignature s2 = makeSample();
        s2.depthFormat = VK_FORMAT_D16_UNORM;
        VKRenderingSignature s3 = makeSample();
        s3.viewMask = 0x7;
        m[s1] = 100;
        m[s2] = 200;
        check(m.size() == 2, "collision: two entries");
        check(m.find(s1) != m.end() && m.find(s1)->second == 100, "collision: find s1");
        check(m.find(s2) != m.end() && m.find(s2)->second == 200, "collision: find s2");
        check(m.find(s3) == m.end(), "collision: unstored s3 not found");
        VKRenderingSignature s1copy = s1;
        check(m.find(s1copy) != m.end() && m.find(s1copy)->second == 100,
              "collision: copy of s1 retrieves s1's value");
    }

    // --- 10: standard hasher in unordered_map ---
    {
        std::unordered_map<VKRenderingSignature, int, VKRenderingSignatureHash> m;
        VKRenderingSignature s1 = makeSample();
        VKRenderingSignature s2 = makeSample();
        s2.samples = VK_SAMPLE_COUNT_4_BIT;
        m[s1] = 1;
        m[s2] = 2;
        check(m.find(s1) != m.end() && m.find(s1)->second == 1, "hasher: find s1");
        check(m.find(s2) != m.end() && m.find(s2)->second == 2, "hasher: find s2");
    }

    if (g_failures == 0) {
        std::printf("rendering signature contract tests: all PASS\n");
        return 0;
    }
    std::printf("rendering signature contract tests: %u FAIL\n", g_failures);
    return 1;
}
