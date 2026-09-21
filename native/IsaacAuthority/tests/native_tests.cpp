#include <windows.h>
#include "native_state.hpp"
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>
using namespace authority::native;
namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
Shot Tear() { Shot s{}; s.seed = 7; s.position[0] = 320; s.position[1] = 280; s.velocity[0] = 10; s.height = -23.5f; s.fallingSpeed = 0.5f; s.fallingAccel = 0.1f; s.scale = 1; s.damage = 3.5f; s.color[0] = 1; return s; }
std::unique_ptr<World> Room() {
    auto world = std::make_unique<World>(); *world = World{};
    world->magic = kWorldMagic; world->sequence = 5; world->room = 84; world->count = 2; world->npcTotal = 2; world->deaths = 1; world->shots = 1; world->drops = 1; world->doors = 1; world->born = 1;
    world->npcs[0].seed = 11; world->npcs[0].hitPoints = 10; std::memcpy(world->npcs[0].animation, "WalkVert", 9); world->npcs[1].seed = 12; world->npcs[1].position[0] = 100;
    world->died[0] = 9; world->shot[0] = Tear(); world->drop[0].seed = 31; world->drop[0].variant = 20; world->door[0].cell = 7; world->door[0].state = 2;
    world->bornCell[0].index = 40; world->bornCell[0].type = 14; world->bornCell[0].state = 250; world->gridMap[5] = 0x80; world->coins = 15;
    return world;
}

void Packing() {
    const auto world = Room(); std::vector<std::uint8_t> packed(sizeof(World)); auto back = std::make_unique<World>();
    const auto size = PackWorld(*world, packed.data());
    Check(size == offsetof(World, npcs) + 2 * sizeof(Npc) + 4 + sizeof(Shot) + sizeof(Drop) + sizeof(DoorState) + sizeof(Born) + kGridMapBytes, "only the entries in use travel");
    Check(UnpackWorld(packed.data(), size, *back) && std::memcmp(world.get(), back.get(), sizeof(World)) == 0 && ValidWorld(*back), "a world comes back as it went");
    for (std::uint32_t n = 0; n < size; n += 7) Check(!UnpackWorld(packed.data(), n, *back), "a world cut short is no world");
    packed.resize(size + 1); Check(!UnpackWorld(packed.data(), size + 1, *back), "nor one with something behind it");
    auto many = Room(); many->count = kMaxNpcs + 1; std::vector<std::uint8_t> wide(sizeof(World) + sizeof(Npc));
    std::memcpy(wide.data(), many.get(), offsetof(World, npcs));
    Check(!UnpackWorld(wide.data(), static_cast<std::uint32_t>(wide.size()), *back), "a count beyond the list is refused before anything is copied");
    auto shots = std::make_unique<Shots>(); *shots = Shots{}; shots->magic = kShotsMagic; shots->controller = 3; shots->sequence = 9; shots->count = 2; shots->bombs = 1; shots->pets = 1;
    shots->shot[0] = Tear(); shots->shot[1] = Tear(); shots->shot[1].seed = 8; shots->shot[2] = Tear(); shots->shot[2].height = 39; shots->pet[0].seed = 77;
    auto shotsBack = std::make_unique<Shots>(); std::vector<std::uint8_t> carried(sizeof(Shots));
    const auto shotsSize = PackShots(*shots, carried.data());
    Check(shotsSize == offsetof(Shots, shot) + 3 * sizeof(Shot) + sizeof(Pet) && UnpackShots(carried.data(), shotsSize, *shotsBack) && std::memcmp(shots.get(), shotsBack.get(), sizeof(Shots)) == 0,
        "tears, bombs and pets come back as they went");
    shots->count = kMaxTears; shots->bombs = 1; std::memcpy(carried.data(), shots.get(), offsetof(Shots, shot));
    Check(!UnpackShots(carried.data(), static_cast<std::uint32_t>(carried.size()), *shotsBack), "tears and bombs share one list: together they must fit it");
}

