// SPDX-License-Identifier: MIT
#pragma once
// 1.8.1: what the panel and the reports say about this PC. Read only while the
// panel is open. No names of users or folders.
#include <windows.h>
#include <dxgi1_6.h>
#include <cstdint>
#include <string>

namespace mfgunlock::system {
struct Display {
  bool known{};
  int format{};      // DXGI numbering, which reshade::api::format shares
  bool windowsHdr{}; // this monitor is in HDR mode in Windows
  uint32_t width{}, height{}, refreshHz{};
};
constexpr int kFormatRgba16Float = 10, kFormatRgb10a2 = 24;
// ReShade does not expose the swap chain's color space, so this reads the format
// with the monitor's mode: 10 bits alone can be SDR, and FP16 is scRGB by convention.
constexpr const char* OutputName(int format, bool windowsHdr) {
  if (format == kFormatRgba16Float) return "scRGB";
  if (format == kFormatRgb10a2) return windowsHdr ? "HDR10" : "SDR (10-bit)";
  return "SDR";
}
constexpr bool OutputIsScrgb(int format) { return format == kFormatRgba16Float; }
// Windows shows NVIDIA's 621.01 as 32.0.16.2101: the last five digits.
inline std::string DriverName(uint32_t vendor, uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
  if (vendor == 0x10DE) {
    const uint32_t digits = (c % 10u) * 10000u + d % 10000u;
    const uint32_t cents = digits % 100u;
    return std::to_string(digits / 100u) + "." + (cents < 10u ? "0" : "") + std::to_string(cents);
  }
  return std::to_string(a) + "." + std::to_string(b) + "." + std::to_string(c) + "." + std::to_string(d);
}
inline std::string WindowsName(uint32_t major, uint32_t build) {
  if (major != 10) return std::to_string(major) + " (" + std::to_string(build) + ")";
  return std::string(build >= 22000 ? "11" : "10") + " (" + std::to_string(build) + ")";
}

struct Machine {
  std::string gpu{"Unknown"}, driver{"Unknown"}, windows{"Unknown"};
};
inline std::string Utf8(const wchar_t* text) {
  const int count = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (count <= 1) return {};
  std::string out(size_t(count - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), count, nullptr, nullptr);
  return out;
}
// Does not change during a session. dxgi.dll is resolved here because not every
// target of this panel links it.
inline const Machine& ReadMachine() {
  static const Machine machine = [] {
    Machine out;
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW os{sizeof(os)};
    if (auto get = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        get != nullptr && get(&os) == 0)
      out.windows = WindowsName(os.dwMajorVersion, os.dwBuildNumber);
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    const HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto create = dxgi != nullptr ? reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    IDXGIFactory1* factory = nullptr;
    if (create != nullptr && SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
      IDXGIAdapter1* adapter = nullptr;
      if (SUCCEEDED(factory->EnumAdapters1(0, &adapter))) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
          out.gpu = Utf8(desc.Description);
          LARGE_INTEGER umd{};
          if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
            out.driver = DriverName(desc.VendorId, HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart), LOWORD(umd.LowPart));
        }
        adapter->Release();
      }
      factory->Release();
    }
    return out;
  }();
  return machine;
}
inline uint32_t RefreshHz(HWND window) {
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info)) return 0;
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  return EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) ? mode.dmDisplayFrequency : 0;
}
// Whether Windows has this window's monitor in HDR mode.
inline bool MonitorInHdr(HWND window) {
  using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
  const HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
  auto create = dxgi != nullptr ? reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
  IDXGIFactory1* factory = nullptr;
  if (create == nullptr || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return false;
  const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
  bool hdr = false, found = false;
  IDXGIAdapter1* adapter = nullptr;
  for (UINT a = 0; !found && SUCCEEDED(factory->EnumAdapters1(a, &adapter)); ++a) {
    IDXGIOutput* output = nullptr;
    for (UINT o = 0; !found && SUCCEEDED(adapter->EnumOutputs(o, &output)); ++o) {
      IDXGIOutput6* output6 = nullptr;
      if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&output6)))) {
        DXGI_OUTPUT_DESC1 desc{};
        if (SUCCEEDED(output6->GetDesc1(&desc)) && desc.Monitor == monitor) {
          found = true;
          hdr = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        }
        output6->Release();
      }
      output->Release();
    }
    adapter->Release();
  }
  factory->Release();
  return hdr;
}
inline Display g_display{};  // written and read on the overlay's thread
inline std::string Report() {
  const auto& machine = ReadMachine();
  std::string report = "gpu=" + machine.gpu + "\ndriver=" + machine.driver + "\nwindows=" + machine.windows;
  if (g_display.known)
    report += std::string("\noutput=") + OutputName(g_display.format, g_display.windowsHdr) + "\nformat=" +
              std::to_string(g_display.format) + "\nresolution=" + std::to_string(g_display.width) + "x" +
              std::to_string(g_display.height) + "\nrefresh_hz=" + std::to_string(g_display.refreshHz);
  return report + "\n";
}
}  // namespace mfgunlock::system
