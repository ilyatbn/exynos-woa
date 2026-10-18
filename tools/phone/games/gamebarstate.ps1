# (user session, via run-as-user.ps1) gamebarstate.ps1 - why does a fullscreen game get the "Get an app to open this
# 'ms-gamingoverlay' link" popup? Game Bar / Game DVR user settings, the Xbox Game Bar package, the ms-gamingoverlay
# protocol handler. -> C:\S10eTmp\gamebar.out. notes/games-nfsu2.md.
$o = @()
$o += 'GameDVR: ' + ((Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\GameDVR' -ErrorAction SilentlyContinue | Select-Object AppCaptureEnabled, HistoricalCaptureEnabled | Out-String).Trim() -replace '\s+', ' ')
$o += 'GameConfigStore: ' + ((Get-ItemProperty 'HKCU:\System\GameConfigStore' -ErrorAction SilentlyContinue | Select-Object GameDVR_Enabled, GameDVR_FSEBehaviorMode | Out-String).Trim() -replace '\s+', ' ')
$o += 'GameBar: ' + ((Get-ItemProperty 'HKCU:\Software\Microsoft\GameBar' -ErrorAction SilentlyContinue | Select-Object UseNexusForGameBarEnabled, ShowStartupPanel, AutoGameModeEnabled | Out-String).Trim() -replace '\s+', ' ')
$o += 'packages: ' + ((Get-AppxPackage *Xbox* , *GamingOverlay*, *GamingApp* -ErrorAction SilentlyContinue | ForEach-Object Name) -join ', ')
$o += 'protocol ms-gamingoverlay: ' + (Test-Path 'Registry::HKEY_CLASSES_ROOT\ms-gamingoverlay')
$o | Set-Content C:\S10eTmp\gamebar.out
