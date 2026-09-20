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
// What an enemy does is the host's too. Two simulations of one enemy choose different attacks - each has its own random
// numbers and its own idea of where the players stand - so the host also sends what its enemy's behaviour is made of (the
// fields the game's Lua calls State, StateFrame, ProjectileCooldown, V1, V2, I1, I2 and the target position), and a guest
// writes them into its twin every frame: the guest's own code then plays the host's attack, a frame or two later. An enemy
// the guest lacks for a third of a second is created there with the game's own Game::Spawn (RVA 0x28b20) under the host's
// seed, so that from then on it is matched by seed like any other.
//
// The state alone stops a guest's enemies from attacking: an enemy's own code starts the attack animation at the moment it
// changes state by itself, and the shot is an event of that animation - but the host is always a frame or two ahead, so
// the guest's enemy finds itself in the attack state without ever having started the animation. So the name of the
// animation the host's enemy plays travels too (the sprite is at Entity+0x48; its current animation, +0x34, begins with
// its name), and a guest whose twin plays another one starts the host's with the sprite's own Play (RVA 0xa380, forced).
// Frames are left alone: jumping over a frame could jump over the shot.
//
// Step 4, first event: damage to a player. A character's health is its owner's (the user's model), so the owner's hearts
// travel with its body and replace the copy's; a blow to a remote player's copy is ignored here (Entity_Player's
// TakeDamage, slot 8, RVA 0x3729d0) - the neighbour's enemies and shots stand a little differently and would hit a copy
// the owner never saw hit. When the owner has become a co-op ghost (the byte IsCoopGhost reads, +0x20a9) and the copy has
// not, the copy takes a blow nothing survives through the game's own TakeDamage, so the game's own death runs. A ghost
// that comes back to life at its owner is not followed yet, only counted.
//
// Second event: a change of room. In the game every simulation starts a transition by itself, when a player touches a
// door; a copy that misses the door by a step leaves one game in a room the other has left, and from then on nothing of
// one game applies to the other. So the room is an event: every body names its game's room and how many changes of room
// that game has seen (an epoch); a game that hears of a newer change follows it with the game's own request for a room
// transition (Game::StartRoomTransition, RVA 0x2fd7c0, which only latches the request at Game+0x1b83c). On equal epochs a
// guest follows the host.
//
// Third event: a room is cleared. The game clears a room the moment no enemy is left alive in it (Room::TriggerClear, RVA
// 0x4068f0: doors open, the award drops). On a guest that moment can come while the host still fights - an enemy the host
// saw split in two is created here a third of a second later - so a guest's own clearing is held until the host's room is
// clear (bit 0 of the room descriptor's flags, +0x44), and then done with the same function. TriggerClear is not virtual:
// its first six bytes become a jump, like the service factory's. Without word from the host for two seconds a guest
// clears by itself rather than stay behind closed doors.
//
// Fourth event: a player takes something. The game decides that where the player touches the pickup (Entity_Pickup's
// collision, slot 23 of its table, RVA 0x2e8ae0: coins, hearts, keys, chests, pedestals, shop items all go through it). A
// copy led along its owner's path touches by luck - it missed a chest its owner opened - so a copy's touch does nothing
// here, and the owner's game reports every touch that changed the pickup (kind, seed, price, gone); the other games then
// run the same collision for their copy of that player against their twin of the pickup: the game's own code gives the
// coin, opens the chest, takes the price. The last eight takings ride in every body, numbered, so a lost body loses none.
//
// Death, earlier: the owner's body also says that its player is dying (the entity's dead flag, +0x173, set while the death
// animation plays), not only that it has become a ghost - a copy that waited for the ghost died a whole animation late. A
// copy still dying when its owner lives again (an extra life) is brought back with the game's own Revive (RVA 0x3a2220).
//
// The room's grid - rocks, poop, TNT, webs - is the host's as well (the user's rule: block it at the others, take it from
// the host). Each game broke them by its own shots, and poop even takes a random extra from every hit, so one game's rock
// stood where the other's was rubble. A guest's own breaking is held (the classes' Hurt and Destroy, slots 4 and 5 of
// their tables) while the host is heard and in the same room; the host's world names every cell that is no longer as the
// room was built (Room+0x24: 448 cells; a cell's type +4, variant +8, state +0xc, sprite +0x40), and a guest brings its
// own cell there with the game's own Destroy (the Lua wrapper, RVA 0x45de20) or, for poop and TNT on the way, the state
// and poop's "State%d" animation. The guest's bomb breaks a rock when the host's copy of that bomb has - a round trip late.
//
// A fireplace (entity type 33) is an enemy that never dies of shots: it goes out at a hit point or less. Cutting a guest's
// lethal blow down to a remainder - the rule for enemies - put a guest's fire out while the host's still burned. So a
// guest's blows to a fireplace do nothing while the host is heard, and the fireplace travels with the host's enemies
// whatever its maximum is: its hit points, state and animation are the host's.
//
// Shots. An enemy's projectiles are the host's, a player's tears are its owner's: each game rolled them by itself, so a
// boss's volley was there in one game and not in the other, and a tear's random effect struck for the owner and not for
// the copy. The host's world lists its living projectiles (Entity_Projectile, table 0x764990), every game lists its own
// player's living tears (Entity_Tear, table 0x764eac, spawned by that player) in a packet of their own, 'SHT1'. A game that
// receives a list keeps exactly it: a shot it has is set to the owner's place, flight, size, damage, flags and colour; a
// shot it lacks is created with the game's own Spawn under the owner's seed; a shot of its own making, which no list
// names, is removed (the entity's own Remove, slot 10); a shot that is gone at its owner dies here (what the game's Die()
// does: the dead flag). A shot that ended here first is not created again: the seeds a list has brought are remembered.
// While its owner sends tears a copy does not shoot: its weapon's fire delay (player +0x13dc, else +0x13e0; +0xc) is held
// up. Tears of familiars, lasers, knives and bombs are still every game's own.
//
// What lies in the room is the host's too (docs/j460-world-replication-map.md):
// - pickups. The host's world lists its living pickups (seed, kind, place, price, frames left, the group of which one may
//   be taken, shop slot), and a guest keeps exactly that list: its own pickup of a seed gets the host's place and price; one
//   the host has and the guest never had is created with the game's own Spawn under the host's seed (never with subtype 0,
//   which makes the game roll one); one of the guest's own making is removed after a few snapshots; one the host no longer
//   has is removed later, so that the owner's taking (above) runs first. A pedestal holding another item than the host's
//   gets the host's with the pickup's own Morph (RVA 0x2e30a0). A list that did not fit removes nothing.
// - the team's coins, bombs and keys (every player carries the same numbers: +0x1368, +0x1364, +0x135c) are the host's,
//   except for a second and a half after they changed here - the owner's own taking or buying is ahead of the host.
// - doors (GridEntity_Door, table 0x768698, in a grid cell of type 16): variant +8, state +0xc, busted +0x391. A door that
//   differs from the host's for a few snapshots - and did not just change here - gets the host's three values and the
//   door's own refresh (RVA 0x30ee40), which is how the game's Close, Bar and SetLocked end.
// - the trap enemies the game keeps in a room (Room::IsPersistentRoomEntity, RVA 0x3ed2a0: types 42, 44 with a variant,
//   202, 203, 218, 235, 236, 804, 809, 852, 877, 893, 965) travel with the host's enemies whatever their maximum.
// - a player's bombs are its owner's, like its tears (Entity_Bomb, table 0x7670f4, spawned by that player; Dr. Fetus'
//   too): where and when a bomb goes off is what breaks the host's world, so the copy's bomb must be the owner's. They
//   ride behind the tears in the same packet: frames to the explosion (+0x410, and its twin +0x414, as the game's
//   SetExplosionCountdown writes them), damage +0x418, flags +0x428, fetus +0x448, radius +0x44c. A bomb that is gone at
//   its owner goes off here at once (countdown 0) rather than vanish. Troll bombs have no player behind them: untouched.
// Every rule of this module can be left out when it starts (the third word of the configuration, a mask in hex), so that
// a rule that misbehaves in a live run is switched off without a rebuild.
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
#include <cstdio>
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
constexpr std::uintptr_t kGame = 0x871678, kRoom = 0x18300, kRoomIndex = 0x18304, kDimension = 0x1830c, kListData = 0x125c, kListCount = 0x1264;
constexpr std::uintptr_t kTransition = 0x2fd7c0, kRoomTransition = 0x1b83c;
constexpr std::array<std::uint8_t, 6> kTransitionEntry{0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68};
constexpr int kGridWidth = 13, kFollowRetryFrames = 60; constexpr unsigned kFade = 1;
constexpr std::uintptr_t kKill = 0x45dc30, kNpcDamage = 0x2d60a0, kPlayerDamage = 0x3729d0, kGhost = 0x20a9;
// Hearts as the game's own getters read them: containers, red, eternal, soul, black mask, two words that follow them,
// bone (+0x1d88), rotten (+0x1da4), golden (+0x194c).
constexpr std::uintptr_t kHealth[] = {0x1340, 0x1344, 0x1348, 0x134c, 0x1350, 0x1354, 0x1358, 0x1d88, 0x1da4, 0x194c};
constexpr int kHealthFields = 10, kDeathRetryFrames = 30;
constexpr std::array<std::uint8_t, 10> kKillEntry{0x55, 0x8b, 0xec, 0x83, 0xec, 0x28, 0xf3, 0x0f, 0x10, 0x0d};
constexpr std::uint32_t kBodyMagic = 0x31524c50, kWorldMagic = 0x314e4c57;  // "PLR1", "WLN1"
constexpr int kControllers = 8, kMaxNpcs = 48, kMaxDeaths = 16, kDeathFrames = 90, kOrphanSnapshots = 20;
constexpr std::uintptr_t kType = 0x28, kVariant = 0x2c, kSubtype = 0x30, kTarget = 0x334;
// Entity_NPC, from the Lua property registration of J460.
constexpr std::uintptr_t kState = 0xb64, kStateFrame = 0x410, kCooldown = 0xba0, kV1 = 0xbb0, kV2 = 0xbb8, kI1 = 0xbc0, kI2 = 0xbc4;
// The sprite: Entity+0x48 (Entity::GetSprite, RVA 0x35dd0; read in live memory: +0x7c points at 'WalkVert', 'Shake', ...);
// its current animation +0x34 (an object that begins with its name, a std::string).
constexpr std::uintptr_t kSprite = 0x48, kAnimation = 0x34, kSpritePlay = 0xa380;
constexpr std::array<std::uint8_t, 8> kSpritePlayEntry{0x55, 0x8B, 0xEC, 0x80, 0x7D, 0x0C, 0x00, 0x56};
constexpr int kAnimationName = 24;
constexpr std::uintptr_t kSpawn = 0x28b20, kTriggerClear = 0x4068f0, kDescriptor = 0x4, kRoomFlags = 0x44;
constexpr std::array<std::uint8_t, 9> kClearEntry{0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x53, 0x8B, 0xD9};  // the first six move as they are
constexpr std::uint64_t kHostSilenceMs = 2000;
constexpr std::array<std::uint8_t, 8> kSpawnEntry{0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x6A, 0x00};
constexpr int kSpawnAfterSnapshots = 10, kSpawnRetryFrames = 90, kMaxSpawnsPerFrame = 4;
constexpr std::uint64_t kFreshMs = 250;
// Entity_Pickup: the table at RVA 0x767f24, collision in slot 23; price +0x534, the frames it cannot be taken yet +0x54c.
constexpr std::uintptr_t kPickupTable = 0x767f24, kPickupCollision = 0x2e8ae0, kPrice = 0x534, kWait = 0x54c, kRevive = 0x3a2220;
constexpr std::array<std::uint8_t, 8> kReviveEntry{0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC};
// The grid: Room+0x24 holds 448 cells; GridEntity: type +4, variant +8, state +0xc, sprite +0x40.
constexpr std::uintptr_t kGrid = 0x24, kGridType = 0x4, kGridVariant = 0x8, kGridState = 0xc, kGridSprite = 0x40, kGridDestroy = 0x45de20;
constexpr std::uintptr_t kRockTable = 0x768738, kPoopTable = 0x768648, kTntTable = 0x769300, kWebTable = 0x769558;
constexpr std::array<std::uint8_t, 8> kGridDestroyEntry{0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x2C, 0x8B, 0x01};
constexpr std::uint32_t kGridCells = 448, kMaxCells = 96, kFireplace = 33;
// Shots: both classes keep height +0x410, falling speed +0x414, falling acceleration +0x418 (the Lua accessors of J460).
// Projectile: damage +0x41c, scale +0x420, flags +0x438 (64 bits). Tear: scale +0x424 through its own setter (RVA 0x276120),
// flags +0x428 (128 bits), damage = the entity's collision damage +0x388. Any entity: who spawned it +0x3c8 (a counted
// reference: only Spawn sets it), colour +0xf0 (the sprite's, 44 bytes).
constexpr std::uintptr_t kProjectileTable = 0x764990, kTearTable = 0x764eac, kHeight = 0x410, kFallingSpeed = 0x414, kFallingAccel = 0x418;
constexpr std::uintptr_t kProjectileDamage = 0x41c, kProjectileScale = 0x420, kProjectileFlags = 0x438, kTearScale = 0x424, kTearFlags = 0x428, kTearSetScale = 0x276120;
constexpr std::uintptr_t kCollisionDamage = 0x388, kSpawner = 0x3c8, kColor = 0xf0, kWeapon = 0x13dc, kWeaponOther = 0x13e0, kFireDelay = 0xc;
constexpr std::array<std::uint8_t, 8> kTearSetScaleEntry{0x55, 0x8B, 0xEC, 0xF3, 0x0F, 0x10, 0x45, 0x08};
constexpr std::uint32_t kProjectile = 9, kTear = 2, kShotsMagic = 0x31544853, kMaxShots = 64, kMaxTears = 32, kKnownShots = 256, kMaxShotSpawns = 24;   // "SHT1"
constexpr std::uintptr_t kBombTable = 0x7670f4, kBombCountdown = 0x410, kBombCountdownTwin = 0x414, kBombDamage = 0x418, kBombFlags = 0x428, kBombFetus = 0x448, kBombRadius = 0x44c;
constexpr std::uint32_t kBomb = 4; constexpr int kOfEnemy = 0, kOfTear = 1, kOfBomb = 2;   // the kinds of shot
constexpr std::uint64_t kTearsHeardMs = 3000; constexpr float kHeldFireDelay = 5.0f;
// Pickups: the group of which one may be taken +0x528, price +0x534, shop slot +0x53c, frames left +0x540 (Lua accessors).
constexpr std::uintptr_t kOptions = 0x528, kShopItemId = 0x53c, kTimeout = 0x540, kPickupMorph = 0x2e30a0;
constexpr std::array<std::uint8_t, 8> kPickupMorphEntry{0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x95, 0xB0};
constexpr std::uint32_t kPickup = 5, kCollectible = 100, kMaxDrops = 64, kAges = 128, kMaxDropSpawns = 16;
constexpr std::uint32_t kDropMadeHereSnapshots = 6, kDropGoneSnapshots = 20, kDropItemSnapshots = 15;
constexpr std::uintptr_t kKeys = 0x135c, kBombs = 0x1364, kCoins = 0x1368, kPlayers = 0x1baa8; constexpr std::uint32_t kCountersHoldFrames = 45;
constexpr std::uintptr_t kDoorTable = 0x768698, kDoorBusted = 0x391, kDoorRefresh = 0x30ee40; constexpr std::uint32_t kGridDoor = 16, kMaxDoors = 8;
constexpr std::array<std::uint8_t, 8> kDoorRefreshEntry{0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x6D, 0xC9};
constexpr std::uint32_t kDoorSnapshots = 4, kDoorHoldFrames = 30;
// The rules that can be left out: the third word of the configuration is their mask in hex.
constexpr std::uint32_t kFollow = 1, kBehaviour = 2, kClear = 4, kTaken = 8, kGridRule = 16, kFire = 32, kProjectiles = 64, kTears = 128, kDrops = 256,
    kCounters = 512, kDoors = 1024, kTraps = 2048, kBombsRule = 4096, kAllRules = 0x1FFF;
