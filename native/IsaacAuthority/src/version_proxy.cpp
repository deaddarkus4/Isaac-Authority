// version.dll, put beside isaac-ng.exe: how the installed module gets into the game without anybody starting anything.
//
// The game's EOS library imports VERSION.dll, and Windows looks for it in the game's folder first. This one passes every
// call on to the system's own version.dll (loaded by its full path) and, in isaac-ng.exe only, loads
// IsaacAuthorityNative.dll from the same folder and calls its IsaacAuthorityNativeAuto - after which that module looks
// after itself (see native_adapter.cpp). Removing the two files is the whole uninstall. 32-bit only, as the game is.
#include <windows.h>
#include <cstddef>

namespace {
const char* const kNames[17] = {
    "GetFileVersionInfoA", "GetFileVersionInfoByHandle", "GetFileVersionInfoExA", "GetFileVersionInfoExW", "GetFileVersionInfoSizeA",
    "GetFileVersionInfoSizeExA", "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW", "GetFileVersionInfoW", "VerFindFileA", "VerFindFileW",
    "VerInstallFileA", "VerInstallFileW", "VerLanguageNameA", "VerLanguageNameW", "VerQueryValueA", "VerQueryValueW"};
HMODULE self = nullptr;

void __stdcall Missing() {}

DWORD WINAPI LoadAuthority(void*) noexcept {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(self, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return 1;
    wchar_t* leaf = path + length; while (leaf > path && leaf[-1] != L'\\') --leaf;
    const wchar_t name[] = L"IsaacAuthorityNative.dll";
    if (static_cast<std::size_t>(leaf - path) + sizeof(name) / sizeof(wchar_t) > MAX_PATH) return 1;
    for (std::size_t n = 0; n < sizeof(name) / sizeof(wchar_t); ++n) leaf[n] = name[n];
    const HMODULE module = LoadLibraryW(path);
    const auto start = module ? reinterpret_cast<DWORD(WINAPI*)(void*)>(GetProcAddress(module, "IsaacAuthorityNativeAuto")) : nullptr;
    return start ? start(nullptr) : 2;
}
}

extern "C" FARPROC realVersion[17] = {};

// Each export is a jump through the table: whatever the arguments, they pass untouched.
#define FORWARD(index, name) extern "C" __declspec(naked) void Forward_##name() { __asm { jmp dword ptr [realVersion + index * 4] } }
FORWARD(0, GetFileVersionInfoA) FORWARD(1, GetFileVersionInfoByHandle) FORWARD(2, GetFileVersionInfoExA) FORWARD(3, GetFileVersionInfoExW)
FORWARD(4, GetFileVersionInfoSizeA) FORWARD(5, GetFileVersionInfoSizeExA) FORWARD(6, GetFileVersionInfoSizeExW) FORWARD(7, GetFileVersionInfoSizeW)
FORWARD(8, GetFileVersionInfoW) FORWARD(9, VerFindFileA) FORWARD(10, VerFindFileW) FORWARD(11, VerInstallFileA) FORWARD(12, VerInstallFileW)
FORWARD(13, VerLanguageNameA) FORWARD(14, VerLanguageNameW) FORWARD(15, VerQueryValueA) FORWARD(16, VerQueryValueW)

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    self = instance; DisableThreadLibraryCalls(instance);
    wchar_t system[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(system, MAX_PATH);   // a 32-bit process is shown the 32-bit folder under this name
    const wchar_t name[] = L"\\version.dll";
    if (!length || length + sizeof(name) / sizeof(wchar_t) > MAX_PATH) return FALSE;
    for (std::size_t n = 0; n < sizeof(name) / sizeof(wchar_t); ++n) system[length + n] = name[n];
    const HMODULE real = LoadLibraryW(system);
    if (!real) return FALSE;
    for (int n = 0; n < 17; ++n) { realVersion[n] = GetProcAddress(real, kNames[n]); if (!realVersion[n]) realVersion[n] = reinterpret_cast<FARPROC>(&Missing); }
    // Only the game itself gets the module: the crash uploader beside it loads this library too.
    wchar_t image[MAX_PATH]{}; const DWORD got = GetModuleFileNameW(nullptr, image, MAX_PATH);
    const wchar_t game[] = L"isaac-ng.exe"; const DWORD tail = sizeof(game) / sizeof(wchar_t) - 1;
    if (got >= tail && got < MAX_PATH && lstrcmpiW(image + got - tail, game) == 0)
        if (const HANDLE thread = CreateThread(nullptr, 0, &LoadAuthority, nullptr, 0, nullptr)) CloseHandle(thread);   // runs once the loader is done with this library
    return TRUE;
}
