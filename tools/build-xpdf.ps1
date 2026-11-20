[CmdletBinding()]
param(
    [Parameter(Mandatory)] [ValidateSet('x86', 'x64')] [string] $Architecture,
    [string] $Work = $(if ($env:TTK_BUILD_ROOT) { $env:TTK_BUILD_ROOT } else { Join-Path $env:LOCALAPPDATA 'ttkConverter/build' }),
    [ValidateRange(1, 32)] [int] $Jobs = 4
)

$ErrorActionPreference = 'Stop'
$Work = [IO.Path]::GetFullPath($Work)
$downloads = Join-Path $Work 'downloads'
$output = Join-Path $Work "xpdf-static/$Architecture"
$build = Join-Path $Work "xpdf-build/$Architecture"
$sources = Join-Path $build 'source'
$prefix = Join-Path $build 'install'
$toolchain = Join-Path $Work 'toolchain/bin'
$machine = if ($Architecture -eq 'x86') { 'i686' } else { 'x86_64' }
$triple = "$machine-w64-mingw32"
New-Item -ItemType Directory -Force -Path $downloads, $sources, $prefix, $output | Out-Null

$assets = @(
    @{ Name = 'xpdf_source'; Folder = 'xpdf-4.06'; Url = 'https://dl.xpdfreader.com/xpdf-4.06.tar.gz'; Sha256 = '1c38f527c46caee0f712386d42a885b96a31ed9ce11904e872559859894d137e' },
    @{ Name = 'zlib'; Folder = 'zlib-1.3.1'; Url = 'https://zlib.net/fossils/zlib-1.3.1.tar.gz'; Sha256 = '9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23' },
    @{ Name = 'libpng_source'; Folder = 'libpng-1.6.59'; Url = 'https://codeload.github.com/pnggroup/libpng/tar.gz/refs/tags/v1.6.59'; Sha256 = '2540302a1844ad2b2b501977abecfa850f265f97b78f065a712ab4074a89f5b5' },
    @{ Name = 'freetype_source'; Folder = 'freetype-2.14.3'; Url = 'https://download.savannah.gnu.org/releases/freetype/freetype-2.14.3.tar.xz'; Sha256 = '36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f' }
)

