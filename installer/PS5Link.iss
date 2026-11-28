; SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#ifndef PackageDir
  #error PackageDir must point to the staged PS5Link files.
#endif
#ifndef PackageVersion
  #error PackageVersion must be supplied by build-installer.ps1.
#endif

[Setup]
AppId={{9AA0F444-6856-499A-B2BE-8BD7D47238E6}
AppName=PS5Link
AppVersion={#PackageVersion}
AppPublisher=PS5Link
AppPublisherURL=https://github.com/Q1W1RTY/PS5Link
AppSupportURL=https://github.com/Q1W1RTY/PS5Link/issues
AppUpdatesURL=https://github.com/Q1W1RTY/PS5Link/releases
Uninstallable=not IsPortableUpdate
CreateUninstallRegKey=not IsPortableUpdate
UninstallDisplayIcon={app}\PS5LinkLauncher.exe
DefaultDirName={localappdata}\Programs\PS5Link
DefaultGroupName=PS5Link
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.19041
CloseApplications=yes
CloseApplicationsFilter=PS5Link.exe,PS5LinkLauncher.exe
RestartApplications=no
DisableProgramGroupPage=yes
DisableWelcomePage=no
SetupIconFile=..\gui\assets\ps5link\PS5Link.ico
WizardStyle=modern dark polar includetitlebar hidebevels
WizardImageFile=wizard-panel.png
WizardSmallImageFile=..\gui\assets\ps5link\PS5Link.png
Compression=lzma2/max
SolidCompression=yes
OutputBaseFilename=PS5Link-{#PackageVersion}-win-x64-setup
VersionInfoVersion={#PackageVersion}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; Flags: unchecked

[InstallDelete]
Type: filesandordirs; Name: "{app}\LICENSES\MSYS2"

[Files]
Source: "{#PackageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\PS5Link"; Filename: "{app}\PS5LinkLauncher.exe"; WorkingDir: "{app}"; Check: not IsPortableUpdate
Name: "{autodesktop}\PS5Link"; Filename: "{app}\PS5LinkLauncher.exe"; WorkingDir: "{app}"; Tasks: desktopicon; Check: not IsPortableUpdate

[Run]
Filename: "{app}\PS5LinkLauncher.exe"; WorkingDir: "{app}"; Flags: nowait; Check: IsAutomaticUpdate
Filename: "{app}\PS5LinkLauncher.exe"; Description: "{cm:LaunchProgram,PS5Link}"; Flags: nowait postinstall skipifsilent unchecked

[Code]
function OpenProcess(Access: Cardinal; InheritHandle: Boolean; ProcessId: Cardinal): THandle;
  external 'OpenProcess@kernel32.dll stdcall';
function WaitForSingleObject(Handle: THandle; Milliseconds: Cardinal): Cardinal;
  external 'WaitForSingleObject@kernel32.dll stdcall';
function CloseHandle(Handle: THandle): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';

function UpdateProcessId: Cardinal;
var
  Parsed: Int64;
begin
  Parsed := StrToInt64Def(ExpandConstant('{param:PS5LINKUPDATEPID|0}'), 0);
  if (Parsed > 0) and (Parsed <= $FFFFFFFF) then
    Result := Cardinal(Parsed)
  else
    Result := 0;
end;

function IsAutomaticUpdate: Boolean;
begin
  Result := UpdateProcessId > 0;
end;

function IsPortableUpdate: Boolean;
var
  I: Integer;
begin
  Result := ExpandConstant('{param:PS5LINKPORTABLE|0}') = '1';
  if Result or not IsAutomaticUpdate then
    Exit;

  { PS5Link 1.10.6 sent only /NOICONS for portable updates. }
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), '/NOICONS') = 0 then
    begin
      Result := True;
      Exit;
    end;
end;

function InitializeSetup: Boolean;
var
  Launcher: THandle;
begin
  Result := True;
  if not IsAutomaticUpdate then
    Exit;

  { Wait for the launcher to save its settings and release the executable. }
  Launcher := OpenProcess($00100000, False, UpdateProcessId);
  if Launcher <> 0 then
  begin
    Result := WaitForSingleObject(Launcher, 15000) = 0;
    CloseHandle(Launcher);
  end;
end;
