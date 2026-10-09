// SPDX-License-Identifier: BSD-2-Clause-Patent
using System.Security.Cryptography;
using System.Xml.Linq;
using S9Woa.Installer.Core.Deploy;
using S9Woa.Installer.Core.Image;
using S9Woa.Installer.Core.Processes;

namespace S9Woa.Installer.Core.Tests;

public class OobeAndExportTests
{
    private static readonly XNamespace Ns = "urn:schemas-microsoft-com:unattend";

    [Fact]
    public void UnattendCreatesLocalAdminAndSkipsOobe()
    {
        var xml = UnattendXml.Build(new UnattendOptions { Username = "Alex", Locale = "en-GB", TimeZone = "GMT Standard Time" });
        var doc = XDocument.Parse(xml);

        var settings = doc.Root!.Elements(Ns + "settings").Single(s => s.Attribute("pass")!.Value == "oobeSystem");
        Assert.All(doc.Root!.Descendants(Ns + "component").Attributes("processorArchitecture"), a => Assert.Equal("arm64", a.Value));

        var shell = settings.Elements(Ns + "component").Single(c => c.Attribute("name")!.Value == "Microsoft-Windows-Shell-Setup");

        var account = shell.Descendants(Ns + "LocalAccount").Single();
        Assert.Equal("Alex", account.Element(Ns + "Name")!.Value);
        Assert.Equal("Administrators", account.Element(Ns + "Group")!.Value);

        var oobe = shell.Element(Ns + "OOBE")!;
        Assert.Equal("true", oobe.Element(Ns + "HideOnlineAccountScreens")!.Value);
        Assert.Equal("true", oobe.Element(Ns + "HideEULAPage")!.Value);

        var intl = settings.Elements(Ns + "component").Single(c => c.Attribute("name")!.Value == "Microsoft-Windows-International-Core");
        Assert.Equal("en-GB", intl.Element(Ns + "UILanguage")!.Value);
        Assert.Equal(@"Windows\Panther\unattend.xml", UnattendXml.RelativePath);
    }

    [Fact]
    public void UnattendRejectsEmptyUsername() =>
        Assert.Throws<ArgumentException>(() => UnattendXml.Build(new UnattendOptions { Username = " " }));

    [Fact]
    public void UnattendSetsDisplayScalingBeforeTheAccountIsCreated()
    {
        var doc = XDocument.Parse(UnattendXml.Build(new UnattendOptions()));
        Assert.Equal(["specialize", "oobeSystem"], doc.Root!.Elements(Ns + "settings").Select(s => s.Attribute("pass")!.Value));

        var specialize = doc.Root!.Elements(Ns + "settings").First();
        var deployment = specialize.Elements(Ns + "component").Single();
        Assert.Equal("Microsoft-Windows-Deployment", deployment.Attribute("name")!.Value);
        var commands = deployment.Descendants(Ns + "RunSynchronousCommand").ToList();
        Assert.Equal(Enumerable.Range(1, commands.Count).Select(i => i.ToString()), commands.Select(c => c.Element(Ns + "Order")!.Value));
        var paths = commands.Select(c => c.Element(Ns + "Path")!.Value).ToList();
        Assert.All(paths, p => Assert.True(p.Length < 260, p)); // RunSynchronous path limit

        // The default profile (which the OOBE account is copied from) is loaded, set to 275 DPI
        // custom scaling, and unloaded; the sign-in screen's profile gets the same.
        Assert.StartsWith("cmd.exe /c reg.exe load HKU\\S9WoaDefaultUser \"%SystemDrive%\\Users\\Default\\NTUSER.DAT\"", paths[0], StringComparison.Ordinal);
        var unload = paths.FindIndex(p => p.StartsWith("reg.exe unload HKU\\S9WoaDefaultUser", StringComparison.Ordinal));
        foreach (var key in new[] { @"HKU\S9WoaDefaultUser", @"HKU\.DEFAULT" })
        {
            var logPixels = paths.FindIndex(p => p == $"reg.exe add \"{key}\\Control Panel\\Desktop\" /v LogPixels /t REG_DWORD /d 275 /f");
            var scaling = paths.FindIndex(p => p == $"reg.exe add \"{key}\\Control Panel\\Desktop\" /v Win8DpiScaling /t REG_DWORD /d 1 /f");
            Assert.True(logPixels > 0 && scaling > 0, key);
            Assert.Equal(key.EndsWith("DefaultUser", StringComparison.Ordinal), logPixels < unload && scaling < unload);
        }

        // No scaling requested: no specialize pass at all; out-of-range values are refused.
        var none = XDocument.Parse(UnattendXml.Build(new UnattendOptions { Dpi = null }));
        Assert.Equal(["oobeSystem"], none.Root!.Elements(Ns + "settings").Select(s => s.Attribute("pass")!.Value));
        Assert.Throws<ArgumentOutOfRangeException>(() => UnattendXml.Build(new UnattendOptions { Dpi = 72 }));
        Assert.Throws<ArgumentOutOfRangeException>(() => UnattendXml.Build(new UnattendOptions { Dpi = 1000 }));
    }

