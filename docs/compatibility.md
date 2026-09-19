# Repentance+ binary compatibility

Checked on 18 September 2026 against patcher source commit `a90af38705de6f2e083ead53312f9d924005ffab` (version `1.4.0`).

## Examined executable

| Property | Value |
| --- | --- |
| Game version embedded in EXE | `Binding of Isaac: Repentance+ v1.9.7.17.J460` |
| Steam build, from local app manifest | `22878971` |
| Repentance+ depot / manifest | `3353471` / `229910742625134068` |
| PE architecture | x86 (`0x014c`) |
| PE timestamp (UTC) | `2026-04-21 02:40:39` |
| File size | `9,362,440` bytes |
| SHA-256 before patching | `3bdfc8bae0dc7e334b76009d0ad45dfbb16ee5f00c06ffbc3a0094e34d44616b` |

These identifiers describe the locally examined Steam executable. They do not establish compatibility with every storefront or later game update.

## Signature verification

Each original signature occurs exactly once. None of the patched signatures occurs in the examined original file. Offsets below are file offsets, not virtual addresses.

| Patch region | File offset | Region length |
| --- | --- | --- |
| Co-op mods | `0x50DC05` | 27 bytes |
| Analytics | `0x505080` | 11 bytes |
| Character toggle | `0x4EC589` | 40 bytes |
| Character cycle | `0x4EC60A` | 92 bytes |
| Character relocation | `0x89E0BE` | 6 bytes |
| Character availability | `0x548F70` | 32 bytes |
| Character availability relocation | `0x8A15B6` | 14 bytes |

No changes to the existing patch signatures are needed for this executable. The co-op and analytics signatures are also unchanged between release `v1.3.1` and the examined source commit.

## Patch execution on a temporary copy

A temporary .NET console harness compiled the existing `GamePatcher.cs` with `GameFinder` 4.3.3 and called its actual patch methods on a copy of the examined EXE.

| Operation | First call | Changed bytes | Status after call | Repeat call |
| --- | --- | --- | --- | --- |
| `PatchGameExecutable` | `true` | 6 | `Patched` | `false` |
| `PatchGameExecutableAnalytics` | `true` | 1 | `Patched` | `false` |
| `PatchGameExecutableCoopCharacters` | `true` | 149 | `Patched` | `false` |

All initial statuses were `NotPatched`. The resulting copy retained its original length. All 156 changed bytes matched the expected replacements, with every other byte unchanged. Repeating all three operations preserved the patched copy's SHA-256. The installed EXE's SHA-256 was unchanged before and after the checks.

The code signatures lie in `.text`, and the relocation signatures lie in `.reloc`. The character patch's absolute address `0x00C79888` maps to writable `.data`; this verifies address mapping, not the runtime meaning of the table stored there.

The self-contained Release publication also completed successfully using `FolderProfile2`.

## Scope

During the initial binary checks, the installed game executable was not patched or launched. Binary checks do not establish successful online play or synchronization for arbitrary mods. The optional character patch remains experimental.

EID was not installed during the initial binary checks, so its compatibility was not tested. It depends on the installed EID Lua files as well as the game version.

The current patcher replaces files without creating backups; preserve an original copy before applying it to an installation.

## Source and release history

