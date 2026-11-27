#pragma once

#include "sensors/sensor_types.h"

#include <windows.h>
#include <optional>
#include <string>

namespace mmoni {

class AmdGpuProvider {
public:
    AmdGpuProvider() = default;
    ~AmdGpuProvider();

    AmdGpuProvider(const AmdGpuProvider&) = delete;
    AmdGpuProvider& operator=(const AmdGpuProvider&) = delete;

    std::optional<ProcessorReading> read(const GpuDevice& gpu);
    void reset_backoff() noexcept { last_attempt_ms_ = 0; }
    void shutdown();

private:
    bool ensure_init();

    uint64_t last_attempt_ms_ = 0;
    bool ready_ = false;
    HMODULE dll_ = nullptr;
    void* terminate_fn_ = nullptr;
    void* system_ = nullptr;
    void* perf_services_ = nullptr;
};

} // namespace mmoni
