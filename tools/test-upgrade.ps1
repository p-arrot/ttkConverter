[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Installer,
    [Parameter(Mandatory)] [string] $PreviousInstaller,
    [Parameter(Mandatory)] [string] $LegacyInstaller,
    [ValidatePattern('^[A-Za-z0-9-]+$')] [string] $AppId = 'ttkConverter',
    [string] $Work = $(if ($env:TTK_BUILD_ROOT) { $env:TTK_BUILD_ROOT } else { Join-Path $env:LOCALAPPDATA 'ttkconverter-build' })
)

$ErrorActionPreference = 'Stop'
$Installer = (Get-Item -LiteralPath $Installer).FullName
$PreviousInstaller = (Get-Item -LiteralPath $PreviousInstaller).FullName
$LegacyInstaller = (Get-Item -LiteralPath $LegacyInstaller).FullName
$newVersion = (Get-Item -LiteralPath $Installer).VersionInfo.ProductVersion
$newVersion = ([version] $newVersion).ToString(3)
$keyRoot = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall'
$currentKey = Join-Path $keyRoot "${AppId}_is1"
$legacyKey = Join-Path $keyRoot "$AppId-Win10-11-x64_is1"
foreach ($key in @($currentKey, $legacyKey, (Join-Path $keyRoot "$AppId-Win7-SP1-x64_is1"),
                  (Join-Path $keyRoot "$AppId-Win7-SP1-x86_is1"))) {
    if (Test-Path -LiteralPath $key) { throw "Run upgrade tests in a profile with no installed ttkConverter: $key" }
}

$sandbox = [IO.Path]::GetFullPath((Join-Path $Work ('upgrade-test/' + [Guid]::NewGuid().ToString('N'))))
New-Item -ItemType Directory -Path $sandbox -Force | Out-Null
$silent = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')

function RunSetup([string] $Path, [string] $Label, [string] $Directory, [string[]] $Extra) {
    $installArguments = $silent + "/LOG=`"$sandbox/$Label.log`""
    if ($Directory) { $installArguments += "/DIR=`"$Directory`"" }
    if ($Extra) { $installArguments += $Extra }
    (Start-Process -FilePath $Path -ArgumentList $installArguments -WindowStyle Hidden -Wait -PassThru).ExitCode
}

function AssertInstall([string] $Dir, [string] $Version) {
    $app = Join-Path $Dir 'ttkConverter.exe'
    if (!(Test-Path -LiteralPath $app) -or (Get-Item -LiteralPath $app).VersionInfo.ProductVersion -ne $Version) {
        throw "Expected ttkConverter $Version at $Dir"
    }
}

function RemoveFixtureInstall([string] $Key) {
    if (!(Test-Path -LiteralPath $Key)) { return }
    $entry = Get-ItemProperty -LiteralPath $Key
    $dir = [IO.Path]::GetFullPath($entry.InstallLocation)
    if (!$dir.StartsWith($sandbox + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to uninstall a directory outside the test workspace: $dir"
    }
    $program = $entry.UninstallString.Trim('"')
    if ([IO.Path]::GetDirectoryName($program) -ne $dir.TrimEnd('\', '/') -or
        [IO.Path]::GetFileName($program) -notmatch '^unins\d{3}\.exe$') {
        throw "Unexpected fixture uninstaller path: $program"
    }
    $exitCode = (Start-Process -FilePath $program -ArgumentList $silent -WindowStyle Hidden -Wait -PassThru).ExitCode
    if ($exitCode -or (Test-Path -LiteralPath $Key)) { throw "Failed to remove the fixture at $dir" }
}

try {
    $stable = Join-Path $sandbox 'stable'
    if ((RunSetup $PreviousInstaller 'previous' $stable @()) -ne 0) { throw 'Could not install the previous version' }
    $previousVersion = (Get-ItemProperty -LiteralPath $currentKey).DisplayVersion
    AssertInstall $stable $previousVersion
    $keep = Join-Path $stable 'previous-result.txt'
    Set-Content -LiteralPath $keep -Value 'existing conversion result' -Encoding ascii
    if ((RunSetup $Installer 'wrong-upgrade-path' (Join-Path $sandbox 'wrong') @()) -eq 0) {
        throw 'Moving an existing stable AppId without uninstall unexpectedly succeeded'
    }
    AssertInstall $stable $previousVersion
    if ((RunSetup $Installer 'in-place-upgrade' '' @()) -ne 0) { throw 'In-place upgrade failed' }
    AssertInstall $stable $newVersion
    if ((Get-ItemProperty -LiteralPath $currentKey).DisplayVersion -ne $newVersion -or
        (Get-Content -LiteralPath $keep) -ne 'existing conversion result') {
        throw 'In-place upgrade did not preserve the install or user output'
    }
    RemoveFixtureInstall $currentKey
    if (!(Test-Path -LiteralPath $keep)) { throw 'Updated uninstaller removed user output' }
    Write-Output 'PASS existing stable installation upgraded in place and preserved user output'

    $target = Join-Path $sandbox 'migration'
    $legacy = Join-Path $target 'Win10-11-x64'
    if ((RunSetup $LegacyInstaller 'legacy' $legacy @()) -ne 0) { throw 'Could not install the legacy fixture' }
    AssertInstall $legacy '1.0.0'
    $keepLegacy = Join-Path $legacy 'previous-result.txt'
    Set-Content -LiteralPath $keepLegacy -Value 'legacy conversion result' -Encoding ascii
    if ((RunSetup $Installer 'legacy-overlap' $legacy @('/MIGRATELEGACY=1')) -eq 0) {
        throw 'Installing the new AppId over the old unmanaged directory unexpectedly succeeded'
    }
    AssertInstall $legacy '1.0.0'
    if ((RunSetup $Installer 'keep-legacy' $target @()) -ne 0) { throw 'New install alongside legacy failed' }
    AssertInstall $target $newVersion
    AssertInstall $legacy '1.0.0'
    RemoveFixtureInstall $currentKey
    if ((RunSetup $Installer 'migrate-legacy' $target @('/MIGRATELEGACY=1')) -ne 0) { throw 'Legacy migration failed' }
    AssertInstall $target $newVersion
    if ((Test-Path -LiteralPath $legacyKey) -or (Test-Path -LiteralPath (Join-Path $legacy 'ttkConverter.exe')) -or
        (Get-Content -LiteralPath $keepLegacy) -ne 'legacy conversion result') {
        throw 'Legacy migration did not remove only the old application'
    }
    Write-Output 'PASS legacy overlap guard, silent keep, and opt-in migration preserving user output'
} finally {
    RemoveFixtureInstall $currentKey
    RemoveFixtureInstall $legacyKey
}