constexpr int kMaxTaken = 8, kTakenRetryFrames = 30, kAliveBodies = 5; constexpr float kTakenReach = 120.0f;
#pragma pack(push, 1)
struct Taken { std::uint32_t number, room, seed, variant, subtype, low; float position[2]; };
struct Body {
    std::uint32_t magic, controller, sequence, room; float position[2], velocity[2]; std::uint32_t ghost; std::int32_t health[kHealthFields];
    std::uint32_t dimension, roomEpoch, host, dying, takenTotal; Taken taken[kMaxTaken];
};
struct Npc {
    std::uint32_t seed, type, variant, subtype; float position[2], velocity[2], hitPoints;
    std::int32_t state, stateFrame, cooldown; float v1[2], v2[2]; std::int32_t i1, i2; float target[2];
    char animation[kAnimationName];   // empty: none, or a name too long to carry
};
struct Cell { std::uint16_t index, type; std::int32_t state; };
struct Shot {
    std::uint32_t seed, variant, subtype; float position[2], velocity[2], height, fallingSpeed, fallingAccel, scale, damage;
    std::uint64_t flags[2]; float color[11];
};
struct Drop { std::uint32_t seed, variant, subtype; float position[2], velocity[2]; std::int32_t price, timeout, options, shopItemId; };
struct DoorState { std::uint16_t cell; std::uint8_t busted, reserved; std::int32_t variant, state; };
struct World {
    std::uint32_t magic, sequence, room, count, deaths, clear, cells, shots, drops, dropsTotal, doors, npcTotal; std::int32_t coins, bombs, keys;
    Npc npcs[kMaxNpcs]; std::uint32_t died[kMaxDeaths]; Cell grid[kMaxCells]; Shot shot[kMaxShots]; Drop drop[kMaxDrops]; DoorState door[kMaxDoors];
};
// Tears first, then bombs. A bomb in a Shot: height = frames to the explosion, fallingSpeed = radius multiplier,
// fallingAccel = 1 for a fetus bomb, damage = explosion damage.
struct Shots { std::uint32_t magic, controller, sequence, room, count, bombs; Shot shot[kMaxTears]; };
// What a reader outside copies: generation is odd while the content is being written.
struct Published { std::uint32_t magic, generation; Body body; };
struct PublishedWorld { std::uint32_t magic, generation; World world; };
struct PublishedShots { std::uint32_t magic, generation; Shots shots; };
struct Stats {
    std::uint32_t published, received, applied, stale, rejected, otherRoom;
    std::uint32_t worldPublished, worldReceived, worldApplied, npcMatched, npcOnlyHost, npcOnlyLocal, npcKilled, hitPointFixes, deathsHeld, npcPaired, npcRemoved;
    std::uint32_t healthFixes, copyDamageIgnored, copyDeaths, copyRevivalsMissed, roomFollows, roomFollowFailures, stateFixes, npcSpawned, npcSpawnFailures, clearsHeld, clearsFromHost;
    std::uint32_t copyRevivals, copyTouchesIgnored, taken, takenApplied, takenMissed, animationFixes, gridHeld, gridFixes, gridMismatch, fireHeld;
    std::uint32_t projectilesMade, projectilesEnded, projectilesDropped, tearsSent, tearsMade, tearsEnded, tearsDropped, fireHolds;
    std::uint32_t dropsMade, dropsRemoved, dropsMorphed, dropsSkipped, counterFixes, doorFixes, doorMismatch, bombsMade, bombsEnded, bombsDropped;
    float correctionSum, correctionMax, npcCorrectionSum, npcCorrectionMax;
};
#pragma pack(pop)
static_assert(sizeof(Taken) == 32 && sizeof(Body) == 56 + 4 * kHealthFields + kMaxTaken * 32 && sizeof(Npc) == 104 && sizeof(Cell) == 8 && sizeof(Shot) == 108 && sizeof(Shots) == 24 + kMaxTears * 108 &&
              sizeof(Drop) == 44 && sizeof(DoorState) == 12 &&
              sizeof(World) == 60 + kMaxNpcs * 104 + kMaxDeaths * 4 + kMaxCells * 8 + kMaxShots * 108 + kMaxDrops * 44 + kMaxDoors * 12 && sizeof(Shots) != sizeof(Body), "wire layout");
