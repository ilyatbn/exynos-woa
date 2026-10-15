# prefer.ps1 [-Restore] - make the Exynos9820 MFC MFT the preferred Media Foundation H.264 decoder (D10 V4), or put
# the backed-up value (Microsoft H264 Video Decoder MFT) back. Run elevated on the phone. Also links the MFT into the
# video decoder category (Transforms\Categories\{d6c02d4b...}\<clsid>), which MFTRegister cannot do itself: that key
# is TrustedInstaller-owned too (MFTEnumEx did not list the MFT without it, vidc8).
#
# HKLM\SOFTWARE\Classes\MediaFoundation\Transforms\Preferred is owned by TrustedInstaller; Administrators and SYSTEM
# only have ReadKey. The script enables SeTakeOwnership + SeRestore, saves the key's security descriptor, takes
# ownership, grants Administrators write, sets the ONE value, then writes the saved descriptor back (owner
# TrustedInstaller, original ACEs). The old value is kept in HKLM\SOFTWARE\exynos-woa\MfcMft\PreferredH264.
param([switch]$Restore)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Priv {
    [StructLayout(LayoutKind.Sequential)] struct LUID { public uint Lo; public int Hi; }
    [StructLayout(LayoutKind.Sequential)] struct TP { public uint Count; public LUID Luid; public uint Attr; }
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool OpenProcessToken(IntPtr p, uint a, out IntPtr t);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool LookupPrivilegeValue(string s, string n, out LUID l);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool AdjustTokenPrivileges(IntPtr t, bool d, ref TP n, uint l, IntPtr p, IntPtr r);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    public static void Enable(string name) {
        IntPtr tok; LUID l;
        if (!OpenProcessToken(GetCurrentProcess(), 0x28, out tok)) throw new Exception("OpenProcessToken " + Marshal.GetLastWin32Error());
        if (!LookupPrivilegeValue(null, name, out l)) throw new Exception("LookupPrivilegeValue " + name);
        TP tp = new TP { Count = 1, Luid = l, Attr = 2 };
        if (!AdjustTokenPrivileges(tok, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero) || Marshal.GetLastWin32Error() != 0)
            throw new Exception("AdjustTokenPrivileges " + name + " " + Marshal.GetLastWin32Error());
    }
}
'@

$sub    = 'SOFTWARE\Classes\MediaFoundation\Transforms\Preferred'
$name   = '{34363248-0000-0010-8000-00AA00389B71}'   # MFVideoFormat_H264
$mine   = '{6A2E8C51-0B7D-4C3E-8F14-982AC0DE0D11}'   # CLSID_Exynos9820MfcH264
$backup = 'HKLM:\SOFTWARE\exynos-woa\MfcMft'

[Priv]::Enable('SeTakeOwnershipPrivilege')
[Priv]::Enable('SeRestorePrivilege')

$hklm = [Microsoft.Win32.Registry]::LocalMachine
$old  = $hklm.OpenSubKey($sub).GetValue($name)
if ($Restore) {
    $want = (Get-ItemProperty $backup -ErrorAction Stop).PreferredH264
} else {
    if (-not (Test-Path $backup)) { New-Item $backup -Force | Out-Null }
    if (-not (Get-ItemProperty $backup -ErrorAction SilentlyContinue).PreferredH264) {
        New-ItemProperty $backup -Name PreferredH264 -Value $old -PropertyType String -Force | Out-Null
    }
    $want = $mine
}

$rights = [System.Security.AccessControl.RegistryRights]
$perm   = [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree
$admins = New-Object System.Security.Principal.SecurityIdentifier('S-1-5-32-544')

# Run $Action on HKLM\$Sub with Administrators temporarily allowed $Right; the original descriptor (owner
# TrustedInstaller, original ACEs) is written back afterwards whatever happens.
function With-Right([string]$Sub, [System.Security.AccessControl.RegistryRights]$Right, [scriptblock]$Action) {
    $k     = $hklm.OpenSubKey($Sub, $perm, $rights::ReadPermissions -bor $rights::TakeOwnership)
    $sddl  = $k.GetAccessControl('Owner,Access').GetSecurityDescriptorSddlForm('Owner,Access')
    $own   = New-Object System.Security.AccessControl.RegistrySecurity
    $own.SetOwner($admins)
    $k.SetAccessControl($own)
    $k.Close()
    try {
        $k   = $hklm.OpenSubKey($Sub, $perm, $rights::ReadPermissions -bor $rights::ChangePermissions)
        $acl = $k.GetAccessControl('Access')
        $acl.AddAccessRule((New-Object System.Security.AccessControl.RegistryAccessRule($admins, $Right, 'Allow')))
        $k.SetAccessControl($acl)
        $k.Close()
        $k = $hklm.OpenSubKey($Sub, $perm, $Right -bor $rights::QueryValues -bor $rights::EnumerateSubKeys)
        & $Action $k
        $k.Close()
    } finally {
        $k    = $hklm.OpenSubKey($Sub, $perm, $rights::ReadPermissions -bor $rights::ChangePermissions -bor $rights::TakeOwnership)
        $back = New-Object System.Security.AccessControl.RegistrySecurity
        $back.SetSecurityDescriptorSddlForm($sddl, 'Owner,Access')
        $k.SetAccessControl($back)
        $k.Close()
    }
}

$cat  = 'SOFTWARE\Classes\MediaFoundation\Transforms\Categories\d6c02d4b-6833-45b4-971a-05a4b04bab91'   # video decoders
$link = '6a2e8c51-0b7d-4c3e-8f14-982ac0de0d11'
if ($Restore) {
    With-Right $cat ($rights::CreateSubKey -bor $rights::Delete) { param($k) $k.DeleteSubKey($link, $false) }
} else {
    With-Right $cat $rights::CreateSubKey { param($k) $k.CreateSubKey($link).Close() }
}
With-Right $sub $rights::SetValue { param($k) $k.SetValue($name, $want, 'String') }

$now    = $hklm.OpenSubKey($sub).GetValue($name)
$linked = $null -ne $hklm.OpenSubKey("$cat\$link")
$owner  = (Get-Acl "HKLM:\$sub").Owner
"H.264 preferred decoder: $old -> $now; category link present: $linked (key owner $owner)"
