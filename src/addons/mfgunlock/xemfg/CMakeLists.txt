enable_language(C)
enable_language(RC)
set(XEMFG_SDK_ROOT "" CACHE PATH "Intel XeSS SDK headers")
set(XEMFG_INPUTS "${CMAKE_SOURCE_DIR}/../build/xemfg-inputs" CACHE PATH "Verified, local XeMFG data resources")
set(XEMFG_GENERATED "${CMAKE_SOURCE_DIR}/../build/xemfg-generated" CACHE PATH "Verified program resource table")
if(NOT EXISTS "${XEMFG_SDK_ROOT}/inc/xess_fg/xefg_swapchain_d3d12.h")
  message(FATAL_ERROR "Set XEMFG_SDK_ROOT to the Intel SDK checkout")
endif()
foreach(required IN ITEMS embedded_xess_programs.generated.h embedded_xess_programs.rc)
  if(NOT EXISTS "${XEMFG_GENERATED}/${required}")
    message(FATAL_ERROR "Run prepare_xemfg_inputs.py and vendor/generate_xess_programs.py first")
  endif()
endforeach()
configure_file(notices.rc.in "${CMAKE_CURRENT_BINARY_DIR}/notices.rc" @ONLY)
configure_file(native_notices.rc.in "${CMAKE_CURRENT_BINARY_DIR}/native_notices.rc" @ONLY)
add_test(NAME xemfg_version_resource COMMAND "${CMAKE_COMMAND}" -DRC=${CMAKE_CURRENT_SOURCE_DIR}/native_notices.rc.in
  -P "${CMAKE_SOURCE_DIR}/version_resource_check.cmake")
set(XEMFG_MODEL_SOURCES backend.cpp
  vendor/host_unlock.cpp vendor/runtime_payload.cpp vendor/sdk_pacing.cpp native_xvk_stub.cpp vendor/cuda/cuda_route.cpp
  vendor/third_party/minhook/src/buffer.c vendor/third_party/minhook/src/hook.c
  vendor/third_party/minhook/src/trampoline.c vendor/third_party/minhook/src/hde/hde64.c
  "${XEMFG_GENERATED}/embedded_xess_programs.rc"
  "${XEMFG_INPUTS}/embedded_xess_shaders.rc"
  "${XEMFG_INPUTS}/embedded_xefg_cuda.rc")
