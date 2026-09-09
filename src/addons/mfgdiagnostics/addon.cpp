// SPDX-License-Identifier: MIT
// Separate, opt-in observer. The stable MFG Unlock implementation is untouched.
#include <windows.h>
#include <tlhelp32.h>
#include <d3d12.h>
#include <algorithm>
#include <filesystem>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>
#include "../mfgunlock/ngx_hook.hpp"
#include "./nvapi_observer.hpp"
#include "./trace.hpp"

#pragma comment(lib, "version.lib")

namespace {
using namespace mfgdiagnostics;
using Json = nlohmann::json;
constexpr char kSection[] = "RenoDX.MFGDiagnostics";
constexpr const char* kLabels[] = {"HDR_OFF_2x", "HDR_OFF_3x", "HDR_OFF_4x",
                                 "HDR_ON_2x", "HDR_ON_3x", "HDR_ON_4x"};
Capture<16384> g_capture;
bool g_enabled = false;
std::atomic_bool g_d3d12{false}, g_installed{false}, g_saving{false};
std::atomic<uint32_t> g_truncated_tag_batches{0};
std::atomic<uint32_t> g_optional_exports{0};
SRWLOCK g_install_lock = SRWLOCK_INIT;
std::vector<mfgunlock::hook::HookItem> g_hooks;
std::mutex g_output_mutex;
std::string g_output_message;
int g_label = 0;
int g_recorded_label = 0;
uint64_t g_frequency = 1;

struct TrackedResourceInfo {
  uint64_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  uint16_t levels = 0;
  uint16_t samples = 0;
  uint32_t flags = 0;
};
std::shared_mutex g_resources_mutex;
std::unordered_map<uint64_t, TrackedResourceInfo> g_resources;
std::mutex g_device_mutex;
IUnknown* g_native_d3d12_device = nullptr;

PFun_slGetFeatureFunction* g_get_function = nullptr;
PFun_slSetConstants* g_set_constants = nullptr;
using SetTagFn = sl::Result (*)(const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t,
                               sl::CommandBuffer*);
SetTagFn g_set_tag = nullptr;
PFun_slSetTagForFrame* g_set_tag_for_frame = nullptr;
std::atomic<PFun_slDLSSGSetOptions*> g_set_options{nullptr};
std::atomic<PFun_slDLSSGGetState*> g_get_state{nullptr};
using LoadWFn = HMODULE (WINAPI*)(LPCWSTR);
using LoadExWFn = HMODULE (WINAPI*)(LPCWSTR, HANDLE, DWORD);
LoadWFn g_load_w = nullptr;
LoadExWFn g_load_ex_w = nullptr;
bool g_loader_hooked = false;

uint64_t Ticks() {
  LARGE_INTEGER value{};
  QueryPerformanceCounter(&value);
  return static_cast<uint64_t>(value.QuadPart);
}

uint64_t Ticket() {
  if (g_capture.active.load(std::memory_order_acquire) == 0) return 0;
  return g_capture.Ticket(GetTickCount64());
}

void Submit(Event event, uint64_t ticket, uint64_t begin, uint32_t viewport,
            uint32_t frame, sl::Result result) {
  event.generation = ticket;
  event.begin = begin;
  event.end = Ticks();
  event.thread = GetCurrentThreadId();
  event.viewport = viewport;
  event.frame = frame;
  event.result = static_cast<uint32_t>(result);
  g_capture.Submit(event);
}

sl::Result HookOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options) {
  auto* real = g_set_options.load(std::memory_order_acquire);
  const uint64_t ticket = Ticket();
  if (ticket == 0) return real(viewport, options);
  const uint64_t begin = Ticks();
  auto event = SnapshotOptions(options);
  const auto result = real(viewport, options); // Exactly once; never alter options.
  Submit(event, ticket, begin, static_cast<uint32_t>(viewport), UINT32_MAX, result);
  return result;
}

sl::Result HookState(const sl::ViewportHandle& viewport, sl::DLSSGState& state,
                     const sl::DLSSGOptions* options) {
  auto* real = g_get_state.load(std::memory_order_acquire);
  const uint64_t ticket = Ticket();
  if (ticket == 0) return real(viewport, state, options);
  const uint64_t begin = Ticks();
  const auto result = real(viewport, state, options);
  Submit(SnapshotState(state, result), ticket, begin, static_cast<uint32_t>(viewport),
         UINT32_MAX, result);
  return result;
}

