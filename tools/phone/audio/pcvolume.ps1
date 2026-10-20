# pcvolume.ps1 [-Level 0..1] [-Mute 0|1] - the PC's default speaker endpoint: prints mute + master level (scalar), and
# sets them when given (Core Audio IMMDeviceEnumerator -> IAudioEndpointVolume, documented COM interfaces). Used by the
# phone mic test (notes/d8-mic.md) to make the PC tone audible; mictest restores the previous values.
param([double]$Level = -1, [int]$Mute = -1)
Add-Type -Language CSharp @'
using System;
using System.Runtime.InteropServices;
namespace PcVol {
  [Guid("5CDF2C82-841E-4546-9722-0CF74078229A"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  public interface IAudioEndpointVolume {
    int RegisterControlChangeNotify(IntPtr p); int UnregisterControlChangeNotify(IntPtr p);
    int GetChannelCount(out uint c); int SetMasterVolumeLevel(float l, Guid g); int SetMasterVolumeLevelScalar(float l, Guid g);
    int GetMasterVolumeLevel(out float l); int GetMasterVolumeLevelScalar(out float l);
    int SetChannelVolumeLevel(uint c, float l, Guid g); int SetChannelVolumeLevelScalar(uint c, float l, Guid g);
    int GetChannelVolumeLevel(uint c, out float l); int GetChannelVolumeLevelScalar(uint c, out float l);
    int SetMute([MarshalAs(UnmanagedType.Bool)] bool m, Guid g); int GetMute(out bool m);
  }
  [Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  public interface IMMDevice { int Activate(ref Guid iid, int ctx, IntPtr p, [MarshalAs(UnmanagedType.IUnknown)] out object o); }
  [Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  public interface IMMDeviceEnumerator { int EnumAudioEndpoints(int f, int s, out IntPtr d); int GetDefaultAudioEndpoint(int f, int r, out IMMDevice d); }
  [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] public class MMDeviceEnumerator {}
  public static class V {
    public static IAudioEndpointVolume Get() {
      var e = (IMMDeviceEnumerator)new MMDeviceEnumerator(); IMMDevice d; e.GetDefaultAudioEndpoint(0, 1, out d);
      Guid iid = typeof(IAudioEndpointVolume).GUID; object o; d.Activate(ref iid, 1, IntPtr.Zero, out o); return (IAudioEndpointVolume)o;
    }
    public static void Set(double level, int mute) {
      var v = Get();
      if (level >= 0) v.SetMasterVolumeLevelScalar((float)level, Guid.Empty);
      if (mute >= 0) v.SetMute(mute != 0, Guid.Empty);
    }
    public static string Read() {
      var v = Get(); bool m; float l; v.GetMute(out m); v.GetMasterVolumeLevelScalar(out l);
      return "mute " + (m ? 1 : 0) + " level " + Math.Round(l, 3);
    }
  }
}
'@
[PcVol.V]::Set($Level, $Mute)
[PcVol.V]::Read()
