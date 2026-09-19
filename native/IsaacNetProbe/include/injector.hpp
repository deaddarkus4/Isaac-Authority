#pragma once
#include <windows.h>
#include <filesystem>

namespace isaac_probe {
std::filesystem::path ProcessImagePath(DWORD pid);
DWORD FindProcess(const std::filesystem::path& expected_image);
// Used only after the caller validates the target and chooses our local DLL.
DWORD InvokeProbeExport(DWORD pid, const std::filesystem::path& dll,
                        const char* export_name, bool load_if_missing);
}
