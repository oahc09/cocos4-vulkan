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

#include "VKGraphicsPipelineKey.h"

#if defined(VKGRAPHICSKEY_STANDALONE_PROBE)
int main() { return 0; }
#else
    #include <cstdio>
    #include <cstdlib>
    #include <new>
    #include <type_traits>
    #include <unordered_map>
    #include <vector>
    #include "VKRenderingSignature.h"

using namespace cc::gfx;
static unsigned failures = 0;
static unsigned g_gpkInjections = 0;
static bool g_gpkFailing = false;
static long g_gpkSkipAllocs = 0;
static long g_gpkAllocCount = 0;
void *operator new(size_t size) {
    ++g_gpkAllocCount;
    if (g_gpkSkipAllocs > 0) {
        --g_gpkSkipAllocs;
    } else if (g_gpkFailing) {
        ++g_gpkInjections;
        throw std::bad_alloc();
    }
    if (void *p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }
static void check(bool value, const char *label) {
    if (!value) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", label);
    }
}
template <class T>
static T token(uintptr_t value) {
    if constexpr (std::is_pointer_v<T>)
        return reinterpret_cast<T>(value);
    else
        return static_cast<T>(value);
}

struct Fixture {
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
    VkFormat color = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineCreateInfo root{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    Fixture() {
        const uint32_t code[]{0x07230203, 0x00010000, 0, 1, 0};
        for (unsigned i = 0; i < 2; ++i) {
            stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
            stages[i].module = token<VkShaderModule>(101 + i);
            stages[i].pName = "main";
            check(VulkanShaderStageKey::build(stages[i], code, 5, shaders[i]), "fixture shader key");
        }
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        check(VulkanPipelineLayoutKey::build(layoutInfo, {}, layout), "fixture layout key");
        VkAttachmentDescription2 desc{VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
        desc.format = color;
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
        check(VulkanClassicRenderPassKey::build(rp, pass), "fixture RP key");
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
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &color;
        root.stageCount = 2;
        root.pStages = stages;
        root.layout = token<VkPipelineLayout>(103);
        root.renderPass = token<VkRenderPass>(104);
        root.pVertexInputState = &vertex;
        root.pInputAssemblyState = &assembly;
        root.pRasterizationState = &raster;
        root.pMultisampleState = &ms;
        root.pViewportState = &viewport;
        root.pDynamicState = &dynamic;
        root.pColorBlendState = &blend;
    }
    bool build(VulkanGraphicsPipelineKey &out) const {
        return VulkanGraphicsPipelineKey::build(root, keys, 2, layout, &pass, out);
    }
};

int main() {
    check(VulkanGraphicsPipelineKey{}.hash() == UINT64_C(0x791321a567999f23), "independent default LE-byte FNV vector");
    Fixture fixture;
    VulkanGraphicsPipelineKey classic, other, dynamic;
    check(fixture.build(classic), "complete Classic build");
    const std::vector<uint32_t> classicWords{
        0, 0, 0, 0, 0, 1, 0, 16, 0, 1, 0, 0, 109, 0, 0, 3, 0, 0,
        1, 0, 2, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 0,
        1, 0, 0, 15, 0, 0, 0, 0, 0};
    check(classic.words() == classicWords, "all 46 Classic state words match hand-specified stream");
    check(classic.hash() == UINT64_C(0xcee3d48d38ab88ca), "independent full Classic FNV vector");
    check(classic.valid() && !classic.dynamicRendering() && classic.stages().size() == 2 &&
              classic.layout() == fixture.layout && classic.classicPass() == fixture.pass && classic.raster().valid(),
          "Classic full children published");
    fixture.root.renderPass = token<VkRenderPass>(105);
    check(fixture.build(other) && other == classic && other.hash() == classic.hash(), "RP handle is not content");
    fixture.root.pNext = &fixture.rendering;
    fixture.root.renderPass = VK_NULL_HANDLE;
    check(fixture.build(dynamic), "complete Dynamic build");
    check(dynamic.hash() == UINT64_C(0xe5a4c3ca20117c6f), "independent full Dynamic FNV vector");
    check(dynamic.valid() && dynamic.dynamicRendering() && !dynamic.classicPass().valid() &&
              dynamic.renderingSignature().colorFormats == std::vector<VkFormat>{fixture.color} &&
              dynamic.renderingSignature().samples == VK_SAMPLE_COUNT_1_BIT && dynamic != classic,
          "Dynamic uses rendering signature rather than RP handle/key");
    const auto saved = dynamic;
    fixture.color = VK_FORMAT_R16G16B16A16_SFLOAT;
    check(fixture.build(other) && other != saved && other.hash() != saved.hash(), "Dynamic color content differs");
    check(saved.renderingSignature().colorFormats == std::vector<VkFormat>{VK_FORMAT_R8G8B8A8_UNORM}, "caller formats were copied");

    fixture.root.pNext = nullptr;
    fixture.root.renderPass = token<VkRenderPass>(104);
    check(fixture.build(other) && other == classic, "Dynamic-to-Classic rebuild clears stale signature");
    fixture.root.pNext = &fixture.root;
    check(!fixture.build(other) && !other.valid() && other.words().empty() && other.stages().empty() &&
              !other.layout().valid() && !other.raster().valid() && !other.classicPass().valid() &&
              other.renderingSignature() == VKRenderingSignature{},
          "unsupported extension resets all children");
    fixture.root.pNext = nullptr;
    check(fixture.build(other) && other == classic, "recovery restores entire Classic tuple");

    // Direct aliases of every owned child and stage entry point survive snapshot-before-publish.
    if (other.stages().size() == 2) {
        const VulkanShaderStageKey *aliases[]{&other.stages()[0], &other.stages()[1]};
        VkPipelineShaderStageCreateInfo infos[]{fixture.stages[0], fixture.stages[1]};
        infos[0].pName = other.stages()[0].entryPoint().c_str();
        infos[1].pName = other.stages()[1].entryPoint().c_str();
        fixture.root.pStages = infos;
        check(VulkanGraphicsPipelineKey::build(fixture.root, aliases, 2, other.layout(), &other.classicPass(), other) && other == classic,
              "true aliased input preserves complete content");
        fixture.root.pStages = fixture.stages;
    } else
        check(false, "alias requires two valid stages");
    fixture.rectangle.width = 99;
    fixture.scissor.extent.height = 99;
    check(fixture.build(other) && other == classic, "dynamic viewport/scissor values are ignored");
    fixture.root.pDynamicState = nullptr;
    check(fixture.build(other) && other != classic, "static viewport/scissor values are captured");
    fixture.rectangle.width = 98;
    VulkanGraphicsPipelineKey changed;
    check(fixture.build(changed) && changed != other, "static viewport scalar changes content");
    // ===================== Task A/B/C additions =====================
    auto verifyReset = [](const VulkanGraphicsPipelineKey &k) {
        check(!k.valid() && !k.dynamicRendering(), "reset: invalid + non-dynamic");
        check(k.stages().empty() && k.words().empty(), "reset: stages/words empty");
        check(!k.layout().valid() && !k.raster().valid() && !k.classicPass().valid(),
              "reset: children invalid");
        check(k.renderingSignature() == VKRenderingSignature{}, "reset: signature default");
        check(k == VulkanGraphicsPipelineKey{}, "reset: equals fresh default key");
        check(k.hash() == VulkanGraphicsPipelineKey{}.hash(), "reset: default hash");
    };

    // ---- Task A: field-family single-field sensitivity ----
    {
        Fixture f;
        VulkanGraphicsPipelineKey base, k;
        check(f.build(base), "sens base Classic");
        auto mut = [&]() {
            check(f.build(k), "sens build");
            return k != base && k.hash() != base.hash();
        };
        f.root.flags = VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;
        check(mut(), "root flags sens");
        f.root.flags = 0;
        f.root.basePipelineIndex = -1;
        check(mut(), "basePipelineIndex sens");
        f.root.basePipelineIndex = 0;
        {
            // swap stage order together with keys: each stage still matches its key
            VkPipelineShaderStageCreateInfo st[2] = {f.stages[1], f.stages[0]};
            f.root.pStages = st;
            const VulkanShaderStageKey *swapped[2] = {f.shaders + 1, f.shaders + 0};
            check(VulkanGraphicsPipelineKey::build(f.root, swapped, 2, f.layout, &f.pass, k),
                  "stage order build");
            check(k != base && k.hash() != base.hash(), "stage ordered children sens");
            f.root.pStages = f.stages;
        }
        f.binding.stride = 32;
        check(mut(), "binding stride sens");
        f.binding.stride = 16;
        f.attribute.offset = 4;
        check(mut(), "attribute offset sens");
        f.attribute.offset = 0;
        f.assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        check(mut(), "topology sens");
        f.assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        f.assembly.primitiveRestartEnable = VK_TRUE;
        check(mut(), "primitiveRestart sens");
        f.assembly.primitiveRestartEnable = VK_FALSE;
        VkPipelineTessellationStateCreateInfo tess{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
        tess.patchControlPoints = 3;
        f.root.pTessellationState = &tess;
        check(mut(), "tess presence sens");
        VulkanGraphicsPipelineKey withTess;
        check(f.build(withTess), "tess baseline");
        tess.patchControlPoints = 4;
        check(f.build(k) && k != withTess && k.hash() != withTess.hash(), "tess patchCount sens");
        f.root.pTessellationState = nullptr;
        f.dynamic.flags = 1;
        check(mut(), "dynamic flags sens");
        f.dynamic.flags = 0;
        f.blend.flags = 1;
        check(mut(), "blend flags sens");
        f.blend.flags = 0;
        f.blend.logicOpEnable = VK_TRUE;
        check(mut(), "logicOpEnable sens");
        f.blend.logicOpEnable = VK_FALSE;
        f.blend.logicOp = VK_LOGIC_OP_COPY_INVERTED;
        check(mut(), "logicOp sens");
        f.blend.logicOp = VK_LOGIC_OP_CLEAR;
        f.attachment.blendEnable = VK_TRUE;
        check(mut(), "blendEnable sens");
        f.attachment.blendEnable = VK_FALSE;
        f.attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        check(mut(), "srcColorFactor sens");
        f.attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        f.attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        check(mut(), "dstColorFactor sens");
        f.attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        f.attachment.colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
        check(mut(), "colorBlendOp sens");
        f.attachment.colorBlendOp = VK_BLEND_OP_ADD;
        f.attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
        check(mut(), "srcAlphaFactor sens");
        f.attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        f.attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        check(mut(), "dstAlphaFactor sens");
        f.attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        f.attachment.alphaBlendOp = VK_BLEND_OP_MIN;
        check(mut(), "alphaBlendOp sens");
        f.attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        f.attachment.colorWriteMask = 7;
        check(mut(), "colorWriteMask sens");
        f.attachment.colorWriteMask = 15;
        f.blend.blendConstants[0] = 0.5f;
        check(mut(), "blendConstant sens");
        f.blend.blendConstants[0] = 0.0f;
    }

    // Each case owns a fresh pointer-bearing fixture; no restore can hide another field's bug.
    {
        const auto sensitivity = [&](const char *label, void (*change)(Fixture &), bool staticViewport = false) {
            Fixture f;
            if (staticViewport) f.root.pDynamicState = nullptr;
            VulkanGraphicsPipelineKey before, after;
            check(f.build(before), "fresh sensitivity prime");
            change(f);
            check(f.build(after) && after != before && after.hash() != before.hash(), label);
        };
        sensitivity("binding number", [](Fixture &f) { f.binding.binding = 2; });
        sensitivity("binding input rate", [](Fixture &f) { f.binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE; });
        sensitivity("attribute location", [](Fixture &f) { f.attribute.location = 2; });
        sensitivity("attribute binding", [](Fixture &f) { f.attribute.binding = 2; });
        sensitivity("attribute format", [](Fixture &f) { f.attribute.format = VK_FORMAT_R32G32_SFLOAT; });
        sensitivity("vertex flags", [](Fixture &f) { f.vertex.flags = 1; });
        sensitivity("assembly flags", [](Fixture &f) { f.assembly.flags = 1; });
        sensitivity("viewport flags", [](Fixture &f) { f.viewport.flags = 1; });
        sensitivity("dynamic state order", [](Fixture &f) { std::swap(f.dynamicStates[0], f.dynamicStates[1]); });
        sensitivity("dynamic state count", [](Fixture &f) { f.dynamic.dynamicStateCount = 1; });
        sensitivity("static viewport x", [](Fixture &f) { f.rectangle.x = 5; }, true);
        sensitivity("static viewport y", [](Fixture &f) { f.rectangle.y = 5; }, true);
        sensitivity("static viewport width", [](Fixture &f) { f.rectangle.width = 5; }, true);
        sensitivity("static viewport height", [](Fixture &f) { f.rectangle.height = 5; }, true);
        sensitivity("static viewport minDepth", [](Fixture &f) { f.rectangle.minDepth = .25F; }, true);
        sensitivity("static viewport maxDepth", [](Fixture &f) { f.rectangle.maxDepth = .75F; }, true);
        sensitivity("static scissor x", [](Fixture &f) { f.scissor.offset.x = 5; }, true);
        sensitivity("static scissor y", [](Fixture &f) { f.scissor.offset.y = 5; }, true);
        sensitivity("static scissor width", [](Fixture &f) { f.scissor.extent.width = 5; }, true);
        sensitivity("static scissor height", [](Fixture &f) { f.scissor.extent.height = 5; }, true);
        for (unsigned i = 0; i < 4; ++i) {
            Fixture f;
            VulkanGraphicsPipelineKey before, after;
            check(f.build(before), "blend constant prime");
            f.blend.blendConstants[i] = .25F;
            check(f.build(after) && after != before && after.hash() != before.hash(), "each blend constant independently changes content");
        }
    }

    // ---- Dynamic single-field sensitivity on signature ----
    {
        Fixture f;
        f.root.pNext = &f.rendering;
        f.root.renderPass = VK_NULL_HANDLE;
        VulkanGraphicsPipelineKey base, k;
        check(f.build(base), "sens base Dynamic");
        check(base.dynamicRendering(), "base dynamic flag");
        f.color = VK_FORMAT_R16G16B16A16_SFLOAT;
        check(f.build(k) && k != base && k.hash() != base.hash(), "sig color format sens");
        f.color = VK_FORMAT_R8G8B8A8_UNORM;
        f.rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
        check(f.build(k) && k != base && k.hash() != base.hash(), "sig depth format sens");
        f.rendering.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
        f.ms.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;
        check(f.build(k) && k != base && k.hash() != base.hash(), "sig samples sens");
        f.ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        f.rendering.viewMask = 0x3u;
        check(f.build(k) && k != base && k.hash() != base.hash(), "sig viewMask sens");
        f.rendering.viewMask = 0;
        f.rendering.stencilAttachmentFormat = VK_FORMAT_S8_UINT;
        check(f.build(k) && k != base && k.hash() != base.hash(), "sig stencil format sens");
        f.rendering.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
        f.root.pNext = nullptr;
        f.root.renderPass = token<VkRenderPass>(104);
    }

    // ---- Task B: rejection + full reset + recover (fresh fixture per case) ----
    {
        auto rejectCase = [&](const char *label, void (*corrupt)(Fixture &)) {
            Fixture f;
            VulkanGraphicsPipelineKey prime, out;
            check(f.build(prime) && f.build(out) && out == prime, label);
            corrupt(f);
            check(!VulkanGraphicsPipelineKey::build(f.root, f.keys, 2, f.layout, &f.pass, out),
                  label);
            verifyReset(out);
            Fixture g;
            check(g.build(out) && out == prime, label);
        };
        rejectCase("bad layout invalid", [](Fixture &f) { f.layout = VulkanPipelineLayoutKey{}; });
        rejectCase("root sType", [](Fixture &f) { f.root.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO; });
        rejectCase("root layout handle", [](Fixture &f) { f.root.layout = VK_NULL_HANDLE; });
        rejectCase("vertex required", [](Fixture &f) { f.root.pVertexInputState = nullptr; });
        rejectCase("assembly required", [](Fixture &f) { f.root.pInputAssemblyState = nullptr; });
        rejectCase("raster required", [](Fixture &f) { f.root.pRasterizationState = nullptr; });
        rejectCase("multisample required", [](Fixture &f) { f.root.pMultisampleState = nullptr; });
        rejectCase("viewport required", [](Fixture &f) { f.root.pViewportState = nullptr; });
        rejectCase("vertex sType", [](Fixture &f) { f.vertex.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO; });
        rejectCase("vertex pNext", [](Fixture &f) { f.vertex.pNext = &f.root; });
        rejectCase("assembly pNext", [](Fixture &f) { f.assembly.pNext = &f.root; });
        rejectCase("viewport pNext", [](Fixture &f) { f.viewport.pNext = &f.root; });
        rejectCase("dynamic pNext", [](Fixture &f) { f.dynamic.pNext = &f.root; });
        rejectCase("blend pNext", [](Fixture &f) { f.blend.pNext = &f.root; });
        rejectCase("vertex binding count cap", [](Fixture &f) { f.vertex.vertexBindingDescriptionCount = 257; });
        rejectCase("vertex null attributes", [](Fixture &f) { f.vertex.pVertexAttributeDescriptions = nullptr; });
        rejectCase("dynamic NULL states", [](Fixture &f) { f.dynamic.pDynamicStates = nullptr; });
        rejectCase("viewport static NULL scissor", [](Fixture &f) { f.root.pDynamicState = nullptr; f.viewport.pScissors = nullptr; });
        rejectCase("stageCount 0", [](Fixture &f) { f.root.stageCount = 0; });
        rejectCase("stageCount 6", [](Fixture &f) { f.root.stageCount = 6; });
        rejectCase("stage key count mismatch", [](Fixture &f) { f.root.stageCount = 1; });
        rejectCase("null stages", [](Fixture &f) { f.root.pStages = nullptr; });
        rejectCase("null stage keys", [](Fixture &f) { f.keys[0] = nullptr; });
        rejectCase("stage entry mismatch", [](Fixture &f) { f.stages[0].pName = "vs"; });
        rejectCase("stage null module", [](Fixture &f) { f.stages[0].module = VK_NULL_HANDLE; });
        rejectCase("stage spec constants", [](Fixture &f) {
            static const VkSpecializationInfo spec{};
            f.stages[0].pSpecializationInfo = &spec;
        });
        rejectCase("stage pNext", [](Fixture &f) { f.stages[0].pNext = &f.root; });
        rejectCase("compute stage", [](Fixture &f) {
            f.stages[0].stage = VK_SHADER_STAGE_COMPUTE_BIT;
            const uint32_t words[]{0x07230203, 0x00010000, 0, 1, 0};
            check(VulkanShaderStageKey::build(f.stages[0], words, 5, f.shaders[0]), "compute rejection matching key");
        });
        rejectCase("duplicate stage", [](Fixture &f) { f.stages[1] = f.stages[0]; f.keys[1] = f.keys[0]; });
        rejectCase("NULL renderPass without Dynamic", [](Fixture &f) { f.root.renderPass = VK_NULL_HANDLE; });
        rejectCase("Classic with invalid classicPass key", [](Fixture &f) {
            f.pass = VulkanClassicRenderPassKey{};
        });
        rejectCase("subpass beyond RP", [](Fixture &f) { f.root.subpass = 99; });
        rejectCase("derivative flag", [](Fixture &f) {
            f.root.flags = VK_PIPELINE_CREATE_DERIVATIVE_BIT;
        });
        rejectCase("library flag", [](Fixture &f) {
            f.root.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
        });
        rejectCase("basePipelineHandle", [](Fixture &f) {
            f.root.basePipelineHandle = token<VkPipeline>(77);
        });
        rejectCase("vertex null bindings", [](Fixture &f) {
            f.vertex.pVertexBindingDescriptions = nullptr;
        });
        rejectCase("viewport static null arrays", [](Fixture &f) {
            f.root.pDynamicState = nullptr;
            f.viewport.pViewports = nullptr;
        });
        rejectCase("blend null attachments", [](Fixture &f) {
            f.blend.pAttachments = nullptr;
        });
        rejectCase("unknown root pNext", [](Fixture &f) {
            f.root.pNext = &f.root;
        });
        rejectCase("Classic with extra Rendering node", [](Fixture &f) {
            f.root.pNext = &f.rendering;
        });
        rejectCase("Dynamic without Rendering node", [](Fixture &f) {
            f.root.renderPass = VK_NULL_HANDLE;
        });
        // Dynamic NULL ignored arrays is valid:
        {
            Fixture f;
            f.root.pNext = &f.rendering;
            f.root.renderPass = VK_NULL_HANDLE;
            check(f.build(other), "Dynamic NULL ignored arrays valid");
            f.viewport.pViewports = nullptr;
            f.viewport.pScissors = nullptr;
            VulkanGraphicsPipelineKey nullArrays;
            check(f.build(nullArrays) && nullArrays == other, "actually NULL ignored arrays preserve complete tuple");
            f.rectangle.x = 42;
            VulkanGraphicsPipelineKey k;
            check(f.build(k) && k == other, "Dynamic viewport array mutation ignored");
        }
        // Dynamic root pNext cycle
        {
            Fixture f;
            VulkanGraphicsPipelineKey expected, out;
            check(f.build(expected) && f.build(out), "chain failure primes actual output");
            f.root.pNext = &f.rendering;
            f.rendering.pNext = &f.rendering; // cycle
            f.root.renderPass = VK_NULL_HANDLE;
            check(!f.build(out), "Dynamic pNext cycle rejected");
            verifyReset(out);
            f.rendering.pNext = nullptr;
            f.root.pNext = nullptr;
            f.root.renderPass = token<VkRenderPass>(104);
            check(f.build(out) && out == expected, "chain failure restores entire tuple");
        }
        // duplicate Rendering nodes
        {
            Fixture f;
            VulkanGraphicsPipelineKey expected, out;
            check(f.build(expected) && f.build(out), "chain failure primes actual output");
            VkPipelineRenderingCreateInfo r2{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
            f.rendering.pNext = &r2;
            f.root.pNext = &f.rendering;
            f.root.renderPass = VK_NULL_HANDLE;
            check(!f.build(out), "duplicate Rendering nodes rejected");
            verifyReset(out);
            f.rendering.pNext = nullptr;
            f.root.pNext = nullptr;
            f.root.renderPass = token<VkRenderPass>(104);
            check(f.build(out) && out == expected, "chain failure restores entire tuple");
        }
        // FSR + Rendering two-node chain accepted (Dynamic)
        {
            Fixture f;
            VkPipelineFragmentShadingRateStateCreateInfoKHR fsr{
                VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR};
            fsr.fragmentSize = {1, 1};
            f.rendering.pNext = &fsr;
            f.root.pNext = &f.rendering;
            f.root.renderPass = VK_NULL_HANDLE;
            check(f.build(other), "FSR+Rendering Dynamic accepted");
            f.rendering.pNext = nullptr;
            fsr.pNext = &f.rendering;
            f.root.pNext = &fsr;
            VulkanGraphicsPipelineKey reversed;
            check(f.build(reversed) && reversed == other && reversed.hash() == other.hash(), "both valid typed pNext orders are canonical");
            fsr.pNext = nullptr;
            f.root.pNext = &fsr;
            VulkanGraphicsPipelineKey out = other;
            check(!f.build(out), "Dynamic requires Rendering root node");
            verifyReset(out);
            f.root.pNext = nullptr;
        }
        // FSR zero extent rejected (Classic)
        {
            Fixture f;
            VulkanGraphicsPipelineKey expected, out;
            check(f.build(expected) && f.build(out), "chain failure primes actual output");
            VkPipelineFragmentShadingRateStateCreateInfoKHR fsr{
                VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR};
            fsr.fragmentSize = {0, 1};
            f.root.pNext = &fsr;
            check(!f.build(out), "FSR zero extent rejected");
            verifyReset(out);
            f.rendering.pNext = nullptr;
            f.root.pNext = nullptr;
            f.root.renderPass = token<VkRenderPass>(104);
            check(f.build(out) && out == expected, "chain failure restores entire tuple");
            f.root.pNext = nullptr;
        }
    }

    // ---- ownership: caller mutation/scope destroy; two owners; alias; map ----
    {
        Fixture f;
        VulkanGraphicsPipelineKey a;
        check(f.build(a), "own base");
        const auto saved = a;
        f.binding.stride = 64;
        f.attribute.offset = 8;
        f.assembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        f.attachment.colorWriteMask = 3;
        VulkanGraphicsPipelineKey b;
        check(f.build(b), "own rebuild with mutations");
        check(b != a, "own rebuild differs after family mutations");
        check(a == saved, "a intact after caller mutations");
        Fixture g;
        check(g.build(b) && b == saved, "two owner rebuild matches saved");
        check(!a.words().empty() && a.words().data() != b.words().data(),
              "two owners distinct word arrays");
        {
            Fixture tmp;
            check(tmp.build(a) && a == saved, "actual input scope rebuilds surviving owner");
        }
        check(a == saved, "a survives scope destroy");
        struct ZeroHash {
            size_t operator()(const VulkanGraphicsPipelineKey &k) const noexcept { return 0; }
        };
        Fixture h;
        h.assembly.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        VulkanGraphicsPipelineKey k3;
        check(h.build(k3), "third key build");
        std::unordered_map<VulkanGraphicsPipelineKey, int, ZeroHash> map;
        map[a] = 1;
        map[k3] = 2;
        check(map.size() == 2, "map size 2 (constant hash)");
        check(map.find(a) != map.end() && map.find(a)->second == 1, "map find a");
        check(map.find(k3) != map.end() && map.find(k3)->second == 2, "map find k3");
        Fixture h2;
        h2.blend.blendConstants[2] = 0.75f;
        VulkanGraphicsPipelineKey k4;
        check(h2.build(k4), "unstored key build");
        check(map.find(k4) == map.end(), "unstored key not found");
    }

    // ---- Dynamic true alias (child getters as live inputs) ----
    {
        Fixture f;
        VulkanGraphicsPipelineKey dyn;
        f.root.pNext = &f.rendering;
        f.root.renderPass = VK_NULL_HANDLE;
        check(f.build(dyn), "dyn alias base build");
        check(dyn.stages().size() == 2, "dyn alias stages present");
        const auto expected = dyn;
        if (dyn.stages().size() == 2) {
            const VulkanShaderStageKey *aliases[2] = {&dyn.stages()[0], &dyn.stages()[1]};
            VkPipelineShaderStageCreateInfo infos[]{f.stages[0], f.stages[1]};
            infos[0].pName = dyn.stages()[0].entryPoint().c_str();
            infos[1].pName = dyn.stages()[1].entryPoint().c_str();
            f.root.pStages = infos;
            f.rendering.pColorAttachmentFormats = dyn.renderingSignature().colorFormats.data();
            check(VulkanGraphicsPipelineKey::build(f.root, aliases, 2, dyn.layout(), nullptr, dyn) && dyn == expected && dyn.hash() == expected.hash(),
                  "true Dynamic aliases preserve every child, array and state word");
        }
    }

    // ---- Task C: real allocation points ----
    {
        auto measureAndFailNth = [&](bool dynamic, const char *label) {
            Fixture f;
            if (dynamic) {
                f.root.pNext = &f.rendering;
                f.root.renderPass = VK_NULL_HANDLE;
            }
            const VulkanClassicRenderPassKey *passPtr = dynamic ? nullptr : &f.pass;
            VulkanGraphicsPipelineKey expected;
            check(VulkanGraphicsPipelineKey::build(f.root, f.keys, 2, f.layout, passPtr, expected),
                  label);
            // measure total allocations in a fresh build (counting outside failure)
            g_gpkAllocCount = 0;
            g_gpkFailing = false;
            g_gpkSkipAllocs = 0;
            VulkanGraphicsPipelineKey probe;
            check(VulkanGraphicsPipelineKey::build(f.root, f.keys, 2, f.layout, passPtr, probe),
                  "alloc measure build");
            const long measured = g_gpkAllocCount;
            std::printf("gpk %s fresh allocation count: %ld\n", label, measured);
            check(measured > 0, "allocation count must be > 0");
            // failNth: every point must inject exactly once, return false, reset, recover
            for (long nth = 1; nth <= measured; ++nth) {
                VulkanGraphicsPipelineKey k;
                check(VulkanGraphicsPipelineKey::build(f.root, f.keys, 2, f.layout, passPtr, k),
                      "failNth prime");
                g_gpkInjections = 0;
                g_gpkFailing = true;
                g_gpkSkipAllocs = nth - 1;
                bool ok = VulkanGraphicsPipelineKey::build(f.root, f.keys, 2, f.layout, passPtr, k);
                g_gpkFailing = false;
                g_gpkSkipAllocs = 0;
                char tag[64];
                std::snprintf(tag, sizeof(tag), "%s failNth%ld", label, nth);
                check(!ok, tag);
                std::string injLabel = std::string(tag) + ": exactly one injection";
                check(g_gpkInjections == 1, injLabel.c_str());
                std::string resetLabel = std::string(tag) + ": full reset";
                check(!k.valid() && k.words().empty() && k.stages().empty(), resetLabel.c_str());
                std::string defLabel = std::string(tag) + ": default key";
                check(k == VulkanGraphicsPipelineKey{}, defLabel.c_str());
                std::string recLabel = std::string(tag) + ": recover tuple";
                check(VulkanGraphicsPipelineKey::build(f.root, f.keys, 2, f.layout, passPtr, k) &&
                          k == expected,
                      recLabel.c_str());
            }
        };
        measureAndFailNth(false, "Classic");
        measureAndFailNth(true, "Dynamic");
    }

    std::printf("graphics pipeline key tests: failures=%u\n", failures);
    return failures ? 1 : 0;
}
#endif
