// SPDX-License-Identifier: BSD-2-Clause-Patent
using System.Diagnostics;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using S9Woa.Installer.Core;
using S9Woa.Installer.Core.Deploy;
using S9Woa.Installer.Core.Device;
using S9Woa.Installer.Core.Host;
using S9Woa.Installer.Core.Image;
using S9Woa.Installer.Core.Stages;

namespace S9Woa.Installer.App.Pages;

public sealed partial class InstallPage : Page, IWizardStep
{
    private readonly List<StageItem> _items;
    private readonly StageProgress _progress = new();
    private StageItem? _current;
    private CancellationTokenSource? _cts;
    private bool _running;

    public InstallPage()
    {
        InitializeComponent();
        _items = StageCatalog.All.Select(s => new StageItem(s)
        {
            Status = AppServices.State.StatusOf(s.Id),
            DeviceCodename = AppServices.CurrentDevice?.Codename,
        }).ToList();
        Timeline.ItemsSource = _items;
        ExperimentalToggle.IsOn = AppServices.ExperimentalEnabled;
        // The opt-in only matters while some step is still experimental FOR THIS PHONE.
        ExperimentalRow.Visibility = StageCatalog.All.Any(s =>
            StageCatalog.Resolve(s.Id, AppServices.CurrentDevice?.Codename) == StageAvailability.Experimental)
            ? Visibility.Visible : Visibility.Collapsed;
        VerifyToggle.IsOn = AppServices.VerifyWrites;
        SkipTwrpToggle.IsOn = AppServices.SkipTwrpFlash;
        var dev = AppServices.CurrentDevice;
        ReviewPhone.Text = dev is null ? "Not connected" : $"{dev.Model} · {dev.Bootloader}";
        ReviewMedia.Text = AppServices.MediaPath is null ? "Not selected" : Path.GetFileName(AppServices.MediaPath);
        ReviewProfile.Text = AppServices.Profile switch
        {
            SlimProfile.None => "Stock",
            SlimProfile.Lite => "Lite",
            _ => "Core",
        };
        ReviewAccount.Text = AppServices.Unattend.Username + (AppServices.Unattend.Password is null ? " · no password" : "");
        AppServices.LogWritten += OnLog;
        Unloaded += (_, _) => AppServices.LogWritten -= OnLog;
        UpdateNow();
    }

    public event EventHandler? StateChanged;

    public bool CanAdvance => !_running;

    public bool CanGoBack => !_running;

    public string? NextLabel => _running ? "Installing…" : _items.Any(i => i.Status == StageStatus.Failed) ? "Resume" : "Install";

    public Task<bool> OnAdvanceAsync()
    {
        _ = RunAllAsync();
        return Task.FromResult(false);
    }

    private void OnLog(string line) => DispatcherQueue.TryEnqueue(() =>
    {
        LogText.Text += line + Environment.NewLine;
        LogScroll.ChangeView(null, LogScroll.ScrollableHeight, null);
        if (_running && _progress.Observe(line))
        {
            UpdateNow();
        }
    });

    /// <summary>The progress card: the running step with its own bar, or what comes next, plus the whole install.</summary>
    private void UpdateNow()
    {
        static bool Finished(StageItem i) => i.Status is StageStatus.Done or StageStatus.Skipped;
        var total = _items.Count;
        var finished = _items.Count(Finished);
        var running = _running ? _current : null;
        OverallProgress.Value = StageProgress.Overall(_items.Select(i => (i.Definition.Id, Finished(i))), running?.Definition.Id, _progress.Fraction);
        OverallText.Text = $"{finished} of {total} steps done";
        if (running is not null)
        {
            NowCaption.Text = $"Step {_items.IndexOf(running) + 1} of {total}";
            NowTitle.Text = running.Title;
            NowDetail.Text = _progress.Detail.Length > 0 ? _progress.Detail : running.Summary;
            StepProgress.Visibility = Visibility.Visible;
            StepProgress.IsIndeterminate = _progress.Fraction is null;
            StepProgress.Value = _progress.Fraction ?? 0;
            return;
        }
        StepProgress.Visibility = Visibility.Collapsed;
        var failed = _items.FirstOrDefault(i => i.Status == StageStatus.Failed);
        var next = failed ?? _items.FirstOrDefault(i => !Finished(i));
        if (next is null)
        {
            NowCaption.Text = "Finished";
            NowTitle.Text = "Windows is installed";
            NowDetail.Text = "Unplug the phone. Windows finishes setting itself up and signs you in.";
            return;
        }
        NowCaption.Text = failed is not null ? "Stopped at" : finished == 0 ? "First step" : "Next step";
        NowTitle.Text = next.Title;
        NowDetail.Text = next.Summary;
    }

    private void SetStatus(StageItem item, StageStatus status, string? detail = null)
    {
        item.Status = status;
        AppServices.State.Set(item.Definition.Id, status, detail is null ? null : AppServices.Redact(detail));
        AppServices.SaveState();
    }

    private void SetRunning(bool running)
    {
        _running = running;
        CancelButton.Visibility = running ? Visibility.Visible : Visibility.Collapsed;
        ExperimentalToggle.IsEnabled = !running;
        VerifyToggle.IsEnabled = !running;
        SkipTwrpToggle.IsEnabled = !running;
        HeadlineText.Text = running ? "Installing Windows" : HeadlineText.Text;
        UpdateNow();
        StateChanged?.Invoke(this, EventArgs.Empty);
    }

