// Changing the game's own online from inside, by the user's model: a character belongs to its owner, the world to the host.
//
// Step 1: the local player answers to the keyboard at once. In the game's lockstep a player's input is scheduled some
// frames ahead and every game, the player's own included, applies it only then: 134-149 ms from key to movement between
// two windows of one machine, against 15 ms in a solo game. Here the local player's update reads the physical device
// directly, while the game's own net device keeps broadcasting the scheduled input to the others as before.
//
// The two games then no longer compute the same frames, which the lockstep takes for a desync and answers by splitting the
// lobby. So the comparison of frame checksums (RVA 0x50d4a0, thiscall, one argument) is made to answer "equal". That is a
// change of code, and like the localhost module this one refuses to work anywhere but in an isolated test instance.
//
// Step 2, the other half of the same model: a remote player stands where its owner says. After its own update every game
// publishes the body of its local player (position and velocity) in a slot of its memory; whatever carries it to the other
// games - for now a relay outside, later the game's own connection - delivers it as a PLR1 datagram to this module's
// loopback socket, and after the update of a remote player the newest body of its owner replaces what the delayed input
// produced. Everything else about that player still comes from the game's own simulation.
//
// Which controller is the local player's is told from outside (the lobby's device numbers: 2, 3, ...), in
// %LOCALAPPDATA%\IsaacAuthority\native-<pid>.cfg as a single number: the harness reads it from the game's log.
#include <winsock2.h>
#include <windows.h>
#include "profile.hpp"
#include "vtable_slot.hpp"
#include <array>
#include <atomic>
#include <cmath>
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
// Entity_Player: update is slot 3 of the table at RVA 0x76bdd0; position +0x33c, velocity +0x360, controller +0x1618.
constexpr std::uintptr_t kPlayerTable = 0x76bdd0, kPlayerUpdate = 0x382af0, kPosition = 0x33c, kVelocity = 0x360, kController = 0x1618;
constexpr std::uint32_t kBodyMagic = 0x31524c50;  // "PLR1"
constexpr int kControllers = 8;
constexpr std::uint64_t kFreshMs = 250;
#pragma pack(push, 1)
struct Body { std::uint32_t magic, controller, sequence; float position[2], velocity[2]; };
// What a reader outside copies: generation is odd while the body is being written.
struct Published { std::uint32_t magic, generation; Body body; };
struct Stats { std::uint32_t published, received, applied, stale, rejected; float correctionSum, correctionMax; };
#pragma pack(pop)
static_assert(sizeof(Body) == 28 && sizeof(Published) == 36, "PLR1 layout");
using WithDevice = int(__thiscall*)(void*, int, void*, void*, void*, int*);
using PlayerUpdate = void(__thiscall*)(void*);
std::uintptr_t base = 0;
void** slot = nullptr; void** playerSlot = nullptr;
WithDevice original = nullptr; PlayerUpdate originalPlayer = nullptr;
int ownController = -1;
SRWLOCK lifecycle = SRWLOCK_INIT, inboxLock = SRWLOCK_INIT;
std::atomic<bool> running{false};
std::atomic<unsigned> counters[2]{};  // reads of the own player's input answered by the keyboard; all other reads
Published published{};
Stats stats{};
struct Inbox { Body body{}; std::uint64_t at = 0; std::uint32_t appliedSequence = 0; };
Inbox inbox[kControllers];
SOCKET udp = INVALID_SOCKET; HANDLE worker = nullptr; bool winsock = false; unsigned short port = 0;
std::uint32_t sequence = 0;

// The physical device answers whatever window has the focus: a game in the background (the test instances do not pause
// there) would walk its player with keys meant for another window. Asked at most once a millisecond.
bool Focused() {
    static std::uint64_t asked = 0; static bool focused = false;
    const auto now = GetTickCount64();
    if (now != asked) {
        DWORD owner = 0; const HWND front = GetForegroundWindow();
        focused = front && GetWindowThreadProcessId(front, &owner) && owner == GetCurrentProcessId(); asked = now;
    }
    return focused;
}

int __fastcall OnInput(void* self, void*, int controller, void* reader, void* in, void* out, int* device) {
    if (running.load(std::memory_order_acquire) && controller == ownController && Focused()) {
        counters[0].fetch_add(1, std::memory_order_relaxed);
        return original(self, kKeyboard, reader, in, out, device);
    }
    counters[1].fetch_add(1, std::memory_order_relaxed);
    return original(self, controller, reader, in, out, device);
}

bool Finite(const float* v) { return std::isfinite(v[0]) && std::isfinite(v[1]); }

