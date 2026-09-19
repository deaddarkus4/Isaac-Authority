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
    c = Example(); c.buttons = 1u << (kActions - Bomb); Check(!Valid(c), "unknown button");
    c = Example(); c.sequence = 0; Check(!Valid(c), "sequence starts at one");
}
void Actions() {
    auto c = Example();
    Check(Value(c, Right) == 1 && Value(c, Left) == 0 && Value(c, Up) == 0 && Value(c, Down) == 0, "movement right only");
    Check(Value(c, ShootUp) == 0.75f && Value(c, ShootDown) == 0 && Pressed(c, ShootUp) && !Pressed(c, ShootLeft), "shooting up");
    Check(Pressed(c, Bomb) && !Pressed(c, Item) && Value(c, kActions) == 0 && Value(c, -1) == 0 && Value(c, 12) == 0, "buttons and unknown actions");
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
        Wire(); Actions(); Freshness();
        for (int i = 1; i < argc; ++i) Contract(argv[i]);
        std::cout << "PASS input wire, action mapping, freshness, module contract\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