void Ranges() {
    Check(ValidShot(Tear()), "a plain tear");
    auto s = Tear(); s.height = 1.0e6f; Check(ValidShot(s), "a bomb carries its frames to the explosion as the height, and they may be very many");
    s = Tear(); s.fallingSpeed = 1.0e6f; Check(!ValidShot(s), "a falling speed no shot has (this check was once lost inside a comment)");
    s = Tear(); s.fallingAccel = -1000.0f; Check(!ValidShot(s), "a falling acceleration no shot has");
    s = Tear(); s.color[10] = std::numeric_limits<float>::quiet_NaN(); Check(!ValidShot(s), "a colour that is no number");
    s = Tear(); s.position[1] = std::numeric_limits<float>::infinity(); Check(!ValidShot(s), "a place that is no number");
    s = Tear(); s.scale = -1; Check(!ValidShot(s), "a negative size");
    s = Tear(); s.damage = 1.0e6f; Check(!ValidShot(s), "damage beyond any shot's");
    auto world = Room(); Check(ValidWorld(*world), "the example room");
    world->shot[0].fallingSpeed = 1.0e6f; Check(!ValidWorld(*world), "one wild shot spoils the whole world: the sender reads another layout");
    world = Room(); std::memset(world->npcs[0].animation, 'A', kAnimationName); Check(!ValidWorld(*world), "an animation's name must end");
    world = Room(); world->npcs[1].hitPoints = std::numeric_limits<float>::quiet_NaN(); Check(!ValidWorld(*world), "hit points that are no number");
    world = Room(); world->npcs[0].entityCollision = kEntityCollisionClasses; Check(!ValidWorld(*world), "a collision class the game does not have");
    world = Room(); world->npcs[1].gridCollision = 3; world->npcs[1].entityCollision = 4; world->npcs[0].renderZ = -1000; Check(ValidWorld(*world), "a fireplace that burns beside one that has gone out: no collisions, drawn under everything");
    world = Room(); world->sequence = 0; Check(!ValidWorld(*world), "sequences start at one");
    world = Room(); world->doors = kMaxDoors + 1; Check(!ValidWorld(*world), "more doors than the list holds");
}

FrameHeader Header(std::uint32_t sequence, std::uint32_t total, std::uint8_t chunk) {
    return FrameHeader{kFrameMagic, kOfWorld, chunk, static_cast<std::uint8_t>((total + kChunkBytes - 1) / kChunkBytes), 0, sequence, total, 1};
}
std::uint32_t Part(std::uint32_t total, std::uint8_t chunk) { const auto from = chunk * kChunkBytes; return total - from < kChunkBytes ? total - from : kChunkBytes; }

void Frames() {
    const std::uint32_t total = 2 * kChunkBytes + 100; std::vector<std::uint8_t> whole(total); for (std::uint32_t n = 0; n < total; ++n) whole[n] = static_cast<std::uint8_t>(n * 31 + 7);
    auto assembly = std::make_unique<Assembly>(); bool broke = false; const auto most = static_cast<std::uint32_t>(sizeof(World));
    const auto put = [&](std::uint32_t sequence, std::uint8_t chunk) { return Gather(*assembly, Header(sequence, total, chunk), whole.data() + chunk * kChunkBytes, Part(total, chunk), most, broke); };
    Check(put(1, 0) == Piece::Kept && put(1, 1) == Piece::Kept && put(1, 2) == Piece::Whole && !broke && assembly->total == total && std::memcmp(assembly->data, whole.data(), total) == 0,
        "three datagrams in order make the frame");
    Check(put(1, 2) == Piece::Kept && put(1, 0) == Piece::Kept && put(1, 1) == Piece::Kept, "a whole is reported once: its late doubles start nothing");
    Check(put(3, 2) == Piece::Kept && put(3, 0) == Piece::Kept && put(2, 1) == Piece::Kept && put(3, 1) == Piece::Whole && !broke, "any order; a piece of an overtaken sequence changes nothing");
    Check(put(4, 0) == Piece::Kept && put(5, 0) == Piece::Kept && broke, "a newer sequence gives up the unfinished one, and that is counted");
    Check(put(5, 1) == Piece::Kept && !broke && put(5, 2) == Piece::Whole, "and is put together itself");
    auto bad = Header(6, total, 0);
    bad.chunk = 3; Check(Gather(*assembly, bad, whole.data(), kChunkBytes, most, broke) == Piece::Refused, "a chunk beyond the last");
    bad = Header(6, total, 0); bad.chunks = kMaxChunks + 1; Check(Gather(*assembly, bad, whole.data(), kChunkBytes, most, broke) == Piece::Refused, "more chunks than any frame has");
    bad = Header(6, total, 0); bad.total = most + 1; Check(Gather(*assembly, bad, whole.data(), kChunkBytes, most, broke) == Piece::Refused, "larger than the largest whole of its kind");
    bad = Header(6, total, 0); bad.total = 0; Check(Gather(*assembly, bad, whole.data(), kChunkBytes, most, broke) == Piece::Refused, "an empty frame");
    Check(Gather(*assembly, Header(6, total, 0), whole.data(), kChunkBytes - 1, most, broke) == Piece::Refused, "a middle chunk is a full one");
    Check(Gather(*assembly, Header(6, total, 2), whole.data(), 99, most, broke) == Piece::Refused && Gather(*assembly, Header(6, total, 2), whole.data(), 101, most, broke) == Piece::Refused,
        "the last chunk ends exactly where the frame does");
    Check(Gather(*assembly, Header(6, total, 0), whole.data(), kChunkBytes + 1, most, broke) == Piece::Refused, "no chunk is larger than a chunk");
    Check(assembly->sequence == 5, "nothing refused has touched the assembly");
    assembly->Reset(); Check(put(1, 0) == Piece::Kept && put(1, 1) == Piece::Kept && put(1, 2) == Piece::Whole, "after a reset the neighbour's count from 1 is taken again");
}

