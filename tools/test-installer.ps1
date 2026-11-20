[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Installer,
    [Parameter(Mandatory)] [string] $Fixtures,
    [string] $Work = $(if ($env:TTK_BUILD_ROOT) { $env:TTK_BUILD_ROOT } else { Join-Path $env:LOCALAPPDATA 'ttkconverter-build' }),
    [string] $Python = 'python.exe',
    [switch] $UseDefaultDirectory
)

$ErrorActionPreference = 'Stop'
$Installer = (Get-Item -LiteralPath $Installer).FullName
$Fixtures = (Get-Item -LiteralPath $Fixtures).FullName
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\ttkConverter_is1'
if (Test-Path -LiteralPath $uninstallKey) {
    throw 'Installer tests require a profile without a registered ttkConverter 1.1 or later installation.'
}
$test = [IO.Path]::GetFileNameWithoutExtension($Installer) + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$directory = if ($UseDefaultDirectory) {
    Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Programs\ttkConverter'
} else {
    [IO.Path]::GetFullPath((Join-Path $Work "install-test/$test"))
}
if (Test-Path -LiteralPath $directory) {
    throw "Installer test directory must be absent at the start: $directory"
}
$silent = @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART')
$installArgs = if ($UseDefaultDirectory) { $silent } else { $silent + "/DIR=`"$directory`"" }

try {
    $installed = Start-Process -FilePath $Installer -ArgumentList $installArgs `
        -WindowStyle Hidden -Wait -PassThru
    if ($installed.ExitCode) { throw "Installation failed ($($installed.ExitCode))" }
    $app = Join-Path $directory 'ttkConverter.exe'
    if (!(Test-Path -LiteralPath $app -PathType Leaf)) { throw 'Application was not installed' }
    if ($UseDefaultDirectory) {
        $shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'ttkConverter\ttkConverter.lnk'
        if (!(Test-Path -LiteralPath $shortcut -PathType Leaf)) { throw 'Default Start Menu shortcut is missing' }
        $linkReader = New-Object -ComObject WScript.Shell
        if ($linkReader.CreateShortcut($shortcut).TargetPath -ne $app) {
            throw 'Default Start Menu shortcut targets the wrong executable'
        }
        Write-Output "PASS default folder and Start Menu shortcut: $directory"
    }
    $size = (Get-ChildItem -LiteralPath $directory -Recurse -File | Measure-Object Length -Sum).Sum
    Write-Output ('Installed bytes: {0} ({1:N1} MiB)' -f $size,($size/1MB))

    $gui = Start-Process -FilePath $app -WindowStyle Hidden -PassThru
    try {
        if (!$gui.WaitForInputIdle(8000) -or $gui.HasExited) { throw 'GUI startup failed' }
        Write-Output 'PASS GUI startup'
    } finally {
        if (!$gui.HasExited) { Stop-Process -Id $gui.Id }
    }

    & $Python -X utf8 (Join-Path $PSScriptRoot 'smoke-test.py') --app $app --fixtures $Fixtures --all-formats
    if ($LASTEXITCODE) { throw "Conversion checks failed ($LASTEXITCODE)" }
} finally {
    if (Test-Path -LiteralPath $directory -PathType Container) {
        $uninstaller = Get-ChildItem -LiteralPath $directory -Filter 'unins*.exe' -File | Select-Object -First 1
        if ($uninstaller) {
            $removed = Start-Process -FilePath $uninstaller.FullName -ArgumentList $silent `
                -WindowStyle Hidden -Wait -PassThru
            if ($removed.ExitCode -or (Test-Path -LiteralPath (Join-Path $directory 'ttkConverter.exe'))) {
                throw "Test installation could not be removed: $directory"
            }
            Write-Output 'PASS uninstall'
        }
    }
}
