#define Payload GetEnv("PRUSA_INSTALL_PAYLOAD")
#define Output GetEnv("PRUSA_INSTALL_OUTPUT")
#define InstallerVersion "2.9.7.8"
#if Payload == "" || Output == ""
  #error PRUSA_INSTALL_PAYLOAD and PRUSA_INSTALL_OUTPUT must be set.
#endif

[Setup]
; Keep the AppId, privileges and install mode from the first installer for in-place upgrades.
AppId={{CD88DF34-AB88-423B-870D-4DA3AC903830}
AppName=PrusaSlicer Three-Point Circle
AppVersion={#InstallerVersion}
VersionInfoVersion={#InstallerVersion}
VersionInfoProductVersion={#InstallerVersion}
DefaultDirName={localappdata}\Programs\PrusaSlicer Three-Point Circle
DefaultGroupName=PrusaSlicer Three-Point Circle
UsePreviousAppDir=yes
DisableDirPage=auto
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
UninstallDisplayIcon={app}\prusa-slicer.exe
CloseApplications=yes
RestartApplications=no
OutputDir={#Output}
OutputBaseFilename=PrusaSlicer-{#InstallerVersion}-Three-Point-Circle-x64-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#Payload}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\PrusaSlicer Three-Point Circle"; Filename: "{app}\prusa-slicer.exe"; WorkingDir: "{app}"