    private async Task RunAllAsync()
    {
        _cts = new CancellationTokenSource();
        ResultBar.IsOpen = false;
        SetRunning(true);
        try
        {
            foreach (var item in _items)
            {
                if (item.Status is StageStatus.Done or StageStatus.Skipped)
                {
                    continue;
                }
                var avail = StageCatalog.Resolve(item.Definition.Id, AppServices.CurrentDevice?.Codename);
                if (avail == StageAvailability.NotImplemented)
                {
                    Report(InfoBarSeverity.Informational, item.Title,
                        "This step isn't automated yet. Nothing has been written to the phone beyond the finished steps.");
                    return;
                }
                if (avail == StageAvailability.Experimental && !AppServices.ExperimentalEnabled)
                {
                    Report(InfoBarSeverity.Informational, item.Title,
                        "This step is experimental: automated, but not yet validated end to end. Continue only on your own "
                        + $"{AppServices.PhoneDisplayName(AppServices.CurrentDevice)} with its backup.",
                        ("Run experimental steps", () =>
                        {
                            ExperimentalToggle.IsOn = true;
                            _ = RunAllAsync();
                        }));
                    return;
                }
                SetStatus(item, StageStatus.Running);
                _current = item;
                _progress.Begin(item.Definition.Id);
                UpdateNow();
                AppServices.Log($"stage {item.Definition.Id}: {item.Title}");
                var (ok, message) = await RunStageAsync(item.Definition, _cts.Token);
                SetStatus(item, ok ? StageStatus.Done : StageStatus.Failed, message);
                UpdateNow();
                if (!ok)
                {
                    AppServices.Log($"stage {item.Definition.Id} stopped: {message}");
                    Report(InfoBarSeverity.Error, item.Title, message);
                    return;
                }
            }
            HeadlineText.Text = "Windows is installed";
            LeadText.Text = "Unplug the phone. Windows finishes setting itself up and signs you in.";
            Report(InfoBarSeverity.Success, "All done",
                $"Windows is installed on your {AppServices.PhoneDisplayName(AppServices.CurrentDevice)}.");
        }
        catch (OperationCanceledException) when (_cts.IsCancellationRequested)
        {
            foreach (var i in _items.Where(i => i.Status == StageStatus.Running))
            {
                SetStatus(i, StageStatus.Pending);
            }
            AppServices.Log("install cancelled");
            Report(InfoBarSeverity.Warning, "Cancelled", "Nothing further was written. You can resume later.");
        }
        catch (Exception e)
        {
            // Every stop is logged and shown, including errors no step anticipated: a stop without
            // a message leaves nobody able to tell what happened.
            foreach (var i in _items.Where(i => i.Status == StageStatus.Running))
            {
                SetStatus(i, StageStatus.Failed, e.Message);
            }
            AppServices.Log($"install stopped: {e.GetType().Name}: {e.Message}");
            Report(InfoBarSeverity.Error, "Stopped", e.Message);
        }
        finally
        {
            SetRunning(false);
            if (HeadlineText.Text == "Installing Windows")
            {
                HeadlineText.Text = "Ready to install";
            }
        }
    }

