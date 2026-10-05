/****************************************************************************
 Copyright (c) 2020-2023 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do so,
 subject to the following conditions:

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

#include "VKGPUObjects.h"
#include "application/ApplicationManager.h"
#include "platform/interfaces/modules/IXRInterface.h"
#include <cstdlib>

namespace cc {
namespace gfx {

namespace {


#ifndef CC_VULKAN_VALIDATION
    #define CC_VULKAN_VALIDATION (CC_DEBUG > 0)
#endif

using ccstd::vector;

#if CC_VULKAN_VALIDATION
constexpr uint32_t DISABLE_VALIDATION_ASSERTIONS = 1; // 0 for default behavior, otherwise assertions will be disabled
VKAPI_ATTR VkBool32 VKAPI_CALL debugUtilsMessengerCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                                                           VkDebugUtilsMessageTypeFlagsEXT /*messageType*/,
                                                           const VkDebugUtilsMessengerCallbackDataEXT *callbackData,
                                                           void *userData) {
    if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        static_cast<CCVKGPUContext *>(userData)->validationErrorCount.fetch_add(1, std::memory_order_relaxed);
        CC_LOG_ERROR("%s: %s", callbackData->pMessageIdName, callbackData->pMessage);
        CC_ASSERT(DISABLE_VALIDATION_ASSERTIONS);
        return VK_FALSE;
    }
    if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        CC_LOG_WARNING("%s: %s", callbackData->pMessageIdName, callbackData->pMessage);
        return VK_FALSE;
    }
    if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        // CC_LOG_INFO("%s: %s", callbackData->pMessageIdName, callbackData->pMessage);
        return VK_FALSE;
    }
    if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) {
        // CC_LOG_DEBUG("%s: %s", callbackData->pMessageIdName, callbackData->pMessage);
        return VK_FALSE;
    }
    CC_LOG_ERROR("%s: %s", callbackData->pMessageIdName, callbackData->pMessage);
    return VK_FALSE;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugReportCallback(VkDebugReportFlagsEXT flags,
                                                   VkDebugReportObjectTypeEXT /*type*/,
                                                   uint64_t /*object*/,
                                                   size_t /*location*/,
                                                   int32_t /*messageCode*/,
                                                   const char *layerPrefix,
                                                   const char *message,
                                                   void *userData) {
    if (flags & VK_DEBUG_REPORT_ERROR_BIT_EXT) {
        static_cast<CCVKGPUContext *>(userData)->validationErrorCount.fetch_add(1, std::memory_order_relaxed);
        CC_LOG_ERROR("%s: %s", layerPrefix, message);
        CC_ASSERT(DISABLE_VALIDATION_ASSERTIONS);
        return VK_FALSE;
    }
    if (flags & (VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT)) {
        CC_LOG_WARNING("%s: %s", layerPrefix, message);
        return VK_FALSE;
    }
    if (flags & VK_DEBUG_REPORT_INFORMATION_BIT_EXT) {
        // CC_LOG_INFO("%s: %s", layerPrefix, message);
        return VK_FALSE;
    }
    if (flags & VK_DEBUG_REPORT_DEBUG_BIT_EXT) {
        // CC_LOG_DEBUG("%s: %s", layerPrefix, message);
        return VK_FALSE;
    }
    CC_LOG_ERROR("%s: %s", layerPrefix, message);
    return VK_FALSE;
}
#endif
} // namespace

