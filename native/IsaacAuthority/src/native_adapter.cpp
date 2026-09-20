// Changing the game's own online from inside, by the user's model: a character belongs to its owner, the world to the host.
//
// Step 1: the local player answers to the keyboard at once. In the game's lockstep a player's input is scheduled some
// frames ahead and every game, the player's own included, applies it only then: 134-149 ms from key to movement between
// two windows of one machine, against 15 ms in a solo game. Here the local player's update reads the physical device
// directly, while the game's own net device keeps broadcasting the scheduled input to the others as before.
//
// The two games then no longer compute the same frames, which the lockstep takes for a desync and answers by splitting the
// lobby. So the comparison of frame checksums (RVA 0x50d4a0, thiscall, one argument) is made to answer "equal". That is a
// change of code, and like the localhost module this one refuses to work anywhere but in an isolated test instance.
//
// Step 2: a remote player stands where its owner says. After its own update every game publishes the body of its local
// player (room, position, velocity); after the update of a remote player the newest body of its owner, if it is of the same
// room, replaces what the delayed input produced. Everything else about that player still comes from the game itself.
//
// Step 3: the enemies are the host's. Once a frame the host publishes the living enemies of its room - seed, position,
// velocity, hit points - and the seeds of those that died lately. Every game still simulates its own enemies; a guest lays
// the host's state over the ones with the same seed (the games generate a room alike, so the seeds agree) and kills the
// ones the host saw die, with the function the game's own Lua Kill() runs (RVA 0x45dc30). An enemy born in the middle of a
// fight gets its seed from a random stream the two games no longer share, so whatever is left without a seed match is paired
// with the nearest unpaired enemy of the same type and variant, and a local enemy the host has no counterpart for during
// some twenty snapshots is removed. A guest's own simulation must
// not kill an enemy the host still has - the guest would lose sight of it for good - so on a guest a blow that would kill
// (Entity_NPC's TakeDamage, slot 8, RVA 0x2d60a0) is cut down to one the enemy just survives; its death comes from the host.
//
// Whatever carries the published slots to the other games - for now a relay outside, later the game's own connection -
// delivers them as PLR1 and WLN1 datagrams to this module's loopback socket.
//
// %LOCALAPPDATA%\IsaacAuthority\native-<pid>.cfg, written by the harness from the game's own log: the lobby's device number
// of the local player (2, 3, ...) and, for the game that made the lobby, the word "host".
#include <winsock2.h>
#include <windows.h>
#include "profile.hpp"
#include "vtable_slot.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
using namespace authority;
// InputManager at RVA 0x857b18; slot 29 is WithDevice(controller, reader, in, out, deviceId), thiscall, ret 0x14.
constexpr std::uintptr_t kManager = 0x857b18, kManagerTable = 0x782950, kWithDevice = 0x620fb0;
constexpr std::uintptr_t kCompare = 0x50d4a0, kSaveLeaf = 0x77e04c;
constexpr std::array<std::uint8_t, 8> kCompareEntry{0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18, 0x53, 0x56};
constexpr std::array<std::uint8_t, 8> kCompareEqual{0xB8, 0x01, 0x00, 0x00, 0x00, 0xC2, 0x04, 0x00};  // mov eax, 1; ret 4
constexpr char kIsolated[] = "IsaacAuthority-";
constexpr int kKeyboard = 0;
// Entity: exists +0x172, dead +0x173, position +0x33c, velocity +0x360, hit points +0x380, maximum +0x384, seed +0x3ec.
// Entity_Player: update is slot 3 of the table at RVA 0x76bdd0; controller +0x1618. Entity_NPC: table at RVA 0x767468.
constexpr std::uintptr_t kPlayerTable = 0x76bdd0, kPlayerUpdate = 0x382af0, kNpcTable = 0x767468, kController = 0x1618;
constexpr std::uintptr_t kExists = 0x172, kDead = 0x173, kPosition = 0x33c, kVelocity = 0x360, kHitPoints = 0x380, kMaxHitPoints = 0x384, kSeed = 0x3ec;
// Game at RVA 0x871678: current room +0x18300, its grid index +0x18304; the room's entity list: data +0x125c, count +0x1264.
constexpr std::uintptr_t kGame = 0x871678, kRoom = 0x18300, kRoomIndex = 0x18304, kListData = 0x125c, kListCount = 0x1264;
constexpr std::uintptr_t kKill = 0x45dc30, kNpcDamage = 0x2d60a0;
constexpr std::array<std::uint8_t, 10> kKillEntry{0x55, 0x8b, 0xec, 0x83, 0xec, 0x28, 0xf3, 0x0f, 0x10, 0x0d};
constexpr std::uint32_t kBodyMagic = 0x31524c50, kWorldMagic = 0x314e4c57;  // "PLR1", "WLN1"
constexpr int kControllers = 8, kMaxNpcs = 48, kMaxDeaths = 16, kDeathFrames = 90, kOrphanSnapshots = 20;
constexpr std::uintptr_t kType = 0x28, kVariant = 0x2c;
constexpr std::uint64_t kFreshMs = 250;
#pragma pack(push, 1)
struct Body { std::uint32_t magic, controller, sequence, room; float position[2], velocity[2]; };
struct Npc { std::uint32_t seed, type, variant; float position[2], velocity[2], hitPoints; };
struct World { std::uint32_t magic, sequence, room, count, deaths; Npc npcs[kMaxNpcs]; std::uint32_t died[kMaxDeaths]; };
// What a reader outside copies: generation is odd while the content is being written.
struct Published { std::uint32_t magic, generation; Body body; };
struct PublishedWorld { std::uint32_t magic, generation; World world; };
struct Stats {
    std::uint32_t published, received, applied, stale, rejected, otherRoom;
    std::uint32_t worldPublished, worldReceived, worldApplied, npcMatched, npcOnlyHost, npcOnlyLocal, npcKilled, hitPointFixes, deathsHeld, npcPaired, npcRemoved;
    float correctionSum, correctionMax, npcCorrectionSum, npcCorrectionMax;
};
#pragma pack(pop)
static_assert(sizeof(Body) == 32 && sizeof(Npc) == 32 && sizeof(World) == 20 + kMaxNpcs * 32 + kMaxDeaths * 4, "wire layout");
using WithDevice = int(__thiscall*)(void*, int, void*, void*, void*, int*);
using PlayerUpdate = void(__thiscall*)(void*);
using NpcDamage = char(__thiscall*)(void*, float, std::uint32_t, std::uint32_t, void*, int);
std::uintptr_t base = 0;
void** slot = nullptr; void** playerSlot = nullptr; void** damageSlot = nullptr;
WithDevice original = nullptr; PlayerUpdate originalPlayer = nullptr; NpcDamage originalDamage = nullptr;
int ownController = -1; bool host = false;
SRWLOCK lifecycle = SRWLOCK_INIT, inboxLock = SRWLOCK_INIT;
std::atomic<bool> running{false};
std::atomic<unsigned> counters[2]{};  // reads of the own player's input answered by the keyboard; all other reads
Published published{}; PublishedWorld publishedWorld{};
Stats stats{};
struct Inbox { Body body{}; std::uint64_t at = 0; std::uint32_t appliedSequence = 0; };
Inbox inbox[kControllers];
World worldInbox{}; std::uint64_t worldAt = 0; std::uint32_t worldApplied = 0;
SOCKET udp = INVALID_SOCKET; HANDLE worker = nullptr; bool winsock = false; unsigned short port = 0;
std::uint32_t sequence = 0, worldSequence = 0, frame = 0;
// The host's memory of the room: who lived a frame ago, and who died lately (seed, frame of death).
std::uint32_t livedRoom = 0xffffffff, lived[kMaxNpcs]{}, livedCount = 0;
struct Death { std::uint32_t seed, frame; } deaths[kMaxDeaths]{}; std::uint32_t deathCount = 0;
// The guest's memory of the room: which local enemy stands for which of the host's, and for how many snapshots a local
// enemy has had no counterpart.
struct Alias { std::uint32_t hostSeed, localSeed; } aliases[kMaxNpcs]{}; std::uint32_t aliasCount = 0, aliasRoom = 0xffffffff;
struct Orphan { std::uint32_t seed, age; } orphans[kMaxNpcs]{}; std::uint32_t orphanCount = 0;

