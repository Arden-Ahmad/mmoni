#include "ui/history_chart.h"

#include <wrl/client.h>
#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace mmoni {

namespace {

constexpr D2D1_COLOR_F make_rgb(uint8_t r, uint8_t g, uint8_t b, float a = 1.0f) noexcept {
    return {r / 255.0f, g / 255.0f, b / 255.0f, a};
}

constexpr D2D1_COLOR_F kCpuBaseColor = make_rgb(111, 231, 207);
constexpr D2D1_COLOR_F kGpuBaseColor = make_rgb(178, 184, 255);
constexpr D2D1_COLOR_F kWarmColor = make_rgb(251, 191, 36);
constexpr D2D1_COLOR_F kHotColor = make_rgb(251, 113, 133);

void trace_curve(ID2D1GeometrySink* sink, const std::vector<D2D1_POINT_2F>& pts, bool smooth) {
    if (!smooth || pts.size() < 3) {
        for (size_t i = 1; i < pts.size(); ++i) {
            sink->AddLine(pts[i]);
        }
        return;
    }

    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const auto p0 = pts[i > 0 ? i - 1 : 0];
        const auto p1 = pts[i];
        const auto p2 = pts[i + 1];
        const auto p3 = pts[std::min(pts.size() - 1, i + 2)];

        const float min_y = std::min(p1.y, p2.y);
        const float max_y = std::max(p1.y, p2.y);

        D2D1_BEZIER_SEGMENT seg{};
        seg.point1 = {
            p1.x + (p2.x - p0.x) / 6.0f,
            std::clamp(p1.y + (p2.y - p0.y) / 6.0f, min_y - 2.0f, max_y + 2.0f)
        };
        seg.point2 = {
            p2.x - (p3.x - p1.x) / 6.0f,
            std::clamp(p2.y - (p3.y - p1.y) / 6.0f, min_y - 2.0f, max_y + 2.0f)
        };
        seg.point3 = p2;
        sink->AddBezier(seg);
    }
}

} // namespace

ChartStats HistoryChart::update(
    const RingBuffer& history,
    uint64_t now_ms,
    Metric metric,
    TimeRange range,
    VisualTheme theme,
    float warm_threshold,
    float hot_threshold) {

    metric_ = metric;
    theme_ = theme;
    window_seconds_ = static_cast<int>(range);
    warm_threshold_ = warm_threshold;
    hot_threshold_ = hot_threshold;

    const int raw_count = history.copy_timed(raw_buffer_.data(), static_cast<int>(raw_buffer_.size()));

    bucket_seconds_ = (window_seconds_ <= 60) ? 1 : (window_seconds_ <= 300 ? 4 : 10);
    const int total_slots = window_seconds_ / bucket_seconds_;
    plot_count_ = std::min(total_slots, kMaxRenderPoints);

    std::array<double, kMaxRenderPoints> bsum{};
    std::array<int, kMaxRenderPoints> bcnt{};

    std::optional<float> min_v, max_v, latest_v;
    double sum = 0.0;
    int valid_count = 0;

    const uint64_t window_ms = static_cast<uint64_t>(window_seconds_) * 1000ULL;
    const uint64_t bucket_ms = static_cast<uint64_t>(bucket_seconds_) * 1000ULL;

    // Bin every sample strictly by real monotonic age (now_ms - sample.timestamp_ms) so pauses
    // leave honest timeline gaps and hover scrub ages are wall-clock accurate.
    for (int i = 0; i < raw_count; ++i) {
        const auto& sample = raw_buffer_[i];
        if (!sample.reading.has_value() || sample.reading->is_sleeping) continue;
        const auto val = sample.reading->get(metric);
        if (!val.has_value() || !std::isfinite(*val)) continue;
        if (sample.timestamp_ms > now_ms) continue;

        const uint64_t age_ms = now_ms - sample.timestamp_ms;
        if (age_ms > window_ms + bucket_ms / 2ULL) continue;

        const int slots_from_end = static_cast<int>((age_ms + bucket_ms / 2ULL) / bucket_ms);
        const int slot = plot_count_ - 1 - slots_from_end;
        if (slot < 0 || slot >= plot_count_) continue;

        const float v = *val;
        if (!min_v.has_value() || v < *min_v) min_v = v;
        if (!max_v.has_value() || v > *max_v) max_v = v;
        latest_v = v;
        sum += v;
        ++valid_count;

        bsum[slot] += v;
        ++bcnt[slot];
    }

    for (int slot = 0; slot < plot_count_; ++slot) {
        plot_values_[slot] = (bcnt[slot] > 0)
            ? std::optional<float>(static_cast<float>(bsum[slot] / bcnt[slot]))
            : std::nullopt;
    }

    latest_value_ = latest_v;
    data_min_ = min_v.value_or(0.0f);
    data_max_ = max_v.value_or(0.0f);

    ChartStats stats{};
    stats.has_data = valid_count > 0;
    stats.min = min_v;
    stats.max = max_v;
    stats.avg = stats.has_data ? std::optional<float>(static_cast<float>(sum / valid_count)) : std::nullopt;
    stats.latest = latest_v;
    return stats;
}

