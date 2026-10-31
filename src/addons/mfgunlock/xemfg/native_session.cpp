// SPDX-License-Identifier: MIT
// One bounded session header per process, written from the discovery worker
// (never under the loader lock). File names only: no directories or users.
#include "native_internal.hpp"
#include "native_session.hpp"
#include <psapi.h>
#include <algorithm>
namespace mfgunlock::xemfg::native {
namespace {
std::string Utf8(const std::wstring& text) {
  if(text.empty()) return {};
  const int n=WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),nullptr,0,nullptr,nullptr);
  std::string out(size_t(n>0?n:0),'\0');
  if(n>0) WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),out.data(),n,nullptr,nullptr);
  return out;
}
std::wstring Lower(std::wstring text) { std::transform(text.begin(),text.end(),text.begin(),::towlower); return text; }
// Modules whose presence changes presentation, input or Streamline behavior.
bool Relevant(const std::wstring& name) {
  static constexpr const wchar_t* prefixes[]={L"sl.",L"nvngx",L"libxess",L"libxell",L"reshade",L"dxgi",L"d3d12",
      L"gameoverlayrenderer",L"rtsshooks",L"discordhook",L"nvidia",L"optiscaler",L"dinput8",L"reframework",
      L"amd_fidelityfx",L"ffx_",L"winmm",L"version",L"dlss"};
  const auto lower=Lower(name);
  for(const auto* prefix:prefixes) if(lower.rfind(prefix,0)==0) return true;
  return lower.size()>4 && (lower.ends_with(L".asi") || lower.ends_with(L".addon64") || lower.ends_with(L".addon"));
}
}
std::string FileVersion(HMODULE module) {
  wchar_t path[32768]{};
  if(!module || !GetModuleFileNameW(module,path,DWORD(std::size(path)))) return "unavailable";
  return FileVersion(std::wstring(path));
}
std::string FileVersion(const std::wstring& path) {
  DWORD unused{}; const auto size=GetFileVersionInfoSizeW(path.c_str(),&unused);
  if(!size || size>4'000'000) return "unavailable";
  std::vector<char> data(size); VS_FIXEDFILEINFO* info{}; UINT length{};
  if(!GetFileVersionInfoW(path.c_str(),0,size,data.data())
      || !VerQueryValueW(data.data(),L"\\",reinterpret_cast<void**>(&info),&length) || length<sizeof(*info)) return "unavailable";
  return std::to_string(HIWORD(info->dwFileVersionMS))+'.'+std::to_string(LOWORD(info->dwFileVersionMS))+'.'
      +std::to_string(HIWORD(info->dwFileVersionLS))+'.'+std::to_string(LOWORD(info->dwFileVersionLS));
}
void LogSession() noexcept {
  static std::atomic<bool> written{};
  if(written.exchange(true)) return;
  try {
    SYSTEMTIME utc{}; GetSystemTime(&utc);
    char stamp[32]{};
    snprintf(stamp,sizeof(stamp),"%04u-%02u-%02uT%02u:%02u:%02uZ",utc.wYear,utc.wMonth,utc.wDay,utc.wHour,utc.wMinute,utc.wSecond);
    wchar_t exe[32768]{}; GetModuleFileNameW(nullptr,exe,DWORD(std::size(exe)));
    const auto executable=std::filesystem::path(exe).filename().wstring();
    std::string fields="\"utc\":\""+std::string(stamp)+"\",\"executable\":\""+escape(Utf8(executable))
        +"\",\"executable_version\":\""+FileVersion(std::wstring(exe))+"\"";
    using RtlGetVersionFn=LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW os{sizeof(os)};
    if(auto get=reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlGetVersion")); get && !get(&os))
      fields+=",\"windows\":\""+std::to_string(os.dwMajorVersion)+'.'+std::to_string(os.dwMinorVersion)+'.'+std::to_string(os.dwBuildNumber)+"\"";
    ComPtr<IDXGIFactory1> factory; ComPtr<IDXGIAdapter1> adapter; DXGI_ADAPTER_DESC1 desc{};
    if(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumAdapters1(0,&adapter))
        && SUCCEEDED(adapter->GetDesc1(&desc))) {
      LARGE_INTEGER umd{};
      const bool driver=SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice),&umd));
      fields+=",\"gpu\":\""+escape(Utf8(desc.Description))+"\",\"vendor_id\":"+std::to_string(desc.VendorId)
          +",\"device_id\":"+std::to_string(desc.DeviceId)+",\"driver\":\""+(driver?
            std::to_string(HIWORD(umd.HighPart))+'.'+std::to_string(LOWORD(umd.HighPart))+'.'+std::to_string(HIWORD(umd.LowPart))+'.'+std::to_string(LOWORD(umd.LowPart)):
            std::string("unavailable"))+"\"";
    }
    fields+=",\"addon\":\"1.7.1\"";
    xess_mod::WriteLog("native_session",fields);
  } catch(...) {}
}
namespace { std::atomic<uint64_t> sdkVersionSeen{}; }
std::string StreamlineSdkVersion() {
  const auto v=sdkVersionSeen.load();
  if(!v) return "unobserved";
  return std::to_string((v>>48)&0xffff)+'.'+std::to_string((v>>32)&0xffff)+'.'+std::to_string((v>>16)&0xffff);
}
// Streamline 2.x packs major/minor/patch above a 0xfedc magic; older callers did not.
bool StreamlineSdkMagic() noexcept { return (sdkVersionSeen.load()&0xffff)==0xfedc; }
void LogStreamlineVersion(uint64_t sdkVersion) noexcept {
  // Games commonly call slInit before this addon hooks it; the first device
  // boundary then logs with "unobserved" and the interposer file version.
  static std::atomic<bool> logged{};
  if(!sdkVersion && logged.exchange(true)) return;
  logged=true;
  try {
    if(sdkVersion) sdkVersionSeen=sdkVersion;
    std::string fields="\"sdk_version\":\""+StreamlineSdkVersion()+"\",\"sdk_magic\":"+(StreamlineSdkMagic()?"true":"false")
        +",\"interposer\":\""+FileVersion(GetModuleHandleW(L"sl.interposer.dll"))+"\"";
    // Plugins may be OTA copies under another file name; report the mapped ones.
    for(const auto* name:{L"sl.common.dll",L"sl.dlss_g.dll",L"sl.reflex.dll",L"sl.pcl.dll",L"nvngx_dlssg.dll"}) {
      const auto module=GetModuleHandleW(name);
      fields+=",\""+Utf8(std::wstring(name))+"\":\""+(module?FileVersion(module):std::string("not_mapped"))+"\"";
    }
    xess_mod::WriteLog("native_streamline_version",fields);
  } catch(...) {}
}
void LogModules(uint64_t output) noexcept {
  // Output recreation repeats this call; only a changed inventory is logged.
  static std::mutex lock; static std::string previous; static uint32_t budget{};
  std::lock_guard guard(lock);
  if(budget>=4) return;
  try {
    std::string fields="\"output\":"+std::to_string(output);
    std::array<HMODULE,2048> modules{}; DWORD bytes{};
    std::string relevant; size_t listed{},skipped{};
    if(EnumProcessModules(GetCurrentProcess(),modules.data(),DWORD(sizeof(modules)),&bytes)) {
      for(size_t i=0;i<(std::min)(size_t(bytes/sizeof(HMODULE)),modules.size());++i) {
        // Borrow a reference: the snapshot does not keep images mapped.
        HMODULE reference{};
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(modules[i]),&reference)) continue;
        wchar_t path[32768]{};
        const auto n=GetModuleFileNameW(reference,path,DWORD(std::size(path)));
        if(n && n<std::size(path)) {
          const auto name=std::filesystem::path(path).filename().wstring();
          if(Relevant(name)) {
            if(listed<48) {
              relevant+=std::string(listed?",":"")+"{\"name\":\""+escape(Utf8(name))+"\",\"version\":\""+FileVersion(std::wstring(path))+"\"}";
              ++listed;
            } else ++skipped;
          }
        }
        FreeLibrary(reference);
      }
    }
    if(relevant==previous) return;
    previous=relevant; ++budget;
    fields+=",\"modules\":["+relevant+"],\"modules_omitted\":"+std::to_string(skipped);
    xess_mod::WriteLog("native_modules",fields);
  } catch(...) {}
}
}
