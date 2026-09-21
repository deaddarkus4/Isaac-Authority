#include "native_state.hpp"
#include <cstdlib>
#include <cstring>

namespace authority::native {
namespace {
template <class T> void Put(std::uint8_t*& at, const T* from, std::uint32_t count) { std::memcpy(at, from, count * sizeof(T)); at += count * sizeof(T); }
template <class T> bool Take(const std::uint8_t*& at, const std::uint8_t* end, T* to, std::uint32_t count, std::uint32_t most) {
    if (count > most || static_cast<std::size_t>(end - at) < count * sizeof(T)) return false;
    std::memcpy(to, at, count * sizeof(T)); at += count * sizeof(T); return true;
}
}

std::uint32_t PackWorld(const World& world, std::uint8_t* out) {
    auto* at = out; Put(at, reinterpret_cast<const std::uint8_t*>(&world), static_cast<std::uint32_t>(offsetof(World, npcs)));
    Put(at, world.npcs, world.count); Put(at, world.died, world.deaths); Put(at, world.grid, world.cells); Put(at, world.shot, world.shots); Put(at, world.drop, world.drops);
    Put(at, world.door, world.doors); Put(at, world.enemyBomb, world.enemyBombs); Put(at, world.slot, world.slots); Put(at, world.bornCell, world.born);
    Put(at, world.gridMap, kGridMapBytes);
    return static_cast<std::uint32_t>(at - out);
}

bool UnpackWorld(const std::uint8_t* from, std::uint32_t size, World& world) {
    const auto* at = from; const auto* end = from + size; world = World{};
    return Take(at, end, reinterpret_cast<std::uint8_t*>(&world), static_cast<std::uint32_t>(offsetof(World, npcs)), static_cast<std::uint32_t>(offsetof(World, npcs))) &&
        Take(at, end, world.npcs, world.count, kMaxNpcs) && Take(at, end, world.died, world.deaths, kMaxDeaths) && Take(at, end, world.grid, world.cells, kMaxCells) &&
        Take(at, end, world.shot, world.shots, kMaxShots) && Take(at, end, world.drop, world.drops, kMaxDrops) && Take(at, end, world.door, world.doors, kMaxDoors) &&
        Take(at, end, world.enemyBomb, world.enemyBombs, kMaxEnemyBombs) && Take(at, end, world.slot, world.slots, kMaxSlots) && Take(at, end, world.bornCell, world.born, kMaxBorn) &&
        Take(at, end, world.gridMap, kGridMapBytes, kGridMapBytes) && at == end;
}

std::uint32_t PackShots(const Shots& shots, std::uint8_t* out) {
    auto* at = out; Put(at, reinterpret_cast<const std::uint8_t*>(&shots), static_cast<std::uint32_t>(offsetof(Shots, shot)));
    Put(at, shots.shot, shots.count + shots.bombs); Put(at, shots.pet, shots.pets);
    return static_cast<std::uint32_t>(at - out);
}

bool UnpackShots(const std::uint8_t* from, std::uint32_t size, Shots& shots) {
    const auto* at = from; const auto* end = from + size; shots = Shots{};
    return Take(at, end, reinterpret_cast<std::uint8_t*>(&shots), static_cast<std::uint32_t>(offsetof(Shots, shot)), static_cast<std::uint32_t>(offsetof(Shots, shot))) &&
        shots.count <= kMaxTears && shots.bombs <= kMaxTears && Take(at, end, shots.shot, shots.count + shots.bombs, kMaxTears) && Take(at, end, shots.pet, shots.pets, kMaxPets) && at == end;
}

bool ValidShot(const Shot& shot) {
    if (!Finite(shot.position) || !Finite(shot.velocity) || !std::isfinite(shot.height) || !std::isfinite(shot.fallingSpeed) || !std::isfinite(shot.fallingAccel) ||
        !std::isfinite(shot.scale) || !std::isfinite(shot.damage)) return false;
    for (const float c : shot.color) if (!std::isfinite(c) || std::fabs(c) > 100.0f) return false;
    // The height's upper bound is that wide for bombs: a bomb carries its frames to the explosion there, and a remote one's may be very many.
    return std::fabs(shot.position[0]) < 10000.0f && std::fabs(shot.position[1]) < 10000.0f && std::fabs(shot.velocity[0]) < 1000.0f && std::fabs(shot.velocity[1]) < 1000.0f &&
        shot.height > -5000.0f && shot.height < 2.0e9f && std::fabs(shot.fallingSpeed) < 1000.0f && std::fabs(shot.fallingAccel) < 100.0f &&
        shot.scale >= 0.0f && shot.scale < 50.0f && shot.damage >= 0.0f && shot.damage < 100000.0f;
}

bool ValidWorld(const World& world) {
    if (world.magic != kWorldMagic || !world.sequence || world.count > kMaxNpcs || world.deaths > kMaxDeaths || world.cells > kMaxCells || world.shots > kMaxShots ||
        world.drops > kMaxDrops || world.doors > kMaxDoors || world.enemyBombs > kMaxEnemyBombs || world.slots > kMaxSlots || world.born > kMaxBorn) return false;
    for (std::uint32_t n = 0; n < world.slots; ++n) if (!Finite(world.slot[n].position) || world.slot[n].animation[kAnimationName - 1]) return false;
    for (std::uint32_t n = 0; n < world.enemyBombs; ++n) if (!ValidShot(world.enemyBomb[n])) return false;
    for (std::uint32_t n = 0; n < world.drops; ++n) if (!Finite(world.drop[n].position) || !Finite(world.drop[n].velocity)) return false;
    for (std::uint32_t n = 0; n < world.shots; ++n) if (!ValidShot(world.shot[n])) return false;
    for (std::uint32_t n = 0; n < world.count; ++n)
        if (!Finite(world.npcs[n].position) || !Finite(world.npcs[n].velocity) || !std::isfinite(world.npcs[n].hitPoints) || world.npcs[n].animation[kAnimationName - 1] ||
            world.npcs[n].gridCollision >= kGridCollisionClasses || world.npcs[n].entityCollision >= kEntityCollisionClasses ||
            world.npcs[n].renderZ < -100000 || world.npcs[n].renderZ > 100000) return false;
    return true;
}

Piece Gather(Assembly& assembly, const FrameHeader& header, const std::uint8_t* payload, std::uint32_t part, std::uint32_t most, bool& broke) {
    broke = false;
    if (!header.chunks || header.chunks > kMaxChunks || header.chunk >= header.chunks || !header.total || header.total > most || most > sizeof(assembly.data) ||
        part > kChunkBytes) return Piece::Refused;
    const std::uint32_t from = header.chunk * kChunkBytes;
    if (from + part > header.total || (header.chunk + 1u < header.chunks && part != kChunkBytes) || (header.chunk + 1u == header.chunks && from + part != header.total))
        return Piece::Refused;
    if (header.sequence < assembly.sequence || (header.sequence == assembly.sequence && assembly.done)) return Piece::Kept;   // overtaken, or a late double of a frame that is whole
    if (header.sequence > assembly.sequence || header.total != assembly.total || header.chunks != assembly.chunks) {
        broke = assembly.sequence && assembly.have && assembly.have != (1u << assembly.chunks) - 1u;
        assembly.sequence = header.sequence; assembly.total = header.total; assembly.chunks = header.chunks; assembly.have = 0; assembly.done = false;
    }
    std::memcpy(assembly.data + from, payload, part); assembly.have |= 1u << header.chunk;
    if (assembly.have != (1u << header.chunks) - 1u) return Piece::Kept;
    assembly.have = 0; assembly.done = true;   // whole: once
    return Piece::Whole;
}

std::uint32_t NextSession(std::uint32_t last, std::uint64_t tenths) {
    auto next = static_cast<std::uint32_t>(tenths);
    if (last && static_cast<std::int32_t>(next - last) <= 0) next = last + 1;
    return next ? next : 1;
}

bool NewerSession(std::uint32_t known, std::uint32_t came, std::uint64_t silentMs) {
    if (!came || came == known) return false;
    return !known || static_cast<std::int32_t>(came - known) > 0 || silentMs > kSessionSilenceMs;
}

Discord Agree(bool host, std::uint32_t rules, const Heard* heard, std::uint32_t neighbours) {
    std::uint32_t hosts = host ? 1 : 0, answered = 0;
    for (std::uint32_t n = 0; n < neighbours; ++n) {
        if (!heard[n].stage) continue;
        ++answered; if (heard[n].host) ++hosts;
        if (heard[n].rules != rules) return Discord::Rules;
    }
    return hosts > 1 || (answered == neighbours && !hosts) ? Discord::Hosts : Discord::None;
}

std::uint8_t NextStage(std::uint8_t now, const Heard* heard, std::uint32_t neighbours) {
    if (!neighbours) return now;
    std::uint8_t least = 0xff;
    for (std::uint32_t n = 0; n < neighbours; ++n) if (heard[n].stage < least) least = heard[n].stage;
    if (now == kHere && least >= kHere + 1) now = kCompareOff;
    if (now == kCompareOff && least >= kCompareOff + 1) now = kLive;
    return now;
}

void TakeLogLine(MatchLog& match, const std::string& line, std::uint64_t now) {
    if (line.find("Start Networked") != std::string::npos) { const auto number = match.number + 1; match = MatchLog{}; match.on = true; match.number = number; match.changedAt = now; return; }
    if (line.find("Menu Game Init") != std::string::npos || line.find("Leaving current lobby") != std::string::npos) { match.on = false; match.changedAt = now; return; }
    if (!match.on) return;
    if (const auto remote = line.find("Adding remote player, UserID = "); remote != std::string::npos) {
        const auto device = line.find(", device ID = ", remote);
        if (device != std::string::npos && match.remotes < 8) {
            match.remoteIds[match.remotes] = std::strtoull(line.c_str() + remote + 31, nullptr, 10);
            match.remoteDevices[match.remotes++] = static_cast<int>(std::strtol(line.c_str() + device + 14, nullptr, 10));
            // A player may also come into a match that runs: the game lets one in where the floor changes ("Join Existing
            // Game"), and the others add that player with this same line. Counted like a leaving, so that whoever follows
            // the roster starts anew with the player; the adds of a match's start come before anybody has settled on it.
            ++match.roster;
        }
        match.expectOwn = false; match.changedAt = now;
    } else if (line.find("Adding local player") != std::string::npos) { match.expectOwn = true; match.changedAt = now; }
    else if (const auto set = line.find("Setting controller ID to "); set != std::string::npos && match.expectOwn && line.find("(Prev: 0)") != std::string::npos) {
        match.own = static_cast<int>(std::strtol(line.c_str() + set + 25, nullptr, 10)); match.expectOwn = false; match.changedAt = now;
    } else if (const auto gone = line.find("Input device (ID = "); gone != std::string::npos && line.find(") disconnected", gone) != std::string::npos) {
        // A player has left; the match goes on for the others. Whoever now has the lowest device number is the host.
        const int device = static_cast<int>(std::strtol(line.c_str() + gone + 19, nullptr, 10));
        for (int n = 0; n < match.remotes; ++n) {
            if (match.remoteDevices[n] != device) continue;
            for (int k = n + 1; k < match.remotes; ++k) { match.remoteIds[k - 1] = match.remoteIds[k]; match.remoteDevices[k - 1] = match.remoteDevices[k]; }
            --match.remotes; ++match.roster; match.changedAt = now;
            break;
        }
    }
}

void Playout::Take(const InputRecord& record) {
    ++heard;
    if (started && record.frame + kPlayoutRestart < next) started = false;   // the sender counts from the start again
    if (!started) { *this = Playout{}; heard = 1; started = true; next = newest = record.frame; }
    if (record.frame < next) { ++stale; return; }
    // Far ahead of what is being played: what lies between is given up, as Play() would give it up.
    while (record.frame - next >= kPlayoutSlots) { if (filled[next % kPlayoutSlots]) { filled[next % kPlayoutSlots] = false; ++skipped; } ++next; }
    slots[record.frame % kPlayoutSlots] = record; filled[record.frame % kPlayoutSlots] = true;
    if (record.frame > newest || newest < next) newest = record.frame;
}

InputRecord Playout::Play() {
    if (!started) { ++repeated; return last; }
    const auto buttons = [](const InputRecord& of) { return static_cast<std::uint16_t>(of.input[0] | of.input[1] << 8); };
    std::uint16_t carried = 0;
    for (; newest >= next && newest - next >= kPlayoutMostWaiting; ++next) {
        if (!filled[next % kPlayoutSlots]) continue;
        carried |= buttons(slots[next % kPlayoutSlots]); filled[next % kPlayoutSlots] = false; ++skipped;
    }
    if (!filled[next % kPlayoutSlots]) {
        // Not here. Later ones are: it is lost, or very late - one frame of grace, then on to the earliest that is here.
        if (newest > next && ++waited > 1) { while (next < newest && !filled[next % kPlayoutSlots]) ++next; waited = 0; }
        if (!filled[next % kPlayoutSlots]) { ++repeated; return last; }
    }
    last = slots[next % kPlayoutSlots]; filled[next % kPlayoutSlots] = false; ++next; waited = 0; ++played;
    if (carried) { const auto all = static_cast<std::uint16_t>(buttons(last) | carried); last.input[0] = static_cast<std::uint8_t>(all); last.input[1] = static_cast<std::uint8_t>(all >> 8); }
    return last;
}

void SharedUnlocks(const std::uint8_t* const* members, std::uint32_t count, std::uint8_t* shared) {
    for (std::uint32_t n = 0; n < kSaveAchievements; ++n) {
        bool all = count != 0;
        for (std::uint32_t m = 0; m < count && all; ++m) all = members[m][n] != 0;
        shared[n] = all ? 1 : 0;
    }
}

std::uint32_t UnlocksLacking(const std::uint8_t* shared, const std::uint8_t* joiner) {
    std::uint32_t lacking = 0;
    for (std::uint32_t n = 0; n < kSaveAchievements; ++n) if (shared[n] && !joiner[n]) ++lacking;
    return lacking;
}
}
