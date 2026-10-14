cmake_minimum_required(VERSION 3.15)
project(VulkanReviewRegressions LANGUAGES CXX)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
enable_testing()
set(VULKAN_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../../cocos/renderer/gfx-vulkan")
set(GENERATED_TEST "${CMAKE_CURRENT_BINARY_DIR}/wsi_lifetime_tests.cpp")
add_custom_command(
    OUTPUT "${GENERATED_TEST}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_wsi_lifetime.py"
        --output "${GENERATED_TEST}"
        --manifest "${CMAKE_CURRENT_BINARY_DIR}/wsi-extraction.json"
    DEPENDS extract_wsi_lifetime.py wsi_lifetime_tests.cpp.in
        "${VULKAN_SOURCE}/VKDevice.cpp" "${VULKAN_SOURCE}/VKSwapchain.cpp" "${VULKAN_SOURCE}/VKQueue.cpp"
    VERBATIM)
add_executable(vulkan_wsi_lifetime_tests "${GENERATED_TEST}")
target_compile_features(vulkan_wsi_lifetime_tests PRIVATE cxx_std_17)
# Assertions must stay disabled: each test expectation uses an always-on CHECK.
target_compile_definitions(vulkan_wsi_lifetime_tests PRIVATE CC_DEBUG=0 NDEBUG)
target_include_directories(vulkan_wsi_lifetime_tests PRIVATE
    "${VULKAN_SOURCE}" "${VULKAN_SOURCE}/dependencies/Vulkan-Headers/include")
add_test(NAME vulkan_wsi_lifetime_release COMMAND vulkan_wsi_lifetime_tests)
add_test(NAME vulkan_windows_external_root
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build_config_tests.py" --cmake "${CMAKE_COMMAND}")

# This independently added case uses the entire production fence-pool class.
set(FENCE_TEST "${CMAKE_CURRENT_BINARY_DIR}/fence_pool_tests.cpp")
add_custom_command(
    OUTPUT "${FENCE_TEST}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_fence_pool.py" --output "${FENCE_TEST}"
    DEPENDS extract_fence_pool.py fence_pool_tests.cpp.in "${VULKAN_SOURCE}/VKGPUObjects.h"
    VERBATIM)
add_executable(vulkan_fence_pool_tests "${FENCE_TEST}")
target_compile_features(vulkan_fence_pool_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_fence_pool_tests PRIVATE CC_DEBUG=0 NDEBUG)
target_include_directories(vulkan_fence_pool_tests PRIVATE "${VULKAN_SOURCE}/dependencies/Vulkan-Headers/include")
add_test(NAME vulkan_failed_submit_fence_pool COMMAND vulkan_fence_pool_tests)
