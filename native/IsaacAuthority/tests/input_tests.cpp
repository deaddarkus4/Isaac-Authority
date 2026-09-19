#include <windows.h>
#include "input_state.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
using namespace authority::input;
namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
Command Example() { Command c; c.session = 77; c.sequence = 1; c.timeMs = 1000; c.controller = 1; c.moveX = 1; c.shootY = -0.75f; c.buttons = 1; return c; }
void Wire() {
    auto c = Example(); std::uint8_t bytes[kBytes]{}; Command out;
    Check(Encode(c, bytes) && Decode(bytes, kBytes, out) && out.session == 77 && out.sequence == 1 && out.controller == 1 &&
        out.moveX == 1 && out.shootY == -0.75f && out.buttons == 1 && out.timeMs == 1000, "input wire roundtrip");
    for (std::size_t n = 0; n < kBytes; ++n) Check(!Decode(bytes, n, out), "reject truncated command");
    for (auto at : {0, 4, 52}) { auto bad = bytes[at]; bytes[at] ^= 0x55; Check(!Decode(bytes, kBytes, out), "reject header and reserved field"); bytes[at] = bad; }
    c = Example(); c.moveX = 1.5f; Check(!Valid(c) && !Encode(c, bytes), "an axis is at most one: the client cannot ask for extra speed");
    c = Example(); c.shootX = std::numeric_limits<float>::quiet_NaN(); Check(!Valid(c), "NaN axis");
    c = Example(); c.controller = 8; Check(!Valid(c), "controller out of range");
    c = Example(); c.controller = -1; Check(!Valid(c), "the any-device index is never driven");
    c = Example(); c.buttons = 1u << kButtonCount; Check(!Valid(c), "unknown button");
    c = Example(); c.sequence = 0; Check(!Valid(c), "sequence starts at one");
}
void Actions() {
    auto c = Example();
    Check(Value(c, Right) == 1 && Value(c, Left) == 0 && Value(c, Up) == 0 && Value(c, Down) == 0, "movement right only");
    Check(Value(c, ShootUp) == 0.75f && Value(c, ShootDown) == 0 && Pressed(c, ShootUp) && !Pressed(c, ShootLeft), "shooting up");
    Check(Pressed(c, Bomb) && !Pressed(c, Item) && Value(c, kActions) == 0 && Value(c, -1) == 0 && Value(c, 12) == 0, "buttons and unknown actions");
    Check(Known(Left) && Known(ShootDown) && Known(Drop) && Known(Join) && Known(MenuDown) && !Known(12) && !Known(13) && !Known(16) && !Known(-1) && !Known(kActions),
        "pause, map and restart stay with the host's own devices");
    c.buttons = 1u << 4; Check(Pressed(c, Join) && !Pressed(c, Bomb) && !Pressed(c, MenuConfirm), "join is its own button");
    c.buttons = 1u << 5 | 1u << 8; Check(Pressed(c, MenuConfirm) && Pressed(c, MenuRight) && !Pressed(c, MenuLeft) && !Pressed(c, Join), "menu buttons");
    c.moveX = -0.4f; Check(Value(c, Left) == 0.4f && !Pressed(c, Left), "a light tilt has a value but is not a press");
    Command idle; Check(Value(idle, Left) == 0 && !Pressed(idle, Bomb), "a default command is neutral");
}
void Freshness() {
    Gate gate(77); auto c = Example();
    Check(!gate.Receive(c, 999) && !gate.Receive(c, 1251), "timestamp bounds");
    Check(gate.Receive(c, 1000) && !gate.Receive(c, 1000), "fresh then duplicate");
    auto older = c; older.moveX = -1; Check(!gate.Receive(older, 1000), "a replayed sequence cannot reverse the player");
    ++c.sequence; c.session = 78; Check(!gate.Receive(c, 1000), "foreign session");
    c.session = 77; Check(gate.Receive(c, 1100) && !Fresh(c, 1300), "accepted, then too old to steer");
}
void Capture() {
    Sample s; Check(Neutral(Compose(s)), "an untouched device composes a neutral command");
    s.value[Right] = 1; s.value[ShootUp] = 0.75f; s.value[Bomb] = 1;
    auto c = Compose(s);
    Check(c.moveX == 1 && c.moveY == 0 && c.shootX == 0 && c.shootY == -0.75f && c.buttons == 1 && !Neutral(c), "a device sample becomes axes and buttons");
    // What the client captured is what the host's game reads back for the same actions.
    for (const auto action : kCaptured) Check(Value(c, action) == s.value[action], "capture and injection are inverse");
    s.value[Left] = 1; Check(Compose(s).moveX == 0, "opposite keys cancel");
    s = {}; s.value[Down] = 3; s.value[ShootLeft] = std::numeric_limits<float>::infinity(); s.value[Up] = std::numeric_limits<float>::quiet_NaN();
    c = Compose(s); c.session = 77; c.sequence = 1;
    Check(c.moveY == 0 && c.shootX == 0 && Valid(c), "a wild device answer never leaves the wire's range");
    s = {}; s.value[Down] = 3; c = Compose(s); Check(c.moveY == 1, "a tilt is clamped to one");
    s = {}; s.value[MenuConfirm] = 1; s.value[Join] = 1; s.value[MenuBack] = 1; s.value[12] = 1;
    Check(Neutral(Compose(s)), "menu buttons, join and the pause key stay on the client");
    Check(Captured(Left) && Captured(Drop) && !Captured(Join) && !Captured(MenuConfirm) && !Captured(12) && !Captured(-1) && !Captured(kActions),
        "only the actions of play are captured");
    s = {}; s.value[Item] = 0.4f; Check(Neutral(Compose(s)), "a half-pressed button is not pressed");
    std::uint8_t bytes[kBytes]{}; s = {}; s.value[Left] = 1; c = Compose(s); c.session = 5; c.sequence = 9; c.timeMs = 1000; Command out;
    Check(Encode(c, bytes) && Decode(bytes, kBytes, out) && out.moveX == -1 && out.controller == 0, "a captured command travels as INP1");
}
// A command that says "walk right at (n mod 100)/100": the value shows which command a step really played.
Command Numbered(std::uint32_t n) { Command c; c.session = 77; c.sequence = n; c.timeMs = 1000; c.controller = 1; c.moveX = (n % 100) / 100.0f; return c; }
void Steps() {
    Playout p; p.Reset(77); Command out;
    Check(!p.Step(out) && !p.Running(), "nothing to play before any command");
    for (std::uint32_t n = 1; n < kPlayoutTarget; ++n) Check(p.Receive(Numbered(n), 1000) && !p.Step(out), "the run starts only behind a small buffer");
    Check(p.Receive(Numbered(kPlayoutTarget), 1000) && p.Step(out) && out.sequence == 1 && out.moveX == 0.01f && p.Played() == 1 && p.starts == 1,
        "then exactly one command per step, from the first");
    // Arrivals out of order and in a burst are played in order, one per step.
    for (std::uint32_t n : {7u, 5u, 6u, 8u}) Check(p.Receive(Numbered(n), 1000), "a reordered burst is kept");
    for (std::uint32_t n = 2; n <= 8; ++n) Check(p.Step(out) && out.sequence == n && out.moveX == n / 100.0f && p.Played() == n, "in sequence order whatever the arrival order");
    Check(!p.substituted && !p.starved && !p.late, "nothing was missing so far");
    Check(!p.Receive(Numbered(8), 1000) && p.late == 1, "a command whose step was played is late");
    // Command 10 is lost: its step repeats command 9 and still counts, so step numbers stay step counts.
    p.Receive(Numbered(9), 1000); p.Receive(Numbered(11), 1000); p.Receive(Numbered(12), 1000);
    Check(p.Step(out) && out.sequence == 9, "nine");
    Check(p.Step(out) && out.sequence == 10 && out.moveX == 0.09f && p.substituted == 1, "a missing command is played as a repeat of the previous one");
    Check(!p.Receive(Numbered(10), 1000) && p.late == 2, "and its late arrival is dropped");
    Check(p.Step(out) && out.sequence == 11 && out.moveX == 0.11f && p.Step(out) && out.sequence == 12, "the run goes on");
    // Nothing newer has arrived: the player keeps doing what it did for a while, then belongs to nobody.
    for (std::uint32_t n = 13; n < 13 + kPlayoutSilence; ++n) Check(p.Step(out) && out.sequence == n && out.moveX == 0.12f, "a starved step repeats the last command");
    Check(p.starved == kPlayoutSilence && !p.Step(out) && !p.Running() && p.Played() == 12 + kPlayoutSilence, "a silent client is a neutral client");
    Check(!p.Step(out), "and stays neutral");
    for (std::uint32_t n = 100; n < 100 + kPlayoutTarget; ++n) p.Receive(Numbered(n), 1000);
    Check(p.Step(out) && out.sequence == 100 && p.starts == 2, "commands coming back start a new run where they are");
    // The host fell far behind the arrivals (a stall): it does not replay history.
    for (std::uint32_t n = 101; n <= 130; ++n) p.Receive(Numbered(n), 1000);
    Check(p.Step(out) && out.sequence == 130 - kPlayoutTarget && p.skipped == 130 - kPlayoutTarget - 101, "far behind the newest arrival, the steps in between are skipped");
    Playout q; q.Reset(77);
    Check(!q.Receive(Numbered(1), 1300) && !q.Receive(Numbered(0), 1000), "stale and invalid commands never enter");
    auto foreign = Numbered(1); foreign.session = 78; Check(!q.Receive(foreign, 1000), "foreign session");
    Check(q.Receive(Numbered(5), 1000) && !q.Receive(Numbered(5), 1000), "a duplicate is not a second command");
    Check(q.Receive(Numbered(3), 1000) && q.Receive(Numbered(4), 1000) && q.Receive(Numbered(6), 1000) && q.Step(out) && out.sequence == 3,
        "before the run starts an older command moves the start back");
    Check(q.Receive(Numbered(1000), 1000) && !q.Running() && !q.Step(out), "a jump far ahead forgets the rest and buffers again");
}
void Contract(const wchar_t* path) {
    HMODULE dll = LoadLibraryW(path); Check(dll != nullptr, "load input adapter");
    using Export = DWORD(WINAPI*)(void*);
    auto start = reinterpret_cast<Export>(GetProcAddress(dll, "IsaacAuthorityInputStart"));
    auto stop = reinterpret_cast<Export>(GetProcAddress(dll, "IsaacAuthorityInputStop"));
    Check(start && stop && start(nullptr) == ERROR_BAD_EXE_FORMAT && stop(nullptr) == ERROR_NOT_READY,
        "input adapter refuses unsupported host and inactive stop");
    FreeLibrary(dll);
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        Wire(); Actions(); Freshness(); Capture(); Steps();
        for (int i = 1; i < argc; ++i) Contract(argv[i]);
        std::cout << "PASS input wire, action mapping, freshness, client capture, playout, module contracts\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
