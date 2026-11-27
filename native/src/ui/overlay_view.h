#pragma once

#include "platform/overlay_window.h"
#include "platform/settings.h"
#include "sensors/sensor_types.h"
#include "ui/compositor_host.h"
#include "ui/d2d_context.h"
#include "ui/history_chart.h"
#include "ui/overlay_renderer.h"

#include <windows.h>
#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <thread>

namespace mmoni {

class OverlayView {
public:
    struct Callbacks {
        std::function<void(POINT)> on_moved;
        std::function<void(TimeRange)> on_range_changed;
        std::function<void(UINT, WPARAM, LPARAM)> on_custom_message;
    };

    OverlayView(const UserSettings& settings, bool elevated, Callbacks callbacks);
    ~OverlayView();

    OverlayView(const OverlayView&) = delete;
    OverlayView& operator=(const OverlayView&) = delete;

    bool create(HINSTANCE instance);
    void destroy();

    HWND hwnd() const noexcept { return hwnd_; }
    bool is_visible() const noexcept { return visible_; }

    void show();
    void hide();
    void place_overlay();
    void handle_display_change();
    void ensure_topmost();

    void apply_settings(const UserSettings& updated);
    void display_snapshot(const SensorSnapshot& snapshot);
    void reset_position();
    void reset_peaks();

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle_message(UINT msg, WPARAM wp, LPARAM lp);

    double minimal_width() const noexcept;
    double minimal_height() const noexcept;
    double minimal_radius() const noexcept;
    double peek_radius() const noexcept;
    double dashboard_radius() const noexcept;
    double current_dashboard_height() const noexcept;

    struct Dim { double w, h, r; };
    Dim get_dimensions(IslandState st) const noexcept;
    D2D1_RECT_F get_chrome_rect(IslandState st) const noexcept;

    enum class HoverAction { None, Peek, Collapse };
    void track_mouse_leave();
    bool cursor_over_chrome() const;
    bool collapse_on_leave() const noexcept;
    void schedule_hover(HoverAction action);
    void arm_hover_timer(DWORD delay_ms);
    void cancel_hover();
    void handle_hover_timer();
    static void CALLBACK on_hover_timer(PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER);

    void transition_to(IslandState next, bool immediate = false);
    void start_vsync_pacer();
    void stop_vsync_pacer();
    void vsync_worker_loop();
    double now_ms() const noexcept;
    void tick_animations();
    void render_now();
    void sync_input_region(const D2D1_RECT_F& rect_dip, float radius_dip);
    bool recover_graphics_device(bool force = false);
    void invalidate_minimal_cache() noexcept;
    bool minimal_visual_changed(const SensorSnapshot& snap) const;
    void refresh_chart_stats();
    HitRegion hit_test(float dip_x, float dip_y) const noexcept;
    void handle_click(HitRegion region);

    UserSettings settings_;
    bool elevated_ = false;
    Callbacks callbacks_;

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    bool visible_ = false;
    bool mouse_tracked_ = false;
    PTP_TIMER hover_timer_ = nullptr;
    HoverAction hover_action_ = HoverAction::None;
    double hover_deadline_ms_ = 0.0;

    D2DContext d2d_;
    CompositorHost compositor_;
    OverlayRenderer renderer_;

    RingBuffer cpu_history_;
    RingBuffer gpu_history_;
    HistoryChart cpu_chart_{false};
    HistoryChart gpu_chart_{true};
    ChartStats cpu_stats_{};
    ChartStats gpu_stats_{};

    std::optional<SensorSnapshot> snapshot_;
    std::optional<std::string> history_gpu_id_;

    std::optional<POINT> anchor_;
    OverlayPlacement placement_{};
    DashboardLayout layout_{};

    IslandState state_ = IslandState::Minimal;
    Metric metric_ = Metric::Temperature;
    bool pinned_ = false;
    bool suppress_hover_until_leave_ = false;
    bool has_status_banner_ = false;
    bool live_heartbeat_bright_ = true;
    bool hovered_chrome_ = false;
    HitRegion hovered_region_ = HitRegion::None;
    HitRegion pressed_region_ = HitRegion::None;

    // Drag state
    bool drag_armed_ = false;
    bool dragging_ = false;
    POINT drag_start_screen_{};
    POINT drag_start_hwnd_{};
    HitRegion drag_origin_region_ = HitRegion::None;

    // The worker waits for the compositor clock only while animating.
    double qpc_to_ms_ = 0.0;
    double last_animation_frame_ms_ = 0.0;
    std::atomic<bool> vsync_stop_{false};
    std::atomic<bool> vsync_active_{false};
    std::atomic<bool> anim_tick_pending_{false};
    HANDLE vsync_wake_event_ = nullptr;
    std::thread vsync_thread_;

    bool morph_animating_ = false;
    double morph_start_ms_ = 0.0;
    double morph_duration_ms_ = 220.0;
    bool morph_expanding_ = false;
    D2D1_RECT_F morph_from_rect_{};
    D2D1_RECT_F morph_to_rect_{};
    float morph_from_r_ = 14.0f;
    float morph_to_r_ = 14.0f;
    float morph_from_op_[3] = {1.0f, 0.0f, 0.0f};
    float morph_to_op_[3] = {1.0f, 0.0f, 0.0f};

    D2D1_RECT_F cur_chrome_rect_{};
    float cur_radius_ = 14.0f;
    float cur_op_[3] = {1.0f, 0.0f, 0.0f};
    float cur_dash_slide_y_ = 0.0f;

    // Sliding segmented thumb animation
    bool thumb_animating_ = false;
    double thumb_start_ms_ = 0.0;
    double thumb_duration_ms_ = 200.0;
    float thumb_from_ = 0.0f;
    float thumb_to_ = 0.0f;
    float cur_thumb_pos_ = 0.0f;

    // Diff cache for zero-churn idle minimal pill updates
    bool has_min_cache_ = false;
    int last_min_cpu_t_ = -9999;
    int last_min_cpu_s_ = -9999;
    int last_min_gpu_t_ = -9999;
    int last_min_gpu_s_ = -9999;
    bool last_min_gpu_sleep_ = false;

    // Last applied HWND input region (pixels) & device-lost recovery throttle
    RECT last_rgn_rect_{};
    int last_rgn_rad_ = -1;
    uint64_t last_recover_ms_ = 0;
};

} // namespace mmoni
