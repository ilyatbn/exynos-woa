# winkey.ps1 [-Key ''|A|N|...] - (user session, via runuser2) press the Windows key, alone (Start menu) or with a
# letter (A = Quick Settings, N = notification center). For checking shell flyouts without the user.
param([string]$Key = '')
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class S10eKeys { [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, UIntPtr e); }
'@
$LWIN = 0x5B; $UP = 2
[S10eKeys]::keybd_event($LWIN, 0, 0, [UIntPtr]::Zero)
if ($Key) { $vk = [byte][char]$Key.ToUpper(); [S10eKeys]::keybd_event($vk, 0, 0, [UIntPtr]::Zero); [S10eKeys]::keybd_event($vk, 0, $UP, [UIntPtr]::Zero) }
[S10eKeys]::keybd_event($LWIN, 0, $UP, [UIntPtr]::Zero)
"pressed Win$(if ($Key) { "+$Key" })" | Out-File C:\S10eTmp\winkey.txt
