// S10eSpeakerTrim - top (earpiece) / bottom (loudspeaker) level control for Exynos9820Audio (>= 1.0.1.17).
// Talks to the driver's private KS property (drivers/Exynos9820Audio/AudControl.h) on the render filter.
//   S10eSpeakerTrim.exe                 tray app (sliders)
//   S10eSpeakerTrim.exe get             print the current trim
//   S10eSpeakerTrim.exe set RA SA RG SG raw values: RcvAtten SpkAtten (1/8 dB) RcvGain SpkGain (0..17)
// Build (PC): csc /target:winexe /platform:anycpu S10eSpeakerTrim.cs  (.NET Framework 4.x, runs on ARM64 Windows)
using System;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Windows.Forms;
using Microsoft.Win32.SafeHandles;

static class Ks
{
    // AUDPROPSETID_SpeakerTrim, AudControl.h
    static readonly Guid TrimSet = new Guid("6FEF068C-7388-4619-86D2-91FCA6A5E4AC");
    static readonly Guid KsCategoryAudio = new Guid("6994AD04-93EF-11D0-A3CC-00A0C9223196");
    const uint IOCTL_KS_PROPERTY = 0x002F0003;   // CTL_CODE(FILE_DEVICE_KS, 0, METHOD_NEITHER, FILE_ANY_ACCESS)
    const uint KSPROPERTY_TYPE_GET = 1, KSPROPERTY_TYPE_SET = 2;

    [StructLayout(LayoutKind.Sequential)] struct KSPROPERTY { public Guid Set; public uint Id; public uint Flags; }
    [StructLayout(LayoutKind.Sequential)] public struct Trim { public uint RcvAtten, SpkAtten, RcvGain, SpkGain; }

    [StructLayout(LayoutKind.Sequential)] struct SP_DEVICE_INTERFACE_DATA { public int cbSize; public Guid ClassGuid; public int Flags; public IntPtr Reserved; }
    [DllImport("setupapi.dll", SetLastError = true)] static extern IntPtr SetupDiGetClassDevs(ref Guid g, IntPtr e, IntPtr h, int flags);
    [DllImport("setupapi.dll", SetLastError = true)] static extern bool SetupDiEnumDeviceInterfaces(IntPtr s, IntPtr d, ref Guid g, int i, ref SP_DEVICE_INTERFACE_DATA data);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr s, ref SP_DEVICE_INTERFACE_DATA data, IntPtr detail, int size, out int req, IntPtr dev);
    [DllImport("setupapi.dll")] static extern bool SetupDiDestroyDeviceInfoList(IntPtr s);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern SafeFileHandle CreateFile(string n, uint access, uint share, IntPtr sa, uint disp, uint flags, IntPtr t);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint code, ref KSPROPERTY inb, int inl, ref Trim outb, int outl, out int ret, IntPtr ov);

    // Every KSCATEGORY_AUDIO interface of the S10e audio device (ACPI\SAMS9827).
    static System.Collections.Generic.List<string> Paths()
    {
        var list = new System.Collections.Generic.List<string>();
        Guid g = KsCategoryAudio;
        IntPtr set = SetupDiGetClassDevs(ref g, IntPtr.Zero, IntPtr.Zero, 0x12);   // DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
        if (set == new IntPtr(-1)) return list;
        try {
            for (int i = 0; ; i++) {
                var d = new SP_DEVICE_INTERFACE_DATA { cbSize = Marshal.SizeOf(typeof(SP_DEVICE_INTERFACE_DATA)) };
                if (!SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref g, i, ref d)) break;
                int req;
                SetupDiGetDeviceInterfaceDetail(set, ref d, IntPtr.Zero, 0, out req, IntPtr.Zero);
                IntPtr buf = Marshal.AllocHGlobal(req);
                try {
                    Marshal.WriteInt32(buf, 8);   // cbSize of SP_DEVICE_INTERFACE_DETAIL_DATA_W on 64-bit
                    if (SetupDiGetDeviceInterfaceDetail(set, ref d, buf, req, out req, IntPtr.Zero)) {
                        string p = Marshal.PtrToStringUni(buf + 4);
                        if (p.IndexOf("sams9827", StringComparison.OrdinalIgnoreCase) >= 0) list.Add(p);
                    }
                } finally { Marshal.FreeHGlobal(buf); }
            }
        } finally { SetupDiDestroyDeviceInfoList(set); }
        return list;
    }

    static Trim Call(bool set, Trim t)
    {
        string err = "no ACPI\\SAMS9827 audio interface";
        foreach (string p in Paths()) {
            using (var h = CreateFile(p, 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero)) {   // R/W, share R/W, OPEN_EXISTING
                if (h.IsInvalid) { err = p + ": open error " + Marshal.GetLastWin32Error(); continue; }
                var k = new KSPROPERTY { Set = TrimSet, Id = 0, Flags = set ? KSPROPERTY_TYPE_SET : KSPROPERTY_TYPE_GET };
                int ret;
                Trim io = t;
                if (DeviceIoControl(h, IOCTL_KS_PROPERTY, ref k, Marshal.SizeOf(k), ref io, Marshal.SizeOf(io), out ret, IntPtr.Zero))
                    return set ? t : io;
                err = p + ": property error " + Marshal.GetLastWin32Error();
            }
        }
        throw new Exception(err);
    }

    public static Trim Get() { return Call(false, new Trim()); }
    public static void Set(Trim t) { Call(true, t); }
}

