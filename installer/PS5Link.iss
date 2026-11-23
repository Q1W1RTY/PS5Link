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

[Files]
Source: "{#PackageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\PS5Link"; Filename: "{app}\PS5LinkLauncher.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\PS5Link"; Filename: "{app}\PS5LinkLauncher.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\PS5LinkLauncher.exe"; Description: "{cm:LaunchProgram,PS5Link}"; Flags: nowait postinstall skipifsilent unchecked
