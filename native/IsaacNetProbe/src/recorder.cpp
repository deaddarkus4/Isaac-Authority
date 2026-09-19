#include "recorder.hpp"
#include <atomic>
#include <array>
#include <cstring>
#include <sstream>
#include <string>

namespace isaac_probe {
namespace {
using DebugOutput = void(WINAPI*)(LPCSTR);
constexpr unsigned kCapacity = 1024;
constexpr unsigned kMessageBytes = 2048;
struct Event {
    LONGLONG tick = 0;
    DWORD thread = 0;
    unsigned length = 0;
    bool truncated = false;
    char message[kMessageBytes]{};
};
SRWLOCK queue_lock = SRWLOCK_INIT;
SRWLOCK lifecycle_lock = SRWLOCK_INIT;
struct LifecycleGuard {
    LifecycleGuard() { AcquireSRWLockExclusive(&lifecycle_lock); }
    ~LifecycleGuard() { ReleaseSRWLockExclusive(&lifecycle_lock); }
};
std::array<Event, kCapacity> queue;
unsigned head = 0, tail = 0, count = 0;
std::atomic<unsigned> dropped{0};
std::atomic<bool> disk_failed{false};
std::atomic<int> state{0}; // 0=off, 1=starting, 2=recording, 3=stopping
HANDLE file = INVALID_HANDLE_VALUE;
HANDLE wake = nullptr;
HANDLE writer = nullptr;
DebugOutput original = nullptr;
void** import_slot = nullptr;
__declspec(thread) bool in_hook = false;

unsigned CopyMessage(char* output, const char* input, bool* truncated) noexcept {
    unsigned n = 0;
    *truncated = false;
    if (!input) return 0;
    __try {
        while (n < kMessageBytes - 1 && input[n] != 0) { output[n] = input[n]; ++n; }
        *truncated = n == kMessageBytes - 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        const char invalid[] = "<unreadable debug string>";
        std::memcpy(output, invalid, sizeof(invalid));
        return sizeof(invalid) - 1;
    }
    output[n] = 0;
    return n;
}

void WINAPI CaptureDebugOutput(LPCSTR message) {
    const DWORD error = GetLastError();
    if (!in_hook && state.load(std::memory_order_acquire) == 2) {
        in_hook = true;
        // The lock protects only the bounded memory queue; disk I/O runs elsewhere.
        AcquireSRWLockExclusive(&queue_lock);
        {
            if (state.load(std::memory_order_acquire) == 2 && count < kCapacity) {
                auto& event = queue[tail];
                LARGE_INTEGER tick{};
                QueryPerformanceCounter(&tick);
                event.tick = tick.QuadPart;
                event.thread = GetCurrentThreadId();
                event.length = CopyMessage(event.message, message, &event.truncated);
                tail = (tail + 1) % kCapacity;
                ++count;
                SetEvent(wake);
            } else if (state.load() == 2) {
                dropped.fetch_add(1, std::memory_order_relaxed);
            }
            ReleaseSRWLockExclusive(&queue_lock);
        }
        in_hook = false;
    }
    SetLastError(error);
    original(message);
}

bool Pop(Event& event) {
    AcquireSRWLockExclusive(&queue_lock);
    const bool available = count != 0;
    if (available) {
        event = queue[head];
        head = (head + 1) % kCapacity;
        --count;
    }
    ReleaseSRWLockExclusive(&queue_lock);
    return available;
}

std::string Escape(const char* message, unsigned length) {
    std::string out;
    out.reserve(length + 32);
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < length; ++i) {
        const auto c = static_cast<unsigned char>(message[i]);
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 32 || c >= 128) {
            // ANSI bytes are escaped individually, keeping each JSON line valid.
            out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15];
        } else out += static_cast<char>(c);
    }
    return out;
}

const char* Classify(const char* message) {
    if (std::strstr(message, "trinket")) return "trinket";
    if (std::strstr(message, "item queue")) return "item_queue";
    if (std::strstr(message, "esync") || std::strstr(message, "checksum mismatch")) return "desync";
    if (std::strstr(message, "room transition") || std::strstr(message, "Room ")) return "room";
    if (std::strstr(message, "Net") || std::strstr(message, "MSG_") || std::strstr(message, "lobby")) return "network";
    if (std::strstr(message, "collectible")) return "collectible";
    return "engine";
}

bool Write(const std::string& text) {
    DWORD written = 0;
    if (!WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) || written != text.size()) {
        disk_failed.store(true);
        return false;
    }
    return true;
}

DWORD WINAPI WriteEvents(void*) noexcept {
    try {
        unsigned long long sequence = 0;
        for (;;) {
            Event event;
            while (Pop(event)) {
                std::ostringstream line;
                line << "{\"type\":\"" << Classify(event.message) << "\",\"sequence\":" << ++sequence
                     << ",\"qpc\":" << event.tick << ",\"thread\":" << event.thread
                     << ",\"droppedTotal\":" << dropped.load()
                     << ",\"truncated\":" << (event.truncated ? "true" : "false")
                     << ",\"message\":\"" << Escape(event.message, event.length) << "\"}\n";
                if (!disk_failed.load()) Write(line.str());
            }
            if (state.load(std::memory_order_acquire) == 3) break;
            WaitForSingleObject(wake, 100);
        }
        std::ostringstream footer;
        footer << "{\"type\":\"session_stop\",\"events\":" << sequence
               << ",\"dropped\":" << dropped.load() << ",\"diskError\":" << (disk_failed.load() ? "true" : "false") << "}\n";
        Write(footer.str());
        FlushFileBuffers(file);
    } catch (...) { disk_failed.store(true); }
    return 0;
}

