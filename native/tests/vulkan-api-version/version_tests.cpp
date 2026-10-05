#include "VKCapabilities.h"

#include <cstdio>

namespace {
VKAPI_ATTR VkResult VKAPI_CALL report14(uint32_t *version) {
    *version = VK_MAKE_VERSION(1, 4, 351);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL failQuery(uint32_t *version) {
    *version = VK_MAKE_VERSION(1, 4, 351);
    return VK_ERROR_INITIALIZATION_FAILED;
}
} // namespace

int main() {
    using namespace cc::gfx;
    unsigned failures = 0;
    auto check = [&failures](bool condition, const char *name) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", name);
            ++failures;
        }
    };

    uint32_t loader = 0;
    check(queryVulkanLoaderVersion(nullptr, &loader) == VK_SUCCESS && loader == VK_API_VERSION_1_0,
          "missing enumeration entry denotes 1.0");
    check(queryVulkanLoaderVersion(failQuery, &loader) == VK_ERROR_INITIALIZATION_FAILED && loader == VK_API_VERSION_1_0,
          "failed enumeration propagates error and never accepts its output");
    check(queryVulkanLoaderVersion(report14, &loader) == VK_SUCCESS, "enumeration succeeds");

    VulkanApiVersionInfo info;
    info.setLoaderVersion(loader);
    check(info.loaderApiVersion == VK_MAKE_VERSION(1, 4, 351), "keep reported loader version");
    check(info.instanceApiVersion == ENGINE_MAX_API_VERSION, "1.4 loader is capped at engine 1.3");
    info.setPhysicalDeviceVersion(VK_API_VERSION_1_1);
    check(info.effectiveDeviceApiVersion == VK_API_VERSION_1_1 && info.supportsBaseline(),
          "modern loader with 1.1 device retains baseline");

    info.setLoaderVersion(VK_API_VERSION_1_1);
    info.setPhysicalDeviceVersion(VK_MAKE_VERSION(1, 3, 296));
    check(info.effectiveDeviceApiVersion == VK_API_VERSION_1_1, "1.1 instance limits modern device");

    info.setLoaderVersion(VK_MAKE_VERSION(1, 4, 0));
    info.constrainInstanceVersion(VK_API_VERSION_1_2);
    info.setPhysicalDeviceVersion(VK_MAKE_VERSION(1, 3, 296));
    check(info.instanceApiVersion == VK_API_VERSION_1_2 && info.effectiveDeviceApiVersion == VK_API_VERSION_1_2,
          "XR 1.2 constraint reaches device capability decisions");

    info.constrainInstanceVersion(VK_MAKE_VERSION(1, 4, 0));
    info.setPhysicalDeviceVersion(VK_MAKE_VERSION(1, 4, 0));
    check(info.instanceApiVersion == ENGINE_MAX_API_VERSION && info.effectiveDeviceApiVersion == ENGINE_MAX_API_VERSION,
          "XR cannot raise engine contract above 1.3");

    info.constrainInstanceVersion(VK_API_VERSION_1_0);
    info.setPhysicalDeviceVersion(VK_API_VERSION_1_2);
    check(!info.supportsBaseline(), "XR below 1.1 does not satisfy baseline");
    info.setLoaderVersion(VK_API_VERSION_1_2);
    info.setPhysicalDeviceVersion(VK_API_VERSION_1_0);
    check(!info.supportsBaseline(), "1.0 device does not satisfy baseline");

    std::printf("Vulkan version contract: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