    private static async Task<(bool Ok, string Message)> RunStageAsync(StageDefinition stage, CancellationToken ct)
    {
        var codename = AppServices.CurrentDevice?.Codename;
        if (StageCatalog.Resolve(stage.Id, codename) == StageAvailability.NotImplemented)
        {
            await Task.Yield();
            return (false, $"Not implemented for {AppServices.CurrentDevice?.Model ?? "the connected phone"} yet. Nothing has been written to the phone.");
        }
        // A stage can be Guided (the installer promises it, with the user driving part
        // of it) while the code behind it is still the reference phone's automation.
        // That code must not run against a different device, so the stage returns the
        // manual procedure - except where the phone can prove the step is already done.
        if (!StageCatalog.AutomatedFor(stage.Id, codename))
        {
            await Task.Yield();
            if (stage.Id == "twrp" && AppServices.CurrentDevice?.RecoveryVersion is string twrpVersion)
            {
                return (true, $"TWRP {twrpVersion} is already running on the phone; nothing was flashed.");
            }
            return (false, StageCatalog.ManualProcedure(stage.Id, codename));
        }

        var log = new Progress<string>(AppServices.Log);
        switch (stage.Id)
        {
            case "host":
                var host = HostPreflight.Evaluate(new LocalHostEnvironment(AppServices.AdbPath), AppServices.WorkDirectory);
                return host.HasBlockers()
                    ? (false, string.Join(" ", host.Where(h => h.Severity == CheckSeverity.Blocker).Select(h => h.Detail)))
                    : (true, "PC ready.");

            case "identify":
                if (AppServices.Adb is null)
                {
                    return (false, "adb.exe not found.");
                }
                var devices = await AppServices.Adb.ListDevicesAsync(ct);
                if (devices.Count == 0 && await AppServices.FindDownloadModeAsync(ct) is (true, var inDownload))
                {
                    if (inDownload is null)
                    {
                        return (false, "The phone is in Download mode but hasn't been identified yet. Start it in Android with USB debugging "
                            + "(or in TWRP) once, then press Resume.");
                    }
                    AppServices.CurrentDevice = inDownload;
                    return (true, $"{inDownload.Model} {inDownload.Bootloader} (in Download mode, identified earlier)");
                }
                if (devices.Count != 1)
                {
                    return (false, devices.Count == 0 ? "No phone connected." : "Connect only one phone.");
                }
                var snap = DeviceSnapshot.FromAdb(devices[0], devices[0].State is AdbState.Device or AdbState.Recovery
                    ? await AppServices.Adb.GetPropertiesAsync(devices[0].Serial, ct) : null);
                AppServices.CurrentDevice = snap;
                if (AppServices.State.DeviceSerial is { } known && known != snap.Serial)
                {
                    return (false, $"This is a different phone ({snap.Serial}) from the one this installation started on ({known}).");
                }
                var checks = DeviceEligibility.Evaluate(snap);
                return checks.HasBlockers()
                    ? (false, string.Join(" ", checks.Where(c => c.Severity == CheckSeverity.Blocker).Select(c => c.Detail)))
                    : (true, $"{snap.Model} {snap.Bootloader}");

            case "unlock":
                var refreshed = await RunStageAsync(StageCatalog.Get("identify"), ct);
                if (!refreshed.Ok)
                {
                    return refreshed;
                }
                return AppServices.CurrentDevice switch
                {
                    { FlashLocked: false } => (true, "Bootloader unlocked."),
                    // Download mode can't report it; a locked bootloader refuses the TWRP flash itself.
                    { Mode: DeviceMode.Download } => (true, "Not reported in Download mode; the TWRP flash checks it."),
                    _ => (false, "Unlock the bootloader (see the Phone page), finish Android setup, re-enable USB debugging, then press Resume."),
                };

            case "twrp":
                return await RunTwrpAsync(log, ct);

            case "backup":
                return await RunBackupAsync(log, ct);

            case "media":
                return await RunMediaAsync(ct);

            case "image":
                return await RunImageAsync(log, ct);

            case "partition":
                return await RunPartitionAsync(ct);

            case "transfer":
                return await RunTransferAsync(log, ct);

            case "uefi":
                return await RunUefiAsync(log, ct);

            case "firstboot":
                return await RunFirstBootAsync(log, ct);

            default:
                await Task.Yield();
                return StageCatalog.Resolve(stage.Id, AppServices.CurrentDevice?.Codename)
                    == StageAvailability.NotImplemented
                    ? (false, "This step is not automated in this preview yet. Nothing has been written to the phone. Follow the project README for the manual procedure.")
                    : (false, "Unknown stage.");
        }
    }

    private static async Task<(bool, string)> RunTwrpAsync(IProgress<string> log, CancellationToken ct)
    {
        if (AppServices.SkipTwrpFlash)
        {
            return await StartExistingTwrpAsync(log, ct);
        }
        if (AppServices.TwrpImagePath is null)
        {
            return (false, "TWRP is not set up. Add it on the Set up page, then press Resume.");
        }
        var resolved = await AppServices.TwrpFlasher.ResolveAsync(ct);
        if (resolved is null && AppServices.Device is not null && await RefreshDeviceAsync(ct) is { Mode: DeviceMode.Recovery })
        {
            // adb only runs in TWRP (stock recovery offers sideload at most), so it is already installed.
            log.Report("The phone is already in TWRP.");
            return await SettleTwrpAsync(log, ct) && await ClearBootRequestAsync(log, ct)
                ? (true, "TWRP is running.")
                : (false, "TWRP started but isn't answering over USB. Check the cable, then press Resume.");
        }
        if (resolved is null && AppServices.Device is not null
            && await RefreshDeviceAsync(ct) is { Mode: DeviceMode.Android } phone)
        {
            log.Report("Restarting the phone into Download mode...");
            await AppServices.Device.RebootAsync(phone.Serial, RebootTarget.Download, ct);
            for (var i = 0; i < 60 && resolved is null; i++)
            {
                await Task.Delay(TimeSpan.FromSeconds(2), ct);
                resolved = await AppServices.TwrpFlasher.ResolveAsync(ct);
                if (i == 20 && resolved is null)
                {
                    log.Report("Still waiting for Download mode. If the phone shows a warning screen, press Volume Up to continue.");
                }
            }
        }
        if (resolved is null)
        {
            return (false, "The phone isn't in Download mode. Power it off, hold Volume Down + Bixby + Power, then press Volume Up "
                + "at the warning screen. Then press Resume.");
        }
        bool restarted;
        try
        {
            restarted = await AppServices.TwrpFlasher.FlashRecoveryAsync(AppServices.TwrpImagePath, log, ct);
        }
        catch (Exception e) when (e is IOException or TimeoutException or InvalidOperationException
            || (e is OperationCanceledException && !ct.IsCancellationRequested))
        {
            // A Download-mode port that disappears mid-session (the phone restarted, or the cable
            // moved) surfaces as a cancelled or closed-port error: report it, never as a silent stop.
            return (false, $"Flashing TWRP failed: {e.Message} Restart the phone into Download mode (hold Volume Down + Power, "
                + "then Volume Down + Bixby + Power) and press Resume.");
        }

        if (AppServices.Device is null || AppServices.CurrentDevice is null)
        {
            return (true, "TWRP flashed. Boot into TWRP now (Volume Up + Bixby + Power).");
        }
        log.Report(restarted
            ? "The phone is restarting into TWRP by itself..."
            : "Now boot TWRP: hold Volume Down + Power until the screen goes off, then immediately hold Volume Up + Bixby + Power.");
        try
        {
            AppServices.CurrentDevice = await AppServices.Device.WaitForModeAsync(AppServices.CurrentDevice.Serial, DeviceMode.Recovery,
                TimeSpan.FromMinutes(5), log, ct);
            return await SettleTwrpAsync(log, ct) && await ClearBootRequestAsync(log, ct)
                ? (true, "TWRP is running.")
                : (false, "TWRP started but isn't answering over USB. Check the cable, then press Resume.");
        }
        catch (TimeoutException)
        {
            return (false, restarted
                ? "TWRP did not come up by itself. Start it: hold Volume Down + Power until the screen goes off, then immediately "
                    + "Volume Up + Bixby + Power. Then press Resume."
                : "TWRP did not come up. The flash succeeded; boot TWRP manually, then press Resume.");
        }
    }

