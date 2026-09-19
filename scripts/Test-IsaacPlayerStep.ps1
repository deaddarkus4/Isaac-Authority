[CmdletBinding()]
param([int]$ProcessId, [switch]$CorrectOnce)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'Binaries\authority-build\Release'
$attach = Join-Path $bin 'IsaacAuthorityAttach.exe'
$dll = Join-Path $bin 'IsaacAuthorityGame.dll'
if (-not $ProcessId) {
    $games = @(Get-Process isaac-ng -ErrorAction SilentlyContinue)
    if ($games.Count -ne 1) { throw 'Expected one running Isaac process; otherwise specify -ProcessId.' }
    $ProcessId = $games[0].Id
}
$directory = Join-Path $env:LOCALAPPDATA 'IsaacAuthority\logs'
$before = @()
if (Test-Path -LiteralPath $directory) { $before = @(Get-ChildItem -LiteralPath $directory -File | Select-Object -ExpandProperty FullName) }
& $attach start $ProcessId $dll
if ($LASTEXITCODE -ne 0) { throw 'Adapter refused to start. Requires original J460 and one player in a local run.' }
try {
    Start-Sleep -Seconds 2
    if ($CorrectOnce) {
        & $attach correct $ProcessId $dll
        if ($LASTEXITCODE -ne 0) { throw 'Correction was not queued. The game may be paused or player-update hook has no observations.' }
    }
    Start-Sleep -Seconds 2
} finally {
    & $attach stop $ProcessId $dll
    if ($LASTEXITCODE -ne 0) { throw 'Adapter stop failed; inspect the result before running another test.' }
}
$files = @(Get-ChildItem -LiteralPath $directory -File -Filter "player-step-$ProcessId-*.jsonl" |
    Where-Object { $_.FullName -notin $before })
if ($files.Count -ne 1) { throw 'Expected exactly one new adapter report.' }
$records = @(Get-Content -LiteralPath $files[0].FullName | ForEach-Object { $_ | ConvertFrom-Json })
$steps = @($records | Where-Object type -eq 'player_step')
$applied = @($steps | Where-Object action -eq 1)
$refused = @($steps | Where-Object action -eq 2)
$threads = @($steps | Select-Object -ExpandProperty thread -Unique)
if ($steps.Count -lt 2 -or $threads.Count -ne 1) { throw 'Not enough player updates on one thread; keep the local game running, not paused.' }
if ($records[-1].vtableRestored -ne $true -or $records[-1].dropped -ne 0) { throw 'Incomplete hook restoration or dropped observations.' }
if ($CorrectOnce) {
    if ($applied.Count -ne 1 -or $refused.Count -ne 0) { throw 'Correction refused; stand still near the center of the start room in a solo run.' }
    $event = $applied[0]
    if ([Math]::Abs(($event.after[0] - $event.before[0]) - 8) -gt 0.0001 -or
        $event.after[1] -ne $event.before[1] -or $event.after[2] -ne $event.before[2] -or $event.after[3] -ne $event.before[3]) {
        throw 'Correction report does not match the requested position-only change.'
    }
    if (@($steps | Where-Object { $_.sequence -gt $event.sequence -and $_.action -eq 3 }).Count -eq 0) {
        throw 'No following player update observed after the correction.'
    }
} elseif ($applied.Count -ne 0) { throw 'Unexpected coordinate change in observation-only mode.' }
$output = Join-Path $root 'Binaries\diagnostics\player-step'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$copy = Join-Path $output $files[0].Name
if (Test-Path -LiteralPath $copy) { throw 'Report destination already exists.' }
Copy-Item -LiteralPath $files[0].FullName -Destination $copy
[pscustomobject]@{ Report = $copy; Updates = $steps.Count; Thread = $threads[0]; Corrections = $applied.Count; VtableRestored = $true } | ConvertTo-Json
