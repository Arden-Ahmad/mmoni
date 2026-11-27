#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mmoni {

enum class Metric : uint8_t {
    Temperature = 0,
    Power = 1,
    Load = 2,
    Clock = 3
};

enum class TimeRange : int {
    Seconds60 = 60,
    Minutes5 = 300,
    Minutes15 = 900
};

enum class VisualTheme : uint8_t {
    LiquidGlass = 0,
    PrecisionDark = 1
};

enum class ChartLayoutMode : uint8_t {
    Split = 0,
    Combined = 1
};

enum class CollapsedDisplayMode : uint8_t {
    TempPower = 0,
    TempLoad = 1,
    TempOnly = 2,
    TempPowerSpark = 3
};

enum ThrottleFlags : uint32_t {
    ThrottleNone = 0,
    ThrottleThermal = 1u << 0,
    ThrottlePower = 1u << 1
};

struct ProcessorReading {
    std::optional<float> temperature;
    std::optional<float> power;
    std::optional<float> load;
    std::optional<float> clock;
    uint32_t throttle = ThrottleNone;
    bool is_sleeping = false;

    std::optional<float> get(Metric metric) const noexcept {
        switch (metric) {
            case Metric::Temperature: return temperature;
            case Metric::Power: return power;
            case Metric::Load: return load;
            case Metric::Clock: return clock;
        }
        return std::nullopt;
    }

    bool has_any_metric() const noexcept {
        return temperature.has_value() || power.has_value() || load.has_value() || clock.has_value();
    }
};

struct GpuDevice {
    std::string id;          // Reboot-stable persistent ID (PCI BDF or vendor:dev:subsys:ordinal)
    std::string name;
    bool is_discrete = false;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint32_t subsys_id = 0;
    uint64_t luid = 0;       // Session-only DXGI LUID for exact 1:1 IGCL matching
    int pci_bus = -1;
    int pci_dev = -1;
    int pci_func = -1;
    int vendor_ordinal = 0;  // 0-based index among same-vendor GPUs (for legacy WPF migration)

    bool operator==(const GpuDevice& other) const noexcept {
        return id == other.id;
    }
};

struct SensorSnapshot {
    std::optional<std::string> cpu_name;
    std::optional<ProcessorReading> cpu;
    std::optional<GpuDevice> selected_gpu;
    std::optional<ProcessorReading> gpu;
    std::vector<GpuDevice> gpus;
    bool pawnio_installed = false;
    std::optional<std::string> error;
    uint64_t timestamp_ms = 0;
    double cpu_poll_ms = 0.0;
    double gpu_poll_ms = 0.0;
};

struct TimedSample {
    uint64_t timestamp_ms = 0;
    std::optional<ProcessorReading> reading;
};

class RingBuffer {
public:
    static constexpr int kMaxCapacity = 900; // 15 minutes at 1 Hz

    void add(const std::optional<ProcessorReading>& reading, uint64_t timestamp_ms) noexcept {
        items_[head_] = TimedSample{timestamp_ms, reading};
        head_ = (head_ + 1) % kMaxCapacity;
        if (count_ < kMaxCapacity) {
            ++count_;
        }
        if (reading.has_value() && !reading->is_sleeping) {
            update_peak(peak_temp_, reading->temperature);
            update_peak(peak_power_, reading->power);
            update_peak(peak_load_, reading->load);
            update_peak(peak_clock_, reading->clock);
        }
    }

    void clear() noexcept {
        items_.fill(TimedSample{});
        head_ = 0;
        count_ = 0;
        reset_peaks();
    }

    void reset_peaks() noexcept {
        peak_temp_.reset();
        peak_power_.reset();
        peak_load_.reset();
        peak_clock_.reset();
    }

    std::optional<float> session_peak(Metric metric) const noexcept {
        switch (metric) {
            case Metric::Temperature: return peak_temp_;
            case Metric::Power: return peak_power_;
            case Metric::Load: return peak_load_;
            case Metric::Clock: return peak_clock_;
        }
        return std::nullopt;
    }

    int copy_timed(TimedSample* out, int out_cap) const noexcept {
        const int take = std::min(count_, out_cap);
        if (take <= 0) return 0;
        const int start = (head_ - take + kMaxCapacity) % kMaxCapacity;
        for (int i = 0; i < take; ++i) {
            out[i] = items_[(start + i) % kMaxCapacity];
        }
        return take;
    }

    int count() const noexcept { return count_; }

private:
    static void update_peak(std::optional<float>& peak, const std::optional<float>& v) noexcept {
        if (v.has_value() && std::isfinite(*v) && (!peak.has_value() || *v > *peak)) {
            peak = *v;
        }
    }

    std::array<TimedSample, kMaxCapacity> items_{};
    int head_ = 0;
    int count_ = 0;
    std::optional<float> peak_temp_;
    std::optional<float> peak_power_;
    std::optional<float> peak_load_;
    std::optional<float> peak_clock_;
};

} // namespace mmoni
