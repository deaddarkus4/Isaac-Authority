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
    world->died[0] = 9; world->shot[0] = Tear(); world->drop[0].seed = 31; world->drop[0].variant = 20; world->door[0].cell = 7; world->door[0].state = 2; world->door[0].deal = 14; world->dealSeed = 0xf20729e3;
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
    world = Room(); world->npcs[0].linked = kNpcHidden | 1; Check(ValidWorld(*world), "a part of something that the host's game does not show");
    world = Room(); world->npcs[0].linked = 8; Check(!ValidWorld(*world), "a bit of an enemy's record that there is not");
    world = Room(); world->sequence = 0; Check(!ValidWorld(*world), "sequences start at one");
    world = Room(); world->doors = kMaxDoors + 1; Check(!ValidWorld(*world), "more doors than the list holds");
    world = Room(); world->door[0].deal = 3; Check(!ValidWorld(*world), "a door to a deal leads to a devil's room or an angel's");
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
    TakeLogLine(match, "[INFO] - Input device (ID = 99) disconnected", 150); Check(match.remotes == 2 && match.roster == 2 && match.changedAt == 140, "a device of nobody's");
    TakeLogLine(match, "[INFO] - Input device (ID = 13) disconnected", 160);
    Check(match.remotes == 1 && match.remoteDevices[0] == 14 && match.remoteIds[0] == 76561198000000002ull && match.roster == 3 && match.changedAt == 160, "a player has left, the others remain");
    TakeLogLine(match, "[INFO] - [Frame: 5120] Adding remote player, UserID = 76561198000000003, device ID = 15", 165);
    Check(match.remotes == 2 && match.remoteDevices[1] == 15 && match.remoteIds[1] == 76561198000000003ull && match.roster == 4 && match.changedAt == 165, "a player has come into the running match");
    TakeLogLine(match, "[INFO] - Menu Game Init", 170); Check(!match.on && match.number == 1, "the match closes");
    TakeLogLine(match, "[INFO] - Start Networked", 200); Check(match.on && match.number == 2 && match.remotes == 0 && match.own < 0 && match.roster == 0, "the next match starts from nothing");
    TakeLogLine(match, "[INFO] - Leaving current lobby", 210); Check(!match.on, "leaving the lobby closes it as well");
    // The host's log of the match through Steam in which the one guest left and came back at the next floor.
    MatchLog host; Done done;
    TakeLogLine(host, "[INFO] - Start Networked ", 1000); TakeLogLine(host, "[INFO] - [Frame: 0] Adding local player, device ID = 0 ", 1001);
    TakeLogLine(host, "[INFO] - Entity_Player::SetControllerId() Setting controller ID to 2, (Prev: 0) ", 1002);
    TakeLogLine(host, "[INFO] - [Frame: 0] Adding remote player, UserID = 76561198000000001, device ID = 3 ", 1003);
    Check(!MayStart(host, done, 2000) && MayStart(host, done, 1003 + kSettleMs + 1), "the module starts once the log has been quiet about the match for a while");
    TakeLogLine(host, "[INFO] - [Frame: 23380] Input device (ID = 3) disconnected ", 50000);
    Check(host.remotes == 0 && !MayStart(host, done, 60000), "alone in the match: nothing to start for");
    done = Done{host.number, host.roster};   // the module has stopped: the other players have left
    TakeLogLine(host, "[INFO] - [Frame 24492] Adding pending join for player 76561198000000001 ", 70000);
    Check(!MayStart(host, done, 80000), "a newcomer that waits in the lobby is not in the match");
    TakeLogLine(host, "[INFO] - [Frame: 26190] Adding remote player, UserID = 76561198000000001, device ID = 4 ", 90000);
    TakeLogLine(host, "[INFO] - Joining existing game on frame 26190", 90001);
    TakeLogLine(host, "[INFO] - Entity_Player::SetControllerId() Setting controller ID to 2, (Prev: 0) ", 90002);
    Check(host.number == 1 && host.own == 2 && host.remotes == 1 && host.remoteDevices[0] == 4, "for a member the newcomer's entry is no new match, and its own device stays what it was");
    Check(MayStart(host, done, 90003 + kSettleMs), "the player is back in the same match: the module starts again");
    done = Done{host.number, host.roster}; Check(!MayStart(host, done, 99000), "and what it is done with stays done while nobody comes or leaves");
    // The newcomer itself never starts a match: the game says that it joins one, and then adds the players.
    MatchLog joiner;
    TakeLogLine(joiner, "[INFO] - Menu Game Init", 10); TakeLogLine(joiner, "[INFO] - Joining existing game on frame 26190", 500);
    Check(joiner.on && joiner.number == 1, "coming into a running match opens the match for the newcomer");
    TakeLogLine(joiner, "[INFO] - [Frame: 26190] Adding remote player, UserID = 76561198000000009, device ID = 2 ", 501);
    TakeLogLine(joiner, "[INFO] - [Frame: 26190] Adding local player, device ID = 0 ", 502);
    TakeLogLine(joiner, "[INFO] - Entity_Player::SetControllerId() Setting controller ID to 3, (Prev: 0) ", 503);
    Check(joiner.own == 3 && joiner.remotes == 1 && MayStart(joiner, Done{}, 503 + kSettleMs + 1), "and its module starts as in any match");
}