add_library(mfgunlock_xemfg SHARED ${XEMFG_MODEL_SOURCES} "${CMAKE_CURRENT_BINARY_DIR}/notices.rc")
target_compile_definitions(mfgunlock_xemfg PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(mfgunlock_xemfg PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/bigobj> /utf-8)
target_compile_options(mfgunlock_xemfg PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/Zi>)
target_link_options(mfgunlock_xemfg PRIVATE /DEBUG:FULL $<$<NOT:$<CONFIG:Debug>>:/OPT:REF> $<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>)
target_include_directories(mfgunlock_xemfg PRIVATE vendor
  "${XEMFG_SDK_ROOT}/inc/xess_fg" "${XEMFG_SDK_ROOT}/inc/xell"
  "${XEMFG_INPUTS}" "${XEMFG_GENERATED}")
target_link_libraries(mfgunlock_xemfg PRIVATE d3d12 dxgi bcrypt version psapi user32)
set_target_properties(mfgunlock_xemfg PROPERTIES PREFIX "" SUFFIX ".asi" OUTPUT_NAME "MFGAdaUnlock.XeMFG"
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(mfgunlock_dual_backend SHARED ../addon.cpp)
target_compile_definitions(mfgunlock_dual_backend PRIVATE
  $<TARGET_PROPERTY:mfgunlock_release_1_4_5,COMPILE_DEFINITIONS> MFGUNLOCK_DUAL_BACKEND)
target_compile_options(mfgunlock_dual_backend PRIVATE /bigobj /utf-8)
target_include_directories(mfgunlock_dual_backend SYSTEM PRIVATE
  $<TARGET_PROPERTY:mfgunlock_addon_compile,INCLUDE_DIRECTORIES>)
target_link_libraries(mfgunlock_dual_backend PRIVATE ${MFGUNLOCK_NATIVE_LIBRARIES})
set_target_properties(mfgunlock_dual_backend PROPERTIES PREFIX "" SUFFIX ".addon64" OUTPUT_NAME "renodx-mfgunlock-dual"
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Plan 4.2: the motion-vector dejitter pass, compiled offline into a header.
file(GLOB XEMFG_FXC_HINTS LIST_DIRECTORIES true "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64")
list(SORT XEMFG_FXC_HINTS ORDER DESCENDING)
find_program(XEMFG_FXC fxc HINTS "$ENV{WindowsSdkVerBinPath}x64" ${XEMFG_FXC_HINTS} REQUIRED)
set(XEMFG_SHADER_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/shaders")
add_custom_command(OUTPUT "${XEMFG_SHADER_HEADERS}/native_dejitter_cs.h"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${XEMFG_SHADER_HEADERS}"
  COMMAND "${XEMFG_FXC}" /nologo /T cs_5_0 /E main /O3 /Vn g_native_dejitter_cs
    /Fh "${XEMFG_SHADER_HEADERS}/native_dejitter_cs.h" "${CMAKE_CURRENT_SOURCE_DIR}/native_dejitter.hlsl"
  DEPENDS native_dejitter.hlsl VERBATIM)
# 1.7 (plan E.2): the HUD stabilization mask.
add_custom_command(OUTPUT "${XEMFG_SHADER_HEADERS}/native_ui_mask_cs.h"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${XEMFG_SHADER_HEADERS}"
  COMMAND "${XEMFG_FXC}" /nologo /T cs_5_0 /E main /O3 /Vn g_native_ui_mask_cs
    /Fh "${XEMFG_SHADER_HEADERS}/native_ui_mask_cs.h" "${CMAKE_CURRENT_SOURCE_DIR}/native_ui_mask.hlsl"
  DEPENDS native_ui_mask.hlsl VERBATIM)
# Plan 4.4: scRGB to HDR10 encoder (fullscreen triangle).
foreach(stage IN ITEMS vs ps)
  string(TOUPPER "${stage}" XEMFG_STAGE)
  set(XEMFG_ENTRY FullscreenVS)
  if(stage STREQUAL "ps")
    set(XEMFG_ENTRY EncodePS)
  endif()
  add_custom_command(OUTPUT "${XEMFG_SHADER_HEADERS}/native_scrgb_${stage}.h"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${XEMFG_SHADER_HEADERS}"
    COMMAND "${XEMFG_FXC}" /nologo /T ${stage}_5_0 /E ${XEMFG_ENTRY} /O3 /Vn g_native_scrgb_${stage}
      /Fh "${XEMFG_SHADER_HEADERS}/native_scrgb_${stage}.h" "${CMAKE_CURRENT_SOURCE_DIR}/native_scrgb.hlsl"
    DEPENDS native_scrgb.hlsl VERBATIM)
endforeach()
set(XEMFG_GPU_PASS_SOURCES native_dejitter.cpp native_hud_copy.cpp "${XEMFG_SHADER_HEADERS}/native_dejitter_cs.h"
  native_ui_mask.cpp "${XEMFG_SHADER_HEADERS}/native_ui_mask_cs.h"
  native_scrgb.cpp "${XEMFG_SHADER_HEADERS}/native_scrgb_vs.h" "${XEMFG_SHADER_HEADERS}/native_scrgb_ps.h")

add_library(mfgunlock_native_xemfg SHARED ../addon.cpp ${XEMFG_MODEL_SOURCES}
  ${XEMFG_GPU_PASS_SOURCES}
  native_bridge.cpp native_factory.cpp native_continuity.cpp native_reframework.cpp native_streamline.cpp native_frame_packet.cpp native_ffx.cpp native_synthetic.cpp native_ngx.cpp native_swapchain.cpp native_latency.cpp native_ui.cpp native_diagnostics.cpp native_session.cpp native_profiles.cpp native_compatibility.cpp native_trace.cpp native_overlay.cpp native_panel.cpp native_controls.cpp native_window.cpp dependency_manager.cpp "${CMAKE_CURRENT_BINARY_DIR}/native_notices.rc")
target_compile_definitions(mfgunlock_native_xemfg PRIVATE
  $<TARGET_PROPERTY:mfgunlock_release_1_4_5,COMPILE_DEFINITIONS>
  MFGUNLOCK_DUAL_BACKEND MFGUNLOCK_NATIVE_XEMFG UNICODE _UNICODE)
target_compile_options(mfgunlock_native_xemfg PRIVATE /bigobj /utf-8 $<$<COMPILE_LANGUAGE:CXX>:/Zi>)
target_include_directories(mfgunlock_native_xemfg PRIVATE vendor
  "${RENODX_SOURCE_DIR}/external/Streamline/external/ngx-sdk/include"
  "${XEMFG_SHADER_HEADERS}"
  "${XEMFG_SDK_ROOT}/inc/xess_fg" "${XEMFG_SDK_ROOT}/inc/xell"
  "${XEMFG_INPUTS}" "${XEMFG_GENERATED}"
  "${RENODX_SOURCE_DIR}/external/reshade/include")
target_include_directories(mfgunlock_native_xemfg SYSTEM PRIVATE
  $<TARGET_PROPERTY:mfgunlock_addon_compile,INCLUDE_DIRECTORIES>)
target_link_libraries(mfgunlock_native_xemfg PRIVATE ${MFGUNLOCK_NATIVE_LIBRARIES} d3d12 dxgi bcrypt psapi delayimp)
# 1.5.2: the DLLs only XeMFG calls load on first use. The DLSS-G backend then
# maps the same imports as 1.4.5 (KERNEL32, USER32, VERSION), also when ReShade
# loads the addon from its own DllMain (ADDON.LoadFromDllMain).
set(XEMFG_DELAY_LOADED d3d12.dll dxgi.dll bcrypt.dll SHELL32.dll)
list(TRANSFORM XEMFG_DELAY_LOADED PREPEND "/DELAYLOAD:" OUTPUT_VARIABLE XEMFG_DELAY_OPTIONS)
target_link_options(mfgunlock_native_xemfg PRIVATE /DEBUG:FULL $<$<NOT:$<CONFIG:Debug>>:/OPT:REF> $<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>
  ${XEMFG_DELAY_OPTIONS})
set_target_properties(mfgunlock_native_xemfg PROPERTIES PREFIX "" SUFFIX ".addon64"
  OUTPUT_NAME "renodx-mfgunlock-native" MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME xemfg_addon_imports COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/xemfg_import_tests.py"
    $<TARGET_FILE:mfgunlock_native_xemfg>)
endif()

# Explicit offline probe only. Never built by ALL, packaged, or installed in a
# game: it records initialization parameter/weight identities for shader audits.
add_library(mfgunlock_native_xemfg_init_probe SHARED EXCLUDE_FROM_ALL
  $<TARGET_PROPERTY:mfgunlock_native_xemfg,SOURCES>)
foreach(prop IN ITEMS COMPILE_DEFINITIONS COMPILE_OPTIONS INCLUDE_DIRECTORIES LINK_LIBRARIES LINK_OPTIONS)
  set_property(TARGET mfgunlock_native_xemfg_init_probe PROPERTY ${prop} $<TARGET_PROPERTY:mfgunlock_native_xemfg,${prop}>)
endforeach()
target_compile_definitions(mfgunlock_native_xemfg_init_probe PRIVATE XESS_INIT_PROBE)
set_target_properties(mfgunlock_native_xemfg_init_probe PROPERTIES PREFIX "" SUFFIX ".addon64"
  OUTPUT_NAME "renodx-mfgunlock-init-probe" MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(xemfg_streamline_fixture SHARED "${CMAKE_SOURCE_DIR}/xemfg_streamline_fixture.cpp")
target_compile_definitions(xemfg_streamline_fixture PRIVATE SL_INTERPOSER)
target_include_directories(xemfg_streamline_fixture PRIVATE "${RENODX_SOURCE_DIR}/external/Streamline/include")
target_link_options(xemfg_streamline_fixture PRIVATE /OPT:NOICF)
set_target_properties(xemfg_streamline_fixture PROPERTIES OUTPUT_NAME "sl.interposer")
add_library(xemfg_ngx_fixture SHARED "${CMAKE_SOURCE_DIR}/xemfg_ngx_fixture.cpp")
target_link_options(xemfg_ngx_fixture PRIVATE /OPT:NOICF)
set_target_properties(xemfg_ngx_fixture PROPERTIES OUTPUT_NAME "_nvngx")
add_executable(xemfg_ngx_smoke "${CMAKE_SOURCE_DIR}/xemfg_ngx_smoke.cpp")
target_compile_definitions(xemfg_ngx_smoke PRIVATE NOMINMAX UNICODE _UNICODE)
target_include_directories(xemfg_ngx_smoke PRIVATE "${RENODX_SOURCE_DIR}/external/Streamline/external/ngx-sdk/include")
target_link_libraries(xemfg_ngx_smoke PRIVATE d3d12 dxgi user32)
add_library(xemfg_ffx_fixture SHARED "${CMAKE_SOURCE_DIR}/xemfg_ffx_fixture.cpp")
# Identical entry bodies must keep distinct addresses, as in a real loader.
target_link_options(xemfg_ffx_fixture PRIVATE /OPT:NOICF)
set_target_properties(xemfg_ffx_fixture PROPERTIES OUTPUT_NAME "amd_fidelityfx_loader_dx12")
add_library(xemfg_sl1_fixture SHARED "${CMAKE_SOURCE_DIR}/xemfg_sl1_fixture.cpp")
target_link_options(xemfg_sl1_fixture PRIVATE /OPT:NOICF)
set_target_properties(xemfg_sl1_fixture PROPERTIES OUTPUT_NAME "sl1.interposer")
add_library(xemfg_observer_fixture SHARED "${CMAKE_SOURCE_DIR}/xemfg_observer_fixture.cpp")
target_include_directories(xemfg_observer_fixture PRIVATE "${RENODX_SOURCE_DIR}/external/Streamline/include")
target_compile_definitions(xemfg_observer_fixture PRIVATE NOMINMAX)
set_target_properties(xemfg_observer_fixture PROPERTIES OUTPUT_NAME "gameoverlayrenderer64")
add_executable(xemfg_native_smoke "${CMAKE_SOURCE_DIR}/xemfg_native_smoke.cpp")
target_compile_definitions(xemfg_native_smoke PRIVATE NOMINMAX UNICODE _UNICODE)
target_include_directories(xemfg_native_smoke PRIVATE "${RENODX_SOURCE_DIR}/external/Streamline/include" "${XEMFG_SDK_ROOT}/inc/xess_fg" "${XEMFG_SDK_ROOT}/inc/xell"
  "${RENODX_SOURCE_DIR}/external/reshade"
  "${RENODX_SOURCE_DIR}/external/reshade/include")
target_link_libraries(xemfg_native_smoke PRIVATE d3d12 dxgi dxguid user32 dbghelp)
# Plan 6.1: the DLSS-G backend against the Streamline fixture.
add_executable(dlssg_streamline_smoke "${CMAKE_SOURCE_DIR}/dlssg_streamline_smoke.cpp")
target_compile_definitions(dlssg_streamline_smoke PRIVATE NOMINMAX UNICODE _UNICODE)
target_include_directories(dlssg_streamline_smoke PRIVATE "${RENODX_SOURCE_DIR}/external/Streamline/include"
  "${RENODX_SOURCE_DIR}/external/Streamline/external/ngx-sdk/include")
target_link_libraries(dlssg_streamline_smoke PRIVATE d3d12 dxgi)
add_executable(xemfg_window_tests "${CMAKE_SOURCE_DIR}/xemfg_window_tests.cpp" native_window.cpp)
target_compile_definitions(xemfg_window_tests PRIVATE NOMINMAX UNICODE _UNICODE)
target_link_libraries(xemfg_window_tests PRIVATE user32)
add_test(NAME xemfg_window_controls COMMAND xemfg_window_tests)
add_library(mfgunlock_window_analysis OBJECT EXCLUDE_FROM_ALL native_window.cpp)
target_compile_definitions(mfgunlock_window_analysis PRIVATE NOMINMAX UNICODE _UNICODE)
target_compile_options(mfgunlock_window_analysis PRIVATE /analyze /W4)
foreach(kind IN ITEMS v1 legacy)
  add_library(xemfg_reframework_${kind} SHARED "${CMAKE_SOURCE_DIR}/xemfg_reframework_fixture.cpp" "${CMAKE_SOURCE_DIR}/xemfg_reframework_fixture.rc")
  target_compile_definitions(xemfg_reframework_${kind} PRIVATE NOMINMAX UNICODE _UNICODE)
  target_link_libraries(xemfg_reframework_${kind} PRIVATE ole32)
  set_target_properties(xemfg_reframework_${kind} PROPERTIES OUTPUT_NAME "reframework-${kind}-fixture")
endforeach()
target_compile_definitions(xemfg_reframework_legacy PRIVATE REF_LEGACY)
add_executable(xemfg_reframework_tests "${CMAKE_SOURCE_DIR}/xemfg_reframework_tests.cpp" native_reframework.cpp)
target_compile_definitions(xemfg_reframework_tests PRIVATE NOMINMAX UNICODE _UNICODE)
target_link_libraries(xemfg_reframework_tests PRIVATE psapi bcrypt version ole32)
add_test(NAME xemfg_reframework_cooperation COMMAND xemfg_reframework_tests $<TARGET_FILE:xemfg_reframework_v1> $<TARGET_FILE:xemfg_reframework_legacy>)
set_tests_properties(xemfg_reframework_cooperation PROPERTIES TIMEOUT 20)
add_executable(xemfg_overlay_shortcut_tests "${CMAKE_SOURCE_DIR}/xemfg_overlay_shortcut_tests.cpp")
target_compile_definitions(xemfg_overlay_shortcut_tests PRIVATE NOMINMAX)
add_test(NAME xemfg_overlay_shortcuts COMMAND xemfg_overlay_shortcut_tests)
set_tests_properties(xemfg_window_controls PROPERTIES TIMEOUT 20)
add_executable(xemfg_latency_tests "${CMAKE_SOURCE_DIR}/xemfg_latency_tests.cpp" native_latency.cpp native_trace.cpp)
target_compile_definitions(xemfg_latency_tests PRIVATE NOMINMAX UNICODE _UNICODE)
target_include_directories(xemfg_latency_tests PRIVATE "${XEMFG_SDK_ROOT}/inc/xess_fg" "${XEMFG_SDK_ROOT}/inc/xell"
  "${RENODX_SOURCE_DIR}/external/Streamline/include")
target_link_libraries(xemfg_latency_tests PRIVATE d3d12 dxgi)
add_test(NAME xemfg_latency_translation COMMAND xemfg_latency_tests)
add_executable(xemfg_constants_tests "${CMAKE_SOURCE_DIR}/xemfg_constants_tests.cpp")
target_compile_definitions(xemfg_constants_tests PRIVATE NOMINMAX)
target_include_directories(xemfg_constants_tests PRIVATE "${XEMFG_SDK_ROOT}/inc/xess_fg" "${RENODX_SOURCE_DIR}/external/Streamline/include")
add_test(NAME xemfg_constants_translation COMMAND xemfg_constants_tests)
add_executable(xemfg_dejitter_numeric "${CMAKE_SOURCE_DIR}/xemfg_dejitter_numeric.cpp" ${XEMFG_GPU_PASS_SOURCES})
target_include_directories(xemfg_dejitter_numeric PRIVATE "${XEMFG_SHADER_HEADERS}")
target_link_libraries(xemfg_dejitter_numeric PRIVATE d3d12 dxgi)
add_test(NAME xemfg_dejitter_numeric COMMAND xemfg_dejitter_numeric)
set_tests_properties(xemfg_dejitter_numeric PROPERTIES TIMEOUT 60)
add_executable(xemfg_ui_mask_numeric "${CMAKE_SOURCE_DIR}/xemfg_ui_mask_numeric.cpp" ${XEMFG_GPU_PASS_SOURCES})
target_include_directories(xemfg_ui_mask_numeric PRIVATE "${XEMFG_SHADER_HEADERS}")
target_link_libraries(xemfg_ui_mask_numeric PRIVATE d3d12 dxgi)
target_compile_definitions(xemfg_ui_mask_numeric PRIVATE NOMINMAX)
add_test(NAME xemfg_ui_mask_numeric COMMAND xemfg_ui_mask_numeric)
set_tests_properties(xemfg_ui_mask_numeric PROPERTIES TIMEOUT 60)
add_executable(xemfg_scrgb_numeric "${CMAKE_SOURCE_DIR}/xemfg_scrgb_numeric.cpp" ${XEMFG_GPU_PASS_SOURCES})
target_include_directories(xemfg_scrgb_numeric PRIVATE "${XEMFG_SHADER_HEADERS}")
target_link_libraries(xemfg_scrgb_numeric PRIVATE d3d12 dxgi)
target_compile_definitions(xemfg_scrgb_numeric PRIVATE NOMINMAX)
add_test(NAME xemfg_scrgb_numeric COMMAND xemfg_scrgb_numeric)
set_tests_properties(xemfg_scrgb_numeric PROPERTIES TIMEOUT 60)
add_executable(xemfg_trace_tests "${CMAKE_SOURCE_DIR}/xemfg_trace_tests.cpp" native_trace.cpp)
target_compile_definitions(xemfg_trace_tests PRIVATE NOMINMAX)
target_link_libraries(xemfg_trace_tests PRIVATE d3d12 dxgi)
add_test(NAME xemfg_stall_trace COMMAND xemfg_trace_tests)
set_tests_properties(xemfg_latency_translation PROPERTIES TIMEOUT 20)
add_executable(xemfg_ui_tests "${CMAKE_SOURCE_DIR}/xemfg_ui_tests.cpp" native_ui.cpp native_trace.cpp)
target_compile_definitions(xemfg_ui_tests PRIVATE NOMINMAX UNICODE _UNICODE)
target_include_directories(xemfg_ui_tests PRIVATE "${XEMFG_SDK_ROOT}/inc/xess_fg" "${XEMFG_SDK_ROOT}/inc/xell"
  "${RENODX_SOURCE_DIR}/external/Streamline/include")
target_link_libraries(xemfg_ui_tests PRIVATE d3d12 dxgi)
add_test(NAME xemfg_ui_composition COMMAND xemfg_ui_tests)
add_executable(xemfg_cadence_tests "${CMAKE_SOURCE_DIR}/xemfg_cadence_tests.cpp")
add_test(NAME xemfg_cadence COMMAND xemfg_cadence_tests)
add_executable(xemfg_hud_input_tests "${CMAKE_SOURCE_DIR}/xemfg_hud_input_tests.cpp")
target_compile_definitions(xemfg_hud_input_tests PRIVATE NOMINMAX)
add_test(NAME xemfg_hud_inputs COMMAND xemfg_hud_input_tests)
add_executable(xemfg_frame_store_tests "${CMAKE_SOURCE_DIR}/xemfg_frame_store_tests.cpp")
target_compile_definitions(xemfg_frame_store_tests PRIVATE NOMINMAX UNICODE _UNICODE)
target_include_directories(xemfg_frame_store_tests PRIVATE "${XEMFG_SDK_ROOT}/inc/xess_fg" "${XEMFG_SDK_ROOT}/inc/xell"
  "${RENODX_SOURCE_DIR}/external/Streamline/include")
add_test(NAME xemfg_frame_store COMMAND xemfg_frame_store_tests)
add_executable(xemfg_profiles_tests "${CMAKE_SOURCE_DIR}/xemfg_profiles_tests.cpp")
target_compile_definitions(xemfg_profiles_tests PRIVATE NOMINMAX UNICODE _UNICODE)
target_include_directories(xemfg_profiles_tests PRIVATE "${RENODX_SOURCE_DIR}/external/json/include")
add_test(NAME xemfg_profiles COMMAND xemfg_profiles_tests "${CMAKE_CURRENT_SOURCE_DIR}/profiles.json")
add_executable(xemfg_async_log_tests "${CMAKE_SOURCE_DIR}/xemfg_async_log_tests.cpp")
add_test(NAME xemfg_async_log COMMAND xemfg_async_log_tests)
add_executable(xemfg_log_budget_tests "${CMAKE_SOURCE_DIR}/xemfg_log_budget_tests.cpp")
add_test(NAME xemfg_log_budget COMMAND xemfg_log_budget_tests)
add_executable(xemfg_display_cap_tests "${CMAKE_SOURCE_DIR}/xemfg_display_cap_tests.cpp")
add_test(NAME xemfg_display_cap COMMAND xemfg_display_cap_tests)
add_executable(xemfg_fence_wait_tests "${CMAKE_SOURCE_DIR}/xemfg_fence_wait_tests.cpp")
target_compile_definitions(xemfg_fence_wait_tests PRIVATE NOMINMAX)
target_link_libraries(xemfg_fence_wait_tests PRIVATE d3d12 dxgi)
add_test(NAME xemfg_fence_wait COMMAND xemfg_fence_wait_tests)
add_executable(xemfg_synthetic_queue_tests "${CMAKE_SOURCE_DIR}/xemfg_synthetic_queue_tests.cpp")
add_test(NAME xemfg_synthetic_queue COMMAND xemfg_synthetic_queue_tests)
add_executable(xemfg_diagnostics_tests "${CMAKE_SOURCE_DIR}/xemfg_diagnostics_tests.cpp" native_diagnostics.cpp)
add_test(NAME xemfg_cpu_diagnostics COMMAND xemfg_diagnostics_tests)
add_executable(xemfg_panel_preview "${CMAKE_SOURCE_DIR}/xemfg_panel_preview.cpp" native_panel.cpp native_controls.cpp native_diagnostics.cpp native_session.cpp native_compatibility.cpp native_trace.cpp native_reframework.cpp dependency_manager.cpp
  "${RENODX_SOURCE_DIR}/external/reshade/deps/imgui/imgui.cpp"
  "${RENODX_SOURCE_DIR}/external/reshade/deps/imgui/imgui_draw.cpp"
  "${RENODX_SOURCE_DIR}/external/reshade/deps/imgui/imgui_tables.cpp"
  "${RENODX_SOURCE_DIR}/external/reshade/deps/imgui/imgui_widgets.cpp")
target_compile_definitions(xemfg_panel_preview PRIVATE NOMINMAX UNICODE _UNICODE MFGUNLOCK_NATIVE_XEMFG MFGUNLOCK_UI_PREVIEW)
target_include_directories(xemfg_panel_preview PRIVATE "${XEMFG_SDK_ROOT}/inc/xess_fg" "${XEMFG_SDK_ROOT}/inc/xell"
  "${RENODX_SOURCE_DIR}/external/Streamline/include" "${RENODX_SOURCE_DIR}/external/reshade"
  "${RENODX_SOURCE_DIR}/external/json/include")
target_link_libraries(xemfg_panel_preview PRIVATE d3d12 dxgi user32 ole32 windowscodecs bcrypt version psapi shell32)
foreach(page RANGE 0 3)
  add_test(NAME xemfg_panel_page_${page} COMMAND xemfg_panel_preview "${CMAKE_BINARY_DIR}/ui-preview/page-${page}.png" ${page} 800)
  set_tests_properties(xemfg_panel_page_${page} PROPERTIES RUN_SERIAL TRUE)
endforeach()
add_test(NAME xemfg_panel_large_font COMMAND xemfg_panel_preview "${CMAKE_BINARY_DIR}/ui-preview/large-font.png" 0 460 22)
set_tests_properties(xemfg_panel_large_font PROPERTIES RUN_SERIAL TRUE)
add_test(NAME xemfg_panel_keyboard_navigation COMMAND xemfg_panel_preview "${CMAKE_BINARY_DIR}/ui-preview/navigation.png" 0 930 20 navigation)
add_test(NAME xemfg_panel_keyboard_navigation_compact COMMAND xemfg_panel_preview "${CMAKE_BINARY_DIR}/ui-preview/navigation-compact.png" 0 460 22 navigation)
set_tests_properties(xemfg_panel_keyboard_navigation xemfg_panel_keyboard_navigation_compact PROPERTIES RUN_SERIAL TRUE)
foreach(scenario IN ITEMS expanded performance pacing tooltip multiplier dependencies)
  set(page 0)
  if(scenario STREQUAL "expanded")
    set(page 2)
  elseif(scenario STREQUAL "performance" OR scenario STREQUAL "dependencies")
    set(page 3)
  endif()
  add_test(NAME xemfg_panel_${scenario} COMMAND xemfg_panel_preview "${CMAKE_BINARY_DIR}/ui-preview/${scenario}.png" ${page} 800 16 ${scenario})
  set_tests_properties(xemfg_panel_${scenario} PROPERTIES RUN_SERIAL TRUE)
endforeach()
set(XEMFG_OFFLINE_RESHADE "" CACHE FILEPATH "Real ReShade addon-enabled DXGI loader for native bridge test")
if(XEMFG_OFFLINE_RESHADE AND XEMFG_OFFLINE_FG AND XEMFG_OFFLINE_LL)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  foreach(scenario IN ITEMS zero-resource jittered-motion simulation-sleep midsim-sleep struct-probe)
    add_test(NAME xemfg_native_${scenario} COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/xemfg_native_tests.py" --directory "${CMAKE_BINARY_DIR}/native-checks"
      --addon $<TARGET_FILE:mfgunlock_native_xemfg> --exe $<TARGET_FILE:xemfg_native_smoke>
      --fixture $<TARGET_FILE:xemfg_streamline_fixture> --reshade "${XEMFG_OFFLINE_RESHADE}"
      --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --${scenario} --active-support)
    set_tests_properties(xemfg_native_${scenario} PROPERTIES RUN_SERIAL TRUE TIMEOUT 150)
  endforeach()
  # Universality conformance matrix (docs/plan/xemfg-plano.md, item 1.9). Each
  # case asserts the target behavior. Cases labelled "backlog" still fail and
  # are excluded from the gate (ctest -LE backlog); a plan item that fixes one
  # removes it from XEMFG_BACKLOG.
  set(XEMFG_CONFORMANCE reflex-v2 reflex-next constants-v3 tag-next backbuffer-subrect
    menu-viewport-first late-interposer sl1-interposer sleep-gap constant-sleep-token late-interposer-hooked
    unsupported-dlssg learned-depth scrgb-output native-game-xefg ffx-adapter dlss-sr-adapter dlss-sr-default-off duplicate-tags reserved-state hud-srgb hud-typeless display-cap scrgb-hudless game-xefg-probe ffx-with-streamline
    ffx-with-reflex ffx-with-dlssg
    # 1.5.8: exclusive fullscreen on a DLSS-SR output (Metro Exodus report),
    # kept by the game and served as borderless.
    exclusive-transition exclusive-first exclusive-borderless exclusive-borderless-first exclusive-borderless-create
    # 1.7.1: evaluations ahead of their Presents (RDR2 on D3D12), and two evaluations in one frame.
    dlss-sr-pipelined dlss-sr-double)
  set(XEMFG_BACKLOG)
  foreach(case IN LISTS XEMFG_CONFORMANCE)
    set(fixture $<TARGET_FILE:xemfg_streamline_fixture>)
    if(case STREQUAL "sl1-interposer")
      set(fixture $<TARGET_FILE:xemfg_sl1_fixture>)
    endif()
    set(extra)
    set(executable $<TARGET_FILE:xemfg_native_smoke>)
    if(case MATCHES "^ffx-")
      set(extra --ffx-fixture $<TARGET_FILE:xemfg_ffx_fixture>)
    endif()
    if(case MATCHES "^dlss-sr-" OR case MATCHES "^exclusive-")
      # No Streamline in this process: a dedicated NGX game executable.
      set(extra --ngx-fixture $<TARGET_FILE:xemfg_ngx_fixture>)
      set(executable $<TARGET_FILE:xemfg_ngx_smoke>)
    endif()
    add_test(NAME xemfg_conformance_${case} COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/xemfg_native_tests.py" --directory "${CMAKE_BINARY_DIR}/native-checks"
      --addon $<TARGET_FILE:mfgunlock_native_xemfg> --exe ${executable}
      --fixture ${fixture} --reshade "${XEMFG_OFFLINE_RESHADE}"
      --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --${case} ${extra})
    set_tests_properties(xemfg_conformance_${case} PROPERTIES RUN_SERIAL TRUE TIMEOUT 150)
    if(case IN_LIST XEMFG_BACKLOG)
      set_tests_properties(xemfg_conformance_${case} PROPERTIES LABELS backlog)
    endif()
  endforeach()
  add_test(NAME dlssg_streamline_fixture COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/dlssg_streamline_tests.py" --directory "${CMAKE_BINARY_DIR}/dlssg-checks"
    --addon $<TARGET_FILE:mfgunlock_native_xemfg> --exe $<TARGET_FILE:dlssg_streamline_smoke>
    --fixture $<TARGET_FILE:xemfg_streamline_fixture> --reshade "${XEMFG_OFFLINE_RESHADE}"
    --ngx-fixture $<TARGET_FILE:xemfg_ngx_fixture>)
  set_tests_properties(dlssg_streamline_fixture PROPERTIES RUN_SERIAL TRUE TIMEOUT 150)
  # 1.5.6: the constant-token game (Plague Tale) on the native Reflex route.
  add_test(NAME xemfg_conformance_reflex-constant-sleep-token COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/xemfg_native_tests.py" --directory "${CMAKE_BINARY_DIR}/native-checks"
    --addon $<TARGET_FILE:mfgunlock_native_xemfg> --exe $<TARGET_FILE:xemfg_native_smoke>
    --fixture $<TARGET_FILE:xemfg_streamline_fixture> --reshade "${XEMFG_OFFLINE_RESHADE}"
    --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --constant-sleep-token --latency-output Reflex)
  set_tests_properties(xemfg_conformance_reflex-constant-sleep-token PROPERTIES RUN_SERIAL TRUE TIMEOUT 150)
  # Real Streamline runtimes: NVIDIA's Streamline_Sample built per SDK release
  # by tools/sl_sample.py under ${XEMFG_SL_SAMPLES_ROOT}/sample-<version>.
  set(XEMFG_SL_SAMPLES_ROOT "" CACHE PATH "Folder with sample-v2.x.y Streamline_Sample builds")
  if(XEMFG_SL_SAMPLES_ROOT)
    foreach(version IN ITEMS v2.7.32 v2.9.0 v2.11.1 v2.14.16)
      add_test(NAME xemfg_sl_sample_${version} COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/xemfg_sl_sample_tests.py" --bin "${XEMFG_SL_SAMPLES_ROOT}/sample-${version}/_bin"
        --addon $<TARGET_FILE:mfgunlock_native_xemfg> --reshade "${XEMFG_OFFLINE_RESHADE}"
        --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}")
      set_tests_properties(xemfg_sl_sample_${version} PROPERTIES RUN_SERIAL TRUE TIMEOUT 120
        LABELS sl_sample SKIP_REGULAR_EXPRESSION "SKIP:")
    endforeach()
  endif()
  add_test(NAME xemfg_native_retained_legacy COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/xemfg_native_tests.py" --directory "${CMAKE_BINARY_DIR}/native-checks"
    --addon $<TARGET_FILE:mfgunlock_native_xemfg> --exe $<TARGET_FILE:xemfg_native_smoke>
    --fixture $<TARGET_FILE:xemfg_streamline_fixture> --reshade "${XEMFG_OFFLINE_RESHADE}"
    --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --legacy-factory --retained-output)
  set_tests_properties(xemfg_native_retained_legacy PROPERTIES RUN_SERIAL TRUE TIMEOUT 150)
  add_test(NAME xemfg_runtime_analysis COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/xemfg_runtime_analysis_tests.py")
  foreach(scenario IN ITEMS observer stale)
    set(extra --stale-input)
    if(scenario STREQUAL "observer")
      set(extra --steam-observer $<TARGET_FILE:xemfg_observer_fixture>)
    endif()
    add_test(NAME xemfg_native_${scenario}_isolation COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/xemfg_native_tests.py" --directory "${CMAKE_BINARY_DIR}/native-checks"
      --addon $<TARGET_FILE:mfgunlock_native_xemfg> --exe $<TARGET_FILE:xemfg_native_smoke>
      --fixture $<TARGET_FILE:xemfg_streamline_fixture> --reshade "${XEMFG_OFFLINE_RESHADE}"
      --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" ${extra})
    set_tests_properties(xemfg_native_${scenario}_isolation PROPERTIES RUN_SERIAL TRUE TIMEOUT 150)
  endforeach()
  add_test(NAME xemfg_native_capcom_flags COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/xemfg_native_tests.py" --directory "${CMAKE_BINARY_DIR}/native-checks"
    --addon $<TARGET_FILE:mfgunlock_native_xemfg> --exe $<TARGET_FILE:xemfg_native_smoke>
    --fixture $<TARGET_FILE:xemfg_streamline_fixture> --reshade "${XEMFG_OFFLINE_RESHADE}"
    --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --capcom-flags)
  set_tests_properties(xemfg_native_capcom_flags PROPERTIES RUN_SERIAL TRUE TIMEOUT 150)
  foreach(scenario IN ITEMS detached not-tracked blocked invalid exception late legacy ambiguous)
    add_test(NAME xemfg_native_ref_${scenario} COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/xemfg_native_tests.py" --directory "${CMAKE_BINARY_DIR}/native-checks"
      --addon $<TARGET_FILE:mfgunlock_native_xemfg> --exe $<TARGET_FILE:xemfg_native_smoke>
      --fixture $<TARGET_FILE:xemfg_streamline_fixture> --reshade "${XEMFG_OFFLINE_RESHADE}"
      --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}"
      --ref-v1 $<TARGET_FILE:xemfg_reframework_v1> --ref-legacy $<TARGET_FILE:xemfg_reframework_legacy> --reframework ${scenario})
    set_tests_properties(xemfg_native_ref_${scenario} PROPERTIES RUN_SERIAL TRUE TIMEOUT 150)
  endforeach()
  add_test(NAME xemfg_native_retire_source_sleep COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/xemfg_native_tests.py" --directory "${CMAKE_BINARY_DIR}/native-checks"
    --addon $<TARGET_FILE:mfgunlock_native_xemfg> --exe $<TARGET_FILE:xemfg_native_smoke>
    --fixture $<TARGET_FILE:xemfg_streamline_fixture> --reshade "${XEMFG_OFFLINE_RESHADE}"
    --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --capcom-flags --latency-output Reflex --retire-sleep)
  set_tests_properties(xemfg_native_retire_source_sleep PROPERTIES RUN_SERIAL TRUE TIMEOUT 40)
  add_test(NAME xemfg_native_producer_shadow COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/xemfg_native_tests.py" --directory "${CMAKE_BINARY_DIR}/native-checks"
    --addon $<TARGET_FILE:mfgunlock_native_xemfg> --exe $<TARGET_FILE:xemfg_native_smoke>
    --fixture $<TARGET_FILE:xemfg_streamline_fixture> --reshade "${XEMFG_OFFLINE_RESHADE}"
    --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --producer-shadow --capcom-flags)
  set_tests_properties(xemfg_native_producer_shadow PROPERTIES RUN_SERIAL TRUE TIMEOUT 150)
  foreach(route IN ITEMS XeLL Reflex)
    add_test(NAME xemfg_native_historical_markers_${route} COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/xemfg_native_tests.py" --directory "${CMAKE_BINARY_DIR}/native-checks"
      --addon $<TARGET_FILE:mfgunlock_native_xemfg> --exe $<TARGET_FILE:xemfg_native_smoke>
      --fixture $<TARGET_FILE:xemfg_streamline_fixture> --reshade "${XEMFG_OFFLINE_RESHADE}"
      --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --legacy-markers --latency-output ${route})
    set_tests_properties(xemfg_native_historical_markers_${route} PROPERTIES RUN_SERIAL TRUE TIMEOUT 150)
    add_test(NAME xemfg_native_tail_sleep_${route} COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/xemfg_native_tests.py" --directory "${CMAKE_BINARY_DIR}/native-checks"
      --addon $<TARGET_FILE:mfgunlock_native_xemfg> --exe $<TARGET_FILE:xemfg_native_smoke>
      --fixture $<TARGET_FILE:xemfg_streamline_fixture> --reshade "${XEMFG_OFFLINE_RESHADE}"
      --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --tail-sleep --latency-output ${route})
    set_tests_properties(xemfg_native_tail_sleep_${route} PROPERTIES RUN_SERIAL TRUE TIMEOUT 150)
  endforeach()
  foreach(scenario IN ITEMS mixed-markers alias-markers stall-trace queue-proxy parallel-factory foreign-refs retained-output secondary-viewport normal-depth hud-conflict)
    add_test(NAME xemfg_native_${scenario} COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/xemfg_native_tests.py" --directory "${CMAKE_BINARY_DIR}/native-checks"
      --addon $<TARGET_FILE:mfgunlock_native_xemfg> --exe $<TARGET_FILE:xemfg_native_smoke>
      --fixture $<TARGET_FILE:xemfg_streamline_fixture> --reshade "${XEMFG_OFFLINE_RESHADE}"
      --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --${scenario})
    set_tests_properties(xemfg_native_${scenario} PROPERTIES RUN_SERIAL TRUE TIMEOUT 150)
  endforeach()
  foreach(route IN ITEMS XeLL Reflex)
    add_test(NAME xemfg_native_legacy_${route} COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/xemfg_native_tests.py" --directory "${CMAKE_BINARY_DIR}/native-checks"
      --addon $<TARGET_FILE:mfgunlock_native_xemfg> --exe $<TARGET_FILE:xemfg_native_smoke>
      --fixture $<TARGET_FILE:xemfg_streamline_fixture> --reshade "${XEMFG_OFFLINE_RESHADE}"
      --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --legacy-factory --latency-output ${route})
    set_tests_properties(xemfg_native_legacy_${route} PROPERTIES RUN_SERIAL TRUE TIMEOUT 150)
  endforeach()
  set(XEMFG_OFFLINE_STREAMLINE "" CACHE PATH "Explicit real Streamline plugin directory for presentation ownership tests")
  if(XEMFG_OFFLINE_STREAMLINE)
    add_test(NAME xemfg_real_streamline_visible COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/xemfg_streamline_recreate.py" --directory "${CMAKE_BINARY_DIR}/native-checks"
      --addon $<TARGET_FILE:mfgunlock_native_xemfg> --exe $<TARGET_FILE:xemfg_native_smoke>
      --reshade "${XEMFG_OFFLINE_RESHADE}" --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}"
      --streamline "${XEMFG_OFFLINE_STREAMLINE}" --generate --early-addon --visible-recreation)
    set_tests_properties(xemfg_real_streamline_visible PROPERTIES RUN_SERIAL TRUE TIMEOUT 85)
    foreach(route IN ITEMS XeLL Reflex)
      add_test(NAME xemfg_real_streamline_legacy_${route} COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/xemfg_streamline_recreate.py" --directory "${CMAKE_BINARY_DIR}/native-checks"
        --addon $<TARGET_FILE:mfgunlock_native_xemfg> --exe $<TARGET_FILE:xemfg_native_smoke>
        --reshade "${XEMFG_OFFLINE_RESHADE}" --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}"
        --streamline "${XEMFG_OFFLINE_STREAMLINE}" --generate --early-addon --legacy-factory --latency-output ${route})
      set_tests_properties(xemfg_real_streamline_legacy_${route} PROPERTIES RUN_SERIAL TRUE TIMEOUT 85)
    endforeach()
    foreach(route IN ITEMS XeLL Reflex)
      foreach(order IN ITEMS early late)
        set(startup_flags)
        if(order STREQUAL "early")
          list(APPEND startup_flags --early-addon)
        endif()
        add_test(NAME xemfg_real_streamline_${route}_${order} COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_SOURCE_DIR}/xemfg_streamline_recreate.py" --directory "${CMAKE_BINARY_DIR}/native-checks"
          --addon $<TARGET_FILE:mfgunlock_native_xemfg> --exe $<TARGET_FILE:xemfg_native_smoke>
          --reshade "${XEMFG_OFFLINE_RESHADE}" --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}"
          --streamline "${XEMFG_OFFLINE_STREAMLINE}" --generate --latency-output ${route} ${startup_flags})
        set_tests_properties(xemfg_real_streamline_${route}_${order} PROPERTIES RUN_SERIAL TRUE TIMEOUT 85)
      endforeach()
    endforeach()
  endif()
  foreach(layout IN ITEMS storage-only preloaded-storage mutated-storage missing)
    add_test(NAME xemfg_native_runtime_${layout} COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/xemfg_native_tests.py" --directory "${CMAKE_BINARY_DIR}/native-checks"
      --addon $<TARGET_FILE:mfgunlock_native_xemfg> --exe $<TARGET_FILE:xemfg_native_smoke>
      --fixture $<TARGET_FILE:xemfg_streamline_fixture> --reshade "${XEMFG_OFFLINE_RESHADE}"
      --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --runtime-layout ${layout})
    set_tests_properties(xemfg_native_runtime_${layout} PROPERTIES RUN_SERIAL TRUE TIMEOUT 150)
  endforeach()
  # 1.5.5: a game presenting with VSync is reported (and overridden only when
  # the user turns on ForceVSyncOff); native_summary is written meanwhile.
  foreach(vsync IN ITEMS reported forced-off)
    set(vsync_flags --game-vsync)
    if(vsync STREQUAL "forced-off")
      list(APPEND vsync_flags --force-vsync-off)
    endif()
    add_test(NAME xemfg_native_game_vsync_${vsync} COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/xemfg_native_tests.py" --directory "${CMAKE_BINARY_DIR}/native-checks"
      --addon $<TARGET_FILE:mfgunlock_native_xemfg> --exe $<TARGET_FILE:xemfg_native_smoke>
      --fixture $<TARGET_FILE:xemfg_streamline_fixture> --reshade "${XEMFG_OFFLINE_RESHADE}"
      --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" ${vsync_flags} --active-support)
    set_tests_properties(xemfg_native_game_vsync_${vsync} PROPERTIES RUN_SERIAL TRUE TIMEOUT 150)
  endforeach()
  foreach(multiplier IN ITEMS 2 3 4 6)
  add_test(NAME xemfg_native_reshade_${multiplier}x COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/xemfg_native_tests.py" --directory "${CMAKE_BINARY_DIR}/native-checks"
    --addon $<TARGET_FILE:mfgunlock_native_xemfg> --exe $<TARGET_FILE:xemfg_native_smoke>
    --fixture $<TARGET_FILE:xemfg_streamline_fixture> --reshade "${XEMFG_OFFLINE_RESHADE}"
    --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --multiplier ${multiplier} --overlay-keys)
  set_tests_properties(xemfg_native_reshade_${multiplier}x PROPERTIES TIMEOUT 150 RUN_SERIAL TRUE)
  endforeach()
  foreach(multiplier IN ITEMS 2 3 4 6)
    add_test(NAME xemfg_native_reflex_${multiplier}x COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/xemfg_native_tests.py" --directory "${CMAKE_BINARY_DIR}/native-checks"
      --addon $<TARGET_FILE:mfgunlock_native_xemfg> --exe $<TARGET_FILE:xemfg_native_smoke>
      --fixture $<TARGET_FILE:xemfg_streamline_fixture> --reshade "${XEMFG_OFFLINE_RESHADE}"
      --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --multiplier ${multiplier} --latency-output Reflex)
    set_tests_properties(xemfg_native_reflex_${multiplier}x PROPERTIES TIMEOUT 150 RUN_SERIAL TRUE)
  endforeach()
endif()

add_executable(xemfg_provider_probe "${CMAKE_SOURCE_DIR}/xemfg_provider_probe.cpp"
  vendor/host_unlock.cpp vendor/runtime_payload.cpp
  "${XEMFG_GENERATED}/embedded_xess_programs.rc" "${XEMFG_INPUTS}/embedded_xess_shaders.rc")
target_compile_definitions(xemfg_provider_probe PRIVATE NOMINMAX UNICODE _UNICODE)
target_include_directories(xemfg_provider_probe PRIVATE vendor "${XEMFG_INPUTS}" "${XEMFG_GENERATED}")
target_link_libraries(xemfg_provider_probe PRIVATE bcrypt)
set(XEMFG_OFFLINE_FG "" CACHE FILEPATH "Explicit local XeSS-FG provider to probe")
set(XEMFG_OFFLINE_LL "" CACHE FILEPATH "Explicit local XeLL provider to probe")
if(XEMFG_OFFLINE_FG AND XEMFG_OFFLINE_LL)
  add_test(NAME xemfg_provider_patch_restore COMMAND xemfg_provider_probe "${XEMFG_OFFLINE_FG}" "${XEMFG_OFFLINE_LL}")
endif()
add_library(xemfg_host_fixture SHARED "${CMAKE_SOURCE_DIR}/xemfg_host_fixture.cpp" "${CMAKE_SOURCE_DIR}/xemfg_host_fixture.rc"
  vendor/third_party/minhook/src/buffer.c vendor/third_party/minhook/src/hook.c
  vendor/third_party/minhook/src/trampoline.c vendor/third_party/minhook/src/hde/hde64.c)
target_compile_definitions(xemfg_host_fixture PRIVATE UNICODE _UNICODE NOMINMAX)
target_include_directories(xemfg_host_fixture PRIVATE "${XEMFG_SDK_ROOT}/inc/xess_fg" "${XEMFG_SDK_ROOT}/inc/xell")
target_link_libraries(xemfg_host_fixture PRIVATE d3d12 dxgi user32)
add_executable(xemfg_runtime_smoke "${CMAKE_SOURCE_DIR}/xemfg_runtime_smoke.cpp")
add_executable(xemfg_loader_smoke "${CMAKE_SOURCE_DIR}/xemfg_loader_smoke.cpp")
target_compile_definitions(xemfg_loader_smoke PRIVATE NOMINMAX UNICODE _UNICODE)
target_include_directories(xemfg_loader_smoke PRIVATE "${XEMFG_SDK_ROOT}/inc/xess_fg")
target_link_libraries(xemfg_loader_smoke PRIVATE "${XEMFG_SDK_ROOT}/lib/libxess_fg.lib")
add_executable(xemfg_optiscaler_present_smoke "${CMAKE_SOURCE_DIR}/xemfg_optiscaler_present_smoke.cpp")
target_compile_definitions(xemfg_optiscaler_present_smoke PRIVATE NOMINMAX UNICODE _UNICODE)
target_include_directories(xemfg_optiscaler_present_smoke PRIVATE "${XEMFG_SDK_ROOT}/inc/xess_fg")
target_link_libraries(xemfg_optiscaler_present_smoke PRIVATE "${XEMFG_SDK_ROOT}/lib/libxess_fg.lib" d3d12 dxgi user32)
add_library(xemfg_foreign_fixture SHARED "${CMAKE_SOURCE_DIR}/xemfg_foreign_fixture.cpp")
target_include_directories(xemfg_foreign_fixture PRIVATE "${XEMFG_SDK_ROOT}/inc/xess_fg")
target_link_libraries(xemfg_foreign_fixture PRIVATE "${XEMFG_SDK_ROOT}/lib/libxess_fg.lib")
add_executable(xemfg_module_snapshot_tests "${CMAKE_SOURCE_DIR}/xemfg_module_snapshot_tests.cpp")
target_compile_definitions(xemfg_module_snapshot_tests PRIVATE NOMINMAX UNICODE _UNICODE)
target_link_libraries(xemfg_module_snapshot_tests PRIVATE version)
add_test(NAME xemfg_module_snapshot COMMAND xemfg_module_snapshot_tests $<TARGET_FILE:xemfg_host_fixture>)
set(XEMFG_OFFLINE_HOST "" CACHE FILEPATH "Explicit OptiScaler 10 loader for the cold-start regression test")
if(XEMFG_OFFLINE_HOST AND XEMFG_OFFLINE_FG AND XEMFG_OFFLINE_LL)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME xemfg_loader_startup COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/xemfg_startup_tests.py"
    --mode loader --directory "${CMAKE_BINARY_DIR}/xemfg-checks"
    --plugin $<TARGET_FILE:mfgunlock_xemfg> --executable $<TARGET_FILE:xemfg_loader_smoke>
    --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --host "${XEMFG_OFFLINE_HOST}")
  add_test(NAME xemfg_early_sdk_lifecycle COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/xemfg_startup_tests.py"
    --mode early-lifecycle --directory "${CMAKE_BINARY_DIR}/xemfg-checks"
    --plugin $<TARGET_FILE:mfgunlock_xemfg> --executable $<TARGET_FILE:xemfg_runtime_smoke>
    --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --host $<TARGET_FILE:xemfg_host_fixture>)
  add_test(NAME xemfg_host_redirect_lifecycle COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/xemfg_startup_tests.py"
    --mode redirect-lifecycle --directory "${CMAKE_BINARY_DIR}/xemfg-checks"
    --plugin $<TARGET_FILE:mfgunlock_xemfg> --executable $<TARGET_FILE:xemfg_runtime_smoke>
    --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --host $<TARGET_FILE:xemfg_host_fixture>)
  add_test(NAME xemfg_optiscaler_present COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/xemfg_startup_tests.py"
    --mode optiscaler-present --directory "${CMAKE_BINARY_DIR}/xemfg-checks"
    --plugin $<TARGET_FILE:mfgunlock_xemfg> --executable $<TARGET_FILE:xemfg_optiscaler_present_smoke>
    --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --host "${XEMFG_OFFLINE_HOST}")
  add_test(NAME xemfg_native_context_publication_guard COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/xemfg_startup_tests.py"
    --mode optiscaler-live-native --directory "${CMAKE_BINARY_DIR}/xemfg-checks"
    --plugin $<TARGET_FILE:mfgunlock_xemfg> --executable $<TARGET_FILE:xemfg_optiscaler_present_smoke>
    --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --host "${XEMFG_OFFLINE_HOST}"
    --foreign-fixture $<TARGET_FILE:xemfg_foreign_fixture>)
  add_test(NAME xemfg_game_first_shared_model COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/xemfg_startup_tests.py"
    --mode optiscaler-game-first --directory "${CMAKE_BINARY_DIR}/xemfg-checks"
    --plugin $<TARGET_FILE:mfgunlock_xemfg> --executable $<TARGET_FILE:xemfg_optiscaler_present_smoke>
    --sdk-fg "${XEMFG_OFFLINE_FG}" --sdk-ll "${XEMFG_OFFLINE_LL}" --host "${XEMFG_OFFLINE_HOST}")
  set_tests_properties(xemfg_loader_startup xemfg_early_sdk_lifecycle xemfg_host_redirect_lifecycle xemfg_optiscaler_present xemfg_native_context_publication_guard xemfg_game_first_shared_model PROPERTIES TIMEOUT 45)
endif()

add_executable(xemfg_reshade_panel_input "${CMAKE_SOURCE_DIR}/xemfg_reshade_panel_input.cpp")
target_compile_definitions(xemfg_reshade_panel_input PRIVATE NOMINMAX UNICODE _UNICODE)
target_include_directories(xemfg_reshade_panel_input PRIVATE "${RENODX_SOURCE_DIR}/external/reshade" "${RENODX_SOURCE_DIR}/external/reshade/include")
target_link_libraries(xemfg_reshade_panel_input PRIVATE d3d12 dxgi user32)

add_subdirectory("${CMAKE_SOURCE_DIR}/xemfg_cuda" "${CMAKE_BINARY_DIR}/xemfg-cuda")

# Targets that include native_internal.hpp own an Output, hence the pass.
foreach(target IN ITEMS xemfg_latency_tests xemfg_ui_tests xemfg_panel_preview)
  target_sources(${target} PRIVATE ${XEMFG_GPU_PASS_SOURCES})
  target_include_directories(${target} PRIVATE "${XEMFG_SHADER_HEADERS}")
endforeach()

# 1.7 (plan F): the xvk D3D12-on-Vulkan layer (vendor/xvk, from dashdogy's MFG
# Unlocker v1.7.0, MIT) for XeMFG on Vulkan games. Built only when the Khronos
# Vulkan headers and the locally extracted SPIR-V program set exist
# (tools/extract_xvk_programs.py): the SPIR-V derives from Intel's DXIL and is
# never committed.
set(XEMFG_VULKAN_HEADERS "${CMAKE_SOURCE_DIR}/../../../deps/vulkan-headers/include" CACHE PATH
  "Khronos Vulkan-Headers include directory (1.3 or later)")
set(XEMFG_XVK_PROGRAMS "${CMAKE_SOURCE_DIR}/../../../deps/xvk-programs" CACHE PATH
  "Folder written by tools/extract_xvk_programs.py")
if(EXISTS "${XEMFG_VULKAN_HEADERS}/vulkan/vulkan.h" AND EXISTS "${XEMFG_XVK_PROGRAMS}/programs.json")
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  set(XEMFG_XVK_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/xvk")
  add_custom_command(OUTPUT "${XEMFG_XVK_GENERATED}/embedded_xvk_programs.generated.h" "${XEMFG_XVK_GENERATED}/embedded_xvk_programs.rc"
      "${XEMFG_XVK_GENERATED}/vk_input_copy_spv.h"
    COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/../../../../tools/generate_xvk_programs.py"
      "${XEMFG_XVK_PROGRAMS}" "${XEMFG_XVK_GENERATED}"
    DEPENDS "${XEMFG_XVK_PROGRAMS}/programs.json" "${XEMFG_XVK_PROGRAMS}/SHA256SUMS.txt"
      "${XEMFG_XVK_PROGRAMS}/vk_input_copy_0.spv" "${XEMFG_XVK_PROGRAMS}/vk_input_copy_1.spv"
      "${XEMFG_XVK_PROGRAMS}/vk_input_copy_2.spv"
      "${CMAKE_CURRENT_SOURCE_DIR}/../../../../tools/generate_xvk_programs.py" VERBATIM)
  file(GLOB XEMFG_XVK_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/xvk/*.cpp")
  add_library(xemfg_xvk STATIC ${XEMFG_XVK_SOURCES} "${XEMFG_XVK_GENERATED}/embedded_xvk_programs.generated.h")
  target_include_directories(xemfg_xvk PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/vendor/xvk" "${XEMFG_XVK_GENERATED}"
    "${XEMFG_VULKAN_HEADERS}")
  target_compile_definitions(xemfg_xvk PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(xemfg_xvk PRIVATE /utf-8 /bigobj)
  set_target_properties(xemfg_xvk PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_link_libraries(xemfg_xvk PUBLIC d3d12 dxgi bcrypt)
  set(XEMFG_XVK_AVAILABLE ON)
  # The Vulkan host (vendor/xvkhost, same release) and the addon's vulkan-1 and
  # NGX Vulkan hooks (native_vulkan.cpp). The program set is embedded as RCDATA
  # 7400+ in the addon itself (vk_host passes its own module to xvk).
  target_sources(mfgunlock_native_xemfg PRIVATE vendor/xvkhost/vk_host.cpp native_vulkan.cpp
    "${XEMFG_XVK_GENERATED}/vk_input_copy_spv.h" "${XEMFG_XVK_GENERATED}/embedded_xvk_programs.rc")
  target_include_directories(mfgunlock_native_xemfg PRIVATE vendor/xvkhost)
  target_compile_definitions(mfgunlock_native_xemfg PRIVATE MFGUNLOCK_XEMFG_VULKAN)
  target_link_libraries(mfgunlock_native_xemfg PRIVATE xemfg_xvk)
  add_executable(xemfg_vulkan_policy_tests "${CMAKE_SOURCE_DIR}/xemfg_vulkan_policy_tests.cpp")
  target_include_directories(xemfg_vulkan_policy_tests PRIVATE "${XEMFG_VULKAN_HEADERS}")
  set_target_properties(xemfg_vulkan_policy_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
  add_test(NAME xemfg_vulkan_policy COMMAND xemfg_vulkan_policy_tests)
endif()
