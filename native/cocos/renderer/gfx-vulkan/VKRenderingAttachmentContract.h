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

// Single-attachment Load/Store/Resolve offline mapping (M5-E).
// Maps one VkAttachmentDescription + caller binding to one
// VkRenderingAttachmentInfo for the future Dynamic Rendering adapter.
// Pure CPU value model: no Vulkan calls, no allocations, no runtime wiring.
// VkImageView values are borrowed handles only; this class does not own or
// extend the lifetime of any real Vulkan resource.

#pragma once

#include <vulkan/vulkan_core.h>

namespace cc {
namespace gfx {

class VKRenderingAttachmentContract final {
public:
    enum class Role { Color,
                      Depth,
                      Stencil };

    // Caller-side binding values. imageView is a borrowed handle value.
    struct Binding {
        VkImageView imageView = VK_NULL_HANDLE;
        VkImageLayout imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResolveModeFlagBits resolveMode = VK_RESOLVE_MODE_NONE;
        VkImageView resolveImageView = VK_NULL_HANDLE;
        VkImageLayout resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    VKRenderingAttachmentContract() { reset_(); }
    ~VKRenderingAttachmentContract() = default;
    VKRenderingAttachmentContract(const VKRenderingAttachmentContract &) = delete;
    VKRenderingAttachmentContract &operator=(const VKRenderingAttachmentContract &) = delete;
    VKRenderingAttachmentContract(VKRenderingAttachmentContract &&) = delete;
    VKRenderingAttachmentContract &operator=(VKRenderingAttachmentContract &&) = delete;

    // Build from a snapshot of all inputs (self-input safe: any input may
    // alias this contract's own published clearValue).
    //  - Color/Depth map description.loadOp/storeOp; Stencil maps
    //    description.stencilLoadOp/stencilStoreOp. Only the selected
    //    role's ops are validated (narrow input contract, NOT the full set
    //    of Vulkan-legal enums).
    //  - Active attachment (imageView != NULL): imageLayout is copied
    //    as-is (it is the in-render layout, not initial/final); resolve
    //    NONE normalizes resolve view/layout; non-NONE requires a non-NULL
    //    resolveImageView and a valid single sample-count bit > 1 taken
    //    from the description input (not proof of real GPU samples).
    //  - Unused attachment (imageView == NULL): non-NONE resolve is
    //    rejected (requested resolve would be silently lost); NONE
    //    publishes a canonical empty attachment and ignores the
    //    description ops entirely.
    // Returns false and fully resets on any rejection.
    bool build(Role role, const VkAttachmentDescription &description,
               const Binding &binding, const VkClearValue &clearValue) {
        // validate role and selected ops first (no state touched)
        bool useStencil = false;
        switch (role) {
            case Role::Color:
            case Role::Depth: break;
            case Role::Stencil: useStencil = true; break;
            default: reset_(); return false;
        }
        // selected ops are validated only for active attachments: the unused
        // path never reads the description ops (ignored inputs stay ignored)
        const bool active = binding.imageView != VK_NULL_HANDLE;
        if (!active) {
            const bool unused = binding.resolveMode == VK_RESOLVE_MODE_NONE;
            reset_();
            ready_ = unused;
            return unused;
        }
        const VkAttachmentLoadOp inLoad = useStencil ? description.stencilLoadOp : description.loadOp;
        const VkAttachmentStoreOp inStore = useStencil ? description.stencilStoreOp : description.storeOp;
        if (!isValidLoadOp_(inLoad) || !isValidStoreOp_(inStore)) {
            reset_();
            return false;
        }
        if (!isValidResolveMode_(binding.resolveMode)) {
            reset_();
            return false;
        }

        // snapshot all scalar inputs BEFORE clearing own state (self-input safe)
        const VkImageView view = binding.imageView;
        const VkImageLayout layout = binding.imageLayout;
        const VkResolveModeFlagBits mode = binding.resolveMode;
        const VkImageView rView = binding.resolveImageView;
        const VkImageLayout rLayout = binding.resolveImageLayout;
        const VkClearValue clear = clearValue;
        const VkSampleCountFlags samples = description.samples;

        reset_();

        if (mode != VK_RESOLVE_MODE_NONE) {
            if (rView == VK_NULL_HANDLE || !isResolveSampleCount_(samples)) {
                return false;
            }
            info_.resolveMode = mode;
            info_.resolveImageView = rView;
            info_.resolveImageLayout = rLayout;
        }
        info_.imageView = view;
        info_.imageLayout = layout;
        info_.clearValue = clear;
        info_.loadOp = inLoad;
        info_.storeOp = inStore;
        ready_ = true;
        return true;
    }

    void reset() { reset_(); }
    bool ready() const { return ready_; }
    const VkRenderingAttachmentInfo &info() const { return info_; }

private:
    static bool isValidLoadOp_(VkAttachmentLoadOp op) {
        switch (op) {
            case VK_ATTACHMENT_LOAD_OP_LOAD:
            case VK_ATTACHMENT_LOAD_OP_CLEAR:
            case VK_ATTACHMENT_LOAD_OP_DONT_CARE: return true;
            default: return false;
        }
    }
    static bool isValidStoreOp_(VkAttachmentStoreOp op) {
        switch (op) {
            case VK_ATTACHMENT_STORE_OP_STORE:
            case VK_ATTACHMENT_STORE_OP_DONT_CARE: return true;
            default: return false;
        }
    }
    static bool isValidResolveMode_(VkResolveModeFlagBits mode) {
        // exactly one known bit, or NONE; combos and unknown bits rejected
        switch (mode) {
            case VK_RESOLVE_MODE_NONE:
            case VK_RESOLVE_MODE_SAMPLE_ZERO_BIT:
            case VK_RESOLVE_MODE_AVERAGE_BIT:
            case VK_RESOLVE_MODE_MIN_BIT:
            case VK_RESOLVE_MODE_MAX_BIT: return true;
            default: return false;
        }
    }
    static bool isResolveSampleCount_(VkSampleCountFlags samples) {
        // single known bit > 1 (description input only, not GPU proof)
        switch (samples) {
            case VK_SAMPLE_COUNT_2_BIT:
            case VK_SAMPLE_COUNT_4_BIT:
            case VK_SAMPLE_COUNT_8_BIT:
            case VK_SAMPLE_COUNT_16_BIT:
            case VK_SAMPLE_COUNT_32_BIT:
            case VK_SAMPLE_COUNT_64_BIT: return true;
            default: return false;
        }
    }

    void reset_() {
        ready_ = false;
        info_ = {};
        info_.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        info_.pNext = nullptr;
        info_.imageView = VK_NULL_HANDLE;
        info_.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        info_.resolveMode = VK_RESOLVE_MODE_NONE;
        info_.resolveImageView = VK_NULL_HANDLE;
        info_.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        info_.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        info_.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }

    bool ready_ = false;
    VkRenderingAttachmentInfo info_{};
};

} // namespace gfx
} // namespace cc
