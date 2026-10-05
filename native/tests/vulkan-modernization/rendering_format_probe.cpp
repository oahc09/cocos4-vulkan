// ============================================================================
// M6 device-capability format probe (query-only, no device creation). rev2.
//
// instance 1.3 (loader <1.3 -> UNSUPPORTED exit 3) -> enumerate ALL physical
// devices -> per device: Properties2 + DepthStencilResolveProperties (>=1.2),
// framebuffer sample-count limits, fixed format matrix (RGBA8 x2 usages,
// D16/D32/D24S8/D32S8/S8 x DEPTH_STENCIL_ATTACHMENT and x |TRANSFER_SRC) via
// vkGetPhysicalDeviceFormatProperties and vkGetPhysicalDeviceImageFormatProperties.
// VK_ERROR_FORMAT_NOT_SUPPORTED is DATA (NA); any other non-SUCCESS image-format
// result fails the whole probe (no QUERY_COMPLETE). No logical device/queue/
// image/draw; proves QUERY results only. Exit 0 = QUERY_COMPLETE; 1 = failure;
// 3 = loader/instance UNSUPPORTED. --self-test runs a CPU-only judge test.
// ============================================================================

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "volk.h"

namespace {

bool g_anyFailure = false;

int fail_(const char *what) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    return 1;
}

std::string sampleBits(VkSampleCountFlags f) {
    // per-call owned string: no shared static buffer aliasing in one printf
    char head[16];
    std::snprintf(head, sizeof(head), "0x%x", static_cast<unsigned>(f));
    std::string out = std::string(head) + "(";
    bool first = true;
    for (int bit = 1 << 6; bit >= 1; bit >>= 1) {
        if (f & static_cast<VkSampleCountFlags>(bit)) {
            if (!first) out += ",";
            out += std::to_string(bit);
            first = false;
        }
    }
    out += ")";
    return out;
}

// VK_ERROR_FORMAT_NOT_SUPPORTED is expected query data, not a probe failure.
bool imageFormatOk(VkResult r, bool *na) {
    *na = (r == VK_ERROR_FORMAT_NOT_SUPPORTED);
    return (r == VK_SUCCESS) || *na;
}

void probeImageFormat(VkPhysicalDevice pd, VkFormat fmt, VkImageUsageFlags usage, const char *tag) {
    VkImageFormatProperties props{};
    const VkResult r = vkGetPhysicalDeviceImageFormatProperties(
        pd, fmt, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &props);
    bool na = false;
    if (!imageFormatOk(r, &na)) {
        std::printf("imgfmt %s: result=%d UNEXPECTED\n", tag, static_cast<int>(r));
        g_anyFailure = true;
        return;
    }
    if (na) {
        std::printf("imgfmt %s: result=%d NA sampleCounts=0\n", tag, static_cast<int>(r));
        return;
    }
    std::printf("imgfmt %s: result=%d sampleCounts=%s maxExtent=%ux%ux%u maxMip=%u maxLayer=%u\n",
                tag, static_cast<int>(r), sampleBits(props.sampleCounts).c_str(),
                props.maxExtent.width, props.maxExtent.height, props.maxExtent.depth,
                props.maxMipLevels, props.maxArrayLayers);
}

int selfTest() {
    // CPU-only: judge classification + per-call formatting, no loader/GPU
    bool ok = true;
    bool na = false;
    if (!imageFormatOk(VK_ERROR_FORMAT_NOT_SUPPORTED, &na) || !na) ok = false; // NA accepted
    if (imageFormatOk(VK_ERROR_OUT_OF_HOST_MEMORY, &na)) ok = false;           // must reject
    if (imageFormatOk(VK_ERROR_DEVICE_LOST, &na)) ok = false;                  // must reject
    if (!imageFormatOk(VK_SUCCESS, &na) || na) ok = false;                     // success ok
    // distinct flags format distinctly (1 / 2 / 4 must not alias)
    const std::string a = sampleBits(0x1);
    const std::string b = sampleBits(0x2);
    const std::string c = sampleBits(0x4);
    if (a.find("(1)") == std::string::npos || b.find("(2)") == std::string::npos ||
        c.find("(4)") == std::string::npos || a == b || b == c || a == c) {
        ok = false;
    }
    std::printf("self-test: %s\n", ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}

} // namespace

