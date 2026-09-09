// SPDX-License-Identifier: MIT
// Exercise the actual observer wrappers without installing hooks or using a GPU.
#include <iostream>
#include <thread>
#define DllMain DiagnosticDllMainForTests
#include "../src/addons/mfgdiagnostics/addon.cpp"
#undef DllMain

#define CHECK(condition) do { if (!(condition)) { \
  std::cerr << "FAILED: " #condition " at line " << __LINE__ << '\n'; \
  std::abort(); } } while (false)

namespace {
int g_calls = 0;
sl::Result g_test_result = sl::Result::eOk;
uint32_t g_received_count = 0;
const void* g_received_address = nullptr;
struct TestFrame : sl::FrameToken {
  operator uint32_t() const override { return 42; }
};

sl::Result TestConstants(const sl::Constants& values, const sl::FrameToken&,
                          const sl::ViewportHandle&) {
  ++g_calls;
  g_received_address = &values;
  return g_test_result;
}

sl::Result TestTagFrame(const sl::FrameToken&, const sl::ViewportHandle&,
                        const sl::ResourceTag* tags, uint32_t count, sl::CommandBuffer*) {
  ++g_calls;
  g_received_address = tags;
  g_received_count = count;
  return g_test_result;
}

sl::Result ForwardLegacyTag(const sl::ViewportHandle& viewport, const sl::ResourceTag* tags,
                            uint32_t count, sl::CommandBuffer* commands) {
  TestFrame frame;
  return HookTagForFrame(frame, viewport, tags, count, commands);
}

sl::Result TestOptions(const sl::ViewportHandle&, const sl::DLSSGOptions& value) {
  ++g_calls;
  g_received_count = value.numFramesToGenerate;
  g_received_address = &value;
  return g_test_result;
}

sl::Result TestState(const sl::ViewportHandle&, sl::DLSSGState& state,
                      const sl::DLSSGOptions* options) {
  ++g_calls;
  g_received_address = options;
  state.numFramesActuallyPresented = 4;
  state.numFramesToGenerateMax = 3;
  state.status = sl::DLSSGStatus::eFailHDRFormatNotSupported;
  return g_test_result;
}

sl::Result TestTag(const sl::ViewportHandle&, const sl::ResourceTag* tags, uint32_t count,
                   sl::CommandBuffer*) {
  ++g_calls;
  g_received_address = tags;
  g_received_count = count;
  return g_test_result;
}

sl::Result TestFunction(sl::Feature, const char*, void*& function) {
  ++g_calls;
  function = reinterpret_cast<void*>(&TestOptions);
  return g_test_result;
}

template <typename T>
void GuardedSnapshot(const T& source, size_t prefix, auto snapshot) {
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  auto* pages = static_cast<unsigned char*>(VirtualAlloc(nullptr, info.dwPageSize * 2,
      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  CHECK(pages != nullptr);
  DWORD old = 0;
  CHECK(VirtualProtect(pages + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &old));
  auto* value = reinterpret_cast<T*>(pages + info.dwPageSize - prefix);
  std::memcpy(value, &source, prefix);
  snapshot(*value);
  CHECK(std::memcmp(value, &source, prefix) == 0);
  VirtualFree(pages, 0, MEM_RELEASE);
}
}

int main() {
  sl::DLSSGOptions options{};
  options.mode = sl::DLSSGMode::eOn;
  options.numFramesToGenerate = 3;
  options.colorBufferFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
  options.enableUserInterfaceRecomposition = sl::eTrue;
  options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockNoClientQueues;
  options.dynamicTargetFrameRate = 165;
  CHECK(SnapshotOptions(options).values[1] == 3);
  CHECK(SnapshotOptions(options).values[16] == sl::eTrue);
  CHECK(SnapshotOptions(options).floats[0] == 165);

  options.structVersion = 1;
  GuardedSnapshot(options, offsetof(sl::DLSSGOptions, bReserved15), [](const auto& value) {
    const auto event = SnapshotOptions(value);
    CHECK(event.recognized && event.values[10] == DXGI_FORMAT_R10G10B10A2_UNORM);
    CHECK(event.values[15] == kUnknown && event.values[16] == kUnknown);
  });
  options.structVersion = 3;
  GuardedSnapshot(options, offsetof(sl::DLSSGOptions, enableUserInterfaceRecomposition), [](const auto& value) {
    const auto event = SnapshotOptions(value);
    CHECK(event.values[15] == 1 && event.values[16] == kUnknown);
  });
  options.structVersion = 99;
  GuardedSnapshot(options, sizeof(sl::BaseStructure), [](const auto& value) {
    CHECK(!SnapshotOptions(value).recognized);
  });
  options.structVersion = 5;

  sl::Constants constants{};
  constants.jitterOffset = {0.25f, -0.5f};
  constants.mvecScale = {1.0f / 1280.0f, 1.0f / 720.0f};
  constants.reset = sl::eInvalid;
  constants.motionVectorsJittered = sl::eTrue;
  constants.structVersion = 1;
  GuardedSnapshot(constants, offsetof(sl::Constants, minRelativeLinearDepthObjectSeparation), [](const auto& value) {
    const auto event = SnapshotConstants(value);
    CHECK(event.recognized && event.floats[80] == 0.25f && event.floats[81] == -0.5f);
    CHECK(event.values[3] == sl::eInvalid && event.values[6] == sl::eTrue);
    CHECK(std::isnan(event.floats[103]));
  });

  sl::ResourceTag null_tag(nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent);
  CHECK(SnapshotTag(null_tag, nullptr).values[2] == 0);
  sl::Extent subrect{0, 0, 2560, 1440};
  sl::ResourceTag extent_only(nullptr, sl::kBufferTypeBackbuffer,
                             sl::ResourceLifecycle::eValidUntilPresent, &subrect);
  CHECK(SnapshotTag(extent_only, nullptr).values[11] == 2560);
  CHECK(SnapshotTag(extent_only, nullptr).values[4] == kUnknown);

  // A non-public/legacy Resource layout is resolved only through an exact,
  // unique match in ReShade's live D3D12 resource registry. No fake COM object
  // is dereferenced by this test or by the fallback.
  alignas(uint64_t) std::array<unsigned char, 64> opaque_resource{};
  constexpr uint64_t legacy_native = 0x123456780ull;
  std::memcpy(opaque_resource.data() + 8, &legacy_native, sizeof(legacy_native));
  sl::ResourceTag legacy_tag(reinterpret_cast<sl::Resource*>(opaque_resource.data()),
                             sl::kBufferTypeHUDLessColor,
                             sl::ResourceLifecycle::eOnlyValidNow, &subrect);
  {
    std::unique_lock lock(g_resources_mutex);
    g_resources[legacy_native] = {2560, 1440, DXGI_FORMAT_R10G10B10A2_UNORM, 1, 1, 0};
  }
  g_d3d12.store(true);
  auto legacy_event = SnapshotTag(legacy_tag, nullptr);
  ResolveTrackedResource(*legacy_tag.resource, legacy_event);
  CHECK(legacy_event.values[3] == 1 && legacy_event.values[4] == legacy_native);
  CHECK(legacy_event.values[16] == DXGI_FORMAT_R10G10B10A2_UNORM);
  CHECK(legacy_event.values[20] == 1 && legacy_event.values[24] == 8);
  CHECK(legacy_event.values[25] == 1);
  constexpr uint64_t ambiguous_native = 0x223456780ull;
  std::memcpy(opaque_resource.data() + 40, &ambiguous_native, sizeof(ambiguous_native));
  {
    std::unique_lock lock(g_resources_mutex);
    g_resources[ambiguous_native] = {1920, 1080, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1, 0};
  }
  auto ambiguous_event = SnapshotTag(legacy_tag, nullptr);
  ResolveTrackedResource(*legacy_tag.resource, ambiguous_event);
  CHECK(ambiguous_event.values[20] == kUnknown && ambiguous_event.values[25] == 2);
  {
    std::unique_lock lock(g_resources_mutex);
    g_resources.clear();
  }

  Capture<2> capture;
  Event event(Kind::output);
  capture.Start(100, 50);
  event.generation = capture.Ticket(101);
  CHECK(event.generation != 0);
  capture.Submit(event);
  CHECK(capture.events.size() == 1);
  capture.Start(110, 50);
  capture.Submit(event); // Previous capture's in-flight event must not leak.
  CHECK(capture.events.empty());
  event.generation = capture.Ticket(111);
  std::thread contended([&]() {
    std::lock_guard lock(capture.mutex);
    std::thread writer([&]() { capture.Submit(event); });
    writer.join();
  });
  contended.join();
  CHECK(capture.dropped == 1 && capture.events.empty());
  capture.Submit(event);
  capture.Submit(event);
  capture.Submit(event);
  CHECK(capture.full && capture.active == 0 && capture.events.size() == 2);
  capture.Start(200, 10);
  CHECK(capture.Ticket(210) == 0);

  sl::ViewportHandle viewport(7);
  g_set_options.store(TestOptions);
  g_get_state.store(TestState);
  g_set_tag = TestTag;
  g_set_constants = TestConstants;
  g_set_tag_for_frame = TestTagFrame;
  g_get_function = TestFunction;
  const auto original = options;
  for (bool enabled : {false, true}) {
    if (enabled) g_capture.Start(GetTickCount64(), 10000);
    else g_capture.Stop();
    for (auto result : {sl::Result::eOk, sl::Result::eErrorNotInitialized}) {
      g_test_result = result;
      g_calls = 0;
      CHECK(HookOptions(viewport, options) == result);
      CHECK(g_calls == 1 && g_received_count == 3 && g_received_address == &options);
      CHECK(std::memcmp(&original, &options, sizeof(options)) == 0);
      sl::DLSSGState state{};
      g_calls = 0;
      CHECK(HookState(viewport, state, &options) == result);
      CHECK(g_calls == 1 && g_received_address == &options);
      CHECK(state.numFramesActuallyPresented == 4 && state.numFramesToGenerateMax == 3);
      CHECK(state.status == sl::DLSSGStatus::eFailHDRFormatNotSupported);
      g_calls = 0;
      CHECK(HookTag(viewport, &null_tag, 1, nullptr) == result);
      CHECK(g_calls == 1 && g_received_address == &null_tag && g_received_count == 1);
      TestFrame frame;
      g_calls = 0;
      CHECK(HookConstants(constants, frame, viewport) == result);
      CHECK(g_calls == 1 && g_received_address == &constants);
      g_calls = 0;
      CHECK(HookTagForFrame(frame, viewport, &null_tag, 1, nullptr) == result);
      CHECK(g_calls == 1 && g_received_address == &null_tag);
    }
  }
  g_test_result = sl::Result::eOk;
  g_capture.Start(GetTickCount64(), 10000);
  g_set_tag = ForwardLegacyTag;
  g_calls = 0;
  CHECK(HookTag(viewport, &null_tag, 1, nullptr) == sl::Result::eOk);
  CHECK(g_calls == 1 && g_capture.events.size() == 1);
  CHECK(g_capture.events.front().frame == UINT32_MAX);
  CHECK(g_capture.events.front().viewport == 7);
  g_set_tag = TestTag;
  g_calls = 0;
  CHECK(HookTag(viewport, nullptr, 0, nullptr) == sl::Result::eOk);
  CHECK(g_calls == 1 && g_capture.events.back().kind == Kind::tag_batch);
  void* function = nullptr;
  CHECK(HookFunction(sl::kFeatureDLSS, "slDLSSGSetOptions", function) == sl::Result::eOk);
  CHECK(function == reinterpret_cast<void*>(&TestOptions));
  CHECK(HookFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", function) == sl::Result::eOk);
  CHECK(function == reinterpret_cast<void*>(&HookOptions));
  g_capture.Stop();
  std::cout << "PASS: version boundaries, nonmutation, exact-once forwarding, null/subrect tags, "
               "feature isolation, capture generations, bounded storage, contention, timeout.\n";
}
