# (user session, via run-as-user.ps1) keyrec-run.ps1 - record the keyboard for up to 180 s (F12 stops) into C:\S10eTmp\gamekeys.rec.
Start-Process -Wait -FilePath C:\S10eTmp\keyrec.exe -ArgumentList 180
