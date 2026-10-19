# proctree.ps1 [-Pid N] [-Names cmd.exe,powershell.exe,keyplay.exe] - pid <- parent, start time, command line of one
# process and of every process with one of the given names (which task left a console window in front; notes/games-unity.md).
param([int]$Id = 0, [string[]]$Names = @('cmd.exe', 'powershell.exe', 'keyplay.exe', 'conhost.exe'))
Get-CimInstance Win32_Process | Where-Object { $_.ProcessId -eq $Id -or $Names -contains $_.Name } | ForEach-Object {
  "{0} <- {1} {2:HH:mm:ss} s{3} {4}" -f $_.ProcessId, $_.ParentProcessId, $_.CreationDate, $_.SessionId, $_.CommandLine
}
