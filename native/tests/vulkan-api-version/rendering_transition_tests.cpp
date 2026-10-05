// Single-image pre/post render transition CPU contract tests (M5-F).
// Reads go through owned references; each edge is fed to the existing
// VulkanBarrierEncoding and compared field-by-field after count checks.

#include <cstdint>
#include <cstdio>
#include <type_traits>
#include "VKBarrierEncoding.h"
#include "VKRenderingTransitionContract.h"

using cc::gfx::VKRenderingTransitionContract;
using cc::gfx::VulkanBarrierEncoding;
using Input = VKRenderingTransitionContract::Input;
using Use = VKRenderingTransitionContract::Use;

namespace {
unsigned g_failures = 0;
void check(bool ok, const char *w) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", w);
        ++g_failures;
    }
}

const VkImage IMG = reinterpret_cast<VkImage>(static_cast<uintptr_t>(0xE123));

Input makeInput() {
    Input in;
    in.image = IMG;
    in.range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    in.range.baseMipLevel = 1;
    in.range.levelCount = 3;
    in.range.baseArrayLayer = 2;
    in.range.layerCount = 4;
    in.before = {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT};
    in.during = {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    in.after = {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT};
    in.preFlags = VK_DEPENDENCY_BY_REGION_BIT;
    in.postFlags = VK_DEPENDENCY_DEVICE_GROUP_BIT;
    return in;
}

bool sameRange(const VkImageSubresourceRange &a, const VkImageSubresourceRange &b) {
    return a.aspectMask == b.aspectMask && a.baseMipLevel == b.baseMipLevel &&
           a.levelCount == b.levelCount && a.baseArrayLayer == b.baseArrayLayer &&
           a.layerCount == b.layerCount;
}

void verifyInput(const Input &actual, const Input &expected) {
    check(actual.image == expected.image, "input: image snapshot");
    check(sameRange(actual.range, expected.range), "input: full range snapshot");
    check(actual.preFlags == expected.preFlags && actual.postFlags == expected.postFlags,
          "input: flags snapshot");
    const auto verifyUse = [](const Use &a, const Use &e) {
        check(a.layout == e.layout && a.stages == e.stages && a.accesses == e.accesses,
              "input: full use snapshot");
    };
    verifyUse(actual.before, expected.before);
    verifyUse(actual.during, expected.during);
    verifyUse(actual.after, expected.after);
}

// full edge verification through owned reference
void verifyEdge(const VKRenderingTransitionContract::Edge &e, VkPipelineStageFlags src,
                VkPipelineStageFlags dst, VkDependencyFlags flags, VkImageLayout oldL,
                VkImageLayout newL, VkAccessFlags srcA, VkAccessFlags dstA,
                const Input &expected = makeInput()) {
    check(e.srcStages == src, "ve: srcStages");
    check(e.dstStages == dst, "ve: dstStages");
    check(e.flags == flags, "ve: flags");
    const auto &b = e.barrier;
    check(b.sType == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, "ve: sType");
    check(b.pNext == nullptr, "ve: pNext null");
    check(b.oldLayout == oldL, "ve: oldLayout");
    check(b.newLayout == newL, "ve: newLayout");
    check(b.srcAccessMask == srcA, "ve: srcAccess");
    check(b.dstAccessMask == dstA, "ve: dstAccess");
    check(b.image == expected.image, "ve: image");
    check(b.subresourceRange.aspectMask == expected.range.aspectMask, "ve: aspect");
    check(b.subresourceRange.baseMipLevel == expected.range.baseMipLevel, "ve: baseMip");
    check(b.subresourceRange.levelCount == expected.range.levelCount, "ve: levelCount");
    check(b.subresourceRange.baseArrayLayer == expected.range.baseArrayLayer, "ve: baseLayer");
    check(b.subresourceRange.layerCount == expected.range.layerCount, "ve: layerCount");
    check(b.srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED, "ve: srcQF ignored");
    check(b.dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED, "ve: dstQF ignored");
}

// feed one edge to the existing encoder, verify Sync2 node field-by-field
void verifyEdgeThroughEncoder(const VKRenderingTransitionContract::Edge &e,
                              VkPipelineStageFlags src, VkPipelineStageFlags dst,
                              VkDependencyFlags flags, VkImageLayout oldL, VkImageLayout newL,
                              VkAccessFlags srcA, VkAccessFlags dstA,
                              const Input &expected = makeInput()) {
    VulkanBarrierEncoding enc;
    check(enc.build(e.srcStages, e.dstStages, e.flags, 0, nullptr, 0, nullptr, 1, &e.barrier),
          "enc: build");
    const auto &info = enc.info();
    check(info.sType == VK_STRUCTURE_TYPE_DEPENDENCY_INFO, "enc: dep sType");
    check(info.pNext == nullptr, "enc: dep pNext");
    check(info.dependencyFlags == flags, "enc: flags");
    check(info.memoryBarrierCount == 0 && info.pMemoryBarriers == nullptr, "enc: no memory node");
    check(info.bufferMemoryBarrierCount == 0 && info.pBufferMemoryBarriers == nullptr,
          "enc: no buffer node");
    // guard: count and pointer verified before reading the image node
    if (info.imageMemoryBarrierCount == 1 && info.pImageMemoryBarriers != nullptr) {
        const auto &n = info.pImageMemoryBarriers[0];
        check(n.sType == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, "enc: img2 sType");
        check(n.pNext == nullptr, "enc: img2 pNext");
        check(n.srcStageMask == src, "enc: srcStage");
        check(n.dstStageMask == dst, "enc: dstStage");
        check(n.srcAccessMask == srcA, "enc: srcAccess");
        check(n.dstAccessMask == dstA, "enc: dstAccess");
        check(n.oldLayout == oldL, "enc: oldLayout");
        check(n.newLayout == newL, "enc: newLayout");
        check(n.image == expected.image, "enc: image");
        check(sameRange(n.subresourceRange, expected.range), "enc: all five range fields");
        check(n.srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED, "enc: srcQF");
        check(n.dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED, "enc: dstQF");
    } else {
        check(false, "enc: image node guard failed");
    }
}

void verifyPre(const VKRenderingTransitionContract &c, const Input &in) {
    verifyEdge(c.pre(), in.before.stages, in.during.stages, in.preFlags, in.before.layout,
               in.during.layout, in.before.accesses, in.during.accesses, in);
}
void verifyPost(const VKRenderingTransitionContract &c, const Input &in) {
    verifyEdge(c.post(), in.during.stages, in.after.stages, in.postFlags, in.during.layout,
               in.after.layout, in.during.accesses, in.after.accesses, in);
}

void verifyComplete(const VKRenderingTransitionContract &c, const Input &in) {
    check(c.ready(), "complete: ready");
    verifyInput(c.input(), in);
    verifyPre(c, in);
    verifyPost(c, in);
    verifyEdgeThroughEncoder(c.pre(), in.before.stages, in.during.stages, in.preFlags,
                             in.before.layout, in.during.layout, in.before.accesses, in.during.accesses, in);
    verifyEdgeThroughEncoder(c.post(), in.during.stages, in.after.stages, in.postFlags,
                             in.during.layout, in.after.layout, in.during.accesses, in.after.accesses, in);
}

void verifyCleared(const VKRenderingTransitionContract &c) {
    check(!c.ready(), "vx: not ready");
    const auto &in = c.input();
    verifyInput(in, Input{});
    check(in.image == VK_NULL_HANDLE, "vx: image null");
    check(in.range.aspectMask == 0, "vx: aspect 0");
    check(in.before.stages == 0 && in.during.stages == 0 && in.after.stages == 0, "vx: stages 0");
    check(in.before.accesses == 0 && in.during.accesses == 0 && in.after.accesses == 0, "vx: access 0");
    for (const auto *e : {&c.pre(), &c.post()}) {
        check(e->srcStages == 0 && e->dstStages == 0 && e->flags == 0, "vx: edge scalars 0");
        check(e->barrier.sType == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, "vx: sType");
        check(e->barrier.pNext == nullptr, "vx: pNext null");
        check(e->barrier.image == VK_NULL_HANDLE, "vx: barrier image null");
        check(e->barrier.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
                  e->barrier.newLayout == VK_IMAGE_LAYOUT_UNDEFINED,
              "vx: layouts undefined");
        check(e->barrier.srcAccessMask == 0 && e->barrier.dstAccessMask == 0, "vx: access 0");
        check(e->barrier.subresourceRange.levelCount == 0, "vx: levelCount 0");
        check(sameRange(e->barrier.subresourceRange, VkImageSubresourceRange{}), "vx: all range fields 0");
        check(e->barrier.srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED, "vx: srcQF");
        check(e->barrier.dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED, "vx: dstQF");
    }
}
} // namespace

