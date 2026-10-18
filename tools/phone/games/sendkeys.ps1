# (user session, via run-as-user.ps1) sendkeys.ps1 - press keys in the foreground window (the running game) from
# C:\S10eTmp\sendkeys.txt, same "WAIT_MS KEY [HOLD_MS]" format and scan codes as gamedrive.ps1. For step-by-step
# menu driving with a scan-out dump between steps. KEY CLICK = left click at the screen centre (MOUSEEVENTF_ABSOLUTE
# 0x8000 | MOVE 0x1, then LEFTDOWN 0x2 / LEFTUP 0x4): a task-started game window never gets the foreground (fgstate.ps1:
# foreground 0) and stays black; a click activates it the way a user would. -> appends to C:\S10eTmp\gamedrive.out. notes/games-nfsu2.md.
Add-Type -Namespace W -Name S -MemberDefinition @'
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra);
[DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, System.UIntPtr extra);
'@
$codes = @{ ESC = 0x01; ENTER = 0x1C; SPACE = 0x39; UP = 0xC8; DOWN = 0xD0; LEFT = 0xCB; RIGHT = 0xCD }
foreach ($line in (Get-Content C:\S10eTmp\sendkeys.txt)) {
  $f = ($line -split '#')[0].Trim() -split '\s+'
  if ($f.Count -lt 2) { continue }
  Start-Sleep -Milliseconds ([int]$f[0])
  if ($f[1] -eq 'CLICK') {
    [W.S]::mouse_event(0x8001, 32768, 32768, 0, [UIntPtr]::Zero)
    [W.S]::mouse_event(0x2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80; [W.S]::mouse_event(0x4, 0, 0, 0, [UIntPtr]::Zero)
    "sendkeys $(Get-Date -Format HH:mm:ss) CLICK" | Add-Content C:\S10eTmp\gamedrive.out
    continue
  }
  $c = if ($codes.ContainsKey($f[1])) { $codes[$f[1]] } else { [Convert]::ToInt32($f[1], 16) }
  $hold = if ($f.Count -ge 3) { [int]$f[2] } else { 80 }
  $flags = 0x8 -bor ($(if ($c -gt 0x7F) { 0x1 } else { 0 }))
  [W.S]::keybd_event(0, [byte]($c -band 0x7F), $flags, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds $hold
  [W.S]::keybd_event(0, [byte]($c -band 0x7F), $flags -bor 0x2, [UIntPtr]::Zero)
  "sendkeys $(Get-Date -Format HH:mm:ss) $($f[1]) hold $hold" | Add-Content C:\S10eTmp\gamedrive.out
}
