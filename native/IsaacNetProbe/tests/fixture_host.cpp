#include <windows.h>
#include <string>

// Match the game's separate logging routine: reload the IAT target per call.
__declspec(noinline) void EmitEvents() {
    OutputDebugStringA("[INFO] - Adding trinket 100 (Vibrant Bulb) to test player\n");
    OutputDebugStringA("[INFO] - item queue flush\n");
    OutputDebugStringA("[Frame: 25] Desync detected! synthetic fixture\n");
}

int wmain() {
    const auto name = L"Local\\IsaacProbeFixtureExit-" + std::to_wstring(GetCurrentProcessId());
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
    if (!stop) return 1;
    for (unsigned i = 0; i < 1000; ++i) {
        if (WaitForSingleObject(stop, 10) == WAIT_OBJECT_0) break;
        EmitEvents();
    }
    CloseHandle(stop);
    return 0;
}
