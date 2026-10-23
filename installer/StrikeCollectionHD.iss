; Inno Setup script for Strike Remastered Collection HD (docs/03-collection-plan.md).
; Build: install Inno Setup 6 (https://jrsoftware.org/isinfo.php), then run
;   installer\build_installer.bat
; or open this file in the Inno Setup Compiler. Output: dist\StrikeCollectionHD-Setup.exe
;
; No game data is included. The ROM page checks each file the player picks by
; SHA-1 and copies accepted ones to %APPDATA%\DesertStrikeHD\DesertStrikeHD\roms\
; as <id>.bin, the same place the in-app "Add ROM" uses.

#define AppName "Strike Remastered Collection HD"
#define AppExe "StrikeCollectionHD.exe"
#define AppVersion "0.6.0"

[Setup]
AppId={{6E0B5F2A-3C1D-4B8E-9A47-5D2C1F7E8B90}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Personal project
DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=..\dist
OutputBaseFilename=StrikeCollectionHD-Setup
SetupIconFile=..\res\desertstrikehd.ico
UninstallDisplayIcon={app}\{#AppExe}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Shortcuts:"

[Files]
Source: "..\dist\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\hdpack_xbrz.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\tools\xbrz\License.txt"; DestDir: "{app}"; DestName: "hdpack_xbrz-LICENSE.txt"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion isreadme

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "Launch {#AppName}"; Flags: nowait postinstall skipifsilent

[Code]
const
  GameCount = 3;

var
  RomPage: TInputFileWizardPage;
  Ids, Titles, Sha: array[0..2] of String;
  Ok: array[0..2] of Boolean;

function RomDir: String;
begin
  Result := ExpandConstant('{userappdata}\DesertStrikeHD\DesertStrikeHD\roms');
end;

procedure InitializeWizard;
var
  i: Integer;
begin
  Ids[0] := 'desert'; Titles[0] := 'Desert Strike: Return to the Gulf (1992)'; Sha[0] := 'd7e7d8c358eb845b84fb08f904cc0b95d0a4053d';
  Ids[1] := 'jungle'; Titles[1] := 'Jungle Strike (1993)'; Sha[1] := '7a7568e39341b1bb218280ee05c2b37c273317b5';
  Ids[2] := 'urban';  Titles[2] := 'Urban Strike (1994)'; Sha[2] := '897a8f6a08d6d7d6e5316f8047532b3e4603e705';

  RomPage := CreateInputFilePage(wpSelectDir,
    'Your game ROMs',
    'Point to your own Sega Genesis / Mega Drive dumps (USA/Europe releases).',
    'Each file is checked when you click Next. You need at least one game now; ' +
    'the others can be added later from the game picker (Add ROM). ' +
    'Games you have already added are kept.');
  for i := 0 to GameCount - 1 do
    RomPage.Add(Titles[i] + ':', 'Genesis ROM (*.bin;*.md;*.gen;*.smd)|*.bin;*.md;*.gen;*.smd|All files|*.*', '.bin');
end;

function AlreadyHave(i: Integer): Boolean;
begin
  Result := FileExists(RomDir + '\' + Ids[i] + '.bin');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  i, n: Integer;
  f, h, msg: String;
begin
  Result := True;
  if CurPageID <> RomPage.ID then Exit;
  n := 0;
  msg := '';
  for i := 0 to GameCount - 1 do begin
    Ok[i] := False;
    f := Trim(RomPage.Values[i]);
    if f <> '' then begin
      if not FileExists(f) then
        msg := msg + Titles[i] + ': file not found.' + #13#10
      else begin
        h := Lowercase(GetSHA1OfFile(f));
        if h = Sha[i] then begin
          Ok[i] := True;
          n := n + 1;
        end else
          msg := msg + Titles[i] + ': this file is not the supported dump (SHA-1 ' + h + ').' + #13#10;
      end;
    end else if AlreadyHave(i) then
      n := n + 1;
  end;
  if msg <> '' then begin
    MsgBox(msg + #13#10 + 'Pick a different file or leave the field empty.', mbError, MB_OK);
    Result := False;
  end else if n = 0 then begin
    MsgBox('Choose at least one game ROM to continue.', mbInformation, MB_OK);
    Result := False;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  i: Integer;
begin
  if CurStep <> ssPostInstall then Exit;
  ForceDirectories(RomDir);
  for i := 0 to GameCount - 1 do
    if Ok[i] then
      if not CopyFile(RomPage.Values[i], RomDir + '\' + Ids[i] + '.bin', False) then
        MsgBox('Could not copy the ROM for ' + Titles[i] + '.', mbError, MB_OK);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  d: String;
begin
  if CurUninstallStep <> usPostUninstall then Exit;
  d := ExpandConstant('{userappdata}\DesertStrikeHD');
  if DirExists(d) then
    if MsgBox('Also delete your settings, save states, HD packs and the copied ROMs?' + #13#10 + d,
              mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
      DelTree(d, True, True, True);
end;
