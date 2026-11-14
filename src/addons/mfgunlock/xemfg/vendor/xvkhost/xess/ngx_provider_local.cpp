// SPDX-License-Identifier: MIT
// MFGAdaUnlock 1.10.0: this build's part of upstream's xess/ngx_provider.cpp (dashdogy/MFG-Unlocker 128d081, MIT).
// Upstream's unit also owns the provider API table and its DLL export for OptiScaler's shim; this build never
// exports it (XMX on Vulkan calls the provider from the addon's NGX Vulkan hooks), so only the process-wide refusal
// and the whole-batch switch the provider reads are here.
#include "ngx_provider.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

#include "ngx_provider_policy.h"

namespace ngx_provider
{
namespace
{
std::mutex gRefusalLock;
std::string gRefusal;
std::atomic<bool> gRefused{false};
}

void NoteRefusal(const std::string& reason) noexcept
{
    try {
        std::lock_guard lock(gRefusalLock);
        if (gRefused.load(std::memory_order_acquire)) return;
        gRefusal = reason;
        gRefused.store(true, std::memory_order_release);
    } catch (...) {
    }
}

uint32_t Refused(char* reason, uint32_t capacity) noexcept
{
    if (!gRefused.load(std::memory_order_acquire)) return 0;
    if (reason && capacity) {
        std::lock_guard lock(gRefusalLock);
        const size_t length = (std::min)(gRefusal.size(), size_t(capacity - 1));
        std::memcpy(reason, gRefusal.data(), length);
        reason[length] = '\0';
    }
    return 1;
}

uint32_t MaxGeneratedFrames() noexcept { return gRefused.load(std::memory_order_acquire) ? 0 : kMaxGenerated; }

bool Offered() noexcept { return false; }

bool WholeBatch() noexcept
{
    static const bool whole = [] {
        wchar_t value[8]{};
        return GetEnvironmentVariableW(L"RTXMFG_PROVIDER_WHOLE_BATCH", value, 8) != 0 && value[0] == L'1';
    }();
    return whole;
}
}
