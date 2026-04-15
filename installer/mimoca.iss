#define MyAppName "MiMoCA"
#define MyAppVersion GetEnv("MIMOCA_VERSION")
#if MyAppVersion == ""
  #define MyAppVersion "0.1.0"
#endif
#define MyAppPublisher "MiMoCA"
#define MyAppExeName "scripts\\launch_mimoca.bat"

[Setup]
AppId={{D39F60A7-A96D-4BA1-A70E-B867047DFC9A}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\Programs\{#MyAppName}
DefaultGroupName={#MyAppName}
UninstallDisplayIcon={app}\bin\mimoca.exe
Compression=lzma
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=dist
OutputBaseFilename=MiMoCA-Setup-{#MyAppVersion}

[Files]
Source: "..\release\MiMoCA\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional icons:"

[Run]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\scripts\first_launch_setup.ps1"" -InstallRoot ""{app}"""; Flags: runhidden waituntilterminated
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{localappdata}\MiMoCA\logs"
Type: filesandordirs; Name: "{localappdata}\MiMoCA\temp"
Type: files; Name: "{localappdata}\MiMoCA\*.tmp"

[Code]
var
  RemoveModelCache: Boolean;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    RemoveModelCache := MsgBox(
      'Do you also want to remove cached model files? '#13#10 +
      'Choose No to retain them for faster future reinstalls.',
      mbConfirmation, MB_YESNO) = IDYES;

    DelTree(ExpandConstant('{localappdata}\\MiMoCA\\app_data'), True, True, True);
    DelTree(ExpandConstant('{localappdata}\\MiMoCA\\sidecar_venv'), True, True, True);

    if RemoveModelCache then
      DelTree(ExpandConstant('{localappdata}\\MiMoCA\\model_cache'), True, True, True);
  end;
end;
