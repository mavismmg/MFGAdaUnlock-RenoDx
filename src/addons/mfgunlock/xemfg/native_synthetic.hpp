// SPDX-License-Identifier: MIT
#pragma once
#include "native_internal.hpp"

// Synthesized source frames for input adapters without Reflex/PCL markers
// (FidelityFX FG, plan 5.3; DLSS-SR, plan 5.4). User decision 2026-10-22:
// markers and the XeLL wait are synthesized around the source Present.
// Ids are consecutive from 1: xellSleep + SIMULATION_START right after the
// previous Present (or at the submission, when it comes first: see
// synthetic_queue.hpp), SIMULATION_END and
// RENDERSUBMIT_START when the adapter has the frame's inputs,
// RENDERSUBMIT_END/PRESENT_START before and PRESENT_END after Present.
namespace mfgunlock::xemfg::native::synthetic {
// Adapter identities (compared by address; one entity across translation units).
inline constexpr char kFidelityFx[]="fidelityfx";
inline constexpr char kDlssSr[]="dlss_sr";
// One adapter owns an output's synthesized frames; the adapter sets the
// latency route state (native generator off, XeLL as the only wait).
void Begin(const std::shared_ptr<Output>& o,const char* adapter);
void End(const Output* o);
// The adapter that owns the active output, or nullptr. Lock-free.
const char* Owner() noexcept;
// The frame id that receives this adapter's inputs; 0 when it does not own o
// or Streamline is delivering frames. The first accepted submission makes the
// synthesized frames active (1.5.1): until then an owner is only armed and
// never touches frame ids, markers or waits.
uint32_t SubmitFrame(const std::shared_ptr<Output>& o,const char* adapter);
// The adapter owns the output and its synthesized frames are active.
bool Driving(const char* adapter) noexcept;
void BeforeSourcePresent(const std::shared_ptr<Output>& o);
void AfterSourcePresent(const std::shared_ptr<Output>& o);
// 1.7.1: totals for native_summary. Never waits: the caller may hold Output::io.
struct Counters { uint64_t evaluations,merged,emptyPresents; bool pipelined,refused; };
bool TryReadCounters(Counters& out);
}
