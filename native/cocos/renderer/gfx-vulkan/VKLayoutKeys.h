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

// Owning layout key value models (M7-B1, rev3). Pure CPU, no Vulkan calls,
// no device-support claims. Deterministic FNV-1a 64-bit hashing with explicit
// little-endian uint32 mixing. Full content equality is the final authority.
// Build functions take a local owning snapshot, validate, then move-publish;
// failures (including bad_alloc) fully reset to default invalid.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace cc {
namespace gfx {
namespace layout_key_detail {

constexpr uint64_t FNV_BASIS = 14695981039346656037ULL;
constexpr uint64_t FNV_PRIME = 1099511628211ULL;

inline void hashU32(uint64_t &seed, uint32_t v) noexcept {
    for (int i = 0; i < 4; ++i) {
        seed ^= static_cast<uint64_t>((v >> (i * 8)) & 0xFFU);
        seed *= FNV_PRIME;
    }
}

} // namespace layout_key_detail

class VulkanDescriptorSetLayoutKey final {
public:
    struct Binding {
        uint32_t binding = 0;
        VkDescriptorType descriptorType = static_cast<VkDescriptorType>(0);
        uint32_t descriptorCount = 0;
        VkShaderStageFlags stageFlags = 0;
        bool operator==(const Binding &o) const noexcept {
            return binding == o.binding && descriptorType == o.descriptorType &&
                   descriptorCount == o.descriptorCount && stageFlags == o.stageFlags;
        }
        bool operator!=(const Binding &o) const noexcept { return !(*this == o); }
    };

    VulkanDescriptorSetLayoutKey() = default;
    ~VulkanDescriptorSetLayoutKey() = default;
    VulkanDescriptorSetLayoutKey(const VulkanDescriptorSetLayoutKey &) = default;
    VulkanDescriptorSetLayoutKey &operator=(const VulkanDescriptorSetLayoutKey &) = default;
    VulkanDescriptorSetLayoutKey(VulkanDescriptorSetLayoutKey &&) = default;
    VulkanDescriptorSetLayoutKey &operator=(VulkanDescriptorSetLayoutKey &&) = default;

    static constexpr uint32_t MAX_BINDINGS = 4096;

    static bool build(const VkDescriptorSetLayoutCreateInfo &info,
                      VulkanDescriptorSetLayoutKey &out) noexcept {
        out = VulkanDescriptorSetLayoutKey{};
        if (info.sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO) return false;
        if (info.pNext != nullptr) return false;
        if (info.bindingCount > MAX_BINDINGS) return false;
        if (info.bindingCount > 0 && info.pBindings == nullptr) return false;
        for (uint32_t i = 0; i < info.bindingCount; ++i) {
            if (info.pBindings[i].pImmutableSamplers != nullptr) return false;
        }
        try {
            std::vector<Binding> sorted;
            sorted.reserve(info.bindingCount);
            for (uint32_t i = 0; i < info.bindingCount; ++i) {
                Binding b;
                b.binding = info.pBindings[i].binding;
                b.descriptorType = info.pBindings[i].descriptorType;
                b.descriptorCount = info.pBindings[i].descriptorCount;
                b.stageFlags = info.pBindings[i].stageFlags;
                sorted.push_back(b);
            }
            std::sort(sorted.begin(), sorted.end(),
                      [](const Binding &a, const Binding &b) { return a.binding < b.binding; });
            for (size_t i = 1; i < sorted.size(); ++i) {
                if (sorted[i].binding == sorted[i - 1].binding) return false;
            }
            out.flags_ = info.flags;
            out.bindings_ = std::move(sorted);
            out.valid_ = true;
            return true;
        } catch (...) {
            out = VulkanDescriptorSetLayoutKey{};
            return false;
        }
    }

    bool valid() const noexcept { return valid_; }
    VkDescriptorSetLayoutCreateFlags flags() const noexcept { return flags_; }
    const std::vector<Binding> &bindings() const noexcept { return bindings_; }