// The physical device answers whatever window has the focus: a game in the background (the test instances do not pause
// there) would walk its player with keys meant for another window. Asked at most once a millisecond.
bool Focused() {
    static std::uint64_t asked = 0; static bool focused = false;
    const auto now = GetTickCount64();
    if (now != asked) {
        DWORD owner = 0; const HWND front = GetForegroundWindow();
        focused = front && GetWindowThreadProcessId(front, &owner) && owner == GetCurrentProcessId(); asked = now;
    }
    return focused;
}

int __fastcall OnInput(void* self, void*, int controller, void* reader, void* in, void* out, int* device) {
    if (running.load(std::memory_order_acquire) && controller == ownController && Focused()) {
        counters[0].fetch_add(1, std::memory_order_relaxed);
        return original(self, kKeyboard, reader, in, out, device);
    }
    counters[1].fetch_add(1, std::memory_order_relaxed);
    return original(self, controller, reader, in, out, device);
}

bool Finite(const float* v) { return std::isfinite(v[0]) && std::isfinite(v[1]); }
template <class T> T& At(std::uintptr_t address) { return *reinterpret_cast<T*>(address); }

bool LivingNpc(std::uintptr_t entity) {
    return At<std::uintptr_t>(entity) == base + kNpcTable && At<std::uint8_t>(entity + kExists) && !At<std::uint8_t>(entity + kDead) &&
        At<float>(entity + kMaxHitPoints) > 0;
}

