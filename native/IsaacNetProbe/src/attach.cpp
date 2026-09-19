#include "profile.hpp"
#include "injector.hpp"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) {
        std::cerr << "Usage: IsaacNetProbeAttach --wait <game.exe> <probe.dll> | --attach <pid> <probe.dll> | --stop <pid> <probe.dll>\n";
        return 4;
    }
    try {
        const std::wstring mode = argv[1];
        DWORD pid = 0;
        if (mode == L"--wait") {
            const auto expected = std::filesystem::absolute(argv[2]);
            if (!isaac_probe::AnalyzeBytes(isaac_probe::ReadFile(expected)).supported)
                throw std::runtime_error("Only the original, unpatched J460 executable is supported");
            std::cout << "Waiting up to 10 minutes for Isaac..." << std::endl;
            const auto start = GetTickCount64();
            while (!pid && GetTickCount64() - start < 600000) {
                pid = isaac_probe::FindProcess(expected);
                if (!pid) Sleep(200);
            }
            if (!pid) throw std::runtime_error("Isaac did not start before the timeout");
        } else if (mode == L"--attach" || mode == L"--stop") {
            const auto number = std::stoul(argv[2]);
            if (number == 0) throw std::runtime_error("Invalid PID");
            pid = static_cast<DWORD>(number);
        } else throw std::runtime_error("Unknown command");
        const auto image = isaac_probe::ProcessImagePath(pid);
        if (!isaac_probe::AnalyzeBytes(isaac_probe::ReadFile(image)).supported)
            throw std::runtime_error("Target process is not running the supported original J460 executable");
        const bool stop = mode == L"--stop";
        const DWORD result = isaac_probe::InvokeProbeExport(pid, argv[3], stop ? "IsaacProbeStop" : "IsaacProbeStart", !stop);
        if (result != ERROR_SUCCESS && (!(!stop && result == ERROR_ALREADY_EXISTS)))
            throw std::runtime_error("Probe returned Windows status " + std::to_string(result));
        std::cout << (stop ? "RECORDING STOPPED" : "RECORDING READY") << " PID=" << pid << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
