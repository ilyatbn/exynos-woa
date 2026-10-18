# (user session, via run-as-user.ps1) dismiss.ps1 - close a leftover message box by its title (default: the
# "mpv.exe - System Error" hard-error box of a failed start), so it does not sit in later recordings: FindWindow by
# title, then WM_COMMAND IDOK and WM_CLOSE (WScript AppActivate did not reach the hard-error box). Result ->
# C:\S10eTmp\dismiss.out. notes/g27-x64-apps.md.
param([string]$Title = 'mpv.exe - System Error', [int]$Max = 5)
Add-Type -Name W -Namespace S10eDismiss -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string n);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
'@
$n = 0
while ($n -lt $Max) {
  $h = [S10eDismiss.W]::FindWindow($null, $Title)
  if ($h -eq [IntPtr]::Zero) { break }
  [S10eDismiss.W]::PostMessage($h, 0x0111, [IntPtr]1, [IntPtr]::Zero) | Out-Null   # WM_COMMAND IDOK
  Start-Sleep -Milliseconds 400
  if ([S10eDismiss.W]::FindWindow($null, $Title) -eq $h) { [S10eDismiss.W]::PostMessage($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null; Start-Sleep -Milliseconds 400 }
  $n++
}
"found+closed $n x '$Title', still there: $([S10eDismiss.W]::FindWindow($null, $Title) -ne [IntPtr]::Zero)" | Set-Content C:\S10eTmp\dismiss.out
