#ifndef AppVersion
  #define AppVersion "0.22.2"
#endif
#ifndef StageDir
  #error StageDir is required
#endif
#ifndef OutputDir
  #error OutputDir is required
#endif
[Setup]
AppId={{85969EDB-C1E5-43A3-AE9E-820D37757C8A}
AppName=Burnos
AppVersion={#AppVersion}
AppPublisher=Burnos contributors
AppPublisherURL=https://burnos.app
AppSupportURL=https://github.com/onfire7777/Burnos/issues
AppUpdatesURL=https://github.com/onfire7777/Burnos/releases
DefaultDirName={localappdata}\Programs\Burnos
DefaultGroupName=Burnos
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
WizardStyle=modern
WizardSizePercent=110
SetupIconFile=..\burnos.ico
UninstallDisplayIcon={app}\Burnos.exe
LicenseFile={#StageDir}\LICENSE-WINDOWS.txt
InfoAfterFile={#StageDir}\WELCOME.txt
OutputDir={#OutputDir}
OutputBaseFilename=Burnos-{#AppVersion}-Setup-x64
Compression=lzma2
SolidCompression=yes
CloseApplications=yes
CloseApplicationsFilter=Burnos.exe,mneme-mcp.exe
RestartApplications=no
UninstallDisplayName=Burnos
VersionInfoDescription=Burnos Windows Setup
VersionInfoVersion={#AppVersion}
[Tasks]
Name: desktopicon; Description: "Create a &desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked
[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
[Icons]
Name: "{group}\Burnos"; Filename: "{app}\Burnos.exe"; WorkingDir: "{app}"
Name: "{group}\Getting started"; Filename: "{app}\WELCOME.txt"
Name: "{group}\Burnos website"; Filename: "https://burnos.app"
Name: "{autodesktop}\Burnos"; Filename: "{app}\Burnos.exe"; WorkingDir: "{app}"; Tasks: desktopicon
[Run]
Filename: "{app}\Burnos.exe"; Description: "Launch Burnos"; Flags: nowait postinstall skipifsilent unchecked
