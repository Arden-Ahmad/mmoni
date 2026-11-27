#include "ui/overlay_view.h"
#include "platform/single_instance.h"

#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace mmoni {

namespace {

constexpr wchar_t kOverlayClassName[] = L"MmoniNativeOverlayWindow";

constexpr double kPeekWidth = 320.0;
constexpr double kPeekHeight = 74.0;
constexpr double kDashboardWidth = 368.0;
constexpr double kDashboardBaseHeight = 262.0;
constexpr double kDashboardBannerExtraHeight = 28.0;
constexpr double kStageMaxHeight = kDashboardBaseHeight + kDashboardBannerExtraHeight;

constexpr UINT_PTR kTimerPeek = 1;
constexpr UINT_PTR kTimerCollapse = 2;
constexpr UINT_PTR kTimerAnim = 3;

bool pt_in_rect(const D2D1_RECT_F& r, float x, float y) noexcept {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

float cubic_ease_out(float t) noexcept {
    float inv = 1.0f - std::clamp(t, 0.0f, 1.0f);
    return 1.0f - inv * inv * inv;
}

float back_ease_out(float t, float amp = 0.16f) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    const float inv = 1.0f - t;
    constexpr float kPi = 3.1415926535f;
    return 1.0f - (inv * inv * inv - inv * amp * std::sin(inv * kPi));
}

float lerpf(float a, float b, float t) noexcept {
    return a + (b - a) * t;
}

int quantize_opt(const std::optional<float>& v, float scale) noexcept {
    if (!v.has_value() || !std::isfinite(*v)) return -999999;
    return static_cast<int>(std::lround(*v * scale));
}

} // namespace

OverlayView::OverlayView(const UserSettings& settings, bool elevated, Callbacks callbacks)
    : settings_(settings), elevated_(elevated), callbacks_(std::move(callbacks)) {
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    qpc_to_ms_ = (freq.QuadPart > 0) ? (1000.0 / static_cast<double>(freq.QuadPart)) : 0.0;
    if (settings_.left.has_value() && settings_.top.has_value()) {
        anchor_ = POINT{*settings_.left, *settings_.top};
    }
}

double OverlayView::now_ms() const noexcept {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) * qpc_to_ms_;
}

void OverlayView::start_vsync_pacer() {
    if (!hwnd_) return;
    if (!vsync_wake_event_) {
        vsync_wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    if (!vsync_thread_.joinable()) {
        vsync_stop_.store(false, std::memory_order_release);
        vsync_thread_ = std::thread(&OverlayView::vsync_worker_loop, this);
    }
    vsync_active_.store(true, std::memory_order_release);
    if (vsync_wake_event_) {
        SetEvent(vsync_wake_event_);
    }
}

void OverlayView::stop_vsync_pacer() {
    vsync_active_.store(false, std::memory_order_release);
    anim_tick_pending_.store(false, std::memory_order_release);
}

void OverlayView::vsync_worker_loop() {
    while (!vsync_stop_.load(std::memory_order_acquire)) {
        if (!vsync_active_.load(std::memory_order_acquire)) {
            WaitForSingleObject(vsync_wake_event_, INFINITE);
            continue;
        }

        // Sync 1:1 with the monitor's native DWM refresh rate (60/120/144/165/240 Hz).
        if (FAILED(DwmFlush())) {
            Sleep(8);
        }

        if (vsync_stop_.load(std::memory_order_acquire) || !vsync_active_.load(std::memory_order_acquire)) {
            continue;
        }

        // Coalesce pending tick messages so the UI queue is never flooded.
        bool expected = false;
        if (anim_tick_pending_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            if (hwnd_) {
                PostMessageW(hwnd_, WM_MMONI_ANIM_TICK, 0, 0);
            } else {
                anim_tick_pending_.store(false, std::memory_order_release);
            }
        }
    }
}

OverlayView::~OverlayView() {
    destroy();
}

double OverlayView::minimal_width() const noexcept {
    switch (settings_.collapsed_mode) {
        case CollapsedDisplayMode::TempOnly: return 138.0;
        case CollapsedDisplayMode::TempLoad: return 204.0;
        default: return settings_.theme == VisualTheme::LiquidGlass ? 228.0 : 224.0;
    }
}

double OverlayView::minimal_height() const noexcept {
    return settings_.theme == VisualTheme::LiquidGlass ? 28.0 : 26.0;
}

double OverlayView::minimal_radius() const noexcept {
    return minimal_height() * 0.5;
}

double OverlayView::peek_radius() const noexcept {
    return settings_.theme == VisualTheme::LiquidGlass ? 18.0 : 11.0;
}

double OverlayView::dashboard_radius() const noexcept {
    return settings_.theme == VisualTheme::LiquidGlass ? 20.0 : 12.0;
}

double OverlayView::current_dashboard_height() const noexcept {
    return has_status_banner_ ? (kDashboardBaseHeight + kDashboardBannerExtraHeight) : kDashboardBaseHeight;
}

OverlayView::Dim OverlayView::get_dimensions(IslandState st) const noexcept {
    switch (st) {
        case IslandState::Minimal: return {minimal_width(), minimal_height(), minimal_radius()};
        case IslandState::Peek: return {kPeekWidth, kPeekHeight, peek_radius()};
        case IslandState::Dashboard: return {kDashboardWidth, current_dashboard_height(), dashboard_radius()};
    }
    return {minimal_width(), minimal_height(), minimal_radius()};
}

D2D1_RECT_F OverlayView::get_chrome_rect(IslandState st) const noexcept {
    const auto d = get_dimensions(st);
    const double compact_cx = placement_.compact_offset_x + minimal_width() * 0.5;
    const double max_left = OverlayWindowHelper::kShadowPadLeft + std::max(0.0, kDashboardWidth - d.w);
    const double left = std::clamp(compact_cx - d.w * 0.5, OverlayWindowHelper::kShadowPadLeft, max_left);

    const double max_top = OverlayWindowHelper::kShadowPadTop + std::max(0.0, kStageMaxHeight - d.h);
    const double top = std::clamp(placement_.compact_offset_y, OverlayWindowHelper::kShadowPadTop, max_top);

    return {
        static_cast<float>(left),
        static_cast<float>(top),
        static_cast<float>(left + d.w),
        static_cast<float>(top + d.h)
    };
}

bool OverlayView::create(HINSTANCE instance) {
    instance_ = instance;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &OverlayView::wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kOverlayClassName;
    RegisterClassExW(&wc);

    DWORD ex_style = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP;
    if (settings_.click_through) {
        ex_style |= WS_EX_TRANSPARENT;
    }

    hwnd_ = CreateWindowExW(
        ex_style,
        kOverlayClassName,
        L"Mmoni",
        WS_POPUP,
        0, 0, 404, 322,
        nullptr, nullptr, instance, this);

    if (!hwnd_) return false;

    // Prevent Taskbar thumbnail hover (Aero Peek / Live Preview) from hiding/ghosting the overlay.
    BOOL excluded = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_EXCLUDED_FROM_PEEK, &excluded, sizeof(excluded));
    DwmSetWindowAttribute(hwnd_, DWMWA_DISALLOW_PEEK, &excluded, sizeof(excluded));

    if (!d2d_.init() || !renderer_.init(&d2d_) || !compositor_.init(hwnd_, &d2d_)) {
        destroy();
        return false;
    }

    compositor_.set_opacity(settings_.click_through ? 0.78f : 1.0f);
    place_overlay();
    return true;
}

