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

// Typed pipeline pNext chain composer (M5-D, revised).
// Owns a VKRenderingContract and an FSR state node; assembles a
// deterministic pNext chain (FSR -> Rendering -> NULL) with no borrowed
// pointers and no opaque tails. No Vulkan calls, no runtime wiring.

#pragma once

#include <cstdint>

#include <vulkan/vulkan_core.h>

#include "VKRenderingContract.h"

namespace cc {
namespace gfx {

class VKPipelinePNextChain final {
public:
    VKPipelinePNextChain() { reset_(); }
    ~VKPipelinePNextChain() = default;
    VKPipelinePNextChain(const VKPipelinePNextChain &) = delete;
    VKPipelinePNextChain &operator=(const VKPipelinePNextChain &) = delete;
    VKPipelinePNextChain(VKPipelinePNextChain &&) = delete;
    VKPipelinePNextChain &operator=(VKPipelinePNextChain &&) = delete;

    // Build the chain. nullptr means that node is not attached.
    //   dynamicSignature: if non-null, attach a rendering info node built
    //     from this signature (via the owned VKRenderingContract).
    //   shadingRate: if non-null, attach an FSR state node. The input must
    //     have the correct sType, a NULL pNext (no pre-existing chains --
    //     this includes the chain's own FSR node when it already has a
    //     non-NULL tail from a previous combined build), non-zero
    //     fragmentSize, and valid combinerOps. Values are copied.
    // Deterministic order when both are attached: FSR -> Rendering -> NULL.
    // Returns false on any validation failure; fully clears state.
    bool build(const VKRenderingSignature *dynamicSignature = nullptr,
               const VkPipelineFragmentShadingRateStateCreateInfoKHR *shadingRate = nullptr) {
        bool hasFsr = shadingRate != nullptr;
        if (hasFsr) {
            if (shadingRate->sType != VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR)
                { reset_(); return false; }
            if (shadingRate->pNext != nullptr) { reset_(); return false; }
            if (shadingRate->fragmentSize.width == 0 || shadingRate->fragmentSize.height == 0)
                { reset_(); return false; }
            if (!isValidCombiner_(shadingRate->combinerOps[0]) ||
                !isValidCombiner_(shadingRate->combinerOps[1]))
                { reset_(); return false; }
        }
        bool hasRendering = dynamicSignature != nullptr;
        VKRenderingSignature sigSnapshot;
        if (hasRendering) {
            try { sigSnapshot = *dynamicSignature; }
            catch (...) { reset_(); throw; }
        }
        VkPipelineFragmentShadingRateStateCreateInfoKHR fsrSnapshot = {};
        if (hasFsr) fsrSnapshot = *shadingRate;

        reset_();
        if (hasRendering) {
            if (!contract_.build(sigSnapshot)) { reset_(); return false; }
        }
        if (hasFsr) {
            fsr_ = fsrSnapshot;
            fsr_.pNext = nullptr;
        }
        if (hasFsr && hasRendering) {
            fsr_.pNext = &contract_.pipelineInfo();
            head_ = &fsr_;
        } else if (hasFsr) {
            fsr_.pNext = nullptr;
            head_ = &fsr_;
        } else if (hasRendering) {
            head_ = &contract_.pipelineInfo();
        } else {
            head_ = nullptr;
        }
        hasFsr_ = hasFsr;
        hasRendering_ = hasRendering;
        ready_ = true;
        return true;
    }

    void reset() { reset_(); }
    bool ready() const { return ready_; }
    bool hasFsr() const { return hasFsr_; }
    bool hasRendering() const { return hasRendering_; }

    // Head of the pNext chain (to be linked from
    // VkGraphicsPipelineCreateInfo::pNext). Pointer is valid only while the
    // owner is alive and before the next build/reset call.
    const void *head() const { return head_; }
    const VkPipelineFragmentShadingRateStateCreateInfoKHR &fsrInfo() const { return fsr_; }
    const VKRenderingContract &contract() const { return contract_; }

private:
    static bool isValidCombiner_(VkFragmentShadingRateCombinerOpKHR op) {
        switch (op) {
            case VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR:
            case VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR:
            case VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR:
            case VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MAX_KHR:
            case VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MUL_KHR:
                return true;
            default: return false;
        }
    }

    void reset_() {
        ready_ = false;
        hasFsr_ = false;
        hasRendering_ = false;
        head_ = nullptr;
        fsr_ = {};
        fsr_.sType = VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR;
        contract_.reset();
    }

    bool ready_ = false;
    bool hasFsr_ = false;
    bool hasRendering_ = false;
    const void *head_ = nullptr;
    VkPipelineFragmentShadingRateStateCreateInfoKHR fsr_{};
    VKRenderingContract contract_{};
};

} // namespace gfx
} // namespace cc
