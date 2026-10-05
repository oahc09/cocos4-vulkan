cmake_minimum_required(VERSION 3.15)
project(VulkanApiVersionTests LANGUAGES CXX)

add_executable(vulkan_api_version_tests version_tests.cpp)
target_compile_features(vulkan_api_version_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_api_version_tests PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vulkan_api_version_tests PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)
enable_testing()
add_test(NAME vulkan_api_version_contract COMMAND vulkan_api_version_tests)

# M1-A: capability data-model contract tests (pure data + helpers, no engine,
# no Vulkan loader). Semantics of plan sections 4 / 5.4 / 17.2.
add_executable(vulkan_capability_tests capability_tests.cpp)
target_compile_features(vulkan_capability_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_capability_tests PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vulkan_capability_tests PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)
add_test(NAME vulkan_capability_contract COMMAND vulkan_capability_tests)

# M1-B: feature query/enable chain builder contract tests (synthetic feature
# snapshots; structural pNext/bit-policy/finalize semantics, no GPU).
add_executable(vulkan_feature_chain_tests feature_chain_tests.cpp)
target_compile_features(vulkan_feature_chain_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_feature_chain_tests PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vulkan_feature_chain_tests PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)
add_test(NAME vulkan_feature_chain_contract COMMAND vulkan_feature_chain_tests)

# M2-B: pure WSI swapchain state + acquire-index validity contract tests.
add_executable(vulkan_wsi_state_tests wsi_state_tests.cpp)
target_compile_features(vulkan_wsi_state_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_wsi_state_tests PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vulkan_wsi_state_tests PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)
add_test(NAME vulkan_wsi_state_contract COMMAND vulkan_wsi_state_tests)

# M3-C: GpuCompletionTracker contract tests (pure mock PFN variables, no
# Vulkan loader, no engine).
add_executable(vulkan_completion_tracker_tests completion_tracker_tests.cpp)
target_compile_features(vulkan_completion_tracker_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_completion_tracker_tests PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vulkan_completion_tracker_tests PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)
add_test(NAME vulkan_completion_tracker_contract COMMAND vulkan_completion_tracker_tests)

add_executable(vulkan_timeline_tracker_tests timeline_tracker_tests.cpp)
target_compile_features(vulkan_timeline_tracker_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_timeline_tracker_tests PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vulkan_timeline_tracker_tests PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)
add_test(NAME vulkan_timeline_tracker_contract COMMAND vulkan_timeline_tracker_tests)

# M4-A: shared Submit1/Submit2 model and actual PFN routing contracts.
add_executable(vulkan_submit_batch_tests submit_batch_tests.cpp)
target_compile_features(vulkan_submit_batch_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_submit_batch_tests PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vulkan_submit_batch_tests PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include)
add_test(NAME vulkan_submit_batch_contract COMMAND vulkan_submit_batch_tests)

# M4-A: semantic legacy-to-Barrier2 conversion (no loader or engine).
add_executable(vulkan_barrier_encoding_tests barrier_encoding_tests.cpp)
target_compile_features(vulkan_barrier_encoding_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_barrier_encoding_tests PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vulkan_barrier_encoding_tests PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include)
add_test(NAME vulkan_barrier_encoding_contract COMMAND vulkan_barrier_encoding_tests)

# M4-A4: CPU-only barrier encoding fresh-construct benchmark (no ctest; standalone).
add_executable(vulkan_barrier_encoding_benchmark barrier_encoding_benchmark.cpp)
target_compile_features(vulkan_barrier_encoding_benchmark PRIVATE cxx_std_17)
target_compile_definitions(vulkan_barrier_encoding_benchmark PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vulkan_barrier_encoding_benchmark PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)

# M4-A5: barrier allocation contract tests (single-barrier zero heap alloc).
add_executable(vulkan_barrier_allocation_tests barrier_allocation_tests.cpp)
target_compile_features(vulkan_barrier_allocation_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_barrier_allocation_tests PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vulkan_barrier_allocation_tests PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)
add_test(NAME vulkan_barrier_allocation_contract COMMAND vulkan_barrier_allocation_tests)

# M5-B: pure-owning VKRenderingSignature value model contract tests.
add_executable(vulkan_rendering_signature_tests rendering_signature_tests.cpp)
target_compile_features(vulkan_rendering_signature_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_rendering_signature_tests PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vulkan_rendering_signature_tests PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)
add_test(NAME vulkan_rendering_signature_contract COMMAND vulkan_rendering_signature_tests)

