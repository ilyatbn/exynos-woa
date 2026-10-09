// SPDX-License-Identifier: BSD-2-Clause-Patent
using System.Globalization;
using System.Security.Cryptography;
using System.Text.RegularExpressions;

namespace S9Woa.Installer.Core.Deploy;

/// <summary>
/// Types a partition as an EFI system partition in both copies of the phone's GPT. The reference
/// phone had SYSTEM (the second copy of the boot files) typed this way; a stock flash resets it to
/// basic data, and Windows finds its system partition by this type. Only the type GUID and the two
/// CRCs that depend on it change. The original sectors are saved on the PC first and written back
/// if the new ones do not read back exactly.
/// </summary>
public sealed partial class GptTypeService
{
    private readonly TwrpClient _twrp;

    public GptTypeService(TwrpClient twrp) => _twrp = twrp;

    [GeneratedRegex(@"^/dev/block/(sd[a-z])[0-9]+$")]
    private static partial Regex PartitionNode();

    /// <summary>Returns true when the partition table was changed.</summary>
    public async Task<bool> EnsureEspTypeAsync(string partition, string backupDirectory, IProgress<string>? log = null, CancellationToken ct = default)
    {
        var name = await _twrp.ResolvePartitionNameAsync(partition, ct).ConfigureAwait(false);
        var node = (await _twrp.ShellAsync($"readlink -f {TwrpClient.ByName}/{name}", ct).ConfigureAwait(false)).Trim();
        var match = PartitionNode().Match(node);
        if (!match.Success)
        {
            throw new InvalidOperationException($"{name} resolves to {node}, not a partition of a UFS unit.");
        }
        var unit = match.Groups[1].Value;
        var disk = $"/dev/block/{unit}";
        var geometry = (await _twrp.ShellAsync($"cat /sys/block/{unit}/queue/logical_block_size /sys/block/{unit}/size", ct).ConfigureAwait(false))
            .Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
        var sectorSize = int.Parse(geometry[0], CultureInfo.InvariantCulture);
        var lastLba = long.Parse(geometry[1], CultureInfo.InvariantCulture) * 512 / sectorSize - 1;

        await _twrp.MakeDirAsync(_twrp.RamStagingDir, ct).ConfigureAwait(false);
        var primary = Gpt.ReadHeader(await ReadAsync(disk, sectorSize, 1, 1, ct).ConfigureAwait(false));
        // The backup copy is optional, not assumed: this disk's header declares
        // AlternateLBA at the last LBA, but that region was read back ALL ZERO on the
        // S10e (notes/s10e-root-reads.md §9), so the media carries no backup table.
        // Failing on that would stop the ESP typing on a phone for a feature its disk
        // does not have.
        var maybeBackup = Gpt.ReadHeaderOrNull(await ReadAsync(disk, sectorSize, lastLba, 1, ct).ConfigureAwait(false));
        var backupPresent = maybeBackup is not null;
        if (!backupPresent)
        {
            log?.Report($"No GPT header at the last sector of {disk}; this media has no backup partition table.");
        }
        Gpt.Header backupHeader = (Gpt.Header)maybeBackup!;
        // Each copy is rewritten as one run: header plus its adjacent entry array.
        if (primary.MyLba != 1 || primary.EntriesLba != 2 || primary.AlternateLba != lastLba
            || (backupPresent && (backupHeader.MyLba != lastLba
                || backupHeader.EntriesLba + backupHeader.EntrySectors(sectorSize) != lastLba)))
        {
            throw new InvalidDataException($"{disk} does not have the expected GPT layout (header and entries adjacent, backup at the last sector when there is one); not editing it.");
        }

        var stamp = DateTime.Now.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture);
        var changed = false;
        var copies = new List<(string, long, int, long)>
        {
            ("primary", 1L, 1 + primary.EntrySectors(sectorSize), 1L),
        };
        if (backupPresent)
        {
            copies.Add(("backup", backupHeader.EntriesLba, backupHeader.EntrySectors(sectorSize) + 1, lastLba));
        }
        foreach (var (copy, start, count, headerLba) in copies)
        {
            var region = await ReadAsync(disk, sectorSize, start, count, ct).ConfigureAwait(false);
            var original = (byte[])region.Clone();
            if (!Gpt.SetPartitionType(region, sectorSize, start, headerLba, name, Gpt.EfiSystemType))
            {
                continue;
            }
            Directory.CreateDirectory(backupDirectory);
            await File.WriteAllBytesAsync(Path.Combine(backupDirectory, $"gpt-{copy}-{unit}-lba{start}-{stamp}.bin"), original, ct).ConfigureAwait(false);
            log?.Report($"Marking {name} as an EFI system partition in the {copy} partition table...");
            if (!await WriteVerifiedAsync(disk, sectorSize, start, region, ct).ConfigureAwait(false))
            {
                var restored = await WriteVerifiedAsync(disk, sectorSize, start, original, ct).ConfigureAwait(false);
                throw new InvalidOperationException($"The {copy} partition table did not read back as written; "
                    + (restored ? "the original was put back." : $"putting the original back also failed. A copy is in {backupDirectory}."));
            }
            changed = true;
        }
        return changed;
    }

    private string Remote(string file) => $"{_twrp.RamStagingDir}/{file}";

    private async Task<byte[]> ReadAsync(string disk, int sectorSize, long lba, int count, CancellationToken ct)
    {
        var host = Path.Combine(Path.GetTempPath(), $"s9woa-gpt-{Guid.NewGuid():N}.bin");
        try
        {
            await _twrp.ShellAsync($"dd if={disk} of={Remote("gpt-read.bin")} bs={sectorSize} skip={lba} count={count} 2>/dev/null", ct).ConfigureAwait(false);
            await _twrp.PullAsync(Remote("gpt-read.bin"), host, ct).ConfigureAwait(false);
            var bytes = await File.ReadAllBytesAsync(host, ct).ConfigureAwait(false);
            if (bytes.Length != (long)sectorSize * count)
            {
                throw new InvalidOperationException($"Read {bytes.Length} bytes of {disk} at LBA {lba}, expected {sectorSize * count}.");
            }
            return bytes;
        }
        finally
        {
            File.Delete(host);
            await _twrp.RemoveAsync(Remote("gpt-read.bin"), CancellationToken.None).ConfigureAwait(false);
        }
    }

    private async Task<bool> WriteVerifiedAsync(string disk, int sectorSize, long lba, byte[] data, CancellationToken ct)
    {
        var host = Path.Combine(Path.GetTempPath(), $"s9woa-gpt-{Guid.NewGuid():N}.bin");
        var count = data.Length / sectorSize;
        try
        {
            await File.WriteAllBytesAsync(host, data, ct).ConfigureAwait(false);
            await _twrp.PushAsync(host, Remote("gpt-write.bin"), ct).ConfigureAwait(false);
            await _twrp.ShellAsync($"dd if={Remote("gpt-write.bin")} of={disk} bs={sectorSize} seek={lba} count={count} conv=notrunc,fsync 2>/dev/null", ct).ConfigureAwait(false);
            await _twrp.ShellAsync($"blockdev --flushbufs {disk}", ct).ConfigureAwait(false);
            var sum = TwrpClient.ParseSha256(await _twrp.ShellAsync($"dd if={disk} bs={sectorSize} skip={lba} count={count} 2>/dev/null | sha256sum", ct).ConfigureAwait(false));
            return sum == Convert.ToHexString(SHA256.HashData(data)).ToLowerInvariant();
        }
        finally
        {
            File.Delete(host);
            await _twrp.RemoveAsync(Remote("gpt-write.bin"), CancellationToken.None).ConfigureAwait(false);
        }
    }
}
