using System;

namespace Mmoni.Monitoring;

internal sealed class RingBuffer
{
    public const int MaxCapacity = 900; // 15 minutes at 1 Hz

    private readonly ProcessorReading?[] _items = new ProcessorReading?[MaxCapacity];
    private int _head;
    private int _count;

    private float? _sessionTempPeak;
    private float? _sessionPowerPeak;
    private float? _sessionLoadPeak;
    private float? _sessionClockPeak;

    public int Count => _count;

    public void Add(ProcessorReading? reading)
    {
        _items[_head] = reading;
        _head = (_head + 1) % MaxCapacity;
        if (_count < MaxCapacity)
            _count++;

        if (reading is { IsSleeping: false } value)
        {
            UpdatePeak(ref _sessionTempPeak, value.Temperature);
            UpdatePeak(ref _sessionPowerPeak, value.Power);
            UpdatePeak(ref _sessionLoadPeak, value.Load);
            UpdatePeak(ref _sessionClockPeak, value.Clock);
        }
    }

    public void Clear()
    {
        Array.Clear(_items);
        _head = 0;
        _count = 0;
        ResetPeaks();
    }

    public void ResetPeaks()
    {
        _sessionTempPeak = null;
        _sessionPowerPeak = null;
        _sessionLoadPeak = null;
        _sessionClockPeak = null;
    }

    public float? GetSessionPeak(Metric metric) => metric switch
    {
        Metric.Temperature => _sessionTempPeak,
        Metric.Power => _sessionPowerPeak,
        Metric.Load => _sessionLoadPeak,
        Metric.Clock => _sessionClockPeak,
        _ => null
    };

    public int CopyRecentValues(int windowSize, Metric metric, Span<float?> destination)
    {
        var take = Math.Min(_count, Math.Min(windowSize, destination.Length));
        if (take <= 0)
            return 0;

        var start = (_head - take + MaxCapacity) % MaxCapacity;
        for (var i = 0; i < take; i++)
        {
            var reading = _items[(start + i) % MaxCapacity];
            destination[i] = reading is { IsSleeping: false } r ? r.Get(metric) : null;
        }

        return take;
    }

    public bool HasAnyValue(int windowSize, Metric metric)
    {
        var take = Math.Min(_count, windowSize);
        if (take <= 0)
            return false;

        var start = (_head - take + MaxCapacity) % MaxCapacity;
        for (var i = 0; i < take; i++)
        {
            var reading = _items[(start + i) % MaxCapacity];
            if (reading is { IsSleeping: false } r && r.Get(metric) is not null)
                return true;
        }

        return false;
    }

    private static void UpdatePeak(ref float? peak, float? value)
    {
        if (value is float v && float.IsFinite(v) && (peak is null || v > peak.Value))
            peak = v;
    }
}
