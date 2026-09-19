#pragma once
#include "authority.hpp"
#include <cstddef>
#include <vector>

namespace authority {
enum class Kind : std::uint16_t { Input = 1, Snapshot = 2, Hello = 3 };
struct Packet {
    Kind kind = Kind::Input;
    std::uint64_t session = 0;
    std::uint32_t epoch = 1;
    Input input;
    Snapshot snapshot;
};
constexpr std::size_t kPacketBytes = 72;
std::vector<std::uint8_t> Encode(const Packet& packet);
bool Decode(const std::uint8_t* data, std::size_t size, std::uint64_t expectedSession,
            Packet& output);
}
