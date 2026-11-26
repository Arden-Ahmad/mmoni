using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Security;
using System.Security.Principal;
using System.Threading;
using System.Windows;
using System.Windows.Threading;
using Microsoft.Win32;
using Mmoni.Monitoring;
using Mmoni.Platform;
using Forms = System.Windows.Forms;

namespace Mmoni;

public partial class App : Application
{
    private UserSettings _settings = new();
    private MainWindow? _overlay;
    private SensorMonitor? _monitor;
    private DispatcherTimer? _fullscreenTimer;
    private Forms.NotifyIcon? _tray;
    private Forms.ContextMenuStrip? _menu;
    private Icon? _icon;
    private Mutex? _instance;
    private bool _ownsInstance;
    private bool _exiting;
    private bool _userHidden;
    private bool _fullscreenHidden;
    private bool _sessionLocked;
    private IReadOnlyList<GpuDevice> _gpus = [];

    protected override void OnStartup(StartupEventArgs args)
    {
        base.OnStartup(args);

        using var identity = WindowsIdentity.GetCurrent();
        var userSid = identity.User?.Value ?? "CurrentUser";
        var elevated = new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
        _instance = new Mutex(false, @"Local\Mmoni-" + userSid);
        try
        {
            var timeout = args.Args.Contains("--wait-for-instance") ? TimeSpan.FromSeconds(15) : TimeSpan.Zero;
            _ownsInstance = _instance.WaitOne(timeout);
        }
        catch (AbandonedMutexException)
        {
            _ownsInstance = true;
        }

        if (!_ownsInstance)
        {
            Shutdown();
            return;
        }

        _settings = UserSettings.Load();
        _overlay = new MainWindow(_settings, elevated);
        MainWindow = _overlay;
        _overlay.OverlayMoved += point => SaveSettings(_settings with { Left = point.X, Top = point.Y });
        _overlay.TimeRangeChanged += range => SaveSettings(_settings with { HistoryRange = range });
        _overlay.Closing += (_, closing) =>
        {
            if (!_exiting)
            {
                closing.Cancel = true;
                _userHidden = true;
                SyncVisibilityAndPolling();
            }
        };

        SystemEvents.SessionSwitch += OnSessionSwitch;

        _fullscreenTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(1500) };
        _fullscreenTimer.Tick += (_, _) => CheckFullscreenAutoHide();
        _fullscreenTimer.Start();

        CreateTray(elevated);
        _overlay.Show();