using WithDevice = int(__thiscall*)(void*, int, void*, void*, void*, int*);
using PlayerUpdate = void(__thiscall*)(void*);
using NpcDamage = char(__thiscall*)(void*, float, std::uint32_t, std::uint32_t, void*, int);
using Transition = void(__thiscall*)(void*, int, int, unsigned, void*, int);
using TriggerClear = void(__thiscall*)(void*, char);
using Collision = bool(__thiscall*)(void*, void*, std::uint32_t);
using SpritePlay = void(__thiscall*)(void*, const char*, bool);
using GridChange = std::uint32_t(__thiscall*)(void*, std::uint32_t, void*);   // Hurt(damage, source), Destroy(immediate, source)
using GridDestroy = void(__thiscall*)(void*, std::uint32_t);
using TearSetScale = void(__thiscall*)(void*, float);
using PickupMorph = void(__thiscall*)(void*, int, int, int, std::uint32_t, std::uint32_t, std::uint32_t);   // type, variant, subtype, keep price, keep seed, ignore modifiers
using Spawn = void*(__thiscall*)(void*, std::uint32_t, std::uint32_t, const float*, const float*, void*, std::uint32_t, std::uint32_t);
std::uintptr_t base = 0;
void** slot = nullptr; void** playerSlot = nullptr; void** damageSlot = nullptr; void** playerDamageSlot = nullptr;
WithDevice original = nullptr; PlayerUpdate originalPlayer = nullptr; NpcDamage originalDamage = nullptr, originalPlayerDamage = nullptr;
std::uint32_t deathTried[8]{};  // per controller: the frame a copy was last sent to its death
std::uint32_t aliveSeen[8]{};   // per controller: bodies in a row that said the owner lives
// Takings: this game's own, numbered from 1; per remote controller the last one dealt with and since when the next is tried.
void** pickupSlot = nullptr; Collision originalCollision = nullptr; bool applyingTaken = false;
Taken takenLog[kMaxTaken]{}; std::uint32_t takenTotal = 0, takenDone[8]{}, takenSince[8]{}; bool takenKnown[8]{};
// Rooms: the room this game was in a frame ago, its epoch, the newest epoch heard of, and the room being followed to.
std::uint32_t lastRoom = 0xfffffffe, roomEpoch = 0, heardEpoch = 0, followRoom = 0xfffffffe, followTried = 0; bool following = false;
int ownController = -1; bool host = false;
SRWLOCK lifecycle = SRWLOCK_INIT, inboxLock = SRWLOCK_INIT;
std::atomic<bool> running{false};
std::atomic<unsigned> counters[2]{};  // reads of the own player's input answered by the keyboard; all other reads
Published published{}; PublishedWorld publishedWorld{}; PublishedShots publishedShots{};
Stats stats{};
struct Inbox { Body body{}; std::uint64_t at = 0; std::uint32_t appliedSequence = 0; };
struct ShotsInbox { Shots shots{}; std::uint64_t at = 0; std::uint32_t applied = 0; };
ShotsInbox shotsInbox[kControllers];
// The seeds a list has brought here: a shot of such a seed that is missing here has ended here, and is not made again.
struct Known { std::uint32_t seeds[kKnownShots]; std::uint32_t next; };
Known knownProjectiles{}, knownTears[kControllers]{}, knownBombs[kControllers]{}, knownDrops{};
struct Age { std::uint32_t seed, age; } dropAges[kAges]{}; std::uint32_t dropAgeCount = 0;
std::uint64_t tearsHeardAt[kControllers]{}; std::uint32_t shotsSequence = 0;
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
// The host's enemies this game has no twin for: for how many snapshots, and when one was last created for it.
struct Missing { std::uint32_t seed, age, spawnedAt; } missing[kMaxNpcs]{}; std::uint32_t missingCount = 0;
// Clearing: the way into the game's own TriggerClear past this module's jump, and what the host last said of which room.
std::uint8_t* clearEntry = nullptr; std::uint8_t* clearTrampoline = nullptr; bool clearHooked = false;
std::atomic<std::uint32_t> hostClearRoom{0xfffffffe}, hostClear{0}; std::atomic<std::uint64_t> hostHeardAt{0};
// The grid: what a guest holds - poop's Hurt, the rock's Destroy, TNT's both, the web's Destroy.
struct GridHook { std::uintptr_t table; int slot; std::uintptr_t function; void* replacement; void** at; void* original; bool installed; };
GridHook gridHooks[5] = {{kPoopTable, 4, 0x315330}, {kRockTable, 5, 0x3197d0}, {kTntTable, 4, 0x31f250}, {kTntTable, 5, 0x31f2b0}, {kWebTable, 5, 0x321a10}};
bool applyingGrid = false;
bool GridHeld(); bool HostRules();
std::uint32_t rules = kAllRules;
inline bool On(std::uint32_t rule) { return (rules & rule) != 0; }
// Pickups: the seeds the host's list has brought, for how many snapshots a pickup has been out of step; the counters as
// they were here a frame ago and until when the host's wait; per door what it was here and for how long it differs.
std::int32_t lastCounters[3] = {-1, -1, -1}; std::uint32_t countersHeldUntil = 0;
struct DoorMemory { std::uint32_t cell; std::int32_t variant, state; std::uint8_t busted; std::uint32_t differing, heldUntil; } doorMemory[kMaxDoors]{}; std::uint32_t doorMemoryCount = 0;

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

// The game's own Room::IsPersistentRoomEntity (RVA 0x3ed2a0), as decompiled: the traps a room keeps.
bool PersistentTrap(std::uint32_t type, std::uint32_t variant) {
    switch (type) {
        case 42: case 202: case 203: case 218: case 235: case 236: case 804: case 809: case 852: case 877: case 893: case 965: return true;
        case 44: return variant != 0;
        default: return false;
    }
}

bool LivingNpc(std::uintptr_t entity) {
    return At<std::uintptr_t>(entity) == base + kNpcTable && At<std::uint8_t>(entity + kExists) && !At<std::uint8_t>(entity + kDead) &&
        (At<float>(entity + kMaxHitPoints) > 0 || At<std::uint32_t>(entity + kType) == kFireplace ||
         (On(kTraps) && PersistentTrap(At<std::uint32_t>(entity + kType), At<std::uint32_t>(entity + kVariant))));
}

// The name of the animation an entity's sprite plays, into a zeroed buffer; nothing when there is none or it does not fit.
void AnimationOf(std::uintptr_t entity, char* name) {
    std::memset(name, 0, kAnimationName);
    const auto animation = At<std::uintptr_t>(entity + kSprite + kAnimation);
    if (animation < 0x10000) return;   // none, or not a pointer at all
    const auto size = At<std::uint32_t>(animation + 0x10), capacity = At<std::uint32_t>(animation + 0x14);
    const char* text = capacity > 0xf ? At<const char*>(animation) : reinterpret_cast<const char*>(animation);
    if (text && size && size < static_cast<std::uint32_t>(kAnimationName)) std::memcpy(name, text, size);
}

bool Knows(const Known& known, std::uint32_t seed) { for (const auto s : known.seeds) if (s == seed) return true; return false; }
void Learn(Known& known, std::uint32_t seed) { if (seed && !Knows(known, seed)) known.seeds[known.next++ % kKnownShots] = seed; }

bool LivingShot(std::uintptr_t entity, int kind) {
    return At<std::uintptr_t>(entity) == base + (kind == kOfBomb ? kBombTable : kind == kOfTear ? kTearTable : kProjectileTable) && At<std::uint8_t>(entity + kExists) &&
        !At<std::uint8_t>(entity + kDead);
}

void ShotOf(std::uintptr_t entity, int kind, Shot& shot) {
    const bool tear = kind == kOfTear;
    shot = Shot{};
    shot.seed = At<std::uint32_t>(entity + kSeed); shot.variant = At<std::uint32_t>(entity + kVariant); shot.subtype = At<std::uint32_t>(entity + kSubtype);
    std::memcpy(shot.position, reinterpret_cast<void*>(entity + kPosition), 8); std::memcpy(shot.velocity, reinterpret_cast<void*>(entity + kVelocity), 8);
    std::memcpy(shot.color, reinterpret_cast<void*>(entity + kColor), sizeof(shot.color));
    if (kind == kOfBomb) {
        shot.height = static_cast<float>(At<std::int32_t>(entity + kBombCountdown)); shot.fallingSpeed = At<float>(entity + kBombRadius);
        shot.fallingAccel = At<std::uint8_t>(entity + kBombFetus) ? 1.0f : 0.0f; shot.damage = At<float>(entity + kBombDamage);
        std::memcpy(shot.flags, reinterpret_cast<void*>(entity + kBombFlags), 16);
        return;
    }
    shot.height = At<float>(entity + kHeight); shot.fallingSpeed = At<float>(entity + kFallingSpeed); shot.fallingAccel = At<float>(entity + kFallingAccel);
    shot.scale = At<float>(entity + (tear ? kTearScale : kProjectileScale)); shot.damage = At<float>(entity + (tear ? kCollisionDamage : kProjectileDamage));
    std::memcpy(shot.flags, reinterpret_cast<void*>(entity + (tear ? kTearFlags : kProjectileFlags)), tear ? 16 : 8);
}

void ShotOnto(std::uintptr_t entity, int kind, const Shot& shot) {
    const bool tear = kind == kOfTear;
    std::memcpy(reinterpret_cast<void*>(entity + kPosition), shot.position, 8); std::memcpy(reinterpret_cast<void*>(entity + kVelocity), shot.velocity, 8);
    std::memcpy(reinterpret_cast<void*>(entity + kColor), shot.color, sizeof(shot.color));
    if (kind == kOfBomb) {
        const auto frames = static_cast<std::int32_t>(shot.height);
        At<std::int32_t>(entity + kBombCountdown) = frames; At<std::int32_t>(entity + kBombCountdownTwin) = frames;
        At<float>(entity + kBombRadius) = shot.fallingSpeed; At<std::uint8_t>(entity + kBombFetus) = shot.fallingAccel != 0.0f; At<float>(entity + kBombDamage) = shot.damage;
        std::memcpy(reinterpret_cast<void*>(entity + kBombFlags), shot.flags, 16);
        return;
    }
    At<float>(entity + kHeight) = shot.height; At<float>(entity + kFallingSpeed) = shot.fallingSpeed; At<float>(entity + kFallingAccel) = shot.fallingAccel;
    if (tear) {
        if (At<float>(entity + kTearScale) != shot.scale) reinterpret_cast<TearSetScale>(base + kTearSetScale)(reinterpret_cast<void*>(entity), shot.scale);
        At<float>(entity + kCollisionDamage) = shot.damage;
    } else { At<float>(entity + kProjectileScale) = shot.scale; At<float>(entity + kProjectileDamage) = shot.damage; }
    std::memcpy(reinterpret_cast<void*>(entity + (tear ? kTearFlags : kProjectileFlags)), shot.flags, tear ? 16 : 8);
}

// This room's shots of one kind (and of one spawner, if given) made exactly the list that came.
void ApplyShots(std::uintptr_t room, const Shot* shots, std::uint32_t count, bool complete, int kind, std::uintptr_t spawner, Known& known,
                std::uint32_t& made, std::uint32_t& ended, std::uint32_t& dropped) {
    const auto data = At<std::uintptr_t>(room + kListData); const auto listCount = At<std::uint32_t>(room + kListCount);
    if (!data || listCount > 4096 || count > kMaxShots) return;
    bool taken[kMaxShots]{}; std::uintptr_t remove[kMaxShots]; std::uint32_t removeCount = 0;
    for (std::uint32_t i = 0; i < listCount; ++i) {
        const auto entity = At<std::uintptr_t>(data + i * sizeof(std::uintptr_t));
        if (!entity || !LivingShot(entity, kind) || (spawner && At<std::uintptr_t>(entity + kSpawner) != spawner)) continue;
        const auto seed = At<std::uint32_t>(entity + kSeed); std::uint32_t n = 0;
        while (n < count && (taken[n] || shots[n].seed != seed)) ++n;
        if (n < count) { taken[n] = true; ShotOnto(entity, kind, shots[n]); Learn(known, seed); continue; }
        if (!complete) continue;   // the list did not fit: who is missing from it may well live
        if (Knows(known, seed)) {   // gone at its owner: a bomb goes off now, anything else dies as by the game's Die()
            if (kind == kOfBomb) { At<std::int32_t>(entity + kBombCountdown) = 0; At<std::int32_t>(entity + kBombCountdownTwin) = 0; } else At<std::uint8_t>(entity + kDead) = 1;
            ended++;
        }
        else if (removeCount < kMaxShots) remove[removeCount++] = entity;             // of this game's own making
    }
    for (std::uint32_t r = 0; r < removeCount; ++r) {
        const auto entity = remove[r]; reinterpret_cast<PlayerUpdate>(At<void**>(entity)[10])(reinterpret_cast<void*>(entity)); dropped++;
    }
    const auto game = At<std::uintptr_t>(base + kGame); std::uint32_t spawned = 0;
    for (std::uint32_t n = 0; n < count && game && spawned < kMaxShotSpawns; ++n) {
        if (taken[n] || !shots[n].seed || Knows(known, shots[n].seed)) continue;
        Learn(known, shots[n].seed);
        const auto entity = reinterpret_cast<std::uintptr_t>(reinterpret_cast<Spawn>(base + kSpawn)(reinterpret_cast<void*>(game),
            kind == kOfBomb ? kBomb : kind == kOfTear ? kTear : kProjectile, shots[n].variant,
            shots[n].position, shots[n].velocity, reinterpret_cast<void*>(spawner), shots[n].subtype, shots[n].seed));
        if (entity && LivingShot(entity, kind)) { ShotOnto(entity, kind, shots[n]); made++; ++spawned; }
    }
}

