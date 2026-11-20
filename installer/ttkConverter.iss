#ifndef StageDir
  #error StageDir must point to the staged app.
#endif
#ifndef OutputDir
  #error OutputDir must point to the release directory.
#endif
#ifndef Edition
  #error Edition must be defined.
#endif
#ifndef AppIcon
  #error AppIcon must point to the ttkConverter ICO file.
#endif

#ifndef MyVersion
  #error MyVersion must match the application version.
#endif

[Setup]
AppId=ttkConverter
AppName=ttkConverter
AppVersion={#MyVersion}
AppVerName=ttkConverter {#MyVersion}
VersionInfoVersion={#MyVersion}
VersionInfoProductName=ttkConverter
DefaultDirName={localappdata}\Programs\ttkConverter
DefaultGroupName=ttkConverter
OutputDir={#OutputDir}
OutputBaseFilename=ttkConverter-{#MyVersion}-{#Edition}-Setup
SetupIconFile={#AppIcon}
UninstallDisplayIcon={app}\ttkConverter.exe
PrivilegesRequired=lowest
Compression=lzma2/normal
SolidCompression=yes
WizardStyle=modern
DisableProgramGroupPage=yes
CloseApplications=no
RestartApplications=no
SetupLogging=yes
#if Edition == "Win10-11-x64"
MinVersion=10.0
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
#elif Edition == "Win7-SP1-x64"
MinVersion=6.1sp1
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
#else
MinVersion=6.1sp1
ArchitecturesAllowed=x86os
#endif

[Languages]
#ifdef ChineseMessages
Name: "chinesesimplified"; MessagesFile: "{#ChineseMessages}"
#else
Name: "english"; MessagesFile: "compiler:Default.isl"
#endif

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\ttkConverter"; Filename: "{app}\ttkConverter.exe"
Name: "{userdesktop}\ttkConverter"; Filename: "{app}\ttkConverter.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\ttkConverter.exe"; Description: "打开 ttkConverter"; Flags: postinstall nowait skipifsilent

[Code]
function LegacyInstallUsesTarget(const Edition, Target: String): Boolean;
var
  Key, Previous: String;
begin
  Key := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\ttkConverter-' + Edition + '_is1';
  Result := RegQueryStringValue(HKCU, Key, 'InstallLocation', Previous) and
    (CompareText(AddBackslash(Previous), AddBackslash(Target)) = 0) and
    FileExists(AddBackslash(Previous) + 'ttkConverter.exe');
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Target: String;
begin
  Target := ExpandConstant('{app}');
  Result := '';
  if LegacyInstallUsesTarget('Win10-11-x64', Target) or
     LegacyInstallUsesTarget('Win7-SP1-x64', Target) or
     LegacyInstallUsesTarget('Win7-SP1-x86', Target) then
    Result := '这个目录里有已登记的 ttkConverter 1.0.0。请更换目录，或先卸载旧版，再使用此目录。';
end;
