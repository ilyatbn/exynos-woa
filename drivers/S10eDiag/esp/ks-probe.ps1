# D8 A11: user-mode KS probe of the Exynos9820Audio render filter (run as SYSTEM from extra-audio-swap.cmd pass 2).
# Repeats what AudioSrv's format probe sends (run audA10b: create pin -> RtAudio QUERY_NOTIFICATION_SUPPORT,
# SETWRITEPACKET basic, PACKETCOUNT basic, BUFFER_WITH_NOTIFICATION, HWLATENCY -> close) and prints every status and
# output byte, which the driver cannot see (ACX answers them). Filter level: modes, DATARANGES, MODEDATAFORMATS
# (RAW/DEFAULT), PROPOSEDATAFORMAT get, CINSTANCES. Then a short silent run: ACQUIRE -> PAUSE -> RUN 300 ms (buffer
# zero-filled = silence) with PRESENTATION_POSITION / PACKETCOUNT reads -> STOP -> close.
# notes/d8-audio-win-a5.md "A11". Output: the -Out file (default C:\S10eTmp\ksprobe.txt).
param([string]$Out = 'C:\S10eTmp\ksprobe.txt', [string]$Match = 'sams9827', [switch]$NoRun)
$ErrorActionPreference = 'Continue'
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
using System.Threading;
namespace S10eKs {
  public static class Probe {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sa, uint disp, uint flags, IntPtr tmpl);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr h, uint code, byte[] inBuf, int inLen, byte[] outBuf, int outLen, out int ret, IntPtr ov);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("ksuser.dll")] static extern int KsCreatePin(IntPtr filter, byte[] connect, uint access, out IntPtr pin);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern IntPtr SetupDiGetClassDevsW(ref Guid cls, IntPtr enumr, IntPtr hwnd, uint flags);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiEnumDeviceInterfaces(IntPtr set, IntPtr dev, ref Guid cls, uint idx, ref SP_DEVICE_INTERFACE_DATA d);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiGetDeviceInterfaceDetailW(IntPtr set, ref SP_DEVICE_INTERFACE_DATA d, IntPtr detail, int size, out int req, IntPtr dev);
    [DllImport("setupapi.dll")] static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
    [StructLayout(LayoutKind.Sequential)] struct SP_DEVICE_INTERFACE_DATA { public int cbSize; public Guid cls; public int flags; public IntPtr reserved; }

    const uint IOCTL_KS_PROPERTY = 0x2F0003;
    const uint GET = 1, SET = 2, BASIC = 0x200;
    static readonly Guid Pin = new Guid("8C134960-51AD-11CF-878A-94F801C10000");
    static readonly Guid RtAudio = new Guid("A855A48C-2F78-4729-9051-1968746B9EEF");
    static readonly Guid Modes = new Guid("4F67B528-30C9-40DE-B2FB-859DDD1F3470");
    static readonly Guid Conn = new Guid("1D58C920-AC9B-11CF-A5D6-28DB04C10000");
    static readonly Guid Raw = new Guid("9E90EA20-B493-4FD1-A1A8-7E1361A956CF");
    static readonly Guid Default = new Guid("C18E2F7E-933D-4965-B7D1-1EEF228D2AF3");
    static StringBuilder sb = new StringBuilder();
    static void L(string s) { sb.Append(DateTime.Now.ToString("HH:mm:ss.fff ") + s + "\r\n"); }
    static string Hex(byte[] b, int n) { if (n <= 0) return ""; return BitConverter.ToString(b, 0, Math.Min(n, b.Length)).Replace("-", ""); }

    static byte[] Prop(Guid set, uint id, uint flags, int extra) {
      byte[] p = new byte[24 + extra]; Array.Copy(set.ToByteArray(), 0, p, 0, 16);
      BitConverter.GetBytes(id).CopyTo(p, 16); BitConverter.GetBytes(flags).CopyTo(p, 20); return p;
    }
    // One property: size query first (out 0), then sized; logs status + bytes.
    static byte[] Q(IntPtr h, string name, byte[] inBuf, int fixedOut) {
      int ret; byte[] o;
      if (fixedOut > 0) {
        o = new byte[fixedOut];
        bool ok = DeviceIoControl(h, IOCTL_KS_PROPERTY, inBuf, inBuf.Length, o, o.Length, out ret, IntPtr.Zero);
        L(name + ": " + (ok ? "OK" : "err " + Marshal.GetLastWin32Error()) + " ret " + ret + " " + Hex(o, ret));
        return ok ? o : null;
      }
      bool ok0 = DeviceIoControl(h, IOCTL_KS_PROPERTY, inBuf, inBuf.Length, null, 0, out ret, IntPtr.Zero);
      int e0 = Marshal.GetLastWin32Error();
      L(name + " size query: " + (ok0 ? "OK" : "err " + e0) + " ret " + ret);
      if (ret <= 0) return null;
      o = new byte[ret];
      bool ok1 = DeviceIoControl(h, IOCTL_KS_PROPERTY, inBuf, inBuf.Length, o, o.Length, out ret, IntPtr.Zero);
      L(name + ": " + (ok1 ? "OK" : "err " + Marshal.GetLastWin32Error()) + " ret " + ret + " " + Hex(o, ret));
      return ok1 ? o : null;
    }
    static byte[] KspPin(uint id, uint flags, uint pin, int extra) {
      byte[] p = Prop(Pin, id, flags, 8 + extra); BitConverter.GetBytes(pin).CopyTo(p, 24); return p;
    }
    static byte[] Format(int bits) {   // KSDATAFORMAT_WAVEFORMATEXTENSIBLE 48 kHz stereo PCM
      byte[] f = new byte[104]; int align = 2 * bits / 8;
      BitConverter.GetBytes(104).CopyTo(f, 0);
      new Guid("73647561-0000-0010-8000-00AA00389B71").ToByteArray().CopyTo(f, 16);
      new Guid("00000001-0000-0010-8000-00AA00389B71").ToByteArray().CopyTo(f, 32);
      new Guid("05589F81-C356-11CE-BF01-00AA0055595A").ToByteArray().CopyTo(f, 48);
      BitConverter.GetBytes((ushort)0xFFFE).CopyTo(f, 64); BitConverter.GetBytes((ushort)2).CopyTo(f, 66);
      BitConverter.GetBytes(48000).CopyTo(f, 68); BitConverter.GetBytes(48000 * align).CopyTo(f, 72);
      BitConverter.GetBytes((ushort)align).CopyTo(f, 76); BitConverter.GetBytes((ushort)bits).CopyTo(f, 78);
      BitConverter.GetBytes((ushort)22).CopyTo(f, 80); BitConverter.GetBytes((ushort)bits).CopyTo(f, 82);
      BitConverter.GetBytes(3).CopyTo(f, 84);
      new Guid("00000001-0000-0010-8000-00AA00389B71").ToByteArray().CopyTo(f, 88);
      return f;
    }
    static string FilterPath(string match) {
      Guid cat = new Guid("6994AD04-93EF-11D0-A3CC-00A0C9223196");   // KSCATEGORY_AUDIO
      IntPtr set = SetupDiGetClassDevsW(ref cat, IntPtr.Zero, IntPtr.Zero, 0x12);   // PRESENT | DEVICEINTERFACE
      try {
        for (uint i = 0; ; i++) {
          var d = new SP_DEVICE_INTERFACE_DATA(); d.cbSize = Marshal.SizeOf(d);
          if (!SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref cat, i, ref d)) break;
          int req; SetupDiGetDeviceInterfaceDetailW(set, ref d, IntPtr.Zero, 0, out req, IntPtr.Zero);
          IntPtr buf = Marshal.AllocHGlobal(req); Marshal.WriteInt32(buf, IntPtr.Size == 8 ? 8 : 6);
          string p = null;
          if (SetupDiGetDeviceInterfaceDetailW(set, ref d, buf, req, out req, IntPtr.Zero)) p = Marshal.PtrToStringUni(buf + 4);
          Marshal.FreeHGlobal(buf);
          if (p != null) { L("interface " + p); if (p.ToLowerInvariant().Contains(match)) return p; }
        }
      } finally { SetupDiDestroyDeviceInfoList(set); }
      return null;
    }

    static void PinProbe(IntPtr filter, int bits, uint iface, bool run) {
      byte[] c = new byte[72 + 104];
      new Guid("1A8766A0-62CE-11CF-A5D6-28DB04C10000").ToByteArray().CopyTo(c, 0);   // KSINTERFACESETID_Standard
      BitConverter.GetBytes(iface).CopyTo(c, 16);
      new Guid("4747B320-62CE-11CF-A5D6-28DB04C10000").ToByteArray().CopyTo(c, 24);  // KSMEDIUMSETID_Standard
      BitConverter.GetBytes(0u).CopyTo(c, 48);                                       // PinId 0 (host pin)
      BitConverter.GetBytes(0x40000000u).CopyTo(c, 64);                               // KSPRIORITY_NORMAL
      Format(bits).CopyTo(c, 72);
      IntPtr pin; int r = KsCreatePin(filter, c, 0xC0000000u, out pin);
      L("--- KsCreatePin " + bits + " bit, interface " + iface + ": " + r + (r == 0 ? "" : " (Win32 error)"));
      if (r != 0) return;
      try {
        Q(pin, "RtAudio.QUERY_NOTIFICATION_SUPPORT get", Prop(RtAudio, 8, GET, 0), 4);
        Q(pin, "RtAudio.SETWRITEPACKET basic", Prop(RtAudio, 12, BASIC, 0), 4);
        Q(pin, "RtAudio.PACKETCOUNT basic", Prop(RtAudio, 9, BASIC, 0), 4);
        Q(pin, "RtAudio.PRESENTATION_POSITION basic", Prop(RtAudio, 10, BASIC, 0), 4);
        Q(pin, "RtAudio.GETREADPACKET basic", Prop(RtAudio, 11, BASIC, 0), 4);
        byte[] b = Prop(RtAudio, 5, GET, 16);                                        // BUFFER_WITH_NOTIFICATION
        BitConverter.GetBytes(2 * 480 * 2 * bits / 8).CopyTo(b, 32);                 // RequestedBufferSize: 2 x 10 ms
        BitConverter.GetBytes(2).CopyTo(b, 36);                                      // NotificationCount
        byte[] buf = Q(pin, "RtAudio.BUFFER_WITH_NOTIFICATION get (2 x 10 ms)", b, 16);
        Q(pin, "RtAudio.HWLATENCY get", Prop(RtAudio, 2, GET, 0), 12);
        Q(pin, "RtAudio.PACKETCOUNT get", Prop(RtAudio, 9, GET, 0), 4);
        if (run && buf != null) {
          foreach (uint st in new uint[] { 1, 2, 3 }) {
            byte[] s = Prop(Conn, 0, SET, 0); byte[] v = BitConverter.GetBytes(st);   // KS set: value in the out buffer
            int ret; bool ok = DeviceIoControl(pin, IOCTL_KS_PROPERTY, s, s.Length, v, 4, out ret, IntPtr.Zero);
            L("Connection.STATE set " + st + ": " + (ok ? "OK" : "err " + Marshal.GetLastWin32Error()));
          }
          for (int i = 0; i < 4; i++) {
            Thread.Sleep(100);
            Q(pin, "RtAudio.PRESENTATION_POSITION get", Prop(RtAudio, 10, GET, 0), 16);
            Q(pin, "RtAudio.PACKETCOUNT get", Prop(RtAudio, 9, GET, 0), 4);
          }
          foreach (uint st in new uint[] { 2, 1, 0 }) {
            byte[] s = Prop(Conn, 0, SET, 0); byte[] v = BitConverter.GetBytes(st);
            int ret; bool ok = DeviceIoControl(pin, IOCTL_KS_PROPERTY, s, s.Length, v, 4, out ret, IntPtr.Zero);
            L("Connection.STATE set " + st + ": " + (ok ? "OK" : "err " + Marshal.GetLastWin32Error()));
          }
        }
      } finally { CloseHandle(pin); L("pin closed"); }
    }

    public static string Run(string match, bool allowRun) {
      try {
        string path = FilterPath(match);
        if (path == null) { L("no KSCATEGORY_AUDIO interface matching " + match); return sb.ToString(); }
        L("probing " + path);
        IntPtr f = CreateFileW(path, 0xC0000000u, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (f == new IntPtr(-1)) { L("CreateFile filter: error " + Marshal.GetLastWin32Error()); return sb.ToString(); }
        try {
          Q(f, "Pin.CTYPES get", Prop(Pin, 1, GET, 0), 4);
          Q(f, "Pin.CINSTANCES pin0 get", KspPin(0, GET, 0, 0), 8);
          Q(f, "Pin.GLOBALCINSTANCES pin0 get", KspPin(8, GET, 0, 0), 8);
          Q(f, "AudioSignalProc.MODES pin0 get", Prop(Modes, 0, GET, 8), 0);   // KSP_PIN, pin id 0
          Q(f, "Pin.DATARANGES pin0 get", KspPin(3, GET, 0, 0), 0);
          foreach (var m in new[] { Raw, Default }) {
            byte[] p = KspPin(16, GET, 0, 16); m.ToByteArray().CopyTo(p, 32);
            Q(f, "Pin.MODEDATAFORMATS pin0 " + (m == Raw ? "RAW" : "DEFAULT") + " get", p, 0);
          }
          Q(f, "Pin.PROPOSEDATAFORMAT pin0 get (default)", KspPin(14, GET, 0, 0), 0);
          PinProbe(f, 16, 1, allowRun);  // KSINTERFACE_STANDARD_LOOPED_STREAMING (WaveRT)
          PinProbe(f, 32, 1, false);
          PinProbe(f, 16, 0, false);   // KSINTERFACE_STANDARD_STREAMING, for comparison
        } finally { CloseHandle(f); }
      } catch (Exception e) { L("EXCEPTION " + e); }
      return sb.ToString();
    }
  }
}
'@
[S10eKs.Probe]::Run($Match.ToLowerInvariant(), -not $NoRun) | Out-File -Encoding ascii $Out
