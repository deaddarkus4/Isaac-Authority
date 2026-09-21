#pragma once
// What the modules of the native path say to each other, and every rule of that exchange which needs no game to be
// checked: the layout of the wire, packing, the ranges a received list must keep, putting a frame together from its
// datagrams, which start of a neighbour's module a frame is of, the handshake's next step, and the match as the game's
// log tells it. native_adapter.cpp is everything that touches the game; tests/native_tests.cpp runs what is here.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace authority::native {
constexpr std::uint32_t kBodyMagic = 0x31524c50, kWorldMagic = 0x314e4c57, kShotsMagic = 0x31544853;   // "PLR1", "WLN1", "SHT1"
constexpr std::uint32_t kFrameMagic = 0x31464149, kChunkBytes = 1180, kMaxPeers = 4;                   // "IAF1"; header and chunk: a datagram of at most 1200 bytes
constexpr std::uint8_t kOfBody = 1, kOfShots = 2, kOfWorld = 3, kOfHello = 4;
// 2: the receiver numbers the sender's player; 3: an enemy's collision damage, a player's hits and blink;
// 4: every frame names its sender's start (the session), and a hello's host and rules are held against the others'.
constexpr std::uint32_t kHelloMagic = 0x314C4548, kProtocol = 4;   // "HEL1"
constexpr std::uint32_t kGridCollisionClasses = 8, kEntityCollisionClasses = 5;   // the game's enums: GRIDCOLL_NONE..PITSONLY, ENTCOLL_NONE..ALL
constexpr std::uint8_t kHere = 0, kCompareOff = 1, kLive = 2;
constexpr int kHealthFields = 10, kMaxTaken = 8, kAnimationName = 24, kMaxNpcs = 48, kMaxDeaths = 16;
constexpr std::uint32_t kMaxCells = 96, kGridMapBytes = 56, kMaxBorn = 48, kMaxShots = 64, kMaxTears = 32, kMaxSlots = 8, kMaxPets = 24, kMaxEnemyBombs = 16,
    kMaxDrops = 64, kMaxDoors = 8, kMaxChunks = 31;

