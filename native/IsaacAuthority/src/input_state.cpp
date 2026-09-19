#include "input_state.hpp"
#include <cmath>
#include <cstring>

namespace authority::input {
namespace {
void Put(std::uint8_t* b, std::size_t at, std::uint32_t n) { for (unsigned i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(n >> (8 * i)); }
std::uint32_t Get(const std::uint8_t* b, std::size_t at) { std::uint32_t n = 0; for (unsigned i = 0; i < 4; ++i) n |= std::uint32_t(b[at + i]) << (8 * i); return n; }
void PutFloat(std::uint8_t* b, std::size_t at, float f) { std::uint32_t bits; std::memcpy(&bits, &f, 4); Put(b, at, bits); }
float GetFloat(const std::uint8_t* b, std::size_t at) { const auto bits = Get(b, at); float f; std::memcpy(&f, &bits, 4); return f; }
bool Axis(float v) { return std::isfinite(v) && v >= -1 && v <= 1; }
float Positive(float v) { return v > 0 ? v : 0; }
}
bool Valid(const Command& c) {
    return c.session && c.sequence && c.controller >= 0 && c.controller <= kMaxController &&
        Axis(c.moveX) && Axis(c.moveY) && Axis(c.shootX) && Axis(c.shootY) && c.buttons < (1u << (kActions - Bomb));
}
bool Fresh(const Command& c, std::uint64_t now) { return c.timeMs <= now && now - c.timeMs <= kMaxAgeMs; }
bool Encode(const Command& c, std::uint8_t* b) {
    if (!Valid(c) || !b) return false;
    std::memset(b, 0, kBytes);
    Put(b, 0, 0x31504e49); Put(b, 4, 1); // "INP1", version
    Put(b, 8, static_cast<std::uint32_t>(c.session)); Put(b, 12, static_cast<std::uint32_t>(c.session >> 32));
    Put(b, 16, c.sequence); Put(b, 20, static_cast<std::uint32_t>(c.controller));
    Put(b, 24, static_cast<std::uint32_t>(c.timeMs)); Put(b, 28, static_cast<std::uint32_t>(c.timeMs >> 32));
    PutFloat(b, 32, c.moveX); PutFloat(b, 36, c.moveY); PutFloat(b, 40, c.shootX); PutFloat(b, 44, c.shootY);
    Put(b, 48, c.buttons);
    return true;
}
bool Decode(const std::uint8_t* b, std::size_t size, Command& out) {
    if (!b || size != kBytes || Get(b, 0) != 0x31504e49 || Get(b, 4) != 1 || Get(b, 52)) return false;
    Command c;
    c.session = Get(b, 8) | (std::uint64_t(Get(b, 12)) << 32); c.sequence = Get(b, 16);
    c.controller = static_cast<std::int32_t>(Get(b, 20)); c.timeMs = Get(b, 24) | (std::uint64_t(Get(b, 28)) << 32);
    c.moveX = GetFloat(b, 32); c.moveY = GetFloat(b, 36); c.shootX = GetFloat(b, 40); c.shootY = GetFloat(b, 44);
    c.buttons = Get(b, 48);
    if (!Valid(c)) return false;
    out = c; return true;
}
float Value(const Command& c, int action) {
    switch (action) {
    case Left: return Positive(-c.moveX);
    case Right: return Positive(c.moveX);
    case Up: return Positive(-c.moveY);
    case Down: return Positive(c.moveY);
    case ShootLeft: return Positive(-c.shootX);
    case ShootRight: return Positive(c.shootX);
    case ShootUp: return Positive(-c.shootY);
    case ShootDown: return Positive(c.shootY);
    default: return action >= Bomb && action < kActions && (c.buttons >> (action - Bomb) & 1u) ? 1.0f : 0.0f;
    }
}
bool Pressed(const Command& c, int action) { return Value(c, action) > 0.5f; }
bool Gate::Receive(const Command& c, std::uint64_t now) {
    if (!Valid(c) || !Fresh(c, now) || c.session != session_ || c.sequence <= sequence_) return false;
    sequence_ = c.sequence; return true;
}
}
