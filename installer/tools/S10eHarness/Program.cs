// SPDX-License-Identifier: BSD-2-Clause-Patent
// Drives S9Woa.Installer.Core's own image and transfer pipeline for the S10e (beyond0lte),
// whose stages the WinUI app still gates. No step here is hand-rolled: each verb calls the
// same Core services the app's InstallPage calls (RunImageAsync / RunPartitionAsync /
// RunTransferAsync), with PartitionMap.For("beyond0lte").
//
//   image    <media.iso|install.wim> <index> <driversRoot> <workDir>
//   check    <adb.exe> <serial>
//   transfer <adb.exe> <serial> <workDir>      (writes USERDATA and CACHE - destructive)
using S9Woa.Installer.Core.Deploy;
using S9Woa.Installer.Core.Image;
using S9Woa.Installer.Core.Processes;

var geometry = PartitionMap.For("beyond0lte");
var runner = new ProcessRunner();
var log = new Progress<string>(m => Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] {m}"));

switch (args.FirstOrDefault())
{
    case "image" when args.Length == 5:
    {
        var media = new WindowsMedia(runner);
        var install = await media.ResolveInstallImageAsync(args[1])
            ?? throw new InvalidOperationException("No install.wim/esd in the media.");
        var index = int.Parse(args[2], System.Globalization.CultureInfo.InvariantCulture);
        var drivers = ImageBuilder.DiscoverDrivers(args[3]);
        Console.WriteLine($"install image {install}, index {index}, build {await media.GetBuildAsync(install, index)}");
        foreach (var d in drivers)
        {
            Console.WriteLine($"driver {d.Name} <- {d.Directory}");
        }
        var work = args[4];
        Directory.CreateDirectory(work);
        var vhdx = Path.Combine(work, "s10ewindows.vhdx");
        if (File.Exists(vhdx))
        {
            File.Delete(vhdx);
        }
        var unattend = new UnattendOptions { Username = "S10e", ComputerName = "GALAXY-S10E" };
        var built = await new VhdxImageBuilder(runner, null, null, geometry).BuildAsync(vhdx, install, index, drivers,
            SlimProfile.Lite, unattend, Path.Combine(work, "out"), log);
        Console.WriteLine($"windows.img {built.WindowsImage} {built.WindowsBytes} B sha256 {built.WindowsSha256}");
        Console.WriteLine($"esp {built.EspDirectory}");
        Console.WriteLine($"winload sha256 {built.LoaderSha256}");
        Console.WriteLine($"ntoskrnl sha256 {built.KernelSha256}");
        return 0;
    }
    case "check" when args.Length == 3:
    {
        // RunPartitionAsync's read-only verification: names present, USERDATA and CACHE sizes.
        var twrp = new TwrpClient(args[1], args[2], runner);
        var parts = await twrp.ListPartitionsAsync();
        foreach (var name in new[] { PartitionMap.WindowsTarget, PartitionMap.UefiTarget, PartitionMap.EfiSystemPartition,
            PartitionMap.RecoveryTarget, PartitionMap.Misc })
        {
            var actual = parts.Keys.FirstOrDefault(k => k.Equals(name, StringComparison.OrdinalIgnoreCase))
                ?? throw new InvalidOperationException($"missing partition {name}");
            Console.WriteLine($"{actual}: {await twrp.PartitionSizeAsync(actual)} B");
        }
        foreach (var (name, expected) in new[] { (PartitionMap.WindowsTarget, geometry.WindowsBytes),
            (PartitionMap.EfiSystemPartition, geometry.CacheBytes) })
        {
            var actual = parts.Keys.First(k => k.Equals(name, StringComparison.OrdinalIgnoreCase));
            var size = await twrp.PartitionSizeAsync(actual);
            if (size != expected)
            {
                Console.WriteLine($"MISMATCH {actual}: {size} B, geometry says {expected} B");
                return 2;
            }
        }
        Console.WriteLine("partitions match the beyond0lte geometry");
        return 0;
    }
    case "transfer" when args.Length == 4:
    {
        var twrp = new TwrpClient(args[1], args[2], runner);
        var outDir = Path.Combine(args[3], "out");
        if (await twrp.IsMountedAsync("/sdcard"))
        {
            await twrp.UnmountAsync("/sdcard");
        }
        try
        {
            await new TransferService(twrp).WriteRawImageAsync(PartitionMap.WindowsTarget, Path.Combine(outDir, "windows.img"),
                new RawWriteOptions { Verify = true }, mountToEnsureUnmounted: "/data", log: log);
            await new BootFilesService(twrp, geometry).WriteAsync(Path.Combine(outDir, "esp"), log);
        }
        finally
        {
            await twrp.ClearInstallStatusAsync(CancellationToken.None);
        }
        Console.WriteLine("Windows and boot files written and verified on the phone.");
        return 0;
    }
    default:
        Console.Error.WriteLine("usage: image <media> <index> <driversRoot> <workDir> | check <adb> <serial> | transfer <adb> <serial> <workDir>");
        return 1;
}
