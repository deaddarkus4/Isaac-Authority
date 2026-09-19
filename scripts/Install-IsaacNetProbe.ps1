[CmdletBinding()]
param(
    [string]$GameExecutable,
    [string]$PackageDirectory,
    [switch]$NoShortcut
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'IsaacPaths.ps1')
if (Get-Process -Name 'isaac-ng' -ErrorAction SilentlyContinue) { throw 'Close Isaac before installing or updating the recorder.' }
$GameExecutable = Resolve-IsaacExecutable -GameExecutable $GameExecutable -AllowBrowse
Write-Output ('Isaac found: ' + $GameExecutable)
$gameDirectory = Split-Path -Parent $GameExecutable
$expected = '3bdfc8bae0dc7e334b76009d0ad45dfbb16ee5f00c06ffbc3a0094e34d44616b'
if ((Get-FileHash -LiteralPath $GameExecutable -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
    throw 'This diagnostic build requires the original unpatched Repentance+ J460 executable.'
}
if (-not $PackageDirectory) {
    if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'IsaacNetProbe.dll')) { $PackageDirectory = $PSScriptRoot }
    else { $PackageDirectory = Join-Path (Split-Path -Parent $PSScriptRoot) 'Binaries\native-build\Release' }
}
$binaries = @('IsaacNetProbe.dll', 'IsaacNetProbeAttach.exe', 'IsaacNetProbeCli.exe')
$launchers = @('Start-IsaacDiagnostics.ps1', 'Start-IsaacDiagnostics.cmd', 'IsaacPaths.ps1')
foreach ($name in $binaries) {
    if (-not (Test-Path -LiteralPath (Join-Path $PackageDirectory $name))) { throw "Missing binary: $name" }
}
foreach ($name in $launchers) {
    if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot $name))) { throw "Missing launcher: $name" }
}
$target = Join-Path $gameDirectory 'IsaacNetProbe'
if ((Test-Path -LiteralPath $target) -and (Get-ChildItem -LiteralPath $target -Force | Select-Object -First 1) `
    -and -not (Test-Path -LiteralPath (Join-Path $target 'installation.json'))) {
    throw 'The destination folder contains files from an unknown installation; it was not overwritten.'
}
New-Item -ItemType Directory -Path $target -Force | Out-Null
foreach ($name in $binaries) { Copy-Item -LiteralPath (Join-Path $PackageDirectory $name) -Destination (Join-Path $target $name) -Force }
foreach ($name in $launchers) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $target $name) -Force }
$files = @($binaries + $launchers | ForEach-Object {
    $file = Join-Path $target $_
    [ordered]@{ name = $_; sha256 = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() }
})
$manifest = [ordered]@{ version = '0.2.1'; installedUtc = [DateTime]::UtcNow.ToString('o'); gameSha256 = $expected; files = $files }
[IO.File]::WriteAllText((Join-Path $target 'installation.json'), ($manifest | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
if (-not $NoShortcut) {
    $desktop = [Environment]::GetFolderPath('Desktop')
    $shortcutPath = Join-Path $desktop 'Isaac Diagnostics.lnk'
    $shell = New-Object -ComObject WScript.Shell
    if (Test-Path -LiteralPath $shortcutPath) {
        $existing = $shell.CreateShortcut($shortcutPath)
        if ($existing.Arguments -notlike '*Start-IsaacDiagnostics.ps1*') { throw 'An unrelated Isaac Diagnostics shortcut already exists.' }
    }
    $shortcut = $shell.CreateShortcut($shortcutPath)
    $shortcut.TargetPath = Join-Path $PSHOME 'powershell.exe'
    $shortcut.Arguments = '-NoProfile -STA -ExecutionPolicy Bypass -File "' + (Join-Path $target 'Start-IsaacDiagnostics.ps1') + '"'
    $shortcut.WorkingDirectory = $target
    $shortcut.IconLocation = $GameExecutable + ',0'
    $shortcut.Description = 'Start Isaac with engine-event recording'
    $shortcut.Save()
    Write-Output ('Shortcut: ' + $shortcutPath)
}
if ((Get-FileHash -LiteralPath $GameExecutable -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) { throw 'The game executable changed during installation.' }
Write-Output ('Installed: ' + $target)
Write-Output 'Start the game using Isaac Diagnostics. Logs: %LOCALAPPDATA%\IsaacNetProbe\logs'
