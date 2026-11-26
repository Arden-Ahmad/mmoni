using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using LibreHardwareMonitor.Hardware;
using PawnIo = LibreHardwareMonitor.PawnIo.PawnIo;

namespace Mmoni.Monitoring;

internal sealed class SensorMonitor : IDisposable
{
    private static readonly string[] CpuTemperatures =
    [
        "CPU Package", "Core (Tctl/Tdie)", "CPU (Tctl/Tdie)", "CPU Die (average)",
        "CPU Die Average", "CPU (Tdie)", "Core Max", "Core Average"
    ];

    private static readonly string[] CpuPowers = ["CPU Package", "Package", "CPU PPT", "CPU Package Power", "PPT"];
    private static readonly string[] GpuTemperatures = ["GPU Core", "GPU Temperature"];
    private static readonly string[] GpuPowers = ["GPU Package", "GPU Total Board Power", "GPU Board Power", "GPU Power", "GPU Chip Power"];
    private static readonly string[] CpuLoads = ["CPU Total"];
    private static readonly string[] GpuLoads = ["GPU Core", "GPU Total", "D3D 3D"];
    private static readonly string[] CpuClocks = ["CPU Core Average"];
    private static readonly string[] GpuClocks = ["GPU Core", "GPU Graphics"];

    private readonly CancellationTokenSource _stop = new();
    private readonly Task _worker;
    private string? _preferredGpuId;
    private int _paused;

    public SensorMonitor(string? preferredGpuId, Action<SensorSnapshot> publish)
    {
        _preferredGpuId = preferredGpuId;
        _worker = Task.Run(() => PollAsync(publish, _stop.Token));
    }

    public void SelectGpu(string? id) => Volatile.Write(ref _preferredGpuId, id);

    public void SetPaused(bool paused) => Volatile.Write(ref _paused, paused ? 1 : 0);

    private async Task PollAsync(Action<SensorSnapshot> publish, CancellationToken token)
    {
        try
        {
            while (!token.IsCancellationRequested)
            {
                try
                {
                    await ReadComputerAsync(publish, token).ConfigureAwait(false);
                }
                catch (Exception exception) when (!token.IsCancellationRequested)
                {
                    Trace.TraceError("Mmoni sensor polling: {0}", exception);
                    publish(new SensorSnapshot(null, null, null, null, [], false, exception.Message));
                    await Task.Delay(TimeSpan.FromSeconds(5), token).ConfigureAwait(false);
                }
            }
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested)
        {
        }
    }

    private async Task ReadComputerAsync(Action<SensorSnapshot> publish, CancellationToken token)
    {
        var computer = new Computer { IsCpuEnabled = true, IsGpuEnabled = true };
        var pawnIoInstalled = PawnIo.IsInstalled;
        try
        {
            computer.Open();
            var topology = BuildTopology(computer);
            using var timer = new PeriodicTimer(TimeSpan.FromSeconds(1));
            do
            {
                token.ThrowIfCancellationRequested();
                if (Volatile.Read(ref _paused) == 0)
                    publish(ReadSnapshot(topology, pawnIoInstalled));
            }
            while (await timer.WaitForNextTickAsync(token).ConfigureAwait(false));
        }
        finally
        {
            computer.Close();
        }
    }

    private static HardwareTopology BuildTopology(Computer computer)
    {
        var cpuHw = computer.Hardware.FirstOrDefault(device => device.HardwareType == HardwareType.Cpu);
        var cpuCache = cpuHw is null ? null : new CachedHardware(cpuHw, isCpu: true, isDiscreteGpu: false, device: null);

        var gpuEntries = computer.Hardware
            .Where(device => device.HardwareType is HardwareType.GpuNvidia or HardwareType.GpuAmd or HardwareType.GpuIntel)
            .OrderBy(device => device.HardwareType == HardwareType.GpuIntel)
            .Select(device =>
            {
                var isDiscrete = device.HardwareType is HardwareType.GpuNvidia or HardwareType.GpuAmd;
                var gpuDevice = new GpuDevice(device.Identifier.ToString(), device.Name, isDiscrete);
                return new CachedHardware(device, isCpu: false, isDiscreteGpu: isDiscrete, device: gpuDevice);
            })
            .ToArray();

        var hasHybridGpus = gpuEntries.Length > 1
            && gpuEntries.Any(g => g.IsDiscreteGpu)
            && gpuEntries.Any(g => !g.IsDiscreteGpu);

        var gpuList = gpuEntries.Select(g => g.Device!).ToArray();
        return new HardwareTopology(cpuCache, gpuEntries, gpuList, hasHybridGpus);
    }

