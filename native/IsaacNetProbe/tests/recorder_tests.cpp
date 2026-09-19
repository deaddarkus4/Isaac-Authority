#include "recorder.hpp"
#include "profile.hpp"
#include <iostream>
#include <thread>
#include <stdexcept>
#include <vector>

void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int wmain() {
    try {
        auto path = std::filesystem::temp_directory_path() /
            (L"IsaacRecorder-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".jsonl");
        SetLastError(0x1234); OutputDebugStringA("baseline"); const DWORD baseline = GetLastError();
        Check(isaac_probe::StartRecorder(GetModuleHandleW(nullptr), path, "synthetic-test") == ERROR_SUCCESS, "Recorder start failed");
        SetLastError(0x1234); OutputDebugStringA("baseline"); Check(GetLastError() == baseline, "LastError behavior changed");
        OutputDebugStringA("[INFO] Adding trinket 100 (Vibrant Bulb)\n");
        OutputDebugStringA("[INFO] item queue flush\n");
        OutputDebugStringA("[Frame: 25] Desync detected! quoted=\"test\" path=C:\\test\n");
        Sleep(150);
        const std::string large(4096, 'x'); OutputDebugStringA(large.c_str());
        std::vector<std::thread> writers;
        for (unsigned t = 0; t < 4; ++t) writers.emplace_back([] {
            for (unsigned i = 0; i < 1200; ++i) OutputDebugStringA("[Frame: 100] network burst\n");
        });
        for (auto& thread : writers) thread.join();
        Check(isaac_probe::StopRecorder() == ERROR_SUCCESS, "Recorder stop failed");
        auto bytes = isaac_probe::ReadFile(path);
        const std::string text(bytes.begin(), bytes.end());
        Check(text.find("\"type\":\"session_start\"") != std::string::npos, "Session start missing");
        Check(text.find("\"type\":\"trinket\"") != std::string::npos, "Trinket event missing");
        Check(text.find("\"type\":\"item_queue\"") != std::string::npos, "Queue event missing");
        Check(text.find("\"type\":\"desync\"") != std::string::npos, "Desync event missing");
        Check(text.find("\\\"test\\\"") != std::string::npos, "JSON quote escaping missing");
        Check(text.find("\"truncated\":true") != std::string::npos, "Long message was not bounded");
        Check(text.find("\"type\":\"session_stop\"") != std::string::npos, "Stop record missing");
        OutputDebugStringA("This must not be captured after stop.");
        Check(isaac_probe::ReadFile(path) == bytes, "Hook still records after stop");
        Check(isaac_probe::StartRecorder(GetModuleHandleW(nullptr), path, "synthetic-test") == ERROR_FILE_EXISTS,
            "Existing log was overwritten");
        Check(isaac_probe::ReadFile(path) == bytes, "Existing log changed");
        std::wcout << L"PASS: " << path.c_str() << L'\n';
        return 0;
    } catch (const std::exception& e) {
        isaac_probe::StopRecorder();
        std::cerr << e.what() << '\n'; return 1;
    }
}
