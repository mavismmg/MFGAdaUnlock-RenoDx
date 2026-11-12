#include <windows.h>

#include <atomic>
#include <barrier>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

#include "../src/addons/mfgunlock/ngx_param_vtable.hpp"

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition     \
                << '\n';                                                        \
      std::abort();                                                              \
    }                                                                           \
  } while (false)

namespace {

using GetUInt32Fn = int (*)(void* self, const char* name, unsigned int* out);
using mfgunlock::ngxparam::kGetUInt32Slot;
using mfgunlock::ngxparam::PatchResult;
using mfgunlock::ngxparam::VTablePatch;

int g_original_calls = 0;
int g_hook_calls = 0;
VTablePatch* g_state = nullptr;

int OriginalGet(void*, const char*, unsigned int* out) {
  ++g_original_calls;
  *out = 1;
  return 0;
}

int HookGet(void* self, const char* name, unsigned int* out) {
  ++g_hook_calls;
  const int result = reinterpret_cast<GetUInt32Fn>(static_cast<void*>(g_state->original))(self, name, out);
  *out = 5;
  return result;
}

// A parameter object whose vtable lives on a read-only page, like the
// provider's vtable in .rdata.
struct FakeParameter {
  void** vtable;
};

void** MakeReadOnlyVTable() {
  auto** table = static_cast<void**>(
      VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  CHECK(table != nullptr);
  for (size_t i = 0; i < 16; ++i) table[i] = nullptr;
  table[kGetUInt32Slot] = reinterpret_cast<void*>(&OriginalGet);
  DWORD previous = 0;
  CHECK(VirtualProtect(table, 4096, PAGE_READONLY, &previous) != 0);
  return table;
}

DWORD Protection(const void* address) {
  MEMORY_BASIC_INFORMATION info{};
  CHECK(VirtualQuery(address, &info, sizeof(info)) == sizeof(info));
  return info.Protect;
}

unsigned int CallSlot(FakeParameter& object) {
  unsigned int value = 0;
  reinterpret_cast<GetUInt32Fn>(object.vtable[kGetUInt32Slot])(&object, "DLSSG.MultiFrameCountMax", &value);
  return value;
}

void PatchesAndRestoresSlot() {
  FakeParameter object{MakeReadOnlyVTable()};
  VTablePatch state;
  g_state = &state;
  g_original_calls = g_hook_calls = 0;

  CHECK(mfgunlock::ngxparam::Patch(state, nullptr, reinterpret_cast<void*>(&HookGet)) ==
        PatchResult::kNoObject);
  CHECK(!state.patched.load());

  CHECK(mfgunlock::ngxparam::Patch(state, &object, reinterpret_cast<void*>(&HookGet)) ==
        PatchResult::kPatched);
  CHECK(state.patched.load());
  CHECK(object.vtable[kGetUInt32Slot] == reinterpret_cast<void*>(&HookGet));
  CHECK(static_cast<void*>(state.original) == reinterpret_cast<void*>(&OriginalGet));
  CHECK(Protection(&object.vtable[kGetUInt32Slot]) == PAGE_READONLY);

  // The hook answers and still chains to the provider's own Get.
  CHECK(CallSlot(object) == 5);
  CHECK(g_hook_calls == 1 && g_original_calls == 1);

  // Every parameter object shares the vtable: a second patch is a no-op.
  FakeParameter other{object.vtable};
  CHECK(mfgunlock::ngxparam::Patch(state, &other, reinterpret_cast<void*>(&HookGet)) ==
        PatchResult::kAlreadyPatched);
  CHECK(static_cast<void*>(state.original) == reinterpret_cast<void*>(&OriginalGet));

  mfgunlock::ngxparam::Restore(state);
  CHECK(!state.patched.load());
  CHECK(object.vtable[kGetUInt32Slot] == reinterpret_cast<void*>(&OriginalGet));
  CHECK(Protection(&object.vtable[kGetUInt32Slot]) == PAGE_READONLY);
  CHECK(CallSlot(object) == 1);
  VirtualFree(object.vtable, 0, MEM_RELEASE);
}

void RejectsObjectWithoutVTable() {
  FakeParameter object{nullptr};
  VTablePatch state;
  CHECK(mfgunlock::ngxparam::Patch(state, &object, reinterpret_cast<void*>(&HookGet)) ==
        PatchResult::kNoObject);
  CHECK(!state.patched.load());
}

// A slot that already holds the hook (with no original recorded in this
// state) is left alone: adopting the hook as the original would make HookGet
// call itself.
void RefusesToAdoptTheHookAsOriginal() {
  FakeParameter object{MakeReadOnlyVTable()};
  VTablePatch first;
  g_state = &first;
  CHECK(mfgunlock::ngxparam::Patch(first, &object, reinterpret_cast<void*>(&HookGet)) ==
        PatchResult::kPatched);
  VTablePatch stale;
  CHECK(mfgunlock::ngxparam::Patch(stale, &object, reinterpret_cast<void*>(&HookGet)) ==
        PatchResult::kForeignHook);
  CHECK(!stale.patched.load());
  CHECK(static_cast<void*>(stale.original) == nullptr);
  CHECK(object.vtable[kGetUInt32Slot] == reinterpret_cast<void*>(&HookGet));
  mfgunlock::ngxparam::Restore(first);
  CHECK(object.vtable[kGetUInt32Slot] == reinterpret_cast<void*>(&OriginalGet));
  VirtualFree(object.vtable, 0, MEM_RELEASE);
}

// NGX hands out parameter blocks on several threads. However the calls
// interleave, exactly one installs the hook, and the saved original is never
// the hook itself (that would make HookGet call itself until the stack ends).
void ConcurrentPatchKeepsTheOriginal() {
  constexpr int kThreads = 8;
  constexpr int kRounds = 400;
  FakeParameter object{MakeReadOnlyVTable()};
  VTablePatch state;
  g_state = &state;
  std::atomic<int> installed{0};
  std::barrier start(kThreads);
  std::barrier done(kThreads, []() noexcept {});
  std::vector<std::thread> threads;
  std::atomic_bool failed{false};
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int round = 0; round < kRounds; ++round) {
        start.arrive_and_wait();
        if (mfgunlock::ngxparam::Patch(state, &object, reinterpret_cast<void*>(&HookGet)) ==
            PatchResult::kPatched)
          installed.fetch_add(1);
        done.arrive_and_wait();
        if (t == 0) {
          if (installed.exchange(0) != 1 ||
              static_cast<void*>(state.original) != reinterpret_cast<void*>(&OriginalGet) ||
              object.vtable[kGetUInt32Slot] != reinterpret_cast<void*>(&HookGet))
            failed = true;
          mfgunlock::ngxparam::Restore(state);
          if (object.vtable[kGetUInt32Slot] != reinterpret_cast<void*>(&OriginalGet)) failed = true;
        }
        done.arrive_and_wait();
      }
    });
  }
  for (auto& thread : threads) thread.join();
  CHECK(!failed.load());
  VirtualFree(object.vtable, 0, MEM_RELEASE);
}

}  // namespace

