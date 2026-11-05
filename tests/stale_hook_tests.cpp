// 1.8.3: a hook left by a copy of the addon that ReShade unloaded and loaded
// again at the same address must not be hooked over: the new trampoline would
// call the detour forever (TLOU Part II, 1.8.2, stack overflow in
// HookedCreateFeature).
#include <cstdlib>
#include <iostream>
#include "../src/addons/mfgunlock/ngx_hook.hpp"
extern "C" __declspec(dllexport) void ReShadeLogMessage(void*, int, const char*) {}
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::abort(); } } while (false)
namespace h = mfgunlock::hook;

static int detour_calls = 0;
__declspec(noinline) int Target(int value) {
  volatile int result = value * 3 + 1;
  return result + (value > 1000 ? Target(value - 1) : 0);
}
static int (*real_target)(int) = nullptr;
__declspec(noinline) int Detour(int value) {
  ++detour_calls;
  if (detour_calls > 64) return -1;  // the recursion this test guards against
  return real_target(value) + 1;
}

int main() {
  CHECK(!h::JumpsIntoSelf(reinterpret_cast<const void*>(&Target)));
  CHECK(!h::JumpsIntoSelf(reinterpret_cast<const void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "Sleep"))));

  // The first copy hooks the function.
  real_target = &Target;
  CHECK(h::InstallOne(reinterpret_cast<void**>(&real_target), reinterpret_cast<void*>(&Detour)));
  CHECK(Target(2) == 8);
  CHECK(detour_calls == 1);
  // That hook now leads into this image: a later copy sees its jump.
  CHECK(h::JumpsIntoSelf(reinterpret_cast<const void*>(&Target)));

  // The copy that "reloaded" starts with a fresh, unresolved trampoline.
  void* stale_copy = reinterpret_cast<void*>(&Target);
  CHECK(!h::InstallOne(&stale_copy, reinterpret_cast<void*>(&Detour)));
  CHECK(stale_copy == reinterpret_cast<void*>(&Target));
  void* named = nullptr;
  std::vector<h::HookItem> hooks{{"GetTickCount", &named, reinterpret_cast<void*>(&Detour)}};
  CHECK(h::Install(GetModuleHandleW(L"kernel32.dll"), hooks, "kernel32.dll"));
  CHECK(h::Uninstall(hooks));

  detour_calls = 0;
  CHECK(Target(2) == 8);
  CHECK(detour_calls == 1);
  CHECK(h::Uninstall({{"Target", reinterpret_cast<void**>(&real_target), reinterpret_cast<void*>(&Detour)}}));
  CHECK(!h::JumpsIntoSelf(reinterpret_cast<const void*>(&Target)));
  std::cout << "a hook leading into this image is refused, not stacked\n";
}