    /// <summary>
    /// "TWRP is already on the phone": nothing is flashed. A phone in Android is restarted into
    /// recovery; otherwise the user starts TWRP with the key combination.
    /// </summary>
    private static async Task<(bool, string)> StartExistingTwrpAsync(IProgress<string> log, CancellationToken ct)
    {
        if (AppServices.Device is null)
        {
            return (false, "adb.exe not found.");
        }
        var dev = await RefreshDeviceAsync(ct);
        var serial = dev?.Serial ?? AppServices.State.DeviceSerial;
        if (serial is null)
        {
            return (false, "Connect the phone (Android with USB debugging, or TWRP) so the installer can identify it, then press Resume.");
        }
        log.Report("Skipping the TWRP flash: TWRP is already on the phone.");
        if (dev is { Mode: DeviceMode.Android })
        {
            log.Report("Restarting the phone into TWRP...");
            await AppServices.Device.RebootAsync(serial, RebootTarget.Recovery, ct);
        }
        else if (dev is not { Mode: DeviceMode.Recovery })
        {
            log.Report("Start TWRP: hold Volume Down + Power until the screen goes off, then immediately hold Volume Up + Bixby + Power.");
        }
        try
        {
            AppServices.CurrentDevice = dev is { Mode: DeviceMode.Recovery }
                ? dev
                : await AppServices.Device.WaitForModeAsync(serial, DeviceMode.Recovery, TimeSpan.FromMinutes(5), log, ct);
        }
        catch (TimeoutException)
        {
            return (false, "TWRP did not come up. Start it with Volume Up + Bixby + Power, then press Resume. "
                + "If the phone shows Samsung's own recovery, turn off \"TWRP is already on the phone\" so the installer flashes it.");
        }
        return await SettleTwrpAsync(log, ct) && await ClearBootRequestAsync(log, ct)
            ? (true, "TWRP is running (not flashed; it was already on the phone).")
            : (false, "TWRP started but isn't answering over USB. Check the cable, then press Resume.");
    }

    /// <summary>Clears any Android boot request left in MISC (e.g. from Android's own reboot to recovery), so later restarts are normal.</summary>
    private static async Task<bool> ClearBootRequestAsync(IProgress<string> log, CancellationToken ct)
    {
        if (AppServices.CurrentDevice is not { } dev || AppServices.Twrp(dev.Serial) is not { } twrp)
        {
            return false;
        }
        await new BootRouteService(twrp).ClearBootRequestAsync(log, ct);
        // Neutralise a stale ui.zip theme override for this boot (baked heal covers the next one).
        await twrp.RemoveStaleThemeOverrideAsync(ct);
        return true;
    }

    /// <summary>Re-reads the connected phone so stages act on its current mode (Android, TWRP, ...).</summary>
    private static async Task<DeviceSnapshot?> RefreshDeviceAsync(CancellationToken ct)
    {
        if (AppServices.Adb is null)
        {
            return AppServices.CurrentDevice;
        }
        var devices = await AppServices.Adb.ListDevicesAsync(ct);
        if (devices.Count == 0 && await AppServices.FindDownloadModeAsync(ct) is (true, var inDownload))
        {
            return AppServices.CurrentDevice = inDownload;
        }
        if (devices.Count != 1)
        {
            return AppServices.CurrentDevice = null;
        }
        var d = devices[0];
        return AppServices.CurrentDevice = DeviceSnapshot.FromAdb(d,
            d.State is AdbState.Device or AdbState.Recovery ? await AppServices.Adb.GetPropertiesAsync(d.Serial, ct) : null);
    }

    /// <summary>TWRP only needs this long after boot before the installer uses it.</summary>
    private const double SettleSeconds = 30;