// Every game: its own player's living tears.
void PublishShots(std::uintptr_t player, std::uintptr_t room, std::uint32_t roomIndex) {
    const auto data = At<std::uintptr_t>(room + kListData); const auto count = At<std::uint32_t>(room + kListCount);
    if (!data || count > 4096) return;
    publishedShots.generation |= 1;
    auto& shots = publishedShots.shots; shots.magic = kShotsMagic; shots.controller = static_cast<std::uint32_t>(ownController); shots.sequence = ++shotsSequence;
    shots.room = roomIndex; shots.count = 0; shots.bombs = 0;
    for (std::uint32_t i = 0; i < count && shots.count < kMaxTears; ++i) {
        const auto entity = At<std::uintptr_t>(data + i * sizeof(std::uintptr_t));
        if (entity && LivingShot(entity, kOfTear) && At<std::uintptr_t>(entity + kSpawner) == player) ShotOf(entity, kOfTear, shots.shot[shots.count++]);
    }
    for (std::uint32_t i = 0; i < count && shots.count + shots.bombs < kMaxTears; ++i) {
        const auto entity = At<std::uintptr_t>(data + i * sizeof(std::uintptr_t));
        if (entity && LivingShot(entity, kOfBomb) && At<std::uintptr_t>(entity + kSpawner) == player) ShotOf(entity, kOfBomb, shots.shot[shots.count + shots.bombs++]);
    }
    stats.tearsSent += shots.count;
    publishedShots.generation++;
}

// A remote player's tears over its copy's, and the copy's own fire held while they come.
void ApplyTears(std::uintptr_t player, int controller, std::uintptr_t room, std::uint32_t roomIndex) {
    static Shots shots;   // the game's thread only
    bool fresh = false; const auto now = GetTickCount64();
    AcquireSRWLockShared(&inboxLock);
    const auto& box = shotsInbox[controller];
    if (box.shots.sequence && box.shots.sequence != box.applied && now - box.at <= kFreshMs) { shots = box.shots; fresh = true; }
    ReleaseSRWLockShared(&inboxLock);
    if (fresh && shots.room == roomIndex) {
        const bool complete = shots.count + shots.bombs < kMaxTears;
        if (On(kTears)) ApplyShots(room, shots.shot, shots.count, complete, kOfTear, player, knownTears[controller], stats.tearsMade, stats.tearsEnded, stats.tearsDropped);
        if (On(kBombsRule))
            ApplyShots(room, shots.shot + shots.count, shots.bombs, complete, kOfBomb, player, knownBombs[controller], stats.bombsMade, stats.bombsEnded, stats.bombsDropped);
        for (std::uint32_t b = 0; b < shots.bombs; ++b) if (shots.shot[shots.count + b].fallingAccel != 0.0f) tearsHeardAt[controller] = now;   // fetus bombs are this player's fire
        AcquireSRWLockExclusive(&inboxLock); shotsInbox[controller].applied = shots.sequence; ReleaseSRWLockExclusive(&inboxLock);
        if (shots.count) tearsHeardAt[controller] = now;
    }
    if (On(kTears) && tearsHeardAt[controller] && now - tearsHeardAt[controller] <= kTearsHeardMs) {
        auto weapon = At<std::uintptr_t>(player + kWeapon); if (!weapon) weapon = At<std::uintptr_t>(player + kWeaponOther);
        if (weapon && At<float>(weapon + kFireDelay) < kHeldFireDelay) { At<float>(weapon + kFireDelay) = kHeldFireDelay; stats.fireHolds++; }
    }
}

// 1 rock, 2 poop, 3 TNT, 4 web; 0 for every other cell.
int GridKind(std::uintptr_t grid) {
    const auto table = At<std::uintptr_t>(grid) - base;
    return table == kRockTable ? 1 : table == kPoopTable ? 2 : table == kTntTable ? 3 : table == kWebTable ? 4 : 0;
}
// No longer as the room was built: a rock starts in state 1, the others in 0.
bool GridChanged(std::uintptr_t grid) {
    const int kind = GridKind(grid); const auto state = At<std::int32_t>(grid + kGridState);
    return kind == 1 ? state != 1 : kind ? state != 0 : false;
}

bool LivingPickup(std::uintptr_t entity) {
    return At<std::uintptr_t>(entity) == base + kPickupTable && At<std::uint8_t>(entity + kExists) && !At<std::uint8_t>(entity + kDead);
}

// Host: the living enemies of the room, and who of last frame's is gone.
void PublishWorld(std::uintptr_t player, std::uintptr_t room, std::uint32_t roomIndex) {
    const auto data = At<std::uintptr_t>(room + kListData); const auto count = At<std::uint32_t>(room + kListCount);
    if (!data || count > 4096) return;
    if (roomIndex != livedRoom) { livedRoom = roomIndex; livedCount = 0; deathCount = 0; }
    publishedWorld.generation |= 1;   // odd: being written, also after a write that never finished
    auto& world = publishedWorld.world; world.magic = kWorldMagic; world.sequence = ++worldSequence; world.room = roomIndex; world.count = 0;
    const auto descriptor = At<std::uintptr_t>(room + kDescriptor); world.clear = descriptor ? At<std::uint32_t>(descriptor + kRoomFlags) & 1 : 0;
    world.npcTotal = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto entity = At<std::uintptr_t>(data + i * sizeof(std::uintptr_t));
        if (!entity || !LivingNpc(entity)) continue;
        if (world.npcTotal++ >= kMaxNpcs) continue;   // counted, so that a guest knows the list is not the whole room
        auto& npc = world.npcs[world.count++];
        npc.seed = At<std::uint32_t>(entity + kSeed); npc.hitPoints = At<float>(entity + kHitPoints);
        npc.type = At<std::uint32_t>(entity + kType); npc.variant = At<std::uint32_t>(entity + kVariant); npc.subtype = At<std::uint32_t>(entity + kSubtype);
        npc.state = At<std::int32_t>(entity + kState); npc.stateFrame = At<std::int32_t>(entity + kStateFrame); npc.cooldown = At<std::int32_t>(entity + kCooldown);
        npc.i1 = At<std::int32_t>(entity + kI1); npc.i2 = At<std::int32_t>(entity + kI2);
        std::memcpy(npc.v1, reinterpret_cast<void*>(entity + kV1), 8); std::memcpy(npc.v2, reinterpret_cast<void*>(entity + kV2), 8);
        std::memcpy(npc.target, reinterpret_cast<void*>(entity + kTarget), 8);
        AnimationOf(entity, npc.animation);
        std::memcpy(npc.position, reinterpret_cast<void*>(entity + kPosition), 8); std::memcpy(npc.velocity, reinterpret_cast<void*>(entity + kVelocity), 8);
    }
    // A list that did not fit tells nothing about who is gone: nobody of it is called dead.
    if (world.npcTotal > kMaxNpcs) livedCount = 0;
    for (std::uint32_t i = 0; i < livedCount; ++i) {
        bool alive = false;
        for (std::uint32_t n = 0; n < world.count && !alive; ++n) alive = world.npcs[n].seed == lived[i];
        if (!alive) { deaths[deathCount % kMaxDeaths] = Death{lived[i], frame}; ++deathCount; }
    }
    livedCount = world.npcTotal > kMaxNpcs ? 0 : world.count;
    for (std::uint32_t n = 0; n < world.count; ++n) lived[n] = world.npcs[n].seed;
    world.deaths = 0;
    for (std::uint32_t i = 0; i < kMaxDeaths && i < deathCount; ++i)
        if (frame - deaths[i].frame <= kDeathFrames) world.died[world.deaths++] = deaths[i].seed;
    world.shots = 0;
    for (std::uint32_t i = 0; i < count && world.shots < kMaxShots; ++i) {
        const auto entity = At<std::uintptr_t>(data + i * sizeof(std::uintptr_t));
        if (entity && LivingShot(entity, kOfEnemy)) ShotOf(entity, kOfEnemy, world.shot[world.shots++]);
    }
    world.drops = world.dropsTotal = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto entity = At<std::uintptr_t>(data + i * sizeof(std::uintptr_t));
        if (!entity || !LivingPickup(entity)) continue;
        if (world.dropsTotal++ >= kMaxDrops) continue;
        auto& drop = world.drop[world.drops++];
        drop.seed = At<std::uint32_t>(entity + kSeed); drop.variant = At<std::uint32_t>(entity + kVariant); drop.subtype = At<std::uint32_t>(entity + kSubtype);
        std::memcpy(drop.position, reinterpret_cast<void*>(entity + kPosition), 8); std::memcpy(drop.velocity, reinterpret_cast<void*>(entity + kVelocity), 8);
        drop.price = At<std::int32_t>(entity + kPrice); drop.timeout = At<std::int32_t>(entity + kTimeout);
        drop.options = At<std::int32_t>(entity + kOptions); drop.shopItemId = At<std::int32_t>(entity + kShopItemId);
    }
    world.doors = 0;
    for (std::uint32_t i = 0; i < kGridCells && world.doors < kMaxDoors; ++i) {
        const auto grid = At<std::uintptr_t>(room + kGrid + i * 4);
        if (!grid || At<std::uintptr_t>(grid) != base + kDoorTable || At<std::uint32_t>(grid + kGridType) != kGridDoor) continue;
        world.door[world.doors++] = DoorState{static_cast<std::uint16_t>(i), At<std::uint8_t>(grid + kDoorBusted), 0, At<std::int32_t>(grid + kGridVariant), At<std::int32_t>(grid + kGridState)};
    }
    world.coins = At<std::int32_t>(player + kCoins); world.bombs = At<std::int32_t>(player + kBombs); world.keys = At<std::int32_t>(player + kKeys);
    // The grid. More changed cells than fit: a window over them that moves with every snapshot.
    world.cells = 0; std::uint32_t changed = 0, seen = 0;
    for (std::uint32_t i = 0; i < kGridCells; ++i) { const auto grid = At<std::uintptr_t>(room + kGrid + i * 4); if (grid && GridChanged(grid)) ++changed; }
    const std::uint32_t first = changed > kMaxCells ? (world.sequence * kMaxCells) % changed : 0;
    for (std::uint32_t i = 0; i < kGridCells && world.cells < kMaxCells; ++i) {
        const auto grid = At<std::uintptr_t>(room + kGrid + i * 4);
        if (!grid || !GridChanged(grid) || seen++ < first) continue;
        world.grid[world.cells++] = Cell{static_cast<std::uint16_t>(i), static_cast<std::uint16_t>(At<std::uint32_t>(grid + kGridType)), At<std::int32_t>(grid + kGridState)};
    }
    publishedWorld.generation++;
    stats.worldPublished++;
}

