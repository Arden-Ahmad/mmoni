; Mmoni Windows Installer Script (Inno Setup 6)
; Build via: powershell -ExecutionPolicy Bypass -File scripts\build-installer.ps1

#ifndef MyAppVersion
  #define MyAppVersion "1.0.0"
#endif

#ifndef PublishDir
  #define PublishDir "..\publish\win-x64-standalone"
#endif

#ifndef InstallerFlavor
  #define InstallerFlavor "standalone"
#endif

#ifndef OutputDir
  #define OutputDir "..\dist"
#endif

#define MyAppName "Mmoni"
#define MyAppPublisher "Mmoni"
#define MyAppExeName "Mmoni.exe"

[Setup]
; Stable AppId so upgrades replace existing installations cleanly in Control Panel / Installed Apps.
AppId={{8F6D4A1E-7C2B-4B9E-9A5F-2E4D1C8B3A70}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
AllowNoIcons=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=Mmoni-Setup-{#MyAppVersion}-win-x64-{#InstallerFlavor}
SetupIconFile=..\Assets\Mmoni.ico
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
VersionInfoVersion={#MyAppVersion}.0
VersionInfoProductName={#MyAppName}
VersionInfoDescription={#MyAppName} Setup ({#InstallerFlavor})

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "autostart"; Description: "Start Mmoni automatically when Windows starts (Administrator mode, no UAC prompt)"; GroupDescription: "Windows Startup:"; Flags: checkedonce
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#PublishDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Parameters: "--enable-startup"; Flags: runhidden waituntilterminated; Tasks: autostart
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent runascurrentuser

[UninstallRun]
Filename: "{app}\{#MyAppExeName}"; Parameters: "--disable-startup"; Flags: runhidden waituntilterminated; RunOnceId: "MmoniDisableStartup"

[Code]
procedure StopRunningMmoni(const AppDir: string);
var
  ExePath: string;
  ResultCode: Integer;
begin
  ExePath := AddBackslash(AppDir) + '{#MyAppExeName}';
  if FileExists(ExePath) then
  begin
    // Ask the running instance to dispose hardware sensors and tray icon cleanly.
    Exec(ExePath, '--exit', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;

  // Fallback in case an older instance without --exit is still running.
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#MyAppExeName}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

function IsDotNet10DesktopInstalled(): Boolean;
var
  FindRec: TFindRec;
  SearchPath: string;
begin
  Result := False;
  SearchPath := ExpandConstant('{pf}\dotnet\shared\Microsoft.WindowsDesktop.App\10.*');
  if FindFirst(SearchPath, FindRec) then
  begin
    try
      repeat
        if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
        begin
          Result := True;
          Break;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

function InitializeSetup(): Boolean;
var
  Flavor: string;
  Answer: Integer;
begin
  Result := True;
  Flavor := '{#InstallerFlavor}';

  if (CompareText(Flavor, 'net10') = 0) and (not IsDotNet10DesktopInstalled()) then
  begin
    if not WizardSilent() then
    begin
      Answer := MsgBox(
        'Microsoft .NET 10 Desktop Runtime (x64) was not detected on this PC.' + #13#10 + #13#10 +
        'You can either install the .NET 10 Desktop Runtime from Microsoft, or use the Mmoni Standalone installer instead.' + #13#10 + #13#10 +
        'Do you want to continue installing anyway?',
        mbConfirmation, MB_YESNO);
      Result := (Answer = IDYES);
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): string;
begin
  StopRunningMmoni(ExpandConstant('{app}'));
  Result := '';
end;

function InitializeUninstall(): Boolean;
begin
  StopRunningMmoni(ExpandConstant('{app}'));
  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
  SettingsDir: string;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    // Safety net: ensure scheduled task is always removed even if the exe failed to run.
    Exec(ExpandConstant('{sys}\schtasks.exe'), '/Delete /TN "Mmoni" /F', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

    SettingsDir := ExpandConstant('{localappdata}\Mmoni');
    if DirExists(SettingsDir) and (not UninstallSilent()) then
    begin
      if MsgBox('Do you also want to remove your saved Mmoni settings and overlay position?', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
      begin
        DelTree(SettingsDir, True, True, True);
      end;
    end;
  end;
end;
