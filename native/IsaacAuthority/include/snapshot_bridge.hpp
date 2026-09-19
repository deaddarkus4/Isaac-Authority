#pragma once
#include "authority.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace authority::bridge {
// Local test protocol: sender and receiver share the Windows uptime clock.
constexpr std::size_t kBytes = 64;
constexpr std::uint64_t kMaxAgeMs = 250, kSessionMs = 10000;
constexpr std::uint64_t kReplicaSessionMs = 30000;
struct State {
    std::uint64_t session = 0, sentMs = 0;
    std::uint32_t epoch = 1, seed = 0, sequence = 0;
    bool replica = false;
    Body body;
};
std::array<std::uint8_t, kBytes> Encode(const State& state);
bool Decode(const std::uint8_t* bytes, std::size_t size, State& state);
bool Fresh(const State& state, std::uint64_t now);
// Tiny local experiment only: bounded movement around an initially stationary body.
bool CanApply(const State& state, const Body& current, const Body& baseline,
              std::uint64_t now, bool samePlayerAndRoom);
bool CanApplyReplica(const State& state, const Body& current, std::uint64_t now, bool samePlayerAndRoom);
// Holds the last received player state through packet gaps. No client-side player simulation.
// Expired/changed contexts cannot be reactivated until Reset (a new attachment).
class PlayerReplica {
public:
    void Reset() { ready_ = false; invalid_ = false; body_ = {}; }
    bool Adopt(const State& state, const Body& current, std::uint64_t now, bool sameContext);
    bool Hold(std::uint64_t now, std::uint64_t deadline, bool sameContext);
    const Body& Current() const { return body_; }
private:
    Body body_;
    bool ready_ = false, invalid_ = false;
};
class Gate {
public:
    Gate(std::uint64_t session, std::uint32_t seed, bool replica = false) : session_(session), seed_(seed), replica_(replica) {}
    bool Accept(const State& state, std::uint64_t now);
private:
    std::uint64_t session_;
    std::uint32_t seed_, last_ = 0;
    bool replica_;
};
}
