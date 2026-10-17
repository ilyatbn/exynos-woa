# (user session, via run-as-user.ps1) gameshortcut.ps1 - a desktop shortcut for the game in C:\S10eTmp\gamelaunch.args,
# so the user starts it in the foreground (a launch from the scheduled task never owns the foreground: a fullscreen
# swapchain drops out at once - notes/g24-compute-cs4x.md).
$exe = (Get-Content C:\S10eTmp\gamelaunch.args -Raw).Trim()
$w = New-Object -ComObject WScript.Shell
$l = $w.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) ([IO.Path]::GetFileNameWithoutExtension($exe) + ".lnk")))
$l.TargetPath = $exe
$l.WorkingDirectory = Split-Path $exe
$l.Save()
"shortcut $($l.FullName)" | Set-Content C:\S10eTmp\gameshortcut.out
