#ifndef MyAppName
  #define MyAppName "Ladybird Agentic"
#endif
#ifndef MyAppVersion
  #define MyAppVersion "0.1.0-agentic"
#endif
#ifndef StageDir
  #define StageDir "D:\ladybird\Build\packages-windows\stage\ladybird-0.1.0-agentic-Windows-x86_64"
#endif
#ifndef OutDir
  #define OutDir "D:\ladybird\Build\packages-windows"
#endif
#ifndef IconFile
  #define IconFile "D:\ladybird\Build\packages-windows\ladybird.ico"
#endif

[Setup]
AppId={{7E8C3B4E-2B2F-4F29-9A63-1D6B688E2B01}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=Ladybird Agentic
AppPublisherURL=https://browser.automators.work
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutDir}
OutputBaseFilename=ladybird-{#MyAppVersion}-Windows-x86_64-setup
SetupIconFile={#IconFile}
UninstallDisplayIcon={app}\ladybird.ico
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
DisableProgramGroupPage=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\bin\Ladybird.exe"; IconFilename: "{app}\ladybird.ico"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\bin\Ladybird.exe"; IconFilename: "{app}\ladybird.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\bin\Ladybird.exe"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
