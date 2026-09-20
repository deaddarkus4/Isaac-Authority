// Step 1 of changing the game's own online from inside: the local player answers to the keyboard at once.
//
// In the game's lockstep a player's input is scheduled some frames ahead and every game, the player's own included,
// applies it only then: 134-149 ms from key to movement between two windows of one machine, against 15 ms in a solo game.
// Here the owner of a character is its authority (the user's model): the local player's update reads the physical device
// directly, while the game's own net device keeps broadcasting the scheduled input to the others as before.
//
// The two games then no longer compute the same frames, which the lockstep takes for a desync and answers by splitting the
// lobby. So the comparison of frame checksums (RVA 0x50d4a0, thiscall, one argument) is made to answer "equal". That is a
// change of code, and like the localhost module this one refuses to work anywhere but in an isolated test instance.
//
// Which controller is the local player's is told from outside (the lobby's device numbers: 2, 3, ...), in
// %LOCALAPPDATA%\IsaacAuthority\native-<pid>.cfg as a single number: the harness reads it from the game's log.
#include "profile.hpp"
#include "vtable_slot.hpp"
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace {
using namespace authority;
// InputManager at RVA 0x857b18; slot 29 is WithDevice(controller, reader, in, out, deviceId), thiscall, ret 0x14.
constexpr std::uintptr_t kManager = 0x857b18, kManagerTable = 0x782950, kWithDevice = 0x620fb0;
constexpr std::uintptr_t kCompare = 0x50d4a0, kSaveLeaf = 0x77e04c;
constexpr std::array<std::uint8_t, 8> kCompareEntry{0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18, 0x53, 0x56};
constexpr std::array<std::uint8_t, 8> kCompareEqual{0xB8, 0x01, 0x00, 0x00, 0x00, 0xC2, 0x04, 0x00};  // mov eax, 1; ret 4
constexpr char kIsolated[] = "IsaacAuthority-";
constexpr int kKeyboard = 0;
using WithDevice = int(__thiscall*)(void*, int, void*, void*, void*, int*);
std::uintptr_t base = 0;
void** slot = nullptr;
WithDevice original = nullptr;
int ownController = -1;
bool compareOff = false;
SRWLOCK lifecycle = SRWLOCK_INIT;
std::atomic<bool> running{false};
std::atomic<unsigned> counters[2]{};  // reads of the own player's input answered by the keyboard; all other reads

int __fastcall OnInput(void* self, void*, int controller, void* reader, void* in, void* out, int* device) {
    if (running.load(std::memory_order_acquire) && controller == ownController) {
        counters[0].fetch_add(1, std::memory_order_relaxed);
        return original(self, kKeyboard, reader, in, out, device);
    }
    counters[1].fetch_add(1, std::memory_order_relaxed);
    return original(self, controller, reader, in, out, device);
}

DWORD Patch(std::uintptr_t rva, const std::array<std::uint8_t, 8>& bytes, const std::array<std::uint8_t, 8>& expected) {
    auto* at = reinterpret_cast<std::uint8_t*>(base + rva); DWORD old = 0;
    if (!VirtualProtect(at, bytes.size(), PAGE_EXECUTE_READWRITE, &old)) return GetLastError();
    const bool matches = std::memcmp(at, expected.data(), expected.size()) == 0;
    if (matches) std::memcpy(at, bytes.data(), bytes.size());
    DWORD ignored = 0; const BOOL restored = VirtualProtect(at, bytes.size(), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), at, bytes.size());
    return !matches ? ERROR_REVISION_MISMATCH : restored ? ERROR_SUCCESS : GetLastError();
}

std::filesystem::path Folder() {
    wchar_t local[32768]{}; const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
    if (!n || n >= 32768) throw std::runtime_error("LOCALAPPDATA unavailable");
    return std::filesystem::path(local) / L"IsaacAuthority";
}
}

extern "C" DWORD WINAPI IsaacAuthorityNativeStart(void*) noexcept {
    AcquireSRWLockExclusive(&lifecycle);
    DWORD result = ERROR_INVALID_DATA;
    try {
        if (running.load()) { ReleaseSRWLockExclusive(&lifecycle); return ERROR_ALREADY_INITIALIZED; }
        wchar_t image[32768]{};
        if (!GetModuleFileNameW(nullptr, image, 32768) || !isaac_probe::AnalyzeBytes(isaac_probe::ReadFile(image)).supported) throw ERROR_BAD_EXE_FORMAT;
        base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (std::memcmp(reinterpret_cast<const char*>(base + kSaveLeaf), kIsolated, sizeof(kIsolated) - 1) != 0) throw ERROR_ACCESS_DENIED;
        const auto folder = Folder();
        std::ifstream config(folder / (L"native-" + std::to_wstring(GetCurrentProcessId()) + L".cfg"));
        int controller = -1;
        if (!(config >> controller) || controller < 1 || controller > 7) throw ERROR_BAD_CONFIGURATION;
        const auto table = *reinterpret_cast<std::uintptr_t*>(base + kManager);
        slot = reinterpret_cast<void**>(base + kManagerTable + 29 * sizeof(void*));
        if (table != base + kManagerTable || *slot != reinterpret_cast<void*>(base + kWithDevice)) throw ERROR_REVISION_MISMATCH;
        original = reinterpret_cast<WithDevice>(*slot);
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&OnInput), &pinned)) throw GetLastError();
        if (const DWORD failure = Patch(kCompare, kCompareEqual, kCompareEntry)) throw failure;
        compareOff = true;
        ownController = controller; counters[0] = 0; counters[1] = 0;
        running.store(true, std::memory_order_release);
        if (const DWORD failure = ExchangeSlot(slot, reinterpret_cast<void*>(original), reinterpret_cast<void*>(&OnInput))) {
            running = false; Patch(kCompare, kCompareEntry, kCompareEqual); compareOff = false; throw failure;
        }
        std::ofstream descriptor(folder / (L"native-" + std::to_wstring(GetCurrentProcessId()) + L".json"), std::ios::trunc);
        descriptor << "{\"pid\":" << GetCurrentProcessId() << ",\"role\":\"native-online\",\"ownController\":" << ownController
                   << ",\"counters\":" << reinterpret_cast<std::uintptr_t>(&counters) << "}\n";
        result = ERROR_SUCCESS;
    } catch (DWORD failure) { result = failure ? failure : ERROR_INVALID_DATA; } catch (...) { result = ERROR_INVALID_DATA; }
    ReleaseSRWLockExclusive(&lifecycle);
    return result;
}

extern "C" DWORD WINAPI IsaacAuthorityNativeStop(void*) noexcept {
    AcquireSRWLockExclusive(&lifecycle);
    DWORD result = ERROR_NOT_READY;
    if (running.load()) {
        running.store(false, std::memory_order_release);
        result = ExchangeSlot(slot, reinterpret_cast<void*>(&OnInput), reinterpret_cast<void*>(original));
        // The comparison stays off on purpose: the games have already diverged, and turning it back on would split the lobby.
    }
    ReleaseSRWLockExclusive(&lifecycle);
    return result;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
