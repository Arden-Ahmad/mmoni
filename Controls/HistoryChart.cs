using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using Mmoni.Monitoring;

namespace Mmoni.Controls;

internal readonly record struct ChartStats(
    bool HasData,
    float? Min,
    float? Max,
    float? Avg,
    float? Latest);

internal readonly record struct ChartScrubInfo(
    int SecondsAgo,
    float Value,
    double RelativeX);

public sealed class HistoryChart : FrameworkElement
{
    private const int MaxRenderPoints = 90;

    private static readonly Pen GridPenGlass = CreatePen(Color.FromArgb(18, 255, 255, 255), 1, dashed: false);
    private static readonly Pen GridPenMinimal = CreatePen(Color.FromArgb(24, 255, 255, 255), 1, dashed: true);
    private static readonly Pen ScrubLinePen = CreatePen(Color.FromArgb(80, 255, 255, 255), 1, dashed: true);

    private static readonly Color CpuBaseColor = Color.FromRgb(111, 231, 207);
    private static readonly Color GpuBaseColor = Color.FromRgb(178, 184, 255);
    private static readonly Color WarmColor = Color.FromRgb(251, 191, 36);
    private static readonly Color HotColor = Color.FromRgb(251, 113, 133);

    private static readonly Pen CpuPenGlass = CreatePen(CpuBaseColor, 1.85);
    private static readonly Pen GpuPenGlass = CreatePen(GpuBaseColor, 1.85);
    private static readonly Pen WarmPenGlass = CreatePen(WarmColor, 1.85);
    private static readonly Pen HotPenGlass = CreatePen(HotColor, 1.85);

    private static readonly Pen CpuPenSharp = CreatePen(CpuBaseColor, 1.45);
    private static readonly Pen GpuPenSharp = CreatePen(GpuBaseColor, 1.45);
    private static readonly Pen WarmPenSharp = CreatePen(WarmColor, 1.45);
    private static readonly Pen HotPenSharp = CreatePen(HotColor, 1.45);

    private static readonly Brush CpuFillGlass = CreateFill(CpuBaseColor, 88);
    private static readonly Brush GpuFillGlass = CreateFill(GpuBaseColor, 88);
    private static readonly Brush WarmFillGlass = CreateFill(WarmColor, 76);
    private static readonly Brush HotFillGlass = CreateFill(HotColor, 82);

    private static readonly Brush CpuFillSharp = CreateFill(CpuBaseColor, 46);
    private static readonly Brush GpuFillSharp = CreateFill(GpuBaseColor, 46);
    private static readonly Brush WarmFillSharp = CreateFill(WarmColor, 42);
    private static readonly Brush HotFillSharp = CreateFill(HotColor, 46);

    private static readonly Brush CpuGlowBrush = CreateGlow(CpuBaseColor);
    private static readonly Brush GpuGlowBrush = CreateGlow(GpuBaseColor);
    private static readonly Brush WarmGlowBrush = CreateGlow(WarmColor);
    private static readonly Brush HotGlowBrush = CreateGlow(HotColor);

    private static readonly Brush WhiteBrush = CreateFrozenBrush(Colors.White);

    private readonly float?[] _rawBuffer = new float?[RingBuffer.MaxCapacity];
    private readonly float?[] _plotValues = new float?[MaxRenderPoints];
    private readonly List<Point> _segment = new(MaxRenderPoints);
    private readonly Point[] _allPoints = new Point[MaxRenderPoints];
    private readonly bool[] _pointValid = new bool[MaxRenderPoints];

    private int _plotCount;
    private int _windowSeconds = 60;
    private int _bucketSeconds = 1;
    private Metric _metric = Metric.Temperature;
    private VisualTheme _theme = VisualTheme.LiquidGlass;
    private float _warmThreshold = 80f;
    private float _hotThreshold = 90f;
    private float? _latestValue;
    private float _dataMin;
    private float _dataMax;
    private int? _scrubSlot;

    public bool IsGpu { get; set; }

    internal event Action<ChartScrubInfo?>? ScrubChanged;