# M5-C: offline owning pipeline/secondary rendering contract model tests.
add_executable(vulkan_rendering_contract_tests rendering_contract_tests.cpp)
target_compile_features(vulkan_rendering_contract_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_rendering_contract_tests PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vulkan_rendering_contract_tests PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)
add_test(NAME vulkan_rendering_contract_contract COMMAND vulkan_rendering_contract_tests)

# M5-D: typed pipeline pNext chain composer tests.
add_executable(vulkan_pipeline_pnext_chain_tests pipeline_pnext_chain_tests.cpp)
target_compile_features(vulkan_pipeline_pnext_chain_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_pipeline_pnext_chain_tests PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vulkan_pipeline_pnext_chain_tests PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)
add_test(NAME vulkan_pipeline_pnext_chain_contract COMMAND vulkan_pipeline_pnext_chain_tests)

# M5-E: single-attachment Load/Store/Resolve mapping tests.
add_executable(vulkan_rendering_attachment_tests rendering_attachment_tests.cpp)
target_compile_features(vulkan_rendering_attachment_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_rendering_attachment_tests PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vulkan_rendering_attachment_tests PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)
add_test(NAME vulkan_rendering_attachment_contract COMMAND vulkan_rendering_attachment_tests)

# M5-F: single-image pre/post render transition tests.
add_executable(vulkan_rendering_transition_tests rendering_transition_tests.cpp)
target_compile_features(vulkan_rendering_transition_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_rendering_transition_tests PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vulkan_rendering_transition_tests PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)
add_test(NAME vulkan_rendering_transition_contract COMMAND vulkan_rendering_transition_tests)

# M5-G: classic RenderPass -> rendering signature adapter tests.
add_executable(vulkan_render_pass_signature_tests render_pass_signature_tests.cpp)
target_compile_features(vulkan_render_pass_signature_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_render_pass_signature_tests PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vulkan_render_pass_signature_tests PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)
add_test(NAME vulkan_render_pass_signature_contract COMMAND vulkan_render_pass_signature_tests)

# M5-G: standalone include probe (header must compile on its own).
add_executable(vulkan_render_pass_signature_probe render_pass_signature_tests.cpp)
target_compile_features(vulkan_render_pass_signature_probe PRIVATE cxx_std_17)
target_compile_definitions(vulkan_render_pass_signature_probe PRIVATE VK_NO_PROTOTYPES VKRPSIG_STANDALONE_PROBE)
target_include_directories(vulkan_render_pass_signature_probe PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)
# compile-only probe: no add_test (a main(){return 0;} run is not a contract)

# M5-H: graphics pipeline rendering entry adapter tests.
add_executable(vulkan_pipeline_rendering_adapter_tests pipeline_rendering_adapter_tests.cpp)
target_compile_features(vulkan_pipeline_rendering_adapter_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_pipeline_rendering_adapter_tests PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vulkan_pipeline_rendering_adapter_tests PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)
add_test(NAME vulkan_pipeline_rendering_adapter_contract COMMAND vulkan_pipeline_rendering_adapter_tests)

# M5-H: standalone include probe (compile-only).
add_executable(vulkan_pipeline_rendering_adapter_probe pipeline_rendering_adapter_tests.cpp)
target_compile_features(vulkan_pipeline_rendering_adapter_probe PRIVATE cxx_std_17)
target_compile_definitions(vulkan_pipeline_rendering_adapter_probe PRIVATE VK_NO_PROTOTYPES VKPIPELINERENDER_STANDALONE_PROBE)
target_include_directories(vulkan_pipeline_rendering_adapter_probe PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)

# M5-I: secondary CB begin/inheritance adapter tests.
add_executable(vulkan_secondary_inheritance_adapter_tests secondary_inheritance_adapter_tests.cpp)
target_compile_features(vulkan_secondary_inheritance_adapter_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_secondary_inheritance_adapter_tests PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vulkan_secondary_inheritance_adapter_tests PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)
add_test(NAME vulkan_secondary_inheritance_adapter_contract COMMAND vulkan_secondary_inheritance_adapter_tests)