void Sessions() {
    Check(NextSession(0, 1000) == 1000 && NextSession(1000, 1005) == 1005, "a start is numbered by the clock");
    Check(NextSession(1000, 1000) == 1001 && NextSession(1000, 900) == 1001, "and always higher than the start before it, whatever the clock says");
    Check(NextSession(0, 0) == 1 && NextSession(0, 0x100000000ull) == 1 && NextSession(0xffffffffu, 5) == 5, "never 0: that means no start heard yet");
    Check(NewerSession(0, 1000, 0), "the first start heard of a neighbour");
    Check(!NewerSession(1000, 1000, 100000) && !NewerSession(1000, 0, 100000), "the same start is not a new one, and 0 is none");
    Check(NewerSession(1000, 1001, 0), "the neighbour has started anew");
    Check(!NewerSession(1001, 1000, 500), "a straggler of the start it left behind");
    Check(NewerSession(1001, 1000, kSessionSilenceMs + 1), "unless the newer one has long been silent: a clock set back must not shut a neighbour out");
    Check(NewerSession(0xfffffff0u, 5, 0) && !NewerSession(5, 0xfffffff0u, 0), "the numbers may wrap around");
}

void Handshake() {
    const std::uint32_t rules = 0x3FFFF; Heard heard[kMaxPeers];
    Check(NextStage(kHere, heard, 2) == kHere && Agree(false, rules, heard, 2) == Discord::None, "nobody heard: nothing changes and nothing is wrong yet");
    heard[0] = Heard{kHere + 1, true, rules};
    Check(NextStage(kHere, heard, 2) == kHere && Agree(false, rules, heard, 2) == Discord::None, "one of two heard: the comparison stays on");
    heard[1] = Heard{kHere + 1, false, rules};
    Check(Agree(false, rules, heard, 2) == Discord::None && NextStage(kHere, heard, 2) == kCompareOff, "every neighbour has said hello: the comparison goes off");
    Check(NextStage(kCompareOff, heard, 2) == kCompareOff, "but nobody is live while a neighbour still compares");
    heard[0].stage = kCompareOff + 1; Check(NextStage(kCompareOff, heard, 2) == kCompareOff, "one of two has said so");
    heard[1].stage = kLive + 1; Check(NextStage(kCompareOff, heard, 2) == kLive && NextStage(kHere, heard, 2) == kLive, "all have: live, even in one step for a module that joins late");
    heard[0].stage = 0; Check(NextStage(kLive, heard, 2) == kLive, "a neighbour that starts anew takes nobody back from live");
    Check(NextStage(kHere, heard, 0) == kHere, "without neighbours there is no handshake");
    heard[0] = Heard{kHere + 1, true, rules}; heard[1] = Heard{0, false, 0};
    Check(Agree(true, rules, heard, 2) == Discord::Hosts, "two hosts: said at once, whoever is still to come");
    Check(Agree(false, rules, heard, 1) == Discord::None && Agree(true, rules, heard + 1, 1) == Discord::None, "one host among those heard, or still somebody to hear");
    heard[0].host = false; heard[1] = Heard{kHere + 1, false, rules};
    Check(Agree(false, rules, heard, 2) == Discord::Hosts, "everybody heard and nobody hosts: no world would be applied anywhere");
    heard[1].stage = 0; Check(Agree(false, rules, heard, 2) == Discord::None, "no host yet, but one neighbour is still to be heard");
    heard[0].rules = rules & ~16u; Check(Agree(true, rules, heard, 2) == Discord::Rules, "a neighbour that leaves a rule out plays another game");
}