    /// <summary>
    /// Waits until TWRP has been up for <see cref="SettleSeconds"/> (no wait at all when it has
    /// been up longer) and its shell answers. Nothing needs the SD card: everything is staged in
    /// TWRP's RAM. A shell wedged by the ntfs-3g mount deadlock is broken from here.
    /// </summary>
    private static async Task<bool> SettleTwrpAsync(IProgress<string> log, CancellationToken ct)
    {
        var dev = AppServices.CurrentDevice;
        var twrp = dev is null ? null : AppServices.Twrp(dev.Serial);
        if (twrp is null)
        {
            return false;
        }
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(90);
        var reported = false;
        var reportedHung = false;
        while (DateTime.UtcNow < deadline)
        {
            try
            {
                // A TWRP wedged by the ntfs-3g FUSE self-deadlock never answers a shell. The baked-in
                // on-phone watchdog usually clears it; this breaks it from the host too.
                if (!await twrp.EnsureResponsiveAsync(ct))
                {
                    if (!reportedHung)
                    {
                        log.Report("TWRP isn't answering yet (checking for the NTFS mount deadlock)...");
                        reportedHung = true;
                    }
                    continue;
                }
                var up = await twrp.UptimeSecondsAsync(ct);
                if (up >= SettleSeconds)
                {
                    return true;
                }
                if (!reported)
                {
                    log.Report($"TWRP started {up:0} s ago; giving it {SettleSeconds - up:0} s more...");
                    reported = true;
                }
                await Task.Delay(TimeSpan.FromSeconds(Math.Min(SettleSeconds - up, 5)), ct);
            }
            catch (Exception e) when (e is InvalidOperationException or TimeoutException)
            {
                // adb reconnecting, or a shell timed out; try again shortly.
                await Task.Delay(TimeSpan.FromSeconds(2), ct);
            }
        }
        return false;
    }

    private static async Task<(bool, string)> RunBackupAsync(IProgress<string> log, CancellationToken ct)
    {
        var dev = await RefreshDeviceAsync(ct);
        if (dev is null || dev.Mode != DeviceMode.Recovery)
        {
            return (false, "Boot the phone into TWRP first (the Install TWRP step).");
        }
        var twrp = AppServices.Twrp(dev.Serial);
        if (twrp is null)
        {
            return (false, "adb.exe not found.");
        }
        if (!await SettleTwrpAsync(log, ct))
        {
            return (false, "TWRP isn't answering over USB. Check the cable, then press Resume.");
        }
        var dir = AppServices.BackupFolderFor(dev.Serial);
        var manifest = await new BackupService(twrp).BackupAsync(dir, dev.Model ?? "model-not-reported", dev.Serial, log, ct);
        return (true, $"Backed up {manifest.Partitions.Count} partitions to {dir}. Keep this folder safe.");
    }

    private static async Task<(bool, string)> RunMediaAsync(CancellationToken ct)
    {
        if (AppServices.MediaPath is null || !File.Exists(AppServices.MediaPath))
        {
            return (false, "Choose Windows media on the Windows image page.");
        }
        var media = new WindowsMedia(AppServices.Runner);
        string? install;
        try
        {
            install = await media.ResolveInstallImageAsync(AppServices.MediaPath, ct);
        }
        catch (InvalidOperationException e)
        {
            return (false, e.Message);
        }
        if (install is null)
        {
            return (false, "The media does not contain sources\\install.wim or install.esd. Choose a Windows 11 ARM64 ISO, or its install.wim or install.esd.");
        }
        var editions = await media.GetEditionsAsync(install, ct);
        var chosen = WindowsMedia.ChooseEdition(editions);
        var build = await media.GetBuildAsync(install, chosen.Index, ct);
        AppServices.InstallImagePath = install;
        AppServices.EditionIndex = chosen.Index;
        AppServices.MediaBuild = build;
        var note = "";
        if (AppServices.Toolset.LoadFirmwareCatalog() is { } catalog && catalog.Choose(build) is { Exact: false } nearest)
        {
            // Any build may be installed; only warn that its firmware was made for another build.
            note = $" Not a validated build: it will use the UEFI for Windows {nearest.Image.Windows} (validated: {catalog.SupportedBuilds}), "
                + "so Windows may stop at the Samsung logo.";
            AppServices.Log($"warning: Windows {build ?? "(unknown build)"} has no matching UEFI; using the one for {nearest.Image.Windows}.");
        }
        return (true, $"Using {chosen.Name}{(build is null ? "" : $" {build}")} (index {chosen.Index}) from {Path.GetFileName(install)}.{note}");
    }

