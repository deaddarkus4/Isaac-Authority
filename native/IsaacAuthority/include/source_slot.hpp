#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace authority {
// One game-thread writer, external read-only sampling with generation before/after.
struct alignas(8) SourceSlot {
    std::uint32_t magic = 0x43525349, version = 1;
    volatile LONG generation = 0;
    std::uint32_t alive = 0, sequence = 0, seed = 0, room = 0, player = 0;
    std::uint64_t timeMs = 0;
    float x = 0, y = 0, vx = 0, vy = 0;
    std::uint32_t thread = 0, token = 0;
};
static_assert(sizeof(SourceSlot) == 64 && offsetof(SourceSlot, timeMs) == 32 && offsetof(SourceSlot, x) == 40);
inline void Invalidate(SourceSlot& slot) {
    InterlockedIncrement(&slot.generation); slot.alive = 0; InterlockedIncrement(&slot.generation);
}
}