void Tables() {
    std::uint32_t table[6] = {1, 2, 3, 4, 5, 6}, count = 6;
    Prune(table, count, [](std::uint32_t each) { return each % 2 == 0; });
    Check(count == 3 && table[0] == 2 && table[1] == 4 && table[2] == 6, "what is no longer needed goes, the rest keeps its order");
    Prune(table, count, [](std::uint32_t) { return true; }); Check(count == 3, "nothing to drop");
    Prune(table, count, [](std::uint32_t) { return false; }); Check(count == 0, "all gone: the table is free again");
    Prune(table, count, [](std::uint32_t) { return true; }); Check(count == 0, "an empty table");
}

void Log() {
    MatchLog match;
    TakeLogLine(match, "[INFO] - Adding local player", 10); Check(!match.on && !match.expectOwn, "outside a match the lines mean nothing");
    TakeLogLine(match, "[INFO] - Start Networked", 100); Check(match.on && match.number == 1 && match.own < 0 && match.changedAt == 100, "a match opens");
    TakeLogLine(match, "[INFO] - Setting controller ID to 9, (Prev: 0)", 101); Check(match.own < 0, "a controller set before the local player is added is not the own one");
    TakeLogLine(match, "[INFO] - Adding local player", 110); TakeLogLine(match, "[INFO] - Setting controller ID to 12, (Prev: 0)", 120);
    Check(match.own == 12 && !match.expectOwn && match.changedAt == 120, "the own device");
    TakeLogLine(match, "[INFO] - Adding remote player, UserID = 76561198000000001, device ID = 13", 130);
    TakeLogLine(match, "[INFO] - Adding remote player, UserID = 76561198000000002, device ID = 14", 140);
    Check(match.remotes == 2 && match.remoteIds[0] == 76561198000000001ull && match.remoteDevices[0] == 13 && match.remoteIds[1] == 76561198000000002ull && match.remoteDevices[1] == 14,
        "the other players: Steam id and this game's device number");
    TakeLogLine(match, "[INFO] - Input device (ID = 99) disconnected", 150); Check(match.remotes == 2 && match.roster == 0 && match.changedAt == 140, "a device of nobody's");
    TakeLogLine(match, "[INFO] - Input device (ID = 13) disconnected", 160);
    Check(match.remotes == 1 && match.remoteDevices[0] == 14 && match.remoteIds[0] == 76561198000000002ull && match.roster == 1 && match.changedAt == 160, "a player has left, the others remain");
    TakeLogLine(match, "[INFO] - Menu Game Init", 170); Check(!match.on && match.number == 1, "the match closes");
    TakeLogLine(match, "[INFO] - Start Networked", 200); Check(match.on && match.number == 2 && match.remotes == 0 && match.own < 0 && match.roster == 0, "the next match starts from nothing");
    TakeLogLine(match, "[INFO] - Leaving current lobby", 210); Check(!match.on, "leaving the lobby closes it as well");
}

void Contract(const wchar_t* path) {
    HMODULE dll = LoadLibraryW(path); Check(dll != nullptr, "load native adapter");
    using Export = DWORD(WINAPI*)(void*);
    auto start = reinterpret_cast<Export>(GetProcAddress(dll, "IsaacAuthorityNativeStart"));
    auto stop = reinterpret_cast<Export>(GetProcAddress(dll, "IsaacAuthorityNativeStop"));
    Check(start && stop && GetProcAddress(dll, "IsaacAuthorityNativeAuto"), "the three ways in");
    Check(start(nullptr) == ERROR_BAD_CONFIGURATION && stop(nullptr) == ERROR_NOT_READY, "native adapter refuses a start nobody configured and an inactive stop");
    FreeLibrary(dll);
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        Packing(); Ranges(); Frames(); Sessions(); Handshake(); Tables(); Log();
        for (int i = 1; i < argc; ++i) Contract(argv[i]);
        std::cout << "PASS native packing, ranges, frame assembly, sessions, handshake, tables, match log, module contract\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
