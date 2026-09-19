#pragma once
#include <winsock2.h>
#include <windows.h>
#include "world_state.hpp"
#include <atomic>

namespace authority::world {
class Receiver {
public:
    DWORD Start();
    void Stop();
    bool Take(Frame& frame);
    std::uint64_t Session() const { return session_; }
    std::uint64_t Deadline() const { return deadline_; }
    unsigned short Port() const { return port_; }
    std::atomic<unsigned> accepted{0}, rejected{0}, replaced{0}, errors{0}, roomChanges{0};
private:
    static DWORD WINAPI Run(void* self);
    SOCKET socket_ = INVALID_SOCKET;
    HANDLE worker_ = nullptr;
    bool winsock_ = false, pending_ = false;
    std::atomic<bool> running_{false};
    SRWLOCK lock_ = SRWLOCK_INIT;
    Frame latest_;
    std::uint64_t session_ = 0, deadline_ = 0;
    unsigned short port_ = 0;
};
struct alignas(8) Published {
    std::uint32_t magic = 0x31525357, version = 1;
    volatile LONG generation = 0;
    std::uint32_t alive = 0, bytes = 0, reserved = 0;
    std::uint64_t session = 0;
    std::array<std::uint8_t, kMaxBytes> packet{};
};
// The endpoint descriptor states this size: modules built before the player section publish 3072 bytes.
static_assert(sizeof(Published) == 3168 && offsetof(Published, packet) == 32);
}
