# ============================================================================
# Vulkan shader toolchain source-build (M0-E)
#
# Standalone CMake project that builds the LOCKED shader toolchain from
# official sources, replacing the non-rebuildable prebuilt glslang/SPIRV-Tools
# blobs inside (git-ignored) native/external:
#
#   glslang       KhronosGroup/glslang       tag vulkan-sdk-1.3.296.0
#                 commit 46ef757e048e760b46601e6e77ae0cb72c97bd2f
#   SPIRV-Tools   KhronosGroup/SPIRV-Tools   (glslang known_good.json pin)
#                 commit 6dcc7e350a0b9871a825414d42329e44b0eb8109
#   SPIRV-Headers KhronosGroup/SPIRV-Headers (glslang known_good.json pin)
#                 commit 2a9b6f951c7d6b04b6c21fe1bf3f475b68b84801
#
# Order: SPIRV-Headers -> SPIRV-Tools -> glslang (glslang's CMake picks up the
# already-defined SPIRV-Tools-opt target, never a mismatched find_package).
# Static engine inputs, no upstream tests/CLI/HLSL, optimizer kept ON.
# SPIRV-Tools also defines a shared target; the engine never links it.
#
# Reproducible anywhere via the pinned codeload URLs; a local download cache
# (CC_SHADER_UPSTREAM_CACHE with glslang.tar.gz / spirv-tools.tar.gz /
# spirv-headers.tar.gz) may be reused - URL_HASH is enforced either way.
#
# Typical use (Windows / VS 2026):
#   cmake -S native/cmake/vulkan-shader-toolchain ^
#         -B native/build/vulkan-modernization/shader-toolchain/win64-vs ^
#         -G "Visual Studio 18 2026" -A x64 ^
#         -DCC_SHADER_UPSTREAM_CACHE=<repo>/native/build/vulkan-modernization/upstream ^
#         -DCC_BUILD_SPIRV_HARNESS=ON
#   cmake --build <build> --config Release
#   cmake --install <build> --config Release --prefix <prefix>
# ============================================================================

cmake_minimum_required(VERSION 3.21)

project(vulkan_shader_toolchain LANGUAGES C CXX)

include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
if(WIN32)
    # Use one Debug naming convention across glslang and SPIRV-Tools.
    set(CMAKE_DEBUG_POSTFIX "d")
endif()

set(CC_SHADER_UPSTREAM_CACHE "" CACHE PATH
    "Optional dir with predownloaded glslang.tar.gz / spirv-tools.tar.gz / spirv-headers.tar.gz (hash still enforced)")

set(CC_SHADER_TOOLCHAIN_GLSLANG_COMMIT        "46ef757e048e760b46601e6e77ae0cb72c97bd2f")
set(CC_SHADER_TOOLCHAIN_SPIRV_TOOLS_COMMIT    "6dcc7e350a0b9871a825414d42329e44b0eb8109")
set(CC_SHADER_TOOLCHAIN_SPIRV_HEADERS_COMMIT  "2a9b6f951c7d6b04b6c21fe1bf3f475b68b84801")

set(CC_SHADER_TOOLCHAIN_GLSLANG_SHA256        "ef16e5924e07f49b6280158645051ed5930ee5c5628e573fd2fb49e210aaca71")
set(CC_SHADER_TOOLCHAIN_SPIRV_TOOLS_SHA256    "e3665378ab0ae8efb327027ee103d10c3c9e05823dea8b28893c00886ffe1a23")
set(CC_SHADER_TOOLCHAIN_SPIRV_HEADERS_SHA256  "1698e1373bd6e59a263acef821c4d955c561b991feb6db8199833ef19ffe8a37")

function(cc_shader_source_url out_var org repo commit cache_file)
    if(CC_SHADER_UPSTREAM_CACHE AND EXISTS "${CC_SHADER_UPSTREAM_CACHE}/${cache_file}")
        file(TO_CMAKE_PATH "${CC_SHADER_UPSTREAM_CACHE}/${cache_file}" local_path)
        # NOTE: pass the local absolute path directly (ExternalProject/FetchContent
        # treat a local path as the source; a file:///E:/ URL would be misparsed
        # to "/E:/..." on Windows). URL_HASH is still enforced on the copy.
        set(${out_var} "${local_path}" PARENT_SCOPE)
        message(STATUS "${repo}: using local cache ${CC_SHADER_UPSTREAM_CACHE}/${cache_file} (URL_HASH enforced)")
    else()
        set(${out_var} "https://codeload.github.com/${org}/${repo}/tar.gz/${commit}" PARENT_SCOPE)
        message(STATUS "${repo}: downloading pinned commit ${commit}")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# 1) SPIRV-Headers (header-only)
# ---------------------------------------------------------------------------
set(SPIRV_HEADERS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)   # real upstream option name
set(SPIRV_HEADERS_ENABLE_INSTALL ON CACHE BOOL "" FORCE)  # real upstream option name

cc_shader_source_url(spirv_headers_url KhronosGroup SPIRV-Headers
    "${CC_SHADER_TOOLCHAIN_SPIRV_HEADERS_COMMIT}" spirv-headers.tar.gz)
FetchContent_Declare(spirv-headers
    URL ${spirv_headers_url}
    URL_HASH SHA256=${CC_SHADER_TOOLCHAIN_SPIRV_HEADERS_SHA256})
FetchContent_MakeAvailable(spirv-headers)

# ---------------------------------------------------------------------------
# 2) SPIRV-Tools (static, no tests, no CLI tools)
# ---------------------------------------------------------------------------
set(SPIRV_SKIP_TESTS        ON  CACHE BOOL "" FORCE)
set(SPIRV_SKIP_EXECUTABLES  ON  CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS       OFF CACHE BOOL "" FORCE) # glslang and engine inputs are static