    [Fact]
    public void UnattendFileBytesMatchTheirDeclaredEncoding()
    {
        // Windows Setup reads the file's bytes, honouring the XML declaration. A "utf-16"
        // declaration over UTF-8 bytes stopped OOBE on the phone with "Windows Setup encountered
        // an internal error while loading or searching for an unattend answer file".
        var bytes = UnattendXml.BuildBytes(new UnattendOptions { Username = "Zoë", Password = "p<&>w" });
        Assert.StartsWith("<?xml version=\"1.0\" encoding=\"utf-8\"?>", System.Text.Encoding.UTF8.GetString(bytes), StringComparison.Ordinal);
        Assert.NotEqual(0xEF, bytes[0]); // no byte order mark

        using var stream = new MemoryStream(bytes);
        var strict = new System.Xml.XmlDocument();
        strict.Load(stream); // throws on an encoding mismatch
        var ns = new System.Xml.XmlNamespaceManager(strict.NameTable);
        ns.AddNamespace("u", Ns.NamespaceName);
        Assert.Equal("Zoë", strict.SelectSingleNode("//u:LocalAccount/u:Name", ns)!.InnerText);
        Assert.Equal("p<&>w", strict.SelectSingleNode("//u:AutoLogon/u:Password/u:Value", ns)!.InnerText);
    }

    /// <summary>An in-memory disk source over a fixed byte buffer.</summary>
    private sealed class MemoryDiskSource : IRawDiskSource, IDisposable
    {
        private readonly byte[] _data;
        public MemoryDiskSource(byte[] data) => _data = data;
        public long Length => _data.Length;
        public Stream OpenRead(long offset) => new MemoryStream(_data, (int)offset, _data.Length - (int)offset, writable: false);
        public void Dispose() { }
    }

    [Fact]
    public async Task ExporterCopiesExtentAndPadsToMib()
    {
        var disk = new byte[3 * 1024 * 1024 + 500];
        new Random(11).NextBytes(disk);
        var dst = Path.GetTempFileName();
        try
        {
            var extent = new PartitionExtent(1024 * 1024, 2 * 1024 * 1024 + 100);
            var (bytes, sha) = await new RawImageExporter().ExportAsync(new MemoryDiskSource(disk), extent, dst);

            Assert.Equal(3 * 1024 * 1024, bytes); // rounded up to whole MiB
            var written = await File.ReadAllBytesAsync(dst);
            Assert.Equal(bytes, written.Length);
            Assert.Equal(disk.AsSpan((int)extent.Offset, (int)extent.Length).ToArray(), written.AsSpan(0, (int)extent.Length).ToArray());
            Assert.All(written.AsSpan((int)extent.Length).ToArray(), b => Assert.Equal(0, b));
            Assert.Equal(Convert.ToHexString(SHA256.HashData(written)).ToLowerInvariant(), sha);
        }
        finally
        {
            File.Delete(dst);
        }
    }

