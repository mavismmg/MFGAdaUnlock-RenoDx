// SPDX-License-Identifier: MIT
#pragma once
// 1.8.3: the D3D12 state each resource had when the game tagged it for
// Streamline. DLSS-G hands NGX the game's own depth, motion vectors and
// HUD-less color in those states (a tag valid until Present is not copied),
// and only a resource Streamline copied arrives in another one. XMX assumed
// COPY_DEST for all of them: in the Streamline Sample 2.14.16 that broke every
// frame's barriers (about 15,000 debug-layer errors in 35 s; none with
// NVIDIA's generator). Filled by the slSetTag hooks, read by XMX's copies.
#include <windows.h>
#include <array>
#include <cstdint>

namespace mfgunlock::tagstates {
struct Entry {
  const void* resource{};
  uint32_t state{};
};
inline SRWLOCK lock = SRWLOCK_INIT;
inline std::array<Entry, 32> entries{};
inline uint32_t next = 0;

inline void Record(const void* resource, uint32_t state) {
  if (resource == nullptr) return;
  AcquireSRWLockExclusive(&lock);
  Entry* slot = nullptr;
  for (auto& entry : entries)
    if (entry.resource == resource) slot = &entry;
  if (slot == nullptr) slot = &entries[next++ % entries.size()];
  *slot = {resource, state};
  ReleaseSRWLockExclusive(&lock);
}

// The tagged state, or `fallback` for a resource no tag named (a copy
// Streamline made, which it leaves in the state NGX expects).
inline uint32_t StateOf(const void* resource, uint32_t fallback) {
  uint32_t state = fallback;
  AcquireSRWLockShared(&lock);
  for (const auto& entry : entries)
    if (entry.resource == resource && resource != nullptr) state = entry.state;
  ReleaseSRWLockShared(&lock);
  return state;
}
}  // namespace mfgunlock::tagstates
