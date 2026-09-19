# Vanilla online desync while picking up Vibrant Bulb

Observed on 18 September 2026 in Steam Repentance+ `v1.9.7.17.J460`, build `22878971`, session `09_18_2026__14_08_07`.

## Verified local installation

- The installed EXE SHA-256 is `3bdfc8bae0dc7e334b76009d0ad45dfbb16ee5f00c06ffbc3a0094e34d44616b`, exactly matching the original executable examined before any patching.
- All seven original patch signatures occur once; none of the patched signatures occurs. Co-op, analytics, and optional character patches are absent.
- Every installed mod has a `disable.it` marker. The session ran only `resources/scripts/enums.lua` and `resources/scripts/main.lua`; no mod Lua entrypoint was executed.
- The global options still contain `EnableMods=1` and `EnableDebugConsole=1`. Individual mods are disabled. These settings were recorded, not changed during this investigation.
- These checks concern the local installation. The other participants' complete binaries and runtime configuration were not independently verified in this pass.

The desync therefore occurred on the original local executable with no mod Lua running. It cannot be attributed to the currently removed IsaacOnlineModded byte changes on this PC.

## Two consecutive events

| Event | Frame | Players remaining before drop | Only entity difference reported |
| --- | --- | --- | --- |
| First | 41112 | 4 | Isaac (`1.0.0`) post-update Y velocity: `1.8918772935867310` on three clients versus `1.9434483051300049` on the affected client's simulation |
| Second | 41230 | 3 | Samson (`1.0.6`) post-update X velocity: `1.3025702238082886` on two clients versus `1.3461362123489380` on the affected client's simulation |

In each event, all participants agreed on the global RNG checksum. The recovery log also reports the same shared-save checksum (`4106280689`) for all remaining participants. This differs from the earlier mod-related investigation, where an extra entity/global RNG divergence or a shared-save mismatch was observed.

The game selected `SYNC_PLAN_DROP` for the client whose entity state differed. No native crash occurred in this session, and the local game later shut down normally.

## Strongly implicated trigger: Vibrant Bulb (trinket 100)

Both desync screenshots show the affected character holding the yellow bulb above their head with a `VIBRANT BULB` pickup banner. After the first player is removed, the bulb is left for another player, who picks it up and experiences the second desync.

The session log supports this sequence:

- Line 3233: the room contains pickup `5.350.100`.
- Line 3235: the first player's previous trinket, Liberty Cap (`5.350.32`), appears on the floor.
- Lines 3236-3237: first desync, frame 41112.
- Lines 3328-3330: the pending item queue is flushed while the player is removed; `Adding trinket 100 (Vibrant Bulb)` and a new `5.350.100` pickup are logged.
- Lines 3343-3344: second desync, frame 41230.
- Lines 3419-3421: the same item-queue/`Adding trinket 100`/drop sequence occurs for Samson.

The item-addition messages are delayed until queue flushing during player removal. Their printed position after the mismatch is not evidence that the pickup began after the desync; the screenshots show the pickup in progress at both mismatches.

This is strong evidence for a vanilla online synchronization problem triggered by Vibrant Bulb pickup/stat changes. The exact internal defect (for example, when a movement-stat cache is updated on different peers) has not been established by reverse engineering or a controlled replay.

A separate player published a [Vibrant Bulb desync demonstration for v1.9.7.17 on 9 May 2026](https://www.youtube.com/watch?v=XJqnYI8fYaM). This is supporting user-reported evidence, not an official developer diagnosis.

## Practical next step

For ordinary online play, avoid picking up or passing Vibrant Bulb between players for now. For a controlled reproduction, use unmodified copies on all PCs and compare picking up the bulb with and without a fully charged active item, including a trinket swap. Record each client's game version and logs. Do not treat bypassing the desync check as a fix: the simulations already disagree about player motion.

Automatic report submission failed with `Couldn't resolve host name` after both detections. That failure concerns the report upload; it appears after the gameplay mismatch and does not explain it.

The original local artifacts are preserved under `Binaries/diagnostics/vanilla-desync-20260918-173424/`. No game files, settings, or saves were modified during this investigation, and no report was sent externally.
