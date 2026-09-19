// Host side of client input: commands received over loopback UDP replace what the game reads from one controller,
// so the host's own movement and firing code simulates that player. No game code is patched: one slot of the
// input manager's vtable is exchanged, like the entity update slots of the other adapters.
// Client side (ISAAC_INPUT_CLIENT), the same slot read the other way: the module asks the client's own device what it
// answers for the actions of play, publishes that as a command for the host, and withholds it from the local game.
#include <winsock2.h>
#include <windows.h>
#include "input_state.hpp"
#include "profile.hpp"
#include "vtable_slot.hpp"
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <sstream>

namespace {
using namespace authority;
using namespace authority::input;
#if defined(ISAAC_INPUT_CLIENT) || defined(ISAAC_INPUT_PREDICT)
constexpr bool kClient = true;
#else
constexpr bool kClient = false;
#endif
// A predicting client also walks its own player at once, with the very command it sends to the host. That player is the
// one its game created for the joining controller: index 1 in both games of this two-player prototype.
#ifdef ISAAC_INPUT_PREDICT
constexpr bool kPredict = true;
#else
constexpr bool kPredict = false;
#endif
constexpr int kOwnController = 1;
// InputManager at RVA 0x857b18; slot 29 is WithDevice(controller, reader, in, out, deviceId), thiscall, ret 0x14.
// Readers: action value (float to out, returns value != 0), action pressed, action triggered (both return bool).
constexpr std::uintptr_t kManager = 0x857b18, kManagerTable = 0x782950, kWithDevice = 0x620fb0;
constexpr std::uintptr_t kReadValue = 0x620ab0, kReadPressed = 0x620940, kReadTriggered = 0x6209e0;
constexpr std::uint64_t kDurationMs = 30000;
using WithDevice = int(__thiscall*)(void*, int, void*, void*, void*, int*);
std::uintptr_t base = 0;
void** slot = nullptr;
WithDevice original = nullptr;
bool installed = false;
std::atomic<bool> running{false};
std::atomic<unsigned> active{0}, values{0}, presses{0}, triggers{0}, passedThrough{0}, accepted{0}, rejected{0}, errors{0};
// What the game asks: [controller + 1 (-1..7)][reader][action]. Shows e.g. which controllers are polled for a join button.
constexpr int kControllers = 9, kPolledActions = 40;
std::atomic<unsigned> polls[kControllers][3][kPolledActions]{};
SRWLOCK lifecycle = SRWLOCK_INIT, state = SRWLOCK_INIT;
Command latest;          // guarded by state
// A real device reports "just pressed" for one whole frame, and the game may ask several times in that frame
// (the co-op join asks twice). A press seen by the receiver stays pending until the game first asks, then
// answers true for one frame's worth of time. Guarded by state.
bool pendingEdge[kActions]{};
std::int64_t edgeOpened[kActions]{};
std::int64_t ticksPerFrame = 0; // about 12 ms: shorter than a 60 Hz frame, far longer than one polling pass
bool have = false;
std::uint64_t session = 0, deadline = 0;
SOCKET udp = INVALID_SOCKET;
HANDLE worker = nullptr, report = INVALID_HANDLE_VALUE;
bool winsock = false;
unsigned short port = 0;
struct Guard { Guard() { AcquireSRWLockExclusive(&lifecycle); } ~Guard() { ReleaseSRWLockExclusive(&lifecycle); } };
// The client's latest command, read from outside like the world slot: an odd generation means "being written".
struct alignas(8) PublishedCommand {
    std::uint32_t magic = 0x31534e49, version = 1;
    volatile LONG generation = 0;
    std::uint32_t alive = 0, bytes = 0, reserved = 0;
    std::uint64_t session = 0;
    std::array<std::uint8_t, kBytes> packet{};
};
static_assert(sizeof(PublishedCommand) == 88 && offsetof(PublishedCommand, packet) == 32);
PublishedCommand published;
// The keyboard is controller 0. Sampling a little faster than the game's 60 Hz input never skips a frame of it.
constexpr int kLocalController = 0;
constexpr std::uint64_t kSampleMs = 8;
std::atomic<DWORD> owner{0};
std::uint64_t lastSampleMs = 0;
std::uint32_t captureSequence = 0;
std::atomic<unsigned> samples{0}, activeSamples{0}, withheld{0};
// The command whose movement the game last read for the driven controller. The world module of the same process asks:
// on the host to acknowledge it in the snapshot, on a predicting client to label its own history.
std::atomic<std::int32_t> consumedController{0};
std::atomic<std::uint32_t> consumedSequence{0};

// Answer for the driven controller from the latest command. A silent client is a neutral client: without a fresh
// command nothing is injected and the caller decides.
bool Inject(int controller, std::uintptr_t kind, const void* in, void* out, int& result) {
    bool injected = false;
    AcquireSRWLockExclusive(&state);
    if (have && controller == latest.controller && Fresh(latest, GetTickCount64())) {
        const int action = *static_cast<const int*>(in);
        if (Known(action)) {
            injected = true;
            if (kind == base + kReadValue) {
                const auto value = Value(latest, action);
                if (out) *static_cast<float*>(out) = value;
                result = value != 0; ++values;
                if (action <= Down) { consumedController.store(latest.controller); consumedSequence.store(latest.sequence); }
            } else if (kind == base + kReadPressed) { result = Pressed(latest, action); ++presses; }
            else {
                LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
                if (pendingEdge[action]) { pendingEdge[action] = false; edgeOpened[action] = now.QuadPart; }
                result = edgeOpened[action] && now.QuadPart - edgeOpened[action] <= ticksPerFrame;
                ++triggers;
            }
        }
    }
    ReleaseSRWLockExclusive(&state);
    return injected;
}

// Ask the client's own device, on the game's thread and through the game's own reader, what it answers right now.
void Capture(void* self) {
    const auto now = GetTickCount64();
    if (now - lastSampleMs < kSampleMs) return;
    lastSampleMs = now;
    Sample sample;
    for (const auto action : kCaptured) {
        int asked = action; float value = 0;
        if (original(self, kLocalController, reinterpret_cast<void*>(base + kReadValue), &asked, &value, nullptr)) sample.value[action] = value;
    }
    auto command = Compose(sample);
    command.session = session; command.sequence = ++captureSequence; command.timeMs = now; command.controller = kLocalController;
    std::uint8_t bytes[kBytes];
    if (!Encode(command, bytes)) { ++errors; return; }
    InterlockedIncrement(&published.generation);
    std::copy(bytes, bytes + kBytes, published.packet.begin()); published.bytes = kBytes; published.alive = 1;
    InterlockedIncrement(&published.generation);
    ++samples;
    if (!Neutral(command)) ++activeSamples;
    if (kPredict) {
        // The local game reads the same command the host will: movement only. What the player fires, drops or uses
        // is the host's to decide, and its results come back in the world.
        AcquireSRWLockExclusive(&state);
        latest = command; latest.controller = kOwnController; latest.shootX = latest.shootY = 0; latest.buttons = 0; have = true;
        ReleaseSRWLockExclusive(&state);
    }
}
void InvalidatePublication() { InterlockedIncrement(&published.generation); published.alive = 0; InterlockedIncrement(&published.generation); }

int __fastcall OnInput(void* self, void*, int controller, void* reader, void* in, void* out, int* device) {
    active.fetch_add(1);
    int result = 0; bool injected = false;
    const auto kind = reinterpret_cast<std::uintptr_t>(reader);
    const bool known = in && (kind == base + kReadValue || kind == base + kReadPressed || kind == base + kReadTriggered);
    if (kClient) {
        if (running.load(std::memory_order_acquire) && GetTickCount64() <= deadline) {
            DWORD nobody = 0; owner.compare_exchange_strong(nobody, GetCurrentThreadId());
            if (owner.load() == GetCurrentThreadId()) {
                Capture(self);
                // This keyboard plays on the host. Its answers for the actions of play are withheld here, so it cannot
                // steer a local player as well; menus, pause and restart stay local.
                if (known && controller == kLocalController && Captured(*static_cast<const int*>(in))) {
                    if (kind == base + kReadValue && out) *static_cast<float*>(out) = 0;
                    injected = true; ++withheld;
                }
                if (kPredict) {
                    // The own player walks by the captured command. Whatever device really sits at that index, a
                    // gamepad for one, never steers it: a known action without a command is neutral, not the device's.
                    if (known && controller == kOwnController && Known(*static_cast<const int*>(in))) {
                        if (!Inject(controller, kind, in, out, result)) { result = 0; if (kind == base + kReadValue && out) *static_cast<float*>(out) = 0; }
                        injected = true;
                    }
                }
            }
        }
    } else if (running.load(std::memory_order_acquire) && known) {
        const int asked = *static_cast<const int*>(in), reads = kind == base + kReadValue ? 0 : kind == base + kReadPressed ? 1 : 2;
        if (controller >= -1 && controller < kControllers - 1 && asked >= 0 && asked < kPolledActions) ++polls[controller + 1][reads][asked];
        // A silent client is a neutral client: without a fresh command the real device decides again.
        injected = Inject(controller, kind, in, out, result);
    }
    if (!injected) { ++passedThrough; result = original(self, controller, reader, in, out, device); }
    active.fetch_sub(1, std::memory_order_release);
    return result;
}
DWORD WINAPI Receive(void*) {
    Gate gate(session);
    while (running.load() && GetTickCount64() <= deadline) {
        for (unsigned drain = 0; drain < 64 && running.load(); ++drain) {
            std::uint8_t bytes[kBytes + 1]{}; sockaddr_in from{}; int size = sizeof(from);
            const auto n = recvfrom(udp, reinterpret_cast<char*>(bytes), sizeof(bytes), 0, reinterpret_cast<sockaddr*>(&from), &size);
            if (n == SOCKET_ERROR) {
                const auto error = WSAGetLastError();
                if (error == WSAEWOULDBLOCK) break;
                if (error == WSAEMSGSIZE || error == WSAECONNRESET) { ++rejected; continue; }
                ++errors; running = false; break;
            }
            Command command;
            if (from.sin_family != AF_INET || from.sin_addr.s_addr != htonl(INADDR_LOOPBACK) ||
                !Decode(bytes, static_cast<std::size_t>(n), command) || !gate.Receive(command, GetTickCount64())) { ++rejected; continue; }
            AcquireSRWLockExclusive(&state);
            for (int action = 0; action < kActions; ++action)
                if (Known(action) && Pressed(command, action) && !(have && Pressed(latest, action))) pendingEdge[action] = true;
            latest = command; have = true;
            ReleaseSRWLockExclusive(&state);
            ++accepted;
        }
        Sleep(1);
    }
    return 0;
}
void CloseNetwork() {
    if (worker) { WaitForSingleObject(worker, INFINITE); CloseHandle(worker); worker = nullptr; }
    if (udp != INVALID_SOCKET) { closesocket(udp); udp = INVALID_SOCKET; }
    if (winsock) { WSACleanup(); winsock = false; }
}
bool Restore() {
    bool ok = true;
    if (installed) {
        ok = ExchangeSlot(slot, reinterpret_cast<void*>(&OnInput), reinterpret_cast<void*>(original)) == 0;
        if (ok) installed = false;
    }
    const auto until = GetTickCount64() + 2000;
    while (active.load(std::memory_order_acquire) && GetTickCount64() < until) Sleep(1);
    return ok && !active.load();
}
bool WriteText(const std::filesystem::path& path, const std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0; const BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
    CloseHandle(file); return ok && n == text.size();
}
}

