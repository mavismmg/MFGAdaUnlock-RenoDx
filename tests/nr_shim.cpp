// SPDX-License-Identifier: MIT
// 1.12.0 (DLSS 5, N0/N2): the module through which MFG Unlock calls nvngx_dlssnr.dll.
//
// The neural rendering snippet answers only callers whose module path contains "nvngx.dll"
// (it returns NVSDK_NGX_Result_FAIL_PlatformError otherwise, before reading any argument), so the
// calls go through this module, built as mfgunlock_nvngx.dll. It forwards and nothing else. Each call
// keeps its result in a volatile local before returning: a tail call would leave the snippet's frame
// with this module's caller as the return address, and the check would see that module instead.
#include <windows.h>

using InitExtFn = int(__cdecl*)(unsigned long long, const wchar_t*, void*, int, const void*);
using CreateFn = int(__cdecl*)(void*, int, void*, void**);
using EvaluateFn = int(__cdecl*)(void*, const void*, const void*, void*);
using ReleaseFn = int(__cdecl*)(void*);
using ShutdownFn = int(__cdecl*)();

#define MFG_SHIM extern "C" __declspec(dllexport) __declspec(noinline)

MFG_SHIM int MfgNr_Init(void* fn, unsigned long long app_id, const wchar_t* data_path, void* device, int sdk,
                        const void* info) {
  volatile int result = reinterpret_cast<InitExtFn>(fn)(app_id, data_path, device, sdk, info);
  return result;
}
MFG_SHIM int MfgNr_Create(void* fn, void* list, int feature, void* parameters, void** handle) {
  volatile int result = reinterpret_cast<CreateFn>(fn)(list, feature, parameters, handle);
  return result;
}
MFG_SHIM int MfgNr_Evaluate(void* fn, void* list, const void* handle, const void* parameters, void* callback) {
  volatile int result = reinterpret_cast<EvaluateFn>(fn)(list, handle, parameters, callback);
  return result;
}
MFG_SHIM int MfgNr_Release(void* fn, void* handle) {
  volatile int result = reinterpret_cast<ReleaseFn>(fn)(handle);
  return result;
}
MFG_SHIM int MfgNr_Shutdown(void* fn) {
  volatile int result = reinterpret_cast<ShutdownFn>(fn)();
  return result;
}
// The ABI this shim forwards with; the caller refuses another.
MFG_SHIM int MfgNr_AbiVersion() { return 1; }

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
