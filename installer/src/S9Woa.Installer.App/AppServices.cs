// SPDX-License-Identifier: BSD-2-Clause-Patent
using S9Woa.Installer.Core;
using S9Woa.Installer.Core.Deploy;
using S9Woa.Installer.Core.Device;
using S9Woa.Installer.Core.Image;
using S9Woa.Installer.Core.Processes;
using S9Woa.Installer.Core.Recovery;
using S9Woa.Installer.Core.Stages;
using S9Woa.Installer.Core.Toolset;

namespace S9Woa.Installer.App;

/// <summary>Process-wide services and wizard selections shared by the pages.</summary>
internal static class AppServices
{
    private static readonly object LogLock = new();

    public static IProcessRunner Runner { get; } = new ProcessRunner();

    /// <summary>
    /// Settings, logs, backups and the toolset: %LOCALAPPDATA%\S9WoaInstaller, or the portable
    /// <c>data</c> folder when one sits next to the app.
    /// </summary>
    public static string DataDirectory { get; } = InstallState.ResolveDirectory(AppContext.BaseDirectory);
    public static string LogDirectory { get; } = Path.Combine(DataDirectory, "logs");
    public static string LogFile { get; } = Path.Combine(LogDirectory, $"installer-{DateTime.Now:yyyyMMdd-HHmmss}.log");
    public static InstallState State { get; } = InstallState.Load(DataDirectory);

