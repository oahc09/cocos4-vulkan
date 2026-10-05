// Shader stage key owning value model CPU tests (M7-C1).
// Run contract #21 (20 old + this); probe variant compile-only.
// Includes allocation failNth coverage and known-vector assertions.

#if defined(VKSHADERKEY_STANDALONE_PROBE)
    #include "VKShaderStageKey.h"
int main() { return 0; }
#else

    #include <cstdio>
    #include <cstdlib>
    #include <cstring>
    #include <new>
    #include <type_traits>
    #include <unordered_map>
    #include "VKShaderStageKey.h"

using cc::gfx::VulkanShaderStageKey;

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
        g_failNth = failNth;
        g_injectedFailures = 0;
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

void verifyDefault(const VulkanShaderStageKey &k) {
    check(!k.valid() && k.flags() == 0 && k.stage() == 0 && k.entryPoint().empty() &&
              k.spirvWords().empty() && k.hash() == 0x95426f30246c6927ULL,
          "default shader key: all fields cleared");
}

VkShaderModule fakeModule(uintptr_t v) {
    return reinterpret_cast<VkShaderModule>(v);
}

VkPipelineShaderStageCreateInfo makeInfo(VkShaderStageFlagBits stage, const char *entry,
                                         VkShaderModule mod) {
    VkPipelineShaderStageCreateInfo i = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    i.stage = stage;
    i.pName = entry;
    i.module = mod;
    return i;
}

} // namespace

