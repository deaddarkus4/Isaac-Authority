#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <map>

namespace authority {
constexpr std::size_t kHistory = 256;
struct Vec { float x = 0, y = 0; };
struct Body { Vec position, velocity; };
struct Input { std::uint32_t sequence = 0; Vec movement; };
struct Snapshot {
    std::uint32_t epoch = 1, tick = 0, acknowledged = 0;
    std::array<Body, 2> players{};
};
bool ValidMovement(Vec value);
bool ValidBody(const Body& body);
// Standalone test-room motion, NOT recovered Isaac physics.
void StepFixture(Body& body, Vec movement);

// Called once by the host's simulation tick. Receive never advances simulation.
class Host {
public:
    bool Receive(std::uint32_t epoch, Input input);
    void Tick(Vec localMovement = {});
    void NewRoom();
    const Snapshot& State() const { return state_; }
    std::size_t Queued() const { return pending_.size(); }
private:
    Snapshot state_;
    std::map<std::uint32_t, Input> pending_;
};

class Client {
public:
    // False means wait for a fresh snapshot; history is bounded, never silently lost.
    bool Predict(Vec movement, Input& generated);
    bool Reconcile(const Snapshot& snapshot);
    const Body& Predicted() const { return predicted_; }
    const Snapshot& Authoritative() const { return authoritative_; }
    std::size_t Pending() const { return pending_.size(); }
    const std::deque<Input>& PendingInputs() const { return pending_; }
    bool Ready() const { return ready_; }
    std::uint32_t Epoch() const { return authoritative_.epoch; }
    std::uint32_t Corrections() const { return corrections_; }
private:
    Snapshot authoritative_;
    Body predicted_;
    std::deque<Input> pending_;
    std::uint32_t next_ = 1, corrections_ = 0;
    bool ready_ = false;
};
}
