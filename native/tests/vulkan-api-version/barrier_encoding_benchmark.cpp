// CPU-only VulkanBarrierEncoding fresh-construct benchmark (M4-A4, revised).
// No GPU, no loader, no engine, no PFN. Measures the cost of building
// the production VulkanBarrierEncoding from scratch (fresh vectors,
// construct → build → consume → destruct) versus a logical legacy
// data-walk reference over the same inputs.
//
// The reference is CPU data traversal only — it is NOT the old Vulkan
// driver cost. We compare encoder conversion overhead, not GPU behavior.
//
// Anti-folding: inputs change per iteration; a non-inline consume function
// receives the output and accumulates into a volatile sink. Both paths
// hash the SAME complete logical field set; a preflight sensitivity check
// checks representative single-field mutations before timing.

#include "VKBarrierEncoding.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <string>

using cc::gfx::VulkanBarrierEncoding;

#if defined(_MSC_VER)
    #define NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    #define NOINLINE __attribute__((noinline))
#else
    #define NOINLINE
#endif

namespace {

struct Inputs {
    VkPipelineStageFlags src, dst;
    VkDependencyFlags flags;
    VkMemoryBarrier mem[1];
    VkImageMemoryBarrier img[1];
    uint32_t memCount, imgCount;
};

Inputs makeInputs(int scenario, uint32_t iter) {
    Inputs in{};
    in.src = VK_PIPELINE_STAGE_TRANSFER_BIT | (iter & 1 ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : 0);
    in.dst = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | (iter & 2 ? VK_PIPELINE_STAGE_VERTEX_INPUT_BIT : 0);
    in.flags = (iter & 4) ? VK_DEPENDENCY_BY_REGION_BIT : 0;
    in.mem[0] = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                 VK_ACCESS_TRANSFER_WRITE_BIT | (iter & 8),
                 VK_ACCESS_SHADER_READ_BIT | (iter & 16)};
    in.img[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    in.img[0].pNext = nullptr;
    in.img[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    in.img[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    in.img[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    in.img[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    in.img[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    in.img[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    in.img[0].image = (VkImage)(uintptr_t)(0x2000 + iter);
    in.img[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,
                                   (iter >> 3) & 1, (iter >> 4) & 3, (iter >> 5) & 1, (iter >> 6) & 3};
    switch (scenario) {
        case 0: in.memCount=1; in.imgCount=0; break;
        case 1: in.memCount=0; in.imgCount=1; break;
        default: in.memCount=1; in.imgCount=1; break;
    }
    return in;
}

// Complete logical hash: both paths must consume the SAME field set.
// fields: counts, flags, per-barrier src/dstStage, src/dstAccess,
// image old/newLayout, src/dstQueueFamily, image handle,
// subresource (aspect, baseMip, levelCount, baseLayer, layerCount).
// The legacy command-wide stages map per-barrier just like the encoder.
NOINLINE uint64_t hashEncoding(const VkDependencyInfo &info) {
    uint64_t acc = info.memoryBarrierCount + info.imageMemoryBarrierCount;
    acc += (uint64_t)info.dependencyFlags << 32;
    for (uint32_t i = 0; i < info.memoryBarrierCount; ++i) {
        const auto &b = info.pMemoryBarriers[i];
        acc += b.srcStageMask * 31 + b.dstStageMask * 37;
        acc += b.srcAccessMask * 41 + b.dstAccessMask * 43;
    }
    for (uint32_t i = 0; i < info.imageMemoryBarrierCount; ++i) {
        const auto &b = info.pImageMemoryBarriers[i];
        acc += b.srcStageMask * 31 + b.dstStageMask * 37;
        acc += b.srcAccessMask * 41 + b.dstAccessMask * 43;
        acc += b.oldLayout * 47 + b.newLayout * 53;
        acc += b.srcQueueFamilyIndex * 59 + b.dstQueueFamilyIndex * 61;
        acc += (uint64_t)(uintptr_t)b.image * 67;
        acc += b.subresourceRange.aspectMask * 71;
        acc += b.subresourceRange.baseMipLevel * 73 + b.subresourceRange.levelCount * 79;
        acc += b.subresourceRange.baseArrayLayer * 83 + b.subresourceRange.layerCount * 89;
    }
    return acc;
}

// reference: walk the same logical fields from raw inputs
NOINLINE uint64_t hashReference(const Inputs &in) {
    uint64_t acc = in.memCount + in.imgCount;
    acc += (uint64_t)in.flags << 32;
    for (uint32_t i = 0; i < in.memCount; ++i) {
        acc += in.src * 31 + in.dst * 37;
        acc += in.mem[i].srcAccessMask * 41 + in.mem[i].dstAccessMask * 43;
    }
    for (uint32_t i = 0; i < in.imgCount; ++i) {
        const auto &im = in.img[i];
        acc += in.src * 31 + in.dst * 37;
        acc += im.srcAccessMask * 41 + im.dstAccessMask * 43;
        acc += im.oldLayout * 47 + im.newLayout * 53;
        acc += im.srcQueueFamilyIndex * 59 + im.dstQueueFamilyIndex * 61;
        acc += (uint64_t)(uintptr_t)im.image * 67;
        acc += im.subresourceRange.aspectMask * 71;
        acc += im.subresourceRange.baseMipLevel * 73 + im.subresourceRange.levelCount * 79;
        acc += im.subresourceRange.baseArrayLayer * 83 + im.subresourceRange.layerCount * 89;
    }
    return acc;
}

uint64_t runEncoding(int scenario, uint32_t iterations) {
    volatile uint64_t sink = 0;
    for (uint32_t i = 0; i < iterations; ++i) {
        Inputs in = makeInputs(scenario, i);
        VulkanBarrierEncoding enc;
        if (!enc.build(in.src, in.dst, in.flags, in.memCount, in.mem, 0, nullptr, in.imgCount, in.img)) {
            std::fprintf(stderr, "FAIL: build returned false\n"); std::exit(1);
        }
        sink += hashEncoding(enc.info());
    }
    return sink;
}

uint64_t runReference(int scenario, uint32_t iterations) {
    volatile uint64_t sink = 0;
    for (uint32_t i = 0; i < iterations; ++i) {
        Inputs in = makeInputs(scenario, i);
        sink += hashReference(in);
    }
    return sink;
}

// strict numeric parse: entire string must be digits, range-checked
bool parseUint(const char *s, uint32_t &out) {
    if (!s || !*s) return false;
    uint64_t v = 0;
    for (const char *p = s; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (uint64_t)(*p - '0');
        if (v > 100000000) return false;
    }
    if (!v) return false;
    out = (uint32_t)v;
    return true;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2 || argc > 4) {
        std::fprintf(stderr, "usage: %s <csv_path> [iterations=1000000] [repeats=9]\n", argv[0]);
        return 2;
    }
    const std::string csvPath = argv[1];
    uint32_t iterations = 1000000, repeats = 9;
    if (argc > 2 && !parseUint(argv[2], iterations)) {
        std::fprintf(stderr, "invalid iterations: '%s'\n", argv[2]); return 2;
    }
    if (argc > 3 && !parseUint(argv[3], repeats)) {
        std::fprintf(stderr, "invalid repeats: '%s'\n", argv[3]); return 2;
    }
    if (repeats > 100) { std::fprintf(stderr, "repeats out of range\n"); return 2; }

    // === preflight: sensitivity + equivalence (before timing) ===
    {
        // equivalence: same inputs through both paths → same hash
        for (int sc = 0; sc < 3; ++sc) {
            uint64_t h1 = runEncoding(sc, 10000);
            uint64_t h2 = runReference(sc, 10000);
            if (h1 != h2) {
                std::fprintf(stderr, "FAIL: hash mismatch scenario %d: enc=%llu ref=%llu\n",
                             sc, (unsigned long long)h1, (unsigned long long)h2);
                return 1;
            }
        }
        // Mutate the ENCODED output, rather than merely the reference
        // fixture: the measured output consumer must notice the corruption.
        Inputs in = makeInputs(2, 123);
        VulkanBarrierEncoding encoding;
        if (!encoding.build(in.src, in.dst, in.flags, 1, in.mem, 0, nullptr, 1, in.img)) return 1;
        const auto &info = encoding.info();
        const uint64_t expected = hashReference(in);
        if (hashEncoding(info) != expected) return 1;
        auto *memory = const_cast<VkMemoryBarrier2 *>(info.pMemoryBarriers);
        auto *image = const_cast<VkImageMemoryBarrier2 *>(info.pImageMemoryBarriers);
        const auto savedMemory = *memory;
        const auto savedImage = *image;
        auto noticed = [&](const char *field) {
            const bool different = hashEncoding(info) != expected;
            *memory = savedMemory; *image = savedImage;
            if (!different) std::fprintf(stderr, "FAIL: encoded hash insensitive to %s\n", field);
            return different;
        };
        memory->dstStageMask ^= VK_PIPELINE_STAGE_TRANSFER_BIT;
        if (!noticed("memory.dstStage")) return 1;
        memory->srcAccessMask ^= VK_ACCESS_TRANSFER_WRITE_BIT;
        if (!noticed("memory.srcAccess")) return 1;
        image->dstStageMask ^= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        if (!noticed("image.dstStage")) return 1;
        image->newLayout = VK_IMAGE_LAYOUT_GENERAL;
        if (!noticed("image.newLayout")) return 1;
        image->dstQueueFamilyIndex = 3;
        if (!noticed("image.dstQueueFamily")) return 1;
        image->image = (VkImage)(uintptr_t)0x9999;
        if (!noticed("image.handle")) return 1;
        image->subresourceRange.levelCount += 1;
        if (!noticed("image.levelCount")) return 1;
        std::fprintf(stdout, "preflight: equivalence+7 encoded-field sensitivity checks PASS\n");
    }

    // === warmup both paths (>=10000 conversions each, not timed) ===
    runReference(0, 10000); runReference(1, 10000); runReference(2, 10000);
    runEncoding(0, 10000); runEncoding(1, 10000); runEncoding(2, 10000);

    std::ofstream csv(csvPath);
    if (!csv) { std::fprintf(stderr, "cannot open %s\n", csvPath.c_str()); return 1; }
    csv << std::fixed << std::setprecision(9);
    csv << "scenario,path,repeat,order,ns_total,ns_per_call,iterations,checksum\n";

    const char *scenarioNames[] = {"memory1", "image1", "memory1_image1"};
    const char *pathNames[] = {"reference", "encoding"};

    for (int sc = 0; sc < 3; ++sc) {
        for (uint32_t rep = 0; rep < repeats; ++rep) {
            const bool refFirst = (rep % 2 == 0); // alternate order per round
            uint64_t sums[2] = {0, 0};
            for (int step = 0; step < 2; ++step) {
                const int path = refFirst ? step : (1 - step);
                auto t0 = std::chrono::steady_clock::now();
                uint64_t cs = path == 0 ? runReference(sc, iterations) : runEncoding(sc, iterations);
                auto t1 = std::chrono::steady_clock::now();
                sums[path] = cs;
                auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                csv << scenarioNames[sc] << "," << pathNames[path] << "," << (rep + 1)
                    << "," << (refFirst ? (step == 0 ? "ref_first" : "enc_second")
                                        : (step == 0 ? "enc_first" : "ref_second"))
                    << "," << ns << "," << (double(ns) / iterations)
                    << "," << iterations << "," << cs << "\n";
            }
            if (sums[0] != sums[1]) {
                std::fprintf(stderr, "FAIL: round checksum mismatch scenario %d rep %u\n", sc, rep + 1);
                csv.close(); return 1;
            }
        }
    }
    csv.close();
    if (csv.fail()) { std::fprintf(stderr, "csv stream failed\n"); return 1; }
    std::fprintf(stdout, "CSV written: %s\n", csvPath.c_str());
    return 0;
}