    uint64_t hash() const noexcept {
        // schema: [domain=1, schema=1, valid, flags, bindingCount,
        //          each binding {binding,type,count,stages}]
        uint64_t seed = layout_key_detail::FNV_BASIS;
        layout_key_detail::hashU32(seed, 1); // domain DS
        layout_key_detail::hashU32(seed, 1); // schema
        layout_key_detail::hashU32(seed, static_cast<uint32_t>(valid_));
        layout_key_detail::hashU32(seed, static_cast<uint32_t>(flags_));
        layout_key_detail::hashU32(seed, static_cast<uint32_t>(bindings_.size()));
        for (const auto &b : bindings_) {
            layout_key_detail::hashU32(seed, b.binding);
            layout_key_detail::hashU32(seed, static_cast<uint32_t>(b.descriptorType));
            layout_key_detail::hashU32(seed, b.descriptorCount);
            layout_key_detail::hashU32(seed, static_cast<uint32_t>(b.stageFlags));
        }
        return seed;
    }

    bool operator==(const VulkanDescriptorSetLayoutKey &o) const noexcept {
        if (valid_ != o.valid_ || flags_ != o.flags_ || bindings_.size() != o.bindings_.size()) return false;
        for (size_t i = 0; i < bindings_.size(); ++i)
            if (!(bindings_[i] == o.bindings_[i])) return false;
        return true;
    }
    bool operator!=(const VulkanDescriptorSetLayoutKey &o) const noexcept { return !(*this == o); }

private:
    VkDescriptorSetLayoutCreateFlags flags_ = 0;
    std::vector<Binding> bindings_;
    bool valid_ = false;
};

class VulkanPipelineLayoutKey final {
public:
    struct PushRange {
        VkShaderStageFlags stageFlags = 0;
        uint32_t offset = 0;
        uint32_t size = 0;
        bool operator==(const PushRange &o) const noexcept {
            return stageFlags == o.stageFlags && offset == o.offset && size == o.size;
        }
        bool operator!=(const PushRange &o) const noexcept { return !(*this == o); }
    };

    VulkanPipelineLayoutKey() = default;
    ~VulkanPipelineLayoutKey() = default;
    VulkanPipelineLayoutKey(const VulkanPipelineLayoutKey &) = default;
    VulkanPipelineLayoutKey &operator=(const VulkanPipelineLayoutKey &) = default;
    VulkanPipelineLayoutKey(VulkanPipelineLayoutKey &&) = default;
    VulkanPipelineLayoutKey &operator=(VulkanPipelineLayoutKey &&) = default;

    static constexpr uint32_t MAX_SETS = 4096;
    static constexpr uint32_t MAX_PUSH = 128;

    static bool build(const VkPipelineLayoutCreateInfo &info,
                      const std::vector<VulkanDescriptorSetLayoutKey> &orderedSetKeys,
                      VulkanPipelineLayoutKey &out) noexcept {
        // Phase 1: shape checks (read orderedSetKeys; may alias out.children_)
        bool shapeOk = true;
        if (info.sType != VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO) shapeOk = false;
        if (info.pNext != nullptr) shapeOk = false;
        if (info.setLayoutCount != orderedSetKeys.size()) shapeOk = false;
        if (info.setLayoutCount > 0 && info.pSetLayouts == nullptr) shapeOk = false;
        if (info.setLayoutCount > MAX_SETS) shapeOk = false;
        if (info.pushConstantRangeCount > 0 && info.pPushConstantRanges == nullptr) shapeOk = false;
        if (info.pushConstantRangeCount > MAX_PUSH) shapeOk = false;
        if (!shapeOk) {
            out = VulkanPipelineLayoutKey{};
            return false;
        }
        for (const auto &k : orderedSetKeys) {
            if (!k.valid()) {
                out = VulkanPipelineLayoutKey{};
                return false;
            }
        }

        // Phase 2: snapshot BEFORE any reset (orderedSetKeys may alias out.children_)
        std::vector<VulkanDescriptorSetLayoutKey> savedChildren;
        std::vector<PushRange> savedPushRanges;
        try {
            savedChildren = orderedSetKeys;
            savedPushRanges.reserve(info.pushConstantRangeCount);
            for (uint32_t i = 0; i < info.pushConstantRangeCount; ++i) {
                PushRange pr;
                pr.stageFlags = info.pPushConstantRanges[i].stageFlags;
                pr.offset = info.pPushConstantRanges[i].offset;
                pr.size = info.pPushConstantRanges[i].size;
                savedPushRanges.push_back(pr);
            }
        } catch (...) {
            out = VulkanPipelineLayoutKey{};
            return false;
        }

        // Phase 3: deferred reset (safe now, snapshots are owned)
        out = VulkanPipelineLayoutKey{};

        // Phase 4: publish
        out.flags_ = info.flags;
        out.setCount_ = info.setLayoutCount;
        out.children_ = std::move(savedChildren);
        out.pushRanges_ = std::move(savedPushRanges);
        out.valid_ = true;
        return true;
    }

