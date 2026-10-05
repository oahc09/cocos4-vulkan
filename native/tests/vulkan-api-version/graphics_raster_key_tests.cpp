/****************************************************************************
 Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
****************************************************************************/

#include "VKGraphicsRasterKey.h"

#ifdef VKGRAPHICRASTER_STANDALONE_PROBE
int main() { return 0; }
#else

    #include <cstdio>
    #include <cstdlib>
    #include <cstring>
    #include <new>
    #include <string>
    #include <unordered_map>
    #include <vector>

using cc::gfx::VulkanGraphicsRasterKey;
using RKey = VulkanGraphicsRasterKey;

static int g_failures = 0;
static bool g_leadFailNextAllocation = false;
static unsigned g_leadInjections = 0;
static bool g_countAllocations = false;
static size_t g_allocationCalls = 0, g_failNth = 0;
void *operator new(size_t size) {
    if (g_leadFailNextAllocation) {
        g_leadFailNextAllocation = false;
        ++g_leadInjections;
        throw std::bad_alloc();
    }
    if (g_countAllocations && ++g_allocationCalls == g_failNth) {
        ++g_leadInjections;
        throw std::bad_alloc();
    }
    if (void *p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }

    #define CHECK(cond, label)                    \
        do {                                      \
            if (!(cond)) {                        \
                std::printf("FAIL: %s\n", label); \
                ++g_failures;                     \
            }                                     \
        } while (0)

static float asFloat(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

static VkPipelineRasterizationStateCreateInfo defRaster() {
    VkPipelineRasterizationStateCreateInfo r = {};
    r.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    r.polygonMode = VK_POLYGON_MODE_FILL;
    r.cullMode = VK_CULL_MODE_BACK_BIT;
    r.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    r.lineWidth = 1.0f;
    return r;
}

static VkPipelineMultisampleStateCreateInfo defMS() {
    VkPipelineMultisampleStateCreateInfo m = {};
    m.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    m.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    m.minSampleShading = 1.0f;
    return m;
}

static VkPipelineDepthStencilStateCreateInfo defDS() {
    VkPipelineDepthStencilStateCreateInfo d = {};
    d.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    d.depthTestEnable = VK_TRUE;
    d.depthWriteEnable = VK_TRUE;
    d.depthCompareOp = VK_COMPARE_OP_LESS;
    d.minDepthBounds = 0.0f;
    d.maxDepthBounds = 1.0f;
    return d;
}

static size_t dsWords() {
    // 1 presence + 1 flags + 5 bools/ops + 7 front + 7 back + 2 bounds = 23
    return 23u;
}

static size_t totalWords(uint32_t maskWords, bool withDS) {
    // raster: 11, ms: 7 + maskWords, ds: withDS ? 23 : 1
    return 11u + 7u + maskWords + (withDS ? dsWords() : 1u);
}

static uint32_t checkedWord(const RKey &key, size_t index) {
    const bool ok = index < key.words().size();
    CHECK(ok, "word index is inside owned array");
    return ok ? key.words()[index] : 0;
}

struct FullFixture {
    VkPipelineRasterizationStateCreateInfo raster = defRaster();
    VkPipelineMultisampleStateCreateInfo ms = defMS();
    VkPipelineDepthStencilStateCreateInfo ds = defDS();
    uint32_t mask[2] = {0xa5c3f00f, 0x12345678};
    FullFixture() {
        raster.flags = 0x11;
        raster.depthClampEnable = 1;
        raster.polygonMode = VK_POLYGON_MODE_LINE;
        raster.cullMode = VK_CULL_MODE_FRONT_BIT;
        raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
        raster.depthBiasEnable = 1;
        raster.depthBiasConstantFactor = 0.5F;
        raster.depthBiasClamp = 0.25F;
        raster.depthBiasSlopeFactor = 2.F;
        raster.lineWidth = 1.5F;
        ms.flags = 0x22;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;
        ms.sampleShadingEnable = 1;
        ms.minSampleShading = 0.75F;
        ms.pSampleMask = mask;
        ms.alphaToCoverageEnable = 1;
        ds.flags = 0x33;
        ds.depthWriteEnable = 0;
        ds.depthCompareOp = VK_COMPARE_OP_GREATER;
        ds.depthBoundsTestEnable = 1;
        ds.stencilTestEnable = 1;
        ds.front = {VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_INCREMENT_AND_WRAP,
                    VK_STENCIL_OP_DECREMENT_AND_CLAMP, VK_COMPARE_OP_EQUAL, 0x123, 0x456, 0x789};
        ds.back = {VK_STENCIL_OP_ZERO, VK_STENCIL_OP_DECREMENT_AND_WRAP,
                   VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_COMPARE_OP_LESS_OR_EQUAL, 0xabc, 0xdef, 0x135};
        ds.minDepthBounds = 0.125F;
        ds.maxDepthBounds = 0.875F;
    }
};

static void verifyFullContracts() {
    // Manually specified raw values, including every front/back field and IEEE float bits.
    const std::vector<uint32_t> expected = {
        0x11, 1, 0, 1, 1, 1, 1, 0x3f000000, 0x3e800000, 0x40000000, 0x3fc00000,
        0x22, 4, 1, 0x3f400000, 1, 0xa5c3f00f, 1, 0,
        1, 0x33, 1, 0, 4, 1, 1,
        2, 6, 4, 2, 0x123, 0x456, 0x789,
        1, 7, 3, 3, 0xabc, 0xdef, 0x135, 0x3e000000, 0x3f600000};
    FullFixture fixture;
    RKey full;
    CHECK(RKey::build(fixture.raster, fixture.ms, &fixture.ds, full), "full asymmetric build");
    CHECK(full.valid() && full.words() == expected, "all 42 asymmetric words match independent stream");

    const auto different = [&](const FullFixture &changed) {
        RKey key;
        CHECK(RKey::build(changed.raster, changed.ms, &changed.ds, key), "single field builds");
        CHECK(key != full && key.hash() != full.hash(), "single field changes complete value/hash");
    };
    // Each edit starts from a newly constructed fixture; no pointer aliases are copied.
    #define EDIT_FIELD(field, value) \
        do {                         \
            FullFixture f;           \
            f.field = value;         \
            different(f);            \
        } while (0)
    EDIT_FIELD(raster.flags, 0x12);
    EDIT_FIELD(raster.depthClampEnable, 0);
    EDIT_FIELD(raster.rasterizerDiscardEnable, 1);
    EDIT_FIELD(raster.polygonMode, VK_POLYGON_MODE_FILL);
    EDIT_FIELD(raster.cullMode, VK_CULL_MODE_BACK_BIT);
    EDIT_FIELD(raster.frontFace, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    EDIT_FIELD(raster.depthBiasEnable, 0);
    EDIT_FIELD(raster.depthBiasConstantFactor, 0.75F);
    EDIT_FIELD(raster.depthBiasClamp, 0.5F);
    EDIT_FIELD(raster.depthBiasSlopeFactor, 3.F);
    EDIT_FIELD(raster.lineWidth, 2.F);
    EDIT_FIELD(ms.flags, 0x23);
    EDIT_FIELD(ms.rasterizationSamples, VK_SAMPLE_COUNT_2_BIT);
    EDIT_FIELD(ms.sampleShadingEnable, 0);
    EDIT_FIELD(ms.minSampleShading, 0.5F);
    EDIT_FIELD(mask[0], 0xa5c3f010);
    EDIT_FIELD(ms.alphaToCoverageEnable, 0);
    EDIT_FIELD(ms.alphaToOneEnable, 1);
    EDIT_FIELD(ds.flags, 0x34);
    EDIT_FIELD(ds.depthTestEnable, 0);
    EDIT_FIELD(ds.depthWriteEnable, 1);
    EDIT_FIELD(ds.depthCompareOp, VK_COMPARE_OP_LESS);
    EDIT_FIELD(ds.depthBoundsTestEnable, 0);
    EDIT_FIELD(ds.stencilTestEnable, 0);
    EDIT_FIELD(ds.front.failOp, VK_STENCIL_OP_KEEP);
    EDIT_FIELD(ds.front.passOp, VK_STENCIL_OP_KEEP);
    EDIT_FIELD(ds.front.depthFailOp, VK_STENCIL_OP_KEEP);
    EDIT_FIELD(ds.front.compareOp, VK_COMPARE_OP_ALWAYS);
    EDIT_FIELD(ds.front.compareMask, 0x124);
    EDIT_FIELD(ds.front.writeMask, 0x457);
    EDIT_FIELD(ds.front.reference, 0x78a);
    EDIT_FIELD(ds.back.failOp, VK_STENCIL_OP_KEEP);
    EDIT_FIELD(ds.back.passOp, VK_STENCIL_OP_KEEP);
    EDIT_FIELD(ds.back.depthFailOp, VK_STENCIL_OP_KEEP);
    EDIT_FIELD(ds.back.compareOp, VK_COMPARE_OP_ALWAYS);
    EDIT_FIELD(ds.back.compareMask, 0xabd);
    EDIT_FIELD(ds.back.writeMask, 0xdf0);
    EDIT_FIELD(ds.back.reference, 0x136);
    EDIT_FIELD(ds.minDepthBounds, 0.25F);
    EDIT_FIELD(ds.maxDepthBounds, 0.75F);
    #undef EDIT_FIELD

    for (unsigned badCase = 0; badCase < 9; ++badCase) {
        RKey output;
        CHECK(RKey::build(fixture.raster, fixture.ms, &fixture.ds, output), "each rejection is freshly primed");
        CHECK(output.words() == expected, "each prime has complete asymmetric value");
        FullFixture bad;
        switch (badCase) {
            case 0: bad.raster.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO; break;
            case 1: bad.raster.pNext = &bad; break;
            case 2: bad.ms.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO; break;
            case 3: bad.ms.pNext = &bad; break;
            case 4: bad.ds.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO; break;
            case 5: bad.ds.pNext = &bad; break;
            case 6: bad.ms.rasterizationSamples = static_cast<VkSampleCountFlagBits>(0); break;
            case 7: bad.ms.rasterizationSamples = static_cast<VkSampleCountFlagBits>(3); break;
            case 8: bad.ms.rasterizationSamples = static_cast<VkSampleCountFlagBits>(128); break;
        }
        CHECK(!RKey::build(bad.raster, bad.ms, &bad.ds, output), "bad shape rejects");
        CHECK(!output.valid() && output.words().empty() && output == RKey{} &&
                  output.hash() == UINT64_C(0xf4e07088e006fb32),
              "each rejection resets the full value");
        CHECK(RKey::build(fixture.raster, fixture.ms, &fixture.ds, output) && output.words() == expected,
              "each rejection independently recovers complete tuple");
    }

    RKey owned;
    bool notBorrowed = false;
    {
        FullFixture caller;
        std::vector<uint32_t> mask{caller.mask[0]};
        caller.ms.pSampleMask = mask.data();
        CHECK(RKey::build(caller.raster, caller.ms, &caller.ds, owned), "scoped full caller builds");
        notBorrowed = owned.words().data() != mask.data() && owned.words() == expected;
        CHECK(notBorrowed, "owned words never borrow caller mask");
        if (notBorrowed) {
            caller.raster = defRaster();
            caller.ms = defMS();
            caller.ds = defDS();
            mask[0] ^= 1;
            mask.reserve(mask.capacity() + 64);
            CHECK(owned.words() == expected, "caller all families/mask realloc cannot change tuple");
        }
    }
    if (notBorrowed) CHECK(owned.words() == expected, "full tuple remains after real caller destruction");
    RKey copy = full;
    CHECK(copy == full && copy.words().data() != full.words().data(), "two equal owners have distinct arrays");
    FullFixture small;
    CHECK(RKey::build(small.raster, small.ms, nullptr, copy), "rebuild copied owner");
    CHECK(full.words() == expected, "rebuilding one owner preserves every field of the other");

    RKey fresh;
    g_allocationCalls = 0;
    g_failNth = 0;
    g_countAllocations = true;
    const bool built = RKey::build(fixture.raster, fixture.ms, &fixture.ds, fresh);
    g_countAllocations = false;
    const size_t points = g_allocationCalls;
    CHECK(built && fresh.words() == expected && points > 0, "fresh allocation measurement must observe real points");
    std::printf("Lead fresh build allocation points: %zu\n", points);
    for (size_t point = 1; point <= points; ++point) {
        RKey output;
        CHECK(RKey::build(fixture.raster, fixture.ms, &fixture.ds, output), "allocation prime outside guard");
        g_leadInjections = 0;
        g_allocationCalls = 0;
        g_failNth = point;
        g_countAllocations = true;
        const bool succeeded = RKey::build(fixture.raster, fixture.ms, &fixture.ds, output);
        g_countAllocations = false;
        g_failNth = 0;
        CHECK(g_leadInjections == 1 && !succeeded, "each measured point must inject exactly once");
        CHECK(!output.valid() && output.words().empty() && output == RKey{} &&
                  output.hash() == RKey{}.hash(),
              "each allocation failure fully resets");
        CHECK(RKey::build(fixture.raster, fixture.ms, &fixture.ds, output) && output.words() == expected,
              "allocation failure recovers all 42 words");
    }
}

int main() {
    verifyFullContracts();
    // Lead independently computed LE-byte FNV vectors; preserve on revision.
    {
        RKey invalid;
        CHECK(invalid.hash() == UINT64_C(0xf4e07088e006fb32), "Lead default LE-byte FNV vector");
        auto raster = defRaster();
        auto ms = defMS();
        RKey key;
        CHECK(RKey::build(raster, ms, nullptr, key), "Lead base vector build");
        CHECK(key.hash() == UINT64_C(0xa4d043e3d1cf3611), "Lead built LE-byte FNV vector");

        // Copy is a regular value operation: do not silently swallow allocation failure.
        RKey destination;
        bool copyThrew = false;
        g_leadInjections = 0;
        g_leadFailNextAllocation = true;
        try {
            destination = key;
        } catch (const std::bad_alloc &) {
            copyThrew = true;
        }
        g_leadFailNextAllocation = false;
        CHECK(g_leadInjections == 1 && copyThrew, "Lead copy assignment propagates one real bad_alloc");
        CHECK(key.valid(), "Lead source survives failed copy");

        // This model promises opaque scalar snapshots, not VkBool32 normalization.
        raster.depthClampEnable = 1;
        RKey one, two;
        CHECK(RKey::build(raster, ms, nullptr, one), "Lead opaque bool base");
        raster.depthClampEnable = 2;
        CHECK(RKey::build(raster, ms, nullptr, two), "Lead opaque bool mutation");
        CHECK(one != two, "Lead opaque VkBool32 raw values stay distinct");
    }
    // --- 1: default invalid ---
    {
        RKey k;
        CHECK(!k.valid(), "default invalid");
        CHECK(k.words().empty(), "default words empty");
    }

    // --- 2: basic valid build + word count ---
    {
        auto r = defRaster();
        auto m = defMS();
        RKey k;
        CHECK(RKey::build(r, m, nullptr, k), "basic build");
        CHECK(k.valid(), "basic valid");
        auto d = defDS();
        RKey k2;
        CHECK(RKey::build(r, m, &d, k2), "build with DS");
        CHECK(k2.words().size() == totalWords(1, true), "basic word count with DS");
        CHECK(k != k2, "DS presence distinguishes");
        CHECK(k.hash() != k2.hash(), "DS presence hash differs");
    }

    // --- 3: hand-written expected word stream (not built by key) ---
    {
        auto r = defRaster();
        auto m = defMS();
        RKey k;
        CHECK(RKey::build(r, m, nullptr, k), "stream build");
        const std::vector<uint32_t> expect = {
            r.flags, 0u, 0u,
            static_cast<uint32_t>(r.polygonMode),
            static_cast<uint32_t>(r.cullMode),
            static_cast<uint32_t>(r.frontFace),
            0u,
            0x00000000u, // depthBiasConstantFactor default 0.0f
            0x00000000u, // depthBiasClamp
            0x00000000u, // depthBiasSlopeFactor
            0x3f800000u, // lineWidth 1.0f
            m.flags, 1u, 0u,
            0x3f800000u, // minSampleShading 1.0f
            1u,          // maskWordCount
            0xffffffffu, // normalized mask
            0u, 0u,      // alphaToCoverage/alphaToOne
            0u           // DS absent
        };
        CHECK(k.words() == expect, "expected word stream match");
    }

    // --- 4: single-field mutation sensitivity (raster) ---
    {
        auto r1 = defRaster();
        auto m = defMS();
        RKey base;
        CHECK(RKey::build(r1, m, nullptr, base), "raster base");
        auto r2 = r1;
        r2.flags = 0x1u; // non-zero opaque flag bits
        RKey k;
        CHECK(RKey::build(r2, m, nullptr, k), "flags build");
        CHECK(k != base && k.hash() != base.hash(), "flags sensitivity");
        r2 = r1;
        r2.depthClampEnable = VK_TRUE;
        CHECK(RKey::build(r2, m, nullptr, k) && k != base, "depthClamp sens");
        r2 = r1;
        r2.rasterizerDiscardEnable = VK_TRUE;
        CHECK(RKey::build(r2, m, nullptr, k) && k != base, "rasterDiscard sens");
        r2 = r1;
        r2.polygonMode = VK_POLYGON_MODE_LINE;
        CHECK(RKey::build(r2, m, nullptr, k) && k != base, "polygonMode sens");
        r2 = r1;
        r2.cullMode = VK_CULL_MODE_FRONT_BIT;
        CHECK(RKey::build(r2, m, nullptr, k) && k != base, "cullMode sens");
        r2 = r1;
        r2.frontFace = VK_FRONT_FACE_CLOCKWISE;
        CHECK(RKey::build(r2, m, nullptr, k) && k != base, "frontFace sens");
        r2 = r1;
        r2.depthBiasEnable = VK_TRUE;
        CHECK(RKey::build(r2, m, nullptr, k) && k != base, "depthBiasEnable sens");
        r2 = r1;
        r2.depthBiasConstantFactor = 0.5f;
        CHECK(RKey::build(r2, m, nullptr, k) && k != base, "depthBiasConst sens");
        r2 = r1;
        r2.depthBiasClamp = 0.25f;
        CHECK(RKey::build(r2, m, nullptr, k) && k != base, "depthBiasClamp sens");
        r2 = r1;
        r2.depthBiasSlopeFactor = 2.0f;
        CHECK(RKey::build(r2, m, nullptr, k) && k != base, "depthBiasSlope sens");
        r2 = r1;
        r2.lineWidth = 2.0f;
        CHECK(RKey::build(r2, m, nullptr, k) && k != base, "lineWidth sens");
    }

    // --- 5: multisample sensitivity ---
    {
        auto r = defRaster();
        auto m1 = defMS();
        RKey base;
        CHECK(RKey::build(r, m1, nullptr, base), "ms base");
        auto m2 = m1;
        m2.flags = 1u;
        RKey k;
        CHECK(RKey::build(r, m2, nullptr, k) && k != base, "ms flags sens");
        m2 = m1;
        m2.rasterizationSamples = VK_SAMPLE_COUNT_2_BIT;
        CHECK(RKey::build(r, m2, nullptr, k) && k != base, "samples sens");
        m2 = m1;
        m2.sampleShadingEnable = VK_TRUE;
        CHECK(RKey::build(r, m2, nullptr, k) && k != base, "sampleShading sens");
        m2 = m1;
        m2.minSampleShading = 0.5f;
        CHECK(RKey::build(r, m2, nullptr, k) && k != base, "minSampleShading sens");
        m2 = m1;
        m2.alphaToCoverageEnable = VK_TRUE;
        CHECK(RKey::build(r, m2, nullptr, k) && k != base, "alphaToCoverage sens");
        m2 = m1;
        m2.alphaToOneEnable = VK_TRUE;
        CHECK(RKey::build(r, m2, nullptr, k) && k != base, "alphaToOne sens");
        m2 = m1;
        uint32_t mask0 = 0x12345678u;
        m2.pSampleMask = &mask0;
        CHECK(RKey::build(r, m2, nullptr, k) && k != base, "mask value sens");
        // NULL mask == all-ones
        auto m3 = m1;
        const uint32_t allOnes = 0xffffffffu;
        m3.pSampleMask = &allOnes;
        RKey kn, ka;
        CHECK(RKey::build(r, m1, nullptr, kn) && RKey::build(r, m3, nullptr, ka),
              "null/all-ones builds");
        CHECK(kn == ka && kn.hash() == ka.hash(), "NULL mask == all-ones");
    }

    // --- 6: depth-stencil sensitivity ---
    {
        auto r = defRaster();
        auto m = defMS();
        auto d1 = defDS();
        RKey base;
        CHECK(RKey::build(r, m, &d1, base), "ds base");
        auto d2 = d1;
        RKey k;
        d2.flags = 1u;
        CHECK(RKey::build(r, m, &d2, k) && k != base, "ds flags sens");
        d2 = d1;
        d2.depthTestEnable = VK_FALSE;
        CHECK(RKey::build(r, m, &d2, k) && k != base, "depthTest sens");
        d2 = d1;
        d2.depthWriteEnable = VK_FALSE;
        CHECK(RKey::build(r, m, &d2, k) && k != base, "depthWrite sens");
        d2 = d1;
        d2.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        CHECK(RKey::build(r, m, &d2, k) && k != base, "depthCompareOp sens");
        d2 = d1;
        d2.depthBoundsTestEnable = VK_TRUE;
        CHECK(RKey::build(r, m, &d2, k) && k != base, "depthBoundsTest sens");
        d2 = d1;
        d2.stencilTestEnable = VK_TRUE;
        CHECK(RKey::build(r, m, &d2, k) && k != base, "stencilTest sens");
        d2 = d1;
        d2.front.failOp = VK_STENCIL_OP_INCREMENT_AND_WRAP;
        CHECK(RKey::build(r, m, &d2, k) && k != base, "front failOp sens");
        d2 = d1;
        d2.front.passOp = VK_STENCIL_OP_ZERO;
        CHECK(RKey::build(r, m, &d2, k) && k != base, "front passOp sens");
        d2 = d1;
        d2.front.depthFailOp = VK_STENCIL_OP_REPLACE;
        CHECK(RKey::build(r, m, &d2, k) && k != base, "front depthFailOp sens");
        d2 = d1;
        d2.front.compareOp = VK_COMPARE_OP_EQUAL;
        CHECK(RKey::build(r, m, &d2, k) && k != base, "front compareOp sens");
        d2 = d1;
        d2.front.compareMask = 0xABu;
        CHECK(RKey::build(r, m, &d2, k) && k != base, "front compareMask sens");
        d2 = d1;
        d2.front.writeMask = 0xCDu;
        CHECK(RKey::build(r, m, &d2, k) && k != base, "front writeMask sens");
        d2 = d1;
        d2.front.reference = 7u;
        CHECK(RKey::build(r, m, &d2, k) && k != base, "front reference sens");
        d2 = d1;
        d2.back = d2.front;
        d2.back.failOp = VK_STENCIL_OP_DECREMENT_AND_WRAP;
        CHECK(RKey::build(r, m, &d2, k) && k != base, "back failOp sens");
        d2 = d1;
        d2.back.passOp = VK_STENCIL_OP_ZERO;
        CHECK(RKey::build(r, m, &d2, k) && k != base && k.hash() != base.hash(), "back passOp sens");
        d2 = d1;
        d2.back.depthFailOp = VK_STENCIL_OP_REPLACE;
        CHECK(RKey::build(r, m, &d2, k) && k != base && k.hash() != base.hash(), "back depthFailOp sens");
        d2 = d1;
        d2.back.compareOp = VK_COMPARE_OP_GREATER;
        CHECK(RKey::build(r, m, &d2, k) && k != base && k.hash() != base.hash(), "back compareOp sens");
        d2 = d1;
        d2.back.compareMask = 0x5Au;
        CHECK(RKey::build(r, m, &d2, k) && k != base && k.hash() != base.hash(), "back compareMask sens");
        d2 = d1;
        d2.back.writeMask = 0xA5u;
        CHECK(RKey::build(r, m, &d2, k) && k != base && k.hash() != base.hash(), "back writeMask sens");
        d2 = d1;
        d2.back.reference = 9u;
        CHECK(RKey::build(r, m, &d2, k) && k != base && k.hash() != base.hash(), "back reference sens");
        d2 = d1;
        d2.minDepthBounds = 0.1f;
        CHECK(RKey::build(r, m, &d2, k) && k != base, "minDepthBounds sens");
        d2 = d1;
        d2.maxDepthBounds = 0.9f;
        CHECK(RKey::build(r, m, &d2, k) && k != base, "maxDepthBounds sens");
    }

    // --- 7: float +/-0 bit fidelity ---
    {
        auto r = defRaster();
        auto m = defMS();
        RKey kn, kp;
        auto rn = r;
        rn.depthBiasConstantFactor = asFloat(0x80000000u); // -0.0f
        auto rp = r;
        rp.depthBiasConstantFactor = 0.0f; // +0.0f
        CHECK(RKey::build(rn, m, nullptr, kn), "neg zero build");
        CHECK(RKey::build(rp, m, nullptr, kp), "pos zero build");
        CHECK(kn != kp, "-0 != +0 by words");
        CHECK(kn.hash() != kp.hash(), "-0 != +0 by hash");
        CHECK(checkedWord(kn, 7) == 0x80000000u, "-0 bits preserved");
        CHECK(checkedWord(kp, 7) == 0x00000000u, "+0 bits preserved");
    }

    // --- 8: sample counts 1/2/4/8/16/32/64, mask words 1 or 2 ---
    {
        auto r = defRaster();
        auto m = defMS();
        const VkSampleCountFlagBits samples[] = {
            VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_2_BIT, VK_SAMPLE_COUNT_4_BIT,
            VK_SAMPLE_COUNT_8_BIT, VK_SAMPLE_COUNT_16_BIT, VK_SAMPLE_COUNT_32_BIT,
            VK_SAMPLE_COUNT_64_BIT};
        RKey prev;
        for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
            m.rasterizationSamples = samples[i];
            RKey k;
            CHECK(RKey::build(r, m, nullptr, k), "samples build");
            const uint32_t want = (1u << i) >= 64u ? 2u : 1u;
            // word count check
            const size_t expectWords = 11u + 7u + want + 1u;
            CHECK(k.words().size() == expectWords, "sample word count");
            if (i > 0) {
                CHECK(k != prev, "sample distinct");
                CHECK(k.hash() != prev.hash(), "sample hash distinct");
            }
            prev = k;
        }
        // 1/32/64: explicit two-different-word mask, second word single mutation, NULL == two all-ones
        {
            auto r = defRaster();
            auto m = defMS();
            m.rasterizationSamples = VK_SAMPLE_COUNT_64_BIT;
            const uint32_t two[2] = {0xAAAAAAAAu, 0x55555555u};
            m.pSampleMask = two;
            RKey base;
            CHECK(RKey::build(r, m, nullptr, base), "two-word mask base");
            CHECK(checkedWord(base, 16) == 0xAAAAAAAAu && checkedWord(base, 17) == 0x55555555u,
                  "two mask words stored in order");
            const uint32_t mut[2] = {0xAAAAAAAAu, 0x55555556u};
            m.pSampleMask = mut;
            RKey k;
            CHECK(RKey::build(r, m, nullptr, k) && k != base && k.hash() != base.hash(),
                  "second word single mutation sens");
            const uint32_t allOnes[2] = {0xffffffffu, 0xffffffffu};
            m.pSampleMask = allOnes;
            RKey kn, ka;
            CHECK(RKey::build(r, m, nullptr, kn), "null 64x build");
            m.pSampleMask = nullptr;
            CHECK(RKey::build(r, m, nullptr, ka), "all-ones 64x build");
            CHECK(kn == ka && kn.hash() == ka.hash(), "NULL == two all-ones (64x)");
            // 32x: one word
            m.rasterizationSamples = VK_SAMPLE_COUNT_32_BIT;
            const uint32_t one[1] = {0xCAFEBABEu};
            m.pSampleMask = one;
            RKey k32;
            CHECK(RKey::build(r, m, nullptr, k32), "32x explicit mask");
            CHECK(checkedWord(k32, 16) == 0xCAFEBABEu && k32.words().size() == 20u,
                  "32x one mask word");
            // 1x: one word
            m.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            const uint32_t single[1] = {0xDEADBEEFu};
            m.pSampleMask = single;
            RKey k1;
            CHECK(RKey::build(r, m, nullptr, k1) && checkedWord(k1, 16) == 0xDEADBEEFu,
                  "1x explicit mask");
        }

        // 64x: mask words must be 2 (both 0xffffffff when NULL)
        m.rasterizationSamples = VK_SAMPLE_COUNT_64_BIT;
        RKey k64;
        CHECK(RKey::build(r, m, nullptr, k64), "64x build");
        CHECK(checkedWord(k64, 16) == 0xffffffffu &&
                  checkedWord(k64, 17) == 0xffffffffu,
              "64x two mask words all-ones");
    }

    // --- 9: rejection cycles (prime -> bad -> full reset -> recover) ---
    {
        auto r = defRaster();
        auto m = defMS();
        auto d = defDS();
        RKey good;
        CHECK(RKey::build(r, m, &d, good), "reject prime");
        CHECK(good.valid(), "reject prime valid");

        // bad sType raster
        auto rb = r;
        rb.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        RKey k;
        CHECK(!RKey::build(rb, m, nullptr, k), "bad raster sType");
        CHECK(!k.valid() && k.words().empty(), "bad raster sType reset");
        CHECK(RKey::build(r, m, &d, k) && k.valid(), "recover raster sType");

        // bad pNext raster
        int dummy = 0;
        rb = r;
        rb.pNext = &dummy;
        CHECK(!RKey::build(rb, m, nullptr, k), "bad raster pNext");
        CHECK(!k.valid(), "bad raster pNext reset");

        // bad sType ms
        auto mb = m;
        mb.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        CHECK(!RKey::build(r, mb, nullptr, k), "bad ms sType");
        CHECK(!k.valid(), "bad ms sType reset");

        // bad pNext ms
        mb = m;
        mb.pNext = &dummy;
        CHECK(!RKey::build(r, mb, nullptr, k), "bad ms pNext");
        CHECK(!k.valid(), "bad ms pNext reset");

        // bad sType DS
        auto db = d;
        db.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        CHECK(!RKey::build(r, m, &db, k), "bad ds sType");
        CHECK(!k.valid(), "bad ds sType reset");

        // bad pNext DS
        db = d;
        db.pNext = &dummy;
        CHECK(!RKey::build(r, m, &db, k), "bad ds pNext");
        CHECK(!k.valid(), "bad ds pNext reset");

        // samples = 0
        mb = m;
        mb.rasterizationSamples = static_cast<VkSampleCountFlagBits>(0);
        CHECK(!RKey::build(r, mb, nullptr, k), "samples 0 reject");
        CHECK(!k.valid(), "samples 0 reset");

        // samples = 128
        mb = m;
        mb.rasterizationSamples = static_cast<VkSampleCountFlagBits>(128);
        CHECK(!RKey::build(r, mb, nullptr, k), "samples 128 reject");
        CHECK(!k.valid(), "samples 128 reset");

        // samples = 3 (non power of two)
        mb = m;
        mb.rasterizationSamples = static_cast<VkSampleCountFlagBits>(3);
        CHECK(!RKey::build(r, mb, nullptr, k), "samples 3 reject");
        CHECK(!k.valid(), "samples 3 reset");

        // recover full
        CHECK(RKey::build(r, m, &d, k) && k.valid(), "full recover");
        CHECK(k == good, "recover equals prime");
    }

    // --- 10: owning: caller mutation/realloc/destroy, two owners ---
    {
        auto r = defRaster();
        auto m = defMS();
        std::vector<uint32_t> maskVec{0x12345678u};
        m.rasterizationSamples = VK_SAMPLE_COUNT_2_BIT;
        m.pSampleMask = maskVec.data();
        RKey a;
        CHECK(RKey::build(r, m, nullptr, a), "owning build");
        // caller mutates + reallocs + destroys source
        maskVec[0] = 0xdeadbeefu;
        maskVec.push_back(42u);
        maskVec.resize(0);
        maskVec.shrink_to_fit();
        CHECK(checkedWord(a, 16) == 0x12345678u, "owning mask snapshot");
        // two owners: rebuild b with different mask content via public build
        std::vector<uint32_t> maskVec2{0x87654321u};
        m.pSampleMask = maskVec2.data();
        RKey b;
        CHECK(RKey::build(r, m, nullptr, b), "two-owner rebuild");
        CHECK(a != b, "two owners distinct content");
        CHECK(checkedWord(a, 16) == 0x12345678u, "a intact");
        CHECK(checkedWord(b, 16) == 0x87654321u, "b content");
        // b goes out of scope; a must survive untouched (verify below after scope)
        {
            std::vector<uint32_t> maskTmp{0x11111111u};
            m.pSampleMask = maskTmp.data();
            RKey tmpKey;
            CHECK(RKey::build(r, m, nullptr, tmpKey), "scoped owner build");
            CHECK(checkedWord(tmpKey, 16) == 0x11111111u, "scoped content");
        }
        CHECK(checkedWord(a, 16) == 0x12345678u && a.valid(), "a survives scoped destroy");
        // copy/move value semantics
        RKey c = a; // copy ctor
        CHECK(c == a && checkedWord(c, 16) == checkedWord(a, 16), "copy ctor value");
        RKey d;
        d = a; // copy assign
        CHECK(d == a, "copy assign value");
        RKey e = std::move(c); // move ctor
        CHECK(e == a && c.words().empty(), "move ctor steals words");
    }

    // --- 11: big -> small -> bad -> default -> recover ---
    {
        auto r = defRaster();
        auto m = defMS();
        m.rasterizationSamples = VK_SAMPLE_COUNT_64_BIT;
        RKey k;
        CHECK(RKey::build(r, m, nullptr, k), "big build");
        CHECK(k.words().size() > 16u, "big word count");
        m.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        CHECK(RKey::build(r, m, nullptr, k), "small build");
        CHECK(k.words().size() == 20u, "small word count");
        auto mb = m;
        mb.pNext = &r; // bad
        CHECK(!RKey::build(r, mb, nullptr, k), "bad build");
        CHECK(!k.valid() && k.words().empty(), "bad reset");
        CHECK(RKey::build(r, m, nullptr, k) && k.valid(), "recover");
    }

    // --- 12: constant-zero hash map (colliding keys coexist) ---
    {
        struct ZeroHash {
            size_t operator()(const RKey &k) const noexcept { return 0; }
        };
        std::unordered_map<RKey, int, ZeroHash> map;
        auto r = defRaster();
        auto m = defMS();
        RKey k1;
        CHECK(RKey::build(r, m, nullptr, k1), "map k1");
        auto r2 = r;
        r2.lineWidth = 3.0f;
        RKey k2;
        CHECK(RKey::build(r2, m, nullptr, k2), "map k2");
        map[k1] = 1;
        map[k2] = 2;
        CHECK(map.size() == 2, "map size 2 (constant hash)");
        CHECK(map.find(k1) != map.end() && map.find(k1)->second == 1, "map k1 found");
        RKey k3;
        auto r3 = r;
        r3.depthClampEnable = VK_TRUE;
        CHECK(RKey::build(r3, m, nullptr, k3), "map k3");
        CHECK(map.find(k3) == map.end(), "unstored key not found");
    }

    // --- 13: allocation failNth on fresh build ---
    // Fixture and expected/prime are OUTSIDE the injection guard.
    // After reserve(totalWords) the snapshot vector performs exactly one
    // allocation per fresh build; the copy-assignment throw test above already
    // proves operator new is observable. Here we verify point 1 end-to-end.
    {
        auto r = defRaster();
        auto ms = defMS();
        auto ds = defDS();
        RKey expected;
        CHECK(RKey::build(r, ms, &ds, expected), "alloc expected build");
        CHECK(expected.valid() && expected.words().size() == 42u, "alloc expected 42 words");

        RKey k;
        CHECK(RKey::build(r, ms, &ds, k), "failNth prime");
        g_leadInjections = 0;
        g_leadFailNextAllocation = true;
        bool ok = RKey::build(r, ms, &ds, k);
        g_leadFailNextAllocation = false;
        CHECK(g_leadInjections == 1, "failNth1 must actually inject; no fallback PASS");
        if (g_leadInjections == 1) {
            CHECK(!ok, "failNth1: build must return false");
            CHECK(!k.valid() && k.words().empty(), "failNth1: full reset");
            CHECK(k.hash() == RKey{}.hash(), "failNth1: default hash");
            CHECK(RKey::build(r, ms, &ds, k), "failNth1 recover");
            CHECK(k == expected && k.words() == expected.words(), "failNth1 recover tuple");
        }
    }

    // --- summary ---
    if (g_failures == 0) {
        std::printf("graphics raster key tests: all PASS\n");
        return 0;
    }
    std::printf("graphics raster key tests: %d FAIL\n", g_failures);
    return 1;
}
#endif // VKGRAPHICRASTER_STANDALONE_PROBE
