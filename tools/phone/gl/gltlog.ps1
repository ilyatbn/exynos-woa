# gltlog.ps1 [-Name "Stardew Valley"] [-Last 4] [-Exited] [-Pattern <regex>] - the ICD's timing / present lines from
# the malikm tlog (C:\S10eTmp\tlog\malikm-<exe>-<pid>.txt): CPU present "gl present" (Mesa 0154), DXGI present
# "gl dxgi" (0158), shared-resource import "kmt open shared" / "bo-from-allocation" (0157). Default: the RUNNING
# process of that name only, so an older run's file is never read; -Exited: the newest file of that exe (a test that
# has finished). Prints nothing if there is no file / no line yet. notes/gl-icd.md.
param([string]$Name = 'Stardew Valley', [int]$Last = 4, [switch]$Exited,
      [string]$Pattern = 'gl present|gl dxgi|kmt open shared|bo-from-allocation')
if ($Exited) {
  $f = Get-ChildItem "C:\S10eTmp\tlog\malikm-$Name.exe-*.txt" -ErrorAction SilentlyContinue | Where-Object { $_.Name -notmatch 'stderr' } |
       Sort-Object LastWriteTime | Select-Object -Last 1
} else {
  $p = Get-Process $Name -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $p) { exit 1 }
  $f = Get-ChildItem "C:\S10eTmp\tlog\malikm-$Name.exe-$($p.Id).txt" -ErrorAction SilentlyContinue
}
if ($f) { Select-String -Path $f.FullName -Pattern $Pattern | Select-Object -Last $Last | ForEach-Object Line }
