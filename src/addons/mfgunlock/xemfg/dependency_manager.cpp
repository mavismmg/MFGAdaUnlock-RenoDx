// SPDX-License-Identifier: MIT
#include "dependency_manager.hpp"
#include "../backend_selection.hpp"
#include "../panel_ui.hpp"
#include "vendor/sha256_util.h"
#include <nlohmann/json.hpp>
#include <shellapi.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <fstream>

namespace mfgunlock::dependencies {
namespace {
std::mutex mutex;
Status status;
std::atomic<bool> busy{};
std::atomic<bool> initial{};
HMODULE self{};
std::string Utf8(const std::wstring& w) {
  if(w.empty()) return {};
  const int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,w.data(),int(w.size()),nullptr,0,nullptr,nullptr);
  std::string result(n,0); if(n) WideCharToMultiByte(CP_UTF8,0,w.data(),int(w.size()),result.data(),n,nullptr,nullptr);return result;
}
std::string Resource(unsigned id) {
  const auto resource=FindResourceW(self,MAKEINTRESOURCEW(id),RT_RCDATA);
  if(!resource) throw std::runtime_error(T(Str::kRepairResourcesAreUnavailableUse_0cd3));
  const auto size=SizeofResource(self,resource); const auto bytes=LockResource(LoadResource(self,resource));
  if(!bytes || !size) throw std::runtime_error(T(Str::kRepairResourceCouldNotBe_162b));
  return std::string(static_cast<const char*>(bytes),size);
}
std::string Hash(const std::filesystem::path& path) {
  std::ifstream input(path,std::ios::binary); if(!input) throw std::runtime_error("Cannot read the dependency file.");
  sha256_util::Hasher hash; std::array<char,65536> buffer{};
  while(input) { input.read(buffer.data(),buffer.size()); if(input.gcount() && !hash.Update(buffer.data(),size_t(input.gcount()))) throw std::runtime_error(T(Str::kHashFailed_2050)); }
  sha256_util::Digest digest{}; if(input.bad() || !hash.Finish(digest)) throw std::runtime_error(T(Str::kHashFailed_2050));
  return sha256_util::LowerHex(digest);
}
std::wstring ModulePath(HMODULE module) {
  std::wstring path(32768,0); const auto n=GetModuleFileNameW(module,path.data(),DWORD(path.size()));
  if(!n || n>=path.size()) throw std::runtime_error(T(Str::kCannotIdentifyAMappedModule_5ce6)); path.resize(n); return path;
}
std::string Version(const std::filesystem::path& path) {
  DWORD unused{}; const auto size=GetFileVersionInfoSizeW(path.c_str(),&unused); if(!size) return "unavailable";
  std::vector<char> data(size); VS_FIXEDFILEINFO* info{}; UINT length{};
  if(!GetFileVersionInfoW(path.c_str(),0,size,data.data()) || !VerQueryValueW(data.data(),L"\\",reinterpret_cast<void**>(&info),&length) || length<sizeof(*info)) return "unavailable";
  return std::to_string(HIWORD(info->dwFileVersionMS))+'.'+std::to_string(LOWORD(info->dwFileVersionMS))+'.'+std::to_string(HIWORD(info->dwFileVersionLS))+'.'+std::to_string(LOWORD(info->dwFileVersionLS));
}
std::wstring Quote(const std::wstring& s) {
  if(s.find(L'"')!=s.npos || s.find(L'\n')!=s.npos) throw std::runtime_error("Invalid helper argument.");
  return L"\""+s+L"\"";
}
// Store/Game Pass executables run through a WindowsApps junction. Report and
// plan against the directory that actually holds the files.
std::filesystem::path FinalPath(const std::filesystem::path& path) {
  const auto handle=CreateFileW(path.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
  if(handle==INVALID_HANDLE_VALUE) return path;
  std::wstring buffer(32768,0); const auto n=GetFinalPathNameByHandleW(handle,buffer.data(),DWORD(buffer.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
  CloseHandle(handle);
  if(!n || n>=buffer.size()) return path;
  buffer.resize(n);
  if(buffer.rfind(L"\\\\?\\UNC\\",0)==0) return L"\\\\"+buffer.substr(8);
  if(buffer.rfind(L"\\\\?\\",0)==0) return buffer.substr(4);
  return buffer;
}
bool Named(const std::filesystem::path& path,const wchar_t* name) { return _wcsicmp(path.filename().c_str(),name)==0; }
// Same layout rules as the external helper, which remains the write authority.
std::filesystem::path InstallRoot(const std::filesystem::path& directory) {
  auto current=directory;
  for(int i=0;i<5 && current.has_parent_path() && current.parent_path()!=current;++i) {
    const auto parent=current.parent_path(),grandparent=parent.parent_path();
    if(Named(parent,L"common") && Named(grandparent,L"steamapps")) return current;
    if(Named(current,L"Content") && Named(grandparent,L"XboxGames")) return current;
    std::error_code error; if(std::filesystem::is_directory(current/L".egstore",error)) return current;
    current=parent;
  }
  if(Named(directory,L"Win64") || Named(directory,L"WinGDK")) if(Named(directory.parent_path(),L"Binaries")) {
    const auto candidate=directory.parent_path().parent_path().parent_path();
    if(candidate.has_relative_path()) return candidate;
  }
  return directory;
}
// Bounded, never follows links or junctions, skips the helper's backups.
std::vector<std::filesystem::path> ShippedCopies(const std::filesystem::path& root,bool& complete) {
  std::vector<std::filesystem::path> found; size_t visited{};
  std::error_code error;
  std::filesystem::recursive_directory_iterator it(root,std::filesystem::directory_options::skip_permission_denied,error),end;
  for(;!error && it!=end;it.increment(error)) {
    if(++visited>250000) { complete=false; break; }
    const auto attributes=GetFileAttributesW(it->path().c_str());
    const bool directory=attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_DIRECTORY);
    if(directory) {
      if((attributes&FILE_ATTRIBUTE_REPARSE_POINT) || Named(it->path(),L".mfg-dependency-backups")) it.disable_recursion_pending();
      else if(it.depth()>=9) { it.disable_recursion_pending(); complete=false; }
      continue;
    }
    if(!(attributes&FILE_ATTRIBUTE_REPARSE_POINT) && (Named(it->path(),L"libxess_fg.dll") || Named(it->path(),L"libxell.dll"))) found.push_back(it->path());
  }
  if(error) complete=false;
  return found;
}
void Run(bool repair) noexcept {
  try {
    Status next; next.checked=true;
    const auto catalog=nlohmann::json::parse(Resource(9010));
    const auto exeDirectory=backend::Directory(); if(exeDirectory.empty()) throw std::runtime_error(T(Str::kGameDirectoryIsUnavailable_657c));
    const auto root=FinalPath(exeDirectory),installRoot=InstallRoot(root);
    next.installRoot=Utf8(installRoot.wstring());
    const auto shipped=ShippedCopies(installRoot,next.scanComplete);
    for(const auto& entry:catalog.at("files")) {
      if(!entry.at("required").get<bool>()) continue;
      const auto name=entry.at("name").get<std::string>(); const std::wstring wide(name.begin(),name.end());
      std::filesystem::path mapped;
      HMODULE module{};
      if(GetModuleHandleExW(0,wide.c_str(),&module)) {
        try { mapped=FinalPath(ModulePath(module)); } catch(...) { FreeLibrary(module); throw; }
        FreeLibrary(module);
      }
      std::vector<std::pair<std::filesystem::path,const char*>> paths{{root/wide,T(Str::kGameFolder_551d)}};
      const auto add=[&](const std::filesystem::path& path,const char* role) {
        for(const auto& p:paths) if(_wcsicmp(p.first.c_str(),path.c_str())==0) return;
        paths.emplace_back(path,role);
      };
      if(std::filesystem::exists(root/L"_storage_"/wide)) add(root/L"_storage_"/wide,"Mod loader copy");
      if(!mapped.empty()) add(mapped,T(Str::kLoadedByTheGame_1e3c));
      for(const auto& copy:shipped) if(Named(copy,wide.c_str())) add(copy,T(Str::kShippedWithTheGame_7a3f));
      for(const auto& [path,role]:paths) {
        File file; file.name=name; file.path=Utf8(path.wstring()); file.required=true; file.role=role;
        file.mapped=!mapped.empty() && _wcsicmp(path.c_str(),mapped.c_str())==0;
        if(std::filesystem::exists(path)) {
          try {
            file.sha256=Hash(path); file.version=Version(path); file.health=Health::Unknown;
            if(file.sha256==entry.at("sha256").get<std::string>()) file.health=Health::Valid;
            else if(entry.contains("accepted_older")) for(const auto& hash:entry.at("accepted_older"))
              if(file.sha256==hash.get<std::string>()) file.health=Health::UpdateOptional;
          } catch(...) { file.health=Health::Error; }
        }
        next.files.push_back(std::move(file));
      }
    }
    next.message=next.scanComplete?T(Str::kExactHashesCheckedXessUpscaling_9a83):
        T(Str::kExactHashesCheckedThisInstallation_c602);
    if(repair) {
      const auto helper=Resource(9011);
      wchar_t temp[MAX_PATH]{}; if(!GetTempPathW(MAX_PATH,temp)) throw std::runtime_error(T(Str::kCannotLocateHelperStagingDirectory_94b4));
      const auto folder=std::filesystem::path(temp)/(L"MFG-XeHelper-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
      if(!std::filesystem::create_directory(folder)) throw std::runtime_error(T(Str::kCannotCreatePrivateHelperStaging_66f9));
      const auto write=[&](const wchar_t* name,const std::string& bytes) {
        std::ofstream output(folder/name,std::ios::binary); output.write(bytes.data(),bytes.size()); output.close();
        std::ifstream verify(folder/name,std::ios::binary); const std::string actual((std::istreambuf_iterator<char>(verify)),{});
        if(!verify || actual!=bytes) throw std::runtime_error(T(Str::kHelperStagingFailedVerification_bf26));
      };
      write(L"catalog.json",Resource(9010)); write(L"Repair-XeDependencies.ps1",helper);
      FILETIME created{},exited{},kernel{},user{};
      if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)) throw std::runtime_error(T(Str::kCannotIdentifyTheGameProcess_9656));
      ULARGE_INTEGER ticks{}; ticks.LowPart=created.dwLowDateTime; ticks.HighPart=created.dwHighDateTime;
      wchar_t system[MAX_PATH]{}; if(!GetSystemDirectoryW(system,MAX_PATH)) throw std::runtime_error(T(Str::kCannotLocatePowershell_13d9));
      const auto exe=std::filesystem::path(system)/L"WindowsPowerShell/v1.0/powershell.exe";
      const auto arguments=L"-NoProfile -ExecutionPolicy Bypass -File "+Quote((folder/L"Repair-XeDependencies.ps1").wstring())
          +L" -GameDirectory "+Quote(root.wstring())+L" -GameProcessId "+std::to_wstring(GetCurrentProcessId())
          +L" -GameCreationTicks "+std::to_wstring(ticks.QuadPart)+L" -GameExecutable "+Quote(ModulePath(nullptr))
          +L" -CatalogPath "+Quote((folder/L"catalog.json").wstring());
      SHELLEXECUTEINFOW launch{sizeof(launch)}; launch.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NO_UI;
      launch.lpFile=exe.c_str(); launch.lpParameters=arguments.c_str(); launch.nShow=SW_SHOWNORMAL;
      if(!ShellExecuteExW(&launch) || !launch.hProcess) throw std::runtime_error(T(Str::kCouldNotStartTheExternal_fb89));
      next.helperRunning=true; next.busy=false; next.message=T(Str::kExternalHelperOpenedReviewIts_b6f1);
      { std::lock_guard guard(mutex); status=next; }
      WaitForSingleObject(launch.hProcess,INFINITE); DWORD code{}; GetExitCodeProcess(launch.hProcess,&code); CloseHandle(launch.hProcess);
      next.helperRunning=false; next.message=code==3?T(Str::kContinuingInTheAdministratorHelper_e2b4):
          code?T(Str::kRepairStoppedReviewTheHelper_5290):T(Str::kHelperFinishedRestartAfterA_4be2);
    }
    { std::lock_guard guard(mutex); status=std::move(next); }
  } catch(const std::exception& e) { std::lock_guard guard(mutex); status.checked=true; status.busy=false; status.message=e.what(); }
  busy=false;
}
void Queue(bool repair) {
  if(busy.exchange(true)) return;
  // This process already pins the presentation hooks; the optional worker must
  // also never return into an unloaded addon. No file work runs in Draw().
  if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&Queue),&self)) { busy=false; return; }
  { std::lock_guard guard(mutex); status.busy=true; }
  try { std::thread(Run,repair).detach(); } catch(...) { busy=false; std::lock_guard guard(mutex); status.busy=false;status.message=T(Str::kCouldNotStartTheDependency_a679); }
}
}
void Inspect() { initial=true; Queue(false); }
void Repair() { initial=true; Queue(true); }
Status Snapshot() { std::lock_guard guard(mutex); return status; }
void Draw() {
#if defined(MFGUNLOCK_UI_PREVIEW)
  Status value{true,false,false,T(Str::kSyntheticDependencyStatusNoFiles_787c),
      {{"libxess_fg.dll","game/libxess_fg.dll","1.3.1.78","fixture",Health::Valid,true,true,T(Str::kGameFolder_551d)},
       {"libxess_fg.dll","game/Engine/Plugins/libxess_fg.dll","1.1.0.0","fixture",Health::Unknown,false,true,T(Str::kShippedWithTheGame_7a3f)},
       {"libxell.dll","game/libxell.dll","1.3.2.10","fixture",Health::Valid,true,true,T(Str::kGameFolder_551d)}},"game"};
#else
  if(!initial.exchange(true)) Queue(false);
  const auto value=Snapshot();
#endif
  ui::Section(T(Str::kIntelRuntimeFiles_6adc),T(Str::kXemfgNeedsIntelSFrame_8832));
  ui::Card card("##intel_dependencies");
  if(value.busy) ui::Description(T(Str::kCheckingThisGameSInstallation_c5c3));
  bool needsInstall{};
  for(size_t i=0;i<value.files.size();++i) {
    const auto& f=value.files[i];
    const bool usable=f.health==Health::Valid || f.health==Health::UpdateOptional;
    needsInstall=needsInstall || !usable;
    const char* health=f.health==Health::Valid?T(Str::kCompatible_a653):f.health==Health::UpdateOptional?T(Str::kCompatibleUpdateAvailable_58ac):
        f.health==Health::Missing?T(Str::kMissingInstallNeeded_8000):f.health==Health::Unknown?T(Str::kDifferentVersionUpdateNeeded_192f):T(Str::kCannotReadFile_4e64);
    const auto label=f.name+"  \xC2\xB7  "+(f.role.empty()?std::string(f.mapped?T(Str::kLoaded_a720):T(Str::kOnDisk_1a8d)):f.role);
    const auto detail=f.path+"\nVersion: "+(f.version.empty()?"-":f.version)+"\nSHA-256: "+(f.sha256.empty()?"-":f.sha256)
        +"\nFile presence does not prove selection. Loaded identifies a mapped module, not a live feature.";
    ui::Readout(("##dep_"+std::to_string(i)).c_str(),label.c_str(),health,detail.c_str(),usable?ui::Accent:ui::Warning);
  }
  if(const auto folder=backend::Directory(); !folder.empty()) {
    ImGui::Separator();
    const auto text=Utf8(FinalPath(folder).wstring());
    ui::Readout("##dep_target",T(Str::kIntelFilesFolder_b144),text.c_str(),
        T(Str::kXemfgLoadsLibxessFgDll_78f3));
  }
  if(!value.installRoot.empty()) {
    ImGui::Separator();
    ui::Readout("##dep_root",T(Str::kGameInstallation_e96d),value.installRoot.c_str(),
        T(Str::kDetectedFromTheRunningExecutable_7c25));
  }
  if(!value.message.empty()) ui::Description(value.message.c_str());
  ImGui::Spacing();
  ImGui::BeginDisabled(busy.load());
  ui::ButtonRow actions(2);
  if(actions(T(Str::kCheckAgain_fccf),T(Str::kReadsExactFileHashesOff_197d))) Inspect();
  if(actions(needsInstall?T(Str::kInstallUpdateFiles_73d5):T(Str::kReviewFiles_b9fa),T(Str::kOpensAnExternalHelperThat_0dbd))) Repair();
  ImGui::EndDisabled();
}
}

