[CmdletBinding()]
param([int]$ProcessId, [int]$Seconds = 10)
$ErrorActionPreference = 'Stop'
if (-not $ProcessId) {
    $games = @(Get-Process isaac-ng -ErrorAction SilentlyContinue)
    if ($games.Count -ne 1) { throw 'Expected one running Isaac process; otherwise specify -ProcessId.' }
    $ProcessId = $games[0].Id
}
$directory = Join-Path (Split-Path -Parent $PSScriptRoot) 'Binaries\diagnostics\state-reader'
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$name = 'state-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8) + '.jsonl'
$output = Join-Path $directory $name
& python (Join-Path $PSScriptRoot 'Read-IsaacState.py') --pid $ProcessId --seconds $Seconds --output $output
if ($LASTEXITCODE -ne 0) { throw "State sampling failed; see $output" }
Write-Output $output
