#include "authority.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace authority {
bool ValidMovement(Vec v) {
    return std::isfinite(v.x) && std::isfinite(v.y) &&
        std::abs(v.x) <= 1 && std::abs(v.y) <= 1;
}
bool ValidBody(const Body& b) {
    return std::isfinite(b.position.x) && std::isfinite(b.position.y) &&
        std::isfinite(b.velocity.x) && std::isfinite(b.velocity.y) &&
        std::abs(b.position.x) <= 10000 && std::abs(b.position.y) <= 10000 &&
        std::abs(b.velocity.x) <= 100 && std::abs(b.velocity.y) <= 100;
}
void StepFixture(Body& b, Vec m) {
    const float length = std::sqrt(m.x * m.x + m.y * m.y);
    if (length > 1) { m.x /= length; m.y /= length; }
    b.velocity.x = b.velocity.x * 0.8f + m.x * 0.5f;
    b.velocity.y = b.velocity.y * 0.8f + m.y * 0.5f;
    b.position.x = std::clamp(b.position.x + b.velocity.x, -400.0f, 400.0f);
    b.position.y = std::clamp(b.position.y + b.velocity.y, -240.0f, 240.0f);
}
bool Host::Receive(std::uint32_t epoch, Input in) {
    if (epoch != state_.epoch || !ValidMovement(in.movement) ||
        in.sequence <= state_.acknowledged ||
        in.sequence - state_.acknowledged > kHistory) return false;
    return pending_.emplace(in.sequence, in).second;
}
void Host::Tick(Vec local) {
    if (!ValidMovement(local)) throw std::invalid_argument("invalid host input");
    if (state_.tick == std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("session tick exhausted");
    Vec remote{};
    if (!pending_.empty()) {
        const auto it = pending_.begin();
        remote = it->second.movement;
        state_.acknowledged = it->first; // Missing earlier commands are retired too.
        pending_.erase(it);
    }
    StepFixture(state_.players[0], local);
    StepFixture(state_.players[1], remote);
    ++state_.tick;
}
void Host::NewRoom() {
    if (state_.epoch == std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("room epoch exhausted");
    const auto epoch = state_.epoch + 1;
    state_ = Snapshot{};
    state_.epoch = epoch;
    pending_.clear();
}
bool Client::Predict(Vec m, Input& generated) {
    if (!ready_ || !ValidMovement(m) || pending_.size() >= kHistory ||
        next_ == std::numeric_limits<std::uint32_t>::max()) return false;
    generated = {next_++, m};
    pending_.push_back(generated);
    StepFixture(predicted_, m);
    return true;
}
bool Client::Reconcile(const Snapshot& s) {
    if (!s.epoch || !ValidBody(s.players[0]) || !ValidBody(s.players[1])) return false;
    if (ready_ && (s.epoch < authoritative_.epoch ||
        (s.epoch == authoritative_.epoch && (s.tick <= authoritative_.tick ||
        s.acknowledged < authoritative_.acknowledged || s.acknowledged >= next_)))) return false;
    if (!ready_ || s.epoch != authoritative_.epoch) {
        if (s.acknowledged != 0) return false; // No mid-room join in this prototype.
        pending_.clear();
        next_ = 1;
    }
    authoritative_ = s;
    while (!pending_.empty() && pending_.front().sequence <= s.acknowledged)
        pending_.pop_front();
    Body corrected = s.players[1];
    for (const Input& in : pending_) StepFixture(corrected, in.movement);
    if (ready_ && (std::abs(corrected.position.x - predicted_.position.x) > 0.001f ||
        std::abs(corrected.position.y - predicted_.position.y) > 0.001f ||
        std::abs(corrected.velocity.x - predicted_.velocity.x) > 0.001f ||
        std::abs(corrected.velocity.y - predicted_.velocity.y) > 0.001f)) ++corrections_;
    predicted_ = corrected;
    ready_ = true;
    return true;
}
}
