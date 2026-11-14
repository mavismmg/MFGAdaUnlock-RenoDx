#pragma once
// The frame XeFG presents on the calling thread. sdk_pacing's hook of XeFG's internal present opens the scope
// around the call, and the xvk swap chain's Present1 reads it for NVIDIA Reflex: out-of-band present markers name the
// batch's frame, and the real frame's present carries its present id. No Vulkan types, so modules on the pinned Vulkan
// 1.2 headers can include it.
#include <cstdint>

namespace xvk
{
void SetOutputFrame(uint64_t frame, bool generated) noexcept;   // XeFG's frame id (the low 32 bits are the host's)
void ClearOutputFrame() noexcept;

struct OutputFrameScope
{
    OutputFrameScope(uint64_t frame, bool generated) noexcept { SetOutputFrame(frame, generated); }
    ~OutputFrameScope() { ClearOutputFrame(); }
    OutputFrameScope(const OutputFrameScope&) = delete;
    OutputFrameScope& operator=(const OutputFrameScope&) = delete;
};
}