    private static async Task<(bool, string)> RunImageAsync(IProgress<string> log, CancellationToken ct)
    {
        if (AppServices.InstallImagePath is null)
        {
            var media = await RunMediaAsync(ct);
            if (!media.Item1)
            {
                return media;
            }
        }
        var drivers = ImageBuilder.DiscoverDrivers(AppServices.DriversDirectory);
        if (drivers.Count == 0)
        {
            return (false, "The phone drivers are not set up. Add them on the Set up page (release download or build folder).");
        }
        var outDir = Path.Combine(AppServices.WorkDirectory, "out");
        var vhdx = Path.Combine(AppServices.WorkDirectory, "s9windows.vhdx");
        if (AppServices.CurrentDevice is null || !PartitionMap.IsKnown(AppServices.CurrentDevice.Codename))
        {
            return (false, "Identify the phone first: the Windows volume is built to the exact geometry of the phone's USERDATA, and that depends on which phone it is.");
        }
        var geometry = PartitionMap.For(AppServices.CurrentDevice.Codename);
        Directory.CreateDirectory(AppServices.WorkDirectory);
        if (File.Exists(vhdx))
        {
            File.Delete(vhdx);
        }
        var built = await new VhdxImageBuilder(AppServices.Runner, null, null, geometry).BuildAsync(vhdx,
            AppServices.InstallImagePath!, AppServices.EditionIndex, drivers, AppServices.Profile,
            AppServices.Unattend, outDir, log, ct);
        AppServices.Built = built;
        var firmwareNote = "";
        if (AppServices.Toolset.LoadFirmwareCatalog() is { } catalog && catalog.Choose(AppServices.MediaBuild, built.LoaderSha256, built.KernelSha256) is { } firmware)
        {
            AppServices.State.FirmwareFile = firmware.Image.File;
            AppServices.SaveState();
            if (firmware.Exact)
            {
                firmwareNote = $", UEFI for Windows {firmware.Image.Windows}";
            }
            else
            {
                firmwareNote = $". No UEFI was built for this Windows (boot loader {built.LoaderSha256[..12]}…, kernel {built.KernelSha256[..12]}…); "
                    + $"using the one for Windows {firmware.Image.Windows}, so Windows may stop at the Samsung logo";
                log.Report($"warning: no UEFI matches this image's boot loader and kernel; using the one for Windows {firmware.Image.Windows}.");
            }
        }
        return (true, $"Windows image built: {built.WindowsBytes >> 20} MiB volume + boot files, {drivers.Count} drivers, {AppServices.Profile} profile{firmwareNote}.");
    }

    private static TwrpClient? RequireTwrp(out DeviceSnapshot? dev, out string error)
    {
        dev = AppServices.CurrentDevice;
        if (dev is null || dev.Mode != DeviceMode.Recovery)
        {
            error = "Boot the phone into TWRP first (the Install TWRP step).";
            return null;
        }
        var twrp = AppServices.Twrp(dev.Serial);
        if (twrp is null)
        {
            error = "adb.exe not found.";
            return null;
        }
        error = "";
        return twrp;
    }

    private static async Task<(bool, string)> RunPartitionAsync(CancellationToken ct)
    {
        await RefreshDeviceAsync(ct);
        var twrp = RequireTwrp(out var dev, out var err);
        if (twrp is null)
        {
            return (false, err);
        }
        var parts = await twrp.ListPartitionsAsync(ct);
        if (!PartitionMap.IsKnown(dev?.Codename))
        {
            return (false, "No partition layout is recorded for this phone, so there is nothing to verify against. Nothing has been written.");
        }
        var geometry = PartitionMap.For(dev?.Codename);
        var required = new List<string>
        {
            PartitionMap.WindowsTarget, PartitionMap.UefiTarget, PartitionMap.EfiSystemPartition,
            PartitionMap.RecoveryTarget, PartitionMap.Misc,
        };
        if (geometry.SecondaryEspPartition is string secondarySlot)
        {
            required.Add(secondarySlot);
        }
        var missing = required.Where(n => !parts.Keys.Contains(n, StringComparer.OrdinalIgnoreCase)).ToList();
        if (missing.Count > 0)
        {
            return (false, $"The phone did not expose the expected partition(s): {string.Join(", ", missing)}.");
        }
        foreach (var (name, expected) in new[] { (PartitionMap.WindowsTarget, geometry.WindowsBytes),
            (PartitionMap.EfiSystemPartition, geometry.CacheBytes) })
        {
            var actual = parts.Keys.First(k => k.Equals(name, StringComparison.OrdinalIgnoreCase));
            var size = await twrp.PartitionSizeAsync(actual, ct);
            if (size != expected)
            {
                return (false, $"{actual} is {size:N0} bytes, not the validated {expected:N0}. This phone's layout differs; stopping before any write.");
            }
        }
        return (true, $"Partitions verified: USERDATA ({geometry.WindowsBytes >> 20} MiB) and the CACHE boot partition match the validated layout.");
    }

    private static async Task<(bool, string)> RunTransferAsync(IProgress<string> log, CancellationToken ct)
    {
        await RefreshDeviceAsync(ct);
        var twrp = RequireTwrp(out var dev, out var err);
        if (twrp is null || dev is null)
        {
            return (false, err);
        }
        if (!PartitionMap.IsKnown(dev.Codename))
        {
            return (false, "No partition layout is recorded for this phone, so nothing was written.");
        }
        var geometry = PartitionMap.For(dev.Codename);
        if (!File.Exists(AppServices.WindowsImagePath))
        {
            return (false, $"No Windows image at {AppServices.WindowsImagePath}. Run the Build the Windows image step first.");
        }
        if (await twrp.IsMountedAsync("/sdcard", ct))
        {
            await twrp.UnmountAsync("/sdcard", ct);
        }
        try
        {
            await new TransferService(twrp).WriteRawImageAsync(PartitionMap.WindowsTarget, AppServices.WindowsImagePath,
                new RawWriteOptions { Verify = AppServices.VerifyWrites }, mountToEnsureUnmounted: "/data", log: log, ct: ct);

            if (Directory.Exists(AppServices.EspDirectory))
            {
                log.Report("Writing the Windows boot files...");
                await new BootFilesService(twrp, geometry).WriteAsync(AppServices.EspDirectory, log, ct);
            }
            if (geometry.SecondaryEspPartition is string secondarySlot)
            {
                await new GptTypeService(twrp).EnsureEspTypeAsync(secondarySlot,
                    Path.Combine(AppServices.BackupFolderFor(dev.Serial), "gpt"), log, ct);
            }
        }
        finally
        {
            // Take the "Installing Windows" screen down once the writing is done.
            await twrp.ClearInstallStatusAsync(CancellationToken.None);
        }
        return (true, AppServices.VerifyWrites
            ? "Windows and boot files written and verified on the phone."
            : "Windows and boot files written to the phone.");
    }