// Host: the living enemies of the room, and who of last frame's is gone.
void PublishWorld(std::uintptr_t room, std::uint32_t roomIndex) {
    const auto data = At<std::uintptr_t>(room + kListData); const auto count = At<std::uint32_t>(room + kListCount);
    if (!data || count > 4096) return;
    if (roomIndex != livedRoom) { livedRoom = roomIndex; livedCount = 0; deathCount = 0; }
    publishedWorld.generation++;
    auto& world = publishedWorld.world; world.magic = kWorldMagic; world.sequence = ++worldSequence; world.room = roomIndex; world.count = 0;
    for (std::uint32_t i = 0; i < count && world.count < kMaxNpcs; ++i) {
        const auto entity = At<std::uintptr_t>(data + i * sizeof(std::uintptr_t));
        if (!entity || !LivingNpc(entity)) continue;
        auto& npc = world.npcs[world.count++];
        npc.seed = At<std::uint32_t>(entity + kSeed); npc.hitPoints = At<float>(entity + kHitPoints);
        npc.type = At<std::uint32_t>(entity + kType); npc.variant = At<std::uint32_t>(entity + kVariant);
        std::memcpy(npc.position, reinterpret_cast<void*>(entity + kPosition), 8); std::memcpy(npc.velocity, reinterpret_cast<void*>(entity + kVelocity), 8);
    }
    for (std::uint32_t i = 0; i < livedCount; ++i) {
        bool alive = false;
        for (std::uint32_t n = 0; n < world.count && !alive; ++n) alive = world.npcs[n].seed == lived[i];
        if (!alive) { deaths[deathCount % kMaxDeaths] = Death{lived[i], frame}; ++deathCount; }
    }
    livedCount = world.count;
    for (std::uint32_t n = 0; n < world.count; ++n) lived[n] = world.npcs[n].seed;
    world.deaths = 0;
    for (std::uint32_t i = 0; i < kMaxDeaths && i < deathCount; ++i)
        if (frame - deaths[i].frame <= kDeathFrames) world.died[world.deaths++] = deaths[i].seed;
    publishedWorld.generation++;
    stats.worldPublished++;
}

