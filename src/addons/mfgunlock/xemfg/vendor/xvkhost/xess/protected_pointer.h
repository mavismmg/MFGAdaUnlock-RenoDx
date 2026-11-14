#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <intrin.h>

namespace protected_pointer
{
using ProtectMemoryFn = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD, PDWORD);
using FlushInstructionCacheFn = BOOL(WINAPI*)(HANDLE, LPCVOID, SIZE_T);

enum class PublishDisposition : uint32_t
{
    eNotPublishedRestored = 0,
    ePublishedRestored = 1,
    eRolledBackRestored = 2,
    eIndeterminate = 3,
};

struct PublishResult
{
    PublishDisposition disposition = PublishDisposition::eIndeterminate;
    bool replacementWasPublished = false;
};

inline void* ReadPointer(uintptr_t address) noexcept
{
    return ReadPointerAcquire(
        reinterpret_cast<PVOID const volatile*>(address));
}

inline bool ProtectionMatches(uintptr_t address, DWORD expected,
    size_t size = 1) noexcept
{
    MEMORY_BASIC_INFORMATION memory{};
    if (!address || size == 0
        || VirtualQuery(reinterpret_cast<const void*>(address), &memory,
               sizeof(memory))
            != sizeof(memory)
        || memory.State != MEM_COMMIT)
    {
        return false;
    }
    const uintptr_t regionBegin = reinterpret_cast<uintptr_t>(
        memory.BaseAddress);
    if (memory.RegionSize < size || address < regionBegin
        || address - regionBegin > memory.RegionSize - size)
    {
        return false;
    }
    constexpr DWORD kProtectionMask = 0xFFu | PAGE_GUARD | PAGE_NOCACHE
        | PAGE_WRITECOMBINE;
    return (memory.Protect & kProtectionMask)
        == (expected & kProtectionMask);
}

inline bool QueryProtection(uintptr_t address, DWORD& protection,
    size_t size = 1) noexcept
{
    MEMORY_BASIC_INFORMATION memory{};
    if (!address || size == 0
        || VirtualQuery(reinterpret_cast<const void*>(address), &memory,
               sizeof(memory))
            != sizeof(memory)
        || memory.State != MEM_COMMIT)
    {
        return false;
    }
    const uintptr_t regionBegin = reinterpret_cast<uintptr_t>(
        memory.BaseAddress);
    if (memory.RegionSize < size || address < regionBegin
        || address - regionBegin > memory.RegionSize - size)
    {
        return false;
    }
    protection = memory.Protect;
    return true;
}

inline bool SetAndVerifyProtection(void* address, size_t size,
    DWORD protection, ProtectMemoryFn protectMemory) noexcept
{
    DWORD ignored = 0;
    protectMemory(address, size, protection, &ignored);
    return ProtectionMatches(
        reinterpret_cast<uintptr_t>(address), protection, size);
}

inline bool RestoreProtectionWithRetry(void* address, size_t size,
    DWORD protection, ProtectMemoryFn protectMemory) noexcept
{
    // A failed VirtualProtect is normally non-mutating, but retry once so a
    // transient injected or system failure cannot leave our temporary RW page
    // behind.  Verification, rather than the API return alone, is authoritative.
    for (uint32_t attempt = 0; attempt < 2; ++attempt)
    {
        if (SetAndVerifyProtection(address, size, protection, protectMemory))
            return true;
    }
    return false;
}

