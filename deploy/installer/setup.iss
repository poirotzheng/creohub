; =====================================================================
; creo-hub Inno Setup 6 script (M4c; docs\39 6.1) - BUILD-TIME ONLY.
; Compile with build_installer.cmd (ISCC). The setup.exe it produces:
;   - is fully OFFLINE (payload embedded), no Node/DSH required to install
;   - installs per-user (PrivilegesRequired=lowest), Win7 SP1+ (MinVersion)
;   - calls our install.cmd KERNEL for registration (script remains the
;     single source of install logic; Inno only copies files + wizard)
; NOTE: keep this file ASCII.
; =====================================================================

#define AppName "creo-hub"
#ifndef AppVer
  #define AppVer "1.0.0"
#endif
#ifndef DistDir
  #define DistDir "..\..\dist\creo-hub-1.0.0-win10"
#endif
#ifndef OutDir
  #define OutDir "..\..\dist"
#endif

[Setup]
AppId={{7B4E2C1A-9F3D-4A62-8E15-0C2AB7D9E401}
AppName={#AppName}
AppVersion={#AppVer}
AppVerName={#AppName} {#AppVer}
AppPublisher=creo-hub
DefaultDirName={localappdata}\Programs\creo-hub
DefaultGroupName=creo-hub
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
MinVersion=6.1sp1
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutDir}
#ifndef OutBase
  #define OutBase "creo-hub-" + AppVer + "-setup"
#endif
OutputBaseFilename={#OutBase}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
#ifndef Uninstallable
  #define Uninstallable "yes"
#endif
Uninstallable={#Uninstallable}
UninstallDisplayName={#AppName} {#AppVer}
SetupLogging=yes
DisableWelcomePage=no

[Files]
Source: "{#DistDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Tasks]
Name: "agentshortcut"; Description: "创建助手快捷方式（开始菜单 + 桌面）"; GroupDescription: "助手 (hub-agent)："
Name: "agentautostart"; Description: "登录时自动启动助手（当前用户，可选）"; GroupDescription: "助手 (hub-agent)："; Flags: unchecked

[Icons]
Name: "{group}\creo-hub 助手"; Filename: "{app}\agent\start_agent.cmd"; WorkingDir: "{app}\agent"; Tasks: agentshortcut; Check: AgentExists
Name: "{userdesktop}\creo-hub 助手"; Filename: "{app}\agent\start_agent.cmd"; WorkingDir: "{app}\agent"; Tasks: agentshortcut; Check: AgentExists

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "CreoHubAgent"; ValueData: """{app}\agent\start_agent.cmd"""; Flags: uninsdeletevalue; Tasks: agentautostart; Check: AgentExists

[Run]
Filename: "{app}\deploy\install.cmd"; Parameters: "--src ""{app}"" --root ""{app}"" --silent --yes{code:GetCreoHomeArg}"; StatusMsg: "Registering creo-hub with Creo..."; Flags: runhidden waituntilterminated

[UninstallRun]
Filename: "{app}\deploy\uninstall.cmd"; Parameters: "--root ""{app}"" --silent{code:GetCreoHomeArg}"; Flags: runhidden waituntilterminated; RunOnceId: "CreoHubUnregister"

[Code]
{ The agent is optional (win7 profile has no Node/agent): shortcuts and the
  Run entry are only created when the payload really contains start_agent.cmd. }
function AgentExists(): Boolean;
begin
  Result := FileExists(ExpandConstant('{app}\agent\start_agent.cmd'));
end;

{ Read /CREOHOME=<dir> (or --creo-home=<dir>) from the setup command line.
  When absent, registration is written to <app>\creo-hub.dat only and the
  wizard tells the user how to register manually. }
function GetParam(const Name: string): string;
var
  i: Integer;
  p: string;
begin
  Result := '';
  for i := 1 to ParamCount do
  begin
    p := ParamStr(i);
    if Pos(Name + '=', p) = 1 then
      Result := Copy(p, Length(Name) + 2, Length(p));
  end;
  { tolerate both quoted and unquoted values }
  if (Result <> '') and (Result[1] = '"') then
    Delete(Result, 1, 1);
  if (Result <> '') and (Result[Length(Result)] = '"') then
    SetLength(Result, Length(Result) - 1);
  if (Result <> '') and (Result[Length(Result)] = '\') then
    SetLength(Result, Length(Result) - 1);
end;

function GetCreoHomeArg(Param: string): string;
var
  h: string;
begin
  h := GetParam('/CREOHOME');
  if h = '' then
    h := GetParam('--creo-home');
  if h <> '' then
    Result := ' --creo-home "' + h + '"'
  else
    Result := '';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  h: string;
begin
  if CurStep = ssPostInstall then
  begin
    h := GetParam('/CREOHOME');
    if h = '' then
      h := GetParam('--creo-home');
    { NEVER block silent installs with a modal box (they run unattended):
      log it instead, the same text lands in the setup log. }
    if h = '' then
    begin
      Log('creo-hub: no /CREOHOME given - registration written to <app>\creo-hub.dat only');
      Log('creo-hub: add to config.pro -> toolkit_registry_file <install dir>\creo-hub.dat');
      if not WizardSilent then
        MsgBox('creo-hub was installed, but no /CREOHOME=<Creo config dir> was given.' + #13#10 +
               'Registration file: <install dir>\creo-hub.dat' + #13#10 + #13#10 +
               'Add this line to your config.pro to finish registration:' + #13#10 +
               '  toolkit_registry_file <install dir>\creo-hub.dat',
               mbInformation, MB_OK);
    end;
  end;
end;
