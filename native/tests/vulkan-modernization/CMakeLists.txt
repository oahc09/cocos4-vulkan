cmake_minimum_required(VERSION 3.15)
project(SpirvTargetHarness LANGUAGES C CXX)

# ============================================================================
# SPIR-V target-env verification harness (M0-B)
#
# Links the SAME prebuilt glslang static libraries the engine links
# (native/external/<platform>/libs/glslang) and uses the SAME compile flow as
# cocos/renderer/gfx-base/SPIRVUtils.cpp to prove, per Vulkan target
# (1.1 / 1.2 / 1.3), which SPIR-V version the bundled toolchain can really
# produce. This is NOT the engine build and does not touch engine sources.
#
# CC_GLSLANG_EXTERNAL_ROOT can be pointed at an upgraded external/ tree later
# to re-run the identical verification against the new dependency set.
# ============================================================================

set(CC_GLSLANG_EXTERNAL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../external" CACHE PATH
    "Root of native/external providing sources/ headers and <platform>/libs/glslang")

get_filename_component(CC_GLSLANG_EXTERNAL_ROOT "${CC_GLSLANG_EXTERNAL_ROOT}" ABSOLUTE)

if(NOT EXISTS "${CC_GLSLANG_EXTERNAL_ROOT}/sources/glslang/Public/ShaderLang.h")
    message(FATAL_ERROR
        "glslang headers not found under '${CC_GLSLANG_EXTERNAL_ROOT}/sources'. "
        "Set -DCC_GLSLANG_EXTERNAL_ROOT to a valid native/external root.")
endif()

set(CC_GLSLANG_LIBS
    glslang
    glslang-default-resource-limits
    MachineIndependent
    OGLCompiler
    OSDependent
    SPIRV
    SPIRV-Tools-opt
    SPIRV-Tools
    GenericCodeGen
)

if(WIN32)
    set(CC_GLSLANG_LIB_DIR "${CC_GLSLANG_EXTERNAL_ROOT}/win64/libs/glslang")
    if(NOT EXISTS "${CC_GLSLANG_LIB_DIR}/glslang.lib")
        message(FATAL_ERROR "win64 glslang release libs not found under '${CC_GLSLANG_LIB_DIR}'")
    endif()
    set(CC_GLSLANG_IMPORTED_TARGETS)
    foreach(gl IN LISTS CC_GLSLANG_LIBS)
        add_library(harness_${gl} STATIC IMPORTED GLOBAL)
        set_target_properties(harness_${gl} PROPERTIES
            IMPORTED_LOCATION         "${CC_GLSLANG_LIB_DIR}/${gl}.lib"
            IMPORTED_LOCATION_DEBUG   "${CC_GLSLANG_LIB_DIR}/${gl}d.lib"
        )
        list(APPEND CC_GLSLANG_IMPORTED_TARGETS harness_${gl})
    endforeach()
elseif(ANDROID)
    if(NOT ANDROID_ABI)
        message(FATAL_ERROR "ANDROID_ABI not set; configure through the Android toolchain.")
    endif()
    set(CC_GLSLANG_LIB_DIR "${CC_GLSLANG_EXTERNAL_ROOT}/android/${ANDROID_ABI}/glslang")
    if(NOT EXISTS "${CC_GLSLANG_LIB_DIR}/libglslang.a")
        message(FATAL_ERROR "android glslang libs not found under '${CC_GLSLANG_LIB_DIR}'")
    endif()
    set(CC_GLSLANG_IMPORTED_TARGETS)
    foreach(gl IN LISTS CC_GLSLANG_LIBS)
        add_library(harness_${gl} STATIC IMPORTED GLOBAL)
        set_target_properties(harness_${gl} PROPERTIES
            IMPORTED_LOCATION "${CC_GLSLANG_LIB_DIR}/lib${gl}.a"
        )
        list(APPEND CC_GLSLANG_IMPORTED_TARGETS harness_${gl})
    endforeach()
else()
    message(FATAL_ERROR "spirv_target_harness: unsupported platform for bundled glslang prebuilts (win64/android implemented).")
endif()

add_executable(spirv_target_harness spirv_target_harness.cpp)

target_include_directories(spirv_target_harness PRIVATE
    "${CC_GLSLANG_EXTERNAL_ROOT}/sources"        # glslang/..., spirv/... headers (same root as engine CC_EXTERNAL_INCLUDES)
    "${CC_GLSLANG_EXTERNAL_ROOT}/sources/khronos" # vulkan/..., spirv/... headers (same root as engine public include)
)

target_compile_features(spirv_target_harness PRIVATE cxx_std_17)

target_link_libraries(spirv_target_harness PRIVATE ${CC_GLSLANG_IMPORTED_TARGETS})

# ============================================================================
# Vulkan loader / instance / device entry-loading smoke (M0-C)
#
# Uses ONLY the vendored Vulkan-Headers (vulkan-sdk-1.3.296.0) + regenerated
# volk (header 296) from the engine tree. The old external/sources/khronos
# headers are deliberately NOT on this target's include path.
# ============================================================================

set(CC_VK_GFX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../cocos/renderer/gfx-vulkan")
set(CC_VK_VENDORED_INCLUDE "${CC_VK_GFX_DIR}/dependencies/Vulkan-Headers/include")

if(NOT EXISTS "${CC_VK_VENDORED_INCLUDE}/vulkan/vulkan_core.h")
    message(FATAL_ERROR "vendored Vulkan-Headers not found under '${CC_VK_VENDORED_INCLUDE}'")
endif()

add_executable(vulkan_loader_smoke loader_smoke.cpp "${CC_VK_GFX_DIR}/volk.c")

target_include_directories(vulkan_loader_smoke PRIVATE
    "${CC_VK_VENDORED_INCLUDE}"   # <vulkan/vulkan.h> -> vendored 1.3.296 headers ONLY
    "${CC_VK_GFX_DIR}"            # "volk.h"
)

target_compile_features(vulkan_loader_smoke PRIVATE cxx_std_17)

add_executable(vulkan_rendering_format_probe rendering_format_probe.cpp "${CC_VK_GFX_DIR}/volk.c")
target_include_directories(vulkan_rendering_format_probe PRIVATE
    ${CC_VK_VENDORED_INCLUDE}
    ${CC_VK_GFX_DIR}
)
target_compile_features(vulkan_rendering_format_probe PRIVATE cxx_std_17)
if(WIN32)
    target_compile_definitions(vulkan_rendering_format_probe PRIVATE VK_USE_PLATFORM_WIN32_KHR)
elseif(ANDROID)
    target_compile_definitions(vulkan_rendering_format_probe PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
endif()

if(WIN32)
    target_compile_definitions(vulkan_loader_smoke PRIVATE VK_USE_PLATFORM_WIN32_KHR)
elseif(ANDROID)
    target_compile_definitions(vulkan_loader_smoke PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
endif()
