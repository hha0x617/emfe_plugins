; emfe_plugins — Inno Setup installer skeleton
;
; Per-user installer that copies emfe_plugin_*.dll under whichever of these the
; user has the corresponding host frontend for:
;   %LOCALAPPDATA%\emfe_WinUI3Cpp\plugins\
;   %LOCALAPPDATA%\emfe_CsWPF\plugins\
;
; Both hosts scan a system plugins/ directory next to the executable AND a
; per-user plugins/ directory under %LOCALAPPDATA%, with the per-user copy
; winning on filename collisions, so this installer is the recommended path
; for end users who installed the host via setup.exe to %ProgramFiles% (where
; they don't have write access).
;
; --- Build ---
;   1. Install Inno Setup 6.3+ from https://jrsoftware.org/isinfo.php
;      (the installer relies on x64compatible architecture handling.)
;   2. Build the four plugin DLLs as documented in emfe_plugins/README.md.
;      Expected output paths (relative to the repo root):
;        em8\build\bin\Release\emfe_plugin_em8.dll
;        mc68030\build\bin\Release\emfe_plugin_mc68030.dll
;        mc6809\target\release\emfe_plugin_mc6809.dll
;        z8000\build\bin\Release\emfe_plugin_z8000.dll
;   3. Compile this script:
;        iscc installer\emfe_plugins.iss
;      Override version on the command line:
;        iscc /DAppVersion=0.2.0 installer\emfe_plugins.iss
;   4. Resulting setup.exe lands in dist\.
;
; --- Detection logic ---
; The installer marks the WinUI3Cpp / CsWPF component "enabled and pre-checked"
; only when %LOCALAPPDATA%\emfe_<frontend>\ already exists. Both hosts create
; that directory on first launch (config/state), so its presence is a
; reliable proxy until the host installers register an uninstall registry key.
; A "Force install for both" override checkbox handles fresh machines where
; the host has been installed but never run.

#define AppName       "emfe_plugins"
#ifndef AppVersion
  #define AppVersion  "0.1.0"
#endif
#define AppPublisher  "hagiwara"
#define AppURL        "https://github.com/hha0x617/emfe_plugins"

; Plugin DLL source paths. Relative to this .iss file (installer/).
; Override with /DPluginsRoot=... on iscc to point at a staged copy
; (e.g. CI's dist\plugins\).
#ifndef PluginsRoot
  #define PluginsRoot "..\"
#endif

#define EM8_DLL       PluginsRoot + "em8\build\bin\Release\emfe_plugin_em8.dll"
#define MC68030_DLL   PluginsRoot + "mc68030\build\bin\Release\emfe_plugin_mc68030.dll"
#define MC6809_DLL    PluginsRoot + "mc6809\target\release\emfe_plugin_mc6809.dll"
#define Z8000_DLL     PluginsRoot + "z8000\build\bin\Release\emfe_plugin_z8000.dll"

[Setup]
; AppId pins the installer's identity for upgrade/uninstall tracking.
; Do NOT regenerate after the first public release — changing it makes
; older installers fail to detect existing installs as upgrades.
AppId={{C7E1F2A0-2B5C-4F7E-9D2E-ABCDEF010203}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}/issues
AppUpdatesURL={#AppURL}/releases
; The installer itself doesn't deploy files under {app} except the uninstaller,
; so this directory is just a holding place for unins000.exe / unins000.dat.
DefaultDirName={localappdata}\emfe_plugins_installer
DefaultGroupName=emfe_plugins
DisableProgramGroupPage=yes
DisableDirPage=yes
PrivilegesRequired=lowest
OutputDir=..\dist
OutputBaseFilename=emfe_plugins-{#AppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
UninstallDisplayName={#AppName} {#AppVersion}
UninstallDisplayIcon={app}\unins000.exe

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"

[Components]
; Description text contains the literal frontend name so the [Code] section
; can identify each item in WizardForm.ComponentsList by substring match.
Name: "winui3cpp"; Description: "Install for emfe_WinUI3Cpp ({localappdata}\emfe_WinUI3Cpp\plugins\)"; Types: full custom
Name: "cswpf";     Description: "Install for emfe_CsWPF ({localappdata}\emfe_CsWPF\plugins\)";         Types: full custom

[Files]
; --- emfe_WinUI3Cpp target ---
Source: "{#EM8_DLL}";     DestDir: "{localappdata}\emfe_WinUI3Cpp\plugins"; Components: winui3cpp; Flags: ignoreversion
Source: "{#MC68030_DLL}"; DestDir: "{localappdata}\emfe_WinUI3Cpp\plugins"; Components: winui3cpp; Flags: ignoreversion
Source: "{#MC6809_DLL}";  DestDir: "{localappdata}\emfe_WinUI3Cpp\plugins"; Components: winui3cpp; Flags: ignoreversion
Source: "{#Z8000_DLL}";   DestDir: "{localappdata}\emfe_WinUI3Cpp\plugins"; Components: winui3cpp; Flags: ignoreversion

; --- emfe_CsWPF target ---
Source: "{#EM8_DLL}";     DestDir: "{localappdata}\emfe_CsWPF\plugins"; Components: cswpf; Flags: ignoreversion
Source: "{#MC68030_DLL}"; DestDir: "{localappdata}\emfe_CsWPF\plugins"; Components: cswpf; Flags: ignoreversion
Source: "{#MC6809_DLL}";  DestDir: "{localappdata}\emfe_CsWPF\plugins"; Components: cswpf; Flags: ignoreversion
Source: "{#Z8000_DLL}";   DestDir: "{localappdata}\emfe_CsWPF\plugins"; Components: cswpf; Flags: ignoreversion

[UninstallDelete]
; Drop the plugins\ directories if empty after uninstall. The parent
; %LOCALAPPDATA%\emfe_<frontend>\ tree is owned by the host and must be left
; alone (it holds the user's config/state).
Type: dirifempty; Name: "{localappdata}\emfe_WinUI3Cpp\plugins"
Type: dirifempty; Name: "{localappdata}\emfe_CsWPF\plugins"

[Code]
var
  ForceAllCheckBox: TNewCheckBox;

function IsWinUI3CppInstalled(): Boolean;
begin
  Result := DirExists(ExpandConstant('{localappdata}\emfe_WinUI3Cpp'));
end;

function IsCsWPFInstalled(): Boolean;
begin
  Result := DirExists(ExpandConstant('{localappdata}\emfe_CsWPF'));
end;

procedure RefreshComponents();
var
  i: Integer;
  forceAll: Boolean;
  enableWin, enableCs: Boolean;
begin
  forceAll := (ForceAllCheckBox <> nil) and ForceAllCheckBox.Checked;
  enableWin := forceAll or IsWinUI3CppInstalled();
  enableCs  := forceAll or IsCsWPFInstalled();
  with WizardForm.ComponentsList do begin
    for i := 0 to Items.Count - 1 do begin
      if Pos('emfe_WinUI3Cpp', Items[i]) > 0 then begin
        ItemEnabled[i] := enableWin;
        Checked[i]     := enableWin;
      end else if Pos('emfe_CsWPF', Items[i]) > 0 then begin
        ItemEnabled[i] := enableCs;
        Checked[i]     := enableCs;
      end;
    end;
  end;
end;

procedure ForceAllClick(Sender: TObject);
begin
  RefreshComponents();
end;

procedure InitializeWizard();
begin
  ForceAllCheckBox := TNewCheckBox.Create(WizardForm);
  ForceAllCheckBox.Parent := WizardForm.SelectComponentsPage;
  // Shrink the components list to make room for the override checkbox.
  WizardForm.ComponentsList.Height := WizardForm.ComponentsList.Height - ScaleY(28);
  ForceAllCheckBox.Top    := WizardForm.ComponentsList.Top + WizardForm.ComponentsList.Height + ScaleY(8);
  ForceAllCheckBox.Left   := WizardForm.ComponentsList.Left;
  ForceAllCheckBox.Width  := WizardForm.ComponentsList.Width;
  ForceAllCheckBox.Height := ScaleY(20);
  ForceAllCheckBox.Caption := 'Force install for both (override detection)';
  ForceAllCheckBox.OnClick := @ForceAllClick;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpSelectComponents then
    RefreshComponents();
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpSelectComponents then begin
    if not (IsComponentSelected('winui3cpp') or IsComponentSelected('cswpf')) then begin
      MsgBox('Select at least one host frontend to install for.', mbError, MB_OK);
      Result := False;
    end;
  end;
end;
