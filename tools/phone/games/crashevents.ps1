# crashevents.ps1 [-Exe SPEED2] [-Count 3] - the newest Application Error (1000) events of a process: time, pid,
# faulting module + offset, exception. notes/g25-geometry-shaders.md.
param([string]$Exe = 'SPEED2', [int]$Count = 3)
Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000 } -MaxEvents 50 -ErrorAction SilentlyContinue |
  Where-Object { $_.Message -match $Exe } | Select-Object -First $Count | ForEach-Object {
    $m = $_.Message
    $pid_ = if ($m -match 'Faulting process id: (0x[0-9A-Fa-fx]+)') { $Matches[1] } else { '?' }
    $mod = if ($m -match 'Faulting module name: ([^,]+)') { $Matches[1] } else { '?' }
    $off = if ($m -match 'Fault offset: (0x[0-9a-fA-F]+)') { $Matches[1] } else { '?' }
    $exc = if ($m -match 'Exception code: (0x[0-9a-fA-F]+)') { $Matches[1] } else { '?' }
    "$($_.TimeCreated) pid $pid_ $mod +$off exception $exc"
  }
