// Makes the game's own online run on its built-in localhost service instead of Steam.
//
// J460 chooses its network service in one factory (RVA 0x629370, cdecl, one argument: 0 none, 1 localhost, 2 Steam,
// 3 EOS). The localhost service is complete code - lobbies and connections between the game's windows on one machine over
// WM_COPYDATA - but no caller ever asks for it. This module turns every request for a service into a request for that one,
// so that two isolated instances can play the game's online with each other and nothing reaches Steam or another player.
//
// Unlike the other modules this one changes code: six bytes at the factory's entry become a jump to a stub that rewrites
// the argument and runs the six original bytes. It refuses to do that anywhere but in an isolated test instance.
//
// It also gives the game's "local user" the identity the localhost lobby knows this game by: the game's window handle with
// 0xfefefefe above it. With Steam running the local user carries the Steam id instead; the game then files its own save
// under an id no lobby member has, never finds a save for its own player, and the match never starts (RVA 0x50c400).
#include "profile.hpp"
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
constexpr std::uintptr_t kFactory = 0x629370, kSaveLeaf = 0x77e04c;
// The local user object (its 64-bit id at +8), the GLFW window the game runs in and the word that says it is a Win32 one.
constexpr std::uintptr_t kLocalUser = 0x873674, kWindow = 0x87999c, kPlatform = 0x873694, kWin32Handle = 0x2b4;
constexpr std::uint32_t kWin32 = 0x60001, kLocalhostUser = 0xfefefefe;
constexpr std::array<std::uint8_t, 6> kEntry{0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08};  // push ebx; mov ebx, esp; sub esp, 8
constexpr char kIsolated[] = "IsaacAuthority-";
std::uint8_t* stub = nullptr; std::uint8_t* entry = nullptr;
volatile LONG counters[2]{};  // requests for a service; requests turned into localhost
std::uint32_t* identity = nullptr; std::uint32_t steamIdentity[2]{};  // kept in memory only, to put it back on Stop

// True when the local user now answers to the id the localhost service gives this game.
bool Rename(std::uintptr_t base) {
    const auto user = *reinterpret_cast<std::uint32_t**>(base + kLocalUser);
    const auto window = *reinterpret_cast<std::uintptr_t*>(base + kWindow);
    if (!user || !window || *reinterpret_cast<std::uint32_t*>(base + kPlatform) != kWin32) return false;
    const auto handle = *reinterpret_cast<HWND*>(window + kWin32Handle);
    DWORD owner = 0;
    if (!IsWindow(handle) || !GetWindowThreadProcessId(handle, &owner) || owner != GetCurrentProcessId()) return false;
    steamIdentity[0] = user[2]; steamIdentity[1] = user[3];
    user[2] = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(handle)); user[3] = kLocalhostUser;
    identity = user;
    return true;
}

void Put(std::uint8_t*& at, std::initializer_list<std::uint8_t> bytes) { for (const auto byte : bytes) *at++ = byte; }
void Put32(std::uint8_t*& at, std::uint32_t value) { std::memcpy(at, &value, 4); at += 4; }

DWORD Write(const std::array<std::uint8_t, 6>& bytes, const std::array<std::uint8_t, 6>& expected) {
    DWORD old = 0;
    if (!VirtualProtect(entry, bytes.size(), PAGE_EXECUTE_READWRITE, &old)) return GetLastError();
    const bool matches = std::memcmp(entry, expected.data(), expected.size()) == 0;
    if (matches) std::memcpy(entry, bytes.data(), bytes.size());
    DWORD ignored = 0; const BOOL restored = VirtualProtect(entry, bytes.size(), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), entry, bytes.size());
    return !matches ? ERROR_REVISION_MISMATCH : restored ? ERROR_SUCCESS : GetLastError();
}