    private SensorSnapshot ReadSnapshot(HardwareTopology topology, bool pawnIoInstalled)
    {
        var preferredId = Volatile.Read(ref _preferredGpuId);
        CachedHardware? selectedGpu = null;
        if (preferredId is not null)
        {
            foreach (var entry in topology.Gpus)
            {
                if (entry.Device!.Id == preferredId)
                {
                    selectedGpu = entry;
                    break;
                }
            }
        }

        selectedGpu ??= topology.Gpus.Length > 0 ? topology.Gpus[0] : null;

        var cpuReading = topology.Cpu?.Read(checkSleep: false);
        var gpuReading = selectedGpu?.Read(checkSleep: topology.HasHybridGpus && selectedGpu.IsDiscreteGpu);

        return new SensorSnapshot(
            topology.Cpu?.Hardware.Name,
            cpuReading,
            selectedGpu?.Device,
            gpuReading,
            topology.GpuDevices,
            pawnIoInstalled);
    }

    private sealed class HardwareTopology(
        CachedHardware? cpu,
        CachedHardware[] gpus,
        GpuDevice[] gpuDevices,
        bool hasHybridGpus)
    {
        public CachedHardware? Cpu { get; } = cpu;
        public CachedHardware[] Gpus { get; } = gpus;
        public GpuDevice[] GpuDevices { get; } = gpuDevices;
        public bool HasHybridGpus { get; } = hasHybridGpus;
    }

    private sealed class CachedHardware
    {
        private readonly bool _isCpu;
        private readonly bool _isIntelCpu;
        private readonly string? _vendorToken;
        private bool _sensorsResolved;

        private ISensor? _temperature;
        private ISensor? _power;
        private ISensor? _load;
        private ISensor? _clock;
        private ISensor[] _coreClocks = [];
        private ISensor[] _thermalThrottles = [];
        private ISensor[] _powerThrottles = [];

        public CachedHardware(IHardware hardware, bool isCpu, bool isDiscreteGpu, GpuDevice? device)
        {
            Hardware = hardware;
            _isCpu = isCpu;
            IsDiscreteGpu = isDiscreteGpu;
            Device = device;
            _isIntelCpu = isCpu && hardware.Identifier.ToString().StartsWith("/intelcpu/", StringComparison.Ordinal);
            _vendorToken = hardware.HardwareType switch
            {
                HardwareType.GpuNvidia => "VEN_10DE",
                HardwareType.GpuAmd => "VEN_1002",
                _ => null
            };
        }

        public IHardware Hardware { get; }
        public bool IsDiscreteGpu { get; }
        public GpuDevice? Device { get; }

        public ProcessorReading? Read(bool checkSleep)
        {
            if (checkSleep && _vendorToken is not null && GpuPowerStateDetector.IsVendorGpuSleeping(_vendorToken))
                return new ProcessorReading(null, null, null, null, ThrottleFlags.None, IsSleeping: true);

            try
            {
                Hardware.Update();
                if (!_sensorsResolved)
                    ResolveSensors();

                var temperature = ReadValue(_temperature);
                var power = ReadValue(_power);

                // The Intel CPU backend can expose zero package watts when MSR access fails.
                if (_isIntelCpu && temperature is null && power is 0f)
                    power = null;

                var load = ReadValue(_load);
                var clock = ReadValue(_clock) ?? MaxCoreClock();

                var throttle = ThrottleFlags.None;
                if (IsAnyActive(_thermalThrottles))
                    throttle |= ThrottleFlags.Thermal;
                if (IsAnyActive(_powerThrottles))
                    throttle |= ThrottleFlags.Power;

                // Retry sensor resolution next tick if primary metrics were all missing (e.g., driver wake).
                if (temperature is null && power is null && load is null)
                    _sensorsResolved = false;

                return new ProcessorReading(temperature, power, load, clock, throttle, IsSleeping: false);
            }
            catch (Exception exception)
            {
                Trace.TraceWarning("Mmoni could not update {0}: {1}", Hardware.Name, exception);
                _sensorsResolved = false;
                return null;
            }
        }

