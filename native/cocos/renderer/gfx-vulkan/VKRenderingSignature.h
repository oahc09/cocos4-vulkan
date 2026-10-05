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

// Pure-owning rendering signature value model (M5-B).
// No Vulkan objects, no loader, no engine dependencies. Deterministic
// equality and hash over logical fields only — this is NOT a Vulkan
// compatibility check or device-support assertion.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace cc {
namespace gfx {

struct VKRenderingSignature {
    std::vector<VkFormat> colorFormats;
    VkFormat depthFormat{VK_FORMAT_UNDEFINED};
    VkFormat stencilFormat{VK_FORMAT_UNDEFINED};
    VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
    uint32_t viewMask{0};

    bool operator==(const VKRenderingSignature &o) const {
        return colorFormats == o.colorFormats &&
               depthFormat == o.depthFormat &&
               stencilFormat == o.stencilFormat &&
               samples == o.samples &&
               viewMask == o.viewMask;
    }
    bool operator!=(const VKRenderingSignature &o) const { return !(*this == o); }

    // Deterministic field hash: mixes count, each color format in order,
    // depth, stencil, samples, viewMask. Each field is mixed as 8 bytes
    // little-endian. FNV-1a 64-bit per RFC 9923:
    //   offset basis = 0xcbf29ce484222325 (14695981039346656037)
    //   prime        = 0x100000001b3     (1099511628211)
    // No struct padding, no pointer addresses, no cached value.
    // Hash collision requires full equality check (operator==); hash alone
    // does not imply semantic equality.
    uint64_t hash() const noexcept {
        uint64_t h = 14695981039346656037ULL; // FNV-1a 64-bit offset basis
        auto mix = [&h](uint64_t v) noexcept {
            for (int i = 0; i < 8; ++i) {
                h ^= (v >> (i * 8)) & 0xFF;
                h *= 1099511628211ULL; // FNV-1a 64-bit prime
            }
        };
        mix(static_cast<uint64_t>(colorFormats.size()));
        for (VkFormat f : colorFormats) mix(static_cast<uint64_t>(f));
        mix(static_cast<uint64_t>(depthFormat));
        mix(static_cast<uint64_t>(stencilFormat));
        mix(static_cast<uint64_t>(samples));
        mix(static_cast<uint64_t>(viewMask));
        return h;
    }
};

// Explicit hasher functor for unordered containers (NOT a std::hash
// specialization). Collision resolution requires operator== on the key.
struct VKRenderingSignatureHash {
    size_t operator()(const VKRenderingSignature &s) const noexcept {
        return static_cast<size_t>(s.hash());
    }
};

} // namespace gfx
} // namespace cc