// Guest: the host's grid over the local one, by the game's own Destroy; poop and TNT on their way by their state.
void ApplyGrid(std::uintptr_t room, const World& world) {
    for (std::uint32_t c = 0; c < world.cells; ++c) {
        const auto& cell = world.grid[c];
        const auto grid = cell.index < kGridCells ? At<std::uintptr_t>(room + kGrid + cell.index * 4) : 0;
        if (!grid) continue;
        const int kind = GridKind(grid); const auto state = At<std::int32_t>(grid + kGridState);
        if (!kind || At<std::uint32_t>(grid + kGridType) != cell.type) { stats.gridMismatch++; continue; }
        if (state == cell.state) continue;
        const std::int32_t broken = kind == 1 ? 2 : kind == 2 ? 1000 : kind == 3 ? 4 : 1;
        const bool breakIt = kind == 1 ? cell.state == broken && state != broken : cell.state >= broken && state < broken;
        if (breakIt) {
            applyingGrid = true; reinterpret_cast<GridDestroy>(base + kGridDestroy)(reinterpret_cast<void*>(grid), 0); applyingGrid = false;
            stats.gridFixes++;
        } else if ((kind == 2 || kind == 3) && cell.state < broken && state < broken) {
            At<std::int32_t>(grid + kGridState) = cell.state; stats.gridFixes++;
            if (kind == 2 && At<std::uint32_t>(grid + kGridVariant) != 9) {   // as poop's own Hurt names it
                const int step = cell.state / 250; char name[16];
                std::snprintf(name, sizeof(name), "State%d", (step < 0 ? 0 : step > 3 ? 3 : step) + 1);
                reinterpret_cast<SpritePlay>(base + kSpritePlay)(reinterpret_cast<void*>(grid + kGridSprite), name, true);
            }
        }
    }
}

std::uint32_t& AgeOf(std::uint32_t seed) {
    for (std::uint32_t a = 0; a < dropAgeCount; ++a) if (dropAges[a].seed == seed) return dropAges[a].age;
    auto& fresh = dropAges[dropAgeCount < kAges ? dropAgeCount++ : seed % kAges]; fresh = Age{seed, 0};
    return fresh.age;
}

// In step again: forget the count, without making an entry for every pickup that never was out of step.
void InStep(std::uint32_t seed) { for (std::uint32_t a = 0; a < dropAgeCount; ++a) if (dropAges[a].seed == seed) dropAges[a].age = 0; }

// Guest: exactly the host's pickups.
void ApplyDrops(std::uintptr_t room, const World& world) {
    const auto data = At<std::uintptr_t>(room + kListData); const auto listCount = At<std::uint32_t>(room + kListCount);
    if (!data || listCount > 4096 || world.drops > kMaxDrops) return;
    const bool complete = world.dropsTotal <= kMaxDrops;
    bool taken[kMaxDrops]{}; std::uintptr_t remove[kMaxDrops]; std::uint32_t removeCount = 0;
    struct { std::uintptr_t entity; std::uint32_t subtype; } morph[8]; std::uint32_t morphCount = 0;
    for (std::uint32_t i = 0; i < listCount; ++i) {
        const auto entity = At<std::uintptr_t>(data + i * sizeof(std::uintptr_t));
        if (!entity || !LivingPickup(entity)) continue;
        const auto seed = At<std::uint32_t>(entity + kSeed); std::uint32_t n = 0;
        while (n < world.drops && (taken[n] || world.drop[n].seed != seed)) ++n;
        if (n < world.drops) {
            const auto& drop = world.drop[n]; taken[n] = true; Learn(knownDrops, seed);
            if (At<std::uint32_t>(entity + kVariant) != drop.variant) continue;   // opened or changed here a moment ago: the owner's taking settles it
            std::memcpy(reinterpret_cast<void*>(entity + kPosition), drop.position, 8); std::memcpy(reinterpret_cast<void*>(entity + kVelocity), drop.velocity, 8);
            if (At<std::int32_t>(entity + kPrice) != drop.price) At<std::int32_t>(entity + kPrice) = drop.price;
            if (At<std::int32_t>(entity + kTimeout) != drop.timeout) At<std::int32_t>(entity + kTimeout) = drop.timeout;
            if (At<std::int32_t>(entity + kOptions) != drop.options) At<std::int32_t>(entity + kOptions) = drop.options;
            const auto subtype = At<std::uint32_t>(entity + kSubtype);
            if (drop.variant == kCollectible && drop.subtype && subtype && subtype != drop.subtype) {
                if (++AgeOf(seed) >= kDropItemSnapshots && morphCount < 8) { morph[morphCount].entity = entity; morph[morphCount++].subtype = drop.subtype; AgeOf(seed) = 0; }
            } else InStep(seed);
            continue;
        }
        if (!complete) continue;
        if (++AgeOf(seed) >= (Knows(knownDrops, seed) ? kDropGoneSnapshots : kDropMadeHereSnapshots) && removeCount < kMaxDrops) remove[removeCount++] = entity;
    }
    for (std::uint32_t r = 0; r < removeCount; ++r) {
        const auto entity = remove[r]; reinterpret_cast<PlayerUpdate>(At<void**>(entity)[10])(reinterpret_cast<void*>(entity)); stats.dropsRemoved++;
    }
    for (std::uint32_t m = 0; m < morphCount; ++m) {
        reinterpret_cast<PickupMorph>(base + kPickupMorph)(reinterpret_cast<void*>(morph[m].entity), static_cast<int>(kPickup), static_cast<int>(kCollectible),
            static_cast<int>(morph[m].subtype), 1, 1, 1);
        stats.dropsMorphed++;
    }
    const auto game = At<std::uintptr_t>(base + kGame); std::uint32_t spawned = 0;
    for (std::uint32_t n = 0; n < world.drops && game && spawned < kMaxDropSpawns; ++n) {
        const auto& drop = world.drop[n];
        if (taken[n] || !drop.seed || Knows(knownDrops, drop.seed)) continue;
        Learn(knownDrops, drop.seed);
        if (!drop.subtype) { stats.dropsSkipped++; continue; }   // subtype 0 asks the game to roll one
        const auto entity = reinterpret_cast<std::uintptr_t>(reinterpret_cast<Spawn>(base + kSpawn)(reinterpret_cast<void*>(game), kPickup, drop.variant, drop.position, drop.velocity,
            nullptr, drop.subtype, drop.seed));
        if (!entity || !LivingPickup(entity)) continue;
        At<std::int32_t>(entity + kPrice) = drop.price; At<std::int32_t>(entity + kTimeout) = drop.timeout;
        At<std::int32_t>(entity + kOptions) = drop.options; At<std::int32_t>(entity + kShopItemId) = drop.shopItemId;
        stats.dropsMade++; ++spawned;
    }
}

// Guest: the host's doors, unless a door has just changed here.
void ApplyDoors(std::uintptr_t room, const World& world) {
    for (std::uint32_t d = 0; d < world.doors && d < kMaxDoors; ++d) {
        const auto& door = world.door[d];
        const auto grid = door.cell < kGridCells ? At<std::uintptr_t>(room + kGrid + door.cell * 4) : 0;
        if (!grid || At<std::uintptr_t>(grid) != base + kDoorTable || At<std::uint32_t>(grid + kGridType) != kGridDoor) { stats.doorMismatch++; continue; }
        const auto variant = At<std::int32_t>(grid + kGridVariant), state = At<std::int32_t>(grid + kGridState); const auto busted = At<std::uint8_t>(grid + kDoorBusted);
        std::uint32_t m = 0;
        while (m < doorMemoryCount && doorMemory[m].cell != door.cell) ++m;
        if (m == doorMemoryCount) { if (doorMemoryCount == kMaxDoors) continue; doorMemory[doorMemoryCount++] = DoorMemory{door.cell, variant, state, busted, 0, 0}; }
        auto& memory = doorMemory[m];
        if (memory.variant != variant || memory.state != state || memory.busted != busted) memory.heldUntil = frame + kDoorHoldFrames;   // the game changed it here
        memory.variant = variant; memory.state = state; memory.busted = busted;
        if (variant == door.variant && state == door.state && busted == door.busted) { memory.differing = 0; continue; }
        if (door.state < 1 || door.state > 4 || frame < memory.heldUntil || ++memory.differing < kDoorSnapshots) continue;
        At<std::int32_t>(grid + kGridVariant) = door.variant; At<std::int32_t>(grid + kGridState) = door.state; At<std::uint8_t>(grid + kDoorBusted) = door.busted;
        reinterpret_cast<PlayerUpdate>(base + kDoorRefresh)(reinterpret_cast<void*>(grid));
        memory.variant = door.variant; memory.state = door.state; memory.busted = door.busted; memory.differing = 0; stats.doorFixes++;
    }
}

// Guest: the team's coins, bombs and keys are the host's, once they have stood still here for a while.
void ApplyCounters(std::uintptr_t game, std::uintptr_t player, const World& world) {
    const std::uintptr_t offsets[3] = {kCoins, kBombs, kKeys}; const std::int32_t hosts[3] = {world.coins, world.bombs, world.keys};
    for (int k = 0; k < 3; ++k) if (lastCounters[k] >= 0 && At<std::int32_t>(player + offsets[k]) != lastCounters[k]) countersHeldUntil = frame + kCountersHoldFrames;
    if (frame >= countersHeldUntil) {
        const auto first = At<std::uintptr_t>(game + kPlayers), last = At<std::uintptr_t>(game + kPlayers + sizeof(std::uintptr_t));
        for (int k = 0; k < 3; ++k) {
            if (hosts[k] < 0 || hosts[k] > 999 || At<std::int32_t>(player + offsets[k]) == hosts[k]) continue;
            for (auto at = first; first && at < last && at - first < 8 * sizeof(std::uintptr_t); at += sizeof(std::uintptr_t))
                if (const auto each = At<std::uintptr_t>(at)) At<std::int32_t>(each + offsets[k]) = hosts[k];
            stats.counterFixes++;
        }
    }
    for (int k = 0; k < 3; ++k) lastCounters[k] = At<std::int32_t>(player + offsets[k]);
}

