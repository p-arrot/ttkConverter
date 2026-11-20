[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Root,
    [Parameter(Mandatory)] [ValidateSet('x86','x64')] [string] $Architecture,
    [string] $ReadObj = 'llvm-readobj.exe',
    [switch] $Legacy
)

$ErrorActionPreference = 'Stop'
$Root = [IO.Path]::GetFullPath($Root)
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
    if ($Legacy -and ($data -match "Symbol: ($afterWin7)(\s|$)" -or
                    $data -match 'Name: (api-ms-win-|ext-ms-win-|ucrtbase\.dll)')) {
        throw "Potential post-Win7 static import: $($entry.FullName)"
    }
    if ($Legacy -and $data -match 'MajorSubsystemVersion: (\d+)\s+MinorSubsystemVersion: (\d+)') {
        if ([int] $Matches[1] -gt 6 -or ([int] $Matches[1] -eq 6 -and [int] $Matches[2] -gt 1)) {
            throw "Subsystem later than Win7: $($entry.FullName)"
        }
    }
    $relative = $entry.FullName.Substring($Root.Length).TrimStart('\', '/')
    $names = $inspection | Select-String '^\s*Name: (\S+\.dll)\s*$' | ForEach-Object { $_.Matches[0].Groups[1].Value }
    $report.Add(($relative + ' ' + ($names -join ', ')))
}
$report | Set-Content -LiteralPath (Join-Path $Root 'pe-imports.txt') -Encoding UTF8
Write-Output "Checked $($entries.Count) $Architecture PE files; legacy import rules: $Legacy"
