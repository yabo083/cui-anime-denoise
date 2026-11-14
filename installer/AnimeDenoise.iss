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
; broadcast WM_SETTINGCHANGE after install/uninstall so the user PATH edit
; (task "addpath") reaches Explorer and new shells without a relogin
ChangesEnvironment=yes

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
Name: "addpath"; Description: "把安装目录加入用户 PATH（anime-denoise 命令行随处可用）"; GroupDescription: "集成："
Name: "contextmenu"; Description: "图片文件右键菜单加入「Anime Denoise 去噪」"; GroupDescription: "集成："

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

[Registry]
; Context-menu verb on image files. The perceived-type branch covers every
; extension Windows already tags PerceivedType=image (png, jpg, jpeg, bmp,
; gif, tif, tiff, webp on current builds); the explicit entries below cover
; the formats that carry no default perceived type. One verb name either
; way, so a file never shows it twice.
Root: HKCU; Subkey: "Software\Classes\SystemFileAssociations\image\shell\AnimeDenoise"; ValueType: string; ValueData: "用 Anime Denoise 去噪"; Flags: uninsdeletekey; Tasks: contextmenu
Root: HKCU; Subkey: "Software\Classes\SystemFileAssociations\image\shell\AnimeDenoise"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\{#AppExeName}"; Tasks: contextmenu
Root: HKCU; Subkey: "Software\Classes\SystemFileAssociations\image\shell\AnimeDenoise\command"; ValueType: string; ValueData: """{app}\{#AppExeName}"" ""%1"""; Tasks: contextmenu
Root: HKCU; Subkey: "Software\Classes\SystemFileAssociations\.avif\shell\AnimeDenoise"; ValueType: string; ValueData: "用 Anime Denoise 去噪"; Flags: uninsdeletekey; Tasks: contextmenu
Root: HKCU; Subkey: "Software\Classes\SystemFileAssociations\.avif\shell\AnimeDenoise"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\{#AppExeName}"; Tasks: contextmenu
Root: HKCU; Subkey: "Software\Classes\SystemFileAssociations\.avif\shell\AnimeDenoise\command"; ValueType: string; ValueData: """{app}\{#AppExeName}"" ""%1"""; Tasks: contextmenu
Root: HKCU; Subkey: "Software\Classes\SystemFileAssociations\.heic\shell\AnimeDenoise"; ValueType: string; ValueData: "用 Anime Denoise 去噪"; Flags: uninsdeletekey; Tasks: contextmenu
Root: HKCU; Subkey: "Software\Classes\SystemFileAssociations\.heic\shell\AnimeDenoise"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\{#AppExeName}"; Tasks: contextmenu
Root: HKCU; Subkey: "Software\Classes\SystemFileAssociations\.heic\shell\AnimeDenoise\command"; ValueType: string; ValueData: """{app}\{#AppExeName}"" ""%1"""; Tasks: contextmenu
Root: HKCU; Subkey: "Software\Classes\SystemFileAssociations\.heif\shell\AnimeDenoise"; ValueType: string; ValueData: "用 Anime Denoise 去噪"; Flags: uninsdeletekey; Tasks: contextmenu
Root: HKCU; Subkey: "Software\Classes\SystemFileAssociations\.heif\shell\AnimeDenoise"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\{#AppExeName}"; Tasks: contextmenu
Root: HKCU; Subkey: "Software\Classes\SystemFileAssociations\.heif\shell\AnimeDenoise\command"; ValueType: string; ValueData: """{app}\{#AppExeName}"" ""%1"""; Tasks: contextmenu

[Run]
Filename: "{app}\{#AppExeName}"; Flags: nowait postinstall skipifsilent
; The app's self-update hands off with /autolaunch=1 (upd_run_installer);
; bring the app back up after a silent reinstall so the user lands where
; they were. The check runs at the [Run] stage, after the install itself.
Filename: "{app}\{#AppExeName}"; Flags: nowait; Check: UpdaterAutoLaunch

[UninstallDelete]
; Only the installation directory. %LOCALAPPDATA%\AnimeDenoise (settings,
; logs, projects, downloaded models and updates) is handled in [Code] below.
Type: filesandordirs; Name: "{app}"

[Code]
{ --- user PATH add/remove for the anime-denoise CLI (task "addpath") --- }

function PathHasSegment(Paths, Seg: String): Boolean;
var
  Hay: String;
begin
  { compare whole ';'-separated segments, case-insensitive, tolerant of a
    stored trailing backslash }
  while (Length(Seg) > 0) and (Seg[Length(Seg)] = '\') do
    Seg := Copy(Seg, 1, Length(Seg) - 1);
  Hay := ';' + Uppercase(Paths) + ';';
  Result := (Pos(';' + Uppercase(Seg) + ';', Hay) > 0) or
            (Pos(';' + Uppercase(Seg) + '\;', Hay) > 0);
end;

procedure PathAdd(Dir: String);
var
  Paths: String;
begin
  if not RegQueryStringValue(HKCU, 'Environment', 'Path', Paths) then
    Paths := '';
  if PathHasSegment(Paths, Dir) then
    Exit;
  if (Paths <> '') and (Paths[Length(Paths)] <> ';') then
    Paths := Paths + ';';
  Paths := Paths + Dir;
  RegWriteExpandStringValue(HKCU, 'Environment', 'Path', Paths);
end;

procedure PathRemove(Dir: String);
var
  Paths, Seg, Item, Cmp, NewPath: String;
  P: Integer;
begin
  if not RegQueryStringValue(HKCU, 'Environment', 'Path', Paths) then
    Exit;
  Seg := Dir;
  while (Length(Seg) > 0) and (Seg[Length(Seg)] = '\') do
    Seg := Copy(Seg, 1, Length(Seg) - 1);
  NewPath := '';
  repeat
    P := Pos(';', Paths);
    if P = 0 then P := Length(Paths) + 1;
    Item := Copy(Paths, 1, P - 1);
    Paths := Copy(Paths, P + 1, Length(Paths));
    Cmp := Item;
    while (Length(Cmp) > 0) and (Cmp[Length(Cmp)] = '\') do
      Cmp := Copy(Cmp, 1, Length(Cmp) - 1);
    { keep the ORIGINAL segment text; only the comparison is normalized }
    if (Item <> '') and (Uppercase(Cmp) <> Uppercase(Seg)) then
    begin
      if NewPath <> '' then
        NewPath := NewPath + ';';
      NewPath := NewPath + Item;
    end;
  until Length(Paths) = 0;
  if NewPath <> '' then
    RegWriteExpandStringValue(HKCU, 'Environment', 'Path', NewPath)
  else
    RegDeleteValue(HKCU, 'Environment', 'Path');
end;

{ --- self-update handoff: relaunch the app after /autolaunch=1 installs --- }

function UpdaterAutoLaunch(): Boolean;
begin
  Result := ExpandConstant('{param:autolaunch|0}') = '1';
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    if WizardIsTaskSelected('addpath') then
      PathAdd(ExpandConstant('{app}'));
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DataDir: String;
begin
  if CurUninstallStep <> usPostUninstall then
    Exit;
  { always take our segment back out of the user PATH; a no-op when the
    task was never run or the value is absent }
  PathRemove(ExpandConstant('{app}'));
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