// Guest: the host's enemies over the local ones.
void ApplyWorld(std::uintptr_t player, std::uintptr_t room, std::uint32_t roomIndex) {
    static World world;  // the game's thread only
    bool fresh = false;
    AcquireSRWLockShared(&inboxLock);
    if (worldInbox.sequence && worldInbox.sequence != worldApplied && GetTickCount64() - worldAt <= kFreshMs) { world = worldInbox; fresh = true; }
    ReleaseSRWLockShared(&inboxLock);
    if (!fresh) return;
    if (world.room != roomIndex) { stats.otherRoom++; return; }
    hostClearRoom.store(world.room); hostClear.store(world.clear); hostHeardAt.store(GetTickCount64());
    if (aliasRoom != roomIndex) { aliasRoom = roomIndex; aliasCount = 0; orphanCount = 0; missingCount = 0; dropAgeCount = 0; doorMemoryCount = 0; }
    if (On(kCounters)) if (const auto game = At<std::uintptr_t>(base + kGame)) ApplyCounters(game, player, world);
    const auto data = At<std::uintptr_t>(room + kListData); const auto count = At<std::uint32_t>(room + kListCount);
    if (!data || count > 4096) return;
    std::uintptr_t local[kMaxNpcs]; int partner[kMaxNpcs]; bool taken[kMaxNpcs]{}; std::uint32_t locals = 0;
    std::uint32_t wanted[kMaxSpawnsPerFrame]; std::uint32_t wantedCount = 0;
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
        if (best < 0) {
            stats.npcOnlyHost++;
            std::uint32_t m = 0;
            while (m < missingCount && missing[m].seed != world.npcs[n].seed) ++m;
            if (m == missingCount && missingCount < kMaxNpcs) missing[missingCount++] = Missing{world.npcs[n].seed, 0, 0};
            if (m < missingCount && ++missing[m].age >= kSpawnAfterSnapshots && (!missing[m].spawnedAt || frame - missing[m].spawnedAt >= kSpawnRetryFrames) &&
                wantedCount < kMaxSpawnsPerFrame) { missing[m].spawnedAt = frame ? frame : 1; wanted[wantedCount++] = n; }
            continue;
        }
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
            if (index < orphanCount && world.npcTotal <= kMaxNpcs && ++orphans[index].age > kOrphanSnapshots) { doomed[doomedCount++] = entity; stats.npcRemoved++; }
            continue;
        }
        if (index < orphanCount) orphans[index].age = 0;
        const auto& npc = world.npcs[partner[l]];
        auto* position = reinterpret_cast<float*>(entity + kPosition);
        const float dx = npc.position[0] - position[0], dy = npc.position[1] - position[1], distance = std::sqrt(dx * dx + dy * dy);
        std::memcpy(position, npc.position, 8); std::memcpy(reinterpret_cast<void*>(entity + kVelocity), npc.velocity, 8);
        if (At<float>(entity + kHitPoints) != npc.hitPoints) { At<float>(entity + kHitPoints) = npc.hitPoints; stats.hitPointFixes++; }
        // What the enemy does: the host's. The guest's own code goes on from here, so it plays the host's attack.
        if (!On(kBehaviour)) { stats.npcCorrectionSum += distance; if (distance > stats.npcCorrectionMax) stats.npcCorrectionMax = distance; stats.npcMatched++; continue; }
        if (At<std::int32_t>(entity + kState) != npc.state) { At<std::int32_t>(entity + kState) = npc.state; stats.stateFixes++; }
        At<std::int32_t>(entity + kStateFrame) = npc.stateFrame; At<std::int32_t>(entity + kCooldown) = npc.cooldown;
        At<std::int32_t>(entity + kI1) = npc.i1; At<std::int32_t>(entity + kI2) = npc.i2;
        std::memcpy(reinterpret_cast<void*>(entity + kV1), npc.v1, 8); std::memcpy(reinterpret_cast<void*>(entity + kV2), npc.v2, 8);
        std::memcpy(reinterpret_cast<void*>(entity + kTarget), npc.target, 8);
        if (npc.animation[0]) {
            char own[kAnimationName]; AnimationOf(entity, own);
            if (std::memcmp(own, npc.animation, kAnimationName) != 0) {
                reinterpret_cast<SpritePlay>(base + kSpritePlay)(reinterpret_cast<void*>(entity + kSprite), npc.animation, true); stats.animationFixes++;
            }
        }
        stats.npcCorrectionSum += distance; if (distance > stats.npcCorrectionMax) stats.npcCorrectionMax = distance;
        stats.npcMatched++;
    }
    // Killing spawns effects, which may move the list: only after the walk over it.
    for (std::uint32_t d = 0; d < doomedCount; ++d) { reinterpret_cast<PlayerUpdate>(base + kKill)(reinterpret_cast<void*>(doomed[d])); stats.npcKilled++; }
    // And creating changes the list as well: last of all.
    const auto game = At<std::uintptr_t>(base + kGame);
    for (std::uint32_t w = 0; w < wantedCount && game && On(kBehaviour); ++w) {
        const auto& npc = world.npcs[wanted[w]];
        const auto made = reinterpret_cast<Spawn>(base + kSpawn)(reinterpret_cast<void*>(game), npc.type, npc.variant, npc.position, npc.velocity, nullptr, npc.subtype, npc.seed ? npc.seed : 1);
        if (made) stats.npcSpawned++; else stats.npcSpawnFailures++;
    }
    // The host's room is clear and this one is not: clear it as the game does.
    const auto descriptor = At<std::uintptr_t>(room + kDescriptor);
    if (On(kClear) && world.clear && clearTrampoline && descriptor && !(At<std::uint32_t>(descriptor + kRoomFlags) & 1)) {
        reinterpret_cast<TriggerClear>(clearTrampoline)(reinterpret_cast<void*>(room), 0); stats.clearsFromHost++;
    }
    if (On(kGridRule)) ApplyGrid(room, world);
    if (On(kProjectiles))
        ApplyShots(room, world.shot, world.shots, world.shots < kMaxShots, kOfEnemy, 0, knownProjectiles, stats.projectilesMade, stats.projectilesEnded, stats.projectilesDropped);
    if (On(kDrops)) ApplyDrops(room, world);
    if (On(kDoors)) ApplyDoors(room, world);
    stats.worldApplied++; worldApplied = world.sequence;
}

// What tells a pickup taken, opened or bought from the same pickup a moment earlier.
struct Shape { std::uint32_t type, variant, subtype, seed; std::int32_t price; std::uint8_t exists, dead; };
Shape ShapeOf(std::uintptr_t pickup) {
    return Shape{At<std::uint32_t>(pickup + kType), At<std::uint32_t>(pickup + kVariant), At<std::uint32_t>(pickup + kSubtype), At<std::uint32_t>(pickup + kSeed),
                 At<std::int32_t>(pickup + kPrice), At<std::uint8_t>(pickup + kExists), At<std::uint8_t>(pickup + kDead)};
}
bool Same(const Shape& a, const Shape& b) {
    return a.type == b.type && a.variant == b.variant && a.subtype == b.subtype && a.seed == b.seed && a.price == b.price && a.exists == b.exists && a.dead == b.dead;
}

bool __fastcall OnPickupCollision(void* self, void*, void* collider, std::uint32_t low) {
    const auto other = reinterpret_cast<std::uintptr_t>(collider);
    if (!running.load(std::memory_order_acquire) || !On(kTaken) || applyingTaken || !other || At<std::uintptr_t>(other) != base + kPlayerTable)
        return originalCollision(self, collider, low);
    if (At<int>(other + kController) != ownController) { stats.copyTouchesIgnored++; return false; }
    const auto pickup = reinterpret_cast<std::uintptr_t>(self); const Shape before = ShapeOf(pickup);
    float position[2]; std::memcpy(position, reinterpret_cast<void*>(pickup + kPosition), 8);
    const bool result = originalCollision(self, collider, low);
    if (!Same(before, ShapeOf(pickup))) {
        const auto game = At<std::uintptr_t>(base + kGame);
        takenLog[takenTotal % kMaxTaken] = Taken{takenTotal + 1, game ? At<std::uint32_t>(game + kRoomIndex) : 0, before.seed, before.variant, before.subtype, low & 0xff,
                                                 {position[0], position[1]}};
        ++takenTotal; stats.taken++;
    }
    return result;
}

// A remote player's takings, in order: its copy takes here what its owner's game saw it take.
void ApplyTaken(std::uintptr_t player, int controller, const Body& body, std::uintptr_t room, std::uint32_t roomIndex) {
    if (!takenKnown[controller]) { takenKnown[controller] = true; takenDone[controller] = body.takenTotal; return; }   // what came before this module is not replayed
    if (body.takenTotal < takenDone[controller]) takenDone[controller] = body.takenTotal;
    while (takenDone[controller] < body.takenTotal) {
        const std::uint32_t number = takenDone[controller] + 1; const Taken* taken = nullptr;
        for (const auto& t : body.taken) if (t.number == number) taken = &t;
        if (!taken || taken->room != roomIndex) { takenDone[controller] = number; takenSince[controller] = 0; stats.takenMissed++; continue; }
        if (!takenSince[controller]) takenSince[controller] = frame ? frame : 1;
        const auto data = At<std::uintptr_t>(room + kListData); const auto count = At<std::uint32_t>(room + kListCount);
        std::uintptr_t found = 0; float foundDistance = 0;
        for (std::uint32_t i = 0; data && count <= 4096 && i < count; ++i) {
            const auto entity = At<std::uintptr_t>(data + i * sizeof(std::uintptr_t));
            if (!entity || At<std::uintptr_t>(entity) != base + kPickupTable || !At<std::uint8_t>(entity + kExists) || At<std::uint8_t>(entity + kDead)) continue;
            const auto variant = At<std::uint32_t>(entity + kVariant), subtype = At<std::uint32_t>(entity + kSubtype);
            if (variant != taken->variant) continue;
            if (At<std::uint32_t>(entity + kSeed) == taken->seed) { found = entity; break; }
            if (subtype != taken->subtype && !(variant == 100 && subtype != 0)) continue;   // a pedestal may hold another item here
            const auto* position = reinterpret_cast<float*>(entity + kPosition);
            const float dx = position[0] - taken->position[0], dy = position[1] - taken->position[1], distance = dx * dx + dy * dy;
            if (distance <= kTakenReach * kTakenReach && (!found || distance < foundDistance)) { found = entity; foundDistance = distance; }
        }
        bool done = false;
        if (found) {
            const Shape before = ShapeOf(found);
            At<std::int32_t>(found + kWait) = 0;   // the owner's game has decided: no waiting here
            applyingTaken = true; originalCollision(reinterpret_cast<void*>(found), reinterpret_cast<void*>(player), taken->low); applyingTaken = false;
            done = !Same(before, ShapeOf(found));
        }
        if (done) stats.takenApplied++;
        else if (frame - takenSince[controller] < static_cast<std::uint32_t>(kTakenRetryFrames)) return;   // in order: again next frame
        else stats.takenMissed++;
        takenDone[controller] = number; takenSince[controller] = 0;
    }
}

