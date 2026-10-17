# (user session, via run-as-user.ps1) gamelaunch.ps1 with debug-output capture: dbgcap.exe listens 30 s
# (C:\S10eTmp\dbgcap.txt) while the game in gamelaunch.args starts and is watched; the script waits for dbgcap to end
# (the scheduled task kills leftovers when it finishes). notes/games-nfsu2.md.
Remove-Item C:\S10eTmp\dbgcap.txt -ErrorAction SilentlyContinue
$cap = Start-Process -FilePath C:\S10eTmp\dbgcap.exe -ArgumentList '30', 'C:\S10eTmp\dbgcap.txt' -WindowStyle Hidden -PassThru
Start-Sleep -Milliseconds 500
& C:\S10eTmp\gamelaunch.ps1
$cap | Wait-Process -Timeout 40 -ErrorAction SilentlyContinue
