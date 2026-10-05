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

// Classic RenderPass -> rendering signature tuple adapter (M5-G).
// Extracts a VKRenderingSignature per subpass from already-mapped Vulkan
// create-info values, purely on the CPU. valid only means the tuple was
// extracted; it is NOT proof of Dynamic Rendering eligibility, feature
// support, pipeline compatibility, or Vulkan creatability. No hash/cache
// key is changed by this adapter.

#pragma once

#include <vulkan/vulkan_core.h>

#include "VKRenderingSignature.h"

namespace cc {
namespace gfx {

// Plain copy/movable value struct: no self-referencing arrays, no pNext.
struct VKSubpassRenderingMetadata {
    VKRenderingSignature signature;
    bool valid = false;
};

// Build one subpass's rendering metadata from mapped Vulkan values.
// Input arrays are borrowed for the duration of the call only; no pointers
// into them are retained. Caller guarantees counts match real lengths.
// On any rejection out stays/becomes empty (valid=false); allocation
// exceptions propagate with out left empty - never a fake PASS.
inline bool buildVKSubpassRenderingMetadata(uint32_t attachmentCount,
                                            const VkAttachmentDescription2 *attachments,
                                            const VkSubpassDescription2 &subpass,
                                            VkSampleCountFlagBits samples,
                                            VKSubpassRenderingMetadata &out) {
    out = {};
    if (subpass.sType != VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2) return false;
    if (subpass.pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS) return false;
    // count>0 requires a real array; attachmentless (count 0, null) is legal
    // and any actual reference will fail the bounds check below
    if (attachmentCount > 0 && attachments == nullptr) return false;
    if (subpass.colorAttachmentCount > 0 && subpass.pColorAttachments == nullptr) return false;
    switch (samples) {
        case VK_SAMPLE_COUNT_1_BIT:
        case VK_SAMPLE_COUNT_2_BIT:
        case VK_SAMPLE_COUNT_4_BIT:
        case VK_SAMPLE_COUNT_8_BIT:
        case VK_SAMPLE_COUNT_16_BIT:
        case VK_SAMPLE_COUNT_32_BIT:
        case VK_SAMPLE_COUNT_64_BIT: break;
        default: return false;
    }

    VKRenderingSignature pending;
    // colors keep slot order; UNUSED keeps its slot as UNDEFINED
    if (subpass.colorAttachmentCount > 0) {
        pending.colorFormats.resize(subpass.colorAttachmentCount, VK_FORMAT_UNDEFINED);
        for (uint32_t i = 0; i < subpass.colorAttachmentCount; ++i) {
            const VkAttachmentReference2 &ref = subpass.pColorAttachments[i];
            if (ref.sType != VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2) {
                out = {};
                return false;
            }
            if (ref.attachment == VK_ATTACHMENT_UNUSED) continue; // slot stays UNDEFINED
            if (ref.attachment >= attachmentCount) {
                out = {};
                return false;
            }
            const VkAttachmentDescription2 &desc = attachments[ref.attachment];
            if (desc.sType != VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2) {
                out = {};
                return false;
            }
            if (desc.format == VK_FORMAT_UNDEFINED) {
                out = {};
                return false;
            }
            pending.colorFormats[i] = desc.format;
        }
    }

    // Depth/stencil classification from the referenced description format
    // (reference.aspectMask is only meaningful for input attachments and is
    // deliberately ignored here). Seven core D/S formats:
    //   depth-only:  D16_UNORM, X8_D24_UNORM_PACK32, D32_SFLOAT
    //   stencil-only: S8_UINT
    //   both:        D16_UNORM_S8_UINT, D24_UNORM_S8_UINT, D32_SFLOAT_S8_UINT
    if (subpass.pDepthStencilAttachment != nullptr) {
        const VkAttachmentReference2 &ref = *subpass.pDepthStencilAttachment;
        if (ref.sType != VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2) {
            out = {};
            return false;
        }
        if (ref.attachment != VK_ATTACHMENT_UNUSED) {
            if (ref.attachment >= attachmentCount) {
                out = {};
                return false;
            }
            const VkAttachmentDescription2 &desc = attachments[ref.attachment];
            if (desc.sType != VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2) {
                out = {};
                return false;
            }
            switch (desc.format) {
                case VK_FORMAT_D16_UNORM:
                case VK_FORMAT_X8_D24_UNORM_PACK32:
                case VK_FORMAT_D32_SFLOAT:
                    pending.depthFormat = desc.format;
                    pending.stencilFormat = VK_FORMAT_UNDEFINED;
                    break;
                case VK_FORMAT_S8_UINT:
                    pending.depthFormat = VK_FORMAT_UNDEFINED;
                    pending.stencilFormat = desc.format;
                    break;
                case VK_FORMAT_D16_UNORM_S8_UINT:
                case VK_FORMAT_D24_UNORM_S8_UINT:
                case VK_FORMAT_D32_SFLOAT_S8_UINT:
                    pending.depthFormat = desc.format;
                    pending.stencilFormat = desc.format;
                    break;
                case VK_FORMAT_UNDEFINED:
                default: // non-D/S formats are rejected for the D/S slot
                {
                    out = {};
                    return false;
                }
            }
        }
    }

    pending.samples = samples;
    pending.viewMask = subpass.viewMask;

    // input/resolve/preserve attachments and pNext chains do not affect this
    // tuple and are not traversed. This is NOT a full subpass validity check.
    out.signature = std::move(pending);
    out.valid = true;
    return true;
}

} // namespace gfx
} // namespace cc
