#pragma once

#include "sensors/sensor_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mmoni {

struct DisplayPciNode {
    std::wstring instance_id;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint32_t subsys_id = 0;
    int pci_bus = -1;
    int pci_dev = -1;
    int pci_func = -1;
};

class GpuSleepGuard {
public:
    std::vector<DisplayPciNode> enumerate_display_nodes();
    bool is_adapter_sleeping(const GpuDevice& gpu);
    void invalidate_cache() noexcept { last_scan_tick_ = 0; }

private:
    const std::vector<DisplayPciNode>& get_cached_nodes();

    std::vector<DisplayPciNode> cached_nodes_;
    uint64_t last_scan_tick_ = 0;
};

} // namespace mmoni
