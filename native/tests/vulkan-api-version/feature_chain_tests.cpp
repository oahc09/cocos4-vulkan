// Vulkan feature query / enable chain builder contract tests (M1-B).
//
// These are SYNTHETIC structural/contract tests over feature snapshots: they
// walk real VkBaseOutStructure pNext chains, check sType sets/duplicates/tail,
// bit-level enable policy and finalize semantics. They are NOT proofs against
// a real GPU/driver or Validation layers.
//
// Covered plan rules (sections 4/5/17 + VUID pNext-02829/02830/06532):
//   - canonical tiering, no duplicate promoted structs
//   - 1.1 never gets VkPhysicalDeviceVulkan11Features
//   - 1.3 core features need no extension names
//   - available-but-not-enabled extension => supported only, not in enable
//     chain, not enabled
//   - unrequested/unsupported bits stay VK_FALSE (no aggregate whole-copy)
//   - per-member descriptor indexing / FSR partial support
//   - device creation failure clears all enabled
//   - rebuild leaks no stale pNext/bits
//   - properties chain is separate
//   - owning type is non-copyable/non-movable

#include "VKFeatureChain.h"

#include <cstdio>
#include <type_traits>
#include <vector>

namespace {

unsigned g_failures = 0;

void check(bool condition, const char *name) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", name);
        ++g_failures;
    }
}

// Walk the real pNext chain starting at head; returns sType list.
std::vector<VkStructureType> walkChain(const void *pNextHead, bool *tailIsNullptr) {
    std::vector<VkStructureType> types;
    const VkBaseOutStructure *node = static_cast<const VkBaseOutStructure *>(pNextHead);
    while (node != nullptr) {
        types.push_back(node->sType);
        node = node->pNext;
    }
    *tailIsNullptr = true; // loop ended at nullptr
    return types;
}

bool contains(const std::vector<VkStructureType> &v, VkStructureType t) {
    for (VkStructureType e : v) {
        if (e == t) return true;
    }
    return false;
}

size_t countOf(const std::vector<VkStructureType> &v, VkStructureType t) {
    size_t n = 0;
    for (VkStructureType e : v) {
        if (e == t) ++n;
    }
    return n;
}

bool allUnique(const std::vector<VkStructureType> &v) {
    for (size_t i = 0; i < v.size(); ++i) {
        for (size_t j = i + 1; j < v.size(); ++j) {
            if (v[i] == v[j]) return false;
        }
    }
    return true;
}

cc::gfx::VulkanFeatureExtensionFlags allAvailable() {
    cc::gfx::VulkanFeatureExtensionFlags f;
    f.khrTimelineSemaphore = true;
    f.khrBufferDeviceAddress = true;
    f.extDescriptorIndexing = true;
    f.khrSeparateDepthStencilLayouts = true;
    f.khrSynchronization2 = true;
    f.khrDynamicRendering = true;
    f.khrMaintenance4 = true;
    f.khrFragmentShadingRate = true;
    f.extMemoryBudget = true;
    f.extGraphicsPipelineLibrary = true;
    return f;
}

void requestAllModeled(cc::gfx::GfxVKCapabilities &caps) {
    using cc::gfx::VulkanCapabilityState;
    VulkanCapabilityState *states[] = {
        &caps.timelineSemaphore, &caps.bufferDeviceAddress, &caps.separateDepthStencilLayouts,
        &caps.synchronization2, &caps.dynamicRendering, &caps.maintenance4,
        &caps.fragmentShadingRateDetail.pipelineFragmentShadingRate,
        &caps.fragmentShadingRateDetail.primitiveFragmentShadingRate,
        &caps.fragmentShadingRateDetail.attachmentFragmentShadingRate,
        &caps.memoryBudget, &caps.graphicsPipelineLibrary,
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
        s->requested = true;
    }
}

} // namespace

