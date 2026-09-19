[CmdletBinding()]
param([string]$GameExecutable)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'IsaacPaths.ps1')
$GameExecutable = Resolve-IsaacExecutable -GameExecutable $GameExecutable
$expected = '3bdfc8bae0dc7e334b76009d0ad45dfbb16ee5f00c06ffbc3a0094e34d44616b'
if ((Get-FileHash -LiteralPath $GameExecutable -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
    throw 'The network test requires original J460.'
}
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'Binaries\authority-build\Release'
$target = Join-Path (Split-Path -Parent $GameExecutable) 'IsaacAuthority\Network'
$files = @(
    (Join-Path $bin 'IsaacAuthorityNetwork.dll'),
    (Join-Path $bin 'IsaacAuthorityAttach.exe'),
    (Join-Path $PSScriptRoot 'Test-IsaacNetworkState.py'),
    (Join-Path $PSScriptRoot 'Read-IsaacState.py')
)
foreach ($file in $files) { if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing file: $file" } }
if (Test-Path -LiteralPath $target) {
    $manifestPath = Join-Path $target 'installation.json'
    if (-not (Test-Path -LiteralPath $manifestPath)) { throw 'Unknown installation folder; inspect before overwriting.' }
    $previous = Get-Content -Raw -Encoding UTF8 -LiteralPath $manifestPath | ConvertFrom-Json
    if ($previous.component -ne 'IsaacAuthorityNetwork') { throw 'Different component owns the destination.' }
}
foreach ($game in @(Get-Process -Name isaac-ng -ErrorAction SilentlyContinue)) {
    if (@($game.Modules | Where-Object ModuleName -eq 'IsaacAuthorityNetwork.dll').Count) {
        throw 'Restart Isaac before updating an already loaded network DLL.'
    }
}
New-Item -ItemType Directory -Path $target -Force | Out-Null
$installed = @()
foreach ($file in $files) {
    $name = Split-Path -Leaf $file
    $destination = Join-Path $target $name
    Copy-Item -LiteralPath $file -Destination $destination -Force
    $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
    if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $hash) { throw "Installed file differs: $name" }
    $installed += [ordered]@{name=$name; sha256=$hash.ToLowerInvariant()}
}
& (Join-Path $bin 'game_adapter_tests.exe') (Join-Path $target 'IsaacAuthorityNetwork.dll')
if ($LASTEXITCODE -ne 0) { throw 'Installed network DLL contract failed.' }
if ((Get-FileHash -LiteralPath $GameExecutable -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
    throw 'Game executable changed during installation.'
}
$manifest = [ordered]@{
    component='IsaacAuthorityNetwork'; version='0.2.0-experimental'; installedUtc=[DateTime]::UtcNow.ToString('o')
    gameSha256=$expected; autoStart=$false; transport='loopback'; liveTestVerified=$false; files=$installed
}
[IO.File]::WriteAllText((Join-Path $target 'installation.json'), ($manifest | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
Write-Output ('Installed network experiment: ' + $target)