std::array<std::uint8_t, 6> Jump() {
    std::array<std::uint8_t, 6> jump{0xE9, 0, 0, 0, 0, 0x90};
    const auto distance = static_cast<std::uint32_t>(stub - (entry + 5)); std::memcpy(jump.data() + 1, &distance, 4);
    return jump;
}
}

extern "C" DWORD WINAPI IsaacAuthorityLocalhostStart(void*) noexcept {
    try {
        if (stub) return ERROR_ALREADY_INITIALIZED;
        wchar_t image[32768]{};
        if (!GetModuleFileNameW(nullptr, image, 32768) || !isaac_probe::AnalyzeBytes(isaac_probe::ReadFile(image)).supported) return ERROR_BAD_EXE_FORMAT;
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        // The isolated launcher replaces the game's save folder name; the user's own game keeps the original and is left alone.
        if (std::memcmp(reinterpret_cast<const char*>(base + kSaveLeaf), kIsolated, sizeof(kIsolated) - 1) != 0) return ERROR_ACCESS_DENIED;
        entry = reinterpret_cast<std::uint8_t*>(base + kFactory);
        if (std::memcmp(entry, kEntry.data(), kEntry.size()) != 0) return ERROR_REVISION_MISMATCH;
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&Jump), &pinned)) return GetLastError();
        auto* code = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!code) return GetLastError();
        auto* at = code;
        Put(at, {0xFF, 0x05}); Put32(at, reinterpret_cast<std::uint32_t>(&counters[0]));   // inc [requests]
        Put(at, {0x83, 0x7C, 0x24, 0x04, 0x00});                                           // cmp dword [esp+4], 0   "no service" stays
        Put(at, {0x74, 0x0E});                                                             // je original
        Put(at, {0xC7, 0x44, 0x24, 0x04, 0x01, 0x00, 0x00, 0x00});                         // mov dword [esp+4], 1   localhost
        Put(at, {0xFF, 0x05}); Put32(at, reinterpret_cast<std::uint32_t>(&counters[1]));   // inc [turned]
        for (const auto byte : kEntry) *at++ = byte;                                       // original: the six bytes of the entry
        Put(at, {0xE9}); Put32(at, static_cast<std::uint32_t>((entry + kEntry.size()) - (at + 4)));
        DWORD old = 0;
        if (!VirtualProtect(code, 64, PAGE_EXECUTE_READ, &old)) { const auto error = GetLastError(); VirtualFree(code, 0, MEM_RELEASE); return error; }
        stub = code;
        const DWORD failure = Write(Jump(), kEntry);
        if (failure) { stub = nullptr; VirtualFree(code, 0, MEM_RELEASE); return failure; }
        const bool renamed = Rename(base);
        wchar_t local[32768]{}; const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
        if (n && n < 32768) {
            const auto directory = std::filesystem::path(local) / L"IsaacAuthority"; std::filesystem::create_directories(directory);
            std::ofstream descriptor(directory / (L"localhost-" + std::to_wstring(GetCurrentProcessId()) + L".json"), std::ios::trunc);
            descriptor << "{\"pid\":" << GetCurrentProcessId() << ",\"role\":\"localhost-service\",\"factoryRva\":" << kFactory
                       << ",\"stub\":" << reinterpret_cast<std::uintptr_t>(stub) << ",\"counters\":" << reinterpret_cast<std::uintptr_t>(&counters)
                       << ",\"localUserRenamed\":" << (renamed ? "true" : "false") << "}\n";
        }
        return ERROR_SUCCESS;
    } catch (...) { return ERROR_INVALID_DATA; }
}

extern "C" DWORD WINAPI IsaacAuthorityLocalhostStop(void*) noexcept {
    if (!stub) return ERROR_NOT_READY;
    const DWORD failure = Write(kEntry, Jump());
    if (identity) { identity[2] = steamIdentity[0]; identity[3] = steamIdentity[1]; identity = nullptr; }
    // The stub stays allocated: a thread may still be inside it, and it is 64 bytes.
    if (!failure) stub = nullptr;
    return failure;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