sl::Result HookConstants(const sl::Constants& values, const sl::FrameToken& frame,
                         const sl::ViewportHandle& viewport) {
  const uint64_t ticket = Ticket();
  if (ticket == 0) return g_set_constants(values, frame, viewport);
  const uint64_t begin = Ticks();
  auto event = SnapshotConstants(values);
  const auto result = g_set_constants(values, frame, viewport);
  Submit(event, ticket, begin, static_cast<uint32_t>(viewport),
         static_cast<uint32_t>(frame), result);
  return result;
}

// A legacy call can forward to the frame-based API. Preserve both calls while
// recording the outer observation only; legacy frame identity remains unknown.
thread_local bool g_inside_tag = false;
struct TagScope {
  bool nested = g_inside_tag;
  TagScope() { g_inside_tag = true; }
  ~TagScope() { g_inside_tag = nested; }
};

void FillTrackedDescriptor(Event& event, uint64_t native, uint32_t source_offset,
                           const TrackedResourceInfo& info) {
  event.values[3] = native != 0;
  event.values[4] = native;
  event.values[14] = info.width;
  event.values[15] = info.height;
  event.values[16] = info.format;
  event.values[17] = info.levels;
  event.values[18] = info.samples;
  event.values[19] = info.flags;
  event.values[20] = 1;
  event.values[24] = source_offset;
}

// Some shipped Streamline integrations hand slSetTag a Resource layout that
// does not validate against the public header used to build this observer. Do
// not guess that ABI or invoke COM through a guessed pointer. ReShade tells us
// every live D3D12 resource handle, so scan the bounded Resource storage for a
// pointer that exactly matches that live set. Accept only one unique match.
void ResolveTrackedResource(const sl::Resource& resource, Event& event) {
  if (!g_d3d12.load(std::memory_order_relaxed)) return;
  const auto* bytes = reinterpret_cast<const unsigned char*>(&resource);
  std::shared_lock lock(g_resources_mutex);

  uint64_t matched = 0;
  uint32_t matched_offset = UINT32_MAX;
  const TrackedResourceInfo* matched_info = nullptr;
  uint32_t matches = 0;
  for (uint32_t offset = 0; offset <= 56; offset += sizeof(uint64_t)) {
    uint64_t candidate = 0;
    std::memcpy(&candidate, bytes + offset, sizeof(candidate));
    if (candidate == 0 || candidate == matched) continue;
    const auto found = g_resources.find(candidate);
    if (found == g_resources.end()) continue;
    ++matches;
    matched = candidate;
    matched_offset = offset;
    matched_info = &found->second;
  }
  event.values[25] = matches;
  if (matches == 1)
    FillTrackedDescriptor(event, matched, matched_offset, *matched_info);
}

template <typename Call>
sl::Result ObserveTags(const sl::ViewportHandle& viewport, uint32_t frame,
                       const sl::ResourceTag* tags, uint32_t count,
                       sl::CommandBuffer* commands, Call&& call) {
  const TagScope scope;
  const uint64_t ticket = scope.nested ? 0 : Ticket();
  if (ticket == 0) return call();
  const uint64_t begin = Ticks();
  // No allocations or unbounded traversal on the application's tagging path.
  std::array<Event, 64> samples = {
      Event(Kind::tag)};
  const uint32_t size = tags == nullptr ? 0 : (std::min)(count, uint32_t{64});
  if (count > 64) g_truncated_tag_batches.fetch_add(1, std::memory_order_relaxed);
  for (uint32_t i = 0; i < size; ++i) {
    samples[i] = SnapshotTag(tags[i], commands);
    if (samples[i].recognized && tags[i].resource != nullptr)
      ResolveTrackedResource(*tags[i].resource, samples[i]);
  }
  const auto result = call();
  if (tags == nullptr || count > 64) {
    Event batch(Kind::tag_batch);
    batch.recognized = true;
    batch.values[0] = count;
    batch.values[1] = tags != nullptr;
    batch.values[2] = size;
    Submit(batch, ticket, begin, static_cast<uint32_t>(viewport), frame, result);
  }
  for (uint32_t i = 0; i < size; ++i)
    Submit(samples[i], ticket, begin, static_cast<uint32_t>(viewport), frame, result);
  return result;
}

