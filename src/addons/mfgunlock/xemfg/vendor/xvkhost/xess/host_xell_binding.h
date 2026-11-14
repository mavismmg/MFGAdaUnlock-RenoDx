#pragma once
// xellbind.1: the XeLL that the XeMFG standalone hosts' XeFG uses is the one beside it.
//
// The hosts (d3d12_host.cpp, vulkan/vk_host.cpp) load the player's libxell.dll and libxess_fg.dll by path, and create
// their XeLL context with that libxell. XeFG finds its XeLL at run time by name: libxess_fg 1.3.1 calls
// GetModuleHandleExA(0, "libxell.dll"), which returns the first libxell.dll the process loaded. A game that ships its
// own XeLL (Cyberpunk 2077: libxell 1.1.0.6 beside the EXE, loaded at start) gives XeFG that copy, and XeFG refuses the
// host's context: "XeLL context ... is not supported", xefgSwapChainSetLatencyReduction fails (xefg_latency).
//
// Bind replaces the module-lookup imports of the pair's libxess_fg only (its own IAT slots, caller-scoped:
// GetModuleHandleExA / GetModuleHandleA / LoadLibraryA / LoadLibraryExA): a lookup of the bare name libxell.dll answers
// with the pair's libxell (a reference taken as the call would take one); every other name, flag or caller passes
// through. Each slot is replaced only while it holds the system function (protected_pointer, read back). Logged:
// which lookups were bound and whether another libxell.dll is loaded.
#include <Windows.h>

namespace xess_host_xell
{
void Bind(HMODULE fg, HMODULE ll) noexcept;
}
