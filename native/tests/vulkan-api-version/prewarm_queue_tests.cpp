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

#include "VKPipelinePrewarm.h"

#ifdef VKPREWARM_STANDALONE_PROBE
int main() { return 0; }
#else

    #include <cstdio>
    #include <cstdlib>
    #include <memory>
    #include <new>
    #include <stdexcept>
    #include <string>
    #include <utility>
    #include <vector>

using namespace cc::gfx;
using Queue = VulkanPipelinePrewarmQueue;
using Key = Queue::Key;

static unsigned failures = 0;
static void check(bool ok, const char *label) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", label);
    }
}

static unsigned g_pqInjections = 0;
static bool g_pqFailing = false;
static long g_pqSkipAllocs = 0;
static long g_pqAllocCount = 0;
void *operator new(size_t size) {
    ++g_pqAllocCount;
    if (g_pqSkipAllocs > 0) {
        --g_pqSkipAllocs;
    } else if (g_pqFailing) {
        ++g_pqInjections;
        throw std::bad_alloc();
    }
    if (void *p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }

// ---- fixtures: sentinel handles only; no Vulkan calls anywhere ----
static Key makeComputeKey(VkPipelineCreateFlags flags = 0x42) {
    VulkanShaderStageKey shader;
    VkPipelineShaderStageCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.module = reinterpret_cast<VkShaderModule>(static_cast<uintptr_t>(0xC001));
    info.pName = "main";
    const uint32_t words[]{0x07230203, 0x00010000, 0, 1, 0};
    if (!VulkanShaderStageKey::build(info, words, 5, shader)) return Key{};
    VulkanPipelineLayoutKey layout;
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (!VulkanPipelineLayoutKey::build(layoutInfo, {}, layout)) return Key{};
    VulkanComputePipelineKey key;
    if (!VulkanComputePipelineKey::build(flags, shader, layout, key)) return Key{};
    return Key{std::move(key)};
}

struct GraphicsFixture {
    VulkanShaderStageKey shaders[2];
    const VulkanShaderStageKey *keys[2] = {&shaders[0], &shaders[1]};
    VulkanPipelineLayoutKey layout;
    VulkanClassicRenderPassKey pass;
    VkPipelineShaderStageCreateInfo stages[2]{};
    VkVertexInputBindingDescription binding{0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attribute{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkViewport rectangle{1, 2, 17, 11, 0, 1};
    VkRect2D scissor{{3, 4}, {17, 11}};
    VkDynamicState dynamicStates[2]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState attachment{};
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo root{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    GraphicsFixture() {
        const uint32_t code[]{0x07230203, 0x00010000, 0, 1, 0};
        for (unsigned i = 0; i < 2; ++i) {
            stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
            stages[i].module = reinterpret_cast<VkShaderModule>(static_cast<uintptr_t>(101 + i));
            stages[i].pName = "main";
            VulkanShaderStageKey::build(stages[i], code, 5, shaders[i]);
        }
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        VulkanPipelineLayoutKey::build(layoutInfo, {}, layout);
        VkAttachmentDescription2 desc{VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
        desc.format = VK_FORMAT_R8G8B8A8_UNORM;
        desc.samples = VK_SAMPLE_COUNT_1_BIT;
        VkAttachmentReference2 ref{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
        ref.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        VkSubpassDescription2 subpass{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &ref;
        VkRenderPassCreateInfo2 rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
        rp.attachmentCount = 1;
        rp.pAttachments = &desc;
        rp.subpassCount = 1;
        rp.pSubpasses = &subpass;
        VulkanClassicRenderPassKey::build(rp, pass);
        vertex.vertexBindingDescriptionCount = 1;
        vertex.pVertexBindingDescriptions = &binding;
        vertex.vertexAttributeDescriptionCount = 1;
        vertex.pVertexAttributeDescriptions = &attribute;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        raster.lineWidth = 1;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        viewport.pViewports = &rectangle;
        viewport.pScissors = &scissor;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamicStates;
        attachment.srcColorBlendFactor = attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.colorWriteMask = 15;
        blend.attachmentCount = 1;
        blend.pAttachments = &attachment;
        root.stageCount = 2;
        root.pStages = stages;
        root.layout = reinterpret_cast<VkPipelineLayout>(static_cast<uintptr_t>(103));
        root.renderPass = reinterpret_cast<VkRenderPass>(static_cast<uintptr_t>(104));
        root.pVertexInputState = &vertex;
        root.pInputAssemblyState = &assembly;
        root.pRasterizationState = &raster;
        root.pMultisampleState = &ms;
        root.pViewportState = &viewport;
        root.pDynamicState = &dynamic;
        root.pColorBlendState = &blend;
    }
    bool buildKey(VulkanGraphicsPipelineKey &out) const {
        return VulkanGraphicsPipelineKey::build(root, keys, 2, layout, &pass, out);
    }
    Key key() const {
        VulkanGraphicsPipelineKey g;
        if (!buildKey(g)) return Key{};
        return Key{std::move(g)};
    }
};

static Queue::Job makeJob(const char *label, Key expected, Queue::Creator creator) {
    Queue::Job job;
    job.label = label;
    job.expected = std::move(expected);
    job.create = std::move(creator);
    return job;
}

static Queue::Job okJob(const char *label, Key expected) {
    return makeJob(label, std::move(expected), []() {
        return Queue::CreateResult{true, Key{} /*replaced below*/};
    });
}

int main() {
    const Key computeKey = makeComputeKey();
    GraphicsFixture fixture;
    const Key graphicsKey = fixture.key();
    check(std::holds_alternative<VulkanComputePipelineKey>(computeKey) &&
              std::get<VulkanComputePipelineKey>(computeKey).valid(),
          "compute fixture valid");
    check(std::holds_alternative<VulkanGraphicsPipelineKey>(graphicsKey) &&
              std::get<VulkanGraphicsPipelineKey>(graphicsKey).valid(),
          "graphics fixture valid");

    // ---- 1: enqueue valid jobs; manifest is queue-owned ----
    {
        Queue q;
        auto job = okJob("graphics-main", graphicsKey);
        job.create = [key = graphicsKey]() mutable {
            return Queue::CreateResult{true, key};
        };
        check(q.enqueue(job) == Queue::EnqueueStatus::Added, "enqueue graphics Added");
        // caller mutates label + key; queue-owned entry unchanged
        job.label = "mutated";
        job.expected = Key{};
        check(q.size() == 1 && q.entries()[0].job.label == "graphics-main", "caller label mutation ignored");
        check(q.entries()[0].job.expected == graphicsKey,
              "queue-owned key intact after caller mutation");
        // scoped destroy of source
        {
            const auto scopedKey = makeComputeKey(0x771);
            auto scopedJob = makeJob("scoped", scopedKey, [key = scopedKey]() { return Queue::CreateResult{true, key}; });
            check(q.enqueue(scopedJob) == Queue::EnqueueStatus::Added, "scoped source publishes surviving entry");
        }
        check(q.size() == 2 && q.entries()[0].job.expected == graphicsKey &&
                  q.entries()[1].job.expected == makeComputeKey(0x771),
              "complete values survive input destruction");
        check(q.drain(2).ready == 2, "surviving closures produce both complete keys");
    }

    // ---- 2: closure lifetime via shared_ptr/weak_ptr ----
    {
        Queue q;
        auto state = std::make_shared<int>(7);
        std::weak_ptr<int> watch = state;
        auto job = makeJob("closure", computeKey, [state, key = computeKey]() {
            return Queue::CreateResult{true, key};
        });
        check(q.enqueue(job) == Queue::EnqueueStatus::Added, "closure enqueue");
        job = Queue::Job{}; // Leave the queue as the only closure owner BEFORE observation.
        state.reset();
        check(!watch.expired(), "closure keeps capture alive");
        auto report = q.drain(8);
        check(report.attempted == 1 && report.ready == 1 && report.failed == 0,
              "closure drain ready");
        check(watch.expired() == false, "entry still owns capture after drain");
        check(q.clear(), "clear");
        state.reset();
        check(watch.expired(), "clear releases closure capture");
    }

    // ---- 3: FIFO / budget / failure consumes budget / continue later ----
    {
        Queue q;
        std::vector<unsigned> order;
        const Key kR1 = makeComputeKey(0xB1);
        const Key kF1 = makeComputeKey(0xB2);
        auto readyJob = makeJob("r1", kR1, [&, key = kR1]() {
            order.push_back(1);
            return Queue::CreateResult{true, key};
        });
        auto failJob = makeJob("f1", kF1, [&]() {
            order.push_back(2);
            return Queue::CreateResult{false, Key{}};
        });
        auto readyJob2 = makeJob("r2", graphicsKey, [&, key = graphicsKey]() {
            order.push_back(3);
            return Queue::CreateResult{true, key};
        });
        check(q.enqueue(readyJob) == Queue::EnqueueStatus::Added, "fifo add1");
        check(q.enqueue(failJob) == Queue::EnqueueStatus::Added, "fifo add2");
        check(q.enqueue(readyJob2) == Queue::EnqueueStatus::Added, "fifo add3");
        auto r0 = q.drain(0);
        check(r0.attempted == 0 && r0.ready == 0 && r0.failed == 0 && !r0.busy && order.empty(), "drain 0 attempts nothing");
        auto r1 = q.drain(1);
        check(r1.attempted == 1 && r1.ready == 1 && r1.failed == 0 && !r1.busy && order == std::vector<unsigned>{1}, "drain budget 1 first FIFO ready");
        check(q.entries()[0].state == Queue::JobState::Ready, "entry0 Ready");
        auto r2 = q.drain(1);
        check(r2.attempted == 1 && r2.failed == 1 && r2.ready == 0 && !r2.busy && order == std::vector<unsigned>{1, 2}, "failure consumes budget");
        check(q.entries()[1].state == Queue::JobState::Failed, "entry1 Failed");
        auto r3 = q.drain(8);
        check(r3.attempted == 1 && r3.ready == 1 && r3.failed == 0 && !r3.busy && order == std::vector<unsigned>{1, 2, 3}, "next drain continues at cursor");
        check(q.entries()[2].state == Queue::JobState::Ready, "entry2 Ready");
        auto r4 = q.drain(8);
        check(r4.attempted == 0 && r4.ready == 0 && r4.failed == 0 && !r4.busy && order == std::vector<unsigned>{1, 2, 3}, "done jobs never rerun");
        // append after drain joins the same wave
        const Key kLate = makeComputeKey(0xB3);
        auto late = makeJob("late", kLate, [key = kLate]() {
            return Queue::CreateResult{true, key};
        });
        check(q.enqueue(late) == Queue::EnqueueStatus::Added, "late append");
        auto r5 = q.drain(8);
        check(r5.attempted == 1 && r5.ready == 1, "late job drained");
    }

    // ---- 4: failure modes: created=false / invalid actual / different actual / throw ----
    {
        Queue q;
        const Key kA = makeComputeKey(0xA0);
        const Key kB = makeComputeKey(0xA1);
        const Key kC = makeComputeKey(0xA2);
        const Key kD = makeComputeKey(0xA3);
        const Key kE = makeComputeKey(0xA4);
        const Key kF = makeComputeKey(0xA5);
        auto j1 = makeJob("not-created", kA, [key = kA]() { return Queue::CreateResult{false, key}; });
        auto j2 = makeJob("invalid-actual", kB, []() { return Queue::CreateResult{true, Key{}}; });
        auto j3 = makeJob("wrong-actual", kC, [key = graphicsKey]() {
            return Queue::CreateResult{true, key};
        });
        auto j4 = makeJob("throws", kD, []() -> Queue::CreateResult { throw std::runtime_error("boom"); });
        auto j5 = makeJob("badalloc", kE, []() -> Queue::CreateResult { throw std::bad_alloc(); });
        check(q.enqueue(j1) == Queue::EnqueueStatus::Added, "f add1");
        check(q.enqueue(j2) == Queue::EnqueueStatus::Added, "f add2");
        check(q.enqueue(j3) == Queue::EnqueueStatus::Added, "f add3");
        check(q.enqueue(j4) == Queue::EnqueueStatus::Added, "f add4");
        check(q.enqueue(j5) == Queue::EnqueueStatus::Added, "f add5");
        auto j6 = makeJob("wrong-compute-tuple", kF, [key = kA]() { return Queue::CreateResult{true, key}; });
        check(q.enqueue(j6) == Queue::EnqueueStatus::Added, "f add6");
        auto report = q.drain(8);
        check(report.attempted == 6 && report.failed == 6 && report.ready == 0 && !report.busy,
              "all six isolated failure modes Failed");
        for (size_t i = 0; i < 6; ++i) {
            check(q.entries()[i].state == Queue::JobState::Failed, "failure entry state");
        }
    }

    // ---- 5: callback reentrancy: enqueue Busy / clear false / nested drain busy ----
    {
        Queue q;
        Queue *qPtr = &q;
        auto reentrant = makeJob("reentrant", computeKey, [qPtr, key = computeKey]() {
            check(qPtr->enqueue(okJob("inner", key)) == Queue::EnqueueStatus::Busy,
                  "reentrant enqueue Busy");
            check(!qPtr->clear(), "reentrant clear false");
            auto nested = qPtr->drain(8);
            check(nested.busy && nested.attempted == 0 && nested.ready == 0 && nested.failed == 0, "nested drain busy zero");
            return Queue::CreateResult{true, key};
        });
        check(q.enqueue(reentrant) == Queue::EnqueueStatus::Added, "reentrant add");
        unsigned laterCalls = 0;
        const auto laterKey = makeComputeKey(0x772);
        check(q.enqueue(makeJob("after-reentry", laterKey, [&, key = laterKey]() {
            ++laterCalls;
            return Queue::CreateResult{true, key};
        })) == Queue::EnqueueStatus::Added,
              "append independent successor");
        auto report = q.drain(8);
        check(report.attempted == 2 && report.ready == 2 && report.failed == 0 && !report.busy && laterCalls == 1, "outer continues to successor after reentrancy");
        check(q.entries()[0].state == Queue::JobState::Ready, "reentrant entry Ready");
    }

    // ---- 6: capacity 0 / full; full duplicate still Duplicate ----
    {
        Queue q{0};
        auto job = okJob("cap0", computeKey);
        job.create = [key = computeKey]() { return Queue::CreateResult{true, key}; };
        check(q.enqueue(job) == Queue::EnqueueStatus::Full, "capacity 0 new job Full");
        check(q.enqueue(job) == Queue::EnqueueStatus::Full, "capacity 0 duplicate Full (no entry to dup)");
        Queue q2{1};
        auto j1 = makeJob("c1", computeKey, [key = computeKey]() { return Queue::CreateResult{true, key}; });
        auto j2 = makeJob("c2", graphicsKey, [key = graphicsKey]() { return Queue::CreateResult{true, key}; });
        auto jDup = makeJob("dup-of-c1", computeKey, [key = computeKey]() { return Queue::CreateResult{true, key}; });
        check(q2.enqueue(j1) == Queue::EnqueueStatus::Added, "cap1 add");
        check(q2.enqueue(jDup) == Queue::EnqueueStatus::Duplicate, "full queue duplicate still Duplicate");
        check(q2.enqueue(j2) == Queue::EnqueueStatus::Full, "full queue new job Full");
    }

    // ---- 7: dedup semantics ----
    {
        Queue q{8};
        auto original = makeJob("original", computeKey, [key = computeKey]() {
            return Queue::CreateResult{true, key};
        });
        check(q.enqueue(original) == Queue::EnqueueStatus::Added, "dedup original Added");
        auto dupLabel = makeJob("different-label", computeKey, [key = computeKey]() {
            return Queue::CreateResult{true, key};
        });
        check(q.enqueue(dupLabel) == Queue::EnqueueStatus::Duplicate,
              "same key different label Duplicate");
        auto sameLabel = makeJob("original", graphicsKey, [key = graphicsKey]() {
            return Queue::CreateResult{true, key};
        });
        check(q.enqueue(sameLabel) == Queue::EnqueueStatus::Added,
              "same label different key (different type) kept");
        auto graphicsDup = makeJob("g2", graphicsKey, [key = graphicsKey]() {
            return Queue::CreateResult{true, key};
        });
        check(q.enqueue(graphicsDup) == Queue::EnqueueStatus::Duplicate,
              "graphics duplicate (same label as another entry is irrelevant)");
    }

    // ---- 8: invalid jobs ----
    {
        Queue q;
        auto emptyLabel = okJob("", computeKey);
        check(q.enqueue(emptyLabel) == Queue::EnqueueStatus::Invalid, "empty label Invalid");
        std::string longLabel(257, 'x');
        auto longJob = okJob(longLabel.c_str(), computeKey);
        check(q.enqueue(longJob) == Queue::EnqueueStatus::Invalid, "257-char label Invalid");
        auto nullCreator = makeJob("null-creator", computeKey, nullptr);
        check(q.enqueue(nullCreator) == Queue::EnqueueStatus::Invalid, "null creator Invalid");
        check(q.enqueue(makeJob("invalid-key", Key{}, []() { return Queue::CreateResult{}; })) ==
                  Queue::EnqueueStatus::Invalid,
              "default invalid key Invalid");
        check(q.enqueue(makeJob("invalid-graphics", Key{VulkanGraphicsPipelineKey{}},
                                []() { return Queue::CreateResult{}; })) == Queue::EnqueueStatus::Invalid,
              "invalid graphics key Invalid");
        check(q.enqueue(makeJob("invalid-compute", Key{VulkanComputePipelineKey{}},
                                []() { return Queue::CreateResult{}; })) == Queue::EnqueueStatus::Invalid,
              "invalid compute key Invalid");
        check(q.size() == 0, "invalid enqueues add nothing");
    }

    // ---- 9: clear then new wave ----
    {
        Queue q;
        auto job = makeJob("wave1", computeKey, [key = computeKey]() { return Queue::CreateResult{true, key}; });
        check(q.enqueue(job) == Queue::EnqueueStatus::Added, "wave1 add");
        check(q.drain(8).ready == 1, "wave1 drain");
        check(q.clear(), "clear for wave2");
        check(q.size() == 0, "cleared size 0");
        auto job2 = makeJob("wave1", computeKey, [key = computeKey]() { return Queue::CreateResult{true, key}; });
        check(q.enqueue(job2) == Queue::EnqueueStatus::Added, "same key re-added after clear");
    }

    // ---- 10: allocation failNth on enqueue ----
    {
        // Measure this graphics shape; do not infer a worst case for all jobs.
        long measured = 0;
        {
            Queue q;
            auto job = makeJob("alloc-probe", graphicsKey, [key = graphicsKey]() {
                return Queue::CreateResult{true, key};
            });
            g_pqAllocCount = 0;
            g_pqFailing = false;
            g_pqSkipAllocs = 0;
            check(q.enqueue(job) == Queue::EnqueueStatus::Added, "alloc probe add");
            measured = g_pqAllocCount;
        }
        std::printf("prewarm enqueue fresh allocation points: %ld\n", measured);
        check(measured > 0, "enqueue allocation count must be > 0");

        for (long nth = 1; nth <= measured; ++nth) {
            Queue q;
            auto prior = makeJob("prior", computeKey, [key = computeKey]() {
                return Queue::CreateResult{true, key};
            });
            check(q.enqueue(prior) == Queue::EnqueueStatus::Added, "failNth prior add");
            const size_t priorSize = q.size();
            const Key priorKey = q.entries()[0].job.expected;
            const auto priorState = q.entries()[0].state;

            g_pqInjections = 0;
            // construct the victim job OUTSIDE the guard
            auto job = makeJob("victim", graphicsKey, [key = graphicsKey]() {
                return Queue::CreateResult{true, key};
            });
            g_pqFailing = true;
            g_pqSkipAllocs = nth - 1;
            const auto status = q.enqueue(job);
            g_pqFailing = false;
            g_pqSkipAllocs = 0;
            char tag[64];
            std::snprintf(tag, sizeof(tag), "failNth%ld", nth);
            check(status == Queue::EnqueueStatus::OutOfMemory, tag);
            std::string injLabel = std::string(tag) + ": exactly one injection";
            check(g_pqInjections == 1, injLabel.c_str());
            // original manifest fully unchanged
            std::string keepLabel = std::string(tag) + ": manifest unchanged";
            check(q.size() == priorSize &&
                      q.entries()[0].job.label == "prior" &&
                      q.entries()[0].job.expected == priorKey &&
                      q.entries()[0].state == priorState,
                  keepLabel.c_str());
            // recover: same job enqueues Added and drains
            auto retry = makeJob("victim", graphicsKey, [key = graphicsKey]() {
                return Queue::CreateResult{true, key};
            });
            std::string recLabel = std::string(tag) + ": recover Added";
            check(q.enqueue(retry) == Queue::EnqueueStatus::Added, recLabel.c_str());
            std::string drainLabel = std::string(tag) + ": recover drain";
            check(q.drain(8).ready == 2, drainLabel.c_str()); // prior + retry
        }
    }

    if (failures == 0) {
        std::printf("prewarm queue tests: all PASS\n");
        return 0;
    }
    std::printf("prewarm queue tests: %u FAIL\n", failures);
    return 1;
}
#endif
