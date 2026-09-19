#include "profile.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace isaac_probe {
namespace {
template<class T> bool ReadAt(const std::vector<std::uint8_t>& bytes, std::size_t offset, T& value) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) return false;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return true;
}
std::string Sha256(const std::vector<std::uint8_t>& bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Could not initialize SHA-256");
    unsigned char digest[32]{};
    const auto status = BCryptHash(algorithm, nullptr, 0,
        const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw std::runtime_error("Could not calculate SHA-256");
    std::ostringstream output;
    for (unsigned char value : digest) output << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(value);
    return output.str();
}
}

const std::vector<Anchor>& Anchors() {
    static const std::vector<Anchor> anchors = {
        {"broadcast_frame_input", 0x777e50, "NetInputDevice::broadcast_frame_input"},
        {"process_input_message", 0x777ea0, "NetInputDevice::ProcessInputMessage - CHECKSUM MISMATCH!"},
        {"confirm_desync", 0x777258, "[Frame: %u] NetDesyncHandler::process_desync_confirm_message()"},
        {"start_recovery", 0x777b00, "[Frame: %d] NetDesyncHandler::process_desync_recovery_start_message"},
        {"drop_plan", 0x777428, " - resolutionPlan = SYNC_PLAN_DROP"},
        {"input_delay_option", 0x77a69c, "OnlineInputDelay=%d"}
    };
    return anchors;
}

std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Could not open input file");
    const auto size = input.tellg();
    if (size < 0 || size > 256 * 1024 * 1024) throw std::runtime_error("Input size is invalid or exceeds 256 MiB");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()), size))
        throw std::runtime_error("Could not read the entire input file");
    return bytes;
}

Analysis AnalyzeBytes(const std::vector<std::uint8_t>& bytes) {
    Analysis result;
    result.byte_count = bytes.size();
    if (bytes.size() > (std::numeric_limits<ULONG>::max)()) {
        result.reason = "input_too_large";
        return result;
    }
    result.sha256 = Sha256(bytes);
    IMAGE_DOS_HEADER dos{};
    if (!ReadAt(bytes, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) {
        result.reason = "invalid_dos_header";
        return result;
    }
    IMAGE_NT_HEADERS32 nt{};
    const auto nt_offset = static_cast<std::size_t>(dos.e_lfanew);
    if (!ReadAt(bytes, nt_offset, nt) || nt.Signature != IMAGE_NT_SIGNATURE) {
        result.reason = "invalid_pe_header";
        return result;
    }
    if (nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        result.reason = "unsupported_architecture";
        return result;
    }
    if (result.sha256 != kOriginalSha256) {
        result.reason = "unsupported_executable_hash";
        return result;
    }
    result.image_size = nt.OptionalHeader.SizeOfImage;
    const std::size_t section_offset = nt_offset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
    for (const auto& anchor : Anchors()) {
        bool found = false;
        for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
            IMAGE_SECTION_HEADER section{};
            if (!ReadAt(bytes, section_offset + i * sizeof(section), section)) break;
            if (anchor.string_rva < section.VirtualAddress) continue;
            const auto delta = anchor.string_rva - section.VirtualAddress;
            const auto length = std::strlen(anchor.expected_prefix);
            if (delta >= section.SizeOfRawData || length > section.SizeOfRawData - delta) continue;
            const std::size_t offset = static_cast<std::size_t>(section.PointerToRawData) + delta;
            if (offset <= bytes.size() && length <= bytes.size() - offset &&
                std::memcmp(bytes.data() + offset, anchor.expected_prefix, length) == 0) found = true;
        }
        if (!found) {
            result.reason = "expected_string_anchor_missing";
            return result;
        }
    }
    result.supported = true;
    result.reason = "original_j460_inventory_verified";
    return result;
}

void WriteReport(const std::filesystem::path& path, const Analysis& analysis,
                 std::uintptr_t runtime_base, bool runtime_strings_verified) {
    std::ostringstream output;
    output << "{\n  \"schemaVersion\": 1,\n  \"mode\": \"inventory_only\",\n  \"hooksInstalled\": 0,\n"
        << "  \"supported\": " << (analysis.supported ? "true" : "false") << ",\n"
        << "  \"reason\": \"" << analysis.reason << "\",\n"
        << "  \"sha256\": \"" << analysis.sha256 << "\",\n"
        << "  \"bytes\": " << analysis.byte_count << ",\n"
        << "  \"runtimeBase\": " << runtime_base << ",\n"
        << "  \"runtimeStringsVerified\": " << (runtime_strings_verified ? "true" : "false") << ",\n"
        << "  \"anchors\": [";
    if (analysis.supported) {
        bool first = true;
        for (const auto& anchor : Anchors()) {
            if (!first) output << ',';
            first = false;
            output << "\n    {\"name\": \"" << anchor.name << "\", \"stringRva\": " << anchor.string_rva << '}';
        }
    }
    output << "\n  ]\n}\n";
    const std::string text = output.str();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Could not create report: path must be writable and must not already exist");
    DWORD written = 0;
    const BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
    if (!ok || written != text.size()) throw std::runtime_error("Report write failed");
}
}
