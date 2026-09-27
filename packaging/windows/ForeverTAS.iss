[Setup]
AppId=dev.skycrafter.forevertas
AppName=ForeverTAS
AppVersion={#AppVersion}
AppPublisher=Skycrafter-dev
AppPublisherURL=https://github.com/Skycrafter-dev/ForeverTAS
DefaultDirName={localappdata}\Programs\ForeverTAS
DefaultGroupName=ForeverTAS
UninstallDisplayIcon={app}\ForeverTAS.exe
PrivilegesRequired=lowest
ArchitecturesAllowed=x64
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseFilename}
Compression=lzma2/fast
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion
Source: "{#AddBackslash(SourcePath)}installed.marker"; DestDir: "{app}"; DestName: ".forevertas-installed"; Flags: ignoreversion

[InstallDelete]
Type: files; Name: "{app}\amdhip64*.dll"
Type: files; Name: "{app}\cudart64_*.dll"
Type: files; Name: "{app}\nvrtc64_*.dll"
Type: files; Name: "{app}\nvrtc-builtins64_*.dll"
Type: files; Name: "{app}\nvJitLink_*.dll"

[Icons]
Name: "{autoprograms}\ForeverTAS"; Filename: "{app}\ForeverTAS.exe"
Name: "{autodesktop}\ForeverTAS"; Filename: "{app}\ForeverTAS.exe"; Tasks: desktopicon

[Tasks]
Name: desktopicon; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"

[Run]
Filename: "{app}\ForeverTAS.exe"; Description: "Launch ForeverTAS"; Flags: nowait postinstall skipifsilent
Filename: "{app}\ForeverTAS.exe"; Flags: nowait skipifnotsilent
