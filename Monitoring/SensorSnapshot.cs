using System;
using System.Collections.Generic;

namespace Mmoni.Monitoring;

internal enum Metric
{
    Temperature,
    Power,
    Load,
    Clock
}

internal enum TimeRange
{
    Seconds60 = 60,
    Minutes5 = 300,
    Minutes15 = 900
}

internal enum VisualTheme
{
    LiquidGlass,
    PrecisionDark
}

internal enum ChartLayoutMode
{
    Split,
    Combined
}

internal enum CollapsedDisplayMode
{
    TempPower,
    TempLoad,
    TempOnly,
    TempPowerSpark
}

[Flags]
internal enum ThrottleFlags
{
    None = 0,
    Thermal = 1,
    Power = 2
}

internal readonly record struct ProcessorReading(
    float? Temperature,
    float? Power,
    float? Load,
    float? Clock,
    ThrottleFlags Throttle = ThrottleFlags.None,
    bool IsSleeping = false)
{
    public float? Get(Metric metric) => metric switch
    {
        Metric.Temperature => Temperature,
        Metric.Power => Power,
        Metric.Load => Load,
        Metric.Clock => Clock,
        _ => null
    };
}

internal sealed record GpuDevice(string Id, string Name, bool IsDiscrete = false);

internal sealed record SensorSnapshot(
    string? CpuName,
    ProcessorReading? Cpu,
    GpuDevice? SelectedGpu,
    ProcessorReading? Gpu,
    IReadOnlyList<GpuDevice> Gpus,
    bool PawnIoInstalled,
    string? Error = null);
