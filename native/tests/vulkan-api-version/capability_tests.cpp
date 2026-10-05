// Capability data-model contract tests (M1-A).
//
// These tests exercise SEMANTIC rules from the plan (sections 4 / 5.4 / 17.2),
// not getter round-trips:
//   - supported / requested / enabled are independent stages
//   - enabled only after: creation succeeded AND feature in enable chain AND
//     requested AND supported
//   - device creation failure disables everything
//   - descriptor-indexing family members stay individual states
//   - runtime path dependency normalization (A/B independence of barrier
//     encoder vs Submit2, forceClassicRenderPass, etc.)
//   - runtime path requests never raise the version model

#include "VKCapabilities.h"

#include <cstdio>

namespace {

unsigned g_failures = 0;

void check(bool condition, const char *name) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", name);
        ++g_failures;
    }
}

cc::gfx::VulkanCapabilityState makeEnabledState() {
    cc::gfx::VulkanCapabilityState s;
    s.supported = true;
    s.requested = true;
    cc::gfx::finalizeFeature(s, /*creationSucceeded=*/true, /*includedInEnableChain=*/true);
    return s;
}

// A capabilities set where every relevant family member is queried+requested
// and the device creation succeeded with everything in the chain.
cc::gfx::GfxVKCapabilities makeFullyEnabledCaps() {
    cc::gfx::GfxVKCapabilities caps;
    cc::gfx::VulkanCapabilityState *states[] = {
        &caps.timelineSemaphore,
        &caps.bufferDeviceAddress,
        &caps.separateDepthStencilLayouts,
        &caps.synchronization2,
        &caps.dynamicRendering,
        &caps.maintenance4,
        &caps.fragmentShadingRate,
        &caps.memoryBudget,
        &caps.graphicsPipelineLibrary,
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
    for (auto *s : states) {
        s->supported = true;
        s->requested = true;
        cc::gfx::finalizeFeature(*s, true, true);
    }
    return caps;
}

} // namespace