# Hand the in-tree SPIRV-Headers to SPIRV-Tools so it never find_package()s a
# different version (upstream honors SPIRV-Headers_SOURCE_DIR when defined).
set(SPIRV-Headers_SOURCE_DIR "${spirv-headers_SOURCE_DIR}")

cc_shader_source_url(spirv_tools_url KhronosGroup SPIRV-Tools
    "${CC_SHADER_TOOLCHAIN_SPIRV_TOOLS_COMMIT}" spirv-tools.tar.gz)
FetchContent_Declare(spirv-tools
    URL ${spirv_tools_url}
    URL_HASH SHA256=${CC_SHADER_TOOLCHAIN_SPIRV_TOOLS_SHA256})
FetchContent_MakeAvailable(spirv-tools)

# ---------------------------------------------------------------------------
# 3) glslang (static, no tests / CLI / HLSL; optimizer ON against the
#    already-defined SPIRV-Tools-opt target)
# ---------------------------------------------------------------------------
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE) # no glslangValidator CLI
set(ENABLE_HLSL             OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT              ON  CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL          OFF CACHE BOOL "" FORCE) # SPIRV-Tools-opt target already exists
set(BUILD_TESTING           OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL   ON  CACHE BOOL "" FORCE) # real upstream option name

cc_shader_source_url(glslang_url KhronosGroup glslang
    "${CC_SHADER_TOOLCHAIN_GLSLANG_COMMIT}" glslang.tar.gz)
FetchContent_Declare(glslang
    URL ${glslang_url}
    URL_HASH SHA256=${CC_SHADER_TOOLCHAIN_GLSLANG_SHA256})
FetchContent_MakeAvailable(glslang)

# ---------------------------------------------------------------------------
# MSVC runtime note: upstream targets use the CMake default dynamic CRT
# (/MD Release, /MDd Debug) which matches the engine default; no override set.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Provenance + library inventory installed alongside headers/libs
# ---------------------------------------------------------------------------
set(CC_SHADER_TOOLCHAIN_VERSION_FILE "${CMAKE_CURRENT_BINARY_DIR}/shader-toolchain-version.txt")
string(TIMESTAMP CC_SHADER_TOOLCHAIN_BUILD_STAMP "%Y-%m-%d %H:%M:%S")
file(WRITE "${CC_SHADER_TOOLCHAIN_VERSION_FILE}"
"vulkan-shader-toolchain (source-built, M0-E)
glslang        commit ${CC_SHADER_TOOLCHAIN_GLSLANG_COMMIT} (tag vulkan-sdk-1.3.296.0) sha256 ${CC_SHADER_TOOLCHAIN_GLSLANG_SHA256}
SPIRV-Tools    commit ${CC_SHADER_TOOLCHAIN_SPIRV_TOOLS_COMMIT} (glslang known_good pin)  sha256 ${CC_SHADER_TOOLCHAIN_SPIRV_TOOLS_SHA256}
SPIRV-Headers  commit ${CC_SHADER_TOOLCHAIN_SPIRV_HEADERS_COMMIT} (glslang known_good pin) sha256 ${CC_SHADER_TOOLCHAIN_SPIRV_HEADERS_SHA256}
built ${CC_SHADER_TOOLCHAIN_BUILD_STAMP} generator=${CMAKE_GENERATOR} platform=${CMAKE_SYSTEM_NAME}
")
install(FILES "${CC_SHADER_TOOLCHAIN_VERSION_FILE}" DESTINATION .)

# Actual library inventory of this install (names differ per platform/toolchain)
install(CODE "
    file(GLOB _libs \"\${CMAKE_INSTALL_PREFIX}/lib/*\")
    file(WRITE \"\${CMAKE_INSTALL_PREFIX}/lib-list.txt\" \"Installed libraries (actual link inputs; link order for engines: glslang -> MachineIndependent -> GenericCodeGen -> OSDependent -> SPIRV -> SPIRV-Tools-opt -> SPIRV-Tools)\\n\")
    foreach(_l \${_libs})
        file(APPEND \"\${CMAKE_INSTALL_PREFIX}/lib-list.txt\" \"  \${_l}\\n\")
    endforeach()
    message(STATUS \"lib-list.txt written to \${CMAKE_INSTALL_PREFIX}\")
")

# ---------------------------------------------------------------------------
# Optional harness exe against THIS toolchain (never the old external libs)
# ---------------------------------------------------------------------------
option(CC_BUILD_SPIRV_HARNESS "Build SPIR-V target harness against this source-built toolchain" OFF)

if(CC_BUILD_SPIRV_HARNESS)
    set(CC_HARNESS_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../tests/vulkan-modernization" CACHE PATH "Shader harness source dir")
    if(NOT EXISTS "${CC_HARNESS_SRC_DIR}/spirv_target_harness.cpp")
        message(FATAL_ERROR "harness source not found under '${CC_HARNESS_SRC_DIR}'")
    endif()
    add_executable(spirv_target_harness "${CC_HARNESS_SRC_DIR}/spirv_target_harness.cpp")
    target_compile_features(spirv_target_harness PRIVATE cxx_std_17)
    # Same source adapts old/new glslang APIs via __has_include; this target
    # links ONLY the source-built toolchain targets below.
    target_link_libraries(spirv_target_harness PRIVATE
        glslang
        SPIRV
        glslang-default-resource-limits
    )
    # sanity: never accidentally point at the old external tree
    get_target_property(_inc glslang INTERFACE_INCLUDE_DIRECTORIES)
    message(STATUS "harness glslang includes: ${_inc}")
endif()