sl::Result HookTag(const sl::ViewportHandle& viewport, const sl::ResourceTag* tags,
                   uint32_t count, sl::CommandBuffer* commands) {
  return ObserveTags(viewport, UINT32_MAX, tags, count, commands,
                      [&]() { return g_set_tag(viewport, tags, count, commands); });
}

sl::Result HookTagForFrame(const sl::FrameToken& frame, const sl::ViewportHandle& viewport,
                           const sl::ResourceTag* tags, uint32_t count,
                           sl::CommandBuffer* commands) {
  return ObserveTags(viewport, static_cast<uint32_t>(frame), tags, count, commands,
      [&]() { return g_set_tag_for_frame(frame, viewport, tags, count, commands); });
}

sl::Result HookFunction(sl::Feature feature, const char* name, void*& function) {
  const auto result = g_get_function(feature, name, function);
  if (result != sl::Result::eOk || feature != sl::kFeatureDLSS_G ||
      name == nullptr || function == nullptr) return result;
  if (std::strcmp(name, "slDLSSGSetOptions") == 0 &&
      function != reinterpret_cast<void*>(&HookOptions)) {
    g_set_options.store(reinterpret_cast<PFun_slDLSSGSetOptions*>(function), std::memory_order_release);
    function = reinterpret_cast<void*>(&HookOptions);
  } else if (std::strcmp(name, "slDLSSGGetState") == 0 &&
             function != reinterpret_cast<void*>(&HookState)) {
    g_get_state.store(reinterpret_cast<PFun_slDLSSGGetState*>(function), std::memory_order_release);
    function = reinterpret_cast<void*>(&HookState);
  }
  return result;
}

void TryInstall() {
  if (!g_enabled || g_installed.load(std::memory_order_acquire)) return;
  if (!TryAcquireSRWLockExclusive(&g_install_lock)) return;
  HMODULE module = GetModuleHandleW(L"sl.interposer.dll");
  if (module != nullptr && !g_installed.load(std::memory_order_relaxed)) {
    g_hooks.clear();
    const mfgunlock::hook::HookItem candidates[] = {
      {"slGetFeatureFunction", reinterpret_cast<void**>(&g_get_function), reinterpret_cast<void*>(&HookFunction)},
      {"slSetConstants", reinterpret_cast<void**>(&g_set_constants), reinterpret_cast<void*>(&HookConstants)},
      {"slSetTag", reinterpret_cast<void**>(&g_set_tag), reinterpret_cast<void*>(&HookTag)},
      {"slSetTagForFrame", reinterpret_cast<void**>(&g_set_tag_for_frame), reinterpret_cast<void*>(&HookTagForFrame)}
    };
    uint32_t exports = 0;
    for (size_t i = 0; i < std::size(candidates); ++i) {
      if (GetProcAddress(module, std::get<0>(candidates[i])) == nullptr) continue;
      g_hooks.push_back(candidates[i]);
      exports |= 1u << i;
    }
    if (!g_hooks.empty() && mfgunlock::hook::Install(module, g_hooks, "SL HDR observation")) {
      g_optional_exports.store(exports, std::memory_order_relaxed);
      g_installed.store(true, std::memory_order_release);
    }
  }
  ReleaseSRWLockExclusive(&g_install_lock);
}

HMODULE WINAPI HookLoadW(LPCWSTR path) {
  HMODULE module = g_load_w(path);
  if (module != nullptr) TryInstall();
  return module;
}

HMODULE WINAPI HookLoadExW(LPCWSTR path, HANDLE file, DWORD flags) {
  HMODULE module = g_load_ex_w(path, file, flags);
  if (module != nullptr && (flags & (LOAD_LIBRARY_AS_DATAFILE |
      LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)) == 0) TryInstall();
  return module;
}

const std::vector<mfgunlock::hook::HookItem> kLoaderHooks = {
  {"LoadLibraryW", reinterpret_cast<void**>(&g_load_w), reinterpret_cast<void*>(&HookLoadW)},
  {"LoadLibraryExW", reinterpret_cast<void**>(&g_load_ex_w), reinterpret_cast<void*>(&HookLoadExW)}
};

