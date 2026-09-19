[CmdletBinding()]
param(
    [string]$GameExecutable,
    [string]$GhidraDirectory,
    [switch]$Reanalyze,
    [switch]$FixLoggerAnalysis
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'IsaacPaths.ps1')
$GameExecutable = Resolve-IsaacExecutable -GameExecutable $GameExecutable -AllowBrowse
$root = Split-Path -Parent $PSScriptRoot
if (-not $GhidraDirectory) { $GhidraDirectory = Join-Path $root 'Binaries\tools\ghidra_12.1.3_PUBLIC' }
$headless = Join-Path $GhidraDirectory 'support\analyzeHeadless.bat'
if (-not (Test-Path -LiteralPath $headless)) { throw 'Install the portable Ghidra 12.1.3 release or specify -GhidraDirectory.' }
$expected = '3bdfc8bae0dc7e334b76009d0ad45dfbb16ee5f00c06ffbc3a0094e34d44616b'
if ((Get-FileHash -LiteralPath $GameExecutable -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
    throw 'This analysis profile only supports the verified original J460 executable.'
}
$projectDirectory = Join-Path $root 'Binaries\research\ghidra-projects'
$sampleDirectory = Join-Path $root 'Binaries\research\samples'
$outputDirectory = Join-Path $root 'Binaries\research\j460-ghidra'
foreach ($directory in @($projectDirectory, $sampleDirectory, $outputDirectory)) {
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
}
$sample = Join-Path $sampleDirectory 'isaac-ng-J460.exe'
if (-not (Test-Path -LiteralPath $sample)) { Copy-Item -LiteralPath $GameExecutable -Destination $sample }
if ((Get-FileHash -LiteralPath $sample -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
    throw 'Existing analysis sample has a different hash; it was not overwritten.'
}
$arguments = @($projectDirectory, 'IsaacJ460', '-max-cpu', '4', '-analysisTimeoutPerFile', '300')
if (Test-Path -LiteralPath (Join-Path $projectDirectory 'IsaacJ460.gpr')) {
    $arguments += @('-process', 'isaac-ng-J460.exe')
    if (-not $Reanalyze) { $arguments += '-noanalysis' }
} else {
    $arguments += @('-import', $sample)
}
$arguments += @('-scriptPath', (Join-Path $root 'reverse\ghidra'))
if ($FixLoggerAnalysis) { $arguments += @('-postScript', 'FixJ460Logger.java') }
$arguments += @('-postScript', 'ExportIsaacNetwork.java', $outputDirectory,
    '-log', (Join-Path $outputDirectory 'headless.log'),
    '-scriptlog', (Join-Path $outputDirectory 'script.log'))
$completion = Join-Path $outputDirectory 'export-complete.txt'
if (Test-Path -LiteralPath $completion) { Remove-Item -LiteralPath $completion }
& $headless @arguments
if ($LASTEXITCODE -ne 0) { throw "Ghidra exited with code $LASTEXITCODE" }
$index = Join-Path $outputDirectory 'anchors.tsv'
if (-not (Test-Path -LiteralPath $index) -or -not (Test-Path -LiteralPath $completion)) {
    throw 'Ghidra did not finish the export; inspect headless.log and script.log.'
}
Write-Output "Analysis output: $outputDirectory"
