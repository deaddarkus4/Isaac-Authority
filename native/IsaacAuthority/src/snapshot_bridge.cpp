#include "snapshot_bridge.hpp"
#include <cmath>
#include <cstring>

namespace authority::bridge {
namespace {
std::uint32_t Get(const std::uint8_t* b, std::size_t at) {
    std::uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i) n |= std::uint32_t(b[at + i]) << (8 * i);
    return n;
}
void Put(std::uint8_t* b, std::size_t at, std::uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(n >> (8 * i));
}
bool Bounded(const Body& b, bool replica = false) {
    return std::isfinite(b.position.x) && std::isfinite(b.position.y) &&
        std::isfinite(b.velocity.x) && std::isfinite(b.velocity.y) &&
        b.position.x >= (replica ? 64 : 100) && b.position.x <= (replica ? 576 : 500) &&
        b.position.y >= (replica ? 96 : 100) && b.position.y <= 400 &&
        std::abs(b.velocity.x) < (replica ? 20.0f : 0.01f) && std::abs(b.velocity.y) < (replica ? 20.0f : 0.01f);
}
}
std::array<std::uint8_t, kBytes> Encode(const State& s) {
    std::array<std::uint8_t, kBytes> bytes{}; auto b = bytes.data();
    Put(b, 0, 0x50414e53); Put(b, 4, s.replica ? 0x00020001 : 0x00010001); // SNAP v1, stationary/replica
    Put(b, 8, static_cast<std::uint32_t>(s.session)); Put(b, 12, static_cast<std::uint32_t>(s.session >> 32));
    Put(b, 16, s.epoch); Put(b, 20, s.seed); Put(b, 24, s.sequence);
    Put(b, 32, static_cast<std::uint32_t>(s.sentMs)); Put(b, 36, static_cast<std::uint32_t>(s.sentMs >> 32));
    const float values[]{s.body.position.x, s.body.position.y, s.body.velocity.x, s.body.velocity.y};
    for (unsigned i = 0; i < 4; ++i) {
        std::uint32_t bits; std::memcpy(&bits, &values[i], 4); Put(b, 40 + i * 4, bits);
    }
    return bytes;
}
bool Decode(const std::uint8_t* b, std::size_t size, State& out) {
    if (!b || size != kBytes || Get(b, 0) != 0x50414e53 || (Get(b, 4) != 0x00010001 && Get(b, 4) != 0x00020001) ||
        Get(b, 28) || Get(b, 56) || Get(b, 60)) return false;
    State s;
    s.replica = Get(b, 4) == 0x00020001;
    s.session = Get(b, 8) | (std::uint64_t(Get(b, 12)) << 32);
    s.epoch = Get(b, 16); s.seed = Get(b, 20); s.sequence = Get(b, 24);
    s.sentMs = Get(b, 32) | (std::uint64_t(Get(b, 36)) << 32);
    float values[4]{};
    for (unsigned i = 0; i < 4; ++i) { const auto bits = Get(b, 40 + i * 4); std::memcpy(&values[i], &bits, 4); }
    s.body = {{values[0], values[1]}, {values[2], values[3]}};
    if (!s.session || s.epoch != 1 || !s.sequence || !Bounded(s.body, s.replica)) return false;
    out = s; return true;
}
bool Fresh(const State& s, std::uint64_t now) {
    return s.sentMs <= now && now - s.sentMs <= kMaxAgeMs;
}
bool Gate::Accept(const State& s, std::uint64_t now) {
    if (s.session != session_ || s.seed != seed_ || s.epoch != 1 || s.sequence <= last_ || s.replica != replica_ ||
        !Bounded(s.body, replica_) || !Fresh(s, now)) return false;
    last_ = s.sequence; return true;
}
bool CanApply(const State& s, const Body& current, const Body& baseline,
              std::uint64_t now, bool samePlayerAndRoom) {
    return !s.replica && samePlayerAndRoom && Fresh(s, now) && Bounded(s.body) && Bounded(current) && Bounded(baseline) &&
        std::abs(s.body.position.x - current.position.x) <= 16 &&
        std::abs(s.body.position.y - current.position.y) <= 16 &&
        std::abs(s.body.position.x - baseline.position.x) <= 16 &&
        std::abs(s.body.position.y - baseline.position.y) <= 16;
}
bool CanApplyReplica(const State& s, const Body& current, std::uint64_t now, bool sameContext) {
    return s.replica && sameContext && Fresh(s, now) && Bounded(s.body, true) && Bounded(current, true) &&
        std::abs(s.body.position.x - current.position.x) <= 128 &&
        std::abs(s.body.position.y - current.position.y) <= 128;
}
bool PlayerReplica::Adopt(const State& s, const Body& current, std::uint64_t now, bool sameContext) {
    if (invalid_ || !CanApplyReplica(s, current, now, sameContext)) return false;
    body_ = s.body; ready_ = true; return true;
}
bool PlayerReplica::Hold(std::uint64_t now, std::uint64_t deadline, bool sameContext) {
    if (!sameContext || now > deadline) { invalid_ = true; ready_ = false; }
    return ready_ && !invalid_;
}
}