inline PublishResult ReplaceProtectedPointer(uintptr_t address,
    uintptr_t expected, uintptr_t replacement,
    ProtectMemoryFn protectMemory = &VirtualProtect,
    DWORD temporaryWritableProtection = PAGE_READWRITE) noexcept
{
    if (!address || !expected || !replacement || !protectMemory
        || (temporaryWritableProtection != PAGE_READWRITE
            && temporaryWritableProtection != PAGE_WRITECOPY)
        || (address & (alignof(void*) - 1)) != 0)
    {
        return {};
    }

    auto* const destination = reinterpret_cast<void* volatile*>(address);
    void* const protectionAddress = reinterpret_cast<void*>(address);
    DWORD oldProtection = 0;
    if (!QueryProtection(address, oldProtection, sizeof(void*)))
        return {};
    if (!SetAndVerifyProtection(protectionAddress, sizeof(void*),
            temporaryWritableProtection, protectMemory))
    {
        // VirtualProtect can mutate the page even when it reports failure.
        // Best-effort restoration keeps an initial protection failure from
        // leaving an IAT or immutable table writable. A competing writer may
        // also have published the exact replacement while the page was
        // transiently writable, so re-read only after restoring protection;
        // callers use replacementWasPublished to retain replacement-owned
        // storage even though this attempt cannot claim publication.
        const bool restored = RestoreProtectionWithRetry(protectionAddress,
            sizeof(void*), oldProtection, protectMemory);
        void* const observed = ReadPointer(address);
        void* const expectedPointer = reinterpret_cast<void*>(expected);
        void* const replacementPointer = reinterpret_cast<void*>(replacement);
        return {restored && observed == expectedPointer
                ? PublishDisposition::eNotPublishedRestored
                : PublishDisposition::eIndeterminate,
            observed == replacementPointer};
    }

    void* const expectedPointer = reinterpret_cast<void*>(expected);
    void* const replacementPointer = reinterpret_cast<void*>(replacement);
    void* const previous = InterlockedCompareExchangePointer(destination,
        replacementPointer, expectedPointer);
    if (previous != expectedPointer)
    {
        const bool replacementObservedBeforeRestore =
            previous == replacementPointer;
        const bool foreignPointerObserved =
            previous != replacementPointer;
        const bool restored = RestoreProtectionWithRetry(protectionAddress,
            sizeof(void*), oldProtection, protectMemory);
        void* const observed = ReadPointer(address);
        return {!foreignPointerObserved && restored
                    && observed == expectedPointer
                ? PublishDisposition::eNotPublishedRestored
                : PublishDisposition::eIndeterminate,
            replacementObservedBeforeRestore
                || observed == replacementPointer};
    }

    if (SetAndVerifyProtection(protectionAddress, sizeof(void*), oldProtection,
            protectMemory))
    {
        const void* const observed = ReadPointer(address);
        if (observed == replacementPointer)
            return {PublishDisposition::ePublishedRestored, true};
        if (observed == expectedPointer)
            return {PublishDisposition::eRolledBackRestored, true};
        return {PublishDisposition::eIndeterminate, true};
    }

    // The replacement was visible, but the requested protection could not be
    // verified. VirtualProtect is allowed to have changed the page despite a
    // failure result, so it is not safe to assume the slot remains writable.
    // Re-observe both state dimensions before deciding whether to publish,
    // preserve a third writer, or attempt rollback.
    void* const observedAfterRestore = ReadPointer(address);
    if (observedAfterRestore == replacementPointer
        && ProtectionMatches(address, oldProtection, sizeof(void*)))
    {
        return {PublishDisposition::ePublishedRestored, true};
    }
    if (observedAfterRestore != replacementPointer)
    {
        const bool protectionRestored = RestoreProtectionWithRetry(
            protectionAddress, sizeof(void*), oldProtection, protectMemory);
        if (observedAfterRestore == expectedPointer && protectionRestored
            && ReadPointer(address) == expectedPointer)
        {
            return {PublishDisposition::eRolledBackRestored, true};
        }
        return {PublishDisposition::eIndeterminate, true};
    }

    // Never issue the rollback CAS until writable protection is positively
    // established. If that cannot be proven, leave the already-visible
    // replacement in place, retry the original protection, and classify the
    // final exact state without risking a write fault.
    if (!SetAndVerifyProtection(protectionAddress, sizeof(void*),
            temporaryWritableProtection, protectMemory))
    {
        const bool protectionRestored = RestoreProtectionWithRetry(
            protectionAddress, sizeof(void*), oldProtection, protectMemory);
        if (protectionRestored && ReadPointer(address) == replacementPointer)
            return {PublishDisposition::ePublishedRestored, true};
        return {PublishDisposition::eIndeterminate, true};
    }

    // Never free replacement storage after this point: another reader may
    // have retained the briefly published pointer.
    void* const rollbackPrevious = InterlockedCompareExchangePointer(
        destination, expectedPointer, replacementPointer);
    const bool slotRestored = rollbackPrevious == replacementPointer
        || rollbackPrevious == expectedPointer;
    const bool protectionRestored = RestoreProtectionWithRetry(protectionAddress,
        sizeof(void*), oldProtection, protectMemory);
    if (slotRestored && protectionRestored
        && ReadPointer(address) == expectedPointer)
    {
        return {PublishDisposition::eRolledBackRestored, true};
    }
    return {PublishDisposition::eIndeterminate, true};
}