extern "C" DWORD WINAPI IsaacAuthorityInputStart(void*) noexcept {
    Guard guard;
    if (report != INVALID_HANDLE_VALUE) return ERROR_ALREADY_EXISTS;
    DWORD failure = ERROR_INVALID_DATA;
    try {
        wchar_t image[32768]{};
        if (!GetModuleFileNameW(nullptr, image, 32768) || !isaac_probe::AnalyzeBytes(isaac_probe::ReadFile(image)).supported) return ERROR_BAD_EXE_FORMAT;
        base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto table = *reinterpret_cast<std::uintptr_t*>(base + kManager);
        slot = reinterpret_cast<void**>(base + kManagerTable + 29 * sizeof(void*));
        if (table != base + kManagerTable || *slot != reinterpret_cast<void*>(base + kWithDevice)) return ERROR_REVISION_MISMATCH;
        original = reinterpret_cast<WithDevice>(*slot);
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&OnInput), &pinned)) return GetLastError();
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&session), sizeof(session), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return ERROR_GEN_FAILURE;
        if (!session) session = 1;
        have = false; latest = {};
        for (auto& edge : pendingEdge) edge = false;
        for (auto& opened : edgeOpened) opened = 0;
        LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency); ticksPerFrame = frequency.QuadPart * 12 / 1000;
        values = presses = triggers = passedThrough = accepted = rejected = errors = 0;
        for (auto& controller : polls) for (auto& reader : controller) for (auto& count : reader) count = 0;
        samples = activeSamples = withheld = 0; owner = 0; lastSampleMs = 0; captureSequence = 0; port = 0;
        consumedController = 0; consumedSequence = 0;
        published = PublishedCommand{}; published.session = session;
        // The client opens no socket: its command is published in a slot and carried to the host from outside.
        if (!kClient) {
            WSADATA data{}; const auto started = WSAStartup(MAKEWORD(2, 2), &data);
            if (started) return static_cast<DWORD>(started);
            winsock = true; udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            u_long nonblocking = 1; int size = sizeof(address);
            if (udp == INVALID_SOCKET || bind(udp, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ||
                ioctlsocket(udp, FIONBIO, &nonblocking) || getsockname(udp, reinterpret_cast<sockaddr*>(&address), &size)) {
                failure = static_cast<DWORD>(WSAGetLastError()); throw std::runtime_error("Cannot open input socket");
            }
            port = ntohs(address.sin_port);
        }
        deadline = GetTickCount64() + kDurationMs;
        wchar_t local[32768]{}; const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
        if (!n || n >= 32768) throw std::runtime_error("LOCALAPPDATA unavailable");
        const auto directory = std::filesystem::path(local) / L"IsaacAuthority"; std::filesystem::create_directories(directory / L"logs");
        const std::wstring role = kClient ? L"input-client-" : L"input-host-";
        const auto unique = role + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(session);
        report = CreateFileW((directory / L"logs" / (unique + L".jsonl")).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (report == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create input log");
        running.store(true, std::memory_order_release);
        if (!kClient) {
            worker = CreateThread(nullptr, 0, &Receive, nullptr, 0, nullptr);
            if (!worker) { failure = GetLastError(); throw std::runtime_error("Cannot start input receiver"); }
        }
        failure = ExchangeSlot(slot, reinterpret_cast<void*>(original), reinterpret_cast<void*>(&OnInput));
        installed = *slot == reinterpret_cast<void*>(&OnInput);
        if (failure) throw std::runtime_error("Cannot hook input manager");
        std::ostringstream descriptor;
        descriptor << "{\"pid\":" << GetCurrentProcessId() << ",\"role\":\"" << (kClient ? "input-client" : "input-host") << "\",\"session\":\"" << session
            << "\",\"deadlineMs\":" << deadline << ",\"port\":" << port << ",\"address\":" << reinterpret_cast<std::uintptr_t>(&published)
            << ",\"bytes\":" << sizeof(published) << "}\n";
        const auto temp = directory / (unique + L".tmp");
        const auto ready = directory / (role + std::to_wstring(GetCurrentProcessId()) + L".json");
        failure = ERROR_WRITE_FAULT;
        if (!WriteText(temp, descriptor.str()) || !MoveFileExW(temp.c_str(), ready.c_str(), MOVEFILE_REPLACE_EXISTING)) throw std::runtime_error("Cannot publish input endpoint");
        return ERROR_SUCCESS;
    } catch (...) {
        running = false; Restore(); CloseNetwork();
        if (report != INVALID_HANDLE_VALUE) { CloseHandle(report); report = INVALID_HANDLE_VALUE; }
        return failure ? failure : ERROR_INVALID_DATA;
    }
}
extern "C" DWORD WINAPI IsaacAuthorityInputStop(void*) noexcept {
    Guard guard;
    if (report == INVALID_HANDLE_VALUE) return ERROR_NOT_READY;
    running = false; CloseNetwork();
    if (!Restore()) return ERROR_BUSY;
    InvalidatePublication();
    std::ostringstream out;
    out << "{\"type\":\"stop\",\"role\":\"" << (kClient ? "input-client" : "input-host") << "\",\"samples\":" << samples.load()
        << ",\"activeSamples\":" << activeSamples.load() << ",\"withheld\":" << withheld.load()
        << ",\"predicts\":" << (kPredict ? "true" : "false") << ",\"consumedSequence\":" << consumedSequence.load()
        << ",\"accepted\":" << accepted.load() << ",\"rejected\":" << rejected.load()
        << ",\"networkErrors\":" << errors.load() << ",\"valueReads\":" << values.load() << ",\"pressedReads\":" << presses.load()
        << ",\"triggeredReads\":" << triggers.load() << ",\"passedThrough\":" << passedThrough.load()
        << ",\"controller\":" << latest.controller << ",\"lastSequence\":" << latest.sequence << ",\"polls\":[";
    bool first = true; // [controller, reader (0 value, 1 pressed, 2 triggered), action, count]
    for (int c = 0; c < kControllers; ++c) for (int r = 0; r < 3; ++r) for (int a = 0; a < kPolledActions; ++a) {
        const auto count = polls[c][r][a].load();
        if (!count) continue;
        out << (first ? "" : ",") << '[' << c - 1 << ',' << r << ',' << a << ',' << count << ']'; first = false;
    }
    out << "],\"slotRestored\":true}\n";
    const auto text = out.str(); DWORD written = 0;
    const BOOL ok = WriteFile(report, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(report); report = INVALID_HANDLE_VALUE;
    return ok && written == text.size() && !errors.load() ? ERROR_SUCCESS : ERROR_INVALID_DATA;
}
// For the world module loaded in the same game: which command's movement the game last read, and for which controller.
extern "C" DWORD WINAPI IsaacAuthorityInputAcknowledged(std::int32_t* controller, std::uint32_t* sequence) noexcept {
    if (!controller || !sequence) return ERROR_INVALID_PARAMETER;
    const auto consumed = consumedSequence.load();
    if (!running.load() || !consumed) return ERROR_NOT_READY;
    *controller = consumedController.load(); *sequence = consumed;
    return ERROR_SUCCESS;
}
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
