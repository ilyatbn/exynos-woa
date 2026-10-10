// SPDX-License-Identifier: BSD-2-Clause-Patent
using S9Woa.Installer.Core.Deploy;
using S9Woa.Installer.Core.Image;
using S9Woa.Installer.Core.Processes;

namespace S9Woa.Installer.Core.Tests;

public class ImageBuilderTests
{
    private sealed class RecordingRunner : IProcessRunner
    {
        public List<string> Calls { get; } = [];
        public Func<IReadOnlyList<string>, string>? StdOut { get; init; }
        public Task<ProcessResult> RunAsync(string fileName, IReadOnlyList<string> arguments, TimeSpan timeout,
            CancellationToken cancellationToken = default)
        {
            Calls.Add($"{Path.GetFileName(fileName)} {string.Join(' ', arguments)}");
            return Task.FromResult(new ProcessResult(0, StdOut?.Invoke(arguments) ?? "", ""));
        }
    }

    [Fact]
    public void ParsesEditionsAndChooses()
    {
        const string output =
            "Index : 1\r\nName : Windows 11 Home\r\nDescription : Windows 11 Home\r\nSize : 1\r\n\r\n" +
            "Index : 6\r\nName : Windows 11 Pro\r\nDescription : Windows 11 Pro\r\nSize : 1\r\n";
        var editions = WindowsMedia.ParseImageInfo(output);
        Assert.Equal(2, editions.Count);
        Assert.Equal(6, editions[1].Index);
        Assert.Equal("Windows 11 Pro", editions[1].Name);
        Assert.Equal(6, WindowsMedia.ChooseEdition(editions).Index);
        Assert.Equal(1, WindowsMedia.ChooseEdition([editions[0]]).Index);
        Assert.Throws<InvalidOperationException>(() => WindowsMedia.ChooseEdition([]));

        // The validated IoT Enterprise 23H2 media: Enterprise at 1, IoT Enterprise at 2.
        var iot = WindowsMedia.ParseImageInfo(
            "Index : 1\r\nName : Windows 11 Enterprise\r\nDescription : x\r\n\r\n" +
            "Index : 2\r\nName : Windows 11 IoT Enterprise\r\nDescription : x\r\n");
        Assert.Equal(2, WindowsMedia.ChooseEdition(iot).Index);
    }

    [Fact]
    public void FindsInstallImage()
    {
        var dir = Directory.CreateTempSubdirectory("s9woa-media").FullName;
        try
        {
            Directory.CreateDirectory(Path.Combine(dir, "sources"));
            var wim = Path.Combine(dir, "sources", "install.wim");
            File.WriteAllText(wim, "");
            Assert.Equal(wim, WindowsMedia.FindInstallImage(dir));
            Assert.Equal(wim, WindowsMedia.FindInstallImage(wim));
            Assert.Null(WindowsMedia.FindInstallImage(Path.Combine(dir, "nope.wim")));
        }
        finally
        {
            Directory.Delete(dir, recursive: true);
        }
    }

    [Fact]
    public async Task OpensAnIsoReadOnlyAndFindsItsInstallImage()
    {
        var script = WindowsMedia.MountIsoScript(@"C:\iso\it's.iso");
        Assert.Contains(@"-ImagePath 'C:\iso\it''s.iso'", script, StringComparison.Ordinal);
        Assert.Contains("Mount-DiskImage -InputObject $img -Access ReadOnly -PassThru", script, StringComparison.Ordinal);
        Assert.DoesNotContain("-StorageType", script, StringComparison.Ordinal);
        Assert.Contains("if (-not $img.Attached)", script, StringComparison.Ordinal);

        // No drive letter: a clear error instead of "no install.wim".
        var runner = new RecordingRunner { StdOut = _ => "\r\n" };
        await Assert.ThrowsAsync<InvalidOperationException>(() => new WindowsMedia(runner, @"C:\sys").ResolveInstallImageAsync(@"C:\w.iso"));
        Assert.Contains(runner.Calls, c => c.StartsWith("powershell.exe -NoProfile", StringComparison.Ordinal));

        // A .wim is used as is, without PowerShell.
        var wimRunner = new RecordingRunner();
        Assert.Null(await new WindowsMedia(wimRunner, @"C:\sys").ResolveInstallImageAsync(@"C:\missing\install.wim"));
        Assert.Empty(wimRunner.Calls);
    }

