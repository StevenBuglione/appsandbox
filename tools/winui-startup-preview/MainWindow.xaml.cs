using System;
using System.IO;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Windows.Graphics;
using Windows.UI;

namespace Linguum.WinUIStartupPreview;

public sealed partial class MainWindow : Window
{
    private readonly DispatcherTimer _copyTimer = new();
    private readonly string[] _friendlyLines =
    [
        "Linguum is finding its feet…",
        "Warming up the cozy bits…",
        "Teaching the pixels where to go…",
        "Making room for something good…"
    ];

    private int _friendlyLine;
    private bool _sidebarExpanded = true;

    public MainWindow()
    {
        InitializeComponent();
        ConfigureWindow();
        StartFriendlyMotion();
    }

    private void ConfigureWindow()
    {
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);

        AppWindow.TitleBar.PreferredHeightOption = TitleBarHeightOption.Standard;
        AppWindow.TitleBar.ButtonBackgroundColor = Colors.Transparent;
        AppWindow.TitleBar.ButtonInactiveBackgroundColor = Colors.Transparent;
        AppWindow.TitleBar.ButtonHoverBackgroundColor = Color.FromArgb(32, 255, 255, 255);
        AppWindow.TitleBar.ButtonPressedBackgroundColor = Color.FromArgb(48, 255, 255, 255);
        AppWindow.TitleBar.ButtonForegroundColor = Colors.White;
        AppWindow.TitleBar.ButtonInactiveForegroundColor = Color.FromArgb(170, 255, 255, 255);
        AppWindow.Resize(new SizeInt32(1100, 720));

        var iconPath = Path.Combine(
            AppContext.BaseDirectory, "Assets", "linguum-orbit-mark.ico");
        if (File.Exists(iconPath))
        {
            AppWindow.SetIcon(iconPath);
        }

        SystemBackdrop = new MicaBackdrop
        {
            Kind = Microsoft.UI.Composition.SystemBackdrops.MicaKind.BaseAlt
        };
    }

    private void StartFriendlyMotion()
    {
        var orbit = new DoubleAnimation
        {
            From = 0,
            To = 360,
            Duration = new Duration(TimeSpan.FromSeconds(1.8)),
            RepeatBehavior = RepeatBehavior.Forever,
            EnableDependentAnimation = false
        };
        Storyboard.SetTarget(orbit, OrbitRotation);
        Storyboard.SetTargetProperty(orbit, "Angle");
        var storyboard = new Storyboard();
        storyboard.Children.Add(orbit);
        storyboard.Begin();

        _copyTimer.Interval = TimeSpan.FromSeconds(2.8);
        _copyTimer.Tick += (_, _) =>
        {
            _friendlyLine = (_friendlyLine + 1) % _friendlyLines.Length;
            FriendlyStatus.Text = _friendlyLines[_friendlyLine];
        };
        _copyTimer.Start();
    }

    private void ToggleSidebar()
    {
        _sidebarExpanded = !_sidebarExpanded;
        SidebarColumn.Width = _sidebarExpanded
            ? new GridLength(236)
            : new GridLength(0);
        Sidebar.Visibility = _sidebarExpanded
            ? Visibility.Visible
            : Visibility.Collapsed;
        SidebarMenuItem.IsChecked = _sidebarExpanded;
    }

    private void ToggleDetails()
    {
        var visible = TechnicalDetails.Visibility != Visibility.Visible;
        TechnicalDetails.Visibility = visible
            ? Visibility.Visible
            : Visibility.Collapsed;
        DetailsMenuItem.IsChecked = visible;
    }

    private void OnSidebarButtonClicked(object sender, RoutedEventArgs args) =>
        ToggleSidebar();

    private void OnSidebarMenuClicked(object sender, RoutedEventArgs args) =>
        ToggleSidebar();

    private void OnDetailsMenuClicked(object sender, RoutedEventArgs args) =>
        ToggleDetails();

    private void OnExitClicked(object sender, RoutedEventArgs args) =>
        Close();

    private async void OnAboutClicked(object sender, RoutedEventArgs args)
    {
        var dialog = new ContentDialog
        {
            XamlRoot = WindowRoot.XamlRoot,
            Title = "Linguum",
            Content = "A native Windows shell for the Linguum framework.",
            CloseButtonText = "Done"
        };
        await dialog.ShowAsync();
    }
}