        private void ResolveSensors()
        {
            var sensors = Hardware.Sensors;
            _temperature = FindSensor(sensors, SensorType.Temperature, _isCpu ? CpuTemperatures : GpuTemperatures);
            _power = FindSensor(sensors, SensorType.Power, _isCpu ? CpuPowers : GpuPowers);
            _load = FindSensor(sensors, SensorType.Load, _isCpu ? CpuLoads : GpuLoads);
            _clock = FindSensor(sensors, SensorType.Clock, _isCpu ? CpuClocks : GpuClocks);

            if (_isCpu && _clock is null)
            {
                _coreClocks = sensors
                    .Where(s => s.SensorType == SensorType.Clock
                        && s.Name.Contains("Core", StringComparison.OrdinalIgnoreCase))
                    .ToArray();
            }
            else
            {
                _coreClocks = [];
            }

            _thermalThrottles = sensors
                .Where(s => s.SensorType is SensorType.Factor or SensorType.Level or SensorType.SmallData or SensorType.Control
                    && (s.Name.Contains("Thermal Throttling", StringComparison.OrdinalIgnoreCase)
                        || s.Name.Contains("Thermal Limit", StringComparison.OrdinalIgnoreCase)
                        || s.Name.Contains("PROCHOT", StringComparison.OrdinalIgnoreCase)))
                .ToArray();

            _powerThrottles = sensors
                .Where(s => s.SensorType is SensorType.Factor or SensorType.Level or SensorType.SmallData or SensorType.Control
                    && (s.Name.Contains("Power Limit", StringComparison.OrdinalIgnoreCase)
                        || s.Name.Contains("Power Throttling", StringComparison.OrdinalIgnoreCase)))
                .ToArray();

            _sensorsResolved = sensors.Length > 0;
        }

        private static ISensor? FindSensor(ISensor[] sensors, SensorType type, string[] names)
        {
            foreach (var name in names)
            {
                foreach (var sensor in sensors)
                {
                    if (sensor.SensorType == type && sensor.Name.Equals(name, StringComparison.OrdinalIgnoreCase))
                        return sensor;
                }
            }

            return null;
        }

        private static float? ReadValue(ISensor? sensor) =>
            sensor?.Value is float value && float.IsFinite(value) ? value : null;

        private float? MaxCoreClock()
        {
            float? max = null;
            foreach (var sensor in _coreClocks)
            {
                if (sensor.Value is float value && float.IsFinite(value) && value > 0 && (max is null || value > max.Value))
                    max = value;
            }

            return max;
        }

        private static bool IsAnyActive(ISensor[] sensors)
        {
            foreach (var sensor in sensors)
            {
                if (sensor.Value is float value && float.IsFinite(value) && value >= 0.5f)
                    return true;
            }

            return false;
        }
    }

    /// <summary>
    /// Queries Windows PnP Device Power Data (DEVPKEY_Device_PowerData) without waking the dGPU via NVAPI/ADL.
    /// </summary>
    private static class GpuPowerStateDetector
    {
        private const string DisplayClassGuid = "{4d36e968-e325-11ce-bfc1-08002be10318}";
        private const uint CmGetIdListFilterClass = 0x00000200;
        private const uint CmGetIdListFilterPresent = 0x00000100;
        private const uint CmLocateDevNodeNormal = 0x00000000;
        private const uint CrSuccess = 0;
        private const int PowerDeviceD3 = 4;

