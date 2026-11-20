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
CloseApplications=yes
CloseApplicationsFilter=ttkConverter.exe
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
const
  UninstallRoot = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\';
  StableAppId = 'ttkConverter';
  ExeName = 'ttkConverter.exe';

type
  TRegisteredInstall = record
    Directory: String;
    ActualVersion: Int64;
    HasVersion: Boolean;
    Uninstaller: String;
  end;

var
  StableInstall: TRegisteredInstall;
  LegacyInstalls: array[0..2] of TRegisteredInstall;
  LegacyCandidate: Integer;
  TargetVersion: Int64;
  MigrationPage: TInputOptionWizardPage;
  MigrateAfterInstall: Boolean;

function LegacyEdition(Index: Integer): String;
begin
  case Index of
    0: Result := 'Win10-11-x64';
    1: Result := 'Win7-SP1-x64';
    2: Result := 'Win7-SP1-x86';
  end;
end;

function AbsoluteLocalPath(const Path: String): String;
var
  Normal: String;
begin
  Result := '';
  Normal := PathNormalizeSlashes(Trim(Path));
  if Length(Normal) < 3 then Exit;
  if (Normal[2] <> ':') or (Normal[3] <> '\') or PathHasInvalidCharacters(Normal, True) then Exit;
  Result := RemoveBackslashUnlessRoot(ExpandFileName(Normal));
end;

function VerifiedUninstaller(const AppId, Directory: String): String;
var
  Value, Name: String;
  I: Integer;
begin
  Result := '';
  if not RegQueryStringValue(HKCU, UninstallRoot + AppId + '_is1', 'UninstallString', Value) then Exit;
  Value := Trim(Value);
  if Length(Value) < 3 then Exit;
  if (Value[1] <> '"') or (Value[Length(Value)] <> '"') then Exit;
  Value := AbsoluteLocalPath(Copy(Value, 2, Length(Value) - 2));
  if (Value = '') or not PathSame(ExtractFileDir(Value), Directory) then Exit;
  Name := Lowercase(ExtractFileName(Value));
  if Length(Name) <> 12 then Exit;
  if (Copy(Name, 1, 5) <> 'unins') or (Copy(Name, 9, 4) <> '.exe') then Exit;
  for I := 6 to 8 do
    if (Name[I] < '0') or (Name[I] > '9') then Exit;
  if FileExists(Value) and FileExists(ChangeFileExt(Value, '.dat')) then Result := Value;
end;

procedure ReadRegisteredInstall(const AppId: String; var Install: TRegisteredInstall);
var
  Value, InstallDir, BinaryPath: String;
begin
  Install.Directory := '';
  Install.ActualVersion := 0;
  Install.HasVersion := False;
  Install.Uninstaller := '';
  if not RegQueryStringValue(HKCU, UninstallRoot + AppId + '_is1', 'InstallLocation', Value) then Exit;
  InstallDir := AbsoluteLocalPath(Value);
  if InstallDir = '' then Exit;
  BinaryPath := PathCombine(InstallDir, ExeName);
  if not FileExists(BinaryPath) then Exit;
  Install.Directory := InstallDir;
  Install.HasVersion := GetPackedVersion(BinaryPath, Install.ActualVersion);
  Install.Uninstaller := VerifiedUninstaller(AppId, InstallDir);
  if Install.HasVersion then
    Log('Detected ' + AppId + ' ' + VersionToStr(Install.ActualVersion) + ' at ' + InstallDir)
  else
    Log('Detected ' + AppId + ' with unreadable executable version at ' + InstallDir);
end;

function InitializeSetup: Boolean;
var
  I: Integer;
  LegacyVersion: Int64;
begin
  Result := StrToVersion('{#MyVersion}', TargetVersion);
  if not Result then Exit;
  ReadRegisteredInstall(StableAppId, StableInstall);
  LegacyCandidate := -1;
  LegacyVersion := PackVersionComponents(1, 0, 0, 0);
  for I := 0 to 2 do begin
    ReadRegisteredInstall(StableAppId + '-' + LegacyEdition(I), LegacyInstalls[I]);
    if (StableInstall.Directory = '') and LegacyInstalls[I].HasVersion and
       (ComparePackedVersion(LegacyInstalls[I].ActualVersion, LegacyVersion) = 0) and
       (ComparePackedVersion(LegacyInstalls[I].ActualVersion, TargetVersion) < 0) then begin
      if LegacyInstalls[I].Uninstaller = '' then
        Log('Legacy uninstall record failed validation; preserving ' + LegacyInstalls[I].Directory)
      else if (LegacyCandidate < 0) or (LegacyEdition(I) = '{#Edition}') then
        LegacyCandidate := I;
    end;
  end;
end;

procedure InitializeWizard;
begin
  if (LegacyCandidate >= 0) and not WizardSilent then begin
    MigrationPage := CreateInputOptionPage(wpSelectDir, '检测到旧版 ttkConverter',
      '已找到 ttkConverter 1.0.0。',
      '旧版位置  ' + LegacyInstalls[LegacyCandidate].Directory + #13#10 +
      '选择新版安装成功后如何处理这个旧版。', True, True);
    MigrationPage.Add('升级到新版，安装成功后卸载这个旧版');
    MigrationPage.Add('安装新版并保留这个旧版');
    MigrationPage.SelectedValueIndex := 0;
  end;
end;

function MigrationSelected: Boolean;
var
  I: Integer;
begin
  Result := False;
  if LegacyCandidate < 0 then Exit;
  if WizardSilent then begin
    for I := 1 to ParamCount do
      if CompareText(ParamStr(I), '/MIGRATELEGACY=1') = 0 then begin
        Result := True;
        Exit;
      end;
  end else if MigrationPage <> nil then
    Result := MigrationPage.SelectedValueIndex = 0;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpReady then begin
    if (StableInstall.HasVersion and (ComparePackedVersion(StableInstall.ActualVersion, TargetVersion) < 0)) or
       MigrationSelected then
      WizardForm.NextButton.Caption := '升级'
    else
      WizardForm.NextButton.Caption := SetupMessage(msgButtonInstall);
  end;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := '目标版本  ttkConverter {#MyVersion}' + NewLine + NewLine +
    '安装目录' + NewLine + Space + WizardDirValue;
  if StableInstall.HasVersion then
    Result := Result + NewLine + NewLine + '已安装版本  ' + VersionToStr(StableInstall.ActualVersion) +
      NewLine + Space + StableInstall.Directory;
  if LegacyCandidate >= 0 then begin
    Result := Result + NewLine + NewLine + '检测到旧版  ' +
      VersionToStr(LegacyInstalls[LegacyCandidate].ActualVersion) +
      NewLine + Space + LegacyInstalls[LegacyCandidate].Directory + NewLine + Space;
    if MigrationSelected then
      Result := Result + '新版安装成功后卸载此旧版'
    else
      Result := Result + '保留此旧版';
  end;
  if MemoTasksInfo <> '' then Result := Result + NewLine + NewLine + MemoTasksInfo;
end;

function LegacyPathConflicts(Index: Integer; const Target: String): Boolean;
var
  OldDirectory: String;
begin
  OldDirectory := LegacyInstalls[Index].Directory;
  Result := False;
  if OldDirectory = '' then Exit;
  if PathSame(OldDirectory, Target) or PathStartsWith(Target, AddBackslash(OldDirectory), True) then
    Result := True
  else if PathStartsWith(OldDirectory, AddBackslash(Target), True) then
    Result := not PathSame(OldDirectory, PathCombine(Target, LegacyEdition(Index)));
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Target: String;
  I: Integer;
begin
  MigrateAfterInstall := False;
  Target := AbsoluteLocalPath(ExpandConstant('{app}'));
  Result := '';
  if StableInstall.Directory <> '' then begin
    if not PathSame(Target, StableInstall.Directory) then
      Result := '请使用现有安装目录升级 ttkConverter。' + #13#10 + StableInstall.Directory
    else if not StableInstall.HasVersion then
      Result := '无法读取现有程序的版本。请核对安装目录，或先从系统应用列表卸载它。'
    else if ComparePackedVersion(StableInstall.ActualVersion, TargetVersion) > 0 then
      Result := '现有安装较新，请使用与它相同或更新的安装包。';
  end;
  for I := 0 to 2 do
    if (Result = '') and LegacyPathConflicts(I, Target) then
      Result := '所选目录与一个已登记的旧版安装重叠。请选择其他目录。' + #13#10 + LegacyInstalls[I].Directory;
  if Result <> '' then begin
    Log('Install blocked at ' + Target + '. ' + Result);
    Exit;
  end;
  MigrateAfterInstall := MigrationSelected;
  if MigrateAfterInstall then
    Log('Selected legacy migration from ' + LegacyInstalls[LegacyCandidate].Directory + ' to ' + Target)
  else if StableInstall.Directory <> '' then
    Log('Selected in-place setup at ' + Target)
  else
    Log('Selected fresh setup at ' + Target + '; legacy installs preserved');
end;

procedure RegisterExtraCloseApplicationsResources;
begin
  if MigrationSelected then
    RegisterExtraCloseApplicationsResource(False, PathCombine(LegacyInstalls[LegacyCandidate].Directory, ExeName));
end;

procedure MigrationFailed(const Reason: String);
begin
  Log('Legacy cleanup skipped or failed. ' + Reason);
  if not WizardSilent then
    MsgBox('新版已安装。旧版处理未完成，请在 Windows 应用列表检查旧版。', mbInformation, MB_OK);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Selected, Current: TRegisteredInstall;
  InstalledVersion: Int64;
  ExitCode: Integer;
begin
  if (CurStep <> ssPostInstall) or not MigrateAfterInstall or (LegacyCandidate < 0) then Exit;
  if not GetPackedVersion(ExpandConstant('{app}\' + ExeName), InstalledVersion) then begin
    MigrationFailed('Cannot verify the newly installed executable.');
    Exit;
  end;
  if ComparePackedVersion(InstalledVersion, TargetVersion) <> 0 then begin
    MigrationFailed('Installed executable version does not match the setup version.');
    Exit;
  end;
  Selected := LegacyInstalls[LegacyCandidate];
  ReadRegisteredInstall(StableAppId + '-' + LegacyEdition(LegacyCandidate), Current);
  if not Current.HasVersion or (Current.Uninstaller = '') or
     (ComparePackedVersion(Current.ActualVersion, Selected.ActualVersion) <> 0) or
     not PathSame(Current.Directory, Selected.Directory) or not PathSame(Current.Uninstaller, Selected.Uninstaller) then begin
    MigrationFailed('The selected legacy uninstall registration changed.');
    Exit;
  end;
  if not Exec(Current.Uninstaller, '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART', Current.Directory,
              SW_HIDE, ewWaitUntilTerminated, ExitCode) then
    MigrationFailed('Could not launch the legacy uninstaller. ' + IntToStr(ExitCode))
  else if ExitCode <> 0 then
    MigrationFailed('Legacy uninstaller returned ' + IntToStr(ExitCode))
  else
    Log('Legacy uninstall completed at ' + Current.Directory);
end;
