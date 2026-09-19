#include "world_receiver.hpp"
#include "profile.hpp"
#include "vtable_slot.hpp"
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <sstream>

namespace {
using namespace authority;
using namespace authority::world;
#ifdef ISAAC_WORLD_SOURCE
constexpr bool kSource = true;
#else
constexpr bool kSource = false;
#endif
#ifdef ISAAC_WORLD_ROOMS
constexpr bool kRooms = true;
#else
constexpr bool kRooms = false;
#endif
#ifdef ISAAC_WORLD_NPCS
constexpr bool kNpcs = true;
#else
constexpr bool kNpcs = false;
#endif
#ifdef ISAAC_WORLD_PLAYERS
constexpr bool kPlayers = true;
#else
constexpr bool kPlayers = false;
#endif
constexpr std::uintptr_t kPlayerTable = 0x76bdd0, kPlayerUpdate = 0x382af0;
// Entity_Player::ControllerIndex: whose input drives this player.
constexpr std::uintptr_t kController = 0x1618;
// Entity_NPC shares the entity vtable layout: slot 3 is Update.
constexpr std::uintptr_t kNpcTable = 0x767468, kNpcUpdate = 0x2c4b30;
// What Lua's entity:Kill() runs: builds an empty damage source and calls the virtual Kill (slot 9).
constexpr std::uintptr_t kKill = 0x45dc30;
constexpr std::uintptr_t kTearTable = 0x764eac, kTearUpdate = 0x2670f0, kRemove = 0x2acb00, kSpawn = 0x28b20;
// Game::StartRoomTransition only latches a request in RoomTransition (Game+0x1b83c); the game loads the room later.
constexpr std::uintptr_t kTransition = 0x2fd7c0, kTransitionUnwind = 0x6fc200, kRoomTransition = 0x1b83c;
constexpr std::uint64_t kTravelMs = 5000, kSettleMs = 1500;
// Updates, not steps: every player of a co-op run spends the grace of an unreadable room.
constexpr unsigned kLocalGrace = kPlayers ? 90 * kMaxPlayers : 90, kFade = 1;
using Update = void(__thiscall*)(void*);
using Spawn = void*(__thiscall*)(void*, unsigned, unsigned, const Vec&, const Vec&, void*, unsigned, unsigned);
using Transition = void(__thiscall*)(void*, int, int, unsigned, void*, int);
// Every player of the run in list order; target is the first of them.
struct Team { std::array<std::uintptr_t, kMaxPlayers> players{}; unsigned count = 0; };
std::uintptr_t base = 0, target = 0;
Team team;
std::uint32_t targetSeed = 0;
RoomKey localRoom;
Update originalPlayer = nullptr, originalTear = nullptr, originalNpc = nullptr;
void** playerSlot = nullptr;
void** tearSlot = nullptr;
void** npcSlot = nullptr;
bool playerInstalled = false, tearInstalled = false, npcInstalled = false;
std::atomic<bool> running{false}, stopRequested{false}, cleaned{false};
std::atomic<unsigned> active{0};
std::atomic<DWORD> owner{0};
SRWLOCK lifecycle = SRWLOCK_INIT;
HANDLE report = INVALID_HANDLE_VALUE;
Receiver receiver;
Published published;
Ids ids;
Frame lastFrame;
std::uint64_t session = 0, deadline = 0;
std::uint32_t sourceSequence = 0, sourceEpoch = 1;
bool haveFrame = false;
unsigned created = 0, removed = 0, faults = 0, contextChanges = 0, tearHolds = 0, playerHolds = 0, frameCount = 0, dropped = 0;
bool traveling = false;
Frame travelFrame;
std::uint64_t travelDeadline = 0;
unsigned transitions = 0, arrivals = 0, levelMismatches = 0, localFailures = 0, selfMoves = 0, vanished = 0, settled = 0;
std::uint32_t departedEpoch = 0;
std::uint64_t relocatedAt = 0;
// Enemies of the replica's own room that the host also has; corrected after each of their own updates.
struct Adopted { std::uintptr_t address = 0; Npc state; };
std::array<Adopted, kMaxNpcs> adopted{};
unsigned npcKilled = 0, npcKillUnconfirmed = 0, npcCorrections = 0, npcUpdates = 0, npcStateDrift = 0, npcVisibleDrift = 0, npcUnmatched = 0, npcOrphans = 0, lastFault = 0;
float npcDriftMax = 0;
double npcDriftSum = 0;
std::array<std::uint32_t, kMaxPlayers> playerCollision{};
bool playerShielded = false;
// Which player the game updated last, and how often the list order was not the update order.
int lastUpdated = -1;
unsigned orderSurprises = 0, rosterMismatches = 0;
struct Tracked { std::uintptr_t address = 0; std::uint32_t localIndex = 0; Entity state; };
std::array<Tracked, kMaxEntities> tracked{};
struct Record { std::uint64_t now = 0; DWORD thread = 0; unsigned created = 0, removed = 0, action = 0; Frame frame; };
std::array<Record, 2048> records;
struct Guard { Guard() { AcquireSRWLockExclusive(&lifecycle); } ~Guard() { ReleaseSRWLockExclusive(&lifecycle); } };
template<class T> bool Read(std::uintptr_t address, T& value) {
    SIZE_T got = 0;
    return address >= 0x10000 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), &value, sizeof(value), &got) && got == sizeof(value);
}
bool ReadBody(std::uintptr_t entity, Body& body) {
    return Read(entity + 0x33c, body.position) && Read(entity + 0x360, body.velocity) &&
        std::isfinite(body.position.x) && std::isfinite(body.position.y) && std::isfinite(body.velocity.x) && std::isfinite(body.velocity.y);
}
bool Describe(std::uintptr_t game, std::uint32_t index, std::uint32_t dimension, RoomKey& key) {
    std::uintptr_t data = 0; std::uint32_t offset = 0, descIndex = 0, descDimension = 0, anchor = 0;
    if (index >= 169 || dimension > 2 || !Read(game, key.stage) || !Read(game + 4, key.stageType) ||
        !Read(game + 0x17adc + (dimension * 169 + index) * 4, offset) || offset >= 527) return false;
    const auto desc = game + 0x14 + offset * 0xb8;
    if (!Read(desc, descIndex) || !Read(desc + 0xc, descDimension) || descDimension != dimension) return false;
    // Every cell of a large room maps to one descriptor anchored at its first cell.
    if (descIndex != index && (!kRooms || descIndex >= 169 ||
        !Read(game + 0x17adc + (dimension * 169 + descIndex) * 4, anchor) || anchor != offset)) return false;
    key.index = index; key.dimension = dimension;
    return Read(desc + 0x10, data) && Read(data + 8, key.type) && Read(data + 0xc, key.variant) && Read(data + 0x48, key.shape) &&
        Read(desc + 0x5c, key.spawnSeed) && Read(desc + 0x40, key.visits);
}
bool Local(std::uintptr_t& game, std::uintptr_t& room, Team& found, RoomKey& key) {
    std::uintptr_t manager = 0, begin = 0, end = 0, netBegin = 0, netEnd = 0;
    std::uint32_t index = 0, dimension = 0;
    if (!Read(base + 0x871678, game) || !Read(base + 0x87169c, manager) ||
        !Read(manager + 0x4b3d8, netBegin) || !Read(manager + 0x4b3dc, netEnd) || netBegin != netEnd ||
        !Read(game + 0x1baa8, begin) || !Read(game + 0x1baac, end) || end <= begin || (end - begin) % 4) return false;
    // Only the modules built for several players accept a co-op run.
    found = {}; found.count = static_cast<unsigned>((end - begin) / 4);
    if (found.count > (kPlayers ? kMaxPlayers : 1)) return false;
    for (unsigned i = 0; i < found.count; ++i) if (!Read(begin + i * 4, found.players[i])) return false;
    return Read(game + 0x18300, room) && room >= 0x10000 && Read(game + 0x18304, index) && Read(game + 0x1830c, dimension) &&
        Describe(game, index, dimension, key);
}
bool SameTeam(const Team& a, const Team& b) { return a.count == b.count && a.players == b.players; }
// Bodies of the whole team in list order. The source also says which controller drives each player.
bool ReadPlayers(Frame& frame, bool controllers) {
    frame.playerCount = team.count;
    for (unsigned i = 0; i < team.count; ++i) {
        std::int32_t controller = 0;
        if (!ReadBody(team.players[i], frame.players[i].body) || (controllers && !Read(team.players[i] + kController, controller))) return false;
        if (controllers) frame.players[i].controller = static_cast<std::uint32_t>(controller);
    }
    return true;
}
bool List(std::uintptr_t room, std::array<std::uintptr_t, 4096>& pointers, unsigned& count) {
    std::uintptr_t data = 0; unsigned capacity = 0;
    if (!Read(room + 0x125c, data) || !Read(room + 0x1260, capacity) || !Read(room + 0x1264, count) ||
        count > 4096 || count > capacity) return false;
    if (!count) return true;
    SIZE_T got = 0; const auto bytes = count * sizeof(std::uintptr_t);
    return data >= 0x10000 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(data), pointers.data(), bytes, &got) && got == bytes;
}
bool Tear(std::uintptr_t address, Entity& e, std::uint32_t* index = nullptr) {
    unsigned char exists = 0, dead = 0; std::uintptr_t table = 0;
    return Read(address, table) && table == base + kTearTable && Read(address + 0x172, exists) && exists &&
        Read(address + 0x173, dead) && !dead && Read(address + 0x28, e.type) && e.type == 2 &&
        Read(address + 0x2c, e.variant) && Read(address + 0x30, e.subtype) && Read(address + 0x3ec, e.seed) &&
        (!index || Read(address + 0x20, *index)) && ReadBody(address, e.body) && Read(address + 0x410, e.height) &&
        Read(address + 0x414, e.fallingSpeed) && Read(address + 0x418, e.fallingAccel) && Read(address + 0x420, e.scale);
}
bool ReadNpc(std::uintptr_t address, Npc& n) {
    unsigned char exists = 0, dead = 0, visible = 0; std::uintptr_t table = 0;
    if (!Read(address, table) || table != base + kNpcTable || !Read(address + 0x172, exists) || !exists ||
        !Read(address + 0x173, dead) || dead || !Read(address + 0x171, visible)) return false;
    n.visible = visible ? 1u : 0u;
    return Read(address + 0x28, n.type) && Read(address + 0x2c, n.variant) && Read(address + 0x30, n.subtype) &&
        Read(address + 0x3ec, n.seed) && ReadBody(address, n.body) && Read(address + 0x334, n.target) &&
        Read(address + 0x380, n.hitPoints) && Read(address + 0x384, n.maxHitPoints) && Read(address + 0xb64, n.state) &&
        Read(address + 0x184, n.gridCollision) && Read(address + 0x188, n.entityCollision);
}
bool WriteNpc(std::uintptr_t address, const Npc& n) {
    __try {
        *reinterpret_cast<Vec*>(address + 0x33c) = n.body.position; *reinterpret_cast<Vec*>(address + 0x360) = n.body.velocity;
        *reinterpret_cast<Vec*>(address + 0x334) = n.target; *reinterpret_cast<float*>(address + 0x380) = n.hitPoints;
        *reinterpret_cast<std::int32_t*>(address + 0xb64) = n.state; *reinterpret_cast<unsigned char*>(address + 0x171) = n.visible ? 1 : 0;
        *reinterpret_cast<std::uint32_t*>(address + 0x184) = n.gridCollision; *reinterpret_cast<std::uint32_t*>(address + 0x188) = n.entityCollision;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool KillNpc(std::uintptr_t address) {
    __try { reinterpret_cast<Update>(base + kKill)(reinterpret_cast<void*>(address)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The replica's enemies fight their own simulation; its held players must not take their hits.
bool Shield(bool on) {
    __try {
        for (unsigned i = 0; i < team.count; ++i) {
            auto& collision = *reinterpret_cast<std::uint32_t*>(team.players[i] + 0x188);
            if (on) { if (!playerShielded) playerCollision[i] = collision; collision = 0; }
            else if (playerShielded) collision = playerCollision[i];
        }
        playerShielded = on; return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool WriteBody(std::uintptr_t address, const Body& body) {
    __try { *reinterpret_cast<Vec*>(address + 0x33c) = body.position; *reinterpret_cast<Vec*>(address + 0x360) = body.velocity; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool WriteTear(std::uintptr_t address, const Entity& e) {
    __try {
        *reinterpret_cast<Vec*>(address + 0x33c) = e.body.position; *reinterpret_cast<Vec*>(address + 0x360) = e.body.velocity;
        *reinterpret_cast<float*>(address + 0x410) = e.height; *reinterpret_cast<float*>(address + 0x414) = e.fallingSpeed;
        *reinterpret_cast<float*>(address + 0x418) = e.fallingAccel; *reinterpret_cast<float*>(address + 0x420) = e.scale;
        // Replica tears are display objects: no independent damage, pickup or grid collision effects.
        *reinterpret_cast<unsigned*>(address + 0x184) = 0; *reinterpret_cast<unsigned*>(address + 0x188) = 0;
        *reinterpret_cast<float*>(address + 0x388) = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::uintptr_t SpawnTear(std::uintptr_t game, const Entity& e) {
    __try { return reinterpret_cast<std::uintptr_t>(reinterpret_cast<Spawn>(base + kSpawn)(
        reinterpret_cast<void*>(game), 2, 0, e.body.position, e.body.velocity, reinterpret_cast<void*>(target), 0, e.seed)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
bool RemoveTear(Tracked& t) {
    if (!t.address) return true;
    std::uint32_t seed = 0, index = 0; unsigned char exists = 0;
    if (!Read(t.address + 0x3ec, seed) || !Read(t.address + 0x20, index) || !Read(t.address + 0x172, exists)) return false;
    if (exists && seed == t.state.seed && index == t.localIndex) {
        __try { reinterpret_cast<Update>(base + kRemove)(reinterpret_cast<void*>(t.address)); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        if (!Read(t.address + 0x172, exists) || exists) return false;
        ++removed;
    } else ++vanished; // The game already destroyed it, e.g. together with the room it was in.
    t = {}; return true;
}
bool Clear() {
    bool ok = true;
    for (auto& entry : tracked) if (!RemoveTear(entry)) ok = false;
    adopted = {};
    if (playerShielded && !Shield(false)) ok = false;
    haveFrame = false;
    if (!ok) ++faults;
    return ok;
}
void Publish(const Frame& frame) {
    Packet packet;
    if (!Encode(frame, packet)) { ++faults; return; }
    InterlockedIncrement(&published.generation);
    published.bytes = static_cast<std::uint32_t>(packet.size); published.packet = packet.bytes; published.alive = 1;
    InterlockedIncrement(&published.generation);
}
void InvalidatePublication() { InterlockedIncrement(&published.generation); published.alive = 0; InterlockedIncrement(&published.generation); }
// Actions: 0 captured, 1 applied, 2 held, 3 room requested, 4 requested room reached, 5 moved by the replica's own game.
// Only world states are published.
void RecordFrame(const Frame& frame, unsigned action = 0) {
    if (frameCount < records.size()) records[frameCount++] = {GetTickCount64(), GetCurrentThreadId(), created, removed, action, frame};
    else ++dropped;
    if (action < 3) Publish(frame);
}
bool RequestRoom(std::uintptr_t game, int index, int direction, int dimension) {
    __try { reinterpret_cast<Transition>(base + kTransition)(reinterpret_cast<void*>(game), index, direction, kFade, nullptr, dimension); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool BeginTravel(std::uintptr_t game, const Frame& frame) {
    RoomKey there; std::uint32_t mode = 1; std::int32_t latched = -1;
    // Never walk into a room the host did not describe: both levels must come from the same run seed.
    if (!Describe(game, frame.room.index, frame.room.dimension, there) || !SamePlace(there, frame.room)) { ++levelMismatches; return false; }
    if (!Read(game + kRoomTransition, mode) || mode || !Clear()) return false;
    InvalidatePublication();
    if (!RequestRoom(game, static_cast<int>(frame.room.index), Direction(localRoom, frame.room), static_cast<int>(frame.room.dimension)) ||
        !Read(game + kRoomTransition, mode) || mode != 1 || !Read(game + kRoomTransition + 0x14, latched) ||
        latched != static_cast<std::int32_t>(frame.room.index)) return false;
    traveling = true; travelFrame = frame; travelDeadline = GetTickCount64() + kTravelMs; ++transitions;
    RecordFrame(frame, 3); return true;
}
bool Capture(std::uintptr_t room, Frame& frame) {
    std::array<std::uintptr_t, 4096> pointers{}; unsigned count = 0;
    std::array<Identity, kMaxEntities> identities{}; std::array<std::uint32_t, kMaxEntities> assigned{};
    // lastFault: 1 list or player, 2 entity or tear read, 3 too many tears, 4 enemy read, 5 too many enemies,
    // 6 network ids, 7 value outside the wire format.
    if (!List(room, pointers, count) || !ReadPlayers(frame, kPlayers)) { lastFault = 1; return false; }
    for (unsigned i = 0; i < count; ++i) {
        std::uint32_t type = 0; unsigned char exists = 0, dead = 0; std::uintptr_t table = 0;
        if (!Read(pointers[i], table) || !Read(pointers[i] + 0x28, type) || !Read(pointers[i] + 0x172, exists) ||
            !Read(pointers[i] + 0x173, dead)) { lastFault = 2; return false; }
        if (!exists || dead) continue;
        if (kNpcs) {
            if (table == base + kNpcTable) {
                if (frame.npcCount == kMaxNpcs) { lastFault = 5; return false; } // Never publish a truncated roster.
                if (!ReadNpc(pointers[i], frame.npcs[frame.npcCount])) { lastFault = 4; return false; }
                ++frame.npcCount; continue;
            }
        }
        if (type != 2) continue;
        if (frame.count == kMaxEntities) { lastFault = 3; return false; } // Never publish a truncated roster as a full snapshot.
        auto& entity = frame.entities[frame.count]; auto& identity = identities[frame.count];
        identity.address = static_cast<std::uint32_t>(pointers[i]);
        if (!Tear(pointers[i], entity, &identity.index)) { lastFault = 2; return false; }
        identity.seed = entity.seed; ++frame.count;
    }
    if (!ids.Assign(identities.data(), frame.count, assigned.data())) { lastFault = 6; return false; }
    for (unsigned i = 0; i < frame.count; ++i) frame.entities[i].id = assigned[i];
    if (!Valid(frame)) { lastFault = 7; return false; }
    return true;
}
// Match the host's enemies to the replica's own by identity and bring those to the host's state. The readback
// lists only matched enemies, so an unmatched one can never pass for an applied one.
bool Adopt(std::uintptr_t game, const Frame& frame, Frame& readback) {
    std::array<std::uintptr_t, 4096> pointers{}; unsigned count = 0; std::uintptr_t room = 0;
    std::array<bool, kMaxNpcs> matched{};
    if (!Read(game + 0x18300, room) || !List(room, pointers, count)) return false;
    adopted = {}; readback.npcCount = 0;
    for (unsigned p = 0; p < count; ++p) {
        Npc local;
        if (!ReadNpc(pointers[p], local)) continue;
        bool found = false;
        for (unsigned i = 0; i < frame.npcCount && !found; ++i) if (!matched[i] && SameNpc(frame.npcs[i], local)) {
            found = matched[i] = true; adopted[i] = {pointers[p], frame.npcs[i]};
            Npc actual;
            if (!WriteNpc(pointers[p], frame.npcs[i]) || !ReadNpc(pointers[p], actual)) return false;
            ++npcCorrections; readback.npcs[readback.npcCount++] = actual;
        }
        if (found) continue;
        // Enemies the host never listed are left alone until later spawns are replicated.
        if (!Departed(haveFrame ? &lastFrame : nullptr, frame, local)) { ++npcOrphans; continue; }
        unsigned char dead = 0;
        if (!KillNpc(pointers[p])) return false;
        ++npcKilled;
        if (!Read(pointers[p] + 0x173, dead) || !dead) ++npcKillUnconfirmed;
    }
    for (unsigned i = 0; i < frame.npcCount; ++i) if (!matched[i]) ++npcUnmatched;
    return true;
}
bool Apply(std::uintptr_t game, const Frame& frame, Frame& readback) {
    Delta delta;
    // The host's players are this game's players one to one; a different number of them cannot be shown.
    if (frame.playerCount != team.count) { ++rosterMismatches; return false; }
    if (!Plan(haveFrame ? lastFrame : Frame{}, frame, delta)) return false;
    for (auto& entry : tracked) if (entry.address) {
        bool found = false;
        for (unsigned i = 0; i < frame.count; ++i) if (frame.entities[i].id == entry.state.id) found = true;
        if (!found && !RemoveTear(entry)) return false;
    }
    readback = frame;
    for (unsigned i = 0; i < frame.count; ++i) {
        const auto& entity = frame.entities[i]; Tracked* match = nullptr;
        for (auto& entry : tracked) if (entry.address && entry.state.id == entity.id) match = &entry;
        if (!match) {
            for (auto& entry : tracked) if (!entry.address) { match = &entry; break; }
            if (!match) return false;
            const auto address = SpawnTear(game, entity);
            if (!address) return false;
            match->address = address; match->state = entity;
            if (!Read(address + 0x20, match->localIndex)) return false;
            ++created;
        }
        Entity actual; std::uint32_t index = 0;
        if (!Tear(match->address, actual, &index) || actual.seed != entity.seed || index != match->localIndex ||
            actual.variant != entity.variant || actual.subtype != entity.subtype || !WriteTear(match->address, entity) ||
            !Tear(match->address, actual)) return false;
        match->state = entity; actual.id = entity.id; readback.entities[i] = actual;
    }
    for (unsigned i = 0; i < team.count; ++i) if (!WriteBody(team.players[i], frame.players[i].body)) return false;
    if (!ReadPlayers(readback, false)) return false;
    if (kNpcs) { if (!Adopt(game, frame, readback) || !Shield(true)) return false; }
    lastFrame = frame; haveFrame = true; return true;
}
bool HeldReadback(Frame& frame) {
    frame = lastFrame; frame.npcCount = 0; // Held enemies keep their last host state; only tears are read back here.
    if (!ReadPlayers(frame, false)) return false;
    for (unsigned i = 0; i < frame.count; ++i) {
        bool found = false;
        for (const auto& entry : tracked) if (entry.address && entry.state.id == frame.entities[i].id) {
            Entity actual; std::uint32_t index = 0;
            if (!Tear(entry.address, actual, &index) || actual.seed != entry.state.seed || index != entry.localIndex) return false;
            actual.id = entry.state.id; frame.entities[i] = actual; found = true; break;
        }
        if (!found) return false;
    }
    return true;
}
void __fastcall OnTear(void* object, void*) {
    active.fetch_add(1);
    bool hold = false;
    if (running.load(std::memory_order_acquire) && owner.load() == GetCurrentThreadId()) {
        const auto address = reinterpret_cast<std::uintptr_t>(object);
        for (const auto& entry : tracked) if (entry.address == address) {
            Entity check; std::uint32_t index = 0;
            if (Tear(address, check, &index) && check.seed == entry.state.seed && index == entry.localIndex && WriteTear(address, entry.state)) {
                hold = true; ++tearHolds;
            } else ++faults;
            break;
        }
    }
    if (!hold) originalTear(object);
    active.fetch_sub(1, std::memory_order_release);
}
void __fastcall OnNpc(void* object, void*) {
    active.fetch_add(1);
    originalNpc(object);
    if (running.load(std::memory_order_acquire) && owner.load() == GetCurrentThreadId() && haveFrame) {
        const auto address = reinterpret_cast<std::uintptr_t>(object);
        for (const auto& entry : adopted) if (entry.address == address) {
            Npc local;
            // How far one update of the replica's own AI moved this enemy away from the host's last word.
            if (ReadNpc(address, local) && SameNpc(local, entry.state)) {
                const auto dx = local.body.position.x - entry.state.body.position.x, dy = local.body.position.y - entry.state.body.position.y;
                const auto drift = std::sqrt(dx * dx + dy * dy);
                ++npcUpdates; npcDriftSum += drift; npcDriftMax = (std::max)(npcDriftMax, drift);
                if (local.state != entry.state.state) ++npcStateDrift;
                if (local.visible != entry.state.visible) ++npcVisibleDrift;
                if (WriteNpc(address, entry.state)) ++npcCorrections; else ++faults;
            }
            break;
        }
    }
    active.fetch_sub(1, std::memory_order_release);
}
void __fastcall OnPlayer(void* object, void*) {
    active.fetch_add(1);
    if (!running.load(std::memory_order_acquire)) { originalPlayer(object); active.fetch_sub(1); return; }
    DWORD empty = 0; owner.compare_exchange_strong(empty, GetCurrentThreadId());
    if (owner.load() != GetCurrentThreadId()) { originalPlayer(object); active.fetch_sub(1); return; }
    const auto self = reinterpret_cast<std::uintptr_t>(object);
    std::uintptr_t game = 0, room = 0; Team found; RoomKey key; std::uint32_t seed = 0; unsigned index = 0;
    bool local = Local(game, room, found, key) && Read(found.players[0] + 0x3ec, seed);
    if (local) { while (index < found.count && found.players[index] != self) ++index; local = index < found.count; }
    if (kPlayers) {
        if (local && found.count > 1) {
            if (lastUpdated >= 0 && index != (static_cast<unsigned>(lastUpdated) + 1) % found.count) ++orderSurprises;
            lastUpdated = static_cast<int>(index);
            // One update per game step carries the step: the source captures after its last player, the replica
            // applies with its first. The other players are only held, or left to the game.
            if (index != (kSource ? found.count - 1 : 0)) {
                const bool held = !kSource && haveFrame && !traveling && SameTeam(found, team);
                if (held) {
                    if (!WriteBody(self, lastFrame.players[index].body)) ++faults;
                    ++playerHolds;
                } else originalPlayer(object);
                active.fetch_sub(1, std::memory_order_release); return;
            }
        }
    }
    const bool expired = stopRequested.load() || GetTickCount64() > deadline;
    // A room being loaded may be unreadable for a few updates: publish nothing and let the game run.
    if (kRooms) {
        if (!local && !expired && ++localFailures <= kLocalGrace) {
            InvalidatePublication(); originalPlayer(object); active.fetch_sub(1, std::memory_order_release); return;
        }
    }
    if (local) localFailures = 0;
    const bool elsewhere = local && !SameRoom(key, localRoom) && !(traveling && SamePlace(key, travelFrame.room));
    // Suppressing the player update does not disable doors: a player held in a doorway is carried through by the
    // replica's own game. That is a new place to follow the host from, not a foreign context.
    const bool relocated = kRooms && !kSource && elsewhere;
    const bool contextMismatch = !local || (!kSource && (!SameTeam(found, team) || seed != targetSeed || (elsewhere && !relocated)));
    if (contextMismatch || expired) {
        if (contextMismatch) ++contextChanges;
        if (!kSource) Clear();
        InvalidatePublication(); running = false; cleaned = true;
        originalPlayer(object); active.fetch_sub(1); return;
    }
    if (kSource) {
        // A player who joined or left starts a new generation too: the roster is part of the context.
        if (!SameTeam(found, team) || seed != targetSeed || !SameRoom(key, localRoom)) {
            ++sourceEpoch; ids.Reset(); localRoom = key; team = found; target = found.players[0]; targetSeed = seed; ++contextChanges;
        }
        originalPlayer(object);
        if (!Local(game, room, found, key) || !SameTeam(found, team) || !Read(target + 0x3ec, seed)) {
            InvalidatePublication(); active.fetch_sub(1); return;
        }
        if (seed != targetSeed || !SameRoom(key, localRoom)) {
            ++sourceEpoch; ids.Reset(); localRoom = key; targetSeed = seed; ++contextChanges;
        }
        Frame frame; frame.session = session; frame.timeMs = GetTickCount64(); frame.epoch = sourceEpoch;
        frame.sequence = ++sourceSequence; frame.room = key;
        if (Capture(room, frame)) RecordFrame(frame);
        else { ++faults; InvalidatePublication(); running = false; }
    } else if (relocated) {
        originalPlayer(object);
        // Snapshots of the room just left may still be in flight; do not walk back for them at once.
        departedEpoch = haveFrame ? lastFrame.epoch : 0; relocatedAt = GetTickCount64();
        auto moved = lastFrame; moved.room = key; moved.count = 0;
        Clear(); InvalidatePublication(); traveling = false; localRoom = key; ++selfMoves;
        RecordFrame(moved, 5);
    } else if (traveling) {
        // The game owns its player while it performs the transition it was asked for.
        originalPlayer(object);
        if (SamePlace(key, travelFrame.room)) {
            traveling = false; localRoom = key; ++arrivals;
            auto reached = travelFrame; reached.room = key; reached.count = 0; RecordFrame(reached, 4);
        } else if (GetTickCount64() > travelDeadline) { ++faults; Clear(); InvalidatePublication(); running = false; cleaned = true; }
    } else {
        if (haveFrame) {
            if (!WriteBody(target, lastFrame.players[0].body)) ++faults;
            if (kNpcs) { if (!Shield(true)) ++faults; }
            ++playerHolds;
        } else originalPlayer(object);
        Frame frame;
        if (!receiver.Take(frame) || !Fresh(frame, GetTickCount64())) {
            Frame readback;
            if (haveFrame) {
                if (HeldReadback(readback)) RecordFrame(readback, 2);
                else { ++faults; Clear(); InvalidatePublication(); running = false; cleaned = true; }
            }
        } else if (kRooms) {
            const auto route = Decide(localRoom, haveFrame ? &lastFrame : nullptr, frame);
            if (route == Route::Travel && frame.epoch == departedEpoch && GetTickCount64() - relocatedAt < kSettleMs) ++settled;
            else {
                Frame readback;
                // A newer host generation of this room owns none of the copies made for the previous one.
                bool ok = route == Route::Travel ? BeginTravel(game, frame) : (route != Route::Restart || Clear()) && Apply(game, frame, readback);
                if (ok && route != Route::Travel) RecordFrame(readback, 1);
                if (!ok) { ++faults; Clear(); InvalidatePublication(); running = false; cleaned = true; }
            }
        } else {
            const bool compatible = frame.room.type == localRoom.type && frame.room.variant == localRoom.variant &&
                frame.room.shape == localRoom.shape && frame.room.stage == localRoom.stage && frame.room.stageType == localRoom.stageType;
            if (!compatible || (haveFrame && (frame.epoch != lastFrame.epoch || !SameRoom(frame.room, lastFrame.room)))) {
                ++contextChanges; Clear(); InvalidatePublication(); running = false; cleaned = true;
            } else {
                Frame readback;
                if (Apply(game, frame, readback)) RecordFrame(readback, 1);
                else { ++faults; Clear(); InvalidatePublication(); running = false; cleaned = true; }
            }
        }
    }
    active.fetch_sub(1, std::memory_order_release);
}
void BodyJson(std::ostringstream& out, const Body& b) { out << '[' << b.position.x << ',' << b.position.y << ',' << b.velocity.x << ',' << b.velocity.y << ']'; }
void FrameJson(std::ostringstream& out, const Frame& f) {
    out << "\"sequence\":" << f.sequence << ",\"epoch\":" << f.epoch << ",\"timeMs\":" << f.timeMs << ",\"room\":["
        << f.room.stage << ',' << f.room.stageType << ',' << f.room.index << ',' << f.room.dimension << ',' << f.room.type << ','
        << f.room.variant << ',' << f.room.shape << ',' << f.room.spawnSeed << ',' << f.room.visits << "],\"player\":";
    BodyJson(out, f.players[0].body);
    // Listed exactly when the wire format lists them: more than one player, or a first player not on controller 0.
    if (f.playerCount != 1 || f.players[0].controller) {
        out << ",\"players\":[";
        for (unsigned i = 0; i < f.playerCount; ++i) {
            out << (i ? "," : "") << "{\"controller\":" << f.players[i].controller << ",\"body\":"; BodyJson(out, f.players[i].body); out << '}';
        }
        out << ']';
    }
    out << ",\"entities\":[";
    for (unsigned i = 0; i < f.count; ++i) {
        const auto& e = f.entities[i]; if (i) out << ',';
        out << "{\"id\":" << e.id << ",\"seed\":" << e.seed << ",\"type\":" << e.type << ",\"variant\":" << e.variant
            << ",\"subtype\":" << e.subtype << ",\"body\":"; BodyJson(out, e.body);
        out << ",\"tear\":[" << e.height << ',' << e.fallingSpeed << ',' << e.fallingAccel << ',' << e.scale << "]}";
    }
    out << "],\"npcs\":[";
    for (unsigned i = 0; i < f.npcCount; ++i) {
        const auto& n = f.npcs[i]; if (i) out << ',';
        out << "{\"type\":" << n.type << ",\"variant\":" << n.variant << ",\"subtype\":" << n.subtype << ",\"seed\":" << n.seed << ",\"body\":";
        BodyJson(out, n.body);
        out << ",\"target\":[" << n.target.x << ',' << n.target.y << "],\"hp\":[" << n.hitPoints << ',' << n.maxHitPoints
            << "],\"state\":" << n.state << ",\"flags\":[" << n.visible << ',' << n.gridCollision << ',' << n.entityCollision << "]}";
    }
    out << ']';
}
std::filesystem::path Directory() {
    wchar_t buffer[32768]{}; const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, 32768);
    if (!n || n >= 32768) throw std::runtime_error("LOCALAPPDATA unavailable");
    return std::filesystem::path(buffer) / L"IsaacAuthority";
}
bool WriteFileText(const std::filesystem::path& path, const std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0; const BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
    CloseHandle(file); return ok && n == text.size();
}
bool Restore() {
    bool ok = true;
    if (npcInstalled) {
        ok = ExchangeSlot(npcSlot, reinterpret_cast<void*>(&OnNpc), reinterpret_cast<void*>(originalNpc)) == 0;
        if (ok) npcInstalled = false;
    }
    if (tearInstalled) {
        const bool restored = ExchangeSlot(tearSlot, reinterpret_cast<void*>(&OnTear), reinterpret_cast<void*>(originalTear)) == 0;
        if (restored) tearInstalled = false;
        ok = ok && restored;
    }
    if (playerInstalled) {
        const bool restored = ExchangeSlot(playerSlot, reinterpret_cast<void*>(&OnPlayer), reinterpret_cast<void*>(originalPlayer)) == 0;
        if (restored) playerInstalled = false;
        ok = ok && restored;
    }
    const auto until = GetTickCount64() + 2000;
    while (active.load(std::memory_order_acquire) && GetTickCount64() < until) Sleep(1);
    return ok && !active.load();
}
}

extern "C" DWORD WINAPI IsaacAuthorityWorldStart(void*) noexcept {
    Guard guard;
    if (report != INVALID_HANDLE_VALUE) return ERROR_ALREADY_EXISTS;
    DWORD failure = ERROR_INVALID_DATA;
    try {
        wchar_t image[32768]{};
        if (!GetModuleFileNameW(nullptr, image, 32768) || !isaac_probe::AnalyzeBytes(isaac_probe::ReadFile(image)).supported) return ERROR_BAD_EXE_FORMAT;
        base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        std::uintptr_t game = 0, room = 0;
        if (!Local(game, room, team, localRoom) || !Read(team.players[0] + 0x3ec, targetSeed) ||
            localRoom.type != 1 || localRoom.variant != 2 || localRoom.shape != 1) return ERROR_NOT_READY;
        target = team.players[0];
        Frame initial; initial.session = 1; initial.sequence = 1; initial.room = localRoom; ids.Reset();
        if (!Capture(room, initial) || initial.count) return ERROR_NOT_READY;
        playerSlot = reinterpret_cast<void**>(base + kPlayerTable + 12);
        tearSlot = reinterpret_cast<void**>(base + kTearTable + 12);
        std::uintptr_t playerFunction = 0, tearFunction = 0, removeFunction = 0;
        std::array<unsigned char, 8> spawnPrefix{};
        constexpr std::array<unsigned char, 8> expectedSpawn{0x55,0x8b,0xec,0x83,0xe4,0xf8,0x6a,0x00};
        if (!Read(reinterpret_cast<std::uintptr_t>(playerSlot), playerFunction) || playerFunction != base + kPlayerUpdate ||
            !Read(reinterpret_cast<std::uintptr_t>(tearSlot), tearFunction) || tearFunction != base + kTearUpdate ||
            !Read(base + kTearTable + 40, removeFunction) || removeFunction != base + kRemove ||
            !Read(base + kSpawn, spawnPrefix) || spawnPrefix != expectedSpawn) return ERROR_REVISION_MISMATCH;
        if constexpr (kNpcs) {
            std::uintptr_t npcFunction = 0;
            npcSlot = reinterpret_cast<void**>(base + kNpcTable + 12);
            if (!Read(reinterpret_cast<std::uintptr_t>(npcSlot), npcFunction) || npcFunction != base + kNpcUpdate) return ERROR_REVISION_MISMATCH;
            originalNpc = reinterpret_cast<Update>(npcFunction);
        }
        if constexpr (kNpcs && !kSource) {
            // push ebp; mov ebp,esp; sub esp,0x28; movss xmm1,[...]
            std::array<unsigned char, 10> killPrefix{};
            constexpr std::array<unsigned char, 10> expectedKill{0x55,0x8b,0xec,0x83,0xec,0x28,0xf3,0x0f,0x10,0x0d};
            if (!Read(base + kKill, killPrefix) || killPrefix != expectedKill) return ERROR_REVISION_MISMATCH;
        }
        if constexpr (kRooms && !kSource) {
            // push ebp; mov ebp,esp; push -1; push <relocated unwind table>
            std::array<unsigned char, 6> transitionPrefix{}; std::uintptr_t unwind = 0;
            constexpr std::array<unsigned char, 6> expectedTransition{0x55,0x8b,0xec,0x6a,0xff,0x68};
            if (!Read(base + kTransition, transitionPrefix) || transitionPrefix != expectedTransition ||
                !Read(base + kTransition + 6, unwind) || unwind != base + kTransitionUnwind) return ERROR_REVISION_MISMATCH;
        }
        originalPlayer = reinterpret_cast<Update>(playerFunction); originalTear = reinterpret_cast<Update>(tearFunction);
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&OnPlayer), &pinned)) return GetLastError();
        if (kSource) {
            if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&session), sizeof(session), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return ERROR_GEN_FAILURE;
            if (!session) session = 1;
            deadline = GetTickCount64() + kDurationMs;
        } else {
            const auto error = receiver.Start(); if (error) return error;
            session = receiver.Session(); deadline = receiver.Deadline();
        }
        ids.Reset(); tracked = {}; lastFrame = {}; haveFrame = false;
        sourceSequence = 0; sourceEpoch = 1; created = removed = faults = contextChanges = tearHolds = playerHolds = frameCount = dropped = 0;
        traveling = false; travelFrame = {}; travelDeadline = 0; transitions = arrivals = levelMismatches = localFailures = 0;
        selfMoves = vanished = settled = 0; departedEpoch = 0; relocatedAt = 0;
        adopted = {}; npcKilled = npcKillUnconfirmed = npcCorrections = npcUpdates = npcStateDrift = npcVisibleDrift = npcUnmatched = npcOrphans = lastFault = 0;
        npcDriftMax = 0; npcDriftSum = 0; playerCollision = {}; playerShielded = false;
        lastUpdated = -1; orderSurprises = rosterMismatches = 0;
        stopRequested = false; cleaned = false; owner = 0;
        published = Published{}; published.session = session;
        const auto directory = Directory(); std::filesystem::create_directories(directory / L"logs");
        const std::wstring role = std::wstring(kPlayers ? L"coop-" : kNpcs ? L"npc-" : kRooms ? L"room-" : L"world-") + (kSource ? L"source-" : L"replica-");
        const auto unique = role + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(session);
        const auto log = directory / L"logs" / (unique + L".jsonl");
        report = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (report == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create world log");
        running.store(true, std::memory_order_release);
        if constexpr (kNpcs && !kSource) {
            failure = ExchangeSlot(npcSlot, reinterpret_cast<void*>(originalNpc), reinterpret_cast<void*>(&OnNpc));
            npcInstalled = *npcSlot == reinterpret_cast<void*>(&OnNpc);
            if (failure) throw std::runtime_error("Cannot hook enemy update");
        }
        if (!kSource) {
            failure = ExchangeSlot(tearSlot, reinterpret_cast<void*>(originalTear), reinterpret_cast<void*>(&OnTear));
            tearInstalled = *tearSlot == reinterpret_cast<void*>(&OnTear);
            if (failure) throw std::runtime_error("Cannot hook tear update");
        }
        failure = ExchangeSlot(playerSlot, reinterpret_cast<void*>(originalPlayer), reinterpret_cast<void*>(&OnPlayer));
        playerInstalled = *playerSlot == reinterpret_cast<void*>(&OnPlayer);
        if (failure) throw std::runtime_error("Cannot hook player update");
        std::ostringstream descriptor;
        descriptor << "{\"pid\":" << GetCurrentProcessId() << ",\"role\":\"" << (kSource ? "source" : "replica")
            << "\",\"address\":" << reinterpret_cast<std::uintptr_t>(&published) << ",\"bytes\":" << sizeof(published)
            << ",\"session\":\"" << session << "\",\"deadlineMs\":" << deadline << ",\"port\":" << (kSource ? 0 : receiver.Port()) << "}\n";
        const auto temp = directory / (unique + L".tmp");
        const auto ready = directory / (role + std::to_wstring(GetCurrentProcessId()) + L".json");
        failure = ERROR_WRITE_FAULT;
        if (!WriteFileText(temp, descriptor.str()) || !MoveFileExW(temp.c_str(), ready.c_str(), MOVEFILE_REPLACE_EXISTING)) throw std::runtime_error("Cannot publish world endpoint");
        return ERROR_SUCCESS;
    } catch (...) {
        running = false; receiver.Stop(); Restore();
        if (report != INVALID_HANDLE_VALUE) { CloseHandle(report); report = INVALID_HANDLE_VALUE; }
        return failure ? failure : ERROR_INVALID_DATA;
    }
}
extern "C" DWORD WINAPI IsaacAuthorityWorldStop(void*) noexcept {
    Guard guard;
    if (report == INVALID_HANDLE_VALUE) return ERROR_NOT_READY;
    stopRequested = true; receiver.Stop();
    if (kSource) { running = false; cleaned = true; }
    else if (running.load()) {
        const auto until = GetTickCount64() + 2000;
        while (!cleaned.load() && GetTickCount64() < until) Sleep(1);
        if (!cleaned.load()) return ERROR_NOT_READY; // Cleanup must run on a live game update.
    }
    running = false;
    const bool restored = Restore();
    if (!restored) return ERROR_BUSY;
    InvalidatePublication();
    try {
        std::ostringstream out; out.precision(9);
        out << "{\"type\":\"start\",\"role\":\"" << (kSource ? "source" : "replica") << "\",\"scope\":\""
            << (kPlayers ? "standard-tears-rooms-enemies-players" : kNpcs ? "standard-tears-rooms-enemies" : kRooms ? "standard-tears-rooms" : "standard-tears")
            << "\",\"worldSnapshot\":false}\n";
        for (unsigned i = 0; i < frameCount; ++i) {
            const auto& r = records[i];
            out << "{\"type\":\"frame\",\"observedMs\":" << r.now << ",\"thread\":" << r.thread << ",\"created\":" << r.created << ",\"removed\":" << r.removed << ",\"action\":" << r.action << ',';
            FrameJson(out, r.frame); out << "}\n";
        }
        unsigned remaining = 0; for (const auto& entry : tracked) if (entry.address) ++remaining;
        out << "{\"type\":\"stop\",\"frames\":" << frameCount << ",\"created\":" << created << ",\"removed\":" << removed
            << ",\"remaining\":" << remaining << ",\"faults\":" << faults << ",\"contextChanges\":" << contextChanges
            << ",\"tearHolds\":" << tearHolds << ",\"playerHolds\":" << playerHolds << ",\"dropped\":" << dropped
            << ",\"transitions\":" << transitions << ",\"arrivals\":" << arrivals << ",\"levelMismatches\":" << levelMismatches
            << ",\"traveling\":" << (traveling ? "true" : "false") << ",\"selfMoves\":" << selfMoves << ",\"vanished\":" << vanished
            << ",\"settled\":" << settled << ",\"npcCorrections\":" << npcCorrections << ",\"npcUpdates\":" << npcUpdates
            << ",\"npcStateDrift\":" << npcStateDrift << ",\"npcVisibleDrift\":" << npcVisibleDrift << ",\"npcDriftMax\":" << npcDriftMax
            << ",\"npcDriftMean\":" << (npcUpdates ? npcDriftSum / npcUpdates : 0.0) << ",\"npcUnmatched\":" << npcUnmatched
            << ",\"npcOrphans\":" << npcOrphans << ",\"npcKilled\":" << npcKilled << ",\"npcKillUnconfirmed\":" << npcKillUnconfirmed << ",\"playerShielded\":" << (playerShielded ? "true" : "false") << ",\"lastFault\":" << lastFault
            << ",\"players\":" << team.count << ",\"rosterMismatches\":" << rosterMismatches << ",\"updateOrderSurprises\":" << orderSurprises
            << ",\"slotsRestored\":true,\"accepted\":" << receiver.accepted.load() << ",\"rejected\":" << receiver.rejected.load()
            << ",\"replaced\":" << receiver.replaced.load() << ",\"networkErrors\":" << receiver.errors.load() << "}\n";
        const auto text = out.str(); DWORD written = 0;
        const BOOL ok = WriteFile(report, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        CloseHandle(report); report = INVALID_HANDLE_VALUE;
        return ok && written == text.size() && !remaining && !faults ? ERROR_SUCCESS : ERROR_INVALID_DATA;
    } catch (...) { CloseHandle(report); report = INVALID_HANDLE_VALUE; return ERROR_WRITE_FAULT; }
}
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
