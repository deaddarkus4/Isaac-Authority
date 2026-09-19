#include "injector.hpp"
#include <tlhelp32.h>
#include <string>
#include <stdexcept>

namespace isaac_probe {
namespace {
struct Handle {
    HANDLE value;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string(message) + " (Windows error " + std::to_string(GetLastError()) + ")");
}
std::uintptr_t ModuleBase(DWORD pid, const std::wstring& name, std::filesystem::path* full_path = nullptr) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    Require(snapshot.value != INVALID_HANDLE_VALUE, "Cannot enumerate process modules");
    MODULEENTRY32W module{}; module.dwSize = sizeof(module);
    if (Module32FirstW(snapshot.value, &module)) {
        do {
            if (_wcsicmp(module.szModule, name.c_str()) == 0) {
                if (full_path) *full_path = module.szExePath;
                return reinterpret_cast<std::uintptr_t>(module.modBaseAddr);
            }
        } while (Module32NextW(snapshot.value, &module));
    }
    return 0;
}
std::uintptr_t RemoteSystemFunction(DWORD pid, const char* name) {
    FARPROC function = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), name);
    Require(function != nullptr, "System function unavailable");
    HMODULE owner = nullptr;
    Require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(function), &owner) != FALSE, "Cannot resolve system function owner");
    wchar_t filename[32768]{};
    Require(GetModuleFileNameW(owner, filename, 32768) != 0, "Cannot resolve system module path");
    const auto remote = ModuleBase(pid, std::filesystem::path(filename).filename().wstring());
    Require(remote != 0, "System function module missing in target");
    return remote + reinterpret_cast<std::uintptr_t>(function) - reinterpret_cast<std::uintptr_t>(owner);
}
DWORD Call(HANDLE process, std::uintptr_t address, void* argument, bool& completed) {
    completed = false;
    Handle thread(CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(address), argument, 0, nullptr));
    Require(thread.value != nullptr, "Cannot create probe initialization thread");
    const DWORD wait = WaitForSingleObject(thread.value, 15000);
    if (wait == WAIT_TIMEOUT) throw std::runtime_error("Probe initialization timed out; the game was not terminated");
    Require(wait == WAIT_OBJECT_0, "Cannot wait for probe initialization");
    completed = true;
    DWORD result = 0;
    Require(GetExitCodeThread(thread.value, &result) != FALSE, "Cannot read probe initialization result");
    return result;
}
}

std::filesystem::path ProcessImagePath(DWORD pid) {
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    Require(process.value != nullptr, "Cannot open target process");
    wchar_t filename[32768]{}; DWORD length = 32768;
    Require(QueryFullProcessImageNameW(process.value, 0, filename, &length) != FALSE, "Cannot resolve target executable");
    return std::filesystem::path(filename);
}

DWORD FindProcess(const std::filesystem::path& expected_image) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    Require(snapshot.value != INVALID_HANDLE_VALUE, "Cannot enumerate processes");
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot.value, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, expected_image.filename().c_str()) != 0) continue;
            try {
                if (std::filesystem::equivalent(ProcessImagePath(entry.th32ProcessID), expected_image)) return entry.th32ProcessID;
            } catch (...) { /* Process may exit during enumeration. */ }
        } while (Process32NextW(snapshot.value, &entry));
    }
    return 0;
}

DWORD InvokeProbeExport(DWORD pid, const std::filesystem::path& dll_path, const char* export_name, bool load_if_missing) {
    const auto dll = std::filesystem::absolute(dll_path);
    Require(std::filesystem::is_regular_file(dll), "Probe DLL not found");
    // Map our DLL locally only to read the export RVA; do not run its initialization.
    HMODULE local = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    Require(local != nullptr, "Cannot read probe DLL exports");
    FARPROC entry = GetProcAddress(local, export_name);
    const auto rva = entry ? reinterpret_cast<std::uintptr_t>(entry) - reinterpret_cast<std::uintptr_t>(local) : 0;
    FreeLibrary(local);
    Require(rva != 0, "Probe export missing");
    Handle process(OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid));
    Require(process.value != nullptr, "Cannot open target for probe loading");
    std::filesystem::path loaded_path;
    auto base = ModuleBase(pid, dll.filename().wstring(), &loaded_path);
    if (base && !std::filesystem::equivalent(loaded_path, dll))
        throw std::runtime_error("A different DLL with the same name is already loaded; restart the target first");
    if (base == 0 && load_if_missing) {
        const auto load_library = RemoteSystemFunction(pid, "LoadLibraryW");
        const auto text = dll.wstring();
        const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
        void* remote_path = VirtualAllocEx(process.value, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        Require(remote_path != nullptr, "Cannot allocate DLL path in target");
        SIZE_T written = 0;
        if (!WriteProcessMemory(process.value, remote_path, text.c_str(), bytes, &written) || written != bytes) {
            VirtualFreeEx(process.value, remote_path, 0, MEM_RELEASE);
            throw std::runtime_error("Cannot copy DLL path into target");
        }
        bool completed = false;
        try {
            const DWORD module = Call(process.value, load_library, remote_path, completed);
            VirtualFreeEx(process.value, remote_path, 0, MEM_RELEASE);
            remote_path = nullptr;
            Require(module != 0, "Windows could not load probe DLL in target");
        } catch (...) {
            // A timed-out loader may still use the path; leave it until process exit.
            if (completed && remote_path) VirtualFreeEx(process.value, remote_path, 0, MEM_RELEASE);
            throw;
        }
        base = ModuleBase(pid, dll.filename().wstring(), &loaded_path);
        if (base && !std::filesystem::equivalent(loaded_path, dll))
            throw std::runtime_error("The loaded DLL path does not match the selected probe");
    }
    Require(base != 0, "Probe is not loaded in target");
    bool completed = false;
    return Call(process.value, base + rva, nullptr, completed);
}
}