// Guest: the host's enemies over the local ones.
void ApplyWorld(std::uintptr_t room, std::uint32_t roomIndex) {
    static World world;  // the game's thread only
    bool fresh = false;
    AcquireSRWLockShared(&inboxLock);
    if (worldInbox.sequence && worldInbox.sequence != worldApplied && GetTickCount64() - worldAt <= kFreshMs) { world = worldInbox; fresh = true; }
    ReleaseSRWLockShared(&inboxLock);
    if (!fresh) return;
    if (world.room != roomIndex) { stats.otherRoom++; return; }
    if (aliasRoom != roomIndex) { aliasRoom = roomIndex; aliasCount = 0; orphanCount = 0; }
    const auto data = At<std::uintptr_t>(room + kListData); const auto count = At<std::uint32_t>(room + kListCount);
    if (!data || count > 4096) return;
    std::uintptr_t local[kMaxNpcs]; int partner[kMaxNpcs]; bool taken[kMaxNpcs]{}; std::uint32_t locals = 0;
    for (std::uint32_t i = 0; i < count && locals < kMaxNpcs; ++i) {
        const auto entity = At<std::uintptr_t>(data + i * sizeof(std::uintptr_t));
        if (entity && LivingNpc(entity)) { local[locals] = entity; partner[locals] = -1; ++locals; }
    }
    // 1. by seed, or by the pairing remembered from an earlier snapshot
    for (std::uint32_t l = 0; l < locals; ++l) {
        auto seed = At<std::uint32_t>(local[l] + kSeed);
        for (std::uint32_t a = 0; a < aliasCount; ++a) if (aliases[a].localSeed == seed) { seed = aliases[a].hostSeed; break; }
        for (std::uint32_t n = 0; n < world.count; ++n) if (!taken[n] && world.npcs[n].seed == seed) { partner[l] = static_cast<int>(n); taken[n] = true; break; }
    }
    // 2. what is left: the nearest unpaired enemy of the same kind
    for (std::uint32_t n = 0; n < world.count; ++n) {
        if (taken[n]) continue;
        int best = -1; float bestDistance = 0;
        for (std::uint32_t l = 0; l < locals; ++l) {
            if (partner[l] >= 0 || At<std::uint32_t>(local[l] + kType) != world.npcs[n].type || At<std::uint32_t>(local[l] + kVariant) != world.npcs[n].variant) continue;
            const auto* position = reinterpret_cast<float*>(local[l] + kPosition);
            const float dx = position[0] - world.npcs[n].position[0], dy = position[1] - world.npcs[n].position[1], distance = dx * dx + dy * dy;
            if (best < 0 || distance < bestDistance) { best = static_cast<int>(l); bestDistance = distance; }
        }
        if (best < 0) { stats.npcOnlyHost++; continue; }
        partner[best] = static_cast<int>(n); taken[n] = true; stats.npcPaired++;
        if (aliasCount < kMaxNpcs) aliases[aliasCount++] = Alias{world.npcs[n].seed, At<std::uint32_t>(local[best] + kSeed)};
    }
    // 3. the host's state over the partners; who has none grows older and is removed in the end; who died at the host dies here
    std::uintptr_t doomed[kMaxNpcs]; std::uint32_t doomedCount = 0;
    for (std::uint32_t l = 0; l < locals; ++l) {
        const auto entity = local[l]; const auto ownSeed = At<std::uint32_t>(entity + kSeed); auto seed = ownSeed;
        for (std::uint32_t a = 0; a < aliasCount; ++a) if (aliases[a].localSeed == ownSeed) { seed = aliases[a].hostSeed; break; }
        bool died = false;
        for (std::uint32_t d = 0; d < world.deaths && !died; ++d) died = world.died[d] == seed;
        if (died) { doomed[doomedCount++] = entity; continue; }
        std::uint32_t index = orphanCount;
        for (std::uint32_t o = 0; o < orphanCount; ++o) if (orphans[o].seed == ownSeed) { index = o; break; }
        if (partner[l] < 0) {
            stats.npcOnlyLocal++;
            if (index == orphanCount && orphanCount < kMaxNpcs) orphans[orphanCount++] = Orphan{ownSeed, 0};
            if (index < orphanCount && ++orphans[index].age > kOrphanSnapshots) { doomed[doomedCount++] = entity; stats.npcRemoved++; }
            continue;
        }
        if (index < orphanCount) orphans[index].age = 0;
        const auto& npc = world.npcs[partner[l]];
        auto* position = reinterpret_cast<float*>(entity + kPosition);
        const float dx = npc.position[0] - position[0], dy = npc.position[1] - position[1], distance = std::sqrt(dx * dx + dy * dy);
        std::memcpy(position, npc.position, 8); std::memcpy(reinterpret_cast<void*>(entity + kVelocity), npc.velocity, 8);
        if (At<float>(entity + kHitPoints) != npc.hitPoints) { At<float>(entity + kHitPoints) = npc.hitPoints; stats.hitPointFixes++; }
        stats.npcCorrectionSum += distance; if (distance > stats.npcCorrectionMax) stats.npcCorrectionMax = distance;
        stats.npcMatched++;
    }
    // Killing spawns effects, which may move the list: only after the walk over it.
    for (std::uint32_t d = 0; d < doomedCount; ++d) { reinterpret_cast<PlayerUpdate>(base + kKill)(reinterpret_cast<void*>(doomed[d])); stats.npcKilled++; }
    stats.worldApplied++; worldApplied = world.sequence;
}

