#include "world_state.hpp"
#include <cmath>
#include <cstring>
#include <limits>

namespace authority::world {
namespace {
void Put(std::uint8_t* b, std::size_t at, std::uint32_t n) { for (unsigned i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(n >> (8 * i)); }
std::uint32_t Get(const std::uint8_t* b, std::size_t at) { std::uint32_t n = 0; for (unsigned i = 0; i < 4; ++i) n |= std::uint32_t(b[at + i]) << (8 * i); return n; }
void PutFloat(std::uint8_t* b, std::size_t at, float f) { std::uint32_t bits; std::memcpy(&bits, &f, 4); Put(b, at, bits); }
float GetFloat(const std::uint8_t* b, std::size_t at) { const auto bits = Get(b, at); float f; std::memcpy(&f, &bits, 4); return f; }
bool Number(float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; }
bool BodyValid(const Body& b) {
    return Number(b.position.x, -128, 2048) && Number(b.position.y, -128, 2048) &&
        Number(b.velocity.x, -40, 40) && Number(b.velocity.y, -40, 40);
}
bool SameIdentity(const Identity& a, const Identity& b) { return a.address == b.address && a.seed == b.seed && a.index == b.index; }
bool Has(const Frame& f, std::uint32_t id) {
    for (std::size_t i = 0; i < f.count; ++i) if (f.entities[i].id == id) return true;
    return false;
}
}
bool SameRoom(const RoomKey& a, const RoomKey& b) {
    return a.stage == b.stage && a.stageType == b.stageType && a.index == b.index && a.dimension == b.dimension &&
        a.type == b.type && a.variant == b.variant && a.shape == b.shape && a.spawnSeed == b.spawnSeed && a.visits == b.visits;
}
bool SamePlace(const RoomKey& a, const RoomKey& b) {
    return a.stage == b.stage && a.stageType == b.stageType && a.index == b.index && a.dimension == b.dimension &&
        a.type == b.type && a.variant == b.variant && a.shape == b.shape && a.spawnSeed == b.spawnSeed;
}
int Direction(const RoomKey& from, const RoomKey& to) {
    if (from.dimension != to.dimension || from.shape != 1 || to.shape != 1 || from.index >= 169 || to.index >= 169) return -1;
    const auto column = from.index % 13;
    if (to.index == from.index + 1 && column != 12) return 2;
    if (to.index + 1 == from.index && column != 0) return 0;
    if (to.index == from.index + 13) return 3;
    if (to.index + 13 == from.index) return 1;
    return -1;
}
Route Decide(const RoomKey& local, const Frame* applied, const Frame& next) {
    if (!SamePlace(next.room, local)) return Route::Travel;
    if (applied && (applied->epoch != next.epoch || !SameRoom(applied->room, next.room))) return Route::Restart;
    return Route::Apply;
}
bool Valid(const Frame& f) {
    if (!f.session || !f.epoch || !f.sequence || f.count > kMaxEntities || !BodyValid(f.player) ||
        f.room.stage > 20 || f.room.stageType > 10 || f.room.dimension > 2 || f.room.type > 40 ||
        f.room.shape > 12 || !f.room.shape) return false;
    for (std::size_t i = 0; i < f.count; ++i) {
        const auto& e = f.entities[i];
        if (!e.id || !e.seed || e.type != 2 || e.variant != 0 || e.subtype != 0 || !BodyValid(e.body) ||
            !Number(e.height, -500, 100) || !Number(e.fallingSpeed, -100, 100) ||
            !Number(e.fallingAccel, -10, 10) || !Number(e.scale, 0.01f, 10)) return false;
        for (std::size_t j = 0; j < i; ++j) if (f.entities[j].id == e.id) return false;
    }
    return true;
}
bool Fresh(const Frame& f, std::uint64_t now) { return f.timeMs <= now && now - f.timeMs <= kMaxAgeMs; }
bool Encode(const Frame& f, Packet& p) {
    if (!Valid(f)) return false;
    p = {}; p.size = kHeader + f.count * kRecord; auto b = p.bytes.data();
    Put(b, 0, 0x31444c57); Put(b, 4, 1);
    Put(b, 8, static_cast<std::uint32_t>(f.session)); Put(b, 12, static_cast<std::uint32_t>(f.session >> 32));
    Put(b, 16, f.epoch); Put(b, 20, f.sequence);
    Put(b, 24, static_cast<std::uint32_t>(f.timeMs)); Put(b, 28, static_cast<std::uint32_t>(f.timeMs >> 32));
    const std::uint32_t room[]{f.room.stage, f.room.stageType, f.room.index, f.room.dimension, f.room.type,
        f.room.variant, f.room.shape, f.room.spawnSeed, f.room.visits};
    for (unsigned i = 0; i < 9; ++i) Put(b, 32 + i * 4, room[i]);
    const float player[]{f.player.position.x, f.player.position.y, f.player.velocity.x, f.player.velocity.y};
    for (unsigned i = 0; i < 4; ++i) PutFloat(b, 68 + i * 4, player[i]);
    Put(b, 84, f.count);
    for (std::size_t i = 0; i < f.count; ++i) {
        const auto at = kHeader + i * kRecord; const auto& e = f.entities[i];
        Put(b, at, e.id); Put(b, at + 4, e.seed); Put(b, at + 8, e.type); Put(b, at + 12, e.variant); Put(b, at + 16, e.subtype);
        const float values[]{e.body.position.x, e.body.position.y, e.body.velocity.x, e.body.velocity.y,
            e.height, e.fallingSpeed, e.fallingAccel, e.scale};
        for (unsigned j = 0; j < 8; ++j) PutFloat(b, at + 20 + j * 4, values[j]);
    }
    return true;
}
bool Decode(const std::uint8_t* b, std::size_t size, Frame& out) {
    if (!b || size < kHeader || Get(b, 0) != 0x31444c57 || Get(b, 4) != 1 || Get(b, 88) || Get(b, 92)) return false;
    Frame f; f.session = Get(b, 8) | (std::uint64_t(Get(b, 12)) << 32); f.epoch = Get(b, 16); f.sequence = Get(b, 20);
    f.timeMs = Get(b, 24) | (std::uint64_t(Get(b, 28)) << 32);
    f.room = {Get(b, 32), Get(b, 36), Get(b, 40), Get(b, 44), Get(b, 48), Get(b, 52), Get(b, 56), Get(b, 60), Get(b, 64)};
    f.player = {{GetFloat(b, 68), GetFloat(b, 72)}, {GetFloat(b, 76), GetFloat(b, 80)}};
    f.count = Get(b, 84);
    if (f.count > kMaxEntities || size != kHeader + f.count * kRecord) return false;
    for (std::size_t i = 0; i < f.count; ++i) {
        const auto at = kHeader + i * kRecord;
        if (Get(b, at + 52)) return false;
        f.entities[i] = {Get(b, at), Get(b, at + 4), Get(b, at + 8), Get(b, at + 12), Get(b, at + 16),
            {{GetFloat(b, at + 20), GetFloat(b, at + 24)}, {GetFloat(b, at + 28), GetFloat(b, at + 32)}},
            GetFloat(b, at + 36), GetFloat(b, at + 40), GetFloat(b, at + 44), GetFloat(b, at + 48)};
    }
    if (!Valid(f)) return false;
    out = f; return true;
}
Decision Gate::Receive(const Frame& f, std::uint64_t now) {
    if (!Valid(f) || !Fresh(f, now) || f.session != session_ || f.epoch < epoch_ ||
        (f.epoch == epoch_ && (f.sequence <= sequence_ || !SameRoom(f.room, room_)))) return Decision::Reject;
    const bool changed = epoch_ && f.epoch > epoch_;
    epoch_ = f.epoch; sequence_ = f.sequence; room_ = f.room;
    return changed ? Decision::RoomChanged : Decision::Accept;
}
bool Ids::Assign(const Identity* keys, std::size_t count, std::uint32_t* output) {
    if (count > kMaxEntities || (count && (!keys || !output))) return false;
    for (std::size_t i = 0; i < count; ++i) {
        if (!keys[i].address || !keys[i].seed) return false;
        for (std::size_t j = 0; j < i; ++j) if (SameIdentity(keys[i], keys[j])) return false;
    }
    auto nextEntries = entries_; auto nextId = next_;
    for (auto& entry : nextEntries) {
        bool present = false;
        for (std::size_t i = 0; i < count; ++i) if (SameIdentity(entry.key, keys[i])) present = true;
        if (!present) entry = {};
    }
    for (std::size_t i = 0; i < count; ++i) {
        std::uint32_t id = 0;
        for (const auto& entry : nextEntries) if (entry.id && SameIdentity(entry.key, keys[i])) id = entry.id;
        if (!id) {
            if (!nextId || nextId == (std::numeric_limits<std::uint32_t>::max)()) return false;
            for (auto& entry : nextEntries) if (!entry.id) { entry = {keys[i], nextId++}; id = entry.id; break; }
        }
        if (!id) return false;
        output[i] = id;
    }
    entries_ = nextEntries; next_ = nextId; return true;
}
bool Plan(const Frame& before, const Frame& after, Delta& out) {
    if (before.count > kMaxEntities || !Valid(after) || (before.sequence &&
        (before.session != after.session || before.epoch != after.epoch || !SameRoom(before.room, after.room)))) return false;
    Delta d;
    for (std::size_t i = 0; i < before.count; ++i) if (!Has(after, before.entities[i].id)) d.remove[d.removes++] = before.entities[i].id;
    for (std::size_t i = 0; i < after.count; ++i) {
        const auto id = after.entities[i].id;
        for (std::size_t j = 0; j < before.count; ++j) if (before.entities[j].id == id &&
            (before.entities[j].seed != after.entities[i].seed || before.entities[j].type != after.entities[i].type ||
             before.entities[j].variant != after.entities[i].variant || before.entities[j].subtype != after.entities[i].subtype)) return false;
        if (Has(before, id)) d.update[d.updates++] = id; else d.create[d.creates++] = id;
    }
    out = d; return true;
}
}