- [Release v1.3.1](https://github.com/xADDBx/Isaac-Online-Modded/releases/tag/v1.3.1): published 28 March 2025.
- [Source commit a90af38](https://github.com/xADDBx/Isaac-Online-Modded/commit/a90af38705de6f2e083ead53312f9d924005ffab): committed 31 August 2026; adds diagnostics and the experimental character patch.

The published release date alone does not determine whether a patch applies to a newer executable.

## Runtime follow-up: startup desync on 18 September 2026

After the initial checks, the base co-op and analytics patches were installed, with the original EXE backed up. The installed SHA-256 is `c4118519ba5c051826fe16507715de602b51be4f2cd71534ea13037a0f9bae30`; only the seven expected bytes differ from the original. The optional character patch was not applied.

A later three-player session, `09_18_2026__12_55_08`, reported a desync on frame 25. Both remote clients agreed with each other; the local client had a different global RNG state and an additional entity `6.11.0` at `(0,0)`. The game then separated the clients into different lobbies. This is a gameplay-state divergence, not evidence of ordinary latency alone.

Planetarium Chance 11.1 was loaded in that session. Its `main.lua:34–40` creates precisely entity `6.11.0` at `Vector.Zero`, checks `Exists()`, then removes it to determine a HUD offset. The check can run from initialization and rendering. [Isaac.Spawn uses the global Random() function for its seed](https://wofsauge.github.io/IsaacDocs/rep/Isaac.html#spawn), so removing the entity does not make that probe free of gameplay side effects. The matching entity is strong evidence that this mod contributes to the divergence; a repeat session is still needed to verify the diagnosis.

At the user's request, Planetarium Chance was temporarily disabled using its `disable.it` marker. Its Lua files were not changed. Re-enable it through the game's Mods menu or remove only that marker when the game is closed.

Stats+ was also loaded. Immediately before the mismatch, the log records repeated temporary additions/removals of The Sad Onion. Its Tear Cap provider performs exactly that operation to estimate the tear limit. This is a second candidate if desync persists; Stats+ was left unchanged so the next test isolates the Planetarium change. EID 5.23 was installed by this point, but no evidence links its Beast guard to this startup event.

Next validation: fully restart Isaac and repeat a new online run with the same participants and otherwise unchanged mods. Check the new session for the extra `6.11.0`, global RNG divergence, and desync. No successful post-disable multiplayer session has been verified yet.

## Runtime follow-up: crash at 16:27 on 18 September 2026

After Planetarium Chance was disabled, another online run ended with a native crash. The latest log shows a four-player start, a shared-save checksum mismatch for frame 25, the removal of two remote players during recovery at frame 28, and a crash after entering a new room at frame 388. In this desync, all four clients had identical entity-state and global RNG checksums; the differing save checksums are a separate failure from the earlier extra entity `6.11.0`.

Windows reported an access violation (`0xC0000005`) in `Lua5.3.3r.dll` at module offset `0x280A`. Local analysis with the installed x86 debugger resolved this to `lua_rawgeti+0x6A`, called from `luaL_unref+0x3E`: a `mov ecx,[ecx]` instruction attempted to read inaccessible memory. No external symbol server was used. The game's `Lua stack trace:` section is empty, and deeper stack frames lack reliable symbols. This identifies a native Lua reference/registry operation, not a specific mod responsible for it. The installed game EXE still has the expected co-op + analytics hash above.

Stats+ again added and removed The Sad Onion repeatedly for each player immediately before the startup desync. Its Tear Cap provider performs those operations. Separately, `services/PlayerService.lua` caches `EntityPlayer` references and schedules a refresh on `POST_PLAYER_INIT`; it does not explicitly invalidate the cache when an online player leaves. The log contains no Stats+ reload between the player removals and the crash. Stale player references are therefore a concrete code concern, but the available evidence does not establish them as the cause of this native exception.

For the next isolation test, Stats+ was temporarily disabled using `mods/stats-plus_2729900570/disable.it`. Planetarium Chance remains disabled. Stats+ Lua files and its stored configuration were preserved unchanged; the other existing mod enable/disable choices were left as found.

Local diagnostic copies are under `Binaries/diagnostics/crash-20260918-162728/`: the log, crash report, Windows minidump, and a copy of the Stats+ configuration. These files were kept locally. A new multiplayer run without Stats+ is still required to check whether the startup desync and room-transition crash recur. Do not consider this a verified crash fix yet.

## Runtime follow-up: peer left after a room transition

The later session `09_18_2026__13_35_59` loaded EID, Enhanced Boss Bars, Regret Pedestals, and Watch Out Laser scripts. Stats+ and Planetarium Chance scripts did not run. The host log records a transition starting at frame 32, the new room spawning its entities at frame 57, and a peer leaving the lobby at frame 60. There is no checksum mismatch, desync notification, exception, or detailed connection-failure reason around this event. The remaining players continued. The user confirmed that the affected peer's application remained open and that Stats+ and Planetarium Chance were disabled there as well.

The peer's supplied session directory matches the event: its local user identity matches the participant who left at frame 60. However, its 211-line session log contains no gameplay chronology; it is the exact prefix of their separately supplied 382-line `log1.txt`. Begin/end save snapshots differ, so the short log is not proof that the wrong session was supplied or that no run occurred. The later log warns that `options.ini` could not be written, but it does not establish why the peer left or why runtime logging is absent.

An audit of the four active Lua mods did not establish the cause of this disconnect. Watch Out Laser does have local settings that choose different indicator entities (`Laser=true` versus `false`), so comparing settings as well as file versions matters. Regret Pedestals and other entity caches have potential lifetime issues, but those code concerns do not prove a cause for an early exit without a crash.

`scripts/Collect-IsaacDiagnostics.ps1` now collects recent logs, hashes of the game and enabled mods, selected settings, and Watch Out Laser booleans for comparison across PCs. It was run and its ZIP verified locally. A matching report from the affected peer is still needed; the collector is a diagnostic aid, not a gameplay fix.

## Runtime follow-up: desync on the original EXE with mods disabled

The user subsequently restored the original EXE and disabled all individual mods. Both were verified: the executable hash matches the original hash in this report, all original patch signatures are present, and the session executed only the two stock Lua scripts.

Session `09_18_2026__14_08_07` still produced two desyncs, on frames 41112 and 41230. All clients agreed on RNG and shared-save checksums; the differences were in one player's movement velocity. Both screenshots show the affected player picking up Vibrant Bulb, and the log ties both events to trinket 100. This strongly implicates a base-game online synchronization issue around that pickup, independently of the currently removed patch on the local PC.

See the [Vibrant Bulb investigation](vanilla-vibrant-bulb-desync.md) for exact checks, evidence, and remaining uncertainty. This later observation does not establish that every earlier crash or mod-related desync had the same cause.