// The game's thread, right after the game's own update of one player.
void AfterUpdate(std::uintptr_t player) noexcept {
    __try {
        const int controller = At<int>(player + kController);
        auto* position = reinterpret_cast<float*>(player + kPosition); auto* velocity = reinterpret_cast<float*>(player + kVelocity);
        const auto game = At<std::uintptr_t>(base + kGame);
        if (!game) return;
        const auto room = At<std::uintptr_t>(game + kRoom); const auto roomIndex = At<std::uint32_t>(game + kRoomIndex);
        if (controller == ownController) {
            ++frame;
            published.generation++;   // odd: being written
            published.body.magic = kBodyMagic; published.body.controller = static_cast<std::uint32_t>(controller); published.body.sequence = ++sequence;
            published.body.room = roomIndex;
            std::memcpy(published.body.position, position, 8); std::memcpy(published.body.velocity, velocity, 8);
            published.generation++;
            stats.published++;
            if (room) { if (host) PublishWorld(room, roomIndex); else ApplyWorld(room, roomIndex); }
            return;
        }
        if (controller < 0 || controller >= kControllers) return;
        Body body{}; bool fresh = false;
        AcquireSRWLockShared(&inboxLock);
        if (inbox[controller].body.sequence && inbox[controller].body.sequence != inbox[controller].appliedSequence) {
            fresh = GetTickCount64() - inbox[controller].at <= kFreshMs; body = inbox[controller].body;
        }
        ReleaseSRWLockShared(&inboxLock);
        if (!body.sequence) return;
        if (!fresh) { stats.stale++; return; }
        // A body of another room is not a position in this one: around a door the two games change rooms a moment apart.
        if (body.room != roomIndex) { stats.otherRoom++; return; }
        const float dx = body.position[0] - position[0], dy = body.position[1] - position[1], distance = std::sqrt(dx * dx + dy * dy);
        std::memcpy(position, body.position, 8); std::memcpy(velocity, body.velocity, 8);
        AcquireSRWLockExclusive(&inboxLock); inbox[controller].appliedSequence = body.sequence; ReleaseSRWLockExclusive(&inboxLock);
        stats.applied++; stats.correctionSum += distance; if (distance > stats.correctionMax) stats.correctionMax = distance;
    } __except (EXCEPTION_EXECUTE_HANDLER) { stats.rejected++; }
}

