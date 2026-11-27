#pragma once

#include "sensors/sensor_types.h"
#include "vendor/nvml_abi.h"

#include <windows.h>
#include <optional>
#include <string>

namespace mmoni {

class NvidiaGpuProvider {
public:
    NvidiaGpuProvider() = default;
    ~NvidiaGpuProvider();

    NvidiaGpuProvider(const NvidiaGpuProvider&) = delete;
    NvidiaGpuProvider& operator=(const NvidiaGpuProvider&) = delete;

    // Lazy initialization so we never wake a sleeping Optimus dGPU during startup.
    std::optional<ProcessorReading> read(const GpuDevice& gpu);
    void reset_backoff() noexcept;
    void shutdown();

private:
    bool ensure_nvml();
    bool ensure_nvapi();
    std::optional<ProcessorReading> read_nvml(const GpuDevice& gpu);
    std::optional<ProcessorReading> read_nvapi(const GpuDevice& gpu);

    // NVML state (with retry backoff)
    uint64_t nvml_last_attempt_ms_ = 0;
    bool nvml_ready_ = false;
    HMODULE nvml_dll_ = nullptr;
    vendor::pfn_nvmlInit_v2 nvmlInit_ = nullptr;
    vendor::pfn_nvmlShutdown nvmlShutdown_ = nullptr;
    vendor::pfn_nvmlDeviceGetCount_v2 nvmlGetCount_ = nullptr;
    vendor::pfn_nvmlDeviceGetHandleByIndex_v2 nvmlGetHandle_ = nullptr;
    vendor::pfn_nvmlDeviceGetName nvmlGetName_ = nullptr;
    vendor::pfn_nvmlDeviceGetPciInfo_v3 nvmlGetPciInfo_ = nullptr;
    vendor::pfn_nvmlDeviceGetTemperature nvmlGetTemp_ = nullptr;
    vendor::pfn_nvmlDeviceGetPowerUsage nvmlGetPower_ = nullptr;
    vendor::pfn_nvmlDeviceGetUtilizationRates nvmlGetUtil_ = nullptr;
    vendor::pfn_nvmlDeviceGetClockInfo nvmlGetClock_ = nullptr;
    vendor::pfn_nvmlDeviceGetCurrentClocksThrottleReasons nvmlGetThrottle_ = nullptr;

    // NVAPI fallback state (with retry backoff)
    uint64_t nvapi_last_attempt_ms_ = 0;
    bool nvapi_ready_ = false;
    HMODULE nvapi_dll_ = nullptr;
    vendor::pfn_NvAPI_Unload nvapiUnload_ = nullptr;
    vendor::pfn_NvAPI_EnumPhysicalGPUs nvapiEnum_ = nullptr;
    vendor::pfn_NvAPI_GPU_GetFullName nvapiName_ = nullptr;
    vendor::pfn_NvAPI_GPU_GetThermalSettings nvapiTherm_ = nullptr;
    vendor::pfn_NvAPI_GPU_GetDynamicPstatesInfoEx nvapiUtil_ = nullptr;
    vendor::pfn_NvAPI_GPU_GetAllClockFrequencies nvapiClocks_ = nullptr;
    using pfn_NvAPI_GPU_GetBusId = vendor::NvAPI_Status (*)(vendor::NvPhysicalGpuHandle, uint32_t*);
    using pfn_NvAPI_GPU_GetBusSlotId = vendor::NvAPI_Status (*)(vendor::NvPhysicalGpuHandle, uint32_t*);
    pfn_NvAPI_GPU_GetBusId nvapiBusId_ = nullptr;
    pfn_NvAPI_GPU_GetBusSlotId nvapiBusSlotId_ = nullptr;
};

} // namespace mmoni
