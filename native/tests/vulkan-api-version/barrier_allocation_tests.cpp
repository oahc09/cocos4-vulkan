// CPU-only barrier allocation contract tests (M4-A5).
// Verifies that single-barrier VulkanBarrierEncoding build/destruct performs
// ZERO heap allocations, and that larger counts spill correctly.

#include "VKBarrierEncoding.h"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <new>

using cc::gfx::VulkanBarrierEncoding;

// ---- global allocation counter (test-only, flag-gated) ----
static long g_allocCount = 0;
static bool g_counting = false;

void* operator new(size_t size) {
    if (g_counting) ++g_allocCount;
    void* p = std::malloc(size ? size : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

namespace {
unsigned g_failures = 0;
void check(bool ok, const char* w) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", w); ++g_failures; } }

struct Guard {
    Guard() { g_allocCount = 0; g_counting = true; }
    ~Guard() { g_counting = false; }
};

// stack fixtures
struct Fix {
    VkMemoryBarrier mem[4]{};
    VkBufferMemoryBarrier buf[4]{};
    VkImageMemoryBarrier img[4]{};
    void init() {
        for (int i = 0; i < 4; ++i) {
            mem[i].sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            mem[i].pNext = nullptr; mem[i].srcAccessMask = 1; mem[i].dstAccessMask = 2;
            buf[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            buf[i].pNext = nullptr; buf[i].srcAccessMask = 1; buf[i].dstAccessMask = 2;
            buf[i].srcQueueFamilyIndex = 0; buf[i].dstQueueFamilyIndex = 0;
            buf[i].buffer = (VkBuffer)(uintptr_t)(0x100+i); buf[i].offset = 0; buf[i].size = 256;
            img[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            img[i].pNext = nullptr; img[i].srcAccessMask = 1; img[i].dstAccessMask = 2;
            img[i].srcQueueFamilyIndex = 0; img[i].dstQueueFamilyIndex = 0;
            img[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            img[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            img[i].image = (VkImage)(uintptr_t)(0x200+i);
            img[i].subresourceRange.aspectMask = 1;
            img[i].subresourceRange.baseMipLevel = 0;
            img[i].subresourceRange.levelCount = 1;
            img[i].subresourceRange.baseArrayLayer = 0;
            img[i].subresourceRange.layerCount = 1;
        }
    }
};

} // namespace

int main() {
    Fix fx; fx.init();
    const VkPipelineStageFlags SRC = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkPipelineStageFlags DST = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

    // --- 1: memory1 → expect 0 heap allocations ---
    { Guard g; long before = g_allocCount;
      VulkanBarrierEncoding enc;
      check(enc.build(SRC, DST, 0, 1, fx.mem, 0, nullptr, 0, nullptr), "mem1 build");
      long allocs = g_allocCount - before;
      std::printf("mem1: allocations=%ld\n", allocs);
      check(allocs == 0, "mem1: single memory barrier must not heap-allocate");
      check(enc.info().memoryBarrierCount == 1, "mem1: count");
    }

    // --- 2: image1 → expect 0 ---
    { Guard g; long before = g_allocCount;
      VulkanBarrierEncoding enc;
      check(enc.build(SRC, DST, 0, 0, nullptr, 0, nullptr, 1, fx.img), "img1 build");
      long allocs = g_allocCount - before;
      std::printf("img1: allocations=%ld\n", allocs);
      check(allocs == 0, "img1: single image barrier must not heap-allocate");
      check(enc.info().imageMemoryBarrierCount == 1, "img1: count");
    }

    // --- 3: mixed (1 memory + 1 image) → expect 0 ---
    { Guard g; long before = g_allocCount;
      VulkanBarrierEncoding enc;
      check(enc.build(SRC, DST, 0, 1, fx.mem, 0, nullptr, 1, fx.img), "mixed build");
      long allocs = g_allocCount - before;
      std::printf("mixed: allocations=%ld\n", allocs);
      check(allocs == 0, "mixed: 1mem+1img must not heap-allocate");
    }

    // --- 4: execution-only (0 counts) → expect 0, still has 1 memory node ---
    { Guard g; long before = g_allocCount;
      VulkanBarrierEncoding enc;
      check(enc.build(SRC, DST, 0, 0, nullptr, 0, nullptr, 0, nullptr), "exec-only build");
      long allocs = g_allocCount - before;
      std::printf("exec-only: allocations=%ld\n", allocs);
      check(allocs == 0, "exec-only: must not heap-allocate");
      check(enc.info().memoryBarrierCount == 1, "exec-only: must have 1 memory node");
    }

    // --- 5: buffer1 → expect 0 ---
    { Guard g; long before = g_allocCount;
      VulkanBarrierEncoding enc;
      check(enc.build(SRC, DST, 0, 0, nullptr, 1, fx.buf, 0, nullptr), "buf1 build");
      long allocs = g_allocCount - before;
      std::printf("buf1: allocations=%ld\n", allocs);
      check(allocs == 0, "buf1: single buffer barrier must not heap-allocate");
    }

    // --- 6: spill: 3 memory nodes → vector fallback OK, must succeed ---
    { VulkanBarrierEncoding enc;
      check(enc.build(SRC, DST, 0, 3, fx.mem, 0, nullptr, 0, nullptr), "spill3 build");
      check(enc.info().memoryBarrierCount == 3, "spill3: count");
      check(enc.info().pMemoryBarriers[2].srcStageMask == SRC, "spill3: field preserved");
    }

    // --- 7: invalid pNext → build fails, info zeroed, no stale data ---
    { VkMemoryBarrier bad[1] = {{VK_STRUCTURE_TYPE_MEMORY_BARRIER, (void*)1, 1, 2}};
      VulkanBarrierEncoding enc;
      check(enc.build(SRC, DST, 0, 1, fx.mem, 0, nullptr, 0, nullptr), "valid first");
      check(!enc.build(SRC, DST, 0, 1, bad, 0, nullptr, 0, nullptr), "invalid pNext rejected");
      check(enc.info().memoryBarrierCount == 0 && enc.info().pMemoryBarriers == nullptr,
            "invalid rebuild: info zeroed");
    }

    // --- 8: rebuild cycle: valid → invalid → valid again, no stale ---
    { VulkanBarrierEncoding enc;
      check(enc.build(SRC, DST, 0, 1, fx.mem, 0, nullptr, 1, fx.img), "cycle1");
      check(enc.build(SRC, DST, 0, 0, nullptr, 0, nullptr, 0, nullptr), "cycle2 exec-only ok");
      // rebuild with different composition
      check(enc.build(SRC, DST, 0, 0, nullptr, 0, nullptr, 1, fx.img), "cycle3 img-only");
      check(enc.info().imageMemoryBarrierCount == 1 && enc.info().memoryBarrierCount == 0,
            "cycle3: img-only after rebuild has no memory barriers");
    }

    // --- 9: original array modification doesn't change output ---
    { VkMemoryBarrier local[1] = {{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, 111, 222}};
      VulkanBarrierEncoding enc;
      check(enc.build(SRC, DST, 0, 1, local, 0, nullptr, 0, nullptr), "own build");
      local[0].srcAccessMask = 999; // mutate caller array
      check(enc.info().pMemoryBarriers[0].srcAccessMask == 111, "owning: original change not reflected");
    }

    // Crossing all three storage boundaries must retain order and own
    // every field; then returning to inline/failure must discard old views.
    {
        Fix local; local.init();
        for (int i=0; i<3; ++i) {
            local.mem[i].srcAccessMask = 100+i;
            local.buf[i].offset = 256*(i+1); local.buf[i].size = VK_WHOLE_SIZE;
            local.buf[i].srcQueueFamilyIndex = 3; local.buf[i].dstQueueFamilyIndex = 7;
            local.img[i].subresourceRange.baseMipLevel = i;
            local.img[i].subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
            local.img[i].subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
        }
        VulkanBarrierEncoding enc;
        long allocations = 0;
        {
            Guard guard;
            check(enc.build(SRC, DST, VK_DEPENDENCY_BY_REGION_BIT, 3, local.mem, 3, local.buf, 3, local.img), "all-type spill build");
            allocations = g_allocCount;
        }
        check(allocations > 0, "large fresh batch actually uses heap fallback");
        const auto &info = enc.info();
        check(info.memoryBarrierCount==3 && info.bufferMemoryBarrierCount==3 && info.imageMemoryBarrierCount==3, "all-type spill counts");
        for (int i=0; i<3; ++i) {
            const auto &m=info.pMemoryBarriers[i]; const auto &b=info.pBufferMemoryBarriers[i]; const auto &im=info.pImageMemoryBarriers[i];
            check(m.srcStageMask==SRC && m.dstStageMask==DST && m.srcAccessMask==100+i && m.dstAccessMask==2, "spill memory scopes/access/order");
            check(b.sType==VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 && !b.pNext && b.srcStageMask==SRC && b.dstStageMask==DST &&
                  b.srcQueueFamilyIndex==3 && b.dstQueueFamilyIndex==7 && b.buffer==local.buf[i].buffer &&
                  b.offset==256*(i+1) && b.size==VK_WHOLE_SIZE, "spill buffer range/queue/order");
            check(im.sType==VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 && !im.pNext && im.srcStageMask==SRC && im.dstStageMask==DST &&
                  im.image==local.img[i].image && im.oldLayout==local.img[i].oldLayout && im.newLayout==local.img[i].newLayout &&
                  im.subresourceRange.baseMipLevel==i && im.subresourceRange.levelCount==VK_REMAINING_MIP_LEVELS &&
                  im.subresourceRange.layerCount==VK_REMAINING_ARRAY_LAYERS, "spill image layout/range/order");
        }
        local.mem[2].srcAccessMask=999; local.buf[2].offset=999; local.img[2].subresourceRange.baseMipLevel=999;
        check(info.pMemoryBarriers[2].srcAccessMask==102 && info.pBufferMemoryBarriers[2].offset==768 &&
              info.pImageMemoryBarriers[2].subresourceRange.baseMipLevel==2, "spill storage owns all input arrays");
        check(enc.build(SRC,DST,0,1,local.mem,1,local.buf,1,local.img) && enc.info().memoryBarrierCount==1 &&
              enc.info().bufferMemoryBarrierCount==1 && enc.info().imageMemoryBarrierCount==1, "all spill types return to inline");
        local.img[0].pNext=&local;
        check(!enc.build(SRC,DST,0,1,local.mem,1,local.buf,1,local.img), "inline rebuild rejects unknown extension");
        check(enc.info().sType==VK_STRUCTURE_TYPE_DEPENDENCY_INFO && !enc.info().pMemoryBarriers && !enc.info().memoryBarrierCount &&
              !enc.info().pBufferMemoryBarriers && !enc.info().bufferMemoryBarrierCount &&
              !enc.info().pImageMemoryBarriers && !enc.info().imageMemoryBarrierCount, "failed rebuild clears every view");
        local.img[0].pNext=nullptr;
        check(enc.build(SRC,DST,0,0,nullptr,0,nullptr,0,nullptr) && enc.info().memoryBarrierCount==1 &&
              enc.info().pMemoryBarriers[0].srcStageMask==SRC && enc.info().pMemoryBarriers[0].dstStageMask==DST &&
              !enc.info().pMemoryBarriers[0].srcAccessMask && !enc.info().pMemoryBarriers[0].dstAccessMask &&
              !enc.info().pBufferMemoryBarriers && !enc.info().pImageMemoryBarriers, "failed rebuild recovers execution-only scopes");
    }

    if (g_failures == 0) { std::printf("barrier allocation tests: all PASS\n"); return 0; }
    std::printf("barrier allocation tests: %u FAIL\n", g_failures);
    return 1;
}
