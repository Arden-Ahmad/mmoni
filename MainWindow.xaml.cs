using System;
using System.Globalization;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Threading;
using Mmoni.Controls;
using Mmoni.Monitoring;
using Mmoni.Platform;
using PixelPoint = System.Drawing.Point;

namespace Mmoni;

internal enum IslandState
{
    Minimal,
    Peek,
    Dashboard
}

public partial class MainWindow : Window
{
    private const double PeekWidth = 320;
    private const double PeekHeight = 74;
    private const double DashboardWidth = 368;
    private const double DashboardBaseHeight = 262;
    private const double DashboardBannerExtraHeight = 28;
    private const double StageMaxHeight = DashboardBaseHeight + DashboardBannerExtraHeight;
    private const int TransitionFrameRate = 60;
    private const string Missing = "—";

    private static readonly Brush CpuBrush = FreezeBrush(111, 231, 207);
    private static readonly Brush GpuBrush = FreezeBrush(178, 184, 255);
    private static readonly Brush WarmBrush = FreezeBrush(251, 191, 36);
    private static readonly Brush HotBrush = FreezeBrush(251, 113, 133);
    private static readonly Brush QuietBrush = FreezeBrush(154, 164, 186);
    private static readonly Brush SleepDotBrush = FreezeBrush(106, 116, 138);
    private static readonly Brush LiveGreenBrush = FreezeBrush(74, 222, 128);
    private static readonly Brush LiveGreenGlowBrush = CreateDotGlow(74, 222, 128);
    private static readonly Brush LiveWarmGlowBrush = CreateDotGlow(251, 191, 36);
    private static readonly Brush PinActiveBg = FreezeBrush(34, 111, 231, 207);
    private static readonly Brush PinActiveBorder = FreezeBrush(70, 111, 231, 207);

    // Visual theme surface brushes
    private static readonly Brush GlassSurfaceIdle = FreezeBrush(168, 18, 22, 34);
    private static readonly Brush GlassSurfaceHover = FreezeBrush(184, 24, 30, 44);
    private static readonly Brush GlassSurfaceExpanded = FreezeBrush(178, 17, 21, 33);
    private static readonly Brush PrecisionSurface = FreezeBrush(244, 18, 21, 28);
    private static readonly Brush GlassCardBg = FreezeBrush(16, 255, 255, 255);
    private static readonly Brush GlassCardBorder = FreezeBrush(32, 255, 255, 255);
    private static readonly Brush PrecisionCardBg = FreezeBrush(255, 26, 30, 40);
    private static readonly Brush PrecisionCardBorder = FreezeBrush(255, 49, 56, 71);
    private static readonly Brush GlassSegBg = FreezeBrush(120, 10, 13, 22);
    private static readonly Brush PrecisionSegBg = FreezeBrush(224, 11, 13, 18);
    private static readonly Brush PrecisionSegThumb = FreezeBrush(34, 255, 255, 255);
    private static readonly Brush GlassSegThumbBorder = FreezeBrush(54, 255, 255, 255);
    private static readonly Brush PrecisionSegThumbBorder = FreezeBrush(28, 255, 255, 255);
    private static readonly Brush ThrottleThermalBg = FreezeBrush(48, 251, 113, 133);
    private static readonly Brush ThrottleThermalBorder = FreezeBrush(104, 251, 113, 133);
    private static readonly Brush ThrottlePowerBg = FreezeBrush(44, 251, 191, 36);
    private static readonly Brush ThrottlePowerBorder = FreezeBrush(96, 251, 191, 36);

    // Pre-frozen radial glow brushes (Normal / Warm / Hot)
    private static readonly Brush CpuGlowNormal = CreateCornerGlow(0.14, 0.16, 46, 111, 231, 207);
    private static readonly Brush CpuGlowWarm = CreateCornerGlow(0.14, 0.16, 56, 251, 191, 36);
    private static readonly Brush CpuGlowHot = CreateCornerGlow(0.14, 0.16, 66, 251, 113, 133);

    private static readonly Brush GpuGlowNormal = CreateCornerGlow(0.86, 0.16, 48, 178, 184, 255);
    private static readonly Brush GpuGlowWarm = CreateCornerGlow(0.86, 0.16, 56, 251, 191, 36);
    private static readonly Brush GpuGlowHot = CreateCornerGlow(0.86, 0.16, 66, 251, 113, 133);
    private static readonly Brush GpuGlowSleep = CreateCornerGlow(0.86, 0.16, 22, 120, 130, 154);

    private readonly RingBuffer _cpuHistory = new();
    private readonly RingBuffer _gpuHistory = new();
    private readonly DispatcherTimer _peekTimer;
    private readonly DispatcherTimer _collapseTimer;
    private readonly GlassBackdrop _backdrop = new();
    private readonly bool _elevated;

    // Cached XAML resource brushes (avoids FindResource in hot paths)
    private readonly Brush _powerColorBrush;
    private readonly Brush _glassBorderBrush;
    private readonly Brush _glassBorderHoverBrush;
    private readonly Brush _precisionBorderBrush;
    private readonly Brush _glassSegThumbBrush;

    private UserSettings _settings;
    private IslandState _state = IslandState.Minimal;
    private PixelPoint? _anchor;
    private OverlayPlacement _stagePlacement;
    private PixelPoint _cachedStagePos;
    private (int Left, int Top, int Width, int Height, int Radius) _lastSyncedBackdrop;
    private Point? _dragStart;
    private UIElement? _dragSource;
    private bool _dragging;
    private bool _pinned;
    private bool _suppressHoverUntilLeave;
    private bool _hasStatusBanner;
    private bool _trackingAnimationFrames;
    private int _animationVersion;
    private Metric _metric = Metric.Temperature;
    private SensorSnapshot? _snapshot;
    private string? _historyGpu;
    private bool _liveHeartbeatBright = true;

    // Diff caches for zero-churn idle updates
    private string? _lastMinCpuTemp;
    private Brush? _lastMinCpuBrush;
    private string? _lastMinCpuSec;
    private string? _lastMinGpuTemp;
    private Brush? _lastMinGpuBrush;
    private string? _lastMinGpuSec;
    private bool? _lastMinGpuSleep;
    private Brush? _lastCpuAmbientGlow;
    private Brush? _lastGpuAmbientGlow;
    private string? _lastCpuModel;
    private string? _lastGpuModel;
    private string? _lastStatusTooltip;

    internal MainWindow(UserSettings settings, bool elevated)
    {
        _settings = settings;
        _elevated = elevated;
        if (settings.Left is int left && settings.Top is int top)
            _anchor = new PixelPoint(left, top);

        _peekTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(280) };
        _peekTimer.Tick += (_, _) => OnPeekHoverTick();