Json ModuleInfo(HMODULE module) {
  wchar_t path[32768]{};
  if (module == nullptr || GetModuleFileNameW(module, path, std::size(path)) == 0) return nullptr;
  Json info = {{"file", std::filesystem::path(path).filename().string()}};
  DWORD ignored = 0;
  std::vector<unsigned char> data(GetFileVersionInfoSizeW(path, &ignored));
  if (!data.empty() && GetFileVersionInfoW(path, 0, static_cast<DWORD>(data.size()), data.data())) {
    VS_FIXEDFILEINFO* version = nullptr;
    UINT size = 0;
    if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&version), &size) &&
        size >= sizeof(VS_FIXEDFILEINFO))
      info["file_version"] = {HIWORD(version->dwFileVersionMS), LOWORD(version->dwFileVersionMS),
                              HIWORD(version->dwFileVersionLS), LOWORD(version->dwFileVersionLS)};
  }
  return info;
}

Json FunctionOwner(const void* function) {
  HMODULE module = nullptr;
  if (function != nullptr)
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(function), &module);
  return ModuleInfo(module);
}

Json ObserveNvapi() {
  IUnknown* device = nullptr;
  {
    std::lock_guard lock(g_device_mutex);
    device = g_native_d3d12_device;
    if (device != nullptr) device->AddRef();
  }
  const auto snapshot = nvapi::Observe(device, GetCurrentProcessId());
  if (device != nullptr) device->Release();

  Json result = {
      {"library_loaded", snapshot.library_loaded},
      {"initialize_status", snapshot.initialize_status},
      {"sleep_status_result", snapshot.sleep_status},
      {"latency_result", snapshot.latency_status},
      {"ngx_override_result", snapshot.ngx_override_status},
  };
  if (snapshot.sleep_status == nvapi::kOk) {
    result["sleep"] = {
        {"low_latency_mode", snapshot.sleep.low_latency_mode != 0},
        {"fullscreen_vrr", snapshot.sleep.fullscreen_vrr != 0},
        {"control_panel_vsync", snapshot.sleep.control_panel_vsync != 0},
        {"sleep_interval_us", snapshot.sleep.sleep_interval_us},
        {"game_sleep", snapshot.sleep.game_sleep != 0},
        {"fullscreen_independent_flip",
         snapshot.sleep.fullscreen_independent_flip != 0},
        {"frame_generation_multiplier",
         snapshot.sleep.frame_generation_multiplier},
        {"dynamic_frame_generation_control",
         snapshot.sleep.dynamic_frame_generation_control != 0},
        {"dynamic_frame_time_target_us",
         snapshot.sleep.dynamic_frame_time_target_us},
    };
  }
  if (snapshot.latency_status == nvapi::kOk) {
    result["latency_frames"] = Json::array();
    for (const auto& frame : snapshot.latency.frames) {
      if (frame.frame_id == 0) continue;
      result["latency_frames"].push_back({
          {"frame_id", frame.frame_id},
          {"gpu_active_render_time_us", frame.gpu_active_render_time_us},
          {"gpu_frame_time_us", frame.gpu_frame_time_us},
          {"cross_adapter_copy_time_us", frame.cross_adapter_copy_time_us},
          {"ai_frame_time_us", frame.ai_frame_time_us},
          {"input_sample_time", frame.input_sample_time},
          {"simulation_start_time", frame.simulation_start_time},
          {"simulation_end_time", frame.simulation_end_time},
          {"render_submit_start_time", frame.render_submit_start_time},
          {"render_submit_end_time", frame.render_submit_end_time},
          {"present_start_time", frame.present_start_time},
          {"present_end_time", frame.present_end_time},
          {"driver_start_time", frame.driver_start_time},
          {"driver_end_time", frame.driver_end_time},
          {"os_render_queue_start_time", frame.os_render_queue_start_time},
          {"os_render_queue_end_time", frame.os_render_queue_end_time},
          {"gpu_render_start_time", frame.gpu_render_start_time},
          {"gpu_render_end_time", frame.gpu_render_end_time},
          {"camera_constructed_time", frame.camera_constructed_time},
      });
    }
  }
  if (snapshot.ngx_override_status == nvapi::kOk) {
    result["ngx_override"] = {
        {"feedback_super_resolution", snapshot.ngx.feedback_super_resolution},
        {"feedback_ray_reconstruction", snapshot.ngx.feedback_ray_reconstruction},
        {"feedback_frame_generation", snapshot.ngx.feedback_frame_generation},
        {"scaling_ratio", snapshot.ngx.scaling_ratio},
        {"performance_mode", snapshot.ngx.performance_mode},
        {"render_preset", snapshot.ngx.render_preset},
        {"frame_generation_count", snapshot.ngx.frame_generation_count},
        {"frame_generation_preset", snapshot.ngx.frame_generation_preset},
        {"frame_generation_mode", snapshot.ngx.frame_generation_mode},
    };
  }
  return result;
}

