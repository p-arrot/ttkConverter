[CmdletBinding()]
param(
    [ValidateSet('Win10-11-x64', 'Win7-SP1-x64', 'Win7-SP1-x86')]
    [string] $Edition,
    [string] $Work = $(if ($env:TTK_BUILD_ROOT) { $env:TTK_BUILD_ROOT } else { Join-Path $env:LOCALAPPDATA 'ttkconverter-build' }),
    [string] $CMake = 'cmake.exe',
    [string] $Ninja = 'ninja.exe',
    [switch] $Package,
    [string] $MediaPayload,
    [string] $PdfPayload
)

$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$buildDefinition = Get-Content -LiteralPath (Join-Path $project 'CMakeLists.txt') -Raw
if ($buildDefinition -notmatch '(?m)^project\(ttkConverter\s+VERSION\s+(\d+\.\d+\.\d+)\b') {
    throw 'Cannot read the ttkConverter version from CMakeLists.txt'
}
$version = $Matches[1]
$Work = [IO.Path]::GetFullPath($Work)
$toolchain = Join-Path $Work 'toolchain/bin'
$build = Join-Path $Work "app/$Edition"
$stage = Join-Path $Work "package/$Edition"
$architecture = if ($Edition.EndsWith('x86')) { 'x86' } else { 'x64' }
$machine = if ($architecture -eq 'x86') { 'i686' } else { 'x86_64' }
$target = "$machine-w64-mingw32"
$modern = if ($Edition -eq 'Win10-11-x64') { 'ON' } else { 'OFF' }
$clang = (Join-Path $toolchain "$target-clang++.exe").Replace('\', '/')
$windres = (Join-Path $toolchain "$target-windres.exe").Replace('\', '/')
if (!(Test-Path -LiteralPath $clang) -or !(Test-Path -LiteralPath $windres)) {
    throw "LLVM MinGW toolchain is missing: $toolchain"
}

$ninjaPath = (Get-Command $Ninja).Source.Replace('\', '/')
& $CMake --fresh -S $project -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninjaPath" `
    -DCMAKE_SYSTEM_NAME=Windows "-DCMAKE_SYSTEM_PROCESSOR=$machine" "-DCMAKE_CXX_COMPILER=$clang" `
    "-DCMAKE_RC_COMPILER=$windres" -DCMAKE_BUILD_TYPE=Release "-DTTK_MODERN=$modern"
if ($LASTEXITCODE) { throw "CMake configure failed ($LASTEXITCODE)" }
& $CMake --build $build --parallel 4
if ($LASTEXITCODE) { throw "CMake build failed ($LASTEXITCODE)" }
if (!$Package) { return }

if (!$MediaPayload) {
    $MediaPayload = if ($modern -eq 'ON') { Join-Path $Work 'media-modern/engines' } else { Join-Path $Work "media-$architecture/engines" }
}
if (!$PdfPayload) { $PdfPayload = Join-Path $Work "pdf-public/$architecture/engines" }
foreach ($name in @('ffmpeg.exe', 'ffprobe.exe')) {
    if (!(Test-Path -LiteralPath (Join-Path $MediaPayload $name))) { throw "Missing $name in $MediaPayload" }
}
foreach ($name in @('pdfinfo.exe', 'pdftotext.exe', 'pdftopng.exe', 'tesseract.exe', 'tessdata')) {
    if (!(Test-Path -LiteralPath (Join-Path $PdfPayload $name))) { throw "Missing $name in $PdfPayload" }
}
$stageParent = [IO.Path]::GetFullPath((Join-Path $Work 'package'))
if (Test-Path -LiteralPath $stage) {
    $item = Get-Item -LiteralPath $stage -Force
    if ($item.LinkType -or [IO.Path]::GetDirectoryName($item.FullName) -ne $stageParent -or $item.Name -ne $Edition) {
        throw "Unexpected packaging directory: $($item.FullName)"
    }
    Remove-Item -LiteralPath $item.FullName -Recurse -Force
}
$engines = Join-Path $stage 'engines'
$pdfEngines = Join-Path $engines 'pdf'
New-Item -ItemType Directory -Force -Path $pdfEngines | Out-Null
Copy-Item -LiteralPath (Join-Path $build 'ttkConverter.exe') -Destination $stage -Force
Get-ChildItem -LiteralPath $MediaPayload -File | Where-Object {
    $_.Name -in @('ffmpeg.exe', 'ffprobe.exe') -or $_.Extension -eq '.dll'
} | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $engines -Force }
Get-ChildItem -LiteralPath $PdfPayload | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $pdfEngines -Force -Recurse
}
$licenses = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Path $licenses -Force | Out-Null
foreach ($name in @('ffmpeg', 'x264', 'x265', 'aom', 'vpx', 'lame', 'opus', 'vorbis', 'ogg', 'webp', 'zlib')) {
    $library = Join-Path $Work "source/$name"
    if (!(Test-Path -LiteralPath $library)) { continue }
    Get-ChildItem -LiteralPath $library -File | Where-Object { $_.Name -match '^(COPYING|LICENSE|PATENTS|COPYRIGHT)(\.|$)' } |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $licenses "$name-$($_.Name).txt") -Force }
}
Copy-Item -LiteralPath (Join-Path $project 'installer/THIRD-PARTY.txt') -Destination $licenses -Force
Copy-Item -LiteralPath (Join-Path $project 'LICENSE') -Destination (Join-Path $licenses 'ttkConverter-LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $project 'assets/ttk-rounded-OFL.txt') -Destination $licenses -Force
Copy-Item -LiteralPath (Join-Path $Work 'toolchain/LICENSE.TXT') -Destination (Join-Path $licenses 'llvm-mingw-LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $Work 'downloads/winpthreads-LICENSE.txt') -Destination $licenses -Force
if ($modern -eq 'ON') {
    Copy-Item -LiteralPath (Join-Path $Work 'source/amf/LICENSE.txt') -Destination (Join-Path $licenses 'amf-LICENSE.txt') -Force
    Copy-Item -LiteralPath (Join-Path $Work 'source/nvcodec/include/ffnvcodec/nvEncodeAPI.h') -Destination $licenses -Force
}
& (Join-Path $engines 'ffmpeg.exe') -version | Set-Content -LiteralPath (Join-Path $licenses 'ffmpeg-build.txt') -Encoding UTF8
if ($LASTEXITCODE) { throw 'The staged FFmpeg executable did not start' }
if (Test-Path -LiteralPath (Join-Path $Work 'downloads/dependencies.json')) {
    Copy-Item -LiteralPath (Join-Path $Work 'downloads/dependencies.json') -Destination $licenses -Force
}
& (Join-Path $PSScriptRoot 'audit-pe.ps1') -Root $stage -Architecture $architecture `
    -ReadObj (Join-Path $toolchain 'llvm-readobj.exe') -Legacy:($modern -eq 'OFF')

$release = Join-Path $project 'release'
New-Item -ItemType Directory -Path $release -Force | Out-Null
$inno = Join-Path $Work 'inno/ISCC.exe'
$innoArgs = @("/DEdition=$Edition", "/DMyVersion=$version", "/DStageDir=$stage", "/DOutputDir=$release", "/DAppIcon=$(Join-Path $project 'src/app.ico')")
$chinese = Join-Path $Work 'downloads/ChineseSimplified.isl'
if (Test-Path -LiteralPath $chinese) { $innoArgs += "/DChineseMessages=$chinese" }
& $inno @innoArgs (Join-Path $project 'installer/ttkConverter.iss')
if ($LASTEXITCODE) { throw "Inno Setup compilation failed ($LASTEXITCODE)" }
$setup = Join-Path $release "ttkConverter-$version-$Edition-Setup.exe"
$hash = (Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $([IO.Path]::GetFileName($setup))" | Set-Content -LiteralPath "$setup.sha256" -Encoding ascii
Get-Item -LiteralPath $setup | Select-Object Name,Length
