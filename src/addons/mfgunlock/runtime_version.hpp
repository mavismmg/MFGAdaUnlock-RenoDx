/*
 * Loaded-module version helpers.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

#pragma comment(lib, "version.lib")

namespace mfgunlock::runtimeversion {

struct Version {
  uint16_t major = 0;
  uint16_t minor = 0;
  uint16_t patch = 0;
  uint16_t revision = 0;
  bool valid = false;
};

inline constexpr uint64_t Pack(uint16_t major, uint16_t minor, uint16_t patch,
                               uint16_t revision) {
  return (static_cast<uint64_t>(major) << 48u) |
         (static_cast<uint64_t>(minor) << 32u) |
         (static_cast<uint64_t>(patch) << 16u) | revision;
}

inline constexpr uint64_t Pack(const Version& version) {
  return version.valid
             ? Pack(version.major, version.minor, version.patch, version.revision)
             : 0;
}

inline constexpr bool Is(uint64_t packed, uint16_t major, uint16_t minor,
                         uint16_t patch) {
  return packed != 0 && packed == Pack(major, minor, patch, 0);
}

inline bool IsMappedImage(HMODULE module) {
  if (module == nullptr) return false;
  MEMORY_BASIC_INFORMATION info{};
  return VirtualQuery(module, &info, sizeof(info)) == sizeof(info) &&
         info.AllocationBase == module && info.Type == MEM_IMAGE &&
         info.State == MEM_COMMIT;
}

inline Version FromModule(HMODULE module) {
  Version result{};
  if (!IsMappedImage(module)) return result;

  std::vector<wchar_t> path(32768);
  const DWORD length =
      GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) return result;

  DWORD ignored = 0;
  const DWORD bytes = GetFileVersionInfoSizeW(path.data(), &ignored);
  if (bytes == 0) return result;
  std::vector<unsigned char> storage(bytes);
  if (!GetFileVersionInfoW(path.data(), 0, bytes, storage.data())) return result;

  VS_FIXEDFILEINFO* fixed = nullptr;
  UINT fixed_size = 0;
  if (!VerQueryValueW(storage.data(), L"\\", reinterpret_cast<void**>(&fixed),
                      &fixed_size) ||
      fixed == nullptr || fixed_size < sizeof(VS_FIXEDFILEINFO) ||
      fixed->dwSignature != 0xFEEF04BDu) {
    return result;
  }

  result.major = HIWORD(fixed->dwFileVersionMS);
  result.minor = LOWORD(fixed->dwFileVersionMS);
  result.patch = HIWORD(fixed->dwFileVersionLS);
  result.revision = LOWORD(fixed->dwFileVersionLS);
  result.valid = true;
  return result;
}

inline Version FromAddress(const void* address, HMODULE* owner = nullptr) {
  HMODULE module = nullptr;
  if (address == nullptr ||
      !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(address), &module)) {
    return {};
  }
  if (owner != nullptr) *owner = module;
  return FromModule(module);
}

}  // namespace mfgunlock::runtimeversion
