#define MyAppName "CyzonVSModManager"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "Cyzon"
#define MyAppExeName "CyzonVSMM.exe"

[Setup]
AppId={{A87B9B2D-82B1-4C31-9C12-123456789ABC}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}

DefaultDirName={localappdata}\Programs\CyzonVSModManager
DefaultGroupName={#MyAppName}

OutputDir=installer
OutputBaseFilename=CyzonVSModManager-Setup

SetupIconFile=icon.ico
UninstallDisplayIcon={app}\icon.ico

PrivilegesRequired=lowest
Uninstallable=yes

CloseApplications=yes
RestartApplications=no

Compression=lzma2
SolidCompression=yes
WizardStyle=modern

ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
Source: "CyzonVSMM.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "SDL3.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "icon.ico"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\CyzonVSModManager"; Filename: "{app}\CyzonVSMM.exe"; IconFilename: "{app}\icon.ico"
Name: "{autodesktop}\CyzonVSModManager"; Filename: "{app}\CyzonVSMM.exe"; IconFilename: "{app}\icon.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\CyzonVSMM.exe"; Description: "Launch CyzonVSModManager"; Flags: nowait postinstall skipifsilent