[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $BuildRoot,
    [ValidateSet('x86', 'x64')] [string] $Architecture = 'x64',
    [ValidateRange(1, 32)] [int] $Jobs = 4
)

$ErrorActionPreference = 'Stop'
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
$work = (Join-Path $BuildRoot "ocr-build/llvm23/$Architecture").Replace('\', '/')
$output = Join-Path $BuildRoot "ocr-static/$Architecture"
$downloads = Join-Path $BuildRoot 'downloads'
$toolchain = (Join-Path $BuildRoot 'toolchain/bin').Replace('\', '/')
$target = if ($Architecture -eq 'x86') { 'i686-w64-mingw32' } else { 'x86_64-w64-mingw32' }
$processor = if ($Architecture -eq 'x86') { 'i686' } else { 'x86_64' }
$sources = @{
    zlib = @('zlib', '9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23')
    png = @('libpng_source', '2540302a1844ad2b2b501977abecfa850f265f97b78f065a712ab4074a89f5b5')
    lept = @('leptonica_source', '4289d0a4224b614010072253531c0455a33a4d7c7a0017fe7825ed382290c0da')
    tess = @('tesseract_source', '7e70870f8341e5ea228af2836ce79a36eefa11b01b56177b4a8997f330c014b8')
}

function Invoke-Checked([string] $Program, [string[]] $Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}

function Expand-Source([string] $Component) {
    $asset = $sources[$Component]
    $archive = Join-Path $downloads $asset[0]
    if (!(Test-Path -LiteralPath $archive -PathType Leaf) -or (Get-FileHash -LiteralPath $archive).Hash -ne $asset[1]) {
        throw "Missing or invalid OCR source archive: $archive"
    }
    $directory = Join-Path $work "$Component-src"
    $stamp = Join-Path $directory '.ttk-archive-sha256'
    if (!(Test-Path -LiteralPath $stamp) -or (Get-Content -LiteralPath $stamp -Raw).Trim() -ne $asset[1]) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
        Invoke-Checked 'tar.exe' @('-xf', $archive, '-C', $directory, '--strip-components', '1')
        Set-Content -LiteralPath $stamp -Value $asset[1] -Encoding Ascii
    }
}

$win7Flags = '-D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -ffunction-sections -fdata-sections'
$common = @(
    '-G', 'Ninja', '-DCMAKE_SYSTEM_NAME=Windows', '-DCMAKE_SYSTEM_VERSION=6.1', "-DCMAKE_SYSTEM_PROCESSOR=$processor",
    "-DCMAKE_C_COMPILER=$toolchain/$target-clang.exe", "-DCMAKE_CXX_COMPILER=$toolchain/$target-clang++.exe",
    "-DCMAKE_RC_COMPILER=$toolchain/$target-windres.exe",
    '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', '-DBUILD_SHARED_LIBS=OFF',
    '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE', '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE',
    "-DCMAKE_C_FLAGS=$win7Flags", "-DCMAKE_CXX_FLAGS=$win7Flags",
    '-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG', '-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG',
    '-DCMAKE_EXE_LINKER_FLAGS=-static -Wl,--gc-sections,--major-subsystem-version,6,--minor-subsystem-version,1'
)

function Configure([string] $Component, [string[]] $Settings) {
    Invoke-Checked 'cmake.exe' (@('-S', (Join-Path $work "$Component-src"), '-B', (Join-Path $work "$Component-build")) + $common + $Settings)
}

function Build([string] $Component, [string] $BuildTarget) {
    $arguments = @('--build', (Join-Path $work "$Component-build"), '--parallel', $Jobs.ToString())
    if ($BuildTarget) { $arguments += @('--target', $BuildTarget) }
    Invoke-Checked 'cmake.exe' $arguments
}

foreach ($component in 'zlib', 'png', 'lept', 'tess') { Expand-Source $component }
$simd = Join-Path $work 'tess-src/src/arch/simddetect.cpp'
$simdSource = Get-Content -LiteralPath $simd -Raw
if (!$simdSource.Contains('std::getenv("DOTPRODUCT")')) {
    # Tesseract 5.3.0 accidentally relies on a transitive header for getenv.
    $header = '#include "simddetect.h"'
    $call = 'getenv("DOTPRODUCT")'
    if (!$simdSource.Contains($header) -or !$simdSource.Contains($call)) {
        throw 'The pinned Tesseract SIMD source no longer matches the compiler compatibility patch'
    }
    $simdSource = $simdSource.Replace($header, $header + "`n" + '#include <cstdlib>')
    $simdSource = $simdSource.Replace($call, 'std::getenv("DOTPRODUCT")')
    Set-Content -LiteralPath $simd -Value $simdSource -Encoding Utf8 -NoNewline
}
Configure 'zlib' @('-DZLIB_BUILD_EXAMPLES=OFF')
Build 'zlib' 'zlibstatic'
$zlibHeaders = "$work/zlib-include"
New-Item -ItemType Directory -Path $zlibHeaders -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $work 'zlib-src/zlib.h'),(Join-Path $work 'zlib-build/zconf.h') -Destination $zlibHeaders -Force

