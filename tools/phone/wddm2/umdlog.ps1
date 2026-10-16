# umdlog.ps1 [-Exe dwm] [-Tail 40] - the newest malikm timing/trace log of a process (Mesa winsys malikm_tlog,
# written to %MALIKM_TGSI_DIR%\malikm-<exe>-<pid>.txt when that machine variable is set).
param([string]$Exe = 'dwm', [int]$Tail = 40)
$d = [Environment]::GetEnvironmentVariable('MALIKM_TGSI_DIR', 'Machine')
"MALIKM_TGSI_DIR = $d"
if (-not $d) { return }
$f = Get-ChildItem $d -Filter "malikm-$Exe*" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $f) { "no malikm-$Exe* log"; return }
"$($f.FullName) $($f.Length) B $($f.LastWriteTime)"
Get-Content $f.FullName -Tail $Tail