class TrimForm : Form
{
    // Split/Join work in absolute half-dB: 0 = analog gain 10 (10.5 dB) with digital 0 dB; above 0 the analog gain rises
    // (1 dB/step, driver cap 17 = +7 dB, the stock music value), below 0 the digital attenuation grows. The sliders show
    // half-dB relative to the per-speaker baseline below (range -20 dB .. the gain cap).
    const int GainBase = 10, GainMax = 17;
    // 0 dB on each slider = the driver's default balance (Driver.c AudReadConfig, set by ear 2026-10-16):
    // top gain 12 / atten 0, bottom gain 10 / atten 40. In absolute half-dB (Join): +4 and -10.
    static readonly int TopBase = Join(12, 0), BottomBase = Join(10, 40);
    readonly TrackBar top = new TrackBar(), bottom = new TrackBar();
    readonly Label topL = new Label(), bottomL = new Label(), status = new Label();
    readonly Timer apply = new Timer { Interval = 150 };
    readonly NotifyIcon tray = new NotifyIcon();
    bool loading;

    // level in 0.5 dB units -> (gain, atten): use analog gain for the positive part, digital attenuation for the rest
    static void Split(int halfDb, out uint gain, out uint atten)
    {
        if (halfDb >= 0) {
            int g = (halfDb + 1) / 2;               // whole dB of analog gain, round up
            gain = (uint)(GainBase + g);
            atten = (uint)((g * 2 - halfDb) * 4);   // trim the extra half dB digitally (1/8 dB steps)
        } else {
            gain = GainBase;
            atten = (uint)(-halfDb * 4);
        }
    }
    static int Join(uint gain, uint atten) { return ((int)gain - GainBase) * 2 - (int)(atten / 4); }

    TrimForm()
    {
        Text = "S10e speakers"; FormBorderStyle = FormBorderStyle.FixedToolWindow; StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(360, 190); Font = new Font("Segoe UI", 10f);
        Setup(top, topL, "Top (earpiece)", 10, TopBase);
        Setup(bottom, bottomL, "Bottom (loudspeaker)", 80, BottomBase);
        status.SetBounds(12, 155, 340, 24); Controls.Add(status);
        apply.Tick += delegate { apply.Stop(); Push(); };
        tray.Icon = SystemIcons.Information; tray.Text = "S10e speakers"; tray.Visible = true;
        tray.MouseClick += delegate { Show(); WindowState = FormWindowState.Normal; Activate(); };
        var menu = new ContextMenuStrip();
        menu.Items.Add("Default (0 / 0 dB)", null, delegate { top.Value = 0; bottom.Value = 0; });
        menu.Items.Add("Exit", null, delegate { tray.Visible = false; Application.Exit(); });
        tray.ContextMenuStrip = menu;
        FormClosing += (s, e) => { if (e.CloseReason == CloseReason.UserClosing) { e.Cancel = true; Hide(); } };
        Load += delegate { Pull(); };
    }

    void Setup(TrackBar t, Label l, string name, int y, int bas)
    {
        var n = new Label { Text = name }; n.SetBounds(12, y, 200, 22); Controls.Add(n);
        l.SetBounds(270, y, 80, 22); l.TextAlign = ContentAlignment.MiddleRight; Controls.Add(l);
        t.SetBounds(8, y + 22, 344, 40); t.Minimum = -40; t.Maximum = (GainMax - GainBase) * 2 - bas; t.TickFrequency = 4;
        t.SmallChange = 1; t.LargeChange = 4;
        t.ValueChanged += delegate { l.Text = (t.Value / 2.0).ToString("+0.0;-0.0;0.0") + " dB"; if (!loading) apply.Start(); };
        Controls.Add(t);
    }

    void Pull()
    {
        try {
            var t = Ks.Get();
            loading = true;
            top.Value = Math.Max(top.Minimum, Math.Min(top.Maximum, Join(t.RcvGain, t.RcvAtten) - TopBase));
            bottom.Value = Math.Max(bottom.Minimum, Math.Min(bottom.Maximum, Join(t.SpkGain, t.SpkAtten) - BottomBase));
            loading = false;
            status.Text = string.Format("driver: top gain {0} att {1}, bottom gain {2} att {3}", t.RcvGain, t.RcvAtten, t.SpkGain, t.SpkAtten);
        } catch (Exception e) { loading = false; status.Text = e.Message; }
    }

    void Push()
    {
        var t = new Ks.Trim();
        Split(top.Value + TopBase, out t.RcvGain, out t.RcvAtten);
        Split(bottom.Value + BottomBase, out t.SpkGain, out t.SpkAtten);
        try { Ks.Set(t); Pull(); } catch (Exception e) { status.Text = e.Message; }
    }

    [STAThread]
    static int Main(string[] a)
    {
        if (a.Length > 0) {
            try {
                if (a[0] == "set" && a.Length == 5)
                    Ks.Set(new Ks.Trim { RcvAtten = uint.Parse(a[1]), SpkAtten = uint.Parse(a[2]), RcvGain = uint.Parse(a[3]), SpkGain = uint.Parse(a[4]) });
                var t = Ks.Get();
                Console.WriteLine("RcvAtten {0} SpkAtten {1} RcvGain {2} SpkGain {3}", t.RcvAtten, t.SpkAtten, t.RcvGain, t.SpkGain);
                return 0;
            } catch (Exception e) { Console.WriteLine("error: " + e.Message); return 1; }
        }
        Application.EnableVisualStyles();
        Application.Run(new TrimForm());
        return 0;
    }
}
