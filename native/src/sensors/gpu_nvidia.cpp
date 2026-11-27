#include "sensors/gpu_nvidia.h"

#include <shlobj.h>
#include <cstring>
#include <string>

namespace mmoni {

namespace {
constexpr uint64_t kRetryBackoffMs = 15000ULL;
} // namespace

NvidiaGpuProvider::~NvidiaGpuProvider() {
    shutdown();
}

void NvidiaGpuProvider::reset_backoff() noexcept {
    nvml_last_attempt_ms_ = 0;
    nvapi_last_attempt_ms_ = 0;
}

void NvidiaGpuProvider::shutdown() {
    if (nvml_ready_ && nvmlShutdown_) {
        nvmlShutdown_();
    }
    nvml_ready_ = false;
    nvml_last_attempt_ms_ = 0;
    if (nvml_dll_) {
        FreeLibrary(nvml_dll_);
        nvml_dll_ = nullptr;
    }

    if (nvapi_ready_ && nvapiUnload_) {
        nvapiUnload_();
    }
    nvapi_ready_ = false;
    nvapi_last_attempt_ms_ = 0;
    if (nvapi_dll_) {
        FreeLibrary(nvapi_dll_);
        nvapi_dll_ = nullptr;
    }
}

bool NvidiaGpuProvider::ensure_nvml() {
    if (nvml_ready_) return true;
    const uint64_t now = GetTickCount64();
    if (nvml_last_attempt_ms_ != 0 && now - nvml_last_attempt_ms_ < kRetryBackoffMs) {
        return false;
    }
    nvml_last_attempt_ms_ = now;

    // Hardened load from System32 first, then trusted Program Files\NVIDIA Corporation\NVSMI.
    nvml_dll_ = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!nvml_dll_) {
        PWSTR pf = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &pf)) && pf) {
            std::wstring candidate = std::wstring(pf) + L"\\NVIDIA Corporation\\NVSMI\\nvml.dll";
            CoTaskMemFree(pf);
            nvml_dll_ = LoadLibraryExW(candidate.c_str(), nullptr,
                                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        }
    }
    if (!nvml_dll_) return false;

    nvmlInit_ = reinterpret_cast<vendor::pfn_nvmlInit_v2>(GetProcAddress(nvml_dll_, "nvmlInit_v2"));
    if (!nvmlInit_) nvmlInit_ = reinterpret_cast<vendor::pfn_nvmlInit_v2>(GetProcAddress(nvml_dll_, "nvmlInit"));
    nvmlShutdown_ = reinterpret_cast<vendor::pfn_nvmlShutdown>(GetProcAddress(nvml_dll_, "nvmlShutdown"));
    nvmlGetCount_ = reinterpret_cast<vendor::pfn_nvmlDeviceGetCount_v2>(GetProcAddress(nvml_dll_, "nvmlDeviceGetCount_v2"));
    nvmlGetHandle_ = reinterpret_cast<vendor::pfn_nvmlDeviceGetHandleByIndex_v2>(GetProcAddress(nvml_dll_, "nvmlDeviceGetHandleByIndex_v2"));
    nvmlGetName_ = reinterpret_cast<vendor::pfn_nvmlDeviceGetName>(GetProcAddress(nvml_dll_, "nvmlDeviceGetName"));
    nvmlGetPciInfo_ = reinterpret_cast<vendor::pfn_nvmlDeviceGetPciInfo_v3>(GetProcAddress(nvml_dll_, "nvmlDeviceGetPciInfo_v3"));
    nvmlGetTemp_ = reinterpret_cast<vendor::pfn_nvmlDeviceGetTemperature>(GetProcAddress(nvml_dll_, "nvmlDeviceGetTemperature"));
    nvmlGetPower_ = reinterpret_cast<vendor::pfn_nvmlDeviceGetPowerUsage>(GetProcAddress(nvml_dll_, "nvmlDeviceGetPowerUsage"));
    nvmlGetUtil_ = reinterpret_cast<vendor::pfn_nvmlDeviceGetUtilizationRates>(GetProcAddress(nvml_dll_, "nvmlDeviceGetUtilizationRates"));
    nvmlGetClock_ = reinterpret_cast<vendor::pfn_nvmlDeviceGetClockInfo>(GetProcAddress(nvml_dll_, "nvmlDeviceGetClockInfo"));
    nvmlGetThrottle_ = reinterpret_cast<vendor::pfn_nvmlDeviceGetCurrentClocksThrottleReasons>(
        GetProcAddress(nvml_dll_, "nvmlDeviceGetCurrentClocksEventReasons"));
    if (!nvmlGetThrottle_) {
        nvmlGetThrottle_ = reinterpret_cast<vendor::pfn_nvmlDeviceGetCurrentClocksThrottleReasons>(
            GetProcAddress(nvml_dll_, "nvmlDeviceGetCurrentClocksThrottleReasons"));
    }

    if (!nvmlInit_ || !nvmlGetCount_ || !nvmlGetHandle_ || nvmlInit_() != vendor::NVML_SUCCESS) {
        FreeLibrary(nvml_dll_);
        nvml_dll_ = nullptr;
        return false;
    }

    nvml_ready_ = true;
    return true;
}

