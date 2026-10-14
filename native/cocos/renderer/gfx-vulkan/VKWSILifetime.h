#pragma once

#include <vulkan/vulkan_core.h>
#include <cstdint>

namespace cc {
namespace gfx {

constexpr uint64_t VULKAN_WSI_TIMEOUT = 10000000000ULL;

// DEVICE_LOST permits lifetime teardown, not successful rendering or reuse.
// Never advance a normal completion counter based on this predicate.
inline bool wsiTeardownSafe(VkResult result) {
    return result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST;
}

} // namespace gfx
} // namespace cc
