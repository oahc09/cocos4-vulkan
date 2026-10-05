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

#pragma once

#include <algorithm>
#include <cstdint>
#include <vulkan/vulkan_core.h>

namespace cc {
namespace gfx {

constexpr uint32_t BACKEND_BASELINE_API_VERSION = VK_API_VERSION_1_1;
constexpr uint32_t ENGINE_MAX_API_VERSION = VK_API_VERSION_1_3;

struct VulkanApiVersionInfo {
    uint32_t loaderApiVersion{VK_API_VERSION_1_0};
    uint32_t engineRequestedApiVersion{VK_API_VERSION_1_0};
    uint32_t xrConstrainedApiVersion{VK_API_VERSION_1_0};
    uint32_t instanceApiVersion{VK_API_VERSION_1_0};
    uint32_t physicalDeviceApiVersion{VK_API_VERSION_1_0};
    uint32_t effectiveDeviceApiVersion{VK_API_VERSION_1_0};

    void setLoaderVersion(uint32_t version) {
        loaderApiVersion = version;
        engineRequestedApiVersion = std::min(version, ENGINE_MAX_API_VERSION);
        constrainInstanceVersion(engineRequestedApiVersion);
    }

    void constrainInstanceVersion(uint32_t version) {
        xrConstrainedApiVersion = version;
        instanceApiVersion = std::min(engineRequestedApiVersion, version);
    }

    void setPhysicalDeviceVersion(uint32_t version) {
        physicalDeviceApiVersion = version;
        effectiveDeviceApiVersion = std::min(instanceApiVersion, version);
    }

    bool supportsBaseline() const {
        return effectiveDeviceApiVersion >= BACKEND_BASELINE_API_VERSION;
    }
};

// An absent entry denotes a 1.0 loader; an existing entry's failure must propagate.
inline VkResult queryVulkanLoaderVersion(PFN_vkEnumerateInstanceVersion enumerateVersion, uint32_t *version) {
    *version = VK_API_VERSION_1_0;
    if (!enumerateVersion) {
        return VK_SUCCESS;
    }
    uint32_t reportedVersion = VK_API_VERSION_1_0;
    const VkResult result = enumerateVersion(&reportedVersion);
    if (result == VK_SUCCESS) {
        *version = reportedVersion;
    }
    return result;
}

// ============================================================================
// Capability data model (plan section 4) + state progression (section 5.4) +
// runtime path dependency normalization (section 17.2).
//
// Invariants (M1-A):
//   supported != requested != enabled - three independent stages.
//   enabled becomes true ONLY when: device creation succeeded AND the feature
//   was included in the enable chain AND it was requested AND it is supported.
//   Requesting never mutates supported; finalizing never mutates the version
//   model. Runtime paths never raise the version model.
// Pure data + helpers only: no platform dependencies, no engine integration,
// no assumption that any real entry point has been loaded.
// ============================================================================

struct VulkanCapabilityState {
    bool supported{false};
    bool requested{false};
    bool enabled{false};
};

// Descriptor indexing is a feature FAMILY: each sub-feature carries its own
// state and must never be collapsed into one bool (plan section 4).
struct VulkanDescriptorIndexingCaps {
    VulkanCapabilityState shaderUniformBufferArrayNonUniformIndexing;
    VulkanCapabilityState shaderSampledImageArrayNonUniformIndexing;
    VulkanCapabilityState shaderStorageBufferArrayNonUniformIndexing;
    VulkanCapabilityState shaderStorageImageArrayNonUniformIndexing;

    VulkanCapabilityState descriptorBindingUniformBufferUpdateAfterBind;
    VulkanCapabilityState descriptorBindingSampledImageUpdateAfterBind;
    VulkanCapabilityState descriptorBindingStorageImageUpdateAfterBind;
    VulkanCapabilityState descriptorBindingStorageBufferUpdateAfterBind;
    VulkanCapabilityState descriptorBindingPartiallyBound;
    VulkanCapabilityState descriptorBindingVariableDescriptorCount;
    VulkanCapabilityState runtimeDescriptorArray;
};

// Fragment shading rate is 3 INDEPENDENT features (pipeline / primitive /
// attachment); one sub-feature being supported never implies another.
struct VulkanFragmentShadingRateCaps {
    VulkanCapabilityState pipelineFragmentShadingRate;
    VulkanCapabilityState primitiveFragmentShadingRate;
    VulkanCapabilityState attachmentFragmentShadingRate;
};

struct GfxVKCapabilities {
    VulkanApiVersionInfo version;

    VulkanCapabilityState timelineSemaphore;
    VulkanCapabilityState bufferDeviceAddress;
    VulkanCapabilityState separateDepthStencilLayouts;

    VulkanDescriptorIndexingCaps descriptorIndexing;

    VulkanCapabilityState synchronization2;
    VulkanCapabilityState dynamicRendering;
    VulkanCapabilityState maintenance4;

    // Family SUMMARY over fragmentShadingRateDetail with "any" semantics:
    // supported == any sub-feature supported; requested/enabled are owned by
    // the per-sub-feature states and the summary must not be used as the
    // source for enablement of a specific sub-feature.
    VulkanCapabilityState fragmentShadingRate;
    VulkanFragmentShadingRateCaps fragmentShadingRateDetail;

    VulkanCapabilityState memoryBudget;
    VulkanCapabilityState graphicsPipelineLibrary;
    VulkanCapabilityState swapchainMaintenance1;

    bool debugUtils{false};
    bool portabilitySubset{false};

