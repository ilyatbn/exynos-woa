# mfcsd.ps1 - G17 V6d (notes/g17-dxva-mfc.md): the security descriptor the Exynos9820Mfc device object really has,
# read through its device interface path (CreateFile with READ_CONTROL only, GetSecurityInfo) - compare with the
# DEVPKEY_Device_SecuritySDS the INF / mfcacl.ps1 sets.
Add-Type -Namespace Sd -Name Api -MemberDefinition @'
[DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)] public static extern int CM_Get_Device_Interface_List_SizeW(out uint len, ref Guid g, IntPtr dev, uint flags);
[DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)] public static extern int CM_Get_Device_Interface_ListW(ref Guid g, IntPtr dev, char[] buf, uint len, uint flags);
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern IntPtr CreateFileW(string n, uint acc, uint share, IntPtr sa, uint disp, uint flags, IntPtr t);
[DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
[DllImport("advapi32.dll")] public static extern uint GetSecurityInfo(IntPtr h, int type, uint info, IntPtr o, IntPtr g, IntPtr d, IntPtr s, out IntPtr sd);
[DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern bool ConvertSecurityDescriptorToStringSecurityDescriptorW(IntPtr sd, uint rev, uint info, out IntPtr str, IntPtr len);
[DllImport("kernel32.dll")] public static extern IntPtr LocalFree(IntPtr p);
'@
$g = [Guid]'6a2e8c51-0b7d-4c3e-8f14-982ac0de0d10'
$len = 0
[void][Sd.Api]::CM_Get_Device_Interface_List_SizeW([ref]$len, [ref]$g, [IntPtr]::Zero, 0)
$buf = New-Object char[] $len
[void][Sd.Api]::CM_Get_Device_Interface_ListW([ref]$g, [IntPtr]::Zero, $buf, $len, 0)
$path = (New-Object string (, $buf)).Split([char]0)[0]
"interface $path"
$h = [Sd.Api]::CreateFileW($path, 0x00020000, 7, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)   # READ_CONTROL, OPEN_EXISTING
if ($h -eq [IntPtr]-1) { "open READ_CONTROL failed $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"; return }
$sd = [IntPtr]::Zero
$r = [Sd.Api]::GetSecurityInfo($h, 1, 0x4, [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero, [ref]$sd)   # SE_FILE_OBJECT, DACL
$str = [IntPtr]::Zero
if ($r -eq 0 -and [Sd.Api]::ConvertSecurityDescriptorToStringSecurityDescriptorW($sd, 1, 0x4, [ref]$str, [IntPtr]::Zero)) {
  "device object DACL: $([Runtime.InteropServices.Marshal]::PtrToStringUni($str))"
  [void][Sd.Api]::LocalFree($str)
} else { "GetSecurityInfo $r" }
[void][Sd.Api]::LocalFree($sd)
[void][Sd.Api]::CloseHandle($h)