void Unlocks() {
    std::uint8_t host[kSaveAchievements]{}, guest[kSaveAchievements]{}, joiner[kSaveAchievements]{}, shared[kSaveAchievements]{};
    for (std::uint32_t n : {0u, 7u, 8u, 100u, 641u}) host[n] = 1;
    for (std::uint32_t n : {7u, 8u, 100u, 300u, 641u}) guest[n] = 1;
    const std::uint8_t* members[] = {host, guest};
    SharedUnlocks(members, 2, shared);
    Check(!shared[0] && shared[7] && shared[8] && shared[100] && !shared[300] && shared[641], "the session's unlocks are what every member has");
    Check(UnlocksLacking(shared, host) == 0 && UnlocksLacking(shared, guest) == 0 && UnlocksLacking(shared, joiner) == 4, "its members lack none, an empty save lacks them all");
    joiner[7] = joiner[8] = joiner[100] = 1;
    Check(UnlocksLacking(shared, joiner) == 1, "the last achievement counts too");
    joiner[641] = 0x7f; joiner[5] = 1;
    Check(UnlocksLacking(shared, joiner) == 0, "a save with more than the session's unlocks may come in, and any byte but 0 is an unlock");
    SharedUnlocks(members, 0, shared);
    Check(!shared[7] && !shared[641] && UnlocksLacking(shared, joiner) == 0, "of no members nothing is shared");
}

void Inputs() {
    const auto record = [](std::uint32_t frame, std::uint16_t buttons) {
        InputRecord made{frame, {}}; std::memcpy(made.input, kNeutralInput, kInputBytes);
        made.input[0] = static_cast<std::uint8_t>(buttons); made.input[1] = static_cast<std::uint8_t>(buttons >> 8); return made;
    };
    const auto pressed = [](const InputRecord& of) { return static_cast<std::uint16_t>(of.input[0] | of.input[1] << 8); };
    Playout playout;
    const auto first = playout.Play();
    Check(std::memcmp(first.input, kNeutralInput, kInputBytes) == 0 && playout.repeated == 1, "before anything has come a frame plays the game's own empty input");
    // The sender's numbers are its own: this game's frames are never asked about.
    playout.Take(record(1000, 1)); playout.Take(record(1001, 2)); playout.Take(record(1002, 4));
    Check(pressed(playout.Play()) == 1 && pressed(playout.Play()) == 2 && pressed(playout.Play()) == 4 && playout.played == 3, "what came is played in the sender's order, a record a frame");
    Check(pressed(playout.Play()) == 4 && pressed(playout.Play()) == 4 && playout.repeated == 2, "nothing has come: the last record again, and the frame does not wait (the counts are of this sender's match)");
    playout.Take(record(1001, 8)); Check(playout.stale == 1 && pressed(playout.Play()) == 4, "a record of a frame already played is stale");
    // A record that stays away while later ones are here: a frame's grace, then it is given up.
    playout.Take(record(1004, 16)); playout.Take(record(1005, 32));
    Check(pressed(playout.Play()) == 4 && pressed(playout.Play()) == 16 && pressed(playout.Play()) == 32, "a hole is given up after a frame's grace");
    playout.Take(record(1003, 64)); Check(playout.stale == 2, "and what comes for it afterwards is stale");
    // Overtaken on the way: played in the sender's order all the same.
    playout.Take(record(1007, 2)); playout.Take(record(1006, 1));
    Check(pressed(playout.Play()) == 1 && pressed(playout.Play()) == 2, "records that overtook each other are played in order");
    // This game stood still for a while: never more than kPlayoutMostWaiting behind, and no press is lost on the way.
    for (std::uint32_t n = 0; n < 20; ++n) playout.Take(record(1008 + n, n == 3 ? 0x100 : 0));
    const auto caught = playout.Play();
    Check((pressed(caught) & 0x100) && playout.skipped == 12 && playout.newest - playout.next < kPlayoutMostWaiting, "what is jumped over keeps its buttons");
    // The sender's count starts over: a new match.
    playout.Take(record(3, 7)); Check(playout.started && playout.next == 3 && playout.heard == 1 && pressed(playout.Play()) == 7, "a count that starts over is a new match");
    // Far ahead of what is being played: reached after the frame of grace.
    playout.Take(record(500, 9)); Check(pressed(playout.Play()) == 7 && pressed(playout.Play()) == 9, "a record far ahead is reached after the frame of grace");
}