void __fastcall OnPlayer(void* object, void*) {
    originalPlayer(object);
    if (running.load(std::memory_order_acquire)) AfterUpdate(reinterpret_cast<std::uintptr_t>(object));
}

char __fastcall OnNpcDamage(void* self, void*, float damage, std::uint32_t flagsLow, std::uint32_t flagsHigh, void* source, int countdown) {
    if (running.load(std::memory_order_acquire) && !host) {
        const auto entity = reinterpret_cast<std::uintptr_t>(self); const float hitPoints = At<float>(entity + kHitPoints);
        if (At<float>(entity + kMaxHitPoints) > 0 && damage >= hitPoints) { damage = hitPoints > 0.02f ? hitPoints - 0.01f : 0.0f; stats.deathsHeld++; }
    }
    return originalDamage(self, damage, flagsLow, flagsHigh, source, countdown);
}

bool ValidWorld(const World& world, int got) {
    if (got != sizeof(World) || world.magic != kWorldMagic || !world.sequence || world.count > kMaxNpcs || world.deaths > kMaxDeaths) return false;
    for (std::uint32_t n = 0; n < world.count; ++n)
        if (!Finite(world.npcs[n].position) || !Finite(world.npcs[n].velocity) || !std::isfinite(world.npcs[n].hitPoints)) return false;
    return true;
}

DWORD WINAPI Receive(void*) noexcept {
    static char packet[sizeof(World)];
    while (running.load(std::memory_order_acquire)) {
        fd_set set; FD_ZERO(&set); FD_SET(udp, &set); timeval wait{0, 50000};
        if (select(0, &set, nullptr, nullptr, &wait) <= 0) continue;
        const int got = recv(udp, packet, sizeof(packet), 0);
        if (got == sizeof(Body)) {
            Body body; std::memcpy(&body, packet, sizeof(body));
            const bool valid = body.magic == kBodyMagic && body.controller < kControllers && body.sequence &&
                static_cast<int>(body.controller) != ownController && Finite(body.position) && Finite(body.velocity);
            if (!valid) { stats.rejected++; continue; }
            AcquireSRWLockExclusive(&inboxLock);
            if (body.sequence > inbox[body.controller].body.sequence) { inbox[body.controller].body = body; inbox[body.controller].at = GetTickCount64(); stats.received++; }
            ReleaseSRWLockExclusive(&inboxLock);
        } else if (!host && ValidWorld(*reinterpret_cast<const World*>(packet), got)) {   // the host takes nobody's world
            AcquireSRWLockExclusive(&inboxLock);
            if (reinterpret_cast<const World*>(packet)->sequence > worldInbox.sequence) { std::memcpy(&worldInbox, packet, sizeof(World)); worldAt = GetTickCount64(); stats.worldReceived++; }
            ReleaseSRWLockExclusive(&inboxLock);
        } else stats.rejected++;
    }
    return 0;
}

void CloseNetwork() {
    if (worker) { WaitForSingleObject(worker, 2000); CloseHandle(worker); worker = nullptr; }
    if (udp != INVALID_SOCKET) { closesocket(udp); udp = INVALID_SOCKET; }
    if (winsock) { WSACleanup(); winsock = false; }
}