bool HistoryChart::set_scrub_x(float local_x, float chart_width) {
    if (plot_count_ <= 1 || chart_width <= 4.0f) return false;
    const float ratio = std::clamp((local_x - 2.0f) / (chart_width - 4.0f), 0.0f, 1.0f);
    const int slot = static_cast<int>(std::lround(ratio * static_cast<float>(plot_count_ - 1)));
    if (scrub_slot_.has_value() && *scrub_slot_ == slot) return false;
    scrub_slot_ = slot;
    return true;
}

bool HistoryChart::clear_scrub() {
    if (!scrub_slot_.has_value()) return false;
    scrub_slot_.reset();
    return true;
}

std::optional<ChartScrubInfo> HistoryChart::current_scrub() const noexcept {
    if (!scrub_slot_.has_value()) return std::nullopt;
    const int slot = *scrub_slot_;
    if (slot < 0 || slot >= plot_count_ || !plot_values_[slot].has_value()) return std::nullopt;

    ChartScrubInfo info{};
    info.seconds_ago = (plot_count_ - 1 - slot) * bucket_seconds_;
    info.value = *plot_values_[slot];
    info.relative_x = (plot_count_ > 1) ? static_cast<float>(slot) / static_cast<float>(plot_count_ - 1) : 1.0f;
    return info;
}

D2D1_COLOR_F HistoryChart::select_accent_color() const noexcept {
    if (metric_ == Metric::Temperature && latest_value_.has_value()) {
        if (*latest_value_ >= hot_threshold_) return kHotColor;
        if (*latest_value_ >= warm_threshold_) return kWarmColor;
    }
    return is_gpu_ ? kGpuBaseColor : kCpuBaseColor;
}

