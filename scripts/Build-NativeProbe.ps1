[CmdletBinding()]
param([string]$GameExecutable)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root 'native\IsaacNetProbe'
$build = Join-Path $root 'Binaries\native-build'
$configure = @('-S', $source, '-B', $build, '-G', 'Visual Studio 17 2022', '-A', 'Win32')
# Explicitly clear the optional test path when omitted, including in an existing cache.
$configure += ('-DISAAC_TEST_EXECUTABLE=' + $GameExecutable)
& cmake @configure
if ($LASTEXITCODE -ne 0) { throw 'Native probe configuration failed.' }
& cmake --build $build --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'Native probe build failed.' }
& ctest --test-dir $build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Native probe tests failed.' }
Write-Output ('Built probe: ' + (Join-Path $build 'Release\IsaacNetProbe.dll'))
