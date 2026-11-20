[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Root,
    [Parameter(Mandatory)] [ValidateSet('x86','x64')] [string] $Architecture,
    [string] $ReadObj = 'llvm-readobj.exe',
    [switch] $Legacy,
    [string] $AppLocalRuntimeDirectory
)

$ErrorActionPreference = 'Stop'
$Root = [IO.Path]::GetFullPath($Root)
if ($AppLocalRuntimeDirectory) {
    $AppLocalRuntimeDirectory = [IO.Path]::GetFullPath($AppLocalRuntimeDirectory)
    $prefix = $Root.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (!$AppLocalRuntimeDirectory.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or
        !(Test-Path -LiteralPath $AppLocalRuntimeDirectory -PathType Container)) {
        throw 'The app-local runtime directory must be inside the inspected package'
    }
}
$machine = if ($Architecture -eq 'x86') { 'I386' } else { 'AMD64' }
$afterWin7 = @(
    'CreateFile2', 'GetCurrentThreadStackLimits', 'GetDpiForWindow', 'GetOverlappedResultEx',
    'GetSystemTimePreciseAsFileTime', 'IsWow64Process2', 'SetProcessDpiAwarenessContext',
    'SetThreadDescription', 'WaitOnAddress', 'WakeByAddressAll', 'WakeByAddressSingle'
) -join '|'
$report = [System.Collections.Generic.List[string]]::new()
$entries = Get-ChildItem -LiteralPath $Root -Recurse -File | Where-Object { $_.Extension -in '.exe','.dll' }
foreach ($entry in $entries) {
    $inspection = & $ReadObj --file-headers --coff-imports $entry.FullName
    if ($LASTEXITCODE) { throw "Cannot inspect $($entry.FullName)" }
    $data = $inspection -join "`n"
    if ($data -notmatch "Machine: IMAGE_FILE_MACHINE_$machine\b") {
        throw "Unexpected PE architecture: $($entry.FullName)"
    }
    $appLocal = $AppLocalRuntimeDirectory -and $entry.DirectoryName -eq $AppLocalRuntimeDirectory
    $names = @($inspection | Select-String '^\s*Name: (\S+\.dll)\s*$' | ForEach-Object { $_.Matches[0].Groups[1].Value })
    if ($Legacy -and $data -match "Symbol: ($afterWin7)(\s|$)") {
        throw "Potential post-Win7 static import: $($entry.FullName)"
    }
    if ($Legacy) {
        foreach ($name in $names) {
            if ($name -notmatch '^(api-ms-win-|ext-ms-win-|ucrtbase\.dll$)') { continue }
            if ($appLocal -and (Test-Path -LiteralPath (Join-Path $AppLocalRuntimeDirectory $name) -PathType Leaf)) { continue }
            # The vendor's app-local UCRT also references the system's original API set contracts.
            if ($appLocal -and $entry.Name -eq 'ucrtbase.dll' -and $name -match '^api-ms-win-core-[a-z]+-l1-1-0\.dll$') { continue }
            throw "Potential post-Win7 static import $name in $($entry.FullName)"
        }
    }
    if ($Legacy -and $data -match 'MajorSubsystemVersion: (\d+)\s+MinorSubsystemVersion: (\d+)') {
        $major = [int] $Matches[1]
        $minor = [int] $Matches[2]
        # Allow the exact vendor API forwarders, not a newer executable subsystem.
        $forwarder = $appLocal -and $entry.Name -match '^api-ms-win-(crt|core)-.*\.dll$' -and !$names
        if (!$forwarder -and ($major -gt 6 -or ($major -eq 6 -and $minor -gt 1))) {
            throw "Subsystem later than Win7: $($entry.FullName)"
        }
    }
    $relative = $entry.FullName.Substring($Root.Length).TrimStart('\', '/')
    $report.Add(($relative + ' ' + ($names -join ', ')))
}
$report | Set-Content -LiteralPath (Join-Path $Root 'pe-imports.txt') -Encoding UTF8
Write-Output "Checked $($entries.Count) $Architecture PE files; legacy import rules: $Legacy"