// The game's thread, right after the game's own update of one player.
void AfterUpdate(std::uintptr_t player) noexcept {
    __try {
        const int controller = *reinterpret_cast<int*>(player + kController);
        auto* position = reinterpret_cast<float*>(player + kPosition); auto* velocity = reinterpret_cast<float*>(player + kVelocity);
        if (controller == ownController) {
            published.generation++;   // odd: being written
            published.body.magic = kBodyMagic; published.body.controller = static_cast<std::uint32_t>(controller); published.body.sequence = ++sequence;
            published.body.position[0] = position[0]; published.body.position[1] = position[1];
            published.body.velocity[0] = velocity[0]; published.body.velocity[1] = velocity[1];
            published.generation++;
            stats.published++;
            return;
        }
        if (controller < 0 || controller >= kControllers) return;
        Body body{}; bool fresh = false;
        AcquireSRWLockShared(&inboxLock);
        if (inbox[controller].body.sequence && inbox[controller].body.sequence != inbox[controller].appliedSequence) {
            fresh = GetTickCount64() - inbox[controller].at <= kFreshMs; body = inbox[controller].body;
        }
        ReleaseSRWLockShared(&inboxLock);
        if (!body.sequence) return;
        if (!fresh) { stats.stale++; return; }
        const float dx = body.position[0] - position[0], dy = body.position[1] - position[1], distance = std::sqrt(dx * dx + dy * dy);
        position[0] = body.position[0]; position[1] = body.position[1]; velocity[0] = body.velocity[0]; velocity[1] = body.velocity[1];
        AcquireSRWLockExclusive(&inboxLock); inbox[controller].appliedSequence = body.sequence; ReleaseSRWLockExclusive(&inboxLock);
        stats.applied++; stats.correctionSum += distance; if (distance > stats.correctionMax) stats.correctionMax = distance;
    } __except (EXCEPTION_EXECUTE_HANDLER) { stats.rejected++; }
}

void __fastcall OnPlayer(void* object, void*) {
    originalPlayer(object);
    if (running.load(std::memory_order_acquire)) AfterUpdate(reinterpret_cast<std::uintptr_t>(object));
}

DWORD WINAPI Receive(void*) noexcept {
    while (running.load(std::memory_order_acquire)) {
        fd_set set; FD_ZERO(&set); FD_SET(udp, &set); timeval wait{0, 50000};
        if (select(0, &set, nullptr, nullptr, &wait) <= 0) continue;
        Body body{}; const int got = recv(udp, reinterpret_cast<char*>(&body), sizeof(body), 0);
        const bool valid = got == sizeof(body) && body.magic == kBodyMagic && body.controller < kControllers && body.sequence &&
            static_cast<int>(body.controller) != ownController && Finite(body.position) && Finite(body.velocity);
        if (!valid) { stats.rejected++; continue; }
        AcquireSRWLockExclusive(&inboxLock);
        if (body.sequence > inbox[body.controller].body.sequence) { inbox[body.controller].body = body; inbox[body.controller].at = GetTickCount64(); stats.received++; }
        ReleaseSRWLockExclusive(&inboxLock);
    }
    return 0;
}

void CloseNetwork() {
    if (worker) { WaitForSingleObject(worker, 2000); CloseHandle(worker); worker = nullptr; }
    if (udp != INVALID_SOCKET) { closesocket(udp); udp = INVALID_SOCKET; }
    if (winsock) { WSACleanup(); winsock = false; }
}

DWORD Patch(std::uintptr_t rva, const std::array<std::uint8_t, 8>& bytes, const std::array<std::uint8_t, 8>& expected) {
    auto* at = reinterpret_cast<std::uint8_t*>(base + rva); DWORD old = 0;
    if (!VirtualProtect(at, bytes.size(), PAGE_EXECUTE_READWRITE, &old)) return GetLastError();
    // Already in place counts as done: an earlier build of this module leaves the comparison off when it stops.
    const bool matches = std::memcmp(at, expected.data(), expected.size()) == 0 || std::memcmp(at, bytes.data(), bytes.size()) == 0;
    if (matches) std::memcpy(at, bytes.data(), bytes.size());
    DWORD ignored = 0; const BOOL restored = VirtualProtect(at, bytes.size(), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), at, bytes.size());
    return !matches ? ERROR_REVISION_MISMATCH : restored ? ERROR_SUCCESS : GetLastError();
}

std::filesystem::path Folder() {
    wchar_t local[32768]{}; const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
    if (!n || n >= 32768) throw static_cast<DWORD>(ERROR_ENVVAR_NOT_FOUND);
    return std::filesystem::path(local) / L"IsaacAuthority";
}
}

