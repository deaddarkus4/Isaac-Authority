# IsaacNetProbe 0.2.1 — engine event recorder

Windows x86 diagnostics for the original Repentance+ J460 executable, SHA-256 `3bdfc8bae0dc7e334b76009d0ad45dfbb16ee5f00c06ffbc3a0094e34d44616b`.

The recorder captures the game's formatted `OutputDebugStringA` messages, including trinket additions, item-queue flushes, room changes, and emitted networking/desync messages. JSONL events include sequence, QPC timestamp, thread ID, category and message. It does not implement new multiplayer behavior or capture raw input/packet payloads.

## Build and test

Requires Visual Studio 2022 C++ x86 tools and CMake 3.20+:

```powershell
.\scripts\Build-NativeProbe.ps1 -GameExecutable "C:\Program Files (x86)\Steam\steamapps\common\The Binding of Isaac Rebirth\isaac-ng.exe"
```

Five tests cover malformed images, DLL exports/unsupported-host rejection, concurrent recording, loading into a separate synthetic x86 process and original-J460 recognition. The test-only `ProbeRecorderFixture.dll` tests the shared recorder without requiring a game; it is never installed or packaged. Production `IsaacProbeStart` always validates J460. Release binaries use the static C++ runtime and are written to `Binaries/native-build/Release/`.

## Install and start

Close Isaac and run `scripts/Install-IsaacNetProbe.ps1`. The installer searches registered Steam libraries on every configured drive, using `appmanifest_250900.acf` and the standard folder fallback. If no installation or multiple installations are found, a file picker lets you select `isaac-ng.exe`. `-GameExecutable` remains available for an explicit path or folder. Files are installed in the selected game's `IsaacNetProbe` subdirectory; the **Isaac Diagnostics** shortcut is created on the desktop.

The shared `scripts/IsaacPaths.ps1` is shipped with the installer, launcher and diagnostic collector. Modern and legacy library files, duplicate paths, Unicode paths and non-default manifest directory names are covered by `scripts/tests/Test-IsaacPaths.ps1`. Package 0.2.1 changes discovery and packaging; the native recorder binaries are unchanged from 0.2.0.

Launch that shortcut or `IsaacNetProbe/Start-IsaacDiagnostics.cmd`. The launcher starts Isaac through Steam if necessary, loads the DLL into the matching process and calls its start export. **RECORDING READY** is reported only after initialization succeeds. Ordinary Steam launches do not automatically activate the recorder.

Logs: `%LOCALAPPDATA%\IsaacNetProbe\logs\session-*.jsonl`. The latest path is in `%LOCALAPPDATA%\IsaacNetProbe\last-session.txt`; loader output is stored beside it. The diagnostic collector includes the three latest native logs.

To stop a running recorder, use `IsaacNetProbeAttach.exe --stop <pid> IsaacNetProbe.dll`. Closing the game removes all process-local changes. To uninstall, close Isaac, remove its `IsaacNetProbe` subdirectory and the shortcut. The original EXE and mod settings are preserved.

## Implementation and limits

- Only the game's `OutputDebugStringA` import slot, RVA `0x71827C`, is replaced. The game logger calls this slot directly at RVA `0x61140E`. No inferred C++ gameplay ABI is used.
- Every call is forwarded to its previous target with the original message and incoming `LastError`. Gameplay arguments, RNG and saves are not edited.
- A 1024-event queue separates capture from file I/O. A short lock protects queue memory; disk operations run on a worker. Overflow appears in `droppedTotal` and the explicit-stop footer. Messages are bounded to 2047 bytes and mark truncation.
- Non-ASCII ANSI bytes are preserved as byte escapes. QPC timestamps are local to the process; align different PCs using frame labels and UTC.
- Process termination can stop the writer before its queue drains. Missing `session_stop` alone is not proof of a crash; ignore an incomplete last line.
- The DLL is pinned until process exit to protect in-flight callbacks. Explicit stop restores the previous import target and drains the queue.
- Other DLLs' debug output and previously cached function pointers are outside this capture point. The current game's logger was verified to read this import directly.
- Logs can contain names, Steam IDs, paths and other game-emitted text. They are stored locally, not uploaded.

`IsaacProbeInspect` and `IsaacNetProbeCli inspect <exe> <new-report.json>` remain available. Their inventory reports do not describe the running recorder. New exports are `DWORD WINAPI IsaacProbeStart(void*)` and `DWORD WINAPI IsaacProbeStop(void*)`, invoked outside the loader lock. `DllMain` does no work.

The [static candidate map](profiles/j460-static-candidates.json) and [stage 1 report](../../docs/native-stage1.md) remain research artifacts. Direct frame/input/stat-cache hooks have not yet been enabled.