// Formatting, module inventory and file I/O run only on the explicitly requested
// export worker after recording has stopped. Never enumerate modules in Present.
DWORD WINAPI ExportWorker(LPVOID pinned_module) {
  std::string message;
  try {
    Json root = {{"schema", 1}, {"label", kLabels[g_recorded_label]},
      {"qpc_frequency", g_frequency}, {"dropped_events", g_capture.dropped.load()},
      {"capacity_exhausted", g_capture.full.load()},
      {"truncated_tag_batches", g_truncated_tag_batches.load()},
      {"interposer_export_mask", g_optional_exports.load()},
      {"options_downstream_owner", FunctionOwner(reinterpret_cast<const void*>(g_set_options.load()))},
      {"state_downstream_owner", FunctionOwner(reinterpret_cast<const void*>(g_get_state.load()))},
      {"nvapi_at_export", ObserveNvapi()},
      {"notes", "Read-only diagnostic boundary, not proof of GPU pixels or final provider evaluation. "
                "Legacy tags have no explicit frame ID. Format does not establish color encoding. "
                "Other addon wrappers may be downstream. No extra Streamline GetState calls, "
                "NVAPI setters or latency markers are issued. NVAPI is sampled only at export."}};
    root["events"] = Json::array();
    std::unordered_map<uint64_t, uint64_t> ids;
    auto identity = [&](uint64_t value) -> Json {
      if (value == kUnknown || value == 0) return nullptr;
      auto [it, inserted] = ids.try_emplace(value, ids.size() + 1);
      return it->second;
    };
    {
      std::lock_guard lock(g_capture.mutex);
      for (const auto& event : g_capture.events) {
        Json row = {{"kind", static_cast<int>(event.kind)}, {"begin_qpc", event.begin},
          {"end_qpc", event.end}, {"thread", event.thread}, {"version", event.version},
          {"recognized", event.recognized}};
        row["viewport"] = event.viewport == UINT32_MAX ? Json(nullptr) : Json(event.viewport);
        row["frame"] = event.frame == UINT32_MAX ? Json(nullptr) : Json(event.frame);
        row["result"] = event.result == UINT32_MAX ? Json(nullptr) : Json(event.result);
        row["values"] = Json::array();
        for (auto value : event.values)
          row["values"].push_back(value == kUnknown ? Json(nullptr) : Json(value));
        row["floats"] = event.floats;
        if (event.kind == Kind::tag) {
          row["values"][4] = identity(event.values[4]);
          row["values"][13] = identity(event.values[13]);
        } else if (event.kind == Kind::output) {
          row["values"][0] = identity(event.values[0]);
        }
        root["events"].push_back(std::move(row));
      }
    }
    root["loaded_candidates_at_export"] = Json::array();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot != INVALID_HANDLE_VALUE) {
      MODULEENTRY32W entry{};
      entry.dwSize = sizeof(entry);
      if (Module32FirstW(snapshot, &entry)) do {
        std::wstring path(entry.szExePath);
        std::transform(path.begin(), path.end(), path.begin(), towlower);
        const auto name = std::filesystem::path(path).filename().wstring();
        if (name.starts_with(L"sl.") || name.find(L"nvngx") != std::wstring::npos ||
            name.find(L"mfg") != std::wstring::npos || path.find(L"\\models\\dlssg\\") != std::wstring::npos)
          root["loaded_candidates_at_export"].push_back(ModuleInfo(entry.hModule));
      } while (Module32NextW(snapshot, &entry));
      CloseHandle(snapshot);
    }
    wchar_t temporary[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temporary) == 0) throw std::runtime_error("Cannot locate TEMP");
    const auto directory = std::filesystem::path(temporary) / "MFGUnlock-HDR-Diagnostics";
    std::filesystem::create_directories(directory);
    const auto path = directory / (std::string(kLabels[g_recorded_label]) + "-" +
        std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + ".json");
    const std::string serialized = root.dump();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create capture file");
    DWORD written = 0;
    const bool saved = WriteFile(file, serialized.data(), static_cast<DWORD>(serialized.size()), &written, nullptr) &&
                       written == serialized.size();
    CloseHandle(file);
    message = saved ? "Saved: " + path.string() : "Capture write failed; file may be incomplete.";
  } catch (const std::exception& error) {
    message = std::string("Export failed: ") + error.what();
  }
  {
    std::lock_guard lock(g_output_mutex);
    g_output_message = std::move(message);
  }
  g_saving.store(false, std::memory_order_release);
  FreeLibraryAndExitThread(static_cast<HMODULE>(pinned_module), 0);
}

