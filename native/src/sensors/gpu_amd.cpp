#include "sensors/gpu_amd.h"

#include "vendor/adlx/ADLX.h"
#include "vendor/adlx/ISystem.h"
#include "vendor/adlx/IPerformanceMonitoring.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace mmoni {

namespace {
constexpr uint64_t kRetryBackoffMs = 15000ULL;
} // namespace

AmdGpuProvider::~AmdGpuProvider() {
    shutdown();
}

void AmdGpuProvider::shutdown() {
    if (perf_services_) {
        static_cast<adlx::IADLXPerformanceMonitoringServices*>(perf_services_)->Release();
        perf_services_ = nullptr;
    }
    system_ = nullptr;
    if (ready_ && terminate_fn_) {
        auto term = reinterpret_cast<ADLXTerminate_Fn>(terminate_fn_);
        term();
    }
    ready_ = false;
    last_attempt_ms_ = 0;
    terminate_fn_ = nullptr;
    if (dll_) {
        FreeLibrary(dll_);
        dll_ = nullptr;
    }
}

bool AmdGpuProvider::ensure_init() {
    if (ready_) return true;
    const uint64_t now = GetTickCount64();
    if (last_attempt_ms_ != 0 && now - last_attempt_ms_ < kRetryBackoffMs) {
        return false;
    }
    last_attempt_ms_ = now;

    dll_ = LoadLibraryExW(L"amdadlx64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll_) return false;

    auto init_fn = reinterpret_cast<ADLXInitialize_Fn>(GetProcAddress(dll_, ADLX_INIT_FUNCTION_NAME));
    auto term_fn = reinterpret_cast<ADLXTerminate_Fn>(GetProcAddress(dll_, ADLX_TERMINATE_FUNCTION_NAME));
    if (!init_fn || !term_fn) {
        FreeLibrary(dll_);
        dll_ = nullptr;
        return false;
    }

    adlx::IADLXSystem* sys = nullptr;
    if (ADLX_FAILED(init_fn(ADLX_FULL_VERSION, &sys)) || !sys) {
        FreeLibrary(dll_);
        dll_ = nullptr;
        return false;
    }

    adlx::IADLXPerformanceMonitoringServices* pms = nullptr;
    if (ADLX_FAILED(sys->GetPerformanceMonitoringServices(&pms)) || !pms) {
        term_fn();
        FreeLibrary(dll_);
        dll_ = nullptr;
        return false;
    }

    terminate_fn_ = reinterpret_cast<void*>(term_fn);
    system_ = sys;
    perf_services_ = pms;
    ready_ = true;
    return true;
}

std::optional<ProcessorReading> AmdGpuProvider::read(const GpuDevice& target_gpu) {
    if (!ensure_init()) return std::nullopt;

    auto* sys = static_cast<adlx::IADLXSystem*>(system_);
    auto* pms = static_cast<adlx::IADLXPerformanceMonitoringServices*>(perf_services_);

    adlx::IADLXGPUList* gpus = nullptr;
    if (ADLX_FAILED(sys->GetGPUs(&gpus)) || !gpus) {
        return std::nullopt;
    }

    adlx::IADLXGPU* chosen = nullptr;
    adlx::IADLXGPU* unique_match = nullptr;
    int unique_matches = 0;

    char expected_dev_hex[16]{};
    std::snprintf(expected_dev_hex, sizeof(expected_dev_hex), "%04X", target_gpu.device_id & 0xFFFFu);

    const adlx_uint count = gpus->Size();
    for (adlx_uint i = 0; i < count; ++i) {
        adlx::IADLXGPU* gpu = nullptr;
        if (ADLX_FAILED(gpus->At(i, &gpu)) || !gpu) continue;

        // ADLX UniqueId encodes (bus << 8) | (dev << 3) | func on AMD Windows drivers.
        adlx_int uid = -1;
        if (target_gpu.pci_bus >= 0 && ADLX_SUCCEEDED(gpu->UniqueId(&uid)) && uid >= 0) {
            const int adlx_bus = (uid >> 8) & 0xFF;
            const int adlx_dev = (uid >> 3) & 0x1F;
            if (adlx_bus == target_gpu.pci_bus && (target_gpu.pci_dev < 0 || adlx_dev == target_gpu.pci_dev)) {
                chosen = gpu;
                chosen->Acquire();
                gpu->Release();
                break;
            }
        }

        const char* dev_id_str = nullptr;
        bool dev_matched = (target_gpu.device_id != 0 && ADLX_SUCCEEDED(gpu->DeviceId(&dev_id_str)) && dev_id_str &&
                            _stricmp(dev_id_str, expected_dev_hex) == 0);
        const char* name = nullptr;
        bool name_matched = (!target_gpu.name.empty() && ADLX_SUCCEEDED(gpu->Name(&name)) && name &&
                             (target_gpu.name == name || target_gpu.name.find(name) != std::string::npos));
        if (dev_matched || name_matched) {
            if (unique_match) unique_match->Release();
            unique_match = gpu;
            unique_match->Acquire();
            ++unique_matches;
        }

        gpu->Release();
    }

    if (!chosen) {
        if (unique_matches == 1 && unique_match) {
            chosen = unique_match;
            unique_match = nullptr;
        } else if (count == 1 && target_gpu.pci_bus < 0) {
            adlx::IADLXGPU* single = nullptr;
            if (ADLX_SUCCEEDED(gpus->At(0, &single)) && single) {
                chosen = single;
            }
        }
    }
    if (unique_match) unique_match->Release();
    gpus->Release();

    if (!chosen) return std::nullopt;

    adlx::IADLXGPUMetrics* metrics = nullptr;
    ADLX_RESULT res = pms->GetCurrentGPUMetrics(chosen, &metrics);
    chosen->Release();
    if (ADLX_FAILED(res) || !metrics) {
        return std::nullopt;
    }

    ProcessorReading r{};
    adlx_double dval = 0.0;
    adlx_int ival = 0;

    if (ADLX_SUCCEEDED(metrics->GPUTemperature(&dval)) && std::isfinite(dval)) {
        r.temperature = static_cast<float>(dval);
    } else if (ADLX_SUCCEEDED(metrics->GPUHotspotTemperature(&dval)) && std::isfinite(dval)) {
        r.temperature = static_cast<float>(dval);
    }

    if (ADLX_SUCCEEDED(metrics->GPUTotalBoardPower(&dval)) && std::isfinite(dval)) {
        r.power = static_cast<float>(dval);
    } else if (ADLX_SUCCEEDED(metrics->GPUPower(&dval)) && std::isfinite(dval)) {
        r.power = static_cast<float>(dval);
    }

    if (ADLX_SUCCEEDED(metrics->GPUUsage(&dval)) && std::isfinite(dval)) {
        r.load = static_cast<float>(dval);
    }

    if (ADLX_SUCCEEDED(metrics->GPUClockSpeed(&ival)) && ival >= 0) {
        r.clock = static_cast<float>(ival);
    }

    metrics->Release();
    if (!r.has_any_metric()) return std::nullopt;
    return r;
}

} // namespace mmoni