extern "C" DWORD WINAPI IsaacAuthorityNativeStart(void*) noexcept {
    AcquireSRWLockExclusive(&lifecycle);
    DWORD result = ERROR_INVALID_DATA;
    try {
        if (running.load()) throw static_cast<DWORD>(ERROR_ALREADY_INITIALIZED);
        wchar_t image[32768]{};
        if (!GetModuleFileNameW(nullptr, image, 32768) || !isaac_probe::AnalyzeBytes(isaac_probe::ReadFile(image)).supported) throw static_cast<DWORD>(ERROR_BAD_EXE_FORMAT);
        base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (std::memcmp(reinterpret_cast<const char*>(base + kSaveLeaf), kIsolated, sizeof(kIsolated) - 1) != 0) throw static_cast<DWORD>(ERROR_ACCESS_DENIED);
        const auto folder = Folder();
        std::ifstream config(folder / (L"native-" + std::to_wstring(GetCurrentProcessId()) + L".cfg"));
        int controller = -1;
        if (!(config >> controller) || controller < 1 || controller >= kControllers) throw static_cast<DWORD>(ERROR_BAD_CONFIGURATION);
        const auto table = *reinterpret_cast<std::uintptr_t*>(base + kManager);
        slot = reinterpret_cast<void**>(base + kManagerTable + 29 * sizeof(void*));
        playerSlot = reinterpret_cast<void**>(base + kPlayerTable + 3 * sizeof(void*));
        if (table != base + kManagerTable || *slot != reinterpret_cast<void*>(base + kWithDevice) ||
            *playerSlot != reinterpret_cast<void*>(base + kPlayerUpdate)) throw static_cast<DWORD>(ERROR_REVISION_MISMATCH);
        original = reinterpret_cast<WithDevice>(*slot); originalPlayer = reinterpret_cast<PlayerUpdate>(*playerSlot);
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&OnInput), &pinned)) throw GetLastError();
        ownController = controller; counters[0] = 0; counters[1] = 0; published = Published{}; published.magic = kBodyMagic; stats = Stats{}; sequence = 0;
        for (auto& box : inbox) box = Inbox{};
        WSADATA data{};
        if (const int started = WSAStartup(MAKEWORD(2, 2), &data)) throw static_cast<DWORD>(started);
        winsock = true; udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); int size = sizeof(address);
        if (udp == INVALID_SOCKET || bind(udp, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || getsockname(udp, reinterpret_cast<sockaddr*>(&address), &size)) {
            const auto failure = static_cast<DWORD>(WSAGetLastError()); CloseNetwork(); throw failure;
        }
        port = ntohs(address.sin_port);
        if (const DWORD failure = Patch(kCompare, kCompareEqual, kCompareEntry)) { CloseNetwork(); throw failure; }
        running.store(true, std::memory_order_release);
        worker = CreateThread(nullptr, 0, &Receive, nullptr, 0, nullptr);
        DWORD failure = worker ? ExchangeSlot(slot, reinterpret_cast<void*>(original), reinterpret_cast<void*>(&OnInput)) : GetLastError();
        if (!failure) {
            failure = ExchangeSlot(playerSlot, reinterpret_cast<void*>(originalPlayer), reinterpret_cast<void*>(&OnPlayer));
            if (failure) ExchangeSlot(slot, reinterpret_cast<void*>(&OnInput), reinterpret_cast<void*>(original));
        }
        if (failure) { running = false; CloseNetwork(); Patch(kCompare, kCompareEntry, kCompareEqual); throw failure; }
        std::ofstream descriptor(folder / (L"native-" + std::to_wstring(GetCurrentProcessId()) + L".json"), std::ios::trunc);
        descriptor << "{\"pid\":" << GetCurrentProcessId() << ",\"role\":\"native-online\",\"ownController\":" << ownController
                   << ",\"counters\":" << reinterpret_cast<std::uintptr_t>(&counters) << ",\"port\":" << port
                   << ",\"published\":" << reinterpret_cast<std::uintptr_t>(&published) << ",\"publishedBytes\":" << sizeof(published)
                   << ",\"stats\":" << reinterpret_cast<std::uintptr_t>(&stats) << ",\"statsBytes\":" << sizeof(stats) << "}\n";
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
        const DWORD second = ExchangeSlot(playerSlot, reinterpret_cast<void*>(&OnPlayer), reinterpret_cast<void*>(originalPlayer));
        if (!result) result = second;
        CloseNetwork();
        // The comparison stays off on purpose: the games have already diverged, and turning it back on would split the lobby.
    }
    ReleaseSRWLockExclusive(&lifecycle);
    return result;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