    public HistoryChart()
    {
        Cursor = Cursors.Cross;
    }

    internal ChartStats Display(
        RingBuffer history,
        Metric metric,
        TimeRange range,
        VisualTheme theme,
        float warmThreshold,
        float hotThreshold)
    {
        _metric = metric;
        _theme = theme;
        _windowSeconds = (int)range;
        _warmThreshold = warmThreshold;
        _hotThreshold = hotThreshold;

        var rawCount = history.CopyRecentValues(_windowSeconds, metric, _rawBuffer);

        float? min = null;
        float? max = null;
        float? latest = null;
        double sum = 0;
        var validCount = 0;

        for (var i = 0; i < rawCount; i++)
        {
            if (_rawBuffer[i] is float v)
            {
                if (min is null || v < min.Value) min = v;
                if (max is null || v > max.Value) max = v;
                latest = v;
                sum += v;
                validCount++;
            }
        }

        _latestValue = latest;
        _dataMin = min ?? 0f;
        _dataMax = max ?? 0f;

        // Downsample cleanly when viewing 5m (300s -> 75 buckets of 4s) or 15m (900s -> 90 buckets of 10s).
        _bucketSeconds = _windowSeconds switch
        {
            <= 60 => 1,
            <= 300 => 4,
            _ => 10
        };

        var totalSlots = _windowSeconds / _bucketSeconds;
        _plotCount = Math.Min(totalSlots, MaxRenderPoints);

        if (_bucketSeconds == 1)
        {
            var offset = _plotCount - rawCount;
            for (var slot = 0; slot < _plotCount; slot++)
            {
                var rawIdx = slot - offset;
                _plotValues[slot] = rawIdx >= 0 && rawIdx < rawCount ? _rawBuffer[rawIdx] : null;
            }
        }
        else
        {
            // Align buckets to the newest sample on the right edge.
            for (var slot = 0; slot < _plotCount; slot++)
            {
                var slotsFromEnd = _plotCount - 1 - slot;
                var endExclusive = rawCount - slotsFromEnd * _bucketSeconds;
                var start = endExclusive - _bucketSeconds;
                if (endExclusive <= 0)
                {
                    _plotValues[slot] = null;
                    continue;
                }

                var clampedStart = Math.Max(0, start);
                var clampedEnd = Math.Min(rawCount, endExclusive);
                double bucketSum = 0;
                var bucketCount = 0;
                for (var i = clampedStart; i < clampedEnd; i++)
                {
                    if (_rawBuffer[i] is float v)
                    {
                        bucketSum += v;
                        bucketCount++;
                    }
                }

                _plotValues[slot] = bucketCount > 0 ? (float)(bucketSum / bucketCount) : null;
            }
        }

        if (_scrubSlot is int slotIndex)
            NotifyScrub(slotIndex);

        InvalidateVisual();
        return new ChartStats(
            validCount > 0,
            min,
            max,
            validCount > 0 ? (float)(sum / validCount) : null,
            latest);
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        if (_plotCount <= 1 || ActualWidth <= 4)
            return;

        var x = e.GetPosition(this).X;
        var ratio = Math.Clamp((x - 2) / (ActualWidth - 4), 0, 1);
        var slot = (int)Math.Round(ratio * (_plotCount - 1));
        if (_scrubSlot != slot)
        {
            _scrubSlot = slot;
            NotifyScrub(slot);
            InvalidateVisual();
        }
    }

    protected override void OnMouseLeave(MouseEventArgs e)
    {
        base.OnMouseLeave(e);
        if (_scrubSlot is not null)
        {
            _scrubSlot = null;
            ScrubChanged?.Invoke(null);
            InvalidateVisual();
        }
    }

    private void NotifyScrub(int slot)
    {
        if (slot < 0 || slot >= _plotCount || _plotValues[slot] is not float value)
        {
            ScrubChanged?.Invoke(null);
            return;
        }

        var secondsAgo = (_plotCount - 1 - slot) * _bucketSeconds;
        var relX = _plotCount > 1 ? (double)slot / (_plotCount - 1) : 1.0;
        ScrubChanged?.Invoke(new ChartScrubInfo(secondsAgo, value, relX));
    }