bool CCVKGPUContext::initialize() {
    // only enable the absolute essentials
    ccstd::vector<const char *> requestedLayers{
        //"VK_LAYER_KHRONOS_synchronization2",
    };
    ccstd::vector<const char *> requestedExtensions{
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
        VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME,
    };

    ///////////////////// Instance Creation /////////////////////

    if (volkInitialize()) {
        return false;
    }

    uint32_t loaderVersion = VK_API_VERSION_1_0;
    const VkResult versionResult = queryVulkanLoaderVersion(vkEnumerateInstanceVersion, &loaderVersion);
    if (versionResult != VK_SUCCESS) {
        CC_LOG_ERROR("Querying Vulkan loader version failed: %d", versionResult);
        return false;
    }
    apiVersionInfo.setLoaderVersion(loaderVersion);
    if (loaderVersion < BACKEND_BASELINE_API_VERSION) {
        CC_LOG_ERROR("Vulkan backend requires loader API >= 1.1, reported 0x%x", loaderVersion);
        return false;
    }
#if CC_DEBUG > 0
    // Development-only version cap for compatibility validation on modern GPUs.
    if (const char *cap = std::getenv("CC_VULKAN_FORCE_API")) {
        uint32_t capVersion = 0;
        if (std::strcmp(cap, "1.1") == 0) capVersion = VK_API_VERSION_1_1;
        if (std::strcmp(cap, "1.2") == 0) capVersion = VK_API_VERSION_1_2;
        if (std::strcmp(cap, "1.3") == 0) capVersion = VK_API_VERSION_1_3;
        if (!capVersion) {
            CC_LOG_ERROR("Invalid CC_VULKAN_FORCE_API: %s (expected 1.1, 1.2, or 1.3)", cap);
            return false;
        }
        apiVersionInfo.engineRequestedApiVersion = std::min(apiVersionInfo.engineRequestedApiVersion, capVersion);
    }
#endif

    IXRInterface *xr = CC_GET_XR_INTERFACE();
    apiVersionInfo.constrainInstanceVersion(xr ? xr->getXRVkApiVersion(apiVersionInfo.engineRequestedApiVersion)
                                              : apiVersionInfo.engineRequestedApiVersion);
    if (VK_API_VERSION_VARIANT(apiVersionInfo.instanceApiVersion) != 0 ||
        apiVersionInfo.instanceApiVersion < BACKEND_BASELINE_API_VERSION) {
        CC_LOG_ERROR("Vulkan backend requires instance/XR API >= 1.1, XR returned 0x%x", apiVersionInfo.xrConstrainedApiVersion);
        return false;
    }
    minorVersion = VK_VERSION_MINOR(apiVersionInfo.instanceApiVersion);

    uint32_t availableLayerCount;
    VK_CHECK(vkEnumerateInstanceLayerProperties(&availableLayerCount, nullptr));
    ccstd::vector<VkLayerProperties> supportedLayers(availableLayerCount);
    VK_CHECK(vkEnumerateInstanceLayerProperties(&availableLayerCount, supportedLayers.data()));

    uint32_t availableExtensionCount;
    VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &availableExtensionCount, nullptr));
    ccstd::vector<VkExtensionProperties> supportedExtensions(availableExtensionCount);
    VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &availableExtensionCount, supportedExtensions.data()));

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
    requestedExtensions.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_WIN32_KHR)
    requestedExtensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_VI_NN)
    requestedExtensions.push_back(VK_NN_VI_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_MACOS_MVK)
    requestedExtensions.push_back(VK_MVK_MACOS_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
    requestedExtensions.push_back(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_XCB_KHR)
    requestedExtensions.push_back(VK_KHR_XCB_SURFACE_EXTENSION_NAME);
#else
    #pragma error Platform not supported
#endif

    if (isExtensionSupported(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME, supportedExtensions)) {
        requestedExtensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    }

#if CC_VULKAN_VALIDATION
    // Determine the optimal validation layers to enable that are necessary for useful debugging
    ccstd::vector<ccstd::vector<const char *>> validationLayerPriorityList{
        // The preferred validation layer is "VK_LAYER_KHRONOS_validation"
        {"VK_LAYER_KHRONOS_validation"},

        // Otherwise we fallback to using the LunarG meta layer
        {"VK_LAYER_LUNARG_standard_validation"},

        // Otherwise we attempt to enable the individual layers that compose the LunarG meta layer since it doesn't exist
        {
            "VK_LAYER_GOOGLE_threading",
            "VK_LAYER_LUNARG_parameter_validation",
            "VK_LAYER_LUNARG_object_tracker",
            "VK_LAYER_LUNARG_core_validation",
            "VK_LAYER_GOOGLE_unique_objects",
        },

        // Otherwise as a last resort we fallback to attempting to enable the LunarG core layer
        {"VK_LAYER_LUNARG_core_validation"},
    };
    for (ccstd::vector<const char *> &validationLayers : validationLayerPriorityList) {
        bool found = true;
        for (const char *layer : validationLayers) {
            if (!isLayerSupported(layer, supportedLayers)) {
                found = false;
                break;
            }
        }
        if (found) {
            requestedLayers.insert(requestedLayers.end(), validationLayers.begin(), validationLayers.end());
            break;
        }
    }
#endif

#if CC_DEBUG
    // Check if VK_EXT_debug_utils is supported, which supersedes VK_EXT_Debug_Report
    if (isExtensionSupported(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, supportedExtensions)) {
        debugUtils = true;
        requestedExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    } else {
        debugReport = isExtensionSupported(VK_EXT_DEBUG_REPORT_EXTENSION_NAME, supportedExtensions);
        requestedExtensions.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
    }
#endif

    // just filter out the unsupported layers & extensions
    for (const char *layer : requestedLayers) {
        if (isLayerSupported(layer, supportedLayers)) {
            layers.push_back(layer);
        }
    }
    for (const char *extension : requestedExtensions) {
        if (isExtensionSupported(extension, supportedExtensions)) {
            extensions.push_back(extension);
        }
    }

#if CC_VULKAN_VALIDATION
    validationEnabled = !layers.empty();
    bool validationFeaturesAvailable = false;
    if (checkExtension(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)) {
        validationFeaturesAvailable = true;
    } else if (isLayerSupported("VK_LAYER_KHRONOS_validation", supportedLayers) && validationEnabled) {
        uint32_t count = 0;
        VkResult result = vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &count, nullptr);
        if (result == VK_SUCCESS) {
            ccstd::vector<VkExtensionProperties> layerExtensions(count);
            result = vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &count, layerExtensions.data());
            if (result == VK_SUCCESS && isExtensionSupported(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME, layerExtensions)) {
                extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
                validationFeaturesAvailable = true;
            }
        }
    }
    if (!validationEnabled) {
        CC_LOG_WARNING("Vulkan validation unavailable; development validation is not verified.");
    }