bool RequestRoom(std::uintptr_t game, std::uint32_t index, int direction, std::uint32_t dimension) {
    __try { reinterpret_cast<Transition>(base + kTransition)(reinterpret_cast<void*>(game), static_cast<int>(index), direction, kFade, nullptr, static_cast<int>(dimension)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Once a frame, with the local player: keep this game's epoch, and follow whoever changed room after this game did.
void FollowRoom(std::uintptr_t game, std::uint32_t roomIndex) {
    if (roomIndex != lastRoom) {
        if (following && roomIndex == followRoom) following = false;          // arrived where it was led: not a change of its own
        else if (lastRoom != 0xfffffffe) roomEpoch = (roomEpoch > heardEpoch ? roomEpoch : heardEpoch) + 1;
        lastRoom = roomIndex;
    }
    Body leader{}; bool found = false;
    AcquireSRWLockShared(&inboxLock);
    for (const auto& box : inbox) {
        if (!box.body.sequence || GetTickCount64() - box.at > kFreshMs) continue;
        if (box.body.roomEpoch > heardEpoch) heardEpoch = box.body.roomEpoch;
        const bool newer = box.body.roomEpoch > roomEpoch || (box.body.roomEpoch == roomEpoch && box.body.host && !host);
        if (newer && box.body.room != roomIndex && (!found || box.body.roomEpoch > leader.roomEpoch)) { leader = box.body; found = true; }
    }
    ReleaseSRWLockShared(&inboxLock);
    if (!found || At<std::uint32_t>(game + kRoomTransition) != 0 || (followTried && frame - followTried < kFollowRetryFrames)) return;
    const auto step = static_cast<std::int32_t>(leader.room) - static_cast<std::int32_t>(roomIndex);
    const int direction = step == 1 ? 2 : step == -1 ? 0 : step == kGridWidth ? 3 : step == -kGridWidth ? 1 : -1;   // left 0, up 1, right 2, down 3
    followTried = frame ? frame : 1;
    if (RequestRoom(game, leader.room, direction, leader.dimension) && At<std::uint32_t>(game + kRoomTransition) != 0) {
        following = true; followRoom = leader.room; roomEpoch = leader.roomEpoch; stats.roomFollows++;
    } else stats.roomFollowFailures++;
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
            if (On(kFollow)) FollowRoom(game, roomIndex);
            published.generation |= 1;   // odd: being written, also after a write that never finished
            published.body.magic = kBodyMagic; published.body.controller = static_cast<std::uint32_t>(controller); published.body.sequence = ++sequence;
            published.body.room = roomIndex;
            std::memcpy(published.body.position, position, 8); std::memcpy(published.body.velocity, velocity, 8);
            published.body.dimension = At<std::uint32_t>(game + kDimension); published.body.roomEpoch = roomEpoch; published.body.host = host ? 1 : 0;
            published.body.ghost = At<std::uint8_t>(player + kGhost); published.body.dying = At<std::uint8_t>(player + kDead);
            published.body.takenTotal = takenTotal; std::memcpy(published.body.taken, takenLog, sizeof(takenLog));
            for (int h = 0; h < kHealthFields; ++h) published.body.health[h] = At<std::int32_t>(player + kHealth[h]);
            published.generation++;
            stats.published++;
            if (room) { PublishShots(player, room, roomIndex); if (host) PublishWorld(player, room, roomIndex); else ApplyWorld(player, room, roomIndex); }
            return;
        }
        if (controller < 0 || controller >= kControllers) return;
        if (room && (On(kTears) || On(kBombsRule))) ApplyTears(player, controller, room, roomIndex);
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
        const bool ghost = At<std::uint8_t>(player + kGhost) != 0, dying = At<std::uint8_t>(player + kDead) != 0, gone = body.ghost || body.dying;
        aliveSeen[controller] = gone ? 0 : aliveSeen[controller] + 1;
        if (gone && !ghost && !dying) {
            if (frame - deathTried[controller] >= kDeathRetryFrames) {
                deathTried[controller] = frame; stats.copyDeaths++;
                static std::uint8_t nobody[0x40]{};   // an empty damage source, as the game's own Kill() builds one
                originalPlayerDamage(reinterpret_cast<void*>(player), 1000.0f, 0, 0, nobody, 0);
            }
        } else if (!gone && dying && !ghost) {
            if (aliveSeen[controller] >= static_cast<std::uint32_t>(kAliveBodies)) { reinterpret_cast<PlayerUpdate>(base + kRevive)(reinterpret_cast<void*>(player)); stats.copyRevivals++; }
        } else if (!gone && ghost) {
            stats.copyRevivalsMissed++;
        } else if (!ghost && !dying) {
            bool fixed = false;
            for (int h = 0; h < kHealthFields; ++h)
                if (At<std::int32_t>(player + kHealth[h]) != body.health[h]) { At<std::int32_t>(player + kHealth[h]) = body.health[h]; fixed = true; }
            if (fixed) stats.healthFixes++;
        }
        if (room && On(kTaken)) ApplyTaken(player, controller, body, room, roomIndex);
    } __except (EXCEPTION_EXECUTE_HANDLER) { stats.rejected++; applyingTaken = false; applyingGrid = false; }
}

void __fastcall OnPlayer(void* object, void*) {
    originalPlayer(object);
    if (running.load(std::memory_order_acquire)) AfterUpdate(reinterpret_cast<std::uintptr_t>(object));
}

char __fastcall OnNpcDamage(void* self, void*, float damage, std::uint32_t flagsLow, std::uint32_t flagsHigh, void* source, int countdown) {
    if (running.load(std::memory_order_acquire) && !host) {
        const auto entity = reinterpret_cast<std::uintptr_t>(self); const float hitPoints = At<float>(entity + kHitPoints);
        if (At<std::uint32_t>(entity + kType) == kFireplace && On(kFire) && HostRules()) { stats.fireHeld++; return 0; }
        if (At<float>(entity + kMaxHitPoints) > 0 && damage >= hitPoints) { damage = hitPoints > 0.02f ? hitPoints - 0.01f : 0.0f; stats.deathsHeld++; }
    }
    return originalDamage(self, damage, flagsLow, flagsHigh, source, countdown);
}

char __fastcall OnPlayerDamage(void* self, void*, float damage, std::uint32_t flagsLow, std::uint32_t flagsHigh, void* source, int countdown) {
    if (running.load(std::memory_order_acquire) && At<int>(reinterpret_cast<std::uintptr_t>(self) + kController) != ownController) {
        stats.copyDamageIgnored++;
        return 0;
    }
    return originalPlayerDamage(self, damage, flagsLow, flagsHigh, source, countdown);
}

// A guest's own breaking waits for the host's word, as its clearing does.
bool HostRules() {
    if (!running.load(std::memory_order_acquire) || host) return false;
    const auto game = At<std::uintptr_t>(base + kGame);
    return game && GetTickCount64() - hostHeardAt.load() <= kHostSilenceMs && hostClearRoom.load() == At<std::uint32_t>(game + kRoomIndex);
}
bool GridHeld() { return !applyingGrid && On(kGridRule) && HostRules(); }

template <int N> std::uint32_t __fastcall OnGrid(void* self, void*, std::uint32_t argument, void* source) {
    if (GridHeld()) { stats.gridHeld++; return 0; }
    return reinterpret_cast<GridChange>(gridHooks[N].original)(self, argument, source);
}

DWORD HookGrid() {
    void* replacements[5] = {reinterpret_cast<void*>(&OnGrid<0>), reinterpret_cast<void*>(&OnGrid<1>), reinterpret_cast<void*>(&OnGrid<2>),
                             reinterpret_cast<void*>(&OnGrid<3>), reinterpret_cast<void*>(&OnGrid<4>)};
    for (int n = 0; n < 5; ++n) {
        auto& hook = gridHooks[n]; hook.replacement = replacements[n];
        hook.at = reinterpret_cast<void**>(base + hook.table + hook.slot * sizeof(void*));
        if (!hook.installed && *hook.at != reinterpret_cast<void*>(base + hook.function)) return ERROR_REVISION_MISMATCH;
    }
    for (auto& hook : gridHooks) {
        if (hook.installed) continue;
        hook.original = *hook.at;   // stays set after the hook is gone: a thread may still be inside it
        if (const DWORD failure = ExchangeSlot(hook.at, hook.original, hook.replacement)) return failure;
        hook.installed = true;
    }
    return ERROR_SUCCESS;
}

void UnhookGrid() {
    for (auto& hook : gridHooks) if (hook.installed && !ExchangeSlot(hook.at, hook.replacement, hook.original)) hook.installed = false;
}

void __fastcall OnTriggerClear(void* room, void*, char silent) {
    if (running.load(std::memory_order_acquire) && !host && On(kClear)) {
        const auto game = At<std::uintptr_t>(base + kGame);
        const bool heard = GetTickCount64() - hostHeardAt.load() <= kHostSilenceMs;
        const bool sameRoom = game && hostClearRoom.load() == At<std::uint32_t>(game + kRoomIndex);
        if (heard && sameRoom && !hostClear.load()) { stats.clearsHeld++; return; }
    }
    reinterpret_cast<TriggerClear>(clearTrampoline)(room, silent);
}

DWORD HookClear() {
    clearEntry = reinterpret_cast<std::uint8_t*>(base + kTriggerClear);
    if (std::memcmp(clearEntry, kClearEntry.data(), kClearEntry.size()) != 0) return ERROR_REVISION_MISMATCH;
    auto* code = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!code) return GetLastError();
    std::memcpy(code, clearEntry, 6); code[6] = 0xE9;
    const auto back = static_cast<std::uint32_t>((clearEntry + 6) - (code + 11)); std::memcpy(code + 7, &back, 4);
    DWORD old = 0; VirtualProtect(code, 32, PAGE_EXECUTE_READ, &old);
    clearTrampoline = code;
    if (!VirtualProtect(clearEntry, 6, PAGE_EXECUTE_READWRITE, &old)) return GetLastError();
    std::uint8_t jump[6]{0xE9, 0, 0, 0, 0, 0x90};
    const auto there = static_cast<std::uint32_t>(reinterpret_cast<std::uint8_t*>(&OnTriggerClear) - (clearEntry + 5)); std::memcpy(jump + 1, &there, 4);
    std::memcpy(clearEntry, jump, 6);
    DWORD ignored = 0; VirtualProtect(clearEntry, 6, old, &ignored); FlushInstructionCache(GetCurrentProcess(), clearEntry, 6);
    clearHooked = true;
    return ERROR_SUCCESS;
}

void UnhookClear() {
    if (!clearHooked) return;
    DWORD old = 0;
    if (VirtualProtect(clearEntry, 6, PAGE_EXECUTE_READWRITE, &old)) {
        std::memcpy(clearEntry, kClearEntry.data(), 6);
        DWORD ignored = 0; VirtualProtect(clearEntry, 6, old, &ignored); FlushInstructionCache(GetCurrentProcess(), clearEntry, 6);
        clearHooked = false;   // the trampoline stays allocated: a thread may still be inside it
    }
}

bool ValidShot(const Shot& shot) {
    if (!Finite(shot.position) || !Finite(shot.velocity) || !std::isfinite(shot.height) || !std::isfinite(shot.fallingSpeed) || !std::isfinite(shot.fallingAccel) ||
        !std::isfinite(shot.scale) || !std::isfinite(shot.damage)) return false;
    for (const float c : shot.color) if (!std::isfinite(c) || std::fabs(c) > 100.0f) return false;
    // Far outside what a shot can be: the sender read something else (another build's layout), and nothing of it is applied.
    return std::fabs(shot.position[0]) < 10000.0f && std::fabs(shot.position[1]) < 10000.0f && std::fabs(shot.velocity[0]) < 1000.0f && std::fabs(shot.velocity[1]) < 1000.0f &&
        shot.height > -5000.0f && shot.height < 2.0e9f &&   // a bomb carries its frames to the explosion here, and a remote one's may be very many std::fabs(shot.fallingSpeed) < 1000.0f && std::fabs(shot.fallingAccel) < 100.0f &&
        shot.scale >= 0.0f && shot.scale < 50.0f && shot.damage >= 0.0f && shot.damage < 100000.0f;
}

