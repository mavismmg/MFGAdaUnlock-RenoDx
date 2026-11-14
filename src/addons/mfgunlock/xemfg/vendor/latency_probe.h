// SPDX-License-Identifier: MIT
#pragma once
// MFGAdaUnlock 1.10.0: upstream's latency probe feeds MFG Unlocker's own menu
// (latency_probe.cpp, latency_monitor.h), which this build does not carry. The
// pacing component only labels its outputs for it, so this is a no-op.
#include <cstdint>

namespace latency_probe {
inline void XefgOutput(uint64_t, bool) noexcept {}
}
