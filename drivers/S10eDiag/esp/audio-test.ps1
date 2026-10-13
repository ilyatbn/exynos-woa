# D8 Windows audio test (runs in the logged-on user's session via run-as-user.ps1 from extra-audio.cmd).
# 1) WASAPI diagnostics: default render endpoint, its state, mix format, IAudioClient.Initialize HRESULT.
# 2) Plays 2 s tones through a WASAPI shared-mode stream on the default endpoint and changes the volume with injected
#    volume keys in between, so the PC microphone (tools/audio-listen) sees playback and the level changes.
# 3) A5: after every key step the endpoint volume (IAudioEndpointVolume: scalar, dB, mute, QueryHardwareSupport) is
#    logged - with the driver's volume + mute elements, hw support must be 3 and the scalar must follow the keys.
# 4) A8: QUIET (user request): tone amplitude 0.03 (was 0.25; A12: 0.1 so the PC mic can hear it), volume ~10 % / ~6 % / ~16 % (was 50/30/70; the A > B < C
#    pattern stays for the mic). Diagnostics for AUDCLNT_E_UNSUPPORTED_FORMAT: the endpoint property store (every
#    property; blobs = formats in hex), IsFormatSupported (shared) for three 48 kHz formats, and one EXCLUSIVE-mode
#    48 kHz 16-bit tone (bypasses the engine's device format). notes/d8-audio-win-a5.md "A8".
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
  [Guid("5CDF2C82-841E-4546-9722-0CF74078229A"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IAudioEndpointVolume {
    [PreserveSig] int RegisterControlChangeNotify(IntPtr n);
    [PreserveSig] int UnregisterControlChangeNotify(IntPtr n);
    [PreserveSig] int GetChannelCount(out uint n);
    [PreserveSig] int SetMasterVolumeLevel(float db, IntPtr ctx);
    [PreserveSig] int SetMasterVolumeLevelScalar(float v, IntPtr ctx);
    [PreserveSig] int GetMasterVolumeLevel(out float db);
    [PreserveSig] int GetMasterVolumeLevelScalar(out float v);
    [PreserveSig] int SetChannelVolumeLevel(uint ch, float db, IntPtr ctx);
    [PreserveSig] int SetChannelVolumeLevelScalar(uint ch, float v, IntPtr ctx);
    [PreserveSig] int GetChannelVolumeLevel(uint ch, out float db);
    [PreserveSig] int GetChannelVolumeLevelScalar(uint ch, out float v);
    [PreserveSig] int SetMute(int mute, IntPtr ctx);
    [PreserveSig] int GetMute(out int mute);
    [PreserveSig] int GetVolumeStepInfo(out uint step, out uint count);
    [PreserveSig] int VolumeStepUp(IntPtr ctx);
    [PreserveSig] int VolumeStepDown(IntPtr ctx);
    [PreserveSig] int QueryHardwareSupport(out uint mask);
    [PreserveSig] int GetVolumeRange(out float min, out float max, out float inc);
  }
  [StructLayout(LayoutKind.Sequential)] public struct PKEY { public Guid fmtid; public uint pid; }
  [StructLayout(LayoutKind.Explicit, Size = 24)] public struct PV {
    [FieldOffset(0)] public ushort vt; [FieldOffset(8)] public uint u4; [FieldOffset(8)] public IntPtr ptr;
    [FieldOffset(8)] public uint cb; [FieldOffset(16)] public IntPtr data;
  }
  [Guid("886d8eeb-8cf2-4446-8d02-cdba1dbdcf99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IPropertyStore {
    [PreserveSig] int GetCount(out uint n);
    [PreserveSig] int GetAt(uint i, out PKEY key);
    [PreserveSig] int GetValue(ref PKEY key, out PV v);
  }
  [Guid("F294ACFC-3146-4483-A7BF-ADDCA7C260E2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IAudioRenderClient {
    [PreserveSig] int GetBuffer(uint frames, out IntPtr data);
    [PreserveSig] int ReleaseBuffer(uint frames, int flags);
  }
  public static class Wasapi {
    [DllImport("ole32.dll")] static extern int PropVariantClear(ref PV v);
    public const double Amp = 0.1;    // A12: 0.1 (-20 dBFS; amps at -10 dB, ~10 % volume) - quiet but mic-detectable (A8 0.03 was not)
    // A8: every endpoint property (VT_LPWSTR text, VT_UI4 number, VT_BLOB hex = formats). One line per property.
    public static string Props() {
      string info; var d = Dev(out info); if (d == null) return info;
      IntPtr ps; int hr = d.OpenPropertyStore(0, out ps); if (hr != 0) return "OpenPropertyStore hr 0x" + hr.ToString("X8");
      var st = (IPropertyStore)Marshal.GetObjectForIUnknown(ps); Marshal.Release(ps);
      uint n; st.GetCount(out n); var sb = new System.Text.StringBuilder(info + "; " + n + " properties");
      for (uint i = 0; i < n; i++) {
        PKEY k; if (st.GetAt(i, out k) != 0) continue; PV v; if (st.GetValue(ref k, out v) != 0) continue;
        string val;
        if (v.vt == 31) val = Marshal.PtrToStringUni(v.ptr);
        else if (v.vt == 19 || v.vt == 3) val = v.u4.ToString();
        else if (v.vt == 11) val = (v.u4 & 0xFFFF) != 0 ? "true" : "false";
        else if (v.vt == 65) { int c = (int)Math.Min(v.cb, 128u); var b = new byte[c]; if (c > 0) Marshal.Copy(v.data, b, 0, c);
          val = "blob " + v.cb + ": " + BitConverter.ToString(b).Replace("-", ""); }
        else val = "vt " + v.vt;
        PropVariantClear(ref v);
        string f8 = k.fmtid.ToString().Substring(0, 8);   // engine/endpoint keys and every blob (formats) only
        if (v.vt != 65 && f8 != "f19f064d" && f8 != "e4870e26" && f8 != "1da5d803" && f8 != "3d6e1656") continue;
        sb.Append("\r\n  {" + k.fmtid + "}," + k.pid + " = " + val);
      }
      return sb.ToString();
    }
    static IntPtr Wfx(int tag, int bits, bool ext) {   // 48 kHz stereo; ext = WAVEFORMATEXTENSIBLE (tag 0xFFFE, sub = tag)
      IntPtr f = Marshal.AllocCoTaskMem(40); int align = 2 * bits / 8;
      Marshal.WriteInt16(f, 0, (short)(ext ? unchecked((short)0xFFFE) : (short)tag)); Marshal.WriteInt16(f, 2, 2);
      Marshal.WriteInt32(f, 4, 48000); Marshal.WriteInt32(f, 8, 48000 * align); Marshal.WriteInt16(f, 12, (short)align);
      Marshal.WriteInt16(f, 14, (short)bits); Marshal.WriteInt16(f, 16, (short)(ext ? 22 : 0));
      if (ext) { Marshal.WriteInt16(f, 18, (short)bits); Marshal.WriteInt32(f, 20, 3);
        byte[] g = new Guid(tag, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71).ToByteArray(); Marshal.Copy(g, 0, f + 24, 16); }
      return f;
    }
    // A8: IsFormatSupported (shared) for 48k/16 PCM, 48k/16 EXT, 48k float EXT.
    public static string Formats() {
      string info; var d = Dev(out info); if (d == null) return info;
      Guid g = typeof(IAudioClient).GUID; object o; int hr = d.Activate(ref g, 23, IntPtr.Zero, out o); if (hr != 0) return "Activate 0x" + hr.ToString("X8");
      var c = (IAudioClient)o; var sb = new System.Text.StringBuilder();
      foreach (var t in new[] { Tuple.Create(1, 16, false, "PCM16"), Tuple.Create(1, 16, true, "EXT16"), Tuple.Create(3, 32, true, "EXTfloat32") }) {
        IntPtr f = Wfx(t.Item1, t.Item2, t.Item3), cl; int r = c.IsFormatSupported(0, f, out cl);
        sb.Append(t.Item4 + " hr 0x" + r.ToString("X8") + (cl != IntPtr.Zero ? " (closest offered)" : "") + "; ");
        if (cl != IntPtr.Zero) Marshal.FreeCoTaskMem(cl); Marshal.FreeCoTaskMem(f);
      }
      long def, min; hr = c.GetDevicePeriod(out def, out min); sb.Append("device period hr 0x" + hr.ToString("X8") + " " + def + "/" + min);
      return sb.ToString();
    }
    // A8: EXCLUSIVE-mode 48 kHz 16-bit EXT tone (no engine, no device format), timer-driven, amplitude Amp.
    public static string Exclusive(double hz, int ms) {
      string info; var d = Dev(out info); if (d == null) return info;
      Guid g = typeof(IAudioClient).GUID; object o; int hr = d.Activate(ref g, 23, IntPtr.Zero, out o); if (hr != 0) return "Activate 0x" + hr.ToString("X8");
      var c = (IAudioClient)o; IntPtr f = Wfx(1, 16, true); IntPtr cl;
      int sup = c.IsFormatSupported(1, f, out cl);
      long def, min; c.GetDevicePeriod(out def, out min); if (def <= 0) def = 100000;
      hr = c.Initialize(1, 0, def * 4, def, f, IntPtr.Zero);
      if (hr != 0) { Marshal.FreeCoTaskMem(f); return "exclusive IsFormatSupported 0x" + sup.ToString("X8") + ", Initialize 0x" + hr.ToString("X8") + " (period " + def + ")"; }
      uint size; c.GetBufferSize(out size);
      Guid rg = typeof(IAudioRenderClient).GUID; object ro; hr = c.GetService(ref rg, out ro); if (hr != 0) return "exclusive GetService 0x" + hr.ToString("X8");
      var r = (IAudioRenderClient)ro; long total = 48000L * ms / 1000, done = 0; double ph = 0, step = 2 * Math.PI * hz / 48000;
      hr = c.Start(); if (hr != 0) return "exclusive Start 0x" + hr.ToString("X8");
      var sw = System.Diagnostics.Stopwatch.StartNew(); long lastDone = -1, lastChange = 0;
      while (done < total) {
        uint pad; c.GetCurrentPadding(out pad); uint n = size - pad; if (n > total - done) n = (uint)(total - done);
        if (done != lastDone) { lastDone = done; lastChange = sw.ElapsedMilliseconds; }
        else if (sw.ElapsedMilliseconds - lastChange > 3000) { c.Stop(); return "exclusive STALLED: " + done + "/" + total + " frames, padding " + pad + " of " + size; }
        if (n > 0) { IntPtr p; r.GetBuffer(n, out p);
          for (int i = 0; i < n; i++) { short v = (short)(Amp * 32767 * Math.Sin(ph)); ph += step; Marshal.WriteInt16(p, i * 4, v); Marshal.WriteInt16(p, i * 4 + 2, v); }
          r.ReleaseBuffer(n, 0); done += n; }
        Thread.Sleep(5);
      }
      Thread.Sleep(200); c.Stop(); Marshal.FreeCoTaskMem(f);
      return "exclusive played " + done + " frames (48 kHz 16-bit, buffer " + size + ", period " + def + ")";
    }
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
    // A5: endpoint volume state (what the volume keys / flyout change). hw 3 = volume + mute on the driver's elements.
    public static string Vol() {
      string info; var d = Dev(out info); if (d == null) return info;
      Guid g = typeof(IAudioEndpointVolume).GUID; object o; int hr = d.Activate(ref g, 23, IntPtr.Zero, out o);
      if (hr != 0) return "endpoint volume Activate hr 0x" + hr.ToString("X8");
      var v = (IAudioEndpointVolume)o; float sc, db, mn, mx, inc; int mute; uint hw;
      int h1 = v.GetMasterVolumeLevelScalar(out sc), h2 = v.GetMasterVolumeLevel(out db), h3 = v.GetMute(out mute);
      int h4 = v.QueryHardwareSupport(out hw), h5 = v.GetVolumeRange(out mn, out mx, out inc);
      return "endpoint volume scalar " + sc.ToString("F2") + " = " + db.ToString("F1") + " dB, mute " + mute + ", hw support 0x" +
        hw.ToString("X") + ", range " + mn.ToString("F1") + ".." + mx.ToString("F1") + " step " + inc.ToString("F2") +
        " (hr " + h1.ToString("X") + "/" + h2.ToString("X") + "/" + h3.ToString("X") + "/" + h4.ToString("X") + "/" + h5.ToString("X") + ")";
    }
    // Shared-mode tone in the mix format (float or 16-bit), amplitude Amp (0.1, A12).
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
          for (int i = 0; i < n; i++) { double v = Amp * Math.Sin(ph); ph += step;
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
  L ("vol: " + [S10e.Wasapi]::Vol())
  L ("props: " + [S10e.Wasapi]::Props())
  L ("formats: " + [S10e.Wasapi]::Formats())
  Key 0xAE 50; Key 0xAF 5; L 'volume keys: 0 then +5 steps (~10%)'; L ("vol: " + [S10e.Wasapi]::Vol())
  Start-Sleep 1; L 'play 1000 Hz exclusive 48k/16 (amplitude 0.1, endpoint volume via the driver element)'; L ([S10e.Wasapi]::Exclusive(1000, 1500))
  Start-Sleep 1; L 'play 1500 Hz A (~10%)'; L ([S10e.Wasapi]::Tone(1500, 2000))
  Start-Sleep 1; Key 0xAE 2; L 'volume down x2 (~6%)'; L ("vol: " + [S10e.Wasapi]::Vol())
  Start-Sleep 1; L 'play 1500 Hz B (~6%)'; L ([S10e.Wasapi]::Tone(1500, 2000))
  Start-Sleep 1; Key 0xAF 5; L 'volume up x5 (~16%)'; L ("vol: " + [S10e.Wasapi]::Vol())
  Start-Sleep 1; L 'play 2000 Hz C (~16%)'; L ([S10e.Wasapi]::Tone(2000, 2000))
  Start-Sleep 1; Key 0xAD 1; L 'mute key'; L ("vol: " + [S10e.Wasapi]::Vol())
  Start-Sleep 1; L 'play 2000 Hz D (muted)'; L ([S10e.Wasapi]::Tone(2000, 2000))
  Key 0xAD 1; Key 0xAE 3; L 'unmuted, volume back to ~10%'; L ("vol: " + [S10e.Wasapi]::Vol())
} catch { L "ERROR $_" }
L 'end'
