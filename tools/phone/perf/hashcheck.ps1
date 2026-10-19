# hashcheck.ps1 [-Out file] - data-integrity check for UFS driver changes (notes/ufs-async-plan.md stage 0): SHA-256 of
# a fixed file set read UNCACHED (FILE_FLAG_NO_BUFFERING) with 4 KiB, 64 KiB and 1 MiB requests; every request size
# must give the same hash, and the hashes must match the baseline taken on the proven driver (w38). Read-only.
param([string]$Out = "")
Add-Type -TypeDefinition @'
using System; using System.IO; using System.Security.Cryptography; using Microsoft.Win32.SafeHandles; using System.Runtime.InteropServices;
public static class HC {
  [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  static extern SafeFileHandle CreateFile(string n, uint a, uint s, IntPtr sa, uint c, uint f, IntPtr t);
  public static string Hash(string p, int bs) {
    var h = CreateFile(p, 0x80000000, 7, IntPtr.Zero, 3, 0x20000000, IntPtr.Zero);
    if (h.IsInvalid) return "open-error-" + Marshal.GetLastWin32Error();
    using (var f = new FileStream(h, FileAccess.Read, 4096, false)) using (var sha = SHA256.Create()) {
      long len = f.Length; var buf = new byte[bs]; long tot = 0;
      while (tot < len) { int n = f.Read(buf, 0, bs); if (n <= 0) break; int use = (int)Math.Min(n, len - tot); sha.TransformBlock(buf, 0, use, null, 0); tot += use; }
      sha.TransformFinalBlock(buf, 0, 0); return BitConverter.ToString(sha.Hash).Replace("-", "").Substring(0, 16);
    }
  }
}
'@
$files = "$env:WINDIR\System32\edgehtml.dll", "$env:WINDIR\System32\mshtml.dll", "$env:WINDIR\System32\Windows.UI.Xaml.dll",
         "$env:WINDIR\System32\ntoskrnl.exe", "$env:WINDIR\System32\shell32.dll"
$files += Get-ChildItem "$env:ProgramFiles\BraveSoftware\Brave-Browser\Application" -Recurse -Filter chrome.dll -ErrorAction SilentlyContinue |
          Select-Object -First 1 -ExpandProperty FullName
$r = foreach ($f in $files) {
  if (-not (Test-Path $f)) { continue }
  $hs = foreach ($bs in 4096, 65536, 1048576) { [HC]::Hash($f, $bs) }
  $same = ($hs | Select-Object -Unique).Count -eq 1
  "{0} {1} {2}" -f $hs[0], $(if ($same) { "sizes-agree" } else { "SIZES-DIFFER " + ($hs -join "/") }), (Split-Path $f -Leaf)
}
if ($Out) { $r | Set-Content $Out } else { $r }