#pragma pack(push, 1)
struct Taken { std::uint32_t number, room, seed, variant, subtype, low; float position[2]; };
struct Body {
    std::uint32_t magic, controller, sequence, room; float position[2], velocity[2]; std::uint32_t ghost; std::int32_t health[kHealthFields];
    std::uint32_t dimension, roomEpoch, host, dying, takenTotal, hurt, floor; Taken taken[kMaxTaken];
    std::uint32_t hits; std::int32_t headFrameDelay;   // blows that landed on the owner so far; the owner's blink
    std::uint32_t floorEpoch;                          // how many changes of floor this game has seen, as roomEpoch counts rooms
};
struct Npc {
    std::uint32_t seed, type, variant, subtype; float position[2], velocity[2], hitPoints;
    std::int32_t state, stateFrame, cooldown; float v1[2], v2[2]; std::int32_t i1, i2; float target[2];
    char animation[kAnimationName];   // empty: none, or a name too long to carry
    std::uint32_t linked;             // 1 has a parent, 2 has a child: a part of something, never created or removed by the lists
    float collisionDamage;            // what touching it costs
    // Whom and what it collides with at all (the game's GridCollisionClass and EntityCollisionClass) and the layer it is
    // drawn in (RenderZOffset). Read in a live pair: a fireplace the host's game has put out keeps its collision damage of
    // 1 - what the game changes is both classes to 0 and the layer to -1000. A guest's twin never goes out by itself (its
    // state is the host's), so without these it went on hurting the guest and its embers were drawn over the coin it dropped.
    std::uint32_t gridCollision, entityCollision; std::int32_t renderZ;
};
struct Cell { std::uint16_t index, type; std::int32_t state; };
struct Born { std::uint16_t index, type; std::uint32_t variant, seed; std::int32_t state; };   // state: a guest's cell that is broken further than this is not this cell
struct Shot {
    std::uint32_t seed, variant, subtype; float position[2], velocity[2], height, fallingSpeed, fallingAccel, scale, damage;
    std::uint64_t flags[2]; float color[11];
};
struct Drop { std::uint32_t seed, variant, subtype; float position[2], velocity[2]; std::int32_t price, timeout, options, shopItemId; };
struct DoorState { std::uint16_t cell; std::uint8_t busted, reserved; std::int32_t variant, state; };
struct SlotState { std::uint32_t seed, variant, subtype; float position[2]; std::int32_t state, prize, timeout, donation, trigger; char animation[kAnimationName]; };
struct World {
    std::uint32_t magic, sequence, room, count, deaths, clear, cells, shots, drops, dropsTotal, doors, npcTotal, hurt, enemyBombs, floor, slots, born; std::int32_t coins, bombs, keys;
    Npc npcs[kMaxNpcs]; std::uint32_t died[kMaxDeaths]; Cell grid[kMaxCells]; Shot shot[kMaxShots]; Drop drop[kMaxDrops]; DoorState door[kMaxDoors]; Shot enemyBomb[kMaxEnemyBombs]; SlotState slot[kMaxSlots]; Born bornCell[kMaxBorn]; std::uint8_t gridMap[kGridMapBytes];
};
// Tears first, then bombs. A bomb in a Shot: height = frames to the explosion, fallingSpeed = radius multiplier,
// fallingAccel = 1 for a fetus bomb, damage = explosion damage.
struct Pet { std::uint32_t seed, variant, subtype; float position[2], velocity[2]; std::uint32_t reserved; };
struct Shots { std::uint32_t magic, controller, sequence, room, count, bombs, pets; Shot shot[kMaxTears]; Pet pet[kMaxPets]; };
// session: which start of the sender's module this frame is of (see NewerSession); never 0.
struct FrameHeader { std::uint32_t magic; std::uint8_t kind, chunk, chunks, reserved; std::uint32_t sequence, total, session; };
struct Hello { std::uint32_t magic, protocol, rules; std::uint8_t controller, host, stage, reserved; };
#pragma pack(pop)
static_assert(sizeof(Taken) == 32 && sizeof(Body) == 76 + 4 * kHealthFields + kMaxTaken * 32 && sizeof(Npc) == 124 && sizeof(Pet) == 32 && sizeof(Cell) == 8 && sizeof(Shot) == 108 && sizeof(Shots) == 28 + kMaxTears * 108 + kMaxPets * 32 &&
              sizeof(Drop) == 44 && sizeof(DoorState) == 12 && sizeof(SlotState) == 64 && sizeof(Born) == 16 && sizeof(FrameHeader) == 20 && sizeof(Hello) == 16 &&
              sizeof(World) == 80 + kMaxNpcs * 124 + kMaxDeaths * 4 + kMaxCells * 8 + kMaxShots * 108 + kMaxDrops * 44 + kMaxDoors * 12 + kMaxEnemyBombs * 108 + kMaxSlots * 64 +
                                   kMaxBorn * 16 + kGridMapBytes && sizeof(Shots) != sizeof(Body) && sizeof(FrameHeader) + kChunkBytes <= 1200 &&
              sizeof(World) <= kMaxChunks * kChunkBytes, "wire layout");

inline bool Finite(const float* v) { return std::isfinite(v[0]) && std::isfinite(v[1]); }

// Only what is in use travels, in the order of the struct: the header, then every array by its count. Packing writes at
// most sizeof of the struct; unpacking takes nothing that names more entries than fit or whose size is not exactly its content's.
std::uint32_t PackWorld(const World& world, std::uint8_t* out);
bool UnpackWorld(const std::uint8_t* from, std::uint32_t size, World& world);
std::uint32_t PackShots(const Shots& shots, std::uint8_t* out);
bool UnpackShots(const std::uint8_t* from, std::uint32_t size, Shots& shots);
// Far outside what a shot can be: the sender read something else (another build's layout), and nothing of it is applied.
bool ValidShot(const Shot& shot);
bool ValidWorld(const World& world);

