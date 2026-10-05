add_executable(vulkan_engine_smoke engine_smoke.cpp)
target_link_libraries(vulkan_engine_smoke PRIVATE ${ENGINE_NAME})
target_compile_features(vulkan_engine_smoke PRIVATE cxx_std_17)

# M2-D: real Win32 WSI stress harness (own window, real swapchain flows).
if(WIN32)
    add_executable(vulkan_pipeline_cache_smoke pipeline_cache_smoke.cpp)
    target_link_libraries(vulkan_pipeline_cache_smoke PRIVATE ${ENGINE_NAME} user32)
    target_compile_features(vulkan_pipeline_cache_smoke PRIVATE cxx_std_17)
    add_executable(vulkan_wsi_smoke wsi_smoke.cpp)
    target_link_libraries(vulkan_wsi_smoke PRIVATE ${ENGINE_NAME} user32)
    target_compile_features(vulkan_wsi_smoke PRIVATE cxx_std_17)

    # M3-G: recording-lifetime forensic harness (offscreen, no swapchain).
    add_executable(vulkan_recording_lifetime_smoke recording_lifetime_smoke.cpp)
    target_link_libraries(vulkan_recording_lifetime_smoke PRIVATE ${ENGINE_NAME} user32)
    target_compile_features(vulkan_recording_lifetime_smoke PRIVATE cxx_std_17)

    # M3-J: staging content lifetime forensic harness.
    add_executable(vulkan_staging_lifetime_smoke staging_lifetime_smoke.cpp)
    target_link_libraries(vulkan_staging_lifetime_smoke PRIVATE ${ENGINE_NAME} user32)
    target_compile_features(vulkan_staging_lifetime_smoke PRIVATE cxx_std_17)

    # M6-A5: real D32 depth-only SAMPLE_ZERO MSAA resolve harness.
    add_executable(vulkan_depth_resolve_smoke depth_resolve_smoke.cpp)
    target_link_libraries(vulkan_depth_resolve_smoke PRIVATE ${ENGINE_NAME} user32)
    target_compile_features(vulkan_depth_resolve_smoke PRIVATE cxx_std_17)

    # M6-A4: real color MSAA resolve harness (Classic vs Dynamic hybrid).
    add_executable(vulkan_msaa_resolve_smoke msaa_resolve_smoke.cpp)
    target_link_libraries(vulkan_msaa_resolve_smoke PRIVATE ${ENGINE_NAME} user32)
    target_compile_features(vulkan_msaa_resolve_smoke PRIVATE cxx_std_17)

    # M6-A0: real-GPU Classic vs Dynamic rendering clear comparison (offscreen).
    add_executable(vulkan_dynamic_rendering_smoke dynamic_rendering_smoke.cpp)
    target_link_libraries(vulkan_dynamic_rendering_smoke PRIVATE ${ENGINE_NAME} user32)
    target_compile_features(vulkan_dynamic_rendering_smoke PRIVATE cxx_std_17)

    add_executable(vulkan_dynamic_depth_stencil_smoke dynamic_depth_stencil_smoke.cpp)
    target_link_libraries(vulkan_dynamic_depth_stencil_smoke PRIVATE ${ENGINE_NAME} user32)
    target_compile_features(vulkan_dynamic_depth_stencil_smoke PRIVATE cxx_std_17)

    # M3-M: ordinary descriptor mutation after recording.
    add_executable(vulkan_descriptor_lifetime_smoke descriptor_lifetime_smoke.cpp)
    target_link_libraries(vulkan_descriptor_lifetime_smoke PRIVATE ${ENGINE_NAME} user32)
    target_compile_features(vulkan_descriptor_lifetime_smoke PRIVATE cxx_std_17)

    # M3-L1: event/split-barrier lifetime harness.
    add_executable(vulkan_event_lifetime_smoke event_lifetime_smoke.cpp)
    target_link_libraries(vulkan_event_lifetime_smoke PRIVATE ${ENGINE_NAME} user32)
    target_compile_features(vulkan_event_lifetime_smoke PRIVATE cxx_std_17)

    # M2-F: aborted-frame / recreate-failure red-case harness (--case <name>,
    # one process per case so a crash cannot hide the other evidence).
    add_executable(vulkan_wsi_lifecycle_smoke wsi_lifecycle_smoke.cpp)
    target_link_libraries(vulkan_wsi_lifecycle_smoke PRIVATE ${ENGINE_NAME} user32)
    target_compile_features(vulkan_wsi_lifecycle_smoke PRIVATE cxx_std_17)
endif()

# M7-G2: prewarm pipeline cache experiment (real engine, Win32).
if(WIN32)
    add_executable(vulkan_pipeline_prewarm_smoke pipeline_prewarm_smoke.cpp)
    target_include_directories(vulkan_pipeline_prewarm_smoke PRIVATE
        ../../cocos/renderer/gfx-vulkan
        ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include)
    target_link_libraries(vulkan_pipeline_prewarm_smoke PRIVATE ${ENGINE_NAME} user32)
    target_compile_features(vulkan_pipeline_prewarm_smoke PRIVATE cxx_std_17)
endif()

# PERF-A1: independent first graphics use / first-frame present API timing.
if(WIN32)
    add_executable(vulkan_first_use_smoke first_use_smoke.cpp)
    target_include_directories(vulkan_first_use_smoke PRIVATE
        ../../cocos/renderer/gfx-vulkan
        ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include)
    target_link_libraries(vulkan_first_use_smoke PRIVATE ${ENGINE_NAME} user32)
    target_compile_features(vulkan_first_use_smoke PRIVATE cxx_std_17)
endif()
