# umdlog.ps1 [-Exe dwm] [-Tail 40] - the newest malikm timing/trace log of a process (Mesa winsys malikm_tlog,
# written to %MALIKM_TGSI_DIR%\malikm-<exe>-<pid>.txt when that machine variable is set).
param([string]$Exe = 'dwm', [int]$Tail = 40, [switch]$Stderr, [int]$ProcId = 0, [string]$Match = '')
# -ProcId: that process's log (the newest-by-time pick goes wrong across boots - the phone clock resets);
# -Match: only lines matching this regex (e.g. 'app frames|app umd top').
$d = [Environment]::GetEnvironmentVariable('MALIKM_TGSI_DIR', 'Machine')
"MALIKM_TGSI_DIR = $d"
if (-not $d) { return }
$filter = if ($ProcId) { "malikm-$Exe*-$ProcId*" } else { "malikm-$Exe*" }
$f = Get-ChildItem $d -Filter $filter -ErrorAction SilentlyContinue | Where-Object { ($_.Name -like '*-stderr.txt') -eq [bool]$Stderr } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $f) { "no malikm-$Exe* log"; return }
"$($f.FullName) $($f.Length) B $($f.LastWriteTime)"
if ($Match) { Get-Content $f.FullName | Where-Object { $_ -match $Match } | Select-Object -Last $Tail | ForEach-Object { $_.Substring(0, [Math]::Min(240, $_.Length)) } }
else { Get-Content $f.FullName -Tail $Tail }
