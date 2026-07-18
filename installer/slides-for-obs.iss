; ============================================================================
; slides-for-obs.iss - Inno Setup installer for the "Slides for OBS" plugin
;
; Produces SlidesForOBS-Setup.exe, which:
;   * finds the OBS Studio install directory automatically,
;   * installs the plugin DLL and its bundled data (poppler tools) into the
;     correct OBS locations,
;   * doubles as an UPDATER - run it again over an existing install and it
;     cleanly overwrites the old files,
;   * refuses to run while OBS is open (Windows locks the loaded DLL, which
;     would corrupt an update),
;   * checks for LibreOffice at the end and, if it's missing, points the user
;     at the download page (the plugin needs LibreOffice to convert slides).
;
; Expected build layout (matches the CI artifact), rooted at {#DistDir}:
;   {#DistDir}\bin\64bit\slides-for-obs.dll
;   {#DistDir}\data\...            (bin\*.exe/*.dll, locale\en-US.ini)
;
; CI passes /DDistDir=<abs path>, /DMyAppVersion=<ver>, /O<abs output dir>.
; ============================================================================

#define MyAppName "Slides for OBS"
#define MyAppPublisher "Jordan Hunter"
#ifndef MyAppVersion
  #define MyAppVersion "0.1.0"
#endif
#ifndef DistDir
  #define DistDir "dist\slides-for-obs"
#endif
; Newline for MsgBox strings. Using a define avoids Pascal source lines that
; begin with '#' (e.g. a continuation starting with #13#10), which Inno's
; preprocessor would try to read as a directive and abort on.
#define NL "#13#10"

[Setup]
AppId={{B7E9F1C2-3A4D-4E5F-9A8B-0C1D2E3F4A5B}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
; We install into the OBS folders resolved at runtime (see [Code]); the dir
; page is disabled and DefaultDirName is a plain constant so ISCC does not
; evaluate code during compile.
DefaultDirName={autopf}\obs-studio
DisableDirPage=yes
DisableProgramGroupPage=yes
UninstallDisplayName={#MyAppName}
OutputBaseFilename=SlidesForOBS-Setup
OutputDir=Output
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible

[Files]
; Plugin DLL -> OBS\obs-plugins\64bit\
Source: "{#DistDir}\bin\64bit\slides-for-obs.dll"; \
  DestDir: "{code:GetObsDir}\obs-plugins\64bit"; Flags: ignoreversion

; Data folder (poppler tools + locale) -> OBS\data\obs-plugins\slides-for-obs\
Source: "{#DistDir}\data\*"; \
  DestDir: "{code:GetObsDir}\data\obs-plugins\slides-for-obs"; \
  Flags: ignoreversion recursesubdirs createallsubdirs

[UninstallDelete]
; Sweep the whole plugin data folder on uninstall.
Type: filesandordirs; Name: "{code:GetObsDir}\data\obs-plugins\slides-for-obs"

[Code]
var
  ObsDir: String;

{ Locate the OBS Studio install directory. }
function DetectObsDir(): String;
var
  P: String;
begin
  Result := '';

  { 1) Registry: OBS's uninstall entry records InstallLocation. }
  if RegQueryStringValue(HKLM,
      'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio',
      'InstallLocation', P) then
    if (P <> '') and DirExists(P) then
    begin
      Result := P;
      exit;
    end;

  { 2) Common default install paths. }
  P := ExpandConstant('{commonpf64}\obs-studio');
  if DirExists(P) then begin Result := P; exit; end;

  P := ExpandConstant('{commonpf}\obs-studio');
  if DirExists(P) then begin Result := P; exit; end;
end;

{ Exposed to [Files]/[UninstallDelete] via {code:GetObsDir}. }
function GetObsDir(Param: String): String;
begin
  Result := ObsDir;
end;

{ Is OBS currently running? (Its DLL would be locked during an update.) }
function IsObsRunning(): Boolean;
var
  ResultCode: Integer;
begin
  Result := False;
  if Exec(ExpandConstant('{cmd}'),
      '/C tasklist /FI "IMAGENAME eq obs64.exe" | find /I "obs64.exe"',
      '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    Result := (ResultCode = 0);
end;

function InitializeSetup(): Boolean;
begin
  Result := True;

  { Find OBS up front; if we can't, let the user continue or bail. }
  ObsDir := DetectObsDir();
  if ObsDir = '' then
  begin
    if MsgBox('OBS Studio was not found in the usual locations.' {#NL}
              'Please make sure OBS Studio is installed before installing '
              'this plugin.' {#NL}{#NL}
              'Continue anyway using the default OBS folder?',
              mbConfirmation, MB_YESNO) = IDYES then
      ObsDir := ExpandConstant('{commonpf64}\obs-studio')
    else
    begin
      Result := False;
      exit;
    end;
  end;

  { Refuse to run while OBS is open - the plugin DLL would be locked. }
  while IsObsRunning() do
  begin
    if MsgBox('OBS Studio is currently running.' {#NL}
              'Please close OBS completely, then click Retry to continue.' {#NL}
              '' {#NL}
              '(Installing while OBS is open would fail to replace the '
              'plugin file.)',
              mbError, MB_RETRYCANCEL) = IDCANCEL then
    begin
      Result := False;
      exit;
    end;
  end;
end;

{ Is LibreOffice installed? The plugin needs it to convert slides. }
function LibreOfficeInstalled(): Boolean;
begin
  Result :=
    FileExists(ExpandConstant('{commonpf64}\LibreOffice\program\soffice.exe')) or
    FileExists(ExpandConstant('{commonpf}\LibreOffice\program\soffice.exe')) or
    FileExists(ExpandConstant('{commonpf32}\LibreOffice\program\soffice.exe'));
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ErrCode: Integer;
begin
  if CurStep = ssPostInstall then
  begin
    if not LibreOfficeInstalled() then
    begin
      if MsgBox('Slides for OBS was installed successfully.' {#NL}{#NL}
                'However, LibreOffice was not found on this computer. The '
                'plugin needs LibreOffice (free) to convert your slides - '
                'without it, presentations will not render.' {#NL}{#NL}
                'Open the LibreOffice download page now?',
                mbInformation, MB_YESNO) = IDYES then
        ShellExec('open', 'https://www.libreoffice.org/download/download/',
                  '', '', SW_SHOW, ewNoWait, ErrCode);
    end;
  end;
end;