bool NvidiaGpuProvider::ensure_nvapi() {
    if (nvapi_ready_) return true;
    const uint64_t now = GetTickCount64();
    if (nvapi_last_attempt_ms_ != 0 && now - nvapi_last_attempt_ms_ < kRetryBackoffMs) {
        return false;
    }
    nvapi_last_attempt_ms_ = now;

    nvapi_dll_ = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!nvapi_dll_) return false;

    auto query = reinterpret_cast<vendor::pfn_nvapi_QueryInterface>(GetProcAddress(nvapi_dll_, "nvapi_QueryInterface"));
    if (!query) {
        FreeLibrary(nvapi_dll_);
        nvapi_dll_ = nullptr;
        return false;
    }

    auto init = reinterpret_cast<vendor::pfn_NvAPI_Initialize>(query(0x0150E828));
    nvapiUnload_ = reinterpret_cast<vendor::pfn_NvAPI_Unload>(query(0xD22BDD7E));
    nvapiEnum_ = reinterpret_cast<vendor::pfn_NvAPI_EnumPhysicalGPUs>(query(0xE5AC921F));
    nvapiName_ = reinterpret_cast<vendor::pfn_NvAPI_GPU_GetFullName>(query(0xCEEE8E9F));
    nvapiTherm_ = reinterpret_cast<vendor::pfn_NvAPI_GPU_GetThermalSettings>(query(0xE3640A56));
    nvapiUtil_ = reinterpret_cast<vendor::pfn_NvAPI_GPU_GetDynamicPstatesInfoEx>(query(0x60DED2ED));
    nvapiClocks_ = reinterpret_cast<vendor::pfn_NvAPI_GPU_GetAllClockFrequencies>(query(0xDCB616C3));
    nvapiBusId_ = reinterpret_cast<pfn_NvAPI_GPU_GetBusId>(query(0x1BE0B8E5));
    nvapiBusSlotId_ = reinterpret_cast<pfn_NvAPI_GPU_GetBusSlotId>(query(0x2A0A350F));

    if (!init || !nvapiEnum_ || init() != vendor::NVAPI_OK) {
        FreeLibrary(nvapi_dll_);
        nvapi_dll_ = nullptr;
        return false;
    }

    nvapi_ready_ = true;
    return true;
}

std::optional<ProcessorReading> NvidiaGpuProvider::read_nvml(const GpuDevice& gpu) {
    if (!ensure_nvml()) return std::nullopt;

    unsigned int count = 0;
    if (nvmlGetCount_(&count) != vendor::NVML_SUCCESS || count == 0) {
        return std::nullopt;
    }

    vendor::nvmlDevice_t chosen = nullptr;
    vendor::nvmlDevice_t model_match = nullptr;
    int model_matches = 0;

    for (unsigned int i = 0; i < count; ++i) {
        vendor::nvmlDevice_t dev = nullptr;
        if (nvmlGetHandle_(i, &dev) != vendor::NVML_SUCCESS || !dev) continue;

        if (nvmlGetPciInfo_) {
            vendor::nvmlPciInfo_t pci{};
            if (nvmlGetPciInfo_(dev, &pci) == vendor::NVML_SUCCESS) {
                // Exact 1:1 PCI BDF match (handles dual identical cards cleanly).
                if (gpu.pci_bus >= 0 && static_cast<int>(pci.bus) == gpu.pci_bus &&
                    (gpu.pci_dev < 0 || static_cast<int>(pci.device) == gpu.pci_dev)) {
                    chosen = dev;
                    break;
                }
                const uint32_t dev_id = (pci.pciDeviceId >> 16) & 0xFFFFu;
                if (gpu.device_id != 0 && dev_id == (gpu.device_id & 0xFFFFu)) {
                    model_match = dev;
                    ++model_matches;
                }
            }
        }
    }

    // Only accept model-only match when unambiguous (never guess GPU 0 on multi-card systems).
    if (!chosen) {
        if (model_matches == 1) {
            chosen = model_match;
        } else if (count == 1 && gpu.pci_bus < 0) {
            nvmlGetHandle_(0, &chosen);
        }
    }
    if (!chosen) return std::nullopt;

    ProcessorReading r{};
    unsigned int temp = 0;
    if (nvmlGetTemp_ && nvmlGetTemp_(chosen, vendor::NVML_TEMPERATURE_GPU, &temp) == vendor::NVML_SUCCESS) {
        r.temperature = static_cast<float>(temp);
    }

    unsigned int power_mw = 0;
    if (nvmlGetPower_ && nvmlGetPower_(chosen, &power_mw) == vendor::NVML_SUCCESS) {
        r.power = static_cast<float>(power_mw) / 1000.0f;
    }

    vendor::nvmlUtilization_t util{};
    if (nvmlGetUtil_ && nvmlGetUtil_(chosen, &util) == vendor::NVML_SUCCESS) {
        r.load = static_cast<float>(util.gpu);
    }

    unsigned int clock_mhz = 0;
    if (nvmlGetClock_ && nvmlGetClock_(chosen, vendor::NVML_CLOCK_GRAPHICS, &clock_mhz) == vendor::NVML_SUCCESS) {
        r.clock = static_cast<float>(clock_mhz);
    }

    uint64_t reasons = 0;
    if (nvmlGetThrottle_ && nvmlGetThrottle_(chosen, &reasons) == vendor::NVML_SUCCESS) {
        if ((reasons & (vendor::NVML_CLOCKS_THROTTLE_REASON_SW_THERMAL_SLOWDOWN |
                        vendor::NVML_CLOCKS_THROTTLE_REASON_HW_THERMAL_SLOWDOWN)) != 0) {
            r.throttle |= ThrottleThermal;
        }
        if ((reasons & (vendor::NVML_CLOCKS_THROTTLE_REASON_SW_POWER_CAP |
                        vendor::NVML_CLOCKS_THROTTLE_REASON_HW_POWER_BRAKE)) != 0) {
            r.throttle |= ThrottlePower;
        }
    }

    if (!r.has_any_metric()) return std::nullopt;
    return r;
}

