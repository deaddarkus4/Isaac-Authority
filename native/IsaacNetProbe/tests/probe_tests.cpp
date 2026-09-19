#include "profile.hpp"
#include <windows.h>
#include <cstring>
#include <iostream>
#include <stdexcept>

void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 1) {
            Check(!isaac_probe::AnalyzeBytes({}).supported, "Empty input accepted");
            std::vector<std::uint8_t> bytes(512, 0);
            IMAGE_DOS_HEADER dos{};
            dos.e_magic = IMAGE_DOS_SIGNATURE;
            dos.e_lfanew = 0x7fffffff;
            std::memcpy(bytes.data(), &dos, sizeof(dos));
            Check(!isaac_probe::AnalyzeBytes(bytes).supported, "Out-of-bounds PE header accepted");
            dos.e_lfanew = 128;
            std::memcpy(bytes.data(), &dos, sizeof(dos));
            IMAGE_NT_HEADERS32 nt{};
            nt.Signature = IMAGE_NT_SIGNATURE;
            nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
            nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
            std::memcpy(bytes.data() + 128, &nt, sizeof(nt));
            Check(isaac_probe::AnalyzeBytes(bytes).reason == "unsupported_architecture", "x64 image not rejected");
        } else if (argc == 3 && std::wstring(argv[1]) == L"--game") {
            const auto original = isaac_probe::ReadFile(argv[2]);
            Check(isaac_probe::AnalyzeBytes(original).supported, "Original J460 not recognized");
            auto modified = original;
            modified[0x505080] ^= 1;
            Check(isaac_probe::AnalyzeBytes(modified).reason == "unsupported_executable_hash", "Modified image accepted");
            Check(isaac_probe::ReadFile(argv[2]) == original, "Installed file changed");
        } else if (argc == 3 && std::wstring(argv[1]) == L"--dll") {
            HMODULE dll = LoadLibraryW(argv[2]);
            Check(dll != nullptr, "DLL could not be loaded");
            using Inspect = int(__cdecl*)(const wchar_t*);
            auto entry = reinterpret_cast<Inspect>(GetProcAddress(dll, "IsaacProbeInspect"));
            Check(entry != nullptr, "Export missing");
            Check(entry(nullptr) == 4, "Null path was not rejected");
            using Lifecycle = DWORD(WINAPI*)(void*);
            auto start = reinterpret_cast<Lifecycle>(GetProcAddress(dll, "IsaacProbeStart"));
            auto stop = reinterpret_cast<Lifecycle>(GetProcAddress(dll, "IsaacProbeStop"));
            Check(start != nullptr && stop != nullptr, "Recorder exports missing");
            Check(start(nullptr) == ERROR_BAD_EXE_FORMAT, "Recorder accepted an unsupported host");
            Check(stop(nullptr) == ERROR_NOT_READY, "Unstarted recorder reported running");
            const auto report = std::filesystem::temp_directory_path() /
                (L"IsaacNetProbe-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".json");
            Check(entry(report.c_str()) == 1, "Unsupported host process accepted");
            const auto bytes = isaac_probe::ReadFile(report);
            const std::string text(bytes.begin(), bytes.end());
            Check(text.find("\"hooksInstalled\": 0") != std::string::npos, "Unexpected hook status");
            Check(entry(report.c_str()) == 3, "Existing file was not protected");
            Check(isaac_probe::ReadFile(report) == bytes, "Existing report was overwritten");
            FreeLibrary(dll);
            std::filesystem::remove(report);
        } else {
            throw std::runtime_error("Unexpected test arguments");
        }
        std::cout << "PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
