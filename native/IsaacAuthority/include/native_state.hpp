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
// 5: the game no longer waits for a remote player's input - which a game that still waits must not be matched with.
// 6: a player's blows at the world are told by its owner (they ride with its shots) and a copy's count nowhere; a door to a
//    deal is the host's (its kind rides with the door, the seed its room was made from with the world).
// 7: whether the host's enemy is shown at all rides in its record (kNpcHidden in linked): a guest that does not know the bit
//    would take a hidden enemy for a part of something.
constexpr std::uint32_t kHelloMagic = 0x314C4548, kProtocol = 7;   // "HEL1"
constexpr std::uint32_t kGridCollisionClasses = 8, kEntityCollisionClasses = 5;   // the game's enums: GRIDCOLL_NONE..PITSONLY, ENTCOLL_NONE..ALL
constexpr std::uint32_t kNpcParts = 3, kNpcHidden = 4;                              // in Npc::linked
constexpr std::uint8_t kDevilRoom = 14, kAngelRoom = 15;                           // the game's RoomType of the two deals
constexpr std::uint8_t kHere = 0, kCompareOff = 1, kLive = 2;
constexpr int kHealthFields = 10, kMaxTaken = 8, kAnimationName = 24, kMaxNpcs = 48, kMaxDeaths = 16;
constexpr std::uint32_t kMaxCells = 96, kGridMapBytes = 56, kMaxBorn = 48, kMaxShots = 64, kMaxTears = 32, kMaxSlots = 8, kMaxPets = 24, kMaxEnemyBombs = 16,
    kMaxDrops = 64, kMaxDoors = 8, kMaxChunks = 31, kMaxHits = 48;

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
    // 1 has a parent, 2 has a child: a part of something, never created or removed by the lists. 4 (kNpcHidden): the host's
    // game does not show it (Entity's "visible", +0x171). The game hides an enemy for the first frames of its appearing
    // (state 1) and shows it from inside that state; a guest's twin whose state is written from outside leaves the state
    // without that and stays unseen - reported from a match through Steam as enemies that are there and cannot be seen.
    std::uint32_t linked;
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
// deal: 0, or the type of the room behind a door that leads to the floor's deal (room -1): 14 a devil's, 15 an angel's.
struct DoorState { std::uint16_t cell; std::uint8_t busted, deal; std::int32_t variant, state; };
struct SlotState { std::uint32_t seed, variant, subtype; float position[2]; std::int32_t state, prize, timeout, donation, trigger; char animation[kAnimationName]; };
struct World {
    std::uint32_t magic, sequence, room, count, deaths, clear, cells, shots, drops, dropsTotal, doors, npcTotal, hurt, enemyBombs, floor, slots, born;
    std::uint32_t dealSeed;   // what the level's generator of deals held before the host's game made this floor's deal room from it; 0: not known
    std::int32_t coins, bombs, keys;
    Npc npcs[kMaxNpcs]; std::uint32_t died[kMaxDeaths]; Cell grid[kMaxCells]; Shot shot[kMaxShots]; Drop drop[kMaxDrops]; DoorState door[kMaxDoors]; Shot enemyBomb[kMaxEnemyBombs]; SlotState slot[kMaxSlots]; Born bornCell[kMaxBorn]; std::uint8_t gridMap[kGridMapBytes];
};
// Tears first, then bombs. A bomb in a Shot: height = frames to the explosion, fallingSpeed = radius multiplier,
// fallingAccel = 1 for a fetus bomb, damage = explosion damage.
struct Pet { std::uint32_t seed, variant, subtype; float position[2], velocity[2]; std::uint32_t reserved; };
// A blow of the sender's own player at the world, as its game saw it land: kind 0 at an enemy (target: the enemy's seed as
// the host knows it), kinds 1..5 at a cell of the grid (target: the cell; the kind is the grid hook's number plus one, and
// flagsLow carries that call's argument). damage, the flags and the countdown are what the game handed to its TakeDamage;
// the source is named by its type, variant and its spawner's type - the entity itself is the owner's copy wherever it lands.
struct Hit { std::uint32_t number, room, target; float damage; std::uint32_t flagsLow, flagsHigh; std::int32_t countdown; std::uint16_t kind, sourceType, sourceVariant, spawnerType; };
struct Shots { std::uint32_t magic, controller, sequence, room, count, bombs, pets, hits; Shot shot[kMaxTears]; Pet pet[kMaxPets]; Hit hit[kMaxHits]; };
// session: which start of the sender's module this frame is of (see NewerSession); never 0.
struct FrameHeader { std::uint32_t magic; std::uint8_t kind, chunk, chunks, reserved; std::uint32_t sequence, total, session; };
struct Hello { std::uint32_t magic, protocol, rules; std::uint8_t controller, host, stage, reserved; };
#pragma pack(pop)
static_assert(sizeof(Taken) == 32 && sizeof(Body) == 76 + 4 * kHealthFields + kMaxTaken * 32 && sizeof(Npc) == 124 && sizeof(Pet) == 32 && sizeof(Cell) == 8 && sizeof(Shot) == 108 && sizeof(Hit) == 36 &&
              sizeof(Shots) == 32 + kMaxTears * 108 + kMaxPets * 32 + kMaxHits * 36 &&
              sizeof(Drop) == 44 && sizeof(DoorState) == 12 && sizeof(SlotState) == 64 && sizeof(Born) == 16 && sizeof(FrameHeader) == 20 && sizeof(Hello) == 16 &&
              sizeof(World) == 84 + kMaxNpcs * 124 + kMaxDeaths * 4 + kMaxCells * 8 + kMaxShots * 108 + kMaxDrops * 44 + kMaxDoors * 12 + kMaxEnemyBombs * 108 + kMaxSlots * 64 +
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
// When the installed module may start in the match the log tells of: the match is on, the own device and somebody else are
// known, and the log has said nothing new of it for kSettleMs. A match the module is done with - everybody left, not every
// player has the module, it could not start - is done with as it stood then: when a player comes or leaves afterwards, the
// module tries again. (Seen in a match through Steam: the one guest left and came back into the running match at the next
// floor, and the host's module stayed "off - the other players have left" to the end.)
constexpr std::uint64_t kSettleMs = 1500;
struct Done { std::uint32_t number = 0, roster = 0; };
bool MayStart(const MatchLog& match, const Done& done, std::uint64_t now);

// The game's lockstep counts a frame only when every player's input for that frame has come, and drops an input that comes
// for a frame already counted: with a real ping between the players every late packet is a frame that waits (measured in
// the user's match through Steam: 21 waits of over 50 ms in 25 s, the longest 351 ms). A remote player stands where its
// owner says anyway, so nothing has to wait for its input: what came is played out in the sender's own order, one record
// a frame - by the sender's frame numbers, never this game's, so the two games' counts may drift apart - and a frame for
// which nothing has come plays the last record again. The game takes a press from the change between two frames' buttons,
// so a record played twice presses nothing twice.
constexpr std::uint32_t kInputBytes = 12, kPlayoutSlots = 64, kPlayoutMostWaiting = 8, kPlayoutRestart = 600;
#pragma pack(push, 1)
struct InputRecord { std::uint32_t frame; std::uint8_t input[kInputBytes]; };   // as the game keeps and sends it: the frame, 16 buttons, four axes, two bytes
#pragma pack(pop)
static_assert(sizeof(InputRecord) == 16, "the game's input record");
constexpr std::uint8_t kNeutralInput[kInputBytes] = {0, 0, 0xff, 0x7f, 0xff, 0x7f, 0xff, 0x7f, 0xff, 0x7f, 0x3c, 0};   // what the game itself puts into an empty record
struct Playout {
    InputRecord slots[kPlayoutSlots]{}; bool filled[kPlayoutSlots]{};
    std::uint32_t next = 0, newest = 0, waited = 0; bool started = false;
    InputRecord last{0, {0, 0, 0xff, 0x7f, 0xff, 0x7f, 0xff, 0x7f, 0xff, 0x7f, 0x3c, 0}};
    std::uint32_t heard = 0, played = 0, repeated = 0, skipped = 0, stale = 0;   // repeated: frames the game's own lockstep would have waited for
    // A record as it came. One for a frame already played (or given up) is stale; a count that starts over is a new match.
    void Take(const InputRecord& record);
    // The input of this frame: the next record in the sender's order; the last one again while the next has not come (a
    // record that stays away while later ones are here is given up after a frame's grace); and never more than
    // kPlayoutMostWaiting frames behind the newest - what is jumped over keeps its buttons, so that no press is lost.
    InputRecord Play();
};

// A player's blows at the world are its owner's to tell. A remote player's shots are copies led by their owner's lists, a
// ping behind the owner's own; what the owner's tear hit - a fireplace, a poop, an enemy that has walked on since - the copy
// reaches late or never, for it ends with its owner's. Reported from a match with 100 ms and more between the players: a
// guest's tears did not put a fire out. So every game tells the blows of its own player as it saw them land, the host plays
// them on its world with the game's own TakeDamage (or the grid's Hurt and Destroy), and a blow of a copy counts nowhere.
// They ride with the shots, unreliably: every blow is said again for kHitResendFrames frames, numbered from 1, and a
// receiver plays each number once.
constexpr std::uint32_t kHitResendFrames = 10, kHitKinds = 6;
struct HitLog {
    Hit ring[kMaxHits]{}; std::uint32_t madeAt[kMaxHits]{}; std::uint32_t total = 0;
    void Note(Hit hit, std::uint32_t frame);                     // gives it its number
    std::uint32_t Recent(std::uint32_t frame, Hit* out) const;   // what is still to be said, oldest first; at most kMaxHits
};
// Numbers rising, a kind there is, a damage that can be one.
bool ValidHits(const Hit* hits, std::uint32_t count);
// Where in a list that came the blows not played yet begin (count: none). done: the last number played, 0 while nothing is
// known of this sender; a list that ends below it is of a sender that has started anew. lost: numbers that never came.
std::uint32_t FreshHits(const Hit* hits, std::uint32_t count, std::uint32_t& done, std::uint32_t& lost);

// A guest leads its room by the host's snapshots and lets the game's own code run it in between: an enemy's state, the frame
// of that state, its timers and its place are written from every snapshot, and between two of them the twin runs by itself.
// The host counts a snapshot every frame and sends every second one; while they come as evenly as they were sent, the twin
// has run just as far as the host had when the next one is written, and the writing changes nothing. With a ping that
// trembles they do not: a snapshot that comes j frames late turns the twin's clock j frames back, and the next one, on
// time again, j frames forward. What the enemy's code does at a certain frame of a state - a spawn, a shot, showing itself
// - is then done twice or not at all. Reported from a match with 100 ms and more between the players: enemies that vanish
// and come back (the twin that was made twice is removed without a death, the one never made is created late), and nothing
// of it with bosses or on one machine.
// So snapshots are played by a steady clock, as a remote player's input is. A snapshot's lead is this game's frame when it
// came minus its sequence; what a tenth of the last kLeadWindow leads stay under is the path itself (not the very least:
// snapshots that come while this game stands still look early), what lies above it is trembling. A
// snapshot's moment is its sequence plus the least lead plus a cushion - the trembling nine snapshots of ten stay under,
// between kLeastCushion and kMostCushion frames; it grows at once and shrinks a frame after kCushionPatience calm arrivals.
// In a frame the newest snapshot whose moment has come is played. One whose moment passed more than kLateSlack frames ago
// has been overtaken by this game's own running of the room and is dropped - unless nothing has been played for
// kStarveFrames: then the path has changed or the host's game has stood still, the newest is played and the clock starts from it.
constexpr std::uint32_t kLeadWindow = 32, kStarveFrames = 8, kCushionPatience = 64, kWorldQueue = 8;
constexpr std::int32_t kLeastCushion = 1, kMostCushion = 6, kLateSlack = 1;
struct Dejitter {
    std::int32_t leads[kLeadWindow]{}; std::uint32_t count = 0, next = 0, calm = 0; std::int32_t cushion = 2;
    void Came(std::uint32_t sequence, std::uint32_t frame);     // a snapshot came in this frame of this game
    void Rebase(std::uint32_t sequence, std::uint32_t frame);   // everything before is forgotten
    std::int32_t Least() const;
    // How many frames past its moment (negative: before it) a snapshot is in this frame.
    std::int32_t Past(std::uint32_t sequence, std::uint32_t frame) const { return static_cast<std::int32_t>(frame - sequence) - (Least() + cushion); }
};
// play: which of the waiting snapshots is played in this frame (-1: none); drop: how many of the oldest leave the queue,
// the played one among them; late: how many of those were dropped as overtaken; rebased: the clock started anew.
struct Picked { int play; std::uint32_t drop, late; bool rebased; };
// sequences rise; arrived: the frame each came in; idle: frames since a snapshot was last played.
Picked PickWorld(Dejitter& clock, const std::uint32_t* sequences, const std::uint32_t* arrived, std::uint32_t count, std::uint32_t frame, std::uint32_t idle);

// Who may come into a match that runs (the user's rule): a player whose save has at least every unlock of the session's
// shared save. The game's shared save is its members' achievements ANDed (its builder, RVA 0x51a450), so an unlock of the
// session is an achievement every member has. A game keeps what each lobby member told of its save in its network manager
// (a map by user id at +0x1068; read in a live game: the achievements are 642 bytes, one each, at +0x38 of an entry's value,
// as in the game's own PersistentGameData), and that is the form taken here.
constexpr std::uint32_t kSaveAchievements = 642;
// The session's unlocks from its members' achievements; of no members, none.
void SharedUnlocks(const std::uint8_t* const* members, std::uint32_t count, std::uint8_t* shared);
// How many unlocks of the session a save lacks: 0 and its player may come in.
std::uint32_t UnlocksLacking(const std::uint8_t* shared, const std::uint8_t* joiner);
}
