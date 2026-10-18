# (user session; started by screenrec.exe as its launch target) gamedrive.ps1 - start the game from
# C:\S10eTmp\gamelaunch.args like gamefg.ps1 (synthetic Alt + AllowSetForegroundWindow(ASFW_ANY): a task-started
# process never owns the foreground), then play C:\S10eTmp\gamekeys.txt: one "WAIT_MS KEY [HOLD_MS]" per line, sent as
# scan codes (DirectInput games read scan codes). KEY: ESC ENTER UP DOWN LEFT RIGHT SPACE CLICK or a hex scan code.
# All input comes from this one process: a second injector process (sendkeys.ps1) takes the foreground for a moment
# and the game minimises.
# Log -> C:\S10eTmp\gamedrive.out (game pid, alive/working set every key, exit code). Lets the screen recorder
# (tools/phone/rec) watch a game for a remote viewer. notes/games-nfsu2.md.
Add-Type -Namespace W -Name K -MemberDefinition @'
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra);
[DllImport("user32.dll")] public static extern bool AllowSetForegroundWindow(int pid);
[DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, System.UIntPtr extra);
[DllImport("user32.dll", EntryPoint = "SystemParametersInfo")] public static extern bool SpiGet(uint a, uint b, ref int c, uint d);
[DllImport("user32.dll", EntryPoint = "SystemParametersInfo")] public static extern bool SpiSet(uint a, uint b, System.IntPtr c, uint d);
'@
# the screensaver takes the foreground from a fullscreen game and NFSU2 dies (+0x6562b): off while the game runs,
# the previous value restored when it exits (SPI_GET/SETSCREENSAVEACTIVE 0x10 / 0x11)
$ssWas = 0; [void][W.K]::SpiGet(0x10, 0, [ref]$ssWas, 0); [void][W.K]::SpiSet(0x11, 0, [IntPtr]::Zero, 0)
# a screensaver already running when we start covers the game (scan-out black, foreground 0 - nfs6/nfs7): close it
Get-Process | Where-Object { $_.Path -like '*.scr' } | Stop-Process -Force -ErrorAction SilentlyContinue
$out = 'C:\S10eTmp\gamedrive.out'
$exe = (Get-Content C:\S10eTmp\gamelaunch.args -Raw).Trim()
"start $exe $(Get-Date -Format HH:mm:ss.fff)" | Set-Content $out
[W.K]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [W.K]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
[void][W.K]::AllowSetForegroundWindow(-1)
$p = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -PassThru
"pid $($p.Id)" | Add-Content $out
$codes = @{ ESC = 0x01; ENTER = 0x1C; SPACE = 0x39; UP = 0xC8; DOWN = 0xD0; LEFT = 0xCB; RIGHT = 0xCD }
$t0 = Get-Date
$keys = if (Test-Path C:\S10eTmp\gamekeys.txt) { Get-Content C:\S10eTmp\gamekeys.txt } else { @() }
foreach ($line in $keys) {
  $f = ($line -split '#')[0].Trim() -split '\s+'
  if ($f.Count -lt 2) { continue }
  Start-Sleep -Milliseconds ([int]$f[0])
  if ($f[1] -eq 'PLAY') {    # "0 PLAY C:\S10eTmp\gamekeys.rec": replay a keyrec recording (keyplay.exe), timed from now
    $rec = if ($f.Count -ge 3) { $f[2] } else { 'C:\S10eTmp\gamekeys.rec' }
    "t+{0:N1} s PLAY {1}" -f ((Get-Date) - $t0).TotalSeconds, $rec | Add-Content $out
    Start-Process -Wait -FilePath C:\S10eTmp\keyplay.exe -ArgumentList $rec
    $st = if ($p.HasExited) { 'EXITED 0x{0:X8}' -f $p.ExitCode } else { "alive ws $([int]((Get-Process -Id $p.Id).WorkingSet64 / 1MB)) MB" }
    "t+{0:N1} s PLAY done -> {1}" -f ((Get-Date) - $t0).TotalSeconds, $st | Add-Content $out
    continue
  }
  if ($f[1] -eq 'CLICK') {   # left click at the screen centre - activates the game window the way a user would
    [W.K]::mouse_event(0x8001, 32768, 32768, 0, [UIntPtr]::Zero)
    [W.K]::mouse_event(0x2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80; [W.K]::mouse_event(0x4, 0, 0, 0, [UIntPtr]::Zero)
    "t+{0:N1} s CLICK" -f ((Get-Date) - $t0).TotalSeconds | Add-Content $out
    continue
  }
  $c = if ($codes.ContainsKey($f[1])) { $codes[$f[1]] } else { [Convert]::ToInt32($f[1], 16) }
  $hold = if ($f.Count -ge 3) { [int]$f[2] } else { 80 }
  $flags = 0x8 -bor ($(if ($c -gt 0x7F) { 0x1 } else { 0 }))      # KEYEVENTF_SCANCODE (+ EXTENDEDKEY for arrows)
  [W.K]::keybd_event(0, [byte]($c -band 0x7F), $flags, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds $hold
  [W.K]::keybd_event(0, [byte]($c -band 0x7F), $flags -bor 0x2, [UIntPtr]::Zero)
  $st = if ($p.HasExited) { 'EXITED 0x{0:X8}' -f $p.ExitCode } else { "alive ws $([int]((Get-Process -Id $p.Id).WorkingSet64 / 1MB)) MB" }
  "t+{0:N1} s {1} hold {2} -> {3}" -f ((Get-Date) - $t0).TotalSeconds, $f[1], $hold, $st | Add-Content $out
}
# keep the screensaver off until the game exits (max 30 min), then restore it
$p.WaitForExit(1800000) | Out-Null
[void][W.K]::SpiSet(0x11, [uint32]$ssWas, [IntPtr]::Zero, 0)
"game ended $(if ($p.HasExited) { 'exit 0x{0:X8}' -f $p.ExitCode } else { 'still running' }) $(Get-Date -Format HH:mm:ss); screensaver restored to $ssWas" | Add-Content $out
