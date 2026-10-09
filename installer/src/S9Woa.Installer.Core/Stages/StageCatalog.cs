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
    /// catalog's own states. For the 9820 (`beyond0lte`) nothing is <c>Ready</c>: the
    /// PC-side and read-only stages (image, backup, partition) are <c>Experimental</c>
    /// - automated, never observed end to end on this phone - the stages the user must
    /// drive (unlock, twrp, transfer, firstboot) are <c>Guided</c>, and the UEFI stage
    /// stays <c>NotImplemented</c> because no flashable image exists for this device.
    /// <see cref="AutomatedFor"/> is a second axis: a Guided stage whose code is still
    /// the reference phone's automation does not run at all - it returns the manual
    /// procedure instead. Nothing here is relaxed to make the S10e look supported.
    /// </summary>
    private static readonly Dictionary<string, Dictionary<string, StageAvailability>> PerDevice = new()
    {
        ["beyond0lte"] = new()
        {
            ["image"] = StageAvailability.Experimental,
            ["unlock"] = StageAvailability.Guided,
            ["twrp"] = StageAvailability.Guided,
            ["backup"] = StageAvailability.Experimental,
            ["partition"] = StageAvailability.Experimental,
            ["transfer"] = StageAvailability.Guided,
            ["uefi"] = StageAvailability.NotImplemented,
            ["firstboot"] = StageAvailability.Guided,
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

    /// <summary>
    /// Whether the installer has an implementation that was written for this phone.
    /// This is a second axis than <see cref="Resolve"/>: a stage can be
    /// <c>Guided</c> (the installer can promise it, with the user doing part of it)
    /// while the code behind it is still the reference phone's automation, which must
    /// not run against a different device. Stages absent from a phone's list are
    /// automated; the list is the exception list, so the reference flow is untouched.
    /// </summary>
    private static Dictionary<string, IReadOnlyList<string>> NotAutomated { get; } = new()
    {
        ["beyond0lte"] = ["twrp", "transfer", "firstboot", "uefi"],
    };

    public static bool AutomatedFor(string id, string? codename)
    {
        if (codename is null || !NotAutomated.TryGetValue(codename.ToLowerInvariant(), out var blocked))
        {
            return true;
        }
        return !blocked.Contains(id);
    }

    /// <summary>
    /// What the user has to do instead, and why. Every fact in these texts is a read:
    /// the S10e's official TWRP build and Team Win's own Verified-Boot warning come
    /// from the device page (notes/m9-install-attempt.md §15), the partition names
    /// from the live GPT (notes/s10e-root-reads.md §2).
    /// </summary>
    public static string ManualProcedure(string id, string? codename)
    {
        var key = codename?.ToLowerInvariant();
        if (key is null || !NotAutomated.TryGetValue(key, out var blocked) || !blocked.Contains(id))
        {
            return "This step is not automated for this phone and nothing was written.";
        }
        return id switch
        {
            "twrp" => "Flashing TWRP is not automated for the Galaxy S10e. Boot it yourself: the current official image is "
                + "twrp-3.7.0_9-2-beyond0lte.img (https://twrp.me/samsung/samsunggalaxys10e.html). Team Win's own page warns "
                + "that a stock device is protected by Android Verified Boot: flashing TWRP without first flashing a vbmeta "
                + "image with verity disabled leaves the device unable to boot. Once TWRP is up, press Resume - the "
                + "installer reads the TWRP version off the phone and accepts the step when it answers.",
            "transfer" => "Copying Windows to the phone is not automated for the Galaxy S10e yet: the writes go to USERDATA "
                + "(sda31, LBA 2151168..31234815) and the ESP is CACHE (sda28), which is a different role split from the "
                + "reference phone's. Nothing has been written to this phone.",
            "firstboot" => "The first Windows boot on a Galaxy S10e has never been observed. Until it is, this step is "
                + "yours: what the screen, the disk and the CPU must show is the thing being established.",
            "uefi" => "No flashable UEFI image exists for the Galaxy S10e: the platform builds (BEYOND0LTE_UEFI.fd) but the "
                + "packaging pipeline that turns an FD into a boot image sboot accepts is not present in this checkout, so "
                + "this step cannot be completed yet.",
            _ => "This step is not automated for this phone and nothing was written.",
        };
    }
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
