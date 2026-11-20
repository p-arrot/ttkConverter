[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $BuildRoot,
    [string] $OutputRoot,
    [string] $OcrRoot,
    [string] $XpdfRoot
)

$ErrorActionPreference = 'Stop'
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
if (!$OutputRoot) { $OutputRoot = Join-Path $BuildRoot 'pdf-public' }
if (!$OcrRoot) { $OcrRoot = Join-Path $BuildRoot 'ocr-static' }
if (!$XpdfRoot) { $XpdfRoot = Join-Path $BuildRoot 'xpdf-static' }
$downloads = Join-Path $BuildRoot 'downloads'
$assets = Join-Path $BuildRoot 'pdf-public-assets'
$hashes = @{
    xpdf_source = '1c38f527c46caee0f712386d42a885b96a31ed9ce11904e872559859894d137e'
    xpdf_cmap = 'f946b8d1659812d7ed25d235ae9d693b37cd6927f34d72f32284d286ce2813b4'
    eng = '7d4322bd2a7749724879683fc3912cb542f19906c83bcc1a52132556427170b2'
    chi_sim = 'a5fcb6f0db1e1d6d8522f39db4e848f05984669172e584e8d76b6b3141e1f730'
}

function Get-Asset([string] $Name) {
    $path = Join-Path $downloads $Name
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing PDF asset $path" }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $hashes[$Name]) {
        throw "PDF asset failed SHA-256 verification $path"
    }
    return $path
}

function Expand-Asset([string] $Name, [string] $Destination) {
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    & tar.exe -xf (Get-Asset $Name) -C $Destination --strip-components 1
    if ($LASTEXITCODE -ne 0) { throw "Could not extract PDF asset $Name" }
}

$xpdf = Join-Path $assets 'xpdf'
$cmap = Join-Path $assets 'cmap'
Expand-Asset 'xpdf_source' $xpdf
Expand-Asset 'xpdf_cmap' $cmap

foreach ($architecture in 'x86', 'x64') {
    $output = Join-Path $OutputRoot "$architecture/engines"
    if ((Test-Path -LiteralPath $output) -and
        (Get-ChildItem -LiteralPath $output -File -Filter '*.dll' | Select-Object -First 1)) {
        throw "Static PDF payload needs an output path without a previous dynamic engine installation $output"
    }
    $tessdata = Join-Path $output 'tessdata'
    $mapDir = Join-Path $output 'xpdf-cmaps'
    $notices = Join-Path $output 'third-party'
    $xpdfNotices = Join-Path $notices 'xpdf'
    $ocrNotices = Join-Path $notices 'tesseract'
    New-Item -ItemType Directory -Force -Path $output, $tessdata, $mapDir, $xpdfNotices, $ocrNotices | Out-Null

    $ocr = Join-Path $OcrRoot $architecture
    Copy-Item -LiteralPath (Join-Path $ocr 'tesseract.exe') -Destination $output -Force
    foreach ($notice in Get-ChildItem -LiteralPath (Join-Path $ocr 'licenses') -File) {
        Copy-Item -LiteralPath $notice.FullName -Destination $ocrNotices -Force
    }
    Copy-Item -LiteralPath (Get-Asset 'chi_sim') -Destination (Join-Path $tessdata 'chi_sim.traineddata') -Force
    Copy-Item -LiteralPath (Get-Asset 'eng') -Destination (Join-Path $tessdata 'eng.traineddata') -Force

    $builtXpdf = Join-Path $XpdfRoot $architecture
    foreach ($name in 'pdfinfo.exe', 'pdftotext.exe', 'pdftopng.exe') {
        Copy-Item -LiteralPath (Join-Path $builtXpdf $name) -Destination $output -Force
    }
    $builtXpdfNotices = Join-Path $builtXpdf 'licenses'
    if (Test-Path -LiteralPath $builtXpdfNotices) {
        foreach ($notice in Get-ChildItem -LiteralPath $builtXpdfNotices -File) {
            Copy-Item -LiteralPath $notice.FullName -Destination $xpdfNotices -Force
        }
    }
    foreach ($name in 'README', 'COPYING', 'COPYING3', 'doc') {
        Copy-Item -LiteralPath (Join-Path $xpdf $name) -Destination $xpdfNotices -Recurse -Force
    }
    Copy-Item -LiteralPath (Get-Asset 'xpdf_source') -Destination (Join-Path $xpdfNotices 'xpdf-4.06.tar.gz') -Force
    foreach ($file in Get-ChildItem -LiteralPath $cmap) {
        Copy-Item -LiteralPath $file.FullName -Destination $mapDir -Recurse -Force
    }
    $config = (Get-Content -Raw -LiteralPath (Join-Path $cmap 'add-to-xpdfrc')).Replace(
        '/usr/local/share/xpdf/chinese-simplified/', 'xpdf-cmaps/'
    )
    Set-Content -LiteralPath (Join-Path $output 'xpdfrc') -Value $config -Encoding Ascii

    @"
Bundled PDF and OCR sources for $architecture

Xpdf Command Line Tools 4.06, GNU GPL v2 or v3
https://dl.xpdfreader.com/xpdf-4.06.tar.gz
The matching source archive, licenses, and man pages are in the xpdf folder.
CJK mappings: https://dl.xpdfreader.com/xpdf-chinese-simplified.tar.gz
CMap copyright notices are preserved in the xpdf-cmaps folder.

Tesseract 5.3.0, Apache License 2.0
https://github.com/tesseract-ocr/tesseract/tree/5.3.0
Leptonica 1.83.1, BSD 2-Clause
https://github.com/DanBloomberg/leptonica/tree/1.83.1
libpng 1.6.59, libpng License
https://github.com/pnggroup/libpng/tree/v1.6.59
zlib 1.3.1, zlib License
https://github.com/madler/zlib/tree/v1.3.1

Fast Simplified Chinese and English OCR models, Apache License 2.0
https://github.com/tesseract-ocr/tessdata_fast
"@ | Set-Content -LiteralPath (Join-Path $notices 'PDF-and-OCR-sources.txt') -Encoding Ascii

    $size = (Get-ChildItem -LiteralPath $output -File -Recurse | Measure-Object Length -Sum).Sum
    Write-Output ('PDF and OCR payload {0}: {1:N1} MiB' -f $architecture, ($size / 1MB))
}
