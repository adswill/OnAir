; NSIS installer for OnAir. Built by tools/package/make_windows.sh: makensis -DVERSION=... -DSRC=<folder> -DOUTFILE=<setup.exe>
!ifndef SEP
  !define SEP "/"
!endif
!include "MUI2.nsh"
!include "x64.nsh"
Name "OnAir"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\OnAir"
InstallDirRegKey HKLM "Software\OnAir" "Install_Dir"
RequestExecutionLevel admin
!define UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\OnAir"
!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SRC}${SEP}LICENSE.txt"
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE DirLeave
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_FUNCTION StartOnAir
!define MUI_FINISHPAGE_RUN_TEXT "Start OnAir"
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "OnAir needs 64-bit Windows 10 or 11."
    Abort
  ${EndIf}
FunctionEnd

; The uninstaller removes the whole install folder: a folder that is not OnAir's own (C:\Program Files, C:\Tools ...) would lose everything in it.
Function DirLeave
  StrLen $0 "$INSTDIR"
  IntOp $0 $0 - 5
  ${If} $0 < 0
    StrCpy $INSTDIR "$INSTDIR\OnAir"
  ${Else}
    StrCpy $1 "$INSTDIR" 5 $0
    StrCmp $1 "OnAir" +2 0   ; (not case sensitive)
    StrCpy $INSTDIR "$INSTDIR\OnAir"
  ${EndIf}
FunctionEnd

; Started through explorer.exe: the program then runs with the rights of the user, not as the administrator that the installer is (the same way the updater starts it)
Function StartOnAir
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\OnAir.exe"'
FunctionEnd

Section "OnAir"
  SetShellVarContext current   ; remove the per-user shortcuts that version 0.1.0's first installer created
  Delete "$SMPROGRAMS\OnAir\OnAir.lnk"
  Delete "$SMPROGRAMS\OnAir\Uninstall.lnk"
  RMDir "$SMPROGRAMS\OnAir"
  Delete "$DESKTOP\OnAir.lnk"
  SetShellVarContext all   ; the program is installed for all users: so are its shortcuts
  SetOutPath "$INSTDIR"
  ; an update over an older OnAir: its libraries go first, so that none of a former version stays (an old SoapySDR module next to new libraries, ...)
  IfFileExists "$INSTDIR\OnAir.exe" 0 +4
    Delete "$INSTDIR\*.dll"
    RMDir /r "$INSTDIR\lib"
    RMDir /r "$INSTDIR\licenses"
  File /r "${SRC}${SEP}*.*"
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
