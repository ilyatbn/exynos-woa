# writetest.ps1 -Mode write|verify [-Dir C:\S10eTmp\wt] [-MB 16] - write-path integrity check for UFS driver changes
# (notes/ufs-async-plan.md). write: 3 files (block size 4 KiB / 64 KiB / 1 MiB), each -MB MiB of a deterministic
# pattern (every 4 KiB block carries its file id, block number and a xorshift stream), written with
# FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, then flushed. verify: reads every file back UNCACHED and compares
# each block with the regenerated pattern. Run verify again after a reboot (and from TWRP: sha256 of the files).
param([ValidateSet("write", "verify")][string]$Mode = "verify", [string]$Dir = "C:\S10eTmp\wt", [int]$MB = 16)
Add-Type -TypeDefinition @'
using System; using System.IO; using Microsoft.Win32.SafeHandles; using System.Runtime.InteropServices;
public static class WT {
  [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  static extern SafeFileHandle CreateFile(string n, uint a, uint s, IntPtr sa, uint c, uint f, IntPtr t);
  static void Fill(byte[] b, int off, int fileId, long block) {
    ulong x = 0x9E3779B97F4A7C15UL ^ ((ulong)fileId << 40) ^ (ulong)block;
    for (int i = 0; i < 4096; i += 8) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; BitConverter.GetBytes(x).CopyTo(b, off + i); }
    BitConverter.GetBytes((long)fileId).CopyTo(b, off); BitConverter.GetBytes(block).CopyTo(b, off + 8);
  }
  public static string Write(string p, int fileId, int bs, long bytes) {
    var h = CreateFile(p, 0x40000000, 0, IntPtr.Zero, 2, 0x20000000 | 0x80000000u, IntPtr.Zero);
    if (h.IsInvalid) return "open-error-" + Marshal.GetLastWin32Error();
    using (var f = new FileStream(h, FileAccess.Write, 4096, false)) {
      var buf = new byte[bs]; long block = 0;
      for (long done = 0; done < bytes; done += bs) { for (int o = 0; o < bs; o += 4096) Fill(buf, o, fileId, block++); f.Write(buf, 0, bs); }
      f.Flush(true);
    }
    return "written";
  }
  public static string Verify(string p, int fileId, long bytes) {
    var h = CreateFile(p, 0x80000000, 1, IntPtr.Zero, 3, 0x20000000, IntPtr.Zero);
    if (h.IsInvalid) return "open-error-" + Marshal.GetLastWin32Error();
    using (var f = new FileStream(h, FileAccess.Read, 4096, false)) {
      var buf = new byte[1 << 20]; var exp = new byte[4096]; long block = 0, bad = 0, firstBad = -1;
      for (long done = 0; done < bytes; done += buf.Length) {
        int n = f.Read(buf, 0, buf.Length); if (n <= 0) return "SHORT at " + done;
        for (int o = 0; o < n; o += 4096) {
          Fill(exp, 0, fileId, block);
          for (int i = 0; i < 4096; i++) if (buf[o + i] != exp[i]) { bad++; if (firstBad < 0) firstBad = block; break; }
          block++;
        }
      }
      return bad == 0 ? "OK (" + block + " blocks)" : "BAD " + bad + " blocks, first " + firstBad;
    }
  }
}
'@
New-Item -ItemType Directory -Force $Dir | Out-Null
$sizes = @(4096, 65536, 1048576)
for ($i = 0; $i -lt 3; $i++) {
  $p = Join-Path $Dir ("pattern-{0}.bin" -f $sizes[$i])
  if ($Mode -eq "write") { "{0} {1}" -f (Split-Path $p -Leaf), [WT]::Write($p, $i + 1, $sizes[$i], [long]$MB * 1MB) }
  "{0} verify {1}" -f (Split-Path $p -Leaf), [WT]::Verify($p, $i + 1, [long]$MB * 1MB)
}
