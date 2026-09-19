[CmdletBinding()]
param([string]$GameExecutable, [switch]$VerifyOnly, [string[]]$GameArguments)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'IsaacPaths.ps1')
$GameExecutable = Resolve-IsaacExecutable -GameExecutable $GameExecutable
$gameDirectory = Split-Path -Parent $GameExecutable
$root = Split-Path -Parent $PSScriptRoot
$launcher = Join-Path $root 'Binaries\authority-build\Release\IsaacAuthorityLaunch.exe'
if (-not (Test-Path -LiteralPath $launcher)) { throw 'Build IsaacAuthority first.' }
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
$options = @'
[Options]
SteamCloud=0
PauseOnFocusLost=0
EnableMods=0
EnableDebugConsole=1
TryImportSave=0
Fullscreen=0
WindowWidth=960
WindowHeight=540
WindowPosX=960
WindowPosY=0
MusicVolume=0
SFXVolume=0
EnableEpicOverlay=0
AcceptedPublicBeta_v1.9.7.17=1
AcceptedDataCollectionDisclaimer=1
'@
[IO.File]::WriteAllText((Join-Path $save 'options.ini'), $options, [Text.UTF8Encoding]::new($false))
$launchArguments = @($GameExecutable, $working, $leaf)
if ($VerifyOnly) { $launchArguments += '--verify-only' }
elseif ($GameArguments) { $launchArguments += @('--') + $GameArguments }
$result = & $launcher @launchArguments
if ($LASTEXITCODE -ne 0) { throw 'Isolated launcher failed; no running game was terminated.' }
$launch = $result | ConvertFrom-Json
$manifest = [ordered]@{createdUtc=[DateTime]::UtcNow.ToString('o'); role='replica-test'; gameArguments=@($GameArguments); workingDirectory=$working; saveDirectory=$save; gameExecutable=$GameExecutable; launch=$launch}
[IO.File]::WriteAllText((Join-Path $working 'instance.json'), ($manifest | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
$manifest | ConvertTo-Json -Depth 5