# M5-I: standalone include probe (compile-only).
add_executable(vulkan_secondary_inheritance_adapter_probe secondary_inheritance_adapter_tests.cpp)
target_compile_features(vulkan_secondary_inheritance_adapter_probe PRIVATE cxx_std_17)
target_compile_definitions(vulkan_secondary_inheritance_adapter_probe PRIVATE VK_NO_PROTOTYPES VKSECINH_STANDALONE_PROBE)
target_include_directories(vulkan_secondary_inheritance_adapter_probe PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)

# M5-J: composite VkRenderingInfo owning contract tests.
add_executable(vulkan_rendering_info_contract_tests rendering_info_contract_tests.cpp)
target_compile_features(vulkan_rendering_info_contract_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_rendering_info_contract_tests PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vulkan_rendering_info_contract_tests PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)
add_test(NAME vulkan_rendering_info_contract COMMAND vulkan_rendering_info_contract_tests)

# M7-A: bounded pipeline cache envelope, retrieval, and atomic file replacement.
add_executable(vulkan_pipeline_cache_tests pipeline_cache_tests.cpp)
target_compile_features(vulkan_pipeline_cache_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_pipeline_cache_tests PRIVATE VK_NO_PROTOTYPES NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(vulkan_pipeline_cache_tests PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include)
add_test(NAME vulkan_pipeline_cache_contract COMMAND vulkan_pipeline_cache_tests)

# M5-J: standalone include probe (compile-only).
add_executable(vulkan_rendering_info_contract_probe rendering_info_contract_tests.cpp)
target_compile_features(vulkan_rendering_info_contract_probe PRIVATE cxx_std_17)
target_compile_definitions(vulkan_rendering_info_contract_probe PRIVATE VK_NO_PROTOTYPES VKRINFO_STANDALONE_PROBE)
target_include_directories(vulkan_rendering_info_contract_probe PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)

# M7-B1: layout key owning value model tests.
add_executable(vulkan_layout_key_tests layout_key_tests.cpp)
target_compile_features(vulkan_layout_key_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_layout_key_tests PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vulkan_layout_key_tests PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)
add_test(NAME vulkan_layout_key_contract COMMAND vulkan_layout_key_tests)

# M7-C1: shader stage key owning value model tests.
add_executable(vulkan_shader_stage_key_tests shader_stage_key_tests.cpp)
target_compile_features(vulkan_shader_stage_key_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_shader_stage_key_tests PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vulkan_shader_stage_key_tests PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)
add_test(NAME vulkan_shader_stage_key_contract COMMAND vulkan_shader_stage_key_tests)

# M7-C1: standalone include probe (compile-only).
add_executable(vulkan_shader_stage_key_probe shader_stage_key_tests.cpp)
target_compile_features(vulkan_shader_stage_key_probe PRIVATE cxx_std_17)
target_compile_definitions(vulkan_shader_stage_key_probe PRIVATE VK_NO_PROTOTYPES VKSHADERKEY_STANDALONE_PROBE)
target_include_directories(vulkan_shader_stage_key_probe PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)


# M7-B1: standalone include probe (compile-only).
add_executable(vulkan_layout_key_probe layout_key_tests.cpp)
target_compile_features(vulkan_layout_key_probe PRIVATE cxx_std_17)
target_compile_definitions(vulkan_layout_key_probe PRIVATE VK_NO_PROTOTYPES VKLAYOUTKEY_STANDALONE_PROBE)
target_include_directories(vulkan_layout_key_probe PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)

# M7-D1: compute pipeline key owning value model tests.
add_executable(vulkan_compute_pipeline_key_tests compute_pipeline_key_tests.cpp)
target_compile_features(vulkan_compute_pipeline_key_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_compute_pipeline_key_tests PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vulkan_compute_pipeline_key_tests PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)
add_test(NAME vulkan_compute_pipeline_key_contract COMMAND vulkan_compute_pipeline_key_tests)

# M7-E1: conservative RenderPass2 compatibility content, CPU only.
add_executable(vulkan_classic_render_pass_key_tests classic_render_pass_key_tests.cpp)
target_compile_features(vulkan_classic_render_pass_key_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_classic_render_pass_key_tests PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vulkan_classic_render_pass_key_tests PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include)
add_test(NAME vulkan_classic_render_pass_key_contract COMMAND vulkan_classic_render_pass_key_tests)
add_executable(vulkan_classic_render_pass_key_probe classic_render_pass_key_tests.cpp)
target_compile_features(vulkan_classic_render_pass_key_probe PRIVATE cxx_std_17)
target_compile_definitions(vulkan_classic_render_pass_key_probe PRIVATE VK_NO_PROTOTYPES VKCLASSICKEY_STANDALONE_PROBE)
target_include_directories(vulkan_classic_render_pass_key_probe PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include)

# M7-D1: standalone include probe (compile-only).
add_executable(vulkan_compute_pipeline_key_probe compute_pipeline_key_tests.cpp)
target_compile_features(vulkan_compute_pipeline_key_probe PRIVATE cxx_std_17)
target_compile_definitions(vulkan_compute_pipeline_key_probe PRIVATE VK_NO_PROTOTYPES VKCOMPUTEKEY_STANDALONE_PROBE)
target_include_directories(vulkan_compute_pipeline_key_probe PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)

# M7-F1: VulkanGraphicsRasterKey contract tests (24th runtime contract).
add_executable(vulkan_graphics_raster_key_tests graphics_raster_key_tests.cpp)
target_compile_features(vulkan_graphics_raster_key_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_graphics_raster_key_tests PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vulkan_graphics_raster_key_tests PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include)
add_test(NAME vulkan_graphics_raster_key_contract COMMAND vulkan_graphics_raster_key_tests)

# M7-F2: complete graphics content model; CPU contract and standalone probe.
add_executable(vulkan_graphics_pipeline_key_tests graphics_pipeline_key_tests.cpp)
target_compile_features(vulkan_graphics_pipeline_key_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_graphics_pipeline_key_tests PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vulkan_graphics_pipeline_key_tests PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include)
add_test(NAME vulkan_graphics_pipeline_key_contract COMMAND vulkan_graphics_pipeline_key_tests)
add_executable(vulkan_graphics_pipeline_key_probe graphics_pipeline_key_tests.cpp)
target_compile_features(vulkan_graphics_pipeline_key_probe PRIVATE cxx_std_17)
target_compile_definitions(vulkan_graphics_pipeline_key_probe PRIVATE VK_NO_PROTOTYPES VKGRAPHICSKEY_STANDALONE_PROBE)
target_include_directories(vulkan_graphics_pipeline_key_probe PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include)

# M7-F1: standalone include probe (compile-only).
add_executable(vulkan_graphics_raster_key_probe graphics_raster_key_tests.cpp)
target_compile_features(vulkan_graphics_raster_key_probe PRIVATE cxx_std_17)
target_compile_definitions(vulkan_graphics_raster_key_probe PRIVATE VK_NO_PROTOTYPES VKGRAPHICRASTER_STANDALONE_PROBE)
target_include_directories(vulkan_graphics_raster_key_probe PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)

# M7-G1: VulkanPipelinePrewarmQueue contract tests (26th runtime contract).
add_executable(vulkan_pipeline_prewarm_tests prewarm_queue_tests.cpp)
target_compile_features(vulkan_pipeline_prewarm_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_pipeline_prewarm_tests PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vulkan_pipeline_prewarm_tests PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include)
add_test(NAME vulkan_pipeline_prewarm_contract COMMAND vulkan_pipeline_prewarm_tests)

# M7-G1: standalone include probe (compile-only).
add_executable(vulkan_pipeline_prewarm_probe prewarm_queue_tests.cpp)
target_compile_features(vulkan_pipeline_prewarm_probe PRIVATE cxx_std_17)
target_compile_definitions(vulkan_pipeline_prewarm_probe PRIVATE VK_NO_PROTOTYPES VKPREWARM_STANDALONE_PROBE)
target_include_directories(vulkan_pipeline_prewarm_probe PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include
)

# PERF-B3-F2a: CPU wait phase/identity recorder contract.
add_executable(vulkan_wait_phase_profile_tests wait_phase_profile_tests.cpp)
target_compile_features(vulkan_wait_phase_profile_tests PRIVATE cxx_std_17)
target_compile_definitions(vulkan_wait_phase_profile_tests PRIVATE VK_NO_PROTOTYPES)
target_include_directories(vulkan_wait_phase_profile_tests PRIVATE
    ../../cocos/renderer/gfx-vulkan
    ../vulkan-engine-smoke
    ../../cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/include)
add_test(NAME vulkan_wait_phase_profile_contract COMMAND vulkan_wait_phase_profile_tests)
