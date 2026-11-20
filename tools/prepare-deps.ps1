[CmdletBinding()]
param(
    [string] $Work = $(if ($env:TTK_BUILD_ROOT) { $env:TTK_BUILD_ROOT } else { Join-Path $env:LOCALAPPDATA 'ttkconverter-build' }),
    [switch] $InstallInno,
    [switch] $StagePdf
)

$ErrorActionPreference = 'Stop'
$Work = [IO.Path]::GetFullPath($Work)
$downloads = Join-Path $Work 'downloads'
New-Item -ItemType Directory -Path $downloads -Force | Out-Null
$lock = Join-Path $PSScriptRoot 'dependencies.json'
$dependencies = Get-Content -LiteralPath $lock -Raw | ConvertFrom-Json
foreach ($dependency in $dependencies) {
    $file = Join-Path $downloads $dependency.name
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) {
        Write-Output "Fetching $($dependency.name)"
        $partial = "$file.part"
        & curl.exe -fLsS --retry 2 $dependency.url -o $partial
        if ($LASTEXITCODE) { throw "Download failed: $($dependency.name)" }
        if ((Get-FileHash -LiteralPath $partial).Hash -ne $dependency.sha256) {
            throw "SHA-256 mismatch: $($dependency.name)"
        }
        Move-Item -LiteralPath $partial -Destination $file -Force
    } elseif ((Get-FileHash -LiteralPath $file).Hash -ne $dependency.sha256) {
        throw "SHA-256 mismatch: $file"
    }
}
Copy-Item -LiteralPath $lock -Destination (Join-Path $downloads 'dependencies.json') -Force
$winpthreadsLicense = 'mingw-w64-41eb042358a52011b35350392130dcdc94a8ead8/mingw-w64-libraries/winpthreads/COPYING'
$notice = & tar.exe -xOf (Join-Path $downloads 'mingw_w64_source') $winpthreadsLicense
if ($LASTEXITCODE -or !$notice) { throw 'Cannot extract the Winpthreads notices' }
$notice | Set-Content -LiteralPath (Join-Path $downloads 'winpthreads-LICENSE.txt') -Encoding UTF8

function Expand-TtkAsset([string] $name, [string] $directory, [int] $strip = 1) {
    $ready = Join-Path $directory '.ttk-expanded'
    if (Test-Path -LiteralPath $ready) { return }
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    & tar.exe -xf (Join-Path $downloads $name) -C $directory --strip-components $strip
    if ($LASTEXITCODE) { throw "Cannot expand $name" }
    New-Item -ItemType File -Path $ready -Force | Out-Null
}

if (!(Test-Path -LiteralPath (Join-Path $Work 'toolchain/bin/clang.exe'))) {
    Expand-TtkAsset 'llvm' (Join-Path $Work 'toolchain')
}
foreach ($name in @('ffmpeg','x264','x265','aom','vpx','lame','opus','vorbis','ogg','webp','zlib','amf','nvcodec')) {
    $destination = Join-Path $Work "source/$name"
    if (!(Test-Path -LiteralPath $destination -PathType Container)) { Expand-TtkAsset $name $destination }
}
Expand-TtkAsset 'nasm' (Join-Path $Work 'source/nasm') 0

$prefix = Join-Path $Work 'media-x64/install'
$include = Join-Path $prefix 'include'
$pkgConfig = Join-Path $prefix 'lib/pkgconfig'
New-Item -ItemType Directory -Force -Path $include,$pkgConfig | Out-Null
$amfInclude = Join-Path $include 'AMF'
New-Item -ItemType Directory -Force -Path $amfInclude | Out-Null
Get-ChildItem -LiteralPath (Join-Path $Work 'source/amf/amf/public/include') | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $amfInclude -Recurse -Force
}
Copy-Item -LiteralPath (Join-Path $Work 'source/nvcodec/include/ffnvcodec') -Destination $include -Recurse -Force
(Get-Content -LiteralPath (Join-Path $Work 'source/nvcodec/ffnvcodec.pc.in') -Raw).Replace('@@PREFIX@@', $prefix.Replace('\', '/')) |
    Set-Content -LiteralPath (Join-Path $pkgConfig 'ffnvcodec.pc') -Encoding ascii

if ($InstallInno -and !(Test-Path -LiteralPath (Join-Path $Work 'inno/ISCC.exe'))) {
    $installer = Join-Path $downloads 'innosetup-6.7.3.exe'
    Copy-Item -LiteralPath (Join-Path $downloads 'inno') -Destination $installer -Force
    $innoDirectory = Join-Path $Work 'inno'
    $process = Start-Process -FilePath $installer -WindowStyle Hidden -Wait -PassThru -ArgumentList @(
        '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/CURRENTUSER','/NOICONS',"/DIR=`"$innoDirectory`""
    )
    if ($process.ExitCode) { throw "Inno Setup install failed with exit code $($process.ExitCode)" }
}
if ($StagePdf) {
    foreach ($architecture in 'x86', 'x64') {
        & (Join-Path $PSScriptRoot 'build-ocr.ps1') -BuildRoot $Work -Architecture $architecture
        & (Join-Path $PSScriptRoot 'build-xpdf.ps1') -Work $Work -Architecture $architecture
    }
    & (Join-Path $PSScriptRoot 'stage-pdf.ps1') -BuildRoot $Work
}
