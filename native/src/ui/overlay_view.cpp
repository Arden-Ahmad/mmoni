#include "ui/overlay_view.h"
#include "platform/single_instance.h"

#include <dcomp.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <system_error>

namespace mmoni {

namespace {

constexpr wchar_t kOverlayClassName[] = L"MmoniNativeOverlayWindow";

constexpr double kPeekWidth = 320.0;
constexpr double kPeekHeight = 74.0;
constexpr double kDashboardWidth = 368.0;
constexpr double kDashboardBaseHeight = 262.0;
constexpr double kDashboardBannerExtraHeight = 28.0;
constexpr double kStageMaxHeight = kDashboardBaseHeight + kDashboardBannerExtraHeight;
// Preserve text contrast over bright windows while click-through is active.
constexpr float kClickThroughOpacity = 0.97f;

constexpr UINT_PTR kTimerHover = 1;
constexpr UINT_PTR kTimerAnim = 3;
constexpr UINT_PTR kTimerPresence = 4;
constexpr UINT_PTR kTimerOverlapDebounce = 5;
constexpr DWORD kPeekDelayMs = 280;
constexpr DWORD kCollapseDelayMs = 240;
constexpr DWORD kFallbackFrameMs = 16;
constexpr DWORD kCompositorWaitMs = 34;
constexpr DWORD kPresencePollMs = 45;
constexpr DWORD kOverlapDebounceMs = 80;
constexpr double kMaxAnimationGapMs = 1000.0 / 30.0;

constexpr double kMicroBarWidth = 124.0;
constexpr double kMicroBarHeight = 3.2;
constexpr double kMicroBarRadius = 1.6;
constexpr float kGhostOpacity = 0.12f;
constexpr double kDodgePadX = 10.0;
constexpr double kDodgePadY = 8.0;
constexpr double kUnhidePadX = 18.0;
constexpr double kUnhidePadY = 16.0;
constexpr double kDodgeReturnDelayMs = 120.0;
constexpr double kTopEdgeDwellMs = 220.0;

OverlayView* g_active_overlay_view = nullptr;

float smoothstepf(float edge0, float edge1, float x) noexcept {
    const float t = std::clamp((x - edge0) / std::max(1e-5f, edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

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
        if (!vsync_wake_event_) {
            SetTimer(hwnd_, kTimerAnim, kFallbackFrameMs, nullptr);
            return;
        }
    }
    if (!vsync_thread_.joinable()) {
        vsync_stop_.store(false, std::memory_order_release);
        try {
            vsync_thread_ = std::thread(&OverlayView::vsync_worker_loop, this);
        } catch (const std::system_error&) {
            CloseHandle(vsync_wake_event_);
            vsync_wake_event_ = nullptr;
            SetTimer(hwnd_, kTimerAnim, kFallbackFrameMs, nullptr);
            return;
        }
        KillTimer(hwnd_, kTimerAnim);
    }
    vsync_active_.store(true, std::memory_order_release);
    SetEvent(vsync_wake_event_);
}

void OverlayView::stop_vsync_pacer() {
    vsync_active_.store(false, std::memory_order_release);
    if (hwnd_ && !vsync_wake_event_) KillTimer(hwnd_, kTimerAnim);
}

void OverlayView::vsync_worker_loop() {
    while (!vsync_stop_.load(std::memory_order_acquire)) {
        if (!vsync_active_.load(std::memory_order_acquire)) {
            WaitForSingleObject(vsync_wake_event_, INFINITE);
            continue;
        }

        // Wait for a frame without waiting for our drawing surface updates to be presented.
        const double start = now_ms();
        if (DCompositionWaitForCompositorClock(0, nullptr, kCompositorWaitMs) != 0) {
            const double remaining = kFallbackFrameMs - (now_ms() - start);
            if (remaining > 0.0) Sleep(static_cast<DWORD>(std::ceil(remaining)));
        }

        if (vsync_stop_.load(std::memory_order_acquire) || !vsync_active_.load(std::memory_order_acquire)) {
            continue;
        }

        // Coalesce pending tick messages so the UI queue is never flooded.
        bool expected = false;
        if (anim_tick_pending_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            if (!PostMessageW(hwnd_, WM_MMONI_ANIM_TICK, 0, 0)) {
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
        default: return is_glass_theme(settings_.theme) ? 228.0 : 224.0;
    }
}

double OverlayView::minimal_height() const noexcept {
    return is_glass_theme(settings_.theme) ? 28.0 : 26.0;
}

double OverlayView::minimal_radius() const noexcept {
    return minimal_height() * 0.5;
}

double OverlayView::peek_radius() const noexcept {
    switch (settings_.theme) {
        case VisualTheme::PrecisionDark: return 11.0;
        case VisualTheme::FluentAcrylic: return 15.0;
        default: return 18.0;
    }
}

double OverlayView::dashboard_radius() const noexcept {
    switch (settings_.theme) {
        case VisualTheme::PrecisionDark: return 12.0;
        case VisualTheme::FluentAcrylic: return 16.0;
        default: return 20.0;
    }
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

    const double base_top = placement_.target_offset_y;
    const double max_top = base_top + std::max(0.0, kStageMaxHeight - d.h);
    const double top = std::clamp(placement_.compact_offset_y, base_top, max_top);

    return {
        static_cast<float>(left),
        static_cast<float>(top),
        static_cast<float>(left + d.w),
        static_cast<float>(top + d.h)
    };
}

D2D1_RECT_F OverlayView::get_micro_bar_rect() const noexcept {
    const double compact_cx = placement_.compact_offset_x + minimal_width() * 0.5;
    const double max_left = OverlayWindowHelper::kShadowPadLeft + std::max(0.0, kDashboardWidth - kMicroBarWidth);
    const double left = std::clamp(compact_cx - kMicroBarWidth * 0.5, OverlayWindowHelper::kShadowPadLeft, max_left);
    const double top = placement_.top_edge_offset_y;
    return {
        static_cast<float>(left),
        static_cast<float>(top),
        static_cast<float>(left + kMicroBarWidth),
        static_cast<float>(top + kMicroBarHeight)
    };
}

bool OverlayView::effective_click_through() const noexcept {
    return settings_.click_through || presence_retracted_ || reveal_input_pending_;
}

void OverlayView::sync_compositor_opacity() {
    if (!compositor_.is_ready()) return;
    const float base = settings_.click_through ? kClickThroughOpacity : 1.0f;
    const float target = (!placement_.near_top_edge && cur_retract_ > 0.0005f)
        ? lerpf(base, kGhostOpacity, cur_retract_)
        : base;
    if (std::fabs(target - last_compositor_opacity_) > 0.001f) {
        last_compositor_opacity_ = target;
        compositor_.set_opacity(target);
    }
}

void CALLBACK OverlayView::on_win_event(
    HWINEVENTHOOK,
    DWORD,
    HWND hwnd,
    LONG id_object,
    LONG id_child,
    DWORD,
    DWORD) {
    if (!g_active_overlay_view) return;
    if (id_object != OBJID_WINDOW || id_child != CHILDID_SELF) return;
    if (hwnd && hwnd == g_active_overlay_view->hwnd_) return;
    g_active_overlay_view->schedule_overlap_refresh();
}

void OverlayView::install_win_hooks() {
    if (!hwnd_ || !visible_ || settings_.presence_mode == PresenceMode::AlwaysVisible) return;
    constexpr DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    if (!win_hooks_[0]) {
        win_hooks_[0] = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, &OverlayView::on_win_event, 0, 0, flags);
    }
    if (!win_hooks_[1]) {
        win_hooks_[1] = SetWinEventHook(EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND, nullptr, &OverlayView::on_win_event, 0, 0, flags);
    }
    if (!win_hooks_[2]) {
        win_hooks_[2] = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, &OverlayView::on_win_event, 0, 0, flags);
    }
}

void OverlayView::remove_win_hooks() {
    for (auto& hook : win_hooks_) {
        if (hook) {
            UnhookWinEvent(hook);
            hook = nullptr;
        }
    }
    if (overlap_debounce_active_ && hwnd_) {
        KillTimer(hwnd_, kTimerOverlapDebounce);
        overlap_debounce_active_ = false;
    }
}

void OverlayView::schedule_overlap_refresh() {
    if (!hwnd_ || !visible_ || settings_.presence_mode == PresenceMode::AlwaysVisible) return;
    if (!overlap_debounce_active_) {
        overlap_debounce_active_ = SetTimer(hwnd_, kTimerOverlapDebounce, kOverlapDebounceMs, nullptr) != 0;
    }
}

void OverlayView::refresh_window_overlap(bool immediate_retract) {
    if (!hwnd_ || !visible_ || settings_.presence_mode == PresenceMode::AlwaysVisible) {
        has_overlap_ = false;
        sync_presence_timer();
        if (presence_retracted_) set_retracted(false, immediate_retract);
        return;
    }

    const bool next_overlap = OverlayWindowHelper::has_overlapping_window(hwnd_, placement_.compact_visual);
    const bool changed = (next_overlap != has_overlap_);
    has_overlap_ = next_overlap;
    sync_presence_timer();

    if (immediate_retract && has_overlap_ && settings_.presence_mode == PresenceMode::AutoRetract &&
        state_ == IslandState::Minimal && !presence_summoned_ && !dragging_ && !drag_armed_) {
        set_retracted(true, true);
        return;
    }

    if (changed || !has_overlap_) {
        evaluate_presence();
    }
}

void OverlayView::sync_presence_timer() {
    if (!hwnd_) return;
    const bool want = visible_ && settings_.presence_mode != PresenceMode::AlwaysVisible &&
                      (has_overlap_ || presence_retracted_ || presence_summoned_);
    if (want && !presence_timer_active_) {
        SetTimer(hwnd_, kTimerPresence, kPresencePollMs, nullptr);
        presence_timer_active_ = true;
    } else if (!want && presence_timer_active_) {
        KillTimer(hwnd_, kTimerPresence);
        presence_timer_active_ = false;
    }
}

void OverlayView::set_retracted(bool retracted, bool immediate) {
    if (retracted && (state_ != IslandState::Minimal || dragging_ || drag_armed_)) {
        retracted = false;
    }
    if (presence_retracted_ == retracted && !immediate) return;

    if (!retracted && presence_retracted_) reveal_input_pending_ = true;
    presence_retracted_ = retracted;
    if (presence_retracted_) {
        reveal_input_pending_ = false;
        cancel_hover();
        hovered_chrome_ = false;
        hovered_region_ = HitRegion::None;
        pressed_region_ = HitRegion::None;
        mouse_tracked_ = false;
    }

    sync_overlay_input();

    const float target = presence_retracted_ ? 1.0f : 0.0f;
    if (immediate || !hwnd_ || cur_retract_ == target) {
        retract_animating_ = false;
        cur_retract_ = target;
        sync_compositor_opacity();
        if (!morph_animating_ && !thumb_animating_) stop_vsync_pacer();
        render_now();
        return;
    }

    retract_from_ = cur_retract_;
    retract_to_ = target;
    retract_start_ms_ = now_ms();
    retract_duration_ms_ = presence_retracted_ ? 155.0 : 210.0;
    retract_animating_ = true;
    start_vsync_pacer();
    render_now();
}

void OverlayView::evaluate_presence() {
    if (!visible_ || !hwnd_) return;

    const PresenceMode mode = settings_.presence_mode;
    if (mode == PresenceMode::AlwaysVisible || !has_overlap_) {
        presence_summoned_ = false;
        top_dwell_start_ms_ = 0.0;
        dodge_leave_ms_ = 0.0;
        if (presence_retracted_) set_retracted(false);
        sync_presence_timer();
        return;
    }

    if (dragging_ || drag_armed_ || state_ != IslandState::Minimal) {
        top_dwell_start_ms_ = 0.0;
        dodge_leave_ms_ = 0.0;
        if (presence_retracted_) set_retracted(false);
        return;
    }

    POINT pt{};
    if (!GetCursorPos(&pt)) return;

    const double dx = placement_.dpi_scale_x > 0.0 ? placement_.dpi_scale_x : 1.0;
    const double dy = placement_.dpi_scale_y > 0.0 ? placement_.dpi_scale_y : 1.0;
    const RECT& cv = placement_.compact_visual;

    const int prox_x = static_cast<int>(std::lround(kDodgePadX * dx));
    const int prox_y = static_cast<int>(std::lround(kDodgePadY * dy));
    const int hyst_x = static_cast<int>(std::lround(kUnhidePadX * dx));
    const int hyst_y = static_cast<int>(std::lround(kUnhidePadY * dy));

    const auto padded_top = [&](LONG pad) {
        const LONG top = cv.top - pad;
        return placement_.near_top_edge ? std::min(top, placement_.monitor_work.top) : top;
    };
    const bool in_prox =
        pt.x >= cv.left - prox_x && pt.x <= cv.right + prox_x &&
        pt.y >= padded_top(prox_y) && pt.y <= cv.bottom + prox_y;
    const bool in_hyst =
        pt.x >= cv.left - hyst_x && pt.x <= cv.right + hyst_x &&
        pt.y >= padded_top(hyst_y) && pt.y <= cv.bottom + hyst_y;

    const bool alt_down = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool lbtn_down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const double now = now_ms();

    // Top-edge dwell summon: cursor pinned to y <= monitor_work.top + 1 over the micro-bar (±12 DIPs) with no drag.
    bool top_dwell_triggered = false;
    if (placement_.near_top_edge && presence_retracted_ && !lbtn_down) {
        const D2D1_RECT_F bar = get_micro_bar_rect();
        const int pad_x = static_cast<int>(std::lround(12.0 * dx));
        const int bar_left_px = placement_.stage_window.left + static_cast<int>(std::floor(bar.left * dx)) - pad_x;
        const int bar_right_px = placement_.stage_window.left + static_cast<int>(std::ceil(bar.right * dx)) + pad_x;
        const bool on_top_bezel = (pt.y <= placement_.monitor_work.top + 1) &&
                                  (pt.y >= placement_.monitor_work.top - 2) &&
                                  (pt.x >= bar_left_px) && (pt.x <= bar_right_px);
        if (on_top_bezel) {
            if (top_dwell_start_ms_ <= 0.0) {
                top_dwell_start_ms_ = now;
            } else if (now - top_dwell_start_ms_ >= kTopEdgeDwellMs) {
                top_dwell_triggered = true;
            }
        } else {
            top_dwell_start_ms_ = 0.0;
        }
    } else {
        top_dwell_start_ms_ = 0.0;
    }

    if (mode == PresenceMode::SmartDodge) {
        if (presence_summoned_) {
            if (!in_hyst && !alt_down) {
                presence_summoned_ = false;
            }
            if (presence_retracted_) set_retracted(false);
            return;
        }

        if ((alt_down && in_hyst) || top_dwell_triggered) {
            presence_summoned_ = true;
            dodge_leave_ms_ = 0.0;
            top_dwell_start_ms_ = 0.0;
            set_retracted(false);
            return;
        }

        if (!presence_retracted_) {
            if (in_prox) {
                dodge_leave_ms_ = 0.0;
                set_retracted(true);
            }
        } else {
            if (in_hyst) {
                dodge_leave_ms_ = 0.0;
            } else {
                if (dodge_leave_ms_ <= 0.0) {
                    dodge_leave_ms_ = now;
                } else if (now - dodge_leave_ms_ >= kDodgeReturnDelayMs) {
                    dodge_leave_ms_ = 0.0;
                    set_retracted(false);
                }
            }
        }
        return;
    }

    // PresenceMode::AutoRetract
    if (presence_summoned_) {
        if (!in_hyst && !alt_down) {
            presence_summoned_ = false;
            set_retracted(true);
        } else if (presence_retracted_) {
            set_retracted(false);
        }
        return;
    }

    if ((alt_down && in_hyst) || top_dwell_triggered) {
        presence_summoned_ = true;
        top_dwell_start_ms_ = 0.0;
        set_retracted(false);
        return;
    }

    if (!presence_retracted_) {
        set_retracted(true);
    }
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
        ex_style |= WS_EX_LAYERED | WS_EX_TRANSPARENT;
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

    g_active_overlay_view = this;
    hover_timer_ = CreateThreadpoolTimer(&OverlayView::on_hover_timer, this, nullptr);
    last_compositor_opacity_ = -1.0f;
    sync_compositor_opacity();
    place_overlay();
    return true;
}

void OverlayView::ensure_topmost() {
    if (!hwnd_ || !visible_) return;
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void OverlayView::sync_input_region(const D2D1_RECT_F& rect_dip, float radius_dip) {
    if (!hwnd_) return;
    if (effective_click_through()) {
        clear_input_region();
        return;
    }
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

void OverlayView::clear_input_region() {
    if (!hwnd_) return;
    if (last_rgn_rad_ == -2) return;
    if (SetWindowRgn(hwnd_, nullptr, FALSE) != 0) {
        last_rgn_rect_ = {};
        last_rgn_rad_ = -2;
    }
}

void OverlayView::sync_overlay_input() {
    if (!hwnd_) return;
    if (effective_click_through()) {
        OverlayWindowHelper::set_click_through(hwnd_, true);
        clear_input_region();
        return;
    }
    if (morph_animating_ && morph_expanding_) {
        sync_input_region(morph_to_rect_, morph_to_r_);
    } else {
        sync_input_region(cur_chrome_rect_, cur_radius_);
    }
    OverlayWindowHelper::set_click_through(hwnd_, false);
}

bool OverlayView::recover_graphics_device(bool force) {
    const uint64_t now = GetTickCount64();
    if (!force && last_recover_ms_ != 0 && now - last_recover_ms_ < 1000ULL) {
        return false;
    }
    last_recover_ms_ = now;
    if (!hwnd_) return false;

    compositor_.shutdown();
    d2d_.reset();
    if (!d2d_.init() || !renderer_.init(&d2d_) || !compositor_.init(hwnd_, &d2d_)) {
        return false;
    }

    last_compositor_opacity_ = -1.0f;
    sync_compositor_opacity();
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

void OverlayView::handle_display_change() {
    if (!hwnd_) return;
    if (d2d_.preferred_adapter_changed()) {
        recover_graphics_device(true);
    }
    if (visible_) {
        place_overlay();
        if (settings_.presence_mode != PresenceMode::AlwaysVisible) {
            refresh_window_overlap();
        }
    }
}

void OverlayView::destroy() {
    remove_win_hooks();
    if (presence_timer_active_ && hwnd_) {
        KillTimer(hwnd_, kTimerPresence);
        presence_timer_active_ = false;
    }
    if (g_active_overlay_view == this) {
        g_active_overlay_view = nullptr;
    }

    cancel_hover();
    if (hover_timer_) {
        WaitForThreadpoolTimerCallbacks(hover_timer_, TRUE);
        CloseThreadpoolTimer(hover_timer_);
        hover_timer_ = nullptr;
    }

    vsync_stop_.store(true, std::memory_order_release);
    vsync_active_.store(false, std::memory_order_release);
    if (vsync_wake_event_) SetEvent(vsync_wake_event_);
    if (vsync_thread_.joinable()) vsync_thread_.join();
    anim_tick_pending_.store(false, std::memory_order_release);
    if (vsync_wake_event_) {
        CloseHandle(vsync_wake_event_);
        vsync_wake_event_ = nullptr;
    }

    if (hwnd_) {
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
    install_win_hooks();
    refresh_window_overlap(true);
    sync_presence_timer();
    if (collapse_on_leave()) {
        if (cursor_over_chrome()) track_mouse_leave();
        else schedule_hover(HoverAction::Collapse);
    }
}

void OverlayView::hide() {
    if (!hwnd_) return;
    visible_ = false;
    remove_win_hooks();
    if (presence_timer_active_) {
        KillTimer(hwnd_, kTimerPresence);
        presence_timer_active_ = false;
    }
    cancel_hover();
    mouse_tracked_ = false;
    hovered_chrome_ = false;
    hovered_region_ = HitRegion::None;
    suppress_hover_until_leave_ = false;
    presence_summoned_ = false;
    dodge_leave_ms_ = 0.0;
    top_dwell_start_ms_ = 0.0;
    morph_animating_ = false;
    thumb_animating_ = false;
    retract_animating_ = false;
    stop_vsync_pacer();
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

    morph_animating_ = false;
    retract_animating_ = false;
    cur_retract_ = presence_retracted_ ? 1.0f : 0.0f;
    if (!thumb_animating_) stop_vsync_pacer();
    cur_chrome_rect_ = get_chrome_rect(state_);
    cur_radius_ = static_cast<float>(get_dimensions(state_).r);
    cur_op_[0] = (state_ == IslandState::Minimal) ? 1.0f : 0.0f;
    cur_op_[1] = (state_ == IslandState::Peek) ? 1.0f : 0.0f;
    cur_op_[2] = (state_ == IslandState::Dashboard) ? 1.0f : 0.0f;
    cur_dash_slide_y_ = 0.0f;

    sync_compositor_opacity();
    sync_overlay_input();
    render_now();
}

void OverlayView::apply_settings(const UserSettings& updated) {
    const bool theme_changed = settings_.theme != updated.theme;
    const bool collapsed_changed = settings_.collapsed_mode != updated.collapsed_mode;
    const bool click_changed = settings_.click_through != updated.click_through;
    const bool auto_exp_changed = settings_.auto_expand_on_hover != updated.auto_expand_on_hover;
    const bool range_changed = settings_.history_range != updated.history_range;
    const bool presence_changed = settings_.presence_mode != updated.presence_mode;

    settings_ = updated;

    if (click_changed) {
        cancel_hover();
        if (settings_.click_through && state_ != IslandState::Minimal) {
            pinned_ = false;
            transition_to(IslandState::Minimal);
        }
        sync_compositor_opacity();
        sync_overlay_input();
    }

    if (auto_exp_changed && !settings_.auto_expand_on_hover) {
        if (hover_action_ == HoverAction::Peek) cancel_hover();
        if (state_ == IslandState::Peek) transition_to(IslandState::Minimal);
    }

    if (presence_changed) {
        presence_summoned_ = false;
        dodge_leave_ms_ = 0.0;
        top_dwell_start_ms_ = 0.0;
        if (settings_.presence_mode == PresenceMode::AlwaysVisible) {
            remove_win_hooks();
            has_overlap_ = false;
            sync_presence_timer();
            set_retracted(false);
        } else if (visible_) {
            install_win_hooks();
            refresh_window_overlap();
            evaluate_presence();
        }
    }

    if (range_changed || theme_changed) {
        refresh_chart_stats();
    }

    if (theme_changed || collapsed_changed) {
        place_overlay();
        if (visible_ && settings_.presence_mode != PresenceMode::AlwaysVisible) {
            refresh_window_overlap();
        }
    } else {
        render_now();
    }
}

void OverlayView::reset_position() {
    anchor_.reset();
    presence_summoned_ = false;
    dodge_leave_ms_ = 0.0;
    top_dwell_start_ms_ = 0.0;
    if (state_ != IslandState::Minimal) {
        pinned_ = false;
        transition_to(IslandState::Minimal, true);
    }
    set_retracted(false, true);
    place_overlay();
    if (visible_ && settings_.presence_mode != PresenceMode::AlwaysVisible) {
        refresh_window_overlap();
    }
}

void OverlayView::reset_peaks() {
    cpu_history_.reset_peaks();
    gpu_history_.reset_peaks();
    if (state_ == IslandState::Dashboard) {
        refresh_chart_stats();
        render_now();
    }
}

OverlayView::MinimalVisualKey OverlayView::minimal_visual_key() const noexcept {
    MinimalVisualKey key{};
    key.micro_bar = placement_.near_top_edge && presence_retracted_;
    const auto* cpu = snapshot_ && snapshot_->cpu ? &*snapshot_->cpu : nullptr;
    const auto* gpu = snapshot_ && snapshot_->gpu ? &*snapshot_->gpu : nullptr;
    key.gpu_sleeping = gpu && gpu->is_sleeping;
    if (key.gpu_sleeping) gpu = nullptr;

    const auto temperature = [micro_bar = key.micro_bar](const ProcessorReading* sensor) -> std::optional<float> {
        if (!sensor || !sensor->temperature || !std::isfinite(*sensor->temperature)) return std::nullopt;
        const float value = *sensor->temperature;
        return micro_bar ? value : std::round(value);
    };
    key.cpu_temperature = temperature(cpu);
    key.gpu_temperature = temperature(gpu);

    if (!key.micro_bar && settings_.collapsed_mode != CollapsedDisplayMode::TempOnly) {
        const bool show_load = settings_.collapsed_mode == CollapsedDisplayMode::TempLoad;
        const Metric metric = show_load ? Metric::Load : Metric::Power;
        const float scale = show_load ? 1.0f : 10.0f;
        key.cpu_secondary = quantize_opt(cpu ? cpu->get(metric) : std::nullopt, scale);
        key.gpu_secondary = quantize_opt(gpu ? gpu->get(metric) : std::nullopt, scale);
    }
    return key;
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

    if (settings_.presence_mode != PresenceMode::AlwaysVisible && !dragging_ && !drag_armed_) {
        refresh_window_overlap();
    }

    if (state_ == IslandState::Minimal && !morph_animating_ && !retract_animating_ &&
        last_minimal_visual_ == minimal_visual_key()) {
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

void OverlayView::track_mouse_leave() {
    if (mouse_tracked_) return;
    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
    mouse_tracked_ = TrackMouseEvent(&tme) != FALSE;
}

bool OverlayView::cursor_over_chrome() const {
    POINT pt{};
    if (!visible_ || effective_click_through() || !GetCursorPos(&pt) || WindowFromPoint(pt) != hwnd_) {
        return false;
    }
    ScreenToClient(hwnd_, &pt);
    return pt_in_rect(cur_chrome_rect_, static_cast<float>(pt.x / placement_.dpi_scale_x),
                      static_cast<float>(pt.y / placement_.dpi_scale_y));
}

bool OverlayView::collapse_on_leave() const noexcept {
    return state_ == IslandState::Peek || (state_ == IslandState::Dashboard && !pinned_);
}

void OverlayView::schedule_hover(HoverAction action) {
    if (!visible_ || hover_action_ == action) return;
    const DWORD delay_ms = action == HoverAction::Peek ? kPeekDelayMs : kCollapseDelayMs;
    hover_action_ = action;
    hover_deadline_ms_ = now_ms() + delay_ms;
    arm_hover_timer(delay_ms);
}

void OverlayView::arm_hover_timer(DWORD delay_ms) {
    if (hover_timer_) {
        // A negative FILETIME is relative and uses 100 ns units.
        const auto due = static_cast<ULONGLONG>(-10'000LL * delay_ms);
        FILETIME due_time{static_cast<DWORD>(due), static_cast<DWORD>(due >> 32)};
        SetThreadpoolTimer(hover_timer_, &due_time, 0, 0);
    } else {
        SetTimer(hwnd_, kTimerHover, delay_ms, nullptr);
    }
}

void OverlayView::cancel_hover() {
    if (hover_action_ == HoverAction::None) return;
    hover_action_ = HoverAction::None;
    if (hover_timer_) {
        SetThreadpoolTimer(hover_timer_, nullptr, 0, 0);
    } else {
        KillTimer(hwnd_, kTimerHover);
    }
}

void CALLBACK OverlayView::on_hover_timer(PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER) {
    const auto* self = static_cast<const OverlayView*>(context);
    PostMessageW(self->hwnd_, WM_MMONI_HOVER_TIMER, 0, 0);
}

void OverlayView::handle_hover_timer() {
    const HoverAction action = hover_action_;
    if (action == HoverAction::None) return;

    // A message from the previous hover can still be queued after the timer is rearmed.
    const double remaining = hover_deadline_ms_ - now_ms();
    if (remaining > 0.0) {
        arm_hover_timer(static_cast<DWORD>(std::ceil(remaining)));
        return;
    }
    cancel_hover();

    if (!visible_ || effective_click_through() || drag_armed_ || dragging_) return;
    const bool cursor_inside = cursor_over_chrome();
    if (action == HoverAction::Peek) {
        if (state_ == IslandState::Minimal && settings_.auto_expand_on_hover &&
            !suppress_hover_until_leave_ && cursor_inside) {
            transition_to(IslandState::Peek);
        }
    } else if (!cursor_inside && collapse_on_leave()) {
        transition_to(IslandState::Minimal);
    }
}

void OverlayView::transition_to(IslandState next, bool immediate) {
    if (state_ == next && !immediate) return;
    cancel_hover();

    if (next != IslandState::Minimal && (presence_retracted_ || retract_animating_ || cur_retract_ > 0.0f)) {
        presence_retracted_ = false;
        reveal_input_pending_ = true;
        retract_animating_ = false;
        cur_retract_ = 0.0f;
        sync_compositor_opacity();
    }

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
        if (!thumb_animating_ && !retract_animating_) stop_vsync_pacer();
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

    const bool is_glass = is_glass_theme(settings_.theme);
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
    if (!morph_animating_ && !thumb_animating_ && !retract_animating_) {
        stop_vsync_pacer();
        return;
    }

    const double now = now_ms();
    const bool is_glass = is_glass_theme(settings_.theme);
    // Keep some motion after a stalled frame, including the first frame of a transition.
    const double frame_floor = now - kMaxAnimationGapMs;
    if (morph_animating_) {
        morph_start_ms_ += std::max(0.0, frame_floor - std::max(morph_start_ms_, last_animation_frame_ms_));
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
        thumb_start_ms_ += std::max(0.0, frame_floor - std::max(thumb_start_ms_, last_animation_frame_ms_));
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

    if (retract_animating_) {
        retract_start_ms_ += std::max(0.0, frame_floor - std::max(retract_start_ms_, last_animation_frame_ms_));
        const float raw_t = std::clamp(
            static_cast<float>((now - retract_start_ms_) / std::max(1.0, retract_duration_ms_)),
            0.0f, 1.0f);
        const float e = cubic_ease_out(raw_t);
        cur_retract_ = std::clamp(lerpf(retract_from_, retract_to_, e), 0.0f, 1.0f);
        if (raw_t >= 1.0f) {
            retract_animating_ = false;
            cur_retract_ = retract_to_;
        }
        sync_compositor_opacity();
    }

    if (!morph_animating_ && !thumb_animating_ && !retract_animating_) {
        stop_vsync_pacer();
    }

    last_animation_frame_ms_ = now;
    render_now();
}

void OverlayView::render_now() {
    if (!compositor_.is_ready()) {
        last_minimal_visual_.reset();
        return;
    }

    D2D1_RECT_F draw_rect = cur_chrome_rect_;
    float draw_radius = cur_radius_;
    const bool top_bar_mode = placement_.near_top_edge && cur_retract_ > 0.0005f;
    if (top_bar_mode) {
        const D2D1_RECT_F bar = get_micro_bar_rect();
        draw_rect = {
            lerpf(cur_chrome_rect_.left, bar.left, cur_retract_),
            lerpf(cur_chrome_rect_.top, bar.top, cur_retract_),
            lerpf(cur_chrome_rect_.right, bar.right, cur_retract_),
            lerpf(cur_chrome_rect_.bottom, bar.bottom, cur_retract_)
        };
        draw_radius = std::max(1.0f, lerpf(cur_radius_, static_cast<float>(kMicroBarRadius), cur_retract_));
    }

    const float dx = static_cast<float>(placement_.dpi_scale_x);
    const float dy = static_cast<float>(placement_.dpi_scale_y);
    const bool show_backdrop = visible_ && is_glass_theme(settings_.theme) &&
                               (!placement_.near_top_edge || cur_retract_ < 0.72f);
    compositor_.update_backdrop(
        show_backdrop,
        settings_.theme,
        dx,
        draw_rect.left * dx,
        draw_rect.top * dy,
        (draw_rect.right - draw_rect.left) * dx,
        (draw_rect.bottom - draw_rect.top) * dy,
        draw_radius * dx);

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
    st.chrome_rect = draw_rect;
    st.corner_radius = draw_radius;
    st.minimal_opacity = top_bar_mode
        ? (cur_op_[0] * (1.0f - smoothstepf(0.0f, 0.50f, cur_retract_)))
        : cur_op_[0];
    st.peek_opacity = cur_op_[1];
    st.dashboard_opacity = cur_op_[2];
    st.dashboard_slide_y = cur_dash_slide_y_;
    st.seg_thumb_pos = cur_thumb_pos_;
    st.retract_progress = cur_retract_;
    st.retract_is_top_bar = placement_.near_top_edge;
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
            settings_.theme,
            dx,
            draw_rect.left * dx,
            draw_rect.top * dy,
            (draw_rect.right - draw_rect.left) * dx,
            (draw_rect.bottom - draw_rect.top) * dy,
            draw_radius * dx);
        status = compositor_.render([&](ID2D1DeviceContext* dc, float, float) {
            renderer_.render(dc, st, layout_);
        });
    }

    if (status == RenderStatus::Ok && state_ == IslandState::Minimal && !morph_animating_ && !retract_animating_) {
        last_minimal_visual_ = minimal_visual_key();
    } else {
        last_minimal_visual_.reset();
    }
    // Keep the full stage click-through until the restored pill frame has been submitted.
    if (status == RenderStatus::Ok && reveal_input_pending_ && !retract_animating_ && cur_retract_ == 0.0f) {
        reveal_input_pending_ = false;
        sync_overlay_input();
        if (state_ == IslandState::Minimal && cursor_over_chrome()) {
            track_mouse_leave();
            if (settings_.auto_expand_on_hover && !suppress_hover_until_leave_) {
                schedule_hover(HoverAction::Peek);
            }
        }
    }
}

HitRegion OverlayView::hit_test(float x, float y) const noexcept {
    if (effective_click_through() || !pt_in_rect(cur_chrome_rect_, x, y)) {
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
        thumb_duration_ms_ = is_glass_theme(settings_.theme) ? 230.0 : 160.0;
        thumb_animating_ = true;
        refresh_chart_stats();
        start_vsync_pacer();
        render_now();
    };

    switch (region) {
        case HitRegion::MinimalSurface:
        case HitRegion::PeekSurface:
            pinned_ = true;
            transition_to(IslandState::Dashboard);
            break;
        case HitRegion::DashboardHeader:
        case HitRegion::CollapseButton:
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
        anim_tick_pending_.store(false, std::memory_order_release);
        tick_animations();
        return 0;
    }
    if (msg == WM_MMONI_HOVER_TIMER) {
        handle_hover_timer();
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
            if (effective_click_through()) return HTTRANSPARENT;
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
            if (effective_click_through()) return 0;
            if (!drag_armed_ && !dragging_ && state_ == IslandState::Minimal &&
                settings_.presence_mode != PresenceMode::AlwaysVisible && !presence_summoned_) {
                const bool alt_down = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                if (alt_down) {
                    presence_summoned_ = true;
                } else {
                    if (has_overlap_) {
                        dodge_leave_ms_ = 0.0;
                        set_retracted(true);
                        return 0;
                    }
                    if (!hovered_chrome_) schedule_overlap_refresh();
                }
            }

            track_mouse_leave();

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
                        cancel_hover();
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
                cancel_hover();
                if (state_ == IslandState::Minimal && !dragging_ && !settings_.click_through) {
                    need_redraw = true;
                    if (settings_.auto_expand_on_hover && !suppress_hover_until_leave_) {
                        schedule_hover(HoverAction::Peek);
                    }
                }
            } else if (!inside && hovered_chrome_) {
                hovered_chrome_ = false;
                if (hover_action_ == HoverAction::Peek) cancel_hover();
                suppress_hover_until_leave_ = false;
                if (state_ == IslandState::Minimal) need_redraw = true;
                if (!dragging_ && collapse_on_leave()) {
                    schedule_hover(HoverAction::Collapse);
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
            if (hovered_chrome_ && cursor_over_chrome()) {
                track_mouse_leave();
                return 0;
            }
            if (hover_action_ == HoverAction::Peek) cancel_hover();
            suppress_hover_until_leave_ = false;

            bool need_redraw = hovered_chrome_ || hovered_region_ != HitRegion::None;
            hovered_chrome_ = false;
            hovered_region_ = HitRegion::None;
            if (cpu_chart_.clear_scrub()) need_redraw = true;
            if (gpu_chart_.clear_scrub()) need_redraw = true;

            if (!dragging_ && collapse_on_leave()) {
                schedule_hover(HoverAction::Collapse);
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
                cancel_hover();
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
                if (settings_.presence_mode != PresenceMode::AlwaysVisible) {
                    presence_summoned_ = true;
                    refresh_window_overlap();
                }
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
            if (wp == kTimerHover) {
                handle_hover_timer();
                return 0;
            }
            if (wp == kTimerAnim) {
                tick_animations();
                return 0;
            }
            if (wp == kTimerPresence) {
                evaluate_presence();
                return 0;
            }
            if (wp == kTimerOverlapDebounce) {
                if (!overlap_debounce_active_) return 0;
                KillTimer(hwnd_, kTimerOverlapDebounce);
                overlap_debounce_active_ = false;
                refresh_window_overlap();
                return 0;
            }
            break;
        }

        case WM_DPICHANGED: {
            if (!dragging_) {
                place_overlay();
                if (visible_ && settings_.presence_mode != PresenceMode::AlwaysVisible) {
                    refresh_window_overlap();
                }
            }
            return 0;
        }

        case WM_CLOSE:
            return 0;
    }

    return DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace mmoni
