// SPDX-License-Identifier: BSD-2-Clause-Patent
namespace S9Woa.Installer.Core.Deploy;

/// <summary>
/// The validated Galaxy S9+ (star2lte) partition layout the installer relies on.
/// Names are the stable <c>/dev/block/by-name</c> links (upper case on this phone;
/// lookups are case-insensitive), independent of the sd* node numbering.
/// </summary>
public static class PartitionMap
{
    /// <summary>
    /// Identity- and calibration-critical partitions to back up before anything is
    /// written. <c>EFS</c> holds the IMEI, Wi-Fi/Bluetooth MACs and serial on Exynos
    /// and is irreplaceable; the rest are backed up when present. Missing names are skipped.
    /// </summary>
    public static IReadOnlyList<string> IdentityBackup { get; } =
    [
        "EFS", "CPEFS", "PARAM", "UP_PARAM", "STEADY", "KEYSTORAGE", "PERSISTENT",
    ];

    /// <summary>A backup is only trustworthy if at least these partitions were captured.</summary>
    public static IReadOnlyList<string> RequiredBackup { get; } = ["EFS"];

    /// <summary>Partition that holds the UEFI boot image. RECOVERY keeps TWRP.</summary>
    public const string UefiTarget = "BOOT";

    /// <summary>Partition Windows is written to on this device.</summary>
    public const string WindowsTarget = "USERDATA";

    /// <summary>Partition TWRP is flashed to from Download mode.</summary>
    public const string RecoveryTarget = "RECOVERY";

    /// <summary>
    /// The EFI system partition the UEFI boots from: Android's CACHE (sda21, 600 MiB). It is
    /// reformatted as FAT32 with 4096-byte sectors (the UFS block size) and holds
    /// \EFI\Microsoft\Boot\bootmgfw.efi and the BCD, whose boot-manager device points here.
    /// </summary>
    public const string EfiSystemPartition = "CACHE";

    /// <summary>
    /// Second FAT32 copy of the boot files on Android's SYSTEM partition (sda18). The reference
    /// deployment kept both; firmware builds that only connect the SYSTEM-sized FAT volume use it.
    /// On the reference phone its GPT type is the EFI system partition GUID (a stock flash resets it
    /// to basic data); <see cref="GptTypeService"/> puts that back.
    /// </summary>
    public const string SecondaryEfiSystemPartition = "SYSTEM";

    /// <summary>
    /// Holds the Android bootloader control block. <c>boot-recovery</c> there makes the bootloader
    /// start RECOVERY; the UEFI writes it too when it gives up on Windows.
    /// </summary>
    public const string Misc = "MISC";

    /// <summary>
    /// Samsung derives GPT identifiers from names ("ANDROID MMC DISK", "ANDROID USERDATA", ...),
    /// so they are the same on every star2lte. The BCD addresses Windows by them.
    /// CONFIRMED on the S10e from its own live GPT: the disk signature is the same string, but
    /// the per-partition names there are LOWER CASE ("ANDROID userdata"), so the beyond0lte
    /// GUIDs below are different values — see notes/s10e-root-reads.md §2.
    /// </summary>
    public const string DiskGuid = "{52444e41-494f-2044-4d4d-43204449534b}";
    public const string UserdataGuid = "{52444e41-494f-2044-5553-455244415441}";
    public const string CacheGuid = "{52444e41-494f-2044-4341-434845000000}";
    public const string SystemGuid = "{52444e41-494f-2044-5359-5354454d0000}";

    /// <summary>
    /// Byte geometry of the phone's main UFS unit and of USERDATA on it. The Windows image is
    /// built on a virtual disk with exactly this layout so the NTFS volume matches USERDATA.
    /// </summary>
    public static readonly GeometryType Star2Lte = new(63_963_136_000, 6_951_534_592, 57_004_785_664,
        629_145_600, 57_671_680, 68_148_992, DiskGuid, CacheGuid, UserdataGuid);

    /// <summary>
    /// Beyond0lte (SM-G970F) geometry, from Samsung's own BEYOND0LTE_EUR_OPEN.pit cross-checked
    /// against the phone, and confirmed against the live GPT read in TWRP (notes/s10e-root-reads.md
    /// §2): USERDATA is LBA 2 151 168 .. 31 234 815, i.e. 119 126 642 688 bytes of PARTITION
    /// (the 119 126 622 208 in /proc/partitions is the filesystem, 5 LBAs of tail shorter).
    /// The per-partition GUIDs there are derived from LOWER-CASE names. Never validated by an
    /// install.
    /// </summary>
    public static readonly GeometryType Beyond0Lte = new(127_943_049_216, 8_811_184_128, 119_126_642_688,
        629_145_600, 57_671_680, 67_633_152, DiskGuid,
        "{52444e41-494f-2044-6361-636865000000}",   // "ANDROID cache"
        "{52444e41-494f-2044-7573-657264617461}");  // "ANDROID userdata"

    /// <summary>True only for a codename whose geometry this class actually carries. The
    /// installer must not fall back to a phone's layout for an unidentified device: a wrong
    /// geometry silently builds a Windows volume for the wrong disk and lets the partition
    /// stage verify against the wrong sizes.</summary>
    public static bool IsKnown(string? codename) =>
        codename?.ToLowerInvariant() is "star2lte" or "beyond0lte";

    public static GeometryType For(string? codename) =>
        (codename?.ToLowerInvariant() switch
        {
            "star2lte" => Star2Lte,
            "beyond0lte" => Beyond0Lte,
            _ => throw new ArgumentOutOfRangeException(nameof(codename),
                $"No geometry is recorded for {(codename is null ? "an unidentified phone" : codename)}; identify the phone before any layout-dependent step."),
        });
}

/// <summary>Byte geometry and the GPT identifiers of one supported phone; see <see cref="PartitionMap.For"/>.</summary>
public sealed record GeometryType(long DiskBytes, long WindowsOffset, long WindowsBytes,
    long CacheBytes, long BootBytes, long RecoveryBytes,
    string DiskGuid, string EfiPartitionGuid, string WindowsPartitionGuid);