        _monitor = new SensorMonitor(_settings.GpuId, snapshot =>
        {
            if (_exiting)
                return;

            Dispatcher.InvokeAsync(() =>
            {
                if (_exiting)
                    return;

                if (snapshot.Error is null)
                    _gpus = snapshot.Gpus;

                _overlay.Display(snapshot);
            });
        });
    }

    private void CreateTray(bool elevated)
    {
        using var stream = GetResourceStream(new Uri("Assets/Mmoni.ico", UriKind.Relative))!.Stream;
        using var loadedIcon = new Icon(stream);
        _icon = (Icon)loadedIcon.Clone();
        _menu = new Forms.ContextMenuStrip();

        var visibility = new Forms.ToolStripMenuItem("Show overlay", null, (_, _) => ToggleOverlay());

        // Visual Theme submenu
        var themeMenu = new Forms.ToolStripMenuItem("Visual style");
        var themeGlass = new Forms.ToolStripMenuItem("Liquid Glass (Blur)", null, (_, _) => UpdateSettings(_settings with { Theme = VisualTheme.LiquidGlass }));
        var themePrecision = new Forms.ToolStripMenuItem("Precision Dark (No Blur)", null, (_, _) => UpdateSettings(_settings with { Theme = VisualTheme.PrecisionDark }));
        themeMenu.DropDownItems.AddRange([themeGlass, themePrecision]);

        // Collapsed pill metrics submenu
        var pillMenu = new Forms.ToolStripMenuItem("Minimal pill metrics");
        var pillTempPower = new Forms.ToolStripMenuItem("Temp + Power", null, (_, _) => UpdateSettings(_settings with { CollapsedMode = CollapsedDisplayMode.TempPower }));
        var pillTempLoad = new Forms.ToolStripMenuItem("Temp + Load", null, (_, _) => UpdateSettings(_settings with { CollapsedMode = CollapsedDisplayMode.TempLoad }));
        var pillTempOnly = new Forms.ToolStripMenuItem("Temp only (Compact)", null, (_, _) => UpdateSettings(_settings with { CollapsedMode = CollapsedDisplayMode.TempOnly }));
        pillMenu.DropDownItems.AddRange([pillTempPower, pillTempLoad, pillTempOnly]);

        // History range submenu
        var rangeMenu = new Forms.ToolStripMenuItem("History range");
        var range60s = new Forms.ToolStripMenuItem("60 seconds", null, (_, _) => UpdateSettings(_settings with { HistoryRange = TimeRange.Seconds60 }));
        var range5m = new Forms.ToolStripMenuItem("5 minutes", null, (_, _) => UpdateSettings(_settings with { HistoryRange = TimeRange.Minutes5 }));
        var range15m = new Forms.ToolStripMenuItem("15 minutes", null, (_, _) => UpdateSettings(_settings with { HistoryRange = TimeRange.Minutes15 }));
        rangeMenu.DropDownItems.AddRange([range60s, range5m, range15m]);

        var autoExpand = new Forms.ToolStripMenuItem("Hover Peek (Auto-expand)", null, (_, _) =>
            UpdateSettings(_settings with { AutoExpandOnHover = !_settings.AutoExpandOnHover }));

        var autoHideFs = new Forms.ToolStripMenuItem("Auto-hide in fullscreen", null, (_, _) =>
        {
            UpdateSettings(_settings with { AutoHideInFullscreen = !_settings.AutoHideInFullscreen });
            CheckFullscreenAutoHide();
        });

        var clickThrough = new Forms.ToolStripMenuItem("Click-through", null, (_, _) =>
            UpdateSettings(_settings with { ClickThrough = !_settings.ClickThrough }));

        var resetPosition = new Forms.ToolStripMenuItem("Reset position", null, (_, _) =>
        {
            _overlay!.ResetPosition();
            SaveSettings(_settings with { Left = null, Top = null });
        });

        var resetPeaks = new Forms.ToolStripMenuItem("Reset session peaks", null, (_, _) => _overlay!.ResetPeaks());
        var selectGpu = new Forms.ToolStripMenuItem("GPU");
        var startup = new Forms.ToolStripMenuItem("Start with Windows", null, (_, _) => ToggleStartup());
        var elevate = new Forms.ToolStripMenuItem("Restart as administrator", null, (_, _) => RestartElevated())
        {
            Enabled = !elevated
        };
        var exit = new Forms.ToolStripMenuItem("Exit Mmoni", null, (_, _) => ExitMmoni());

        _menu.Items.AddRange([
            visibility,
            new Forms.ToolStripSeparator(),
            themeMenu,
            pillMenu,
            rangeMenu,
            selectGpu,
            new Forms.ToolStripSeparator(),
            autoExpand,
            autoHideFs,
            clickThrough,
            resetPosition,
            resetPeaks,
            new Forms.ToolStripSeparator(),
            startup,
            elevate,
            new Forms.ToolStripSeparator(),
            exit
        ]);

        _menu.Opening += (_, _) =>
        {
            visibility.Checked = !_userHidden;
            themeGlass.Checked = _settings.Theme == VisualTheme.LiquidGlass;
            themePrecision.Checked = _settings.Theme == VisualTheme.PrecisionDark;
            pillTempPower.Checked = _settings.CollapsedMode == CollapsedDisplayMode.TempPower;
            pillTempLoad.Checked = _settings.CollapsedMode == CollapsedDisplayMode.TempLoad;
            pillTempOnly.Checked = _settings.CollapsedMode == CollapsedDisplayMode.TempOnly;
            range60s.Checked = _settings.HistoryRange == TimeRange.Seconds60;
            range5m.Checked = _settings.HistoryRange == TimeRange.Minutes5;
            range15m.Checked = _settings.HistoryRange == TimeRange.Minutes15;
            autoExpand.Checked = _settings.AutoExpandOnHover;
            autoHideFs.Checked = _settings.AutoHideInFullscreen;
            clickThrough.Checked = _settings.ClickThrough;
            startup.Checked = ReadStartupSetting();
            PopulateGpuMenu(selectGpu);
        };

        _tray = new Forms.NotifyIcon { Text = "Mmoni", Icon = _icon, ContextMenuStrip = _menu, Visible = true };
        _tray.MouseDoubleClick += (_, args) =>
        {
            if (args.Button == Forms.MouseButtons.Left)
                ToggleOverlay();
        };
    }

    private void UpdateSettings(UserSettings updated)
    {
        SaveSettings(updated);
        _overlay?.ApplySettings(updated);
    }

    private void PopulateGpuMenu(Forms.ToolStripMenuItem menuItem)
    {
        foreach (var item in menuItem.DropDownItems.Cast<Forms.ToolStripItem>().ToArray())
            item.Dispose();

        menuItem.DropDownItems.Clear();
        var currentGpu = _gpus.FirstOrDefault(gpu => gpu.Id == _settings.GpuId);
        menuItem.DropDownItems.Add(new Forms.ToolStripMenuItem("Automatic", null, (_, _) => SelectGpu(null))
        {
            Checked = currentGpu is null
        });

        foreach (var gpu in _gpus)
        {
            menuItem.DropDownItems.Add(new Forms.ToolStripMenuItem(gpu.Name, null, (_, _) => SelectGpu(gpu.Id))
            {
                Checked = gpu == currentGpu
            });
        }
    }

    private void SelectGpu(string? id)
    {
        _monitor?.SelectGpu(id);
        SaveSettings(_settings with { GpuId = id });
    }

    private void ToggleOverlay()
    {
        _userHidden = !_userHidden;
        SyncVisibilityAndPolling();
    }

    private void CheckFullscreenAutoHide()
    {
        if (_exiting || _overlay is null)
            return;

        var shouldHide = _settings.AutoHideInFullscreen && OverlayWindow.IsFullscreenAppActive();
        if (shouldHide == _fullscreenHidden)
            return;

        _fullscreenHidden = shouldHide;
        SyncVisibilityAndPolling();
    }

    private void OnSessionSwitch(object sender, SessionSwitchEventArgs e)
    {
        if (e.Reason is SessionSwitchReason.SessionLock or SessionSwitchReason.ConsoleDisconnect)
            _sessionLocked = true;
        else if (e.Reason is SessionSwitchReason.SessionUnlock or SessionSwitchReason.ConsoleConnect)
            _sessionLocked = false;

        Dispatcher.InvokeAsync(SyncVisibilityAndPolling);
    }

    private void SyncVisibilityAndPolling()
    {
        if (_exiting || _overlay is null)
            return;

        var shouldBeVisible = !_userHidden && !_fullscreenHidden && !_sessionLocked;
        _monitor?.SetPaused(!shouldBeVisible);

        if (shouldBeVisible)
        {
            if (!_overlay.IsVisible)
            {
                _overlay.Show();
                _overlay.PlaceOverlay();
            }
        }
        else if (_overlay.IsVisible)
        {
            _overlay.Hide();
        }
    }

    private bool ReadStartupSetting()
    {
        try
        {
            return UserSettings.IsStartupEnabled();
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or SecurityException)
        {
            Trace.TraceWarning("Mmoni startup setting could not be read: {0}", exception.Message);
            return false;
        }
    }

    private void ToggleStartup()
    {
        try
        {
            UserSettings.SetStartupEnabled(!UserSettings.IsStartupEnabled());
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or SecurityException)
        {
            NotifyWarning($"Could not change Windows startup. {exception.Message}");
        }
    }

    private void SaveSettings(UserSettings settings)
    {
        _settings = settings;
        try
        {
            settings.Save();
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            NotifyWarning($"Could not save overlay settings. {exception.Message}");
        }
    }

    private void RestartElevated()
    {
        try
        {
            using var elevated = Process.Start(new ProcessStartInfo(Environment.ProcessPath!)
            {
                UseShellExecute = true,
                Verb = "runas",
                Arguments = "--wait-for-instance"
            });
            if (elevated is not null)
                ExitMmoni();
        }
        catch (Win32Exception exception) when (exception.NativeErrorCode == 1223)
        {
            // A canceled UAC prompt leaves the current instance running.
        }
        catch (Win32Exception exception)
        {
            NotifyWarning($"Could not restart with administrator access. {exception.Message}");
        }
    }

    private void NotifyWarning(string message) => _tray?.ShowBalloonTip(4000, "Mmoni", message, Forms.ToolTipIcon.Warning);

    private void ExitMmoni()
    {
        _exiting = true;
        Shutdown();
    }

    protected override void OnExit(ExitEventArgs args)
    {
        _exiting = true;
        SystemEvents.SessionSwitch -= OnSessionSwitch;
        _fullscreenTimer?.Stop();
        _monitor?.Dispose();
        if (_tray is not null)
        {
            _tray.Visible = false;
            _tray.Dispose();
        }

        _menu?.Dispose();
        _icon?.Dispose();
        if (_ownsInstance)
            _instance?.ReleaseMutex();

        _instance?.Dispose();
        base.OnExit(args);
    }
}
