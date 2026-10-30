; FM2 installer (Inno Setup 6). Built by build-installer.cmd into dist\FM2-Setup-<version>.exe.
;
; What it does:
;  - asks for your own European Forza Motorsport 2 disc image (.iso) and the title update, and checks both
;    against this build (nothing from the game ships with FM2)
;  - optionally takes a folder with your DLC packages
;  - builds the game files from the disc into the install folder, unpacks the title update into
;    Documents\fm2\update, and copies the DLC into Documents\fm2\dlc (per user, no admin rights)
;  - the bundled Python (installer payload) does the disc and title update work; nothing needs installing

#define AppName "Forza Motorsport 2 ReGPU"
#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#define Payload "payload"

[Setup]
AppId={{B2D6E4A1-5C7F-4E3B-9A18-3F0D7C2E6B91}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=zer0ducksgiven
DefaultDirName={localappdata}\FM2
DisableProgramGroupPage=yes
DisableDirPage=no
PrivilegesRequired=lowest
OutputDir=dist
OutputBaseFilename=FM2-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#AppName} {#AppVersion}
UninstallDisplayIcon={app}\launcher\FM2Launcher.exe
CloseApplications=yes

[Messages]
WelcomeLabel2=This will install {#AppName} {#AppVersion}, a native PC port of Forza Motorsport 2.%n%nYou need your own copy of the European Forza Motorsport 2 disc image (.iso) and its title update. Nothing from the game is included in this installer.%n%nPlease close Forza Motorsport 2 before continuing.

[Tasks]
Name: "desktop"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"

[Files]
; The port
Source: "{#Payload}\build\tu\*"; DestDir: "{app}\build\tu"; Excludes: "nvngx_dlss.dll,xenos_dis.exe"; Flags: ignoreversion
Source: "{#Payload}\launcher\*"; DestDir: "{app}\launcher"; Flags: ignoreversion recursesubdirs
Source: "{#Payload}\scripts\*"; DestDir: "{app}"; Flags: ignoreversion
; The helper and the Python that runs it (installed copy)
Source: "{#Payload}\py\*"; DestDir: "{app}\py"; Flags: ignoreversion recursesubdirs
; The same Python and helper, staged for the wizard's disc and title update checks
Source: "{#Payload}\py\*"; DestDir: "{tmp}"; Flags: dontcopy

[Icons]
Name: "{userprograms}\{#AppName}"; Filename: "{app}\launcher\FM2Launcher.exe"; WorkingDir: "{app}"
Name: "{userdesktop}\{#AppName}"; Filename: "{app}\launcher\FM2Launcher.exe"; WorkingDir: "{app}"; Tasks: desktop

[Run]
Filename: "{app}\launcher\FM2Launcher.exe"; Description: "Open the launcher"; Flags: postinstall nowait skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\py"
Type: filesandordirs; Name: "{app}\out"

[Code]
var
  IsoPage: TInputFileWizardPage;
  TuPage: TInputFileWizardPage;
  DlcPage: TInputDirWizardPage;
  DownloadPage: TDownloadWizardPage;
  Python: String;
  Helper: String;

function DocsDir: String;
begin
  Result := ExpandConstant('{userdocs}\fm2');
end;

function Quoted(S: String): String;
begin
  Result := '"' + S + '"';
end;

// The launcher needs the .NET 10 Desktop Runtime (x64).
function DesktopRuntimeInstalled: Boolean;
var
  Found: TFindRec;
begin
  Result := FindFirst(ExpandConstant('{commonpf64}\dotnet\shared\Microsoft.WindowsDesktop.App\10.*'), Found);
  if Result then
    FindClose(Found);
end;

procedure InitializeWizard;
begin
  DownloadPage := CreateDownloadPage('Getting the .NET Desktop Runtime', 'The launcher needs Microsoft''s .NET 10 Desktop Runtime. Downloading it...', nil);
  ExtractTemporaryFiles('*');
  Python := ExpandConstant('{tmp}\python.exe');
  Helper := ExpandConstant('{tmp}\setup_game.py');

  IsoPage := CreateInputFilePage(wpSelectDir,
    'Game disc', 'Choose your European disc image',
    'Select the .iso of Forza Motorsport 2 (European disc). It must be the same build as this installer.');
  IsoPage.Add('Disc image (.iso):', 'Disc image (*.iso)|*.iso|All files|*.*', '.iso');

  TuPage := CreateInputFilePage(IsoPage.ID,
    'Title update', 'Choose the title update file',
    'Select the title update file (named like TU_16L61VA_...). It must be the same build as this installer.');
  TuPage.Add('Title update:', 'All files|*.*', '');

  DlcPage := CreateInputDirPage(TuPage.ID,
    'DLC (optional)', 'Choose the folder with your DLC packages',
    'If you have no DLC, keep this folder as it is and the DLC step is skipped. Otherwise choose the folder with your DLC packages.', False, '');
  DlcPage.Add('');
  // A folder must be given. This default has no DLC in it, so it is skipped.
  DlcPage.Values[0] := ExpandConstant('{userdocs}\fm2\dlc');
end;

// The last line Python printed in the setup step (its error), or ''.
function LastLogLine(Path: String): String;
var
  Lines: TArrayOfString;
  I: Integer;
begin
  Result := '';
  if LoadStringsFromFile(Path, Lines) then
    for I := GetArrayLength(Lines) - 1 downto 0 do
      if Trim(Lines[I]) <> '' then
      begin
        Result := Trim(Lines[I]);
        exit;
      end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  ResultCode: Integer;
  Iso, Tu: String;
  Free, Total: Int64;
begin
  Result := True;
  // The game files built from the disc take 4.8 GB in the install folder; the installer's own
  // check only counts its files. An install on a drive without room failed after copying them,
  // leaving no game files and (built after them) no title update.
  if (CurPageID = wpSelectDir) and GetSpaceOnDisk64(ExtractFileDrive(WizardDirValue), Free, Total) and
    (Free < Int64(6) * 1024 * 1024 * 1024) then
  begin
    MsgBox('There is not enough free space on ' + ExtractFileDrive(WizardDirValue) + ' for the game files: they need 6 GB, ' +
      IntToStr(Free div (1024 * 1024 * 1024)) + ' GB is free. Choose another folder or free some space.', mbError, MB_OK);
    Result := False;
    exit;
  end;
  if (CurPageID = wpReady) and not DesktopRuntimeInstalled then
  begin
    DownloadPage.Clear;
    DownloadPage.Add('https://aka.ms/dotnet/10.0/windowsdesktop-runtime-win-x64.exe', 'windowsdesktop-runtime.exe', '');
    DownloadPage.Show;
    try
      try
        DownloadPage.Download;
        // Microsoft's installer asks for administrator rights itself
        Exec(ExpandConstant('{tmp}\windowsdesktop-runtime.exe'), '/install /quiet /norestart', '', SW_SHOW, ewWaitUntilTerminated, ResultCode);
      except
        MsgBox('The .NET Desktop Runtime could not be downloaded: ' + GetExceptionMessage + #13#10#13#10 +
          'The game installs anyway. Get the runtime from https://dotnet.microsoft.com/download/dotnet/10.0 (".NET Desktop Runtime") to use the launcher.', mbError, MB_OK);
      end;
    finally
      DownloadPage.Hide;
    end;
    exit;
  end;
  if CurPageID = IsoPage.ID then
  begin
    Iso := IsoPage.Values[0];
    if not FileExists(Iso) then
    begin
      MsgBox('Choose your disc image (.iso).', mbError, MB_OK);
      Result := False;
      exit;
    end;
    Exec(Python, '"' + Helper + '" check-iso ' + Quoted(Iso), '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    if ResultCode <> 0 then
    begin
      MsgBox('This disc does not match this build (its default.xex is different). Choose the European Forza Motorsport 2 disc.', mbError, MB_OK);
      Result := False;
    end;
  end
  else if CurPageID = TuPage.ID then
  begin
    Tu := TuPage.Values[0];
    if not FileExists(Tu) then
    begin
      MsgBox('Choose the title update file.', mbError, MB_OK);
      Result := False;
      exit;
    end;
    Exec(Python, '"' + Helper + '" check-tu ' + Quoted(Tu), '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    if ResultCode <> 0 then
    begin
      MsgBox('This title update does not match this build.', mbError, MB_OK);
      Result := False;
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
  Dlc, Params, Log, Why: String;
begin
  if CurStep = ssPostInstall then
  begin
    Dlc := DlcPage.Values[0];
    if (Dlc = '') or (Dlc = ExpandConstant('{userdocs}\fm2\dlc')) then
      Dlc := '-';
    Params := '"' + ExpandConstant('{app}\py\setup_game.py') + '" install ' +
      Quoted(IsoPage.Values[0]) + ' ' + Quoted(ExpandConstant('{app}\game')) + ' ' +
      Quoted(TuPage.Values[0]) + ' ' + Quoted(Dlc) + ' ' + Quoted(DocsDir);
    WizardForm.StatusLabel.Caption := 'Building the game files from your disc (a few minutes)...';
    WizardForm.ProgressGauge.Style := npbstMarquee;
    // Its output in out\setup.log, so a failure says why.
    ForceDirectories(ExpandConstant('{app}\out'));
    Log := ExpandConstant('{app}\out\setup.log');
    if not Exec(ExpandConstant('{cmd}'), '/c ""' + ExpandConstant('{app}\py\python.exe') + '" ' + Params + ' > "' + Log + '" 2>&1"', '',
      SW_HIDE, ewWaitUntilTerminated, ResultCode) or (ResultCode <> 0) then
    begin
      Why := LastLogLine(Log);
      if Why = '' then
        Why := 'code ' + IntToStr(ResultCode);
      MsgBox('The game files could not be built:' + #13#10#13#10 + Why + #13#10#13#10 + 'The details are in ' + Log + '.', mbError, MB_OK);
    end;
    WizardForm.ProgressGauge.Style := npbstNormal;
  end;
end;
