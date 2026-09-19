#include "injector.hpp"
#include "profile.hpp"
#include <windows.h>
#include <iostream>
#include <stdexcept>

struct Child {
    PROCESS_INFORMATION info{};
    HANDLE stop = nullptr;
    ~Child() {
        if (stop) SetEvent(stop);
        if (info.hProcess) { WaitForSingleObject(info.hProcess, 12000); CloseHandle(info.hProcess); }
        if (info.hThread) CloseHandle(info.hThread);
        if (stop) CloseHandle(stop);
    }
};
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    try {
        auto log = std::filesystem::temp_directory_path() /
            (L"IsaacInjection-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".jsonl");
        Check(SetEnvironmentVariableW(L"ISAAC_PROBE_FIXTURE_LOG", log.c_str()) != FALSE, "Cannot set fixture output");
        Child child;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        std::wstring command = L"\"" + std::wstring(argv[1]) + L"\"";
        Check(CreateProcessW(argv[1], command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &child.info) != FALSE, "Could not start fixture");
        const auto event_name = L"Local\\IsaacProbeFixtureExit-" + std::to_wstring(child.info.dwProcessId);
        child.stop = CreateEventW(nullptr, TRUE, FALSE, event_name.c_str());
        Check(child.stop != nullptr, "Could not create fixture stop event");
        Sleep(100);
        Check(isaac_probe::FindProcess(argv[1]) == child.info.dwProcessId, "Process discovery failed");
        Check(isaac_probe::InvokeProbeExport(child.info.dwProcessId, argv[2], "FixtureStart", true) == ERROR_SUCCESS,
            "Remote recorder start failed");
        Sleep(200);
        Check(isaac_probe::InvokeProbeExport(child.info.dwProcessId, argv[2], "FixtureStop", false) == ERROR_SUCCESS,
            "Remote recorder stop failed");
        const auto bytes = isaac_probe::ReadFile(log);
        const std::string text(bytes.begin(), bytes.end());
        Check(text.find("injection-test-fixture") != std::string::npos, "Fixture header missing");
        Check(text.find("\"type\":\"trinket\"") != std::string::npos, "Remote trinket events missing");
        Check(text.find("\"type\":\"item_queue\"") != std::string::npos, "Remote queue events missing");
        Check(text.find("\"type\":\"desync\"") != std::string::npos, "Remote desync events missing");
        Check(text.find("\"type\":\"session_stop\"") != std::string::npos, "Remote stop event missing");
        Check(text.find("\"dropped\":0") != std::string::npos, "Messages lost in the low-volume fixture");
        Sleep(100);
        Check(isaac_probe::ReadFile(log) == bytes, "Remote hook still active after stop");
        SetEvent(child.stop);
        Check(WaitForSingleObject(child.info.hProcess, 3000) == WAIT_OBJECT_0, "Fixture did not exit normally");
        DWORD result = 1; GetExitCodeProcess(child.info.hProcess, &result);
        Check(result == 0, "Fixture failed after recorder unload");
        std::wcout << L"PASS: " << log.c_str() << L'\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
