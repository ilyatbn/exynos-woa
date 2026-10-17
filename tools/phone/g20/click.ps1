# (user session, via run-as-user.ps1) left-click at the fractions of the primary screen in C:\S10eTmp\click.args
# ("0.5 0.74 0.51 0.52" = Game Dev Story Continue, then the autosave, landscape) - G20 gameplay fps runs, resolution independent.
Add-Type -Namespace W -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
[DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, System.UIntPtr e);
'@
$f = (Get-Content C:\S10eTmp\click.args -Raw).Trim() -split '\s+'
$log = @()
for ($i = 0; $i + 1 -lt $f.Count; $i += 2) {     # several "fx fy" pairs = clicks 4 s apart (Continue, then the save)
  if ($i) { Start-Sleep -Seconds 4 }
  $x = [int]([double]$f[$i] * [W.U]::GetSystemMetrics(0)); $y = [int]([double]$f[$i + 1] * [W.U]::GetSystemMetrics(1))
  [void][W.U]::SetCursorPos($x, $y)
  Start-Sleep -Milliseconds 150
  [W.U]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80; [W.U]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
  $log += "click $x,$y"
}
$log | Set-Content C:\S10eTmp\click.out
