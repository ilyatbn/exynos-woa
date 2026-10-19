# launchtime.ps1 -App brave|steam|explorer|<full exe path> [-TimeoutS 120] - (user session, via run-as-user / runapp.sh)
# measure one app launch: ms to the first visible top-level window of the app, ms until its process tree is idle
# (CPU < 3 % of one core over 2 s), physical-disk bytes/reads during that time (PerfDisk raw counters), CPU seconds of
# the app's processes. Appends one line to C:\S10eTmp\launchtime.txt. notes/app-launch-slowness.md.
param([string]$App = "explorer", [int]$TimeoutS = 120)
$ErrorActionPreference = "SilentlyContinue"
Add-Type -Namespace W -Name U -MemberDefinition @'
public delegate bool EnumProc(System.IntPtr h, System.IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, System.IntPtr l);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(System.IntPtr h);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr h, out uint pid);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(System.IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextLength(System.IntPtr h);
'@
function Find-Exe($a) {
  if (Test-Path $a) { return $a }
  $roots = @("$env:ProgramFiles", "${env:ProgramFiles(x86)}", "$env:LOCALAPPDATA")
  $name = @{ brave = "brave.exe"; steam = "steam.exe" }[$a]
  foreach ($r in $roots) { $f = Get-ChildItem $r -Recurse -Filter $name -Depth 4 | Select-Object -First 1; if ($f) { return $f.FullName } }
  return $null
}
function Disk { $d = Get-CimInstance Win32_PerfRawData_PerfDisk_PhysicalDisk | ? Name -eq "_Total"; @([uint64]$d.DiskReadBytesPersec, [uint64]$d.DiskReadsPersec) }
function Windows($procName, $cls) {
  $script:hits = 0
  $pids = @(Get-Process -Name $procName | % Id) + @(Get-Process -Name "${procName}webhelper" | % Id)   # Steam's UI = steamwebhelper
  $cb = [W.U+EnumProc]{ param($h, $l)
    if ([W.U]::IsWindowVisible($h) -and [W.U]::GetWindowTextLength($h) -gt 0) {
      $p = 0; [void][W.U]::GetWindowThreadProcessId($h, [ref]$p)
      $sb = New-Object System.Text.StringBuilder 128; [void][W.U]::GetClassName($h, $sb, 128)
      if (($cls -and $sb.ToString() -eq $cls) -or (-not $cls -and $pids -contains [int]$p)) { $script:hits++ } }
    return $true }
  [void][W.U]::EnumWindows($cb, [IntPtr]::Zero); $script:hits
}
if ($App -eq "explorer") { $exe = "explorer.exe"; $arg = "C:\Windows"; $pn = "explorer"; $cls = "CabinetWClass" }
else { $exe = Find-Exe $App; $arg = $null; $pn = [IO.Path]::GetFileNameWithoutExtension($exe); $cls = $null }
if (-not $exe) { "$(Get-Date -f HH:mm:ss) $App NOT FOUND" | Add-Content C:\S10eTmp\launchtime.txt; exit 1 }
$before = Windows $pn $cls
$cpu0 = (Get-Process -Name $pn | Measure-Object CPU -Sum).Sum
$d0 = Disk
$sw = [Diagnostics.Stopwatch]::StartNew()
if ($arg) { Start-Process $exe $arg } else { Start-Process $exe }
$tWin = -1
while ($sw.ElapsedMilliseconds -lt $TimeoutS * 1000) {
  if ((Windows $pn $cls) -gt $before) { $tWin = $sw.ElapsedMilliseconds; break }
  Start-Sleep -Milliseconds 50
}
# settle: app CPU < 3 % of one core over a 2 s window
$tIdle = -1; $last = (Get-Process -Name $pn | Measure-Object CPU -Sum).Sum
while ($sw.ElapsedMilliseconds -lt $TimeoutS * 1000) {
  Start-Sleep -Milliseconds 2000
  $now = (Get-Process -Name $pn | Measure-Object CPU -Sum).Sum
  if (($now - $last) -lt 0.06) { $tIdle = $sw.ElapsedMilliseconds - 2000; break }
  $last = $now
}
$d1 = Disk
$cpu = (Get-Process -Name $pn | Measure-Object CPU -Sum).Sum - $cpu0
"{0} {1} window_ms={2} idle_ms={3} disk_MB={4:N1} disk_reads={5} app_cpu_s={6:N1} procs={7} exe={8}" -f (Get-Date -f HH:mm:ss), $App, $tWin, $tIdle,
  (($d1[0] - $d0[0]) / 1MB), ($d1[1] - $d0[1]), $cpu, (Get-Process -Name $pn).Count, $exe | Add-Content C:\S10eTmp\launchtime.txt