    // Correctness limit from Maintenance4 properties (see VulkanPropertyQueryChain).
    // Consuming this limit must check caps.maintenance4.enabled.
    VkDeviceSize maxBufferSize{0};
};

inline bool isVulkanBufferSizeAllowed(const GfxVKCapabilities &caps, VkDeviceSize backingSize) {
    return !caps.maintenance4.enabled || backingSize <= caps.maxBufferSize;
}

// Runtime path selection is stored SEPARATELY from capability enablement.
// enabled == true never implies a modern path is selected; all modern paths
// default OFF. useSynchronization2 selects the Sync2 barrier ENCODER; it and
// useSubmit2 are independent A/B choices once synchronization2.enabled is
// true - turning the barrier encoder off must not turn Submit2 off.
// useMaintenance4 selects optional maintenance4-related optimizations, not
// feature enablement. maxBufferSize correctness checks remain mandatory when
// maintenance4 is enabled, even if this optimization path is disabled. Core
// device memory-requirement queries have their own API/entry availability rules.
struct VulkanRuntimePaths {
    bool useTimelineSemaphore{false};
    bool useSynchronization2{false};
    bool useSubmit2{false};
    bool useDynamicRendering{false};
    bool forceClassicRenderPass{false};
    bool useMaintenance4{false};
};

// Stage: engine policy turns a (possibly unsupported) wish into `requested`.
// Recording a request for an unsupported feature is allowed (visible policy
// mistake) but can never become enabled - finalize() enforces that.
inline void requestFeature(VulkanCapabilityState &state, bool requested) {
    state.requested = requested;
}

// Stage (plan section 5.4): vkCreateDevice returned; finalize `enabled`.
// creationSucceeded: device creation result. includedInEnableChain: whether
// the feature structure actually entered this device's pNext enable chain.
// NEVER copy supported into enabled: a created device without the feature in
// its enable chain does not have the feature enabled.
inline void finalizeFeature(VulkanCapabilityState &state, bool creationSucceeded, bool includedInEnableChain) {
    state.enabled = creationSucceeded && includedInEnableChain && state.supported && state.requested;
}

// Device creation failed: no feature is enabled on a device that does not
// exist, regardless of what the chain contained.
inline void markDeviceCreationFailed(GfxVKCapabilities &caps) {
    VulkanCapabilityState *states[] = {
        &caps.timelineSemaphore,
        &caps.bufferDeviceAddress,
        &caps.separateDepthStencilLayouts,
        &caps.synchronization2,
        &caps.dynamicRendering,
        &caps.maintenance4,
        &caps.fragmentShadingRate,
        &caps.fragmentShadingRateDetail.pipelineFragmentShadingRate,
        &caps.fragmentShadingRateDetail.primitiveFragmentShadingRate,
        &caps.fragmentShadingRateDetail.attachmentFragmentShadingRate,
        &caps.memoryBudget,
        &caps.graphicsPipelineLibrary,
        &caps.swapchainMaintenance1,
        &caps.descriptorIndexing.shaderUniformBufferArrayNonUniformIndexing,
        &caps.descriptorIndexing.shaderSampledImageArrayNonUniformIndexing,
        &caps.descriptorIndexing.shaderStorageBufferArrayNonUniformIndexing,
        &caps.descriptorIndexing.shaderStorageImageArrayNonUniformIndexing,
        &caps.descriptorIndexing.descriptorBindingUniformBufferUpdateAfterBind,
        &caps.descriptorIndexing.descriptorBindingSampledImageUpdateAfterBind,
        &caps.descriptorIndexing.descriptorBindingStorageImageUpdateAfterBind,
        &caps.descriptorIndexing.descriptorBindingStorageBufferUpdateAfterBind,
        &caps.descriptorIndexing.descriptorBindingPartiallyBound,
        &caps.descriptorIndexing.descriptorBindingVariableDescriptorCount,
        &caps.descriptorIndexing.runtimeDescriptorArray,
    };
    for (VulkanCapabilityState *state : states) {
        state->enabled = false;
    }
}

// Centralized runtime path dependency normalization (plan section 17.2).
// Hard rules:
//   !synchronization2.enabled -> BOTH Sync2 barrier encoder and Submit2 off
//     (vkQueueSubmit2 belongs to the synchronization2 capability path).
//   synchronization2.enabled  -> barrier encoder and Submit2 stay independent
//     (legacy barrier encoder + Submit2 is a legal A/B combination).
//   forceClassicRenderPass    -> dynamic rendering off (manual override).
//   !dynamicRendering.enabled -> dynamic rendering off.
//   !timelineSemaphore.enabled-> timeline path off (fence completion stays).
//   !maintenance4.enabled     -> maintenance4 optimizations off.
// This function only ever narrows paths; it never mutates the capability
// states or the version model (a 1.1/1.2 device can never be "raised" by
// requesting modern runtime paths).
inline VulkanRuntimePaths normalizeRuntimePaths(const GfxVKCapabilities &caps, VulkanRuntimePaths requestedPaths) {
    VulkanRuntimePaths paths = requestedPaths;
    if (!caps.synchronization2.enabled) {
        paths.useSynchronization2 = false;
        paths.useSubmit2 = false;
    }
    if (!caps.dynamicRendering.enabled) {
        paths.useDynamicRendering = false;
    }
    if (paths.forceClassicRenderPass) {
        paths.useDynamicRendering = false;
    }
    if (!caps.timelineSemaphore.enabled) {
        paths.useTimelineSemaphore = false;
    }
    if (!caps.maintenance4.enabled) {
        paths.useMaintenance4 = false;
    }
    return paths;
}

} // namespace gfx
} // namespace cc
