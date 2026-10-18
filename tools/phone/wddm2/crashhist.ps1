# crashhist.ps1 [-App LogonUI.exe] [-Max 40] - all Application Error (1000) events of one application with exception
# code, offset and module, newest first - "was this crash there before the change?" (notes/perf-threaded-context.md).
param([string]$App = 'LogonUI.exe', [int]$Max = 40)
Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000 } -MaxEvents 2000 -ErrorAction SilentlyContinue |
  Where-Object { $_.Properties[0].Value -eq $App } | Select-Object -First $Max |
  ForEach-Object { '{0}  module {1}  code {2}  offset {3}  pid {4}' -f $_.TimeCreated, $_.Properties[3].Value, $_.Properties[6].Value, $_.Properties[7].Value, $_.Properties[8].Value }