# Resolve the exact archives instead of allowing the default host or registry search.
$modules = "$work/ocr-cmake"
New-Item -ItemType Directory -Path $modules -Force | Out-Null
@'
set(ZLIB_FOUND TRUE)
set(ZLIB_VERSION "1.3.1")
set(ZLIB_VERSION_STRING "1.3.1")
set(ZLIB_INCLUDE_DIRS "${TTK_ZLIB_HEADERS}")
set(ZLIB_LIBRARIES "${TTK_ZLIB_ARCHIVE}")
if(NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB STATIC IMPORTED)
    set_target_properties(ZLIB::ZLIB PROPERTIES
        IMPORTED_LOCATION "${TTK_ZLIB_ARCHIVE}"
        INTERFACE_INCLUDE_DIRECTORIES "${TTK_ZLIB_HEADERS}")
endif()
'@ | Set-Content -LiteralPath "$modules/FindZLIB.cmake" -Encoding Ascii
@'
set(PNG_FOUND TRUE)
set(PNG_INCLUDE_DIRS "${TTK_PNG_HEADERS}")
set(PNG_LIBRARIES "${TTK_PNG_ARCHIVE};${TTK_ZLIB_ARCHIVE}")
'@ | Set-Content -LiteralPath "$modules/FindPNG.cmake" -Encoding Ascii
$zlib = @("-DCMAKE_MODULE_PATH=$modules", "-DTTK_ZLIB_HEADERS=$zlibHeaders", "-DTTK_ZLIB_ARCHIVE=$work/zlib-build/libzlibstatic.a")

Configure 'png' ($zlib + @('-DPNG_SHARED=OFF', '-DPNG_STATIC=ON', '-DPNG_TOOLS=OFF',
    '-DPNG_TESTS=OFF', '-DPNG_HARDWARE_OPTIMIZATIONS=OFF'))
Build 'png' 'png_static'
$pngArchive = Get-ChildItem -LiteralPath (Join-Path $work 'png-build') -Filter '*png16*.a' -File | Select-Object -First 1
if (!$pngArchive) { throw 'Static libpng archive was not produced' }
$pngInclude = "$work/png-build"
Copy-Item -LiteralPath (Join-Path $work 'png-src/png.h'),(Join-Path $work 'png-src/pngconf.h') -Destination $pngInclude -Force

$leptInstall = "$work/lept-install"
$imageLibraries = @('-DCMAKE_DISABLE_FIND_PACKAGE_GIF=ON', '-DCMAKE_DISABLE_FIND_PACKAGE_JPEG=ON',
                    '-DCMAKE_DISABLE_FIND_PACKAGE_TIFF=ON', '-DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON')
Configure 'lept' ($zlib + $imageLibraries + @('-DSW_BUILD=OFF', '-DBUILD_PROG=OFF', '-DLIBWEBP_SUPPORT=OFF',
    '-DOPENJPEG_SUPPORT=OFF', "-DTTK_PNG_HEADERS=$pngInclude", "-DTTK_PNG_ARCHIVE=$($pngArchive.FullName.Replace('\', '/'))",
    "-DCMAKE_INSTALL_PREFIX=$leptInstall"))
Build 'lept'
Invoke-Checked 'cmake.exe' @('--install', (Join-Path $work 'lept-build'))

Configure 'tess' ($imageLibraries + @('-DSW_BUILD=OFF', '-DOPENMP_BUILD=OFF', '-DGRAPHICS_DISABLED=ON',
    '-DDISABLED_LEGACY_ENGINE=ON', '-DBUILD_TRAINING_TOOLS=OFF', '-DBUILD_TESTS=OFF',
    '-DDISABLE_ARCHIVE=ON', '-DDISABLE_CURL=ON', '-DINSTALL_CONFIGS=OFF', "-DCMAKE_PREFIX_PATH=$leptInstall",
    "-DCMAKE_CXX_FLAGS=$win7Flags -DTESSERACT_DISABLE_DEBUG_FONTS"))
Build 'tess' 'tesseract'

$notices = Join-Path $output 'licenses'
New-Item -ItemType Directory -Path $output,$notices -Force | Out-Null
$exeSource = Join-Path $work 'tess-build/bin/tesseract.exe'
$staged = Join-Path $output 'tesseract.exe'
Copy-Item -LiteralPath $exeSource -Destination $staged -Force
Invoke-Checked (Join-Path $toolchain 'llvm-strip.exe') @('--strip-debug', $staged)
Copy-Item -LiteralPath (Join-Path $work 'tess-src/LICENSE') -Destination (Join-Path $notices 'Tesseract-LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $work 'lept-src/leptonica-license.txt') -Destination (Join-Path $notices 'Leptonica-LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $work 'png-src/LICENSE') -Destination (Join-Path $notices 'libpng-LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $work 'zlib-src/LICENSE') -Destination (Join-Path $notices 'zlib-LICENSE.txt') -Force
Invoke-Checked $staged @('--version')
