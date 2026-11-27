#pragma once

#include "sensors/sensor_types.h"

#include <d2d1_1.h>
#include <array>
#include <optional>
#include <vector>

namespace mmoni {

struct ChartStats {
    bool has_data = false;
    std::optional<float> min;
    std::optional<float> max;
    std::optional<float> avg;
    std::optional<float> latest;
};

struct ChartScrubInfo {
    int seconds_ago = 0;
    float value = 0.0f;
    float relative_x = 0.0f;
};

class HistoryChart {
public:
    static constexpr int kMaxRenderPoints = 90;

    explicit HistoryChart(bool is_gpu) noexcept : is_gpu_(is_gpu) {}

    ChartStats update(
        const RingBuffer& history,
        uint64_t now_ms,
        Metric metric,
        TimeRange range,
        VisualTheme theme,
        float warm_threshold,
        float hot_threshold);

    bool set_scrub_x(float local_x, float chart_width);
    bool clear_scrub();
    std::optional<ChartScrubInfo> current_scrub() const noexcept;

    void draw(ID2D1DeviceContext* dc, ID2D1Factory1* factory, const D2D1_RECT_F& bounds) const;

private:
    D2D1_COLOR_F select_accent_color() const noexcept;

    bool is_gpu_ = false;
    std::array<TimedSample, RingBuffer::kMaxCapacity> raw_buffer_{};
    std::array<std::optional<float>, kMaxRenderPoints> plot_values_{};

    int plot_count_ = 0;
    int window_seconds_ = 60;
    int bucket_seconds_ = 1;
    Metric metric_ = Metric::Temperature;
    VisualTheme theme_ = VisualTheme::LiquidGlass;
    float warm_threshold_ = 80.0f;
    float hot_threshold_ = 90.0f;
    std::optional<float> latest_value_;
    float data_min_ = 0.0f;
    float data_max_ = 0.0f;
    std::optional<int> scrub_slot_;
};

} // namespace mmoni
