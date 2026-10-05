// Layout key owning value model CPU tests (M7-B1, rev3).
// Run contract #20 (19 old + this); probe variant compile-only.
// Includes allocation failNth coverage, known vectors, full-field mutation
// sensitivity, canonical ordering, scoped destruction, and map integration.

#if defined(VKLAYOUTKEY_STANDALONE_PROBE)
    #include "VKLayoutKeys.h"
int main() { return 0; }
#else

    #include <cstdio>
    #include <cstdlib>
    #include <cstring>
    #include <functional>
    #include <new>
    #include <type_traits>
    #include <unordered_map>
    #include "VKLayoutKeys.h"

using cc::gfx::VulkanDescriptorSetLayoutKey;
using cc::gfx::VulkanPipelineLayoutKey;
using DSKey = VulkanDescriptorSetLayoutKey;
using PLKey = VulkanPipelineLayoutKey;
using DSBinding = VulkanDescriptorSetLayoutKey::Binding;
using PushRange = VulkanPipelineLayoutKey::PushRange;

// ---- allocation counter + single-shot bad_alloc (test-only) ----
static long g_allocCount = 0;
static bool g_counting = false;
static long g_failNth = 0;
static long g_injectedFailures = 0;

void *operator new(size_t size) {
    if (g_counting) {
        ++g_allocCount;
        if (g_failNth > 0 && g_allocCount >= g_failNth) {
            g_failNth = 0;
            ++g_injectedFailures;
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
struct AllocGuard {
    explicit AllocGuard(long failNth = 0) {
        g_allocCount = 0;
        g_injectedFailures = 0;
        g_failNth = failNth;
        g_counting = true;
    }
    ~AllocGuard() {
        g_counting = false;
        g_failNth = 0;
    }
};

unsigned g_failures = 0;
void check(bool ok, const char *w) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", w);
        ++g_failures;
    }
}

VkDescriptorSetLayoutBinding makeBinding(uint32_t b) {
    VkDescriptorSetLayoutBinding r = {};
    r.binding = b;
    r.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    r.descriptorCount = 1;
    r.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    return r;
}
VkDescriptorSetLayoutCreateInfo makeDsInfo(VkDescriptorSetLayoutBinding *bindings, uint32_t count) {
    VkDescriptorSetLayoutCreateInfo i = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    i.bindingCount = count;
    i.pBindings = bindings;
    return i;
}
VkPipelineLayoutCreateInfo makePlInfo(const VkDescriptorSetLayout *layouts, uint32_t count) {
    VkPipelineLayoutCreateInfo i = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    i.setLayoutCount = count;
    i.pSetLayouts = layouts;
    return i;
}

// full default verification
void verifyDsDefault(const DSKey &k) {
    check(!k.valid(), "dsDef: not valid");
    check(k.flags() == 0, "dsDef: flags 0");
    check(k.bindings().empty(), "dsDef: no bindings");
}
void verifyPlDefault(const PLKey &k) {
    check(!k.valid(), "plDef: not valid");
    check(k.flags() == 0, "plDef: flags 0");
    check(k.setLayoutCount() == 0, "plDef: setCount 0");
    check(k.setLayouts().empty(), "plDef: no children");
    check(k.pushConstantRanges().empty(), "plDef: no push");
}

// full DS verification against expected content
void verifyDs(const DSKey &k, bool wantValid, VkDescriptorSetLayoutCreateFlags wantFlags,
              size_t wantColorCount, const uint32_t *wantBindings, const VkDescriptorType *wantTypes,
              const uint32_t *wantCounts, const VkShaderStageFlags *wantStages) {
    check(k.valid() == wantValid, "vds: valid");
    check(k.flags() == wantFlags, "vds: flags");
    if (k.bindings().size() != wantColorCount) {
        check(false, "vds: bindings size");
        return;
    }
    for (size_t i = 0; i < wantColorCount; ++i) {
        check(k.bindings()[i].binding == wantBindings[i], "vds: binding num");
        check(k.bindings()[i].descriptorType == wantTypes[i], "vds: type");
        check(k.bindings()[i].descriptorCount == wantCounts[i], "vds: count");
        check(k.bindings()[i].stageFlags == wantStages[i], "vds: stage");
    }
}

// full PL verification
void verifyPl(const PLKey &k, bool wantValid, VkPipelineLayoutCreateFlags wantFlags,
              uint32_t wantSetCount, const std::vector<DSKey> &wantChildren,
              const std::vector<PushRange> &wantPush) {
    check(k.valid() == wantValid, "vpl: valid");
    check(k.flags() == wantFlags, "vpl: flags");
    check(k.setLayoutCount() == wantSetCount, "vpl: setCount");
    check(k.setLayouts().size() == wantChildren.size(), "vpl: children size");
    for (size_t i = 0; i < wantChildren.size() && i < k.setLayouts().size(); ++i)
        check(k.setLayouts()[i] == wantChildren[i], "vpl: child content");
    check(k.pushConstantRanges().size() == wantPush.size(), "vpl: push size");
    for (size_t i = 0; i < wantPush.size() && i < k.pushConstantRanges().size(); ++i)
        check(k.pushConstantRanges()[i] == wantPush[i], "vpl: push content");
}

// known empty hash vectors (independently computed via JS BigInt FNV-1a)
constexpr uint64_t DS_EMPTY_HASH = 0x6613d142c55a6ff4ULL;
constexpr uint64_t PL_EMPTY_HASH = 0x3bf2ec3465ed9737ULL;

} // namespace

