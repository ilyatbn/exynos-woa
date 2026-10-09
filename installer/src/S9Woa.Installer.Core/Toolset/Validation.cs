// SPDX-License-Identifier: BSD-2-Clause-Patent
using System.Text;

namespace S9Woa.Installer.Core.Toolset;

/// <summary>Checks that a file is an Android boot image that fits a phone partition.</summary>
public static class BootImage
{
    public const long RecoveryPartitionBytes = 16_512L * 4096; // RECOVERY on beyond0lte (66,048 KiB) — the SMALLER of
                                                               // the two layouts read from the vendor PITs (star2lte is 16,638 x 4096),
                                                               // so an image admitted here fits on both phones.
    public const long BootPartitionBytes = 14_080L * 4096;     // BOOT is 14,080 x 4096 on BOTH star2lte and beyond0lte.

    private static readonly byte[] Magic = "ANDROID!"u8.ToArray();

    /// <summary>Returns null when valid, otherwise a user-facing reason.</summary>
    public static string? Validate(string file, long maxBytes)
    {
        var info = new FileInfo(file);
        if (!info.Exists)
        {
            return "The file does not exist.";
        }
        if (info.Length < 4096)
        {
            return "The file is too small to be a boot image.";
        }
        if (info.Length > maxBytes)
        {
            return $"The image ({info.Length / (1024 * 1024)} MiB) does not fit the {maxBytes / (1024 * 1024)} MiB partition.";
        }
        Span<byte> head = stackalloc byte[8];
        using (var stream = File.OpenRead(file))
        {
            if (stream.Read(head) != head.Length || !head.SequenceEqual(Magic))
            {
                return "This is not an Android boot image (missing the ANDROID! header).";
            }
        }
        return null;
    }

    /// <summary>TWRP must be the build for the phone being installed (star2lte = S9+, beyond0lte = S10e);
    /// official files end in -star2lte.img or -beyond0lte.img.</summary>
    public static string? ValidateTwrp(string file)
    {
        var name = Path.GetFileName(file);
        if (!name.Contains("star2lte", StringComparison.OrdinalIgnoreCase)
            && !name.Contains("beyond0lte", StringComparison.OrdinalIgnoreCase))
        {
            return "This does not look like TWRP for the Galaxy S9+ (Exynos) or the Galaxy S10e (Exynos). "
                + "Official file names end in -star2lte.img or -beyond0lte.img "
                + "(not starlte, which is the S9, or star2qlte, which is the Snapdragon model).";
        }
        return Validate(file, RecoveryPartitionBytes);
    }

    public static string? ValidateUefi(string file) => Validate(file, BootPartitionBytes);

    internal static string Describe(string file) =>
        new StringBuilder(Path.GetFileName(file)).Append(" (").Append(new FileInfo(file).Length / 1024).Append(" KiB)").ToString();
}

/// <summary>
/// Detects whether the Samsung Download-mode USB interface (VID 04E8, PID 685D)
/// is bound to WinUSB/libusbK, which Heimdall needs. The device only appears
/// after the phone has been connected in Download mode at least once.
/// </summary>
public static class DownloadModeDriver
{
    public const string HardwarePrefix = "VID_04E8&PID_685D";
    private const string EnumUsb = @"SYSTEM\CurrentControlSet\Enum\USB";
    private static readonly string[] UsableServices = ["WinUSB", "libusbK", "libusb0"];

    public static ToolStatus Detect(IRegistryReader registry)
    {
        var seen = false;
        foreach (var device in registry.SubKeyNames(EnumUsb)
                     .Where(n => n.StartsWith(HardwarePrefix, StringComparison.OrdinalIgnoreCase)))
        {
            foreach (var instance in registry.SubKeyNames($@"{EnumUsb}\{device}"))
            {
                seen = true;
                var service = registry.GetString($@"{EnumUsb}\{device}\{instance}", "Service");
                if (service is not null && UsableServices.Contains(service, StringComparer.OrdinalIgnoreCase))
                {
                    return new ToolStatus(ToolState.Ready, $"Download mode uses {service}.");
                }
            }
        }
        return new ToolStatus(ToolState.Deferred, seen
            ? "The phone's Download-mode interface still uses the Samsung driver. Put the phone in Download mode, open Zadig, "
              + "choose Options > List All Devices, select the Samsung device, pick WinUSB and click Replace Driver."
            : "Done during Install TWRP: with the phone in Download mode, open Zadig and replace its driver with WinUSB.");
    }
}
