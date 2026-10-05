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

// VulkanGraphicsRasterKey: owning CPU snapshot key for the raster/multisample/
// optional depth-stencil portion of a graphics pipeline. Pure data; not wired
// to the engine runtime. Conservative: records all scalars as opaque raw
// values (VkBool32 kept as raw uint32, no normalization), never treats shape
// as GPU validity. No Vk pointers/handles/padding are stored.
//
// Fixed word stream order (schema 1, hash domain 6):
//   Raster (11 words): flags, depthClampEnable, rasterizerDiscardEnable,
//           polygonMode, cullMode, frontFace, depthBiasEnable,
//           depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor,
//           lineWidth
//   Multisample (7 + maskWordCount): flags, rasterizationSamples,
//           sampleShadingEnable, minSampleShading, maskWordCount,
//           mask[maskWordCount], alphaToCoverageEnable, alphaToOneEnable
//   DepthStencil (1 presence word; if present 1 + 22 more):
//           presence, flags, depthTestEnable, depthWriteEnable,
//           depthCompareOp, depthBoundsTestEnable, stencilTestEnable,
//           front (7), back (7), minDepthBounds, maxDepthBounds
//
// Floats are bit-copied to uint32_t (static_assert(sizeof(float) == 4)).
// Positive and negative zero stay distinct bit patterns.
// maskWordCount = ceil(rasterizationSamples / 32). A NULL pSampleMask is
// normalized to that many 0xffffffff words (equivalent to explicit all-ones).
// rasterizationSamples must be a power of two in [1, 64]; anything else is
// rejected. Unknown pNext on any input struct is rejected. Each sType is
// checked individually.
//
// Copy construction / copy assignment are ordinary value operations: an
// allocation failure propagates bad_alloc to the caller. static build() stays noexcept: any failure
// (including bad_alloc) fully resets out to default-invalid and returns
// false. These two allocation semantics are intentionally different.
//
// FNV-1a 64-bit hash over the byte stream of LE u32 words: each word
// contributes its 4 bytes, lowest byte first; each byte is xor-ed into the
// low 8 bits of the accumulator, then multiplied by the prime. Domain/schema/
// valid/wordCount are hashed the same way. Hash is an accelerator only;
// full word equality gates key equivalence.

#ifndef CC_GFX_VK_GRAPHICS_RASTER_KEY_H
#define CC_GFX_VK_GRAPHICS_RASTER_KEY_H

#include <vulkan/vulkan_core.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace cc {
namespace gfx {

class VulkanGraphicsRasterKey {
public:
    static constexpr uint64_t kFnvBasis = 14695981039346656037ULL;
    static constexpr uint64_t kFnvPrime = 1099511628211ULL;
    static constexpr uint32_t kHashDomain = 6;
    static constexpr uint32_t kHashSchema = 1;

    VulkanGraphicsRasterKey() = default;
    VulkanGraphicsRasterKey(const VulkanGraphicsRasterKey &) = default;
    VulkanGraphicsRasterKey &operator=(const VulkanGraphicsRasterKey &) = default;
    VulkanGraphicsRasterKey(VulkanGraphicsRasterKey &&) noexcept = default;
    VulkanGraphicsRasterKey &operator=(VulkanGraphicsRasterKey &&) noexcept = default;

