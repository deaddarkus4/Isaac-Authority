#ifdef ISAAC_AUTHORITY_UDP
#include "snapshot_receiver.hpp"
#endif
#include <windows.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include "profile.hpp"
#include "vtable_slot.hpp"
#ifdef ISAAC_AUTHORITY_SOURCE
#include "source_slot.hpp"
#endif

namespace {
using Update = void(__thiscall*)(void*);
constexpr std::uintptr_t kVtableRva = 0x76bdd0, kUpdateRva = 0x382af0;
constexpr unsigned kCapacity = 4096;
struct Body { float x, y, vx, vy; };
struct Event {
    DWORD thread = 0, frame = 0, seed = 0;
    std::uintptr_t player = 0;
    unsigned action = 0; // 0=observed, 1=applied, 2=refused, 3=observed next update
    Body before{}, after{};
    ULONGLONG timeMs = 0;
    unsigned sourceSequence = 0;
#ifdef ISAAC_AUTHORITY_UDP
    unsigned networkSequence = 0; // 4=network applied, 5=network refused, 6=context expired
    Body requested{};
    bool originalCalled = true;
#endif
};
std::uintptr_t base = 0, target = 0, room = 0;
DWORD target_seed = 0;
void** slot = nullptr;
Update original = nullptr;
std::atomic<unsigned> mode{0}, active{0}, count{0}, lost{0};
std::atomic<DWORD> owner_thread{0};
std::atomic<ULONGLONG> request_deadline{0};
std::array<Event, kCapacity> events;
HANDLE report = INVALID_HANDLE_VALUE;
SRWLOCK lifecycle = SRWLOCK_INIT;
#ifdef ISAAC_AUTHORITY_SOURCE
authority::SourceSlot sourceSlot;
#endif
#ifdef ISAAC_AUTHORITY_UDP
authority::bridge::Receiver receiver;
Body baseline{};
bool networkMode = false;
bool replicaMode = false;
authority::bridge::PlayerReplica playback;
unsigned lastNetworkSequence = 0;
authority::Body ToBody(const Body& b) { return {{b.x, b.y}, {b.vx, b.vy}}; }
Body FromBody(const authority::Body& b) { return {b.position.x, b.position.y, b.velocity.x, b.velocity.y}; }
bool ApplyBody(std::uintptr_t player, const Body& b) {
    __try {
        *reinterpret_cast<float*>(player + 0x33c) = b.x;
        *reinterpret_cast<float*>(player + 0x340) = b.y;
        *reinterpret_cast<float*>(player + 0x360) = b.vx;
        *reinterpret_cast<float*>(player + 0x364) = b.vy;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
#endif
struct Guard {
    Guard() { AcquireSRWLockExclusive(&lifecycle); }
    ~Guard() { ReleaseSRWLockExclusive(&lifecycle); }
};
template<class T> bool Read(std::uintptr_t address, T& value) {
    SIZE_T got = 0;
    return address >= 0x10000 && ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<void*>(address), &value, sizeof(T), &got) && got == sizeof(T);
}
bool Inspect(std::uintptr_t player, Event& e) {
    DWORD type = 0;
    return Read(player + 0x28, type) && type == 1 &&
        Read(player + 0x3ec, e.seed) && Read(player + 0x33c, e.before.x) &&
        Read(player + 0x340, e.before.y) && Read(player + 0x360, e.before.vx) &&
        Read(player + 0x364, e.before.vy) &&
        std::isfinite(e.before.x) && std::isfinite(e.before.y) &&
        std::isfinite(e.before.vx) && std::isfinite(e.before.vy);
}
// Rechecked on the update thread for every correction. Empty network-device
// vector is necessary; menu/lobby without an active sole player also fails.
bool LocalPlayer(std::uintptr_t& player, std::uintptr_t& current_room, DWORD& frame) {
    std::uintptr_t game = 0, manager = 0, begin = 0, end = 0, net_begin = 0, net_end = 0;
    return Read(base + 0x871678, game) && Read(base + 0x87169c, manager) &&
        Read(manager + 0x4b3d8, net_begin) && Read(manager + 0x4b3dc, net_end) && net_begin == net_end &&
        Read(manager + 0x4b3e4, frame) &&
        Read(game + 0x1baa8, begin) && Read(game + 0x1baac, end) &&
        end >= begin && end - begin == 4 && Read(begin, player) &&
        Read(game + 0x18300, current_room) && current_room >= 0x10000;
}
bool MoveEightPixels(std::uintptr_t player, const Body& b) {
    // One bounded, aligned float store on the game's player-update thread.
    // No entity creation, inventory changes, RNG changes, or disk save calls.
    __try {
        *reinterpret_cast<float*>(player + 0x33c) = b.x + 8.0f;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void __fastcall OnUpdate(void* object, void*) {
    active.fetch_add(1, std::memory_order_acq_rel);
#ifdef ISAAC_AUTHORITY_UDP
    bool originalCalled = true;
    const auto entryMode = mode.load(std::memory_order_acquire);
    if (entryMode >= 3 && replicaMode &&
        reinterpret_cast<std::uintptr_t>(object) == target && owner_thread.load() == GetCurrentThreadId()) {
        std::uintptr_t player = 0, current_room = 0; DWORD frame = 0; Event inspection;
        const bool same = LocalPlayer(player, current_room, frame) && player == target && current_room == room &&
            Inspect(target, inspection) && inspection.seed == target_seed;
        const auto now = GetTickCount64();
        if (playback.Hold(now, receiver.Deadline(), same)) {
            originalCalled = !ApplyBody(target, FromBody(playback.Current()));
            if (originalCalled) mode = 0;
        } else if (!same || now > receiver.Deadline()) mode = 0;
    }
    if (originalCalled)
#endif
    original(object);
    const auto current = mode.load(std::memory_order_acquire);
    if (current != 0) {
        DWORD empty = 0; const DWORD thread = GetCurrentThreadId();
        owner_thread.compare_exchange_strong(empty, thread);
        const auto address = reinterpret_cast<std::uintptr_t>(object);
        if (owner_thread.load() == thread && address == target) {
            Event e; e.thread = thread; e.player = address;
            e.timeMs = GetTickCount64();
#ifdef ISAAC_AUTHORITY_UDP
            e.originalCalled = originalCalled;
#endif
            std::uintptr_t player = 0, current_room = 0;
            const bool local = LocalPlayer(player, current_room, e.frame);
            if (Inspect(address, e)) {
                e.after = e.before;
#ifdef ISAAC_AUTHORITY_SOURCE
                if (!local || player != target || current_room != room || e.seed != target_seed) {
                    authority::Invalidate(sourceSlot); mode = 0;
                } else {
                    InterlockedIncrement(&sourceSlot.generation);
                    sourceSlot.alive = 1; sourceSlot.sequence++; sourceSlot.seed = e.seed;
                    sourceSlot.room = static_cast<std::uint32_t>(room); sourceSlot.player = static_cast<std::uint32_t>(target);
                    sourceSlot.timeMs = e.timeMs; sourceSlot.thread = thread;
                    sourceSlot.x = e.before.x; sourceSlot.y = e.before.y;
                    sourceSlot.vx = e.before.vx; sourceSlot.vy = e.before.vy;
                    e.sourceSequence = sourceSlot.sequence;
                    InterlockedIncrement(&sourceSlot.generation);
                }
#endif
#ifdef ISAAC_AUTHORITY_UDP
                if (current == 4) {
                    const auto now = GetTickCount64();
                    const bool same = local && player == target && current_room == room && e.seed == target_seed;
                    if (!same || now > receiver.Deadline()) { mode = 0; e.action = 6; }
                    else {
                        authority::bridge::State state;
                        if (receiver.Take(state)) {
                            e.networkSequence = state.sequence; e.requested = FromBody(state.body);
                            const bool safe = count.load() >= 2 && (replicaMode ?
                                authority::bridge::CanApplyReplica(state, ToBody(e.before), now, same) :
                                authority::bridge::CanApply(state, ToBody(e.before), ToBody(baseline), now, same));
                            e.action = 5;
                            unsigned expected = 4;
                            if (safe && mode.compare_exchange_strong(expected, 5)) {
                                Event readback;
                                if (ApplyBody(address, e.requested) && Inspect(address, readback)) {
                                    e.after = readback.before; e.action = 4;
                                    if (replicaMode) playback.Adopt(state, ToBody(e.before), now, same);
                                    lastNetworkSequence = state.sequence;
                                    // Stop may have set mode=0 while this update was active.
                                    expected = 5; mode.compare_exchange_strong(expected, 3);
                                } else mode = 0;
                            }
                        }
                    }
                } else
#endif
                if (current == 2) {
                    unsigned expected = 2;
                    if (mode.compare_exchange_strong(expected, 3)) {
                        const bool safe = local && player == target && current_room == room && e.seed == target_seed &&
                            GetTickCount64() <= request_deadline.load() &&
                            e.before.x >= 100 && e.before.x <= 500 && e.before.y >= 100 && e.before.y <= 400 &&
                            std::abs(e.before.vx) < 0.01f && std::abs(e.before.vy) < 0.01f;
                        e.action = safe && MoveEightPixels(address, e.before) ? 1 : 2;
                        if (e.action == 1) Read(address + 0x33c, e.after.x);
                    }
                } else if (current == 3) {
                    e.action = 3;
#ifdef ISAAC_AUTHORITY_UDP
                    e.networkSequence = lastNetworkSequence;
                    unsigned expected = 3; mode.compare_exchange_strong(expected, networkMode ? 4u : 1u);
#else
                    unsigned expected = 3; mode.compare_exchange_strong(expected, 1);
#endif
                }
                const auto index = count.fetch_add(1);
                if (index < kCapacity) events[index] = e;
                else lost.fetch_add(1);
            }
        }
    }
    active.fetch_sub(1, std::memory_order_release);
}
void BodyJson(std::ostringstream& out, const Body& b) {
    out << '[' << b.x << ',' << b.y << ',' << b.vx << ',' << b.vy << ']';
}
}

extern "C" DWORD WINAPI IsaacAuthorityStart(void*) noexcept {
    Guard guard;
    if (mode.load() || report != INVALID_HANDLE_VALUE) return ERROR_ALREADY_EXISTS;
    try {
        wchar_t path[32768]{};
        if (!GetModuleFileNameW(nullptr, path, 32768)) return ERROR_BAD_EXE_FORMAT;
        if (!isaac_probe::AnalyzeBytes(isaac_probe::ReadFile(path)).supported) return ERROR_BAD_EXE_FORMAT;
        base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        DWORD frame = 0;
        if (!LocalPlayer(target, room, frame)) return ERROR_NOT_READY;
        Event initial;
        if (!Inspect(target, initial)) return ERROR_INVALID_DATA;
        target_seed = initial.seed;
#ifdef ISAAC_AUTHORITY_SOURCE
        sourceSlot = authority::SourceSlot{};
        sourceSlot.token = GetTickCount() | 1;
#endif
#ifdef ISAAC_AUTHORITY_UDP
        baseline = initial.before; networkMode = false; replicaMode = false; playback.Reset(); lastNetworkSequence = 0;
#endif
        std::uintptr_t vtable = 0, function = 0;
        if (!Read(target, vtable) || vtable != base + kVtableRva ||
            !Read(vtable + 12, function) || function != base + kUpdateRva) return ERROR_REVISION_MISMATCH;
        std::array<unsigned char, 6> prefix{};
        const std::array<unsigned char, 6> expected_prefix{0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68};
        if (!Read(function, prefix) || prefix != expected_prefix) return ERROR_REVISION_MISMATCH;
        // Export is pinned: an in-flight vtable call must remain executable after Stop.
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&OnUpdate), &pinned)) return GetLastError();
        wchar_t local[32768]{};
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
        if (!length || length >= 32768) return ERROR_PATH_NOT_FOUND;
        const auto directory = std::filesystem::path(local) / L"IsaacAuthority" / L"logs";
        std::filesystem::create_directories(directory);
        const auto filename = directory / (L"player-step-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()) + L".jsonl");
        report = CreateFileW(filename.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (report == INVALID_HANDLE_VALUE) return GetLastError();
        slot = reinterpret_cast<void**>(vtable + 12);
        original = reinterpret_cast<Update>(function);
        count = 0; lost = 0; owner_thread = 0;
        mode.store(1, std::memory_order_release);
        const DWORD status = authority::ExchangeSlot(slot, reinterpret_cast<void*>(original), reinterpret_cast<void*>(&OnUpdate));
        if (status != ERROR_SUCCESS) {
            mode = 0; CloseHandle(report); report = INVALID_HANDLE_VALUE;
        }
        return status;
    } catch (...) { return ERROR_INVALID_DATA; }
}
extern "C" DWORD WINAPI IsaacAuthorityCorrectOnce(void*) noexcept {
#ifdef ISAAC_AUTHORITY_SOURCE
    return ERROR_NOT_READY; // The source module never writes player state.
#else
    Guard guard;
    if (mode.load() != 1 || count.load() < 2 || !owner_thread.load()) return ERROR_NOT_READY;
    request_deadline = GetTickCount64() + 5000;
    unsigned expected = 1;
    return mode.compare_exchange_strong(expected, 2) ? ERROR_SUCCESS : ERROR_BUSY;
#endif
}
extern "C" DWORD WINAPI IsaacAuthorityStop(void*) noexcept {
    Guard guard;
    if (report == INVALID_HANDLE_VALUE) return ERROR_NOT_READY;
    mode = 0;
#ifdef ISAAC_AUTHORITY_UDP
    receiver.Stop();
#endif
    const DWORD status = authority::ExchangeSlot(slot, reinterpret_cast<void*>(&OnUpdate), reinterpret_cast<void*>(original));
    if (status != ERROR_SUCCESS) return status;
    const auto deadline = GetTickCount64() + 2000;
    while (active.load(std::memory_order_acquire) && GetTickCount64() < deadline) Sleep(1);
    if (active.load()) return ERROR_BUSY;
#ifdef ISAAC_AUTHORITY_SOURCE
    authority::Invalidate(sourceSlot);
#endif
    try {
        std::ostringstream text;
        text.precision(9);
        text << "{\"type\":\"start\",\"schemaVersion\":1,\"pid\":" << GetCurrentProcessId()
            << ",\"boundary\":\"after_player_update\",\"worldSnapshot\":false";
#ifdef ISAAC_AUTHORITY_UDP
        text << ",\"network\":" << (networkMode ? "true" : "false") << ",\"playerReplica\":" << (replicaMode ? "true" : "false");
#endif
        text << "}\n";
        const auto size = (std::min)(count.load(), kCapacity);
        for (unsigned i = 0; i < size; ++i) {
            const auto& e = events[i];
            text << "{\"type\":\"player_step\",\"sequence\":" << i + 1 << ",\"thread\":" << e.thread
                << ",\"timeMs\":" << e.timeMs << ",\"sourceSequence\":" << e.sourceSequence
                << ",\"networkFrame\":" << e.frame << ",\"action\":" << e.action << ",\"before\":";
            BodyJson(text, e.before); text << ",\"after\":"; BodyJson(text, e.after);
#ifdef ISAAC_AUTHORITY_UDP
            text << ",\"networkSequence\":" << e.networkSequence << ",\"requested\":"; BodyJson(text, e.requested);
            text << ",\"originalCalled\":" << (e.originalCalled ? "true" : "false");
#endif
            text << "}\n";
        }
        text << "{\"type\":\"stop\",\"events\":" << size << ",\"dropped\":" << lost.load()
            << ",\"vtableRestored\":true";
#ifdef ISAAC_AUTHORITY_UDP
        if (networkMode) {
            const auto stats = receiver.Stats();
            text << ",\"networkAccepted\":" << stats.accepted << ",\"networkRejected\":" << stats.rejected
                << ",\"networkReplaced\":" << stats.replaced << ",\"networkErrors\":" << stats.errors;
        }
#endif
        text << "}\n";
        const auto output = text.str(); DWORD written = 0;
        const BOOL ok = WriteFile(report, output.data(), static_cast<DWORD>(output.size()), &written, nullptr);
        FlushFileBuffers(report); CloseHandle(report); report = INVALID_HANDLE_VALUE;
        return ok && written == output.size() ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
    } catch (...) {
        CloseHandle(report); report = INVALID_HANDLE_VALUE; return ERROR_WRITE_FAULT;
    }
}
#ifdef ISAAC_AUTHORITY_SOURCE
extern "C" DWORD WINAPI IsaacAuthoritySourceStart(void*) noexcept {
    const auto started = IsaacAuthorityStart(nullptr);
    if (started != ERROR_SUCCESS) return started;
    DWORD result = ERROR_SUCCESS;
    {
        Guard guard;
        try {
            wchar_t local[32768]{};
            const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
            if (!length || length >= 32768) throw std::runtime_error("LOCALAPPDATA unavailable");
            const auto directory = std::filesystem::path(local) / L"IsaacAuthority";
            const auto ready = directory / (L"source-" + std::to_wstring(GetCurrentProcessId()) + L".json");
            const auto temp = directory / (L"source-" + std::to_wstring(GetCurrentProcessId()) +
                L"-" + std::to_wstring(sourceSlot.token) + L".tmp");
            std::ostringstream text;
            text << "{\"pid\":" << GetCurrentProcessId() << ",\"address\":" << reinterpret_cast<std::uintptr_t>(&sourceSlot)
                << ",\"bytes\":64,\"token\":" << sourceSlot.token << "}\n";
            const auto output = text.str();
            HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create source descriptor");
            DWORD written = 0; const BOOL ok = WriteFile(file, output.data(), static_cast<DWORD>(output.size()), &written, nullptr);
            CloseHandle(file);
            if (!ok || written != output.size() || !MoveFileExW(temp.c_str(), ready.c_str(), MOVEFILE_REPLACE_EXISTING))
                throw std::runtime_error("Cannot publish source descriptor");
        } catch (...) { result = ERROR_INVALID_DATA; }
    }
    if (result != ERROR_SUCCESS) IsaacAuthorityStop(nullptr);
    return result;
}
#endif
#ifdef ISAAC_AUTHORITY_UDP
DWORD StartNetwork(bool replica) noexcept {
    const auto started = IsaacAuthorityStart(nullptr);
    if (started != ERROR_SUCCESS) return started;
    DWORD result = ERROR_SUCCESS;
    {
        Guard guard;
        if (mode.load() != 1 || report == INVALID_HANDLE_VALUE) return ERROR_NOT_READY;
        try {
            result = receiver.Start(target_seed, replica);
            if (result == ERROR_SUCCESS) {
                wchar_t local[32768]{};
                const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
                if (!length || length >= 32768) throw std::runtime_error("LOCALAPPDATA unavailable");
                const auto directory = std::filesystem::path(local) / L"IsaacAuthority";
                const auto ready = directory / (L"network-" + std::to_wstring(GetCurrentProcessId()) + L".json");
                const auto temp = directory / (L"network-" + std::to_wstring(GetCurrentProcessId()) +
                    L"-" + std::to_wstring(receiver.Session()) + L".tmp");
                std::ostringstream text; text.precision(9);
                text << "{\"schemaVersion\":1,\"pid\":" << GetCurrentProcessId() << ",\"host\":\"127.0.0.1\",\"port\":"
                    << receiver.Port() << ",\"session\":\"" << receiver.Session() << "\",\"epoch\":1,\"seed\":" << target_seed
                    << ",\"deadlineMs\":" << receiver.Deadline() << ",\"replica\":" << (replica ? "true" : "false") << ",\"baseline\":";
                BodyJson(text, baseline); text << "}\n";
                const auto output = text.str();
                HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create network endpoint report");
                DWORD written = 0;
                const BOOL ok = WriteFile(file, output.data(), static_cast<DWORD>(output.size()), &written, nullptr);
                CloseHandle(file);
                if (!ok || written != output.size() || !MoveFileExW(temp.c_str(), ready.c_str(), MOVEFILE_REPLACE_EXISTING))
                    throw std::runtime_error("Cannot publish network endpoint report");
                networkMode = true;
                replicaMode = replica;
                mode.store(4, std::memory_order_release);
            }
        } catch (...) { result = ERROR_INVALID_DATA; }
    }
    if (result != ERROR_SUCCESS) IsaacAuthorityStop(nullptr);
    return result;
}
extern "C" DWORD WINAPI IsaacAuthorityNetworkStart(void*) noexcept { return StartNetwork(false); }
extern "C" DWORD WINAPI IsaacAuthorityReplicaStart(void*) noexcept { return StartNetwork(true); }
#endif
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
