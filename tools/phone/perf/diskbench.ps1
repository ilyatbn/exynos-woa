# diskbench.ps1 [-File path] [-MB 256] - uncached (FILE_FLAG_NO_BUFFERING) read benchmark of the Windows UFS volume:
# sequential 1 MiB reads (MB/s), then 2000 random 4 KiB reads (IOPS, average and p99 latency in ms) on the same file.
# Read-only. Prints one line per test. notes/app-launch-slowness.md.
param([string]$File = "$env:WINDIR\System32\MRT.exe", [int]$MB = 256)
Add-Type -TypeDefinition @'
using System; using System.IO; using System.Diagnostics; using Microsoft.Win32.SafeHandles; using System.Runtime.InteropServices;
public static class DB {
  [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  static extern SafeFileHandle CreateFile(string n, uint a, uint s, IntPtr sa, uint c, uint f, IntPtr t);
  static FileStream Open(string p) {
    var h = CreateFile(p, 0x80000000, 7, IntPtr.Zero, 3, 0x20000000 /* NO_BUFFERING */, IntPtr.Zero);
    if (h.IsInvalid) throw new IOException("open " + Marshal.GetLastWin32Error());
    return new FileStream(h, FileAccess.Read, 4096, false);
  }
  public static string Seq(string p, long maxBytes) {
    using (var f = Open(p)) {
      long len = Math.Min(f.Length & ~0xFFFFFL, maxBytes); var buf = new byte[1 << 20]; var sw = Stopwatch.StartNew(); long tot = 0;
      while (tot < len) { int n = f.Read(buf, 0, buf.Length); if (n <= 0) break; tot += n; }
      double s = sw.Elapsed.TotalSeconds; return String.Format("seq 1MiB: {0:N0} MiB in {1:N2} s = {2:N1} MiB/s", tot / 1048576.0, s, tot / 1048576.0 / s);
    }
  }
  public static string Sweep(string p, int bs, long maxBytes) {
    using (var f = Open(p)) {
      long len = Math.Min(f.Length / bs * bs, maxBytes); var buf = new byte[bs]; var sw = Stopwatch.StartNew(); long tot = 0; int n = 0;
      while (tot < len) { int r = f.Read(buf, 0, bs); if (r <= 0) break; tot += r; n++; }
      double s = sw.Elapsed.TotalSeconds; return String.Format("seq {0,7} B: {1,5:N1} MiB/s, {2,8:N1} us per request ({3} requests)", bs, tot / 1048576.0 / s, s * 1e6 / n, n);
    }
  }
  public static string Rand(string p, int count) {
    using (var f = Open(p)) {
      long blocks = f.Length / 4096; var buf = new byte[4096]; var r = new Random(1); var lat = new double[count]; var all = Stopwatch.StartNew();
      for (int i = 0; i < count; i++) { f.Position = (long)(r.NextDouble() * blocks) * 4096; var sw = Stopwatch.StartNew(); f.Read(buf, 0, 4096); lat[i] = sw.Elapsed.TotalMilliseconds; }
      double s = all.Elapsed.TotalSeconds; Array.Sort(lat); double sum = 0; foreach (var x in lat) sum += x;
      return String.Format("rand 4KiB QD1: {0} reads in {1:N2} s = {2:N0} IOPS, avg {3:N3} ms, p50 {4:N3} ms, p99 {5:N3} ms, max {6:N3} ms",
        count, s, count / s, sum / count, lat[count / 2], lat[count * 99 / 100], lat[count - 1]);
    }
  }
}
'@
"file $File ($([math]::Round((Get-Item $File).Length / 1MB)) MiB)"
[DB]::Seq($File, [long]$MB * 1MB)
[DB]::Rand($File, 2000)
foreach ($bs in 4096, 16384, 65536, 262144, 1048576) { [DB]::Sweep($File, $bs, [long]16 * 1MB) }
