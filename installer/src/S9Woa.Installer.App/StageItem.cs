// SPDX-License-Identifier: BSD-2-Clause-Patent
using System.ComponentModel;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;
using S9Woa.Installer.Core.Stages;

namespace S9Woa.Installer.App;

[Microsoft.UI.Xaml.Data.Bindable]
public sealed partial class StageItem(StageDefinition definition) : INotifyPropertyChanged
{
    private StageStatus _status;

    public event PropertyChangedEventHandler? PropertyChanged;

    public StageDefinition Definition => definition;
    public string Title => definition.Title;
    public string Summary => definition.Summary;

    /// <summary>
    /// The codename of the phone the page showing this item is working with.
    /// Availability is per device, so the badge has to be resolved against it;
    /// reading <c>definition.Availability</c> alone would show the 9810's states
    /// for an S10e and let the 9810 stage code run against it.
    /// </summary>
    public string? DeviceCodename;

    private StageAvailability Resolved => StageCatalog.Resolve(definition.Id, DeviceCodename);

    public string Badge => Resolved switch
    {
        StageAvailability.Guided => "You do this step",
        StageAvailability.Experimental => "Experimental",
        StageAvailability.NotImplemented => "Coming soon",
        _ => definition.Destructive ? "Writes to the phone" : "",
    };

    public Visibility BadgeVisibility => Badge.Length == 0 ? Visibility.Collapsed : Visibility.Visible;

    /// <summary>What the step does, for the compact list's tooltip.</summary>
    public string Tip => Badge.Length == 0 ? Summary : $"{Summary} ({Badge}.)";

    public StageStatus Status
    {
        get => _status;
        set
        {
            _status = value;
            foreach (var n in new[] { nameof(Status), nameof(Glyph), nameof(Brush), nameof(RingActive), nameof(TitleWeight), nameof(TitleBrush) })
            {
                PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(n));
            }
        }
    }

    public Windows.UI.Text.FontWeight TitleWeight =>
        _status == StageStatus.Running ? Microsoft.UI.Text.FontWeights.SemiBold : Microsoft.UI.Text.FontWeights.Normal;

    public Brush TitleBrush => (Brush)Application.Current.Resources[_status switch
    {
        StageStatus.Running => "AccentTextFillColorPrimaryBrush",
        StageStatus.Pending => "TextFillColorSecondaryBrush",
        _ => "TextFillColorPrimaryBrush",
    }];

    public bool RingActive => _status == StageStatus.Running;

    public string Glyph => _status switch
    {
        StageStatus.Done => "\uE73E",
        StageStatus.Failed => "\uE711",
        StageStatus.Skipped => "\uE8D8",
        StageStatus.Running => "",
        _ => "\uEA3A",
    };

    public Brush Brush => (Brush)Application.Current.Resources[_status switch
    {
        StageStatus.Done => "SystemFillColorSuccessBrush",
        StageStatus.Failed => "SystemFillColorCriticalBrush",
        _ => "TextFillColorTertiaryBrush",
    }];
}
