#pragma once
#include <windows.h>
#include <filesystem>
#include <cstdint>

namespace isaac_probe {
// Replaces only the named import in host, forwarding every call to its previous target.
DWORD StartRecorder(HMODULE host, const std::filesystem::path& log_path,
                    const char* build_id, std::uint32_t expected_iat_rva = 0) noexcept;
DWORD StopRecorder() noexcept;
bool RecorderRunning() noexcept;
}