    protected override void OnRender(DrawingContext context)
    {
        var fullW = ActualWidth;
        var fullH = ActualHeight;
        if (fullW <= 6 || fullH <= 6)
            return;

        // Ensure hit-testing works across the entire chart bounds for scrubbing.
        context.DrawRectangle(Brushes.Transparent, null, new Rect(0, 0, fullW, fullH));

        var isGlass = _theme == VisualTheme.LiquidGlass;
        var padX = 2.0;
        var padTop = 8.0;
        var padBottom = 6.0;
        var width = fullW - padX * 2;
        var height = Math.Max(4, fullH - padTop - padBottom);
        var bottomY = fullH - 1;

        var gridPen = isGlass ? GridPenGlass : GridPenMinimal;
        context.DrawLine(gridPen, new Point(padX, padTop + height * 0.2), new Point(fullW - padX, padTop + height * 0.2));
        context.DrawLine(gridPen, new Point(padX, padTop + height * 0.65), new Point(fullW - padX, padTop + height * 0.65));

        if (_plotCount <= 0)
            return;

        var minSpan = _metric switch
        {
            Metric.Temperature => 8f,
            Metric.Power => 8f,
            Metric.Load => 15f,
            _ => 300f
        };

        var span = Math.Max(_dataMax - _dataMin, minSpan);
        var yMin = Math.Max(_metric == Metric.Temperature ? 15f : 0f, _dataMin - span * 0.14f);
        var yMax = _dataMax + span * 0.14f;
        if (_metric == Metric.Load)
            yMax = Math.Min(100f, Math.Max(yMax, yMin + minSpan));
        if (yMax <= yMin)
            yMax = yMin + minSpan;

        var (pen, fill, glow) = SelectBrushes(isGlass);

        Array.Clear(_pointValid, 0, _plotCount);
        _segment.Clear();
        var latestSlot = -1;

        for (var i = 0; i <= _plotCount; i++)
        {
            if (i < _plotCount && _plotValues[i] is float value)
            {
                var x = _plotCount > 1 ? padX + width * i / (_plotCount - 1) : fullW - padX;
                var fraction = Math.Clamp((value - yMin) / (yMax - yMin), 0, 1);
                var pt = new Point(x, padTop + height * (1 - fraction));
                _allPoints[i] = pt;
                _pointValid[i] = true;
                latestSlot = i;
                _segment.Add(pt);
                continue;
            }

            if (_segment.Count > 0)
            {
                DrawSegment(context, _segment, bottomY, pen, fill, isGlass);
                _segment.Clear();
            }
        }

        // Live endpoint dot with soft halo.
        if (latestSlot >= 0 && _pointValid[latestSlot])
        {
            var tip = _allPoints[latestSlot];
            if (isGlass)
                context.DrawEllipse(glow, null, tip, 5.5, 5.5);

            context.DrawEllipse(pen.Brush, null, tip, isGlass ? 2.6 : 2.1, isGlass ? 2.6 : 2.1);
        }

        // Interactive scrub crosshair & dot.
        if (_scrubSlot is int s && s >= 0 && s < _plotCount && _pointValid[s])
        {
            var sp = _allPoints[s];
            context.DrawLine(ScrubLinePen, new Point(sp.X, 1), new Point(sp.X, fullH - 1));
            context.DrawEllipse(WhiteBrush, pen, sp, 3.0, 3.0);
        }
    }

    private (Pen Pen, Brush Fill, Brush Glow) SelectBrushes(bool isGlass)
    {
        if (_metric == Metric.Temperature && _latestValue is float temp)
        {
            if (temp >= _hotThreshold)
                return (isGlass ? HotPenGlass : HotPenSharp, isGlass ? HotFillGlass : HotFillSharp, HotGlowBrush);
            if (temp >= _warmThreshold)
                return (isGlass ? WarmPenGlass : WarmPenSharp, isGlass ? WarmFillGlass : WarmFillSharp, WarmGlowBrush);
        }

        return IsGpu
            ? (isGlass ? GpuPenGlass : GpuPenSharp, isGlass ? GpuFillGlass : GpuFillSharp, GpuGlowBrush)
            : (isGlass ? CpuPenGlass : CpuPenSharp, isGlass ? CpuFillGlass : CpuFillSharp, CpuGlowBrush);
    }