void Hits() {
    const auto blow = [](std::uint32_t target, float damage) { Hit hit{}; hit.room = 84; hit.target = target; hit.damage = damage; hit.sourceType = 2; hit.spawnerType = 1; return hit; };
    HitLog log; Hit said[kMaxHits];
    Check(log.Recent(100, said) == 0, "nothing has landed: nothing to say");
    log.Note(blow(11, 3.5f), 100); log.Note(blow(12, 3.5f), 101); log.Note(blow(11, 3.5f), 108);
    Check(log.Recent(108, said) == 3 && said[0].number == 1 && said[2].number == 3 && said[1].target == 12 && ValidHits(said, 3), "blows are numbered from 1 and said oldest first");
    Check(log.Recent(111, said) == 2 && said[0].number == 2, "a blow is said again for kHitResendFrames frames and no longer");
    Check(log.Recent(200, said) == 0, "and then not at all");
    // The receiver: every number once, whichever of the lists that carried it came through.
    std::uint32_t done = 0, lost = 0; const Hit all[3] = {log.ring[0], log.ring[1], log.ring[2]};
    Check(FreshHits(all, 2, done, lost) == 0 && done == 2 && lost == 0, "the first list of a sender: all of it is new, and nothing before it is called lost");
    Check(FreshHits(all, 2, done, lost) == 2 && done == 2, "the same list again: nothing new");
    Check(FreshHits(all, 3, done, lost) == 2 && done == 3 && lost == 0, "a longer one: what is behind the last number played");
    Hit later = blow(11, 1); later.number = 9; Check(FreshHits(&later, 1, done, lost) == 0 && done == 9 && lost == 5, "numbers that never came are counted as lost");
    Hit anew = blow(11, 1); anew.number = 1; Check(FreshHits(&anew, 1, done, lost) == 0 && done == 1 && lost == 5, "a list that ends below the last number played: the sender has started anew");
    Check(FreshHits(nullptr, 0, done, lost) == 0 && done == 1, "an empty list changes nothing");
    // More blows in a window than the ring holds: the newest stay.
    HitLog busy; for (std::uint32_t n = 0; n < kMaxHits + 10; ++n) busy.Note(blow(n, 1), 50);
    Check(busy.Recent(50, said) == kMaxHits && said[0].number == 11 && said[kMaxHits - 1].number == kMaxHits + 10 && ValidHits(said, kMaxHits), "the ring keeps the newest blows");
    // What a list must keep.
    Hit bad[2] = {log.ring[1], log.ring[0]}; Check(!ValidHits(bad, 2), "numbers must rise");
    bad[0] = log.ring[0]; bad[1] = log.ring[1]; bad[1].kind = kHitKinds; Check(!ValidHits(bad, 2), "a kind there is not");
    bad[1] = log.ring[1]; bad[1].damage = std::numeric_limits<float>::quiet_NaN(); Check(!ValidHits(bad, 2), "damage that is no number");
    bad[1] = log.ring[1]; bad[1].damage = -1; Check(!ValidHits(bad, 2), "negative damage");
    bad[1] = log.ring[1]; bad[1].number = 0; Check(!ValidHits(bad + 1, 1), "numbers start at one");
    // And they travel with the shots.
    auto shots = std::make_unique<Shots>(); *shots = Shots{}; shots->magic = kShotsMagic; shots->controller = 3; shots->sequence = 9; shots->count = 1; shots->shot[0] = Tear();
    shots->hits = log.Recent(108, shots->hit);
    auto back = std::make_unique<Shots>(); std::vector<std::uint8_t> carried(sizeof(Shots)); const auto size = PackShots(*shots, carried.data());
    Check(size == offsetof(Shots, shot) + sizeof(Shot) + 3 * sizeof(Hit) && UnpackShots(carried.data(), size, *back) && std::memcmp(shots.get(), back.get(), sizeof(Shots)) == 0,
        "blows ride behind the tears and the pets");
    shots->hits = kMaxHits + 1; std::memcpy(carried.data(), shots.get(), offsetof(Shots, shot));
    Check(!UnpackShots(carried.data(), static_cast<std::uint32_t>(carried.size()), *back), "more blows than the list holds are refused");
}