    static bool build(const VkPipelineRasterizationStateCreateInfo &raster,
                      const VkPipelineMultisampleStateCreateInfo &ms,
                      const VkPipelineDepthStencilStateCreateInfo *optionalDS,
                      VulkanGraphicsRasterKey &out) noexcept {
        // Phase 1: shape validation (each failure clears out to default invalid).
        if (raster.sType != VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO ||
            raster.pNext != nullptr) {
            out = VulkanGraphicsRasterKey{};
            return false;
        }
        if (ms.sType != VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO ||
            ms.pNext != nullptr) {
            out = VulkanGraphicsRasterKey{};
            return false;
        }
        if (optionalDS != nullptr) {
            if (optionalDS->sType != VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO ||
                optionalDS->pNext != nullptr) {
                out = VulkanGraphicsRasterKey{};
                return false;
            }
        }

        // rasterizationSamples: single bit in [1, 64].
        const uint32_t samples = static_cast<uint32_t>(ms.rasterizationSamples);
        if (samples < 1u || samples > 64u || (samples & (samples - 1u)) != 0u) {
            out = VulkanGraphicsRasterKey{};
            return false;
        }

        const uint32_t maskWordCount = (samples + 31u) / 32u;
        const size_t totalWords =
            11u + 7u + static_cast<size_t>(maskWordCount) +
            (optionalDS != nullptr ? 23u : 1u);

        // Phase 2: local owning snapshot (may throw).
        VulkanGraphicsRasterKey snapshot;
        try {
            std::vector<uint32_t> &w = snapshot.words_;
            w.reserve(totalWords);

            // Raster: opaque raw values (VkBool32 saved as-is).
            push(w, raster.flags);
            push(w, static_cast<uint32_t>(raster.depthClampEnable));
            push(w, static_cast<uint32_t>(raster.rasterizerDiscardEnable));
            push(w, static_cast<uint32_t>(raster.polygonMode));
            push(w, static_cast<uint32_t>(raster.cullMode));
            push(w, static_cast<uint32_t>(raster.frontFace));
            push(w, static_cast<uint32_t>(raster.depthBiasEnable));
            pushFloat(w, raster.depthBiasConstantFactor);
            pushFloat(w, raster.depthBiasClamp);
            pushFloat(w, raster.depthBiasSlopeFactor);
            pushFloat(w, raster.lineWidth);

            // Multisample.
            push(w, ms.flags);
            push(w, samples);
            push(w, static_cast<uint32_t>(ms.sampleShadingEnable));
            pushFloat(w, ms.minSampleShading);
            push(w, maskWordCount);
            if (ms.pSampleMask != nullptr) {
                for (uint32_t i = 0; i < maskWordCount; ++i) push(w, ms.pSampleMask[i]);
            } else {
                for (uint32_t i = 0; i < maskWordCount; ++i) push(w, 0xffffffffu);
            }
            push(w, static_cast<uint32_t>(ms.alphaToCoverageEnable));
            push(w, static_cast<uint32_t>(ms.alphaToOneEnable));

            // DepthStencil.
            if (optionalDS != nullptr) {
                const VkPipelineDepthStencilStateCreateInfo &ds = *optionalDS;
                push(w, 1u);
                push(w, ds.flags);
                push(w, static_cast<uint32_t>(ds.depthTestEnable));
                push(w, static_cast<uint32_t>(ds.depthWriteEnable));
                push(w, static_cast<uint32_t>(ds.depthCompareOp));
                push(w, static_cast<uint32_t>(ds.depthBoundsTestEnable));
                push(w, static_cast<uint32_t>(ds.stencilTestEnable));
                pushStencilFace(w, ds.front);
                pushStencilFace(w, ds.back);
                pushFloat(w, ds.minDepthBounds);
                pushFloat(w, ds.maxDepthBounds);
            } else {
                push(w, 0u);
            }

            snapshot.valid_ = true;
        } catch (...) {
            out = VulkanGraphicsRasterKey{};
            return false;
        }

        // Phase 3: publish (noexcept move).
        out = std::move(snapshot);
        return true;
    }

    bool valid() const noexcept { return valid_; }
    const std::vector<uint32_t> &words() const noexcept { return words_; }

    uint64_t hash() const noexcept {
        // FNV-1a over the LE byte stream of the metadata words + content words.
        uint64_t h = kFnvBasis;
        mix(h, kHashDomain);
        mix(h, kHashSchema);
        mix(h, valid_ ? 1u : 0u);
        mix(h, static_cast<uint32_t>(words_.size()));
        for (size_t i = 0; i < words_.size(); ++i) mix(h, words_[i]);
        return h;
    }

    friend bool operator==(const VulkanGraphicsRasterKey &a,
                           const VulkanGraphicsRasterKey &b) noexcept {
        if (a.valid_ != b.valid_) return false;
        if (a.words_.size() != b.words_.size()) return false;
        for (size_t i = 0; i < a.words_.size(); ++i) {
            if (a.words_[i] != b.words_[i]) return false;
        }
        return true;
    }

    friend bool operator!=(const VulkanGraphicsRasterKey &a,
                           const VulkanGraphicsRasterKey &b) noexcept {
        return !(a == b);
    }

private:
    static void push(std::vector<uint32_t> &w, uint32_t v) {
        w.push_back(v);
    }

    static void pushFloat(std::vector<uint32_t> &w, float f) {
        static_assert(sizeof(float) == 4u, "expected 32-bit float");
        uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        w.push_back(bits);
    }

    static void pushStencilFace(std::vector<uint32_t> &w,
                                const VkStencilOpState &s) {
        push(w, static_cast<uint32_t>(s.failOp));
        push(w, static_cast<uint32_t>(s.passOp));
        push(w, static_cast<uint32_t>(s.depthFailOp));
        push(w, static_cast<uint32_t>(s.compareOp));
        push(w, s.compareMask);
        push(w, s.writeMask);
        push(w, s.reference);
    }

    // Mix one uint32 word into the FNV-1a state as 4 LE bytes.
    static void mix(uint64_t &h, uint32_t v) noexcept {
        h ^= (v & 0xffu);
        h *= kFnvPrime;
        h ^= ((v >> 8) & 0xffu);
        h *= kFnvPrime;
        h ^= ((v >> 16) & 0xffu);
        h *= kFnvPrime;
        h ^= ((v >> 24) & 0xffu);
        h *= kFnvPrime;
    }

    std::vector<uint32_t> words_;
    bool valid_ = false;
};

} // namespace gfx
} // namespace cc

#endif // CC_GFX_VK_GRAPHICS_RASTER_KEY_H
