[CmdletBinding()]
param(
    [string] $Work = $(if ($env:TTK_BUILD_ROOT) { $env:TTK_BUILD_ROOT } else { Join-Path $env:LOCALAPPDATA 'ttkconverter-build' }),
    [string] $Destination
)

$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
if (!$Destination) { $Destination = Join-Path $project 'release/third-party-source.zip' }
$Destination = [IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Path (Split-Path $Destination -Parent) -Force | Out-Null
$downloads = Join-Path $Work 'downloads'
$lock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'dependencies.json') -Raw | ConvertFrom-Json
$sources = [ordered]@{
    ffmpeg = 'ffmpeg-7.1.5.tar.xz'; x264 = 'x264-31e19f92.tar.gz'; x265 = 'x265-4.1.tar.gz'
    aom = 'libaom-3.12.1.tar.gz'; vpx = 'libvpx-1.15.2.tar.gz'; lame = 'lame-3.100.tar.gz'
    opus = 'opus-1.5.2.tar.gz'; vorbis = 'libvorbis-1.3.7.tar.gz'; ogg = 'libogg-1.3.6.tar.gz'
    webp = 'libwebp-1.5.0.tar.gz'; zlib = 'zlib-1.3.1.tar.gz'; nvcodec = 'nv-codec-headers-12.2.72.0.tar.gz'
    xpdf_source = 'xpdf-4.06.tar.gz'; xpdf_cmap = 'xpdf-chinese-simplified.tar.gz'
    tesseract_source = 'tesseract-5.3.0.tar.gz'; leptonica_source = 'leptonica-1.83.1.tar.gz'
    libpng_source = 'libpng-1.6.59.tar.gz'; freetype_source = 'freetype-2.14.3.tar.xz'
    mingw_w64_source = 'mingw-w64-41eb0423.tar.gz'
}
foreach ($key in $sources.Keys) {
    $expected = ($lock | Where-Object name -eq $key).sha256
    if (!$expected -or (Get-FileHash -LiteralPath (Join-Path $downloads $key)).Hash -ne $expected) {
        throw "Source archive did not match the dependency lock: $key"
    }
}

$token = [Guid]::NewGuid().ToString('N')
$headers = Join-Path $Work "amf-headers-$token.tar.gz"
$partial = "$Destination.$token.part"
try {
    & tar.exe -czf $headers -C (Join-Path $Work 'source/amf') 'amf/public/include' 'LICENSE.txt'
    if ($LASTEXITCODE) { throw 'Cannot archive the AMF SDK headers' }
    $zip = [IO.Compression.ZipFile]::Open($partial, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($key in $sources.Keys) {
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, (Join-Path $downloads $key),
                "archives/$($sources[$key])", [IO.Compression.CompressionLevel]::NoCompression) | Out-Null
        }
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $headers,
            'archives/amf-1.4.35-headers.tar.gz') | Out-Null
        foreach ($item in Get-ChildItem -LiteralPath $PSScriptRoot -File) {
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $item.FullName, "tools/$($item.Name)") | Out-Null
        }
        foreach ($name in 'README.md', 'installer/THIRD-PARTY.txt') {
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, (Join-Path $project $name), $name) | Out-Null
        }
    } finally {
        $zip.Dispose()
    }
    Move-Item -LiteralPath $partial -Destination $Destination -Force
} finally {
    foreach ($path in $headers, $partial) {
        if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Item -LiteralPath $path -Force }
    }
}
Get-Item -LiteralPath $Destination | Select-Object Name,Length
