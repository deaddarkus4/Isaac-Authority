#include "world_receiver.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace authority;
using namespace authority::world;
namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
Frame Example() {
    Frame f; f.session = 123; f.sequence = 1; f.timeMs = 1000;
    f.room = {1, 0, 84, 0, 1, 2, 1, 12345, 1}; f.players[0].body = {{320, 280}, {0, 0}};
    f.count = 1; f.entities[0] = {1, 99, 2, 0, 0, {{350, 280}, {10, 0}}, -20, -1, 0.5f, 1}; return f;
}
void Protocol() {
    auto f = Example(); Packet packet; Frame out;
    Check(Encode(f, packet) && packet.size == kHeader + kRecord, "encode one entity");
    Check(Decode(packet.bytes.data(), packet.size, out) && out.entities[0].id == 1 && out.entities[0].height == -20,
        "world wire roundtrip");
    for (std::size_t n = 0; n < packet.size; ++n) Check(!Decode(packet.bytes.data(), n, out), "reject truncated snapshot");
    for (auto at : {0, 4, 88, 92, 148}) {
        auto bad = packet; bad.bytes[at] ^= 0x55;
        Check(!Decode(bad.bytes.data(), bad.size, out), "reject header and reserved fields");
    }
    f.count = 17; Check(!Encode(f, packet), "reject oversized roster without truncation");
    f = Example(); f.count = 2; f.entities[1] = f.entities[0]; Check(!Valid(f), "duplicate network id");
    f = Example(); f.entities[0].body.position.x = std::numeric_limits<float>::quiet_NaN(); Check(!Valid(f), "NaN position");
    f = Example(); f.entities[0].type = 5; Check(!Valid(f), "do not spawn arbitrary entity types");
    f = Example(); f.entities[0].variant = 42; Check(!Valid(f), "unsupported tear variant");
    f = Example(); Gate gate(123);
    Check(gate.Receive(f, 999) == Decision::Reject && gate.Receive(f, 1251) == Decision::Reject, "timestamp bounds");
    Check(gate.Receive(f, 1000) == Decision::Accept && gate.Receive(f, 1000) == Decision::Reject, "fresh then duplicate");
    auto room = f; room.sequence = 2; ++room.room.visits;
    Check(gate.Receive(room, 1000) == Decision::Reject, "room identity cannot mutate within epoch");
    ++room.epoch; Check(gate.Receive(room, 1000) == Decision::RoomChanged, "explicit room epoch invalidation");
    f.sequence = 999; Check(gate.Receive(f, 1000) == Decision::Reject, "late old-room snapshot rejected");
    room.session = 124; Check(gate.Receive(room, 1000) == Decision::Reject, "foreign session");
}
void Lifecycle() {
    Ids ids; Identity keys[]{{0x10000, 42, 1}, {0x20000, 43, 2}}; std::uint32_t out[2]{};
    Check(ids.Assign(keys, 2, out) && out[0] != out[1], "assign two stable network ids");
    const auto first = out[0], second = out[1]; std::swap(keys[0], keys[1]);
    Check(ids.Assign(keys, 2, out) && out[0] == second && out[1] == first, "list reordering preserves ids");
    keys[1].index = 3;
    Check(ids.Assign(keys, 2, out) && out[1] != first, "recycled address and seed with new native index gets new id");
    const auto prior = out[1]; Check(ids.Assign(nullptr, 0, nullptr), "empty room retires entities");
    Check(ids.Assign(keys, 2, out) && out[1] != prior, "removed entity cannot resurrect its old id");
    keys[1] = keys[0]; Check(!ids.Assign(keys, 2, out), "duplicate native identity rejected");
    auto before = Example(); auto after = before; after.sequence = 2;
    after.entities[0].id = 2; after.entities[0].seed = 100; Delta delta;
    Check(Plan(before, after, delta) && delta.creates == 1 && delta.removes == 1 && !delta.updates,
        "full snapshot creates new and retires missing entity");
    after = before; ++after.sequence; after.entities[0].body.position.x += 10;
    Check(Plan(before, after, delta) && delta.updates == 1 && !delta.creates && !delta.removes, "update preserves entity lifetime");
    ++after.entities[0].seed; Check(!Plan(before, after, delta), "same network id cannot change incarnation");
    after = before; ++after.sequence; after.count = 0;
    Check(Plan(before, after, delta) && delta.removes == 1, "empty complete snapshot removes last entity");
    ++after.epoch; Check(!Plan(before, after, delta), "room change requires separate teardown");
}
void Rooms() {
    const auto host = Example(); RoomKey local = host.room;
    local.visits = 7; Check(SamePlace(local, host.room) && !SameRoom(local, host.room), "visit history is local to each game");
    for (int field = 0; field < 8; ++field) {
        auto other = host.room;
        std::uint32_t* fields[]{&other.stage, &other.stageType, &other.index, &other.dimension, &other.type, &other.variant, &other.shape, &other.spawnSeed};
        ++*fields[field]; Check(!SamePlace(other, host.room), "every generated room property identifies the place");
    }
    Check(Decide(local, nullptr, host) == Route::Apply, "first snapshot of the shared room applies");
    auto next = host; ++next.sequence; Check(Decide(local, &host, next) == Route::Apply, "same host generation continues");
    ++next.epoch; ++next.room.visits; Check(Decide(local, &host, next) == Route::Restart, "host re-entered this room: old copies are retired");
    next = host; ++next.epoch; next.room.index = 85; next.room.variant = 300; next.room.spawnSeed = 777;
    Check(Decide(local, &host, next) == Route::Travel && Decide(local, nullptr, next) == Route::Travel, "host in another room requires travel");
    next = host; ++next.room.spawnSeed; Check(Decide(local, nullptr, next) == Route::Travel, "different run seed is never applied in place");
    RoomKey from = host.room, to = host.room;
    to.index = 85; Check(Direction(from, to) == 2 && Direction(to, from) == 0, "horizontal neighbours");
    to.index = 97; Check(Direction(from, to) == 3 && Direction(to, from) == 1, "vertical neighbours");
    from.index = 90; to.index = 91; Check(Direction(from, to) == -1 && Direction(to, from) == -1, "grid rows do not wrap");
    from.index = 84; to.index = 86; Check(Direction(from, to) == -1, "distant rooms have no door direction");
    to.index = 85; to.shape = 4; Check(Direction(from, to) == -1, "large rooms are entered without a guessed door");
    to.shape = 1; to.dimension = 1; Check(Direction(from, to) == -1, "dimensions are not adjacent");
}
void Enemies() {
    auto f = Example(); Packet tearsOnly, packet; Frame out;
    Check(Encode(f, tearsOnly) && tearsOnly.size == kHeader + kRecord && tearsOnly.bytes[4] == 1, "a frame without enemies stays version 1");
    f.npcCount = 2;
    f.npcs[0] = {244, 0, 0, 2078110152u, {{80, 160}, {0, 0}}, {80, 160}, 10, 10, 8, 1, 5, 4};
    f.npcs[1] = {244, 0, 0, 2403336305u, {{560, 160}, {0.5f, -0.25f}}, {0, 0}, 7.5f, 10, 4, 0, 5, 0};
    Check(Encode(f, packet) && packet.size == kHeader + kRecord + 2 * kNpcRecord && packet.bytes[4] == 2, "enemy section makes version 2");
    Check(std::equal(tearsOnly.bytes.begin() + 8, tearsOnly.bytes.begin() + 88, packet.bytes.begin() + 8), "header and tear layout unchanged");
    Check(Decode(packet.bytes.data(), packet.size, out) && out.npcCount == 2 && out.count == 1 && out.entities[0].id == 1 &&
        SameNpc(out.npcs[1], f.npcs[1]) && out.npcs[1].hitPoints == 7.5f && out.npcs[1].state == 4 && !out.npcs[1].visible &&
        out.npcs[1].body.velocity.y == -0.25f && out.npcs[0].entityCollision == 4 && out.npcs[0].target.y == 160, "enemy wire roundtrip");
    for (std::size_t n = 0; n < packet.size; ++n) Check(!Decode(packet.bytes.data(), n, out), "reject truncated enemy section");
    auto bad = packet; bad.bytes[4] = 1; Check(!Decode(bad.bytes.data(), bad.size, out), "version 1 cannot carry enemies");
    bad = packet; bad.bytes[88] = 3; Check(!Decode(bad.bytes.data(), bad.size, out), "enemy count must match the size");
    bad = tearsOnly; bad.bytes[4] = 2; Check(!Decode(bad.bytes.data(), bad.size, out), "version 2 needs an enemy section");
    auto g = f; g.npcs[1].seed = g.npcs[0].seed; Check(!Valid(g), "duplicate enemy identity");
    g = f; g.npcs[0].type = 2; Check(!Valid(g), "a tear is not an enemy");
    g = f; g.npcs[0].type = 1000; Check(!Valid(g), "effects are local cosmetics");
    g = f; g.npcs[0].seed = 0; Check(!Valid(g), "enemy identity needs a seed");
    g = f; g.npcs[0].hitPoints = std::numeric_limits<float>::infinity(); Check(!Valid(g), "non-finite hit points");
    g = f; g.npcs[0].visible = 2; Check(!Valid(g), "visibility is a flag");
    g = f; g.npcCount = static_cast<std::uint32_t>(kMaxNpcs + 1); Check(!Encode(g, packet), "reject oversized enemy roster without truncation");
    g = f; g.npcs[1].variant = 1; Check(!SameNpc(g.npcs[1], f.npcs[1]) && SameNpc(g.npcs[0], f.npcs[0]), "identity is type, variant, subtype and seed");
    Gate gate(123); Check(gate.Receive(f, 1000) == Decision::Accept, "enemy frames pass the same gate");
    auto next = f; ++next.sequence; next.npcCount = 1; // The second worm died on the host.
    Check(Departed(&f, next, f.npcs[1]) && !Departed(&f, next, f.npcs[0]), "an enemy the host stops listing has died there");
    Check(!Departed(nullptr, next, f.npcs[1]), "nothing dies before a first snapshot");
    auto stranger = f.npcs[1]; ++stranger.seed; Check(!Departed(&f, next, stranger), "an enemy the host never listed is not killed");
    auto elsewhere = next; ++elsewhere.epoch; Check(!Departed(&f, elsewhere, f.npcs[1]), "another room generation proves no death");
    elsewhere = next; ++elsewhere.room.visits; Check(!Departed(&f, elsewhere, f.npcs[1]), "another visit proves no death");
    elsewhere = next; ++elsewhere.session; Check(!Departed(&f, elsewhere, f.npcs[1]), "another session proves no death");
}
void Players() {
    auto f = Example(); Packet alone, packet; Frame out;
    Check(Encode(f, alone) && alone.bytes[4] == 1 && !alone.bytes[92], "one player on controller 0 stays version 1");
    f.playerCount = 2; f.players[1] = {1, {{200, 300}, {-4.5f, 0.25f}}};
    Check(Encode(f, packet) && packet.size == alone.size + 2 * kPlayerRecord && packet.bytes[4] == 3 && packet.bytes[92] == 2,
        "a second player makes version 3");
    Check(std::equal(alone.bytes.begin() + 8, alone.bytes.begin() + 92, packet.bytes.begin() + 8) &&
        std::equal(alone.bytes.begin() + kHeader, alone.bytes.begin() + alone.size, packet.bytes.begin() + kHeader),
        "header, first player and tear layout unchanged");
    Check(Decode(packet.bytes.data(), packet.size, out) && out.playerCount == 2 && out.players[0].controller == 0 &&
        out.players[0].body.position.x == 320 && out.players[1].controller == 1 && out.players[1].body.position.y == 300 &&
        out.players[1].body.velocity.x == -4.5f && out.count == 1 && out.entities[0].id == 1, "player wire roundtrip");
    for (std::size_t n = 0; n < packet.size; ++n) Check(!Decode(packet.bytes.data(), n, out), "reject truncated player section");
    auto bad = packet; bad.bytes[4] = 1; Check(!Decode(bad.bytes.data(), bad.size, out), "version 1 cannot list players");
    bad = packet; bad.bytes[92] = 3; Check(!Decode(bad.bytes.data(), bad.size, out), "player count must match the size");
    bad = packet; bad.bytes[92] = 0; Check(!Decode(bad.bytes.data(), bad.size, out), "version 3 needs a player section");
    bad = packet; bad.bytes[packet.size - 4] = 1; Check(!Decode(bad.bytes.data(), bad.size, out), "reserved player field");
    bad = packet; bad.bytes[68] ^= 1; Check(!Decode(bad.bytes.data(), bad.size, out), "header and first record must agree on the first player");
    bad = packet; bad.bytes[packet.size - kPlayerRecord] = 8; Check(!Decode(bad.bytes.data(), bad.size, out), "controller out of range");
    auto enemies = f; enemies.npcCount = 1; enemies.npcs[0] = {244, 0, 0, 2078110152u, {{80, 160}, {0, 0}}, {80, 160}, 10, 10, 8, 1, 5, 4};
    Check(Encode(enemies, packet) && packet.bytes[4] == 3 && packet.size == alone.size + kNpcRecord + 2 * kPlayerRecord &&
        Decode(packet.bytes.data(), packet.size, out) && out.npcCount == 1 && out.playerCount == 2 && out.players[1].controller == 1,
        "players follow the enemy section");
    auto g = Example(); g.players[0].controller = 1;
    Check(Encode(g, packet) && packet.bytes[4] == 3 && packet.bytes[92] == 1 && Decode(packet.bytes.data(), packet.size, out) &&
        out.playerCount == 1 && out.players[0].controller == 1, "a lone player on another controller is listed");
    bad = packet; bad.bytes[packet.size - kPlayerRecord] = 0;
    Check(!Decode(bad.bytes.data(), bad.size, out), "one player on controller 0 has only the version 1 spelling");
    g = f; g.players[1].controller = 0; Check(Valid(g), "two players may share a controller");
    g = f; g.playerCount = 0; Check(!Valid(g), "a run has a player");
    g = f; g.playerCount = static_cast<std::uint32_t>(kMaxPlayers + 1); Check(!Encode(g, packet), "reject oversized team without truncation");
    g = f; g.players[1].body.position.x = std::numeric_limits<float>::quiet_NaN(); Check(!Valid(g), "every player body is checked");
    Gate gate(123); Check(gate.Receive(f, 1000) == Decision::Accept, "player frames pass the same gate");
    Check(kMaxBytes == 3136, "slot payload bound");
}
void Contract(const wchar_t* path) {
    HMODULE dll = LoadLibraryW(path); Check(dll != nullptr, "load world adapter");
    using Export = DWORD(WINAPI*)(void*);
    auto start = reinterpret_cast<Export>(GetProcAddress(dll, "IsaacAuthorityWorldStart"));
    auto stop = reinterpret_cast<Export>(GetProcAddress(dll, "IsaacAuthorityWorldStop"));
    Check(start && stop && start(nullptr) == ERROR_BAD_EXE_FORMAT && stop(nullptr) == ERROR_NOT_READY,
        "world adapter refuses unsupported host and inactive stop");
    FreeLibrary(dll);
}
int Fixture() {
    Receiver receiver; Check(receiver.Start() == 0, "world fixture receiver");
    std::cout << "{\"port\":" << receiver.Port() << ",\"session\":\"" << receiver.Session() << "\"}" << std::endl;
    Frame previous; unsigned creates = 0, removes = 0, updates = 0, roomChanges = 0;
    const auto until = GetTickCount64() + 2500;
    while (GetTickCount64() < until) {
        Frame frame;
        if (receiver.Take(frame)) {
            Delta delta;
            if (Plan(previous, frame, delta)) { creates += static_cast<unsigned>(delta.creates); removes += static_cast<unsigned>(delta.removes); updates += static_cast<unsigned>(delta.updates); previous = frame; }
            else { ++roomChanges; previous = {}; }
        }
        Sleep(1);
    }
    receiver.Stop();
    std::cout << "{\"creates\":" << creates << ",\"removes\":" << removes << ",\"updates\":" << updates
        << ",\"roomChanges\":" << roomChanges << ",\"accepted\":" << receiver.accepted.load()
        << ",\"rejected\":" << receiver.rejected.load() << ",\"errors\":" << receiver.errors.load() << "}\n";
    return 0;
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 2 && std::wstring(argv[1]) == L"--fixture") return Fixture();
        Protocol(); Lifecycle(); Rooms(); Enemies(); Players();
        for (int i = 1; i < argc; ++i) Contract(argv[i]);
        std::cout << "PASS world wire, stable identities, lifetimes, epochs, room routing, enemy and player sections, module contracts\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
