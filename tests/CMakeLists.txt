cmake_minimum_required(VERSION 3.27)
project(mfgunlock_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# ModelPort exposes explicitly asserted release-layout STL views to the Intel
# binary. Debug retains _DEBUG, assertions and /Od; iterator wrappers would
# change those wire layouts. Do not silence the layout assertions with NDEBUG.
add_compile_definitions($<$<CONFIG:Debug>:_ITERATOR_DEBUG_LEVEL=0>)
set(RENODX_SOURCE_DIR "" CACHE PATH "Existing RenoDX checkout with dependencies")
if(NOT EXISTS "${RENODX_SOURCE_DIR}/external/Streamline/include/sl.h")
  message(FATAL_ERROR "Set RENODX_SOURCE_DIR to the existing RenoDX build tree")
endif()
enable_testing()
add_executable(config_store_tests config_store_tests.cpp)
target_compile_definitions(config_store_tests PRIVATE NOMINMAX)
add_test(NAME config_atomic_save COMMAND config_store_tests "${CMAKE_CURRENT_BINARY_DIR}/config-checks")
add_executable(latency_marker_policy_tests latency_marker_policy_tests.cpp)
add_test(NAME latency_marker_admission COMMAND latency_marker_policy_tests)
add_executable(xmx_policy_tests xmx_policy_tests.cpp)
target_compile_definitions(xmx_policy_tests PRIVATE NOMINMAX)
add_test(NAME xmx_policy COMMAND xmx_policy_tests)
add_executable(support_files_tests support_files_tests.cpp)
target_compile_definitions(support_files_tests PRIVATE NOMINMAX)
target_compile_features(support_files_tests PRIVATE cxx_std_20)
add_test(NAME support_files COMMAND support_files_tests)
add_executable(xemfg_compatibility_tests xemfg_compatibility_tests.cpp)
add_test(NAME xemfg_compatibility_policy COMMAND xemfg_compatibility_tests)
add_test(NAME dependency_repair COMMAND powershell -NoProfile -ExecutionPolicy Bypass
  -File "${CMAKE_CURRENT_SOURCE_DIR}/dependency_repair_tests.ps1" -TestRoot "${CMAKE_CURRENT_BINARY_DIR}/dependency-checks")
option(MFGUNLOCK_BUILD_XEMFG "Build the OptiScaler XeMFG companion and dual backend addon" OFF)
set(ENGINEERING_OFFLINE_PROVIDERS "" CACHE STRING "Explicit local provider/plugin inputs for mutation and profile tests")
add_library(mfgunlock_addon_compile OBJECT
  ../src/addons/mfgunlock/addon.cpp)
target_compile_definitions(mfgunlock_addon_compile PRIVATE
  WIN32 _WINDOWS $<$<NOT:$<CONFIG:Debug>>:NDEBUG> NOMINMAX mfgunlock_EXPORTS)
target_compile_options(mfgunlock_addon_compile PRIVATE /bigobj)
target_include_directories(mfgunlock_addon_compile SYSTEM PRIVATE
  "${RENODX_SOURCE_DIR}/external/Detours/include"
  "${RENODX_SOURCE_DIR}/external/json/include"
  "${RENODX_SOURCE_DIR}/external/frozen/include"
  "${RENODX_SOURCE_DIR}/external/gtl/include"
  "${RENODX_SOURCE_DIR}/external/Streamline/include"
  "${RENODX_SOURCE_DIR}/external/Streamline/external/ngx-sdk/include"
  "${RENODX_SOURCE_DIR}/external/DLSS/include"
  "${RENODX_SOURCE_DIR}/external/reshade")
add_library(mfgunlock_1_4_2_control SHARED EXCLUDE_FROM_ALL
  ../src/addons/mfgunlock/addon.cpp)
target_compile_definitions(mfgunlock_1_4_2_control PRIVATE
  $<TARGET_PROPERTY:mfgunlock_addon_compile,COMPILE_DEFINITIONS>
  MFGUNLOCK_LOCAL_LOW_OVERHEAD MFGUNLOCK_LOCAL_STABILITY)
target_compile_options(mfgunlock_1_4_2_control PRIVATE /bigobj)
target_include_directories(mfgunlock_1_4_2_control SYSTEM PRIVATE
  $<TARGET_PROPERTY:mfgunlock_addon_compile,INCLUDE_DIRECTORIES>)
target_link_libraries(mfgunlock_1_4_2_control PRIVATE
  "${RENODX_SOURCE_DIR}/external/Detours/lib.X64/detours.lib"
  user32 gdi32 shell32 ole32 uuid comdlg32 advapi32 version)
get_target_property(MFGUNLOCK_NATIVE_LIBRARIES mfgunlock_1_4_2_control LINK_LIBRARIES)
set_target_properties(mfgunlock_1_4_2_control PROPERTIES
  PREFIX "" SUFFIX ".addon64" OUTPUT_NAME "renodx-mfgunlock-1.4.2-control")
add_library(mfgunlock_local_low_overhead SHARED EXCLUDE_FROM_ALL
  ../src/addons/mfgunlock/addon.cpp)
target_compile_definitions(mfgunlock_local_low_overhead PRIVATE
  $<TARGET_PROPERTY:mfgunlock_1_4_2_control,COMPILE_DEFINITIONS>
  MFGUNLOCK_CONTINUOUS_BORDER_AXES MFGUNLOCK_RELEASE_1_4_3 MFGUNLOCK_RELEASE_1_4_4)
target_compile_options(mfgunlock_local_low_overhead PRIVATE /bigobj)
target_include_directories(mfgunlock_local_low_overhead SYSTEM PRIVATE
  $<TARGET_PROPERTY:mfgunlock_addon_compile,INCLUDE_DIRECTORIES>)
target_link_libraries(mfgunlock_local_low_overhead PRIVATE
  ${MFGUNLOCK_NATIVE_LIBRARIES})
set_target_properties(mfgunlock_local_low_overhead PROPERTIES
  PREFIX "" SUFFIX ".addon64" OUTPUT_NAME "renodx-mfgunlock")
add_library(mfgunlock_engineering_experimental SHARED EXCLUDE_FROM_ALL
  ../src/addons/mfgunlock/addon.cpp)
target_compile_definitions(mfgunlock_engineering_experimental PRIVATE
  $<TARGET_PROPERTY:mfgunlock_local_low_overhead,COMPILE_DEFINITIONS>
  MFGUNLOCK_ENGINEERING_EXPERIMENTAL)
target_compile_options(mfgunlock_engineering_experimental PRIVATE /bigobj)
target_include_directories(mfgunlock_engineering_experimental SYSTEM PRIVATE
  $<TARGET_PROPERTY:mfgunlock_addon_compile,INCLUDE_DIRECTORIES>)
target_link_libraries(mfgunlock_engineering_experimental PRIVATE ${MFGUNLOCK_NATIVE_LIBRARIES})
set_target_properties(mfgunlock_engineering_experimental PROPERTIES
  PREFIX "" SUFFIX ".addon64" OUTPUT_NAME "renodx-mfgunlock-engineering-exp")

# Publish the engineering build under its own release identity; retain the
# experimental lifecycle policy and the historical control targets.
add_library(mfgunlock_release_1_4_5 SHARED EXCLUDE_FROM_ALL
  ../src/addons/mfgunlock/addon.cpp)
target_compile_definitions(mfgunlock_release_1_4_5 PRIVATE
  $<TARGET_PROPERTY:mfgunlock_engineering_experimental,COMPILE_DEFINITIONS>
  MFGUNLOCK_RELEASE_1_4_5)
target_compile_options(mfgunlock_release_1_4_5 PRIVATE /bigobj)
target_include_directories(mfgunlock_release_1_4_5 SYSTEM PRIVATE
  $<TARGET_PROPERTY:mfgunlock_addon_compile,INCLUDE_DIRECTORIES>)
target_link_libraries(mfgunlock_release_1_4_5 PRIVATE ${MFGUNLOCK_NATIVE_LIBRARIES})
set_target_properties(mfgunlock_release_1_4_5 PROPERTIES
  PREFIX "" SUFFIX ".addon64" OUTPUT_NAME "renodx-mfgunlock-1.4.5")

if(MFGUNLOCK_BUILD_XEMFG)
  add_subdirectory(../src/addons/mfgunlock/xemfg xemfg)
endif()

add_executable(release_1_4_5_policy_tests quality_build_policy_tests.cpp)
target_compile_definitions(release_1_4_5_policy_tests PRIVATE
  $<TARGET_PROPERTY:mfgunlock_release_1_4_5,COMPILE_DEFINITIONS>)
target_include_directories(release_1_4_5_policy_tests SYSTEM PRIVATE
  $<TARGET_PROPERTY:mfgunlock_addon_compile,INCLUDE_DIRECTORIES>)
add_test(NAME release_1_4_5_policy COMMAND release_1_4_5_policy_tests)

foreach(engineering_test IN ITEMS patch_transaction queue_ack provider_identity hook_failure stale_hook loader_notify quality_numerical)
  add_executable(${engineering_test}_tests ${engineering_test}_tests.cpp)
  target_compile_definitions(${engineering_test}_tests PRIVATE NOMINMAX
    MFGUNLOCK_LOCAL_LOW_OVERHEAD MFGUNLOCK_LOCAL_STABILITY MFGUNLOCK_CONTINUOUS_BORDER_AXES
    MFGUNLOCK_RELEASE_1_4_3 MFGUNLOCK_RELEASE_1_4_4)
  target_include_directories(${engineering_test}_tests SYSTEM PRIVATE
    $<TARGET_PROPERTY:mfgunlock_addon_compile,INCLUDE_DIRECTORIES>)
  target_link_libraries(${engineering_test}_tests PRIVATE ${MFGUNLOCK_NATIVE_LIBRARIES})
  if(engineering_test STREQUAL "provider_identity")
    add_test(NAME ${engineering_test} COMMAND ${engineering_test}_tests ${ENGINEERING_OFFLINE_PROVIDERS})
    set_tests_properties(${engineering_test} PROPERTIES SKIP_RETURN_CODE 77)
  else()
    add_test(NAME ${engineering_test} COMMAND ${engineering_test}_tests)
  endif()
endforeach()
add_executable(engineering_kernel_emitter engineering_kernel_emitter.cpp)
target_compile_definitions(engineering_kernel_emitter PRIVATE
  $<TARGET_PROPERTY:mfgunlock_engineering_experimental,COMPILE_DEFINITIONS>)
add_executable(engineering_provider_probe provider_probe.cpp)
target_compile_definitions(engineering_provider_probe PRIVATE
  $<TARGET_PROPERTY:mfgunlock_engineering_experimental,COMPILE_DEFINITIONS>)
target_include_directories(engineering_provider_probe SYSTEM PRIVATE
  $<TARGET_PROPERTY:mfgunlock_addon_compile,INCLUDE_DIRECTORIES>)
add_test(NAME presentmon_semantics COMMAND powershell -NoProfile -ExecutionPolicy Bypass
  -File "${CMAKE_CURRENT_SOURCE_DIR}/presentmon_semantics_tests.ps1")
add_library(mfgunlock_border_axis_candidate SHARED EXCLUDE_FROM_ALL
  ../src/addons/mfgunlock/addon.cpp)
target_compile_definitions(mfgunlock_border_axis_candidate PRIVATE
  $<TARGET_PROPERTY:mfgunlock_1_4_2_control,COMPILE_DEFINITIONS>
  MFGUNLOCK_CONTINUOUS_BORDER_AXES)
target_compile_options(mfgunlock_border_axis_candidate PRIVATE /bigobj)
target_include_directories(mfgunlock_border_axis_candidate SYSTEM PRIVATE
  $<TARGET_PROPERTY:mfgunlock_addon_compile,INCLUDE_DIRECTORIES>)
target_link_libraries(mfgunlock_border_axis_candidate PRIVATE
  ${MFGUNLOCK_NATIVE_LIBRARIES})
set_target_properties(mfgunlock_border_axis_candidate PROPERTIES
  PREFIX "" SUFFIX ".addon64" OUTPUT_NAME "renodx-mfgunlock-border-axis-candidate")
add_executable(border_axis_tests border_axis_tests.cpp)
target_compile_definitions(border_axis_tests PRIVATE NOMINMAX)
add_test(NAME border_axis_tests COMMAND border_axis_tests)
add_executable(border_axis_warp_tests local_stability_warp_tests.cpp)
target_compile_definitions(border_axis_warp_tests PRIVATE NOMINMAX
  MFGUNLOCK_LOCAL_LOW_OVERHEAD MFGUNLOCK_LOCAL_STABILITY MFGUNLOCK_CONTINUOUS_BORDER_AXES)
add_test(NAME border_axis_warp_tests COMMAND border_axis_warp_tests)
add_executable(release_warp_tests local_stability_warp_tests.cpp)
target_compile_definitions(release_warp_tests PRIVATE NOMINMAX
  MFGUNLOCK_LOCAL_LOW_OVERHEAD MFGUNLOCK_LOCAL_STABILITY MFGUNLOCK_CONTINUOUS_BORDER_AXES MFGUNLOCK_RELEASE_1_4_3)
add_test(NAME release_warp_tests COMMAND release_warp_tests)
add_executable(stability_runtime_tests stability_runtime_tests.cpp)
target_compile_definitions(stability_runtime_tests PRIVATE NOMINMAX MFGUNLOCK_LOCAL_LOW_OVERHEAD MFGUNLOCK_LOCAL_STABILITY)
target_compile_options(stability_runtime_tests PRIVATE /bigobj)
target_include_directories(stability_runtime_tests SYSTEM PRIVATE
  $<TARGET_PROPERTY:mfgunlock_addon_compile,INCLUDE_DIRECTORIES>)
target_link_libraries(stability_runtime_tests PRIVATE
  ${MFGUNLOCK_NATIVE_LIBRARIES})
add_test(NAME stability_runtime_tests COMMAND stability_runtime_tests)
add_executable(local_stability_warp_tests local_stability_warp_tests.cpp)
target_compile_definitions(local_stability_warp_tests PRIVATE NOMINMAX MFGUNLOCK_LOCAL_STABILITY)
add_test(NAME local_stability_warp_tests COMMAND local_stability_warp_tests)
add_executable(release_isolation_tests release_isolation_tests.cpp)
target_compile_definitions(release_isolation_tests PRIVATE RELEASE_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(NAME release_isolation_tests COMMAND release_isolation_tests)
foreach(policy_variant IN ITEMS local_low_overhead research_baseline border_axis_policy release_policy)
  add_executable(${policy_variant}_tests quality_build_policy_tests.cpp)
  target_compile_definitions(${policy_variant}_tests PRIVATE NOMINMAX)
  target_include_directories(${policy_variant}_tests SYSTEM PRIVATE
    "${RENODX_SOURCE_DIR}/external/reshade")
  add_test(NAME ${policy_variant}_tests COMMAND ${policy_variant}_tests)
endforeach()
target_compile_definitions(local_low_overhead_tests PRIVATE MFGUNLOCK_LOCAL_LOW_OVERHEAD)
target_compile_definitions(border_axis_policy_tests PRIVATE MFGUNLOCK_LOCAL_LOW_OVERHEAD MFGUNLOCK_LOCAL_STABILITY MFGUNLOCK_CONTINUOUS_BORDER_AXES)
target_compile_definitions(release_policy_tests PRIVATE MFGUNLOCK_LOCAL_LOW_OVERHEAD MFGUNLOCK_LOCAL_STABILITY MFGUNLOCK_CONTINUOUS_BORDER_AXES MFGUNLOCK_RELEASE_1_4_3)
add_executable(latency_analysis_tests latency_analysis_tests.cpp)
target_compile_definitions(latency_analysis_tests PRIVATE NOMINMAX)
add_test(NAME latency_analysis_tests COMMAND latency_analysis_tests)
add_executable(runtime_hook_tests runtime_hook_tests.cpp)
target_compile_definitions(runtime_hook_tests PRIVATE NOMINMAX)
target_include_directories(runtime_hook_tests SYSTEM PRIVATE
  "${RENODX_SOURCE_DIR}/external/Streamline/include"
  "${RENODX_SOURCE_DIR}/external/reshade"
  "${RENODX_SOURCE_DIR}/external/Detours/include")
target_link_libraries(runtime_hook_tests PRIVATE
  "${RENODX_SOURCE_DIR}/external/Detours/lib.X64/detours.lib")
add_test(NAME runtime_hook_tests COMMAND runtime_hook_tests)
add_executable(latency_reflex_tests latency_reflex_tests.cpp)
target_compile_definitions(latency_reflex_tests PRIVATE NOMINMAX)
target_include_directories(latency_reflex_tests SYSTEM PRIVATE
  "${RENODX_SOURCE_DIR}/external/Streamline/include"
  "${RENODX_SOURCE_DIR}/external/reshade"
  "${RENODX_SOURCE_DIR}/external/Detours/include")
target_link_libraries(latency_reflex_tests PRIVATE
  "${RENODX_SOURCE_DIR}/external/Detours/lib.X64/detours.lib")
add_test(NAME latency_reflex_tests COMMAND latency_reflex_tests)
add_executable(mfgdiagnostics_tests diagnostics_tests.cpp)
target_compile_definitions(mfgdiagnostics_tests PRIVATE NOMINMAX)
target_compile_options(mfgdiagnostics_tests PRIVATE /bigobj)
target_include_directories(mfgdiagnostics_tests SYSTEM PRIVATE
  "${RENODX_SOURCE_DIR}/external/Streamline/include"
  "${RENODX_SOURCE_DIR}/external/reshade"
  "${RENODX_SOURCE_DIR}/external/json/include"
  "${RENODX_SOURCE_DIR}/external/Detours/include")
target_link_libraries(mfgdiagnostics_tests PRIVATE
  "${RENODX_SOURCE_DIR}/external/Detours/lib.X64/detours.lib")
add_test(NAME mfgdiagnostics_tests COMMAND mfgdiagnostics_tests)
add_executable(hdr_compat_tests hdr_compat_tests.cpp)
target_compile_definitions(hdr_compat_tests PRIVATE NOMINMAX)
target_include_directories(hdr_compat_tests SYSTEM PRIVATE
  "${RENODX_SOURCE_DIR}/external/Streamline/include")
add_test(NAME hdr_compat_tests COMMAND hdr_compat_tests)
add_executable(quality_guard_tests quality_guard_tests.cpp)
target_compile_definitions(quality_guard_tests PRIVATE NOMINMAX)
target_include_directories(quality_guard_tests SYSTEM PRIVATE
  "${RENODX_SOURCE_DIR}/external/Streamline/include")
add_test(NAME quality_guard_tests COMMAND quality_guard_tests)
add_executable(input_diagnostics_tests input_diagnostics_tests.cpp)
target_compile_definitions(input_diagnostics_tests PRIVATE NOMINMAX)
target_include_directories(input_diagnostics_tests SYSTEM PRIVATE
  "${RENODX_SOURCE_DIR}/external/Streamline/include")
add_test(NAME input_diagnostics_tests COMMAND input_diagnostics_tests)
add_executable(pacing_policy_tests pacing_policy_tests.cpp)
add_test(NAME pacing_policy_tests COMMAND pacing_policy_tests)
add_executable(reflex_pacing_tests reflex_pacing_tests.cpp)
target_compile_definitions(reflex_pacing_tests PRIVATE NOMINMAX)
target_include_directories(reflex_pacing_tests SYSTEM PRIVATE
  "${RENODX_SOURCE_DIR}/external/Streamline/include")
add_test(NAME reflex_pacing_tests COMMAND reflex_pacing_tests)
add_executable(memory_policy_tests memory_policy_tests.cpp)
target_include_directories(memory_policy_tests SYSTEM PRIVATE
  "${RENODX_SOURCE_DIR}/external/Streamline/include")
add_test(NAME memory_policy_tests COMMAND memory_policy_tests)
add_executable(force_policy_tests force_policy_tests.cpp)
target_include_directories(force_policy_tests SYSTEM PRIVATE
  "${RENODX_SOURCE_DIR}/external/Streamline/include")
add_test(NAME force_policy_tests COMMAND force_policy_tests)
add_executable(dlss_preset_tests dlss_preset_tests.cpp)
target_include_directories(dlss_preset_tests SYSTEM PRIVATE
  "${RENODX_SOURCE_DIR}/external/Streamline/include")
add_test(NAME dlss_preset_tests COMMAND dlss_preset_tests)
add_executable(gpu_arch_tests gpu_arch_tests.cpp)
target_link_libraries(gpu_arch_tests PRIVATE advapi32)
add_test(NAME gpu_arch_tests COMMAND gpu_arch_tests)
add_executable(ngx_param_vtable_tests ngx_param_vtable_tests.cpp)
add_test(NAME ngx_param_vtable_tests COMMAND ngx_param_vtable_tests)
add_executable(module_scan_tests module_scan_tests.cpp)
add_test(NAME module_scan_tests COMMAND module_scan_tests)
# Plan 5.3: what the loader notification guarantees for the DLSS-G trigger.
add_library(loader_notification_probe_dll SHARED loader_notification_probe_dll.cpp)
add_executable(loader_notification_probe loader_notification_probe.cpp)
add_test(NAME loader_notification_probe COMMAND loader_notification_probe $<TARGET_FILE:loader_notification_probe_dll>)
add_executable(runtime_version_tests runtime_version_tests.cpp)
add_test(NAME runtime_version_tests COMMAND runtime_version_tests)
add_executable(validation_status_tests validation_status_tests.cpp)
add_test(NAME validation_status_tests COMMAND validation_status_tests)
add_executable(provider_probe provider_probe.cpp)
target_compile_definitions(provider_probe PRIVATE NOMINMAX)
target_include_directories(provider_probe SYSTEM PRIVATE
  "${RENODX_SOURCE_DIR}/external/reshade")
add_executable(blackwell_kernel_tests blackwell_kernel_tests.cpp)
target_compile_definitions(blackwell_kernel_tests PRIVATE MFGUNLOCK_LOCAL_STABILITY)
target_compile_definitions(blackwell_kernel_tests PRIVATE NOMINMAX)
target_include_directories(blackwell_kernel_tests SYSTEM PRIVATE
  "${RENODX_SOURCE_DIR}/external/reshade")
add_test(NAME blackwell_kernel_tests COMMAND blackwell_kernel_tests)
add_executable(thin_geometry_tests thin_geometry_tests.cpp)
target_compile_definitions(thin_geometry_tests PRIVATE NOMINMAX)
add_test(NAME thin_geometry_tests COMMAND thin_geometry_tests)
# 1.7 (plan C.3): needs a local 310.9.16 provider; skipped without one.
set(THIN_GEOMETRY_PROVIDER "${CMAKE_SOURCE_DIR}/../../../runtime-310.9.16/vendor/nvngx_dlssg.dll"
  CACHE FILEPATH "Local 310.9.16 nvngx_dlssg.dll for thin_geometry_provider_tests")
add_executable(thin_geometry_provider_tests thin_geometry_provider_tests.cpp)
target_compile_definitions(thin_geometry_provider_tests PRIVATE NOMINMAX)
add_test(NAME thin_geometry_provider_tests COMMAND thin_geometry_provider_tests "${THIN_GEOMETRY_PROVIDER}")
set_tests_properties(thin_geometry_provider_tests PROPERTIES SKIP_RETURN_CODE 77)
add_executable(adaptive_quality_v2_tests adaptive_quality_v2_tests.cpp)
add_test(NAME adaptive_quality_v2_tests COMMAND adaptive_quality_v2_tests)
add_executable(adaptive_inpaint_v3_tests adaptive_inpaint_v3_tests.cpp)
add_test(NAME adaptive_inpaint_v3_tests COMMAND adaptive_inpaint_v3_tests)
add_executable(cuda_temporal_tests cuda_temporal_tests.cpp)
target_compile_definitions(cuda_temporal_tests PRIVATE
  NOMINMAX MFGUNLOCK_CUDA_TEMPORAL_TESTING)
target_include_directories(cuda_temporal_tests SYSTEM PRIVATE
  "${RENODX_SOURCE_DIR}/external/Streamline/include"
  "${RENODX_SOURCE_DIR}/external/reshade"
  "${RENODX_SOURCE_DIR}/external/Detours/include")
target_link_libraries(cuda_temporal_tests PRIVATE
  "${RENODX_SOURCE_DIR}/external/Detours/lib.X64/detours.lib")
add_test(NAME cuda_temporal_tests COMMAND cuda_temporal_tests)
find_program(POWERSHELL_EXECUTABLE NAMES powershell)
if(POWERSHELL_EXECUTABLE)
  add_test(NAME capture_comparison COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile
    -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/compare_tests.ps1")
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
if(Python3_Interpreter_FOUND)
  add_test(NAME research_workflow COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/research_workflow_tests.py")
  add_test(NAME ini_panel_parity COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ini_panel_parity_tests.py")
  add_test(NAME xemfg_replay_corpus COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/xemfg_replay_tests.py")
  add_test(NAME quality_refinement
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/quality_refinement_tests.py")
  add_test(NAME silhouette_guard
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/silhouette_guard_tests.py")
endif()

# Keep candidate and control identities reviewable; no NVIDIA payload is exported.
if(Python3_Interpreter_FOUND)
  foreach(variant IN ITEMS control border-axis-candidate release-1.4.4)
    if(variant STREQUAL "control")
      set(addon_target mfgunlock_1_4_2_control)
    elseif(variant STREQUAL "release-1.4.4")
      set(addon_target mfgunlock_local_low_overhead)
    else()
      set(addon_target mfgunlock_border_axis_candidate)
    endif()
    add_custom_command(TARGET ${addon_target} POST_BUILD
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/write_build_manifest.py"
        --addon "$<TARGET_FILE:${addon_target}>" --source-root "${CMAKE_CURRENT_SOURCE_DIR}/.."
        --variant "${variant}"
      VERBATIM)
  endforeach()
endif()

set_property(TARGET mfgunlock_1_4_2_control mfgunlock_local_low_overhead mfgunlock_border_axis_candidate APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../tools/write_build_manifest.py")

if(Python3_Interpreter_FOUND)
  add_test(NAME engineering_manifest COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/engineering_manifest_tests.py"
    "$<TARGET_FILE:mfgunlock_engineering_experimental>"
    "${CMAKE_CURRENT_BINARY_DIR}/mfgunlock_engineering_experimental.vcxproj")
  set_tests_properties(engineering_manifest PROPERTIES SKIP_RETURN_CODE 77)
  add_test(NAME release_1_4_5_manifest COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/engineering_manifest_tests.py"
    "$<TARGET_FILE:mfgunlock_release_1_4_5>"
    "${CMAKE_CURRENT_BINARY_DIR}/mfgunlock_release_1_4_5.vcxproj"
    "release-1.4.5")
  set_tests_properties(release_1_4_5_manifest PROPERTIES SKIP_RETURN_CODE 77)
endif()
