// SPDX-License-Identifier: BSD-2-Clause-Patent
using System.Text.Json;
using System.Text.Json.Serialization;

namespace S9Woa.Installer.Core.Stages;

public enum StageStatus
{
    Pending,
    Running,
    Done,
    Failed,
    Skipped,
}

public enum StageAvailability
{
    /// <summary>Implemented and validated on the reference device.</summary>
    Ready,

    /// <summary>The user performs this step by hand; the installer guides and verifies.</summary>
    Guided,

    /// <summary>
    /// Automated and unit-tested, but not yet validated end to end on the reference
    /// device. Runs only when the user opts in to experimental stages.
    /// </summary>
    Experimental,

    /// <summary>Planned; the installer stops before this stage.</summary>
    NotImplemented,
}

public sealed record StageDefinition(string Id, string Title, string Summary, StageAvailability Availability,
    bool Destructive = false);

public static class StageCatalog
{
    public static IReadOnlyList<StageDefinition> All { get; } =
    [
        new("host", "Check this PC", "Administrator rights, disk space, adb and Samsung USB driver.", StageAvailability.Ready),
        new("identify", "Identify the phone", "Model, firmware and bootloader state over ADB.", StageAvailability.Ready),
        new("media", "Get Windows", "Use your ISO/ESD, or download ARM64 media from Microsoft.", StageAvailability.Ready),
        new("image", "Build the Windows image", "Apply, add drivers, slim (optional), configure boot and first-run settings. Done on this PC before the phone is touched.", StageAvailability.Ready),
        new("unlock", "Unlock the bootloader", "OEM unlock in Developer options, then the Download-mode unlock. Wipes Android.", StageAvailability.Guided, Destructive: true),
        new("twrp", "Install TWRP", "Flash TWRP to RECOVERY from Download mode and boot it once.", StageAvailability.Ready, Destructive: true),
        new("backup", "Back up the phone", "Copy EFS, modem calibration and the partition table to this PC before anything else is written.", StageAvailability.Ready),
        new("partition", "Prepare partitions", "Locate the target partitions by name and verify the validated layout.", StageAvailability.Ready, Destructive: true),
        new("transfer", "Copy Windows to the phone", "Write the used part of the image and the boot files through TWRP, optionally verifying every block.", StageAvailability.Ready, Destructive: true),
        new("uefi", "Install UEFI", "Flash the UEFI boot image to BOOT. RECOVERY keeps TWRP.", StageAvailability.Ready, Destructive: true),
        new("firstboot", "First boot", "Boot Windows and finish setup.", StageAvailability.Ready),
    ];

    public static StageDefinition Get(string id) => All.First(s => s.Id == id);

    /// <summary>
    /// Availability is per device. The 9810 flow is validated end to end and keeps the
    /// catalog's own states; the 9820 (`beyond0lte`) flow is ported but has never been
    /// observed on a phone, so the stages that write to the device are
    /// <c>NotImplemented</c> there and the image stage — which is PC-side but carries
    /// drivers that are built and test-signed without hardware validation — is
    /// <c>Experimental</c>. Nothing here is relaxed to make the S10e look supported.
    /// </summary>
    private static readonly Dictionary<string, Dictionary<string, StageAvailability>> PerDevice = new()
    {
        ["beyond0lte"] = new()
        {
            ["image"] = StageAvailability.Experimental,
            ["unlock"] = StageAvailability.NotImplemented,
            ["twrp"] = StageAvailability.NotImplemented,
            ["backup"] = StageAvailability.NotImplemented,
            ["partition"] = StageAvailability.NotImplemented,
            ["transfer"] = StageAvailability.NotImplemented,
            ["uefi"] = StageAvailability.NotImplemented,
            ["firstboot"] = StageAvailability.NotImplemented,
        },
    };

    public static StageAvailability Resolve(string id, string? codename = null)
    {
        var declared = Get(id).Availability;
        if (codename is null || !PerDevice.TryGetValue(codename, out var overrides))
        {
            return declared;
        }
        if (overrides.TryGetValue(id, out var found))
        {
            return found;
        }
        return declared;
    }

    /// <summary>False when the stage must not run for this device, whatever its status.</summary>
    public static bool Runnable(string id, string? codename = null) =>
        Resolve(id, codename) != StageAvailability.NotImplemented;
}

public sealed class StageRecord
{
    public StageStatus Status { get; set; }
    public DateTimeOffset? Updated { get; set; }
    public string? Detail { get; set; }
}

/// <summary>
/// Resumable installer state, persisted as JSON in the data folder: %LOCALAPPDATA%\S9WoaInstaller,
/// or the portable <c>data</c> folder next to the app (<see cref="ResolveDirectory"/>).
/// </summary>
public sealed class InstallState
{
    private static readonly JsonSerializerOptions Json = new()
    {
        WriteIndented = true,
        Converters = { new JsonStringEnumConverter() },
    };

    public int SchemaVersion { get; set; } = 1;
    public string? DeviceSerial { get; set; }
    public string? DeviceBootloader { get; set; }
    public string? MediaPath { get; set; }
    public string? SlimProfile { get; set; }
    public string? AccountName { get; set; }

    /// <summary>Read back and hash every block written to the phone (null = default, on).</summary>
    public bool? VerifyWrites { get; set; }

    /// <summary>TWRP is already on the phone: the TWRP step only boots it instead of flashing.</summary>
    public bool SkipTwrpFlash { get; set; }

    /// <summary>The working folder the image is built in (null = the default under this folder).</summary>
    public string? WorkDirectory { get; set; }

    /// <summary>UEFI image (file name in the firmware catalog) matching the built Windows image.</summary>
    public string? FirmwareFile { get; set; }
    public Dictionary<string, StageRecord> Stages { get; set; } = [];

    public static string DefaultDirectory =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "S9WoaInstaller");

    public const string PortableFolderName = "data";

    /// <summary>
    /// Portable mode: a folder named <c>data</c> next to the app keeps the state, logs, backups
    /// and toolset there (a self-contained folder on a USB drive, or one used for a recording);
    /// otherwise <see cref="DefaultDirectory"/>.
    /// </summary>
    public static string ResolveDirectory(string appDirectory, string? defaultDirectory = null)
    {
        var portable = Path.Combine(appDirectory, PortableFolderName);
        return Directory.Exists(portable) ? portable : defaultDirectory ?? DefaultDirectory;
    }

    public StageStatus StatusOf(string id) => Stages.TryGetValue(id, out var r) ? r.Status : StageStatus.Pending;

    public void Set(string id, StageStatus status, string? detail = null, TimeProvider? clock = null)
    {
        _ = StageCatalog.Get(id);
        Stages[id] = new StageRecord { Status = status, Detail = detail, Updated = (clock ?? TimeProvider.System).GetUtcNow() };
    }

    /// <summary>First stage that is neither done nor skipped, in catalog order.</summary>
    public StageDefinition? NextStage() =>
        StageCatalog.All.FirstOrDefault(s => StatusOf(s.Id) is not (StageStatus.Done or StageStatus.Skipped));

    public static InstallState Load(string directory)
    {
        var path = Path.Combine(directory, "state.json");
        return File.Exists(path)
            ? JsonSerializer.Deserialize<InstallState>(File.ReadAllText(path), Json) ?? new InstallState()
            : new InstallState();
    }

    public void Save(string directory)
    {
        Directory.CreateDirectory(directory);
        var path = Path.Combine(directory, "state.json");
        var tmp = path + ".tmp";
        File.WriteAllText(tmp, JsonSerializer.Serialize(this, Json));
        File.Move(tmp, path, overwrite: true);
    }
}
