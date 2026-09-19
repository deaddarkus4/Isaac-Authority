# Shared Steam-library discovery. Compatible with Windows PowerShell 5.1.

function Get-IsaacSteamRoots {
    $roots = [Collections.Generic.List[string]]::new()
    foreach ($key in @('HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam')) {
        try {
            if (-not (Test-Path -LiteralPath $key)) { continue }
            $entry = Get-ItemProperty -LiteralPath $key -ErrorAction Stop
            foreach ($name in @('SteamPath', 'InstallPath')) {
                $property = $entry.PSObject.Properties[$name]
                if ($property -and $property.Value) { $roots.Add([string]$property.Value) }
            }
        } catch { continue }
    }
    return $roots.ToArray()
}

function ConvertFrom-IsaacVdfString([string]$Value) {
    # Do not use Regex.Unescape: a drive path containing \t or \n is not text formatting.
    return $Value.Replace('\\', '\').Replace('\"', '"')
}

function Get-IsaacLibraries {
    param([string[]]$SteamRoots)
    $libraries = [Collections.Generic.List[string]]::new()
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($root in $SteamRoots) {
        if ([string]::IsNullOrWhiteSpace($root)) { continue }
        $normalized = $root.Replace('/', '\').TrimEnd('\')
        if ($seen.Add($normalized)) { $libraries.Add($normalized) }
        foreach ($relative in @('steamapps\libraryfolders.vdf', 'config\libraryfolders.vdf')) {
            $vdf = Join-Path $normalized $relative
            try {
                if (-not (Test-Path -LiteralPath $vdf -PathType Leaf)) { continue }
                $text = [IO.File]::ReadAllText($vdf)
                # Modern "path" entries and legacy numeric library entries.
                foreach ($match in [regex]::Matches($text, '"(?:path|[0-9]+)"\s*"((?:\\.|[^"\\])*)"')) {
                    $value = (ConvertFrom-IsaacVdfString $match.Groups[1].Value).Replace('/', '\').TrimEnd('\')
                    if ([IO.Path]::IsPathRooted($value) -and $seen.Add($value)) { $libraries.Add($value) }
                }
            } catch { continue }
        }
    }
    return $libraries.ToArray()
}

function Find-IsaacExecutables {
    param([string[]]$SteamRoots = (Get-IsaacSteamRoots))
    $found = [Collections.Generic.List[string]]::new()
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($library in (Get-IsaacLibraries -SteamRoots $SteamRoots)) {
        $common = Join-Path $library 'steamapps\common'
        $manifest = Join-Path $library 'steamapps\appmanifest_250900.acf'
        $candidates = [Collections.Generic.List[string]]::new()
        try {
            if (Test-Path -LiteralPath $manifest -PathType Leaf) {
                $match = [regex]::Match([IO.File]::ReadAllText($manifest), '"installdir"\s*"((?:\\.|[^"\\])*)"')
                if ($match.Success) {
                    $directory = ConvertFrom-IsaacVdfString $match.Groups[1].Value
                    if (-not [IO.Path]::IsPathRooted($directory)) {
                        $candidates.Add((Join-Path (Join-Path $common $directory) 'isaac-ng.exe'))
                    }
                }
            }
        } catch { }
        # Steam's apps index or app manifest can be stale even when the EXE is present.
        $candidates.Add((Join-Path $common 'The Binding of Isaac Rebirth\isaac-ng.exe'))
        foreach ($candidate in $candidates) {
            try {
                if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                    $resolved = (Resolve-Path -LiteralPath $candidate -ErrorAction Stop).ProviderPath
                    if ($seen.Add($resolved)) { $found.Add($resolved) }
                }
            } catch { continue }
        }
    }
    return $found.ToArray()
}

function Resolve-IsaacExecutable {
    param(
        [string]$GameExecutable,
        [string[]]$SteamRoots = (Get-IsaacSteamRoots),
        [switch]$AllowBrowse
    )
    if (-not [string]::IsNullOrWhiteSpace($GameExecutable)) {
        $selected = $GameExecutable.Trim().Trim('"')
        if (Test-Path -LiteralPath $selected -PathType Container) { $selected = Join-Path $selected 'isaac-ng.exe' }
        if (-not (Test-Path -LiteralPath $selected -PathType Leaf) -or
            [IO.Path]::GetFileName($selected) -ine 'isaac-ng.exe') {
            throw 'The selected path must point to isaac-ng.exe or its containing folder.'
        }
        return (Resolve-Path -LiteralPath $selected).ProviderPath
    }
    $candidates = @(Find-IsaacExecutables -SteamRoots $SteamRoots)
    if ($candidates.Count -eq 1) { return $candidates[0] }
    if (-not $AllowBrowse) {
        if ($candidates.Count -gt 1) { throw 'More than one Isaac installation was found. Specify -GameExecutable.' }
        throw 'Isaac was not found in the registered Steam libraries. Specify -GameExecutable.'
    }
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    try {
        $dialog.Title = if ($candidates.Count -gt 1) { 'Choose the Isaac installation to use' } else { 'Locate isaac-ng.exe' }
        $dialog.Filter = 'Isaac executable (isaac-ng.exe)|isaac-ng.exe'
        $dialog.FileName = 'isaac-ng.exe'
        $dialog.CheckFileExists = $true
        $dialog.Multiselect = $false
        if ($candidates.Count) { $dialog.InitialDirectory = Split-Path -Parent $candidates[0] }
        if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { throw 'No game executable selected.' }
        return Resolve-IsaacExecutable -GameExecutable $dialog.FileName -SteamRoots @()
    } finally { $dialog.Dispose() }
}
