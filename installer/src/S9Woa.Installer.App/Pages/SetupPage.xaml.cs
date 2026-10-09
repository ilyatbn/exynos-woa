// SPDX-License-Identifier: BSD-2-Clause-Patent
using System.Diagnostics;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.Windows.Storage.Pickers;
using S9Woa.Installer.Core.Toolset;

namespace S9Woa.Installer.App.Pages;

/// <summary>
/// First-run toolset configuration. Installs programs with winget, fetches
/// verified UEFI/driver payloads, runs the signed Samsung driver installer, and
/// takes the user's TWRP download. The install flow stays locked until every
/// required tool is ready.
/// </summary>
public sealed partial class SetupPage : Page, IWizardStep
{
    private readonly List<ToolRow> _rows = Tools.All.Select(t => new ToolRow(t)).ToList();
    private readonly Progress<string> _log = new(AppServices.Log);
    private bool _running;
    private bool _complete;

    public SetupPage()
    {
        InitializeComponent();
        ToolList.ItemsSource = _rows;
        RepoBox.Text = AppServices.Toolset.Config.ReleaseRepo;
        WingetBar.IsOpen = !AppServices.Toolset.WingetAvailable;
        Refresh();
    }

    public event EventHandler? StateChanged;

    public bool CanAdvance => _complete && !_running;

    public bool CanGoBack => !_running;

    public Task<bool> OnAdvanceAsync()
    {
        AppServices.Toolset.Config.SetupCompleted = true;
        AppServices.Toolset.SaveConfig();
        AppServices.Log("setup completed");
        return Task.FromResult(true);
    }

    private ToolRow Row(object sender) => _rows.First(r => r.Id == (string)((FrameworkElement)sender).Tag);

    private void Refresh()
    {
        var statuses = AppServices.Toolset.DetectAll();
        foreach (var row in _rows)
        {
            if (!row.Busy)
            {
                row.Status = statuses[row.Id];
            }
        }
        AppServices.ReloadTools();

        var complete = ToolsetManager.IsComplete(statuses);
        var pending = Tools.All.Where(t => t.Required && statuses[t.Id].State != ToolState.Ready).Select(t => t.Name).ToList();
        SummaryTitle.Text = complete ? "Everything is ready" : pending.Count == 1 ? "1 item still needed" : $"{pending.Count} items still needed";
        SummaryText.Text = complete
            ? "All tools and files are in place."
            : "Set up automatically installs the programs and downloads the verified boot files. Still needed: " + string.Join(", ", pending) + ".";
        AutoButton.IsEnabled = !_running && !complete;
        BuildFolderText.Text = AppServices.Toolset.Config.BuildFolder is { } f
            ? AppServices.Redact($"Local build folder: {f}")
            : "No local build folder. UEFI and drivers come from the latest release.";
        _complete = complete;
        StateChanged?.Invoke(this, EventArgs.Empty);
    }

    private async Task RunAsync(ToolRow? row, string what, Func<Task<string?>> action)
    {
        _running = true;
        if (row is not null)
        {
            row.Busy = true;
        }
        AutoProgress.Visibility = row is null ? Visibility.Visible : Visibility.Collapsed;
        ResultBar.IsOpen = false;
        Refresh();
        try
        {
            var problem = await action();
            if (problem is not null)
            {
                Report(InfoBarSeverity.Error, what, problem);
            }
        }
        catch (Exception e) when (e is InvalidOperationException or HttpRequestException or IOException
                                      or TimeoutException or UnauthorizedAccessException or ArgumentException)
        {
            AppServices.Log($"setup {what}: {e.Message}");
            Report(InfoBarSeverity.Error, what, e.Message);
        }
        finally
        {
            if (row is not null)
            {
                row.Busy = false;
            }
            _running = false;
            AutoProgress.Visibility = Visibility.Collapsed;
            Refresh();
        }
    }

    private static string? ProblemOf(ToolStatus s) => s.State is ToolState.Error or ToolState.Missing ? s.Detail : null;

    private void Report(InfoBarSeverity severity, string title, string message)
    {
        ResultBar.Severity = severity;
        ResultBar.Title = title;
        ResultBar.Message = AppServices.Redact(message);
        ResultBar.IsOpen = true;
    }