void OnDevice(reshade::api::device* device) {
  g_d3d12.store(device->get_api() == reshade::api::device_api::d3d12, std::memory_order_relaxed);
  IUnknown* native = nullptr;
  if (device->get_api() == reshade::api::device_api::d3d12 && device->get_native() != 0) {
    native = reinterpret_cast<IUnknown*>(device->get_native());
    native->AddRef();
  }
  IUnknown* previous = nullptr;
  {
    std::lock_guard lock(g_device_mutex);
    previous = g_native_d3d12_device;
    g_native_d3d12_device = native;
  }
  if (previous != nullptr) previous->Release();
  TryInstall();
}

void OnDestroyDevice(reshade::api::device* device) {
  if (device->get_api() != reshade::api::device_api::d3d12 || device->get_native() == 0) return;
  IUnknown* previous = nullptr;
  {
    std::lock_guard lock(g_device_mutex);
    if (g_native_d3d12_device != reinterpret_cast<IUnknown*>(device->get_native())) return;
    previous = g_native_d3d12_device;
    g_native_d3d12_device = nullptr;
  }
  previous->Release();
}

void OnInitResource(reshade::api::device* device, const reshade::api::resource_desc& desc,
                    const reshade::api::subresource_data*, reshade::api::resource_usage,
                    reshade::api::resource resource) {
  if (device->get_api() != reshade::api::device_api::d3d12 || resource.handle == 0 ||
      desc.type == reshade::api::resource_type::buffer ||
      desc.type == reshade::api::resource_type::unknown) return;
  TrackedResourceInfo info{};
  info.width = desc.texture.width;
  info.height = desc.texture.height;
  info.format = static_cast<uint32_t>(desc.texture.format);
  info.levels = desc.texture.levels;
  info.samples = desc.texture.samples;
  info.flags = static_cast<uint32_t>(desc.flags);
  std::unique_lock lock(g_resources_mutex);
  g_resources[resource.handle] = info;
}

void OnDestroyResource(reshade::api::device* device, reshade::api::resource resource) {
  if (device->get_api() != reshade::api::device_api::d3d12 || resource.handle == 0) return;
  std::unique_lock lock(g_resources_mutex);
  g_resources.erase(resource.handle);
}

void OnPresent(reshade::api::command_queue*, reshade::api::swapchain* swapchain,
                const reshade::api::rect*, const reshade::api::rect*, uint32_t,
                const reshade::api::rect*) {
  const uint64_t ticket = Ticket();
  if (ticket == 0) return;
  const uint64_t begin = Ticks();
  Event event(Kind::output);
  event.recognized = true;
  event.values[0] = reinterpret_cast<uintptr_t>(swapchain);
  event.values[1] = static_cast<uint32_t>(swapchain->get_color_space());
  event.values[5] = swapchain->get_back_buffer_count();
  if (event.values[5] != 0) {
    const auto desc = swapchain->get_device()->get_resource_desc(swapchain->get_back_buffer(0));
    event.values[2] = static_cast<uint32_t>(desc.texture.format);
    event.values[3] = desc.texture.width;
    event.values[4] = desc.texture.height;
  }
  Submit(event, ticket, begin, UINT32_MAX, UINT32_MAX, sl::Result::eOk);
}