    [Fact]
    public async Task ExporterRejectsOutOfRangeExtent()
    {
        var dst = Path.GetTempFileName();
        try
        {
            await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() =>
                new RawImageExporter().ExportAsync(new MemoryDiskSource(new byte[1024]), new PartitionExtent(0, 4096), dst));
        }
        finally
        {
            File.Delete(dst);
        }
    }

    [Theory]
    [InlineData(1, 1048576)]
    [InlineData(1048576, 1048576)]
    [InlineData(1048577, 2097152)]
    public void RoundsUpToMib(long input, long expected) => Assert.Equal(expected, RawImageExporter.RoundUpToMib(input));

    [Fact]
    public void BcdTargetsThePhonePartitionsByGuid()
    {
        var cmds = BootConfiguration.SettingCommands(@"S:\EFI\Microsoft\Boot\BCD");
        var flat = cmds.Select(c => string.Join(' ', c)).ToList();
        Assert.DoesNotContain(flat, c => c.Contains("locate", StringComparison.Ordinal));
        Assert.Contains(flat, c => c.Contains("path \\Windows\\System32\\winload.efi", StringComparison.Ordinal));
        Assert.Contains(flat, c => c.EndsWith("{default} testsigning on", StringComparison.Ordinal));
        Assert.Contains(flat, c => c.EndsWith("{default} numproc 4", StringComparison.Ordinal));
        Assert.Contains(flat, c => c.EndsWith("{default} vsmlaunchtype off", StringComparison.Ordinal));
        Assert.Contains(flat, c => c.EndsWith("{default} hypervisorlaunchtype off", StringComparison.Ordinal));
        Assert.Contains(flat, c => c.EndsWith("{default} bootstatuspolicy IgnoreAllFailures", StringComparison.Ordinal));
        Assert.All(flat, c => Assert.Contains(@"/store S:\EFI\Microsoft\Boot\BCD", c, StringComparison.Ordinal));

        var script = BootConfiguration.DeviceScript(@"S:\EFI\Microsoft\Boot\BCD", PartitionMap.Star2Lte);
        // Boot manager on CACHE; loader device and OS device on USERDATA; both on the Samsung disk.
        Assert.Contains($"DiskSignature = '{PartitionMap.DiskGuid}'; PartitionIdentifier = '{PartitionMap.CacheGuid}'", script, StringComparison.Ordinal);
        Assert.Contains($"DiskSignature = '{PartitionMap.DiskGuid}'; PartitionIdentifier = '{PartitionMap.UserdataGuid}'", script, StringComparison.Ordinal);
        Assert.Contains("[uint32]0x11000001, [uint32]0x21000001", script, StringComparison.Ordinal);
        Assert.Contains("Type = [uint32]0x16000060; Boolean = $false", script, StringComparison.Ordinal);
        Assert.Contains("Type = [uint32]0x25000004; Integer = [uint64]0", script, StringComparison.Ordinal);
        Assert.Contains(@"File = 'S:\EFI\Microsoft\Boot\BCD'", script, StringComparison.Ordinal);

        var s10e = BootConfiguration.DeviceScript(@"S:\EFI\Microsoft\Boot\BCD", PartitionMap.Beyond0Lte);
        // The S10e's GPT identifiers are derived from lower-case names (live GPT, notes/s10e-root-reads.md §2).
        Assert.Contains("PartitionIdentifier = '{52444e41-494f-2044-6361-636865000000}'", s10e, StringComparison.Ordinal);
        Assert.Contains("PartitionIdentifier = '{52444e41-494f-2044-7573-657264617461}'", s10e, StringComparison.Ordinal);
        Assert.DoesNotContain(PartitionMap.UserdataGuid, s10e, StringComparison.Ordinal);
    }
}
