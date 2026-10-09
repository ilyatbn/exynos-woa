// SPDX-License-Identifier: BSD-2-Clause-Patent
using System.Buffers.Binary;
using System.Text;

namespace S9Woa.Installer.Core.Deploy;

/// <summary>
/// Minimal GPT editing for the one change the installer makes to the phone's partition table:
/// the type GUID of a named entry. Ranges, names and partition identifiers are never touched;
/// only the entry's type and the two CRCs that depend on it change.
/// </summary>
public static class Gpt
{
    public static readonly Guid EfiSystemType = new("c12a7328-f81f-11d2-ba4b-00a0c93ec93b");

    private const int NameOffset = 56;
    private const int NameBytes = 72;

    /// <summary>Where a GPT header says its entry array is.</summary>
    public readonly record struct Header(long MyLba, long AlternateLba, long EntriesLba, int EntryCount, int EntrySize)
    {
        public int EntrySectors(int sectorSize) => (int)(((long)EntryCount * EntrySize + sectorSize - 1) / sectorSize);
    }

    /// <summary>
    /// Null when the sector does not carry a GPT header at all - "there is no copy
    /// here", which is a different fact from "this copy is corrupt". A sector that
    /// starts with the signature is parsed by <see cref="ReadHeader"/> and its
    /// failure stays an exception.
    /// </summary>
    public static Header? ReadHeaderOrNull(ReadOnlySpan<byte> sector) =>
        sector.Length >= 92 && sector[..8].SequenceEqual("EFI PART"u8) ? ReadHeader(sector) : null;

    /// <summary>Parses and CRC-checks a header sector.</summary>
    public static Header ReadHeader(ReadOnlySpan<byte> sector)
    {
        if (sector.Length < 92 || !sector[..8].SequenceEqual("EFI PART"u8))
        {
            throw new InvalidDataException("No GPT header signature.");
        }        var size = BinaryPrimitives.ReadInt32LittleEndian(sector[12..]);
        if (size < 92 || size > sector.Length)
        {
            throw new InvalidDataException($"Implausible GPT header size {size}.");
        }
        if (HeaderCrc(sector, size) != BinaryPrimitives.ReadUInt32LittleEndian(sector[16..]))
        {
            throw new InvalidDataException("GPT header CRC mismatch.");
        }
        var header = new Header(
            BinaryPrimitives.ReadInt64LittleEndian(sector[24..]),
            BinaryPrimitives.ReadInt64LittleEndian(sector[32..]),
            BinaryPrimitives.ReadInt64LittleEndian(sector[72..]),
            BinaryPrimitives.ReadInt32LittleEndian(sector[80..]),
            BinaryPrimitives.ReadInt32LittleEndian(sector[84..]));
        if (header.EntrySize < 128 || header.EntryCount is < 1 or > 1024)
        {
            throw new InvalidDataException($"Implausible GPT entry array ({header.EntryCount} x {header.EntrySize}).");
        }
        return header;
    }

    /// <summary>
    /// Sets <paramref name="name"/>'s type GUID in a run of whole sectors read from the disk
    /// starting at <paramref name="regionLba"/>. The run must hold the header at
    /// <paramref name="headerLba"/> and its whole entry array; both CRCs are checked before and
    /// rewritten after. Returns false when the type already matches (nothing is changed).
    /// </summary>
    public static bool SetPartitionType(byte[] region, int sectorSize, long regionLba, long headerLba, string name, Guid type)
    {
        var headerAt = checked((int)((headerLba - regionLba) * sectorSize));
        if (headerLba < regionLba || headerAt + sectorSize > region.Length)
        {
            throw new ArgumentException("The GPT header is outside the region.");
        }
        var headerSector = region.AsSpan(headerAt, sectorSize);
        var header = ReadHeader(headerSector);
        if (header.MyLba != headerLba)
        {
            throw new InvalidDataException($"The GPT header at LBA {headerLba} says it is at LBA {header.MyLba}.");
        }
        var entriesAt = checked((int)((header.EntriesLba - regionLba) * sectorSize));
        var entriesLength = header.EntryCount * header.EntrySize;
        if (header.EntriesLba < regionLba || entriesAt + entriesLength > region.Length)
        {
            throw new ArgumentException("The GPT entry array is outside the region.");
        }
        var entries = region.AsSpan(entriesAt, entriesLength);
        if (Crc32(entries) != BinaryPrimitives.ReadUInt32LittleEndian(headerSector[88..]))
        {
            throw new InvalidDataException("GPT entry array CRC mismatch.");
        }

        var matches = new List<int>();
        for (var i = 0; i < header.EntryCount; i++)
        {
            var entry = entries.Slice(i * header.EntrySize, header.EntrySize);
            if (entry[..16].IndexOfAnyExcept((byte)0) >= 0 && string.Equals(EntryName(entry), name, StringComparison.OrdinalIgnoreCase))
            {
                matches.Add(i * header.EntrySize);
            }
        }
        if (matches.Count != 1)
        {
            throw new InvalidDataException($"Expected exactly one GPT entry named {name}, found {matches.Count}.");
        }

        var typeField = entries.Slice(matches[0], 16);
        var wanted = type.ToByteArray();
        if (typeField.SequenceEqual(wanted))
        {
            return false;
        }
        wanted.CopyTo(typeField);
        BinaryPrimitives.WriteUInt32LittleEndian(headerSector[88..], Crc32(entries));
        var size = BinaryPrimitives.ReadInt32LittleEndian(headerSector[12..]);
        BinaryPrimitives.WriteUInt32LittleEndian(headerSector[16..], HeaderCrc(headerSector, size));
        return true;
    }

    /// <summary>The type GUID of a named entry in a region laid out as for <see cref="SetPartitionType"/>.</summary>
    public static Guid PartitionType(byte[] region, int sectorSize, long regionLba, long headerLba, string name)
    {
        var header = ReadHeader(region.AsSpan(checked((int)((headerLba - regionLba) * sectorSize)), sectorSize));
        var entries = region.AsSpan(checked((int)((header.EntriesLba - regionLba) * sectorSize)), header.EntryCount * header.EntrySize);
        for (var i = 0; i < header.EntryCount; i++)
        {
            var entry = entries.Slice(i * header.EntrySize, header.EntrySize);
            if (string.Equals(EntryName(entry), name, StringComparison.OrdinalIgnoreCase))
            {
                return new Guid(entry[..16]);
            }
        }
        throw new InvalidDataException($"No GPT entry named {name}.");
    }

    private static string EntryName(ReadOnlySpan<byte> entry) =>
        Encoding.Unicode.GetString(entry.Slice(NameOffset, NameBytes)).TrimEnd('\0');

    private static uint HeaderCrc(ReadOnlySpan<byte> sector, int size)
    {
        Span<byte> copy = stackalloc byte[size];
        sector[..size].CopyTo(copy);
        copy.Slice(16, 4).Clear();
        return Crc32(copy);
    }

    private static readonly uint[] Table = BuildTable();

    private static uint[] BuildTable()
    {
        var table = new uint[256];
        for (uint n = 0; n < 256; n++)
        {
            var c = n;
            for (var k = 0; k < 8; k++)
            {
                c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[n] = c;
        }
        return table;
    }

    /// <summary>CRC-32 (IEEE 802.3), as used by GPT.</summary>
    public static uint Crc32(ReadOnlySpan<byte> data)
    {
        var c = 0xFFFFFFFFu;
        foreach (var b in data)
        {
            c = Table[(c ^ b) & 0xFF] ^ (c >> 8);
        }
        return c ^ 0xFFFFFFFFu;
    }
}