int runProbe(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1) {
        if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) return selfTest();
        std::fprintf(stderr, "FAIL: unknown argument '%s' (--self-test only)\n", argv[1]);
        return 1;
    }
    if (volkInitialize() != VK_SUCCESS) return fail_("volkInitialize");

    // real loader version: <1.3 cannot provide the requested instance tier
    uint32_t loaderVer = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion != nullptr) {
        if (vkEnumerateInstanceVersion(&loaderVer) != VK_SUCCESS) return fail_("vkEnumerateInstanceVersion");
    }
    if (loaderVer < VK_API_VERSION_1_3) {
        std::fprintf(stderr, "UNSUPPORTED: loader apiVersion %u.%u < 1.3\n",
                     VK_VERSION_MAJOR(loaderVer), VK_VERSION_MINOR(loaderVer));
        return 3;
    }

    if (vkCreateInstance == nullptr) return fail_("vkCreateInstance PFN missing");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "rendering_format_probe";
    app.apiVersion = VK_API_VERSION_1_3; // requested instance tier (NOT a GPU proof)
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ci, nullptr, &inst);
    if (r == VK_ERROR_INCOMPATIBLE_DRIVER) {
        std::fprintf(stderr, "UNSUPPORTED: instance 1.3 rejected\n");
        return 3;
    }
    if (r != VK_SUCCESS) return fail_("vkCreateInstance");
    volkLoadInstance(inst);
    // Keep the cleanup dispatcher independently of the mutable volk globals.
    struct InstanceOwner {
        VkInstance instance;
        PFN_vkDestroyInstance destroy;
        ~InstanceOwner() {
            if (destroy != nullptr) destroy(instance, nullptr);
        }
    } owner{inst, vkDestroyInstance};
    if (vkEnumeratePhysicalDevices == nullptr || vkGetPhysicalDeviceProperties == nullptr ||
        vkGetPhysicalDeviceProperties2 == nullptr ||
        vkGetPhysicalDeviceFormatProperties == nullptr ||
        vkGetPhysicalDeviceImageFormatProperties == nullptr || vkDestroyInstance == nullptr) {
        return fail_("instance-level PFN missing");
    }

    uint32_t count = 0;
    r = vkEnumeratePhysicalDevices(inst, &count, nullptr);
    if (r != VK_SUCCESS || count == 0) {
        return fail_("enumerate physical devices");
    }
    std::vector<VkPhysicalDevice> devices(count);
    if (vkEnumeratePhysicalDevices(inst, &count, devices.data()) != VK_SUCCESS) {
        return fail_("enumerate physical devices (2)");
    }
    std::printf("loaderApi=%u.%u physicalDevices=%u\n", VK_VERSION_MAJOR(loaderVer),
                VK_VERSION_MINOR(loaderVer), count);

    for (uint32_t d = 0; d < count; ++d) {
        VkPhysicalDevice pd = devices[d];
        VkPhysicalDeviceProperties plain{};
        vkGetPhysicalDeviceProperties(pd, &plain);
        const bool dsrOk = plain.apiVersion >= VK_API_VERSION_1_2;
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        VkPhysicalDeviceDepthStencilResolveProperties dsr{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES};
        if (dsrOk) p2.pNext = &dsr;
        vkGetPhysicalDeviceProperties2(pd, &p2);
        std::printf("device[%u]: name=%s vendorID=0x%x deviceID=0x%x apiVersion=%u.%u.%u\n", d,
                    p2.properties.deviceName, p2.properties.vendorID, p2.properties.deviceID,
                    VK_VERSION_MAJOR(p2.properties.apiVersion),
                    VK_VERSION_MINOR(p2.properties.apiVersion),
                    VK_VERSION_PATCH(p2.properties.apiVersion));
        if (dsrOk) {
            std::printf(
                "dsr: depthModes=0x%x stencilModes=0x%x independentResolve=%u "
                "independentResolveNone=%u\n",
                dsr.supportedDepthResolveModes, dsr.supportedStencilResolveModes,
                static_cast<unsigned>(dsr.independentResolve),
                static_cast<unsigned>(dsr.independentResolveNone));
        } else {
            std::printf("dsr: NA physical<1.2\n");
        }
        std::printf("limits: color=%s depth=%s stencil=%s\n",
                    sampleBits(p2.properties.limits.framebufferColorSampleCounts).c_str(),
                    sampleBits(p2.properties.limits.framebufferDepthSampleCounts).c_str(),
                    sampleBits(p2.properties.limits.framebufferStencilSampleCounts).c_str());

        const struct {
            VkFormat f;
            const char *n;
        } fmts[] = {
            {VK_FORMAT_R8G8B8A8_UNORM, "RGBA8_UNORM"},
            {VK_FORMAT_D16_UNORM, "D16_UNORM"},
            {VK_FORMAT_D32_SFLOAT, "D32_SFLOAT"},
            {VK_FORMAT_D24_UNORM_S8_UINT, "D24_UNORM_S8_UINT"},
            {VK_FORMAT_D32_SFLOAT_S8_UINT, "D32_SFLOAT_S8_UINT"},
            {VK_FORMAT_S8_UINT, "S8_UINT"},
        };
        for (const auto &e : fmts) {
            VkFormatProperties fp{};
            vkGetPhysicalDeviceFormatProperties(pd, e.f, &fp);
            std::printf("fmt %s(%u): optimal=0x%x linear=0x%x buffer=0x%x\n", e.n,
                        static_cast<unsigned>(e.f), fp.optimalTilingFeatures, fp.linearTilingFeatures,
                        fp.bufferFeatures);
        }
        probeImageFormat(pd, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                         "RGBA8+COLOR");
        probeImageFormat(pd, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         "RGBA8+COLOR+XFER");
        const VkFormat dsFmts[] = {VK_FORMAT_D16_UNORM, VK_FORMAT_D32_SFLOAT,
                                   VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT,
                                   VK_FORMAT_S8_UINT};
        const char *dsNames[] = {"D16+DS", "D32+DS", "D24S8+DS", "D32S8+DS", "S8+DS"};
        const char *dsXNames[] = {"D16+DS+XFER", "D32+DS+XFER", "D24S8+DS+XFER", "D32S8+DS+XFER",
                                  "S8+DS+XFER"};
        for (int i = 0; i < 5; ++i) {
            probeImageFormat(pd, dsFmts[i], VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, dsNames[i]);
            probeImageFormat(pd, dsFmts[i],
                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                             dsXNames[i]);
        }
    }

    if (g_anyFailure) return 1; // unexpected results: no QUERY_COMPLETE
    std::printf("QUERY_COMPLETE\n");
    return 0;
}

int main(int argc, char **argv) {
    try {
        return runProbe(argc, argv);
    } catch (const std::exception &error) {
        return fail_(error.what());
    } catch (...) {
        return fail_("unexpected exception");
    }
}
