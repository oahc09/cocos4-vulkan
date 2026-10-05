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

// Graphics pipeline rendering entry adapter (M5-H).
// Applies the typed pNext composer to a VkGraphicsPipelineCreateInfo root.
// Pure CPU: no Vulkan calls; root state pointers/handles stay owned by the
// caller. Narrow input checks only - NOT device support, Dynamic Rendering
// eligibility, or a full VUID set.

#pragma once

#include <vulkan/vulkan_core.h>

#include "VKPipelinePNextChain.h"

namespace cc {
namespace gfx {

enum class VKPipelineRenderingMode {
    Classic, // keep classic render pass + subpass; optional FSR only
    Dynamic, // attach rendering contract; renderPass NULL, subpass 0
};

// Apply the rendering contract to root's pNext/renderPass/subpass.
//  Classic: requires a non-null classicRP. The composer attaches only the
//    optional FSR (no Rendering node); classicRP/subpass are written back.
//    An invalid or null signature never rejects Classic mode.
//  Dynamic: requires a non-null signature the composer accepts, and
//    root.pMultisampleState non-null with the correct sType whose
//    rasterizationSamples equals signature->samples (no rewriting of the
//    multisample state). The composer attaches Rendering + optional FSR;
//    root.renderPass becomes NULL and subpass 0.
// root must have the correct sType and a NULL pNext (unknown pre-existing
// chains are refused, not overwritten). Only pNext/renderPass/subpass are
// written, and only after full success; on any failure the owned chain is
// reset and every root field is unchanged. The chain owner must stay alive
// until vkCreateGraphicsPipelines returns; root.pNext must not be kept
// afterwards.
inline bool applyVKPipelineRenderingContract(
    VkGraphicsPipelineCreateInfo &root, VKPipelinePNextChain &ownedChain,
    VKPipelineRenderingMode mode, VkRenderPass classicRP, uint32_t classicSubpass,
    const VKRenderingSignature *signature,
    const VkPipelineFragmentShadingRateStateCreateInfoKHR *fsr) {
    if (root.sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO) {
        ownedChain.reset();
        return false;
    }
    if (root.pNext != nullptr) { // never overwrite an unknown chain
        ownedChain.reset();
        return false;
    }
    switch (mode) {
        case VKPipelineRenderingMode::Classic:
            if (classicRP == VK_NULL_HANDLE) {
                ownedChain.reset();
                return false;
            }
            break;
        case VKPipelineRenderingMode::Dynamic:
            if (signature == nullptr) {
                ownedChain.reset();
                return false;
            }
            if (root.pMultisampleState == nullptr ||
                root.pMultisampleState->sType !=
                    VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO ||
                root.pMultisampleState->rasterizationSamples != signature->samples) {
                ownedChain.reset();
                return false;
            }
            break;
        default:
            ownedChain.reset();
            return false;
    }

    // nothing written yet: on failure root keeps every field; the composer
    // itself fully clears on rejection and snapshots inputs before reset
    // (self-referencing signature/fsr inputs are read before clearing)
    const bool ok = mode == VKPipelineRenderingMode::Classic
                        ? ownedChain.build(nullptr, fsr)
                        : ownedChain.build(signature, fsr);
    if (!ok) {
        ownedChain.reset();
        return false;
    }

    root.pNext = ownedChain.head(); // nullptr for classic-without-FSR
    if (mode == VKPipelineRenderingMode::Classic) {
        root.renderPass = classicRP;
        root.subpass = classicSubpass;
    } else {
        root.renderPass = VK_NULL_HANDLE;
        root.subpass = 0;
    }
    return true;
}

} // namespace gfx
} // namespace cc
