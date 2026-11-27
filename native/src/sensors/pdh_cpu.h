#pragma once

#include <windows.h>
#include <pdh.h>
#include <optional>

namespace mmoni {

struct PdhCpuSample {
    std::optional<float> load_pct;
    std::optional<float> clock_mhz;
};

class PdhCpuSampler {
public:
    PdhCpuSampler() = default;
    ~PdhCpuSampler();

    PdhCpuSampler(const PdhCpuSampler&) = delete;
    PdhCpuSampler& operator=(const PdhCpuSampler&) = delete;

    bool init();
    void close();
    PdhCpuSample sample();

private:
    static float query_nominal_mhz();

    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER utility_counter_ = nullptr;
    PDH_HCOUNTER perf_counter_ = nullptr;
    PDH_HCOUNTER time_counter_ = nullptr;
    float base_mhz_ = 0.0f;
    bool primed_ = false;
};

} // namespace mmoni
