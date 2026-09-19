#include "snapshot_receiver.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace authority;
using namespace authority::bridge;
namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
State Example() { State s; s.session = 123; s.seed = 42; s.sequence = 1; s.sentMs = 1000;
    s.body = {{300, 240}, {0, 0}}; return s; }
void Protocol() {
    auto s = Example(); auto b = Encode(s); State out;
    Check(Decode(b.data(), b.size(), out) && out.session == 123 && out.seed == 42 &&
        out.body.position.x == 300 && out.sentMs == 1000, "single player wire roundtrip");
    for (std::size_t n = 0; n < b.size(); ++n) Check(!Decode(b.data(), n, out), "truncated datagram");
    Check(!Decode(b.data(), b.size() + 1, out), "oversized datagram");
    for (auto offset : {0, 4, 6, 28, 56, 60}) {
        auto bad = b; bad[offset] ^= 0x55; Check(!Decode(bad.data(), bad.size(), out), "invalid header or reserved fields");
    }
    for (float invalid : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(), 501.0f, 99.0f}) {
        auto bad = s; bad.body.position.x = invalid; const auto bytes = Encode(bad);
        Check(!Decode(bytes.data(), bytes.size(), out), "nonfinite or out of room state");
    }
    Gate gate(123, 42);
    auto bad = s; bad.session = 124; Check(!gate.Accept(bad, 1000), "foreign session");
    bad = s; bad.seed = 43; Check(!gate.Accept(bad, 1000), "different player incarnation");
    bad = s; bad.epoch = 2; Check(!gate.Accept(bad, 1000), "different room epoch");
    bad = s; bad.sequence = 0; Check(!gate.Accept(bad, 1000), "zero sequence");
    Check(!gate.Accept(s, 999), "future timestamp"); Check(!gate.Accept(s, 1251), "expired in transit");
    Check(gate.Accept(s, 1000) && !gate.Accept(s, 1001), "duplicate rejected");
    s.sequence = 3; Check(gate.Accept(s, 1000), "missing predecessor does not block");
    s.sequence = 2; Check(!gate.Accept(s, 1000), "out of order state rejected");
    const Body baseline{{292, 240}, {0, 0}};
    Check(CanApply(s, baseline, baseline, 1001, true), "bounded fresh correction");
    Check(!CanApply(s, baseline, baseline, 1251, true), "expires while game thread paused");
    Check(!CanApply(s, baseline, baseline, 1001, false), "room/player lifetime change");
    Check(!CanApply(s, {{280, 240}, {0, 0}}, baseline, 1001, true), "large current delta");
    Check(!CanApply(s, baseline, {{280, 240}, {0, 0}}, 1001, true), "total displacement bound");
    Check(!CanApply(s, {{292, 240}, {1, 0}}, baseline, 1001, true), "moving player not eligible");
}
void ReplicaRules() {
    auto state = Example(); state.replica = true; state.body.velocity = {2, -1};
    const auto bytes = Encode(state); State decoded;
    Check(Decode(bytes.data(), bytes.size(), decoded) && decoded.replica && decoded.body.velocity.x == 2,
        "moving replica state roundtrip");
    Gate stationary(123, 42), replica(123, 42, true);
    Check(!stationary.Accept(decoded, 1000) && replica.Accept(decoded, 1000), "receiver policies cannot be mixed");
    auto old = Example(); Check(!replica.Accept(old, 1000), "replica rejects stationary-test packet kind");
    PlayerReplica playback;
    const Body initial{{320, 280}, {0, 0}};
    Check(!playback.Hold(1000, 5000, true), "before first snapshot use original player simulation");
    Check(playback.Adopt(state, initial, 1000, true), "adopt initial real game state");
    Check(playback.Hold(2000, 5000, true) && playback.Current().position.x == 300 &&
        playback.Current().velocity.x == 2, "packet gap holds snapshot without predicting motion");
    auto stale = state; stale.body.position.x = 310;
    Check(!playback.Adopt(stale, playback.Current(), 2000, true), "stale state cannot change held body");
    stale.sentMs = 2000; Check(playback.Adopt(stale, playback.Current(), 2000, true), "fresh state resumes after gap");
    Check(!playback.Hold(2001, 5000, false), "room or player change resumes original update");
    Check(!playback.Adopt(stale, initial, 2001, true), "old attachment cannot reactivate after context change");
    playback.Reset(); Check(playback.Adopt(stale, initial, 2001, true), "new attachment clears invalidation");
    Check(!playback.Hold(5001, 5000, true), "session expiry stops suppression");
    state.body.position.x = 575;
    Check(!CanApplyReplica(state, initial, 1000, true), "large initial correction rejected");
    state.body.position.x = 300; state.body.velocity.x = 20;
    Check(!CanApplyReplica(state, initial, 1000, true), "excessive speed rejected");
}
struct Sender {
    SOCKET value = INVALID_SOCKET;
    Sender() {
        WSADATA data{}; Check(WSAStartup(MAKEWORD(2, 2), &data) == 0, "sender Winsock startup");
        value = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        Check(value != INVALID_SOCKET, "sender socket");
    }
    ~Sender() { if (value != INVALID_SOCKET) closesocket(value); WSACleanup(); }
    void Send(unsigned short port, const State& state) {
        auto bytes = Encode(state); sockaddr_in to{}; to.sin_family = AF_INET;
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK); to.sin_port = htons(port);
        Check(sendto(value, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
            reinterpret_cast<sockaddr*>(&to), sizeof(to)) == static_cast<int>(bytes.size()), "send UDP state");
    }
};
void Await(Receiver& receiver, unsigned accepted, unsigned rejected) {
    const auto until = GetTickCount64() + 1500;
    while (GetTickCount64() < until) {
        auto stats = receiver.Stats(); if (stats.accepted >= accepted && stats.rejected >= rejected) return;
        Sleep(1);
    }
    throw std::runtime_error("receiver did not process expected datagrams");
}
void Network() {
    Receiver receiver; Check(receiver.Start(42) == 0 && receiver.Port(), "start receiver on ephemeral loopback port");
    Sender sender, other;
    auto s = Example(); s.session = receiver.Session(); s.sentMs = GetTickCount64();
    auto bad = s; bad.session ^= 1; other.Send(receiver.Port(), bad); Await(receiver, 0, 1);
    sender.Send(receiver.Port(), s); Await(receiver, 1, 1);
    sender.Send(receiver.Port(), s); Await(receiver, 1, 2);
    s.sequence = 2; other.Send(receiver.Port(), s); Await(receiver, 1, 3);
    s.sequence = 3; sender.Send(receiver.Port(), s); Await(receiver, 2, 3);
    s.sequence = 2; sender.Send(receiver.Port(), s); Await(receiver, 2, 4);
    State latest; Check(receiver.Take(latest) && latest.sequence == 3 && !receiver.Take(latest), "latest-only mailbox, consume once");
    Check(receiver.Stats().replaced == 1, "mailbox replacement accounted");
    s.sequence = 4; s.sentMs = GetTickCount64(); sender.Send(receiver.Port(), s); Await(receiver, 3, 4);
    Sleep(270);
    Check(receiver.Take(latest) && !CanApply(latest, s.body, s.body, GetTickCount64(), true), "pending packet expires during paused consumer");
    receiver.Stop(); Check(receiver.Stats().errors == 0, "clean receiver shutdown");
    Check(receiver.Start(42) == 0, "restart receiver");
    Check(receiver.Session() != s.session, "restart rotates session");
    sender.Send(receiver.Port(), s); Await(receiver, 0, 1); receiver.Stop();
}
int Fixture() {
    Receiver receiver; Check(receiver.Start(42) == 0, "fixture start");
    Body baseline{{300, 240}, {0, 0}}, current = baseline;
    std::cout << "{\"pid\":" << GetCurrentProcessId() << ",\"host\":\"127.0.0.1\",\"port\":" << receiver.Port()
        << ",\"session\":\"" << receiver.Session() << "\",\"epoch\":1,\"seed\":42,\"deadlineMs\":" << receiver.Deadline()
        << ",\"baseline\":[300,240,0,0]}" << std::endl;
    const auto end = GetTickCount64() + 3000;
    unsigned applied = 0, refused = 0, last = 0, ticks = 0;
    while (GetTickCount64() < end) {
        ++ticks;
        State state;
        if (receiver.Take(state)) {
            if (CanApply(state, current, baseline, GetTickCount64(), true)) {
                current = state.body; last = state.sequence; ++applied;
            } else ++refused;
        }
        Sleep(5);
    }
    receiver.Stop();
    std::cout.precision(9);
    std::cout << "{\"applied\":" << applied << ",\"refused\":" << refused << ",\"last\":" << last
        << ",\"ticks\":" << ticks << ",\"rejected\":" << receiver.Stats().rejected
        << ",\"body\":[" << current.position.x << ',' << current.position.y << ',' << current.velocity.x << ','
        << current.velocity.y << "]}" << std::endl;
    return 0;
}
}
int main(int argc, char**) {
    try {
        if (argc > 1) return Fixture();
        Protocol(); ReplicaRules(); Network(); std::cout << "PASS snapshot wire, replica hold/resume, lifetime, freshness, UDP, peer, bounded mailbox, restart\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
