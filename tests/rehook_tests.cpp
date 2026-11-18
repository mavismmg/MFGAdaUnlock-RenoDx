// 1.11.0 (F1): a hook another module removed is installed again, and a call that
// comes back through an older generation (a module that hooked over ours) passes
// through instead of looping. SOTTR with the "DLSS 5" addon, 2026-11-18: its
// unhook wrote back the bytes it had saved and took our _nvngx.dll hooks with it.
#include <cstdlib>
#include <cstring>
#include <iostream>
#include "../src/addons/mfgunlock/ngx_hook.hpp"
extern "C" __declspec(dllexport) void ReShadeLogMessage(void*, int, const char*) {}
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::abort(); } } while (false)
namespace h = mfgunlock::hook;

extern "C" __declspec(dllexport) __declspec(noinline) int RehookTarget(int value) {
  volatile int result = value * 3 + 1;
  return result + (value > 1000 ? RehookTarget(value - 1) : 0);
}

static int our_calls = 0, foreign_calls = 0;
int OurDetour(int value);
static h::Rehookable g_hook{"RehookTarget", reinterpret_cast<void*>(&OurDetour)};
static thread_local int t_depth = 0;
__declspec(noinline) int OurDetour(int value) {
  const h::Nesting nest(t_depth);
  CHECK(nest.level < 8);  // a loop would run away here
  const auto real = reinterpret_cast<int (*)(int)>(h::Trampoline(g_hook, nest.level));
  CHECK(real != nullptr);
  if (nest.level > 0) return real(value);
  ++our_calls;
  return real(value) + 1;
}

static int (*foreign_real)(int) = nullptr;
__declspec(noinline) int ForeignDetour(int value) {
  ++foreign_calls;
  return foreign_real(value) + 100;
}

// Another module's detour: code outside this image (mov rax, imm64; jmp rax),
// which FollowJumps does not follow, as a real foreign module's code would not be.
static void* ForeignThunk() {
  auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  CHECK(code != nullptr);
  code[0] = 0x48; code[1] = 0xB8;
  void* target = reinterpret_cast<void*>(&ForeignDetour);
  std::memcpy(code + 2, &target, sizeof(target));
  code[10] = 0xFF; code[11] = 0xE0;
  FlushInstructionCache(GetCurrentProcess(), code, 64);
  return code;
}

static void Write(void* where, const uint8_t* bytes, size_t size) {
  DWORD old = 0;
  CHECK(VirtualProtect(where, size, PAGE_EXECUTE_READWRITE, &old));
  std::memcpy(where, bytes, size);
  DWORD unused = 0;
  VirtualProtect(where, size, old, &unused);
  FlushInstructionCache(GetCurrentProcess(), where, size);
}

int main() {
  HMODULE self = GetModuleHandleW(nullptr);
  auto* target = reinterpret_cast<uint8_t*>(GetProcAddress(self, "RehookTarget"));
  CHECK(target != nullptr);
  const std::vector<h::Rehookable*> set{&g_hook};
  uint8_t original[32];
  std::memcpy(original, target, sizeof(original));

  // Not installed yet: nothing to reinstall.
  CHECK(!h::StillHooked(self, set));
  CHECK(!h::Reinstall(self, set, "test"));

  // First install.
  CHECK(h::InstallGeneration(self, set, "test"));
  CHECK(g_hook.generations.load() == 1);
  CHECK(RehookTarget(2) == 8);
  CHECK(our_calls == 1);
  CHECK(h::StillHooked(self, set));
  CHECK(!h::Reinstall(self, set, "test"));

  // Another module's unhook writes back the bytes it saved: our jump is gone.
  Write(target, original, sizeof(original));
  CHECK(RehookTarget(2) == 7);
  CHECK(our_calls == 1);
  CHECK(!h::StillHooked(self, set));
  CHECK(h::Reinstall(self, set, "test"));
  CHECK(g_hook.generations.load() == 2);
  CHECK(RehookTarget(2) == 8);
  CHECK(our_calls == 2);
  CHECK(h::StillHooked(self, set));

  // Another module hooks over ours: our jump survives inside its trampoline.
  foreign_real = reinterpret_cast<int (*)(int)>(target);
  void* thunk = ForeignThunk();
  CHECK(DetourTransactionBegin() == NO_ERROR);
  CHECK(DetourUpdateThread(GetCurrentThread()) == NO_ERROR);
  CHECK(DetourAttach(reinterpret_cast<PVOID*>(&foreign_real), thunk) == NO_ERROR);
  CHECK(DetourTransactionCommit() == NO_ERROR);
  CHECK(RehookTarget(2) == 108);  // foreign, then ours, then the function
  CHECK(our_calls == 3 && foreign_calls == 1);
  CHECK(!h::StillHooked(self, set));
  // Hooked again on top: the call comes back to us through the older generation,
  // which passes through. Our work runs once per call, the foreign one once too.
  CHECK(h::Reinstall(self, set, "test"));
  CHECK(g_hook.generations.load() == 3);
  CHECK(RehookTarget(2) == 108);
  CHECK(our_calls == 4 && foreign_calls == 2);

  // Bounded: after kMaxGenerations, no more reinstalls.
  Write(target, original, sizeof(original));
  CHECK(h::Reinstall(self, set, "test"));
  CHECK(g_hook.generations.load() == h::kMaxGenerations);
  Write(target, original, sizeof(original));
  CHECK(!h::Reinstall(self, set, "test"));
  CHECK(RehookTarget(2) == 7);
  std::cout << "a removed hook is installed again, and a call that comes back through an older one passes through\n";
}
