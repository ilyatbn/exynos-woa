# kp41.ps1 [-Max 4] - Kernel-Power 41 events with their event data: BugcheckCode / BugcheckParameter1-4 (decimal in the
# event, printed hex). Windows records the bugcheck code there even when no dump (and so no 1001) was written.
# notes/g27-x64-apps.md.
param([int]$Max = 4)
Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Microsoft-Windows-Kernel-Power'; Id = 41 } -MaxEvents $Max |
  ForEach-Object {
    $x = [xml]$_.ToXml(); $d = @{}
    foreach ($n in $x.Event.EventData.Data) { $d[$n.Name] = $n.'#text' }
    '{0:MM-dd HH:mm:ss} BugcheckCode 0x{1:X} P1 0x{2:X} P2 0x{3:X} P3 0x{4:X} P4 0x{5:X} SleepInProgress {6} PowerButton {7}' -f $_.TimeCreated,
      [uint64]$d['BugcheckCode'], [uint64]$d['BugcheckParameter1'], [uint64]$d['BugcheckParameter2'],
      [uint64]$d['BugcheckParameter3'], [uint64]$d['BugcheckParameter4'], $d['SleepInProgress'], $d['PowerButtonTimestamp']
  }
