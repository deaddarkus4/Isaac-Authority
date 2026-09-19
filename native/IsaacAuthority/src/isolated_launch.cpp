#include "profile.hpp"
#include <windows.h>
#include <winternl.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Child {
    PROCESS_INFORMATION info{};
    bool resumed = false;
    ~Child() {
        // Only our still-suspended child is disposable; never terminate a running game.
        if (info.hProcess && !resumed) { TerminateProcess(info.hProcess, 1); WaitForSingleObject(info.hProcess, 3000); }
        if (info.hThread) CloseHandle(info.hThread);
        if (info.hProcess) CloseHandle(info.hProcess);
    }
};
}
int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc == 4 || (argc == 5 && std::wstring(argv[4]) == L"--verify-only"),
            "Usage: IsaacAuthorityLaunch GAME_EXE WORKING_DIRECTORY SAVE_LEAF [--verify-only]");
        const auto exe = std::filesystem::absolute(argv[1]);
        const auto working = std::filesystem::absolute(argv[2]);
        Check(isaac_probe::AnalyzeBytes(isaac_probe::ReadFile(exe)).supported, "Original J460 required");
        Check(std::filesystem::is_directory(working), "Working directory missing");
        constexpr char expected[] = "Binding of Isaac Repentance+/";
        std::string leaf;
        for (const wchar_t* c = argv[3]; *c; ++c) {
            Check((*c >= L'a' && *c <= L'z') || (*c >= L'A' && *c <= L'Z') ||
                  (*c >= L'0' && *c <= L'9') || *c == L'-' || *c == L'_', "Save leaf must be plain ASCII");
            leaf += static_cast<char>(*c);
        }
        Check(leaf.rfind("IsaacAuthority-", 0) == 0 && leaf.size() + 1 < sizeof(expected), "Invalid isolated save leaf");
        std::array<char, sizeof(expected)> replacement{};
        leaf += '/'; std::memcpy(replacement.data(), leaf.data(), leaf.size());
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        Child child;
        std::wstring command = L"\"" + exe.wstring() + L"\"";
        // Child launch context, not a machine-wide environment change.
        Check(SetEnvironmentVariableW(L"SteamAppId", L"250900") && SetEnvironmentVariableW(L"SteamGameId", L"250900"),
            "Cannot set Steam child launch context");
        Check(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
            nullptr, working.c_str(), &startup, &child.info) != FALSE, "Cannot create suspended test instance");
        using Query = NTSTATUS(NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
        const auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
        PROCESS_BASIC_INFORMATION basic{};
        Check(query && query(child.info.hProcess, ProcessBasicInformation, &basic, sizeof(basic), nullptr) >= 0,
            "Cannot locate child PEB");
        std::uintptr_t base = 0; SIZE_T got = 0;
        static_assert(sizeof(void*) == 4, "Launcher must be x86");
        Check(ReadProcessMemory(child.info.hProcess, reinterpret_cast<const char*>(basic.PebBaseAddress) + 8,
            &base, sizeof(base), &got) && got == sizeof(base), "Cannot read child image base");
        // Verified string xref in J460 save-directory initializer RVA 0x5a9510.
        // USERPROFILE is only a fallback after the real user token, so env override is insufficient.
        const auto address = reinterpret_cast<void*>(base + 0x77e04c);
        std::array<char, sizeof(expected)> original{};
        Check(ReadProcessMemory(child.info.hProcess, address, original.data(), original.size(), &got) &&
            got == original.size() && std::memcmp(original.data(), expected, sizeof(expected)) == 0,
            "Child save-directory string does not match J460");
        DWORD protection = 0;
        Check(VirtualProtectEx(child.info.hProcess, address, replacement.size(), PAGE_READWRITE, &protection) != FALSE,
            "Cannot protect child save-directory string");
        SIZE_T written = 0;
        const BOOL changed = WriteProcessMemory(child.info.hProcess, address, replacement.data(), replacement.size(), &written);
        DWORD ignored = 0;
        const BOOL restored = VirtualProtectEx(child.info.hProcess, address, replacement.size(), protection, &ignored);
        Check(changed && written == replacement.size() && restored, "Cannot set isolated child save directory");
        Check(ReadProcessMemory(child.info.hProcess, address, original.data(), original.size(), &got) &&
            got == original.size() && original == replacement, "Child save-directory readback differs");
        if (argc == 5) {
            std::cout << "{\"verified\":true,\"startedGame\":false}\n";
            return 0;
        }
        Check(ResumeThread(child.info.hThread) != static_cast<DWORD>(-1), "Cannot resume isolated game");
        child.resumed = true;
        std::cout << "{\"pid\":" << child.info.dwProcessId << ",\"saveLeaf\":\"" << leaf
            << "\",\"exeFileModified\":false}" << std::endl;
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << " (Windows " << GetLastError() << ")\n"; return 1; }
}
