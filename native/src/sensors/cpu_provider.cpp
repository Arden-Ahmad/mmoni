#include "sensors/cpu_provider.h"
#include "resource.h"

#include <intrin.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace mmoni {

namespace {

// Intel architectural MSRs
constexpr uint32_t kMsrTemperatureTarget = 0x1A2;
constexpr uint32_t kMsrIa32PackageThermStatus = 0x1B1;
constexpr uint32_t kMsrIa32ThermStatus = 0x19C;
constexpr uint32_t kMsrRaplPowerUnit = 0x606;
constexpr uint32_t kMsrPkgEnergyStatus = 0x611;
constexpr uint32_t kMsrPp1EnergyStatus = 0x641;
constexpr uint32_t kMsrPkgPerfStatus = 0x613;

// AMD Family 17h/19h/1Ah (Zen 1-5) MSR & SMN registers
constexpr uint32_t kAmdMsrPwrUnit = 0xC0010299;
constexpr uint32_t kAmdMsrPkgEnergyStat = 0xC001029B;
constexpr uint32_t kAmdSmnThmTconCurTmp = 0x00059800;

std::string trim_spaces(std::string s) {
    // Collapse consecutive spaces and trim ends (CPUID brand strings often have padding).
    std::string out;
    out.reserve(s.size());
    bool prev_space = true;
    for (char c : s) {
        if (c == '\0') break;
        if (c == ' ' || c == '\t') {
            if (!prev_space) {
                out.push_back(' ');
                prev_space = true;
            }
        } else {
            out.push_back(c);
            prev_space = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

} // namespace

void CpuProvider::detect_cpu_identity() {
    int regs[4]{};
    __cpuid(regs, 0);
    char vendor_str[13]{};
    std::memcpy(vendor_str + 0, &regs[1], 4);
    std::memcpy(vendor_str + 4, &regs[3], 4);
    std::memcpy(vendor_str + 8, &regs[2], 4);

    if (std::strcmp(vendor_str, "GenuineIntel") == 0) {
        vendor_ = Vendor::Intel;
    } else if (std::strcmp(vendor_str, "AuthenticAMD") == 0) {
        vendor_ = Vendor::Amd;
    } else {
        vendor_ = Vendor::Unknown;
    }

    __cpuid(regs, 1);
    const uint32_t eax = static_cast<uint32_t>(regs[0]);
    const uint32_t base_family = (eax >> 8) & 0xF;
    const uint32_t ext_family = (eax >> 20) & 0xFF;
    const uint32_t base_model = (eax >> 4) & 0xF;
    const uint32_t ext_model = (eax >> 16) & 0xF;
    family_ = (base_family == 0xF) ? (base_family + ext_family) : base_family;
    model_ = (base_family == 0x6 || base_family == 0xF) ? ((ext_model << 4) | base_model) : base_model;

    __cpuid(regs, static_cast<int>(0x80000000));
    const uint32_t max_ext = static_cast<uint32_t>(regs[0]);
    if (max_ext >= 0x80000004) {
        char brand[49]{};
        for (int i = 0; i < 3; ++i) {
            __cpuid(regs, static_cast<int>(0x80000002 + i));
            std::memcpy(brand + i * 16, regs, 16);
        }
        cpu_name_ = trim_spaces(brand);
    }
    if (cpu_name_.empty()) {
        cpu_name_ = vendor_str;
    }
}

void CpuProvider::init() {
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    qpc_freq_ = freq.QuadPart > 0 ? static_cast<uint64_t>(freq.QuadPart) : 1;

    detect_cpu_identity();
    pdh_.init();

    pawnio_installed_ = PawnIoClient::is_installed();
    if (!pawnio_installed_) return;

    if (vendor_ == Vendor::Intel) {
        init_intel();
    } else if (vendor_ == Vendor::Amd && (family_ == 0x17 || family_ == 0x19 || family_ == 0x1A)) {
        init_amd();
    }
}

void CpuProvider::shutdown() {
    pawnio_.close();
    pdh_.close();
    reset_deltas();
}

void CpuProvider::reset_deltas() noexcept {
    has_pkg_energy_ = false;
    has_pp1_energy_ = false;
    has_amd_energy_ = false;
}

void CpuProvider::init_intel() {
    if (!pawnio_.load_embedded_module(IDR_PAWNIO_INTEL_MSR)) {
        return;
    }

    if (auto target = pawnio_.read_msr(kMsrTemperatureTarget)) {
        const uint32_t tj = static_cast<uint32_t>((*target >> 16) & 0xFF);
        if (tj >= 60 && tj <= 125) {
            intel_tjmax_ = static_cast<float>(tj);
        }
    }

    if (auto unit = pawnio_.read_msr(kMsrRaplPowerUnit)) {
        const uint32_t esu = static_cast<uint32_t>((*unit >> 8) & 0x1F);
        intel_energy_unit_ = std::pow(0.5, static_cast<double>(esu));
    }

    if (intel_energy_unit_ > 0.0) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (auto pkg = pawnio_.read_msr(kMsrPkgEnergyStatus)) {
            last_pkg_energy_ = static_cast<uint32_t>(*pkg & 0xFFFFFFFFULL);
            last_pkg_qpc_ = static_cast<uint64_t>(now.QuadPart);
            has_pkg_energy_ = true;
        }
        if (auto pp1 = pawnio_.read_msr(kMsrPp1EnergyStatus)) {
            last_pp1_energy_ = static_cast<uint32_t>(*pp1 & 0xFFFFFFFFULL);
            last_pp1_qpc_ = static_cast<uint64_t>(now.QuadPart);
            has_pp1_energy_ = true;
        }
    }
}

void CpuProvider::init_amd() {
    if (!pawnio_.load_embedded_module(IDR_PAWNIO_AMD_FAM17)) {
        return;
    }

    // Known Zen 1 / Zen+ X-SKU Tctl offsets (modern Zen 2/3/4/5 have 0 offset).
    amd_tctl_offset_ = 0.0f;
    if (family_ == 0x17 && model_ <= 0x08) {
        if (cpu_name_.find("1600X") != std::string::npos ||
            cpu_name_.find("1700X") != std::string::npos ||
            cpu_name_.find("1800X") != std::string::npos) {
            amd_tctl_offset_ = -20.0f;
        } else if (cpu_name_.find("2700X") != std::string::npos) {
            amd_tctl_offset_ = -10.0f;
        } else if (cpu_name_.find("1900X") != std::string::npos ||
                   cpu_name_.find("1920X") != std::string::npos ||
                   cpu_name_.find("1950X") != std::string::npos ||
                   cpu_name_.find("2920X") != std::string::npos ||
                   cpu_name_.find("2950X") != std::string::npos ||
                   cpu_name_.find("2970WX") != std::string::npos ||
                   cpu_name_.find("2990WX") != std::string::npos) {
            amd_tctl_offset_ = -27.0f;
        }
    }

    if (auto unit = pawnio_.read_msr(kAmdMsrPwrUnit)) {
        const uint32_t esu = static_cast<uint32_t>((*unit >> 8) & 0x1F);
        amd_energy_unit_ = std::pow(0.5, static_cast<double>(esu));
    }

    if (amd_energy_unit_ > 0.0) {
        if (auto pkg = pawnio_.read_msr(kAmdMsrPkgEnergyStat)) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            last_amd_energy_ = static_cast<uint32_t>(*pkg & 0xFFFFFFFFULL);
            last_amd_qpc_ = static_cast<uint64_t>(now.QuadPart);
            has_amd_energy_ = true;
        }
    }
}

void CpuProvider::read_intel_msr(ProcessorReading& out) {
    if (!pawnio_.is_ready()) return;

    // 1) Package temperature & thermal/power throttle flags (single IOCTL!)
    if (auto pkg_therm = pawnio_.read_msr(kMsrIa32PackageThermStatus)) {
        const uint64_t v = *pkg_therm;
        const bool reading_valid = ((v >> 31) & 1ULL) != 0 || ((v >> 16) & 0x7FULL) != 0;
        if (reading_valid) {
            const uint32_t readout = static_cast<uint32_t>((v >> 16) & 0x7FULL);
            const float temp = intel_tjmax_ - static_cast<float>(readout);
            if (temp >= 0.0f && temp <= 130.0f) {
                out.temperature = temp;
            }
        }
        // Bit 0: Thermal Status, Bit 2: PROCHOT / ForcePR Status
        if ((v & 0x1ULL) != 0 || ((v >> 2) & 0x1ULL) != 0) {
            out.throttle |= ThrottleThermal;
        }
        // Bit 10: Power Limitation Status
        if (((v >> 10) & 0x1ULL) != 0) {
            out.throttle |= ThrottlePower;
        }
    } else if (auto core_therm = pawnio_.read_msr(kMsrIa32ThermStatus)) {
        const uint64_t v = *core_therm;
        if (((v >> 31) & 1ULL) != 0) {
            const uint32_t readout = static_cast<uint32_t>((v >> 16) & 0x7FULL);
            const float temp = intel_tjmax_ - static_cast<float>(readout);
            if (temp >= 0.0f && temp <= 130.0f) {
                out.temperature = temp;
            }
        }
        if ((v & 0x1ULL) != 0 || ((v >> 2) & 0x1ULL) != 0) {
            out.throttle |= ThrottleThermal;
        }
        if (((v >> 10) & 0x1ULL) != 0) {
            out.throttle |= ThrottlePower;
        }
    }

    // 2) Package RAPL Energy -> Watts (single IOCTL!)
    if (intel_energy_unit_ > 0.0) {
        if (auto pkg = pawnio_.read_msr(kMsrPkgEnergyStatus)) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            const uint32_t cur = static_cast<uint32_t>(*pkg & 0xFFFFFFFFULL);
            const uint64_t now_qpc = static_cast<uint64_t>(now.QuadPart);

            if (has_pkg_energy_ && now_qpc > last_pkg_qpc_) {
                const double dt = static_cast<double>(now_qpc - last_pkg_qpc_) / static_cast<double>(qpc_freq_);
                const uint32_t delta = cur - last_pkg_energy_; // Unsigned 32-bit wrap-safe
                if (dt > 0.05 && dt < 30.0) {
                    const double watts = (static_cast<double>(delta) * intel_energy_unit_) / dt;
                    if (std::isfinite(watts) && watts >= 0.0 && watts < 1000.0) {
                        out.power = static_cast<float>(watts);
                    }
                }
            }

            last_pkg_energy_ = cur;
            last_pkg_qpc_ = now_qpc;
            has_pkg_energy_ = true;
        }
    }
}

