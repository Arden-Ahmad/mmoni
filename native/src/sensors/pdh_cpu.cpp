#include "sensors/pdh_cpu.h"

#include <pdhmsg.h>
#include <powerbase.h>
#include <winternl.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace mmoni {

namespace {

struct ProcessorPowerInfo {
    ULONG number;
    ULONG max_mhz;
    ULONG current_mhz;
    ULONG mhz_limit;
    ULONG max_idle_state;
    ULONG current_idle_state;
};

} // namespace

PdhCpuSampler::~PdhCpuSampler() {
    close();
}

float PdhCpuSampler::query_nominal_mhz() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const ULONG count = std::max<ULONG>(1, si.dwNumberOfProcessors);
    std::vector<ProcessorPowerInfo> infos(count);
    NTSTATUS status = CallNtPowerInformation(
        ProcessorInformation,
        nullptr,
        0,
        infos.data(),
        static_cast<ULONG>(infos.size() * sizeof(ProcessorPowerInfo)));
    if (status == 0 && !infos.empty() && infos[0].max_mhz > 0) {
        ULONG max_base = 0;
        for (const auto& info : infos) {
            if (info.max_mhz > max_base) max_base = info.max_mhz;
        }
        return static_cast<float>(max_base);
    }

    // Registry fallback (~MHz)
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        DWORD mhz = 0;
        DWORD size = sizeof(mhz);
        if (RegQueryValueExW(key, L"~MHz", nullptr, nullptr, reinterpret_cast<LPBYTE>(&mhz), &size) == ERROR_SUCCESS && mhz > 0) {
            RegCloseKey(key);
            return static_cast<float>(mhz);
        }
        RegCloseKey(key);
    }
    return 0.0f;
}

bool PdhCpuSampler::init() {
    close();
    base_mhz_ = query_nominal_mhz();

    if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) {
        query_ = nullptr;
        return false;
    }

    // Use English counter names so Windows locale never breaks telemetry.
    PdhAddEnglishCounterW(query_, L"\\Processor Information(_Total)\\% Processor Utility", 0, &utility_counter_);
    PdhAddEnglishCounterW(query_, L"\\Processor Information(_Total)\\% Processor Performance", 0, &perf_counter_);
    PdhAddEnglishCounterW(query_, L"\\Processor Information(_Total)\\% Processor Time", 0, &time_counter_);

    if (PdhCollectQueryData(query_) == ERROR_SUCCESS) {
        primed_ = true;
    }
    return true;
}

void PdhCpuSampler::close() {
    if (query_) {
        PdhCloseQuery(query_);
        query_ = nullptr;
        utility_counter_ = nullptr;
        perf_counter_ = nullptr;
        time_counter_ = nullptr;
        primed_ = false;
    }
}

PdhCpuSample PdhCpuSampler::sample() {
    PdhCpuSample out{};
    if (!query_) return out;

    if (PdhCollectQueryData(query_) != ERROR_SUCCESS) {
        return out;
    }
    if (!primed_) {
        primed_ = true;
        return out;
    }

    PDH_FMT_COUNTERVALUE val{};
    if (utility_counter_ && PdhGetFormattedCounterValue(utility_counter_, PDH_FMT_DOUBLE, nullptr, &val) == ERROR_SUCCESS &&
        (val.CStatus == PDH_CSTATUS_VALID_DATA || val.CStatus == PDH_CSTATUS_NEW_DATA) && std::isfinite(val.doubleValue)) {
        out.load_pct = std::clamp(static_cast<float>(val.doubleValue), 0.0f, 100.0f);
    } else if (time_counter_ && PdhGetFormattedCounterValue(time_counter_, PDH_FMT_DOUBLE, nullptr, &val) == ERROR_SUCCESS &&
               (val.CStatus == PDH_CSTATUS_VALID_DATA || val.CStatus == PDH_CSTATUS_NEW_DATA) && std::isfinite(val.doubleValue)) {
        out.load_pct = std::clamp(static_cast<float>(val.doubleValue), 0.0f, 100.0f);
    }

    if (base_mhz_ > 0.0f && perf_counter_ &&
        PdhGetFormattedCounterValue(perf_counter_, PDH_FMT_DOUBLE, nullptr, &val) == ERROR_SUCCESS &&
        (val.CStatus == PDH_CSTATUS_VALID_DATA || val.CStatus == PDH_CSTATUS_NEW_DATA) &&
        std::isfinite(val.doubleValue) && val.doubleValue > 0.0) {
        out.clock_mhz = static_cast<float>(base_mhz_ * (val.doubleValue / 100.0));
    }

    return out;
}

} // namespace mmoni
