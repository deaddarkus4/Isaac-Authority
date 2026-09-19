[CmdletBinding()]
param([string]$GameExecutable)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'IsaacPaths.ps1')
$GameExecutable = Resolve-IsaacExecutable -GameExecutable $GameExecutable
$expected = '3bdfc8bae0dc7e334b76009d0ad45dfbb16ee5f00c06ffbc3a0094e34d44616b'
if ((Get-FileHash -LiteralPath $GameExecutable -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) { throw 'Original J460 required.' }
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'Binaries\authority-build\Release'
$target = Join-Path (Split-Path -Parent $GameExecutable) 'IsaacAuthority\World'
$manifestPath = Join-Path $target 'installation.json'
if (Test-Path -LiteralPath $target) {
    if (-not (Test-Path -LiteralPath $manifestPath)) { throw 'Unknown destination folder.' }
    if ((Get-Content -Raw -Encoding UTF8 -LiteralPath $manifestPath | ConvertFrom-Json).component -ne 'IsaacAuthorityWorld') { throw 'Different component owns destination.' }
}
foreach ($game in @(Get-Process -Name isaac-ng -ErrorAction SilentlyContinue)) {
    if (@($game.Modules | Where-Object { $_.ModuleName -in @('IsaacAuthorityWorldSource.dll','IsaacAuthorityWorldReplica.dll') }).Count) {
        throw 'Restart the games before updating their loaded world modules.'
    }
}
$files = @('IsaacAuthorityWorldSource.dll','IsaacAuthorityWorldReplica.dll','IsaacAuthorityAttach.exe' | ForEach-Object { Join-Path $bin $_ })
$files += @('Test-IsaacWorld.py','isaac_world.py','Test-IsaacGamePair.py','Read-IsaacState.py' | ForEach-Object { Join-Path $PSScriptRoot $_ })
foreach ($file in $files) { if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing file: $file" } }
New-Item -ItemType Directory -Path $target -Force | Out-Null
$entries = @()
foreach ($file in $files) {
    $name = Split-Path -Leaf $file; $destination = Join-Path $target $name
    Copy-Item -LiteralPath $file -Destination $destination -Force
    $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
    if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $hash) { throw 'Installed file hash mismatch.' }
    $entries += [ordered]@{name=$name;sha256=$hash.ToLowerInvariant()}
}
& (Join-Path $bin 'world_tests.exe') (Join-Path $target 'IsaacAuthorityWorldSource.dll') (Join-Path $target 'IsaacAuthorityWorldReplica.dll')
if ($LASTEXITCODE -ne 0) { throw 'Installed world module contract failed.' }
if ((Get-FileHash -LiteralPath $GameExecutable -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) { throw 'Game executable changed.' }
$manifest = [ordered]@{component='IsaacAuthorityWorld';version='0.4.0-experimental';scope='standard-tears';installedUtc=[DateTime]::UtcNow.ToString('o');gameSha256=$expected;autoStart=$false;liveTestVerified=$false;files=$entries}
[IO.File]::WriteAllText($manifestPath,($manifest|ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
Write-Output ('Installed: '+$target)