void CpuProvider::read_amd_msr_smn(ProcessorReading& out) {
    if (!pawnio_.is_ready()) return;

    // 1) Tctl/Tdie via SMN THM_TCON_CUR_TMP (0x00059800)
    if (auto raw = pawnio_.read_smn(kAmdSmnThmTconCurTmp)) {
        const uint32_t v = *raw;
        float temp = static_cast<float>((v >> 21) & 0x7FFu) * 0.125f;
        if ((v & (1u << 19)) != 0) {
            temp -= 49.0f;
        }
        temp += amd_tctl_offset_;
        if (temp >= -20.0f && temp <= 130.0f) {
            out.temperature = temp;
        }
    }

    // 2) Package Power via AMD RAPL MSR 0xC001029B
    if (amd_energy_unit_ > 0.0) {
        if (auto pkg = pawnio_.read_msr(kAmdMsrPkgEnergyStat)) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            const uint32_t cur = static_cast<uint32_t>(*pkg & 0xFFFFFFFFULL);
            const uint64_t now_qpc = static_cast<uint64_t>(now.QuadPart);

            if (has_amd_energy_ && now_qpc > last_amd_qpc_) {
                const double dt = static_cast<double>(now_qpc - last_amd_qpc_) / static_cast<double>(qpc_freq_);
                const uint32_t delta = cur - last_amd_energy_;
                if (dt > 0.05 && dt < 30.0) {
                    const double watts = (static_cast<double>(delta) * amd_energy_unit_) / dt;
                    if (std::isfinite(watts) && watts >= 0.0 && watts < 1000.0) {
                        out.power = static_cast<float>(watts);
                    }
                }
            }

            last_amd_energy_ = cur;
            last_amd_qpc_ = now_qpc;
            has_amd_energy_ = true;
        }
    }
}

