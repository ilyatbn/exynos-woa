# gltlog.ps1 [-Name "Stardew Valley"] [-Last 4] - the ICD's "gl present" timing lines (Mesa 0154) from the malikm tlog
# of the RUNNING process of that name (C:\S10eTmp\tlog\malikm-<exe>-<pid>.txt), so an older run's file is never read.
# Prints nothing if the process or its lines are not there yet. notes/gl-icd.md.
param([string]$Name = 'Stardew Valley', [int]$Last = 4)
$p = Get-Process $Name -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { exit 1 }
$f = Get-ChildItem "C:\S10eTmp\tlog\malikm-$Name.exe-$($p.Id).txt" -ErrorAction SilentlyContinue
if ($f) { Select-String -Path $f.FullName -Pattern 'gl present' | Select-Object -Last $Last | ForEach-Object Line }
