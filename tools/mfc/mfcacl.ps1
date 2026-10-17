# mfcacl.ps1 [-Set SDDL] - G17 V6d (notes/g17-dxva-mfc.md): the Exynos9820Mfc device's security setting - the
# device registry property SPDRP_SECURITY_SDS (what an INF [DDInstall.HW] "HKR,,Security" sets; the PnP manager applies
# it to the device stack at start). -Set writes it the way MS Learn "Setting Device Object Registry Properties After
# Installation" describes (SetupDiSetDeviceRegistryProperty, administrator; effective at the next device start) - the
# same value the driver INF carries. The descriptor the device object really has: mfcsd.ps1.
# -CleanStray: delete the plain property {4d1ebee8-0803-4774-9842-b77db50265e9},26 a first attempt left on the phone's
# devnode (CM_Set_DevNode_PropertyW stores DEVPKEY_Device_SecuritySDS as data PnP never applies, 2026-10-17).
param([string]$Set = '', [switch]$CleanStray)
$dev = Get-PnpDevice -PresentOnly | Where-Object { $_.Service -eq 'Exynos9820Mfc' } | Select-Object -First 1
if (-not $dev) { 'no Exynos9820Mfc device'; return }
"device $($dev.InstanceId) status $($dev.Status)"
Add-Type -Namespace Sdi -Name Api -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct SP_DEVINFO_DATA { public uint cbSize; public Guid ClassGuid; public uint DevInst; public IntPtr Reserved; }
[StructLayout(LayoutKind.Sequential)] public struct DEVPROPKEY { public Guid fmtid; public uint pid; }
[DllImport("setupapi.dll", SetLastError = true)] public static extern IntPtr SetupDiCreateDeviceInfoList(IntPtr g, IntPtr hwnd);
[DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern bool SetupDiOpenDeviceInfoW(IntPtr set, string id, IntPtr hwnd, uint flags, ref SP_DEVINFO_DATA d);
[DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern bool SetupDiGetDeviceRegistryPropertyW(IntPtr set, ref SP_DEVINFO_DATA d, uint prop, out uint type, byte[] buf, uint size, out uint req);
[DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr set, ref SP_DEVINFO_DATA d, uint prop, byte[] buf, uint size);
[DllImport("setupapi.dll")] public static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
[DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)] public static extern int CM_Set_DevNode_PropertyW(uint dn, ref DEVPROPKEY key, uint type, byte[] buf, uint size, uint flags);

'@
$SPDRP_SECURITY_SDS = 0x18
$hdi = [Sdi.Api]::SetupDiCreateDeviceInfoList([IntPtr]::Zero, [IntPtr]::Zero)
$d = New-Object Sdi.Api+SP_DEVINFO_DATA
$d.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($d)
if (-not [Sdi.Api]::SetupDiOpenDeviceInfoW($hdi, $dev.InstanceId, [IntPtr]::Zero, 0, [ref]$d)) {
  "SetupDiOpenDeviceInfo failed $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"; return }
function Get-Sds {
  $t = 0; $req = 0; $buf = New-Object byte[] 2048
  if ([Sdi.Api]::SetupDiGetDeviceRegistryPropertyW($hdi, [ref]$d, $SPDRP_SECURITY_SDS, [ref]$t, $buf, 2048, [ref]$req)) {
    return [Text.Encoding]::Unicode.GetString($buf, 0, [Math]::Max(0, $req - 2)) }
  return "(none: error $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))"
}
"SPDRP_SECURITY_SDS = $(Get-Sds)"
if ($Set) {
  $bytes = [Text.Encoding]::Unicode.GetBytes($Set + [char]0)
  $ok = [Sdi.Api]::SetupDiSetDeviceRegistryPropertyW($hdi, [ref]$d, $SPDRP_SECURITY_SDS, $bytes, $bytes.Length)
  "SetupDiSetDeviceRegistryProperty: $ok $(if (-not $ok) { [Runtime.InteropServices.Marshal]::GetLastWin32Error() })"
  "SPDRP_SECURITY_SDS now $(Get-Sds) (effective at the next device start)"
}
if ($CleanStray) {
  $key = New-Object Sdi.Api+DEVPROPKEY
  $key.fmtid = [Guid]'4d1ebee8-0803-4774-9842-b77db50265e9'; $key.pid = 26
  "delete stray property: CR $([Sdi.Api]::CM_Set_DevNode_PropertyW($d.DevInst, [ref]$key, 0, $null, 0, 0))"   # DEVPROP_TYPE_EMPTY
}
[void][Sdi.Api]::SetupDiDestroyDeviceInfoList($hdi)
