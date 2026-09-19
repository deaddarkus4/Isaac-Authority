#include "profile.hpp"
#include "recorder.hpp"
#include <windows.h>
#include <cstring>
#include <vector>
#include <fstream>
#include <sstream>

namespace {
std::filesystem::path ProbeDirectory() {
    wchar_t local[32768]{};
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
    if (length == 0 || length >= 32768) throw std::runtime_error("LOCALAPPDATA unavailable");
    auto directory = std::filesystem::path(local) / L"IsaacNetProbe" / L"logs";
    std::filesystem::create_directories(directory);
    return directory;
}
}

// Explicit entry point, invoked outside the loader lock. No automatic hooks.
// Returns 0: supported, 1: unsupported host, 2: memory mismatch, 3: I/O error, 4: invalid argument.
extern "C" int __cdecl IsaacProbeInspect(const wchar_t* output_path) noexcept {
    if (output_path == nullptr || *output_path == L'\0') return 4;
    try {
        wchar_t executable[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
        if (length == 0 || length >= 32768) return 3;
        auto analysis = isaac_probe::AnalyzeBytes(isaac_probe::ReadFile(executable));
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        bool memory_matches = analysis.supported;
        if (memory_matches) {
            for (const auto& anchor : isaac_probe::Anchors()) {
                const auto length_bytes = std::strlen(anchor.expected_prefix);
                if (anchor.string_rva >= analysis.image_size || length_bytes > analysis.image_size - anchor.string_rva) {
                    memory_matches = false;
                    break;
                }
                std::vector<char> actual(length_bytes);
                SIZE_T read = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(base + anchor.string_rva),
                    actual.data(), length_bytes, &read) || read != length_bytes ||
                    std::memcmp(actual.data(), anchor.expected_prefix, length_bytes) != 0) {
                    memory_matches = false;
                    break;
                }
            }
        }
        isaac_probe::WriteReport(output_path, analysis, base, memory_matches);
        return !analysis.supported ? 1 : memory_matches ? 0 : 2;
    } catch (...) {
        return 3;
    }
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

extern "C" DWORD WINAPI IsaacProbeStart(void*) noexcept {
    if (isaac_probe::RecorderRunning()) return ERROR_ALREADY_EXISTS;
    try {
        wchar_t path[32768]{};
        if (!GetModuleFileNameW(nullptr, path, 32768)) return ERROR_BAD_EXE_FORMAT;
        const auto assessment = isaac_probe::AnalyzeBytes(isaac_probe::ReadFile(path));
        if (!assessment.supported) return ERROR_BAD_EXE_FORMAT;
        auto directory = ProbeDirectory();
        SYSTEMTIME now{}; GetSystemTime(&now);
        wchar_t stem[128]{};
        swprintf_s(stem, L"session-%04u%02u%02u-%02u%02u%02u-%lu-%llu",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
            GetCurrentProcessId(), GetTickCount64());
        const auto profile = directory / (std::wstring(stem) + L"-profile.json");
        if (IsaacProbeInspect(profile.c_str()) != 0) return ERROR_REVISION_MISMATCH;
        const auto events = directory / (std::wstring(stem) + L".jsonl");
        const DWORD result = isaac_probe::StartRecorder(GetModuleHandleW(nullptr), events,
            isaac_probe::kOriginalSha256, 0x71827c);
        if (result == ERROR_SUCCESS) {
            // Dedicated status file for the launcher, never a game file.
            std::ofstream status(directory.parent_path() / "last-session.txt", std::ios::trunc);
            status << events.u8string() << '\n';
        }
        return result;
    } catch (...) { return ERROR_INVALID_DATA; }
}

extern "C" DWORD WINAPI IsaacProbeStop(void*) noexcept { return isaac_probe::StopRecorder(); }
