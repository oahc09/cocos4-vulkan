# ============================================================================
# Android arm64 cocos_engine build + final-link probe (M0-G)
#
# Minimal top-level entry that includes the FULL engine (native/CMakeLists.txt)
# with CC_USE_VULKAN=ON / CC_USE_GLES3=OFF, cross-compiled with the NDK
# toolchain exactly like a real Android game template (templates/cmake/
# android.cmake links cocos_jni whole-archive + cocos_engine into the final
# shared library). The probe shared library does the same real final link and
# references real engine symbols; it is never executed (no device attached).
#
# Do NOT configure this directory as a desktop build; use the NDK toolchain:
#   see README.md / build.ps1
# ============================================================================

cmake_minimum_required(VERSION 3.18)

project(CocosVulkanAndroidBuild LANGUAGES C CXX)

set(COCOS_X_PATH "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "Engine native/ root")

if(NOT ANDROID)
    message(FATAL_ERROR
        "This entry must be configured with the Android NDK toolchain file "
        "(see README.md). Desktop builds belong to other test entries.")
endif()

# --- engine options (before include), aligned with the real Android template
set(CC_USE_VULKAN ON)
set(CC_USE_GLES3 OFF)
set(CC_USE_GLES2 OFF)
set(USE_PHYSICS_PHYSX OFF) # optional module, unrelated to the Vulkan backend
# everything else stays at engine defaults (V8, audio, spine, ...)

include(${COCOS_X_PATH}/CMakeLists.txt)

# --- final link probe: shared library, same shape as the real game template
add_library(vk_android_probe SHARED android_link_probe.cpp)
target_compile_features(vk_android_probe PRIVATE cxx_std_17)
target_link_libraries(vk_android_probe PRIVATE
    "-Wl,--whole-archive" cocos_jni "-Wl,--no-whole-archive"
    cocos_engine
)
