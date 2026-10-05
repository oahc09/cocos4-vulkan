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

// Composite VkRenderingInfo owning attachment contract (M5-J).
// Integrates multiple color attachments plus optional depth/stencil into a
// stable VkRenderingInfo for the future M6 wiring. Pure CPU owning model:
// no Vulkan calls; VkImageView handles are borrowed values (no GPU lifetime
// extension, no layout proof, no transitions - those stay with
// VKRenderingTransitionContract). Narrow input policy only - NOT a full
// Vulkan legality check (VUID 06069/06085/06086 mirrored narrowly).

#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "VKRenderingAttachmentContract.h"
#include "VKRenderingContract.h"

namespace cc {
namespace gfx {

class VKRenderingInfoContract final {
public:
    struct AttachmentInput {
        VkAttachmentDescription description{};
        VKRenderingAttachmentContract::Binding binding{};
        VkClearValue clear{};
    };
    struct Input {
        VKRenderingSignature signature;
        std::vector<AttachmentInput> colors;
        std::optional<AttachmentInput> depth;
        std::optional<AttachmentInput> stencil;
        VkRect2D renderArea{};
        uint32_t layerCount = 1;
        VkRenderingFlags flags = 0;
    };

    VKRenderingInfoContract() { reset_(); }
    ~VKRenderingInfoContract() = default;
    VKRenderingInfoContract(const VKRenderingInfoContract &) = delete;
    VKRenderingInfoContract &operator=(const VKRenderingInfoContract &) = delete;
    VKRenderingInfoContract(VKRenderingInfoContract &&) = delete;
    VKRenderingInfoContract &operator=(VKRenderingInfoContract &&) = delete;

    // Narrow input policy (first batch; NOT the full set of Vulkan-legal
    // configurations):
    //  - signature is validated by the owned VKRenderingContract (samples
    //    single bit, D/S pair compatibility); viewMask is taken from it.
    //  - colors.size() must equal signature.colorFormats.size(); strict slot
    //    order, UNUSED slots preserved. An active slot (imageView non-null)
    //    requires the signature format non-UNDEFINED and description
    //    format/samples equal to the signature; an inactive slot requires the
    //    signature format UNDEFINED (ignored description ops/samples are not
    //    read; non-NONE resolve on an unused slot is rejected by the single
    //    attachment model).
    //  - depth/stencil optionals must be present exactly when the matching
    //    signature format is non-UNDEFINED, then active with matching
    //    format/samples. Depth maps loadOp/storeOp, Stencil maps the stencil
    //    ops; clear values are kept per node; DONT_CARE store never drops a
    //    resolve.
    //  - when both D/S exist their imageView must match (VUID 06085) and
    //    when both resolves are non-NONE their resolveImageView must match
    //    (VUID 06086); the two nodes keep independent ops/modes.
    //  - flags allow only SECONDARY_CONTENTS/SUSPEND/RESUME this round;
    //    renderArea offset >= 0 and extent non-zero; viewMask==0 requires
    //    layerCount>0 (VUID 06069 mirror). Device limits/extent/features/
    //    resolve support are NOT checked here.
    // All inputs are snapshotted before clearing own state (build(c.input())
    // is safe). Allocation failures fully reset and rethrow; output pointers
    // publish only on full success.
    bool build(const Input &input) {
        try {
            // 1) full Input snapshot FIRST (self-alias safe: inputs may point
            //    into own state; any allocation failure here resets + rethrows)
            Input snap = input;

            reset_();

            // 2) retained contract from the SNAPSHOT signature: validates the
            //    signature (samples single bit, D-S pair) and stays published
            //    on success - reset_() above must not be the last thing that
            //    touched it
            if (!renderingContract_.build(snap.signature)) {
                reset_();
                return false;
            }

            // 3) colors count must match the signature slots exactly
            if (snap.colors.size() != snap.signature.colorFormats.size() ||
                snap.colors.size() > UINT32_MAX) {
                reset_();
                return false;
            }

            // 4) per-slot color checks (each slot its own branch)
            for (size_t i = 0; i < snap.colors.size(); ++i) {
                const auto &c = snap.colors[i];
                const bool active = c.binding.imageView != VK_NULL_HANDLE;
                const VkFormat sigFormat = snap.signature.colorFormats[i];
                if (active) {
                    if (sigFormat == VK_FORMAT_UNDEFINED) {
                        reset_();
                        return false;
                    }
                    if (c.description.format != sigFormat) {
                        reset_();
                        return false;
                    }
                    if (c.description.samples != snap.signature.samples) {
                        reset_();
                        return false;
                    }
                } else {
                    if (sigFormat != VK_FORMAT_UNDEFINED) {
                        reset_();
                        return false;
                    }
                }
            }

            // 5) depth/stencil optionals: present exactly when defined
            const auto checkDs = [](const std::optional<AttachmentInput> &opt, VkFormat sigFormat,
                                    VkSampleCountFlagBits sigSamples) -> bool {
                if (sigFormat == VK_FORMAT_UNDEFINED) return !opt.has_value();
                if (!opt.has_value()) return false;
                if (opt->binding.imageView == VK_NULL_HANDLE) return false;
                if (opt->description.format != sigFormat) return false;
                if (opt->description.samples != sigSamples) return false;
                return true;
            };
            if (!checkDs(snap.depth, snap.signature.depthFormat, snap.signature.samples) ||
                !checkDs(snap.stencil, snap.signature.stencilFormat, snap.signature.samples)) {
                reset_();
                return false;
            }

            // 6) same-view rules for the present D/S pair
            if (snap.depth.has_value() && snap.stencil.has_value()) {
                if (snap.depth->binding.imageView != snap.stencil->binding.imageView) {
                    reset_();
                    return false;
                }
                const bool dRes = snap.depth->binding.resolveMode != VK_RESOLVE_MODE_NONE;
                const bool sRes = snap.stencil->binding.resolveMode != VK_RESOLVE_MODE_NONE;
                if (dRes && sRes &&
                    snap.depth->binding.resolveImageView !=
                        snap.stencil->binding.resolveImageView) {
                    reset_();
                    return false;
                }
            }

            // 7) root field narrow checks
            if (snap.flags & ~(VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT |
                               VK_RENDERING_SUSPENDING_BIT | VK_RENDERING_RESUMING_BIT)) {
                reset_();
                return false;
            }
            if (snap.renderArea.offset.x < 0 || snap.renderArea.offset.y < 0 ||
                snap.renderArea.extent.width == 0 || snap.renderArea.extent.height == 0) {
                reset_();
                return false;
            }
            if (snap.signature.viewMask == 0 && snap.layerCount == 0) {
                reset_();
                return false;
            }

            // 8) assemble owned nodes via local single-attachment models
            colorInfos_.resize(snap.colors.size());
            for (size_t i = 0; i < snap.colors.size(); ++i) {
                VKRenderingAttachmentContract model;
                if (!model.build(VKRenderingAttachmentContract::Role::Color, snap.colors[i].description,
                                 snap.colors[i].binding, snap.colors[i].clear)) {
                    reset_();
                    return false;
                }
                colorInfos_[i] = model.info();
            }
            hasDepth_ = snap.depth.has_value();
            if (hasDepth_) {
                VKRenderingAttachmentContract model;
                if (!model.build(VKRenderingAttachmentContract::Role::Depth, snap.depth->description,
                                 snap.depth->binding, snap.depth->clear)) {
                    reset_();
                    return false;
                }
                depthInfo_ = model.info();
            }
            hasStencil_ = snap.stencil.has_value();
            if (hasStencil_) {
                VKRenderingAttachmentContract model;
                if (!model.build(VKRenderingAttachmentContract::Role::Stencil, snap.stencil->description,
                                 snap.stencil->binding, snap.stencil->clear)) {
                    reset_();
                    return false;
                }
                stencilInfo_ = model.info();
            }

            // 9) publish: root scalars from the snapshot, then move it in
            const VkRenderingFlags pubFlags = snap.flags;
            const VkRect2D pubArea = snap.renderArea;
            const uint32_t pubLayers = snap.layerCount;
            const uint32_t pubViewMask = snap.signature.viewMask;
            input_ = std::move(snap);

            root_.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            root_.pNext = nullptr;
            root_.flags = pubFlags;
            root_.renderArea = pubArea;
            root_.layerCount = pubLayers;
            root_.viewMask = pubViewMask;
            root_.colorAttachmentCount = static_cast<uint32_t>(colorInfos_.size());
            root_.pColorAttachments = colorInfos_.empty() ? nullptr : colorInfos_.data();
            root_.pDepthAttachment = hasDepth_ ? &depthInfo_ : nullptr;
            root_.pStencilAttachment = hasStencil_ ? &stencilInfo_ : nullptr;
            ready_ = true;
            return true;
        } catch (...) {
            reset_();
            throw; // allocation failures never leave stale state
        }
    }