    bool valid() const noexcept { return valid_; }
    VkPipelineLayoutCreateFlags flags() const noexcept { return flags_; }
    uint32_t setLayoutCount() const noexcept { return setCount_; }
    const std::vector<VulkanDescriptorSetLayoutKey> &setLayouts() const noexcept { return children_; }
    const std::vector<PushRange> &pushConstantRanges() const noexcept { return pushRanges_; }

    uint64_t hash() const noexcept {
        // schema: [domain=2, schema=1, valid, flags, setCount,
        //          each child {domain=1, schema=1, valid, flags, bindingCount,
        //                      each binding {binding,type,count,stages}},
        //          pushCount, each push {stages,offset,size}]
        uint64_t seed = layout_key_detail::FNV_BASIS;
        layout_key_detail::hashU32(seed, 2); // domain PL
        layout_key_detail::hashU32(seed, 1); // schema
        layout_key_detail::hashU32(seed, static_cast<uint32_t>(valid_));
        layout_key_detail::hashU32(seed, static_cast<uint32_t>(flags_));
        layout_key_detail::hashU32(seed, setCount_);
        for (const auto &child : children_) {
            // full DS hash stream per child (same as DS domain=1 stream)
            layout_key_detail::hashU32(seed, 1); // child domain DS
            layout_key_detail::hashU32(seed, 1); // child schema
            layout_key_detail::hashU32(seed, static_cast<uint32_t>(child.valid()));
            layout_key_detail::hashU32(seed, static_cast<uint32_t>(child.flags()));
            layout_key_detail::hashU32(seed, static_cast<uint32_t>(child.bindings().size()));
            for (const auto &b : child.bindings()) {
                layout_key_detail::hashU32(seed, b.binding);
                layout_key_detail::hashU32(seed, static_cast<uint32_t>(b.descriptorType));
                layout_key_detail::hashU32(seed, b.descriptorCount);
                layout_key_detail::hashU32(seed, static_cast<uint32_t>(b.stageFlags));
            }
        }
        layout_key_detail::hashU32(seed, static_cast<uint32_t>(pushRanges_.size()));
        for (const auto &pr : pushRanges_) {
            layout_key_detail::hashU32(seed, static_cast<uint32_t>(pr.stageFlags));
            layout_key_detail::hashU32(seed, pr.offset);
            layout_key_detail::hashU32(seed, pr.size);
        }
        return seed;
    }

    bool operator==(const VulkanPipelineLayoutKey &o) const noexcept {
        if (valid_ != o.valid_ || flags_ != o.flags_ || setCount_ != o.setCount_ ||
            children_.size() != o.children_.size() || pushRanges_.size() != o.pushRanges_.size())
            return false;
        for (size_t i = 0; i < children_.size(); ++i)
            if (!(children_[i] == o.children_[i])) return false;
        for (size_t i = 0; i < pushRanges_.size(); ++i)
            if (!(pushRanges_[i] == o.pushRanges_[i])) return false;
        return true;
    }
    bool operator!=(const VulkanPipelineLayoutKey &o) const noexcept { return !(*this == o); }

private:
    VkPipelineLayoutCreateFlags flags_ = 0;
    uint32_t setCount_ = 0;
    std::vector<VulkanDescriptorSetLayoutKey> children_;
    std::vector<PushRange> pushRanges_;
    bool valid_ = false;
};

} // namespace gfx
} // namespace cc
