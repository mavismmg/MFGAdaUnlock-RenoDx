// SPDX-License-Identifier: MIT
/*
 * 1.7 (plan C): which NVIDIA GPU family this machine has, before the DLSS-G
 * provider is patched.
 *
 * The arch-gate rewrite and the quality kernels exist for Ada: the gates let
 * AD10x take the Blackwell path, and the kernels replace the provider's sm_89
 * cubins. An RTX 50 already takes that path and executes sm_120 cubins, so
 * neither change does anything useful there.
 *
 * Provider patching runs from the LoadLibrary hook, under the Windows loader
 * lock, where initializing NVAPI could deadlock. The display-adapter class
 * key in the registry is safe to read there: each installed adapter stores its
 * PCI identity in MatchingDeviceId ("pci\ven_10de&dev_2c05"). The PCI device
 * ID ranges below are NVIDIA's AD10x and GB20x allocations. Anything outside
 * them is Unknown, and Unknown keeps the pre-1.7 behaviour.
 */
#pragma once

#include <windows.h>

#include <cstdint>
#include <cwchar>
#include <iterator>

namespace mfgunlock::gpuarch {

enum class Family : uint32_t { kUnknown = 0, kAda = 1, kBlackwell = 2 };

// AD102..AD107, desktop and laptop: 0x2680-0x28FF.
// GB202..GB207 (RTX 50 and RTX PRO Blackwell): 0x2B80-0x2FFF. GB100/GB200
// data-centre parts sit at 0x29xx and are deliberately left Unknown.
constexpr Family ClassifyDeviceId(uint32_t device_id) {
  if (device_id >= 0x2680 && device_id < 0x2900) return Family::kAda;
  if (device_id >= 0x2B80 && device_id < 0x3000) return Family::kBlackwell;
  return Family::kUnknown;
}

// 1.8.3: GPUs NVIDIA's DLSS-G runtime refuses outright: Pascal, Turing (RTX 20
// and GTX 16) and Ampere (RTX 30), desktop and laptop, 0x1B00-0x267F. Kept
// apart from Family so the Ada and Blackwell decisions above never change.
constexpr bool IsPreAdaDeviceId(uint32_t device_id) {
  return device_id >= 0x1B00 && device_id < 0x2680;
}

// Parses "pci\ven_10de&dev_XXXX..." (any case). Returns false for another
// vendor or a malformed string.
constexpr bool ParseNvidiaDeviceId(const wchar_t* text, uint32_t& device_id) {
  if (text == nullptr) return false;
  constexpr const wchar_t* kPrefix = L"pci\\ven_10de&dev_";
  size_t i = 0;
  for (; kPrefix[i] != 0; ++i) {
    wchar_t c = text[i];
    if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    if (c != kPrefix[i]) return false;
  }
  uint32_t value = 0;
  for (size_t digit = 0; digit < 4; ++digit) {
    const wchar_t c = text[i + digit];
    uint32_t nibble;
    if (c >= L'0' && c <= L'9') nibble = static_cast<uint32_t>(c - L'0');
    else if (c >= L'a' && c <= L'f') nibble = static_cast<uint32_t>(c - L'a' + 10);
    else if (c >= L'A' && c <= L'F') nibble = static_cast<uint32_t>(c - L'A' + 10);
    else return false;
    value = (value << 4) | nibble;
  }
  device_id = value;
  return true;
}

// Combines every NVIDIA adapter: the result is Blackwell only when every
// NVIDIA adapter is Blackwell. One Ada or Unknown adapter keeps the Ada
// behaviour, because the game may render on it.
struct Census {
  uint32_t nvidia = 0;
  uint32_t blackwell = 0;
  uint32_t pre_ada = 0;
  void Add(Family family) {
    ++nvidia;
    if (family == Family::kBlackwell) ++blackwell;
  }
  void AddDevice(uint32_t device_id) {
    Add(ClassifyDeviceId(device_id));
    if (IsPreAdaDeviceId(device_id)) ++pre_ada;
  }
  constexpr bool AllBlackwell() const { return nvidia != 0 && blackwell == nvidia; }
  // Every NVIDIA adapter predates Ada: none can run NVIDIA's frame generation.
  constexpr bool AllPreAda() const { return nvidia != 0 && pre_ada == nvidia; }
};

// The registry functions are taken from kernelbase.dll, which every process
// has mapped: no static advapi32 import (the addon's import policy) and no
// library load under the loader lock.
inline Census ReadInstalledAdapters() {
  Census census;
  const HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
  if (kernelbase == nullptr) return census;
  using OpenKey = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
  using EnumKey = LSTATUS(WINAPI*)(HKEY, DWORD, LPWSTR, LPDWORD, LPDWORD,
                                   LPWSTR, LPDWORD, PFILETIME);
  using GetValue = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD,
                                    PVOID, LPDWORD);
  using CloseKey = LSTATUS(WINAPI*)(HKEY);
  const auto open = reinterpret_cast<OpenKey>(
      GetProcAddress(kernelbase, "RegOpenKeyExW"));
  const auto enumerate = reinterpret_cast<EnumKey>(
      GetProcAddress(kernelbase, "RegEnumKeyExW"));
  const auto get = reinterpret_cast<GetValue>(
      GetProcAddress(kernelbase, "RegGetValueW"));
  const auto close = reinterpret_cast<CloseKey>(
      GetProcAddress(kernelbase, "RegCloseKey"));
  if (!open || !enumerate || !get || !close) return census;
  HKEY klass = nullptr;
  if (open(HKEY_LOCAL_MACHINE,
           L"SYSTEM\\CurrentControlSet\\Control\\Class\\"
           L"{4d36e968-e325-11ce-bfc1-08002be10318}",
           0, KEY_READ, &klass) != ERROR_SUCCESS)
    return census;
  for (DWORD index = 0; index < 64; ++index) {
    wchar_t name[16]{};
    DWORD name_size = static_cast<DWORD>(std::size(name));
    const LSTATUS status = enumerate(klass, index, name, &name_size, nullptr,
                                     nullptr, nullptr, nullptr);
    if (status == ERROR_NO_MORE_ITEMS) break;
    if (status != ERROR_SUCCESS) continue;
    wchar_t value[128]{};
    DWORD bytes = sizeof(value) - sizeof(wchar_t);
    if (get(klass, name, L"MatchingDeviceId", RRF_RT_REG_SZ, nullptr, value,
            &bytes) != ERROR_SUCCESS)
      continue;
    uint32_t device_id = 0;
    if (ParseNvidiaDeviceId(value, device_id))
      census.AddDevice(device_id);
  }
  close(klass);
  return census;
}

// Read once; safe under the loader lock.
inline bool AllNvidiaAdaptersBlackwell() {
  static const bool value = ReadInstalledAdapters().AllBlackwell();
  return value;
}

// Read once; safe under the loader lock.
inline bool AllNvidiaAdaptersPreAda() {
  static const bool value = ReadInstalledAdapters().AllPreAda();
  return value;
}

}  // namespace mfgunlock::gpuarch
