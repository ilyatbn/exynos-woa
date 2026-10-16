# werdump.ps1 -Exe msedge.exe [-Off] - WER LocalDumps (mini dump with heap = type 2 off by default: type 1 mini) for one
# executable into C:\S10eTmp\dumps; catches fail-fast (0xC0000409) crashes that skip the app's own crash reporter.
param([Parameter(Mandatory)][string]$Exe, [switch]$Off, [int]$Type = 1)
$k = "HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\$Exe"
if ($Off) { Remove-Item $k -Recurse -ErrorAction SilentlyContinue; "LocalDumps $Exe off"; return }
New-Item -ItemType Directory -Force C:\S10eTmp\dumps | Out-Null
New-Item -Force $k | Out-Null
Set-ItemProperty $k DumpFolder 'C:\S10eTmp\dumps' -Type ExpandString
Set-ItemProperty $k DumpType $Type -Type DWord
Set-ItemProperty $k DumpCount 5 -Type DWord
"LocalDumps $Exe -> C:\S10eTmp\dumps (type $Type)"
