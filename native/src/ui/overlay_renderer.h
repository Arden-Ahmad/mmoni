#pragma once

#include "platform/settings.h"
#include "sensors/sensor_types.h"
#include "ui/d2d_context.h"
#include "ui/history_chart.h"

#include <d2d1_1.h>
#include <dwrite_1.h>
#include <wrl/client.h>
#include <optional>
#include <string>

namespace mmoni {

enum class IslandState : uint8_t {
    Minimal = 0,
    Peek = 1,
    Dashboard = 2
};

enum class HitRegion : uint8_t {
    None = 0,
    ChromeBody,
    MinimalSurface,
    PeekSurface,
    DashboardHeader,
    PinButton,
    CollapseButton,
    TabTemp,
    TabPower,
    TabLoad,
    TabClock,
    RangeButton,
    CpuCard,
    GpuCard,
    CpuChart,
    GpuChart
};

struct DashboardLayout {
    D2D1_RECT_F chrome{};
    D2D1_RECT_F header{};
    D2D1_RECT_F pin_btn{};
    D2D1_RECT_F collapse_btn{};
    D2D1_RECT_F seg_track{};
    D2D1_RECT_F tab_rects[4]{};
    D2D1_RECT_F range_btn{};
    D2D1_RECT_F cpu_card{};
    D2D1_RECT_F gpu_card{};
    D2D1_RECT_F cpu_chart{};
    D2D1_RECT_F gpu_chart{};
    D2D1_RECT_F status_banner{};
};

struct OverlayRenderState {
    const UserSettings* settings = nullptr;
    const SensorSnapshot* snapshot = nullptr;
    const RingBuffer* cpu_history = nullptr;
    const RingBuffer* gpu_history = nullptr;
    const HistoryChart* cpu_chart = nullptr;
    const HistoryChart* gpu_chart = nullptr;
    ChartStats cpu_stats{};
    ChartStats gpu_stats{};

    IslandState state = IslandState::Minimal;
    Metric metric = Metric::Temperature;
    bool elevated = false;
    bool pinned = false;
    bool hovered_chrome = false;
    bool live_heartbeat_bright = true;
    HitRegion hovered_region = HitRegion::None;
    HitRegion pressed_region = HitRegion::None;

    // Animated geometry (in stage DIPs)
    D2D1_RECT_F chrome_rect{};
    float corner_radius = 14.0f;
    float minimal_opacity = 1.0f;
    float peek_opacity = 0.0f;
    float dashboard_opacity = 0.0f;
    float dashboard_slide_y = 0.0f;
    float seg_thumb_pos = 0.0f; // 0..3 continuous
    float retract_progress = 0.0f; // 0 = normal pill, 1 = fully retracted
    bool retract_is_top_bar = false;
    bool has_status_banner = false;
    std::wstring warning_text;
};

class OverlayRenderer {
public:
    OverlayRenderer() = default;

    bool init(D2DContext* ctx);
    void render(ID2D1DeviceContext* dc, const OverlayRenderState& st, DashboardLayout& out_layout) const;

    static DashboardLayout compute_dashboard_layout(const D2D1_RECT_F& chrome_rect, bool has_banner) noexcept;
    static std::wstring compute_warning_text(const SensorSnapshot* snapshot, bool elevated);

private:
    struct ThermalAccent {
        D2D1_COLOR_F color;
        float heat; // 0.0 = cool, 0.5 = warm, 1.0 = hot
    };

    void draw_shadow(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, float radius, bool is_glass, IslandState state) const;
    void draw_acrylic_grain(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, float opacity) const;
    void draw_curved_double_rim(ID2D1DeviceContext* dc, const OverlayRenderState& st, VisualTheme theme) const;
    void draw_glass_border(
        ID2D1DeviceContext* dc,
        const OverlayRenderState& st,
        VisualTheme theme,
        const ThermalAccent& cpu_accent,
        const ThermalAccent& gpu_accent) const;
    void draw_retracted_bar(
        ID2D1DeviceContext* dc,
        const OverlayRenderState& st,
        const ThermalAccent& cpu_accent,
        const ThermalAccent& gpu_accent,
        float opacity) const;
    void draw_minimal(ID2D1DeviceContext* dc, const OverlayRenderState& st) const;
    void draw_peek(ID2D1DeviceContext* dc, const OverlayRenderState& st) const;
    void draw_dashboard(ID2D1DeviceContext* dc, const OverlayRenderState& st, const DashboardLayout& lay) const;
    void draw_throttle_badge(ID2D1DeviceContext* dc, float x, float y, uint32_t flags) const;
    void draw_card(
        ID2D1DeviceContext* dc,
        const OverlayRenderState& st,
        const D2D1_RECT_F& card_rect,
        const D2D1_RECT_F& chart_rect,
        bool is_gpu) const;

    void draw_text(
        ID2D1DeviceContext* dc,
        const std::wstring& text,
        IDWriteTextFormat* fmt,
        const D2D1_RECT_F& rect,
        const D2D1_COLOR_F& color,
        float opacity = 1.0f,
        bool tabular = true) const;

    D2D1_COLOR_F thermal_color(const std::optional<float>& temp, bool is_gpu, const UserSettings& s) const noexcept;
    ThermalAccent thermal_edge_accent(const std::optional<float>& temp, bool is_gpu, bool sleeping, const UserSettings& s) const noexcept;
    std::wstring format_metric(const std::optional<float>& v, Metric m) const;
    std::wstring format_secondary(const std::optional<ProcessorReading>& r, CollapsedDisplayMode mode) const;

    D2DContext* ctx_ = nullptr;
    mutable Microsoft::WRL::ComPtr<ID2D1Bitmap> noise_bitmap_;
    mutable Microsoft::WRL::ComPtr<ID2D1BitmapBrush> noise_brush_;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_caption_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_pill_temp_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_pill_sec_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_peek_temp_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_peek_sub_left_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_peek_sub_right_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_peek_sub_center_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_peek_sub_ellipsis_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_dash_temp_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_dash_power_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_dash_sub_left_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_dash_sub_center_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_dash_sub_ellipsis_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_seg_tab_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_seg_tab_bold_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_range_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_card_title_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_card_model_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_stat_label_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_stat_val_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_stat_val_right_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_scale_right_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_empty_center_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_badge_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_tooltip_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_banner_;
};

} // namespace mmoni