inline bool BytesEqual(uintptr_t address, const void* expected,
    size_t size) noexcept
{
    return address && expected && size != 0
        && std::memcmp(reinterpret_cast<const void*>(address), expected, size)
            == 0;
}

inline bool AtomicCompareExchangeBytes(uintptr_t address,
    const void* expected, const void* replacement, size_t size,
    unsigned char* observedBytes) noexcept
{
    if (!address || !expected || !replacement || !observedBytes
        || (size != 1 && size != 2 && size != 4 && size != 8)
        || (address & (size - 1)) != 0)
    {
        return false;
    }
    if (size == 1)
    {
        char expectedValue = 0;
        char replacementValue = 0;
        std::memcpy(&expectedValue, expected, 1);
        std::memcpy(&replacementValue, replacement, 1);
        const char observed = _InterlockedCompareExchange8(
            reinterpret_cast<volatile char*>(address), replacementValue,
            expectedValue);
        std::memcpy(observedBytes, &observed, 1);
        return true;
    }
    if (size == 2)
    {
        short expectedValue = 0;
        short replacementValue = 0;
        std::memcpy(&expectedValue, expected, 2);
        std::memcpy(&replacementValue, replacement, 2);
        const short observed = _InterlockedCompareExchange16(
            reinterpret_cast<volatile short*>(address), replacementValue,
            expectedValue);
        std::memcpy(observedBytes, &observed, 2);
        return true;
    }
    if (size == 4)
    {
        long expectedValue = 0;
        long replacementValue = 0;
        std::memcpy(&expectedValue, expected, 4);
        std::memcpy(&replacementValue, replacement, 4);
        const long observed = InterlockedCompareExchange(
            reinterpret_cast<volatile long*>(address), replacementValue,
            expectedValue);
        std::memcpy(observedBytes, &observed, 4);
        return true;
    }
    LONG64 expectedValue = 0;
    LONG64 replacementValue = 0;
    std::memcpy(&expectedValue, expected, 8);
    std::memcpy(&replacementValue, replacement, 8);
    const LONG64 observed = InterlockedCompareExchange64(
        reinterpret_cast<volatile LONG64*>(address), replacementValue,
        expectedValue);
    std::memcpy(observedBytes, &observed, 8);
    return true;
}

