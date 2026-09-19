#include "profile.hpp"
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc != 4 || std::wstring(argv[1]) != L"inspect") {
        std::cerr << "Usage: IsaacNetProbeCli inspect <isaac-ng.exe> <report.json>\n";
        return 4;
    }
    try {
        const auto result = isaac_probe::AnalyzeBytes(isaac_probe::ReadFile(argv[2]));
        isaac_probe::WriteReport(argv[3], result);
        std::cout << result.reason << '\n';
        return result.supported ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 3;
    }
}