std::optional<float> CpuProvider::read_intel_igpu_rapl_watts() {
    if (vendor_ != Vendor::Intel || !pawnio_.is_ready() || intel_energy_unit_ <= 0.0) {
        return std::nullopt;
    }

    auto pp1 = pawnio_.read_msr(kMsrPp1EnergyStatus);
    if (!pp1) return std::nullopt;

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const uint32_t cur = static_cast<uint32_t>(*pp1 & 0xFFFFFFFFULL);
    const uint64_t now_qpc = static_cast<uint64_t>(now.QuadPart);

    std::optional<float> result;
    if (has_pp1_energy_ && now_qpc > last_pp1_qpc_) {
        const double dt = static_cast<double>(now_qpc - last_pp1_qpc_) / static_cast<double>(qpc_freq_);
        const uint32_t delta = cur - last_pp1_energy_;
        if (dt > 0.05 && dt < 30.0) {
            const double watts = (static_cast<double>(delta) * intel_energy_unit_) / dt;
            if (std::isfinite(watts) && watts >= 0.0 && watts < 500.0) {
                result = static_cast<float>(watts);
            }
        }
    }

    last_pp1_energy_ = cur;
    last_pp1_qpc_ = now_qpc;
    has_pp1_energy_ = true;
    return result;
}

ProcessorReading CpuProvider::read() {
    ProcessorReading out{};

    const auto pdh_sample = pdh_.sample();
    out.load = pdh_sample.load_pct;
    out.clock = pdh_sample.clock_mhz;

    if (vendor_ == Vendor::Intel) {
        read_intel_msr(out);
    } else if (vendor_ == Vendor::Amd) {
        read_amd_msr_smn(out);
    }

    return out;
}

} // namespace mmoni