bool ValidWorld(const World& world, int got) {
    if (got != sizeof(World) || world.magic != kWorldMagic || !world.sequence || world.count > kMaxNpcs || world.deaths > kMaxDeaths || world.cells > kMaxCells || world.shots > kMaxShots || world.drops > kMaxDrops || world.doors > kMaxDoors) return false;
    for (std::uint32_t n = 0; n < world.drops; ++n) if (!Finite(world.drop[n].position) || !Finite(world.drop[n].velocity)) return false;
    for (std::uint32_t n = 0; n < world.shots; ++n) if (!ValidShot(world.shot[n])) return false;
    for (std::uint32_t n = 0; n < world.count; ++n)
        if (!Finite(world.npcs[n].position) || !Finite(world.npcs[n].velocity) || !std::isfinite(world.npcs[n].hitPoints) || world.npcs[n].animation[kAnimationName - 1]) return false;
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
        } else if (got == sizeof(Shots)) {
            const auto* shots = reinterpret_cast<const Shots*>(packet);
            bool valid = shots->magic == kShotsMagic && shots->controller < kControllers && shots->sequence && static_cast<int>(shots->controller) != ownController &&
                shots->count <= kMaxTears && shots->bombs <= kMaxTears && shots->count + shots->bombs <= kMaxTears;
            for (std::uint32_t n = 0; valid && n < shots->count + shots->bombs; ++n) valid = ValidShot(shots->shot[n]);
            if (!valid) { stats.rejected++; continue; }
            AcquireSRWLockExclusive(&inboxLock);
            auto& box = shotsInbox[shots->controller];
            if (shots->sequence > box.shots.sequence) { std::memcpy(&box.shots, packet, sizeof(Shots)); box.at = GetTickCount64(); }
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
        std::uint32_t mask = kAllRules; if (!(config >> std::hex >> mask)) mask = kAllRules;
        const auto table = At<std::uintptr_t>(base + kManager);
        slot = reinterpret_cast<void**>(base + kManagerTable + 29 * sizeof(void*));
        playerSlot = reinterpret_cast<void**>(base + kPlayerTable + 3 * sizeof(void*));
        damageSlot = reinterpret_cast<void**>(base + kNpcTable + 8 * sizeof(void*));
        playerDamageSlot = reinterpret_cast<void**>(base + kPlayerTable + 8 * sizeof(void*));
        pickupSlot = reinterpret_cast<void**>(base + kPickupTable + 23 * sizeof(void*));
        if (table != base + kManagerTable || *slot != reinterpret_cast<void*>(base + kWithDevice) || *playerSlot != reinterpret_cast<void*>(base + kPlayerUpdate) ||
            *damageSlot != reinterpret_cast<void*>(base + kNpcDamage) || *playerDamageSlot != reinterpret_cast<void*>(base + kPlayerDamage) ||
            *pickupSlot != reinterpret_cast<void*>(base + kPickupCollision) ||
            std::memcmp(reinterpret_cast<void*>(base + kRevive), kReviveEntry.data(), kReviveEntry.size()) != 0 ||
            std::memcmp(reinterpret_cast<void*>(base + kSpritePlay), kSpritePlayEntry.data(), kSpritePlayEntry.size()) != 0 ||
            std::memcmp(reinterpret_cast<void*>(base + kGridDestroy), kGridDestroyEntry.data(), kGridDestroyEntry.size()) != 0 ||
            std::memcmp(reinterpret_cast<void*>(base + kTearSetScale), kTearSetScaleEntry.data(), kTearSetScaleEntry.size()) != 0 ||
            std::memcmp(reinterpret_cast<void*>(base + kPickupMorph), kPickupMorphEntry.data(), kPickupMorphEntry.size()) != 0 ||
            std::memcmp(reinterpret_cast<void*>(base + kDoorRefresh), kDoorRefreshEntry.data(), kDoorRefreshEntry.size()) != 0 ||
            std::memcmp(reinterpret_cast<void*>(base + kKill), kKillEntry.data(), kKillEntry.size()) != 0 ||
            std::memcmp(reinterpret_cast<void*>(base + kTransition), kTransitionEntry.data(), kTransitionEntry.size()) != 0 ||
            std::memcmp(reinterpret_cast<void*>(base + kSpawn), kSpawnEntry.data(), kSpawnEntry.size()) != 0) throw static_cast<DWORD>(ERROR_REVISION_MISMATCH);
        original = reinterpret_cast<WithDevice>(*slot); originalPlayer = reinterpret_cast<PlayerUpdate>(*playerSlot); originalDamage = reinterpret_cast<NpcDamage>(*damageSlot);
        originalPlayerDamage = reinterpret_cast<NpcDamage>(*playerDamageSlot); originalCollision = reinterpret_cast<Collision>(*pickupSlot);
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&OnInput), &pinned)) throw GetLastError();
        ownController = controller; host = role == "host"; rules = mask; counters[0] = 0; counters[1] = 0; stats = Stats{}; sequence = worldSequence = frame = 0;
        published = Published{}; published.magic = kBodyMagic; publishedWorld = PublishedWorld{}; publishedWorld.magic = kWorldMagic;
        publishedShots = PublishedShots{}; publishedShots.magic = kShotsMagic; shotsSequence = 0; knownProjectiles = Known{}; knownDrops = Known{};
        dropAgeCount = doorMemoryCount = countersHeldUntil = 0; lastCounters[0] = lastCounters[1] = lastCounters[2] = -1;
        for (int c = 0; c < kControllers; ++c) { shotsInbox[c] = ShotsInbox{}; knownTears[c] = Known{}; knownBombs[c] = Known{}; tearsHeardAt[c] = 0; }
        for (auto& box : inbox) box = Inbox{};
        for (auto& tried : deathTried) tried = 0;
        for (int c = 0; c < kControllers; ++c) { aliveSeen[c] = takenDone[c] = takenSince[c] = 0; takenKnown[c] = false; }
        for (auto& taken : takenLog) taken = Taken{};
        takenTotal = 0; applyingTaken = false; applyingGrid = false;
        hostClearRoom = 0xfffffffe; hostClear = 0; hostHeardAt = 0;
        lastRoom = followRoom = 0xfffffffe; roomEpoch = heardEpoch = followTried = 0; following = false;
        worldInbox = World{}; worldAt = 0; worldApplied = 0; livedRoom = aliasRoom = 0xffffffff; livedCount = deathCount = aliasCount = orphanCount = missingCount = 0;
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
        if (!failure) {
            failure = ExchangeSlot(playerDamageSlot, reinterpret_cast<void*>(originalPlayerDamage), reinterpret_cast<void*>(&OnPlayerDamage));
            if (failure) {
                ExchangeSlot(slot, reinterpret_cast<void*>(&OnInput), reinterpret_cast<void*>(original));
                ExchangeSlot(playerSlot, reinterpret_cast<void*>(&OnPlayer), reinterpret_cast<void*>(originalPlayer));
                ExchangeSlot(damageSlot, reinterpret_cast<void*>(&OnNpcDamage), reinterpret_cast<void*>(originalDamage));
            }
        }
        if (!failure) {
            failure = ExchangeSlot(pickupSlot, reinterpret_cast<void*>(originalCollision), reinterpret_cast<void*>(&OnPickupCollision));
            if (failure) {
                ExchangeSlot(slot, reinterpret_cast<void*>(&OnInput), reinterpret_cast<void*>(original));
                ExchangeSlot(playerSlot, reinterpret_cast<void*>(&OnPlayer), reinterpret_cast<void*>(originalPlayer));
                ExchangeSlot(damageSlot, reinterpret_cast<void*>(&OnNpcDamage), reinterpret_cast<void*>(originalDamage));
                ExchangeSlot(playerDamageSlot, reinterpret_cast<void*>(&OnPlayerDamage), reinterpret_cast<void*>(originalPlayerDamage));
            }
        }
        if (!failure && !host && !clearHooked) {
            failure = HookClear();
            if (failure) {
                ExchangeSlot(pickupSlot, reinterpret_cast<void*>(&OnPickupCollision), reinterpret_cast<void*>(originalCollision));
                ExchangeSlot(slot, reinterpret_cast<void*>(&OnInput), reinterpret_cast<void*>(original));
                ExchangeSlot(playerSlot, reinterpret_cast<void*>(&OnPlayer), reinterpret_cast<void*>(originalPlayer));
                ExchangeSlot(damageSlot, reinterpret_cast<void*>(&OnNpcDamage), reinterpret_cast<void*>(originalDamage));
                ExchangeSlot(playerDamageSlot, reinterpret_cast<void*>(&OnPlayerDamage), reinterpret_cast<void*>(originalPlayerDamage));
            }
        }
        if (!failure && !host) {
            failure = HookGrid();
            if (failure) {
                UnhookGrid(); UnhookClear();
                ExchangeSlot(pickupSlot, reinterpret_cast<void*>(&OnPickupCollision), reinterpret_cast<void*>(originalCollision));
                ExchangeSlot(slot, reinterpret_cast<void*>(&OnInput), reinterpret_cast<void*>(original));
                ExchangeSlot(playerSlot, reinterpret_cast<void*>(&OnPlayer), reinterpret_cast<void*>(originalPlayer));
                ExchangeSlot(damageSlot, reinterpret_cast<void*>(&OnNpcDamage), reinterpret_cast<void*>(originalDamage));
                ExchangeSlot(playerDamageSlot, reinterpret_cast<void*>(&OnPlayerDamage), reinterpret_cast<void*>(originalPlayerDamage));
            }
        }
        if (failure) { running = false; CloseNetwork(); Patch(kCompare, kCompareEntry, kCompareEqual); throw failure; }
        std::ofstream descriptor(folder / (L"native-" + std::to_wstring(GetCurrentProcessId()) + L".json"), std::ios::trunc);
        descriptor << "{\"pid\":" << GetCurrentProcessId() << ",\"role\":\"" << (host ? "native-host" : "native-guest") << "\",\"ownController\":" << ownController
                   << ",\"rules\":" << rules
                   << ",\"counters\":" << reinterpret_cast<std::uintptr_t>(&counters) << ",\"port\":" << port
                   << ",\"published\":" << reinterpret_cast<std::uintptr_t>(&published) << ",\"publishedBytes\":" << sizeof(published)
                   << ",\"publishedWorld\":" << reinterpret_cast<std::uintptr_t>(&publishedWorld) << ",\"publishedWorldBytes\":" << sizeof(publishedWorld)
                   << ",\"publishedShots\":" << reinterpret_cast<std::uintptr_t>(&publishedShots) << ",\"publishedShotsBytes\":" << sizeof(publishedShots)
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
        UnhookGrid(); UnhookClear();
        result = ExchangeSlot(slot, reinterpret_cast<void*>(&OnInput), reinterpret_cast<void*>(original));
        const DWORD second = ExchangeSlot(playerSlot, reinterpret_cast<void*>(&OnPlayer), reinterpret_cast<void*>(originalPlayer));
        const DWORD third = ExchangeSlot(damageSlot, reinterpret_cast<void*>(&OnNpcDamage), reinterpret_cast<void*>(originalDamage));
        const DWORD fourth = ExchangeSlot(playerDamageSlot, reinterpret_cast<void*>(&OnPlayerDamage), reinterpret_cast<void*>(originalPlayerDamage));
        const DWORD fifth = ExchangeSlot(pickupSlot, reinterpret_cast<void*>(&OnPickupCollision), reinterpret_cast<void*>(originalCollision));
        if (!result) result = second ? second : third ? third : fourth ? fourth : fifth;
        CloseNetwork();
        // The comparison stays off on purpose: the games have already diverged, and turning it back on would split the lobby.
    }
    ReleaseSRWLockExclusive(&lifecycle);
    return result;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
