[CmdletBinding()]
param([string]$GameExecutable, [string[]]$VerifiedSummary)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'IsaacPaths.ps1')
$GameExecutable = Resolve-IsaacExecutable -GameExecutable $GameExecutable
$expected = '3bdfc8bae0dc7e334b76009d0ad45dfbb16ee5f00c06ffbc3a0094e34d44616b'
if ((Get-FileHash -LiteralPath $GameExecutable -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) { throw 'Original J460 required.' }
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'Binaries\authority-build\Release'
$target = Join-Path (Split-Path -Parent $GameExecutable) 'IsaacAuthority\Room'
$manifestPath = Join-Path $target 'installation.json'
if (Test-Path -LiteralPath $target) {
    if (-not (Test-Path -LiteralPath $manifestPath)) { throw 'Unknown destination folder.' }
    if ((Get-Content -Raw -Encoding UTF8 -LiteralPath $manifestPath | ConvertFrom-Json).component -ne 'IsaacAuthorityRoom') { throw 'Different component owns destination.' }
}
$modules = [ordered]@{source='IsaacAuthorityRoomSource';replica='IsaacAuthorityRoomReplica'}
$files = @($modules.Values | ForEach-Object { Join-Path $bin ($_ + '.dll') })
$files += Join-Path $bin 'IsaacAuthorityAttach.exe'
$files += @('Test-IsaacRoom.py','isaac_level.py','Test-IsaacWorld.py','isaac_world.py','Test-IsaacGamePair.py','Read-IsaacState.py' | ForEach-Object { Join-Path $PSScriptRoot $_ })
foreach ($file in $files) { if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing file: $file" } }
New-Item -ItemType Directory -Path $target -Force | Out-Null
$entries = @(); $installed = [ordered]@{}
foreach ($file in $files) {
    $name = Split-Path -Leaf $file
    $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
    # A loaded module stays locked and pinned until its game exits, and the injector identifies modules by file name.
    # Content-named copies let a new build load beside an old, inactive one without restarting the games.
    $role = $modules.Keys | Where-Object { $modules[$_] + '.dll' -eq $name }
    if ($role) { $name = $modules[$role] + '-' + $hash.Substring(0, 8).ToLowerInvariant() + '.dll'; $installed[$role] = $name }
    $destination = Join-Path $target $name
    if (-not (Test-Path -LiteralPath $destination) -or (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $hash) {
        Copy-Item -LiteralPath $file -Destination $destination -Force
    }
    if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $hash) { throw 'Installed file hash mismatch.' }
    $entries += [ordered]@{name=$name;sha256=$hash.ToLowerInvariant()}
}
foreach ($stale in @(Get-ChildItem -LiteralPath $target -Filter 'IsaacAuthorityRoom*.dll' | Where-Object { $_.Name -notin $installed.Values })) {
    try { Remove-Item -LiteralPath $stale.FullName -Force -ErrorAction Stop } catch { Write-Verbose "Still loaded by a running game: $($stale.Name)" }
}
& (Join-Path $bin 'world_tests.exe') (Join-Path $target $installed.source) (Join-Path $target $installed.replica)
if ($LASTEXITCODE -ne 0) { throw 'Installed room module contract failed.' }
if ((Get-FileHash -LiteralPath $GameExecutable -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) { throw 'Game executable changed.' }
# Live results are recorded only from passed summaries of Test-IsaacRoom.py, with their hashes.
$verified = @()
foreach ($path in @($VerifiedSummary | Where-Object { $_ })) {
    $summary = Get-Content -Raw -Encoding UTF8 -LiteralPath $path | ConvertFrom-Json
    if (-not $summary.passed -or $summary.failures.Count) { throw "Not a passed live test: $path" }
    $verified += [ordered]@{summary=(Split-Path -Leaf $path);sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant();route=$summary.route;utc=$summary.utc}
}
$manifest = [ordered]@{component='IsaacAuthorityRoom';version='0.5.0-experimental';scope='standard-tears-rooms';installedUtc=[DateTime]::UtcNow.ToString('o');gameSha256=$expected;autoStart=$false;liveTestVerified=($verified.Count -gt 0);liveTests=$verified;modules=$installed;files=$entries}
[IO.File]::WriteAllText($manifestPath,($manifest|ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
Write-Output ('Installed: '+$target)