// 1.10.1 (plan item 8): NVIDIA's Streamline files for Add frame generation, in
// MFGUnlock-Streamline beside the addon. Checked against the pinned catalog
// (resource 9013); the same external helper downloads and installs them
// (Repair-XeDependencies.ps1 -Streamline), after the game exits.
namespace mfgunlock::dependencies {
namespace {
std::mutex sl_mutex;
StreamlineStatus sl_status;
std::atomic<bool> sl_busy{};
void RunStreamline(bool repair) noexcept {
  StreamlineStatus next;
  try {
    next.checked=true;
    const auto catalog=nlohmann::json::parse(Resource(9013));
    const auto root=FinalPath(backend::Directory());
    const auto folder=root/L"MFGUnlock-Streamline";
    for(const auto& entry:catalog.at("files")) {
      const auto name=entry.at("name").get<std::string>();
      const auto path=folder/std::wstring(name.begin(),name.end());
      if(!std::filesystem::exists(path)) { if(entry.at("required").get<bool>()) ++next.missing; continue; }
      std::string hash;
      try { hash=Hash(path); } catch(...) {}
      if(hash!=entry.at("sha256").get<std::string>()) ++next.different;
    }
    if(repair) {
      const auto helper=Resource(9011);
      wchar_t temp[MAX_PATH]{}; if(!GetTempPathW(MAX_PATH,temp)) throw std::runtime_error(T(Str::kCannotLocateHelperStagingDirectory_94b4));
      const auto staging=std::filesystem::path(temp)/(L"MFG-XeHelper-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
      if(!std::filesystem::create_directory(staging)) throw std::runtime_error(T(Str::kCannotCreatePrivateHelperStaging_66f9));
      const auto write=[&](const wchar_t* name,const std::string& bytes) {
        std::ofstream output(staging/name,std::ios::binary); output.write(bytes.data(),bytes.size()); output.close();
        std::ifstream verify(staging/name,std::ios::binary); const std::string actual((std::istreambuf_iterator<char>(verify)),{});
        if(!verify || actual!=bytes) throw std::runtime_error(T(Str::kHelperStagingFailedVerification_bf26));
      };
      write(L"streamline_catalog.json",Resource(9013)); write(L"Repair-XeDependencies.ps1",helper);
      FILETIME created{},exited{},kernel{},user{};
      if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)) throw std::runtime_error(T(Str::kCannotIdentifyTheGameProcess_9656));
      ULARGE_INTEGER ticks{}; ticks.LowPart=created.dwLowDateTime; ticks.HighPart=created.dwHighDateTime;
      wchar_t system[MAX_PATH]{}; if(!GetSystemDirectoryW(system,MAX_PATH)) throw std::runtime_error(T(Str::kCannotLocatePowershell_13d9));
      const auto exe=std::filesystem::path(system)/L"WindowsPowerShell/v1.0/powershell.exe";
      const auto arguments=L"-NoProfile -ExecutionPolicy Bypass -File "+Quote((staging/L"Repair-XeDependencies.ps1").wstring())
          +L" -Streamline -GameDirectory "+Quote(root.wstring())+L" -GameProcessId "+std::to_wstring(GetCurrentProcessId())
          +L" -GameCreationTicks "+std::to_wstring(ticks.QuadPart)+L" -GameExecutable "+Quote(ModulePath(nullptr))
          +L" -CatalogPath "+Quote((staging/L"streamline_catalog.json").wstring());
      SHELLEXECUTEINFOW launch{sizeof(launch)}; launch.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NO_UI;
      launch.lpFile=exe.c_str(); launch.lpParameters=arguments.c_str(); launch.nShow=SW_SHOWNORMAL;
      if(!ShellExecuteExW(&launch) || !launch.hProcess) throw std::runtime_error(T(Str::kCouldNotStartTheExternal_fb89));
      next.helperRunning=true; next.message=T(Str::kExternalHelperOpenedReviewIts_b6f1);
      { std::lock_guard guard(sl_mutex); sl_status=next; }
      WaitForSingleObject(launch.hProcess,INFINITE); DWORD code{}; GetExitCodeProcess(launch.hProcess,&code); CloseHandle(launch.hProcess);
      next.helperRunning=false; next.message=code==3?T(Str::kContinuingInTheAdministratorHelper_e2b4):
          code?T(Str::kRepairStoppedReviewTheHelper_5290):T(Str::kHelperFinishedRestartAfterA_4be2);
    }
  } catch(const std::exception& e) { next.checked=true; next.message=e.what(); }
  { std::lock_guard guard(sl_mutex); sl_status=std::move(next); }
  sl_busy=false;
}
void QueueStreamline(bool repair) {
  if(sl_busy.exchange(true)) return;
  if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&QueueStreamline),&self)) { sl_busy=false; return; }
  try { std::thread(RunStreamline,repair).detach(); } catch(...) { sl_busy=false; }
}
}
void InspectStreamline() { QueueStreamline(false); }
void RepairStreamline() { QueueStreamline(true); }
StreamlineStatus StreamlineSnapshot() { std::lock_guard guard(sl_mutex); return sl_status; }
}
