$ErrorActionPreference = 'Stop'
$scripts = Split-Path -Parent $PSScriptRoot
. (Join-Path $scripts 'IsaacPaths.ps1')
$root = Join-Path (Split-Path -Parent $scripts) ('Binaries\path-tests\' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root -Force | Out-Null
$utf8 = [Text.UTF8Encoding]::new($false)
$script:checks = 0
function Check([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:checks++
}
function WriteText([string]$Path, [string]$Text) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force | Out-Null
    [IO.File]::WriteAllText($Path, $Text, $utf8)
}
function VdfPath([string]$Path) { return $Path.Replace('\', '\\') }
function MakeExe([string]$Library, [string]$Directory = 'The Binding of Isaac Rebirth') {
    $exe = Join-Path $Library ('steamapps\common\' + $Directory + '\isaac-ng.exe')
    WriteText $exe 'fixture only, not executable'
    return $exe
}

$steam = Join-Path $root 'Steam on first drive'
$library = Join-Path $root ('Other library [test] & ' + [char]0x0418)
New-Item -ItemType Directory -Path $steam -Force | Out-Null
$expected = MakeExe $library 'Renamed Isaac Folder'
WriteText (Join-Path $library 'steamapps\appmanifest_250900.acf') '"AppState" { "appid" "250900" "installdir" "Renamed Isaac Folder" }'
WriteText (Join-Path $steam 'steamapps\libraryfolders.vdf') ('"libraryfolders" { "0" { "path" "' + (VdfPath $steam) + '" } "1" { "path" "' + (VdfPath $library) + '" "apps" { "250900" "12345" } } }')
$found = @(Find-IsaacExecutables -SteamRoots @($steam))
Check ($found.Count -eq 1 -and $found[0] -eq $expected) 'Modern secondary library / manifest directory lookup failed'
Check ((Resolve-IsaacExecutable -SteamRoots @($steam)) -eq $expected) 'Automatic resolution failed'
Check ((Resolve-IsaacExecutable -GameExecutable (Split-Path -Parent $expected) -SteamRoots @()) -eq $expected) 'Explicit directory lookup failed'
Check ((Resolve-IsaacExecutable -GameExecutable ('"' + $expected + '"') -SteamRoots @()) -eq $expected) 'Quoted explicit file lookup failed'
Check (@(Find-IsaacExecutables -SteamRoots @($steam, $steam.ToUpperInvariant())).Count -eq 1) 'Duplicate library results were returned'

$legacySteam = Join-Path $root 'Legacy Steam'
$legacyLibrary = Join-Path $root 'Legacy library'
$legacyExe = MakeExe $legacyLibrary
WriteText (Join-Path $legacySteam 'config\libraryfolders.vdf') ('"LibraryFolders" { "1" "' + (VdfPath $legacyLibrary) + '" }')
Check ((Resolve-IsaacExecutable -SteamRoots @($legacySteam)) -eq $legacyExe) 'Legacy config/libraryfolders lookup failed'
$fallbackExe = MakeExe (Join-Path $root 'Missing manifest')
Check ((Resolve-IsaacExecutable -SteamRoots @((Join-Path $root 'Missing manifest'))) -eq $fallbackExe) 'Missing-manifest standard folder fallback failed'
Check (@(Find-IsaacExecutables -SteamRoots @((Join-Path $root 'Missing path'))).Count -eq 0) 'Missing libraries should not produce a path'

$failed = $false
try { Resolve-IsaacExecutable -SteamRoots @($steam, $legacySteam) | Out-Null } catch { $failed = $_.Exception.Message -like 'More than one*' }
Check $failed 'Ambiguous installations were silently selected'
$failed = $false
try { Resolve-IsaacExecutable -GameExecutable (Join-Path $root 'absent.exe') -SteamRoots @($steam) | Out-Null } catch { $failed = $true }
Check $failed 'Invalid explicit path unexpectedly fell back to another game'

# Installer integration uses an isolated copy, never launches it or changes a real shortcut.
$actual = @(Find-IsaacExecutables)
if ($actual.Count -eq 1) {
    $integrationLibrary = Join-Path $root 'Installer integration library'
    $copy = Join-Path $integrationLibrary 'steamapps\common\The Binding of Isaac Rebirth\isaac-ng.exe'
    New-Item -ItemType Directory -Path (Split-Path -Parent $copy) -Force | Out-Null
    Copy-Item -LiteralPath $actual[0] -Destination $copy
    $originalHash = (Get-FileHash -LiteralPath $copy -Algorithm SHA256).Hash
    $detected = Resolve-IsaacExecutable -SteamRoots @($integrationLibrary)
    & (Join-Path $scripts 'Install-IsaacNetProbe.ps1') -GameExecutable $detected -NoShortcut
    $installed = Join-Path (Split-Path -Parent $copy) 'IsaacNetProbe'
    Check (Test-Path -LiteralPath (Join-Path $installed 'IsaacPaths.ps1')) 'Shared resolver was not installed'
    Check ((Get-FileHash -LiteralPath $copy -Algorithm SHA256).Hash -eq $originalHash) 'Installer modified the copied game EXE'
    $manifest = [IO.File]::ReadAllText((Join-Path $installed 'installation.json')) | ConvertFrom-Json
    Check ($manifest.version -eq '0.2.1') 'Installer package version was not updated'
}
Write-Output ('PASS: ' + $script:checks + ' path-discovery / installer checks. Fixtures: ' + $root)
