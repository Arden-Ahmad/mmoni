; Mmoni Native v2 Windows Installer Script (Inno Setup 6)
; Same AppId as v1.0.0 for seamless drop-in upgrade.

#ifndef NativeExePath
  #define NativeExePath "..\build\release\Mmoni.exe"
#endif

#ifndef MyAppVersion
  #define MyAppVersion GetStringFileInfo(NativeExePath, "ProductVersion")
#endif

#ifndef OutputDir
  #define OutputDir "..\..\dist"
#endif

#define MyAppName "Mmoni"
#define MyAppPublisher "Mmoni"
#define MyAppExeName "Mmoni.exe"

[Setup]
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
MinVersion=10.0.22621
OutputDir={#OutputDir}
OutputBaseFilename=Mmoni-Setup-{#MyAppVersion}-win-x64-native
SetupIconFile=..\..\Assets\Mmoni.ico
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
VersionInfoVersion={#MyAppVersion}.0
VersionInfoProductName={#MyAppName}
VersionInfoDescription={#MyAppName} Native Setup

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "autostart"; Description: "Start Mmoni automatically when Windows starts (Administrator mode, no UAC prompt)"; GroupDescription: "Windows Startup:"; Flags: checkedonce
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "Additional Icons:"; Flags: unchecked

[Files]
Source: "{#NativeExePath}"; DestDir: "{app}"; DestName: "{#MyAppExeName}"; Flags: ignoreversion

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
  PsCmd: string;
begin
  ExePath := AddBackslash(AppDir) + '{#MyAppExeName}';
  if FileExists(ExePath) then
  begin
    // Ask the running instance to dispose hardware sensors and tray icon cleanly.
    if Exec(ExePath, '--exit', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then
    begin
      Sleep(350);
    end;

    // Fallback: only terminate processes whose executable path matches this installation directory.
    PsCmd := '-NoProfile -ExecutionPolicy Bypass -Command "Get-Process -Name Mmoni -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq ''' + ExePath + ''' } | Stop-Process -Force -ErrorAction SilentlyContinue"';
    Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), PsCmd, '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
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