inline PublishResult ReplaceProtectedBytes(uintptr_t address,
    const void* expected, const void* replacement, size_t size,
    DWORD finalProtection = PAGE_EXECUTE_READ,
    ProtectMemoryFn protectMemory = &VirtualProtect,
    FlushInstructionCacheFn flushInstructionCache = &FlushInstructionCache,
    DWORD temporaryWritableProtection = PAGE_EXECUTE_READWRITE)
    noexcept
{
    if (!address || !expected || !replacement
        || (size != 1 && size != 2 && size != 4 && size != 8)
        || (address & (size - 1)) != 0 || !protectMemory
        || !flushInstructionCache
        || (temporaryWritableProtection != PAGE_EXECUTE_READWRITE
            && temporaryWritableProtection != PAGE_EXECUTE_WRITECOPY))
        return {};

    void* const destination = reinterpret_cast<void*>(address);
    DWORD observedProtection = 0;
    if (!QueryProtection(address, observedProtection, size))
        return {};

    // Executable mutations are trusted only when the page was already in the
    // exact canonical non-writable state. Best-effort repair of a pre-existing
    // writable page is useful containment, but its bytes remain indeterminate
    // for this attempt and must never authorize publication.
    if (!ProtectionMatches(address, finalProtection, size))
    {
        RestoreProtectionWithRetry(destination, size, finalProtection,
            protectMemory);
        return {PublishDisposition::eIndeterminate,
            BytesEqual(address, replacement, size)};
    }
    if (BytesEqual(address, replacement, size))
        return {PublishDisposition::ePublishedRestored, true};
    if (!BytesEqual(address, expected, size))
        return {PublishDisposition::eIndeterminate, false};

    if (!SetAndVerifyProtection(destination, size, temporaryWritableProtection,
            protectMemory))
    {
        const bool restored = RestoreProtectionWithRetry(destination, size,
            finalProtection, protectMemory);
        const bool expectedObserved = BytesEqual(address, expected, size);
        const bool replacementObserved = BytesEqual(
            address, replacement, size);
        if (replacementObserved)
            flushInstructionCache(GetCurrentProcess(), destination, size);
        return {restored && expectedObserved
                ? PublishDisposition::eNotPublishedRestored
                : PublishDisposition::eIndeterminate,
            replacementObserved};
    }

    // Revalidate after making the page writable. A debugger or another
    // diagnostic writer must not be overwritten merely because the earlier
    // read matched.
    if (!BytesEqual(address, expected, size))
    {
        const bool replacementObserved =
            BytesEqual(address, replacement, size);
        if (replacementObserved)
            flushInstructionCache(GetCurrentProcess(), destination, size);
        const bool restored = RestoreProtectionWithRetry(destination, size,
            finalProtection, protectMemory);
        (void)restored;
        // A positively observed foreign/replacement value cannot be laundered
        // into a clean non-publication merely because another writer restores
        // the expected bytes before the final read.
        return {PublishDisposition::eIndeterminate, replacementObserved};
    }

    unsigned char observed[8]{};
    if (!AtomicCompareExchangeBytes(address, expected, replacement, size,
            observed))
    {
        const bool restored = RestoreProtectionWithRetry(destination, size,
            finalProtection, protectMemory);
        return {restored && BytesEqual(address, expected, size)
                    ? PublishDisposition::eNotPublishedRestored
                    : PublishDisposition::eIndeterminate,
            false};
    }
    if (std::memcmp(observed, expected, size) != 0)
    {
        const bool replacementAlreadyVisible =
            std::memcmp(observed, replacement, size) == 0;
        if (replacementAlreadyVisible)
        {
            // A competing writer, rather than this CAS, published these
            // bytes. Flush them for containment, but never claim ownership:
            // otherwise an unrelated debugger/patcher could be laundered
            // into a certified executable mutation on the next scan.
            flushInstructionCache(GetCurrentProcess(), destination, size);
        }
        const bool restored = RestoreProtectionWithRetry(destination, size,
            finalProtection, protectMemory);
        (void)restored;
        return {PublishDisposition::eIndeterminate,
            replacementAlreadyVisible};
    }
    const bool replacementFlushed = flushInstructionCache(
        GetCurrentProcess(), destination, size) != FALSE;
    if (replacementFlushed
        && RestoreProtectionWithRetry(destination, size, finalProtection,
            protectMemory)
        && BytesEqual(address, replacement, size))
    {
        return {PublishDisposition::ePublishedRestored, true};
    }

    // The replacement was visible but the final protection could not be
    // certified. Re-establish writable access before rollback; a failed
    // VirtualProtect may already have made the page read-only.
    if (!SetAndVerifyProtection(destination, size, temporaryWritableProtection,
            protectMemory))
    {
        const bool restored = RestoreProtectionWithRetry(destination, size,
            finalProtection, protectMemory);
        if (replacementFlushed && restored
            && BytesEqual(address, replacement, size))
            return {PublishDisposition::ePublishedRestored, true};
        return {PublishDisposition::eIndeterminate, true};
    }

    if (BytesEqual(address, replacement, size))
    {
        unsigned char rollbackObserved[8]{};
        if (!AtomicCompareExchangeBytes(address, replacement, expected, size,
                rollbackObserved)
            || std::memcmp(rollbackObserved, replacement, size) != 0)
        {
            RestoreProtectionWithRetry(destination, size, finalProtection,
                protectMemory);
            return {PublishDisposition::eIndeterminate, true};
        }
        if (flushInstructionCache(GetCurrentProcess(), destination, size)
            == FALSE)
        {
            RestoreProtectionWithRetry(destination, size, finalProtection,
                protectMemory);
            return {PublishDisposition::eIndeterminate, true};
        }
    }
    const bool restored = RestoreProtectionWithRetry(destination, size,
        finalProtection, protectMemory);
    if (restored && BytesEqual(address, expected, size))
        return {PublishDisposition::eRolledBackRestored, true};
    return {PublishDisposition::eIndeterminate, true};
}
}
