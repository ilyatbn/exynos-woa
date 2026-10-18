# (user session, via run-as-user.ps1) x64app.ps1 - G27.5 real-app check: start an x64 application (default foobar2000,
# no playback), wait, then shot.ps1. Which UMD it loaded: `tasklist /m maliumd*` over ssh afterwards.
param([string]$Exe = 'C:\Program Files\foobar2000\foobar2000.exe', [int]$Wait = 15)
Start-Process -FilePath $Exe
Start-Sleep -Seconds $Wait
& C:\S10eTmp\shot.ps1 -Out C:\S10eTmp\x64app.png
