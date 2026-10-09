// SPDX-License-Identifier: BSD-2-Clause-Patent
namespace S9Woa.Installer.Core.Deploy;

/// <summary>
/// Puts the Windows boot files on the phone. The UEFI boots \EFI\Microsoft\Boot\bootmgfw.efi
/// from CACHE (<see cref="PartitionMap.EfiSystemPartition"/>); a second copy goes to the
/// geometry's secondary ESP slot, which exists only on the reference phone — see
/// <see cref="GeometryType"/>'s <c>SecondaryEspPartition</c>, null for beyond0lte. On a
/// stock phone each holds an Android file system, so each is reformatted as FAT32 with
/// 4096-byte sectors (the UFS block size) and one sector per cluster, which keeps even
/// the 600 MiB CACHE above FAT32's minimum cluster count. The ESP tree itself comes from
/// the image build (bcdboot output with the BCD pointed at the phone's partitions).
/// </summary>
public sealed class BootFilesService
{
    private const string Mount = "/s9woa_esp";
    private readonly TwrpClient _twrp;
    private readonly GeometryType _geometry;

    public BootFilesService(TwrpClient twrp, GeometryType geometry)
    {
        _twrp = twrp;
        _geometry = geometry;
    }

    /// <summary>Formatter command for FAT32 with 4 KiB sectors and 4 KiB clusters, by tool family.</summary>
    internal static string FormatCommand(string tool, string device, string label) =>
        Path.GetFileName(tool) == "newfs_msdos"
            ? $"{tool} -F 32 -S 4096 -c 1 -L {label} {device}"
            : $"{tool} -F 32 -S 4096 -s 1 -n {label} {device}";

    /// <summary>Mount points TWRP may hold on these partitions.</summary>
    private static string[] MountsOf(string partition) => partition.Equals(PartitionMap.EfiSystemPartition, StringComparison.OrdinalIgnoreCase)
        ? ["/cache", Mount]
        : ["/system_root", "/system", Mount];

    public async Task WriteAsync(string espDirectory, IProgress<string>? log = null, CancellationToken ct = default)
    {
        var efi = Path.Combine(espDirectory, "EFI");
        if (!File.Exists(Path.Combine(efi, "Microsoft", "Boot", "bootmgfw.efi")) || !File.Exists(Path.Combine(efi, "Microsoft", "Boot", "BCD")))
        {
            throw new DirectoryNotFoundException($"No complete EFI\\ tree under {espDirectory}. Build the Windows image first.");
        }
        // Rides on the first shell command below (the partition lookup); no extra round trip.
        _twrp.QueueInstallStatus(WinReStatus.BootFiles());
        var targets = new List<string> { PartitionMap.EfiSystemPartition };
        if (_geometry.SecondaryEspPartition is string secondary)
        {
            targets.Add(secondary);
        }
        foreach (var partition in targets)
        {
            await WriteToAsync(partition, efi, log, ct).ConfigureAwait(false);
        }
        log?.Report("Boot files written.");
    }

    private async Task WriteToAsync(string partition, string efi, IProgress<string>? log, CancellationToken ct)
    {
        var name = await _twrp.ResolvePartitionNameAsync(partition, ct).ConfigureAwait(false);
        var device = $"{TwrpClient.ByName}/{name}";
        foreach (var mp in MountsOf(partition))
        {
            if (await _twrp.IsMountedAsync(mp, ct).ConfigureAwait(false))
            {
                await _twrp.UnmountAsync(mp, ct).ConfigureAwait(false);
            }
        }

        if (!await _twrp.IsFat32Async(name, ct).ConfigureAwait(false))
        {
            var tool = await _twrp.FindFatFormatterAsync(ct).ConfigureAwait(false)
                ?? throw new InvalidOperationException("TWRP has no FAT formatter (mkfs.fat/newfs_msdos); cannot create the EFI system partition.");
            log?.Report($"Formatting {name} as a FAT32 EFI system partition (replaces Android's {name.ToLowerInvariant()} partition)...");
            await _twrp.ShellAsync(FormatCommand(tool, device, partition == PartitionMap.EfiSystemPartition ? "ESP" : "SYSTEM"), ct)
                .ConfigureAwait(false);
            if (!await _twrp.IsFat32Async(name, ct).ConfigureAwait(false))
            {
                throw new InvalidOperationException($"{name} did not come out as FAT32 after formatting.");
            }
        }

        await _twrp.MountVfatAsync(device, Mount, ct).ConfigureAwait(false);
        try
        {
            log?.Report($"Copying boot files to {name}...");
            // A previous run's BCD transaction logs must not be replayed over the new store.
            await _twrp.ShellAsync($"rm -rf {Mount}/EFI", ct).ConfigureAwait(false);
            await _twrp.PushTreeAsync(efi, $"{Mount}/", ct).ConfigureAwait(false);
            await _twrp.ShellAsync($"test -f {Mount}/EFI/Microsoft/Boot/bootmgfw.efi && test -f {Mount}/EFI/Microsoft/Boot/BCD && sync", ct)
                .ConfigureAwait(false);
        }
        finally
        {
            await _twrp.UnmountAsync(Mount, ct).ConfigureAwait(false);
        }
    }
}