foreach ($asset in $assets) {
    $archive = Join-Path $downloads $asset.Name
    if (!(Test-Path -LiteralPath $archive)) {
        $partial = "$archive.$Architecture.part"
        & curl.exe --fail --silent --show-error --location --retry 2 --output $partial $asset.Url
        if ($LASTEXITCODE) { throw "Download failed: $($asset.Url)" }
        if ((Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash -ne $asset.Sha256) {
            throw "Download checksum mismatch: $($asset.Name)"
        }
        Move-Item -LiteralPath $partial -Destination $archive -Force
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $asset.Sha256) {
        throw "Source checksum mismatch: $archive"
    }
    $source = Join-Path $sources $asset.Folder
    $stamp = Join-Path $source '.ttk-source-sha256'
    if (!(Test-Path -LiteralPath $stamp)) {
        & tar.exe -xf $archive -C $sources
        if ($LASTEXITCODE) { throw "Extraction failed: $archive" }
        $asset.Sha256 | Set-Content -LiteralPath $stamp -Encoding ascii
    } elseif ((Get-Content -Raw -LiteralPath $stamp).Trim() -ne $asset.Sha256) {
        throw "Extracted source mismatch: $source"
    }
}

function Invoke-CMake([string[]] $Arguments) {
    $cmakeArguments = $Arguments | ForEach-Object { $_.Replace('\', '/') }
    & cmake.exe @cmakeArguments
    if ($LASTEXITCODE) { throw "CMake failed ($LASTEXITCODE): $($Arguments -join ' ')" }
}

$win7 = '-D_WIN32_WINNT=0x0601 -DWINVER=0x0601'
$linker = '-static -Wl,--major-subsystem-version,6,--minor-subsystem-version,1'
$configure = @(
    '--fresh', '-G', 'Ninja', '-DCMAKE_SYSTEM_NAME=Windows', "-DCMAKE_SYSTEM_PROCESSOR=$machine",
    "-DCMAKE_C_COMPILER=$toolchain/$triple-clang.exe", "-DCMAKE_CXX_COMPILER=$toolchain/$triple-clang++.exe",
    "-DCMAKE_RC_COMPILER=$toolchain/$triple-windres.exe", '-DCMAKE_POLICY_VERSION_MINIMUM=3.5',
    '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_C_FLAGS=$win7", "-DCMAKE_CXX_FLAGS=$win7",
    "-DCMAKE_EXE_LINKER_FLAGS=$linker", "-DCMAKE_INSTALL_PREFIX=$prefix", '-DCMAKE_INSTALL_LIBDIR=lib',
    '-DBUILD_SHARED_LIBS=OFF', '-DBUILD_TESTING=OFF'
)

$zlibSource = Join-Path $sources 'zlib-1.3.1'
$zlibBuild = Join-Path $build 'zlib'
$zlibInclude = Join-Path $build 'zlib-include'
Invoke-CMake (@('-S', $zlibSource, '-B', $zlibBuild, '-DZLIB_BUILD_EXAMPLES=OFF') + $configure)
Invoke-CMake @('--build', $zlibBuild, '--target', 'zlibstatic', '--parallel', $Jobs)
New-Item -ItemType Directory -Force -Path $zlibInclude | Out-Null
Copy-Item -LiteralPath "$zlibSource/zlib.h", "$zlibBuild/zconf.h" -Destination $zlibInclude -Force

# Resolve only the verified static archives. The engine build has no host
# registry or library search dependency.
$modules = Join-Path $build 'cmake'
New-Item -ItemType Directory -Force -Path $modules | Out-Null
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
'@ | Set-Content -LiteralPath (Join-Path $modules 'FindZLIB.cmake') -Encoding ascii
$zlib = @("-DCMAKE_MODULE_PATH=$modules", "-DTTK_ZLIB_ARCHIVE=$zlibBuild/libzlibstatic.a", "-DTTK_ZLIB_HEADERS=$zlibInclude")

$pngSource = Join-Path $sources 'libpng-1.6.59'
$pngBuild = Join-Path $build 'png'
Invoke-CMake (@('-S', $pngSource, '-B', $pngBuild, '-DPNG_SHARED=OFF', '-DPNG_STATIC=ON',
    '-DPNG_TESTS=OFF', '-DPNG_TOOLS=OFF') + $configure + $zlib)
Invoke-CMake @('--build', $pngBuild, '--target', 'png_static', '--parallel', $Jobs)
$pngLibrary = Get-ChildItem -LiteralPath $pngBuild -File -Filter '*png16*.a' | Select-Object -First 1
if (!$pngLibrary) { throw "libpng static archive not found in $pngBuild" }
Copy-Item -LiteralPath "$pngSource/png.h", "$pngSource/pngconf.h" -Destination $pngBuild -Force
@'
set(PNG_FOUND TRUE)
set(PNG_INCLUDE_DIRS "${TTK_PNG_HEADERS}")
set(PNG_LIBRARIES "${TTK_PNG_ARCHIVE};${TTK_ZLIB_ARCHIVE}")
'@ | Set-Content -LiteralPath (Join-Path $modules 'FindPNG.cmake') -Encoding ascii

$ftSource = Join-Path $sources 'freetype-2.14.3'
$ftBuild = Join-Path $build 'freetype'
Invoke-CMake (@('-S', $ftSource, '-B', $ftBuild, '-DFT_DISABLE_HARFBUZZ=ON', '-DFT_DISABLE_BROTLI=ON',
    '-DFT_DISABLE_BZIP2=ON', '-DFT_DISABLE_PNG=ON', '-DFT_REQUIRE_ZLIB=ON') + $configure + $zlib)
Invoke-CMake @('--build', $ftBuild, '--target', 'install', '--parallel', $Jobs)

$xpdfSource = Join-Path $sources 'xpdf-4.06'
$xpdfBuild = Join-Path $build 'xpdf'
Invoke-CMake (@('-S', $xpdfSource, '-B', $xpdfBuild,
    '-DNO_FONTCONFIG=ON', '-DCMAKE_DISABLE_FIND_PACKAGE_Qt5Widgets=TRUE', '-DCMAKE_DISABLE_FIND_PACKAGE_Qt6Widgets=TRUE',
    "-DFREETYPE_INCLUDE_DIR_ft2build=$prefix/include/freetype2", "-DFREETYPE_INCLUDE_DIR_freetype_freetype=$prefix/include/freetype2",
    "-DFREETYPE_LIBRARY=$prefix/lib/libfreetype.a", "-DFREETYPE_OTHER_LIBS=$zlibBuild/libzlibstatic.a",
    "-DTTK_PNG_ARCHIVE=$($pngLibrary.FullName)", "-DTTK_PNG_HEADERS=$pngBuild") + $configure + $zlib)
Invoke-CMake @('--build', $xpdfBuild, '--target', 'pdfinfo', 'pdftotext', 'pdftopng', '--parallel', $Jobs)

$notices = Join-Path $output 'licenses'
New-Item -ItemType Directory -Force -Path $notices | Out-Null
foreach ($name in @('pdfinfo', 'pdftotext', 'pdftopng')) {
    $exe = Join-Path $xpdfBuild "xpdf/$name.exe"
    if (!(Test-Path -LiteralPath $exe)) { throw "Xpdf command not built: $exe" }
    & "$toolchain/$triple-strip.exe" --strip-debug $exe
    if ($LASTEXITCODE) { throw "Failed to strip debug symbols: $exe" }
    Copy-Item -LiteralPath $exe -Destination $output -Force
}
Copy-Item -LiteralPath "$xpdfSource/README", "$xpdfSource/COPYING", "$xpdfSource/COPYING3", "$xpdfSource/doc" -Destination $notices -Recurse -Force
Copy-Item -LiteralPath "$ftSource/docs/FTL.TXT", "$ftSource/docs/GPLv2.TXT" -Destination $notices -Force
Copy-Item -LiteralPath "$pngSource/LICENSE" -Destination (Join-Path $notices 'libpng-LICENSE.txt') -Force
Copy-Item -LiteralPath "$zlibSource/LICENSE" -Destination (Join-Path $notices 'zlib-LICENSE.txt') -Force
$assets | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $notices 'source-manifest.json') -Encoding utf8
@'
Xpdf 4.06 command-line tools built by tools/build-xpdf.ps1 with FreeType 2.14.3, libpng 1.6.59 and zlib 1.3.1.
This build uses the GPL version 2 option for Xpdf and FreeType.
'@ |
    Set-Content -LiteralPath (Join-Path $notices 'build-info.txt') -Encoding utf8
Get-ChildItem -LiteralPath $output -Filter '*.exe' | Select-Object Name, Length
