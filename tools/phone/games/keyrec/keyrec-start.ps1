# (user session, via userrun.ps1) keyrec-start.ps1 [-Seconds 600] [-Out C:\S10eTmp\davekeys.rec] [-Process DaveTheDiver.exe]
# - start keyrec.exe (keyboard + mouse buttons, v2) DETACHED and return at once, so the same scheduled-task slot
# (runuser2.ps1 has one) can launch the game right after. F12 stops the recording early. notes/games-unity.md.
param([int]$Seconds = 600, [string]$Out = 'C:\S10eTmp\davekeys.rec', [string]$Process = 'DaveTheDiver.exe')
Start-Process -FilePath C:\S10eTmp\keyrec.exe -ArgumentList $Seconds, $Out, $Process
"keyrec started: $Seconds s -> $Out, times relative to $Process"
