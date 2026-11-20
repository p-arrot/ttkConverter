[CmdletBinding()]
param(
    [string] $Work = $(if ($env:TTK_BUILD_ROOT) { $env:TTK_BUILD_ROOT } else {
        Join-Path $env:LOCALAPPDATA 'ttkconverter-build'
    }),
    [string] $OutputRoot,
    [ValidateSet('x86', 'x64')] [string[]] $Architecture = @('x86', 'x64')
)

$ErrorActionPreference = 'Stop'
$Work = [IO.Path]::GetFullPath($Work)
if (!$OutputRoot) { $OutputRoot = Join-Path $Work 'pdf-opendoc' }
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$cache = Join-Path $Work 'downloads/opendoc'
$manifestPath = Join-Path $PSScriptRoot 'opendoc-assets.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Path $cache -Force | Out-Null

function Get-VerifiedAsset($Asset) {
    $existing = Join-Path $cache $Asset.name
    if ((Test-Path -LiteralPath $existing -PathType Leaf) -and
        (Get-FileHash -LiteralPath $existing -Algorithm SHA256).Hash -eq $Asset.sha256) {
        return $existing
    }

    $pending = "$existing.$([Guid]::NewGuid().ToString('N')).part"
    try {
        Write-Host "Fetching $($Asset.name)"
        $curlArguments = @('--proto', '=https', '--ssl-revoke-best-effort', '-fLsS',
                           '--retry', '2', '--connect-timeout', '15', '--max-time', '120',
                           $Asset.url, '--output', $pending)
        & curl.exe @curlArguments
        if ($LASTEXITCODE -ne 0) { throw "Download failed for $($Asset.name). Restore the verified asset in $cache." }
        if ((Get-FileHash -LiteralPath $pending -Algorithm SHA256).Hash -ne $Asset.sha256) {
            throw "SHA-256 mismatch for $($Asset.name), expected $($Asset.sha256). Restore the verified asset in $cache."
        }
        Move-Item -LiteralPath $pending -Destination $existing -Force
        return $existing
    } finally {
        if (Test-Path -LiteralPath $pending -PathType Leaf) {
            Remove-Item -LiteralPath $pending -Force
        }
    }
}

$modules = foreach ($asset in $manifest.opendoc.files) { Get-VerifiedAsset $asset }
$notices = foreach ($asset in @($manifest.opendoc.notices) + @($manifest.spidermonkey.notices)) {
    Get-VerifiedAsset $asset
}
$worker = Join-Path $PSScriptRoot 'opendoc-shell.mjs'
$sourceNotice = @"
OpenDoc WebAssembly modules are distributed under Apache-2.0. Corresponding source:
$($manifest.opendoc.source)/tree/$($manifest.opendoc.commit)
The OpenDoc NOTICE and bundled font license files are included in this directory.

This package includes the unmodified Mozilla SpiderMonkey JavaScript shell from Firefox
$($manifest.spidermonkey.version). The Mozilla Public License 2.0 and the upstream third-party notices
are included in this directory. Obtain the complete corresponding Mozilla source here:
$($manifest.spidermonkey.source)
SHA-256: $($manifest.spidermonkey.sourceSha256)

Check opendoc-assets.json for the exact archives, asset URLs and SHA-256 digests.
"@

Add-Type -AssemblyName System.IO.Compression.FileSystem
foreach ($target in $Architecture) {
    $archive = Get-VerifiedAsset $manifest.spidermonkey.archives.$target
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        foreach ($required in 'js.exe', 'mozglue.dll', 'nss3.dll', 'ucrtbase.dll') {
            if (!$zip.GetEntry($required)) { throw "Missing $required in $archive" }
        }
        $stage = Join-Path $OutputRoot "$target/engines"
        $licenseRoot = Join-Path $stage 'licenses'
        New-Item -ItemType Directory -Path $licenseRoot -Force | Out-Null
        foreach ($entry in $zip.Entries) {
            if ($entry.FullName -notmatch '^(js\.exe|[a-z0-9_-]+\.dll)$') {
                throw "Unexpected Mozilla archive entry $($entry.FullName)"
            }
            $destination = Join-Path $stage $entry.FullName
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destination, $true)
        }
    } finally {
        $zip.Dispose()
    }
    Copy-Item -LiteralPath $modules -Destination $stage -Force
    Copy-Item -LiteralPath $worker -Destination (Join-Path $stage 'convert.mjs') -Force
    Copy-Item -LiteralPath (@($notices) + $manifestPath) -Destination $licenseRoot -Force
    Set-Content -LiteralPath (Join-Path $licenseRoot 'Source-code.txt') -Value $sourceNotice -Encoding ASCII

    $payload = (Get-ChildItem -LiteralPath $stage -File -Recurse | Measure-Object Length -Sum).Sum
    $details = $manifest.opendoc.commit, $manifest.spidermonkey.version, $stage, ($payload / 1MB)
    Write-Host ('OpenDoc {0} with SpiderMonkey {1} is staged at {2} ({3:N1} MiB).' -f $details)
}
