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
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.
****************************************************************************/

// ============================================================================
// Vulkan feature query / enable chain builder (M1-B, plan sections 4/5/17).
//
// Pure header-only data structure: no platform or engine dependencies. The
// engine integration (querying a real VkPhysicalDevice, creating a real
// VkDevice) is performed by the caller.
//
// Roles and call order:
//   QUERY object:  buildQuery(effectiveApiVersion, availableExtensions)
//                  -> caller runs vkGetPhysicalDeviceFeatures2(device, head())
//                  -> readSupported(caps)            (fills caps.*.supported)
//   ENABLE object: buildEnable(queryObject, requestedCaps,
//                              callerFilteredBaseFeatures,
//                              actuallyEnabledExtensions)
//                  -> caller embeds head() into VkDeviceCreateInfo::pNext and
//                     calls vkCreateDevice
//                  -> finalizeEnabled(caps, creationSucceeded)
//
// Canonical tiering (prevents duplicated pNext structures, VUIDs
// pNext-02829 / pNext-02830 / pNext-06532):
//   effective < 1.1            -> buildQuery/buildEnable return false
//   effective == 1.1           -> independent KHR/EXT feature structs only;
//                                  NEVER VkPhysicalDeviceVulkan11Features
//                                  (that aggregate is provided by Vulkan 1.2)
//   effective == 1.2           -> Vulkan11 + Vulkan12 aggregates; never the
//                                  promoted independent structs of their
//                                  members; 1.3-core features (sync2 / dynamic
//                                  rendering / maintenance4) stay independent
//                                  KHR structs when the extension is available
//   effective >= 1.3           -> + Vulkan13 aggregate; sync2 / dynamic /
//                                  maintenance4 come from the aggregate (core,
//                                  extension names must NOT be required);
//                                  Vulkan 1.4 chains are not modeled (a 1.4
//                                  input is capped to the 1.3 chain shape)
//
// Boundaries (caller responsibilities, NOT handled here):
//   - Extension dependency closure (e.g. dynamic rendering may need
//     VK_KHR_depth_stencil_resolve): `actuallyEnabledExtensions` must mirror
//     the REAL extension list of this vkCreateDevice call; a missing flag
//     means the corresponding independent feature is NOT attached/enabled.
//   - The builder never silently enables an extension or claims one enabled.
//   - baseFeatures must already be policy-filtered by the caller; it is
//     passed through verbatim (no whole-copy of supported bits).
//   - maxBufferSize is a correctness limit: consuming it must check
//     caps.maintenance4.enabled. vkGetDeviceBufferMemoryRequirements itself
//     has its own entry conditions independent of the feature bit.
// ============================================================================

#pragma once

#include <cstdint>
#include <vulkan/vulkan_core.h>

#include "VKCapabilities.h"