void** FindImport(HMODULE host) {
    auto base = reinterpret_cast<unsigned char*>(host);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) return nullptr;
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || directory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) return nullptr;
    auto imports = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (unsigned i = 0; i < directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR) && imports[i].Name; ++i) {
        if (!imports[i].OriginalFirstThunk) continue;
        auto names = reinterpret_cast<const IMAGE_THUNK_DATA32*>(base + imports[i].OriginalFirstThunk);
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + imports[i].FirstThunk);
        for (unsigned j = 0; names[j].u1.AddressOfData; ++j) {
            if (IMAGE_SNAP_BY_ORDINAL32(names[j].u1.Ordinal)) continue;
            auto name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names[j].u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(name->Name), "OutputDebugStringA") == 0)
                return reinterpret_cast<void**>(&slots[j].u1.Function);
        }
    }
    return nullptr;
}

void CloseResources() {
    if (writer) { CloseHandle(writer); writer = nullptr; }
    if (wake) { CloseHandle(wake); wake = nullptr; }
    if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); file = INVALID_HANDLE_VALUE; }
}
}

bool RecorderRunning() noexcept { return state.load() == 2; }

DWORD StartRecorder(HMODULE host, const std::filesystem::path& path, const char* build_id,
                    std::uint32_t expected_iat_rva) noexcept {
    LifecycleGuard guard;
    int expected = 0;
    if (!state.compare_exchange_strong(expected, 1)) return ERROR_ALREADY_EXISTS;
    try {
        import_slot = FindImport(host);
        if (!import_slot || !*import_slot || (expected_iat_rva &&
            reinterpret_cast<std::uintptr_t>(import_slot) - reinterpret_cast<std::uintptr_t>(host) != expected_iat_rva)) {
            state.store(0); return ERROR_PROC_NOT_FOUND;
        }
        original = reinterpret_cast<DebugOutput>(*import_slot);
        file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) { const DWORD error = GetLastError(); state.store(0); return error; }
        wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!wake) { CloseResources(); state.store(0); return ERROR_NOT_ENOUGH_MEMORY; }
        head = tail = count = 0; dropped.store(0); disk_failed.store(false);
        LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
        SYSTEMTIME utc{}; GetSystemTime(&utc);
        char utc_text[32]{};
        sprintf_s(utc_text, "%04u-%02u-%02uT%02u:%02u:%02uZ", utc.wYear, utc.wMonth, utc.wDay,
                  utc.wHour, utc.wMinute, utc.wSecond);
        std::ostringstream header;
        header << "{\"type\":\"session_start\",\"schemaVersion\":2,\"mode\":\"engine_debug_output\",\"pid\":"
               << GetCurrentProcessId() << ",\"qpcFrequency\":" << frequency.QuadPart
               << ",\"build\":\"" << Escape(build_id, static_cast<unsigned>(std::strlen(build_id)))
               << "\",\"utc\":\"" << utc_text << "\",\"messageEncoding\":\"ansi-byte-escape\",\"rawInputsCaptured\":false}\n";
        if (!Write(header.str())) { CloseResources(); state.store(0); return ERROR_WRITE_FAULT; }
        // Keep the callback code mapped for the remainder of the host process.
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&CaptureDebugOutput), &pinned)) {
            CloseResources(); state.store(0); return ERROR_MOD_NOT_FOUND;
        }
        writer = CreateThread(nullptr, 0, WriteEvents, nullptr, 0, nullptr);
        if (!writer) { CloseResources(); state.store(0); return ERROR_NOT_ENOUGH_MEMORY; }
        DWORD protection = 0;
        if (!VirtualProtect(import_slot, sizeof(void*), PAGE_READWRITE, &protection)) {
            state.store(3); SetEvent(wake); WaitForSingleObject(writer, INFINITE);
            CloseResources(); state.store(0); return ERROR_ACCESS_DENIED;
        }
        state.store(2, std::memory_order_release);
        InterlockedExchangePointer(import_slot, reinterpret_cast<void*>(&CaptureDebugOutput));
        DWORD ignored = 0;
        VirtualProtect(import_slot, sizeof(void*), protection, &ignored);
        return ERROR_SUCCESS;
    } catch (...) {
        // All potentially throwing operations precede worker creation / import replacement.
        CloseResources(); state.store(0); return ERROR_INVALID_DATA;
    }
}

DWORD StopRecorder() noexcept {
    LifecycleGuard guard;
    if (state.load() != 2) return ERROR_NOT_READY;
    DWORD protection = 0;
    if (!VirtualProtect(import_slot, sizeof(void*), PAGE_READWRITE, &protection)) return ERROR_ACCESS_DENIED;
    InterlockedCompareExchangePointer(import_slot, reinterpret_cast<void*>(original), reinterpret_cast<void*>(&CaptureDebugOutput));
    DWORD ignored = 0; VirtualProtect(import_slot, sizeof(void*), protection, &ignored);
    // Exclude a producer already inside the callback before closing its event handle.
    AcquireSRWLockExclusive(&queue_lock);
    state.store(3, std::memory_order_release);
    ReleaseSRWLockExclusive(&queue_lock);
    SetEvent(wake);
    WaitForSingleObject(writer, INFINITE);
    const DWORD result = disk_failed.load() ? ERROR_WRITE_FAULT : ERROR_SUCCESS;
    CloseResources(); state.store(0);
    return result;
}
}