    [Fact]
    public void DiscoversDriverFolders()
    {
        var dir = Directory.CreateTempSubdirectory("s9woa-drv").FullName;
        try
        {
            Directory.CreateDirectory(Path.Combine(dir, "Ufs"));
            Directory.CreateDirectory(Path.Combine(dir, "Touch"));
            File.WriteAllText(Path.Combine(dir, "Ufs", "Exynos9810Ufs.inf"), "");
            File.WriteAllText(Path.Combine(dir, "Touch", "S6SY761Touch.inf"), "");
            var found = ImageBuilder.DiscoverDrivers(dir);
            Assert.Equal(2, found.Count);
            Assert.Contains(found, p => p.Name == "Ufs");
            Assert.Empty(ImageBuilder.DiscoverDrivers(Path.Combine(dir, "missing")));
        }
        finally
        {
            Directory.Delete(dir, recursive: true);
        }
    }

    [Fact]
    public async Task BuildAppliesInjectsThenSlims()
    {
        var apply = Directory.CreateTempSubdirectory("s9woa-apply").FullName;
        try
        {
            var runner = new RecordingRunner();
            var drivers = new[] { new DriverPackage("Ufs", @"C:\d\Ufs"), new DriverPackage("Touch", @"C:\d\Touch") };
            await new ImageBuilder(runner, @"C:\sys").BuildAsync(apply, @"C:\m\install.wim", 6, drivers, SlimProfile.None);

            var applyIdx = runner.Calls.FindIndex(c => c.Contains("/Apply-Image", StringComparison.Ordinal));
            var ufsIdx = runner.Calls.FindIndex(c => c.Contains("/Add-Driver", StringComparison.Ordinal) && c.Contains(@"C:\d\Ufs", StringComparison.Ordinal));
            Assert.True(applyIdx >= 0 && ufsIdx > applyIdx);
            Assert.Contains(runner.Calls, c => c.Contains("/Add-Driver", StringComparison.Ordinal) && c.Contains("/ForceUnsigned", StringComparison.Ordinal));
            Assert.Contains(runner.Calls, c => c.Contains(@"/ImageFile:C:\m\install.wim", StringComparison.Ordinal) && c.Contains("/Index:6", StringComparison.Ordinal));
        }
        finally
        {
            Directory.Delete(apply, recursive: true);
        }
    }

    [Fact]
    public void VhdxScriptMirrorsThePhoneDisk()
    {
        var builder = new VhdxImageBuilder(new RecordingRunner(), @"C:\sys", null, PartitionMap.Star2Lte)
            { EspLetter = 'S', WindowsLetter = 'W' };
        var script = builder.CreateScript(@"C:\img\s9.vhdx");
        Assert.Contains("New-VHD -Path 'C:\\img\\s9.vhdx'", script, StringComparison.Ordinal);
        Assert.Contains("-SizeBytes 63963136000", script, StringComparison.Ordinal);
        Assert.Contains("-LogicalSectorSizeBytes 4096 -PhysicalSectorSizeBytes 4096", script, StringComparison.Ordinal);
        Assert.Contains("{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}", script, StringComparison.Ordinal); // ESP
        Assert.Contains("{e3c9e316-0b5c-4db8-817d-f92df00215ae}", script, StringComparison.Ordinal); // MSR
        Assert.Contains("-Offset 6951534592 -Size 57004785664 -Alignment 4096", script, StringComparison.Ordinal); // = USERDATA
        Assert.Contains("-FileSystem NTFS -AllocationUnitSize 4096", script, StringComparison.Ordinal);
        Assert.Contains("-NewDriveLetter S", script, StringComparison.Ordinal);
        Assert.Contains("-NewDriveLetter W", script, StringComparison.Ordinal);
        var attach = builder.AttachReadOnlyScript(@"C:\img\s9.vhdx");
        Assert.Contains("-ReadOnly -NoDriveLetter", attach, StringComparison.Ordinal);
        Assert.Contains("6951534592", attach, StringComparison.Ordinal);
        Assert.Contains("Dismount-VHD", VhdxImageBuilder.DetachScript(@"C:\img\s9.vhdx"), StringComparison.Ordinal);
        Assert.DoesNotContain('C', VhdxImageBuilder.FreeDriveLetters());
    }

