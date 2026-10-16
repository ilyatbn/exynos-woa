# umdgrep.ps1 -Exe dwm -Pattern 'OpenResource|error' [-Logs 3] [-Last 12] - grep the newest malikm event logs of a
# process (MALIKM_TGSI_DIR, not the -stderr captures) for a regex; one block per log, newest first.
param([string]$Exe = 'dwm', [string]$Pattern = 'OpenResource|fail|error', [int]$Logs = 3, [int]$Last = 12)
$d = [Environment]::GetEnvironmentVariable('MALIKM_TGSI_DIR', 'Machine')
Get-ChildItem $d -Filter "malikm-$Exe*.txt" -ErrorAction SilentlyContinue | Where-Object { $_.Name -notlike '*-stderr.txt' } |
  Sort-Object LastWriteTime -Descending | Select-Object -First $Logs | ForEach-Object {
    "== $($_.Name) $($_.Length) B $($_.LastWriteTime)"
    Select-String -Path $_.FullName -Pattern $Pattern | Select-Object -Last $Last | ForEach-Object { $_.Line }
  }
