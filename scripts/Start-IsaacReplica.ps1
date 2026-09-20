[CmdletBinding()]
param([string]$GameExecutable, [switch]$VerifyOnly, [string[]]$GameArguments,
    # The host's save file: the new instance starts with a copy of it as its slot 1. Level generation reads the unlocks
    # of the save, so two games agree on a seed's level only with the same save; the game's own online shares one too.
    [string]$PersistentGameData,
    # The host's run state, the file behind the menu's Continue: with it the new instance enters the host's run as it
    # was when the file was written (run start, a new floor, or Exit game) instead of starting its own from a typed seed.
    [string]$GameState, [int]$WindowPosX = 960, [int]$WindowPosY = 0)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'IsaacPaths.ps1')
$GameExecutable = Resolve-IsaacExecutable -GameExecutable $GameExecutable
$gameDirectory = Split-Path -Parent $GameExecutable
$root = Split-Path -Parent $PSScriptRoot
$launcher = Join-Path $root 'Binaries\authority-build\Release\IsaacAuthorityLaunch.exe'
if (-not (Test-Path -LiteralPath $launcher)) { throw 'Build IsaacAuthority first.' }
if ($PersistentGameData) {
    $sharedSave = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $PersistentGameData))
    if ($sharedSave.Length -lt 16 -or [Text.Encoding]::ASCII.GetString($sharedSave, 0, 16) -ne 'ISAACNGSAVE09R  ') { throw 'Not a Repentance+ persistent game data file.' }
}
if ($GameState) {
    if (-not $PersistentGameData) { throw 'The game loads a run state only beside the persistent game data it was saved with.' }
    $runState = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $GameState))
    if ($runState.Length -lt 16 -or [Text.Encoding]::ASCII.GetString($runState, 0, 11) -ne 'ISAACNG_GSR') { throw 'Not a Repentance+ game state file.' }
}
$id = [guid]::NewGuid().ToString('N').Substring(0, 8)
$leaf = 'IsaacAuthority-' + $id
$working = Join-Path $root ('Binaries\game-instances\replica-' + $id)
$save = Join-Path ([Environment]::GetFolderPath('UserProfile')) ('Documents\My Games\' + $leaf)
if ((Test-Path -LiteralPath $working) -or (Test-Path -LiteralPath $save)) { throw 'Isolated test folder already exists.' }
New-Item -ItemType Directory -Path $working,$save | Out-Null
# --localhost_match writes its log to localhost\log_<time> under the save folder and does not create the folder.
if ($GameArguments -contains '--localhost_match') { New-Item -ItemType Directory -Path (Join-Path $save 'localhost') | Out-Null }
New-Item -ItemType Junction -Path (Join-Path $working 'resources') -Target (Join-Path $gameDirectory 'resources') | Out-Null
New-Item -ItemType Directory -Path (Join-Path $working 'mods'),(Join-Path $working 'data') | Out-Null
# OnlineInputDelay (1..5 frames, the game's default is 3) is how far ahead the game's own online schedules input, that is how
# long a frame can do without waiting for a neighbour's input. 5: the native module reads the own player from the keyboard
# directly, so the delay is not felt, and measured frame hitches come from that waiting.
$options = @"
[Options]
SteamCloud=0
PauseOnFocusLost=0
EnableMods=0
EnableDebugConsole=1
TryImportSave=0
Fullscreen=0
WindowWidth=960
WindowHeight=540
WindowPosX=$WindowPosX
WindowPosY=$WindowPosY
MusicVolume=0
SFXVolume=0
EnableEpicOverlay=0
OnlineInputDelay=5
AcceptedPublicBeta_v1.9.7.17=1
AcceptedDataCollectionDisclaimer=1
"@
[IO.File]::WriteAllText((Join-Path $save 'options.ini'), $options, [Text.UTF8Encoding]::new($false))
# The source file is only read. The copy lives in the isolated profile, which never syncs to Steam Cloud.
$sharedSaveSha256 = $null; $runStateSha256 = $null
if ($GameState) {
    [IO.File]::WriteAllBytes((Join-Path $save 'gamestate1.dat'), $runState)
    $runStateSha256 = (Get-FileHash -LiteralPath (Join-Path $save 'gamestate1.dat') -Algorithm SHA256).Hash.ToLowerInvariant()
}
if ($PersistentGameData) {
    [IO.File]::WriteAllBytes((Join-Path $save 'persistentgamedata1.dat'), $sharedSave)
    $sharedSaveSha256 = (Get-FileHash -LiteralPath (Join-Path $save 'persistentgamedata1.dat') -Algorithm SHA256).Hash.ToLowerInvariant()
}
$launchArguments = @($GameExecutable, $working, $leaf)
if ($VerifyOnly) { $launchArguments += '--verify-only' }
elseif ($GameArguments) { $launchArguments += @('--') + $GameArguments }
$result = & $launcher @launchArguments
if ($LASTEXITCODE -ne 0) { throw 'Isolated launcher failed; no running game was terminated.' }
$launch = $result | ConvertFrom-Json
$manifest = [ordered]@{createdUtc=[DateTime]::UtcNow.ToString('o'); role='replica-test'; sharedSaveSha256=$sharedSaveSha256; runStateSha256=$runStateSha256; gameArguments=@($GameArguments); workingDirectory=$working; saveDirectory=$save; gameExecutable=$GameExecutable; launch=$launch}
[IO.File]::WriteAllText((Join-Path $working 'instance.json'), ($manifest | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
$manifest | ConvertTo-Json -Depth 5