        _collapseTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(240) };
        _collapseTimer.Tick += (_, _) => OnHoverLeaveTick();

        InitializeComponent();

        _powerColorBrush = (Brush)FindResource("PowerColor");
        _glassBorderBrush = (Brush)FindResource("GlassBorderBrush");
        _glassBorderHoverBrush = (Brush)FindResource("GlassBorderHoverBrush");
        _precisionBorderBrush = (Brush)FindResource("PrecisionBorderBrush");
        _glassSegThumbBrush = (Brush)FindResource("GlassSegThumbBrush");

        CpuChart.ScrubChanged += info => UpdateScrubTooltip(isGpu: false, info);
        GpuChart.ScrubChanged += info => UpdateScrubTooltip(isGpu: true, info);
        SegmentedGrid.SizeChanged += (_, _) => UpdateSegThumbPosition(animate: false);

        ApplyVisualTheme();
        ApplyCollapsedModeLayout(reposition: false);
        UpdateRangeLabel();
        UpdateChromeState();

        SourceInitialized += (_, _) => OverlayWindow.ConfigureOverlay(Handle, _settings.ClickThrough);
        Loaded += (_, _) =>
        {
            PlaceOverlay();
            SyncBackdropLive();
        };
        LocationChanged += (_, _) =>
        {
            if (_dragging)
                SyncBackdropLive(ensureZOrder: false, useCachedPos: false);
        };
        DpiChanged += (_, _) =>
        {
            if (!_dragging)
                Dispatcher.InvokeAsync(() =>
                {
                    PlaceOverlay();
                    SyncBackdropLive();
                }, DispatcherPriority.Loaded);
        };
        IsVisibleChanged += (_, _) =>
        {
            if (!IsVisible)
            {
                StopAnimationTracking();
                _backdrop.Hide();
                _lastSyncedBackdrop = default;
            }
            else if (IsLoaded)
            {
                SyncBackdropLive();
            }
        };
        Closed += (_, _) =>
        {
            StopAnimationTracking();
            _backdrop.Dispose();
        };
    }

    internal event Action<PixelPoint>? OverlayMoved;
    internal event Action<TimeRange>? TimeRangeChanged;

    private nint Handle => new WindowInteropHelper(this).Handle;

    private double MinimalWidth => _settings.CollapsedMode switch
    {
        CollapsedDisplayMode.TempOnly => 138,
        CollapsedDisplayMode.TempLoad => 204,
        _ => _settings.Theme == VisualTheme.LiquidGlass ? 228 : 224
    };

    private double MinimalHeight => _settings.Theme == VisualTheme.LiquidGlass ? 28 : 26;
    private double MinimalRadius => MinimalHeight / 2.0;
    private double PeekRadius => _settings.Theme == VisualTheme.LiquidGlass ? 18 : 11;
    private double DashboardRadius => _settings.Theme == VisualTheme.LiquidGlass ? 20 : 12;
    private double CurrentDashboardHeight => _hasStatusBanner ? DashboardBaseHeight + DashboardBannerExtraHeight : DashboardBaseHeight;

    private (double Width, double Height, double Radius) GetDimensions(IslandState state) => state switch
    {
        IslandState.Minimal => (MinimalWidth, MinimalHeight, MinimalRadius),
        IslandState.Peek => (PeekWidth, PeekHeight, PeekRadius),
        _ => (DashboardWidth, CurrentDashboardHeight, DashboardRadius)
    };

    internal void ApplySettings(UserSettings updated)
    {
        var themeChanged = _settings.Theme != updated.Theme;
        var collapsedChanged = _settings.CollapsedMode != updated.CollapsedMode;
        var clickThroughChanged = _settings.ClickThrough != updated.ClickThrough;
        var autoExpandChanged = _settings.AutoExpandOnHover != updated.AutoExpandOnHover;
        var rangeChanged = _settings.HistoryRange != updated.HistoryRange;

        _settings = updated;

        if (clickThroughChanged)
        {
            _peekTimer.Stop();
            _collapseTimer.Stop();
            if (_settings.ClickThrough && _state != IslandState.Minimal)
            {
                _pinned = false;
                TransitionTo(IslandState.Minimal);
            }
            if (Handle != 0)
                OverlayWindow.SetClickThrough(Handle, _settings.ClickThrough);
        }

        if (autoExpandChanged)
        {
            _peekTimer.Stop();
            _collapseTimer.Stop();
            if (!_settings.AutoExpandOnHover && _state == IslandState.Peek)
                TransitionTo(IslandState.Minimal);
        }

        if (themeChanged)
            ApplyVisualTheme();

        if (collapsedChanged)
            ApplyCollapsedModeLayout(reposition: true);

        if (rangeChanged)
            UpdateRangeLabel();

        UpdateChromeState();

        if (_snapshot is not null)
        {
            InvalidateMinimalCache();
            DisplayCurrentVisuals(_snapshot);
        }

        if (themeChanged || collapsedChanged)
        {
            PlaceOverlay();
            SyncBackdropLive();
        }
    }

    internal void ResetPosition()
    {
        _anchor = null;
        if (_state != IslandState.Minimal)
        {
            _pinned = false;
            TransitionTo(IslandState.Minimal, immediate: true);
        }

        PlaceOverlay();
        SyncBackdropLive();
    }

    internal void ResetPeaks()
    {
        _cpuHistory.ResetPeaks();
        _gpuHistory.ResetPeaks();
        if (_state == IslandState.Dashboard)
            RefreshDashboardDetails();
    }

    /// <summary>
    /// Keeps the transparent stage HWND fixed at Dashboard stage dimensions so Minimal &lt;-&gt; Peek &lt;-&gt; Dashboard
    /// transitions NEVER resize or move the HWND (zero 1-frame teleport glitches; empty alpha=0 area is click-through!).
    /// </summary>
    internal void PlaceOverlay()
    {
        if (Handle == 0)
            return;

        var dpi = VisualTreeHelper.GetDpi(this);
        _stagePlacement = OverlayWindow.ComputePlacement(
            Handle,
            _anchor,
            MinimalWidth,
            MinimalHeight,
            DashboardWidth,
            StageMaxHeight,
            dpi.DpiScaleX,
            dpi.DpiScaleY);

        if (_anchor is null)
            _anchor = _stagePlacement.CompactVisual.Location;

        var stageDipW = DashboardWidth + OverlayWindow.ShadowPadLeft + OverlayWindow.ShadowPadRight;
        var stageDipH = StageMaxHeight + OverlayWindow.ShadowPadTop + OverlayWindow.ShadowPadBottom;
        Width = stageDipW;
        Height = stageDipH;

        OverlayWindow.SetBoundsTopMost(
            Handle,
            _stagePlacement.StageWindow.Left,
            _stagePlacement.StageWindow.Top,
            _stagePlacement.StageWindow.Width,
            _stagePlacement.StageWindow.Height);
        _cachedStagePos = new PixelPoint(_stagePlacement.StageWindow.Left, _stagePlacement.StageWindow.Top);

        var (w, h, r) = GetDimensions(_state);
        var margin = GetChromeMarginInStage(_state);
        ClearChromeAnimations();
        ChromeHost.Width = w;
        ChromeHost.Height = h;
        ChromeHost.Margin = margin;
        SetChromeCornerRadius(new CornerRadius(r));
        UpdateInnerClip(w, h, r);

        if (_state == IslandState.Dashboard)
            DashboardSurface.Height = CurrentDashboardHeight;
    }

    private Thickness GetChromeMarginInStage(IslandState state)
    {
        var (w, h, _) = GetDimensions(state);
        var compactCenterX = _stagePlacement.CompactOffsetX + MinimalWidth / 2.0;
        var maxLeft = OverlayWindow.ShadowPadLeft + Math.Max(0, DashboardWidth - w);
        var left = Math.Clamp(compactCenterX - w / 2.0, OverlayWindow.ShadowPadLeft, maxLeft);

        var maxTop = OverlayWindow.ShadowPadTop + Math.Max(0, StageMaxHeight - h);
        var top = Math.Clamp(_stagePlacement.CompactOffsetY, OverlayWindow.ShadowPadTop, maxTop);

        return new Thickness(left, top, 0, 0);
    }

    internal void Display(SensorSnapshot snapshot)
    {
        _snapshot = snapshot;
        if (_historyGpu != snapshot.SelectedGpu?.Id && snapshot.Error is null)
        {
            _gpuHistory.Clear();
            _historyGpu = snapshot.SelectedGpu?.Id;
        }

        _cpuHistory.Add(snapshot.Cpu);
        _gpuHistory.Add(snapshot.Gpu);

        if (_state == IslandState.Dashboard)
        {
            _liveHeartbeatBright = !_liveHeartbeatBright;
            LiveDotContainer.Opacity = _liveHeartbeatBright ? 1.0 : 0.55;
        }

        DisplayCurrentVisuals(snapshot);
    }

    private void DisplayCurrentVisuals(SensorSnapshot snapshot)
    {
        var cpu = snapshot.Cpu;
        var gpu = snapshot.Gpu;

        var cpuTemp = Format(cpu?.Temperature, "°C", "0");
        var cpuBrush = GetThermalBrush(cpu?.Temperature, isGpu: false);
        var gpuSleeping = gpu is { IsSleeping: true };
        var gpuTemp = gpuSleeping ? "Idle" : Format(gpu?.Temperature, "°C", "0");
        var gpuBrush = gpuSleeping ? QuietBrush : GetThermalBrush(gpu?.Temperature, isGpu: true);

        // Update Minimal Pill ONLY if values changed (zero churn when idle!).
        if (_state == IslandState.Minimal || MinimalSurface.Visibility == Visibility.Visible)
        {
            var cpuSec = FormatCollapsedSecondary(cpu);
            var gpuSec = gpuSleeping ? "D3" : FormatCollapsedSecondary(gpu);

            if (_lastMinCpuTemp != cpuTemp) { MinimalCpuTemp.Text = cpuTemp; _lastMinCpuTemp = cpuTemp; }
            if (_lastMinCpuBrush != cpuBrush) { MinimalCpuTemp.Foreground = cpuBrush; _lastMinCpuBrush = cpuBrush; }
            if (_lastMinCpuSec != cpuSec) { MinimalCpuSecondary.Text = cpuSec; _lastMinCpuSec = cpuSec; }

            if (_lastMinGpuTemp != gpuTemp) { MinimalGpuTemp.Text = gpuTemp; _lastMinGpuTemp = gpuTemp; }
            if (_lastMinGpuBrush != gpuBrush) { MinimalGpuTemp.Foreground = gpuBrush; _lastMinGpuBrush = gpuBrush; }
            if (_lastMinGpuSec != gpuSec) { MinimalGpuSecondary.Text = gpuSec; _lastMinGpuSec = gpuSec; }
            if (_lastMinGpuSleep != gpuSleeping)
            {
                MinimalGpuDot.Fill = gpuSleeping ? SleepDotBrush : GpuBrush;
                MinimalGpuSecondary.Foreground = gpuSleeping ? QuietBrush : _powerColorBrush;
                _lastMinGpuSleep = gpuSleeping;
            }
        }

        // Update ambient corner glow in Liquid Glass mode ONLY when the thermal band changes.
        if (_settings.Theme == VisualTheme.LiquidGlass)
        {
            var nextCpuGlow = GetCornerGlowBrush(cpu?.Temperature, isGpu: false, sleeping: false);
            if (!ReferenceEquals(_lastCpuAmbientGlow, nextCpuGlow))
            {
                CpuAmbientGlow.Background = nextCpuGlow;
                _lastCpuAmbientGlow = nextCpuGlow;
            }

            var nextGpuGlow = GetCornerGlowBrush(gpu?.Temperature, isGpu: true, sleeping: gpuSleeping);
            if (!ReferenceEquals(_lastGpuAmbientGlow, nextGpuGlow))
            {
                GpuAmbientGlow.Background = nextGpuGlow;
                _lastGpuAmbientGlow = nextGpuGlow;
            }
        }

        if (_state == IslandState.Peek)
            RefreshPeekDetails(snapshot, cpuTemp, cpuBrush, gpuTemp, gpuBrush, gpuSleeping);
        else if (_state == IslandState.Dashboard)
            RefreshDashboardDetails();
    }

    private void RefreshPeekDetails(
        SensorSnapshot snapshot,
        string cpuTemp,
        Brush cpuBrush,
        string gpuTemp,
        Brush gpuBrush,
        bool gpuSleeping)
    {
        var cpu = snapshot.Cpu;
        var gpu = snapshot.Gpu;

        SetText(PeekCpuTemp, cpuTemp);
        SetForeground(PeekCpuTemp, cpuBrush);
        SetText(PeekCpuPower, Format(cpu?.Power, "W", "0.0"));
        SetText(PeekCpuLoad, Format(cpu?.Load, "%", "0"));
        SetText(PeekCpuClock, FormatClock(cpu?.Clock));
        ApplyThrottleBadge(PeekCpuThrottle, PeekCpuThrottleText, cpu?.Throttle ?? ThrottleFlags.None);

        SetFill(PeekGpuDot, gpuSleeping ? SleepDotBrush : GpuBrush);
        SetText(PeekGpuTemp, gpuTemp);
        SetForeground(PeekGpuTemp, gpuBrush);
        SetVisibility(PeekGpuSubGrid, gpuSleeping ? Visibility.Collapsed : Visibility.Visible);
        SetVisibility(PeekGpuSleepText, gpuSleeping ? Visibility.Visible : Visibility.Collapsed);
        if (!gpuSleeping)
        {
            SetText(PeekGpuPower, Format(gpu?.Power, "W", "0.0"));
            SetText(PeekGpuLoad, Format(gpu?.Load, "%", "0"));
            SetText(PeekGpuClock, FormatClock(gpu?.Clock));
        }
        ApplyThrottleBadge(PeekGpuThrottle, PeekGpuThrottleText, gpuSleeping ? ThrottleFlags.None : (gpu?.Throttle ?? ThrottleFlags.None));
    }

    private void RefreshDashboardDetails()
    {
        var snapshot = _snapshot;
        if (snapshot is null)
            return;

        var cpu = snapshot.Cpu;
        var gpu = snapshot.Gpu;
        var gpuSleeping = gpu is { IsSleeping: true };

        // Header live telemetry (fixed slots, diff-checked to avoid redundant WPF property churn)
        SetText(DashCpuTemp, Format(cpu?.Temperature, "°C", "0"));
        SetForeground(DashCpuTemp, GetThermalBrush(cpu?.Temperature, isGpu: false));
        SetText(DashCpuPower, Format(cpu?.Power, "W", "0.0"));
        SetText(DashCpuLoad, Format(cpu?.Load, "%", "0"));
        SetText(DashCpuClock, FormatClock(cpu?.Clock));
        ApplyThrottleBadge(DashCpuThrottle, DashCpuThrottleText, cpu?.Throttle ?? ThrottleFlags.None);

        SetFill(DashGpuDot, gpuSleeping ? SleepDotBrush : GpuBrush);
        SetText(DashGpuTemp, gpuSleeping ? "Idle" : Format(gpu?.Temperature, "°C", "0"));
        SetForeground(DashGpuTemp, gpuSleeping ? QuietBrush : GetThermalBrush(gpu?.Temperature, isGpu: true));
        SetText(DashGpuPower, gpuSleeping ? "0.0W" : Format(gpu?.Power, "W", "0.0"));
        SetVisibility(DashGpuExtraGrid, gpuSleeping ? Visibility.Collapsed : Visibility.Visible);
        SetVisibility(DashGpuSleepText, gpuSleeping ? Visibility.Visible : Visibility.Collapsed);
        if (!gpuSleeping)
        {
            SetText(DashGpuLoad, Format(gpu?.Load, "%", "0"));
            SetText(DashGpuClock, FormatClock(gpu?.Clock));
        }
        ApplyThrottleBadge(DashGpuThrottle, DashGpuThrottleText, gpuSleeping ? ThrottleFlags.None : (gpu?.Throttle ?? ThrottleFlags.None));

        // Hardware model names (update only when changed)
        var cpuModelText = snapshot.CpuName ?? "CPU unavailable";
        if (!string.Equals(_lastCpuModel, cpuModelText, StringComparison.Ordinal))
        {
            _lastCpuModel = cpuModelText;
            CpuModel.Text = cpuModelText;
            CpuModel.ToolTip = snapshot.CpuName;
        }

        var gpuModelText = snapshot.SelectedGpu?.Name ?? "GPU unavailable";
        if (!string.Equals(_lastGpuModel, gpuModelText, StringComparison.Ordinal))
        {
            _lastGpuModel = gpuModelText;
            GpuModel.Text = gpuModelText;
            GpuModel.ToolTip = snapshot.SelectedGpu?.Name;
        }

        // Dual Glass Card Sparklines + Peak / Avg / Min / Max
        var cpuStats = CpuChart.Display(_cpuHistory, _metric, _settings.HistoryRange, _settings.Theme, _settings.CpuWarmTemp, _settings.CpuHotTemp);
        var gpuStats = GpuChart.Display(_gpuHistory, _metric, _settings.HistoryRange, _settings.Theme, _settings.GpuWarmTemp, _settings.GpuHotTemp);

        var cpuSessionPeak = _cpuHistory.GetSessionPeak(_metric) ?? cpuStats.Max;
        var gpuSessionPeak = _gpuHistory.GetSessionPeak(_metric) ?? gpuStats.Max;

        SetText(CpuPeakText, FormatMetric(cpuSessionPeak));
        SetText(CpuAvgText, FormatMetric(cpuStats.Avg));
        SetText(CpuMaxScale, cpuStats.HasData ? FormatMetric(cpuStats.Max) : string.Empty);
        SetText(CpuMinScale, cpuStats.HasData ? FormatMetric(cpuStats.Min) : string.Empty);
        SetVisibility(CpuEmpty, cpuStats.HasData ? Visibility.Collapsed : Visibility.Visible);

        SetText(GpuPeakText, FormatMetric(gpuSessionPeak));
        SetText(GpuAvgText, FormatMetric(gpuStats.Avg));
        SetText(GpuMaxScale, gpuStats.HasData ? FormatMetric(gpuStats.Max) : string.Empty);
        SetText(GpuMinScale, gpuStats.HasData ? FormatMetric(gpuStats.Min) : string.Empty);
        SetText(GpuEmpty, gpuSleeping ? "dGPU sleeping" : "No readings");
        SetVisibility(GpuEmpty, gpuStats.HasData ? Visibility.Collapsed : Visibility.Visible);

        // Conditional warning banner (hidden when everything is live & healthy)
        var warning = GetWarningStatus(snapshot);
        var shouldShowBanner = warning is not null;
        SetVisibility(StatusBanner, shouldShowBanner ? Visibility.Visible : Visibility.Collapsed);
        if (warning is not null)
        {
            SetText(StatusText, warning);
            var tip = snapshot.Error ?? warning;
            if (!string.Equals(_lastStatusTooltip, tip, StringComparison.Ordinal))
            {
                _lastStatusTooltip = tip;
                StatusText.ToolTip = tip;
            }
            SetFill(StatusDot, snapshot.Error is not null ? HotBrush : WarmBrush);
        }

        var hasError = snapshot.Error is not null;
        SetFill(LiveDot, hasError ? WarmBrush : LiveGreenBrush);
        SetFill(LiveDotGlow, hasError ? LiveWarmGlowBrush : LiveGreenGlowBrush);

        if (shouldShowBanner != _hasStatusBanner)
        {
            _hasStatusBanner = shouldShowBanner;
            if (_state == IslandState.Dashboard)
            {
                DashboardSurface.Height = CurrentDashboardHeight;
                ChromeHost.Height = CurrentDashboardHeight;
                ChromeHost.Margin = GetChromeMarginInStage(IslandState.Dashboard);
                UpdateInnerClip(DashboardWidth, CurrentDashboardHeight, DashboardRadius);
                SyncBackdropLive();
            }
        }
    }

    private void TransitionTo(IslandState nextState, bool immediate = false)
    {
        if (_state == nextState && !immediate)
            return;

        var previousState = _state;
        _state = nextState;
        var version = ++_animationVersion;
        UpdateChromeState();

        var isExpanding = nextState > previousState;
        var (targetW, targetH, targetR) = GetDimensions(nextState);
        var targetMargin = GetChromeMarginInStage(nextState);

        if (immediate)
        {
            StopAnimationTracking();
            ClearChromeAnimations();
            SetSurfaceStateImmediate(nextState);
            ChromeHost.Width = targetW;
            ChromeHost.Height = targetH;
            ChromeHost.Margin = targetMargin;
            SetChromeCornerRadius(new CornerRadius(targetR));
            UpdateInnerClip(targetW, targetH, targetR);

            if (nextState == IslandState.Dashboard)
            {
                DashboardSurface.Height = CurrentDashboardHeight;
                _liveHeartbeatBright = true;
                LiveDotContainer.Opacity = 1.0;
                RefreshDashboardDetails();
                UpdateSegThumbPosition(animate: false);
            }
            else if (nextState == IslandState.Peek && _snapshot is not null)
            {
                DisplayCurrentVisuals(_snapshot);
            }
            SyncBackdropLive();
            return;
        }

        var animFromW = ChromeHost.ActualWidth > 0 ? ChromeHost.ActualWidth : GetDimensions(previousState).Width;
        var animFromH = ChromeHost.ActualHeight > 0 ? ChromeHost.ActualHeight : GetDimensions(previousState).Height;
        var animFromMargin = ChromeHost.Margin;
        var animFromRadius = Chrome.CornerRadius;

        PrepareSurfaceForTransition(nextState);
        if (nextState == IslandState.Dashboard)
        {
            DashboardSurface.Height = CurrentDashboardHeight;
            _liveHeartbeatBright = true;
            LiveDotContainer.Opacity = 1.0;
            RefreshDashboardDetails();
            UpdateSegThumbPosition(animate: false);
        }
        else if (nextState == IslandState.Peek && _snapshot is not null)
        {
            DisplayCurrentVisuals(_snapshot);
        }

        var isGlass = _settings.Theme == VisualTheme.LiquidGlass;
        var duration = isExpanding
            ? TimeSpan.FromMilliseconds(isGlass ? 255 : 180)
            : TimeSpan.FromMilliseconds(isGlass ? 195 : 160);

        IEasingFunction ease = isExpanding && isGlass
            ? new BackEase { EasingMode = EasingMode.EaseOut, Amplitude = 0.16 }
            : new CubicEase { EasingMode = EasingMode.EaseOut };

        ChromeHost.Width = targetW;
        ChromeHost.Height = targetH;
        ChromeHost.Margin = targetMargin;
        SetChromeCornerRadius(new CornerRadius(targetR));

        ChromeShadow.BlurRadius = nextState switch
        {
            IslandState.Minimal => isGlass ? 16 : 10,
            IslandState.Peek => isGlass ? 22 : 14,
            _ => isGlass ? 26 : 18
        };
        ChromeShadow.ShadowDepth = nextState == IslandState.Minimal ? 4 : 8;
        ChromeShadow.Opacity = nextState == IslandState.Minimal ? 0.46 : 0.58;

        // Track live ChromeHost bounds & CornerRadius on every animated frame (capped to 60 fps).
        StartAnimationTracking();

        var heightAnim = WithFrameRate(new DoubleAnimation(animFromH, targetH, duration)
        {
            EasingFunction = ease,
            FillBehavior = FillBehavior.Stop
        });
        heightAnim.Completed += (_, _) =>
        {
            if (version != _animationVersion || _state != nextState)
                return;

            StopAnimationTracking();
            ClearChromeAnimations();
            SetSurfaceStateImmediate(nextState);
            ChromeHost.Width = targetW;
            ChromeHost.Height = targetH;
            ChromeHost.Margin = targetMargin;
            SetChromeCornerRadius(new CornerRadius(targetR));
            UpdateInnerClip(targetW, targetH, targetR);
            SyncBackdropLive();
        };

        ChromeHost.BeginAnimation(WidthProperty, WithFrameRate(new DoubleAnimation(animFromW, targetW, duration)
        {
            EasingFunction = ease,
            FillBehavior = FillBehavior.Stop
        }));
        ChromeHost.BeginAnimation(HeightProperty, heightAnim);
        ChromeHost.BeginAnimation(MarginProperty, WithFrameRate(new ThicknessAnimation(animFromMargin, targetMargin, duration)
        {
            EasingFunction = ease,
            FillBehavior = FillBehavior.Stop
        }));
        Chrome.BeginAnimation(Border.CornerRadiusProperty, WithFrameRate(new CornerRadiusAnimation(animFromRadius, new CornerRadius(targetR), duration)
        {
            EasingFunction = ease,
            FillBehavior = FillBehavior.Stop
        }));

        AnimateSurfaceOpacity(MinimalSurface, nextState == IslandState.Minimal ? 1 : 0, nextState == IslandState.Minimal ? 165 : 85);
        AnimateSurfaceOpacity(PeekSurface, nextState == IslandState.Peek ? 1 : 0, nextState == IslandState.Peek ? 175 : 85);
        AnimateSurfaceOpacity(DashboardSurface, nextState == IslandState.Dashboard ? 1 : 0, nextState == IslandState.Dashboard ? 200 : 95);

        if (nextState == IslandState.Dashboard)
        {
            DashboardSlide.BeginAnimation(TranslateTransform.YProperty,
                WithFrameRate(new DoubleAnimation(-5, 0, TimeSpan.FromMilliseconds(200))
                {
                    EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut },
                    FillBehavior = FillBehavior.Stop
                }));
        }
    }

    private void StartAnimationTracking()
    {
        if (Handle != 0)
            _cachedStagePos = OverlayWindow.GetPosition(Handle);

        if (_trackingAnimationFrames)
            return;

        _trackingAnimationFrames = true;
        CompositionTarget.Rendering += OnAnimationFrame;
    }

    private void StopAnimationTracking()
    {
        if (!_trackingAnimationFrames)
            return;

        _trackingAnimationFrames = false;
        CompositionTarget.Rendering -= OnAnimationFrame;
    }

    private void OnAnimationFrame(object? sender, EventArgs e)
    {
        var w = ChromeHost.ActualWidth;
        var h = ChromeHost.ActualHeight;
        if (w <= 2 || h <= 2)
            return;

        var cr = Chrome.CornerRadius;
        ChromeShadowLayer.CornerRadius = cr;
        SpecularRim.CornerRadius = cr;
        UpdateInnerClip(w, h, cr.TopLeft);
        SyncBackdropLive(ensureZOrder: false, useCachedPos: true);
    }

    private void SetChromeCornerRadius(CornerRadius radius)
    {
        Chrome.CornerRadius = radius;
        ChromeShadowLayer.CornerRadius = radius;
        SpecularRim.CornerRadius = radius;
    }

    private void UpdateInnerClip(double chromeW, double chromeH, double cornerRadius)
    {
        var innerW = Math.Max(0, chromeW - 2);
        var innerH = Math.Max(0, chromeH - 2);
        var r = Math.Max(0, cornerRadius - 1);
        InnerClip.Rect = new Rect(0, 0, innerW, innerH);
        InnerClip.RadiusX = r;
        InnerClip.RadiusY = r;
    }

    private void SyncBackdropLive(bool ensureZOrder = true, bool useCachedPos = false)
    {
        if (!IsVisible || _settings.Theme != VisualTheme.LiquidGlass || Handle == 0)
        {
            _backdrop.Hide();
            _lastSyncedBackdrop = default;
            return;
        }

        var wDip = ChromeHost.ActualWidth > 0 ? ChromeHost.ActualWidth : ChromeHost.Width;
        var hDip = ChromeHost.ActualHeight > 0 ? ChromeHost.ActualHeight : ChromeHost.Height;
        if (double.IsNaN(wDip) || double.IsNaN(hDip) || wDip <= 4 || hDip <= 4)
            return;

        var hwndPos = useCachedPos ? _cachedStagePos : OverlayWindow.GetPosition(Handle);
        if (!useCachedPos)
            _cachedStagePos = hwndPos;

        var dpiX = _stagePlacement.DpiScaleX > 0 ? _stagePlacement.DpiScaleX : VisualTreeHelper.GetDpi(this).DpiScaleX;
        var dpiY = _stagePlacement.DpiScaleY > 0 ? _stagePlacement.DpiScaleY : VisualTreeHelper.GetDpi(this).DpiScaleY;

        var leftPx = hwndPos.X + (int)Math.Round(ChromeHost.Margin.Left * dpiX);
        var topPx = hwndPos.Y + (int)Math.Round(ChromeHost.Margin.Top * dpiY);
        var widthPx = Math.Max(4, (int)Math.Round(wDip * dpiX));
        var heightPx = Math.Max(4, (int)Math.Round(hDip * dpiY));
        var radiusPx = Math.Max(1, (int)Math.Round(Chrome.CornerRadius.TopLeft * dpiX));

        var bounds = (leftPx, topPx, widthPx, heightPx, radiusPx);
        if (!ensureZOrder && _lastSyncedBackdrop == bounds)
            return;

        _lastSyncedBackdrop = bounds;
        _backdrop.SyncBehind(Handle, leftPx, topPx, widthPx, heightPx, radiusPx, ensureZOrder);
    }

    private void PrepareSurfaceForTransition(IslandState target)
    {
        MinimalSurface.IsHitTestVisible = target == IslandState.Minimal;
        PeekSurface.IsHitTestVisible = target == IslandState.Peek;
        DashboardSurface.IsHitTestVisible = target == IslandState.Dashboard;

        if (target == IslandState.Minimal) MinimalSurface.Visibility = Visibility.Visible;
        if (target == IslandState.Peek) PeekSurface.Visibility = Visibility.Visible;
        if (target == IslandState.Dashboard) DashboardSurface.Visibility = Visibility.Visible;
    }

    private void SetSurfaceStateImmediate(IslandState active)
    {
        MinimalSurface.Visibility = active == IslandState.Minimal ? Visibility.Visible : Visibility.Collapsed;
        MinimalSurface.Opacity = active == IslandState.Minimal ? 1 : 0;
        MinimalSurface.IsHitTestVisible = active == IslandState.Minimal;

        PeekSurface.Visibility = active == IslandState.Peek ? Visibility.Visible : Visibility.Collapsed;
        PeekSurface.Opacity = active == IslandState.Peek ? 1 : 0;
        PeekSurface.IsHitTestVisible = active == IslandState.Peek;

        DashboardSurface.Visibility = active == IslandState.Dashboard ? Visibility.Visible : Visibility.Collapsed;
        DashboardSurface.Opacity = active == IslandState.Dashboard ? 1 : 0;
        DashboardSurface.IsHitTestVisible = active == IslandState.Dashboard;
    }

    private static void AnimateSurfaceOpacity(UIElement element, double targetOpacity, double ms)
    {
        var from = element.Opacity;
        element.Opacity = targetOpacity;
        element.BeginAnimation(OpacityProperty, WithFrameRate(new DoubleAnimation(from, targetOpacity, TimeSpan.FromMilliseconds(ms))
        {
            FillBehavior = FillBehavior.Stop
        }));
    }

    private void ClearChromeAnimations()
    {
        ChromeHost.BeginAnimation(WidthProperty, null);
        ChromeHost.BeginAnimation(HeightProperty, null);
        ChromeHost.BeginAnimation(MarginProperty, null);
        Chrome.BeginAnimation(Border.CornerRadiusProperty, null);
        MinimalSurface.BeginAnimation(OpacityProperty, null);
        PeekSurface.BeginAnimation(OpacityProperty, null);
        DashboardSurface.BeginAnimation(OpacityProperty, null);
        DashboardSlide.BeginAnimation(TranslateTransform.YProperty, null);
    }

    private void ApplyVisualTheme()
    {
        var isGlass = _settings.Theme == VisualTheme.LiquidGlass;

        ChromeShadowLayer.Background = isGlass
            ? (_state == IslandState.Minimal ? GlassSurfaceIdle : GlassSurfaceExpanded)
            : PrecisionSurface;
        Chrome.BorderBrush = isGlass ? _glassBorderBrush : _precisionBorderBrush;
        AmbientGlowLayer.Visibility = isGlass ? Visibility.Visible : Visibility.Collapsed;
        SpecularRim.Visibility = isGlass ? Visibility.Visible : Visibility.Collapsed;

        SegmentedTrack.Background = isGlass ? GlassSegBg : PrecisionSegBg;
        SegThumb.Background = isGlass ? _glassSegThumbBrush : PrecisionSegThumb;
        SegThumb.BorderBrush = isGlass ? GlassSegThumbBorder : PrecisionSegThumbBorder;

        var cardBg = isGlass ? GlassCardBg : PrecisionCardBg;
        var cardBorder = isGlass ? GlassCardBorder : PrecisionCardBorder;
        var cardRadius = new CornerRadius(isGlass ? 14 : 8);

        CpuCard.Background = cardBg;
        CpuCard.BorderBrush = cardBorder;
        CpuCard.CornerRadius = cardRadius;

        GpuCard.Background = cardBg;
        GpuCard.BorderBrush = cardBorder;
        GpuCard.CornerRadius = cardRadius;

        if (!isGlass)
        {
            _backdrop.Hide();
            _lastSyncedBackdrop = default;
        }
    }

    private void ApplyCollapsedModeLayout(bool reposition)
    {
        var secWidth = _settings.CollapsedMode switch
        {
            CollapsedDisplayMode.TempOnly => new GridLength(0),
            CollapsedDisplayMode.TempLoad => new GridLength(33),
            _ => new GridLength(45)
        };

        MinimalCpuSecCol.Width = secWidth;
        MinimalGpuSecCol.Width = secWidth;
        var showSecondary = _settings.CollapsedMode != CollapsedDisplayMode.TempOnly;
        MinimalCpuSecondary.Visibility = showSecondary ? Visibility.Visible : Visibility.Collapsed;
        MinimalGpuSecondary.Visibility = showSecondary ? Visibility.Visible : Visibility.Collapsed;
        InvalidateMinimalCache();

        if (reposition && _state == IslandState.Minimal)
        {
            PlaceOverlay();
            SyncBackdropLive();
        }
    }

    private void InvalidateMinimalCache()
    {
        _lastMinCpuTemp = null;
        _lastMinCpuBrush = null;
        _lastMinCpuSec = null;
        _lastMinGpuTemp = null;
        _lastMinGpuBrush = null;
        _lastMinGpuSec = null;
        _lastMinGpuSleep = null;
    }

    private void UpdateChromeState()
    {
        ChromeHost.Opacity = _settings.ClickThrough ? 0.78 : 1.0;

        if (_settings.Theme == VisualTheme.LiquidGlass)
        {
            ChromeShadowLayer.Background = _state == IslandState.Minimal
                ? (ChromeHost.IsMouseOver ? GlassSurfaceHover : GlassSurfaceIdle)
                : GlassSurfaceExpanded;
            Chrome.BorderBrush = _state == IslandState.Minimal && ChromeHost.IsMouseOver
                ? _glassBorderHoverBrush
                : _glassBorderBrush;
        }

        MinimalSurface.ToolTip = _settings.AutoExpandOnHover
            ? "Hover to peek · Click for dashboard · Drag to move"
            : "Click for dashboard · Drag to move";

        PinIcon.Fill = _pinned ? CpuBrush : QuietBrush;
        PinButton.Background = _pinned ? PinActiveBg : Brushes.Transparent;
        PinButton.BorderBrush = _pinned ? PinActiveBorder : Brushes.Transparent;
        PinButton.ToolTip = _pinned ? "Unpin (auto-collapse on leave)" : "Pin dashboard open";
        AutomationProperties.SetName(PinButton, _pinned ? "Unpin dashboard" : "Pin dashboard");
    }

    private void UpdateRangeLabel()
    {
        RangeText.Text = _settings.HistoryRange switch
        {
            TimeRange.Minutes5 => "5m",
            TimeRange.Minutes15 => "15m",
            _ => "60s"
        };
    }

    // =========================================================================
    // INTERACTION HANDLERS (3-STATE HOVER / CLICK / PIN / SCRUB / DRAG)
    // =========================================================================

    private void ChromeSizeChanged(object sender, SizeChangedEventArgs e)
    {
        if (!_trackingAnimationFrames)
            UpdateInnerClip(e.NewSize.Width, e.NewSize.Height, Chrome.CornerRadius.TopLeft);
    }

    private void ChromeMouseEnter(object sender, MouseEventArgs args)
    {
        _collapseTimer.Stop();

        if (_state == IslandState.Minimal && !_dragging && !_settings.ClickThrough)
        {
            UpdateChromeState();

            if (_settings.AutoExpandOnHover && !_suppressHoverUntilLeave)
            {
                _peekTimer.Stop();
                _peekTimer.Start();
            }
        }
    }

    private void ChromeMouseLeave(object sender, MouseEventArgs args)
    {
        _peekTimer.Stop();
        _suppressHoverUntilLeave = false;

        if (_state == IslandState.Minimal)
            UpdateChromeState();

        if ((_state == IslandState.Peek || (_state == IslandState.Dashboard && !_pinned)) && !_dragging)
        {
            _collapseTimer.Stop();
            _collapseTimer.Start();
        }
    }

    private void OnPeekHoverTick()
    {
        _peekTimer.Stop();
        if (_settings.AutoExpandOnHover && _state == IslandState.Minimal && !_dragging && !_settings.ClickThrough && ChromeHost.IsMouseOver)
            TransitionTo(IslandState.Peek);
    }

    private void OnHoverLeaveTick()
    {
        _collapseTimer.Stop();
        if (_dragging || ChromeHost.IsMouseOver)
            return;

        if (_state == IslandState.Peek || (_state == IslandState.Dashboard && !_pinned))
            TransitionTo(IslandState.Minimal);
    }

    private void ClickMinimalSurface(object sender, RoutedEventArgs args)
    {
        _peekTimer.Stop();
        _collapseTimer.Stop();
        _pinned = true;
        TransitionTo(IslandState.Dashboard);
    }

    private void ClickPeekSurface(object sender, RoutedEventArgs args)
    {
        _peekTimer.Stop();
        _collapseTimer.Stop();
        _pinned = true;
        TransitionTo(IslandState.Dashboard);
    }

    private void ClickDashboardHeader(object sender, RoutedEventArgs args)
    {
        _peekTimer.Stop();
        _collapseTimer.Stop();
        _pinned = false;
        _suppressHoverUntilLeave = true;
        TransitionTo(IslandState.Minimal);
    }

    private void TogglePin(object sender, RoutedEventArgs args)
    {
        _pinned = !_pinned;
        UpdateChromeState();
    }

    private void ChangeMetric(object sender, RoutedEventArgs args)
    {
        if (!IsLoaded)
            return;

        _metric = sender == TemperatureTab ? Metric.Temperature
            : sender == PowerTab ? Metric.Power
            : sender == LoadTab ? Metric.Load
            : Metric.Clock;

        UpdateSegThumbPosition(animate: true);
        RefreshDashboardDetails();
    }

    private void UpdateSegThumbPosition(bool animate)
    {
        var idx = _metric switch
        {
            Metric.Temperature => 0,
            Metric.Power => 1,
            Metric.Load => 2,
            _ => 3
        };

        var tabWidth = SegmentedGrid.ActualWidth > 0 ? SegmentedGrid.ActualWidth / 4.0 : 58.0;
        var targetX = idx * tabWidth;

        if (!animate)
        {
            SegThumbTransform.BeginAnimation(TranslateTransform.XProperty, null);
            SegThumbTransform.X = targetX;
            return;
        }

        var isGlass = _settings.Theme == VisualTheme.LiquidGlass;
        var anim = WithFrameRate(new DoubleAnimation(SegThumbTransform.X, targetX, TimeSpan.FromMilliseconds(isGlass ? 230 : 160))
        {
            EasingFunction = isGlass
                ? new BackEase { EasingMode = EasingMode.EaseOut, Amplitude = 0.2 }
                : new CubicEase { EasingMode = EasingMode.EaseOut }
        });
        SegThumbTransform.BeginAnimation(TranslateTransform.XProperty, anim);
    }

    private void CycleTimeRange(object sender, RoutedEventArgs args)
    {
        var next = _settings.HistoryRange switch
        {
            TimeRange.Seconds60 => TimeRange.Minutes5,
            TimeRange.Minutes5 => TimeRange.Minutes15,
            _ => TimeRange.Seconds60
        };

        _settings = _settings with { HistoryRange = next };
        UpdateRangeLabel();
        RefreshDashboardDetails();
        TimeRangeChanged?.Invoke(next);
    }

    private void CardDoubleClickReset(object sender, MouseButtonEventArgs args)
    {
        if (args.ClickCount == 2)
        {
            ResetPeaks();
            args.Handled = true;
        }
    }

    private void UpdateScrubTooltip(bool isGpu, ChartScrubInfo? info)
    {
        var pill = isGpu ? GpuScrubTooltip : CpuScrubTooltip;
        var text = isGpu ? GpuScrubText : CpuScrubText;
        var chart = isGpu ? GpuChart : CpuChart;

        if (info is not ChartScrubInfo scrub)
        {
            pill.Visibility = Visibility.Collapsed;
            return;
        }

        var timeLabel = scrub.SecondsAgo == 0
            ? "Now"
            : scrub.SecondsAgo >= 60
                ? $"-{scrub.SecondsAgo / 60}m{scrub.SecondsAgo % 60:00}s"
                : $"-{scrub.SecondsAgo}s";

        text.Text = $"{timeLabel}: {FormatMetric(scrub.Value)}";
        pill.Visibility = Visibility.Visible;
        pill.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));

        var pillW = pill.DesiredSize.Width;
        var chartW = Math.Max(1, chart.ActualWidth);
        var targetLeft = Math.Clamp(scrub.RelativeX * chartW - pillW / 2.0, 0, Math.Max(0, chartW - pillW));
        pill.Margin = new Thickness(targetLeft, -17, 0, 0);
    }

    private void StartDrag(object sender, MouseButtonEventArgs args)
    {
        _peekTimer.Stop();
        _collapseTimer.Stop();
        _dragStart = args.GetPosition(this);
        _dragSource = sender as UIElement;
    }

    private void EndDrag(object sender, MouseButtonEventArgs args)
    {
        _dragStart = null;
        _dragSource = null;
    }

    private void ContinueDrag(object sender, MouseEventArgs args)
    {
        if (_dragStart is not Point start || args.LeftButton != MouseButtonState.Pressed)
            return;

        var current = args.GetPosition(this);
        if (Math.Abs(current.X - start.X) < SystemParameters.MinimumHorizontalDragDistance
            && Math.Abs(current.Y - start.Y) < SystemParameters.MinimumVerticalDragDistance)
            return;

        _dragStart = null;
        args.Handled = true;
        _dragSource?.ReleaseMouseCapture();
        _dragSource = null;
        _peekTimer.Stop();
        _collapseTimer.Stop();
        _dragging = true;

        try
        {
            DragMove();
        }
        finally
        {
            _dragging = false;
        }

        // Convert dragged stage HWND top-left back to Minimal visual anchor.
        var hwndPos = OverlayWindow.GetPosition(Handle);
        var (dpiX, dpiY) = OverlayWindow.GetMonitorDpiScale(hwndPos, Handle);
        var chromeLeftPx = hwndPos.X + (int)Math.Round(ChromeHost.Margin.Left * dpiX);
        var chromeTopPx = hwndPos.Y + (int)Math.Round(ChromeHost.Margin.Top * dpiY);

        var (stateW, _, _) = GetDimensions(_state);
        var compactPxW = (int)Math.Round(MinimalWidth * dpiX);
        var statePxW = (int)Math.Round(stateW * dpiX);

        _anchor = new PixelPoint(chromeLeftPx + (statePxW - compactPxW) / 2, chromeTopPx);
        if (_state == IslandState.Minimal)
            _suppressHoverUntilLeave = true;

        PlaceOverlay();
        _anchor = _stagePlacement.CompactVisual.Location;
        SyncBackdropLive();
        OverlayMoved?.Invoke(_anchor.Value);
    }

    // =========================================================================
    // FORMATTING & COLOR HELPERS
    // =========================================================================

    private string FormatCollapsedSecondary(ProcessorReading? reading) => _settings.CollapsedMode switch
    {
        CollapsedDisplayMode.TempLoad => Format(reading?.Load, "%", "0"),
        CollapsedDisplayMode.TempOnly => string.Empty,
        _ => Format(reading?.Power, "W", "0.0")
    };

    private string FormatMetric(float? value) => _metric switch
    {
        Metric.Temperature => Format(value, "°C", "0"),
        Metric.Power => Format(value, "W", "0.0"),
        Metric.Load => Format(value, "%", "0"),
        _ => FormatClock(value)
    };

    private static string FormatClock(float? mhz)
    {
        if (mhz is not float number)
            return Missing;

        return number >= 1000f
            ? (number / 1000f).ToString("0.00", CultureInfo.CurrentCulture) + " GHz"
            : number.ToString("0", CultureInfo.CurrentCulture) + " MHz";
    }

    private static string Format(float? value, string unit, string format) =>
        value is float number ? number.ToString(format, CultureInfo.CurrentCulture) + unit : Missing;

    private Brush GetThermalBrush(float? temperature, bool isGpu)
    {
        if (temperature is not float temp)
            return QuietBrush;

        var hot = isGpu ? _settings.GpuHotTemp : _settings.CpuHotTemp;
        var warm = isGpu ? _settings.GpuWarmTemp : _settings.CpuWarmTemp;

        if (temp >= hot) return HotBrush;
        if (temp >= warm) return WarmBrush;
        return isGpu ? GpuBrush : CpuBrush;
    }

    private Brush GetCornerGlowBrush(float? temperature, bool isGpu, bool sleeping)
    {
        if (sleeping)
            return GpuGlowSleep;

        if (temperature is float temp)
        {
            var hot = isGpu ? _settings.GpuHotTemp : _settings.CpuHotTemp;
            var warm = isGpu ? _settings.GpuWarmTemp : _settings.CpuWarmTemp;

            if (temp >= hot) return isGpu ? GpuGlowHot : CpuGlowHot;
            if (temp >= warm) return isGpu ? GpuGlowWarm : CpuGlowWarm;
        }

        return isGpu ? GpuGlowNormal : CpuGlowNormal;
    }

    private static void ApplyThrottleBadge(Border badge, TextBlock label, ThrottleFlags flags)
    {
        if (flags == ThrottleFlags.None)
        {
            if (badge.Visibility != Visibility.Collapsed)
                badge.Visibility = Visibility.Collapsed;
            return;
        }

        var isThermal = flags.HasFlag(ThrottleFlags.Thermal);
        var text = isThermal ? "THRM" : "PWR";
        if (badge.Visibility == Visibility.Visible && string.Equals(label.Text, text, StringComparison.Ordinal))
            return;

        label.Text = text;
        label.Foreground = isThermal ? HotBrush : WarmBrush;
        badge.Background = isThermal ? ThrottleThermalBg : ThrottlePowerBg;
        badge.BorderBrush = isThermal ? ThrottleThermalBorder : ThrottlePowerBorder;
        badge.Visibility = Visibility.Visible;
    }

    private static T WithFrameRate<T>(T timeline) where T : Timeline
    {
        Timeline.SetDesiredFrameRate(timeline, TransitionFrameRate);
        return timeline;
    }

    private static void SetText(TextBlock tb, string value)
    {
        if (!string.Equals(tb.Text, value, StringComparison.Ordinal))
            tb.Text = value;
    }

    private static void SetForeground(TextBlock tb, Brush brush)
    {
        if (!ReferenceEquals(tb.Foreground, brush))
            tb.Foreground = brush;
    }

    private static void SetFill(System.Windows.Shapes.Shape shape, Brush brush)
    {
        if (!ReferenceEquals(shape.Fill, brush))
            shape.Fill = brush;
    }

    private static void SetVisibility(UIElement element, Visibility visibility)
    {
        if (element.Visibility != visibility)
            element.Visibility = visibility;
    }

    private string? GetWarningStatus(SensorSnapshot snapshot)
    {
        if (snapshot.Error is not null)
            return "Sensor polling interrupted · retrying…";

        if (snapshot.Cpu is not { Temperature: not null, Power: not null })
        {
            if (!snapshot.PawnIoInstalled)
                return "Install PawnIO to enable Intel/AMD CPU sensors";
            if (!_elevated)
                return "CPU sensors need administrator access (Tray → Restart as admin)";

            return "Some CPU sensors are unavailable";
        }

        if (snapshot.Gpu is not { IsSleeping: true } and not { Temperature: not null, Power: not null })
            return "Some GPU sensors are unavailable";

        return null;
    }

    private static Brush FreezeBrush(byte r, byte g, byte b) => FreezeBrush(255, r, g, b);

    private static Brush FreezeBrush(byte a, byte r, byte g, byte b)
    {
        var brush = new SolidColorBrush(Color.FromArgb(a, r, g, b));
        brush.Freeze();
        return brush;
    }

    private static Brush CreateCornerGlow(double cx, double cy, byte alpha, byte r, byte g, byte b)
    {
        var brush = new RadialGradientBrush
        {
            Center = new Point(cx, cy),
            GradientOrigin = new Point(cx, cy),
            RadiusX = 0.56,
            RadiusY = 0.72
        };
        brush.GradientStops.Add(new GradientStop(Color.FromArgb(alpha, r, g, b), 0.0));
        brush.GradientStops.Add(new GradientStop(Color.FromArgb(0, r, g, b), 1.0));
        brush.Freeze();
        return brush;
    }

    private static Brush CreateDotGlow(byte r, byte g, byte b)
    {
        var brush = new RadialGradientBrush(
            Color.FromArgb(153, r, g, b),
            Color.FromArgb(0, r, g, b));
        brush.Freeze();
        return brush;
    }
}
