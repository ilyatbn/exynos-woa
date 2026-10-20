# threadcpu.ps1 [-Name DaveTheDiver] [-Seconds 10] [-Top 8] - per-thread CPU use of a process over an interval (two
# samples of each thread's TotalProcessorTime): a thread near 100 % of one core is the frame's bottleneck. Also the
# process total and the number of cores. G32 (notes/games-unity.md): is Dave the Diver bound by the game's own
# (emulated) thread, our UMD's driver thread, or the GPU?
param([string]$Name = 'DaveTheDiver', [int]$Seconds = 10, [int]$Top = 8)
$p = Get-Process $Name -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { "no $Name"; exit 1 }
$a = @{}; foreach ($t in $p.Threads) { try { $a[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds } catch {} }
$p0 = $p.TotalProcessorTime.TotalMilliseconds
Start-Sleep $Seconds
$p.Refresh()
$rows = foreach ($t in $p.Threads) {
  try {
    $ms = $t.TotalProcessorTime.TotalMilliseconds
    if ($a.ContainsKey($t.Id)) { [pscustomobject]@{ Tid = $t.Id; Pct = [math]::Round(($ms - $a[$t.Id]) * 100 / ($Seconds * 1000), 1); Prio = $t.CurrentPriority } }
  } catch {}
}
"{0}: process {1:N0} % of one core over {2} s, {3} cores" -f $Name, (($p.TotalProcessorTime.TotalMilliseconds - $p0) * 100 / ($Seconds * 1000)), $Seconds, [Environment]::ProcessorCount
# thread names (documented GetThreadDescription; Unity and Mesa's util_queue name their threads)
Add-Type -Namespace TC -Name K -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern System.IntPtr OpenThread(uint a, bool i, uint id);
[System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern int GetThreadDescription(System.IntPtr h, out System.IntPtr d);
[System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern bool CloseHandle(System.IntPtr h);
[System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern System.IntPtr LocalFree(System.IntPtr p);
'@
function TName([int]$tid) {
  $h = [TC.K]::OpenThread(0x1000, $false, [uint32]$tid)   # THREAD_QUERY_LIMITED_INFORMATION
  if ($h -eq [IntPtr]::Zero) { return '?' }
  $d = [IntPtr]::Zero; $n = ''
  if ([TC.K]::GetThreadDescription($h, [ref]$d) -ge 0 -and $d -ne [IntPtr]::Zero) {
    $n = [Runtime.InteropServices.Marshal]::PtrToStringUni($d); [void][TC.K]::LocalFree($d)
  }
  [void][TC.K]::CloseHandle($h)
  return $n
}
$rows | Sort-Object Pct -Descending | Select-Object -First $Top | ForEach-Object { "  tid {0,6}  {1,5} %  prio {2}  '{3}'" -f $_.Tid, $_.Pct, $_.Prio, (TName $_.Tid) }
