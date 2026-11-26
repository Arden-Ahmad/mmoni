using System;
using System.Diagnostics;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;
using Microsoft.Win32;
using Mmoni.Monitoring;

namespace Mmoni.Platform;

internal sealed record UserSettings
{
    private static readonly string SettingsPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Mmoni", "settings.json");

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        Converters = { new JsonStringEnumConverter() }
    };

    private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";

    public int? Left { get; init; }
    public int? Top { get; init; }
    public bool ClickThrough { get; init; }
    public bool AutoExpandOnHover { get; init; } = true;
    public bool AutoHideInFullscreen { get; init; } = true;
    public string? GpuId { get; init; }

    public VisualTheme Theme { get; init; } = VisualTheme.LiquidGlass;
    public ChartLayoutMode ChartLayout { get; init; } = ChartLayoutMode.Split;
    public CollapsedDisplayMode CollapsedMode { get; init; } = CollapsedDisplayMode.TempPower;
    public TimeRange HistoryRange { get; init; } = TimeRange.Seconds60;

    public float CpuWarmTemp { get; init; } = 80f;
    public float CpuHotTemp { get; init; } = 90f;
    public float GpuWarmTemp { get; init; } = 78f;
    public float GpuHotTemp { get; init; } = 87f;

    public static UserSettings Load()
    {
        try
        {
            if (File.Exists(SettingsPath))
                return JsonSerializer.Deserialize<UserSettings>(File.ReadAllText(SettingsPath), JsonOptions) ?? new();
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or JsonException)
        {
            Trace.TraceWarning("Mmoni settings could not be read: {0}", exception.Message);
        }

        return new UserSettings();
    }

    public void Save()
    {
        var temporaryPath = SettingsPath + ".tmp";
        Directory.CreateDirectory(Path.GetDirectoryName(SettingsPath)!);
        File.WriteAllText(temporaryPath, JsonSerializer.Serialize(this, JsonOptions));
        File.Move(temporaryPath, SettingsPath, overwrite: true);
    }

    public static bool IsStartupEnabled()
    {
        using var key = Registry.CurrentUser.OpenSubKey(RunKey);
        return key?.GetValue("Mmoni") is string;
    }

    public static void SetStartupEnabled(bool enabled)
    {
        using var key = Registry.CurrentUser.CreateSubKey(RunKey);
        if (enabled)
        {
            var executable = Environment.ProcessPath ?? throw new InvalidOperationException("Executable path unavailable.");
            key.SetValue("Mmoni", $"\"{executable}\"", RegistryValueKind.String);
        }
        else
        {
            key.DeleteValue("Mmoni", throwOnMissingValue: false);
        }
    }
}