void HistoryChart::draw(ID2D1DeviceContext* dc, ID2D1Factory1* factory, const D2D1_RECT_F& bounds) const {
    const float full_w = bounds.right - bounds.left;
    const float full_h = bounds.bottom - bounds.top;
    if (full_w <= 6.0f || full_h <= 6.0f) return;

    const bool is_glass = (theme_ == VisualTheme::LiquidGlass);
    const float pad_x = 2.0f;
    const float pad_top = 8.0f;
    const float pad_bottom = 6.0f;
    const float width = full_w - pad_x * 2.0f;
    const float height = std::max(4.0f, full_h - pad_top - pad_bottom);
    const float bottom_y = bounds.bottom - 1.0f;

    // Horizontal reference grid lines
    ComPtr<ID2D1SolidColorBrush> grid_brush;
    dc->CreateSolidColorBrush(
        make_rgb(255, 255, 255, is_glass ? (18.0f / 255.0f) : (24.0f / 255.0f)),
        &grid_brush);

    ComPtr<ID2D1StrokeStyle> dash_style;
    if (!is_glass) {
        const float dashes[] = {2.0f, 2.5f};
        D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f);
        factory->CreateStrokeStyle(sp, dashes, 2, &dash_style);
    }

    if (grid_brush) {
        const float y1 = bounds.top + pad_top + height * 0.20f;
        const float y2 = bounds.top + pad_top + height * 0.65f;
        dc->DrawLine({bounds.left + pad_x, y1}, {bounds.right - pad_x, y1}, grid_brush.Get(), 1.0f, dash_style.Get());
        dc->DrawLine({bounds.left + pad_x, y2}, {bounds.right - pad_x, y2}, grid_brush.Get(), 1.0f, dash_style.Get());
    }

    if (plot_count_ <= 0) return;

    float min_span = 300.0f;
    switch (metric_) {
        case Metric::Temperature: min_span = 8.0f; break;
        case Metric::Power: min_span = 8.0f; break;
        case Metric::Load: min_span = 15.0f; break;
        case Metric::Clock: min_span = 300.0f; break;
    }

    const float span = std::max(data_max_ - data_min_, min_span);
    const float y_min = std::max(metric_ == Metric::Temperature ? 15.0f : 0.0f, data_min_ - span * 0.14f);
    float y_max = data_max_ + span * 0.14f;
    if (metric_ == Metric::Load) {
        y_max = std::min(100.0f, std::max(y_max, y_min + min_span));
    }
    if (y_max <= y_min) y_max = y_min + min_span;

    std::array<D2D1_POINT_2F, kMaxRenderPoints> all_pts{};
    std::array<bool, kMaxRenderPoints> valid{};
    int latest_slot = -1;

    const D2D1_COLOR_F accent = select_accent_color();

    ComPtr<ID2D1SolidColorBrush> line_brush;
    dc->CreateSolidColorBrush(accent, &line_brush);

    const float top_alpha = is_glass ? (88.0f / 255.0f) : (46.0f / 255.0f);
    D2D1_GRADIENT_STOP stops[2] = {
        {0.0f, {accent.r, accent.g, accent.b, top_alpha}},
        {1.0f, {accent.r, accent.g, accent.b, 0.0f}}
    };
    ComPtr<ID2D1GradientStopCollection> stop_col;
    dc->CreateGradientStopCollection(stops, 2, &stop_col);

    ComPtr<ID2D1LinearGradientBrush> fill_brush;
    if (stop_col) {
        dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties({bounds.left, bounds.top}, {bounds.left, bounds.bottom}),
            stop_col.Get(),
            &fill_brush);
    }

    ComPtr<ID2D1StrokeStyle> round_stroke;
    factory->CreateStrokeStyle(
        D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
        nullptr, 0, &round_stroke);

    std::vector<D2D1_POINT_2F> seg;
    seg.reserve(static_cast<size_t>(plot_count_));

    auto flush_segment = [&]() {
        if (seg.size() < 2) {
            seg.clear();
            return;
        }
        if (fill_brush) {
            ComPtr<ID2D1PathGeometry> area_geom;
            if (SUCCEEDED(factory->CreatePathGeometry(&area_geom)) && area_geom) {
                ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(area_geom->Open(&sink)) && sink) {
                    sink->BeginFigure({seg.front().x, bottom_y}, D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLine(seg.front());
                    trace_curve(sink.Get(), seg, is_glass);
                    sink->AddLine({seg.back().x, bottom_y});
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sink->Close();
                    dc->FillGeometry(area_geom.Get(), fill_brush.Get());
                }
            }
        }
        if (line_brush) {
            ComPtr<ID2D1PathGeometry> line_geom;
            if (SUCCEEDED(factory->CreatePathGeometry(&line_geom)) && line_geom) {
                ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(line_geom->Open(&sink)) && sink) {
                    sink->BeginFigure(seg.front(), D2D1_FIGURE_BEGIN_HOLLOW);
                    trace_curve(sink.Get(), seg, is_glass);
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    sink->Close();
                    dc->DrawGeometry(line_geom.Get(), line_brush.Get(), is_glass ? 1.85f : 1.45f, round_stroke.Get());
                }
            }
        }
        seg.clear();
    };

    for (int i = 0; i <= plot_count_; ++i) {
        if (i < plot_count_ && plot_values_[i].has_value()) {
            const float x = (plot_count_ > 1)
                ? (bounds.left + pad_x + width * static_cast<float>(i) / static_cast<float>(plot_count_ - 1))
                : (bounds.right - pad_x);
            const float frac = std::clamp((*plot_values_[i] - y_min) / (y_max - y_min), 0.0f, 1.0f);
            const D2D1_POINT_2F pt{x, bounds.top + pad_top + height * (1.0f - frac)};
            all_pts[i] = pt;
            valid[i] = true;
            latest_slot = i;
            seg.push_back(pt);
        } else {
            flush_segment();
        }
    }

    // Live endpoint dot with soft radial halo
    if (latest_slot >= 0 && valid[latest_slot] && line_brush) {
        const auto tip = all_pts[latest_slot];
        if (is_glass) {
            D2D1_GRADIENT_STOP gstops[2] = {
                {0.0f, {accent.r, accent.g, accent.b, 145.0f / 255.0f}},
                {1.0f, {accent.r, accent.g, accent.b, 0.0f}}
            };
            ComPtr<ID2D1GradientStopCollection> gcol;
            if (SUCCEEDED(dc->CreateGradientStopCollection(gstops, 2, &gcol)) && gcol) {
                ComPtr<ID2D1RadialGradientBrush> gbrush;
                if (SUCCEEDED(dc->CreateRadialGradientBrush(
                        D2D1::RadialGradientBrushProperties(tip, {0, 0}, 5.5f, 5.5f),
                        gcol.Get(), &gbrush)) && gbrush) {
                    dc->FillEllipse(D2D1::Ellipse(tip, 5.5f, 5.5f), gbrush.Get());
                }
            }
        }
        const float r = is_glass ? 2.6f : 2.1f;
        dc->FillEllipse(D2D1::Ellipse(tip, r, r), line_brush.Get());
    }

    // Interactive hover scrub crosshair & dot
    if (scrub_slot_.has_value()) {
        const int s = *scrub_slot_;
        if (s >= 0 && s < plot_count_ && valid[s]) {
            const auto sp = all_pts[s];
            ComPtr<ID2D1SolidColorBrush> scrub_brush;
            dc->CreateSolidColorBrush(make_rgb(255, 255, 255, 80.0f / 255.0f), &scrub_brush);

            const float dashes[] = {2.0f, 2.5f};
            ComPtr<ID2D1StrokeStyle> sdash;
            factory->CreateStrokeStyle(
                D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                            D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f),
                dashes, 2, &sdash);

            if (scrub_brush) {
                dc->DrawLine({sp.x, bounds.top + 1.0f}, {sp.x, bounds.bottom - 1.0f}, scrub_brush.Get(), 1.0f, sdash.Get());
            }

            ComPtr<ID2D1SolidColorBrush> white_brush;
            dc->CreateSolidColorBrush(make_rgb(255, 255, 255, 1.0f), &white_brush);
            if (white_brush) {
                dc->FillEllipse(D2D1::Ellipse(sp, 3.0f, 3.0f), white_brush.Get());
            }
            if (line_brush) {
                dc->DrawEllipse(D2D1::Ellipse(sp, 3.0f, 3.0f), line_brush.Get(), 1.5f);
            }
        }
    }
}

} // namespace mmoni
