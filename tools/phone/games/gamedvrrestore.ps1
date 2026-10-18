# (user session, via run-as-user.ps1) gamedvrrestore.ps1 - undo gamedvr.ps1 exactly: GameDVR_Enabled = 1 (was 1),
# AppCaptureEnabled removed (was absent). -> C:\S10eTmp\gamedvr.out. notes/games-nfsu2.md.
Set-ItemProperty 'HKCU:\System\GameConfigStore' -Name GameDVR_Enabled -Value 1 -Type DWord
Remove-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\GameDVR' -Name AppCaptureEnabled -ErrorAction SilentlyContinue
"restored: GameDVR_Enabled $((Get-ItemProperty 'HKCU:\System\GameConfigStore').GameDVR_Enabled), AppCaptureEnabled '$((Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\GameDVR' -ErrorAction SilentlyContinue).AppCaptureEnabled)'" | Set-Content C:\S10eTmp\gamedvr.out