    private static async Task<(bool, string)> RunUefiAsync(IProgress<string> log, CancellationToken ct)
    {
        await RefreshDeviceAsync(ct);
        var twrp = RequireTwrp(out _, out var err);
        if (twrp is null)
        {
            return (false, err);
        }
        string? image;
        var note = "";
        if (AppServices.Toolset.LoadFirmwareCatalog() is { } catalog)
        {
            var firmware = catalog.Images.FirstOrDefault(i => i.File == AppServices.State.FirmwareFile)
                ?? catalog.Choose(AppServices.MediaBuild, AppServices.Built?.LoaderSha256, AppServices.Built?.KernelSha256)?.Image;
            if (firmware is null)
            {
                return (false, "The UEFI catalog has no images. Import the UEFI images again on the Set up page, then press Resume.");
            }
            if (!catalog.Verify(firmware))
            {
                return (false, $"{firmware.File} does not match its checksum. Import the UEFI images again on the Set up page.");
            }
            image = catalog.PathOf(firmware);
            note = $" (for Windows {firmware.Windows})";
            log.Report($"Installing the UEFI build for Windows {firmware.Windows}...");
        }
        else
        {
            image = AppServices.UefiImagePath is { } p && File.Exists(p) ? p : null;
        }
        if (image is null)
        {
            return (false, "The UEFI image is not set up. Add it on the Set up page, then press Resume.");
        }
        await new TransferService(twrp).WriteWholePartitionAsync(PartitionMap.UefiTarget, image, log, ct, verify: AppServices.VerifyWrites);
        await twrp.ClearInstallStatusAsync(CancellationToken.None);
        return (true, $"UEFI installed to BOOT{note}. RECOVERY keeps TWRP.");
    }

    private static async Task<(bool, string)> RunFirstBootAsync(IProgress<string> log, CancellationToken ct)
    {
        var dev = await RefreshDeviceAsync(ct);
        if (dev is null || AppServices.Device is null || AppServices.Adb is null)
        {
            return (true, "Unplug the phone and power it on to start Windows.");
        }
        var modules = AppServices.Toolset.EnsureTwrpModules();
        var tries = 0;
        for (var loop = 1; loop <= MaxStarts * 2; loop++)
        {
            if (loop > 1)
            {
                dev = await RefreshDeviceAsync(ct) ?? dev;
            }
            if (dev.Mode == DeviceMode.Recovery && AppServices.Twrp(dev.Serial) is { } twrp)
            {
                // A leftover boot-recovery request, a pending watchdog record or anything left in the
                // retained startup records would send the phone straight back to TWRP or halt UEFI at
                // the Samsung logo (stock Android, Download mode and power loss all leave some behind).
                var records = await new BootRouteService(twrp).ClearAsync(modules, log, ct);
                SaveStartupRecords(records);
                if (records is { Ready: false })
                {
                    return (false, "The phone's startup records could not be cleared, so Windows would stop at the Samsung logo. "
                        + "Restart TWRP (hold Volume Down + Power, then immediately Volume Up + Bixby + Power) and press Resume.");
                }
                await twrp.ClearInstallStatusAsync(ct);
                await twrp.ShellAsync("umount /data 2>/dev/null; umount /cache 2>/dev/null; umount /sdcard 2>/dev/null; sync; true", ct);
            }
            log.Report(tries == 0 ? "Restarting into Windows..." : $"Starting Windows, attempt {tries + 1} of {MaxStarts}...");
            await AppServices.Device.RebootAsync(dev.Serial, RebootTarget.System, ct);

            var (returned, after, record) = await WatchFirstBootAsync(dev.Serial, modules, log, ct);
            if (!returned)
            {
                return (true, "Windows is starting. The first start and setup take several minutes and restart the phone once. "
                    + "If it stays on the Samsung logo for more than 5 minutes, hold Volume Down + Power until the screen goes off, "
                    + "then immediately hold Volume Up + Bixby + Power to open TWRP, and tap Continue there.");
            }
            if (after < QuickBounce)
            {
                // The firmware's recovery latch: a stale boot owner sends it back in ~25 s without trying
                // Windows. The records are cleared before every start, so this is rare; it doesn't count.
                log.Report("The firmware sent the phone back without trying Windows. Clearing its records and trying again...");
                continue;
            }
            // A real attempt that stalled before the Windows dots: the watchdog returns in about two minutes.
            // Measured on the reference phone, Windows starts on roughly one attempt in four to ten, so retry.
            tries++;
            if (tries >= MaxStarts)
            {
                return (false, $"Windows did not start in {tries} attempts; each time the phone's watchdog returned it to TWRP"
                    + (record is null ? "." : $" (last record: {record}).")
                    + " Press Resume to try again, or open Device tools to collect diagnostics.");
            }
            log.Report($"Windows didn't start this time (attempt {tries} of {MaxStarts}; the watchdog returned the phone after "
                + $"{after.TotalSeconds:0} s). Boot on this phone often takes several tries; trying again...");
        }
        return (false, "The phone kept returning to TWRP before trying Windows. Press Resume to try again.");
    }

