# shellstate.ps1 - is the desktop shell up? explorer / shell hosts (start time, responding), the last Application
# Error / Hang events (1000/1001/1002) and the boot time (the phone clock resets at boot - compare to it). G27.
"boot $((Get-CimInstance Win32_OperatingSystem).LastBootUpTime)  now $(Get-Date)"
Get-Process explorer, ShellExperienceHost, StartMenuExperienceHost, ShellHost, dwm -ErrorAction SilentlyContinue |
  Select-Object Name, Id, StartTime, Responding | Format-Table -AutoSize | Out-String -Width 200
Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000, 1001, 1002 } -MaxEvents 8 -ErrorAction SilentlyContinue |
  ForEach-Object { '{0:MM-dd HH:mm:ss} {1} {2}' -f $_.TimeCreated, $_.Id, ((($_.Message -split "`n") | Select-Object -First 3) -join ' | ') }
