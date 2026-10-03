; NSIS installer for OnAir. Built by tools/package/make_windows.sh: makensis -DVERSION=... -DSRC=<folder> -DOUTFILE=<setup.exe>
!include "MUI2.nsh"
Name "OnAir"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\OnAir"
InstallDirRegKey HKLM "Software\OnAir" "Install_Dir"
RequestExecutionLevel admin
!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SRC}/LICENSE.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "OnAir"
  SetOutPath "$INSTDIR"
  File /r "${SRC}/*.*"
  WriteRegStr HKLM "Software\OnAir" "Install_Dir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OnAir" "DisplayName" "OnAir"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OnAir" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OnAir" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateDirectory "$SMPROGRAMS\OnAir"
  CreateShortcut "$SMPROGRAMS\OnAir\OnAir.lnk" "$INSTDIR\OnAir.exe"
  CreateShortcut "$SMPROGRAMS\OnAir\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
  CreateShortcut "$DESKTOP\OnAir.lnk" "$INSTDIR\OnAir.exe"
SectionEnd

Section "Uninstall"
  Delete "$DESKTOP\OnAir.lnk"
  RMDir /r "$SMPROGRAMS\OnAir"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OnAir"
  DeleteRegKey HKLM "Software\OnAir"
SectionEnd
