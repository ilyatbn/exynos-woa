# adapterprop.ps1 [-Drive \\.\PhysicalDrive0] - IOCTL_STORAGE_QUERY_PROPERTY(StorageAdapterProperty): the limits
# StorPort publishes for the UFS miniport (MaximumTransferLength, MaximumPhysicalPages, AlignmentMask).
# notes/ufs-async-plan.md (w42 large transfers).
param([string]$Drive = "\\.\PhysicalDrive0")
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using Microsoft.Win32.SafeHandles;
public static class AP {
  [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  static extern SafeFileHandle CreateFile(string n, uint a, uint s, IntPtr sa, uint c, uint f, IntPtr t);
  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] inb, int inl, byte[] outb, int outl, out int ret, IntPtr ov);
  public static string Query(string d) {
    var h = CreateFile(d, 0, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
    if (h.IsInvalid) return "open-error-" + Marshal.GetLastWin32Error();
    var q = new byte[12]; BitConverter.GetBytes(1).CopyTo(q, 0);   // PropertyId = StorageAdapterProperty, QueryType = Standard
    var o = new byte[64]; int ret;
    if (!DeviceIoControl(h, 0x2D1400, q, q.Length, o, o.Length, out ret, IntPtr.Zero)) return "ioctl-error-" + Marshal.GetLastWin32Error();
    // STORAGE_ADAPTER_DESCRIPTOR: Version, Size, MaximumTransferLength, MaximumPhysicalPages, AlignmentMask, ...
    return String.Format("MaximumTransferLength 0x{0:X} MaximumPhysicalPages {1} AlignmentMask 0x{2:X}",
      BitConverter.ToUInt32(o, 8), BitConverter.ToUInt32(o, 12), BitConverter.ToUInt32(o, 16));
  }
}
'@
[AP]::Query($Drive)
