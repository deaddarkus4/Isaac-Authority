#include "profile.hpp"
#include "injector.hpp"
#include <iostream>
#include <string>
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 4) throw std::runtime_error("Usage: IsaacAuthorityAttach start|source|network|replica|world|world-stop|input|input-stop|localhost|localhost-stop|correct|stop PID DLL");
        const std::wstring action = argv[1];
        const char* entry = action == L"start" ? "IsaacAuthorityStart" :
            action == L"network" ? "IsaacAuthorityNetworkStart" :
            action == L"replica" ? "IsaacAuthorityReplicaStart" :
            action == L"source" ? "IsaacAuthoritySourceStart" :
            action == L"world" ? "IsaacAuthorityWorldStart" :
            action == L"world-stop" ? "IsaacAuthorityWorldStop" :
            action == L"input" ? "IsaacAuthorityInputStart" :
            action == L"input-stop" ? "IsaacAuthorityInputStop" :
            action == L"localhost" ? "IsaacAuthorityLocalhostStart" :
            action == L"localhost-stop" ? "IsaacAuthorityLocalhostStop" :
            action == L"correct" ? "IsaacAuthorityCorrectOnce" : action == L"stop" ? "IsaacAuthorityStop" : nullptr;
        if (!entry) throw std::runtime_error("Unknown action");
        std::size_t used = 0; const auto pid = std::stoul(argv[2], &used);
        if (!pid || used != std::wstring(argv[2]).size()) throw std::runtime_error("Invalid PID");
        if (!isaac_probe::AnalyzeBytes(isaac_probe::ReadFile(isaac_probe::ProcessImagePath(pid))).supported)
            throw std::runtime_error("Original J460 required");
        const DWORD result = isaac_probe::InvokeProbeExport(pid, argv[3], entry, action == L"start" || action == L"network" || action == L"replica" || action == L"source" || action == L"world" || action == L"input" || action == L"localhost");
        std::cout << "{\"export\":\"" << entry << "\",\"result\":" << result << "}\n";
        return result == ERROR_SUCCESS ? 0 : 1;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
}
