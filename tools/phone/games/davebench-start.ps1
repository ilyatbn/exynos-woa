# (user session, via userrun.ps1) davebench-start.ps1 - start davebench.ps1 detached and return at once (the
# scheduled-task slot runuser2.ps1 uses would otherwise be held for the whole run). notes/games-unity.md.
Remove-Item C:\S10eTmp\davebench.log -ErrorAction SilentlyContinue
Start-Process -FilePath powershell.exe -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'C:\S10eTmp\davebench.ps1' -WindowStyle Minimized
"davebench started"
