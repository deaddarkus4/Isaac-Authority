[CmdletBinding()]
param(
    # One save per game, the host's first; only read. Each isolated instance starts from a copy of its own file, as a lobby
    # member of the game's own online would: the game itself builds the shared save when the match starts.
    [Parameter(Mandatory)][string[]]$Saves,
    [string]$GameExecutable)
# Isolated instances for the game's own online on its built-in localhost service (IsaacAuthorityLocalhost.dll): nothing
# reaches Steam or another player. The instances are named NET A, NET B, ... and listed in
# Binaries\game-instances\native-pair.json for Test-IsaacNative.py.
$ErrorActionPreference = 'Stop'
if ($Saves.Count -lt 2 -or $Saves.Count -gt 4) { throw 'Two to four saves, one per game.' }
$root = Split-Path -Parent $PSScriptRoot
$release = Join-Path $root 'Binaries\authority-build\Release'
$module = Join-Path $release 'IsaacAuthorityLocalhost.dll'
if (-not (Test-Path -LiteralPath $module)) { throw 'Build IsaacAuthority first.' }
Add-Type -Namespace IsaacNative -Name Window -MemberDefinition '[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool SetWindowText(IntPtr window, string text);'
$games = @()
for ($n = 0; $n -lt $Saves.Count; $n++) {
    $arguments = @{PersistentGameData=(Resolve-Path -LiteralPath $Saves[$n]).Path; WindowPosX=960 * ($n % 2); WindowPosY=540 * [int][Math]::Floor($n / 2)}
    if ($GameExecutable) { $arguments.GameExecutable = $GameExecutable }
    $instance = & (Join-Path $PSScriptRoot 'Start-IsaacReplica.ps1') @arguments | ConvertFrom-Json
    $title = 'Isaac Authority - NET ' + [char]([int][char]'A' + $n)
    for ($wait = 0; $wait -lt 120; $wait++) {
        Start-Sleep -Milliseconds 500
        $process = Get-Process -Id $instance.launch.pid -ErrorAction SilentlyContinue
        if (-not $process) { throw "Instance $title exited while starting; the others were left running." }
        if ($process.MainWindowHandle -ne [IntPtr]::Zero -and [IsaacNative.Window]::SetWindowText($process.MainWindowHandle, $title)) { break }
    }
    $games += [ordered]@{title=$title; pid=$instance.launch.pid; saveDirectory=$instance.saveDirectory}
}
# The local user takes its localhost identity from the game's window, so the window has to exist first.
Start-Sleep -Seconds 6
foreach ($game in $games) {
    $attached = & (Join-Path $release 'IsaacAuthorityAttach.exe') localhost $game.pid $module | ConvertFrom-Json
    if ($attached.result -ne 0) { throw "The localhost service could not be set in $($game.title): $($attached.result). Do not enter its Online menu." }
}
[IO.File]::WriteAllText((Join-Path $root 'Binaries\game-instances\native-pair.json'), (ConvertTo-Json @($games) -Depth 3), [Text.UTF8Encoding]::new($false))
ConvertTo-Json @($games) -Depth 3
