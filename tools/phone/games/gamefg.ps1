# (user session, via run-as-user.ps1) gamefg.ps1 - gamelaunch.ps1 with foreground rights: a process started from the
# scheduled task never owns the foreground, and a fullscreen swapchain then drops out at once (NFSU2 crashes on it).
# A synthetic Alt press counts as this process's input, which lets it pass the foreground on with
# AllowSetForegroundWindow(ASFW_ANY) (the documented SetForegroundWindow rules). notes/g24-compute-cs4x.md.
Add-Type -Namespace W -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra);
[DllImport("user32.dll")] public static extern bool AllowSetForegroundWindow(int pid);
'@
[W.U]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)
[W.U]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
[void][W.U]::AllowSetForegroundWindow(-1)
& C:\S10eTmp\gamelaunch.ps1
