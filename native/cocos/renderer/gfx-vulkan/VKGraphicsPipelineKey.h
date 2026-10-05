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

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "VKClassicRenderPassKey.h"
#include "VKGraphicsRasterKey.h"
#include "VKLayoutKeys.h"
#include "VKRenderingContract.h"
#include "VKShaderStageKey.h"

namespace cc {
namespace gfx {

// Owned content for the existing complete graphics encoder. This is a narrow
// shape model, not a Vulkan validity/device-support validator. Extra ordered or
// ignored scalar fields can cause conservative misses. Unknown extensions and
// derivative/library pipelines are unsupported rather than silently omitted.
// The caller supplies shader/layout/RP keys from the objects behind the handles.
// No handles, pointers or object IDs are stored or hashed.
class VulkanGraphicsPipelineKey final {
public:
    static constexpr uint32_t MAX_ARRAY = 256;
    static constexpr uint32_t MAX_DYNAMIC_STATES = 64;

    static bool build(const VkGraphicsPipelineCreateInfo &info,
                      const VulkanShaderStageKey *const *stageKeys, size_t stageKeyCount,
                      const VulkanPipelineLayoutKey &layout,
                      const VulkanClassicRenderPassKey *classicPass,
                      VulkanGraphicsPipelineKey &out) noexcept {
        const auto reject = [&out]() noexcept { out = {}; return false; };
        if (info.sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO ||
            !info.stageCount || info.stageCount > 5 || stageKeyCount != info.stageCount ||
            !info.pStages || !stageKeys || !info.layout || !layout.valid() ||
            info.basePipelineHandle || (info.flags & (VK_PIPELINE_CREATE_DERIVATIVE_BIT | VK_PIPELINE_CREATE_LIBRARY_BIT_KHR)) ||
            !shape(info.pVertexInputState, VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO) ||
            !shape(info.pInputAssemblyState, VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO) ||
            !shape(info.pViewportState, VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO) ||
            !info.pRasterizationState || !info.pMultisampleState) return reject();
        const bool dynamic = info.renderPass == VK_NULL_HANDLE;
        if (!dynamic && (!classicPass || !classicPass->valid() || classicPass->words().empty() ||
                         info.subpass >= classicPass->subpassCount())) return reject();
        if (dynamic && info.subpass != 0) return reject();
        try {
            // All inputs, including out's child getters, stay alive until publish.
            VulkanGraphicsPipelineKey snapshot;
            snapshot.layout_ = layout;
            snapshot.dynamic_ = dynamic;
            if (!dynamic) snapshot.classicPass_ = *classicPass;
            if (!VulkanGraphicsRasterKey::build(*info.pRasterizationState, *info.pMultisampleState,
                                                info.pDepthStencilState, snapshot.raster_)) return reject();
            uint32_t seenStages = 0;
            snapshot.stages_.reserve(info.stageCount);
            for (uint32_t i = 0; i < info.stageCount; ++i) {
                const auto &stage = info.pStages[i];
                const auto *key = stageKeys[i];
                if (!key || !key->valid() || key->entryPoint().empty() || key->spirvWords().size() < 5 ||
                    stage.sType != VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO ||
                    stage.pNext || stage.pSpecializationInfo || !stage.module || !stage.pName ||
                    stage.stage == VK_SHADER_STAGE_COMPUTE_BIT || (seenStages & stage.stage) ||
                    stage.flags != key->flags() || stage.stage != key->stage()) return reject();
                const auto &name = key->entryPoint();
                // Bounded comparison; no unbounded strlen on caller-owned input.
                for (size_t n = 0; n <= name.size(); ++n) {
                    const char expected = n == name.size() ? '\0' : name[n];
                    if (stage.pName[n] != expected) return reject();
                }
                seenStages |= stage.stage;
                snapshot.stages_.push_back(*key);
            }

            const VkPipelineRenderingCreateInfo *rendering = nullptr;
            const VkPipelineFragmentShadingRateStateCreateInfoKHR *fsr = nullptr;
            const auto *node = static_cast<const VkBaseInStructure *>(info.pNext);
            uint32_t nodes = 0;
            while (node) {
                if (++nodes > 2) return reject();
                if (node->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO && !rendering) {
                    rendering = reinterpret_cast<const VkPipelineRenderingCreateInfo *>(node);
                } else if (node->sType == VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR && !fsr) {
                    fsr = reinterpret_cast<const VkPipelineFragmentShadingRateStateCreateInfoKHR *>(node);
                } else {
                    return reject();
                }
                node = node->pNext;
            }
            if (dynamic != (rendering != nullptr)) return reject();
            if (dynamic) {
                if (rendering->colorAttachmentCount > MAX_ARRAY ||
                    (rendering->colorAttachmentCount && !rendering->pColorAttachmentFormats)) return reject();
                auto &sig = snapshot.signature_;
                if (rendering->colorAttachmentCount)
                    sig.colorFormats.assign(rendering->pColorAttachmentFormats,
                                            rendering->pColorAttachmentFormats + rendering->colorAttachmentCount);
                sig.depthFormat = rendering->depthAttachmentFormat;
                sig.stencilFormat = rendering->stencilAttachmentFormat;
                sig.viewMask = rendering->viewMask;
                sig.samples = info.pMultisampleState->rasterizationSamples;
                VKRenderingContract check;
                if (!check.build(sig)) return reject();
            }

            auto &w = snapshot.words_;
            w.insert(w.end(), {static_cast<uint32_t>(dynamic), info.flags, info.subpass,
                               static_cast<uint32_t>(info.basePipelineIndex)});
            const auto &vertex = *info.pVertexInputState;
            if (vertex.vertexBindingDescriptionCount > MAX_ARRAY ||
                (vertex.vertexBindingDescriptionCount && !vertex.pVertexBindingDescriptions) ||
                vertex.vertexAttributeDescriptionCount > MAX_ARRAY ||
                (vertex.vertexAttributeDescriptionCount && !vertex.pVertexAttributeDescriptions)) return reject();
            w.insert(w.end(), {vertex.flags, vertex.vertexBindingDescriptionCount});
            for (uint32_t i = 0; i < vertex.vertexBindingDescriptionCount; ++i) {
                const auto &b = vertex.pVertexBindingDescriptions[i];
                w.insert(w.end(), {b.binding, b.stride, static_cast<uint32_t>(b.inputRate)});
            }
            w.push_back(vertex.vertexAttributeDescriptionCount);
            for (uint32_t i = 0; i < vertex.vertexAttributeDescriptionCount; ++i) {
                const auto &a = vertex.pVertexAttributeDescriptions[i];
                w.insert(w.end(), {a.location, a.binding, static_cast<uint32_t>(a.format), a.offset});
            }
            const auto &assembly = *info.pInputAssemblyState;
            w.insert(w.end(), {assembly.flags, static_cast<uint32_t>(assembly.topology), assembly.primitiveRestartEnable});
            w.push_back(info.pTessellationState != nullptr);
            if (info.pTessellationState) {
                if (!shape(info.pTessellationState, VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO)) return reject();
                w.insert(w.end(), {info.pTessellationState->flags, info.pTessellationState->patchControlPoints});
            }
            bool viewportDynamic = false, scissorDynamic = false;
            w.push_back(info.pDynamicState != nullptr);
            if (info.pDynamicState) {
                const auto &d = *info.pDynamicState;
                if (!shape(&d, VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO) ||
                    d.dynamicStateCount > MAX_DYNAMIC_STATES || (d.dynamicStateCount && !d.pDynamicStates)) return reject();
                w.insert(w.end(), {d.flags, d.dynamicStateCount});
                for (uint32_t i = 0; i < d.dynamicStateCount; ++i) {
                    const auto state = d.pDynamicStates[i];
                    w.push_back(static_cast<uint32_t>(state));
                    viewportDynamic |= state == VK_DYNAMIC_STATE_VIEWPORT || state == VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT;
                    scissorDynamic |= state == VK_DYNAMIC_STATE_SCISSOR || state == VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT;
                }
            }
            const auto &vp = *info.pViewportState;
            if (vp.viewportCount > MAX_ARRAY || vp.scissorCount > MAX_ARRAY ||
                (!viewportDynamic && vp.viewportCount && !vp.pViewports) ||
                (!scissorDynamic && vp.scissorCount && !vp.pScissors)) return reject();
            w.insert(w.end(), {vp.flags, vp.viewportCount, vp.scissorCount, static_cast<uint32_t>(!viewportDynamic)});
            if (!viewportDynamic) {
                for (uint32_t i = 0; i < vp.viewportCount; ++i) {
                    const auto &v = vp.pViewports[i];
                    appendFloat(w, v.x);
                    appendFloat(w, v.y);
                    appendFloat(w, v.width);
                    appendFloat(w, v.height);
                    appendFloat(w, v.minDepth);
                    appendFloat(w, v.maxDepth);
                }
            }
            w.push_back(!scissorDynamic);
            if (!scissorDynamic) {
                for (uint32_t i = 0; i < vp.scissorCount; ++i) {
                    const auto &r = vp.pScissors[i];
                    w.insert(w.end(), {static_cast<uint32_t>(r.offset.x), static_cast<uint32_t>(r.offset.y), r.extent.width, r.extent.height});
                }
            }
            w.push_back(info.pColorBlendState != nullptr);
            if (info.pColorBlendState) {
                const auto &b = *info.pColorBlendState;
                if (!shape(&b, VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO) ||
                    b.attachmentCount > MAX_ARRAY || (b.attachmentCount && !b.pAttachments)) return reject();
                w.insert(w.end(), {b.flags, b.logicOpEnable, static_cast<uint32_t>(b.logicOp), b.attachmentCount});
                for (uint32_t i = 0; i < b.attachmentCount; ++i) {
                    const auto &a = b.pAttachments[i];
                    w.insert(w.end(), {a.blendEnable, static_cast<uint32_t>(a.srcColorBlendFactor), static_cast<uint32_t>(a.dstColorBlendFactor),
                                       static_cast<uint32_t>(a.colorBlendOp), static_cast<uint32_t>(a.srcAlphaBlendFactor), static_cast<uint32_t>(a.dstAlphaBlendFactor),
                                       static_cast<uint32_t>(a.alphaBlendOp), a.colorWriteMask});
                }
                for (float value : b.blendConstants) appendFloat(w, value);
            }
            w.push_back(fsr != nullptr);
            if (fsr) {
                if (!fsr->fragmentSize.width || !fsr->fragmentSize.height) return reject();
                w.insert(w.end(), {fsr->fragmentSize.width, fsr->fragmentSize.height,
                                   static_cast<uint32_t>(fsr->combinerOps[0]), static_cast<uint32_t>(fsr->combinerOps[1])});
            }
            snapshot.valid_ = true;
            out = std::move(snapshot);
            return true;
        } catch (...) {
            return reject();
        }
    }

