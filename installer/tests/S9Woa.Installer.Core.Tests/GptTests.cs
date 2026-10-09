// SPDX-License-Identifier: BSD-2-Clause-Patent
using System.Buffers.Binary;
using System.Text;
using S9Woa.Installer.Core.Deploy;

namespace S9Woa.Installer.Core.Tests;

public class GptTests
{
    private const int Sector = 4096;
    private static readonly Guid BasicData = new("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7");

    /// <summary>A primary GPT run (header at LBA 1, 128 x 128-byte entries at LBA 2..5) like the phone's.</summary>
    private static byte[] PrimaryRun(params (string Name, Guid Type)[] parts)
    {
        var region = new byte[Sector * 5];
        var entries = region.AsSpan(Sector, 128 * 128);
        for (var i = 0; i < parts.Length; i++)
        {
            var e = entries.Slice(i * 128, 128);
            parts[i].Type.ToByteArray().CopyTo(e);
            Guid.NewGuid().ToByteArray().CopyTo(e[16..]);
            BinaryPrimitives.WriteInt64LittleEndian(e[32..], 100 + i * 10);
            BinaryPrimitives.WriteInt64LittleEndian(e[40..], 109 + i * 10);
            Encoding.Unicode.GetBytes(parts[i].Name).CopyTo(e[56..]);
        }
        var h = region.AsSpan(0, Sector);
        "EFI PART"u8.CopyTo(h);
        BinaryPrimitives.WriteUInt32LittleEndian(h[8..], 0x00010000);
        BinaryPrimitives.WriteInt32LittleEndian(h[12..], 92);
        BinaryPrimitives.WriteInt64LittleEndian(h[24..], 1);
        BinaryPrimitives.WriteInt64LittleEndian(h[32..], 15_615_999);
        BinaryPrimitives.WriteInt64LittleEndian(h[72..], 2);
        BinaryPrimitives.WriteInt32LittleEndian(h[80..], 128);
        BinaryPrimitives.WriteInt32LittleEndian(h[84..], 128);
        BinaryPrimitives.WriteUInt32LittleEndian(h[88..], Gpt.Crc32(entries));
        BinaryPrimitives.WriteUInt32LittleEndian(h[16..], Gpt.Crc32(h[..92]));
        return region;
    }

    [Fact]
    public void Crc32MatchesTheStandardCheckValue() =>
        Assert.Equal(0xCBF43926u, Gpt.Crc32("123456789"u8));

    [Fact]
    public void RetypesOnlyTheNamedEntryAndRefreshesBothCrcs()
    {
        var region = PrimaryRun(("BOOT", BasicData), ("SYSTEM", BasicData), ("CACHE", BasicData));
        var before = (byte[])region.Clone();

        Assert.True(Gpt.SetPartitionType(region, Sector, 1, 1, "system", Gpt.EfiSystemType));

        Assert.Equal(Gpt.EfiSystemType, Gpt.PartitionType(region, Sector, 1, 1, "SYSTEM"));
        Assert.Equal(BasicData, Gpt.PartitionType(region, Sector, 1, 1, "CACHE"));
        var header = Gpt.ReadHeader(region.AsSpan(0, Sector));
        Assert.Equal(2, header.EntriesLba);
        var changed = Enumerable.Range(0, region.Length).Where(i => region[i] != before[i]).ToList();
        Assert.All(changed, i => Assert.True(i is >= 16 and < 20 or >= 88 and < 92 || i - Sector is >= 128 and < 144, $"byte {i} changed"));

        Assert.False(Gpt.SetPartitionType(region, Sector, 1, 1, "SYSTEM", Gpt.EfiSystemType));
    }

    [Fact]
    public void WorksOnTheBackupCopyWhereEntriesPrecedeTheHeader()
    {
        var primary = PrimaryRun(("SYSTEM", BasicData));
        var backup = new byte[primary.Length];
        primary.AsSpan(Sector).CopyTo(backup);
        var h = backup.AsSpan(4 * Sector, Sector);
        primary.AsSpan(0, Sector).CopyTo(h);
        BinaryPrimitives.WriteInt64LittleEndian(h[24..], 15_615_999);
        BinaryPrimitives.WriteInt64LittleEndian(h[32..], 1);
        BinaryPrimitives.WriteInt64LittleEndian(h[72..], 15_615_995);
        h.Slice(16, 4).Clear();
        BinaryPrimitives.WriteUInt32LittleEndian(h[16..], Gpt.Crc32(h[..92]));

        Assert.True(Gpt.SetPartitionType(backup, Sector, 15_615_995, 15_615_999, "SYSTEM", Gpt.EfiSystemType));
        Assert.Equal(Gpt.EfiSystemType, Gpt.PartitionType(backup, Sector, 15_615_995, 15_615_999, "SYSTEM"));
    }

    [Fact]
    public void RefusesCorruptTablesAndUnknownNames()
    {
        var region = PrimaryRun(("SYSTEM", BasicData));
        Assert.Throws<InvalidDataException>(() => Gpt.SetPartitionType(region, Sector, 1, 1, "USERDATA", Gpt.EfiSystemType));

        region[Sector + 200] ^= 1;
        Assert.Throws<InvalidDataException>(() => Gpt.SetPartitionType(region, Sector, 1, 1, "SYSTEM", Gpt.EfiSystemType));

        var badHeader = PrimaryRun(("SYSTEM", BasicData));
        badHeader[40] ^= 1;
        Assert.Throws<InvalidDataException>(() => Gpt.ReadHeader(badHeader.AsSpan(0, Sector)));
    }

    [Fact]
    public void ASectorWithoutAGptHeaderIsNoCopyAtAll()
    {
        Assert.Null(Gpt.ReadHeaderOrNull(new byte[Sector]));
        var good = PrimaryRun(("SYSTEM", BasicData));
        Assert.True(Gpt.ReadHeaderOrNull(good.AsSpan(0, Sector)) is not null);
        Assert.Equal(1L, Gpt.ReadHeader(good.AsSpan(0, Sector)).MyLba);
    }
}
