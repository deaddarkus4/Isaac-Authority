#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace isaac_probe {
inline constexpr const char* kOriginalSha256 = "3bdfc8bae0dc7e334b76009d0ad45dfbb16ee5f00c06ffbc3a0094e34d44616b";
struct Anchor {
    const char* name;
    std::uint32_t string_rva;
    const char* expected_prefix;
};
struct Analysis {
    bool supported = false;
    std::string reason;
    std::string sha256;
    std::size_t byte_count = 0;
    std::uint32_t image_size = 0;
};
const std::vector<Anchor>& Anchors();
std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path);
Analysis AnalyzeBytes(const std::vector<std::uint8_t>& bytes);
void WriteReport(const std::filesystem::path& path, const Analysis& analysis,
                 std::uintptr_t runtime_base = 0, bool runtime_strings_verified = false);
}
