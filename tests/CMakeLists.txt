cmake_minimum_required(VERSION 3.27)
project(mfgunlock_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(RENODX_SOURCE_DIR "" CACHE PATH "Existing RenoDX checkout with dependencies")
if(NOT EXISTS "${RENODX_SOURCE_DIR}/external/Streamline/include/sl.h")
  message(FATAL_ERROR "Set RENODX_SOURCE_DIR to the existing RenoDX build tree")
endif()
enable_testing()
add_library(mfgunlock_addon_compile OBJECT
  ../src/addons/mfgunlock/addon.cpp)
target_compile_definitions(mfgunlock_addon_compile PRIVATE
  WIN32 _WINDOWS NDEBUG NOMINMAX mfgunlock_EXPORTS)
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
  MFGUNLOCK_CONTINUOUS_BORDER_AXES MFGUNLOCK_RELEASE_1_4_3)
target_compile_options(mfgunlock_local_low_overhead PRIVATE /bigobj)
target_include_directories(mfgunlock_local_low_overhead SYSTEM PRIVATE
  $<TARGET_PROPERTY:mfgunlock_addon_compile,INCLUDE_DIRECTORIES>)
target_link_libraries(mfgunlock_local_low_overhead PRIVATE
  ${MFGUNLOCK_NATIVE_LIBRARIES})
set_target_properties(mfgunlock_local_low_overhead PROPERTIES
  PREFIX "" SUFFIX ".addon64" OUTPUT_NAME "renodx-mfgunlock")
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
  add_test(NAME quality_refinement
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/quality_refinement_tests.py")
  add_test(NAME silhouette_guard
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/silhouette_guard_tests.py")
endif()

# Keep candidate and control identities reviewable; no NVIDIA payload is exported.
if(Python3_Interpreter_FOUND)
  foreach(variant IN ITEMS control border-axis-candidate release-1.4.3)
    if(variant STREQUAL "control")
      set(addon_target mfgunlock_1_4_2_control)
    elseif(variant STREQUAL "release-1.4.3")
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
