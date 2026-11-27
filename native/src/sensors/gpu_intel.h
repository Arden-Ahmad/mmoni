#pragma once

#include "sensors/sensor_types.h"

#include <windows.h>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mmoni {

class CpuProvider;

class IntelGpuProvider {
public:
    IntelGpuProvider() = default;
    ~IntelGpuProvider();

    IntelGpuProvider(const IntelGpuProvider&) = delete;
    IntelGpuProvider& operator=(const IntelGpuProvider&) = delete;

    std::optional<ProcessorReading> read(const GpuDevice& gpu, CpuProvider* cpu_rapl_fallback);
    void reset_backoff() noexcept { last_attempt_ms_ = 0; }
    void reset_deltas() noexcept;
    void shutdown();

private:
    struct DeviceDeltaState {
        void* handle = nullptr;
        uint32_t pci_device_id = 0;
        uint64_t luid = 0;
        int pci_bus = -1;
        int pci_dev = -1;
        std::string name;
        bool has_prev = false;
        double prev_ts = 0.0;
        double prev_energy = 0.0;
        double prev_activity = 0.0;
    };

    bool ensure_init();

    uint64_t last_attempt_ms_ = 0;
    bool ready_ = false;
    HMODULE dll_ = nullptr;
    void* api_handle_ = nullptr;
    void* fn_close_ = nullptr;
    void* fn_telemetry_get_ = nullptr;
    std::vector<DeviceDeltaState> devices_;
};

} // namespace mmoni