void OnOverlay(reshade::api::effect_runtime*) {
  static bool enabled = g_enabled;
  if (ImGui::Checkbox("Enable observation hooks (restart required)", &enabled))
    reshade::set_config_value(nullptr, kSection, "Enabled", enabled ? 1 : 0);
  if (enabled != g_enabled) ImGui::TextUnformatted("Setting saved. Restart the game to apply it.");
  ImGui::TextWrapped("Diagnostic companion, not an HDR fix. It does not modify MFG, buffers, jitter, "
                    "color space or pacing. Capture adds CPU work: benchmark with this addon removed.");
  ImGui::Text("Hooks: %s; legacy tags: %s; frame tags: %s",
      g_installed.load() ? "installed" : "not installed",
      (g_optional_exports.load() & 4) ? "available" : "unavailable",
      (g_optional_exports.load() & 8) ? "available" : "unavailable");
  const bool saving = g_saving.load(std::memory_order_acquire);
  ImGui::BeginDisabled(saving || !g_installed.load());
  ImGui::Combo("Capture label (manual)", &g_label, kLabels, static_cast<int>(std::size(kLabels)));
  if (ImGui::Button("Capture 10 seconds")) {
    g_recorded_label = g_label;
    g_truncated_tag_batches.store(0);
    g_capture.Start(GetTickCount64(), 10000);
  }
  ImGui::SameLine();
  if (ImGui::Button("Stop")) g_capture.Stop();
  ImGui::EndDisabled();
  const bool active = Ticket() != 0;
  ImGui::Text("Capture: %s; dropped: %u; buffer full: %s", active ? "recording" : "stopped",
               g_capture.dropped.load(), g_capture.full.load() ? "yes" : "no");
  ImGui::BeginDisabled(saving || active);
  if (ImGui::Button("Export stopped capture")) {
    HMODULE pinned = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&ExportWorker), &pinned)) {
      g_saving.store(true, std::memory_order_release);
      HANDLE thread = CreateThread(nullptr, 0, ExportWorker, pinned, 0, nullptr);
      if (thread != nullptr) CloseHandle(thread);
      else { g_saving.store(false); FreeLibrary(pinned); }
    }
  }
  ImGui::EndDisabled();
  std::unique_lock lock(g_output_mutex, std::try_to_lock);
  if (lock.owns_lock()) ImGui::TextWrapped("%s", g_output_message.c_str());
  ImGui::TextWrapped("Close the overlay during capture. For options data, switch the native FG setting "
                    "while recording. Restart to disable hooks; do not hot-unload either addon.");
}
} // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "MFG HDR Diagnostics";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION =
    "Experimental read-only Streamline input capture; no HDR or pacing overrides";

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    if (!reshade::register_addon(module)) return FALSE;
    int enabled = 0;
    reshade::get_config_value(nullptr, kSection, "Enabled", enabled);
    g_enabled = enabled != 0;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    g_frequency = static_cast<uint64_t>(frequency.QuadPart);
    if (g_enabled) {
      g_loader_hooked = mfgunlock::hook::Install(GetModuleHandleW(L"kernel32.dll"), kLoaderHooks,
                                                "HDR diagnostic loader observation");
      TryInstall();
      reshade::register_event<reshade::addon_event::init_device>(OnDevice);
      reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
      reshade::register_event<reshade::addon_event::init_resource>(OnInitResource);
      reshade::register_event<reshade::addon_event::destroy_resource>(OnDestroyResource);
      reshade::register_event<reshade::addon_event::present>(OnPresent);
    }
    reshade::register_overlay("MFG HDR Diagnostics", OnOverlay);
  } else if (reason == DLL_PROCESS_DETACH) {
    g_capture.Stop();
    reshade::unregister_overlay("MFG HDR Diagnostics", OnOverlay);
    if (g_enabled) {
      reshade::unregister_event<reshade::addon_event::present>(OnPresent);
      reshade::unregister_event<reshade::addon_event::destroy_resource>(OnDestroyResource);
      reshade::unregister_event<reshade::addon_event::init_resource>(OnInitResource);
      reshade::unregister_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
      reshade::unregister_event<reshade::addon_event::init_device>(OnDevice);
      IUnknown* device = nullptr;
      {
        std::lock_guard lock(g_device_mutex);
        device = g_native_d3d12_device;
        g_native_d3d12_device = nullptr;
      }
      if (device != nullptr) device->Release();
    }
    if (g_loader_hooked) mfgunlock::hook::Uninstall(kLoaderHooks);
    if (g_installed.load()) mfgunlock::hook::Uninstall(g_hooks);
    reshade::unregister_addon(module);
  }
  return TRUE;
}
