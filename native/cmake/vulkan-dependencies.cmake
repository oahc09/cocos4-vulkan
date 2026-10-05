# Build the pinned shader dependencies in a separate project, since external's
# legacy imported targets use the same upstream names (glslang, SPIRV, etc.).
if(CMAKE_VERSION VERSION_LESS 3.21)
    message(FATAL_ERROR "The Vulkan shader source toolchain requires CMake 3.21 or newer")
endif()
include(ExternalProject)

set(CC_SHADER_UPSTREAM_CACHE "" CACHE PATH "Optional cache of hash-verified shader source archives")
set(CC_VULKAN_SHADER_TOOLCHAIN_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/vulkan-shader-toolchain/build"
    CACHE PATH "Build directory for the pinned Vulkan shader toolchain; do not share between platforms")
set(CC_VULKAN_SHADER_INSTALL_DIR "${CMAKE_CURRENT_BINARY_DIR}/vulkan-shader-toolchain/install")
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
endif()

set(shader_library_names glslang MachineIndependent GenericCodeGen OSDependent SPIRV
    glslang-default-resource-limits SPIRV-Tools-opt SPIRV-Tools)
set(shader_byproducts)
foreach(shader_lib IN LISTS shader_library_names)
    if(WIN32)
        set(shader_file "${shader_lib}$<$<CONFIG:Debug>:d>.lib")
    else()
        set(shader_file "lib${shader_lib}.a")
    endif()
    list(APPEND shader_byproducts "${CC_VULKAN_SHADER_INSTALL_DIR}/$<CONFIG>/lib/${shader_file}")
endforeach()

set(shader_cmake_args
    "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>/$<CONFIG>"
    "-DCMAKE_BUILD_TYPE=$<CONFIG>"
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
    "-DCC_SHADER_UPSTREAM_CACHE=${CC_SHADER_UPSTREAM_CACHE}"
    "-DCC_BUILD_SPIRV_HARNESS=OFF"
)
foreach(shader_arg CMAKE_TOOLCHAIN_FILE CMAKE_MAKE_PROGRAM CMAKE_MSVC_RUNTIME_LIBRARY
        CMAKE_OSX_ARCHITECTURES CMAKE_OSX_SYSROOT CMAKE_OSX_DEPLOYMENT_TARGET
        ANDROID_ABI ANDROID_PLATFORM ANDROID_STL ANDROID_ARM_NEON)
    if(DEFINED ${shader_arg} AND NOT "${${shader_arg}}" STREQUAL "")
        string(REPLACE ";" "|" shader_arg_value "${${shader_arg}}")
        list(APPEND shader_cmake_args "-D${shader_arg}=${shader_arg_value}")
    endif()
endforeach()

ExternalProject_Add(cc_vulkan_shader_toolchain
    SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/vulkan-shader-toolchain"
    BINARY_DIR "${CC_VULKAN_SHADER_TOOLCHAIN_BUILD_DIR}"
    INSTALL_DIR "${CC_VULKAN_SHADER_INSTALL_DIR}"
    LIST_SEPARATOR "|"
    CMAKE_ARGS ${shader_cmake_args}
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config $<CONFIG> --parallel 4
    INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR> --config $<CONFIG>
        --prefix <INSTALL_DIR>/$<CONFIG>
    BUILD_ALWAYS TRUE
    BUILD_BYPRODUCTS ${shader_byproducts}
)

set(CC_VULKAN_SHADER_LIBRARIES)
foreach(shader_lib IN LISTS shader_library_names)
    set(shader_target "cc_vk_shader_${shader_lib}")
    add_library(${shader_target} STATIC IMPORTED GLOBAL)
    foreach(shader_config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        if(shader_config STREQUAL DEBUG)
            set(shader_config_dir "Debug")
        elseif(shader_config STREQUAL RELEASE)
            set(shader_config_dir "Release")
        elseif(shader_config STREQUAL RELWITHDEBINFO)
            set(shader_config_dir "RelWithDebInfo")
        else()
            set(shader_config_dir "MinSizeRel")
        endif()
        if(WIN32)
            if(shader_config STREQUAL DEBUG)
                set(shader_file "${shader_lib}d.lib")
            else()
                set(shader_file "${shader_lib}.lib")
            endif()
        else()
            set(shader_file "lib${shader_lib}.a")
        endif()
        set_property(TARGET ${shader_target} PROPERTY IMPORTED_LOCATION_${shader_config}
            "${CC_VULKAN_SHADER_INSTALL_DIR}/${shader_config_dir}/lib/${shader_file}")
    endforeach()
    # Ninja needs to know these files are provided by the dependency project.
    if(WIN32 AND CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(shader_file "${shader_lib}d.lib")
    elseif(WIN32)
        set(shader_file "${shader_lib}.lib")
    else()
        set(shader_file "lib${shader_lib}.a")
    endif()
    set_property(TARGET ${shader_target} PROPERTY IMPORTED_LOCATION
        "${CC_VULKAN_SHADER_INSTALL_DIR}/${CMAKE_BUILD_TYPE}/lib/${shader_file}")
    add_dependencies(${shader_target} cc_vulkan_shader_toolchain)
    list(APPEND CC_VULKAN_SHADER_LIBRARIES ${shader_target})
endforeach()

# Keep the legacy external package intact for backends that still use it.
list(REMOVE_ITEM CC_EXTERNAL_LIBS glslang glslang-default-resource-limits
    MachineIndependent OGLCompiler OSDependent SPIRV SPIRV-Tools-opt SPIRV-Tools GenericCodeGen)
list(APPEND CC_EXTERNAL_LIBS ${CC_VULKAN_SHADER_LIBRARIES})
