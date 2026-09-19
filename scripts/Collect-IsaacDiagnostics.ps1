[CmdletBinding()]
param(
    [string]$GameDirectory,
    [string]$OutputDirectory = [Environment]::GetFolderPath('Desktop')
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'IsaacPaths.ps1')

function Get-Fingerprint([string]$Path) {
    $file = Get-Item -LiteralPath $Path
    return [ordered]@{
        name = $file.Name
        bytes = $file.Length
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

$exePath = Resolve-IsaacExecutable -GameExecutable $GameDirectory -AllowBrowse
$GameDirectory = Split-Path -Parent $exePath

$saveDirectory = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\Binding of Isaac Repentance+'
$savePathFile = Join-Path $GameDirectory 'savedatapath.txt'
if (Test-Path -LiteralPath $savePathFile) {
    $match = [regex]::Match([IO.File]::ReadAllText($savePathFile), '(?m)^Save Data Path:\s*(.+)$')
    if ($match.Success) { $saveDirectory = $match.Groups[1].Value.Trim() }
}
if (-not (Test-Path -LiteralPath $saveDirectory -PathType Container)) {
    throw 'Repentance+ save/log directory was not found. Start the game once first.'
}

$stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$captureDirectory = Join-Path ([IO.Path]::GetTempPath()) ('IsaacDiagnostics-' + $stamp)
New-Item -ItemType Directory -Path $captureDirectory | Out-Null
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).ProviderPath
$errorsFound = [Collections.Generic.List[string]]::new()
$utf8 = [Text.UTF8Encoding]::new($false)

function Copy-Log([string]$Source, [string]$RelativeDestination) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) { return }
    try {
        $target = Join-Path $captureDirectory $RelativeDestination
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        Copy-Item -LiteralPath $Source -Destination $target
    } catch {
        $errorsFound.Add("Could not copy ${RelativeDestination}: $($_.Exception.Message)")
    }
}

