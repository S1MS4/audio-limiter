; Audio Limiter installer.
;
; Per-user install into LOCALAPPDATA rather than Program Files: it needs no
; administrator rights, so there is no UAC prompt, and the app never needs
; elevation at runtime either. That keeps the whole flow to three clicks.

!include "MUI2.nsh"
!include "FileFunc.nsh"

Name "Audio Limiter"
OutFile "..\dist\AudioLimiter-Setup.exe"
Unicode True
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\AudioLimiter"
InstallDirRegKey HKCU "Software\AudioLimiter" "InstallDir"
ShowInstDetails hide
ShowUninstDetails hide

VIProductVersion "1.0.0.0"
VIAddVersionKey "ProductName" "Audio Limiter"
VIAddVersionKey "FileDescription" "Volume ceiling for Discord voice"
VIAddVersionKey "LegalCopyright" "MIT"
VIAddVersionKey "FileVersion" "1.0.0.0"
VIAddVersionKey "ProductVersion" "1.0.0.0"

!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\AudioLimiter.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start Audio Limiter now"
!define MUI_FINISHPAGE_TEXT "Audio Limiter is installed.$\r$\n$\r$\nIt runs in your system tray. Right-click the tray icon to set your volume limit."

!insertmacro MUI_PAGE_LICENSE "..\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ; A running copy holds a lock on its own exe, so the file copy below would
  ; fail with a permission error rather than anything the user can act on.
  FindWindow $0 "AudioLimiterTrayWnd" ""
  StrCmp $0 0 done
    MessageBox MB_OKCANCEL|MB_ICONINFORMATION \
      "Audio Limiter is currently running and will be closed to continue." \
      IDOK closeit
    Abort
  closeit:
    SendMessage $0 ${WM_CLOSE} 0 0
    Sleep 1500
  done:
FunctionEnd

Section "Install"
  SetOutPath "$INSTDIR"
  File "..\build\AudioLimiter.exe"
  File "..\LICENSE"
  File "..\README.md"

  WriteRegStr HKCU "Software\AudioLimiter" "InstallDir" "$INSTDIR"

  ; Start with Windows. The app reads the same value, so the tray menu
  ; checkbox and this stay in sync.
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Run" \
                   "AudioLimiter" '"$INSTDIR\AudioLimiter.exe"'

  CreateShortcut "$SMPROGRAMS\Audio Limiter.lnk" "$INSTDIR\AudioLimiter.exe"

  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\AudioLimiter" \
                   "DisplayName" "Audio Limiter"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\AudioLimiter" \
                   "DisplayVersion" "1.0.0"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\AudioLimiter" \
                   "Publisher" "S1MS4"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\AudioLimiter" \
                   "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\AudioLimiter" \
                   "DisplayIcon" "$INSTDIR\AudioLimiter.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\AudioLimiter" \
                   "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\AudioLimiter" \
                     "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\AudioLimiter" \
                     "NoRepair" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\AudioLimiter" \
                     "EstimatedSize" "$0"
SectionEnd

Section "Uninstall"
  ; Closing the app cleanly matters more here than anywhere else: its shutdown
  ; path is what returns Discord's audio to the normal output device. Killing
  ; it instead would leave the user with silent Discord and no app left to
  ; explain why.
  FindWindow $0 "AudioLimiterTrayWnd" ""
  StrCmp $0 0 gone
    SendMessage $0 ${WM_CLOSE} 0 0
    Sleep 2000
  gone:

  Delete "$INSTDIR\AudioLimiter.exe"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"

  Delete "$SMPROGRAMS\Audio Limiter.lnk"

  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "AudioLimiter"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\AudioLimiter"
  DeleteRegKey HKCU "Software\AudioLimiter"

  ; Settings live in APPDATA and are left alone, so reinstalling keeps the
  ; user's chosen limit. The routing state file is removed because a stale one
  ; would make a future install think it needs to recover.
  Delete "$APPDATA\AudioLimiter\routing.state"
SectionEnd
