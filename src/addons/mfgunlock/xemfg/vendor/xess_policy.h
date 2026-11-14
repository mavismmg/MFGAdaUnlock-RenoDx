#pragma once
#include <cstdint>

namespace xess_mod
{
// Every portable context is created with five generated frames (6x). Lower counts, count 1 included
// (host_unlock.h fg_live_count_no_reinit), run inside it without reinitialization; at a live count below
// the capacity the output matches a context created at that count, so live switches take microseconds
// and match fixed-count output from the first frame.
constexpr uint32_t kGeneratedCapacity = 5;
constexpr bool ValidMultiplier(uint32_t value) noexcept
{
    return value == 0 || (value >= 2 && value <= kGeneratedCapacity + 1);
}
constexpr uint32_t GeneratedCount(uint32_t value) noexcept
{
    return value == 0 ? 1 : value - 1;
}
constexpr bool CanApply(uint32_t desired, uint32_t capacity,
    bool initialized, bool exactOwner, bool closing) noexcept
{
    return ValidMultiplier(desired) && initialized && exactOwner && !closing
        && capacity == kGeneratedCapacity && GeneratedCount(desired) <= capacity;
}
struct ColorTags
{
    uint32_t frame = 0;
    uint32_t mask = 0;
    uint32_t width[5]{};
    uint32_t height[5]{};
    uint32_t x[5]{};
    uint32_t y[5]{};

    void Reset(uint32_t id) noexcept { *this = {}; frame = id; }
    bool Has(unsigned type) const noexcept { return (mask & (1u << type)) != 0; }
    bool Same(unsigned a, unsigned b) const noexcept
    {
        return Has(a) && Has(b) && width[a] != 0 && height[a] != 0
            && width[a] == width[b] && height[a] == height[b]
            && x[a] == x[b] && y[a] == y[b];
    }
    bool CanCompose(uint32_t id, unsigned mode, uint32_t outputWidth, uint32_t outputHeight) const noexcept
    {
        if (frame != id || mode == 1) return false;
        // Resource ids: HUDless 0, depth 1, motion 2, UI 3, backbuffer 4.
        const auto matchesOutput = [&](unsigned type) {
            return Has(type) && width[type] == outputWidth && height[type] == outputHeight
                && x[type] == 0 && y[type] == 0;
        };
        const bool backbufferHudless = Has(4) ? Same(0, 4) : matchesOutput(0);
        const bool backbufferUi = Has(4) ? Same(3, 4) : matchesOutput(3);
        if (mode == 2) return backbufferUi;
        if (mode == 3) return Same(0, 3);
        if (mode == 4) return backbufferHudless;
        if (mode == 5) return Same(0, 3) && backbufferHudless;
        if (Has(0) && Has(3)) return Same(0, 3) && (Has(4) ? Same(0, 4) : true);
        return backbufferHudless || backbufferUi;
    }
};
}
