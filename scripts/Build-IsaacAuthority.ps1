[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root 'native\IsaacAuthority'
$build = Join-Path $root 'Binaries\authority-build'
& cmake -S $source -B $build -G 'Visual Studio 17 2022' -A Win32
if ($LASTEXITCODE -ne 0) { throw 'Authority configuration failed.' }
& cmake --build $build --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'Authority build failed.' }
& ctest --test-dir $build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Authority tests failed.' }
Write-Output ('Built standalone lab: ' + (Join-Path $build 'Release\IsaacAuthorityLab.exe'))