    private async void OnAuto(object sender, RoutedEventArgs e) => await RunAsync(null, "Automatic setup", async () =>
    {
        var result = await AppServices.Toolset.AutoSetupAsync(_log);
        var needsUser = Tools.All.Where(t => t.Required && result[t.Id].State != ToolState.Ready).Select(t => t.Name).ToList();
        if (needsUser.Count == 0)
        {
            Report(InfoBarSeverity.Success, "Setup complete", "Everything is installed. Press Continue.");
        }
        else
        {
            Report(InfoBarSeverity.Informational, "A few items need you",
                string.Join(", ", needsUser) + " need a download from the vendor's page. Use the buttons on each item.");
        }
        return null;
    });

    private async void OnWinget(object sender, RoutedEventArgs e)
    {
        var row = Row(sender);
        await RunAsync(row, row.Name, async () => ProblemOf(await AppServices.Toolset.InstallWingetAsync(row.Id, _log)));
    }

    private async void OnRelease(object sender, RoutedEventArgs e)
    {
        var row = Row(sender);
        await RunAsync(row, row.Name, async () => ProblemOf(await AppServices.Toolset.DownloadReleaseAsync(row.Id, _log)));
    }

    private async void OnPickFile(object sender, RoutedEventArgs e)
    {
        var row = Row(sender);
        var picker = new FileOpenPicker(App.Window!.AppWindow.Id);
        picker.FileTypeFilter.Add(row.Definition.Kind == ToolKind.Payload ? ".img" : ".exe");
        var file = await picker.PickSingleFileAsync();
        if (file is null)
        {
            return;
        }
        await RunAsync(row, row.Name, async () => ProblemOf(await AppServices.Toolset.UseFileAsync(row.Id, file.Path, _log)));
    }

    private async void OnBuildFolder(object sender, RoutedEventArgs e)
    {
        var row = Row(sender);
        var picker = new FolderPicker(App.Window!.AppWindow.Id);
        var folder = await picker.PickSingleFolderAsync();
        if (folder is null)
        {
            return;
        }
        await RunAsync(row, "Build folder", async () =>
        {
            var results = await Task.Run(() => AppServices.Toolset.UseBuildFolder(folder.Path, _log));
            var problems = results.Values.Select(ProblemOf).OfType<string>().ToList();
            return problems.Count == 0 ? null : string.Join(" ", problems);
        });
    }

    private async void OnOpenPage(object sender, RoutedEventArgs e)
    {
        var definition = Row(sender).Definition;
        var codename = AppServices.CurrentDevice?.Codename;
        // The TWRP page is per phone: opening the S9+ page for an S10e offers the
        // user a build that will not boot this device.
        var url = definition.Id == Tools.Twrp && TwrpProfiles.IsKnown(codename)
            ? TwrpProfiles.For(codename).PageUrl
            : definition.PageUrl;
        if (url is null)
        {
            return;
        }
        AppServices.Log($"opening {url}");
        var launched = false;
        try
        {
            launched = await Windows.System.Launcher.LaunchUriAsync(new Uri(url));
        }
        catch (Exception ex) when (ex is InvalidOperationException or UnauthorizedAccessException or System.Runtime.InteropServices.COMException)
        {
            launched = false;
        }
        if (!launched)
        {
            Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
        }
    }

    private void OnLaunchZadig(object sender, RoutedEventArgs e)
    {
        if (AppServices.ZadigPath is null)
        {
            Report(InfoBarSeverity.Warning, "Zadig is not installed", "Install Zadig above first.");
            return;
        }
        Report(InfoBarSeverity.Informational, "Bind the Download-mode driver",
            "With the phone in Download mode: in Zadig choose Options > List All Devices, select the Samsung device "
            + "(USB ID 04E8 685D), choose WinUSB as the target driver and click Replace Driver. Then press Check again.");
        Process.Start(new ProcessStartInfo(AppServices.ZadigPath) { UseShellExecute = true });
    }

    private void OnRepoChanged(object sender, RoutedEventArgs e)
    {
        var repo = RepoBox.Text.Trim();
        if (repo.Length > 0 && repo != AppServices.Toolset.Config.ReleaseRepo)
        {
            AppServices.Toolset.Config.ReleaseRepo = repo;
            AppServices.Toolset.SaveConfig();
            AppServices.Log($"release repository set to {repo}");
        }
    }

    private void OnRecheck(object sender, RoutedEventArgs e) => Refresh();
}
