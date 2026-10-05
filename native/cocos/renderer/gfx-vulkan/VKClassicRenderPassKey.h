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

#include <cstdint>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace cc {
namespace gfx {

// Conservative compatibility key for the modeled RenderPass2 structures.
// Equal valid keys imply compatible valid Vulkan inputs; unequal keys do not
// imply incompatibility. Layouts and load/store operations are omitted, but
// attachment numbering, array lengths, dependencies and resolve modes remain.
// This deliberately does not canonicalize every spec equivalence (including
// single-subpass resolve exceptions). No pointers, handles or padding are kept.
// Input validity/device support still require Vulkan validation separately.
class VulkanClassicRenderPassKey final {
public:
    static constexpr uint32_t MAX_ATTACHMENTS = 256;
    static constexpr uint32_t MAX_SUBPASSES = 256;
    static constexpr uint32_t MAX_DEPENDENCIES = 4096;

    static bool build(const VkRenderPassCreateInfo2 &info, VulkanClassicRenderPassKey &out) noexcept {
        const auto reject = [&out]() noexcept { out = {}; return false; };
        if (info.sType != VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2 || info.pNext ||
            info.attachmentCount > MAX_ATTACHMENTS || (info.attachmentCount && !info.pAttachments) ||
            !info.subpassCount || info.subpassCount > MAX_SUBPASSES || !info.pSubpasses ||
            info.dependencyCount > MAX_DEPENDENCIES || (info.dependencyCount && !info.pDependencies) ||
            info.correlatedViewMaskCount > MAX_SUBPASSES || (info.correlatedViewMaskCount && !info.pCorrelatedViewMasks)) return reject();
        try {
            VulkanClassicRenderPassKey snapshot;
            auto &w = snapshot.words_;
            w.push_back(info.flags);
            w.push_back(info.attachmentCount);
            for (uint32_t i = 0; i < info.attachmentCount; ++i) {
                const auto &a = info.pAttachments[i];
                const auto samples = static_cast<uint32_t>(a.samples);
                if (a.sType != VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2 || a.pNext ||
                    a.format == VK_FORMAT_UNDEFINED || !samples || samples > 64 || (samples & (samples - 1))) return reject();
                w.insert(w.end(), {a.flags, static_cast<uint32_t>(a.format), samples});
            }
            w.push_back(info.subpassCount);
            for (uint32_t i = 0; i < info.subpassCount; ++i) {
                const auto &s = info.pSubpasses[i];
                if (s.sType != VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2 || s.pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS ||
                    s.inputAttachmentCount > MAX_ATTACHMENTS || (s.inputAttachmentCount && !s.pInputAttachments) ||
                    s.colorAttachmentCount > MAX_ATTACHMENTS || (s.colorAttachmentCount && !s.pColorAttachments) ||
                    s.preserveAttachmentCount > MAX_ATTACHMENTS || (s.preserveAttachmentCount && !s.pPreserveAttachments)) return reject();
                w.insert(w.end(), {s.flags, static_cast<uint32_t>(s.pipelineBindPoint), s.viewMask, s.inputAttachmentCount});
                for (uint32_t j = 0; j < s.inputAttachmentCount; ++j)
                    if (!appendReference(w, info.attachmentCount, &s.pInputAttachments[j])) return reject();
                w.push_back(s.colorAttachmentCount);
                for (uint32_t j = 0; j < s.colorAttachmentCount; ++j)
                    if (!appendReference(w, info.attachmentCount, &s.pColorAttachments[j])) return reject();
                w.push_back(s.pResolveAttachments != nullptr);
                if (s.pResolveAttachments)
                    for (uint32_t j = 0; j < s.colorAttachmentCount; ++j)
                        if (!appendReference(w, info.attachmentCount, &s.pResolveAttachments[j])) return reject();
                if (!appendReference(w, info.attachmentCount, s.pDepthStencilAttachment)) return reject();
                w.push_back(s.preserveAttachmentCount);
                for (uint32_t j = 0; j < s.preserveAttachmentCount; ++j) {
                    if (s.pPreserveAttachments[j] >= info.attachmentCount) return reject();
                    w.push_back(s.pPreserveAttachments[j]);
                }
                const VkSubpassDescriptionDepthStencilResolve *ds = nullptr;
                const VkFragmentShadingRateAttachmentInfoKHR *fsr = nullptr;
                const auto *node = static_cast<const VkBaseInStructure *>(s.pNext);
                unsigned nodes = 0;
                while (node) {
                    if (++nodes > 2) return reject(); // cycles/duplicates/unknown tails cannot be published
                    if (node->sType == VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE && !ds)
                        ds = reinterpret_cast<const VkSubpassDescriptionDepthStencilResolve *>(node);
                    else if (node->sType == VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR && !fsr)
                        fsr = reinterpret_cast<const VkFragmentShadingRateAttachmentInfoKHR *>(node);
                    else
                        return reject();
                    node = node->pNext;
                }
                w.push_back(ds != nullptr);
                if (ds) {
                    w.insert(w.end(), {static_cast<uint32_t>(ds->depthResolveMode), static_cast<uint32_t>(ds->stencilResolveMode)});
                    if (!appendReference(w, info.attachmentCount, ds->pDepthStencilResolveAttachment)) return reject();
                }
                w.push_back(fsr != nullptr);
                if (fsr) {
                    if (!appendReference(w, info.attachmentCount, fsr->pFragmentShadingRateAttachment)) return reject();
                    w.insert(w.end(), {fsr->shadingRateAttachmentTexelSize.width, fsr->shadingRateAttachmentTexelSize.height});
                }
            }
            w.push_back(info.dependencyCount);
            for (uint32_t i = 0; i < info.dependencyCount; ++i) {
                const auto &d = info.pDependencies[i];
                if (d.sType != VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2 || d.pNext ||
                    (d.srcSubpass != VK_SUBPASS_EXTERNAL && d.srcSubpass >= info.subpassCount) ||
                    (d.dstSubpass != VK_SUBPASS_EXTERNAL && d.dstSubpass >= info.subpassCount)) return reject();
                w.insert(w.end(), {d.srcSubpass, d.dstSubpass, d.srcStageMask, d.dstStageMask,
                                   d.srcAccessMask, d.dstAccessMask, d.dependencyFlags, static_cast<uint32_t>(d.viewOffset)});
            }
            w.push_back(info.correlatedViewMaskCount);
            for (uint32_t i = 0; i < info.correlatedViewMaskCount; ++i) w.push_back(info.pCorrelatedViewMasks[i]);
            snapshot.subpassCount_ = info.subpassCount;
            snapshot.valid_ = true;
            out = std::move(snapshot);
            return true;
        } catch (...) {
            return reject();
        }
    }

