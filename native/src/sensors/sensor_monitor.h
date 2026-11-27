#pragma once

#include "sensors/sensor_types.h"

#include <windows.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace mmoni {

class SensorMonitor {
public:
    using PublishCallback = std::function<void(SensorSnapshot)>;

    SensorMonitor(std::optional<std::string> preferred_gpu_id, PublishCallback publish);
    ~SensorMonitor();

    SensorMonitor(const SensorMonitor&) = delete;
    SensorMonitor& operator=(const SensorMonitor&) = delete;

    void start();
    void stop();

    void select_gpu(std::optional<std::string> id);
    void set_paused(bool paused) noexcept;
    void request_topology_refresh() noexcept;

    // Synchronous single-shot helpers for diagnostics / benchmark CLI.
    void init_sync();
    SensorSnapshot read_once_sync();
    void shutdown_sync();

private:
    struct WorkerState;
    static DWORD WINAPI worker_thread_proc(LPVOID param);

    std::shared_ptr<WorkerState> state_;
    HANDLE worker_handle_ = nullptr;
};

} // namespace mmoni
