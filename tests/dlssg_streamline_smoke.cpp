// SPDX-License-Identifier: MIT
// Plan 6.1: the DLSS-G backend against a fake Streamline interposer. The game
// side is this executable: it initializes Streamline, lets ReShade load the
// addon with [Backend] Selected=DLSSG and calls the DLSS-G and Reflex
// functions it obtains through slGetFeatureFunction, as a game does. The
// fixture records what reached the provider. ReShade.ini sets the fixed
// multiplier (ForceMultiplier=4).
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <sl.h>
#include <sl_dlss.h>
#include <sl_dlss_d.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <nvsdk_ngx.h>
#include <cstddef>
#include <cstring>
#include <map>
#include <iostream>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
namespace {
void Require(bool value,const char* what) { if(!value) throw std::runtime_error(what); }
struct Token : sl::FrameToken { uint32_t id{}; operator uint32_t() const override {return id;} };
template<class T> T Import(HMODULE m,const char* name) { auto p=reinterpret_cast<T>(GetProcAddress(m,name)); Require(p!=nullptr,name); return p; }
// A game-side NGX parameter block (NGX defines only the interface).
struct Parameters final : NVSDK_NGX_Parameter {
  std::map<std::string,unsigned long long> integers; std::map<std::string,double> reals; std::map<std::string,void*> pointers;
  void Set(const char* n,unsigned long long v) override { integers[n]=v; }
  void Set(const char* n,float v) override { reals[n]=v; }
  void Set(const char* n,double v) override { reals[n]=v; }
  void Set(const char* n,unsigned int v) override { integers[n]=v; }
  void Set(const char* n,int v) override { integers[n]=static_cast<unsigned long long>(static_cast<long long>(v)); }
  void Set(const char* n,ID3D11Resource* v) override { pointers[n]=v; }
  void Set(const char* n,ID3D12Resource* v) override { pointers[n]=v; }
  void Set(const char* n,void* v) override { pointers[n]=v; }
  template<class T> NVSDK_NGX_Result Integer(const char* n,T* out) const {
    const auto it=integers.find(n); if(it==integers.end() || !out) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    *out=static_cast<T>(it->second); return NVSDK_NGX_Result_Success;
  }
  NVSDK_NGX_Result Get(const char* n,unsigned long long* v) const override { return Integer(n,v); }
  NVSDK_NGX_Result Get(const char*,float*) const override { return NVSDK_NGX_Result_FAIL_InvalidParameter; }
  NVSDK_NGX_Result Get(const char*,double*) const override { return NVSDK_NGX_Result_FAIL_InvalidParameter; }
  NVSDK_NGX_Result Get(const char* n,unsigned int* v) const override { return Integer(n,v); }
  NVSDK_NGX_Result Get(const char* n,int* v) const override { return Integer(n,v); }
  NVSDK_NGX_Result Get(const char*,ID3D11Resource**) const override { return NVSDK_NGX_Result_FAIL_InvalidParameter; }
  NVSDK_NGX_Result Get(const char*,ID3D12Resource**) const override { return NVSDK_NGX_Result_FAIL_InvalidParameter; }
  NVSDK_NGX_Result Get(const char*,void**) const override { return NVSDK_NGX_Result_FAIL_InvalidParameter; }
  void Reset() override { integers.clear(); reals.clear(); pointers.clear(); }
};
using CreateFeatureFn=NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*,NVSDK_NGX_Feature,NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**);
HMODULE Owner(const void* address) {
  HMODULE module{};
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      static_cast<LPCWSTR>(address),&module);
  return module;
}
}
int main() {
  try {
    auto fixture=LoadLibraryW(L"sl.interposer.dll"); Require(fixture,"fixture");
    sl::Preferences preferences;
    Require(Import<PFun_slInit*>(fixture,"slInit")(preferences,sl::kSDKVersion)==sl::Result::eOk,"fixture startup");
    // ReShade loads its addons on the first device; the DLSS-G backend hooks
    // the already mapped interposer from its DllMain.
    auto reshade=LoadLibraryW(L".\\dxgi.dll"); Require(reshade,"ReShade");
    ComPtr<IDXGIFactory4> factory;
    Require(SUCCEEDED(Import<HRESULT(WINAPI*)(REFIID,void**)>(reshade,"CreateDXGIFactory1")(IID_PPV_ARGS(&factory))),"factory");
    ComPtr<ID3D12Device> device;
    for(UINT i=0;!device;++i) {
      ComPtr<IDXGIAdapter1> adapter;
      if(factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND) break;
      DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
      if(desc.VendorId==0x10de) D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device));
    }
    Require(device!=nullptr,"NVIDIA device");
    auto addon=GetModuleHandleW(L"renodx-mfgunlock-native.addon64"); Require(addon,"ReShade did not load the addon");
    // 1.6 (plan B.3): the NGX driver module, mapped after the device as games
    // that create DLSS without Streamline do.
    auto ngx=LoadLibraryW(L"_nvngx.dll"); Require(ngx,"NGX fixture");
    // 1.8.3: the preset hook on NGX's CreateFeature cannot be undone, so only a
    // copy of the addon that stays loaded installs it, after the first real
    // Present (ReShade's temporary startup copy left it behind, and TLOU Part II
    // recursed through it). This process never presents: CreateFeature stays
    // NVIDIA's, and a DLSS creation passes unchanged.
    {
      const auto create=Import<CreateFeatureFn>(ngx,"NVSDK_NGX_D3D12_CreateFeature");
      Require(reinterpret_cast<const uint8_t*>(create)[0]!=0xE9,"NGX CreateFeature was hooked before the addon was resident");
      Parameters params; params.Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,10u);
      NVSDK_NGX_Handle* handle{};
      Require(NVSDK_NGX_SUCCEED(create(nullptr,NVSDK_NGX_Feature_SuperSampling,&params,&handle)),"NGX DLSS creation");
      unsigned int v{};
      Require(NVSDK_NGX_SUCCEED(params.Get(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,&v)) && v==10 && params.integers.size()==1,
          "a DLSS creation before the first Present was changed");
      std::cout<<"NGX DLSS creation before the first Present: unchanged"<<std::endl;
    }
    const auto getFeature=Import<PFun_slGetFeatureFunction*>(fixture,"slGetFeatureFunction");
    const auto resolve=[&](sl::Feature feature,const char* name) {
      void* fn{}; Require(getFeature(feature,name,fn)==sl::Result::eOk && fn,name); return fn;
    };
    // The game's functions are the backend's wrappers.
    auto setOptions=reinterpret_cast<PFun_slDLSSGSetOptions*>(resolve(sl::kFeatureDLSS_G,"slDLSSGSetOptions"));
    auto getState=reinterpret_cast<PFun_slDLSSGGetState*>(resolve(sl::kFeatureDLSS_G,"slDLSSGGetState"));
    auto reflexOptions=reinterpret_cast<PFun_slReflexSetOptions*>(resolve(sl::kFeatureReflex,"slReflexSetOptions"));
    auto reflexSleep=reinterpret_cast<PFun_slReflexSleep*>(resolve(sl::kFeatureReflex,"slReflexSleep"));
    Require(Owner(reinterpret_cast<void*>(setOptions))==addon,"slDLSSGSetOptions was not wrapped by the DLSS-G backend");
    Require(Owner(reinterpret_cast<void*>(getState))==addon,"slDLSSGGetState was not wrapped by the DLSS-G backend");
    Require(Owner(reinterpret_cast<void*>(reflexOptions))==addon,"slReflexSetOptions was not wrapped by the DLSS-G backend");
    const auto lastGenerated=Import<uint32_t(*)()>(fixture,"FixtureLastGenerated");
    const auto modes=Import<uint32_t(*)()>(fixture,"FixtureModes");
    const sl::ViewportHandle viewport{0};

    // Fixed multiplier: the game's 2x (one generated frame) reaches the provider as 4x.
    sl::DLSSGOptions on; on.structVersion=sl::kStructVersion3; on.mode=sl::DLSSGMode::eOn; on.numFramesToGenerate=1;
    Require(setOptions(viewport,on)==sl::Result::eOk,"SetOptions on");
    std::cout<<"SetOptions 2x -> provider generated="<<lastGenerated()<<std::endl;
    Require(lastGenerated()==3,"the fixed 4x multiplier did not reach the provider");
    Require((modes()&0xff)==uint32_t(sl::DLSSGMode::eOn),"the game's mode was changed");
    // The game's own request equal to the multiplier is forwarded unchanged.
    on.numFramesToGenerate=3;
    Require(setOptions(viewport,on)==sl::Result::eOk && lastGenerated()==3,"matching request changed");
    // Frame generation off is never rewritten.
    sl::DLSSGOptions off=on; off.mode=sl::DLSSGMode::eOff; off.numFramesToGenerate=1;
    Require(setOptions(viewport,off)==sl::Result::eOk,"SetOptions off");
    Require(lastGenerated()==1 && (modes()&0xff)==uint32_t(sl::DLSSGMode::eOff),"frame generation off was rewritten");
    // An options structure newer than the backend knows is forwarded unchanged.
    sl::DLSSGOptions newer=on; newer.structVersion=6; newer.numFramesToGenerate=1;
    Require(setOptions(viewport,newer)==sl::Result::eOk && lastGenerated()==1,"an unknown options ABI was overridden");
    Require(setOptions(viewport,on)==sl::Result::eOk && lastGenerated()==3,"override did not resume after an unknown ABI");

    // GetState: the provider's answer reaches the game; a v1 caller is not extended.
    sl::DLSSGState state; state.structVersion=sl::kStructVersion3;
    Require(getState(viewport,state,nullptr)==sl::Result::eOk,"GetState");
    Require(state.numFramesActuallyPresented==1 && state.numFramesToGenerateMax==1 && uint32_t(state.status)==0,"GetState changed the provider's state");
    Import<void(*)(uint32_t)>(fixture,"FixtureFgStatus")(4);
    Require(getState(viewport,state,nullptr)==sl::Result::eOk && uint32_t(state.status)==4,"a provider failure flag was masked");
    Import<void(*)(uint32_t)>(fixture,"FixtureFgStatus")(0);
    sl::DLSSGState v1; v1.structVersion=sl::kStructVersion1; v1.numFramesToGenerateMax=0xdeadbeef;
    Require(getState(viewport,v1,nullptr)==sl::Result::eOk && v1.numFramesToGenerateMax==0xdeadbeef,"a v1 state was written past its version");

    // 1.6 (plan B): DLSS Super Resolution preset K selected (ReShade.ini).
    auto dlssOptions=reinterpret_cast<PFun_slDLSSSetOptions*>(resolve(sl::kFeatureDLSS,"slDLSSSetOptions"));
    Require(Owner(reinterpret_cast<void*>(dlssOptions))==addon,"slDLSSSetOptions was not wrapped by the DLSS-G backend");
    const auto dlssPreset=Import<uint32_t(*)(uint32_t)>(fixture,"FixtureDlssPreset");
    const auto dlssMode=Import<uint32_t(*)()>(fixture,"FixtureDlssMode");
    const auto dlssWidth=Import<uint32_t(*)()>(fixture,"FixtureDlssWidth");
    const auto dlssVersion=Import<uint32_t(*)()>(fixture,"FixtureDlssVersion");
    const auto allPresets=[&](sl::DLSSPreset p) { for(uint32_t slot=0;slot<6;++slot) if(dlssPreset(slot)!=uint32_t(p)) return false; return true; };
    sl::DLSSOptions sr; sr.mode=sl::DLSSMode::eMaxQuality; sr.outputWidth=2560; sr.outputHeight=1440;
    sr.qualityPreset=sl::DLSSPreset::ePresetJ;
    Require(dlssOptions(viewport,sr)==sl::Result::eOk,"slDLSSSetOptions");
    std::cout<<"DLSS quality preset game=J -> provider="<<dlssPreset(1)<<std::endl;
    Require(allPresets(sl::DLSSPreset::ePresetK),"the selected preset K did not reach every mode");
    Require(dlssMode()==uint32_t(sl::DLSSMode::eMaxQuality) && dlssWidth()==2560 && dlssVersion()==sl::kStructVersion3,
        "the game's DLSS mode, size or version was changed");
    Require(sr.qualityPreset==sl::DLSSPreset::ePresetJ,"the game's DLSS options were written");
    // DLSS off is never changed.
    sl::DLSSOptions srOff=sr; srOff.mode=sl::DLSSMode::eOff;
    Require(dlssOptions(viewport,srOff)==sl::Result::eOk && dlssPreset(1)==uint32_t(sl::DLSSPreset::ePresetJ),"DLSS off was changed");
    // A version-1 caller provides only its own fields: placed against a no-access
    // page, any read past them faults.
    {
      SYSTEM_INFO info{}; GetSystemInfo(&info);
      auto* pages=static_cast<uint8_t*>(VirtualAlloc(nullptr,info.dwPageSize*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
      Require(pages,"guard pages");
      DWORD old{}; VirtualProtect(pages+info.dwPageSize,info.dwPageSize,PAGE_NOACCESS,&old);
      const size_t v1=offsetof(sl::DLSSOptions,useAutoExposure);
      auto* v1Options=reinterpret_cast<sl::DLSSOptions*>(pages+info.dwPageSize-v1);
      memcpy(v1Options,&sr,v1); v1Options->structVersion=sl::kStructVersion1;
      Require(dlssOptions(viewport,*v1Options)==sl::Result::eOk,"version-1 slDLSSSetOptions");
      Require(allPresets(sl::DLSSPreset::ePresetK) && dlssVersion()==sl::kStructVersion1,"a version-1 caller did not get the preset");
      VirtualFree(pages,0,MEM_RELEASE);
    }
    // A structure newer than the backend knows is forwarded unchanged.
    sl::DLSSOptions srNewer=sr; srNewer.structVersion=4;
    Require(dlssOptions(viewport,srNewer)==sl::Result::eOk && dlssPreset(1)==uint32_t(sl::DLSSPreset::ePresetJ) && dlssVersion()==4,
        "an unknown DLSSOptions ABI was changed");

    // 1.6 (plan B.6): DLSS Ray Reconstruction preset E selected (ReShade.ini).
    auto rrOptions=reinterpret_cast<PFun_slDLSSDSetOptions*>(resolve(sl::kFeatureDLSS_RR,"slDLSSDSetOptions"));
    Require(Owner(reinterpret_cast<void*>(rrOptions))==addon,"slDLSSDSetOptions was not wrapped by the DLSS-G backend");
    const auto rrPreset=Import<uint32_t(*)(uint32_t)>(fixture,"FixtureRrPreset");
    const auto rrVersion=Import<uint32_t(*)()>(fixture,"FixtureRrVersion");
    const auto rrAll=[&](sl::DLSSDPreset p) { for(uint32_t slot=0;slot<6;++slot) if(rrPreset(slot)!=uint32_t(p)) return false; return true; };
    sl::DLSSDOptions rr; rr.mode=sl::DLSSMode::eMaxQuality; rr.outputWidth=2560; rr.outputHeight=1440;
    rr.qualityPreset=sl::DLSSDPreset::ePresetD;
    Require(rrOptions(viewport,rr)==sl::Result::eOk && rrAll(sl::DLSSDPreset::ePresetE) && rrVersion()==sl::kStructVersion3,
        "the selected Ray Reconstruction preset E did not reach every mode");
    Require(rr.qualityPreset==sl::DLSSDPreset::ePresetD,"the game's Ray Reconstruction options were written");
    // A version-2 structure has no presets: forwarded unchanged.
    sl::DLSSDOptions rrV2=rr; rrV2.structVersion=sl::kStructVersion2;
    Require(rrOptions(viewport,rrV2)==sl::Result::eOk && rrVersion()==sl::kStructVersion2 && rrPreset(1)==uint32_t(sl::DLSSDPreset::ePresetD),
        "a version-2 Ray Reconstruction structure was changed");
    std::cout<<"Ray Reconstruction quality preset game=D -> provider=E"<<std::endl;

    // Reflex without a configured cap: the game's options reach Streamline unchanged.
    sl::ReflexOptions reflex; reflex.mode=sl::ReflexMode::eLowLatency; reflex.frameLimitUs=16666;
    Require(reflexOptions(reflex)==sl::Result::eOk,"Reflex options");
    Require((modes()>>8)==uint32_t(sl::ReflexMode::eLowLatency) && Import<uint32_t(*)()>(fixture,"FixtureReflexLimit")()==16666,
        "the game's Reflex options were changed without a configured cap");
    for(uint32_t frame=1;frame<=4;++frame) {
      Token token; token.id=frame;
      Require(reflexSleep(token)==sl::Result::eOk,"Reflex sleep");
    }
    Require(Import<uint32_t(*)()>(fixture,"FixtureReflexSleeps")()==4,"a Reflex sleep did not reach Streamline");

    Require(Import<PFun_slShutdown*>(fixture,"slShutdown")()==sl::Result::eOk,"fixture shutdown");
    std::cout<<"PASS: DLSS-G backend: fixed multiplier, off, unknown ABI, GetState and Reflex through a Streamline fixture"<<std::endl;
    return 0;
  } catch(const std::exception& e) { std::cerr<<e.what()<<std::endl; return 1; }
}
