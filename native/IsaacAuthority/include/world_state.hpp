#pragma once
#include "authority.hpp"
#include <array>
#include <cstdint>

namespace authority::world {
constexpr std::size_t kMaxEntities = 16, kMaxNpcs = 32, kMaxPlayers = 4, kHeader = 96, kRecord = 56, kNpcRecord = 64,
    kPlayerRecord = 24, kMaxBytes = kHeader + kMaxEntities * kRecord + kMaxNpcs * kNpcRecord + kMaxPlayers * kPlayerRecord;
constexpr std::uint64_t kMaxAgeMs = 250, kDurationMs = 30000;
struct RoomKey {
    std::uint32_t stage = 0, stageType = 0, index = 0, dimension = 0;
    std::uint32_t type = 0, variant = 0, shape = 0, spawnSeed = 0, visits = 0;
};
bool SameRoom(const RoomKey& a, const RoomKey& b);
// Same generated room in two games; visit counters are local history and may differ.
bool SamePlace(const RoomKey& a, const RoomKey& b);
// Door direction (0 left, 1 up, 2 right, 3 down) between adjacent 1x1 cells of the 13x13 grid, otherwise -1.
int Direction(const RoomKey& from, const RoomKey& to);
struct Entity {
    std::uint32_t id = 0, seed = 0, type = 2, variant = 0, subtype = 0;
    Body body;
    float height = 0, fallingSpeed = 0, fallingAccel = 0, scale = 1;
};
// An enemy the room itself spawned. Both games create it with the same InitSeed, so the replica corrects its own
// entity instead of spawning a copy; (type, variant, subtype, seed) is the identity and needs no network id.
struct Npc {
    std::uint32_t type = 0, variant = 0, subtype = 0, seed = 0;
    Body body;
    Vec target;
    float hitPoints = 0, maxHitPoints = 0;
    std::int32_t state = 0;
    std::uint32_t visible = 0, gridCollision = 0, entityCollision = 0;
};
bool SameNpc(const Npc& a, const Npc& b);
// A player of the host's game. Players join in the same order in both games, so the place in the list is the
// identity; the controller index says whose input drives this player on the host (two may share one controller).
struct Player { std::uint32_t controller = 0; Body body; };
struct Frame;
// Death is the host's decision: true when its previous snapshot of this room generation listed the enemy and the
// next one no longer does. An enemy the host never listed is not the replica's to kill.
bool Departed(const Frame* previous, const Frame& next, const Npc& npc);
struct Frame {
    std::uint64_t session = 0, timeMs = 0;
    std::uint32_t epoch = 1, sequence = 0;
    RoomKey room;
    std::uint32_t playerCount = 1;
    std::array<Player, kMaxPlayers> players{};
    std::uint32_t count = 0;
    std::array<Entity, kMaxEntities> entities{};
    std::uint32_t npcCount = 0;
    std::array<Npc, kMaxNpcs> npcs{};
};
struct Packet { std::array<std::uint8_t, kMaxBytes> bytes{}; std::size_t size = 0; };
bool Valid(const Frame& frame);
bool Fresh(const Frame& frame, std::uint64_t now);
bool Encode(const Frame& frame, Packet& packet);
bool Decode(const std::uint8_t* bytes, std::size_t size, Frame& frame);
enum class Decision { Reject, Accept, RoomChanged };
class Gate {
public:
    explicit Gate(std::uint64_t session) : session_(session) {}
    Decision Receive(const Frame& frame, std::uint64_t now);
private:
    std::uint64_t session_;
    std::uint32_t epoch_ = 0, sequence_ = 0;
    RoomKey room_;
};
struct Identity { std::uint32_t address = 0, seed = 0, index = 0; };
class Ids {
public:
    void Reset() { entries_ = {}; next_ = 1; }
    bool Assign(const Identity* identities, std::size_t count, std::uint32_t* output);
private:
    struct Entry { Identity key; std::uint32_t id = 0; };
    std::array<Entry, kMaxEntities> entries_{};
    std::uint32_t next_ = 1;
};
struct Delta {
    std::array<std::uint32_t, kMaxEntities> create{}, remove{}, update{};
    std::size_t creates = 0, removes = 0, updates = 0;
};
// Full snapshots: missing entities are removed, never inferred from packet loss alone.
bool Plan(const Frame& previous, const Frame& next, Delta& delta);
// Apply continues the roster; Restart drops copies of an older host generation of this room first;
// Travel means the host is elsewhere and the replica must load that room before applying anything.
enum class Route { Apply, Restart, Travel };
Route Decide(const RoomKey& local, const Frame* applied, const Frame& next);
}
