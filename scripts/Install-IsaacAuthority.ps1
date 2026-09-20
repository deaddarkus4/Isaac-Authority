[CmdletBinding()]
param(
    # Take the module out of the game instead of putting it in.
    [switch]$Uninstall,
    # The folder of isaac-ng.exe; found through Steam when not given.
    [string]$GameFolder,
    # Rules of the module to leave out (for example: lead,doors) - the same for every player of a match.
    [string[]]$Without = @())
# Puts the authority module into The Binding of Isaac: Repentance+ or takes it out. Installed is two files beside
# isaac-ng.exe: version.dll, which the game loads by itself, and IsaacAuthorityNative.dll, which version.dll loads. After
# that nothing is started or stopped by hand: the module follows the game's own log, and in an online match it switches
# itself on only when EVERY player of the match runs it - any other match stays the game's own, untouched.
# No file of the game is changed; removing the two files is the whole uninstall.
$ErrorActionPreference = 'Stop'
$rules = [ordered]@{follow=1; behaviour=2; clear=4; taken=8; grid=16; fire=32; projectiles=64; tears=128; drops=256; counters=512; doors=1024; traps=2048; bombs=4096; hurt=8192; slots=16384; pets=32768; lead=65536; look=131072}
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$files = 'version.dll', 'IsaacAuthorityNative.dll'

function Find-Game {
    $steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction SilentlyContinue).SteamPath
    if (-not $steam) { $steam = (Get-ItemProperty -Path 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam' -Name InstallPath -ErrorAction SilentlyContinue).InstallPath }
    if (-not $steam) { throw 'Steam is not found. Give the folder of isaac-ng.exe: -GameFolder "..."' }
    $libraries = @($steam)
    $list = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path -LiteralPath $list) { $libraries += [regex]::Matches((Get-Content -LiteralPath $list -Raw), '"path"\s+"([^"]+)"') | ForEach-Object { $_.Groups[1].Value -replace '\\\\', '\' } }
    foreach ($library in $libraries | Select-Object -Unique) {
        $folder = Join-Path $library 'steamapps\common\The Binding of Isaac Rebirth'
        if (Test-Path -LiteralPath (Join-Path $folder 'isaac-ng.exe')) { return $folder }
    }
    throw 'The game is not found in the Steam libraries. Give the folder of isaac-ng.exe: -GameFolder "..."'
}

# version.dll is a name other tools use too: only a file that names this module's entry is ours to replace or remove.
function Test-Ours([string]$path) {
    if ((Split-Path -Leaf $path) -ne 'version.dll') { return $true }
    return [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($path)).Contains('IsaacAuthorityNativeAuto')
}

if (-not $GameFolder) { $GameFolder = Find-Game }
if (-not (Test-Path -LiteralPath (Join-Path $GameFolder 'isaac-ng.exe'))) { throw "No isaac-ng.exe in $GameFolder" }
$running = @(Get-Process -Name 'isaac-ng' -ErrorAction SilentlyContinue | Where-Object { $_.Path -and (Split-Path -Parent $_.Path) -eq (Resolve-Path -LiteralPath $GameFolder).Path })
if ($running.Count) { throw 'Close the game first.' }
$settings = Join-Path $env:LOCALAPPDATA 'IsaacAuthority'
try {
    foreach ($file in $files) {
        $target = Join-Path $GameFolder $file
        if ((Test-Path -LiteralPath $target) -and -not (Test-Ours $target)) { throw "$target belongs to something else (another mod loader?). Nothing was changed." }
    }
    if ($Uninstall) {
        foreach ($file in $files) { $target = Join-Path $GameFolder $file; if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Force } }
        Remove-Item -LiteralPath (Join-Path $settings 'native.cfg') -ErrorAction SilentlyContinue
        Write-Host "Removed from $GameFolder. The game is as Steam installed it."
        return
    }
    $unknown = @($Without | Where-Object { $_ -and -not $rules.Contains($_) }); if ($unknown) { throw "Unknown rule: $($unknown -join ', '). Known: $($rules.Keys -join ', ')" }
    foreach ($file in $files) { if (-not (Test-Path -LiteralPath (Join-Path $here $file))) { throw "$file must lie beside this script." } }
    foreach ($file in $files) { Copy-Item -LiteralPath (Join-Path $here $file) -Destination (Join-Path $GameFolder $file) -Force }
    New-Item -ItemType Directory -Force -Path $settings | Out-Null
    $mask = 0x3FFFF; foreach ($name in $Without) { if ($name) { $mask = $mask -band (-bnot $rules[$name]) } }
    if ($mask -ne 0x3FFFF) { [IO.File]::WriteAllText((Join-Path $settings 'native.cfg'), ('auto {0:x}' -f $mask), [Text.Encoding]::ASCII) }
    else { Remove-Item -LiteralPath (Join-Path $settings 'native.cfg') -ErrorAction SilentlyContinue }
    Write-Host "Installed into $GameFolder."
    Write-Host 'Play as usual. The window title ends with "Authority <version>: loaded" from the main menu on, and with "ON, host" or "ON, guest" in an online match of players who ALL have the same version installed.'
} catch [UnauthorizedAccessException] {
    throw "No right to write into $GameFolder - run this as administrator."
}
