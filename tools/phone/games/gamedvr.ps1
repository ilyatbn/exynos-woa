# (user session, via run-as-user.ps1) gamedvr.ps1 - DIAGNOSIS SWITCH (AGENTS rule 12), not a fix: set the two values of
# Settings > Gaming "record game clips" - HKCU\System\GameConfigStore GameDVR_Enabled and
# HKCU\Software\Microsoft\Windows\CurrentVersion\GameDVR AppCaptureEnabled - to the value in C:\S10eTmp\gamedvr.val
# (0 = off for a test run; restore 1 / absent afterwards). Probes the "ms-gamingoverlay" popup that crashes NFSU2 on
# this image without the Xbox Game Bar app. Previous values -> C:\S10eTmp\gamedvr.out. notes/games-nfsu2.md.
$v = [int](Get-Content C:\S10eTmp\gamedvr.val -Raw).Trim()
$k1 = 'HKCU:\System\GameConfigStore'; $k2 = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\GameDVR'
$o1 = (Get-ItemProperty $k1 -ErrorAction SilentlyContinue).GameDVR_Enabled
$o2 = (Get-ItemProperty $k2 -ErrorAction SilentlyContinue).AppCaptureEnabled
if (-not (Test-Path $k2)) { New-Item $k2 -Force | Out-Null }
Set-ItemProperty $k1 -Name GameDVR_Enabled -Value $v -Type DWord
Set-ItemProperty $k2 -Name AppCaptureEnabled -Value $v -Type DWord
"GameDVR_Enabled '$o1' -> $v; AppCaptureEnabled '$o2' -> $v" | Set-Content C:\S10eTmp\gamedvr.out
