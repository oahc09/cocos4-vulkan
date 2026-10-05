// ============================================================================
// Vulkan loader / instance / device entry-loading smoke (M0-C)
//
// Uses ONLY the newly vendored Vulkan-Headers (1.3.296) + regenerated volk
// (header 296) from native/cocos/renderer/gfx-vulkan/dependencies and
// native/cocos/renderer/gfx-vulkan/volk.c. The old external/sources/khronos
// headers are NOT on the include path for this target.
//
// For each requested instance tier (1.1 / 1.2 / 1.3):
//   - create an instance with apiVersion = tier (headless, no surface ext)
//   - pick first physical device, read its real apiVersion
//   - effective = min(instanceTier, physicalApiVersion)
//   - create a VkDevice with NO features / NO extensions (only a queue)
//   - verify, via volk device/instance tables, that the core entry points
//     REQUIRED for the effective version are loaded (non-NULL)
//   - for effective >= 1.3 the Modern set is exactly:
//       vkQueueSubmit2, vkCmdPipelineBarrier2, vkCmdBeginRendering,
//       vkCmdEndRendering, vkGetDeviceBufferMemoryRequirements,
//       vkGetDeviceImageMemoryRequirements
//
// This smoke NEVER CALLS commands whose device feature was not enabled -
// pointer presence is the only thing asserted.
//
// If the system KHRONOS_validation layer is available it is enabled with a
// debug-utils callback and validation ERRORS make the run FAIL. If the layer
// is missing the run explicitly reports VALIDATION NOT AVAILABLE.
//
// Exit codes: 0 = all tiers passed; 1 = any failure.
// ============================================================================

#include <cstdio>
#include <cstring>
#include <vector>

#include "volk.h"

namespace {

int g_validationErrors = 0;
bool g_validationEnabled = false;

VKAPI_ATTR VkBool32 VKAPI_CALL debugUtilsCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
    void * /*pUserData*/) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++g_validationErrors;
        std::printf("  [validation ERROR] %s\n", pCallbackData->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::printf("  [validation warn ] %s\n", pCallbackData->pMessage);
    }
    return VK_FALSE;
}

struct EntryCheck {
    const char *name;
    void *fn;
};

bool runChecks(const char *label, const EntryCheck *checks, size_t count) {
    bool ok = true;
    for (size_t i = 0; i < count; ++i) {
        if (checks[i].fn == nullptr) {
            std::printf("  [%s] MISSING core entry: %s\n", label, checks[i].name);
            ok = false;
        }
    }
    return ok;
}

#define ENTRY(table, fn) { #fn, reinterpret_cast<void *>((table).fn) }

uint32_t tierApiVersion(int minor) {
    return VK_MAKE_API_VERSION(0, 1, static_cast<uint32_t>(minor), 0);
}

const char *versionString(uint32_t v) {
    static char buf[4][32];
    static int idx = 0;
    char *out = buf[idx];
    idx = (idx + 1) % 4;
    std::snprintf(out, 32, "%u.%u.%u", VK_API_VERSION_MAJOR(v),
                  VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
    return out;
}

} // namespace