namespace cc {
namespace gfx {

// Pure data: which relevant device extensions exist (query time) or were
// ACTUALLY enabled for this vkCreateDevice call (enable time). Filled by the
// caller from vkEnumerateDeviceExtensionProperties / the real enabled list.
struct VulkanFeatureExtensionFlags {
    bool khrTimelineSemaphore{false};
    bool khrBufferDeviceAddress{false};
    bool extDescriptorIndexing{false};
    bool khrSeparateDepthStencilLayouts{false};
    bool khrSynchronization2{false};
    bool khrDynamicRendering{false};
    bool khrMaintenance4{false};
    bool khrFragmentShadingRate{false};
    bool extMemoryBudget{false};
    bool extGraphicsPipelineLibrary{false};
};

template <typename HasExtension>
inline VulkanFeatureExtensionFlags getVulkanFeatureExtensionFlags(HasExtension hasExtension) {
    VulkanFeatureExtensionFlags flags;
    flags.khrTimelineSemaphore = hasExtension(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    flags.khrBufferDeviceAddress = hasExtension(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    flags.extDescriptorIndexing = hasExtension(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
    flags.khrSeparateDepthStencilLayouts = hasExtension(VK_KHR_SEPARATE_DEPTH_STENCIL_LAYOUTS_EXTENSION_NAME);
    flags.khrSynchronization2 = hasExtension(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    flags.khrDynamicRendering = hasExtension(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    flags.khrMaintenance4 = hasExtension(VK_KHR_MAINTENANCE_4_EXTENSION_NAME);
    flags.khrFragmentShadingRate = hasExtension(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
    flags.extMemoryBudget = hasExtension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    flags.extGraphicsPipelineLibrary = hasExtension(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
    return flags;
}

// Which chain members are attached (bit indices are stable for introspection).
enum class VulkanFeatureChainMember : uint32_t {
    BASE_FEATURES2 = 0,
    TIMELINE_SEMAPHORE_KHR,
    BUFFER_DEVICE_ADDRESS_KHR,
    DESCRIPTOR_INDEXING_EXT,
    SEPARATE_DEPTH_STENCIL_KHR,
    SYNCHRONIZATION2_KHR,
    DYNAMIC_RENDERING_KHR,
    MAINTENANCE4_KHR,
    FRAGMENT_SHADING_RATE_KHR,
    GRAPHICS_PIPELINE_LIBRARY_EXT,
    VULKAN_11_FEATURES,
    VULKAN_12_FEATURES,
    VULKAN_13_FEATURES,
    COUNT
};

class VulkanFeatureChain {
public:
    VulkanFeatureChain() { reset(); }

    // Owning pNext chains point into this object; copying/moving would dangle.
    VulkanFeatureChain(const VulkanFeatureChain &) = delete;
    VulkanFeatureChain &operator=(const VulkanFeatureChain &) = delete;
    VulkanFeatureChain(VulkanFeatureChain &&) = delete;
    VulkanFeatureChain &operator=(VulkanFeatureChain &&) = delete;

    // ---- query role ---------------------------------------------------------
    // effectiveApiVersion = min(instance, physicalDevice) version.
    // Returns false (empty chain) when the tier is below the 1.1 baseline.
    bool buildQuery(uint32_t effectiveApiVersion, const VulkanFeatureExtensionFlags &available) {
        enableRole_ = false;
        available_ = available;
        tier_ = clampTier_(effectiveApiVersion);
        if (tier_ < VK_API_VERSION_1_1) {
            reset();
            return false;
        }
        reset();
        attachBase_();
        if (tier_ >= VK_API_VERSION_1_2) {
            attach(VulkanFeatureChainMember::VULKAN_11_FEATURES, &vulkan11_);
            attach(VulkanFeatureChainMember::VULKAN_12_FEATURES, &vulkan12_);
        }
        if (tier_ >= VK_API_VERSION_1_3) {
            attach(VulkanFeatureChainMember::VULKAN_13_FEATURES, &vulkan13_);
        } else {
            // 1.3-core features are still KHR extensions on 1.1/1.2: query
            // their structs only when the extension is reported available.
            if (available.khrSynchronization2) attach(VulkanFeatureChainMember::SYNCHRONIZATION2_KHR, &synchronization2_);
            if (available.khrDynamicRendering) attach(VulkanFeatureChainMember::DYNAMIC_RENDERING_KHR, &dynamicRendering_);
            if (available.khrMaintenance4) attach(VulkanFeatureChainMember::MAINTENANCE4_KHR, &maintenance4_);
        }
        if (tier_ < VK_API_VERSION_1_2) {
            // Promoted-to-1.2 features live in independent structs on 1.1.
            if (available.khrTimelineSemaphore) attach(VulkanFeatureChainMember::TIMELINE_SEMAPHORE_KHR, &timelineSemaphore_);
            if (available.khrBufferDeviceAddress) attach(VulkanFeatureChainMember::BUFFER_DEVICE_ADDRESS_KHR, &bufferDeviceAddress_);
            if (available.extDescriptorIndexing) attach(VulkanFeatureChainMember::DESCRIPTOR_INDEXING_EXT, &descriptorIndexing_);
            if (available.khrSeparateDepthStencilLayouts) attach(VulkanFeatureChainMember::SEPARATE_DEPTH_STENCIL_KHR, &separateDepthStencilLayouts_);
        }
        // Never-promoted extensions: independent structs at every tier.
        if (available.khrFragmentShadingRate) attach(VulkanFeatureChainMember::FRAGMENT_SHADING_RATE_KHR, &fragmentShadingRate_);
        if (available.extGraphicsPipelineLibrary) attach(VulkanFeatureChainMember::GRAPHICS_PIPELINE_LIBRARY_EXT, &graphicsPipelineLibrary_);
        finishBuild_();
        return true;
    }

    // Map the FILLED query structs (after the caller ran
    // vkGetPhysicalDeviceFeatures2) into caps.*.supported. Never touches
    // requested/enabled or the version model.
    void readSupported(GfxVKCapabilities &caps) const {
        if (tier_ < VK_API_VERSION_1_1 || enableRole_) return;
        const bool via12 = attached_[static_cast<size_t>(VulkanFeatureChainMember::VULKAN_12_FEATURES)];

        caps.timelineSemaphore.supported = via12 ? (vulkan12_.timelineSemaphore == VK_TRUE)
                                                 : bit_(VulkanFeatureChainMember::TIMELINE_SEMAPHORE_KHR, timelineSemaphore_.timelineSemaphore);
        caps.bufferDeviceAddress.supported = via12 ? (vulkan12_.bufferDeviceAddress == VK_TRUE)
                                                   : bit_(VulkanFeatureChainMember::BUFFER_DEVICE_ADDRESS_KHR, bufferDeviceAddress_.bufferDeviceAddress);
        caps.separateDepthStencilLayouts.supported = via12 ? (vulkan12_.separateDepthStencilLayouts == VK_TRUE)
                                                            : bit_(VulkanFeatureChainMember::SEPARATE_DEPTH_STENCIL_KHR, separateDepthStencilLayouts_.separateDepthStencilLayouts);

        const auto di = [this, via12](VkBool32 VkPhysicalDeviceVulkan12Features::*member12, VkBool32 VkPhysicalDeviceDescriptorIndexingFeatures::*memberExt) {
            return via12 ? (vulkan12_.*member12 == VK_TRUE)
                         : bit_(VulkanFeatureChainMember::DESCRIPTOR_INDEXING_EXT, descriptorIndexing_.*memberExt);
        };
        caps.descriptorIndexing.shaderUniformBufferArrayNonUniformIndexing.supported =
            di(&VkPhysicalDeviceVulkan12Features::shaderUniformBufferArrayNonUniformIndexing, &VkPhysicalDeviceDescriptorIndexingFeatures::shaderUniformBufferArrayNonUniformIndexing);
        caps.descriptorIndexing.shaderSampledImageArrayNonUniformIndexing.supported =
            di(&VkPhysicalDeviceVulkan12Features::shaderSampledImageArrayNonUniformIndexing, &VkPhysicalDeviceDescriptorIndexingFeatures::shaderSampledImageArrayNonUniformIndexing);
        caps.descriptorIndexing.shaderStorageBufferArrayNonUniformIndexing.supported =
            di(&VkPhysicalDeviceVulkan12Features::shaderStorageBufferArrayNonUniformIndexing, &VkPhysicalDeviceDescriptorIndexingFeatures::shaderStorageBufferArrayNonUniformIndexing);
        caps.descriptorIndexing.shaderStorageImageArrayNonUniformIndexing.supported =
            di(&VkPhysicalDeviceVulkan12Features::shaderStorageImageArrayNonUniformIndexing, &VkPhysicalDeviceDescriptorIndexingFeatures::shaderStorageImageArrayNonUniformIndexing);
        caps.descriptorIndexing.descriptorBindingUniformBufferUpdateAfterBind.supported =
            di(&VkPhysicalDeviceVulkan12Features::descriptorBindingUniformBufferUpdateAfterBind, &VkPhysicalDeviceDescriptorIndexingFeatures::descriptorBindingUniformBufferUpdateAfterBind);
        caps.descriptorIndexing.descriptorBindingSampledImageUpdateAfterBind.supported =
            di(&VkPhysicalDeviceVulkan12Features::descriptorBindingSampledImageUpdateAfterBind, &VkPhysicalDeviceDescriptorIndexingFeatures::descriptorBindingSampledImageUpdateAfterBind);
        caps.descriptorIndexing.descriptorBindingStorageImageUpdateAfterBind.supported =
            di(&VkPhysicalDeviceVulkan12Features::descriptorBindingStorageImageUpdateAfterBind, &VkPhysicalDeviceDescriptorIndexingFeatures::descriptorBindingStorageImageUpdateAfterBind);
        caps.descriptorIndexing.descriptorBindingStorageBufferUpdateAfterBind.supported =
            di(&VkPhysicalDeviceVulkan12Features::descriptorBindingStorageBufferUpdateAfterBind, &VkPhysicalDeviceDescriptorIndexingFeatures::descriptorBindingStorageBufferUpdateAfterBind);
        caps.descriptorIndexing.descriptorBindingPartiallyBound.supported =
            di(&VkPhysicalDeviceVulkan12Features::descriptorBindingPartiallyBound, &VkPhysicalDeviceDescriptorIndexingFeatures::descriptorBindingPartiallyBound);
        caps.descriptorIndexing.descriptorBindingVariableDescriptorCount.supported =
            di(&VkPhysicalDeviceVulkan12Features::descriptorBindingVariableDescriptorCount, &VkPhysicalDeviceDescriptorIndexingFeatures::descriptorBindingVariableDescriptorCount);
        caps.descriptorIndexing.runtimeDescriptorArray.supported =
            di(&VkPhysicalDeviceVulkan12Features::runtimeDescriptorArray, &VkPhysicalDeviceDescriptorIndexingFeatures::runtimeDescriptorArray);

        if (attached_[static_cast<size_t>(VulkanFeatureChainMember::VULKAN_13_FEATURES)]) {
            caps.synchronization2.supported = vulkan13_.synchronization2 == VK_TRUE;
            caps.dynamicRendering.supported = vulkan13_.dynamicRendering == VK_TRUE;
            caps.maintenance4.supported = vulkan13_.maintenance4 == VK_TRUE;
        } else {
            caps.synchronization2.supported = bit_(VulkanFeatureChainMember::SYNCHRONIZATION2_KHR, synchronization2_.synchronization2);
            caps.dynamicRendering.supported = bit_(VulkanFeatureChainMember::DYNAMIC_RENDERING_KHR, dynamicRendering_.dynamicRendering);
            caps.maintenance4.supported = bit_(VulkanFeatureChainMember::MAINTENANCE4_KHR, maintenance4_.maintenance4);
        }

        caps.fragmentShadingRateDetail.pipelineFragmentShadingRate.supported = bit_(VulkanFeatureChainMember::FRAGMENT_SHADING_RATE_KHR, fragmentShadingRate_.pipelineFragmentShadingRate);
        caps.fragmentShadingRateDetail.primitiveFragmentShadingRate.supported = bit_(VulkanFeatureChainMember::FRAGMENT_SHADING_RATE_KHR, fragmentShadingRate_.primitiveFragmentShadingRate);
        caps.fragmentShadingRateDetail.attachmentFragmentShadingRate.supported = bit_(VulkanFeatureChainMember::FRAGMENT_SHADING_RATE_KHR, fragmentShadingRate_.attachmentFragmentShadingRate);
        // family summary: ANY sub-feature (documented in VKCapabilities.h)
        caps.fragmentShadingRate.supported =
            caps.fragmentShadingRateDetail.pipelineFragmentShadingRate.supported ||
            caps.fragmentShadingRateDetail.primitiveFragmentShadingRate.supported ||
            caps.fragmentShadingRateDetail.attachmentFragmentShadingRate.supported;

        caps.memoryBudget.supported = available_.extMemoryBudget; // no feature struct
        caps.graphicsPipelineLibrary.supported =
            available_.extGraphicsPipelineLibrary && bit_(VulkanFeatureChainMember::GRAPHICS_PIPELINE_LIBRARY_EXT, graphicsPipelineLibrary_.graphicsPipelineLibrary);
    }

    // ---- enable role ----------------------------------------------------------
    // queryChain must be a built QUERY object. requestedCaps supplies the
    // engine policy (requested flags). baseFeatures is passed through
    // verbatim. actuallyEnabled must mirror the REAL extension list of this
    // vkCreateDevice call - a missing flag forbids attaching/enabling the
    // corresponding KHR/EXT independent feature.
    bool buildEnable(const VulkanFeatureChain &queryChain,
                     const GfxVKCapabilities &policyCaps,
                     const VkPhysicalDeviceFeatures &callerFilteredBaseFeatures,
                     const VulkanFeatureExtensionFlags &actuallyEnabled) {
        enableRole_ = true;
        actuallyEnabled_ = actuallyEnabled;
        tier_ = queryChain.tier_;
        if (queryChain.enableRole_ || queryChain.tier_ < VK_API_VERSION_1_1) {
            reset();
            return false;
        }
        reset();
        features2_.features = callerFilteredBaseFeatures; // verbatim, no expansion

        // Consume the QUERY's real supported bits: refresh a local snapshot via
        // readSupported (it only overwrites `supported`), keeping the policy's
        // `requested` flags. Caller caches are NEVER trusted as substitutes
        // for the query results - stale/unsupported entries get wiped here.
        GfxVKCapabilities requestedCaps = policyCaps;
        queryChain.readSupported(requestedCaps);
        attachBase_();

        const auto want = [&requestedCaps](const VulkanCapabilityState &s) {
            return s.requested; // caller policy already applied
        };
        const auto q = [&queryChain](const VulkanCapabilityState &s) {
            return s.supported;
        };

        if (tier_ >= VK_API_VERSION_1_2) {
            VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            const auto set = [&v12](VkBool32 VkPhysicalDeviceVulkan12Features::*m, bool on) {
                if (on) v12.*m = VK_TRUE;
            };
            set(&VkPhysicalDeviceVulkan12Features::timelineSemaphore, q(requestedCaps.timelineSemaphore) && want(requestedCaps.timelineSemaphore));
            set(&VkPhysicalDeviceVulkan12Features::bufferDeviceAddress, q(requestedCaps.bufferDeviceAddress) && want(requestedCaps.bufferDeviceAddress));
            set(&VkPhysicalDeviceVulkan12Features::separateDepthStencilLayouts, q(requestedCaps.separateDepthStencilLayouts) && want(requestedCaps.separateDepthStencilLayouts));
            const auto setDI = [&v12](VkBool32 VkPhysicalDeviceVulkan12Features::*m, const VulkanCapabilityState &s) {
                if (s.supported && s.requested) v12.*m = VK_TRUE;
            };
            setDI(&VkPhysicalDeviceVulkan12Features::shaderUniformBufferArrayNonUniformIndexing, requestedCaps.descriptorIndexing.shaderUniformBufferArrayNonUniformIndexing);
            setDI(&VkPhysicalDeviceVulkan12Features::shaderSampledImageArrayNonUniformIndexing, requestedCaps.descriptorIndexing.shaderSampledImageArrayNonUniformIndexing);
            setDI(&VkPhysicalDeviceVulkan12Features::shaderStorageBufferArrayNonUniformIndexing, requestedCaps.descriptorIndexing.shaderStorageBufferArrayNonUniformIndexing);
            setDI(&VkPhysicalDeviceVulkan12Features::shaderStorageImageArrayNonUniformIndexing, requestedCaps.descriptorIndexing.shaderStorageImageArrayNonUniformIndexing);
            setDI(&VkPhysicalDeviceVulkan12Features::descriptorBindingUniformBufferUpdateAfterBind, requestedCaps.descriptorIndexing.descriptorBindingUniformBufferUpdateAfterBind);
            setDI(&VkPhysicalDeviceVulkan12Features::descriptorBindingSampledImageUpdateAfterBind, requestedCaps.descriptorIndexing.descriptorBindingSampledImageUpdateAfterBind);
            setDI(&VkPhysicalDeviceVulkan12Features::descriptorBindingStorageImageUpdateAfterBind, requestedCaps.descriptorIndexing.descriptorBindingStorageImageUpdateAfterBind);
            setDI(&VkPhysicalDeviceVulkan12Features::descriptorBindingStorageBufferUpdateAfterBind, requestedCaps.descriptorIndexing.descriptorBindingStorageBufferUpdateAfterBind);
            setDI(&VkPhysicalDeviceVulkan12Features::descriptorBindingPartiallyBound, requestedCaps.descriptorIndexing.descriptorBindingPartiallyBound);
            setDI(&VkPhysicalDeviceVulkan12Features::descriptorBindingVariableDescriptorCount, requestedCaps.descriptorIndexing.descriptorBindingVariableDescriptorCount);
            setDI(&VkPhysicalDeviceVulkan12Features::runtimeDescriptorArray, requestedCaps.descriptorIndexing.runtimeDescriptorArray);
            const bool any12 = v12.timelineSemaphore || v12.bufferDeviceAddress || v12.separateDepthStencilLayouts ||
                               v12.shaderUniformBufferArrayNonUniformIndexing || v12.shaderSampledImageArrayNonUniformIndexing ||
                               v12.shaderStorageBufferArrayNonUniformIndexing || v12.shaderStorageImageArrayNonUniformIndexing ||
                               v12.descriptorBindingUniformBufferUpdateAfterBind || v12.descriptorBindingSampledImageUpdateAfterBind ||
                               v12.descriptorBindingStorageImageUpdateAfterBind || v12.descriptorBindingStorageBufferUpdateAfterBind ||
                               v12.descriptorBindingPartiallyBound || v12.descriptorBindingVariableDescriptorCount || v12.runtimeDescriptorArray;
            if (any12) {
                vulkan12_ = v12;
                attach(VulkanFeatureChainMember::VULKAN_12_FEATURES, &vulkan12_);
            }
            // Vulkan11Features: no members modeled -> never attached (avoid an
            // all-VK_FALSE aggregate); document if members are added later.
        } else {
            // 1.1: independent structs; every one needs its REAL enabled flag.
            if (actuallyEnabled.khrTimelineSemaphore && q(requestedCaps.timelineSemaphore) && want(requestedCaps.timelineSemaphore)) {
                timelineSemaphore_.timelineSemaphore = VK_TRUE;
                attach(VulkanFeatureChainMember::TIMELINE_SEMAPHORE_KHR, &timelineSemaphore_);
            }
            if (actuallyEnabled.khrBufferDeviceAddress && q(requestedCaps.bufferDeviceAddress) && want(requestedCaps.bufferDeviceAddress)) {
                bufferDeviceAddress_.bufferDeviceAddress = VK_TRUE;
                attach(VulkanFeatureChainMember::BUFFER_DEVICE_ADDRESS_KHR, &bufferDeviceAddress_);
            }
            if (actuallyEnabled.extDescriptorIndexing) {
                const auto setDI = [this](VkBool32 VkPhysicalDeviceDescriptorIndexingFeatures::*m, const VulkanCapabilityState &s) {
                    if (s.supported && s.requested) {
                        descriptorIndexing_.*m = VK_TRUE;
                        return true;
                    }
                    return false;
                };
                bool any = false;
                any |= setDI(&VkPhysicalDeviceDescriptorIndexingFeatures::shaderUniformBufferArrayNonUniformIndexing, requestedCaps.descriptorIndexing.shaderUniformBufferArrayNonUniformIndexing);
                any |= setDI(&VkPhysicalDeviceDescriptorIndexingFeatures::shaderSampledImageArrayNonUniformIndexing, requestedCaps.descriptorIndexing.shaderSampledImageArrayNonUniformIndexing);
                any |= setDI(&VkPhysicalDeviceDescriptorIndexingFeatures::shaderStorageBufferArrayNonUniformIndexing, requestedCaps.descriptorIndexing.shaderStorageBufferArrayNonUniformIndexing);
                any |= setDI(&VkPhysicalDeviceDescriptorIndexingFeatures::shaderStorageImageArrayNonUniformIndexing, requestedCaps.descriptorIndexing.shaderStorageImageArrayNonUniformIndexing);
                any |= setDI(&VkPhysicalDeviceDescriptorIndexingFeatures::descriptorBindingUniformBufferUpdateAfterBind, requestedCaps.descriptorIndexing.descriptorBindingUniformBufferUpdateAfterBind);
                any |= setDI(&VkPhysicalDeviceDescriptorIndexingFeatures::descriptorBindingSampledImageUpdateAfterBind, requestedCaps.descriptorIndexing.descriptorBindingSampledImageUpdateAfterBind);
                any |= setDI(&VkPhysicalDeviceDescriptorIndexingFeatures::descriptorBindingStorageImageUpdateAfterBind, requestedCaps.descriptorIndexing.descriptorBindingStorageImageUpdateAfterBind);
                any |= setDI(&VkPhysicalDeviceDescriptorIndexingFeatures::descriptorBindingStorageBufferUpdateAfterBind, requestedCaps.descriptorIndexing.descriptorBindingStorageBufferUpdateAfterBind);
                any |= setDI(&VkPhysicalDeviceDescriptorIndexingFeatures::descriptorBindingPartiallyBound, requestedCaps.descriptorIndexing.descriptorBindingPartiallyBound);
                any |= setDI(&VkPhysicalDeviceDescriptorIndexingFeatures::descriptorBindingVariableDescriptorCount, requestedCaps.descriptorIndexing.descriptorBindingVariableDescriptorCount);
                any |= setDI(&VkPhysicalDeviceDescriptorIndexingFeatures::runtimeDescriptorArray, requestedCaps.descriptorIndexing.runtimeDescriptorArray);
                if (any) attach(VulkanFeatureChainMember::DESCRIPTOR_INDEXING_EXT, &descriptorIndexing_);
            }
            if (actuallyEnabled.khrSeparateDepthStencilLayouts && q(requestedCaps.separateDepthStencilLayouts) && want(requestedCaps.separateDepthStencilLayouts)) {
                separateDepthStencilLayouts_.separateDepthStencilLayouts = VK_TRUE;
                attach(VulkanFeatureChainMember::SEPARATE_DEPTH_STENCIL_KHR, &separateDepthStencilLayouts_);
            }
        }

        if (tier_ >= VK_API_VERSION_1_3) {
            VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            if (q(requestedCaps.synchronization2) && want(requestedCaps.synchronization2)) v13.synchronization2 = VK_TRUE;
            if (q(requestedCaps.dynamicRendering) && want(requestedCaps.dynamicRendering)) v13.dynamicRendering = VK_TRUE;
            if (q(requestedCaps.maintenance4) && want(requestedCaps.maintenance4)) v13.maintenance4 = VK_TRUE;
            if (v13.synchronization2 || v13.dynamicRendering || v13.maintenance4) {
                vulkan13_ = v13;
                attach(VulkanFeatureChainMember::VULKAN_13_FEATURES, &vulkan13_);
            }
        } else {
            // 1.1/1.2: these are KHR extensions - REQUIRE the real enabled flag.
            if (actuallyEnabled.khrSynchronization2 && q(requestedCaps.synchronization2) && want(requestedCaps.synchronization2)) {
                synchronization2_.synchronization2 = VK_TRUE;
                attach(VulkanFeatureChainMember::SYNCHRONIZATION2_KHR, &synchronization2_);
            }
            if (actuallyEnabled.khrDynamicRendering && q(requestedCaps.dynamicRendering) && want(requestedCaps.dynamicRendering)) {
                dynamicRendering_.dynamicRendering = VK_TRUE;
                attach(VulkanFeatureChainMember::DYNAMIC_RENDERING_KHR, &dynamicRendering_);
            }
            if (actuallyEnabled.khrMaintenance4 && q(requestedCaps.maintenance4) && want(requestedCaps.maintenance4)) {
                maintenance4_.maintenance4 = VK_TRUE;
                attach(VulkanFeatureChainMember::MAINTENANCE4_KHR, &maintenance4_);
            }
        }

        if (actuallyEnabled.khrFragmentShadingRate) {
            bool any = false;
            if (q(requestedCaps.fragmentShadingRateDetail.pipelineFragmentShadingRate) && want(requestedCaps.fragmentShadingRateDetail.pipelineFragmentShadingRate)) {
                fragmentShadingRate_.pipelineFragmentShadingRate = VK_TRUE;
                any = true;
            }
            if (q(requestedCaps.fragmentShadingRateDetail.primitiveFragmentShadingRate) && want(requestedCaps.fragmentShadingRateDetail.primitiveFragmentShadingRate)) {
                fragmentShadingRate_.primitiveFragmentShadingRate = VK_TRUE;
                any = true;
            }
            if (q(requestedCaps.fragmentShadingRateDetail.attachmentFragmentShadingRate) && want(requestedCaps.fragmentShadingRateDetail.attachmentFragmentShadingRate)) {
                fragmentShadingRate_.attachmentFragmentShadingRate = VK_TRUE;
                any = true;
            }
            if (any) attach(VulkanFeatureChainMember::FRAGMENT_SHADING_RATE_KHR, &fragmentShadingRate_);
        }
        if (actuallyEnabled.extGraphicsPipelineLibrary && q(requestedCaps.graphicsPipelineLibrary) && want(requestedCaps.graphicsPipelineLibrary)) {
            graphicsPipelineLibrary_.graphicsPipelineLibrary = VK_TRUE;
            attach(VulkanFeatureChainMember::GRAPHICS_PIPELINE_LIBRARY_EXT, &graphicsPipelineLibrary_);
        }

        finishBuild_();
        return true;
    }

    // Finalize caps.*.enabled from what THIS enable object actually put into
    // the chain with VK_TRUE bits. Only a built ENABLE object is valid
    // evidence: a query-role or never-built object clears enabled like a
    // failed creation. Each state is written EXACTLY ONCE via its routed
    // source (aggregate XOR independent) - no second pass may overwrite it.
    void finalizeEnabled(GfxVKCapabilities &caps, bool creationSucceeded) const {
        if (!creationSucceeded || !enableRole_ ||
            !attached_[static_cast<size_t>(VulkanFeatureChainMember::BASE_FEATURES2)]) {
            markDeviceCreationFailed(caps);
            return;
        }
        const auto fin = [](VulkanCapabilityState &s, bool inChainTrue) {
            s.enabled = inChainTrue && s.supported && s.requested;
        };
        const bool via12 = attached_[static_cast<size_t>(VulkanFeatureChainMember::VULKAN_12_FEATURES)];
        if (via12) {
            fin(caps.timelineSemaphore, vulkan12_.timelineSemaphore == VK_TRUE);
            fin(caps.bufferDeviceAddress, vulkan12_.bufferDeviceAddress == VK_TRUE);
            fin(caps.separateDepthStencilLayouts, vulkan12_.separateDepthStencilLayouts == VK_TRUE);
        } else {
            fin(caps.timelineSemaphore, timelineSemaphore_.timelineSemaphore == VK_TRUE && attached_[static_cast<size_t>(VulkanFeatureChainMember::TIMELINE_SEMAPHORE_KHR)]);
            fin(caps.bufferDeviceAddress, bufferDeviceAddress_.bufferDeviceAddress == VK_TRUE && attached_[static_cast<size_t>(VulkanFeatureChainMember::BUFFER_DEVICE_ADDRESS_KHR)]);
            fin(caps.separateDepthStencilLayouts, separateDepthStencilLayouts_.separateDepthStencilLayouts == VK_TRUE && attached_[static_cast<size_t>(VulkanFeatureChainMember::SEPARATE_DEPTH_STENCIL_KHR)]);
        }
        const auto finDI = [&](VkBool32 VkPhysicalDeviceVulkan12Features::*m12, VkBool32 VkPhysicalDeviceDescriptorIndexingFeatures::*mExt, VulkanCapabilityState &s) {
            const bool on = via12 ? (vulkan12_.*m12 == VK_TRUE)
                                  : (attached_[static_cast<size_t>(VulkanFeatureChainMember::DESCRIPTOR_INDEXING_EXT)] && descriptorIndexing_.*mExt == VK_TRUE);
            fin(s, on);
        };
        finDI(&VkPhysicalDeviceVulkan12Features::shaderUniformBufferArrayNonUniformIndexing, &VkPhysicalDeviceDescriptorIndexingFeatures::shaderUniformBufferArrayNonUniformIndexing, caps.descriptorIndexing.shaderUniformBufferArrayNonUniformIndexing);
        finDI(&VkPhysicalDeviceVulkan12Features::shaderSampledImageArrayNonUniformIndexing, &VkPhysicalDeviceDescriptorIndexingFeatures::shaderSampledImageArrayNonUniformIndexing, caps.descriptorIndexing.shaderSampledImageArrayNonUniformIndexing);
        finDI(&VkPhysicalDeviceVulkan12Features::shaderStorageBufferArrayNonUniformIndexing, &VkPhysicalDeviceDescriptorIndexingFeatures::shaderStorageBufferArrayNonUniformIndexing, caps.descriptorIndexing.shaderStorageBufferArrayNonUniformIndexing);
        finDI(&VkPhysicalDeviceVulkan12Features::shaderStorageImageArrayNonUniformIndexing, &VkPhysicalDeviceDescriptorIndexingFeatures::shaderStorageImageArrayNonUniformIndexing, caps.descriptorIndexing.shaderStorageImageArrayNonUniformIndexing);
        finDI(&VkPhysicalDeviceVulkan12Features::descriptorBindingUniformBufferUpdateAfterBind, &VkPhysicalDeviceDescriptorIndexingFeatures::descriptorBindingUniformBufferUpdateAfterBind, caps.descriptorIndexing.descriptorBindingUniformBufferUpdateAfterBind);
        finDI(&VkPhysicalDeviceVulkan12Features::descriptorBindingSampledImageUpdateAfterBind, &VkPhysicalDeviceDescriptorIndexingFeatures::descriptorBindingSampledImageUpdateAfterBind, caps.descriptorIndexing.descriptorBindingSampledImageUpdateAfterBind);
        finDI(&VkPhysicalDeviceVulkan12Features::descriptorBindingStorageImageUpdateAfterBind, &VkPhysicalDeviceDescriptorIndexingFeatures::descriptorBindingStorageImageUpdateAfterBind, caps.descriptorIndexing.descriptorBindingStorageImageUpdateAfterBind);
        finDI(&VkPhysicalDeviceVulkan12Features::descriptorBindingStorageBufferUpdateAfterBind, &VkPhysicalDeviceDescriptorIndexingFeatures::descriptorBindingStorageBufferUpdateAfterBind, caps.descriptorIndexing.descriptorBindingStorageBufferUpdateAfterBind);
        finDI(&VkPhysicalDeviceVulkan12Features::descriptorBindingPartiallyBound, &VkPhysicalDeviceDescriptorIndexingFeatures::descriptorBindingPartiallyBound, caps.descriptorIndexing.descriptorBindingPartiallyBound);
        finDI(&VkPhysicalDeviceVulkan12Features::descriptorBindingVariableDescriptorCount, &VkPhysicalDeviceDescriptorIndexingFeatures::descriptorBindingVariableDescriptorCount, caps.descriptorIndexing.descriptorBindingVariableDescriptorCount);
        finDI(&VkPhysicalDeviceVulkan12Features::runtimeDescriptorArray, &VkPhysicalDeviceDescriptorIndexingFeatures::runtimeDescriptorArray, caps.descriptorIndexing.runtimeDescriptorArray);

        if (attached_[static_cast<size_t>(VulkanFeatureChainMember::VULKAN_13_FEATURES)]) {
            fin(caps.synchronization2, vulkan13_.synchronization2 == VK_TRUE);
            fin(caps.dynamicRendering, vulkan13_.dynamicRendering == VK_TRUE);
            fin(caps.maintenance4, vulkan13_.maintenance4 == VK_TRUE);
        } else {
            fin(caps.synchronization2, synchronization2_.synchronization2 == VK_TRUE && attached_[static_cast<size_t>(VulkanFeatureChainMember::SYNCHRONIZATION2_KHR)]);
            fin(caps.dynamicRendering, dynamicRendering_.dynamicRendering == VK_TRUE && attached_[static_cast<size_t>(VulkanFeatureChainMember::DYNAMIC_RENDERING_KHR)]);
            fin(caps.maintenance4, maintenance4_.maintenance4 == VK_TRUE && attached_[static_cast<size_t>(VulkanFeatureChainMember::MAINTENANCE4_KHR)]);
        }
        fin(caps.fragmentShadingRateDetail.pipelineFragmentShadingRate, fragmentShadingRate_.pipelineFragmentShadingRate == VK_TRUE && attached_[static_cast<size_t>(VulkanFeatureChainMember::FRAGMENT_SHADING_RATE_KHR)]);
        fin(caps.fragmentShadingRateDetail.primitiveFragmentShadingRate, fragmentShadingRate_.primitiveFragmentShadingRate == VK_TRUE && attached_[static_cast<size_t>(VulkanFeatureChainMember::FRAGMENT_SHADING_RATE_KHR)]);
        fin(caps.fragmentShadingRateDetail.attachmentFragmentShadingRate, fragmentShadingRate_.attachmentFragmentShadingRate == VK_TRUE && attached_[static_cast<size_t>(VulkanFeatureChainMember::FRAGMENT_SHADING_RATE_KHR)]);
        caps.fragmentShadingRate.enabled =
            caps.fragmentShadingRateDetail.pipelineFragmentShadingRate.enabled ||
            caps.fragmentShadingRateDetail.primitiveFragmentShadingRate.enabled ||
            caps.fragmentShadingRateDetail.attachmentFragmentShadingRate.enabled;
        // memoryBudget: no feature struct - the REAL enabled extension list decides.
        fin(caps.memoryBudget, actuallyEnabled_.extMemoryBudget);
        fin(caps.graphicsPipelineLibrary, graphicsPipelineLibrary_.graphicsPipelineLibrary == VK_TRUE && attached_[static_cast<size_t>(VulkanFeatureChainMember::GRAPHICS_PIPELINE_LIBRARY_EXT)]);
    }

    // ---- introspection --------------------------------------------------------
    bool attached(VulkanFeatureChainMember m) const { return attached_[static_cast<size_t>(m)]; }
    uint32_t tier() const { return tier_; }
    bool isEnableRole() const { return enableRole_; }

    // Chain head for vkGetPhysicalDeviceFeatures2 / VkDeviceCreateInfo::pNext.
    VkPhysicalDeviceFeatures2 *head() { return &features2_; }
    const VkPhysicalDeviceFeatures2 *head() const { return &features2_; }

    // Owned member access (results after query; inputs after buildEnable).
    VkPhysicalDeviceTimelineSemaphoreFeaturesKHR *timelineSemaphoreKHR() { return &timelineSemaphore_; }
    VkPhysicalDeviceBufferDeviceAddressFeaturesKHR *bufferDeviceAddressKHR() { return &bufferDeviceAddress_; }
    VkPhysicalDeviceDescriptorIndexingFeatures *descriptorIndexingEXT() { return &descriptorIndexing_; }
    VkPhysicalDeviceSeparateDepthStencilLayoutsFeaturesKHR *separateDepthStencilLayoutsKHR() { return &separateDepthStencilLayouts_; }
    VkPhysicalDeviceSynchronization2FeaturesKHR *synchronization2KHR() { return &synchronization2_; }
    VkPhysicalDeviceDynamicRenderingFeaturesKHR *dynamicRenderingKHR() { return &dynamicRendering_; }
    VkPhysicalDeviceMaintenance4FeaturesKHR *maintenance4KHR() { return &maintenance4_; }
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR *fragmentShadingRateKHR() { return &fragmentShadingRate_; }
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT *graphicsPipelineLibraryEXT() { return &graphicsPipelineLibrary_; }
    VkPhysicalDeviceVulkan11Features *vulkan11Features() { return &vulkan11_; }
    VkPhysicalDeviceVulkan12Features *vulkan12Features() { return &vulkan12_; }
    VkPhysicalDeviceVulkan13Features *vulkan13Features() { return &vulkan13_; }
    const VkPhysicalDeviceFeatures2 *features2() const { return &features2_; }

private:
    static uint32_t clampTier_(uint32_t v) {
        if (v > VK_API_VERSION_1_3) return VK_API_VERSION_1_3; // 1.4 chains not modeled
        return v;
    }
    bool bit_(VulkanFeatureChainMember m, VkBool32 b) const {
        return attached_[static_cast<size_t>(m)] && b == VK_TRUE;
    }
    void attachBase_() {
        attached_[static_cast<size_t>(VulkanFeatureChainMember::BASE_FEATURES2)] = true;
    }
    void attach(VulkanFeatureChainMember m, void *structBase) {
        attached_[static_cast<size_t>(m)] = true;
        pending_.push(structBase);
    }
    // reset(): zero every struct and attachment state. buildQuery/buildEnable
    // record attachments via attach() and MUST call finishBuild_() at the end,
    // which links the chain in deterministic order and clears the pending
    // list - repeated rebuilds can never leak stale pNext pointers or bits.
    void reset() {
        zeroStruct_(features2_, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
        zeroStruct_(timelineSemaphore_, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES);
        zeroStruct_(bufferDeviceAddress_, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES);
        zeroStruct_(descriptorIndexing_, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES);
        zeroStruct_(separateDepthStencilLayouts_, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES);
        zeroStruct_(synchronization2_, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES);
        zeroStruct_(dynamicRendering_, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES);
        zeroStruct_(maintenance4_, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES);
        zeroStruct_(fragmentShadingRate_, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR);
        zeroStruct_(graphicsPipelineLibrary_, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT);
        zeroStruct_(vulkan11_, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
        zeroStruct_(vulkan12_, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
        zeroStruct_(vulkan13_, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
        for (size_t i = 0; i < static_cast<size_t>(VulkanFeatureChainMember::COUNT); ++i) {
            attached_[i] = false;
        }
        pending_.clear();
        features2_.pNext = nullptr;
    }
    template <typename T>
    void zeroStruct_(T &s, VkStructureType type) {
        s = {};
        s.sType = type;
    }
    // Slots are recorded in attach order; chain them now, tail = nullptr.
    struct SlotStack {
        void *items[16] = {};
        size_t count = 0;
        void push(void *base) {
            if (count < 16) items[count++] = base;
        }
        void clear() { count = 0; }
    };
    SlotStack pending_;

    void linkChain_() {
        // pNext must point at the STRUCT START (its sType), never at the
        // pNext field itself - so we record struct base addresses.
        VkBaseOutStructure *prev = reinterpret_cast<VkBaseOutStructure *>(&features2_);
        for (size_t i = 0; i < pending_.count; ++i) {
            prev->pNext = reinterpret_cast<VkBaseOutStructure *>(pending_.items[i]);
            prev = prev->pNext;
        }
        prev->pNext = nullptr;
    }
    void finishBuild_() {
        linkChain_();
        pending_.clear();
    }

    uint32_t tier_{0};
    bool enableRole_{false};
    VulkanFeatureExtensionFlags available_{};
    VulkanFeatureExtensionFlags actuallyEnabled_{};
    bool attached_[static_cast<size_t>(VulkanFeatureChainMember::COUNT)] = {};

    VkPhysicalDeviceFeatures2 features2_{};
    VkPhysicalDeviceTimelineSemaphoreFeaturesKHR timelineSemaphore_{};
    VkPhysicalDeviceBufferDeviceAddressFeaturesKHR bufferDeviceAddress_{};
    VkPhysicalDeviceDescriptorIndexingFeatures descriptorIndexing_{};
    VkPhysicalDeviceSeparateDepthStencilLayoutsFeaturesKHR separateDepthStencilLayouts_{};
    VkPhysicalDeviceSynchronization2FeaturesKHR synchronization2_{};
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamicRendering_{};
    VkPhysicalDeviceMaintenance4FeaturesKHR maintenance4_{};
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR fragmentShadingRate_{};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT graphicsPipelineLibrary_{};
    VkPhysicalDeviceVulkan11Features vulkan11_{};
    VkPhysicalDeviceVulkan12Features vulkan12_{};
    VkPhysicalDeviceVulkan13Features vulkan13_{};
};

// Properties query chain (SEPARATE from feature chains; never mixed into
// features/enable pNext): VkPhysicalDeviceProperties2 with
// VkPhysicalDeviceMaintenance4Properties attached only when the tier is
// >= 1.3 core or VK_KHR_maintenance4 is available.
class VulkanPropertyQueryChain {
public:
    VulkanPropertyQueryChain() { reset(); }
    VulkanPropertyQueryChain(const VulkanPropertyQueryChain &) = delete;
    VulkanPropertyQueryChain &operator=(const VulkanPropertyQueryChain &) = delete;
    VulkanPropertyQueryChain(VulkanPropertyQueryChain &&) = delete;
    VulkanPropertyQueryChain &operator=(VulkanPropertyQueryChain &&) = delete;

    bool buildQuery(uint32_t effectiveApiVersion, const VulkanFeatureExtensionFlags &available) {
        reset();
        const uint32_t tier = effectiveApiVersion > VK_API_VERSION_1_3 ? VK_API_VERSION_1_3 : effectiveApiVersion;
        if (tier < VK_API_VERSION_1_1) return false;
        maintenance4Attached_ = tier >= VK_API_VERSION_1_3 || available.khrMaintenance4;
        if (maintenance4Attached_) {
            properties2_.pNext = &maintenance4Properties_;
        }
        return true;
    }

    // Caller runs vkGetPhysicalDeviceProperties2(physicalDevice, chain()).
    // Records the correctness limit into caps. NOTE: consuming maxBufferSize
    // must check caps.maintenance4.enabled; vkGetDeviceBufferMemoryRequirements
    // has its own independent entry conditions.
    void readSupported(GfxVKCapabilities &caps) const {
        caps.maxBufferSize = maintenance4Attached_ ? maintenance4Properties_.maxBufferSize : 0;
    }

    VkPhysicalDeviceProperties2 *chain() { return &properties2_; }
    const VkPhysicalDeviceProperties2 *chain() const { return &properties2_; }
    bool maintenance4Attached() const { return maintenance4Attached_; }

private:
    void reset() {
        maintenance4Properties_ = {};
        maintenance4Properties_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES;
        properties2_ = {};
        properties2_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        maintenance4Attached_ = false;
    }
    bool maintenance4Attached_{false};
    VkPhysicalDeviceProperties2 properties2_{};
    VkPhysicalDeviceMaintenance4Properties maintenance4Properties_{};
};

} // namespace gfx
} // namespace cc