// A sequence being put together from its datagrams, per neighbour and kind.
struct Assembly {
    std::uint32_t sequence = 0, total = 0, have = 0; std::uint8_t chunks = 0; bool done = false; std::uint8_t data[sizeof(World)];   // done: this sequence has been reported whole
    void Reset() { sequence = total = have = 0; chunks = 0; done = false; }
};
enum class Piece { Refused, Kept, Whole };   // Kept: taken or overtaken, and nothing to deliver yet
// One datagram's payload into the sequence it belongs to; most is the largest whole of this kind. A whole is reported once:
// assembly.data holds assembly.total bytes of it. broke: an earlier sequence was given up with pieces missing.
Piece Gather(Assembly& assembly, const FrameHeader& header, const std::uint8_t* payload, std::uint32_t part, std::uint32_t most, bool& broke);

// A module numbers its frames from 1 every time it starts, and a neighbour that did not start anew would drop them all as
// older than what it has. So a start has a number of its own - the clock in tenths of a second, never the same twice in one
// process - which rides in every frame: a receiver that sees a newer one forgets what it had of that neighbour. A frame of
// an older start is a straggler, unless the start known here has been silent for kSessionSilenceMs (a clock set back).
constexpr std::uint64_t kSessionSilenceMs = 3000;
std::uint32_t NextSession(std::uint32_t last, std::uint64_t tenths);
bool NewerSession(std::uint32_t known, std::uint32_t came, std::uint64_t silentMs);

// The handshake, as far as it needs no game. What is known of a neighbour from its hellos of its present start.
struct Heard { std::uint8_t stage = 0; bool host = false; std::uint32_t rules = 0; };   // stage: the one it last named plus one; 0 while it has not been heard
// Whether these modules can play one match: exactly one of them hosts, and all of them run the same rules. Two hosts, or
// none, and nobody's world is applied anywhere while nobody compares checksums any more: the games would drift apart
// unnoticed. Said as soon as it is certain: a second host or other rules at once, no host only when everybody has been heard.
enum class Discord : std::uint8_t { None, Hosts, Rules };
Discord Agree(bool host, std::uint32_t rules, const Heard* heard, std::uint32_t neighbours);
// The stage this module may be at now: past "here" once every neighbour has said hello, live once every one of them has
// its comparison off. Never back.
std::uint8_t NextStage(std::uint8_t now, const Heard* heard, std::uint32_t neighbours);

// Drops the entries a table no longer needs and keeps the order of the rest.
template <class T, class Keep> void Prune(T* table, std::uint32_t& count, Keep keep) {
    std::uint32_t kept = 0;
    for (std::uint32_t n = 0; n < count; ++n) if (keep(table[n])) { if (kept != n) table[kept] = table[n]; ++kept; }
    count = kept;
}

// The match as the game's own log tells it, line by line: "Start Networked" opens one, the lines that add players name the
// own device and the others (Steam id, this game's device number), "Menu Game Init" and "Leaving current lobby" close it, a
// disconnected input device is a player who has left. A player the game lets into a running match is added by the same line
// as at the start.
struct MatchLog {
    bool on = false, expectOwn = false; int own = -1; std::uint32_t number = 0, roster = 0; std::uint64_t changedAt = 0;   // roster: counts the players who have come or left
    std::uint64_t remoteIds[8]{}; int remoteDevices[8]{}; int remotes = 0;
};
void TakeLogLine(MatchLog& match, const std::string& line, std::uint64_t now);

// Who may come into a match that runs (the user's rule): a player whose save has at least every unlock of the session's
// shared save. What a lobby member tells the others of its save (the game's writer, RVA 0x51b130) begins with the
// achievements: 642 flags of a bit each, least significant first, in (642 >> 3) + 1 bytes. The game's shared save is these
// flags ANDed over the members (its builder, RVA 0x51a450), so an unlock of the session is a flag every member has.
constexpr std::uint32_t kSaveAchievements = 642, kSaveAchievementBytes = (kSaveAchievements >> 3) + 1;
// The session's unlocks from its members' blocks; of no members, none.
void SharedUnlocks(const std::uint8_t* const* members, std::uint32_t count, std::uint8_t* shared);
// How many unlocks of the session a save lacks: 0 and its player may come in. Bits past the last achievement say nothing.
std::uint32_t UnlocksLacking(const std::uint8_t* shared, const std::uint8_t* joiner);
}
