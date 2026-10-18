# (user session; started by screenrec.exe as its launch target) gamedrive.ps1 - start the game from
# C:\S10eTmp\gamelaunch.args like gamefg.ps1 (synthetic Alt + AllowSetForegroundWindow(ASFW_ANY): a task-started
# process never owns the foreground), then play C:\S10eTmp\gamekeys.txt: one "WAIT_MS KEY [HOLD_MS]" per line, sent as
# scan codes (DirectInput games read scan codes). KEY: ESC ENTER UP DOWN LEFT RIGHT SPACE or a hex scan code.
# Log -> C:\S10eTmp\gamedrive.out (game pid, alive/working set every key, exit code). Lets the screen recorder
# (tools/phone/rec) watch a game for a remote viewer. notes/games-nfsu2.md.
Add-Type -Namespace W -Name K -MemberDefinition @'
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra);
[DllImport("user32.dll")] public static extern bool AllowSetForegroundWindow(int pid);
'@
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
  $c = if ($codes.ContainsKey($f[1])) { $codes[$f[1]] } else { [Convert]::ToInt32($f[1], 16) }
  $hold = if ($f.Count -ge 3) { [int]$f[2] } else { 80 }
  $flags = 0x8 -bor ($(if ($c -gt 0x7F) { 0x1 } else { 0 }))      # KEYEVENTF_SCANCODE (+ EXTENDEDKEY for arrows)
  [W.K]::keybd_event(0, [byte]($c -band 0x7F), $flags, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds $hold
  [W.K]::keybd_event(0, [byte]($c -band 0x7F), $flags -bor 0x2, [UIntPtr]::Zero)
  $st = if ($p.HasExited) { 'EXITED 0x{0:X8}' -f $p.ExitCode } else { "alive ws $([int]((Get-Process -Id $p.Id).WorkingSet64 / 1MB)) MB" }
  "t+{0:N1} s {1} hold {2} -> {3}" -f ((Get-Date) - $t0).TotalSeconds, $f[1], $hold, $st | Add-Content $out
}
