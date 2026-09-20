[CmdletBinding()]
param([string]$Name = ('IsaacAuthority-' + (Get-Date -Format 'yyyyMMdd-HHmm')),
    # The commit to name in the instructions: the published one, whose hash differs from the working repository's (see Publish-IsaacAuthority.py).
    [string]$Commit)
# Puts together what a player needs: the module, the version.dll that makes the game load it, the installer with two files
# to double-click, and a page of instructions. Output: Binaries\release\<Name>\ and the same as a zip.
# Build first (Build-IsaacAuthority.ps1) with no test game running.
# The instructions are Russian and lie in a file of their own: Windows PowerShell reads a script without a byte order mark
# in the machine's own code page, which breaks such text inside a script.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$target = Join-Path $root ('Binaries\release\' + $Name)
if (Test-Path -LiteralPath $target) { throw "Already there: $target" }
New-Item -ItemType Directory -Force -Path $target | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'Binaries\authority-build\Release\IsaacAuthorityNative.dll') -Destination $target
Copy-Item -LiteralPath (Join-Path $root 'Binaries\authority-build\proxy\Release\version.dll') -Destination $target
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Install-IsaacAuthority.ps1') -Destination $target
$ascii = [Text.Encoding]::ASCII
[IO.File]::WriteAllText((Join-Path $target 'INSTALL.cmd'), "@echo off`r`npowershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0Install-IsaacAuthority.ps1`"`r`npause`r`n", $ascii)
[IO.File]::WriteAllText((Join-Path $target 'UNINSTALL.cmd'), "@echo off`r`npowershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0Install-IsaacAuthority.ps1`" -Uninstall`r`npause`r`n", $ascii)
$commit = $Commit; if (-not $commit) { $commit = (& git -C $root rev-parse --short HEAD 2>$null); if (& git -C $root status --porcelain -- native scripts) { $commit += '+' } }
$readme = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'Install-IsaacAuthority.README.txt'), [Text.Encoding]::UTF8).Replace('{NAME}', $Name).Replace('{COMMIT}', $commit).Replace("`r`n", "`n").Replace("`n", "`r`n")
[IO.File]::WriteAllText((Join-Path $target 'README.txt'), $readme, (New-Object Text.UTF8Encoding($true)))
$zip = $target + '.zip'; if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip }
Compress-Archive -Path (Join-Path $target '*') -DestinationPath $zip
Get-ChildItem -LiteralPath $target | Select-Object Name, Length
"zip: $zip"
