[CmdletBinding()]
param(
    # Rules of the module to leave out, as in Test-IsaacNative.py --without (for example: lead,doors).
    [string[]]$Without = @(),
    # Stop the module in the running game instead of starting it.
    [switch]$Stop,
    # Skip the question at the start.
    [switch]$Yes)
# Starts the native authority module in the game that Steam runs - for a test between two real clients.
#
# Both players run this, each on their own machine, after the online match has started (a Friend Match by invitation: a
# game with changed network code must never meet ordinary players). Nothing but the game's own log is needed: it names the
# local player's device, who made the lobby (the host: the authority over the world) and the Steam ids of the other
# players, to whom the module then sends through Steam itself - no address, no open port.
# The module writes nothing to a save file, but the match is played on the real ones, and the game writes to them what it
# always writes after an online match.
$ErrorActionPreference = 'Stop'
$rules = [ordered]@{follow=1; behaviour=2; clear=4; taken=8; grid=16; fire=32; projectiles=64; tears=128; drops=256; counters=512; doors=1024; traps=2048; bombs=4096; hurt=8192; slots=16384; pets=32768; lead=65536}
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$attach = Join-Path $here 'IsaacAuthorityAttach.exe'; $module = Join-Path $here 'IsaacAuthorityNative.dll'
if (-not (Test-Path -LiteralPath $attach) -or -not (Test-Path -LiteralPath $module)) {   # run from the repository: the build folder
    $release = Join-Path (Split-Path -Parent $here) 'Binaries\authority-build\Release'
    $attach = Join-Path $release 'IsaacAuthorityAttach.exe'; $module = Join-Path $release 'IsaacAuthorityNative.dll'
}
if (-not (Test-Path -LiteralPath $attach) -or -not (Test-Path -LiteralPath $module)) { throw 'IsaacAuthorityAttach.exe and IsaacAuthorityNative.dll must lie beside this script.' }
$games = @(Get-Process -Name 'isaac-ng' -ErrorAction SilentlyContinue)
if ($games.Count -ne 1) { throw "Exactly one running game is expected, found $($games.Count)." }
$game = $games[0]
$folder = Join-Path $env:LOCALAPPDATA 'IsaacAuthority'; New-Item -ItemType Directory -Force -Path $folder | Out-Null
if ($Stop) {
    & $attach 'native-stop' $game.Id $module
    return
}
$log = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\Binding of Isaac Repentance+\log.txt'
if (-not (Test-Path -LiteralPath $log)) { throw "The game's log is not where it is expected: $log" }
# The game keeps the log open: read it shared.
$stream = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite'); try { $text = (New-Object IO.StreamReader($stream)).ReadToEnd() } finally { $stream.Dispose() }
$start = $text.LastIndexOf('Start Networked')
if ($start -lt 0) { throw 'No online match has started in this game yet: start the match first, then run this.' }
$match = $text.Substring($start)
$local = $match.IndexOf('Adding local player')
$controller = if ($local -ge 0) { [regex]::Match($match.Substring($local, [Math]::Min(600, $match.Length - $local)), 'Setting controller ID to (\d+), \(Prev: 0\)') } else { $null }
if (-not $controller -or -not $controller.Success) { throw "The log does not name the local player's device." }
$remote = @([regex]::Matches($match, 'Adding remote player, UserID = (\d{17}), device ID = (\d+)'))
$peers = @($remote | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique)
if ($peers.Count -lt 1 -or $peers.Count -gt 3) { throw "One to three other players are expected in the match, the log names $($peers.Count)." }
# The host is whoever has the lowest device number: every client numbers the players alike, so all agree on it without
# having to tell from the log who made the lobby (under Steam the log does not say).
$own = [int]$controller.Groups[1].Value
$hosting = $own -lt (($remote | ForEach-Object { [int]$_.Groups[2].Value }) | Measure-Object -Minimum).Minimum
$unknown = @($Without | Where-Object { $_ -and -not $rules.Contains($_) }); if ($unknown) { throw "Unknown rule: $($unknown -join ', '). Known: $($rules.Keys -join ', ')" }
$mask = 0x1FFFF; foreach ($name in $Without) { if ($name) { $mask = $mask -band (-bnot $rules[$name]) } }
Write-Host ("This game: device {0}, {1}; other players in the match: {2}." -f $controller.Groups[1].Value, $(if ($hosting) { 'HOST (its world is everybody''s)' } else { 'guest' }), $peers.Count)
if (-not $Yes) {
    Write-Host 'Only in a Friend Match by invitation, and only if every player of the match starts this module: among three or more, a player without it falls out of step and is thrown out by the game.'
    if ((Read-Host 'Type yes to start') -ne 'yes') { return }
}
$configuration = '{0} {1} {2:x} steam {3}' -f $controller.Groups[1].Value, $(if ($hosting) { 'host' } else { 'guest' }), $mask, (($peers | ForEach-Object { "steampeer=$_" }) -join ' ')
[IO.File]::WriteAllText((Join-Path $folder "native-$($game.Id).cfg"), $configuration, [Text.Encoding]::ASCII)
$status = Join-Path $folder "native-$($game.Id).status"; Remove-Item -LiteralPath $status -ErrorAction SilentlyContinue
function Start-Module { $answer = & $attach 'native' $game.Id $module | Out-String; try { ($answer | ConvertFrom-Json).result } catch { $answer.Trim() } }
$result = Start-Module
if ($result -eq 1247) {   # it already runs, from an earlier match: the device numbers and the neighbours are other now, so start it anew
    & $attach 'native-stop' $game.Id $module | Out-Null
    Start-Sleep -Milliseconds 500
    $result = Start-Module
}
if ($result -ne 0) { throw "The module did not start: $result" }
Write-Host 'The module runs. Until the neighbour is heard it changes nothing; then the own player answers the keyboard at once.'
Write-Host 'Its state every two seconds (Ctrl+C stops watching, not the module):'
while (-not $game.HasExited) {
    Start-Sleep -Seconds 2
    if (Test-Path -LiteralPath $status) { Write-Host ((Get-Date).ToString('HH:mm:ss') + '  ' + ([IO.File]::ReadAllText($status)).Trim()) }
    $game.Refresh()
}
