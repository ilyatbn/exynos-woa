// SPDX-License-Identifier: BSD-2-Clause-Patent
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Animation;
using Microsoft.UI.Xaml.Navigation;
using S9Woa.Installer.App.Pages;

namespace S9Woa.Installer.App;

/// <summary>
/// Wizard shell: a step rail, one page at a time, and a fixed footer with Back and a single
/// primary action. Pages implement <see cref="IWizardStep"/> to gate and label that action.
/// </summary>
public sealed partial class MainWindow : Window
{
    private readonly List<WizardStepItem> _steps =
    [
        new(0, "welcome", "Welcome", typeof(WelcomePage)),
        new(1, "setup", "Set up", typeof(SetupPage)),
        new(2, "host", "This PC", typeof(HostPage)),
        new(3, "phone", "Your phone", typeof(PhonePage)),
        new(4, "image", "Windows", typeof(ImagePage)),
        new(5, "install", "Install", typeof(InstallPage)),
    ];

    private int _current = -1;
    private int _furthest;
    private bool _onSidePage;
    private IWizardStep? _page;

    public MainWindow()
    {
        InitializeComponent();
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);
        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "AppIcon.ico"));
        AppWindow.Resize(new Windows.Graphics.SizeInt32(1280, 860));
        _steps[^1].IsLast = true;
        StepList.ItemsSource = _steps;
        AppServices.CurrentDeviceChanged += () => DispatcherQueue.TryEnqueue(UpdatePhoneCard);
        UpdatePhoneCard();
#if DEBUG
        // UI development only: reach every step without completing the earlier ones.
        if (Environment.GetCommandLineArgs().Contains("--unlock-all-steps"))
        {
            _furthest = _steps.Count - 1;
        }
#endif
        GoTo(0);
    }

    /// <summary>Jumps to a step (if already reached) or a side page by tag.</summary>
    public void NavigateTo(string tag)
    {
        switch (tag)
        {
            case "tools":
                ShowSidePage(typeof(ToolsPage));
                return;
            case "about":
                ShowSidePage(typeof(AboutPage));
                return;
        }
        var step = _steps.FirstOrDefault(s => s.Tag == tag);
        if (step is not null && step.Index <= _furthest)
        {
            GoTo(step.Index);
        }
    }

    private void GoTo(int index)
    {
        if (index == _current && !_onSidePage)
        {
            return;
        }
        var forward = index >= _current && !_onSidePage;
        _current = index;
        _furthest = Math.Max(_furthest, index);
        _onSidePage = false;
        UpdateRail();
        ContentFrame.Navigate(_steps[index].Page, null, new SlideNavigationTransitionInfo
        {
            Effect = forward ? SlideNavigationTransitionEffect.FromRight : SlideNavigationTransitionEffect.FromLeft,
        });
    }

    private void ShowSidePage(Type page)
    {
        if (_page is { CanGoBack: false } || ContentFrame.CurrentSourcePageType == page)
        {
            return;
        }
        _onSidePage = true;
        ContentFrame.Navigate(page, null, new DrillInNavigationTransitionInfo());
    }

    private void UpdateRail()
    {
        foreach (var s in _steps)
        {
            s.State = s.Index == _current ? StepState.Current
                : s.Index < _current || s.Index <= _furthest ? StepState.Done
                : StepState.Upcoming;
            s.Reachable = s.Index <= _furthest && s.Index != _current;
        }
    }

    private void OnNavigated(object sender, NavigationEventArgs e)
    {
        if (_page is not null)
        {
            _page.StateChanged -= OnPageStateChanged;
        }
        _page = e.Content as IWizardStep;
        if (_page is not null)
        {
            _page.StateChanged += OnPageStateChanged;
        }
        UpdateFooter();
    }

    private void OnPageStateChanged(object? sender, EventArgs e) => DispatcherQueue.TryEnqueue(UpdateFooter);

    private void UpdateFooter()
    {
        if (_onSidePage)
        {
            StepCaption.Text = "";
            BackButton.Content = "Back to installer";
            BackButton.Visibility = Visibility.Visible;
            BackButton.IsEnabled = true;
            NextButton.Visibility = Visibility.Collapsed;
            StepList.IsHitTestVisible = true;
            return;
        }
        var canLeave = _page?.CanGoBack ?? true;
        StepCaption.Text = $"Step {_current + 1} of {_steps.Count}";
        BackButton.Content = "Back";
        BackButton.Visibility = _current > 0 ? Visibility.Visible : Visibility.Collapsed;
        BackButton.IsEnabled = canLeave;
        var label = _page?.NextLabel;
        var isLast = _current == _steps.Count - 1;
        NextButton.Visibility = isLast && label is null ? Visibility.Collapsed : Visibility.Visible;
        NextButton.Content = label ?? "Continue";
        NextButton.IsEnabled = _page?.CanAdvance ?? true;
        StepList.IsHitTestVisible = canLeave;
        ToolsLink.IsEnabled = canLeave;
        AboutLink.IsEnabled = canLeave;
    }

    private async void OnNext(object sender, RoutedEventArgs e)
    {
        if (_page is null)
        {
            return;
        }
        NextButton.IsEnabled = false;
        var advance = await _page.OnAdvanceAsync();
        if (advance && _current < _steps.Count - 1)
        {
            GoTo(_current + 1);
        }
        else
        {
            UpdateFooter();
        }
    }

    private void OnBack(object sender, RoutedEventArgs e)
    {
        if (_onSidePage)
        {
            GoTo(_current);
        }
        else if (_current > 0)
        {
            GoTo(_current - 1);
        }
    }

    private void OnStepClick(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement { Tag: int index } && index <= _furthest && (_page?.CanGoBack ?? true))
        {
            GoTo(index);
        }
    }

    private void UpdatePhoneCard()
    {
        if (AppServices.CurrentDevice is not { } d)
        {
            PhoneTitle.Text = "No phone yet";
            PhoneDetail.Text = "Connected on step 4";
            PhoneGlyph.Foreground = (Microsoft.UI.Xaml.Media.Brush)Application.Current.Resources["TextFillColorTertiaryBrush"];
            return;
        }
        PhoneTitle.Text = AppServices.PhoneDisplayName(d);
        PhoneDetail.Text = string.Join(" · ", new[] { d.Model, d.Bootloader }.Where(s => !string.IsNullOrEmpty(s)));
        PhoneGlyph.Foreground = (Microsoft.UI.Xaml.Media.Brush)Application.Current.Resources["AccentTextFillColorPrimaryBrush"];
    }

    private void OnTools(object sender, RoutedEventArgs e) => ShowSidePage(typeof(ToolsPage));

    private void OnAbout(object sender, RoutedEventArgs e) => ShowSidePage(typeof(AboutPage));
}
