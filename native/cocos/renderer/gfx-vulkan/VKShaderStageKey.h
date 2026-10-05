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

// Owning shader stage key value model (M7-C1). Pure CPU, no Vulkan calls, no
// device-support claims. Deterministic FNV-1a 64-bit hash with explicit
// little-endian uint32 mixing. Full content equality is the final authority.
// Hash collision does not imply equality; the full tuple operator== decides.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace cc {
namespace gfx {
namespace shader_key_detail {

constexpr uint64_t FNV_BASIS = 14695981039346656037ULL;
constexpr uint64_t FNV_PRIME = 1099511628211ULL;

inline void hashU32(uint64_t &seed, uint32_t v) noexcept {
    for (int i = 0; i < 4; ++i) {
        seed ^= static_cast<uint64_t>((v >> (i * 8)) & 0xFFU);
        seed *= FNV_PRIME;
    }
}

inline void hashBytes(uint64_t &seed, const char *data, size_t byteCount) noexcept {
    for (size_t i = 0; i < byteCount; ++i) {
        seed ^= static_cast<uint64_t>(static_cast<uint8_t>(data[i]));
        seed *= FNV_PRIME;
    }
}

} // namespace shader_key_detail

class VulkanShaderStageKey final {
public:
    VulkanShaderStageKey() = default;
    ~VulkanShaderStageKey() = default;
    VulkanShaderStageKey(const VulkanShaderStageKey &) = default;
    VulkanShaderStageKey &operator=(const VulkanShaderStageKey &) = default;
    VulkanShaderStageKey(VulkanShaderStageKey &&) = default;
    VulkanShaderStageKey &operator=(VulkanShaderStageKey &&) = default;

    static constexpr uint32_t MAX_SPIRV_WORDS = 16U * 1024U * 1024U; // 64 MiB
    static constexpr uint32_t MAX_ENTRY_BYTES = 1024;
    static constexpr uint32_t SPIRV_MAGIC = 0x07230203U;

    static bool build(const VkPipelineShaderStageCreateInfo &info,
                      const uint32_t *words, size_t wordCount,
                      VulkanShaderStageKey &out) noexcept {
        const auto reject = [&out]() noexcept {
            out = VulkanShaderStageKey{};
            return false;
        };
        if (info.sType != VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO ||
            info.pNext || info.pSpecializationInfo || !info.module || !isSingleKnownStage(info.stage) ||
            !info.pName || !words || wordCount < 5 || wordCount > MAX_SPIRV_WORDS) return reject();
        size_t entryLen = 0;
        while (entryLen <= MAX_ENTRY_BYTES && info.pName[entryLen]) ++entryLen;
        if (!entryLen || entryLen > MAX_ENTRY_BYTES || words[0] != SPIRV_MAGIC) return reject();
        try {
            // Both input pointers may refer to out. Keep it alive until the
            // complete local snapshot is ready to publish.
            VulkanShaderStageKey snapshot;
            snapshot.flags_ = info.flags;
            snapshot.stage_ = info.stage;
            snapshot.entryPoint_.assign(info.pName, entryLen);
            snapshot.spirvWords_.assign(words, words + wordCount);
            snapshot.valid_ = true;
            out = std::move(snapshot);
            return true;
        } catch (...) {
            return reject();
        }
    }

    bool valid() const noexcept { return valid_; }
    VkShaderStageFlagBits stage() const noexcept { return stage_; }
    VkPipelineShaderStageCreateFlags flags() const noexcept { return flags_; }
    const std::string &entryPoint() const noexcept { return entryPoint_; }
    const std::vector<uint32_t> &spirvWords() const noexcept { return spirvWords_; }

    uint64_t hash() const noexcept {
        // schema: [domain=3, schema=1, valid, flags, stage,
        //          entryByteCount, entry bytes (no NUL),
        //          wordCount, each SPIRV word LE u32]
        uint64_t seed = shader_key_detail::FNV_BASIS;
        shader_key_detail::hashU32(seed, 3); // domain ShaderStageKey
        shader_key_detail::hashU32(seed, 1); // schema
        shader_key_detail::hashU32(seed, static_cast<uint32_t>(valid_));
        shader_key_detail::hashU32(seed, static_cast<uint32_t>(flags_));
        shader_key_detail::hashU32(seed, static_cast<uint32_t>(stage_));
        shader_key_detail::hashU32(seed, static_cast<uint32_t>(entryPoint_.size()));
        shader_key_detail::hashBytes(seed, entryPoint_.data(), entryPoint_.size());
        shader_key_detail::hashU32(seed, static_cast<uint32_t>(spirvWords_.size()));
        for (uint32_t w : spirvWords_)
            shader_key_detail::hashU32(seed, w);
        return seed;
    }

    bool operator==(const VulkanShaderStageKey &o) const noexcept {
        return valid_ == o.valid_ && flags_ == o.flags_ && stage_ == o.stage_ &&
               entryPoint_ == o.entryPoint_ && spirvWords_ == o.spirvWords_;
    }
    bool operator!=(const VulkanShaderStageKey &o) const noexcept { return !(*this == o); }

private:
    static bool isSingleKnownStage(VkShaderStageFlags stage) noexcept {
        constexpr VkShaderStageFlags known =
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
            VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_GEOMETRY_BIT |
            VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        if (stage == 0 || (stage & ~known)) return false;
        return (stage & (stage - 1)) == 0; // exactly one bit
    }

    VkPipelineShaderStageCreateFlags flags_ = 0;
    VkShaderStageFlagBits stage_ = static_cast<VkShaderStageFlagBits>(0);
    std::string entryPoint_;
    std::vector<uint32_t> spirvWords_;
    bool valid_ = false;
};

} // namespace gfx
} // namespace cc