        private static readonly DevPropKey DevPkeyDevicePowerData = new()
        {
            FmtId = new Guid("A45C254E-DF1C-4EFD-8020-67D146A850E0"),
            Pid = 32
        };

        private static string[]? _cachedDisplayIds;
        private static long _lastScanTick;

        public static bool IsVendorGpuSleeping(string vendorToken)
        {
            try
            {
                var ids = GetDisplayDeviceIds();
                var foundAny = false;
                var allSleeping = true;
                Span<byte> buffer = stackalloc byte[64];

                foreach (var id in ids)
                {
                    if (!id.Contains(vendorToken, StringComparison.OrdinalIgnoreCase))
                        continue;

                    foundAny = true;
                    if (CM_Locate_DevNodeW(out var devInst, id, CmLocateDevNodeNormal) != CrSuccess)
                    {
                        allSleeping = false;
                        continue;
                    }

                    uint bufferSize = 64;
                    var res = CM_Get_DevNode_PropertyW(
                        devInst,
                        in DevPkeyDevicePowerData,
                        out _,
                        ref MemoryMarshal.GetReference(buffer),
                        ref bufferSize,
                        0);

                    if (res != CrSuccess || bufferSize < 8)
                    {
                        allSleeping = false;
                        continue;
                    }

                    // CM_POWER_DATA: uint PD_Size (0..3), DEVICE_POWER_STATE PD_MostRecentPowerState (4..7)
                    var powerState = MemoryMarshal.Read<int>(buffer.Slice(4, 4));
                    if (powerState < PowerDeviceD3)
                        allSleeping = false;
                }

                return foundAny && allSleeping;
            }
            catch
            {
                return false;
            }
        }

        private static string[] GetDisplayDeviceIds()
        {
            var now = Environment.TickCount64;
            if (_cachedDisplayIds is not null && now - _lastScanTick < 15_000)
                return _cachedDisplayIds;

            _lastScanTick = now;
            const uint flags = CmGetIdListFilterClass | CmGetIdListFilterPresent;
            if (CM_Get_Device_ID_List_SizeW(out var length, DisplayClassGuid, flags) != CrSuccess || length <= 1)
            {
                _cachedDisplayIds = [];
                return _cachedDisplayIds;
            }

            var chars = new char[length];
            if (CM_Get_Device_ID_ListW(DisplayClassGuid, chars, length, flags) != CrSuccess)
            {
                _cachedDisplayIds = [];
                return _cachedDisplayIds;
            }

            var list = new List<string>(4);
            var start = 0;
            for (var i = 0; i < chars.Length; i++)
            {
                if (chars[i] == '\0')
                {
                    if (i > start)
                        list.Add(new string(chars, start, i - start));
                    start = i + 1;
                }
            }

            _cachedDisplayIds = list.ToArray();
            return _cachedDisplayIds;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct DevPropKey
        {
            public Guid FmtId;
            public uint Pid;
        }

        [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
        private static extern uint CM_Get_Device_ID_List_SizeW(out uint pulLen, string pszFilter, uint ulFlags);

        [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
        private static extern uint CM_Get_Device_ID_ListW(string pszFilter, [Out] char[] buffer, uint bufferLen, uint ulFlags);

        [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
        private static extern uint CM_Locate_DevNodeW(out uint pdnDevInst, string pDeviceID, uint ulFlags);

        [DllImport("cfgmgr32.dll")]
        private static extern uint CM_Get_DevNode_PropertyW(
            uint dnDevInst,
            in DevPropKey propertyKey,
            out uint propertyType,
            ref byte propertyBuffer,
            ref uint propertyBufferSize,
            uint ulFlags);
    }

    public void Dispose()
    {
        _stop.Cancel();
        try
        {
            _worker.Wait(TimeSpan.FromSeconds(1.5));
        }
        catch (AggregateException exception)
        {
            Trace.TraceError("Mmoni sensor shutdown: {0}", exception);
        }
        finally
        {
            if (_worker.IsCompleted)
            {
                _stop.Dispose();
            }
            else
            {
                _ = _worker.ContinueWith(_ => _stop.Dispose(), TaskScheduler.Default);
            }
        }
    }
}