# Snapshot the logs before hashing the installed files.
Copy-Log (Join-Path $saveDirectory 'log.txt') 'logs\current-log.txt'
$probeLogs = Join-Path $env:LOCALAPPDATA 'IsaacNetProbe\logs'
if (Test-Path -LiteralPath $probeLogs) {
    $recentProbeLogs = @(Get-ChildItem -LiteralPath $probeLogs -File -Filter '*.jsonl' |
        Sort-Object LastWriteTime -Descending | Select-Object -First 3)
    foreach ($probeLog in $recentProbeLogs) {
        Copy-Log $probeLog.FullName (Join-Path 'logs\native-probe' $probeLog.Name)
        $profileName = $probeLog.BaseName + '-profile.json'
        Copy-Log (Join-Path $probeLogs $profileName) (Join-Path 'logs\native-probe' $profileName)
    }
}
foreach ($category in @('sessions', 'desyncs')) {
    $sourceDirectory = Join-Path $saveDirectory ('online_logs\' + $category)
    if (-not (Test-Path -LiteralPath $sourceDirectory)) { continue }
    $recent = @(Get-ChildItem -LiteralPath $sourceDirectory -Directory |
        Sort-Object LastWriteTime -Descending | Select-Object -First 3)
    foreach ($session in $recent) {
        foreach ($log in (Get-ChildItem -LiteralPath $session.FullName -File -Filter '*.txt')) {
            Copy-Log $log.FullName (Join-Path ('logs\' + $category + '\' + $session.Name) $log.Name)
        }
    }
}

$report = [ordered]@{
    formatVersion = 1
    collectedUtc = [DateTime]::UtcNow.ToString('o')
    gameRunning = (@(Get-Process -Name 'isaac-ng' -ErrorAction SilentlyContinue).Count -gt 0)
    gameFiles = @()
    gameVersionString = $null
    steamBuild = $null
    options = [ordered]@{}
    watchOutLaserSettings = @()
    mods = @()
}
foreach ($name in @('isaac-ng.exe', 'Lua5.3.3r.dll', 'Lua5.3.3f.dll', 'steam_api.dll', 'EOSSDK-Win32-Shipping.dll')) {
    $path = Join-Path $GameDirectory $name
    if (Test-Path -LiteralPath $path) { $report.gameFiles += Get-Fingerprint $path }
}
$exeText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($exePath))
$versionMatch = [regex]::Match($exeText, 'Binding of Isaac: Repentance\+ v[0-9A-Za-z.]+')
if ($versionMatch.Success) { $report.gameVersionString = $versionMatch.Value }
$exeText = $null
$steamApps = Split-Path -Parent (Split-Path -Parent $GameDirectory)
$appManifest = Join-Path $steamApps 'appmanifest_250900.acf'
if (Test-Path -LiteralPath $appManifest) {
    $buildMatch = [regex]::Match([IO.File]::ReadAllText($appManifest), '"buildid"\s*"(\d+)"')
    if ($buildMatch.Success) { $report.steamBuild = $buildMatch.Groups[1].Value }
}

$optionsPath = Join-Path $saveDirectory 'options.ini'
if (Test-Path -LiteralPath $optionsPath) {
    $report.options.readOnly = (Get-Item -LiteralPath $optionsPath).IsReadOnly
    foreach ($line in [IO.File]::ReadAllLines($optionsPath)) {
        if ($line -match '^(EnableMods|EnableDebugConsole|OnlineInputDelay|EosCrossplay|VSync|Log)=(.*)$') {
            $report.options[$Matches[1]] = $Matches[2]
        }
    }
}

# This mod's local booleans select different gameplay entities for its indicators.
# Capture only these settings, rather than copying arbitrary mod save data.
foreach ($slot in 1..3) {
    $laserSettingsPath = Join-Path $GameDirectory ('data\watch_out_laser!\save' + $slot + '.dat')
    if (-not (Test-Path -LiteralPath $laserSettingsPath)) { continue }
    try {
        $settings = [IO.File]::ReadAllText($laserSettingsPath) | ConvertFrom-Json
        $values = [ordered]@{}
        foreach ($property in $settings.PSObject.Properties) {
            if ($property.Value -is [bool]) { $values[$property.Name] = $property.Value }
        }
        $report.watchOutLaserSettings += [ordered]@{ slot = $slot; settings = $values }
    } catch {
        $errorsFound.Add("Could not read Watch Out Laser settings for slot ${slot}: $($_.Exception.Message)")
    }
}

$modsPath = Join-Path $GameDirectory 'mods'
if (Test-Path -LiteralPath $modsPath) {
    foreach ($mod in (Get-ChildItem -LiteralPath $modsPath -Directory | Sort-Object Name)) {
        $disabled = Test-Path -LiteralPath (Join-Path $mod.FullName 'disable.it')
        $entry = [ordered]@{ directory = $mod.Name; disabled = $disabled; files = @() }
        if (-not $disabled) {
            Write-Host ('Checking mod: ' + $mod.Name)
            $files = [Collections.Generic.List[object]]::new()
            foreach ($file in (Get-ChildItem -LiteralPath $mod.FullName -Recurse -File | Sort-Object FullName)) {
                if ($file.Extension -in @('.bak', '.tmp')) { continue }
                try {
                    $fingerprint = Get-Fingerprint $file.FullName
                    $fingerprint.name = $file.FullName.Substring($mod.FullName.Length + 1).Replace('\', '/')
                    $files.Add($fingerprint)
                } catch {
                    $relative = $file.FullName.Substring($mod.FullName.Length + 1)
                    $errorsFound.Add("Could not hash $($mod.Name)/${relative}: $($_.Exception.Message)")
                }
            }
            $entry.files = @($files.ToArray())
        }
        $report.mods += $entry
    }
}
$report.errors = @($errorsFound.ToArray())
[IO.File]::WriteAllText((Join-Path $captureDirectory 'report.json'), ($report | ConvertTo-Json -Depth 10), $utf8)
$archive = Join-Path $OutputDirectory ('IsaacDiagnostics-' + $stamp + '.zip')
Compress-Archive -Path (Join-Path $captureDirectory '*') -DestinationPath $archive -CompressionLevel Optimal
Write-Host ''
Write-Host ('Created: ' + $archive)
Write-Host 'Send this ZIP for comparison. Game files and settings were not modified.'
Write-Output $archive
