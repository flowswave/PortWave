Unicode true
!include "MUI2.nsh"
!include "x64.nsh"
Name "PortWave"
OutFile "${OUTPUT}"
; Per-machine install: binaries live in Program Files, user data stays in the
; user profile (%LOCALAPPDATA%\PortWaveCpp), so no configuration is written here.
InstallDir "$PROGRAMFILES64\PortWave"
InstallDirRegKey HKLM "Software\PortWaveCppInstaller" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
SetOverwrite on
AllowSkipFiles off
VIProductVersion "${VERSION}.0"
VIAddVersionKey /LANG=2052 "ProductName" "PortWave"
VIAddVersionKey /LANG=2052 "FileDescription" "PortWave 安装程序"
VIAddVersionKey /LANG=2052 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=2052 "LegalCopyright" "PortWave contributors"
!define MUI_ABORTWARNING
!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
Icon "${ICON}"
UninstallIcon "${ICON}"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"
Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "PortWave 需要 64 位 Windows。"
    Abort
  ${EndIf}
FunctionEnd
Section "PortWave" Main
  SetShellVarContext all
  ; Drop start-menu entries left behind by the earlier per-user installer so the
  ; shell does not show two PortWave shortcuts. Files under %LOCALAPPDATA% stay.
  Delete "$APPDATA\Microsoft\Windows\Start Menu\Programs\PortWave\PortWave.lnk"
  Delete "$APPDATA\Microsoft\Windows\Start Menu\Programs\PortWave\卸载 PortWave.lnk"
  RMDir "$APPDATA\Microsoft\Windows\Start Menu\Programs\PortWave"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PortWaveCpp"
  !include "${PAYLOAD}"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "Software\PortWaveCppInstaller" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PortWaveCpp" "DisplayName" "PortWave"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PortWaveCpp" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PortWaveCpp" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PortWaveCpp" "DisplayIcon" "$INSTDIR\PortWave.exe,0"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PortWaveCpp" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PortWaveCpp" "QuietUninstallString" '$\"$INSTDIR\Uninstall.exe$\" /S'
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PortWaveCpp" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PortWaveCpp" "NoRepair" 1
  CreateDirectory "$SMPROGRAMS\PortWave"
  CreateShortcut "$SMPROGRAMS\PortWave\PortWave.lnk" "$INSTDIR\PortWave.exe"
  CreateShortcut "$SMPROGRAMS\PortWave\卸载 PortWave.lnk" "$INSTDIR\Uninstall.exe"
SectionEnd
Section "Uninstall"
  SetShellVarContext all
  !include "${REMOVEFILES}"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\PortWave\PortWave.lnk"
  Delete "$SMPROGRAMS\PortWave\卸载 PortWave.lnk"
  RMDir "$SMPROGRAMS\PortWave"
  DeleteRegKey HKLM "Software\PortWaveCppInstaller"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PortWaveCpp"
  ; Legacy per-user registration from earlier builds.
  DeleteRegKey HKCU "Software\PortWaveCppInstaller"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PortWaveCpp"
  Delete "$APPDATA\Microsoft\Windows\Start Menu\Programs\PortWave\PortWave.lnk"
  Delete "$APPDATA\Microsoft\Windows\Start Menu\Programs\PortWave\卸载 PortWave.lnk"
  RMDir "$APPDATA\Microsoft\Windows\Start Menu\Programs\PortWave"
SectionEnd
