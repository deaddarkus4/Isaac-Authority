[CmdletBinding()]
param([string]$GameExecutable)
$ErrorActionPreference = 'Stop'
try {
    . (Join-Path $PSScriptRoot 'IsaacPaths.ps1')
    if (-not $GameExecutable) {
        $adjacent = Join-Path (Split-Path -Parent $PSScriptRoot) 'isaac-ng.exe'
        if (Test-Path -LiteralPath $adjacent -PathType Leaf) { $GameExecutable = $adjacent }
    }
    $GameExecutable = Resolve-IsaacExecutable -GameExecutable $GameExecutable -AllowBrowse
    $attach = Join-Path $PSScriptRoot 'IsaacNetProbeAttach.exe'
    $dll = Join-Path $PSScriptRoot 'IsaacNetProbe.dll'
    foreach ($file in @($attach, $dll)) {
        if (-not (Test-Path -LiteralPath $file)) { throw 'The diagnostic package is incomplete. Run its installer first.' }
    }
    $directory = Join-Path $env:LOCALAPPDATA 'IsaacNetProbe'
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
    $stdout = Join-Path $directory ('launcher-' + $stamp + '.log')
    $stderr = Join-Path $directory ('launcher-' + $stamp + '.err')
    $arguments = @('--wait', ('"' + $GameExecutable + '"'), ('"' + $dll + '"'))
    $helper = Start-Process -FilePath $attach -ArgumentList $arguments -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    if (-not (Get-Process -Name 'isaac-ng' -ErrorAction SilentlyContinue)) {
        Start-Process -FilePath 'steam://rungameid/250900' -WindowStyle Hidden
    }
    Write-Host 'Starting Isaac and waiting for the recorder...'
    while (-not $helper.WaitForExit(500)) { }
    $helper.Refresh()
    if ($helper.ExitCode -ne 0) {
        $message = if (Test-Path -LiteralPath $stderr) { [IO.File]::ReadAllText($stderr) } else { 'Unknown loader error' }
        throw "Recorder did not start: $message"
    }
    Write-Host 'RECORDING READY. You can play now.'
    $last = Join-Path $directory 'last-session.txt'
    if (Test-Path -LiteralPath $last) { Write-Host ('Log: ' + [IO.File]::ReadAllText($last).Trim()) }
} catch {
    Write-Host $_.Exception.Message -ForegroundColor Red
    Write-Host 'The game executable was not changed. Press Enter to close.'
    Read-Host | Out-Null
    exit 1
}
