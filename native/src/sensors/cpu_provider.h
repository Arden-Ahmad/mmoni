#pragma once

#include "sensors/pawnio_client.h"
#include "sensors/pdh_cpu.h"
#include "sensors/sensor_types.h"

#include <cstdint>
#include <optional>
#include <string>

namespace mmoni {

class CpuProvider {
public:
    enum class Vendor { Unknown, Intel, Amd };

    CpuProvider() = default;
    ~CpuProvider() = default;

    void init();
    void shutdown();
    void reset_deltas() noexcept;

    ProcessorReading read();
    std::optional<float> read_intel_igpu_rapl_watts();

    const std::string& name() const noexcept { return cpu_name_; }
    Vendor vendor() const noexcept { return vendor_; }
    bool pawnio_installed() const noexcept { return pawnio_installed_; }

private:
    void detect_cpu_identity();
    void init_intel();
    void init_amd();

    void read_intel_msr(ProcessorReading& out);
    void read_amd_msr_smn(ProcessorReading& out);

    std::string cpu_name_;
    Vendor vendor_ = Vendor::Unknown;
    uint32_t family_ = 0;
    uint32_t model_ = 0;

    bool pawnio_installed_ = false;
    PawnIoClient pawnio_;
    PdhCpuSampler pdh_;

    // Intel MSR calibration & energy delta state
    float intel_tjmax_ = 100.0f;
    double intel_energy_unit_ = 0.0;
    bool has_pkg_energy_ = false;
    uint32_t last_pkg_energy_ = 0;
    uint64_t last_pkg_qpc_ = 0;

    // Intel PP1 (iGPU) RAPL fallback energy delta state
    bool has_pp1_energy_ = false;
    uint32_t last_pp1_energy_ = 0;
    uint64_t last_pp1_qpc_ = 0;

    // AMD Zen calibration & energy delta state
    double amd_energy_unit_ = 0.0;
    float amd_tctl_offset_ = 0.0f;
    bool has_amd_energy_ = false;
    uint32_t last_amd_energy_ = 0;
    uint64_t last_amd_qpc_ = 0;

    uint64_t qpc_freq_ = 1;
};

} // namespace mmoni
