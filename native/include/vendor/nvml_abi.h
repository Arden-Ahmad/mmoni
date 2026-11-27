#pragma once

#include <cstdint>

namespace mmoni::vendor {

// ============================================================================
// Minimal NVML C ABI (dynamic LoadLibraryW("nvml.dll"))
// ============================================================================

using nvmlReturn_t = int;
using nvmlDevice_t = struct nvmlDevice_st*;

constexpr nvmlReturn_t NVML_SUCCESS = 0;
constexpr unsigned int NVML_TEMPERATURE_GPU = 0;
constexpr unsigned int NVML_CLOCK_GRAPHICS = 0;

constexpr uint64_t NVML_CLOCKS_THROTTLE_REASON_SW_POWER_CAP = 0x0000000000000004ULL;
constexpr uint64_t NVML_CLOCKS_THROTTLE_REASON_HW_SLOWDOWN = 0x0000000000000008ULL;
constexpr uint64_t NVML_CLOCKS_THROTTLE_REASON_SW_THERMAL_SLOWDOWN = 0x0000000000000020ULL;
constexpr uint64_t NVML_CLOCKS_THROTTLE_REASON_HW_THERMAL_SLOWDOWN = 0x0000000000000040ULL;
constexpr uint64_t NVML_CLOCKS_THROTTLE_REASON_HW_POWER_BRAKE = 0x0000000000000080ULL;

struct nvmlUtilization_t {
    unsigned int gpu;
    unsigned int memory;
};

struct nvmlPciInfo_t {
    char busIdLegacy[16];
    unsigned int domain;
    unsigned int bus;
    unsigned int device;
    unsigned int pciDeviceId;
    unsigned int pciSubSystemId;
    char busId[32];
};

using pfn_nvmlInit_v2 = nvmlReturn_t (*)();
using pfn_nvmlShutdown = nvmlReturn_t (*)();
using pfn_nvmlDeviceGetCount_v2 = nvmlReturn_t (*)(unsigned int* deviceCount);
using pfn_nvmlDeviceGetHandleByIndex_v2 = nvmlReturn_t (*)(unsigned int index, nvmlDevice_t* device);
using pfn_nvmlDeviceGetName = nvmlReturn_t (*)(nvmlDevice_t device, char* name, unsigned int length);
using pfn_nvmlDeviceGetPciInfo_v3 = nvmlReturn_t (*)(nvmlDevice_t device, nvmlPciInfo_t* pci);
using pfn_nvmlDeviceGetTemperature = nvmlReturn_t (*)(nvmlDevice_t device, unsigned int sensorType, unsigned int* temp);
using pfn_nvmlDeviceGetPowerUsage = nvmlReturn_t (*)(nvmlDevice_t device, unsigned int* powerMilliwatts);
using pfn_nvmlDeviceGetUtilizationRates = nvmlReturn_t (*)(nvmlDevice_t device, nvmlUtilization_t* utilization);
using pfn_nvmlDeviceGetClockInfo = nvmlReturn_t (*)(nvmlDevice_t device, unsigned int clockType, unsigned int* clockMhz);
using pfn_nvmlDeviceGetCurrentClocksThrottleReasons = nvmlReturn_t (*)(nvmlDevice_t device, uint64_t* reasons);

// ============================================================================
// Minimal NVAPI C ABI fallback (dynamic LoadLibraryW("nvapi64.dll"))
// ============================================================================

using NvAPI_Status = int;
using NvPhysicalGpuHandle = void*;
constexpr NvAPI_Status NVAPI_OK = 0;

struct NV_GPU_THERMAL_SETTINGS {
    uint32_t version;
    uint32_t count;
    struct {
        int32_t controller;
        int32_t defaultMinTemp;
        int32_t defaultMaxTemp;
        int32_t currentTemp;
        int32_t target;
    } sensor[3];
};

struct NV_GPU_DYNAMIC_PSTATES_INFO_EX {
    uint32_t version;
    uint32_t flags;
    struct {
        uint32_t bIsPresent;
        uint32_t percentage;
    } utilization[8];
};

struct NV_GPU_CLOCK_FREQUENCIES_V2 {
    uint32_t version;
    uint32_t ClockType; // 0 = current
    struct {
        uint32_t bIsPresent;
        uint32_t frequency; // kHz
    } domain[32];
};

using pfn_nvapi_QueryInterface = void* (*)(uint32_t id);
using pfn_NvAPI_Initialize = NvAPI_Status (*)();
using pfn_NvAPI_Unload = NvAPI_Status (*)();
using pfn_NvAPI_EnumPhysicalGPUs = NvAPI_Status (*)(NvPhysicalGpuHandle handles[64], uint32_t* count);
using pfn_NvAPI_GPU_GetFullName = NvAPI_Status (*)(NvPhysicalGpuHandle handle, char name[64]);
using pfn_NvAPI_GPU_GetThermalSettings = NvAPI_Status (*)(NvPhysicalGpuHandle handle, uint32_t sensorIndex, NV_GPU_THERMAL_SETTINGS* settings);
using pfn_NvAPI_GPU_GetDynamicPstatesInfoEx = NvAPI_Status (*)(NvPhysicalGpuHandle handle, NV_GPU_DYNAMIC_PSTATES_INFO_EX* info);
using pfn_NvAPI_GPU_GetAllClockFrequencies = NvAPI_Status (*)(NvPhysicalGpuHandle handle, NV_GPU_CLOCK_FREQUENCIES_V2* clocks);

} // namespace mmoni::vendor
