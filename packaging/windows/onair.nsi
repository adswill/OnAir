; NSIS installer for OnAir. Built by tools/package/make_windows.sh: makensis -DVERSION=... -DSRC=<folder> -DOUTFILE=<setup.exe>
!include "MUI2.nsh"
Name "OnAir"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\OnAir"
InstallDirRegKey HKLM "Software\OnAir" "Install_Dir"
RequestExecutionLevel admin
!define UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\OnAir"
!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SRC}/LICENSE.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\OnAir.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start OnAir"
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "OnAir"
  SetShellVarContext current   ; remove the per-user shortcuts that version 0.1.0's first installer created
  Delete "$SMPROGRAMS\OnAir\OnAir.lnk"
  Delete "$SMPROGRAMS\OnAir\Uninstall.lnk"
  RMDir "$SMPROGRAMS\OnAir"
  Delete "$DESKTOP\OnAir.lnk"
  SetShellVarContext all   ; the program is installed for all users: so are its shortcuts
  SetOutPath "$INSTDIR"
  File /r "${SRC}/*.*"
  WriteRegStr HKLM "Software\OnAir" "Install_Dir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OnAir" "DisplayName" "OnAir"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OnAir" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OnAir" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "${UNINST}" "Publisher" "OnAir project"
  WriteRegStr HKLM "${UNINST}" "DisplayIcon" "$INSTDIR\OnAir.exe"
  WriteRegStr HKLM "${UNINST}" "URLInfoAbout" "https://github.com/adswill/OnAir"
  WriteRegDWORD HKLM "${UNINST}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST}" "NoRepair" 1
  SectionGetSize 0 $0
  WriteRegDWORD HKLM "${UNINST}" "EstimatedSize" $0
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateDirectory "$SMPROGRAMS\OnAir"
  CreateShortcut "$SMPROGRAMS\OnAir\OnAir.lnk" "$INSTDIR\OnAir.exe"
  CreateShortcut "$SMPROGRAMS\OnAir\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
  CreateShortcut "$DESKTOP\OnAir.lnk" "$INSTDIR\OnAir.exe"
SectionEnd

Section "Uninstall"
  SetShellVarContext current   ; per-user shortcuts left by an earlier installer
  Delete "$SMPROGRAMS\OnAir\OnAir.lnk"
  Delete "$SMPROGRAMS\OnAir\Uninstall.lnk"
  RMDir "$SMPROGRAMS\OnAir"
  Delete "$DESKTOP\OnAir.lnk"
  SetShellVarContext all
  Delete "$DESKTOP\OnAir.lnk"
  RMDir /r "$SMPROGRAMS\OnAir"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OnAir"
  DeleteRegKey HKLM "Software\OnAir"
SectionEnd