int main() {
    const VkDescriptorSetLayout dsFake = reinterpret_cast<VkDescriptorSetLayout>(static_cast<uintptr_t>(0xD001));
    static_assert(std::is_copy_constructible<DSKey>::value, "DS copy");
    static_assert(std::is_move_constructible<PLKey>::value, "PL move");

    // --- 1: default invalid ---
    {
        DSKey dk;
        PLKey pk;
        verifyDsDefault(dk);
        verifyPlDefault(pk);
    }

    // --- 2: known empty vectors ---
    {
        VkDescriptorSetLayoutCreateInfo empty = makeDsInfo(nullptr, 0);
        DSKey dk;
        check(DSKey::build(empty, dk), "known ds empty build");
        check(dk.valid() && dk.bindings().empty(), "known ds empty valid");
        check(dk.hash() == DS_EMPTY_HASH, "known ds empty hash exact");
        std::vector<DSKey> kids;
        VkPipelineLayoutCreateInfo plInfo = makePlInfo(nullptr, 0);
        PLKey pk;
        check(PLKey::build(plInfo, kids, pk), "known pl empty build");
        check(pk.valid() && pk.setLayoutCount() == 0, "known pl empty valid");
        check(pk.hash() == PL_EMPTY_HASH, "known pl empty hash exact");
    }

    // --- 3: basic DS build with bindings, flags ---
    {
        VkDescriptorSetLayoutBinding binds[] = {makeBinding(0), makeBinding(1)};
        binds[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        auto info = makeDsInfo(binds, 2);
        info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        DSKey k;
        check(DSKey::build(info, k), "ds basic build");
        check(k.valid(), "ds valid");
        check(k.flags() == info.flags, "ds flags");
        check(k.bindings().size() == 2, "ds count");
        check(k.bindings()[0].binding == 0, "ds b0");
        check(k.bindings()[1].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, "ds b1 type");
        check(k.bindings()[1].stageFlags == VK_SHADER_STAGE_COMPUTE_BIT, "ds b1 stage");
    }

    // --- 4: canonical reorder ---
    {
        VkDescriptorSetLayoutBinding b1[] = {makeBinding(0), makeBinding(1), makeBinding(2)};
        VkDescriptorSetLayoutBinding b2[] = {makeBinding(2), makeBinding(0), makeBinding(1)};
        auto i1 = makeDsInfo(b1, 3);
        auto i2 = makeDsInfo(b2, 3);
        DSKey a, b;
        check(DSKey::build(i1, a) && DSKey::build(i2, b), "reorder builds");
        check(a == b, "reorder canonical equal");
        check(a.hash() == b.hash(), "reorder hash equal");
        VkDescriptorSetLayoutBinding b3[] = {makeBinding(0), makeBinding(2), makeBinding(1)};
        auto i3 = makeDsInfo(b3, 3);
        DSKey c;
        check(DSKey::build(i3, c), "swap build");
        check(c == a, "swap canonical equal (same binding content, sorted)");
        check(c.hash() == a.hash(), "swap canonical hash equal");
    }

    // --- 5: PL with children and push ---
    {
        VkDescriptorSetLayoutBinding binds[] = {makeBinding(0), makeBinding(1)};
        auto dsInfo = makeDsInfo(binds, 2);
        DSKey dsKey;
        check(DSKey::build(dsInfo, dsKey), "pl ds build");
        std::vector<DSKey> children = {dsKey};
        const VkDescriptorSetLayout dsFake = reinterpret_cast<VkDescriptorSetLayout>(static_cast<uintptr_t>(0xD001));
        auto plInfo = makePlInfo(&dsFake, 1);
        plInfo.flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT;
        plInfo.pushConstantRangeCount = 1;
        VkPushConstantRange push = {};
        push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        push.offset = 0;
        push.size = 64;
        plInfo.pPushConstantRanges = &push;
        PLKey k;
        check(PLKey::build(plInfo, children, k), "pl build");
        check(k.valid(), "pl valid");
        check(k.flags() == plInfo.flags, "pl flags");
        check(k.setLayoutCount() == 1, "pl setCount");
        check(k.setLayouts().size() == 1 && k.setLayouts()[0] == dsKey, "pl children");
        check(k.pushConstantRanges().size() == 1, "pl push count");
        check(k.pushConstantRanges()[0].stageFlags == VK_SHADER_STAGE_VERTEX_BIT, "pl push stage");
        check(k.pushConstantRanges()[0].size == 64, "pl push size");
        // verify hash matches by rebuilding from the published values
        PLKey k2;
        check(PLKey::build(plInfo, k.setLayouts(), k2), "pl hash rebuild");
        check(k2.hash() == k.hash(), "pl hash deterministic");
    }

    // --- 6: PL empty ---
    {
        std::vector<DSKey> children;
        VkPipelineLayoutCreateInfo info = makePlInfo(nullptr, 0);
        PLKey k;
        check(PLKey::build(info, children, k), "pl empty build");
        check(k.valid() && k.setLayoutCount() == 0 && k.pushConstantRanges().empty(), "pl empty valid");
    }

    // --- 7: owning deep copy: caller mutation + scope destruction ---
    {
        VkDescriptorSetLayoutBinding binds[] = {makeBinding(0), makeBinding(1)};
        auto dsInfo = makeDsInfo(binds, 2);
        DSKey dsKey;
        check(DSKey::build(dsInfo, dsKey), "own ds build");
        std::vector<DSKey> children = {dsKey};
        const VkDescriptorSetLayout dsFake = reinterpret_cast<VkDescriptorSetLayout>(static_cast<uintptr_t>(0xD001));
        auto plInfo = makePlInfo(&dsFake, 1);
        PLKey pl;
        check(PLKey::build(plInfo, children, pl), "own pl build");
        // mutate caller data
        children[0] = DSKey{};
        binds[0].descriptorCount = 99;
        // DS key and PL children should be unaffected
        check(pl.setLayouts().size() == 1 && pl.setLayouts()[0].valid(), "pl children unaffected");
        check(dsKey.valid() && dsKey.bindings().size() == 2 && dsKey.bindings()[0].descriptorCount == 1,
              "ds deep copy unaffected by caller");
    }

    // --- 8: two owners independent ---
    {
        VkDescriptorSetLayoutBinding binds[] = {makeBinding(0), makeBinding(1)};
        auto dsInfo = makeDsInfo(binds, 2);
        DSKey dsA, dsB;
        check(DSKey::build(dsInfo, dsA), "dsA");
        binds[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        check(DSKey::build(dsInfo, dsB), "dsB");
        check(dsA != dsB, "owners independent");
        check(dsA.hash() != dsB.hash(), "owners hash differ");
    }

    // --- 9: self-input: build(info, key.setLayouts(), key) ---
    {
        VkDescriptorSetLayoutBinding binds[] = {makeBinding(0), makeBinding(1)};
        auto dsInfo = makeDsInfo(binds, 2);
        DSKey dsKey;
        check(DSKey::build(dsInfo, dsKey), "self ds build");
        std::vector<DSKey> children = {dsKey};
        const VkDescriptorSetLayout dsFake = reinterpret_cast<VkDescriptorSetLayout>(static_cast<uintptr_t>(0xD001));
        auto plInfo = makePlInfo(&dsFake, 1);
        PLKey pl;
        check(PLKey::build(plInfo, children, pl), "self pl prime");
        std::vector<DSKey> ownChildren = pl.setLayouts();
        check(PLKey::build(plInfo, ownChildren, pl), "self pl rebuild");
        check(pl.valid() && pl.setLayouts().size() == 1, "self pl rebuild valid");
        const PLKey expected = pl;
        check(PLKey::build(plInfo, pl.setLayouts(), pl), "lead: true aliased children rebuild");
        check(pl == expected, "lead: true alias preserves complete layout key");
    }

    // --- 10: rejections: prime -> bad -> full clear -> recover ---
    {
        VkDescriptorSetLayoutBinding binds[] = {makeBinding(0), makeBinding(1)};
        auto good = makeDsInfo(binds, 2);
        DSKey k;
        DSKey expected;
        check(DSKey::build(good, expected), "DS rejection baseline");
        auto cycle = [&](const char *label, const VkDescriptorSetLayoutCreateInfo &bad) {
            check(DSKey::build(good, k), label);
            check(k == expected, label);
            check(!DSKey::build(bad, k), label);
            verifyDsDefault(k);
            check(DSKey::build(good, k), label);
            check(k == expected, label);
        };
        {
            auto t = good;
            t.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            cycle("ds sType", t);
        }
        {
            auto t = good;
            t.pNext = &t;
            cycle("ds pNext", t);
        }
        {
            auto t = makeDsInfo(nullptr, 1);
            cycle("ds count1 NULL pBindings", t);
        }
        {
            auto t = makeDsInfo(binds, 4097);
            cycle("ds count 4097", t);
        }
        {
            VkDescriptorSetLayoutBinding mutBinds[] = {makeBinding(0), makeBinding(1)};
            static VkSampler dummySampler = VK_NULL_HANDLE;
            mutBinds[0].pImmutableSamplers = &dummySampler;
            VkDescriptorSetLayoutCreateInfo t = makeDsInfo(mutBinds, 2);
            cycle("ds immutable", t);
        }
        {
            VkDescriptorSetLayoutBinding dup[] = {makeBinding(0), makeBinding(0)};
            auto t = makeDsInfo(dup, 2);
            cycle("ds duplicate binding", t);
        }

        // PL rejections
        const VkDescriptorSetLayout dsFake = reinterpret_cast<VkDescriptorSetLayout>(static_cast<uintptr_t>(0xD001));
        VkDescriptorSetLayoutBinding plBinds[] = {makeBinding(0)};
        auto plDsInfo = makeDsInfo(plBinds, 1);
        DSKey goodChild;
        check(DSKey::build(plDsInfo, goodChild), "pl prime child");
        std::vector<DSKey> goodChildren = {goodChild};
        VkPushConstantRange pushRange = {};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.offset = 0;
        pushRange.size = 64;
        VkPipelineLayoutCreateInfo plGood = makePlInfo(&dsFake, 1);
        plGood.flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT;
        plGood.pushConstantRangeCount = 1;
        plGood.pPushConstantRanges = &pushRange;
        PLKey pk;
        PLKey expectedPl;
        check(PLKey::build(plGood, goodChildren, expectedPl), "PL rejection baseline");
        auto plPrime = [&](const char *label) {
            check(PLKey::build(plGood, goodChildren, pk), label);
            check(pk == expectedPl, label);
        };
        auto plCycle = [&](const char *label, VkPipelineLayoutCreateInfo info,
                           const std::vector<DSKey> &kids) {
            plPrime(label);
            check(!PLKey::build(info, kids, pk), label);
            verifyPlDefault(pk);
            plPrime(label);
        };
        {
            auto t = plGood;
            t.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            plCycle("pl sType", t, goodChildren);
        }
        {
            auto t = plGood;
            t.pNext = &t;
            plCycle("pl pNext", t, goodChildren);
        }
        plCycle("pl setcount mismatch", plGood, {});
        {
            auto t = makePlInfo(nullptr, 1);
            plCycle("pl NULL layouts", t, goodChildren);
        }
        {
            auto t = plGood;
            t.pushConstantRangeCount = 1;
            t.pPushConstantRanges = nullptr;
            plCycle("pl push NULL", t, goodChildren);
        }
        {
            auto t = plGood;
            t.pushConstantRangeCount = 129;
            VkPushConstantRange dummy{};
            t.pPushConstantRanges = &dummy;
            plCycle("pl push 129", t, goodChildren);
        }
        {
            std::vector<DSKey> badKids = {DSKey{}}; // invalid child
            auto t = makePlInfo(&dsFake, 1);
            t.setLayoutCount = 1;
            t.pSetLayouts = &dsFake;
            plCycle("pl child invalid", t, badKids);
        }
    }

    // --- 11: unordered_map with custom hasher ---
    {
        struct DSHash {
            size_t operator()(const DSKey &) const { return 0; }
        };
        std::unordered_map<DSKey, int, DSHash> map;
        VkDescriptorSetLayoutBinding b1[] = {makeBinding(0)};
        VkDescriptorSetLayoutBinding b2[] = {makeBinding(1)};
        auto i1 = makeDsInfo(b1, 1);
        auto i2 = makeDsInfo(b2, 1);
        DSKey k1, k2;
        check(DSKey::build(i1, k1) && DSKey::build(i2, k2), "map builds");
        map[k1] = 42;
        map[k2] = 99;
        check(map.size() == 2, "map size 2");
        auto it1 = map.find(k1);
        auto it2 = map.find(k2);
        check(it1 != map.end() && it1->second == 42, "map find k1");
        check(it2 != map.end() && it2->second == 99, "map find k2");
        DSKey k3;
        VkDescriptorSetLayoutBinding b3[] = {makeBinding(9)};
        auto i3 = makeDsInfo(b3, 1);
        check(DSKey::build(i3, k3), "map k3 build");
        check(map.find(k3) == map.end(), "unstored key not found");
        struct PLHash {
            size_t operator()(const PLKey &) const { return 0; }
        };
        std::unordered_map<PLKey, int, PLHash> pipelines;
        auto plInfo = makePlInfo(&dsFake, 1);
        PLKey p1, p2, p3;
        check(PLKey::build(plInfo, {k1}, p1) && PLKey::build(plInfo, {k2}, p2) &&
                  PLKey::build(plInfo, {k3}, p3),
              "collision: PL fixtures");
        pipelines.emplace(p1, 42);
        pipelines.emplace(p2, 99);
        const auto a = pipelines.find(p1), b = pipelines.find(p2);
        check(pipelines.size() == 2 && a != pipelines.end() && a->second == 42 &&
                  b != pipelines.end() && b->second == 99 && pipelines.find(p3) == pipelines.end(),
              "collision: PL full equality rejects wrong bucket entry");
    }

    // --- 13: big -> small -> empty -> bad -> recover, full getter values ---
    {
        VkDescriptorSetLayoutBinding bigBinds[3] = {makeBinding(0), makeBinding(1), makeBinding(2)};
        bigBinds[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bigBinds[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        auto bigInfo = makeDsInfo(bigBinds, 3);
        DSKey k;
        check(DSKey::build(bigInfo, k), "seq big");
        check(k.bindings().size() == 3, "seq big count");
        VkDescriptorSetLayoutBinding smallBinds[1] = {makeBinding(0)};
        auto smallInfo = makeDsInfo(smallBinds, 1);
        check(DSKey::build(smallInfo, k), "seq small");
        check(k.bindings().size() == 1, "seq small count");
        VkDescriptorSetLayoutCreateInfo emptyInfo = makeDsInfo(nullptr, 0);
        check(DSKey::build(emptyInfo, k), "seq empty");
        check(k.bindings().empty() && k.valid(), "seq empty valid");
        auto badInfo = makeDsInfo(nullptr, 1);
        check(!DSKey::build(badInfo, k), "seq bad");
        verifyDsDefault(k);
        check(DSKey::build(bigInfo, k), "seq recover");
        check(k.bindings().size() == 3, "seq recover count");
    }

    // Lead: independent known streams, single-field changes, ownership, and
    // every allocation failure. All handles below are CPU-only sentinels.
    {
        check(DSKey{}.hash() == 0x5b5754e32028a8a5ULL, "known DS invalid stream");
        check(PLKey{}.hash() == 0xecf4d66c52d87ef6ULL, "known PL invalid stream");
        VkDescriptorSetLayoutBinding bindings[] = {makeBinding(9), makeBinding(2)};
        bindings[0].descriptorCount = 7;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[1].descriptorCount = 3;
        bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        auto dsInfo = makeDsInfo(bindings, 2);
        dsInfo.flags = 5; // value-model fixture, not a device-support claim
        DSKey ds;
        check(DSKey::build(dsInfo, ds), "lead: non-symmetric DS fixture");
        const uint32_t nums[] = {2, 9}, counts[] = {3, 7};
        const VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
        const VkShaderStageFlags stages[] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
        verifyDs(ds, true, 5, 2, nums, types, counts, stages);
        for (int field = 0; field < 5; ++field) {
            auto changed = dsInfo;
            VkDescriptorSetLayoutBinding modified[] = {bindings[0], bindings[1]};
            changed.pBindings = modified;
            switch (field) {
                case 0: changed.flags ^= 2; break;
                case 1: modified[0].binding = 11; break;
                case 2: modified[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; break;
                case 3: ++modified[0].descriptorCount; break;
                case 4: modified[0].stageFlags |= VK_SHADER_STAGE_COMPUTE_BIT; break;
            }
            DSKey variant;
            check(DSKey::build(changed, variant) && variant != ds && variant.hash() != ds.hash(),
                  "lead: independent DS field mutation");
        }
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 12, 20};
        auto plInfo = makePlInfo(&dsFake, 1);
        plInfo.flags = 3;
        plInfo.pushConstantRangeCount = 1;
        plInfo.pPushConstantRanges = &push;
        PLKey pl;
        check(PLKey::build(plInfo, {ds}, pl), "lead: nested fixture");
        // Independently encoded LE u32 stream (JS BigInt):
        // 2,1,1,3,1, 1,1,1,5,2, 2,6,3,1, 9,1,7,16, 1,1,12,20.
        check(pl.hash() == 0xd3d8f9ad2256f352ULL, "known nested PL full child stream");
        for (int field = 0; field < 4; ++field) {
            auto changed = plInfo;
            auto modified = push;
            changed.pPushConstantRanges = &modified;
            switch (field) {
                case 0: changed.flags ^= 4; break;
                case 1: modified.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; break;
                case 2: modified.offset += 4; break;
                case 3: modified.size += 4; break;
            }
            PLKey variant;
            check(PLKey::build(changed, {ds}, variant) && variant != pl && variant.hash() != pl.hash(),
                  "lead: independent PL field mutation");
        }
        DSKey other;
        auto changedDs = dsInfo;
        changedDs.flags ^= 2;
        check(DSKey::build(changedDs, other), "lead: distinct child fixture");
        VkDescriptorSetLayout handles[] = {dsFake, dsFake};
        auto twoSets = plInfo;
        twoSets.setLayoutCount = 2;
        twoSets.pSetLayouts = handles;
        const std::vector<DSKey> kids{ds, other};
        PLKey two, reversed;
        check(PLKey::build(twoSets, kids, two) && PLKey::build(twoSets, {other, ds}, reversed) &&
                  two != reversed && two.hash() != reversed.hash(),
              "lead: ordered sets affect key");
        PLKey changedChild;
        check(PLKey::build(plInfo, {other}, changedChild) && changedChild != pl &&
                  changedChild.hash() != pl.hash(),
              "lead: child content affects parent key");

        PLKey surviving;
        {
            auto callerKids = kids;
            auto callerPush = push;
            auto callerInfo = twoSets;
            callerInfo.pPushConstantRanges = &callerPush;
            check(PLKey::build(callerInfo, callerKids, surviving), "lead: scoped owning build");
            const bool isolated = surviving.setLayouts().size() == 2 &&
                                  surviving.setLayouts().data() != callerKids.data() &&
                                  surviving.setLayouts()[0].bindings().data() != callerKids[0].bindings().data();
            check(isolated, "lead: nested child arrays do not borrow input");
            if (isolated) {
                callerKids.clear();
                callerKids.resize(30);
            }
            callerPush.size = 99;
        }
        verifyPl(surviving, true, 3, 2, kids, {{VK_SHADER_STAGE_VERTEX_BIT, 12, 20}});
        PLKey independent = surviving;
        check(independent.setLayouts().data() != surviving.setLayouts().data() &&
                  independent.setLayouts()[0].bindings().data() != surviving.setLayouts()[0].bindings().data(),
              "lead: copying nested key keeps separate arrays");
        const PLKey expected = surviving;
        check(PLKey::build(twoSets, surviving.setLayouts(), surviving) && surviving == expected,
              "lead: alias rebuild includes flags and push data");
        auto emptyInfo = makePlInfo(nullptr, 0);
        check(PLKey::build(plInfo, {ds}, surviving) && surviving == pl, "lead: shrink full tuple");
        check(PLKey::build(emptyInfo, {}, surviving), "lead: empty after shrink");
        verifyPl(surviving, true, 0, 0, {}, {});
        auto bad = twoSets;
        bad.pSetLayouts = nullptr;
        check(!PLKey::build(bad, kids, surviving), "lead: failed rebuild");
        verifyPlDefault(surviving);
        check(PLKey::build(twoSets, kids, surviving) && surviving == expected && independent == expected,
              "lead: recover and leave second owner intact");

        // Fresh local builder storage is allocated on every invocation; prime
        // the output OUTSIDE the guard so capacity cannot shift failNth.
        DSKey measuredDs;
        long dsAllocations = 0;
        bool measured = false;
        {
            AllocGuard guard;
            measured = DSKey::build(dsInfo, measuredDs);
            dsAllocations = g_allocCount;
        }
        check(measured && dsAllocations > 0, "lead: count actual DS allocations");
        for (long n = 1; n <= dsAllocations; ++n) {
            DSKey output = ds;
            bool accepted;
            long injections;
            {
                AllocGuard guard(n);
                accepted = DSKey::build(dsInfo, output);
                injections = g_injectedFailures;
            }
            check(!accepted && injections == 1, "lead: injected DS allocation returns false");
            verifyDsDefault(output);
            check(DSKey::build(dsInfo, output) && output == ds, "lead: DS allocation recovery");
        }
        PLKey measuredPl;
        long plAllocations = 0;
        {
            AllocGuard guard;
            measured = PLKey::build(twoSets, kids, measuredPl);
            plAllocations = g_allocCount;
        }
        check(measured && plAllocations > 0, "lead: count actual nested PL allocations");
        for (long n = 1; n <= plAllocations; ++n) {
            PLKey output = expected;
            bool accepted;
            long injections;
            {
                AllocGuard guard(n);
                accepted = PLKey::build(twoSets, output.setLayouts(), output);
                injections = g_injectedFailures;
            }
            check(!accepted && injections == 1, "lead: alias allocation returns false");
            verifyPlDefault(output);
            check(PLKey::build(twoSets, kids, output) && output == expected, "lead: PL allocation recovery");
        }
        std::printf("layout allocation coverage: DS=%ld PL=%ld all points injected\n", dsAllocations, plAllocations);
    }
    if (g_failures == 0) {
        std::printf("layout key tests: all PASS\n");
        return 0;
    }
    std::printf("layout key tests: %u FAIL\n", g_failures);
    return 1;
}
#endif
