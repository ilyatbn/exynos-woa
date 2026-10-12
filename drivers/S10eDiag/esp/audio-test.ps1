# D8 Windows audio test (runs in the logged-on user's session via run-as-user.ps1 from extra-audio.cmd).
# 1) WASAPI diagnostics: default render endpoint, its state, mix format, IAudioClient.Initialize HRESULT.
# 2) Plays 2 s tones through a WASAPI shared-mode stream on the default endpoint and changes the volume with injected
#    volume keys in between, so the PC microphone (tools/audio-listen) sees playback and the level changes.
# Log: C:\S10eTmp\audiotest.txt
$log = 'C:\S10eTmp\audiotest.txt'
function L($m) { "{0:HH:mm:ss.fff} {1}" -f (Get-Date), $m | Out-File -Append -Encoding ascii $log }
Add-Type -Namespace W -Name K -MemberDefinition '[DllImport("user32.dll")] public static extern void keybd_event(byte v, byte s, uint f, System.UIntPtr e);'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Threading;
namespace S10e {
  [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class MMDeviceEnumeratorCo {}
  [Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IMMDeviceEnumerator {
    [PreserveSig] int EnumAudioEndpoints(int flow, int mask, out IntPtr devs);
    [PreserveSig] int GetDefaultAudioEndpoint(int flow, int role, out IMMDevice dev);
  }
  [Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IMMDevice {
    [PreserveSig] int Activate(ref Guid iid, int ctx, IntPtr p, [MarshalAs(UnmanagedType.IUnknown)] out object o);
    [PreserveSig] int OpenPropertyStore(int access, out IntPtr store);
    [PreserveSig] int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
    [PreserveSig] int GetState(out int state);
  }
  [Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IAudioClient {
    [PreserveSig] int Initialize(int share, int flags, long dur, long period, IntPtr fmt, IntPtr session);
    [PreserveSig] int GetBufferSize(out uint frames);
    [PreserveSig] int GetStreamLatency(out long lat);
    [PreserveSig] int GetCurrentPadding(out uint pad);
    [PreserveSig] int IsFormatSupported(int share, IntPtr fmt, out IntPtr closest);
    [PreserveSig] int GetMixFormat(out IntPtr fmt);
    [PreserveSig] int GetDevicePeriod(out long def, out long min);
    [PreserveSig] int Start();
    [PreserveSig] int Stop();
    [PreserveSig] int Reset();
    [PreserveSig] int SetEventHandle(IntPtr h);
    [PreserveSig] int GetService(ref Guid iid, [MarshalAs(UnmanagedType.IUnknown)] out object o);
  }
  [Guid("F294ACFC-3146-4483-A7BF-ADDCA7C260E2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IAudioRenderClient {
    [PreserveSig] int GetBuffer(uint frames, out IntPtr data);
    [PreserveSig] int ReleaseBuffer(uint frames, int flags);
  }
  public static class Wasapi {
    static IMMDevice Dev(out string info) {
      var e = (IMMDeviceEnumerator)new MMDeviceEnumeratorCo();
      IMMDevice d; int hr = e.GetDefaultAudioEndpoint(0, 0, out d);
      if (hr != 0) { info = "GetDefaultAudioEndpoint hr 0x" + hr.ToString("X8"); return null; }
      string id; d.GetId(out id); int st; d.GetState(out st);
      info = "default render endpoint " + id + " state " + st; return d;
    }
    public static string Diag() {
      string info; var d = Dev(out info); if (d == null) return info;
      Guid g = typeof(IAudioClient).GUID; object o; int hr = d.Activate(ref g, 23, IntPtr.Zero, out o);
      if (hr != 0) return info + "; Activate hr 0x" + hr.ToString("X8");
      var c = (IAudioClient)o; IntPtr f; hr = c.GetMixFormat(out f);
      if (hr != 0) return info + "; GetMixFormat hr 0x" + hr.ToString("X8");
      short tag = Marshal.ReadInt16(f, 0), ch = Marshal.ReadInt16(f, 2), bits = Marshal.ReadInt16(f, 14); int rate = Marshal.ReadInt32(f, 4);
      hr = c.Initialize(0, 0, 2000000, 0, f, IntPtr.Zero);
      return info + "; mix format tag 0x" + tag.ToString("X") + " " + ch + " ch " + rate + " Hz " + bits + " bit; Initialize hr 0x" + hr.ToString("X8");
    }
    // Shared-mode tone in the mix format (float or 16-bit), amplitude 0.25.
    public static string Tone(double hz, int ms) {
      string info; var d = Dev(out info); if (d == null) return info;
      Guid g = typeof(IAudioClient).GUID; object o; int hr = d.Activate(ref g, 23, IntPtr.Zero, out o); if (hr != 0) return "Activate 0x" + hr.ToString("X8");
      var c = (IAudioClient)o; IntPtr f; int mhr = c.GetMixFormat(out f);
      int iflags = 0;
      if (mhr != 0) {   // run audB4: no mix format - ask for 48 kHz 16-bit stereo PCM with engine conversion
        f = Marshal.AllocCoTaskMem(18); Marshal.WriteInt16(f, 0, 1); Marshal.WriteInt16(f, 2, 2);
        Marshal.WriteInt32(f, 4, 48000); Marshal.WriteInt32(f, 8, 192000); Marshal.WriteInt16(f, 12, 4); Marshal.WriteInt16(f, 14, 16); Marshal.WriteInt16(f, 16, 0);
        iflags = unchecked((int)0x88000000);   // AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | SRC_DEFAULT_QUALITY
      }
      short tag = Marshal.ReadInt16(f, 0), ch = Marshal.ReadInt16(f, 2), bits = Marshal.ReadInt16(f, 14); int rate = Marshal.ReadInt32(f, 4);
      bool isFloat = tag == 3 || (tag == unchecked((short)0xFFFE) && bits == 32);
      hr = c.Initialize(0, iflags, 2000000, 0, f, IntPtr.Zero); if (hr != 0) return "Initialize 0x" + hr.ToString("X8") + " (mix format hr 0x" + mhr.ToString("X8") + ")";
      uint size; c.GetBufferSize(out size);
      Guid rg = typeof(IAudioRenderClient).GUID; object ro; hr = c.GetService(ref rg, out ro); if (hr != 0) return "GetService 0x" + hr.ToString("X8");
      var r = (IAudioRenderClient)ro; long total = (long)rate * ms / 1000, done = 0; double ph = 0, step = 2 * Math.PI * hz / rate;
      hr = c.Start(); if (hr != 0) return "Start 0x" + hr.ToString("X8");
      var sw = System.Diagnostics.Stopwatch.StartNew(); long lastDone = -1, lastChange = 0; uint lastPad = 0;
      while (done < total) {
        uint pad; c.GetCurrentPadding(out pad); lastPad = pad; uint n = size - pad; if (n > total - done) n = (uint)(total - done);
        if (done != lastDone) { lastDone = done; lastChange = sw.ElapsedMilliseconds; }
        else if (sw.ElapsedMilliseconds - lastChange > 3000) { c.Stop(); return "STALLED: " + done + "/" + total + " frames written, padding " + pad + " of " + size + " (the stream position does not advance)"; }
        if (n > 0) {
          IntPtr p; r.GetBuffer(n, out p);
          for (int i = 0; i < n; i++) { double v = 0.25 * Math.Sin(ph); ph += step;
            for (int k = 0; k < ch; k++) {
              if (isFloat) { byte[] b = BitConverter.GetBytes((float)v); Marshal.Copy(b, 0, p + (i * ch + k) * 4, 4); }
              else Marshal.WriteInt16(p, (i * ch + k) * 2, (short)(v * 32767)); } }
          r.ReleaseBuffer(n, 0); done += n;
        }
        Thread.Sleep(10);
      }
      Thread.Sleep(300); c.Stop();
      return "played " + done + " frames at " + rate + " Hz (" + (isFloat ? "float" : "int16") + ", " + ch + " ch)";
    }
  }
}
'@
function Key($vk, $n) { for ($i = 0; $i -lt $n; $i++) { [W.K]::keybd_event($vk, 0, 0, [UIntPtr]::Zero); [W.K]::keybd_event($vk, 0, 2, [UIntPtr]::Zero); Start-Sleep -Milliseconds 40 } }
try {
  L "start user=$env:USERNAME"
  L ("diag: " + [S10e.Wasapi]::Diag())
  Key 0xAE 50; Key 0xAF 25; L 'volume keys: 0 then +25 steps (~50%)'
  Start-Sleep 1; L 'play 1500 Hz A (50%)'; L ([S10e.Wasapi]::Tone(1500, 2000))
  Start-Sleep 1; Key 0xAE 10; L 'volume down x10 (~30%)'
  Start-Sleep 1; L 'play 1500 Hz B (30%)'; L ([S10e.Wasapi]::Tone(1500, 2000))
  Start-Sleep 1; Key 0xAF 20; L 'volume up x20 (~70%)'
  Start-Sleep 1; L 'play 2000 Hz C (70%)'; L ([S10e.Wasapi]::Tone(2000, 2000))
  Start-Sleep 1; Key 0xAD 1; L 'mute key'
  Start-Sleep 1; L 'play 2000 Hz D (muted)'; L ([S10e.Wasapi]::Tone(2000, 2000))
  Key 0xAD 1; Key 0xAE 20; L 'unmuted, volume back to ~30%'
} catch { L "ERROR $_" }
L 'end'