    void reset() { reset_(); }
    bool ready() const { return ready_; }
    bool hasDepth() const { return hasDepth_; }
    bool hasStencil() const { return hasStencil_; }
    const Input &input() const { return input_; }
    const VkRenderingInfo &info() const { return root_; }
    const std::vector<VkRenderingAttachmentInfo> &colorInfos() const { return colorInfos_; }
    const VkRenderingAttachmentInfo &depthInfo() const { return depthInfo_; }
    const VkRenderingAttachmentInfo &stencilInfo() const { return stencilInfo_; }
    const VKRenderingContract &renderingContract() const { return renderingContract_; }

private:
    static constexpr VkRenderingAttachmentInfo emptyAttachment_() {
        VkRenderingAttachmentInfo a{};
        a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        a.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        a.resolveMode = VK_RESOLVE_MODE_NONE;
        a.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        return a;
    }

    void reset_() {
        ready_ = false;
        hasDepth_ = false;
        hasStencil_ = false;
        root_ = {};
        root_.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        root_.pNext = nullptr;
        root_.pColorAttachments = nullptr;
        root_.pDepthAttachment = nullptr;
        root_.pStencilAttachment = nullptr;
        colorInfos_.clear();
        depthInfo_ = emptyAttachment_();
        stencilInfo_ = emptyAttachment_();
        input_ = Input{}; // note: default Input has layerCount 1 (input-side
                          // default), while the root above is fully zeroed
        renderingContract_.reset();
    }

    bool ready_ = false;
    bool hasDepth_ = false;
    bool hasStencil_ = false;
    VkRenderingInfo root_{};
    std::vector<VkRenderingAttachmentInfo> colorInfos_;
    VkRenderingAttachmentInfo depthInfo_{};
    VkRenderingAttachmentInfo stencilInfo_{};
    Input input_{};
    VKRenderingContract renderingContract_{};
};

} // namespace gfx
} // namespace cc
