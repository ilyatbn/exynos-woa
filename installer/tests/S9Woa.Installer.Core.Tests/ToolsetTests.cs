// SPDX-License-Identifier: BSD-2-Clause-Patent
using System.IO.Compression;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using S9Woa.Installer.Core.Image;
using S9Woa.Installer.Core.Processes;
using S9Woa.Installer.Core.Toolset;
using S9Woa.Installer.Core.Twrp;

namespace S9Woa.Installer.Core.Tests;

public sealed class ToolsetTests : IDisposable
{
    private readonly string _root = Directory.CreateTempSubdirectory("s9woa-toolset").FullName;

    public void Dispose() => Directory.Delete(_root, recursive: true);

    private ToolsetPaths Paths(string? pathVar = null) => new(
        Path.Combine(_root, "app"), Path.Combine(_root, "data"), Path.Combine(_root, "local"), Path.Combine(_root, "pf"), pathVar);

    private static string Touch(string path, byte[]? content = null)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllBytes(path, content ?? []);
        return path;
    }

    private static byte[] BootImageBytes(int size = 8192)
    {
        var b = new byte[size];
        "ANDROID!"u8.CopyTo(b);
        return b;
    }

    private sealed class FakeRegistry : IRegistryReader
    {
        public Dictionary<string, Dictionary<string, string>> Keys { get; } = new(StringComparer.OrdinalIgnoreCase);
        public bool KeyExists(string p) => Keys.ContainsKey(p) || Keys.Keys.Any(k => k.StartsWith(p + "\\", StringComparison.OrdinalIgnoreCase));
        public IReadOnlyList<string> SubKeyNames(string p) => Keys.Keys
            .Where(k => k.StartsWith(p + "\\", StringComparison.OrdinalIgnoreCase))
            .Select(k => k[(p.Length + 1)..].Split('\\')[0]).Distinct(StringComparer.OrdinalIgnoreCase).ToList();
        public string? GetString(string p, string name) => Keys.TryGetValue(p, out var v) && v.TryGetValue(name, out var s) ? s : null;
    }

    private sealed class FakeVerifier(SignatureInfo result) : ISignatureVerifier
    {
        public SignatureInfo Verify(string file) => result;
    }

    private sealed class FakeRunner(Action<string, IReadOnlyList<string>>? onRun = null) : IProcessRunner
    {
        public List<string> Calls { get; } = [];
        public Task<ProcessResult> RunAsync(string fileName, IReadOnlyList<string> arguments, TimeSpan timeout, CancellationToken ct = default)
        {
            Calls.Add($"{Path.GetFileName(fileName)} {string.Join(' ', arguments)}".Trim());
            onRun?.Invoke(fileName, arguments);
            return Task.FromResult(new ProcessResult(0, "", ""));
        }
    }

    private sealed class FakeHttp(Func<Uri, HttpResponseMessage> respond) : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken ct) =>
            Task.FromResult(respond(request.RequestUri!));
    }

    /// <summary>
    /// Stand-in for the real WinRE builder so the toolset tests do not need the 42 MiB official
    /// TWRP image: it "builds" by copying the raw image to the output and recording a matching
    /// builder version and base hash, which is all Detect(Twrp) checks. The real builder is
    /// exercised end-to-end in WinReBuilderTests against the reference image when present.
    /// </summary>
    private sealed class FakeWinReBuilder(BaseImageKind kind = BaseImageKind.OfficialTwrp, Func<string, BaseImageKind>? classify = null,
        Func<string, WinReStamp?>? stamp = null)
        : IWinReRecoveryBuilder
    {
        /// <summary>The gear GIF passed to the last build (null = built-in gears).</summary>
        public string? LastGears { get; private set; }

        public BaseImageKind Classify(string imagePath) => classify?.Invoke(imagePath) ?? kind;

        public WinReStamp? ReadStamp(string imagePath) => stamp?.Invoke(imagePath);

        public WinReRecoveryInfo Build(string basePath, string outputPath, string? modulesDirectory, string? gearsGifPath = null,
            IProgress<string>? log = null)
        {
            LastGears = gearsGifPath;
            var bytes = File.ReadAllBytes(basePath);
            Directory.CreateDirectory(Path.GetDirectoryName(outputPath)!);
            File.WriteAllBytes(outputPath, bytes);
            var sha = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
            return new WinReRecoveryInfo
            {
                Builder = WinReTwrpBuilder.BuilderVersion,
                BaseSha256 = sha,
                Sha256 = sha,
                Gears = WinReTwrpBuilder.GearsId(gearsGifPath is null ? null : File.ReadAllBytes(gearsGifPath)),
            };
        }
    }

    private ToolsetManager Manager(IProcessRunner? runner = null, FakeRegistry? registry = null,
        ISignatureVerifier? verifier = null, HttpMessageHandler? http = null, string? winget = null,
        IWinReRecoveryBuilder? winre = null, string? deviceCodename = "star2lte")
    {
        ToolsetManager m = new(Paths(), runner ?? new FakeRunner(),
            new HttpClient(http ?? new FakeHttp(_ => new HttpResponseMessage(HttpStatusCode.NotFound))),
            registry ?? new FakeRegistry(), verifier ?? new FakeVerifier(new SignatureInfo(false, null)),
            winget, winre ?? new FakeWinReBuilder());
        m.Config.DeviceCodename = deviceCodename;
        return m;
    }

    [Fact]
    public void PayloadsFollowTheIdentifiedPhoneAndNothingIsAssumed()
    {
        var b0 = Manager(deviceCodename: "beyond0lte");
        Assert.Contains("twrp-beyond0lte.img", b0.TwrpPayload, StringComparison.Ordinal);
        Assert.Equal("Exynos9820Ufs.inf", b0.UfsInf);
        Assert.Equal("twrp-3.7.0_9-2-beyond0lte.img", TwrpProfiles.Beyond0Lte.OfficialImage);
        Assert.Equal("https://twrp.me/samsung/samsunggalaxys10e.html", TwrpProfiles.Beyond0Lte.PageUrl);
        Assert.Equal(ToolState.Missing, b0.Detect(Tools.Twrp).State);

        var none = Manager(deviceCodename: null);
        Assert.Contains("twrp-unidentified", none.TwrpPayload, StringComparison.Ordinal);
        Assert.Null(none.UfsInf);
        Assert.Equal(ToolState.Missing, none.Detect(Tools.Twrp).State);
    }

    [Fact]
    public void LocatorSearchesOverrideAppWingetAndPath()
    {
        var p = Paths();
        Assert.Null(ToolLocator.Find(p, "adb.exe", "Google.PlatformTools"));

        var winget = Touch(Path.Combine(p.LocalAppData, @"Microsoft\WinGet\Packages\Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe\platform-tools\adb.exe"));
        Assert.Equal(winget, ToolLocator.Find(p, "adb.exe", "Google.PlatformTools"));

        var app = Touch(Path.Combine(p.AppDirectory, @"tools\platform-tools\adb.exe"));
        Assert.Equal(app, ToolLocator.Find(p, "adb.exe", "Google.PlatformTools", null, @"tools\platform-tools"));

        var chosen = Touch(Path.Combine(_root, "mine", "adb.exe"));
        Assert.Equal(chosen, ToolLocator.Find(p, "adb.exe", "Google.PlatformTools", chosen, @"tools\platform-tools"));

        var machineZadig = Touch(Path.Combine(p.ProgramFiles, @"WinGet\Packages\akeo.ie.Zadig_x\zadig-2.9.exe"));
        Assert.Equal(machineZadig, ToolLocator.Find(p, "zadig*.exe", "akeo.ie.Zadig"));

        var onPath = Touch(Path.Combine(_root, "bin", "heimdall.exe"));
        Assert.Equal(onPath, ToolLocator.Find(Paths(Path.Combine(_root, "bin")), "heimdall.exe", "BenjaminDobell.Heimdall"));
    }

    [Fact]
    public void WingetInstallIsExactSilentAndNonInteractive()
    {
        var args = string.Join(' ', WingetClient.InstallArguments("Google.PlatformTools"));
        Assert.Contains("install --id Google.PlatformTools --exact --source winget", args, StringComparison.Ordinal);
        Assert.Contains("--silent", args, StringComparison.Ordinal);
        Assert.Contains("--disable-interactivity", args, StringComparison.Ordinal);
        Assert.Contains("--accept-package-agreements", args, StringComparison.Ordinal);
    }

    [Fact]
    public void BootImageValidation()
    {
        var good = Touch(Path.Combine(_root, "twrp-3.7.0_9-0-star2lte.img"), BootImageBytes());
        Assert.Null(BootImage.ValidateTwrp(good));
        Assert.Null(BootImage.ValidateUefi(good));

        var wrongModel = Touch(Path.Combine(_root, "twrp-3.7.0_9-0-star2qlte.img"), BootImageBytes());
        Assert.NotNull(BootImage.ValidateTwrp(wrongModel));
        var s9 = Touch(Path.Combine(_root, "twrp-3.7.0_9-0-starlte.img"), BootImageBytes());
        Assert.NotNull(BootImage.ValidateTwrp(s9));

        var notBoot = Touch(Path.Combine(_root, "x-star2lte.img"), new byte[8192]);
        Assert.Contains("ANDROID!", BootImage.ValidateTwrp(notBoot));
        Assert.NotNull(BootImage.Validate(good, 4096 + 1));
    }

    [Fact]
    public void DetectsDownloadModeWinUsbBinding()
    {
        var reg = new FakeRegistry();
        Assert.Equal(ToolState.Deferred, DownloadModeDriver.Detect(reg).State);

        reg.Keys[@"SYSTEM\CurrentControlSet\Enum\USB\VID_04E8&PID_685D\5&abc"] = new() { ["Service"] = "dg_ssudbus" };
        var samsung = DownloadModeDriver.Detect(reg);
        Assert.Equal(ToolState.Deferred, samsung.State);
        Assert.Contains("Zadig", samsung.Detail);

        reg.Keys[@"SYSTEM\CurrentControlSet\Enum\USB\VID_04E8&PID_685D\5&abc"]["Service"] = "WinUSB";
        Assert.Equal(ToolState.Ready, DownloadModeDriver.Detect(reg).State);
    }

    private static string Sha(byte[] b) => Convert.ToHexString(SHA256.HashData(b)).ToLowerInvariant();

    private static FakeHttp Release(Dictionary<string, byte[]> assets, bool includeSums = true, string? badSumFor = null)
    {
        var sums = string.Join('\n', assets.Select(a => $"{(a.Key == badSumFor ? new string('0', 64) : Sha(a.Value))}  {a.Key}"));
        var all = new Dictionary<string, byte[]>(assets);
        if (includeSums)
        {
            all[ReleaseClient.ChecksumsAsset] = Encoding.ASCII.GetBytes(sums);
        }
        var json = "{\"tag_name\":\"v1\",\"assets\":[" + string.Join(',', all.Select(a =>
            $"{{\"name\":\"{a.Key}\",\"browser_download_url\":\"https://dl.example/{a.Key}\",\"size\":{a.Value.Length}}}")) + "]}";
        return new FakeHttp(uri =>
        {
            if (uri.Host == "api.github.com")
            {
                return new HttpResponseMessage(HttpStatusCode.OK) { Content = new StringContent(json) };
            }
            var name = uri.AbsolutePath.TrimStart('/');
            return all.TryGetValue(name, out var body)
                ? new HttpResponseMessage(HttpStatusCode.OK) { Content = new ByteArrayContent(body) }
                : new HttpResponseMessage(HttpStatusCode.NotFound);
        });
    }

    [Fact]
    public async Task ReleaseDownloadsAreChecksumVerified()
    {
        var uefi = BootImageBytes();
        var ok = Manager(http: Release(new() { ["uefi.img"] = uefi }));
        var status = await ok.DownloadReleaseAsync(Tools.Uefi);
        Assert.Equal(ToolState.Ready, status.State);
        Assert.Equal(uefi, await File.ReadAllBytesAsync(ok.UefiPayload));

        File.Delete(ok.UefiPayload);
        var tampered = Manager(http: Release(new() { ["uefi.img"] = uefi }, badSumFor: "uefi.img"));
        await Assert.ThrowsAsync<InvalidOperationException>(() => tampered.DownloadReleaseAsync(Tools.Uefi));
        Assert.False(File.Exists(tampered.UefiPayload));

        var unsigned = Manager(http: Release(new() { ["uefi.img"] = uefi }, includeSums: false));
        await Assert.ThrowsAsync<InvalidOperationException>(() => unsigned.DownloadReleaseAsync(Tools.Uefi));

        var none = Manager();
        var e = await Assert.ThrowsAsync<InvalidOperationException>(() => none.DownloadReleaseAsync(Tools.Uefi));
        Assert.Contains("no published release", e.Message);
    }

    [Fact]
    public async Task AReleaseFirmwareCatalogIsDownloadedWholeAndVerifiedTwice()
    {
        byte[] Uefi(byte tag) { var b = BootImageBytes(); b[100] = tag; return b; }
        var a = Uefi(1);
        var b = Uefi(2);
        string Catalog(byte[] second) => $$"""
            {"schema":"{{FirmwareCatalog.Schema}}","images":[
              {"file":"star2lte-uefi-22621.2428.img","sha256":"{{Sha(a)}}","windows":"22621.2428","mediaBuilds":["22631.2428"],"loaderSha256":"l1","kernelSha256":"k1"},
              {"file":"star2lte-uefi-22621.7582.img","sha256":"{{Sha(second)}}","windows":"22621.7582","loaderSha256":"l2","kernelSha256":"k2"}]}
            """;
        Dictionary<string, byte[]> Assets(string catalog) => new()
        {
            [FirmwareCatalog.FileName] = Encoding.UTF8.GetBytes(catalog),
            ["star2lte-uefi-22621.2428.img"] = a,
            ["star2lte-uefi-22621.7582.img"] = b,
        };

        var m = Manager(http: Release(Assets(Catalog(b))));
        var status = await m.DownloadReleaseAsync(Tools.Uefi);
        Assert.Equal(ToolState.Ready, status.State);
        Assert.Contains("22621.2428, 22621.7582", status.Detail, StringComparison.Ordinal);
        var catalog = m.LoadFirmwareCatalog()!;
        Assert.Equal(2, catalog.Present(BootImage.BootPartitionBytes).Count);
        Assert.Equal("22621.2428", catalog.Choose("22631.2428")!.Image.Windows);
        Assert.False(File.Exists(m.UefiPayload)); // no legacy single image when the release has a catalog

        // An image that passes SHA256SUMS but not the catalog's own hash is refused, and the
        // catalog already installed stays as it was.
        var tampered = Manager(http: Release(Assets(Catalog(Uefi(3)))));
        Assert.Equal(ToolState.Error, (await tampered.DownloadReleaseAsync(Tools.Uefi)).State);
        Assert.Equal(2, m.LoadFirmwareCatalog()!.Present(BootImage.BootPartitionBytes).Count);
        Assert.Equal(2, tampered.LoadFirmwareCatalog()!.Present(BootImage.BootPartitionBytes).Count);
        Assert.False(Directory.Exists(tampered.UefiCatalogDirectory + ".download"));
    }

    private static byte[] Zip(params (string Name, byte[] Data)[] entries)
    {
        using var ms = new MemoryStream();
        using (var zip = new ZipArchive(ms, ZipArchiveMode.Create, leaveOpen: true))
        {
            foreach (var (name, data) in entries)
            {
                using var s = zip.CreateEntry(name).Open();
                s.Write(data);
            }
        }
        return ms.ToArray();
    }

    [Fact]
    public async Task ReleaseDriversAreExtractedAndValidated()
    {
        var zip = Zip(("Exynos9810Ufs/Exynos9810Ufs.inf", [1]), ("Exynos9810Ufs/Exynos9810Ufs.sys", [2]),
            ("S6SY761Touch/S6SY761Touch.inf", [3]), ("S6SY761Touch/S6SY761Touch.sys", [4]));
        var m = Manager(http: Release(new() { ["drivers.zip"] = zip }));
        var status = await m.DownloadReleaseAsync(Tools.Drivers);
        Assert.Equal(ToolState.Ready, status.State);
        Assert.Equal(2, ToolsetManager.BuiltDriverPackages(m.DriversPayload).Count);
    }

    [Fact]
    public void ZipSlipIsRejected()
    {
        var zipFile = Touch(Path.Combine(_root, "evil.zip"), Zip(("../../escape.txt", [1])));
        Assert.Throws<InvalidOperationException>(() => ReleaseClient.ExtractSafely(zipFile, Path.Combine(_root, "out")));
        Assert.False(File.Exists(Path.Combine(_root, "escape.txt")));
    }

    [Fact]
    public async Task ChosenFilesAreValidatedAndRemembered()
    {
        var m = Manager();
        Assert.False(ToolsetManager.IsComplete(m.DetectAll()));

        var twrp = Touch(Path.Combine(_root, "dl", "twrp-3.7.0_9-0-star2lte.img"), BootImageBytes());
        Assert.Equal(ToolState.Ready, (await m.UseFileAsync(Tools.Twrp, twrp)).State);
        Assert.True(File.Exists(m.TwrpPayload));       // the raw TWRP is kept
        Assert.True(File.Exists(m.TwrpWinrePayload));   // and the WinRE recovery is built from it
        Assert.Equal(m.TwrpWinrePayload, m.ResolvePath(Tools.Twrp)); // flashing uses the WinRE image

        var wrong = Touch(Path.Combine(_root, "dl", "twrp-star2qlte.img"), BootImageBytes());
        Assert.Equal(ToolState.Error, (await m.UseFileAsync(Tools.Twrp, wrong)).State);

        var heimdall = Touch(Path.Combine(_root, "h", "heimdall.exe"));
        Assert.Equal(heimdall, (await m.UseFileAsync(Tools.Heimdall, heimdall)).Path);
        Assert.Equal(ToolState.Error, (await m.UseFileAsync(Tools.Heimdall, twrp)).State);
        Assert.Equal(heimdall, ToolsetConfig.Load(m.Paths.DataDirectory).Overrides[Tools.Heimdall]);
    }

    [Fact]
    public async Task TwrpBuildsWinReAndDetectsStaleBuilds()
    {
        var m = Manager();
        Assert.Equal(ToolState.Missing, m.Detect(Tools.Twrp).State);

        var twrp = Touch(Path.Combine(_root, "twrp-3.7.0_9-0-star2lte.img"), BootImageBytes());
        var status = await m.UseFileAsync(Tools.Twrp, twrp);
        Assert.Equal(ToolState.Ready, status.State);
        Assert.Contains("WinRE recovery built", status.Detail);
        Assert.Equal(m.TwrpWinrePayload, status.Path);

        // A newer builder makes the recorded build stale, so it is no longer Ready.
        var infoPath = m.TwrpWinrePayload + ".json";
        var info = WinReRecoveryInfo.Load(infoPath)!;
        info.Builder = "winre-999";
        info.Save(infoPath);
        Assert.Equal(ToolState.Missing, m.Detect(Tools.Twrp).State);

        // Rebuilt, it is Ready again; a different base image underneath makes it stale too.
        Assert.Equal(ToolState.Ready, (await m.UseFileAsync(Tools.Twrp, twrp)).State);
        File.WriteAllBytes(m.TwrpPayload, BootImageBytes(9000));
        Assert.Equal(ToolState.Missing, m.Detect(Tools.Twrp).State);
    }

    // Stand-ins for "official TWRP" and "an existing WinRE build", told apart by content so the
    // classification survives the image being copied into the payload under another name.
    private static byte[] PrebuiltWinReBytes()
    {
        var b = BootImageBytes(8192);
        "WINRE"u8.CopyTo(b.AsSpan(16));
        return b;
    }

    private static FakeWinReBuilder ByContent() => new(classify: p =>
        File.ReadAllBytes(p).AsSpan().IndexOf("WINRE"u8) >= 0 ? BaseImageKind.WinReBuild : BaseImageKind.OfficialTwrp);

    [Fact]
    public async Task AutoSetupBuildsWinReFromAnAlreadyChosenTwrp()
    {
        var m = Manager();
        // e.g. chosen by an earlier installer version: the raw image is there, the WinRE build is not.
        Touch(m.TwrpPayload, BootImageBytes());
        Assert.Equal(ToolState.Missing, m.Detect(Tools.Twrp).State);

        var result = await m.AutoSetupAsync();
        Assert.Equal(ToolState.Ready, result[Tools.Twrp].State);
        Assert.Equal(m.TwrpWinrePayload, result[Tools.Twrp].Path);
    }

    [Fact]
    public async Task BuildFolderSuppliesTwrpButAPrebuiltNeverReplacesOfficial()
    {
        // Nothing chosen yet: a prebuilt WinRE image in the build folder is taken as-is,
        // and the UEFI image (also named *star2lte*) is not mistaken for TWRP.
        var first = Manager(winre: ByContent());
        var build = Path.Combine(_root, "build");
        Touch(Path.Combine(build, @"twrp\star2lte-winre-recovery.img"), PrebuiltWinReBytes());
        Touch(Path.Combine(build, @"uefi\star2lte-uefi-22621.2428.img"), BootImageBytes());
        var r = first.UseBuildFolder(build);
        Assert.Equal(ToolState.Ready, r[Tools.Twrp].State);
        Assert.Contains("prebuilt", r[Tools.Twrp].Detail, StringComparison.OrdinalIgnoreCase);

        // An official TWRP chosen on the Setup page is not displaced by that prebuilt image.
        var m = Manager(winre: ByContent());
        await m.UseFileAsync(Tools.Twrp, Touch(Path.Combine(_root, "dl", "twrp-3.7.0_9-0-star2lte.img"), BootImageBytes(9000)));
        Assert.False(m.UseBuildFolder(build).ContainsKey(Tools.Twrp));
        Assert.Contains("built from TWRP", m.Detect(Tools.Twrp).Detail);

        // And an official image in the build folder is used and built.
        Touch(Path.Combine(build, @"twrp\twrp-3.7.0_9-0-star2lte.img"), BootImageBytes(10000));
        var fresh = Manager(winre: ByContent());
        File.Delete(fresh.TwrpWinrePayload);
        File.Delete(fresh.TwrpPayload);
        var r2 = fresh.UseBuildFolder(build);
        Assert.Equal(ToolState.Ready, r2[Tools.Twrp].State);
        Assert.Contains("built from TWRP", r2[Tools.Twrp].Detail);
    }

    [Fact]
    public async Task UpdateOsGearsFromTheBuildFolderAreBuiltInAndTracked()
    {
        var winre = new FakeWinReBuilder();
        var m = Manager(winre: winre);
        await m.UseFileAsync(Tools.Twrp, Touch(Path.Combine(_root, "dl", "twrp-3.7.0_9-0-star2lte.img"), BootImageBytes()));
        Assert.Contains("built-in gears", m.Detect(Tools.Twrp).Detail);
        Assert.Null(winre.LastGears);

        // The user's own GIF in <build folder>\twrp is taken into the payload and the recovery
        // is rebuilt with it (a corrupt one elsewhere in the folder is skipped).
        var build = Path.Combine(_root, "build");
        Touch(Path.Combine(build, @"junk\Broken-GearAnimation.gif"), "not a gif"u8.ToArray());
        Touch(Path.Combine(build, @"twrp\UpdateOS-GearAnimation.gif"), TestGifs.Bytes(TestGifs.TwoFrames));
        var r = m.UseBuildFolder(build);
        Assert.Equal(ToolState.Ready, r[Tools.Twrp].State);
        Assert.Contains("UpdateOS gears from your file", r[Tools.Twrp].Detail);
        Assert.Equal(m.TwrpGearsPayload, winre.LastGears);

        // Importing the same GIF again changes nothing.
        Assert.False(m.UseBuildFolder(build).ContainsKey(Tools.Twrp));

        // A different GIF, or none, makes the build stale until it is rebuilt.
        File.WriteAllBytes(m.TwrpGearsPayload, TestGifs.Bytes(TestGifs.Interlaced));
        Assert.Equal(ToolState.Missing, m.Detect(Tools.Twrp).State);
        File.Delete(m.TwrpGearsPayload);
        Assert.Equal(ToolState.Missing, m.Detect(Tools.Twrp).State);
        Assert.Equal(ToolState.Ready, m.BuildWinReRecovery().State);
        Assert.Null(winre.LastGears);
        Assert.Contains("built-in gears", m.Detect(Tools.Twrp).Detail);
    }

    [Fact]
    public void APrebuiltFromTheCurrentBuilderIsReportedAsSuchAndANewerPrebuiltReplacesAnOlderOne()
    {
        // Prebuilt images are told apart by content: "WINRE" plus a marker byte for "stamped".
        static byte[] Prebuilt(int size, bool stamped)
        {
            var b = PrebuiltWinReBytes();
            Array.Resize(ref b, size);
            b[100] = stamped ? (byte)1 : (byte)0;
            return b;
        }
        var winre = new FakeWinReBuilder(classify: _ => BaseImageKind.WinReBuild,
            stamp: p => File.ReadAllBytes(p)[100] == 1 ? new WinReStamp(WinReTwrpBuilder.BuilderVersion, "base", "sha256:gif") : null);
        var build = Path.Combine(_root, "build");
        var image = Path.Combine(build, @"twrp\star2lte-winre-recovery.img");

        var m = Manager(winre: winre);
        Touch(image, Prebuilt(8192, stamped: false));
        Assert.Contains("prebuilt WinRE-look recovery as-is", m.UseBuildFolder(build)[Tools.Twrp].Detail, StringComparison.Ordinal);

        // The folder now holds a newer prebuilt, made by this installer's builder: it replaces the old one.
        Touch(image, Prebuilt(9000, stamped: true));
        var r = m.UseBuildFolder(build);
        Assert.Equal(ToolState.Ready, r[Tools.Twrp].State);
        Assert.Contains($"current builder ({WinReTwrpBuilder.BuilderVersion}), with the UpdateOS gears", r[Tools.Twrp].Detail, StringComparison.Ordinal);
        Assert.Equal(9000, new FileInfo(m.TwrpWinrePayload).Length);

        // The same file again changes nothing.
        Assert.False(m.UseBuildFolder(build).ContainsKey(Tools.Twrp));
    }

    [Fact]
    public void EnsureTwrpModulesCompletesAPartialFolderAndKeepsSuppliedModules()
    {
        // A build folder that supplies only the reader and the acknowledger must not leave the
        // startup-record clearers out (the first boot would then fail to clear the records).
        var m = Manager();
        var supplied = Touch(Path.Combine(m.TwrpModulesDirectory, "rwd1_ack.ko"), [1, 2, 3]);

        var dir = m.EnsureTwrpModules();

        foreach (var module in new[] { "rwd1_ack.ko", "rwd1_evidence_reader.ko", "rwd1_clear_poc.ko", "pram_smp_clear_poc.ko" })
        {
            Assert.True(File.Exists(Path.Combine(dir, module)), module);
        }
        Assert.Equal([1, 2, 3], File.ReadAllBytes(supplied));
    }

    [Fact]
    public async Task TwrpAcceptsAPrebuiltWinReImage()
    {
        var m = Manager(winre: new FakeWinReBuilder(BaseImageKind.WinReBuild));
        var twrp = Touch(Path.Combine(_root, "twrp-3.7.0_9-0-star2lte.img"), BootImageBytes());
        var status = await m.UseFileAsync(Tools.Twrp, twrp);
        Assert.Equal(ToolState.Ready, status.State);
        Assert.Contains("prebuilt", status.Detail, StringComparison.OrdinalIgnoreCase);
        Assert.Equal(m.TwrpWinrePayload, m.ResolvePath(Tools.Twrp));
    }

    [Fact]
    public async Task TwrpRejectsAnImageThatIsNotOfficialTwrp()
    {
        var m = Manager(winre: new FakeWinReBuilder(BaseImageKind.Unknown));
        var twrp = Touch(Path.Combine(_root, "twrp-3.7.0_9-0-star2lte.img"), BootImageBytes());
        var status = await m.UseFileAsync(Tools.Twrp, twrp);
        Assert.Equal(ToolState.Error, status.State);
        Assert.Contains("official TWRP", status.Detail);
    }

    [Fact]
    public async Task SamsungInstallerMustBeSamsungSigned()
    {
        var reg = new FakeRegistry();
        var installer = Touch(Path.Combine(_root, "SAMSUNG_USB_Driver.exe"));
        var runner = new FakeRunner((_, _) => reg.Keys[@"SYSTEM\CurrentControlSet\Services\dg_ssudbus"] = []);

        var forged = Manager(runner, reg, new FakeVerifier(new SignatureInfo(true, "CN=Someone Else")));
        Assert.Equal(ToolState.Error, (await forged.UseFileAsync(Tools.SamsungUsb, installer)).State);
        Assert.Empty(runner.Calls);

        var unsigned = Manager(runner, reg, new FakeVerifier(new SignatureInfo(false, "CN=Samsung Electronics Co., Ltd.")));
        Assert.Equal(ToolState.Error, (await unsigned.UseFileAsync(Tools.SamsungUsb, installer)).State);
        Assert.Empty(runner.Calls);

        var genuine = Manager(runner, reg, new FakeVerifier(new SignatureInfo(true, "CN=Samsung Electronics Co., Ltd., O=Samsung")));
        Assert.Equal(ToolState.Ready, (await genuine.UseFileAsync(Tools.SamsungUsb, installer)).State);
        Assert.Single(runner.Calls);
    }

    [Fact]
    public void BuildFolderImportsUefiAndBuiltDriversOnly()
    {
        var build = Path.Combine(_root, "build");
        Touch(Path.Combine(build, @"firmware\star2lte-uefi-boot.img"), BootImageBytes());
        Touch(Path.Combine(build, @"src\Exynos9810Ufs\Exynos9810Ufs.inf"));           // source only: ignored
        Touch(Path.Combine(build, @"out\Exynos9810Ufs\Exynos9810Ufs.inf"));
        Touch(Path.Combine(build, @"out\Exynos9810Ufs\Exynos9810Ufs.sys"));
        Touch(Path.Combine(build, @"out\S6SY761Touch\S6SY761Touch.inf"));
        Touch(Path.Combine(build, @"out\S6SY761Touch\S6SY761Touch.sys"));

        var m = Manager();
        var r = m.UseBuildFolder(build);
        Assert.Equal(ToolState.Ready, r[Tools.Uefi].State);
        Assert.Equal(ToolState.Ready, r[Tools.Drivers].State);
        Assert.Equal(2, ToolsetManager.BuiltDriverPackages(m.DriversPayload).Count);
        Assert.Equal(build, ToolsetConfig.Load(m.Paths.DataDirectory).BuildFolder);

        // Picking the drivers subfolder (the drivers row's natural choice) still finds the firmware beside it.
        var fresh = Manager();
        var r2 = fresh.UseBuildFolder(Path.Combine(build, "out"));
        Assert.Equal(ToolState.Ready, r2[Tools.Uefi].State);
        Assert.Equal(ToolState.Ready, r2[Tools.Drivers].State);
        Assert.Equal(build, ToolsetConfig.Load(fresh.Paths.DataDirectory).BuildFolder);
        Assert.Equal(build, ToolsetManager.ResolveBuildRoot(build + Path.DirectorySeparatorChar));
    }

    [Fact]
    public async Task AutoSetupInstallsWingetProgramsAndReleasePayloads()
    {
        var p = Paths();
        var winget = Touch(Path.Combine(_root, "winget.exe"));
        var runner = new FakeRunner((_, args) =>
        {
            var id = args[2];
            var exe = id switch { "Google.PlatformTools" => @"platform-tools\adb.exe", "BenjaminDobell.Heimdall" => "heimdall.exe", _ => "zadig-2.9.exe" };
            Touch(Path.Combine(p.LocalAppData, $@"Microsoft\WinGet\Packages\{id}_src\{exe}"));
        });
        var zip = Zip(("Exynos9810Ufs/Exynos9810Ufs.inf", [1]), ("Exynos9810Ufs/Exynos9810Ufs.sys", [2]));
        var reg = new FakeRegistry();
        reg.Keys[@"SYSTEM\CurrentControlSet\Services\dg_ssudbus"] = [];
        var m = Manager(runner, reg, http: Release(new() { ["uefi.img"] = BootImageBytes(), ["drivers.zip"] = zip }), winget: winget);

        var result = await m.AutoSetupAsync();

        Assert.Equal(3, runner.Calls.Count);
        foreach (var id in new[] { Tools.Adb, Tools.Heimdall, Tools.Zadig, Tools.SamsungUsb, Tools.Uefi, Tools.Drivers })
        {
            Assert.Equal(ToolState.Ready, result[id].State);
        }
        Assert.Equal(ToolState.Missing, result[Tools.Twrp].State);   // needs the user's download
        Assert.Equal(ToolState.Deferred, result[Tools.DownloadModeDriver].State);
        Assert.False(ToolsetManager.IsComplete(result));

        await m.UseFileAsync(Tools.Twrp, Touch(Path.Combine(_root, "twrp-3.7.0_9-0-star2lte.img"), BootImageBytes()));
        Assert.True(ToolsetManager.IsComplete(m.DetectAll()));
    }
}
