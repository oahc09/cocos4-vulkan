// Compute pipeline key owning value model CPU tests (M7-D1).
// Run contract #22 (21 old + this); probe variant compile-only.
// Includes allocation failNth coverage and known-vector assertions.

#if defined(VKCOMPUTEKEY_STANDALONE_PROBE)
    #include "VKComputePipelineKey.h"
int main() { return 0; }
#else

    #include <cstdio>
    #include <cstdlib>
    #include <new>
    #include <string>
    #include <type_traits>
    #include <unordered_map>
    #include <utility>
    #include <vector>
    #include "VKComputePipelineKey.h"

using cc::gfx::VulkanComputePipelineKey;
using cc::gfx::VulkanDescriptorSetLayoutKey;
using cc::gfx::VulkanPipelineLayoutKey;
using cc::gfx::VulkanShaderStageKey;
using DSKey = VulkanDescriptorSetLayoutKey;
using PLKey = VulkanPipelineLayoutKey;
using CPKey = VulkanComputePipelineKey;

// ---- allocation counter + single-shot bad_alloc (test-only) ----
static long g_allocCount = 0;
static bool g_counting = false;
static long g_failNth = 0;
static long g_injections = 0;

void *operator new(size_t size) {
    if (g_counting) {
        ++g_allocCount;
        if (g_failNth > 0 && g_allocCount >= g_failNth) {
            g_failNth = 0;
            ++g_injections;
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
        g_injections = 0;
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

VulkanShaderStageKey makeShader(const char *entry, std::vector<uint32_t> words) {
    VkPipelineShaderStageCreateInfo info = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.pName = entry;
    info.module = reinterpret_cast<VkShaderModule>(static_cast<uintptr_t>(0xC001));
    VulkanShaderStageKey k;
    check(VulkanShaderStageKey::build(info, words.data(), words.size(), k), "makeShader");
    return k;
}

VulkanPipelineLayoutKey makeLayout(uint32_t bindingCount, uint32_t pushCount,
                                   uint32_t bindingBase = 0, uint32_t pushOffset = 0) {
    VkDescriptorSetLayoutBinding binds[2] = {};
    for (uint32_t i = 0; i < bindingCount && i < 2; ++i) {
        binds[i].binding = bindingBase + i;
        binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkPushConstantRange push = {};
    push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push.offset = pushOffset;
    push.size = 64;
    static VkDescriptorSetLayout fakeLayouts[2] = {
        reinterpret_cast<VkDescriptorSetLayout>(static_cast<uintptr_t>(0xE001)),
        reinterpret_cast<VkDescriptorSetLayout>(static_cast<uintptr_t>(0xE002))};
    VkPipelineLayoutCreateInfo plInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plInfo.setLayoutCount = bindingCount;
    plInfo.pSetLayouts = fakeLayouts;
    plInfo.pushConstantRangeCount = pushCount;
    plInfo.pPushConstantRanges = pushCount > 0 ? &push : nullptr;
    PLKey k;
    std::vector<VulkanDescriptorSetLayoutKey> dsKeys;
    for (uint32_t i = 0; i < bindingCount && i < 2; ++i) {
        DSKey dk;
        VkDescriptorSetLayoutCreateInfo di = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        di.bindingCount = 1;
        di.pBindings = &binds[i];
        check(cc::gfx::VulkanDescriptorSetLayoutKey::build(di, dk), "makeLayout child");
        dsKeys.push_back(dk);
    }
    check(PLKey::build(plInfo, dsKeys, k), "makeLayout pl");
    return k;
}

void verifyValue(const CPKey &key, VkPipelineCreateFlags flags,
                 const VulkanShaderStageKey &shader, const PLKey &layout, const char *label) {
    check(key.valid() && key.flags() == flags && key.shader() == shader && key.layout() == layout, label);
}

void verifyCleared(const CPKey &key, const char *label) {
    check(key == CPKey{} && !key.valid() && key.flags() == 0 &&
              key.shader() == VulkanShaderStageKey{} && key.layout() == PLKey{} &&
              key.hash() == 0xa4a776ca92d79e50ULL,
          label);
}

} // namespace

int main() {
    static_assert(std::is_copy_constructible<CPKey>::value, "CPKey copy");
    static_assert(std::is_move_constructible<CPKey>::value, "CPKey move");

    // --- 1: default invalid ---
    {
        CPKey k;
        check(!k.valid(), "default invalid");
        check(k.flags() == 0, "default flags 0");
        check(k.hash() == 0xa4a776ca92d79e50ULL, "Lead independent default hash vector");
    }

    // --- 2: basic build + full getters ---
    {
        auto shader = makeShader("main", {0x07230203, 0x00010000, 0, 1, 0});
        auto layout = makeLayout(1, 1);
        CPKey k;
        check(CPKey::build(0x42, shader, layout, k), "basic build");
        check(k.valid(), "basic valid");
        check(k.flags() == 0x42, "basic flags");
        check(k.shader() == shader, "basic shader content");
        check(k.layout() == layout, "basic layout content");
        const CPKey expected = k;
        check(CPKey::build(k.flags(), k.shader(), k.layout(), k), "Lead true getter alias build");
        check(k == expected && k.hash() == expected.hash(), "Lead true getter alias preserves full tuple");
    }

    // --- 3: default invalid vs built-empty-layout valid compute are different ---
    {
        CPKey k;
        auto shader = makeShader("main", {0x07230203, 0x00010000, 0, 1, 0});
        auto emptyLayout = makeLayout(0, 0);
        check(CPKey::build(0, shader, emptyLayout, k), "empty layout valid compute");
        check(k.valid() && k.layout().setLayoutCount() == 0, "empty layout: valid, setCount 0");
        CPKey def;
        check(k != def, "built != default");
        check(k.hash() == 0xf9f23734ca303383ULL, "Lead independent compute empty-layout hash vector");
    }

    // --- 4: single-field mutation sensitivity ---
    {
        auto shader = makeShader("main", {0x07230203, 0x00010000, 0, 1, 0});
        auto layout = makeLayout(1, 1);
        CPKey base;
        check(CPKey::build(0x42, shader, layout, base), "mutation base");

        // flags
        {
            CPKey k;
            check(CPKey::build(0x43, shader, layout, k), "flags build");
            check(k != base, "flags sensitivity eq");
            check(k.hash() != base.hash(), "flags hash");
        }
        // shader entry
        {
            auto s2 = makeShader("other", {0x07230203, 0x00010000, 0, 1, 0});
            CPKey k;
            check(CPKey::build(0x42, s2, layout, k), "entry build");
            check(k != base, "entry sensitivity eq");
            check(k.hash() != base.hash(), "entry hash");
        }
        // One SPIR-V word changes, with all other fields held constant.
        {
            auto changed = makeShader("main", {0x07230203, 0x00010000, 0, 2, 0});
            CPKey key;
            check(CPKey::build(0x42, changed, layout, key), "SPIR-V mutation build");
            check(key != base && key.hash() != base.hash(), "SPIR-V mutation sensitivity");
        }
        // One binding number changes; ordered set count is unchanged.
        {
            auto l2 = makeLayout(1, 1, 3);
            CPKey k;
            check(CPKey::build(0x42, shader, l2, k), "layout build");
            check(k != base, "layout sensitivity eq");
            check(k.hash() != base.hash(), "layout hash");
        }
        {
            auto changed = makeLayout(1, 1, 0, 4);
            CPKey key;
            check(CPKey::build(0x42, shader, changed, key), "push offset mutation build");
            check(key != base && key.hash() != base.hash(), "push offset mutation sensitivity");
        }
    }

    // --- 5: different module handle + same content -> same key ---
    {
        auto s1 = makeShader("main", {0x07230203, 0x00010000, 0, 1, 0});
        VkPipelineShaderStageCreateInfo info2 = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        info2.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info2.pName = "main";
        info2.module = reinterpret_cast<VkShaderModule>(static_cast<uintptr_t>(0x9999));
        VulkanShaderStageKey s2;
        const uint32_t moduleWords[] = {0x07230203, 0x00010000, 0, 1, 0};
        check(VulkanShaderStageKey::build(info2, moduleWords, 5, s2), "module s2 build");
        CPKey a, b;
        check(CPKey::build(0x42, s1, makeLayout(1, 1), a), "module a");
        check(CPKey::build(0x42, s2, makeLayout(1, 1), b), "module b");
        check(a == b, "module: equal despite different handle");
        check(a.hash() == b.hash(), "module: hash equal");
    }

    // --- 6: owning: caller mutation + scope destruction ---
    {
        CPKey k;
        const std::string entry(96, 'x');
        const auto expectedShader = makeShader(entry.c_str(), {0x07230203, 0x00010000, 0, 1, 0});
        const auto expectedLayout = makeLayout(2, 1);
        bool guard = false;
        {
            auto shader = makeShader(entry.c_str(), {0x07230203, 0x00010000, 0, 1, 0});
            auto layout = makeLayout(2, 1);
            check(CPKey::build(0x42, shader, layout, k), "owning build");
            guard = k.valid() && k.shader() == shader && k.layout() == layout &&
                    k.shader().entryPoint().data() != shader.entryPoint().data() &&
                    k.shader().spirvWords().data() != shader.spirvWords().data() &&
                    k.layout().setLayouts().size() == 2 &&
                    k.layout().setLayouts().data() != layout.setLayouts().data() &&
                    k.layout().setLayouts()[0].bindings().data() != layout.setLayouts()[0].bindings().data() &&
                    k.layout().pushConstantRanges().data() != layout.pushConstantRanges().data();
            check(guard, "owning: content correct");
            if (guard) {
                // mutate caller data
                shader = makeShader(std::string(500, 'y').c_str(), std::vector<uint32_t>(1000, 0x07230203));
                layout = makeLayout(1, 0, 7);
            }
        } // caller data destroyed
        if (guard) {
            verifyValue(k, 0x42, expectedShader, expectedLayout, "owning complete contents after caller destruction");
            const CPKey copy = k;
            check(copy == k && copy.shader().spirvWords().data() != k.shader().spirvWords().data() &&
                      copy.layout().setLayouts().data() != k.layout().setLayouts().data(),
                  "copy owns independent arrays");
            CPKey assigned;
            assigned = copy;
            CPKey moved = std::move(assigned);
            check(moved == k && moved.hash() == k.hash(), "copy assignment then move preserves tuple");
        } else
            check(false, "owning: guard failed");
    }

    // --- 7: two owners: different arrays, rebuild one, other intact ---
    {
        auto s1 = makeShader("a", {0x07230203, 0x00010000, 0, 1, 0});
        auto s2 = makeShader("b", {0x07230203, 0x00010000, 0, 1, 0});
        auto l1 = makeLayout(1, 1);
        auto l2 = makeLayout(0, 1);
        CPKey a, b;
        check(CPKey::build(0x42, s1, l1, a) && CPKey::build(0x42, s2, l2, b), "two owners");
        const CPKey expectedB = b;
        check(a.shader().spirvWords().data() != b.shader().spirvWords().data(),
              "two: distinct shader arrays");
        check(a.layout().setLayouts().data() != b.layout().setLayouts().data(),
              "two: distinct layout arrays");
        // rebuild a
        auto s3 = makeShader("c", {0x07230203, 0x00010000, 0, 1, 0});
        auto l3 = makeLayout(0, 0);
        check(CPKey::build(0x42, s3, l3, a), "rebuild a");
        check(a.shader().entryPoint() == "c", "rebuild a entry");
        check(b == expectedB && b.hash() == expectedB.hash(), "b: full tuple intact");
    }

    // --- 8: rejections: prime -> bad -> full default -> recover ---
    {
        auto goodShader = makeShader("main", {0x07230203, 0x00010000, 0, 1, 0});
        auto goodLayout = makeLayout(1, 1);
        auto cycle = [&](const char *label, const VulkanShaderStageKey &badShader,
                         const PLKey &badLayout) {
            CPKey good;
            check(CPKey::build(0x42, goodShader, goodLayout, good), label);
            verifyValue(good, 0x42, goodShader, goodLayout, label);
            check(!CPKey::build(0x42, badShader, badLayout, good), label);
            verifyCleared(good, label);
            check(CPKey::build(0x42, goodShader, goodLayout, good), label);
            verifyValue(good, 0x42, goodShader, goodLayout, label);
        };
        {
            VulkanShaderStageKey bad;
            bad = VulkanShaderStageKey{};
            cycle("invalid shader", bad, goodLayout);
        }
        {
            auto bad = makeShader("main", {0x07230203, 0x00010000, 0, 1, 0});
            bad = VulkanShaderStageKey{}; // invalid
            cycle("invalid shader 2", bad, goodLayout);
        }
        {
            auto bad = makeShader("main", {0x07230203, 0x00010000, 0, 1, 0});
            // make non-compute: manually set stage to VERTEX via rebuild
            VkPipelineShaderStageCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            vi.stage = VK_SHADER_STAGE_VERTEX_BIT;
            vi.pName = "main";
            vi.module = reinterpret_cast<VkShaderModule>(static_cast<uintptr_t>(1));
            const uint32_t w[] = {0x07230203, 0x00010000, 0, 1, 0};
            VulkanShaderStageKey vsKey;
            check(VulkanShaderStageKey::build(vi, w, 5, vsKey), "vertex build");
            cycle("non-compute stage", vsKey, goodLayout);
        }
        {
            PLKey badLayout; // default invalid
            cycle("invalid layout", makeShader("main", {0x07230203, 0x00010000, 0, 1, 0}), badLayout);
        }
    }

    // --- 9: actual builder allocation points, with aliased inputs ---
    {
        const std::string entry(96, 'z');
        const auto shader = makeShader(entry.c_str(), {0x07230203, 0x00010000, 0, 1, 0});
        const auto layout = makeLayout(2, 1);
        CPKey fresh;
        bool accepted = false;
        long points = 0;
        {
            AllocGuard guard;
            accepted = CPKey::build(0x42, shader, layout, fresh);
            points = g_allocCount;
        }
        check(accepted && points >= 4, "fresh builder observes nested allocation points");
        verifyValue(fresh, 0x42, shader, layout, "fresh builder full tuple");
        std::printf("compute pipeline allocation points: %ld\n", points);
        for (long nth = 1; nth <= points; ++nth) {
            CPKey key;
            check(CPKey::build(0x42, shader, layout, key), "allocation prime outside guard");
            long injected = 0;
            {
                AllocGuard guard(nth);
                accepted = CPKey::build(key.flags(), key.shader(), key.layout(), key);
                injected = g_injections;
            }
            check(!accepted && injected == 1, "each actual allocation injected once");
            verifyCleared(key, "allocation failure clears entire tuple");
            check(CPKey::build(0x42, shader, layout, key), "allocation failure recovery");
            verifyValue(key, 0x42, shader, layout, "allocation recovery full tuple");
        }
    }
    // --- 10: unordered_map with constant-zero hash ---
    {
        struct ZeroHash {
            size_t operator()(const CPKey &) const { return 0; }
        };
        std::unordered_map<CPKey, int, ZeroHash> map;
        auto s1 = makeShader("a", {0x07230203, 0x00010000, 0, 1, 0});
        auto s2 = makeShader("b", {0x07230203, 0x00010000, 0, 1, 0});
        auto l1 = makeLayout(1, 1);
        auto l2 = makeLayout(0, 1);
        CPKey k1, k2, k3;
        check(CPKey::build(0x42, s1, l1, k1) && CPKey::build(0x42, s2, l2, k2), "map builds");
        map[k1] = 42;
        map[k2] = 99;
        check(map.size() == 2, "map size 2");
        auto it1 = map.find(k1);
        auto it2 = map.find(k2);
        check(it1 != map.end() && it1->second == 42, "map find k1");
        check(it2 != map.end() && it2->second == 99, "map find k2");
        VkPipelineShaderStageCreateInfo i3 = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        i3.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        i3.pName = "c";
        i3.module = reinterpret_cast<VkShaderModule>(static_cast<uintptr_t>(0xC003));
        const uint32_t w3[] = {0x07230203, 0x00010000, 0, 1, 0};
        VulkanShaderStageKey sk3;
        check(cc::gfx::VulkanShaderStageKey::build(i3, w3, 5, sk3), "k3 build");
        check(CPKey::build(0x42, sk3, l1, k3), "k3 cp build");
        check(map.find(k3) == map.end(), "unstored key not found");
    }

    // --- 11: large -> small -> invalid -> recovery has no stale children ---
    {
        auto large = makeShader(std::string(96, 'q').c_str(), std::vector<uint32_t>(64, 0x07230203));
        auto small = makeShader("main", {0x07230203, 0x00010000, 0, 1, 0});
        auto nested = makeLayout(2, 1);
        auto empty = makeLayout(0, 0);
        CPKey key;
        check(CPKey::build(0xFFFFFFFFU, large, nested, key), "large opaque flags build");
        verifyValue(key, 0xFFFFFFFFU, large, nested, "large full tuple");
        check(CPKey::build(0, small, empty, key), "small rebuild");
        verifyValue(key, 0, small, empty, "small full tuple without stale children");
        check(!CPKey::build(0, small, PLKey{}, key), "invalid rebuild");
        verifyCleared(key, "invalid rebuild complete reset");
        check(CPKey::build(0x42, large, nested, key), "large recovery");
        verifyValue(key, 0x42, large, nested, "large recovery full tuple");
    }

    if (g_failures == 0) {
        std::printf("compute pipeline key tests: all PASS\n");
        return 0;
    }
    std::printf("compute pipeline key tests: %u FAIL\n", g_failures);
    return 1;
}
#endif
