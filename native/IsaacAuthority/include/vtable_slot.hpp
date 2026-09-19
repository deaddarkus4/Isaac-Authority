#pragma once
#include <cstdint>
#include <windows.h>

namespace authority {
// Atomically changes one aligned data pointer, without relocating executable code.
// A failed comparison leaves the current owner's hook untouched.
inline DWORD ExchangeSlot(void** slot, void* expected, void* replacement) {
    if (!slot || reinterpret_cast<std::uintptr_t>(slot) % sizeof(void*)) return ERROR_INVALID_PARAMETER;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return GetLastError();
    const auto actual = InterlockedCompareExchangePointer(slot, replacement, expected);
    DWORD ignored = 0;
    const BOOL restored = VirtualProtect(slot, sizeof(void*), old, &ignored);
    if (!restored) return GetLastError();
    return actual == expected ? ERROR_SUCCESS : ERROR_REVISION_MISMATCH;
}
}
