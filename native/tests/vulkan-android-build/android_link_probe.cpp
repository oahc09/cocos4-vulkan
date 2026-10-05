// Final-link probe for the Android cocos_engine (M0-G).
//
// This translation unit forces real relocations into the engine objects that
// matter for the Vulkan modernization:
//   - cc::gfx::CCVKDevice::getInstance        (gfx-vulkan backend entry)
//   - cc::pipeline::bindingMappingInfo        (out-of-line symbol consumed by
//                                              gfx::DeviceManager::create)
//   - cc::gfx::SPIRVUtils::compileGLSL        (shader compile path, links the
//                                              source-built glslang toolchain)
// The library is only LINKED on the host; it is never executed on Android.

#include "renderer/GFXDeviceManager.h" // pulls CC_USE_VULKAN -> gfx-vulkan/VKDevice.h
#include "renderer/gfx-base/SPIRVUtils.h"
#include "renderer/pipeline/Define.h"

// Game-side entry point expected by the engine platform layer
// (native/cocos/platform/android/AndroidPlatform.cpp:81, provided by a real
// game project's main). Link-only probe: defined but never executed.
// NOTE: declared as plain C++ (mangled) by the engine, not extern "C".
int cocos_main(int /*argc*/, const char ** /*argv*/) {
    return 0;
}

// Runtime-selector based: addresses/calls below cannot be constant-folded by
// the compiler, so the linker must emit real relocations and pull in
// VKDevice.o / SPIRVUtils.o / pipeline Define.o from cocos_engine (and via
// SPIRVUtils the source-built glslang toolchain archives). The exported
// function is a gc-sections root; it is never executed on the host.
extern "C" __attribute__((visibility("default"))) __attribute__((used))
void const *cc_vulkan_android_probe(int selector) {
    switch (selector) {
        case 0:
            return reinterpret_cast<void const *>(&cc::gfx::CCVKDevice::getInstance);
        case 1:
            return reinterpret_cast<void const *>(&cc::pipeline::bindingMappingInfo);
        case 2: {
            // real out-of-line member call -> call relocation to SPIRVUtils.o
            auto *utils = cc::gfx::SPIRVUtils::getInstance();
            utils->compileGLSL(cc::gfx::ShaderStageFlagBit::VERTEX, ccstd::string{});
            return nullptr;
        }
        case 3: {
            // real call into gfx::DeviceManager::create -> pulls the whole
            // Vulkan device bootstrap chain (VKGPUContext, volk, swapchain, ...)
            cc::gfx::DeviceInfo info{cc::pipeline::bindingMappingInfo};
            return cc::gfx::DeviceManager::create(info);
        }
        default:
            return nullptr;
    }
}
