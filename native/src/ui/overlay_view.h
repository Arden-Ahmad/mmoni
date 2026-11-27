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
    D2D1_RECT_F get_micro_bar_rect() const noexcept;

    bool effective_click_through() const noexcept;
    void sync_compositor_opacity();
    void install_win_hooks();
    void remove_win_hooks();
    static void CALLBACK on_win_event(
        HWINEVENTHOOK hook,
        DWORD event,
        HWND hwnd,
        LONG id_object,
        LONG id_child,
        DWORD event_thread,
        DWORD event_time);
    void schedule_overlap_refresh();
    void refresh_window_overlap(bool immediate_retract = false);
    void sync_presence_timer();
    void set_retracted(bool retracted, bool immediate = false);
    void evaluate_presence();

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
    void clear_input_region();
    void sync_overlay_input();
    bool recover_graphics_device(bool force = false);
    struct MinimalVisualKey {
        std::optional<float> cpu_temperature;
        std::optional<float> gpu_temperature;
        int cpu_secondary = 0;
        int gpu_secondary = 0;
        bool gpu_sleeping = false;
        bool micro_bar = false;

        bool operator==(const MinimalVisualKey&) const = default;
    };
    MinimalVisualKey minimal_visual_key() const noexcept;
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

    // Smart Presence (Smart Dodge / Auto-Retract) state & animation
    HWINEVENTHOOK win_hooks_[3]{};
    bool overlap_debounce_active_ = false;
    bool has_overlap_ = false;
    bool presence_retracted_ = false;
    bool presence_summoned_ = false;
    bool presence_timer_active_ = false;
    double dodge_leave_ms_ = 0.0;
    double top_dwell_start_ms_ = 0.0;
    bool retract_animating_ = false;
    bool reveal_input_pending_ = false;
    double retract_start_ms_ = 0.0;
    double retract_duration_ms_ = 180.0;
    float retract_from_ = 0.0f;
    float retract_to_ = 0.0f;
    float cur_retract_ = 0.0f;
    float last_compositor_opacity_ = -1.0f;

    std::optional<MinimalVisualKey> last_minimal_visual_;

    // Last applied HWND input region (pixels) & device-lost recovery throttle
    RECT last_rgn_rect_{};
    int last_rgn_rad_ = -1;
    uint64_t last_recover_ms_ = 0;
};

} // namespace mmoni
