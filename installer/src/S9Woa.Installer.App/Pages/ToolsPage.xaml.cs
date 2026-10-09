// SPDX-License-Identifier: BSD-2-Clause-Patent
using System.Diagnostics;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using S9Woa.Installer.Core.Device;
using S9Woa.Installer.Core.Recovery;
using S9Woa.Installer.Core.Stages;

namespace S9Woa.Installer.App.Pages;

public sealed partial class ToolsPage : Page
{
    public ToolsPage()
    {
        InitializeComponent();
        Loaded += async (_, _) => await RefreshAsync();
    }

    private async Task RefreshAsync()
    {
        try
        {
            if (AppServices.Adb is not null)
            {
                var devices = await AppServices.Adb.ListDevicesAsync();
                if (devices.Count == 1)
                {
                    var d = devices[0];
                    AppServices.CurrentDevice = DeviceSnapshot.FromAdb(d,
                        d.State is AdbState.Device or AdbState.Recovery ? await AppServices.Adb.GetPropertiesAsync(d.Serial) : null);
                }
                else
                {
                    AppServices.CurrentDevice = null;
                }
            }
        }
        catch (Exception e) when (e is InvalidOperationException or TimeoutException)
        {
            AppServices.CurrentDevice = null;
        }
        var s = AppServices.CurrentDevice;
        DeviceLine.Text = s is null
            ? "No phone connected over ADB. These tools work in Android (with USB debugging) and in TWRP."
            : $"{s.Model ?? "Android device"} in {s.Mode}{(s.RecoveryVersion is null ? "" : $" (TWRP {s.RecoveryVersion})")}";
    }

    private async void OnRecovery(object sender, RoutedEventArgs e) => await DeviceCommands.RebootAsync(XamlRoot, RebootTarget.Recovery);

    private async void OnDownload(object sender, RoutedEventArgs e) => await DeviceCommands.RebootAsync(XamlRoot, RebootTarget.Download);

    private async void OnSystem(object sender, RoutedEventArgs e) => await DeviceCommands.RebootAsync(XamlRoot, RebootTarget.System);

    private async void OnRestartToTwrp(object sender, RoutedEventArgs e)
    {
        TwrpTicketText.Text = "Looking for the phone's UFS storage driver...";
        TicketDevice? device;
        try
        {
            device = await Task.Run(() => AppServices.RecoveryTickets.FindDevice());
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            TwrpTicketText.Text = AppServices.Redact($"Could not scan the disks: {ex.Message}");
            return;
        }
        if (device is null)
        {
            TwrpTicketText.Text = "This only works while running Windows on a supported phone with its UFS "
                + "storage driver installed. The recovery-ticket protocol is ported for the Galaxy S9+ only - it has "
                + "not been derived for the S10e. No compatible disk answered on this PC.";
            return;
        }

        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Restart to TWRP now?",
            Content = "Windows will restart and boot into TWRP recovery instead of Windows. "
                + "Save your work and close other apps first.",
            PrimaryButtonText = "Restart to TWRP",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Close,
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary)
        {
            TwrpTicketText.Text = "Cancelled.";
            return;
        }

        try
        {
            AppServices.Log($"arming UCR1 recovery ticket on PhysicalDrive{device.PhysicalDrive}");
            TwrpTicketText.Text = "Scheduling the recovery restart...";
            await Task.Run(() => AppServices.RecoveryTickets.RestartToRecovery());
        }
        catch (RecoveryTicketException ex)
        {
            AppServices.Log($"recovery ticket failed: {ex.Message}");
            TwrpTicketText.Text = AppServices.Redact(ex.Message);
        }
    }

    private async void OnSaveInfo(object sender, RoutedEventArgs e)
    {
        await RefreshAsync();
        var s = AppServices.CurrentDevice;
        if (AppServices.Adb is null || s is null)
        {
            DiagText.Text = "Connect the phone first.";
            return;
        }
        var props = await AppServices.Adb.GetPropertiesAsync(s.Serial);
        Directory.CreateDirectory(AppServices.LogDirectory);
        var file = Path.Combine(AppServices.LogDirectory, $"phone-{DateTime.Now:yyyyMMdd-HHmmss}.txt");
        await File.WriteAllLinesAsync(file, props.OrderBy(p => p.Key, StringComparer.Ordinal).Select(p => $"[{p.Key}]: [{p.Value}]"));
        DiagText.Text = $"Saved to {file}. It contains your phone's serial number; review it before sharing.";
        AppServices.Log($"saved phone properties to {file}");
    }

    private void OnOpenLogs(object sender, RoutedEventArgs e)
    {
        Directory.CreateDirectory(AppServices.LogDirectory);
        Process.Start(new ProcessStartInfo("explorer.exe", AppServices.LogDirectory) { UseShellExecute = true });
    }

    private async void OnReset(object sender, RoutedEventArgs e)
    {
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Reset installer progress?",
            Content = "Finished steps will run again next time. Backups in the working folder are kept.",
            PrimaryButtonText = "Reset",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Close,
        };
        if (await dialog.ShowAsync() == ContentDialogResult.Primary)
        {
            AppServices.State.Stages.Clear();
            AppServices.State.DeviceSerial = null;
            AppServices.State.DeviceBootloader = null;
            AppServices.SaveState();
            AppServices.Log("installer progress reset");
        }
    }
}