    /// <summary>
    /// Real start attempts before giving up. The reference phone historically needed up to about ten
    /// (research: boot succeeds roughly one time in four to ten; failures return in ~122 s).
    /// </summary>
    private const int MaxStarts = 12;

    /// <summary>A return to TWRP faster than this means Windows was never attempted.</summary>
    private static readonly TimeSpan QuickBounce = TimeSpan.FromSeconds(75);

    /// <summary>
    /// Keeps the startup records the installer found (and cleared) in the logs folder, so a start that
    /// still fails can be diagnosed from what the phone held. Only written when something was set.
    /// </summary>
    private static void SaveStartupRecords(StartupClearResult? result)
    {
        if (result is null || result.Before.ReadyToStart)
        {
            return;
        }
        try
        {
            Directory.CreateDirectory(AppServices.LogDirectory);
            var file = Path.Combine(AppServices.LogDirectory, $"startup-records-{DateTime.Now:yyyyMMdd-HHmmss}.txt");
            File.WriteAllLines(file,
            [
                $"recovery record (RWD1): {result.Before.Rwd1Kind}",
                $"  {Convert.ToHexString(result.Before.Rwd1Bytes)}",
                $"startup record (P3): {result.Before.P3Kind}",
                $"  {Convert.ToHexString(result.Before.P3Bytes)}",
                $"cleared: {(result.Cleared.Count == 0 ? "nothing" : string.Join(", ", result.Cleared))}",
                $"read back clear: {result.Ready}",
            ]);
        }
        catch (IOException)
        {
            // Diagnostics only.
        }
    }

    /// <summary>
    /// Windows exposes no adb, so success is the phone staying away for four minutes; a failed start
    /// comes back to TWRP. Returns whether it came back, how long after the restart, and the record.
    /// </summary>
    private static async Task<(bool Returned, TimeSpan After, RecoveryRecord? Record)> WatchFirstBootAsync(
        string serial, string? modules, IProgress<string> log, CancellationToken ct)
    {
        var started = DateTime.UtcNow;
        var gone = false;
        while (DateTime.UtcNow - started < TimeSpan.FromMinutes(4))
        {
            await Task.Delay(TimeSpan.FromSeconds(5), ct);
            var devices = await AppServices.Adb!.ListDevicesAsync(ct);
            var phone = devices.FirstOrDefault(d => d.Serial == serial);
            if (phone is null)
            {
                gone = true;
                continue;
            }
            if (gone && phone.State == AdbState.Recovery)
            {
                var after = DateTime.UtcNow - started;
                log.Report($"The phone came back to TWRP after {after.TotalSeconds:0} s; reading why...");
                await Task.Delay(TimeSpan.FromSeconds(15), ct);
                RecoveryRecord? record = null;
                if (AppServices.Twrp(serial) is { } back)
                {
                    record = await new BootRouteService(back).ReadRecordAsync(modules, ct);
                }
                if (record is not null)
                {
                    log.Report($"Watchdog record: {record}.");
                }
                return (true, after, record);
            }
        }
        return (false, DateTime.UtcNow - started, null);
    }

    private void Report(InfoBarSeverity severity, string title, string message, (string Label, Action Run)? action = null)
    {
        ResultBar.Severity = severity;
        ResultBar.Title = title;
        ResultBar.Message = AppServices.Redact(message);
        ResultBar.ActionButton = null;
        if (action is { } a)
        {
            var button = new Button { Content = a.Label };
            button.Click += (_, _) =>
            {
                ResultBar.IsOpen = false;
                a.Run();
            };
            ResultBar.ActionButton = button;
        }
        ResultBar.IsOpen = true;
    }

    private void OnCancel(object sender, RoutedEventArgs e) => _cts?.Cancel();

    private void OnExperimentalToggled(object sender, RoutedEventArgs e) =>
        AppServices.ExperimentalEnabled = ExperimentalToggle.IsOn;

    private void OnSkipTwrpToggled(object sender, RoutedEventArgs e)
    {
        if (AppServices.SkipTwrpFlash != SkipTwrpToggle.IsOn)
        {
            AppServices.SkipTwrpFlash = SkipTwrpToggle.IsOn;
            AppServices.SaveState();
        }
    }

    private void OnVerifyToggled(object sender, RoutedEventArgs e)
    {
        if (AppServices.VerifyWrites != VerifyToggle.IsOn)
        {
            AppServices.VerifyWrites = VerifyToggle.IsOn;
            AppServices.SaveState();
        }
    }

    private void OnOpenLogs(object sender, RoutedEventArgs e)
    {
        Directory.CreateDirectory(AppServices.LogDirectory);
        Process.Start(new ProcessStartInfo("explorer.exe", AppServices.LogDirectory) { UseShellExecute = true });
    }
}