    /// <summary>Hides phone serials and the Windows user folder in the log and on screen.</summary>
    public static Redactor Privacy { get; } = new(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile));

    static AppServices()
    {
        Privacy.AddSerial(State.DeviceSerial);
        foreach (var version in HostVersions())
        {
            Privacy.Hide(version);
        }
    }

    /// <summary>This PC's own Windows version strings (it may run a preview or internal build).</summary>
    private static IEnumerable<string?> HostVersions()
    {
        var v = Environment.OSVersion.Version;
        using var key = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion");
        yield return key?.GetValue("UBR") is int ubr ? $"{v.Major}.{v.Minor}.{v.Build}.{ubr}" : null;
        yield return key?.GetValue("BuildLabEx") as string;
        yield return key?.GetValue("BuildLab") as string;
    }

    public static string Redact(string? text) => Privacy.Redact(text);
    /// <summary>Where the image is built (chosen on the This PC page); remembered so Resume finds it.</summary>
    public static string WorkDirectory { get; set; } = State.WorkDirectory ?? Path.Combine(DataDirectory, "work");
    public static string BackupDirectory { get; set; } = Path.Combine(DataDirectory, "backups");

    /// <summary>
    /// This phone's backup folder, named by its alias so the serial never shows in a path on screen.
    /// A backup an earlier installer made (in a folder named by the serial) keeps being used.
    /// </summary>
    public static string BackupFolderFor(string serial)
    {
        var legacy = Path.Combine(BackupDirectory, serial);
        return Directory.Exists(legacy) ? legacy : Path.Combine(BackupDirectory, Redactor.Alias(serial));
    }

    /// <summary>Everything the installer needs on this PC, configured on first run by the Setup page.</summary>
    public static ToolsetManager Toolset { get; } = new(
        ToolsetPaths.ForCurrentUser(AppContext.BaseDirectory, DataDirectory), Runner,
        new HttpClient { Timeout = TimeSpan.FromHours(1) }, new LocalMachineRegistry(), new AuthenticodeVerifier());

    public static string DriversDirectory => Toolset.ResolvePath(Tools.Drivers) ?? Toolset.DriversPayload;
    public static string? TwrpImagePath => Toolset.ResolvePath(Tools.Twrp);
    public static string? UefiImagePath => Toolset.ResolvePath(Tools.Uefi);

    /// <summary>Raw Windows volume image (produced by the VHDX build + export) written to USERDATA.</summary>
    public static string WindowsImagePath => Path.Combine(WorkDirectory, "out", "windows.img");

    /// <summary>ESP boot files (produced by the VHDX build) copied to the phone's EFI partition.</summary>
    public static string EspDirectory => Path.Combine(WorkDirectory, "out", "esp");

    /// <summary>OOBE answer-file choices for the built image (the password is never persisted).</summary>
    public static Core.Image.UnattendOptions Unattend { get; set; } =
        new() { Username = string.IsNullOrWhiteSpace(State.AccountName) ? "S9" : State.AccountName };

    /// <summary>Last successful image build, if any.</summary>
    public static Core.Image.BuiltImage? Built { get; set; }

    public static string? AdbPath { get; private set; }
    public static AdbClient? Adb { get; private set; }
    public static DeviceActions? Device { get; private set; }
    public static string? HeimdallPath { get; private set; }
    public static string? ZadigPath { get; private set; }

    /// <summary>TWRP flasher preference order: the built-in Download-mode flasher, then Heimdall if installed.</summary>
    public static TwrpFlashService TwrpFlasher { get; private set; } = new([]);

    /// <summary>Re-resolves program paths after the Setup page installs or changes a tool.</summary>
    public static void ReloadTools()
    {
        AdbPath = Toolset.ResolvePath(Tools.Adb);
        Adb = AdbPath is null ? null : new AdbClient(AdbPath, Runner, Privacy.AddSerial);
        Device = Adb is null ? null : new DeviceActions(Adb, Runner);
        HeimdallPath = Toolset.ResolvePath(Tools.Heimdall);
        ZadigPath = Toolset.ResolvePath(Tools.Zadig);
        ITwrpFlasher native = new Core.Deploy.Odin.OdinTwrpFlasher(new LocalMachineRegistry());
        TwrpFlasher = new TwrpFlashService(HeimdallPath is null ? [native] : [native, new HeimdallTwrpFlasher(HeimdallPath, Runner)]);
    }

    public static TwrpClient? Twrp(string serial) => AdbPath is null ? null : new TwrpClient(AdbPath, serial, Runner);

    /// <summary>
    /// Whether a phone is in Download mode, and if so the phone this installation identified before
    /// (null when it hasn't identified one yet). Only called when adb sees no phone.
    /// </summary>
    public static async Task<(bool Present, DeviceSnapshot? Phone)> FindDownloadModeAsync(CancellationToken ct = default)
    {
        if (await TwrpFlasher.ResolveAsync(ct) is null)
        {
            return (false, null);
        }
        return (true, DeviceSnapshot.InDownloadMode(State.DeviceSerial, State.DeviceBootloader));
    }

    /// <summary>Flash TWRP (off: it is already on the phone, so the TWRP step only starts it).</summary>
    public static bool SkipTwrpFlash { get; set; } = State.SkipTwrpFlash;

    private static DeviceSnapshot? _currentDevice;

    /// <summary>The phone last identified on the phone page; raises <see cref="CurrentDeviceChanged"/>.</summary>
    public static DeviceSnapshot? CurrentDevice
    {
        get => _currentDevice;
        set
        {
            Privacy.AddSerial(value?.Serial);
            _currentDevice = value;
            // The payloads (TWRP image, the WinRE recovery built from it, the UFS
            // driver package) are per phone, so the toolset has to know which one
            // this is. Null here means "no phone", not "the reference phone".
            Toolset.Config.DeviceCodename = value?.Codename;
            Toolset.Config.Save(Toolset.Paths.DataDirectory);
            CurrentDeviceChanged?.Invoke();
        }
    }

    public static event Action? CurrentDeviceChanged;

    public static bool RisksAccepted { get; set; }

    /// <summary>Opt-in to run stages that are automated but not yet validated on the reference device.</summary>
    public static bool ExperimentalEnabled { get; set; }

    /// <summary>Read back and hash every block written to the phone. Slower; on unless turned off.</summary>
    public static bool VerifyWrites { get; set; } = State.VerifyWrites ?? true;

    /// <summary>In-Windows (on the phone) Restart-to-TWRP over the UFS vendor ticket.</summary>
    public static RecoveryTicketService RecoveryTickets { get; } =
        new(new ScsiPassThroughTransportFactory(), new ShutdownExeRestart());

    /// <summary>Chosen Windows edition index inside the media, resolved by the media stage.</summary>
    public static string? InstallImagePath { get; set; }
    public static int EditionIndex { get; set; } = 1;

    /// <summary>The media's Windows build as DISM reports it (e.g. 22621.2428), resolved by the media stage.</summary>
    public static string? MediaBuild { get; set; }

    public static string? MediaPath { get; set; } = State.MediaPath;
    public static SlimProfile Profile { get; set; } =
        Enum.TryParse<SlimProfile>(State.SlimProfile, out var p) ? p : SlimProfile.Lite;

    public static event Action<string>? LogWritten;

    public static void Log(string message)
    {
        var line = $"{DateTime.Now:HH:mm:ss} {Redact(message)}";
        lock (LogLock)
        {
            Directory.CreateDirectory(LogDirectory);
            File.AppendAllText(LogFile, line + Environment.NewLine);
        }
        LogWritten?.Invoke(line);
    }

    public static void SaveState()
    {
        State.MediaPath = MediaPath;
        State.SlimProfile = Profile.ToString();
        State.AccountName = Unattend.Username;
        State.VerifyWrites = VerifyWrites;
        State.SkipTwrpFlash = SkipTwrpFlash;
        State.WorkDirectory = WorkDirectory;
        State.DeviceSerial = CurrentDevice?.Serial ?? State.DeviceSerial;
        State.DeviceBootloader = CurrentDevice?.Bootloader ?? State.DeviceBootloader;
        State.Save(DataDirectory);
    }
}
