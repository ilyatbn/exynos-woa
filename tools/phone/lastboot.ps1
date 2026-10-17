# lastboot.ps1 [-Minutes 30] - why the phone last went down: shutdown/restart requests (User32 1074), unexpected
# shutdown (EventLog 6008), Kernel-Power 41, BugCheck 1001, plus GPU TDRs (Display 4101), newest first.
param([int]$Minutes = 30)
Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = (Get-Date).AddMinutes(-$Minutes) } -ErrorAction SilentlyContinue |
  Where-Object { $_.Id -in 1074, 6008, 41, 1001, 4101, 6005, 6006 } |
  Select-Object -First 12 | ForEach-Object { '{0:HH:mm:ss} {1,5} {2}: {3}' -f $_.TimeCreated, $_.Id, $_.ProviderName,
    (($_.Message -split "`n")[0]).Trim() }