// 1.10.0 (plan 1.2): the int slot (12) is patched with its own state, next to
// the unsigned slot (11), and each restore only touches its own slot.
using GetInt32Fn = int (*)(void* self, const char* name, int* out);
int OriginalGetInt(void*, const char*, int* out) {
  *out = 1;
  return 0;
}
VTablePatch* g_int_state = nullptr;
int HookGetInt(void* self, const char* name, int* out) {
  const int result = reinterpret_cast<GetInt32Fn>(static_cast<void*>(g_int_state->original))(self, name, out);
  *out = 7;
  return result;
}
void PatchesTwoSlotsIndependently() {
  using mfgunlock::ngxparam::kGetInt32Slot;
  auto** table = static_cast<void**>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  CHECK(table != nullptr);
  for (size_t i = 0; i < 16; ++i) table[i] = nullptr;
  table[kGetUInt32Slot] = reinterpret_cast<void*>(&OriginalGet);
  table[kGetInt32Slot] = reinterpret_cast<void*>(&OriginalGetInt);
  DWORD previous = 0;
  CHECK(VirtualProtect(table, 4096, PAGE_READONLY, &previous) != 0);
  FakeParameter object{table};
  VTablePatch state, int_state;
  g_state = &state;
  g_int_state = &int_state;
  CHECK(mfgunlock::ngxparam::Patch(int_state, &object, reinterpret_cast<void*>(&HookGetInt), kGetInt32Slot) ==
        PatchResult::kPatched);
  CHECK(mfgunlock::ngxparam::Patch(state, &object, reinterpret_cast<void*>(&HookGet)) == PatchResult::kPatched);
  int value = 0;
  reinterpret_cast<GetInt32Fn>(object.vtable[kGetInt32Slot])(&object, "FrameGeneration.Available", &value);
  CHECK(value == 7);
  CHECK(CallSlot(object) == 5);
  mfgunlock::ngxparam::Restore(int_state);
  CHECK(object.vtable[kGetInt32Slot] == reinterpret_cast<void*>(&OriginalGetInt));
  CHECK(CallSlot(object) == 5);  // the unsigned slot keeps its hook
  mfgunlock::ngxparam::Restore(state);
  CHECK(object.vtable[kGetUInt32Slot] == reinterpret_cast<void*>(&OriginalGet));
}

int main() {
  PatchesAndRestoresSlot();
  PatchesTwoSlotsIndependently();
  RejectsObjectWithoutVTable();
  RefusesToAdoptTheHookAsOriginal();
  ConcurrentPatchKeepsTheOriginal();
  std::cout << "ngx_param_vtable_tests passed\n";
  return 0;
}
