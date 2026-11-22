; NSIS hooks injected into the generated Tauri installer.
;
; The 1.x Qt build was installed per-machine by Inno Setup into
; "C:\Program Files (x86)\NcduWin" and registered itself under HKLM plus a
; Start Menu group for all users. Upgrading with the per-user Tauri installer
; left that copy behind, so Windows Search kept launching the old executable
; and "Apps & features" kept showing an entry that could not be removed.
;
; This hook removes the legacy installation before the new files are copied.

!include LogicLib.nsh

!define LEGACY_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\{B8F4A3D2-1E5C-4A7B-9D6F-2C8E0A1B3D4F}_is1"

!macro NSIS_HOOK_PREINSTALL
  Call RemoveLegacyInstall
!macroend

Function RemoveLegacyInstall
  ; Inno Setup is 32-bit, so its uninstall key lives in the WOW6432Node view.
  SetRegView 32
  ReadRegStr $0 HKLM "${LEGACY_UNINST_KEY}" "InstallLocation"
  SetRegView lastused

  ${If} $0 == ""
    StrCpy $0 "$PROGRAMFILES32\NcduWin"
  ${EndIf}

  ; Only treat the folder as ours when the Inno uninstaller is present; this
  ; keeps a corrupted registry value from deleting an unrelated directory.
  ${If} ${FileExists} "$0\unins000.exe"
    ; A running 1.x executable would block its own removal.
    ExecWait 'taskkill /F /IM NcduWin.exe'

    ExecWait '"$0\unins000.exe" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART'

    ; Inno copies itself to %TEMP% and returns early, so wait for the original
    ; uninstaller to disappear before deleting whatever is left.
    StrCpy $1 0
    ${Do}
      Sleep 500
      IntOp $1 $1 + 1
      ${IfNot} ${FileExists} "$0\unins000.exe"
        ${ExitDo}
      ${EndIf}
    ${LoopWhile} $1 < 60

    RMDir /r "$0"
    RMDir "$0"
  ${EndIf}

  ; Shortcuts created for all users by the 1.x installer.
  SetShellVarContext all
  Delete "$SMPROGRAMS\NcduWin\NcduWin.lnk"
  Delete "$SMPROGRAMS\NcduWin\Uninstall NcduWin.lnk"
  RMDir "$SMPROGRAMS\NcduWin"
  Delete "$DESKTOP\NcduWin.lnk"
  SetShellVarContext current

  ; Drop the legacy "Apps & features" entry even if its files are already gone.
  SetRegView 32
  DeleteRegKey HKLM "${LEGACY_UNINST_KEY}"
  SetRegView lastused
FunctionEnd
