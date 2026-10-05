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

// Owning compute pipeline key value model (M7-D1). Pure CPU: combines an
// existing VulkanShaderStageKey (compute stage) and VulkanPipelineLayoutKey
// plus VkPipelineCreateFlags into a single content-keyed tuple. No Vulkan
// calls, no engine wiring, no dedup/prewarm. FNV-1a 64-bit hash with domain
// separation; full content equality is the final authority (hash is bucket).

#pragma once

#include <cstdint>
#include <utility>

#include "VKLayoutKeys.h"
#include "VKShaderStageKey.h"

namespace cc {
namespace gfx {

class VulkanComputePipelineKey final {
public:
    VulkanComputePipelineKey() = default;
    ~VulkanComputePipelineKey() = default;
    VulkanComputePipelineKey(const VulkanComputePipelineKey &) = default;
    VulkanComputePipelineKey &operator=(const VulkanComputePipelineKey &) = default;
    VulkanComputePipelineKey(VulkanComputePipelineKey &&) = default;
    VulkanComputePipelineKey &operator=(VulkanComputePipelineKey &&) = default;

    static bool build(VkPipelineCreateFlags flags,
                      const VulkanShaderStageKey &shader,
                      const VulkanPipelineLayoutKey &layout,
                      VulkanComputePipelineKey &out) noexcept {
        // Phase 1: validation (each failure clears out to default invalid)
        if (!shader.valid()) {
            out = VulkanComputePipelineKey{};
            return false;
        }
        if (shader.stage() != VK_SHADER_STAGE_COMPUTE_BIT) {
            out = VulkanComputePipelineKey{};
            return false;
        }
        if (!layout.valid()) {
            out = VulkanComputePipelineKey{};
            return false;
        }

        // Phase 2: local owning snapshots BEFORE any reset
        // (shader and layout may alias out.shader_/out.layout_)
        VulkanComputePipelineKey snapshot;
        try {
            snapshot.shader_ = shader; // owning deep copy (may throw)
            snapshot.layout_ = layout; // owning deep copy
        } catch (...) {
            out = VulkanComputePipelineKey{};
            return false;
        }

        // Phase 3: publish (out was NOT reset before snapshots were taken)
        out = std::move(snapshot);
        out.flags_ = flags;
        out.valid_ = true;
        return true;
    }

    bool valid() const noexcept { return valid_; }
    VkPipelineCreateFlags flags() const noexcept { return flags_; }
    const VulkanShaderStageKey &shader() const noexcept { return shader_; }
    const VulkanPipelineLayoutKey &layout() const noexcept { return layout_; }

    uint64_t hash() const noexcept {
        // schema: [domain=4, schema=1, valid, flags, shaderHash, layoutHash]
        uint64_t seed = shader_key_detail::FNV_BASIS;
        shader_key_detail::hashU32(seed, 4); // domain ComputePipeline
        shader_key_detail::hashU32(seed, 1); // schema
        shader_key_detail::hashU32(seed, static_cast<uint32_t>(valid_));
        shader_key_detail::hashU32(seed, static_cast<uint32_t>(flags_));
        auto sh = shader_.hash();
        shader_key_detail::hashU32(seed, static_cast<uint32_t>(sh & 0xFFFFFFFFU));
        shader_key_detail::hashU32(seed, static_cast<uint32_t>(sh >> 32));
        auto lh = layout_.hash();
        shader_key_detail::hashU32(seed, static_cast<uint32_t>(lh & 0xFFFFFFFFU));
        shader_key_detail::hashU32(seed, static_cast<uint32_t>(lh >> 32));
        return seed;
    }

    bool operator==(const VulkanComputePipelineKey &o) const noexcept {
        return valid_ == o.valid_ && flags_ == o.flags_ &&
               shader_ == o.shader_ && layout_ == o.layout_;
    }
    bool operator!=(const VulkanComputePipelineKey &o) const noexcept { return !(*this == o); }

private:
    VkPipelineCreateFlags flags_ = 0;
    VulkanShaderStageKey shader_;
    VulkanPipelineLayoutKey layout_;
    bool valid_ = false;
};

} // namespace gfx
} // namespace cc