int main() {
    static_assert(!std::is_copy_constructible<VKRenderingTransitionContract>::value &&
                      !std::is_copy_assignable<VKRenderingTransitionContract>::value &&
                      !std::is_move_constructible<VKRenderingTransitionContract>::value &&
                      !std::is_move_assignable<VKRenderingTransitionContract>::value,
                  "non-copy/move");

    // --- 1: initial cleared ---
    {
        VKRenderingTransitionContract c;
        verifyCleared(c);
    }

    // --- 2: full asymmetric states through owner and encoder ---
    {
        VKRenderingTransitionContract c;
        auto in = makeInput();
        check(c.build(in), "full build");
        check(c.ready(), "full: ready");
        verifyComplete(c, in);
        verifyPre(c, in);
        verifyPost(c, in);
        verifyEdgeThroughEncoder(c.pre(), in.before.stages, in.during.stages, in.preFlags,
                                 in.before.layout, in.during.layout, in.before.accesses,
                                 in.during.accesses);
        verifyEdgeThroughEncoder(c.post(), in.during.stages, in.after.stages, in.postFlags,
                                 in.during.layout, in.after.layout, in.during.accesses,
                                 in.after.accesses);
    }

    // --- 3: equal layouts still produce both dependencies ---
    {
        VKRenderingTransitionContract c;
        Input in = makeInput();
        const Use gen{VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
        in.before = in.during = in.after = gen;
        in.preFlags = in.postFlags = VK_DEPENDENCY_BY_REGION_BIT;
        check(c.build(in), "equal layouts");
        verifyComplete(c, in);
        verifyEdge(c.pre(), gen.stages, gen.stages, in.preFlags, VK_IMAGE_LAYOUT_GENERAL,
                   VK_IMAGE_LAYOUT_GENERAL, gen.accesses, gen.accesses);
        verifyEdge(c.post(), gen.stages, gen.stages, in.postFlags, VK_IMAGE_LAYOUT_GENERAL,
                   VK_IMAGE_LAYOUT_GENERAL, gen.accesses, gen.accesses);
    }

    // --- 4: GENERAL offscreen during + SHADER_READ after preserved ---
    {
        VKRenderingTransitionContract c;
        Input in = makeInput();
        in.during.layout = VK_IMAGE_LAYOUT_GENERAL;
        in.after.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        check(c.build(in), "general offscreen");
        verifyComplete(c, in);
        check(c.pre().barrier.newLayout == VK_IMAGE_LAYOUT_GENERAL, "go: pre new GENERAL");
        check(c.post().barrier.oldLayout == VK_IMAGE_LAYOUT_GENERAL, "go: post old GENERAL");
        check(c.post().barrier.newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              "go: post new SHADER_READ");
    }

    // --- 5: after PRESENT allowed and preserved ---
    {
        VKRenderingTransitionContract c;
        Input in = makeInput();
        in.after.layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        check(c.build(in), "present after");
        verifyComplete(c, in);
        check(c.post().barrier.newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, "pa: present kept");
    }

    // --- 6: depth/stencil aspect range preserved ---
    {
        VKRenderingTransitionContract c;
        Input in = makeInput();
        in.range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        check(c.build(in), "ds range");
        verifyComplete(c, in);
        check(c.pre().barrier.subresourceRange.aspectMask ==
                  (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT),
              "ds: aspect preserved");
        check(c.post().barrier.subresourceRange.aspectMask ==
                  (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT),
              "ds: post aspect preserved");
    }

    // --- 7: VK_REMAINING_* preserved ---
    {
        VKRenderingTransitionContract c;
        Input in = makeInput();
        in.range.levelCount = VK_REMAINING_MIP_LEVELS;
        in.range.layerCount = VK_REMAINING_ARRAY_LAYERS;
        check(c.build(in), "remaining");
        verifyComplete(c, in);
        check(c.pre().barrier.subresourceRange.levelCount == VK_REMAINING_MIP_LEVELS,
              "rem: mip levels");
        check(c.post().barrier.subresourceRange.layerCount == VK_REMAINING_ARRAY_LAYERS,
              "rem: array layers");
    }

    // --- 8: before UNDEFINED keeps its stages/accesses (no TOP forcing) ---
    {
        VKRenderingTransitionContract c;
        Input in = makeInput();
        in.before.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        in.before.stages = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        in.before.accesses = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        check(c.build(in), "before undefined");
        verifyComplete(c, in);
        check(c.pre().barrier.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED, "bu: old UNDEFINED");
        check(c.pre().srcStages == VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, "bu: src stage kept");
        check(c.pre().barrier.srcAccessMask == VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
              "bu: src access kept");
    }

    // --- 9: owning: caller mutation leaves owner unchanged ---
    {
        VKRenderingTransitionContract c;
        Input in = makeInput();
        check(c.build(in), "owning build");
        in.image = reinterpret_cast<VkImage>(static_cast<uintptr_t>(0xFFF));
        in.range.levelCount = 9;
        in.before.stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        in.during.layout = VK_IMAGE_LAYOUT_GENERAL;
        in.preFlags = 0;
        in = Input{}; // change every caller field, including all three uses
        verifyComplete(c, makeInput());
        verifyPre(c, makeInput());
        verifyPost(c, makeInput());
    }

    // --- 10: scoped destruction: full read after input dies ---
    {
        VKRenderingTransitionContract c;
        {
            Input in = makeInput();
            check(c.build(in), "scoped");
        }
        verifyComplete(c, makeInput());
        verifyPre(c, makeInput());
        verifyPost(c, makeInput());
    }

    // --- 11: two owners independent ---
    {
        VKRenderingTransitionContract a, b;
        Input in = makeInput();
        check(a.build(in) && b.build(in), "two owners");
        Input in2 = makeInput();
        in2.during.layout = VK_IMAGE_LAYOUT_GENERAL;
        in2.after.layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        check(a.build(in2), "rebuild a");
        verifyComplete(a, in2);
        verifyComplete(b, in);
        check(a.pre().barrier.newLayout == VK_IMAGE_LAYOUT_GENERAL, "a: new GENERAL");
        check(b.pre().barrier.newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              "b: untouched");
    }

    // --- 12: self-input rebuild preserves tuple ---
    {
        VKRenderingTransitionContract c;
        Input in = makeInput();
        check(c.build(in), "self init");
        check(c.build(c.input()), "self rebuild");
        verifyComplete(c, in);
        verifyPre(c, in);
        verifyPost(c, in);
    }

    // --- 13: every invalid shape: prime -> bad -> cleared -> recover ---
    {
        VKRenderingTransitionContract c;
        const Input good = makeInput();
        auto cycle = [&](const char *label, Input bad) {
            check(c.build(good), label);
            verifyComplete(c, good);
            verifyPre(c, good);
            check(!c.build(bad), label);
            verifyCleared(c);
            check(c.build(good), label);
            verifyComplete(c, good);
            verifyPost(c, good);
        };
        auto t = good;
        t.image = VK_NULL_HANDLE;
        cycle("image null", t);
        t = good;
        t.range.aspectMask = 0;
        cycle("aspect 0", t);
        t = good;
        t.range.aspectMask = 0x40;
        cycle("aspect unknown", t);
        t = good;
        t.range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT;
        cycle("aspect mixed", t);
        t = good;
        t.range.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
        cycle("aspect plane", t);
        t = good;
        t.range.levelCount = 0;
        cycle("levelCount 0", t);
        t = good;
        t.range.layerCount = 0;
        cycle("layerCount 0", t);
        t = good;
        t.before.stages = 0;
        cycle("before stages 0", t);
        t = good;
        t.during.stages = 0;
        cycle("during stages 0", t);
        t = good;
        t.after.stages = 0;
        cycle("after stages 0", t);
        t = good;
        t.during.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        cycle("during UNDEFINED", t);
        t = good;
        t.during.layout = VK_IMAGE_LAYOUT_PREINITIALIZED;
        cycle("during PREINIT", t);
        t = good;
        t.during.layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        cycle("during PRESENT", t);
        t = good;
        t.after.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        cycle("after UNDEFINED", t);
        t = good;
        t.after.layout = VK_IMAGE_LAYOUT_PREINITIALIZED;
        cycle("after PREINIT", t);
    }

    // --- 14: rebuild sequence leaves no stale fields ---
    {
        VKRenderingTransitionContract c;
        Input in1 = makeInput();
        check(c.build(in1), "seq 1");
        Input in2 = makeInput();
        in2.image = reinterpret_cast<VkImage>(static_cast<uintptr_t>(0x777));
        in2.range.baseMipLevel = 5;
        in2.range.levelCount = 1;
        in2.before.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        check(c.build(in2), "seq 2");
        verifyComplete(c, in2);
        check(c.pre().barrier.image == reinterpret_cast<VkImage>(static_cast<uintptr_t>(0x777)),
              "seq: new image");
        check(c.pre().barrier.subresourceRange.baseMipLevel == 5, "seq: new baseMip");
        check(c.pre().barrier.subresourceRange.levelCount == 1, "seq: new levelCount");
        check(c.pre().barrier.oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              "seq: new oldLayout");
        Input bad = makeInput();
        bad.during.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(!c.build(bad), "seq bad");
        verifyCleared(c);
        check(c.build(in1), "seq recover");
        verifyComplete(c, in1);
        verifyPre(c, in1);
        verifyPost(c, in1);
    }

    // --- 15: public reset() ---
    {
        VKRenderingTransitionContract c;
        check(c.build(makeInput()), "reset prime");
        c.reset();
        verifyCleared(c);
    }

    if (g_failures == 0) {
        std::printf("rendering transition tests: all PASS\n");
        return 0;
    }
    std::printf("rendering transition tests: %u FAIL\n", g_failures);
    return 1;
}