int main() {
    using namespace cc::gfx;

    // --- defaults are conservative -------------------------------------------
    {
        GfxVKCapabilities caps;
        VulkanRuntimePaths paths;
        check(!caps.timelineSemaphore.supported && !caps.timelineSemaphore.requested && !caps.timelineSemaphore.enabled,
              "defaults: capability state all false");
        check(!paths.useTimelineSemaphore && !paths.useSynchronization2 && !paths.useSubmit2 &&
                    !paths.useDynamicRendering && !paths.forceClassicRenderPass && !paths.useMaintenance4,
              "defaults: all runtime paths OFF");
        check(caps.maxBufferSize == 0 && !caps.debugUtils && !caps.portabilitySubset,
              "defaults: maxBufferSize 0, debugUtils/portabilitySubset false");
    }

    // --- stage independence: supported but NOT requested ----------------------
    {
        VulkanCapabilityState s;
        s.supported = true;
        requestFeature(s, false);
        finalizeFeature(s, /*creationSucceeded=*/true, /*includedInEnableChain=*/true); // even if chain had it
        check(s.supported && !s.requested && !s.enabled,
              "supported but unrequested feature never enabled (even if chain included it)");
    }

    // --- requested but unsupported --------------------------------------------
    {
        VulkanCapabilityState s;
        s.supported = false;
        requestFeature(s, true);
        finalizeFeature(s, /*creationSucceeded=*/true, /*includedInEnableChain=*/true); // policy mistake reaches chain
        check(!s.supported && s.requested && !s.enabled,
              "requested-but-unsupported feature can never be enabled");
    }

    // --- device creation failure ----------------------------------------------
    {
        VulkanCapabilityState s = makeEnabledState();
        finalizeFeature(s, /*creationSucceeded=*/false, /*includedInEnableChain=*/true);
        check(!s.enabled, "device creation failure clears enabled");

        GfxVKCapabilities caps = makeFullyEnabledCaps();
        markDeviceCreationFailed(caps);
        check(!caps.synchronization2.enabled && !caps.dynamicRendering.enabled &&
                  !caps.descriptorIndexing.descriptorBindingPartiallyBound.enabled,
              "markDeviceCreationFailed disables every family member");
    }

    // --- creation succeeded but feature not in the enable chain ---------------
    {
        VulkanCapabilityState s = makeEnabledState();
        finalizeFeature(s, /*creationSucceeded=*/true, /*includedInEnableChain=*/false);
        check(!s.enabled, "created device without feature in enable chain is not enabled");
    }

    // --- descriptor indexing family: individual member states -----------------
    {
        VulkanDescriptorIndexingCaps di;
        di.descriptorBindingPartiallyBound.supported = true; // only this one
        di.descriptorBindingPartiallyBound.requested = true;
        di.descriptorBindingVariableDescriptorCount.supported = false;
        di.descriptorBindingVariableDescriptorCount.requested = true; // requested anyway
        finalizeFeature(di.descriptorBindingPartiallyBound, true, true);
        finalizeFeature(di.descriptorBindingVariableDescriptorCount, true, true);
        check(di.descriptorBindingPartiallyBound.enabled,
              "descriptor family: supported+requested+chained member enabled");
        check(!di.descriptorBindingVariableDescriptorCount.enabled,
              "descriptor family: unsupported member stays disabled");
        check(!di.runtimeDescriptorArray.supported && !di.runtimeDescriptorArray.requested &&
                  !di.runtimeDescriptorArray.enabled,
              "descriptor family: untouched member stays fully false");
    }

    // --- runtime gates: synchronization2 not enabled ---------------------------
    {
        GfxVKCapabilities caps;
        caps.synchronization2 = makeEnabledState();
        caps.synchronization2.enabled = false; // feature NOT enabled
        VulkanRuntimePaths want;
        want.useSynchronization2 = true;
        want.useSubmit2 = true;
        const VulkanRuntimePaths paths = normalizeRuntimePaths(caps, want);
        check(!paths.useSynchronization2 && !paths.useSubmit2,
              "sync2 not enabled: both barrier encoder and Submit2 rejected");
    }

    // --- A/B independence once synchronization2 enabled ------------------------
    {
        GfxVKCapabilities caps;
        caps.synchronization2 = makeEnabledState();
        VulkanRuntimePaths want;
        want.useSynchronization2 = false; // legacy barrier encoder
        want.useSubmit2 = true;            // Submit2 still on
        VulkanRuntimePaths paths = normalizeRuntimePaths(caps, want);
        check(!paths.useSynchronization2 && paths.useSubmit2,
              "sync2 enabled: legacy barrier + Submit2 is a legal A/B combination");
        want.useSynchronization2 = true;
        want.useSubmit2 = false;
        paths = normalizeRuntimePaths(caps, want);
        check(paths.useSynchronization2 && !paths.useSubmit2,
              "sync2 enabled: Sync2 barriers without Submit2 is legal too");
    }

    // --- dynamic rendering: feature gate + manual override ---------------------
    {
        GfxVKCapabilities caps;
        VulkanRuntimePaths want;
        want.useDynamicRendering = true;

        VulkanRuntimePaths paths = normalizeRuntimePaths(caps, want); // dynamic NOT enabled
        check(!paths.useDynamicRendering, "dynamic rendering not enabled: path off");

        caps.dynamicRendering = makeEnabledState();
        paths = normalizeRuntimePaths(caps, want);
        check(paths.useDynamicRendering, "dynamic rendering enabled: path kept");

        paths = normalizeRuntimePaths(caps, want);
        paths.forceClassicRenderPass = true;
        const VulkanRuntimePaths forced = normalizeRuntimePaths(caps, paths);
        check(!forced.useDynamicRendering, "forceClassicRenderPass blocks dynamic rendering");

        // forceClassic also blocks when the caller never asked for dynamic:
        VulkanRuntimePaths classicWant;
        classicWant.forceClassicRenderPass = true;
        classicWant.useDynamicRendering = true;
        check(!normalizeRuntimePaths(caps, classicWant).useDynamicRendering,
              "forceClassicRenderPass blocks dynamic rendering request directly");
    }

    // --- timeline / maintenance4 gates ----------------------------------------
    {
        GfxVKCapabilities caps;
        VulkanRuntimePaths want;
        want.useTimelineSemaphore = true;
        want.useMaintenance4 = true;
        VulkanRuntimePaths paths = normalizeRuntimePaths(caps, want);
        check(!paths.useTimelineSemaphore && !paths.useMaintenance4,
              "timeline/maintenance4 not enabled: paths off");

        caps.timelineSemaphore = makeEnabledState();
        caps.maintenance4 = makeEnabledState();
        paths = normalizeRuntimePaths(caps, want);
        check(paths.useTimelineSemaphore && paths.useMaintenance4,
              "timeline/maintenance4 enabled: paths kept");
    }

    // --- runtime path requests never raise the version model ------------------
    {
        GfxVKCapabilities caps;
        caps.version.setLoaderVersion(VK_API_VERSION_1_1);
        caps.version.setPhysicalDeviceVersion(VK_API_VERSION_1_1);
        check(caps.version.effectiveDeviceApiVersion == VK_API_VERSION_1_1,
              "1.1 effective version setup");

        VulkanRuntimePaths want; // request every modern path
        want.useTimelineSemaphore = true;
        want.useSynchronization2 = true;
        want.useSubmit2 = true;
        want.useDynamicRendering = true;
        want.useMaintenance4 = true;
        const VulkanRuntimePaths paths = normalizeRuntimePaths(caps, want);
        check(!paths.useSynchronization2 && !paths.useSubmit2 && !paths.useDynamicRendering,
              "1.1 device: modern paths narrowed to off");
        check(caps.version.effectiveDeviceApiVersion == VK_API_VERSION_1_1 &&
                  caps.version.instanceApiVersion == VK_API_VERSION_1_1,
              "1.1 version model NOT raised by modern path requests");

        caps.version.setLoaderVersion(VK_API_VERSION_1_2);
        caps.version.setPhysicalDeviceVersion(VK_API_VERSION_1_2);
        normalizeRuntimePaths(caps, want);
        check(caps.version.effectiveDeviceApiVersion == VK_API_VERSION_1_2,
              "1.2 version model NOT raised by modern path requests");
    }

    // The maintenance4 correctness limit applies to the full padded/back-buffer
    // allocation even while its optional optimization path remains disabled.
    {
        GfxVKCapabilities caps;
        caps.maxBufferSize = 1024;
        check(isVulkanBufferSizeAllowed(caps, 4096), "disabled maintenance4 does not impose its limit");
        caps.maintenance4 = makeEnabledState();
        check(isVulkanBufferSizeAllowed(caps, 1024), "maxBufferSize boundary is inclusive");
        check(!isVulkanBufferSizeAllowed(caps, 1025), "maxBufferSize plus one rejected");
        const VkDeviceSize instanceSize = 512;
        check(!isVulkanBufferSizeAllowed(caps, instanceSize * 3), "padded multi-buffer backing size is checked");
        check(!normalizeRuntimePaths(caps, VulkanRuntimePaths{}).useMaintenance4 &&
                  !isVulkanBufferSizeAllowed(caps, 1025), "optimization off cannot bypass correctness limit");
        caps.maxBufferSize = 0;
        check(!isVulkanBufferSizeAllowed(caps, 1), "enabled maintenance4 with missing limit is conservative");
    }

    if (g_failures == 0) {
        std::printf("capability contract tests: all PASS\n");
        return 0;
    }
    std::printf("capability contract tests: %u FAIL\n", g_failures);
    return 1;
}