void OverlayView::ensure_topmost() {
    if (!hwnd_ || !visible_) return;
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void OverlayView::sync_input_region(const D2D1_RECT_F& rect_dip, float radius_dip) {
    if (!hwnd_) return;
    const double dx = placement_.dpi_scale_x > 0.0 ? placement_.dpi_scale_x : 1.0;
    const double dy = placement_.dpi_scale_y > 0.0 ? placement_.dpi_scale_y : 1.0;

    // Expand by 1px so the anti-aliased D2D border is never clipped, while all empty stage
    // pixels outside the visible pill/peek/dashboard pass clicks across processes to apps below.
    const int l = std::max(0, static_cast<int>(std::floor(rect_dip.left * dx)) - 1);
    const int t = std::max(0, static_cast<int>(std::floor(rect_dip.top * dy)) - 1);
    const int r = static_cast<int>(std::ceil(rect_dip.right * dx)) + 2;
    const int b = static_cast<int>(std::ceil(rect_dip.bottom * dy)) + 2;
    const int rad = std::max(1, static_cast<int>(std::lround(radius_dip * dx)) + 1);

    if (last_rgn_rad_ == rad &&
        last_rgn_rect_.left == l && last_rgn_rect_.top == t &&
        last_rgn_rect_.right == r && last_rgn_rect_.bottom == b) {
        return;
    }

    HRGN rgn = CreateRoundRectRgn(l, t, r, b, rad * 2, rad * 2);
    if (rgn) {
        if (SetWindowRgn(hwnd_, rgn, FALSE) != 0) {
            last_rgn_rect_ = {l, t, r, b};
            last_rgn_rad_ = rad;
        } else {
            DeleteObject(rgn);
        }
    }
}

bool OverlayView::recover_graphics_device() {
    const uint64_t now = GetTickCount64();
    if (last_recover_ms_ != 0 && now - last_recover_ms_ < 1000ULL) {
        return false;
    }
    last_recover_ms_ = now;
    if (!hwnd_) return false;

    compositor_.shutdown();
    d2d_.reset();
    if (!d2d_.init() || !renderer_.init(&d2d_) || !compositor_.init(hwnd_, &d2d_)) {
        return false;
    }

    compositor_.set_opacity(settings_.click_through ? 0.78f : 1.0f);
    const int stage_w = placement_.stage_window.right - placement_.stage_window.left;
    const int stage_h = placement_.stage_window.bottom - placement_.stage_window.top;
    if (stage_w > 0 && stage_h > 0) {
        compositor_.resize_surface(
            stage_w,
            stage_h,
            static_cast<float>(placement_.dpi_scale_x * 96.0),
            static_cast<float>(placement_.dpi_scale_y * 96.0));
    }
    return true;
}

void OverlayView::destroy() {
    vsync_stop_.store(true, std::memory_order_release);
    vsync_active_.store(false, std::memory_order_release);
    if (vsync_wake_event_) SetEvent(vsync_wake_event_);
    if (vsync_thread_.joinable()) vsync_thread_.join();
    if (vsync_wake_event_) {
        CloseHandle(vsync_wake_event_);
        vsync_wake_event_ = nullptr;
    }

    if (hwnd_) {
        KillTimer(hwnd_, kTimerPeek);
        KillTimer(hwnd_, kTimerCollapse);
        KillTimer(hwnd_, kTimerAnim);
    }
    compositor_.shutdown();
    d2d_.reset();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    visible_ = false;
}

void OverlayView::show() {
    if (!hwnd_) return;
    visible_ = true;
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    place_overlay();
}

void OverlayView::hide() {
    if (!hwnd_) return;
    visible_ = false;
    KillTimer(hwnd_, kTimerPeek);
    KillTimer(hwnd_, kTimerCollapse);
    KillTimer(hwnd_, kTimerAnim);
    stop_vsync_pacer();
    morph_animating_ = false;
    thumb_animating_ = false;
    ShowWindow(hwnd_, SW_HIDE);
}

void OverlayView::place_overlay() {
    if (!hwnd_) return;

    placement_ = OverlayWindowHelper::compute_placement(
        hwnd_,
        anchor_,
        minimal_width(),
        minimal_height(),
        kDashboardWidth,
        kStageMaxHeight);

    if (!anchor_.has_value()) {
        anchor_ = POINT{placement_.compact_visual.left, placement_.compact_visual.top};
    }

    const int stage_w = placement_.stage_window.right - placement_.stage_window.left;
    const int stage_h = placement_.stage_window.bottom - placement_.stage_window.top;

    OverlayWindowHelper::set_bounds_topmost(
        hwnd_,
        placement_.stage_window.left,
        placement_.stage_window.top,
        stage_w,
        stage_h);

    if (compositor_.resize_surface(
            stage_w,
            stage_h,
            static_cast<float>(placement_.dpi_scale_x * 96.0),
            static_cast<float>(placement_.dpi_scale_y * 96.0)) == RenderStatus::DeviceLost) {
        recover_graphics_device();
    }

    stop_vsync_pacer();
    morph_animating_ = false;
    cur_chrome_rect_ = get_chrome_rect(state_);
    cur_radius_ = static_cast<float>(get_dimensions(state_).r);
    cur_op_[0] = (state_ == IslandState::Minimal) ? 1.0f : 0.0f;
    cur_op_[1] = (state_ == IslandState::Peek) ? 1.0f : 0.0f;
    cur_op_[2] = (state_ == IslandState::Dashboard) ? 1.0f : 0.0f;
    cur_dash_slide_y_ = 0.0f;

    sync_input_region(cur_chrome_rect_, cur_radius_);
    render_now();
}

void OverlayView::apply_settings(const UserSettings& updated) {
    const bool theme_changed = settings_.theme != updated.theme;
    const bool collapsed_changed = settings_.collapsed_mode != updated.collapsed_mode;
    const bool click_changed = settings_.click_through != updated.click_through;
    const bool auto_exp_changed = settings_.auto_expand_on_hover != updated.auto_expand_on_hover;
    const bool range_changed = settings_.history_range != updated.history_range;

    settings_ = updated;

    if (click_changed) {
        if (hwnd_) {
            KillTimer(hwnd_, kTimerPeek);
            KillTimer(hwnd_, kTimerCollapse);
        }
        if (settings_.click_through && state_ != IslandState::Minimal) {
            pinned_ = false;
            transition_to(IslandState::Minimal);
        }
        OverlayWindowHelper::set_click_through(hwnd_, settings_.click_through);
        compositor_.set_opacity(settings_.click_through ? 0.78f : 1.0f);
    }

    if (auto_exp_changed && !settings_.auto_expand_on_hover && state_ == IslandState::Peek) {
        transition_to(IslandState::Minimal);
    }

    invalidate_minimal_cache();

    if (range_changed || theme_changed) {
        refresh_chart_stats();
    }

    if (theme_changed || collapsed_changed) {
        place_overlay();
    } else {
        render_now();
    }
}

void OverlayView::reset_position() {
    anchor_.reset();
    if (state_ != IslandState::Minimal) {
        pinned_ = false;
        transition_to(IslandState::Minimal, true);
    }
    place_overlay();
}

void OverlayView::reset_peaks() {
    cpu_history_.reset_peaks();
    gpu_history_.reset_peaks();
    if (state_ == IslandState::Dashboard) {
        refresh_chart_stats();
        render_now();
    }
}

void OverlayView::invalidate_minimal_cache() noexcept {
    has_min_cache_ = false;
}

bool OverlayView::minimal_visual_changed(const SensorSnapshot& snap) const {
    if (!has_min_cache_) return true;
    const int ct = quantize_opt(snap.cpu ? snap.cpu->temperature : std::nullopt, 1.0f);
    const int cs = (settings_.collapsed_mode == CollapsedDisplayMode::TempLoad)
        ? quantize_opt(snap.cpu ? snap.cpu->load : std::nullopt, 1.0f)
        : (settings_.collapsed_mode == CollapsedDisplayMode::TempOnly
            ? 0
            : quantize_opt(snap.cpu ? snap.cpu->power : std::nullopt, 10.0f));

    const bool gsleep = snap.gpu.has_value() && snap.gpu->is_sleeping;
    const int gt = gsleep ? -888888 : quantize_opt(snap.gpu ? snap.gpu->temperature : std::nullopt, 1.0f);
    const int gs = gsleep ? -888888
        : ((settings_.collapsed_mode == CollapsedDisplayMode::TempLoad)
            ? quantize_opt(snap.gpu ? snap.gpu->load : std::nullopt, 1.0f)
            : (settings_.collapsed_mode == CollapsedDisplayMode::TempOnly
                ? 0
                : quantize_opt(snap.gpu ? snap.gpu->power : std::nullopt, 10.0f)));

    return ct != last_min_cpu_t_ || cs != last_min_cpu_s_ ||
           gt != last_min_gpu_t_ || gs != last_min_gpu_s_ ||
           gsleep != last_min_gpu_sleep_;
}

void OverlayView::refresh_chart_stats() {
    const uint64_t now = snapshot_ && snapshot_->timestamp_ms != 0 ? snapshot_->timestamp_ms : GetTickCount64();
    cpu_stats_ = cpu_chart_.update(cpu_history_, now, metric_, settings_.history_range, settings_.theme,
                                   settings_.cpu_warm_temp, settings_.cpu_hot_temp);
    gpu_stats_ = gpu_chart_.update(gpu_history_, now, metric_, settings_.history_range, settings_.theme,
                                   settings_.gpu_warm_temp, settings_.gpu_hot_temp);
}

void OverlayView::display_snapshot(const SensorSnapshot& snapshot) {
    const std::optional<std::string> next_gpu_id = snapshot.selected_gpu ? std::optional<std::string>(snapshot.selected_gpu->id) : std::nullopt;
    if (history_gpu_id_ != next_gpu_id && !snapshot.error.has_value()) {
        gpu_history_.clear();
        history_gpu_id_ = next_gpu_id;
    }

    const uint64_t ts = snapshot.timestamp_ms != 0 ? snapshot.timestamp_ms : GetTickCount64();
    snapshot_ = snapshot;
    cpu_history_.add(snapshot.cpu, ts);
    gpu_history_.add(snapshot.gpu, ts);

    if (!visible_) return;

    if (state_ == IslandState::Minimal && !morph_animating_) {
        if (!minimal_visual_changed(snapshot)) {
            return; // Zero GPU/D2D churn when idle values haven't changed!
        }
        has_min_cache_ = true;
        last_min_cpu_t_ = quantize_opt(snapshot.cpu ? snapshot.cpu->temperature : std::nullopt, 1.0f);
        last_min_cpu_s_ = (settings_.collapsed_mode == CollapsedDisplayMode::TempLoad)
            ? quantize_opt(snapshot.cpu ? snapshot.cpu->load : std::nullopt, 1.0f)
            : (settings_.collapsed_mode == CollapsedDisplayMode::TempOnly
                ? 0
                : quantize_opt(snapshot.cpu ? snapshot.cpu->power : std::nullopt, 10.0f));
        last_min_gpu_sleep_ = snapshot.gpu.has_value() && snapshot.gpu->is_sleeping;
        last_min_gpu_t_ = last_min_gpu_sleep_ ? -888888 : quantize_opt(snapshot.gpu ? snapshot.gpu->temperature : std::nullopt, 1.0f);
        last_min_gpu_s_ = last_min_gpu_sleep_ ? -888888
            : ((settings_.collapsed_mode == CollapsedDisplayMode::TempLoad)
                ? quantize_opt(snapshot.gpu ? snapshot.gpu->load : std::nullopt, 1.0f)
                : (settings_.collapsed_mode == CollapsedDisplayMode::TempOnly
                    ? 0
                    : quantize_opt(snapshot.gpu ? snapshot.gpu->power : std::nullopt, 10.0f)));
        render_now();
        return;
    }

    if (state_ == IslandState::Dashboard) {
        live_heartbeat_bright_ = !live_heartbeat_bright_;
        refresh_chart_stats();

        const bool next_banner = !OverlayRenderer::compute_warning_text(&(*snapshot_), elevated_).empty();
        if (next_banner != has_status_banner_) {
            has_status_banner_ = next_banner;
            if (!morph_animating_) {
                cur_chrome_rect_ = get_chrome_rect(IslandState::Dashboard);
                sync_input_region(cur_chrome_rect_, cur_radius_);
            }
        }
    }

    render_now();
}

void OverlayView::transition_to(IslandState next, bool immediate) {
    if (state_ == next && !immediate) return;

    const IslandState prev = state_;
    state_ = next;

    if (next == IslandState::Dashboard) {
        live_heartbeat_bright_ = true;
        has_status_banner_ = !OverlayRenderer::compute_warning_text(snapshot_ ? &(*snapshot_) : nullptr, elevated_).empty();
        refresh_chart_stats();
    } else {
        cpu_chart_.clear_scrub();
        gpu_chart_.clear_scrub();
    }

    const D2D1_RECT_F target_rect = get_chrome_rect(next);
    const float target_r = static_cast<float>(get_dimensions(next).r);

    if (immediate || !hwnd_) {
        morph_animating_ = false;
        if (!thumb_animating_) stop_vsync_pacer();
        cur_chrome_rect_ = target_rect;
        cur_radius_ = target_r;
        cur_op_[0] = (next == IslandState::Minimal) ? 1.0f : 0.0f;
        cur_op_[1] = (next == IslandState::Peek) ? 1.0f : 0.0f;
        cur_op_[2] = (next == IslandState::Dashboard) ? 1.0f : 0.0f;
        cur_dash_slide_y_ = 0.0f;
        sync_input_region(cur_chrome_rect_, cur_radius_);
        render_now();
        return;
    }

    const bool is_glass = (settings_.theme == VisualTheme::LiquidGlass);
    morph_expanding_ = static_cast<int>(next) > static_cast<int>(prev);
    morph_duration_ms_ = morph_expanding_ ? (is_glass ? 255.0 : 180.0) : (is_glass ? 195.0 : 160.0);
    morph_start_ms_ = now_ms();
    morph_from_rect_ = cur_chrome_rect_;
    morph_to_rect_ = target_rect;
    morph_from_r_ = cur_radius_;
    morph_to_r_ = target_r;
    for (int i = 0; i < 3; ++i) {
        morph_from_op_[i] = cur_op_[i];
        morph_to_op_[i] = (static_cast<int>(next) == i) ? 1.0f : 0.0f;
    }
    morph_animating_ = true;

    // Pre-expand region when growing so the morph never clips; tighten on collapse completion.
    if (morph_expanding_) {
        sync_input_region(target_rect, target_r);
    }

    start_vsync_pacer();
    render_now();
}

void OverlayView::tick_animations() {
    anim_tick_pending_.store(false, std::memory_order_release);
    if (!morph_animating_ && !thumb_animating_) {
        stop_vsync_pacer();
        return;
    }

    const double now = now_ms();
    const bool is_glass = (settings_.theme == VisualTheme::LiquidGlass);

    if (morph_animating_) {
        const float raw_t = std::clamp(
            static_cast<float>((now - morph_start_ms_) / std::max(1.0, morph_duration_ms_)),
            0.0f, 1.0f);
        const float e = (morph_expanding_ && is_glass) ? back_ease_out(raw_t, 0.16f) : cubic_ease_out(raw_t);

        cur_chrome_rect_ = {
            lerpf(morph_from_rect_.left, morph_to_rect_.left, e),
            lerpf(morph_from_rect_.top, morph_to_rect_.top, e),
            lerpf(morph_from_rect_.right, morph_to_rect_.right, e),
            lerpf(morph_from_rect_.bottom, morph_to_rect_.bottom, e)
        };
        cur_radius_ = std::max(1.0f, lerpf(morph_from_r_, morph_to_r_, e));

        const float fade_in_t = std::clamp(static_cast<float>((now - morph_start_ms_) / 185.0), 0.0f, 1.0f);
        const float fade_out_t = std::clamp(static_cast<float>((now - morph_start_ms_) / 90.0), 0.0f, 1.0f);
        for (int i = 0; i < 3; ++i) {
            const float ft = (morph_to_op_[i] > morph_from_op_[i]) ? fade_in_t : fade_out_t;
            cur_op_[i] = std::clamp(lerpf(morph_from_op_[i], morph_to_op_[i], ft), 0.0f, 1.0f);
        }

        if (state_ == IslandState::Dashboard) {
            const float slide_t = cubic_ease_out(std::clamp(static_cast<float>((now - morph_start_ms_) / 200.0), 0.0f, 1.0f));
            cur_dash_slide_y_ = lerpf(-5.0f, 0.0f, slide_t);
        } else {
            cur_dash_slide_y_ = 0.0f;
        }

        if (raw_t >= 1.0f) {
            morph_animating_ = false;
            cur_chrome_rect_ = morph_to_rect_;
            cur_radius_ = morph_to_r_;
            for (int i = 0; i < 3; ++i) cur_op_[i] = morph_to_op_[i];
            cur_dash_slide_y_ = 0.0f;
            sync_input_region(cur_chrome_rect_, cur_radius_);
        }
    }

    if (thumb_animating_) {
        const float raw_t = std::clamp(
            static_cast<float>((now - thumb_start_ms_) / std::max(1.0, thumb_duration_ms_)),
            0.0f, 1.0f);
        const float e = is_glass ? back_ease_out(raw_t, 0.20f) : cubic_ease_out(raw_t);
        cur_thumb_pos_ = lerpf(thumb_from_, thumb_to_, e);
        if (raw_t >= 1.0f) {
            thumb_animating_ = false;
            cur_thumb_pos_ = thumb_to_;
        }
    }

    if (!morph_animating_ && !thumb_animating_) {
        stop_vsync_pacer();
    }

    render_now();
}

void OverlayView::render_now() {
    if (!compositor_.is_ready()) return;

    const float dx = static_cast<float>(placement_.dpi_scale_x);
    const float dy = static_cast<float>(placement_.dpi_scale_y);
    const bool show_backdrop = visible_ && (settings_.theme == VisualTheme::LiquidGlass);
    compositor_.update_backdrop(
        show_backdrop,
        cur_chrome_rect_.left * dx,
        cur_chrome_rect_.top * dy,
        (cur_chrome_rect_.right - cur_chrome_rect_.left) * dx,
        (cur_chrome_rect_.bottom - cur_chrome_rect_.top) * dy,
        cur_radius_ * dx);

    OverlayRenderState st{};
    st.settings = &settings_;
    st.snapshot = snapshot_ ? &(*snapshot_) : nullptr;
    st.cpu_history = &cpu_history_;
    st.gpu_history = &gpu_history_;
    st.cpu_chart = &cpu_chart_;
    st.gpu_chart = &gpu_chart_;
    st.cpu_stats = cpu_stats_;
    st.gpu_stats = gpu_stats_;
    st.state = state_;
    st.metric = metric_;
    st.elevated = elevated_;
    st.pinned = pinned_;
    st.hovered_chrome = hovered_chrome_;
    st.live_heartbeat_bright = live_heartbeat_bright_;
    st.hovered_region = hovered_region_;
    st.pressed_region = pressed_region_;
    st.chrome_rect = cur_chrome_rect_;
    st.corner_radius = cur_radius_;
    st.minimal_opacity = cur_op_[0];
    st.peek_opacity = cur_op_[1];
    st.dashboard_opacity = cur_op_[2];
    st.dashboard_slide_y = cur_dash_slide_y_;
    st.seg_thumb_pos = cur_thumb_pos_;
    st.has_status_banner = has_status_banner_;
    if (has_status_banner_) {
        st.warning_text = OverlayRenderer::compute_warning_text(st.snapshot, elevated_);
    }

    auto status = compositor_.render([&](ID2D1DeviceContext* dc, float, float) {
        renderer_.render(dc, st, layout_);
    });
    if (status == RenderStatus::DeviceLost && recover_graphics_device()) {
        compositor_.update_backdrop(
            show_backdrop,
            cur_chrome_rect_.left * dx,
            cur_chrome_rect_.top * dy,
            (cur_chrome_rect_.right - cur_chrome_rect_.left) * dx,
            (cur_chrome_rect_.bottom - cur_chrome_rect_.top) * dy,
            cur_radius_ * dx);
        compositor_.render([&](ID2D1DeviceContext* dc, float, float) {
            renderer_.render(dc, st, layout_);
        });
    }
}

HitRegion OverlayView::hit_test(float x, float y) const noexcept {
    if (!pt_in_rect(cur_chrome_rect_, x, y)) {
        return HitRegion::None;
    }
    if (state_ == IslandState::Minimal) return HitRegion::MinimalSurface;
    if (state_ == IslandState::Peek) return HitRegion::PeekSurface;

    if (pt_in_rect(layout_.pin_btn, x, y)) return HitRegion::PinButton;
    if (pt_in_rect(layout_.collapse_btn, x, y)) return HitRegion::CollapseButton;
    if (pt_in_rect(layout_.header, x, y)) return HitRegion::DashboardHeader;
    if (pt_in_rect(layout_.range_btn, x, y)) return HitRegion::RangeButton;
    if (pt_in_rect(layout_.tab_rects[0], x, y)) return HitRegion::TabTemp;
    if (pt_in_rect(layout_.tab_rects[1], x, y)) return HitRegion::TabPower;
    if (pt_in_rect(layout_.tab_rects[2], x, y)) return HitRegion::TabLoad;
    if (pt_in_rect(layout_.tab_rects[3], x, y)) return HitRegion::TabClock;
    if (pt_in_rect(layout_.cpu_chart, x, y)) return HitRegion::CpuChart;
    if (pt_in_rect(layout_.gpu_chart, x, y)) return HitRegion::GpuChart;
    if (pt_in_rect(layout_.cpu_card, x, y)) return HitRegion::CpuCard;
    if (pt_in_rect(layout_.gpu_card, x, y)) return HitRegion::GpuCard;
    return HitRegion::ChromeBody;
}

void OverlayView::handle_click(HitRegion region) {
    auto select_metric = [&](Metric m) {
        if (metric_ == m) return;
        metric_ = m;
        thumb_from_ = cur_thumb_pos_;
        thumb_to_ = static_cast<float>(static_cast<int>(m));
        thumb_start_ms_ = now_ms();
        thumb_duration_ms_ = (settings_.theme == VisualTheme::LiquidGlass) ? 230.0 : 160.0;
        thumb_animating_ = true;
        refresh_chart_stats();
        start_vsync_pacer();
        render_now();
    };

    switch (region) {
        case HitRegion::MinimalSurface:
        case HitRegion::PeekSurface:
            KillTimer(hwnd_, kTimerPeek);
            KillTimer(hwnd_, kTimerCollapse);
            pinned_ = true;
            transition_to(IslandState::Dashboard);
            break;
        case HitRegion::DashboardHeader:
        case HitRegion::CollapseButton:
            KillTimer(hwnd_, kTimerPeek);
            KillTimer(hwnd_, kTimerCollapse);
            pinned_ = false;
            suppress_hover_until_leave_ = true;
            transition_to(IslandState::Minimal);
            break;
        case HitRegion::PinButton:
            pinned_ = !pinned_;
            render_now();
            break;
        case HitRegion::TabTemp: select_metric(Metric::Temperature); break;
        case HitRegion::TabPower: select_metric(Metric::Power); break;
        case HitRegion::TabLoad: select_metric(Metric::Load); break;
        case HitRegion::TabClock: select_metric(Metric::Clock); break;
        case HitRegion::RangeButton: {
            TimeRange next = (settings_.history_range == TimeRange::Seconds60) ? TimeRange::Minutes5
                           : (settings_.history_range == TimeRange::Minutes5) ? TimeRange::Minutes15
                                                                              : TimeRange::Seconds60;
            settings_.history_range = next;
            refresh_chart_stats();
            render_now();
            if (callbacks_.on_range_changed) callbacks_.on_range_changed(next);
            break;
        }
        default:
            break;
    }
}

LRESULT CALLBACK OverlayView::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    OverlayView* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<OverlayView*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<OverlayView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->handle_message(msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT OverlayView::handle_message(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MMONI_ANIM_TICK) {
        tick_animations();
        return 0;
    }

    if (msg >= WM_APP || msg == WM_WTSSESSION_CHANGE || msg == WM_DISPLAYCHANGE ||
        (msg >= 0xC000 && msg <= 0xFFFF)) {
        if (callbacks_.on_custom_message) {
            callbacks_.on_custom_message(msg, wp, lp);
        }
        if (msg >= WM_APP) return 0;
    }

    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_NCHITTEST: {
            if (settings_.click_through) return HTTRANSPARENT;
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd_, &pt);
            const float dip_x = static_cast<float>(pt.x / placement_.dpi_scale_x);
            const float dip_y = static_cast<float>(pt.y / placement_.dpi_scale_y);
            if (!pt_in_rect(cur_chrome_rect_, dip_x, dip_y)) {
                return HTTRANSPARENT;
            }
            return HTCLIENT;
        }

        case WM_SETCURSOR: {
            if (LOWORD(lp) == HTCLIENT) {
                LPCWSTR cur = IDC_ARROW;
                switch (hovered_region_) {
                    case HitRegion::MinimalSurface:
                    case HitRegion::PeekSurface:
                    case HitRegion::DashboardHeader:
                    case HitRegion::PinButton:
                    case HitRegion::CollapseButton:
                    case HitRegion::TabTemp:
                    case HitRegion::TabPower:
                    case HitRegion::TabLoad:
                    case HitRegion::TabClock:
                    case HitRegion::RangeButton:
                        cur = IDC_HAND;
                        break;
                    case HitRegion::CpuChart:
                    case HitRegion::GpuChart:
                        cur = IDC_CROSS;
                        break;
                    default:
                        break;
                }
                SetCursor(LoadCursorW(nullptr, cur));
                return TRUE;
            }
            break;
        }

        case WM_MOUSEMOVE: {
            if (!mouse_tracked_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
                TrackMouseEvent(&tme);
                mouse_tracked_ = true;
            }

            if (drag_armed_ || dragging_) {
                POINT cur_scr{};
                GetCursorPos(&cur_scr);
                const int dx = cur_scr.x - drag_start_screen_.x;
                const int dy = cur_scr.y - drag_start_screen_.y;
                if (!dragging_) {
                    if (std::abs(dx) >= GetSystemMetrics(SM_CXDRAG) ||
                        std::abs(dy) >= GetSystemMetrics(SM_CYDRAG)) {
                        dragging_ = true;
                        drag_armed_ = false;
                        KillTimer(hwnd_, kTimerPeek);
                        KillTimer(hwnd_, kTimerCollapse);
                    }
                }
                if (dragging_) {
                    OverlayWindowHelper::move_topmost(hwnd_, drag_start_hwnd_.x + dx, drag_start_hwnd_.y + dy);
                    return 0;
                }
            }

            const float dip_x = static_cast<float>(GET_X_LPARAM(lp) / placement_.dpi_scale_x);
            const float dip_y = static_cast<float>(GET_Y_LPARAM(lp) / placement_.dpi_scale_y);
            const HitRegion reg = hit_test(dip_x, dip_y);
            const bool inside = (reg != HitRegion::None);

            bool need_redraw = false;

            if (inside && !hovered_chrome_) {
                hovered_chrome_ = true;
                KillTimer(hwnd_, kTimerCollapse);
                if (state_ == IslandState::Minimal && !dragging_ && !settings_.click_through) {
                    need_redraw = true;
                    if (settings_.auto_expand_on_hover && !suppress_hover_until_leave_) {
                        SetTimer(hwnd_, kTimerPeek, 280, nullptr);
                    }
                }
            } else if (!inside && hovered_chrome_) {
                hovered_chrome_ = false;
                KillTimer(hwnd_, kTimerPeek);
                suppress_hover_until_leave_ = false;
                if (state_ == IslandState::Minimal) need_redraw = true;
                if ((state_ == IslandState::Peek || (state_ == IslandState::Dashboard && !pinned_)) && !dragging_) {
                    SetTimer(hwnd_, kTimerCollapse, 240, nullptr);
                }
            }

            if (reg != hovered_region_) {
                hovered_region_ = reg;
                need_redraw = true;
            }

            if (state_ == IslandState::Dashboard) {
                if (reg == HitRegion::CpuChart) {
                    if (cpu_chart_.set_scrub_x(dip_x - layout_.cpu_chart.left, layout_.cpu_chart.right - layout_.cpu_chart.left)) {
                        need_redraw = true;
                    }
                    if (gpu_chart_.clear_scrub()) need_redraw = true;
                } else if (reg == HitRegion::GpuChart) {
                    if (gpu_chart_.set_scrub_x(dip_x - layout_.gpu_chart.left, layout_.gpu_chart.right - layout_.gpu_chart.left)) {
                        need_redraw = true;
                    }
                    if (cpu_chart_.clear_scrub()) need_redraw = true;
                } else {
                    if (cpu_chart_.clear_scrub()) need_redraw = true;
                    if (gpu_chart_.clear_scrub()) need_redraw = true;
                }
            }

            if (need_redraw) render_now();
            return 0;
        }

        case WM_MOUSELEAVE: {
            mouse_tracked_ = false;
            KillTimer(hwnd_, kTimerPeek);
            suppress_hover_until_leave_ = false;

            bool need_redraw = hovered_chrome_ || hovered_region_ != HitRegion::None;
            hovered_chrome_ = false;
            hovered_region_ = HitRegion::None;
            if (cpu_chart_.clear_scrub()) need_redraw = true;
            if (gpu_chart_.clear_scrub()) need_redraw = true;

            if ((state_ == IslandState::Peek || (state_ == IslandState::Dashboard && !pinned_)) && !dragging_) {
                SetTimer(hwnd_, kTimerCollapse, 240, nullptr);
            }
            if (need_redraw) render_now();
            return 0;
        }

        case WM_LBUTTONDOWN: {
            const float dip_x = static_cast<float>(GET_X_LPARAM(lp) / placement_.dpi_scale_x);
            const float dip_y = static_cast<float>(GET_Y_LPARAM(lp) / placement_.dpi_scale_y);
            const HitRegion reg = hit_test(dip_x, dip_y);
            pressed_region_ = reg;

            if (reg == HitRegion::MinimalSurface || reg == HitRegion::PeekSurface || reg == HitRegion::DashboardHeader) {
                KillTimer(hwnd_, kTimerPeek);
                KillTimer(hwnd_, kTimerCollapse);
                drag_armed_ = true;
                dragging_ = false;
                drag_origin_region_ = reg;
                GetCursorPos(&drag_start_screen_);
                drag_start_hwnd_ = OverlayWindowHelper::get_position(hwnd_);
                SetCapture(hwnd_);
            } else if (reg != HitRegion::None) {
                SetCapture(hwnd_);
            }
            render_now();
            return 0;
        }

        case WM_LBUTTONUP: {
            const bool was_dragging = dragging_;
            const HitRegion pressed = pressed_region_;
            drag_armed_ = false;
            dragging_ = false;
            pressed_region_ = HitRegion::None;
            if (GetCapture() == hwnd_) ReleaseCapture();

            if (was_dragging) {
                const POINT hwnd_pos = OverlayWindowHelper::get_position(hwnd_);
                auto [dpi_x, dpi_y] = OverlayWindowHelper::get_monitor_dpi_scale(hwnd_pos, hwnd_);
                const int chrome_left_px = hwnd_pos.x + static_cast<int>(std::llround(cur_chrome_rect_.left * dpi_x));
                const int chrome_top_px = hwnd_pos.y + static_cast<int>(std::llround(cur_chrome_rect_.top * dpi_y));
                const int compact_px_w = static_cast<int>(std::llround(minimal_width() * dpi_x));
                const int state_px_w = static_cast<int>(std::llround(get_dimensions(state_).w * dpi_x));

                anchor_ = POINT{chrome_left_px + (state_px_w - compact_px_w) / 2, chrome_top_px};
                if (state_ == IslandState::Minimal) suppress_hover_until_leave_ = true;

                place_overlay();
                anchor_ = POINT{placement_.compact_visual.left, placement_.compact_visual.top};
                if (callbacks_.on_moved && anchor_.has_value()) {
                    callbacks_.on_moved(*anchor_);
                }
                return 0;
            }

            const float dip_x = static_cast<float>(GET_X_LPARAM(lp) / placement_.dpi_scale_x);
            const float dip_y = static_cast<float>(GET_Y_LPARAM(lp) / placement_.dpi_scale_y);
            const HitRegion released = hit_test(dip_x, dip_y);
            if (released != HitRegion::None && released == pressed) {
                handle_click(released);
            } else {
                render_now();
            }
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            const float dip_x = static_cast<float>(GET_X_LPARAM(lp) / placement_.dpi_scale_x);
            const float dip_y = static_cast<float>(GET_Y_LPARAM(lp) / placement_.dpi_scale_y);
            const HitRegion reg = hit_test(dip_x, dip_y);
            if (reg == HitRegion::CpuCard || reg == HitRegion::GpuCard ||
                reg == HitRegion::CpuChart || reg == HitRegion::GpuChart) {
                reset_peaks();
                return 0;
            }
            break;
        }

        case WM_TIMER: {
            if (wp == kTimerPeek) {
                KillTimer(hwnd_, kTimerPeek);
                if (settings_.auto_expand_on_hover && state_ == IslandState::Minimal &&
                    !dragging_ && !settings_.click_through && hovered_chrome_) {
                    transition_to(IslandState::Peek);
                }
                return 0;
            }
            if (wp == kTimerCollapse) {
                KillTimer(hwnd_, kTimerCollapse);
                if (!dragging_ && !hovered_chrome_ &&
                    (state_ == IslandState::Peek || (state_ == IslandState::Dashboard && !pinned_))) {
                    transition_to(IslandState::Minimal);
                }
                return 0;
            }
            if (wp == kTimerAnim) {
                tick_animations();
                return 0;
            }
            break;
        }

        case WM_DPICHANGED: {
            if (!dragging_) {
                place_overlay();
            }
            return 0;
        }

        case WM_CLOSE:
            return 0;
    }

    return DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace mmoni
