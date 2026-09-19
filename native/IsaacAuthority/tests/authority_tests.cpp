#include "authority.hpp"
#include "wire.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace authority;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool Same(const Body& a, const Body& b) {
    return a.position.x == b.position.x && a.position.y == b.position.y &&
        a.velocity.x == b.velocity.x && a.velocity.y == b.velocity.y;
}
void Protocol() {
    Packet p; p.session = 123; p.input = {1, {0, -1}};
    auto bytes = Encode(p); Packet out;
    Check(Decode(bytes.data(), bytes.size(), 123, out), "input roundtrip");
    Check(out.input.sequence == 1 && out.input.movement.y == -1, "input fields");
    for (std::size_t size = 0; size < bytes.size(); ++size)
        Check(!Decode(bytes.data(), size, 123, out), "reject truncated datagram");
    auto extended = bytes; extended.push_back(0);
    Check(!Decode(extended.data(), extended.size(), 123, out), "reject oversized datagram");
    Check(!Decode(bytes.data(), bytes.size(), 124, out), "session isolation");
    for (auto at : {0, 4, 20, 36, 50}) {
        auto bad = bytes; bad[at] ^= 0x7f;
        Check(!Decode(bad.data(), bad.size(), 123, out), "invalid header or payload");
    }
    auto nan = bytes; nan[40] = 0; nan[41] = 0; nan[42] = 0xc0; nan[43] = 0x7f;
    Check(!Decode(nan.data(), nan.size(), 123, out), "reject NaN");
    p.kind = Kind::Snapshot; p.snapshot.tick = 81; p.snapshot.players[1].position = {7, 9};
    bytes = Encode(p);
    Check(Decode(bytes.data(), bytes.size(), 123, out) && out.snapshot.tick == 81 &&
        out.snapshot.players[1].position.y == 9, "snapshot roundtrip");
    p.kind = Kind::Hello; bytes = Encode(p);
    Check(Decode(bytes.data(), bytes.size(), 123, out) && out.kind == Kind::Hello, "hello roundtrip");
}
void AuthorityRules() {
    Host h;
    Check(h.Receive(1, {2, {1, 0}}), "accept command with missing predecessor");
    Check(!h.Receive(1, {2, {-1, 0}}), "reject conflicting duplicate");
    Check(!h.Receive(1, {10000, {1, 0}}), "bound future input");
    Check(!h.Receive(1, {3, {2, 0}}), "reject invalid movement");
    Check(!h.Receive(2, {3, {1, 0}}), "reject other room input");
    Check(h.State().tick == 0, "receiving must not advance host");
    h.Tick();
    Check(h.State().acknowledged == 2, "retire gap");
    Check(!h.Receive(1, {1, {1, 0}}), "reject late retired input");
    for (unsigned i = 0; i < 500; ++i) h.Tick({1, 0});
    Check(h.State().tick == 501 && h.State().players[0].position.x > 300,
        "host continues without remote input");
    Check(std::abs(h.State().players[1].velocity.x) < 0.0001f, "missing input becomes neutral");
    h.NewRoom();
    Check(h.State().epoch == 2 && h.State().tick == 0 && h.Queued() == 0, "room reset");
    Check(!h.Receive(1, {3, {1, 0}}), "old room cannot affect new world");
    Host burst;
    for (std::uint32_t i = 1; i <= kHistory; ++i) Check(burst.Receive(1, {i, {1, 0}}), "enqueue burst");
    burst.Tick();
    Check(burst.State().acknowledged == 1 && burst.Queued() == kHistory - 1,
        "input flood cannot speed up simulation");
}
void Prediction() {
    Host h; Client c; Input in;
    Check(!c.Predict({1, 0}, in), "snapshot required before prediction");
    Check(c.Reconcile(h.State()), "initial baseline");
    Check(c.Predict({1, 0}, in), "predict input one"); h.Receive(1, in); h.Tick();
    const auto snapshot = h.State();
    Check(c.Predict({0, -1}, in), "predict input two");
    Body expected = snapshot.players[1]; StepFixture(expected, in.movement);
    Check(c.Reconcile(snapshot) && Same(c.Predicted(), expected) && c.Pending() == 1,
        "replay only unacknowledged input");
    Check(!c.Reconcile(snapshot), "stale snapshot rejected");
    auto forged = snapshot; ++forged.tick; forged.acknowledged = 100;
    Check(!c.Reconcile(forged), "reject acknowledgement of unsent input");
    auto drift = snapshot; drift.tick += 2; drift.players[1].velocity.y += 0.52820444f;
    Check(c.Reconcile(drift) && c.Corrections() > 0, "velocity mismatch corrects without disconnect");
    h.NewRoom(); Check(c.Reconcile(h.State()), "new room baseline");
    Check(c.Pending() == 0 && Same(c.Predicted(), Body{}), "discard old room prediction");
    Check(!c.Reconcile(drift), "late snapshot cannot revert room");
    for (unsigned i = 0; i < kHistory; ++i) Check(c.Predict({0, 1}, in), "bounded prediction");
    Check(!c.Predict({0, 1}, in), "pause prediction on overflow");
    for (const auto& pending : c.PendingInputs()) h.Receive(c.Epoch(), pending);
    for (unsigned i = 0; i < kHistory; ++i) h.Tick();
    Check(c.Reconcile(h.State()) && c.Pending() == 0 && c.Predict({}, in), "recover after history fills");
}
void ImpairedNetwork() {
    struct TransitInput { unsigned due; Input input; };
    struct TransitState { unsigned due; Snapshot state; };
    Host host; Client client; client.Reconcile(host.State());
    std::vector<TransitInput> inputs;
    std::vector<TransitState> snapshots;
    unsigned dropped = 0, pausedHostTicks = 0;
    for (unsigned wall = 0; wall < 900; ++wall) {
        const bool clientPaused = wall >= 180 && wall < 360;
        if (!clientPaused && wall < 600) {
            Input in;
            client.Predict({wall % 120 < 60 ? 1.0f : -1.0f, -0.25f}, in);
            // Redundant unacknowledged commands: reordered, delayed, duplicated and dropped.
            unsigned count = 0;
            for (const auto& command : client.PendingInputs()) {
                if (++count > 8) break;
                if ((command.sequence + wall) % 5 == 0) { ++dropped; continue; }
                inputs.push_back({wall + 2 + (command.sequence * 7 + wall) % 13, command});
                if (wall % 7 == 0) inputs.push_back({wall + 20, command});
            }
        } else if (!clientPaused && wall >= 600) {
            for (const auto& in : client.PendingInputs()) inputs.push_back({wall + 2, in});
        }
        for (auto it = inputs.begin(); it != inputs.end();) {
            if (it->due <= wall) { host.Receive(1, it->input); it = inputs.erase(it); }
            else ++it;
        }
        host.Tick({0.1f, 0});
        if (clientPaused) ++pausedHostTicks;
        if (wall % 5 != 0) snapshots.push_back({wall + (wall * 7) % 17, host.State()});
        for (auto it = snapshots.begin(); it != snapshots.end();) {
            if (it->due <= wall) {
                if (!clientPaused) client.Reconcile(it->state);
                it = snapshots.erase(it);
            } else ++it;
        }
        Check(host.State().tick == wall + 1, "host has independent tick under network impairment");
        Check(host.Queued() <= kHistory && client.Pending() <= kHistory, "bounded network histories");
    }
    Check(client.Reconcile(host.State()), "final authoritative snapshot");
    Check(client.Pending() == 0 && Same(client.Predicted(), host.State().players[1]), "eventual convergence");
    Check(dropped > 100 && pausedHostTicks == 180 && client.Corrections() > 0, "fault scenario exercised");
    std::cout << "impaired_network host_ticks=900 paused_client_ticks=180 dropped_inputs=" << dropped
              << " corrections=" << client.Corrections() << " converged=true\n";
}
int main() {
    try { Protocol(); AuthorityRules(); Prediction(); ImpairedNetwork();
        std::cout << "PASS protocol, authority, prediction, impaired network\n"; return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
