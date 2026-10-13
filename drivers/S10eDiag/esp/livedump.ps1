# Writes a live minidump (threads + stacks, handles, unloaded modules - no full memory) of every running process with
# one of the given names, via dbghelp!MiniDumpWriteDump. Called as SYSTEM from extra-*.cmd.
# usage: livedump.ps1 -Out <dir> -Names LogonUI,dwm,winlogon
# g45 (notes/gpu-full-adapter-hang.md): in full mode LogonUI stays up and the autologon never starts - the stacks of
# LogonUI / dwm / winlogon show what they wait on (cdb + MS symbols on the PC).
param([string]$Out, [string[]]$Names)
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
public static class LiveDump {
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
foreach ($n in $Names) {
    foreach ($p in @(Get-Process -Name $n -ErrorAction SilentlyContinue)) {
        $path = Join-Path $Out ("{0}-{1}.dmp" -f $n, $p.Id)
        try { $r = [LiveDump]::Write($p, $path) } catch { $r = "exception " + $_.Exception.Message }
        "{0} pid {1} session {2}: {3} ({4} bytes)" -f $n, $p.Id, $p.SessionId, $r, (Get-Item $path -ErrorAction SilentlyContinue).Length
    }
}
