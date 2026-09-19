#include "recorder.hpp"
#include <windows.h>

// Test-only DLL, never installed with the production probe.
extern "C" DWORD WINAPI FixtureStart(void*) noexcept {
    wchar_t path[32768]{};
    if (!GetEnvironmentVariableW(L"ISAAC_PROBE_FIXTURE_LOG", path, 32768)) return ERROR_INVALID_PARAMETER;
    return isaac_probe::StartRecorder(GetModuleHandleW(nullptr), path, "injection-test-fixture");
}
extern "C" DWORD WINAPI FixtureStop(void*) noexcept { return isaac_probe::StopRecorder(); }
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