int main() {
    static_assert(std::is_copy_constructible<VulkanShaderStageKey>::value, "copy");
    static_assert(std::is_move_constructible<VulkanShaderStageKey>::value, "move");

    // --- 1: known vectors (Lead-provided) ---
    {
        // invalid default hash
        VulkanShaderStageKey invalid;
        check(invalid.hash() == 0x95426f30246c6927ULL, "known invalid hash");
        // valid: flags=0, stage=VERTEX, entry="main", 5 words
        VkPipelineShaderStageCreateInfo info = makeInfo(VK_SHADER_STAGE_VERTEX_BIT, "main", fakeModule(1));
        const uint32_t words[] = {0x07230203, 0x00010000, 0, 1, 0};
        VulkanShaderStageKey k;
        check(VulkanShaderStageKey::build(info, words, 5, k), "known valid build");
        check(k.valid(), "known valid: valid");
        check(k.hash() == 0xc5dcdb39a4bebcccULL, "known valid hash exact");
    }

    // --- 2: single-field mutation sensitivity ---
    {
        VkPipelineShaderStageCreateInfo info = makeInfo(VK_SHADER_STAGE_VERTEX_BIT, "main", fakeModule(1));
        const uint32_t words[] = {0x07230203, 0x00010000, 0, 1, 0};
        VulkanShaderStageKey base;
        check(VulkanShaderStageKey::build(info, words, 5, base), "mutation base");

        // flags
        {
            auto i2 = info;
            i2.flags = 1;
            VulkanShaderStageKey k;
            check(VulkanShaderStageKey::build(i2, words, 5, k), "flags build");
            check(k != base, "flags sensitivity eq");
            check(k.hash() != base.hash(), "flags hash");
        }
        // stage
        {
            auto i2 = info;
            i2.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            VulkanShaderStageKey k;
            check(VulkanShaderStageKey::build(i2, words, 5, k), "stage build");
            check(k != base, "stage sensitivity eq");
            check(k.hash() != base.hash(), "stage hash");
        }
        // entry
        {
            auto i2 = info;
            i2.pName = "other";
            VulkanShaderStageKey k;
            check(VulkanShaderStageKey::build(i2, words, 5, k), "entry build");
            check(k != base, "entry sensitivity eq");
            check(k.hash() != base.hash(), "entry hash");
        }
        // word value
        {
            uint32_t w2[] = {0x07230203, 0x00010001, 0, 1, 0};
            VulkanShaderStageKey k;
            check(VulkanShaderStageKey::build(info, w2, 5, k), "word build");
            check(k != base, "word sensitivity eq");
            check(k.hash() != base.hash(), "word hash");
        }
        // word count
        {
            uint32_t w2[] = {0x07230203, 0x00010000, 0, 1, 0, 0};
            VulkanShaderStageKey k;
            check(VulkanShaderStageKey::build(info, w2, 6, k), "count build");
            check(k != base, "count sensitivity eq");
            check(k.hash() != base.hash(), "count hash");
        }
    }

    // --- 3: module handle change does not affect key ---
    {
        VkPipelineShaderStageCreateInfo i1 = makeInfo(VK_SHADER_STAGE_VERTEX_BIT, "main", fakeModule(0x1111));
        VkPipelineShaderStageCreateInfo i2 = makeInfo(VK_SHADER_STAGE_VERTEX_BIT, "main", fakeModule(0x2222));
        const uint32_t words[] = {0x07230203, 0x00010000, 0, 1, 0};
        VulkanShaderStageKey k1, k2;
        check(VulkanShaderStageKey::build(i1, words, 5, k1) &&
                  VulkanShaderStageKey::build(i2, words, 5, k2),
              "module builds");
        check(k1 == k2, "module: equal despite different handle");
        check(k1.hash() == k2.hash(), "module: hash equal");
    }

    // --- 4: owning: caller mutation + scope destruction ---
    {
        VulkanShaderStageKey k;
        VulkanShaderStageKey expected;
        bool guard = false;
        {
            std::string name(96, 'q');
            VkPipelineShaderStageCreateInfo info = makeInfo(VK_SHADER_STAGE_VERTEX_BIT, name.c_str(), fakeModule(1));
            info.flags = 5;
            std::vector<uint32_t> words = {0x07230203, 0x00010000, 0, 1, 0};
            check(VulkanShaderStageKey::build(info, words.data(), words.size(), k), "owning build");
            check(VulkanShaderStageKey::build(info, words.data(), words.size(), expected), "owning expected fixture");
            guard = k.valid() && k.entryPoint().data() != name.data() && k.spirvWords().data() != words.data();
            check(guard, "owning: both arrays do not borrow input");
            if (guard) {
                name.assign(3000, 'x');
                words[0] = 0xDEADBEEF;
                words.resize(1000);
            }
        } // caller data destroyed
        if (guard) {
            check(k == expected && k.hash() == expected.hash(), "owning: full content after destruction");
            VulkanShaderStageKey copy = k;
            check(copy == expected && copy.entryPoint().data() != k.entryPoint().data() &&
                      copy.spirvWords().data() != k.spirvWords().data(),
                  "owning: copy has separate arrays");
            const uint32_t smaller[] = {0x07230203, 0, 0, 1, 0};
            const auto changed = makeInfo(VK_SHADER_STAGE_FRAGMENT_BIT, "other", fakeModule(2));
            check(VulkanShaderStageKey::build(changed, smaller, 5, k) && copy == expected,
                  "owning: rebuild leaves copied owner intact");
        } else
            check(false, "owning: guard failed");
    }

    // --- 5: two owners: different array addresses, rebuild one, other intact ---
    {
        VkPipelineShaderStageCreateInfo i1 = makeInfo(VK_SHADER_STAGE_VERTEX_BIT, "a", fakeModule(1));
        VkPipelineShaderStageCreateInfo i2 = makeInfo(VK_SHADER_STAGE_FRAGMENT_BIT, "b", fakeModule(2));
        const uint32_t w1[] = {0x07230203, 0x00010000, 0, 1, 0};
        const uint32_t w2[] = {0x07230203, 0x00010000, 0, 1, 0};
        VulkanShaderStageKey a, b;
        check(VulkanShaderStageKey::build(i1, w1, 5, a) && VulkanShaderStageKey::build(i2, w2, 5, b),
              "two owners");
        check(a.spirvWords().data() != b.spirvWords().data(), "two: distinct arrays");
        // rebuild a
        VkPipelineShaderStageCreateInfo i3 = makeInfo(VK_SHADER_STAGE_VERTEX_BIT, "c", fakeModule(3));
        const uint32_t w3[] = {0x07230203, 0x00010000, 0, 1, 0, 9};
        check(VulkanShaderStageKey::build(i3, w3, 6, a), "rebuild a");
        check(a.entryPoint() == "c" && a.spirvWords().size() == 6 && a.spirvWords()[5] == 9, "rebuild: a content");
        check(b.entryPoint() == "b" && b.spirvWords().size() == 5, "b: intact");
    }

    // --- 6: self-alias: build from own getters ---
    {
        VkPipelineShaderStageCreateInfo info = makeInfo(VK_SHADER_STAGE_VERTEX_BIT, "main", fakeModule(1));
        const uint32_t words[] = {0x07230203, 0x00010000, 0, 1, 0};
        VulkanShaderStageKey k;
        check(VulkanShaderStageKey::build(info, words, 5, k), "self prime");
        // self-alias: pName from entryPoint().c_str(), words from spirvWords().data()
        info.pName = k.entryPoint().c_str();
        const VulkanShaderStageKey expected = k;
        check(VulkanShaderStageKey::build(info, k.spirvWords().data(),
                                          k.spirvWords().size(), k),
              "self alias rebuild");
        check(k.valid() && k.entryPoint() == "main" && k.spirvWords().size() == 5,
              "self: full content preserved");
        check(k == expected, "lead: true self-alias preserves entire shader tuple");
    }

    // --- 7: big -> small -> bad -> recover, full fields no residue ---
    {
        VkPipelineShaderStageCreateInfo i1 = makeInfo(VK_SHADER_STAGE_VERTEX_BIT, "long_entry", fakeModule(1));
        std::vector<uint32_t> bigWords(100, 0x07230203);
        VulkanShaderStageKey k;
        check(VulkanShaderStageKey::build(i1, bigWords.data(), 100, k), "big build");
        VkPipelineShaderStageCreateInfo i2 = makeInfo(VK_SHADER_STAGE_FRAGMENT_BIT, "s", fakeModule(2));
        const uint32_t w2[] = {0x07230203, 0x00010000, 0, 1, 0};
        check(VulkanShaderStageKey::build(i2, w2, 5, k), "small build");
        check(k.entryPoint() == "s" && k.spirvWords().size() == 5, "small: no residue");
        // bad
        VkPipelineShaderStageCreateInfo bad = makeInfo(VK_SHADER_STAGE_VERTEX_BIT, "x", fakeModule(3));
        bad.stage = static_cast<VkShaderStageFlagBits>(0);
        check(!VulkanShaderStageKey::build(bad, w2, 2, k), "bad rejected");
        check(!k.valid() && k.entryPoint().empty(), "bad: cleared");
        // recover
        check(VulkanShaderStageKey::build(i1, bigWords.data(), 100, k), "recover");
        check(k.entryPoint() == "long_entry" && k.spirvWords().size() == 100, "recover: full");
    }

    // --- 8: shape rejection negatives: prime -> bad -> cleared -> recover ---
    {
        VkPipelineShaderStageCreateInfo good = makeInfo(VK_SHADER_STAGE_VERTEX_BIT, "main", fakeModule(1));
        const uint32_t words[] = {0x07230203, 0x00010000, 0, 1, 0};
        VulkanShaderStageKey k;
        VulkanShaderStageKey expected;
        good.flags = 5;
        check(VulkanShaderStageKey::build(good, words, 5, expected), "rejection baseline");
        auto cycle = [&](const char *label, VkPipelineShaderStageCreateInfo bad,
                         const uint32_t *w, size_t wc) {
            check(VulkanShaderStageKey::build(good, words, 5, k), label);
            check(k == expected, label);
            check(!VulkanShaderStageKey::build(bad, w, wc, k), label);
            verifyDefault(k);
            check(VulkanShaderStageKey::build(good, words, 5, k), label);
            check(k == expected, label);
        };
        {
            auto t = good;
            t.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            cycle("sType", t, words, 5);
        }
        {
            auto t = good;
            t.pNext = &t;
            cycle("pNext", t, words, 5);
        }
        {
            auto t = good;
            VkSpecializationInfo spec = {};
            t.pSpecializationInfo = &spec;
            cycle("spec", t, words, 5);
        }
        {
            auto t = good;
            t.module = VK_NULL_HANDLE;
            cycle("module NULL", t, words, 5);
        }
        {
            auto t = good;
            t.stage = static_cast<VkShaderStageFlagBits>(0);
            cycle("stage 0", t, words, 5);
        }
        {
            auto t = good;
            t.stage = static_cast<VkShaderStageFlagBits>(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
            cycle("stage combo", t, words, 5);
        }
        {
            auto t = good;
            t.stage = static_cast<VkShaderStageFlagBits>(0x100);
            cycle("stage unknown", t, words, 5);
        }
        {
            auto t = good;
            t.pName = nullptr;
            cycle("pName null", t, words, 5);
        }
        {
            auto t = good;
            t.pName = "";
            cycle("pName empty", t, words, 5);
        }
        {
            auto t = good;
            static char longName[2048];
            std::memset(longName, 'a', 2000);
            t.pName = longName;
            cycle("pName too long", t, words, 5);
        }
        cycle("words null", good, nullptr, 5);
        {
            auto t = good;
            const uint32_t shortWords[] = {0x07230203};
            cycle("word count 4 (<5)", t, shortWords, 4);
        }
        {
            auto t = good;
            cycle("word count huge", t, words, VulkanShaderStageKey::MAX_SPIRV_WORDS + 1ULL);
        }
        {
            auto t = good;
            const uint32_t badMagic[] = {0x12345678, 0, 1, 0, 0};
            cycle("bad magic", t, badMagic, 5);
        }
    }

    // --- 9: allocation failNth ---
    {
        VkPipelineShaderStageCreateInfo info = makeInfo(VK_SHADER_STAGE_VERTEX_BIT, "entry_96_chars_long_entrypoint_for_heap_alloc", fakeModule(1));
        const uint32_t words[] = {0x07230203, 0x00010000, 0, 1, 0, 2, 3, 4, 5, 6};

        // count fresh build allocation points
        long total = 0;
        {
            VulkanShaderStageKey k;
            AllocGuard g;
            check(VulkanShaderStageKey::build(info, words, 10, k), "alloc probe");
            total = g_allocCount;
        }
        check(total >= 1, "must allocate");
        std::printf("fresh build allocation points: %ld\n", total);
        for (long n = 1; n <= total; ++n) {
            VulkanShaderStageKey k;
            check(VulkanShaderStageKey::build(info, words, 10, k), "alloc prime outside guard");
            auto aliased = info;
            aliased.pName = k.entryPoint().c_str();
            bool accepted;
            long injections;
            {
                AllocGuard g(n);
                accepted = VulkanShaderStageKey::build(aliased, k.spirvWords().data(), k.spirvWords().size(), k);
                injections = g_injectedFailures;
            }
            check(!accepted && injections == 1, "noexcept build catches actual bad_alloc");
            verifyDefault(k);
            check(VulkanShaderStageKey::build(info, words, 10, k), "allocation failure recovery");
        }
    }

    // --- 10: unordered_map with constant-zero hash ---
    {
        struct ZeroHash {
            size_t operator()(const VulkanShaderStageKey &) const { return 0; }
        };
        std::unordered_map<VulkanShaderStageKey, int, ZeroHash> map;
        VkPipelineShaderStageCreateInfo i1 = makeInfo(VK_SHADER_STAGE_VERTEX_BIT, "a", fakeModule(1));
        VkPipelineShaderStageCreateInfo i2 = makeInfo(VK_SHADER_STAGE_FRAGMENT_BIT, "b", fakeModule(2));
        const uint32_t w1[] = {0x07230203, 0x00010000, 0, 1, 0};
        const uint32_t w2[] = {0x07230203, 0x00010000, 0, 1, 0};
        VulkanShaderStageKey k1, k2;
        check(VulkanShaderStageKey::build(i1, w1, 5, k1) &&
                  VulkanShaderStageKey::build(i2, w2, 5, k2),
              "map builds");
        map[k1] = 42;
        map[k2] = 99;
        check(map.size() == 2, "map size 2");
        auto it1 = map.find(k1);
        auto it2 = map.find(k2);
        check(it1 != map.end() && it1->second == 42, "map find k1");
        check(it2 != map.end() && it2->second == 99, "map find k2");
        VulkanShaderStageKey k3;
        VkPipelineShaderStageCreateInfo i3 = makeInfo(VK_SHADER_STAGE_VERTEX_BIT, "c", fakeModule(3));
        const uint32_t w3[] = {0x07230203, 0x00010000, 0, 1, 0};
        check(VulkanShaderStageKey::build(i3, w3, 5, k3), "k3 build");
        check(map.find(k3) == map.end(), "unstored key not found");
    }

    // Exact application limit and bounded scanning, not a SPIR-V validator.
    {
        const uint32_t words[] = {0x07230203, 0x00010000, 0, 1, 0};
        std::string name(VulkanShaderStageKey::MAX_ENTRY_BYTES, 'n');
        auto info = makeInfo(VK_SHADER_STAGE_COMPUTE_BIT, name.c_str(), fakeModule(1));
        VulkanShaderStageKey k;
        check(VulkanShaderStageKey::build(info, words, 5, k) && k.entryPoint() == name,
              "entry max length accepted");
        char unterminated[VulkanShaderStageKey::MAX_ENTRY_BYTES + 1];
        std::memset(unterminated, 'n', sizeof(unterminated));
        info.pName = unterminated;
        check(!VulkanShaderStageKey::build(info, words, 5, k), "entry bounded scan rejects without extra read");
        verifyDefault(k);
        info.pName = "main";
        const VkShaderStageFlagBits stages[] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                                VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
                                                VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_COMPUTE_BIT};
        for (auto stage : stages) {
            info.stage = stage;
            check(VulkanShaderStageKey::build(info, words, 5, k) && k.stage() == stage,
                  "each supported stage shape preserved");
        }
    }

    if (g_failures == 0) {
        std::printf("shader stage key tests: all PASS\n");
        return 0;
    }
    std::printf("shader stage key tests: %u FAIL\n", g_failures);
    return 1;
}
#endif