int main() {
    using namespace cc::gfx;
    using ST = VkStructureType;
    const ST sF2 = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    const ST sTimeline = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    const ST sBDA = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    const ST sDI = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    const ST sSDSL = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES;
    const ST sSync2 = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
    const ST sDyn = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    const ST sM4 = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES;
    const ST sFSR = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR;
    const ST sGPL = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT;
    const ST sV11 = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    const ST sV12 = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    const ST sV13 = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

        // --- owning type is non-copyable / non-movable ----------------------------
    static_assert(!std::is_copy_constructible<VulkanFeatureChain>::value &&
                      !std::is_copy_assignable<VulkanFeatureChain>::value &&
                      !std::is_move_constructible<VulkanFeatureChain>::value &&
                      !std::is_move_assignable<VulkanFeatureChain>::value,
                  "VulkanFeatureChain must be non-copyable and non-movable");
    static_assert(!std::is_copy_constructible<VulkanPropertyQueryChain>::value &&
                      !std::is_move_constructible<VulkanPropertyQueryChain>::value,
                  "VulkanPropertyQueryChain must be non-copyable and non-movable");

    // --- tier below baseline is rejected ---------------------------------------
    {
        VulkanFeatureChain q;
        check(!q.buildQuery(VK_API_VERSION_1_0, allAvailable()), "1.0 tier rejected by buildQuery");
        bool tail = false;
        check(walkChain(q.head()->pNext, &tail).empty() && tail, "1.0 tier leaves empty chain");
        VulkanFeatureChain e;
        GfxVKCapabilities caps;
        check(!e.buildEnable(q, caps, VkPhysicalDeviceFeatures{}, allAvailable()),
              "enable refuses on unbuilt/1.0 query chain");
    }

    // --- 1.1, no extensions: FEATURES_2 only, never Vulkan11Features -----------
    {
        VulkanFeatureChain q;
        check(q.buildQuery(VK_API_VERSION_1_1, VulkanFeatureExtensionFlags{}), "1.1 empty-ext buildQuery");
        bool tail = false;
        const auto chain = walkChain(q.head()->pNext, &tail);
        check(chain.empty() && tail, "1.1 empty-ext: only FEATURES_2 (no pNext members)");
        check(!q.attached(VulkanFeatureChainMember::VULKAN_11_FEATURES),
              "1.1 never attaches VkPhysicalDeviceVulkan11Features");
    }

    // --- 1.1 with optional extensions: independent structs, no aggregates ------
    {
        VulkanFeatureChain q;
        check(q.buildQuery(VK_API_VERSION_1_1, allAvailable()), "1.1 full-ext buildQuery");
        bool tail = false;
        const auto chain = walkChain(q.head()->pNext, &tail);
        check(tail && allUnique(chain), "1.1 full-ext: chain unique, tail nullptr");
        check(contains(chain, sTimeline) && contains(chain, sBDA) && contains(chain, sDI) &&
                  contains(chain, sSDSL) && contains(chain, sSync2) && contains(chain, sDyn) &&
                  contains(chain, sM4) && contains(chain, sFSR) && contains(chain, sGPL),
              "1.1 full-ext: all independent structs present");
        check(!contains(chain, sV11) && !contains(chain, sV12) && !contains(chain, sV13),
              "1.1 full-ext: no aggregates");

        // simulate query results, read back supported
        q.timelineSemaphoreKHR()->timelineSemaphore = VK_TRUE;
        q.descriptorIndexingEXT()->descriptorBindingPartiallyBound = VK_TRUE;
        q.synchronization2KHR()->synchronization2 = VK_TRUE;
        GfxVKCapabilities caps;
        q.readSupported(caps);
        check(caps.timelineSemaphore.supported && caps.descriptorIndexing.descriptorBindingPartiallyBound.supported &&
                  caps.synchronization2.supported, "1.1 readSupported maps independent bits");
        check(!caps.descriptorIndexing.runtimeDescriptorArray.supported && !caps.dynamicRendering.supported,
              "1.1 unqueried/unset bits stay unsupported");
    }

    // --- 1.2: Vulkan11+Vulkan12 aggregates, no promoted independents -----------
    {
        VulkanFeatureChain q;
        check(q.buildQuery(VK_API_VERSION_1_2, allAvailable()), "1.2 buildQuery");
        bool tail = false;
        const auto chain = walkChain(q.head()->pNext, &tail);
        check(tail && allUnique(chain), "1.2: chain unique");
        check(contains(chain, sV11) && contains(chain, sV12), "1.2: aggregates attached");
        check(!contains(chain, sTimeline) && !contains(chain, sBDA) && !contains(chain, sDI) && !contains(chain, sSDSL),
              "1.2: no promoted independent structs (no duplicates of aggregate members)");
        check(contains(chain, sSync2) && contains(chain, sDyn) && contains(chain, sM4),
              "1.2: 1.3-core features stay independent KHR structs when available");

        q.vulkan12Features()->timelineSemaphore = VK_TRUE;
        q.vulkan12Features()->runtimeDescriptorArray = VK_TRUE;
        q.synchronization2KHR()->synchronization2 = VK_TRUE;
        GfxVKCapabilities caps;
        q.readSupported(caps);
        check(caps.timelineSemaphore.supported && caps.descriptorIndexing.runtimeDescriptorArray.supported &&
                  caps.synchronization2.supported, "1.2 readSupported maps aggregate bits");
    }

    // --- 1.3 without KHR extension names: core features from Vulkan13 ----------
    {
        VulkanFeatureExtensionFlags none; // no sync2/dynamic/m4 names
        none.khrTimelineSemaphore = true; // promoted ones don't matter on 1.3 anyway
        VulkanFeatureChain q;
        check(q.buildQuery(VK_API_VERSION_1_3, none), "1.3 no-KHR-names buildQuery");
        bool tail = false;
        const auto chain = walkChain(q.head()->pNext, &tail);
        check(contains(chain, sV13), "1.3: Vulkan13 aggregate attached");
        check(!contains(chain, sSync2) && !contains(chain, sDyn) && !contains(chain, sM4),
              "1.3: no independent sync2/dynamic/m4 structs");
        q.vulkan13Features()->synchronization2 = VK_TRUE;
        q.vulkan13Features()->dynamicRendering = VK_TRUE;
        q.vulkan13Features()->maintenance4 = VK_TRUE;
        GfxVKCapabilities caps;
        q.readSupported(caps);
        check(caps.synchronization2.supported && caps.dynamicRendering.supported && caps.maintenance4.supported,
              "1.3 core features supported without any extension names");

        // enable on 1.3 with ALL actuallyEnabled flags false: core still enables
        requestAllModeled(caps);
        caps.dynamicRendering.supported = caps.synchronization2.supported = caps.maintenance4.supported = true;
        VulkanFeatureChain e;
        check(e.buildEnable(q, caps, VkPhysicalDeviceFeatures{}, VulkanFeatureExtensionFlags{}), "1.3 buildEnable");
        e.finalizeEnabled(caps, true);
        check(caps.synchronization2.enabled && caps.dynamicRendering.enabled && caps.maintenance4.enabled,
              "1.3 core enable requires no extension flags");
    }

    // --- 1.4 input capped to 1.3 chain -----------------------------------------
    {
        VulkanFeatureChain q;
        check(q.buildQuery(VK_MAKE_VERSION(1, 4, 0), VulkanFeatureExtensionFlags{}), "1.4 input accepted (capped)");
        check(q.tier() == VK_API_VERSION_1_3, "1.4 input clamps to 1.3 chain");
        bool tail = false;
        const auto chain = walkChain(q.head()->pNext, &tail);
        check(contains(chain, sV13) && !contains(chain, sSync2), "1.4-capped chain equals 1.3 shape");
    }

    // --- available but NOT actually enabled: supported only ---------------------
    {
        VulkanFeatureChain q;
        check(q.buildQuery(VK_API_VERSION_1_2, allAvailable()), "available-not-enabled setup");
        q.vulkan12Features()->timelineSemaphore = VK_TRUE;
        q.synchronization2KHR()->synchronization2 = VK_TRUE;
        GfxVKCapabilities caps;
        q.readSupported(caps);
        requestAllModeled(caps);
        caps.timelineSemaphore.supported = caps.synchronization2.supported = true;

        VulkanFeatureExtensionFlags enabledNow; // extension list of THIS create call
        enabledNow.khrTimelineSemaphore = true;  // 1.2: promoted, flag irrelevant but harmless
        // khrSynchronization2 deliberately NOT enabled
        VulkanFeatureChain e;
        check(e.buildEnable(q, caps, VkPhysicalDeviceFeatures{}, enabledNow), "buildEnable without sync2 ext");
        bool tail = false;
        const auto chain = walkChain(e.head()->pNext, &tail);
        check(!contains(chain, sSync2), "sync2 struct not attached without actually-enabled flag");
        e.finalizeEnabled(caps, /*creationSucceeded=*/true);
        check(caps.synchronization2.supported && caps.synchronization2.requested && !caps.synchronization2.enabled,
              "available-but-not-enabled extension: supported+requested but NOT enabled");
        check(!caps.timelineSemaphore.enabled || e.attached(VulkanFeatureChainMember::VULKAN_12_FEATURES),
              "timeline on 1.2 goes through the aggregate");
    }

    // --- unrequested / unsupported bits stay VK_FALSE ---------------------------
    {
        VulkanFeatureChain q;
        check(q.buildQuery(VK_API_VERSION_1_2, allAvailable()), "bit-policy setup");
        auto *v12 = q.vulkan12Features();
        v12->descriptorBindingPartiallyBound = VK_TRUE;       // only this supported
        v12->runtimeDescriptorArray = VK_TRUE;                 // supported but will be unrequested
        GfxVKCapabilities caps;
        q.readSupported(caps);
        caps.descriptorIndexing.descriptorBindingPartiallyBound.requested = true; // the only request
        VulkanFeatureChain e;
        check(e.buildEnable(q, caps, VkPhysicalDeviceFeatures{}, allAvailable()), "bit-policy buildEnable");
        check(e.attached(VulkanFeatureChainMember::VULKAN_12_FEATURES), "aggregate attached (one member on)");
        const auto &built = *e.vulkan12Features();
        check(built.descriptorBindingPartiallyBound == VK_TRUE, "requested+supported member is VK_TRUE");
        check(built.runtimeDescriptorArray == VK_FALSE, "unrequested member stays VK_FALSE");
        check(built.descriptorBindingVariableDescriptorCount == VK_FALSE, "unsupported member stays VK_FALSE");
        check(built.timelineSemaphore == VK_FALSE && built.shaderUniformBufferArrayNonUniformIndexing == VK_FALSE,
              "no aggregate whole-copy");
        e.finalizeEnabled(caps, true);
        check(caps.descriptorIndexing.descriptorBindingPartiallyBound.enabled &&
                  !caps.descriptorIndexing.runtimeDescriptorArray.enabled,
              "finalize: only supported+requested member enabled");
    }

    // --- baseFeatures passthrough (no expansion) --------------------------------
    {
        VulkanFeatureChain q;
        q.buildQuery(VK_API_VERSION_1_1, VulkanFeatureExtensionFlags{});
        GfxVKCapabilities caps;
        VkPhysicalDeviceFeatures base{};
        base.samplerAnisotropy = VK_TRUE;
        VulkanFeatureChain e;
        e.buildEnable(q, caps, base, VulkanFeatureExtensionFlags{});
        check(e.head()->features.samplerAnisotropy == VK_TRUE && e.head()->features.multiDrawIndirect == VK_FALSE,
              "baseFeatures passed through verbatim");
    }

    // --- FSR partial support: pipeline only -------------------------------------
    {
        VulkanFeatureChain q;
        VulkanFeatureExtensionFlags av;
        av.khrFragmentShadingRate = true;
        check(q.buildQuery(VK_API_VERSION_1_1, av), "FSR setup");
        q.fragmentShadingRateKHR()->pipelineFragmentShadingRate = VK_TRUE; // pipeline only
        GfxVKCapabilities caps;
        q.readSupported(caps);
        check(caps.fragmentShadingRateDetail.pipelineFragmentShadingRate.supported &&
                  !caps.fragmentShadingRateDetail.primitiveFragmentShadingRate.supported &&
                  !caps.fragmentShadingRateDetail.attachmentFragmentShadingRate.supported,
              "FSR: only pipeline supported");
        check(caps.fragmentShadingRate.supported, "FSR family summary = any");
        requestAllModeled(caps); // request all three
        VulkanFeatureExtensionFlags en;
        en.khrFragmentShadingRate = true;
        VulkanFeatureChain e;
        e.buildEnable(q, caps, VkPhysicalDeviceFeatures{}, en);
        check(e.attached(VulkanFeatureChainMember::FRAGMENT_SHADING_RATE_KHR), "FSR struct attached");
        check(e.fragmentShadingRateKHR()->pipelineFragmentShadingRate == VK_TRUE &&
                  e.fragmentShadingRateKHR()->primitiveFragmentShadingRate == VK_FALSE &&
                  e.fragmentShadingRateKHR()->attachmentFragmentShadingRate == VK_FALSE,
              "FSR enable: pipeline bit only (unsupported members stay VK_FALSE)");
        e.finalizeEnabled(caps, true);
        check(caps.fragmentShadingRateDetail.pipelineFragmentShadingRate.enabled &&
                  !caps.fragmentShadingRateDetail.primitiveFragmentShadingRate.enabled,
              "FSR finalize: pipeline enabled, primitive not (no impersonation)");
        check(caps.fragmentShadingRate.enabled, "FSR family summary enabled = any");
    }

    // --- memoryBudget: enabled follows the REAL extension list -------------------
    {
        VulkanFeatureChain q;
        VulkanFeatureExtensionFlags av;
        av.extMemoryBudget = true;
        q.buildQuery(VK_API_VERSION_1_1, av);
        GfxVKCapabilities caps;
        q.readSupported(caps);
        check(caps.memoryBudget.supported, "memoryBudget supported = extension available");
        caps.memoryBudget.requested = true;
        VulkanFeatureChain e; // create call WITHOUT the extension
        e.buildEnable(q, caps, VkPhysicalDeviceFeatures{}, VulkanFeatureExtensionFlags{});
        e.finalizeEnabled(caps, true);
        check(caps.memoryBudget.supported && caps.memoryBudget.requested && !caps.memoryBudget.enabled,
              "memoryBudget not enabled when extension not in the real create list");
        VulkanFeatureChain e2; // create call WITH the extension
        VulkanFeatureExtensionFlags en;
        en.extMemoryBudget = true;
        e2.buildEnable(q, caps, VkPhysicalDeviceFeatures{}, en);
        e2.finalizeEnabled(caps, true);
        check(caps.memoryBudget.enabled, "memoryBudget enabled when extension really enabled");
    }

    // --- device creation failure clears enabled, keeps supported/requested -------
    {
        VulkanFeatureChain q;
        check(q.buildQuery(VK_API_VERSION_1_3, allAvailable()), "failure-path setup");
        q.vulkan13Features()->synchronization2 = VK_TRUE;
        q.vulkan13Features()->maintenance4 = VK_TRUE;
        GfxVKCapabilities caps;
        q.readSupported(caps);
        requestAllModeled(caps);
        VulkanFeatureChain e;
        e.buildEnable(q, caps, VkPhysicalDeviceFeatures{}, VulkanFeatureExtensionFlags{});
        e.finalizeEnabled(caps, /*creationSucceeded=*/false);
        check(!caps.synchronization2.enabled && !caps.maintenance4.enabled &&
                  !caps.descriptorIndexing.runtimeDescriptorArray.enabled,
              "creation failure clears every enabled");
        check(caps.synchronization2.supported && caps.synchronization2.requested &&
                  caps.version.effectiveDeviceApiVersion == VK_API_VERSION_1_0 /* untouched by this builder */,
              "creation failure keeps supported/requested/version untouched");
    }

    // --- rebuild leaks no stale pNext / bits --------------------------------------
    {
        VulkanFeatureChain q;
        q.buildQuery(VK_API_VERSION_1_2, allAvailable());
        q.vulkan12Features()->timelineSemaphore = VK_TRUE;
        q.buildQuery(VK_API_VERSION_1_1, VulkanFeatureExtensionFlags{}); // rebuild smaller
        bool tail = false;
        const auto chain = walkChain(q.head()->pNext, &tail);
        check(chain.empty() && tail, "rebuild: stale members dropped");
        check(q.vulkan12Features()->timelineSemaphore == VK_FALSE, "rebuild: old bits zeroed");
        GfxVKCapabilities caps;
        q.readSupported(caps);
        check(!caps.timelineSemaphore.supported, "rebuild: supported not leaked from previous build");

        // enable object rebuild
        q.buildQuery(VK_API_VERSION_1_1, allAvailable());
        q.synchronization2KHR()->synchronization2 = VK_TRUE;
        q.readSupported(caps);
        requestAllModeled(caps);
        caps.synchronization2.supported = true;
        VulkanFeatureChain e;
        e.buildEnable(q, caps, VkPhysicalDeviceFeatures{}, allAvailable());
        check(e.attached(VulkanFeatureChainMember::SYNCHRONIZATION2_KHR), "enable built");
        e.buildEnable(q, caps, VkPhysicalDeviceFeatures{}, VulkanFeatureExtensionFlags{}); // rebuild w/o ext
        const auto chain2 = walkChain(e.head()->pNext, &tail);
        check(!contains(chain2, sSync2) && e.synchronization2KHR()->synchronization2 == VK_FALSE,
              "enable rebuild: struct dropped and bit zeroed");
    }

    // --- properties chain separate -----------------------------------------------
    {
        VulkanPropertyQueryChain p;
        VulkanFeatureExtensionFlags none;
        check(p.buildQuery(VK_API_VERSION_1_1, none), "props 1.1 no m4");
        check(!p.maintenance4Attached(), "1.1 without KHR_maintenance4: no m4 properties");
        check(p.buildQuery(VK_API_VERSION_1_3, none), "props 1.3");
        check(p.maintenance4Attached(), "1.3 core: m4 properties attached");
        bool tail = false;
        const auto chain = walkChain(p.chain()->pNext, &tail);
        check(chain.size() == 1 && chain[0] == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES && tail,
              "props chain is exactly Properties2 -> Maintenance4Properties");

        // simulate vkGetPhysicalDeviceProperties2 fill: const_cast is not needed
        // for a test-only write through the exposed chain head.
        auto *m4 = reinterpret_cast<VkPhysicalDeviceMaintenance4Properties *>(p.chain()->pNext);
        m4->maxBufferSize = 4096ull * 1024 * 1024;
        GfxVKCapabilities caps;
        p.readSupported(caps);
        check(caps.maxBufferSize == m4->maxBufferSize, "maxBufferSize recorded from properties");
        check(!caps.maintenance4.enabled, "consuming the limit still requires maintenance4.enabled");
    }

    // =========================================================================
    // M1-B revision tests (Lead review findings; written RED-first)
    // =========================================================================

    // --- rev1: 1.1 independent route must survive finalize (defect: the
    //     unconditional via12 pass overwrote enabled back to false) ----------
    {
        VulkanFeatureChain q;
        VulkanFeatureExtensionFlags av;
        av.khrTimelineSemaphore = true;
        av.khrBufferDeviceAddress = true;
        av.khrSeparateDepthStencilLayouts = true;
        check(q.buildQuery(VK_API_VERSION_1_1, av), "rev1 setup");
        q.timelineSemaphoreKHR()->timelineSemaphore = VK_TRUE;
        q.bufferDeviceAddressKHR()->bufferDeviceAddress = VK_TRUE;
        q.separateDepthStencilLayoutsKHR()->separateDepthStencilLayouts = VK_TRUE;
        GfxVKCapabilities caps;
        q.readSupported(caps);
        caps.timelineSemaphore.requested = true;
        caps.bufferDeviceAddress.requested = true;
        caps.separateDepthStencilLayouts.requested = true;
        VulkanFeatureChain e;
        check(e.buildEnable(q, caps, VkPhysicalDeviceFeatures{}, av), "rev1 buildEnable");
        e.finalizeEnabled(caps, /*creationSucceeded=*/true);
        check(caps.timelineSemaphore.enabled, "rev1: 1.1 timeline stays enabled after finalize");
        check(caps.bufferDeviceAddress.enabled, "rev1: 1.1 BDA stays enabled after finalize");
        check(caps.separateDepthStencilLayouts.enabled, "rev1: 1.1 SDSL stays enabled after finalize");
        check(caps.timelineSemaphore.supported && caps.timelineSemaphore.requested &&
                  caps.version.effectiveDeviceApiVersion == VK_API_VERSION_1_0,
              "rev1: supported/requested/version preserved");

        // 1.2 aggregate contrast: same features via the Vulkan12 aggregate
        VulkanFeatureChain q2;
        check(q2.buildQuery(VK_API_VERSION_1_2, av), "rev1 contrast setup");
        q2.vulkan12Features()->timelineSemaphore = VK_TRUE;
        q2.vulkan12Features()->bufferDeviceAddress = VK_TRUE;
        q2.vulkan12Features()->separateDepthStencilLayouts = VK_TRUE;
        GfxVKCapabilities caps2;
        q2.readSupported(caps2);
        caps2.timelineSemaphore.requested = caps2.bufferDeviceAddress.requested = caps2.separateDepthStencilLayouts.requested = true;
        VulkanFeatureChain e2;
        check(e2.buildEnable(q2, caps2, VkPhysicalDeviceFeatures{}, VulkanFeatureExtensionFlags{}), "rev1 contrast buildEnable");
        e2.finalizeEnabled(caps2, true);
        check(caps2.timelineSemaphore.enabled && caps2.bufferDeviceAddress.enabled &&
                  caps2.separateDepthStencilLayouts.enabled,
              "rev1: 1.2 aggregate route still enables");
    }

    // --- rev2: stale caller caps.supported must NOT substitute for the
    //     query real bits; unqueried extensions never enable -----------------
    {
        VulkanFeatureChain q;
        VulkanFeatureExtensionFlags noneAvail; // no extension available
        check(q.buildQuery(VK_API_VERSION_1_1, noneAvail), "rev2 setup");
        GfxVKCapabilities caps;
        q.readSupported(caps); // everything legitimately unsupported

        // caller fakes stale supported + requested for core/descriptor/FSR/GPL
        caps.timelineSemaphore.supported = true;
        caps.timelineSemaphore.requested = true;
        caps.synchronization2.supported = true;
        caps.synchronization2.requested = true;
        caps.descriptorIndexing.descriptorBindingPartiallyBound.supported = true;
        caps.descriptorIndexing.descriptorBindingPartiallyBound.requested = true;
        caps.fragmentShadingRateDetail.pipelineFragmentShadingRate.supported = true;
        caps.fragmentShadingRateDetail.pipelineFragmentShadingRate.requested = true;
        caps.graphicsPipelineLibrary.supported = true;
        caps.graphicsPipelineLibrary.requested = true;

        // caller ALSO wrongly claims the extensions were actually enabled
        VulkanFeatureExtensionFlags wrongEnabled;
        wrongEnabled.khrTimelineSemaphore = true;
        wrongEnabled.khrSynchronization2 = true;
        wrongEnabled.extDescriptorIndexing = true;
        wrongEnabled.khrFragmentShadingRate = true;
        wrongEnabled.extGraphicsPipelineLibrary = true;

        VulkanFeatureChain e;
        check(e.buildEnable(q, caps, VkPhysicalDeviceFeatures{}, wrongEnabled), "rev2 buildEnable");
        const bool anyTrue =
            e.timelineSemaphoreKHR()->timelineSemaphore == VK_TRUE ||
            e.synchronization2KHR()->synchronization2 == VK_TRUE ||
            e.descriptorIndexingEXT()->descriptorBindingPartiallyBound == VK_TRUE ||
            e.fragmentShadingRateKHR()->pipelineFragmentShadingRate == VK_TRUE ||
            e.graphicsPipelineLibraryEXT()->graphicsPipelineLibrary == VK_TRUE ||
            e.vulkan12Features()->timelineSemaphore == VK_TRUE ||
            e.vulkan13Features()->synchronization2 == VK_TRUE;
        check(!anyTrue, "rev2: stale supported never turns into a VK_TRUE bit");
        e.finalizeEnabled(caps, true);
        check(!caps.timelineSemaphore.enabled && !caps.synchronization2.enabled &&
                  !caps.descriptorIndexing.descriptorBindingPartiallyBound.enabled &&
                  !caps.fragmentShadingRateDetail.pipelineFragmentShadingRate.enabled &&
                  !caps.graphicsPipelineLibrary.enabled,
              "rev2: nothing enabled from stale caps / unqueried extensions");
    }

    // --- rev3: finalize guards (query-role / never-built / failed-build) ----
    {
        // (a) QUERY object mistakenly used as enable evidence
        VulkanFeatureChain q;
        VulkanFeatureExtensionFlags av;
        av.khrTimelineSemaphore = true;
        av.khrSynchronization2 = true;
        q.buildQuery(VK_API_VERSION_1_1, av);
        q.timelineSemaphoreKHR()->timelineSemaphore = VK_TRUE;
        q.synchronization2KHR()->synchronization2 = VK_TRUE;
        GfxVKCapabilities caps;
        q.readSupported(caps);
        caps.timelineSemaphore.requested = true;
        caps.synchronization2.requested = true;
        caps.timelineSemaphore.enabled = true; // pre-existing state
        caps.synchronization2.enabled = true;
        q.finalizeEnabled(caps, /*creationSucceeded=*/true);
        check(!caps.timelineSemaphore.enabled && !caps.synchronization2.enabled,
              "rev3a: query-role finalize clears enabled instead of enabling");

        // (b) never-built object
        VulkanFeatureChain neverBuilt;
        caps.timelineSemaphore.enabled = true;
        neverBuilt.finalizeEnabled(caps, true);
        check(!caps.timelineSemaphore.enabled, "rev3b: never-built finalize clears enabled");

        // (c) failed buildEnable (query not built) then finalize
        VulkanFeatureChain unbuiltQuery;
        GfxVKCapabilities capsC;
        capsC.timelineSemaphore.requested = true;
        VulkanFeatureChain bad;
        check(!bad.buildEnable(unbuiltQuery, capsC, VkPhysicalDeviceFeatures{}, VulkanFeatureExtensionFlags{}),
              "rev3c: buildEnable rejects unbuilt query");
        capsC.timelineSemaphore.enabled = true;
        bad.finalizeEnabled(capsC, true);
        check(!capsC.timelineSemaphore.enabled, "rev3c: failed-build finalize clears enabled");

        // (d) enable rebuild must not resurrect old states
        VulkanFeatureChain qd;
        VulkanFeatureExtensionFlags avd;
        avd.khrTimelineSemaphore = true;
        qd.buildQuery(VK_API_VERSION_1_1, avd);
        qd.timelineSemaphoreKHR()->timelineSemaphore = VK_TRUE;
        GfxVKCapabilities capsD;
        qd.readSupported(capsD);
        capsD.timelineSemaphore.requested = true;
        VulkanFeatureChain ed;
        ed.buildEnable(qd, capsD, VkPhysicalDeviceFeatures{}, avd);
        ed.finalizeEnabled(capsD, true);
        check(capsD.timelineSemaphore.enabled, "rev3d setup: first device enabled timeline");
        ed.buildEnable(qd, capsD, VkPhysicalDeviceFeatures{}, VulkanFeatureExtensionFlags{}); // ext gone
        ed.finalizeEnabled(capsD, true);
        check(!capsD.timelineSemaphore.enabled, "rev3d: rebuild without extension must not keep/resurrect enabled");
    }

    if (g_failures == 0) {
        std::printf("feature chain contract tests: all PASS\n");
        return 0;
    }
    std::printf("feature chain contract tests: %u FAIL\n", g_failures);
    return 1;
}