void Steady() {
    // An even path: every second frame of the host's, ten frames on the way.
    Dejitter clock;
    for (std::uint32_t s = 100; s < 180; s += 2) clock.Came(s, s + 10);
    Check(clock.Least() == 10 && clock.cushion == 2, "an even path: its lead is known, the cushion is still the one it started with");
    for (std::uint32_t s = 180; s < 308; s += 2) clock.Came(s, s + 10);
    Check(clock.cushion == kLeastCushion, "a calm path: the cushion shrinks to the least, a frame at a time and only after a long calm");
    std::uint32_t one[1] = {300}, came[1] = {310};
    auto picked = PickWorld(clock, one, came, 1, 310, 2);
    Check(picked.play == -1 && picked.drop == 0, "a snapshot waits for its moment: its sequence, the path, the cushion");
    picked = PickWorld(clock, one, came, 1, 311, 3);
    Check(picked.play == 0 && picked.drop == 1 && picked.late == 0 && !picked.rebased, "and is played in it");
    // One that trembled four frames late: this game has run the room past it already.
    one[0] = 302; came[0] = 316; picked = PickWorld(clock, one, came, 1, 316, 5);
    Check(picked.play == -1 && picked.drop == 1 && picked.late == 1, "a snapshot overtaken by this game's own running is dropped, not played backwards");
    // Two at once after a gap: the newer one is on time.
    std::uint32_t two[2] = {304, 306}, cameTwo[2] = {317, 317}; picked = PickWorld(clock, two, cameTwo, 2, 317, 6);
    Check(picked.play == 1 && picked.drop == 2 && picked.late == 0, "of several whose moment has come the newest is played");
    // A snapshot a frame past its moment is still this moment's: frames are whole, and the two games' do not begin together.
    one[0] = 308; came[0] = 320; picked = PickWorld(clock, one, came, 1, 320, 3);
    Check(picked.play == 0, "a frame of slack");
    // The host's game stood still, or the path grew longer: everything is late, and after kStarveFrames the clock starts anew.
    std::uint32_t after[2] = {320, 322}, cameAfter[2] = {340, 340}; picked = PickWorld(clock, after, cameAfter, 2, 340, kStarveFrames - 1);
    Check(picked.play == -1 && picked.late == 2, "late, but something was played a moment ago: dropped");
    picked = PickWorld(clock, after, cameAfter, 2, 340, kStarveFrames);
    Check(picked.play == 1 && picked.drop == 2 && picked.rebased && clock.Least() == 18, "nothing played for too long: the newest is played and the clock starts from it");
    clock.Came(324, 342); one[0] = 324; came[0] = 342;
    Check(PickWorld(clock, one, came, 1, 342, 2).play == -1 && PickWorld(clock, one, came, 1, 343, 3).play == 0, "and from then on the new path is the path");
    // Trembling: half of the snapshots four frames late. The cushion covers it at once; three wild ones of 32 do not move it.
    Dejitter shaky;
    for (std::uint32_t n = 0; n < 32; ++n) shaky.Came(1000 + 2 * n, 1010 + 2 * n + (n % 2 ? 4 : 0));
    Check(shaky.Least() == 10 && shaky.cushion == 4, "the cushion grows to what nine of ten stay under");
    Dejitter spiky;
    for (std::uint32_t n = 0; n < 32; ++n) spiky.Came(1000 + 2 * n, 1010 + 2 * n + (n % 11 == 5 ? 20 : 0));
    Check(spiky.cushion == 2, "rare spikes are dropped as late, not waited for by everything else");
    // This game stood still for five frames: three snapshots came meanwhile and look early. They are not the path.
    Dejitter stalled;
    for (std::uint32_t n = 0; n < 32; ++n) stalled.Came(1000 + 2 * n, 1010 + 2 * n - (n >= 20 && n < 23 ? 5 : 0));
    Check(stalled.Least() == 10, "a few snapshots that look early do not move the path");
    // A new room: the adapter empties the clock, and it starts from the newest snapshot of that room.
    clock.count = 0; one[0] = 400; came[0] = 450; picked = PickWorld(clock, one, came, 1, 450, 0);
    Check(picked.rebased && picked.play == -1 && PickWorld(clock, one, came, 1, 451, 1).play == 0, "an emptied clock starts from the newest snapshot, which waits out the cushion");
    Check(PickWorld(clock, nullptr, nullptr, 0, 500, 100).play == -1, "nothing waits: nothing is played");
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
        Packing(); Ranges(); Frames(); Sessions(); Handshake(); Tables(); Log(); Unlocks(); Inputs(); Hits(); Steady();
        for (int i = 1; i < argc; ++i) Contract(argv[i]);
        std::cout << "PASS native packing, ranges, frame assembly, sessions, handshake, tables, match log, unlocks, input playout, blows, steady worlds, module contract\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
