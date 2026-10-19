# closeapps.ps1 - (user session) close what launchtime.ps1 opened: File Explorer windows (Shell.Application, the shell
# process stays), Brave and Steam (graceful close, then kill after 10 s). notes/app-launch-slowness.md.
(New-Object -ComObject Shell.Application).Windows() | ForEach-Object { try { $_.Quit() } catch {} }
foreach ($n in "brave", "steam", "steamwebhelper") { Get-Process -Name $n -ErrorAction SilentlyContinue | ForEach-Object { [void]$_.CloseMainWindow() } }
Start-Sleep 10
foreach ($n in "brave", "steamwebhelper", "steam") { Stop-Process -Name $n -Force -ErrorAction SilentlyContinue }
