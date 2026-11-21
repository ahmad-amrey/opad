; Windows installer for OPAD (Inno Setup 6): OPAD-<version>-windows-x64-setup.exe from the single-file exes of the
; windows-static presets (build/windows-static/single, `cmake --workflow --preset windows-single`; CI:
; windows-static-ucrt64).
;
;   iscc /DAppVersion=0.9.0 installer\opad.iss
;
; Per user, no administrator: the program goes to %LOCALAPPDATA%\Programs\OPAD, a Start menu entry (and, if ticked, a
; desktop icon) and an uninstaller in Settings > Apps.
;
; The user's settings are never touched. OPAD keeps them, with its cache, recovery snapshots and templates, in the
; user's own folders (the registry, HKCU\Software\opad\OPAD, %LOCALAPPDATA%\opad and %APPDATA%\opad: app/AppData.hpp), never beside
; the program; this script installs only the exes and the notices, so an update replaces those files and nothing else,
; and the uninstaller removes only what it installed. The Build workflow checks both (install, update, uninstall).
;
; AppId names this program to Windows across versions: never change it, or an update installs a second copy beside the
; first instead of updating it.

#ifndef AppVersion
  #define AppVersion "0.9.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\build\windows-static\single"
#endif
#ifndef OutputDir
  #define OutputDir "..\build\windows-static\installer"
#endif

[Setup]
AppId={{AC06BE6D-6FAC-4EA0-8655-97BEBEF52662}
AppName=OPAD
AppVersion={#AppVersion}
AppVerName=OPAD {#AppVersion}
AppPublisher=OPAD
AppPublisherURL=https://github.com/ahmad-amrey/opad
AppSupportURL=https://github.com/ahmad-amrey/opad/issues
AppUpdatesURL=https://github.com/ahmad-amrey/opad/releases
VersionInfoVersion={#AppVersion}
PrivilegesRequired=lowest
DefaultDirName={autopf}\OPAD
DefaultGroupName=OPAD
DisableProgramGroupPage=yes
UsePreviousAppDir=yes
LicenseFile=..\LICENSE
SetupIconFile=..\app\res\opad.ico
UninstallDisplayIcon={app}\OPAD.exe
UninstallDisplayName=OPAD
OutputDir={#OutputDir}
OutputBaseFilename=OPAD-{#AppVersion}-windows-x64-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; An OPAD still open would hold its exe: setup offers to close it (the user saves first), and does not restart it.
CloseApplications=yes
RestartApplications=no

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\OPAD-{#AppVersion}-windows-x64.exe"; DestDir: "{app}"; DestName: "OPAD.exe"; Flags: ignoreversion
Source: "{#SourceDir}\opad-cli-{#AppVersion}-windows-x64.exe"; DestDir: "{app}"; DestName: "opad-cli.exe"; Flags: ignoreversion
Source: "{#SourceDir}\THIRD-PARTY-NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\OPAD"; Filename: "{app}\OPAD.exe"
Name: "{autodesktop}\OPAD"; Filename: "{app}\OPAD.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\OPAD.exe"; Description: "{cm:LaunchProgram,OPAD}"; Flags: nowait postinstall skipifsilent

; No [UninstallDelete] and no [InstallDelete]: nothing of the user's is ever removed or replaced.
