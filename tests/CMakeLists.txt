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
add_executable(pacing_policy_tests pacing_policy_tests.cpp)
add_test(NAME pacing_policy_tests COMMAND pacing_policy_tests)
add_executable(force_policy_tests force_policy_tests.cpp)
target_include_directories(force_policy_tests SYSTEM PRIVATE
  "${RENODX_SOURCE_DIR}/external/Streamline/include")
add_test(NAME force_policy_tests COMMAND force_policy_tests)
add_executable(runtime_version_tests runtime_version_tests.cpp)
add_test(NAME runtime_version_tests COMMAND runtime_version_tests)
add_executable(provider_probe provider_probe.cpp)
target_compile_definitions(provider_probe PRIVATE NOMINMAX)
target_include_directories(provider_probe SYSTEM PRIVATE
  "${RENODX_SOURCE_DIR}/external/reshade")
add_executable(blackwell_kernel_tests blackwell_kernel_tests.cpp)
target_compile_definitions(blackwell_kernel_tests PRIVATE NOMINMAX)
target_include_directories(blackwell_kernel_tests SYSTEM PRIVATE
  "${RENODX_SOURCE_DIR}/external/reshade")
add_test(NAME blackwell_kernel_tests COMMAND blackwell_kernel_tests)
add_executable(thin_geometry_tests thin_geometry_tests.cpp)
target_compile_definitions(thin_geometry_tests PRIVATE NOMINMAX)
add_test(NAME thin_geometry_tests COMMAND thin_geometry_tests)
find_program(POWERSHELL_EXECUTABLE NAMES powershell)
if(POWERSHELL_EXECUTABLE)
  add_test(NAME capture_comparison COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile
    -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/compare_tests.ps1")
endif()