int main() {
    std::printf("=== Vulkan loader smoke (vendored Vulkan-Headers 1.3.296 / volk 296) ===\n");

    if (volkInitialize() != VK_SUCCESS) {
        std::printf("FAIL: volkInitialize()\n");
        return 1;
    }
    const uint32_t loaderVersion = volkGetInstanceVersion();
    if (loaderVersion == 0) {
        std::printf("FAIL: loader does not support Vulkan 1.1+ (volkGetInstanceVersion == 0)\n");
        return 1;
    }
    std::printf("loader instance version: %s\n", versionString(loaderVersion));

    // --- validation layer availability (probed once) --------------------------
    bool layerAvailable = false;
    {
        uint32_t count = 0;
        if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS || count == 0) {
            std::printf("FAIL: vkEnumerateInstanceLayerProperties(count)\n");
            return 1;
        }
        std::vector<VkLayerProperties> layers(count);
        if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) {
            std::printf("FAIL: vkEnumerateInstanceLayerProperties(data)\n");
            return 1;
        }
        for (const auto &l : layers) {
            if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                layerAvailable = true;
                break;
            }
        }
    }
    if (!layerAvailable) {
        std::printf("VALIDATION NOT AVAILABLE: VK_LAYER_KHRONOS_validation not installed; "
                    "this smoke run is NOT validated by layers.\n");
    }

    // --- debug utils extension availability (loader + layer provided) ----------
    bool debugUtilsAvailable = false;
    {
        uint32_t count = 0;
        if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) == VK_SUCCESS && count > 0) {
            std::vector<VkExtensionProperties> exts(count);
            if (vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data()) == VK_SUCCESS) {
                for (const auto &e : exts) {
                    if (std::strcmp(e.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
                        debugUtilsAvailable = true;
                    }
                }
            }
        }
    }

    bool allTiersPass = true;
    const int tiers[] = {1, 2, 3};

    for (int tier : tiers) {
        const uint32_t tierVersion = tierApiVersion(tier);
        std::printf("\n--- instance tier: Vulkan 1.%d (apiVersion=0x%08X) ---\n", tier, tierVersion);

        // --- instance ---------------------------------------------------------
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "vulkan_loader_smoke";
        app.pEngineName = "cocos-vk-modernization";
        app.apiVersion = tierVersion;

        const char *layersToEnable[1] = {"VK_LAYER_KHRONOS_validation"};
        const char *extsToEnable[1] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};

        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app;
        if (layerAvailable) {
            ci.enabledLayerCount = 1;
            ci.ppEnabledLayerNames = layersToEnable;
        }
        bool useDebugUtils = layerAvailable && debugUtilsAvailable;
        if (useDebugUtils) {
            ci.enabledExtensionCount = 1;
            ci.ppEnabledExtensionNames = extsToEnable;
        }

        VkInstance instance = VK_NULL_HANDLE;
        VkResult res = vkCreateInstance(&ci, nullptr, &instance);
        if (res != VK_SUCCESS || instance == VK_NULL_HANDLE) {
            std::printf("FAIL: vkCreateInstance(tier 1.%d) -> %d\n", tier, static_cast<int>(res));
            allTiersPass = false;
            continue;
        }
        volkLoadInstance(instance);

        // --- physical device ----------------------------------------------------
        uint32_t pdCount = 0;
        if (vkEnumeratePhysicalDevices(instance, &pdCount, nullptr) != VK_SUCCESS || pdCount == 0) {
            std::printf("FAIL: no physical devices (tier 1.%d)\n", tier);
            vkDestroyInstance(instance, nullptr);
            allTiersPass = false;
            continue;
        }
        std::vector<VkPhysicalDevice> devices(pdCount);
        if (vkEnumeratePhysicalDevices(instance, &pdCount, devices.data()) != VK_SUCCESS ||
            devices[0] == VK_NULL_HANDLE) {
            std::printf("FAIL: vkEnumeratePhysicalDevices(data, tier 1.%d)\n", tier);
            vkDestroyInstance(instance, nullptr);
            allTiersPass = false;
            continue;
        }
        VkPhysicalDevice physicalDevice = devices[0];

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physicalDevice, &props);
        const uint32_t physVersion = props.apiVersion;
        const uint32_t effective = (physVersion < tierVersion) ? physVersion : tierVersion;
        const int effMinor = static_cast<int>(VK_API_VERSION_MINOR(effective));
        std::printf("physical device: %s\n", props.deviceName);
        std::printf("physical apiVersion: %s   effective(instance,physical): %s\n",
                    versionString(physVersion), versionString(effective));
        std::printf("note: requesting a lower instance apiVersion does NOT emulate an older GPU;\n"
                    "             entry availability below reflects the negotiated interface version only.\n");

        // --- debug messenger (if validation + debug utils) ----------------------
        VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
        if (useDebugUtils) {
            VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            mi.pfnUserCallback = debugUtilsCallback;
            if (vkCreateDebugUtilsMessengerEXT(instance, &mi, nullptr, &messenger) == VK_SUCCESS) {
                g_validationEnabled = true;
            } else {
                std::printf("WARN: validation layer enabled but messenger creation failed; "
                            "validation errors will not be counted\n");
            }
        } else if (layerAvailable) {
            std::printf("note: validation layer enabled but VK_EXT_debug_utils unavailable; "
                        "validation errors will not be counted\n");
        }

        // --- queue family --------------------------------------------------------
        uint32_t qfCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qfCount, nullptr);
        if (qfCount == 0) {
            std::printf("FAIL: no queue families (tier 1.%d)\n", tier);
            if (messenger) vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
            vkDestroyInstance(instance, nullptr);
            allTiersPass = false;
            continue;
        }
        std::vector<VkQueueFamilyProperties> qf(qfCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qfCount, qf.data());
        uint32_t familyIndex = qfCount; // invalid sentinel
        for (uint32_t i = 0; i < qfCount; ++i) {
            if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && qf[i].queueCount > 0) {
                familyIndex = i;
                break;
            }
        }
        if (familyIndex == qfCount) {
            for (uint32_t i = 0; i < qfCount; ++i) {
                if (qf[i].queueCount > 0) { familyIndex = i; break; }
            }
        }
        if (familyIndex == qfCount) {
            std::printf("FAIL: no usable queue family (tier 1.%d)\n", tier);
            if (messenger) vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
            vkDestroyInstance(instance, nullptr);
            allTiersPass = false;
            continue;
        }

        // --- device: NO features, NO extensions ---------------------------------
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = familyIndex;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;

        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;

        VkDevice device = VK_NULL_HANDLE;
        res = vkCreateDevice(physicalDevice, &dci, nullptr, &device);
        if (res != VK_SUCCESS || device == VK_NULL_HANDLE) {
            std::printf("FAIL: vkCreateDevice(tier 1.%d) -> %d\n", tier, static_cast<int>(res));
            if (messenger) vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
            vkDestroyInstance(instance, nullptr);
            allTiersPass = false;
            continue;
        }

        VolkDeviceTable table{};
        volkLoadDeviceTable(&table, device);

        bool tierOk = true;

        // --- required core entries per EFFECTIVE version -------------------------
        if (effMinor >= 1) {
            const EntryCheck instance11[] = {
                {"vkGetPhysicalDeviceProperties2(instance)", reinterpret_cast<void *>(vkGetPhysicalDeviceProperties2)},
                {"vkGetPhysicalDeviceFeatures2(instance)", reinterpret_cast<void *>(vkGetPhysicalDeviceFeatures2)},
            };
            tierOk = runChecks("1.1-instance", instance11, 2) && tierOk;

            const EntryCheck device11[] = {
                ENTRY(table, vkBindBufferMemory2),
                ENTRY(table, vkGetBufferMemoryRequirements2),
                ENTRY(table, vkCreateDescriptorUpdateTemplate),
                ENTRY(table, vkGetDescriptorSetLayoutSupport),
            };
            tierOk = runChecks("1.1-device", device11, 4) && tierOk;
        }
        if (effMinor >= 2) {
            const EntryCheck device12[] = {
                ENTRY(table, vkCreateRenderPass2),
                ENTRY(table, vkCmdBeginRenderPass2),
                ENTRY(table, vkGetSemaphoreCounterValue),
                ENTRY(table, vkCmdDrawIndirectCount),
            };
            tierOk = runChecks("1.2-device", device12, 4) && tierOk;
        }
        if (effMinor >= 3) {
            const EntryCheck device13[] = {
                ENTRY(table, vkQueueSubmit2),
                ENTRY(table, vkCmdPipelineBarrier2),
                ENTRY(table, vkCmdBeginRendering),
                ENTRY(table, vkCmdEndRendering),
                ENTRY(table, vkGetDeviceBufferMemoryRequirements),
                ENTRY(table, vkGetDeviceImageMemoryRequirements),
            };
            tierOk = runChecks("1.3-device(Modern)", device13, 6) && tierOk;
        }

        std::printf("tier 1.%d result: required entries for effective %s -> %s\n",
                    tier, versionString(effective), tierOk ? "ALL LOADED" : "MISSING ENTRIES");
        allTiersPass = allTiersPass && tierOk;

        vkDestroyDevice(device, nullptr);
        if (messenger != VK_NULL_HANDLE) vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
        vkDestroyInstance(instance, nullptr);
    }

    std::printf("\n=== smoke summary ===\n");
    std::printf("all instance tiers passed required-entry checks: %s\n",
                allTiersPass ? "YES" : "NO");
    if (g_validationEnabled) {
        std::printf("validation (KHRONOS_validation + debug utils): ENABLED, errors=%d\n",
                    g_validationErrors);
    } else {
        std::printf("validation (KHRONOS_validation + debug utils): NOT ACTIVE -> NOT VERIFIED\n");
    }

    const bool pass = allTiersPass && g_validationErrors == 0;
    std::printf("smoke exit: %d\n", pass ? 0 : 1);
    return pass ? 0 : 1;
}
