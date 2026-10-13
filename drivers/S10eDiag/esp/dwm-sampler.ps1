# A poor man's profiler for DWM (notes/gpu-hw-path.md H4): after -Delay seconds, -Count stack-only minidumps of dwm.exe
# -Interval ms apart (dbghelp!MiniDumpWriteDump, as livedump.ps1, the type compiled once so the interval holds). The
# PC symbolises them (cdb + Microsoft symbols + the UMD's libgallium_d3d10.pdb): the busy thread's frames over the
# samples show where DWM's time goes. Run as SYSTEM (DWM belongs to Window Manager\DWM-1), in the background.
# usage: dwm-sampler.ps1 -Out <dir> -Delay 80 -Count 8 -Interval 1000
param([string]$Out, [int]$Delay = 80, [int]$Count = 8, [int]$Interval = 1000)
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
public static class DwmSample {
    [DllImport("dbghelp.dll", SetLastError = true)]
    static extern bool MiniDumpWriteDump(IntPtr hProcess, uint ProcessId, IntPtr hFile, uint DumpType,
                                         IntPtr ExceptionParam, IntPtr UserStreamParam, IntPtr CallbackParam);
    public static string Write(System.Diagnostics.Process p, string path) {
        // MiniDumpWithHandleData 0x4 | WithUnloadedModules 0x20 | WithProcessThreadData 0x100 | WithThreadInfo 0x1000
        using (FileStream f = new FileStream(path, FileMode.Create, FileAccess.ReadWrite, FileShare.None)) {
            bool ok = MiniDumpWriteDump(p.Handle, (uint)p.Id, f.SafeFileHandle.DangerousGetHandle(), 0x1124,
                                        IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
            return ok ? "ok" : ("error " + Marshal.GetLastWin32Error());
        }
    }
}
'@
New-Item -ItemType Directory -Force $Out | Out-Null
Start-Sleep -Seconds $Delay
for ($i = 1; $i -le $Count; $i++) {
    $t0 = Get-Date
    foreach ($p in @(Get-Process -Name dwm -ErrorAction SilentlyContinue)) {
        $path = Join-Path $Out ("dwm-{0}-s{1:D2}.dmp" -f $p.Id, $i)
        try { $r = [DwmSample]::Write($p, $path) } catch { $r = "exception " + $_.Exception.Message }
        "{0:HH:mm:ss.fff} sample {1} dwm pid {2} cpu {3:N2} s: {4}" -f $t0, $i, $p.Id, $p.CPU, $r
    }
    $left = $Interval - [int]((Get-Date) - $t0).TotalMilliseconds
    if ($left -gt 0) { Start-Sleep -Milliseconds $left }
}