    bool valid() const noexcept { return valid_; }
    bool dynamicRendering() const noexcept { return dynamic_; }
    const std::vector<VulkanShaderStageKey> &stages() const noexcept { return stages_; }
    const VulkanPipelineLayoutKey &layout() const noexcept { return layout_; }
    const VulkanGraphicsRasterKey &raster() const noexcept { return raster_; }
    const VulkanClassicRenderPassKey &classicPass() const noexcept { return classicPass_; }
    const VKRenderingSignature &renderingSignature() const noexcept { return signature_; }
    const std::vector<uint32_t> &words() const noexcept { return words_; }

    uint64_t hash() const noexcept {
        uint64_t h = UINT64_C(14695981039346656037);
        mix(h, 7);
        mix(h, 1);
        mix(h, valid_);
        mix(h, static_cast<uint32_t>(stages_.size()));
        for (const auto &stage : stages_) mix64(h, stage.hash());
        mix64(h, layout_.hash());
        mix64(h, raster_.hash());
        mix64(h, classicPass_.hash());
        mix64(h, signature_.hash());
        mix(h, static_cast<uint32_t>(words_.size()));
        for (uint32_t w : words_) mix(h, w);
        return h;
    }
    bool operator==(const VulkanGraphicsPipelineKey &other) const noexcept {
        return valid_ == other.valid_ && dynamic_ == other.dynamic_ && stages_ == other.stages_ &&
               layout_ == other.layout_ && raster_ == other.raster_ && classicPass_ == other.classicPass_ &&
               signature_ == other.signature_ && words_ == other.words_;
    }
    bool operator!=(const VulkanGraphicsPipelineKey &other) const noexcept { return !(*this == other); }

private:
    template <class T>
    static bool shape(const T *node, VkStructureType type) noexcept {
        return node && node->sType == type && node->pNext == nullptr;
    }
    static void appendFloat(std::vector<uint32_t> &w, float value) {
        static_assert(sizeof(float) == sizeof(uint32_t), "32-bit float required");
        uint32_t raw;
        std::memcpy(&raw, &value, sizeof(raw));
        w.push_back(raw);
    }
    static void mix(uint64_t &h, uint32_t w) noexcept {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            h ^= (w >> shift) & 255U;
            h *= UINT64_C(1099511628211);
        }
    }
    static void mix64(uint64_t &h, uint64_t value) noexcept {
        mix(h, static_cast<uint32_t>(value));
        mix(h, static_cast<uint32_t>(value >> 32));
    }
    std::vector<VulkanShaderStageKey> stages_;
    VulkanPipelineLayoutKey layout_;
    VulkanGraphicsRasterKey raster_;
    VulkanClassicRenderPassKey classicPass_;
    VKRenderingSignature signature_;
    std::vector<uint32_t> words_;
    bool dynamic_ = false;
    bool valid_ = false;
};

} // namespace gfx
} // namespace cc
