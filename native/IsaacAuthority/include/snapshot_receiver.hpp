#pragma once
#include <winsock2.h>
#include <windows.h>
#include "snapshot_bridge.hpp"
#include <atomic>

namespace authority::bridge {
struct Counters { unsigned accepted = 0, rejected = 0, replaced = 0, errors = 0; };
class Receiver {
public:
    ~Receiver() { Stop(); }
    DWORD Start(std::uint32_t seed, bool replica = false);
    void Stop();
    // Game thread never waits for the network worker. Mailbox has one slot.
    bool Take(State& state);
    Counters Stats() const { return {accepted_.load(), rejected_.load(), replaced_.load(), errors_.load()}; }
    std::uint64_t Session() const { return session_; }
    std::uint64_t Deadline() const { return deadline_; }
    unsigned short Port() const { return port_; }
private:
    static DWORD WINAPI Run(void* self);
    SOCKET socket_ = INVALID_SOCKET;
    HANDLE worker_ = nullptr;
    bool winsock_ = false, pending_ = false;
    bool replica_ = false;
    std::atomic<bool> running_{false};
    SRWLOCK lock_ = SRWLOCK_INIT;
    State latest_;
    std::uint64_t session_ = 0, deadline_ = 0;
    std::uint32_t seed_ = 0;
    unsigned short port_ = 0;
    std::atomic<unsigned> accepted_{0}, rejected_{0}, replaced_{0}, errors_{0};
};
}
