#include <cstdlib>
#include <iostream>
#include <new>
#include "../src/addons/mfgunlock/loadhook.hpp"
extern "C" __declspec(dllexport) void ReShadeLogMessage(void*,int,const char*){}
static bool measuring=false;static size_t allocations=0;
void* operator new(size_t size){if(measuring)++allocations;if(auto* p=std::malloc(size))return p;throw std::bad_alloc();}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,size_t) noexcept {std::free(p);}
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; return EXIT_FAILURE; } } while (false)
static int providers=0,plugins=0,interposers=0;
int main(){
  namespace l=mfgunlock::loadhook;
  HMODULE module=GetModuleHandleW(L"kernel32.dll");
  // Warm APIs/CRT before counting the common loader notification path.
  l::internal::Notify(module,L"kernel32.dll");
  measuring=true;
  for(int i=0;i<10000;++i)l::internal::Notify(module,L"kernel32.dll");
  measuring=false;
  CHECK(allocations==0);
  l::g_on_dlssg_loaded=[](HMODULE){++providers;};
  l::g_on_dlssg_plugin_loaded=[](HMODULE mod){++plugins;l::internal::Notify(mod,L"sl.interposer.dll");};
  l::g_on_interposer_loaded=[](){++interposers;};
  l::internal::Notify(module,L"NVNGX_DLSSG.DLL");
  l::internal::Notify(module,L"C:\\models\\dlssg\\opaque.bin");
  l::internal::Notify(module,L"SL.DLSS_G.DLL");
  CHECK(providers==2 && plugins==1 && interposers==1);
  CHECK(l::internal::NameContains(L"C:\\MODELS\\SL_DLSS_G_0\\1\\opaque.dll",l::internal::kDlssgPluginOtaNeedle));
  // 1.8.5: the NVIDIA app's override copy, and not the other Streamline plugins.
  CHECK(l::internal::NameContains(L"C:\\ProgramData\\NVIDIA\\NGX\\models\\sl_dlss_g_override_0\\versions\\134672\\files\\190_E658700.dll",l::internal::kDlssgPluginOtaNeedle));
  CHECK(!l::internal::NameContains(L"C:\\ProgramData\\NVIDIA\\NGX\\models\\sl_dlss_0\\versions\\1\\files\\190_E658703.dll",l::internal::kDlssgPluginOtaNeedle));
  CHECK(!l::internal::NameContains(L"C:\\ProgramData\\NVIDIA\\NGX\\models\\sl_dlss_d_override_0\\versions\\1\\files\\190_E658700.dll",l::internal::kDlssgPluginOtaNeedle));
  std::cout<<"10000 warmed Notify calls: zero C++ heap allocations; basename, OTA, case and callback reentry passed\n";
}
