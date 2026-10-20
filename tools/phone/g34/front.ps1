# front.ps1 [-Name DaveTheDiver] - (user session, via runuser2.ps1) bring a process's main window to the front
# (Steam's library window had covered Dave after a launch; notes/g34-fl11.md). Writes C:\S10eTmp\front.txt.
param([string]$Name = 'DaveTheDiver')
Add-Type -Name W -Namespace S10eFront -MemberDefinition @'
[DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr h, bool alt);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
'@
$p = Get-Process $Name -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p -or $p.MainWindowHandle -eq 0) { "no window for $Name" | Set-Content C:\S10eTmp\front.txt; exit 1 }
[S10eFront.W]::ShowWindow($p.MainWindowHandle, 9) | Out-Null      # SW_RESTORE
[S10eFront.W]::SwitchToThisWindow($p.MainWindowHandle, $true)
"front $Name pid $($p.Id) '$($p.MainWindowTitle)'" | Set-Content C:\S10eTmp\front.txt