    [Fact]
    public void Beyond0LteGeometryIsUsedWhenThePhoneIsAnS10e()
    {
        var geometry = PartitionMap.For("beyond0lte");
        Assert.True(127_943_049_216L == geometry.DiskBytes);
        Assert.True(8_811_184_128L == geometry.WindowsOffset);
        Assert.True(119_126_642_688L == geometry.WindowsBytes);
        Assert.True(629_145_600L == geometry.CacheBytes);
        var builder = new VhdxImageBuilder(new RecordingRunner(), @"C:\sys", null, geometry)
            { EspLetter = 'S', WindowsLetter = 'W' };
        var script = builder.CreateScript(@"C:\img\b0.vhdx");
        Assert.Contains("-SizeBytes 127943049216", script, StringComparison.Ordinal);
        Assert.Contains("-Offset 8811184128 -Size 119126642688 -Alignment 4096", script, StringComparison.Ordinal);
        Assert.DoesNotContain("6951534592", script, StringComparison.Ordinal);
    }

    [Fact]
    public void AnUnidentifiedPhoneGetsNoGeometryAtAll()
    {
        Assert.False(PartitionMap.IsKnown(null));
        Assert.False(PartitionMap.IsKnown("hax"));
        Assert.Throws<ArgumentOutOfRangeException>(() => PartitionMap.For(null));
        Assert.Throws<ArgumentOutOfRangeException>(() => PartitionMap.For("star2lteX"));
        Assert.Throws<InvalidOperationException>(() => new VhdxImageBuilder(new RecordingRunner(), @"C:\sys"));
    }

    [Fact]
    public void OnlyTheReferencePhoneHasASecondaryEsp()
    {
        Assert.Equal("SYSTEM", PartitionMap.Star2Lte.SecondaryEspPartition);
        Assert.Null(PartitionMap.Beyond0Lte.SecondaryEspPartition);
    }

    [Fact]
    public void GeometrySizesAreWholeLogicalBlocks()
    {
        foreach (var g in new[] { PartitionMap.Star2Lte, PartitionMap.Beyond0Lte })
        {
            Assert.Equal(0L, g.WindowsOffset % 4096);
            Assert.Equal(0L, g.WindowsBytes % 4096);
            Assert.Equal(0L, g.CacheBytes % 4096);
            Assert.Equal(0L, g.BootBytes % 4096);
            Assert.Equal(0L, g.RecoveryBytes % 4096);
        }
        Assert.Equal(16_638L * 4096, PartitionMap.Star2Lte.RecoveryBytes);
        Assert.Equal(16_512L * 4096, PartitionMap.Beyond0Lte.RecoveryBytes);
    }

    private sealed class FailingOfflineRunner(bool offlineSupported) : IProcessRunner
    {
        public List<string> Calls { get; } = [];
        public Task<ProcessResult> RunAsync(string fileName, IReadOnlyList<string> arguments, TimeSpan timeout,
            CancellationToken cancellationToken = default)
        {
            Calls.Add(string.Join(' ', arguments));
            var ok = offlineSupported || !arguments.Contains("/offline");
            return Task.FromResult(new ProcessResult(ok ? 0 : 1, ok ? "Boot files successfully created." : "The parameter is incorrect.", ""));
        }
    }

    [Theory]
    [InlineData(true, 1)]
    [InlineData(false, 2)]
    public async Task BcdbootTargetsThePhoneNotThisPc(bool offlineSupported, int calls)
    {
        var runner = new FailingOfflineRunner(offlineSupported);
        var builder = new VhdxImageBuilder(runner, @"C:\sys", null, PartitionMap.Star2Lte);
        await builder.WriteBootFilesAsync(@"W:\Windows", "S:", CancellationToken.None);
        Assert.Equal(calls, runner.Calls.Count);
        Assert.Equal(@"W:\Windows /s S: /f UEFI /offline", runner.Calls[0]);
        if (calls == 2)
        {
            Assert.Equal(@"W:\Windows /s S: /f UEFI", runner.Calls[1]);
        }
    }
}