DWORD Patch(std::uintptr_t rva, const std::array<std::uint8_t, 8>& bytes, const std::array<std::uint8_t, 8>& expected) {
    auto* at = reinterpret_cast<std::uint8_t*>(base + rva); DWORD old = 0;
    if (!VirtualProtect(at, bytes.size(), PAGE_EXECUTE_READWRITE, &old)) return GetLastError();
    // Already in place counts as done: an earlier start of this module leaves the comparison off when it stops.
    const bool matches = std::memcmp(at, expected.data(), expected.size()) == 0 || std::memcmp(at, bytes.data(), bytes.size()) == 0;
    if (matches) std::memcpy(at, bytes.data(), bytes.size());
    DWORD ignored = 0; const BOOL restored = VirtualProtect(at, bytes.size(), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), at, bytes.size());
    return !matches ? ERROR_REVISION_MISMATCH : restored ? ERROR_SUCCESS : GetLastError();
}

std::filesystem::path Folder() {
    wchar_t local[32768]{}; const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
    if (!n || n >= 32768) throw static_cast<DWORD>(ERROR_ENVVAR_NOT_FOUND);
    return std::filesystem::path(local) / L"IsaacAuthority";
}
}

extern "C" DWORD WINAPI IsaacAuthorityNativeStart(void*) noexcept {
    AcquireSRWLockExclusive(&lifecycle);
    DWORD result = ERROR_INVALID_DATA;
    try {
        if (running.load()) throw static_cast<DWORD>(ERROR_ALREADY_INITIALIZED);
        wchar_t image[32768]{};
        if (!GetModuleFileNameW(nullptr, image, 32768) || !isaac_probe::AnalyzeBytes(isaac_probe::ReadFile(image)).supported) throw static_cast<DWORD>(ERROR_BAD_EXE_FORMAT);
        base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (std::memcmp(reinterpret_cast<const char*>(base + kSaveLeaf), kIsolated, sizeof(kIsolated) - 1) != 0) throw static_cast<DWORD>(ERROR_ACCESS_DENIED);
        const auto folder = Folder();
        std::ifstream config(folder / (L"native-" + std::to_wstring(GetCurrentProcessId()) + L".cfg"));
        int controller = -1; std::string role;
        if (!(config >> controller) || controller < 1 || controller >= kControllers) throw static_cast<DWORD>(ERROR_BAD_CONFIGURATION);
        config >> role;
        const auto table = At<std::uintptr_t>(base + kManager);
        slot = reinterpret_cast<void**>(base + kManagerTable + 29 * sizeof(void*));
        playerSlot = reinterpret_cast<void**>(base + kPlayerTable + 3 * sizeof(void*));
        damageSlot = reinterpret_cast<void**>(base + kNpcTable + 8 * sizeof(void*));
        if (table != base + kManagerTable || *slot != reinterpret_cast<void*>(base + kWithDevice) || *playerSlot != reinterpret_cast<void*>(base + kPlayerUpdate) ||
            *damageSlot != reinterpret_cast<void*>(base + kNpcDamage) ||
            std::memcmp(reinterpret_cast<void*>(base + kKill), kKillEntry.data(), kKillEntry.size()) != 0) throw static_cast<DWORD>(ERROR_REVISION_MISMATCH);
        original = reinterpret_cast<WithDevice>(*slot); originalPlayer = reinterpret_cast<PlayerUpdate>(*playerSlot); originalDamage = reinterpret_cast<NpcDamage>(*damageSlot);
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&OnInput), &pinned)) throw GetLastError();
        ownController = controller; host = role == "host"; counters[0] = 0; counters[1] = 0; stats = Stats{}; sequence = worldSequence = frame = 0;
        published = Published{}; published.magic = kBodyMagic; publishedWorld = PublishedWorld{}; publishedWorld.magic = kWorldMagic;
        for (auto& box : inbox) box = Inbox{};
        worldInbox = World{}; worldAt = 0; worldApplied = 0; livedRoom = aliasRoom = 0xffffffff; livedCount = deathCount = aliasCount = orphanCount = 0;
        WSADATA data{};
        if (const int started = WSAStartup(MAKEWORD(2, 2), &data)) throw static_cast<DWORD>(started);
        winsock = true; udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); int size = sizeof(address);
        if (udp == INVALID_SOCKET || bind(udp, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || getsockname(udp, reinterpret_cast<sockaddr*>(&address), &size)) {
            const auto failure = static_cast<DWORD>(WSAGetLastError()); CloseNetwork(); throw failure;
        }
        port = ntohs(address.sin_port);
        if (const DWORD failure = Patch(kCompare, kCompareEqual, kCompareEntry)) { CloseNetwork(); throw failure; }
        running.store(true, std::memory_order_release);
        worker = CreateThread(nullptr, 0, &Receive, nullptr, 0, nullptr);
        DWORD failure = worker ? ExchangeSlot(slot, reinterpret_cast<void*>(original), reinterpret_cast<void*>(&OnInput)) : GetLastError();
        if (!failure) {
            failure = ExchangeSlot(playerSlot, reinterpret_cast<void*>(originalPlayer), reinterpret_cast<void*>(&OnPlayer));
            if (failure) ExchangeSlot(slot, reinterpret_cast<void*>(&OnInput), reinterpret_cast<void*>(original));
        }
        if (!failure) {
            failure = ExchangeSlot(damageSlot, reinterpret_cast<void*>(originalDamage), reinterpret_cast<void*>(&OnNpcDamage));
            if (failure) {
                ExchangeSlot(slot, reinterpret_cast<void*>(&OnInput), reinterpret_cast<void*>(original));
                ExchangeSlot(playerSlot, reinterpret_cast<void*>(&OnPlayer), reinterpret_cast<void*>(originalPlayer));
            }
        }
        if (failure) { running = false; CloseNetwork(); Patch(kCompare, kCompareEntry, kCompareEqual); throw failure; }
        std::ofstream descriptor(folder / (L"native-" + std::to_wstring(GetCurrentProcessId()) + L".json"), std::ios::trunc);
        descriptor << "{\"pid\":" << GetCurrentProcessId() << ",\"role\":\"" << (host ? "native-host" : "native-guest") << "\",\"ownController\":" << ownController
                   << ",\"counters\":" << reinterpret_cast<std::uintptr_t>(&counters) << ",\"port\":" << port
                   << ",\"published\":" << reinterpret_cast<std::uintptr_t>(&published) << ",\"publishedBytes\":" << sizeof(published)
                   << ",\"publishedWorld\":" << reinterpret_cast<std::uintptr_t>(&publishedWorld) << ",\"publishedWorldBytes\":" << sizeof(publishedWorld)
                   << ",\"stats\":" << reinterpret_cast<std::uintptr_t>(&stats) << ",\"statsBytes\":" << sizeof(stats) << "}\n";
        result = ERROR_SUCCESS;
    } catch (DWORD failure) { result = failure ? failure : ERROR_INVALID_DATA; } catch (...) { result = ERROR_INVALID_DATA; }
    ReleaseSRWLockExclusive(&lifecycle);
    return result;
}

extern "C" DWORD WINAPI IsaacAuthorityNativeStop(void*) noexcept {
    AcquireSRWLockExclusive(&lifecycle);
    DWORD result = ERROR_NOT_READY;
    if (running.load()) {
        running.store(false, std::memory_order_release);
        result = ExchangeSlot(slot, reinterpret_cast<void*>(&OnInput), reinterpret_cast<void*>(original));
        const DWORD second = ExchangeSlot(playerSlot, reinterpret_cast<void*>(&OnPlayer), reinterpret_cast<void*>(originalPlayer));
        const DWORD third = ExchangeSlot(damageSlot, reinterpret_cast<void*>(&OnNpcDamage), reinterpret_cast<void*>(originalDamage));
        if (!result) result = second ? second : third;
        CloseNetwork();
        // The comparison stays off on purpose: the games have already diverged, and turning it back on would split the lobby.
    }
    ReleaseSRWLockExclusive(&lifecycle);
    return result;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