#endif

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pEngineName = "Cocos Creator";
    app.apiVersion = apiVersionInfo.instanceApiVersion;

    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    if (checkExtension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        instanceInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = utils::toUint(extensions.size());
    instanceInfo.ppEnabledExtensionNames = extensions.data();
    instanceInfo.enabledLayerCount = utils::toUint(layers.size());
    instanceInfo.ppEnabledLayerNames = layers.data();

#if CC_VULKAN_VALIDATION
    VkDebugUtilsMessengerCreateInfoEXT debugUtilsCreateInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    VkDebugReportCallbackCreateInfoEXT debugReportCreateInfo{VK_STRUCTURE_TYPE_DEBUG_REPORT_CREATE_INFO_EXT};
    if (validationEnabled && debugUtils) {
        debugUtilsCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
        debugUtilsCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugUtilsCreateInfo.pfnUserCallback = debugUtilsMessengerCallback;
        debugUtilsCreateInfo.pUserData = this;

        instanceInfo.pNext = &debugUtilsCreateInfo;
    } else if (validationEnabled && debugReport) {
        debugReportCreateInfo.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT |
                                      VK_DEBUG_REPORT_WARNING_BIT_EXT |
                                      VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT |
                                      VK_DEBUG_REPORT_INFORMATION_BIT_EXT |
                                      VK_DEBUG_REPORT_DEBUG_BIT_EXT;
        debugReportCreateInfo.pfnCallback = debugReportCallback;
        debugReportCreateInfo.pUserData = this;

        instanceInfo.pNext = &debugReportCreateInfo;
    }
    const VkValidationFeatureEnableEXT enabledValidationFeatures[]{
        VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT,
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
    };
    VkValidationFeaturesEXT validationFeatures{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    if (validationEnabled && validationFeaturesAvailable) {
        validationFeatures.enabledValidationFeatureCount = utils::toUint(std::size(enabledValidationFeatures));
        validationFeatures.pEnabledValidationFeatures = enabledValidationFeatures;
        validationFeatures.pNext = instanceInfo.pNext;
        instanceInfo.pNext = &validationFeatures;
        synchronizationValidationEnabled = true;
        bestPracticesValidationEnabled = true;
    } else if (validationEnabled) {
        CC_LOG_WARNING("VK_EXT_validation_features unavailable; synchronization/best-practices validation is not verified.");
    }
#endif

    // Create the Vulkan instance
    if (xr) {
        xr->initializeVulkanData(vkGetInstanceProcAddr);
        vkInstance = xr->createXRVulkanInstance(instanceInfo);
    } else {
        VkResult res = vkCreateInstance(&instanceInfo, nullptr, &vkInstance);
        if (res != VK_SUCCESS) {
            CC_LOG_ERROR("Create Vulkan instance failed: %d", res);
            return false;
        }
    }
    if (vkInstance == VK_NULL_HANDLE) {
        CC_LOG_ERROR("Vulkan instance creation returned a null handle.");
        return false;
    }
    volkLoadInstanceOnly(vkInstance);
    if (!vkGetPhysicalDeviceFeatures2 || !vkGetPhysicalDeviceProperties2) {
        CC_LOG_ERROR("Vulkan 1.1 physical-device query entries are missing.");
        destroy();
        return false;
    }

#if CC_VULKAN_VALIDATION
    VkResult messengerResult = VK_SUCCESS;
    if (validationEnabled && debugUtils && vkCreateDebugUtilsMessengerEXT) {
        messengerResult = vkCreateDebugUtilsMessengerEXT(vkInstance, &debugUtilsCreateInfo, nullptr, &vkDebugUtilsMessenger);
    } else if (validationEnabled && debugReport && vkCreateDebugReportCallbackEXT) {
        messengerResult = vkCreateDebugReportCallbackEXT(vkInstance, &debugReportCreateInfo, nullptr, &vkDebugReport);
    }
    if (messengerResult != VK_SUCCESS) {
        CC_LOG_ERROR("Vulkan validation messenger creation failed: %d", messengerResult);
        destroy();
        return false;
    }
#endif

    ///////////////////// Physical Device Selection /////////////////////

    // Querying valid physical devices on the machine
    uint32_t physicalDeviceCount{0};
    VkResult res = vkEnumeratePhysicalDevices(vkInstance, &physicalDeviceCount, nullptr);

    if (res || physicalDeviceCount < 1) {
        destroy();
        return false;
    }

    ccstd::vector<VkPhysicalDevice> physicalDeviceHandles;
    if (xr) {
        // XR owns device selection; never inspect unfilled entries for other GPUs.
        physicalDeviceCount = 1;
        physicalDeviceHandles.push_back(xr->getXRVulkanGraphicsDevice());
        if (physicalDeviceHandles[0] == VK_NULL_HANDLE) {
            CC_LOG_ERROR("XR runtime returned a null Vulkan physical device.");
            destroy();
            return false;
        }
    } else {
        physicalDeviceHandles.resize(physicalDeviceCount);
        res = vkEnumeratePhysicalDevices(vkInstance, &physicalDeviceCount, physicalDeviceHandles.data());
        if (res != VK_SUCCESS || physicalDeviceCount == 0) {
            destroy();
            return false;
        }
    }

    ccstd::vector<VkPhysicalDeviceProperties> physicalDevicePropertiesList(physicalDeviceCount);

    uint32_t deviceIndex;
    for (deviceIndex = 0U; deviceIndex < physicalDeviceCount; ++deviceIndex) {
        VkPhysicalDeviceProperties &properties = physicalDevicePropertiesList[deviceIndex];
        vkGetPhysicalDeviceProperties(physicalDeviceHandles[deviceIndex], &properties);
    }

    for (deviceIndex = 0U; deviceIndex < physicalDeviceCount; ++deviceIndex) {
        VkPhysicalDeviceProperties &properties = physicalDevicePropertiesList[deviceIndex];
        if (properties.apiVersion >= BACKEND_BASELINE_API_VERSION && properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            break;
        }
    }

    if (deviceIndex == physicalDeviceCount) {
        for (deviceIndex = 0; deviceIndex < physicalDeviceCount; ++deviceIndex) {
            if (physicalDevicePropertiesList[deviceIndex].apiVersion >= BACKEND_BASELINE_API_VERSION) break;
        }
    }
    if (deviceIndex == physicalDeviceCount) {
        CC_LOG_ERROR("No Vulkan physical device meets the API 1.1 baseline.");
        destroy();
        return false;
    }

    physicalDevice = physicalDeviceHandles[deviceIndex];
    physicalDeviceProperties = physicalDevicePropertiesList[deviceIndex];

    apiVersionInfo.setPhysicalDeviceVersion(physicalDeviceProperties.apiVersion);
    majorVersion = VK_VERSION_MAJOR(apiVersionInfo.effectiveDeviceApiVersion);
    minorVersion = VK_VERSION_MINOR(apiVersionInfo.effectiveDeviceApiVersion);
    CC_LOG_INFO("Vulkan API: loader=%u.%u requested=%u.%u XR=%u.%u instance=%u.%u physical=%u.%u effective=%u.%u",
                VK_VERSION_MAJOR(apiVersionInfo.loaderApiVersion), VK_VERSION_MINOR(apiVersionInfo.loaderApiVersion),
                VK_VERSION_MAJOR(apiVersionInfo.engineRequestedApiVersion), VK_VERSION_MINOR(apiVersionInfo.engineRequestedApiVersion),
                VK_VERSION_MAJOR(apiVersionInfo.xrConstrainedApiVersion), VK_VERSION_MINOR(apiVersionInfo.xrConstrainedApiVersion),
                VK_VERSION_MAJOR(apiVersionInfo.instanceApiVersion), VK_VERSION_MINOR(apiVersionInfo.instanceApiVersion),
                VK_VERSION_MAJOR(apiVersionInfo.physicalDeviceApiVersion), VK_VERSION_MINOR(apiVersionInfo.physicalDeviceApiVersion),
                majorVersion, minorVersion);

    uint32_t deviceExtensionCount = 0;
    res = vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &deviceExtensionCount, nullptr);
    if (res != VK_SUCCESS) {
        destroy();
        return false;
    }
    ccstd::vector<VkExtensionProperties> deviceExtensions(deviceExtensionCount);
    res = vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &deviceExtensionCount, deviceExtensions.data());
    if (res != VK_SUCCESS) {
        destroy();
        return false;
    }

    capabilities.version = apiVersionInfo;
    capabilities.debugUtils = debugUtils;
    const auto available = getVulkanFeatureExtensionFlags([&deviceExtensions](const char *name) {
        return isExtensionSupported(name, deviceExtensions);
    });
    if (!featureQuery.buildQuery(apiVersionInfo.effectiveDeviceApiVersion, available)) {
        destroy();
        return false;
    }
    vkGetPhysicalDeviceFeatures2(physicalDevice, featureQuery.head());
    featureQuery.readSupported(capabilities);
    physicalDeviceFeatures = featureQuery.features2()->features;

    VulkanPropertyQueryChain propertyQuery;
    propertyQuery.buildQuery(apiVersionInfo.effectiveDeviceApiVersion, available);
    if (minorVersion >= 2 || isExtensionSupported(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME, deviceExtensions)) {
        physicalDeviceDepthStencilResolveProperties.pNext = propertyQuery.chain()->pNext;
        propertyQuery.chain()->pNext = &physicalDeviceDepthStencilResolveProperties;
    }
    vkGetPhysicalDeviceProperties2(physicalDevice, propertyQuery.chain());
    propertyQuery.readSupported(capabilities);
    // The property chain is local; retain only values, never its borrowed links.
    physicalDeviceDepthStencilResolveProperties.pNext = nullptr;

    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &physicalDeviceMemoryProperties);
    uint32_t queueFamilyPropertiesCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyPropertiesCount, nullptr);
    queueFamilyProperties.resize(queueFamilyPropertiesCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyPropertiesCount, queueFamilyProperties.data());
    return true;
}

void CCVKGPUContext::destroy() {
#if CC_VULKAN_VALIDATION
    if (vkDebugUtilsMessenger != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(vkInstance, vkDebugUtilsMessenger, nullptr);
        vkDebugUtilsMessenger = VK_NULL_HANDLE;
    }
    if (vkDebugReport != VK_NULL_HANDLE) {
        vkDestroyDebugReportCallbackEXT(vkInstance, vkDebugReport, nullptr);
        vkDebugReport = VK_NULL_HANDLE;
    }
#endif

    if (vkInstance != VK_NULL_HANDLE) {
        vkDestroyInstance(vkInstance, nullptr);
        vkInstance = VK_NULL_HANDLE;
    }
}

} // namespace gfx
} // namespace cc