std::optional<ProcessorReading> NvidiaGpuProvider::read_nvapi(const GpuDevice& gpu) {
    if (!ensure_nvapi()) return std::nullopt;

    vendor::NvPhysicalGpuHandle handles[64]{};
    uint32_t count = 0;
    if (nvapiEnum_(handles, &count) != vendor::NVAPI_OK || count == 0) {
        return std::nullopt;
    }

    vendor::NvPhysicalGpuHandle chosen = nullptr;
    vendor::NvPhysicalGpuHandle name_match = nullptr;
    int name_matches = 0;

    for (uint32_t i = 0; i < count; ++i) {
        if (gpu.pci_bus >= 0 && nvapiBusId_) {
            uint32_t bus = 0;
            uint32_t slot = 0;
            if (nvapiBusId_(handles[i], &bus) == vendor::NVAPI_OK && static_cast<int>(bus) == gpu.pci_bus) {
                if (gpu.pci_dev < 0 || !nvapiBusSlotId_ ||
                    (nvapiBusSlotId_(handles[i], &slot) == vendor::NVAPI_OK && static_cast<int>(slot) == gpu.pci_dev)) {
                    chosen = handles[i];
                    break;
                }
            }
        }
        if (!gpu.name.empty() && nvapiName_) {
            char name[64]{};
            if (nvapiName_(handles[i], name) == vendor::NVAPI_OK && gpu.name.find(name) != std::string::npos) {
                name_match = handles[i];
                ++name_matches;
            }
        }
    }

    if (!chosen) {
        if (name_matches == 1) chosen = name_match;
        else if (count == 1 && gpu.pci_bus < 0) chosen = handles[0];
    }
    if (!chosen) return std::nullopt;

    ProcessorReading r{};
    if (nvapiTherm_) {
        vendor::NV_GPU_THERMAL_SETTINGS therm{};
        therm.version = static_cast<uint32_t>(sizeof(therm) | (2u << 16));
        if (nvapiTherm_(chosen, 15, &therm) == vendor::NVAPI_OK && therm.count > 0) {
            r.temperature = static_cast<float>(therm.sensor[0].currentTemp);
        }
    }
    if (nvapiUtil_) {
        vendor::NV_GPU_DYNAMIC_PSTATES_INFO_EX pstates{};
        pstates.version = static_cast<uint32_t>(sizeof(pstates) | (1u << 16));
        if (nvapiUtil_(chosen, &pstates) == vendor::NVAPI_OK && pstates.utilization[0].bIsPresent) {
            r.load = static_cast<float>(pstates.utilization[0].percentage);
        }
    }
    if (nvapiClocks_) {
        vendor::NV_GPU_CLOCK_FREQUENCIES_V2 clocks{};
        clocks.version = static_cast<uint32_t>(sizeof(clocks) | (2u << 16));
        clocks.ClockType = 0;
        if (nvapiClocks_(chosen, &clocks) == vendor::NVAPI_OK && clocks.domain[0].bIsPresent) {
            r.clock = static_cast<float>(clocks.domain[0].frequency) / 1000.0f;
        }
    }

    if (!r.has_any_metric()) return std::nullopt;
    return r;
}

std::optional<ProcessorReading> NvidiaGpuProvider::read(const GpuDevice& gpu) {
    auto reading = read_nvml(gpu);
    const bool needs_fallback = !reading.has_value() ||
        (!reading->temperature.has_value() || !reading->load.has_value() || !reading->clock.has_value());

    if (needs_fallback) {
        if (auto fb = read_nvapi(gpu)) {
            if (!reading.has_value()) {
                return fb;
            }
            if (!reading->temperature.has_value()) reading->temperature = fb->temperature;
            if (!reading->load.has_value()) reading->load = fb->load;
            if (!reading->clock.has_value()) reading->clock = fb->clock;
        }
    }
    return reading;
}

} // namespace mmoni
