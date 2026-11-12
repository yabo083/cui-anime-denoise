; Anime Denoise - per-user Inno Setup 6 script.
;
; Built by installer\build_installer.ps1, which stages the payload in
; ..\dist\stage and passes the version:
;
;   ISCC.exe /DAppVersion=1.2.0 AnimeDenoise.iss
;
; The AppId GUID is permanent: changing it would make every future version
; look like a different product and break upgrades.
;
; Everything installs under %LOCALAPPDATA%\Programs\AnimeDenoise
; (PrivilegesRequired=lowest, no UAC prompt). User data lives in
; %LOCALAPPDATA%\AnimeDenoise and is *not* removed by the uninstaller unless
; the user explicitly asks for it at the end of uninstall.

#define AppName        "Anime Denoise"
#define AppExeName     "AnimeDenoise.exe"
#define AppPublisher   "Anime Denoise"
#define AppIdGuid      "{{315B3DFF-9738-4542-B28A-D929E54F030C}"

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

; Paths are relative to this script (Inno resolves Source paths that way too).
#define StageDir       "..\dist\stage"
#define StageDirFull   AddBackslash(SourcePath) + StageDir

[Setup]
AppId={#AppIdGuid}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
VersionInfoVersion={#AppVersion}
DefaultDirName={localappdata}\Programs\AnimeDenoise
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=..\dist
OutputBaseFilename=AnimeDenoise-{#AppVersion}-setup
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\{#AppExeName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; x64compatible requires Inno Setup 6.3+; the build script refuses
; to run without ISCC anyway, and 6.3 is the current 6.x line.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=yes
RestartApplications=yes

; assets\app.ico is optional in the repository; only reference it when the
; staging step actually produced it.
#if FileExists(StageDirFull + "\assets\app.ico")
SetupIconFile=..\dist\stage\assets\app.ico
#endif

[Languages]
; ChineseSimplified.isl is not part of every Inno Setup 6 install (6.7 ships
; it separately). Use it when present, otherwise fall back to the built-in
; English wizard; the [Messages] overrides below stay Chinese either way.
#if FileExists(AddBackslash(CompilerPath) + "Languages\ChineseSimplified.isl")
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
#else
Name: "english"; MessagesFile: "compiler:Default.isl"
#endif

[Messages]
; A few overrides so the wizard reads naturally in Chinese. The rest comes
; from compiler:Languages\ChineseSimplified.isl.
WelcomeLabel1=欢迎使用 [name] 安装向导
WelcomeLabel2=这将在你的电脑上安装 [name/ver]。%n%n建议关闭其他正在运行的应用程序后继续。
FinishedHeadingLabel=[name] 安装完成
FinishedLabel=安装程序已将 [name] 安装到你的电脑上。%n%n选择“完成”按钮退出安装向导。
SelectTasksLabel2=请选择安装程序在安装 [name] 时要执行的其他任务，然后单击“下一步”。

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#StageDir}\{#AppExeName}"; DestDir: "{app}"; Flags: ignoreversion
#if FileExists(StageDirFull + "\anime-denoise.exe")
Source: "{#StageDir}\anime-denoise.exe"; DestDir: "{app}"; Flags: ignoreversion
#endif
Source: "{#StageDir}\runtime\*"; DestDir: "{app}\runtime"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\models\*"; DestDir: "{app}\models"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\assets\*"; DestDir: "{app}\assets"; Flags: ignoreversion recursesubdirs createallsubdirs
#if FileExists(StageDirFull + "\README.md")
Source: "{#StageDir}\README.md"; DestDir: "{app}"; Flags: ignoreversion
#endif
#if FileExists(StageDirFull + "\LICENSE")
Source: "{#StageDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
#endif
#if FileExists(StageDirFull + "\THIRD-PARTY-NOTICES.txt")
Source: "{#StageDir}\THIRD-PARTY-NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
#endif

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExeName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Only the installation directory. %LOCALAPPDATA%\AnimeDenoise (settings,
; logs, projects, downloaded models and updates) is handled in [Code] below.
Type: filesandordirs; Name: "{app}"

[Code]
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DataDir: String;
begin
  if CurUninstallStep <> usPostUninstall then
    Exit;
  { Silent uninstall (/SILENT, /VERYSILENT) never touches user data. }
  if UninstallSilent then
    Exit;
  DataDir := ExpandConstant('{localappdata}\AnimeDenoise');
  if not DirExists(DataDir) then
    Exit;
  if MsgBox('是否同时删除用户数据？' + #13#10 +
            '（设置、日志、项目、已下载的模型与更新包）' + #13#10 + #13#10 +
            DataDir, mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
    DelTree(DataDir, True, True, True);
end;
