#include "wire.hpp"
#include <cstring>
#include <stdexcept>

namespace authority {
namespace {
void Put(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(n >> (8 * i));
}
std::uint32_t Get(const std::uint8_t* b, std::size_t at) {
    std::uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i) n |= std::uint32_t(b[at + i]) << (8 * i);
    return n;
}
void PutFloat(std::vector<std::uint8_t>& b, std::size_t at, float v) {
    static_assert(sizeof(float) == 4);
    std::uint32_t bits; std::memcpy(&bits, &v, 4); Put(b, at, bits);
}
float GetFloat(const std::uint8_t* b, std::size_t at) {
    const auto bits = Get(b, at); float v; std::memcpy(&v, &bits, 4); return v;
}
}
std::vector<std::uint8_t> Encode(const Packet& p) {
    std::vector<std::uint8_t> b(kPacketBytes, 0);
    Put(b, 0, 0x48545541); // AUTH, little endian. No packed C++ structs on wire.
    Put(b, 4, 1u | (std::uint32_t(p.kind) << 16));
    Put(b, 8, static_cast<std::uint32_t>(p.session));
    Put(b, 12, static_cast<std::uint32_t>(p.session >> 32));
    Put(b, 16, p.epoch);
    if (p.kind == Kind::Input) {
        Put(b, 20, 1); // Only remote player slot 1 is client-owned.
        Put(b, 24, p.input.sequence);
        PutFloat(b, 40, p.input.movement.x); PutFloat(b, 44, p.input.movement.y);
    } else if (p.kind == Kind::Snapshot) {
        Put(b, 20, 2); Put(b, 28, p.snapshot.tick); Put(b, 32, p.snapshot.acknowledged);
        for (std::size_t i = 0; i < 2; ++i) {
            const auto& v = p.snapshot.players[i]; const auto at = 40 + i * 16;
            PutFloat(b, at, v.position.x); PutFloat(b, at + 4, v.position.y);
            PutFloat(b, at + 8, v.velocity.x); PutFloat(b, at + 12, v.velocity.y);
        }
    } else if (p.kind != Kind::Hello) throw std::invalid_argument("unknown packet kind");
    Packet checked;
    if (!Decode(b.data(), b.size(), p.session, checked)) throw std::invalid_argument("invalid packet");
    return b;
}
bool Decode(const std::uint8_t* b, std::size_t size, std::uint64_t session, Packet& out) {
    if (!b || size != kPacketBytes || Get(b, 0) != 0x48545541 ||
        (Get(b, 4) & 0xffff) != 1 || !session ||
        (std::uint64_t(Get(b, 8)) | (std::uint64_t(Get(b, 12)) << 32)) != session ||
        !Get(b, 16) || Get(b, 36)) return false;
    Packet p; p.session = session; p.epoch = Get(b, 16);
    p.kind = static_cast<Kind>(Get(b, 4) >> 16);
    if (p.kind == Kind::Input) {
        if (Get(b, 20) != 1 || !Get(b, 24) || Get(b, 28) || Get(b, 32)) return false;
        for (std::size_t i = 48; i < size; ++i) if (b[i]) return false;
        p.input = {Get(b, 24), {GetFloat(b, 40), GetFloat(b, 44)}};
        if (!ValidMovement(p.input.movement)) return false;
    } else if (p.kind == Kind::Snapshot) {
        if (Get(b, 20) != 2 || Get(b, 24)) return false;
        p.snapshot.epoch = p.epoch; p.snapshot.tick = Get(b, 28);
        p.snapshot.acknowledged = Get(b, 32);
        for (std::size_t i = 0; i < 2; ++i) {
            const auto at = 40 + i * 16;
            p.snapshot.players[i] = {{GetFloat(b, at), GetFloat(b, at + 4)},
                {GetFloat(b, at + 8), GetFloat(b, at + 12)}};
            if (!ValidBody(p.snapshot.players[i])) return false;
        }
    } else if (p.kind == Kind::Hello) {
        for (std::size_t i = 20; i < size; ++i) if (b[i]) return false;
    } else return false;
    out = p;
    return true;
}
}
