; Inno Setup script — builds Bass-Better-er-Windows.exe, a one-click VST3 installer.
; Build:  iscc /DAppVer=0.1.1 packaging\windows\installer.iss
; Expects the plugin staged at  pkgsrc\Bass Better-er.vst3  (the release workflow does this).

#ifndef AppVer
  #define AppVer "0.0.0"
#endif

[Setup]
AppId={{8F4B2C10-7E2A-4C3D-9B6E-BA55BA55BA55}
AppName=Bass Better-er
AppVersion={#AppVer}
AppPublisher=Box of Rules
DefaultDirName={commoncf}\VST3
DisableDirPage=yes
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; SourceDir = repo root (the .iss lives in packaging/windows); the workflow stages the
; VST3 into <repo>/pkgsrc. OutputDir = this script's folder, where the workflow grabs it.
SourceDir={#SourcePath}..\..
OutputDir={#SourcePath}
OutputBaseFilename=Bass-Better-er-Windows
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=Bass Better-er
AppPublisherURL=https://github.com/boxofrules/bass-betterer

[Files]
Source: "pkgsrc\Bass Better-er.vst3\*"; DestDir: "{commoncf}\VST3\Bass Better-er.vst3"; \
  Flags: recursesubdirs createallsubdirs ignoreversion

[Run]
; nothing to run — the host rescans VST3 on next launch