    bool valid() const noexcept { return valid_; }
    uint32_t subpassCount() const noexcept { return subpassCount_; }
    const std::vector<uint32_t> &words() const noexcept { return words_; }
    uint64_t hash() const noexcept {
        uint64_t h = 14695981039346656037ULL;
        const auto mix = [&h](uint32_t v) {
            for (unsigned i = 0; i < 4; ++i) {
                h ^= (v >> (8 * i)) & 255U;
                h *= 1099511628211ULL;
            }
        };
        mix(5);
        mix(1);
        mix(valid_);
        mix(static_cast<uint32_t>(words_.size()));
        for (auto v : words_) mix(v);
        return h;
    }
    bool operator==(const VulkanClassicRenderPassKey &o) const noexcept {
        return valid_ == o.valid_ && subpassCount_ == o.subpassCount_ && words_ == o.words_;
    }
    bool operator!=(const VulkanClassicRenderPassKey &o) const noexcept { return !(*this == o); }

private:
    static bool appendReference(std::vector<uint32_t> &words, uint32_t count, const VkAttachmentReference2 *r) {
        if (r && (r->sType != VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2 || r->pNext)) return false;
        if (!r || r->attachment == VK_ATTACHMENT_UNUSED) {
            words.insert(words.end(), {VK_ATTACHMENT_UNUSED, 0});
            return true;
        }
        if (r->attachment >= count) return false;
        words.insert(words.end(), {r->attachment, r->aspectMask});
        return true;
    }
    std::vector<uint32_t> words_;
    uint32_t subpassCount_ = 0;
    bool valid_ = false;
};

} // namespace gfx
} // namespace cc
