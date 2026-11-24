#define Payload GetEnv("PRUSA_INSTALL_PAYLOAD")
#define Output GetEnv("PRUSA_INSTALL_OUTPUT")
#include "project-branding.iss"
#if Payload == "" || Output == ""
  #error PRUSA_INSTALL_PAYLOAD and PRUSA_INSTALL_OUTPUT must be set.
#endif

[Setup]
; Keep the AppId, privileges and install mode from the first installer for in-place upgrades.
AppId={{CD88DF34-AB88-423B-870D-4DA3AC903830}
AppName={#ProjectName}
AppVersion={#ProjectVersion}
AppVerName={#ProjectName} {#ProjectVersion}
VersionInfoVersion={#ProjectVersion}
VersionInfoProductName={#ProjectName}
VersionInfoProductVersion={#ProjectVersion}
DefaultDirName={localappdata}\Programs\{#ProjectName}
DefaultGroupName={#ProjectName}
UsePreviousAppDir=yes
UsePreviousGroup=no
DisableDirPage=auto
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
UninstallDisplayIcon={app}\prusa-slicer.exe
UninstallDisplayName={#ProjectName} {#ProjectVersion}
CloseApplications=yes
RestartApplications=no
OutputDir={#Output}
OutputBaseFilename=PrusaSlicer-{#ProjectVersion}-Three-Point-Circle-x64-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#Payload}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#ProjectName}"; Filename: "{app}\prusa-slicer.exe"; WorkingDir: "{app}"; Comment: "{#ProjectName} {#ProjectVersion}"

[InstallDelete]
Type: files; Name: "{userprograms}\PrusaSlicer Three-Point Circle\PrusaSlicer Three-Point Circle.lnk"
