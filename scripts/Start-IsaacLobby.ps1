[CmdletBinding()]
param(
    # Every participant's save, the host's first. They are only read. As in the game's own online, nobody plays from their
    # own save: the lobby's save is what all of them have in common (isaac_save.py merge, the rule of the game's builder),
    # and every participant starts from it - the host too, as an isolated instance and not the user's main game.
    [Parameter(Mandatory)][string[]]$Saves,
    # One game per participant by default; up to four, tiled 960x540 on the screen.
    [ValidateRange(1, 4)][int]$Instances = 0,
    [string]$GameExecutable, [string]$Python = 'python',
    # Build and describe the lobby's save without starting any game.
    [switch]$MergeOnly)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$tool = Join-Path $PSScriptRoot 'isaac_save.py'
if ($Instances -eq 0) { $Instances = [Math]::Min(4, [Math]::Max(2, $Saves.Count)) }
$sources = @()
foreach ($path in $Saves) {
    $resolved = (Resolve-Path -LiteralPath $path).Path
    $sources += [ordered]@{path=$resolved; sha256=(Get-FileHash -LiteralPath $resolved -Algorithm SHA256).Hash.ToLowerInvariant()}
}
$id = [guid]::NewGuid().ToString('N').Substring(0, 8)
$folder = Join-Path $root ('Binaries\game-instances\lobby-' + $id)
if (Test-Path -LiteralPath $folder) { throw 'Lobby folder already exists.' }
New-Item -ItemType Directory -Path $folder | Out-Null
$lobbySave = Join-Path $folder 'lobby.dat'
$merged = & $Python $tool merge --output $lobbySave @($sources | ForEach-Object { $_.path })
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $lobbySave)) { throw 'The saves could not be merged; no game was started.' }
# What each participant gives up for the lobby: how much of their own save is not in the shared one.
$shared = @{}
foreach ($line in $merged) { if ($line -match '^(\S+)\s+(\d+) of\s+(\d+) set') { $shared[$Matches[1]] = [int]$Matches[2] } }
$members = @()
foreach ($source in $sources) {
    $own = @{}
    foreach ($line in (& $Python $tool info $source.path)) { if ($line -match '^(\S+)\s+(\d+) of\s+(\d+) set') { $own[$Matches[1]] = [int]$Matches[2] } }
    if ($LASTEXITCODE -ne 0) { throw "Not a readable save: $($source.path)" }
    $members += [ordered]@{sha256=$source.sha256; achievements=$own['achievements']; collectibles=$own['collectibles']; bosses=$own['bosses']}
}
$manifest = [ordered]@{createdUtc=[DateTime]::UtcNow.ToString('o'); lobby=$id; lobbySave=$lobbySave
    lobbySaveSha256=(Get-FileHash -LiteralPath $lobbySave -Algorithm SHA256).Hash.ToLowerInvariant()
    shared=[ordered]@{achievements=$shared['achievements']; collectibles=$shared['collectibles']; bosses=$shared['bosses']}
    members=$members; instances=@()}
if (-not $MergeOnly) {
    Add-Type -Namespace IsaacLobby -Name Window -MemberDefinition '[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool SetWindowText(IntPtr window, string text);'
    $starter = Join-Path $PSScriptRoot 'Start-IsaacReplica.ps1'
    for ($n = 0; $n -lt $Instances; $n++) {
        $arguments = @{PersistentGameData=$lobbySave; WindowPosX=960 * ($n % 2); WindowPosY=540 * [int][Math]::Floor($n / 2)}
        if ($GameExecutable) { $arguments.GameExecutable = $GameExecutable }
        $instance = & $starter @arguments | ConvertFrom-Json
        if ($instance.sharedSaveSha256 -ne $manifest.lobbySaveSha256) { throw 'An instance started from a different file than the lobby save.' }
        # A number, not a role: which game hosts is decided by the test that attaches to them, not by the window.
        $title = "Isaac Authority - LOBBY $id #$($n + 1)"
        $named = $false
        for ($wait = 0; $wait -lt 120 -and -not $named; $wait++) {
            Start-Sleep -Milliseconds 500
            $process = Get-Process -Id $instance.launch.pid -ErrorAction SilentlyContinue
            if (-not $process) { throw "Instance $($n + 1) exited while starting; the others were left running." }
            if ($process.MainWindowHandle -ne [IntPtr]::Zero) { $named = [IsaacLobby.Window]::SetWindowText($process.MainWindowHandle, $title) }
        }
        $manifest.instances += [ordered]@{number=$n + 1; pid=$instance.launch.pid; title=$(if ($named) { $title } else { $null })
            workingDirectory=$instance.workingDirectory; saveDirectory=$instance.saveDirectory; sharedSaveSha256=$instance.sharedSaveSha256}
    }
}
[IO.File]::WriteAllText((Join-Path $folder 'lobby.json'), ($manifest | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
$manifest | ConvertTo-Json -Depth 5
