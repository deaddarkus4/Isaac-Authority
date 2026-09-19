[CmdletBinding()]
param([string]$GameExecutable, [string[]]$VerifiedSummary, [ValidateSet('Room','Npc','Input','Coop')][string]$Stage = 'Room')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'IsaacPaths.ps1')
$GameExecutable = Resolve-IsaacExecutable -GameExecutable $GameExecutable
$expected = '3bdfc8bae0dc7e334b76009d0ad45dfbb16ee5f00c06ffbc3a0094e34d44616b'
if ((Get-FileHash -LiteralPath $GameExecutable -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) { throw 'Original J460 required.' }
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'Binaries\authority-build\Release'
# Npc: the same adapter built with enemy correction; installed beside the verified Room package, not over it.
$component = 'IsaacAuthority' + $Stage
# Input: the host-side module that lets received client commands drive one controller of the host's game.
# Coop: the world adapter for every player of a co-op run, the input module that creates and drives the second one on the
# host, and its client side that captures the client's keyboard: together the closed loop.
$release = @{Room=@('0.5.0-experimental','standard-tears-rooms');Npc=@('0.6.0-experimental','standard-tears-rooms-enemies');Input=@('0.7.0-experimental','client-input-on-host');Coop=@('0.9.0-experimental','standard-tears-rooms-enemies-players-closed-loop')}[$Stage]
$target = Join-Path (Split-Path -Parent $GameExecutable) (Join-Path 'IsaacAuthority' $Stage)
$manifestPath = Join-Path $target 'installation.json'
if (Test-Path -LiteralPath $target) {
    if (-not (Test-Path -LiteralPath $manifestPath)) { throw 'Unknown destination folder.' }
    if ((Get-Content -Raw -Encoding UTF8 -LiteralPath $manifestPath | ConvertFrom-Json).component -ne $component) { throw 'Different component owns destination.' }
}
$modules = if ($Stage -eq 'Input') { [ordered]@{host=$component} } else { [ordered]@{source=($component + 'Source');replica=($component + 'Replica')} }
if ($Stage -eq 'Coop') { $modules['host'] = 'IsaacAuthorityInput'; $modules['client'] = 'IsaacAuthorityInputClient' }
# The injector refuses a file name that a game already loaded from another folder, and the Input package installs the
# same input module: this package's copy carries the package's name.
$installedBase = [ordered]@{}
foreach ($role in $modules.Keys) { $installedBase[$role] = if ($modules[$role].StartsWith($component)) { $modules[$role] } else { $component + $modules[$role].Substring('IsaacAuthority'.Length) } }
$files = @($modules.Values | ForEach-Object { Join-Path $bin ($_ + '.dll') })
$files += Join-Path $bin 'IsaacAuthorityAttach.exe'
if ($modules.Contains('host')) { $files += @('Test-IsaacInput.py','isaac_input.py' | ForEach-Object { Join-Path $PSScriptRoot $_ }) }
if ($Stage -eq 'Coop') { $files += Join-Path $PSScriptRoot 'Test-IsaacCoop.py' }
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
    if ($role) { $name = $installedBase[$role] + '-' + $hash.Substring(0, 8).ToLowerInvariant() + '.dll'; $installed[$role] = $name }
    $destination = Join-Path $target $name
    if (-not (Test-Path -LiteralPath $destination) -or (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $hash) {
        Copy-Item -LiteralPath $file -Destination $destination -Force
    }
    if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $hash) { throw 'Installed file hash mismatch.' }
    $entries += [ordered]@{name=$name;sha256=$hash.ToLowerInvariant()}
}
foreach ($stale in @(@($modules.Values) + @($installedBase.Values) | Select-Object -Unique | ForEach-Object { Get-ChildItem -LiteralPath $target -Filter ($_ + '-*.dll') } | Where-Object { $_.Name -notin $installed.Values })) {
    try { Remove-Item -LiteralPath $stale.FullName -Force -ErrorAction Stop } catch { Write-Verbose "Still loaded by a running game: $($stale.Name)" }
}
# World modules and the input module answer to different contracts.
$world = @($installed.Keys | Where-Object { $_ -notin 'host','client' } | ForEach-Object { Join-Path $target $installed[$_] })
if ($world.Count) {
    & (Join-Path $bin 'world_tests.exe') $world
    if ($LASTEXITCODE -ne 0) { throw 'Installed world module contract failed.' }
}
$inputs = @($installed.Keys | Where-Object { $_ -in 'host','client' } | ForEach-Object { Join-Path $target $installed[$_] })
if ($inputs.Count) {
    & (Join-Path $bin 'input_tests.exe') $inputs
    if ($LASTEXITCODE -ne 0) { throw 'Installed input module contract failed.' }
}
if ((Get-FileHash -LiteralPath $GameExecutable -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) { throw 'Game executable changed.' }
# Live results are recorded only from passed summaries of the stage's live test, with their hashes.
$verified = @()
foreach ($path in @($VerifiedSummary | Where-Object { $_ })) {
    $summary = Get-Content -Raw -Encoding UTF8 -LiteralPath $path | ConvertFrom-Json
    if (-not $summary.passed -or $summary.failures.Count) { throw "Not a passed live test: $path" }
    $verified += [ordered]@{summary=(Split-Path -Leaf $path);sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant();route=$summary.route;mode=$summary.mode;utc=$summary.utc}
}
$manifest = [ordered]@{component=$component;version=$release[0];scope=$release[1];installedUtc=[DateTime]::UtcNow.ToString('o');gameSha256=$expected;autoStart=$false;liveTestVerified=($verified.Count -gt 0);liveTests=$verified;modules=$installed;files=$entries}
[IO.File]::WriteAllText($manifestPath,($manifest|ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
Write-Output ('Installed: '+$target)