    private static void DrawSegment(
        DrawingContext context,
        List<Point> points,
        double bottomY,
        Pen pen,
        Brush fill,
        bool smooth)
    {
        if (points.Count < 2)
            return;

        var area = new StreamGeometry();
        using (var g = area.Open())
        {
            g.BeginFigure(new Point(points[0].X, bottomY), isFilled: true, isClosed: true);
            g.LineTo(points[0], isStroked: false, isSmoothJoin: false);
            TraceCurve(g, points, smooth, isStroked: false);
            g.LineTo(new Point(points[^1].X, bottomY), isStroked: false, isSmoothJoin: false);
        }
        area.Freeze();
        context.DrawGeometry(fill, null, area);

        var line = new StreamGeometry();
        using (var g = line.Open())
        {
            g.BeginFigure(points[0], isFilled: false, isClosed: false);
            TraceCurve(g, points, smooth, isStroked: true);
        }
        line.Freeze();
        context.DrawGeometry(null, pen, line);
    }

    private static void TraceCurve(StreamGeometryContext g, List<Point> pts, bool smooth, bool isStroked)
    {
        if (!smooth || pts.Count < 3)
        {
            for (var i = 1; i < pts.Count; i++)
                g.LineTo(pts[i], isStroked, isSmoothJoin: true);
            return;
        }

        // Catmull-Rom to cubic Bezier segments with overshoot clamping.
        for (var i = 0; i < pts.Count - 1; i++)
        {
            var p0 = pts[Math.Max(0, i - 1)];
            var p1 = pts[i];
            var p2 = pts[i + 1];
            var p3 = pts[Math.Min(pts.Count - 1, i + 2)];

            var minY = Math.Min(p1.Y, p2.Y);
            var maxY = Math.Max(p1.Y, p2.Y);

            var cp1 = new Point(
                p1.X + (p2.X - p0.X) / 6.0,
                Math.Clamp(p1.Y + (p2.Y - p0.Y) / 6.0, minY - 2, maxY + 2));
            var cp2 = new Point(
                p2.X - (p3.X - p1.X) / 6.0,
                Math.Clamp(p2.Y - (p3.Y - p1.Y) / 6.0, minY - 2, maxY + 2));

            g.BezierTo(cp1, cp2, p2, isStroked, isSmoothJoin: true);
        }
    }

    private static Pen CreatePen(Color color, double thickness, bool dashed = false)
    {
        var brush = new SolidColorBrush(color);
        brush.Freeze();
        var pen = new Pen(brush, thickness)
        {
            StartLineCap = PenLineCap.Round,
            EndLineCap = PenLineCap.Round,
            LineJoin = PenLineJoin.Round
        };
        if (dashed)
            pen.DashStyle = new DashStyle([2, 2.5], 0);
        pen.Freeze();
        return pen;
    }

    private static Brush CreateFill(Color color, byte topAlpha)
    {
        var brush = new LinearGradientBrush(
            Color.FromArgb(topAlpha, color.R, color.G, color.B),
            Color.FromArgb(0, color.R, color.G, color.B),
            new Point(0, 0),
            new Point(0, 1));
        brush.Freeze();
        return brush;
    }

    private static Brush CreateGlow(Color color)
    {
        var brush = new RadialGradientBrush(
            Color.FromArgb(145, color.R, color.G, color.B),
            Color.FromArgb(0, color.R, color.G, color.B));
        brush.Freeze();
        return brush;
    }

    private static Brush CreateFrozenBrush(Color color)
    {
        var brush = new SolidColorBrush(color);
        brush.Freeze();
        return brush;
    }
}
