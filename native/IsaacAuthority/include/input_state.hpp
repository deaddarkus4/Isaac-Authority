#pragma once
#include <cstddef>
#include <cstdint>

// A client's input for one player of the host's game. The host keeps simulating that player with the game's own
// movement and firing code; the client never sends positions.
namespace authority::input {
constexpr std::size_t kBytes = 56;
constexpr std::uint64_t kMaxAgeMs = 250;
constexpr int kMaxController = 7;
// ButtonAction values of the game: four movement and four shooting directions, then single buttons.
enum Action : int { Left = 0, Right, Up, Down, ShootLeft, ShootRight, ShootUp, ShootDown, Bomb, Item, PillCard, Drop,
    MenuConfirm = 14, MenuBack = 15, Join = 19, MenuLeft = 20, MenuRight = 21, MenuUp = 22, MenuDown = 23, kActions = 40 };
// Bit n of Command::buttons is kButtons[n]. Join is how a local co-op player enters: the game polls every free
// controller index for it, so a client can join the host's run without a physical controller.
constexpr int kButtons[] = {Bomb, Item, PillCard, Drop, Join, MenuConfirm, MenuBack, MenuLeft, MenuRight, MenuUp, MenuDown};
constexpr std::uint32_t kButtonCount = sizeof(kButtons) / sizeof(kButtons[0]);
struct Command {
    std::uint64_t session = 0, timeMs = 0;
    std::uint32_t sequence = 0;
    std::int32_t controller = 0;
    float moveX = 0, moveY = 0, shootX = 0, shootY = 0; // -1..1, screen axes: +x right, +y down
    std::uint32_t buttons = 0;                          // bit n is kButtons[n]
};
bool Valid(const Command& command);
bool Encode(const Command& command, std::uint8_t* bytes);
bool Decode(const std::uint8_t* bytes, std::size_t size, Command& command);
bool Known(int action);
// Analogue value the game would read from a device for this action: 0..1.
float Value(const Command& command, int action);
bool Pressed(const Command& command, int action);
// Accepts only newer, fresh commands of one session; a stale command never moves the player.
class Gate {
public:
    explicit Gate(std::uint64_t session) : session_(session) {}
    bool Receive(const Command& command, std::uint64_t now);
private:
    std::uint64_t session_;
    std::uint32_t sequence_ = 0;
};
bool Fresh(const Command& command, std::uint64_t now);
// The client side. What the local device answers for the actions of play becomes one command; menu buttons and the
// co-op join stay local, so a client's Enter key never confirms anything in the host's game.
constexpr int kCaptured[] = {Left, Right, Up, Down, ShootLeft, ShootRight, ShootUp, ShootDown, Bomb, Item, PillCard, Drop};
bool Captured(int action);
struct Sample { float value[kActions] = {}; };
// Axes and buttons only; the caller names the session, sequence, time and controller.
Command Compose(const Sample& sample);
bool Neutral(const Command& command);
}
