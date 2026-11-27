#include "platform/overlay_window.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>

namespace mmoni {

void OverlayWindowHelper::set_click_through(HWND hwnd, bool enabled) noexcept {
    if (!hwnd) return;
    const LONG_PTR current = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    // Cross-process click-through on Windows requires WS_EX_LAYERED paired with WS_EX_TRANSPARENT.
    constexpr LONG_PTR kClickThroughBits = WS_EX_LAYERED | WS_EX_TRANSPARENT;
    const LONG_PTR next = enabled
        ? (current | kClickThroughBits)
        : (current & ~kClickThroughBits);
    if (current != next) {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, next);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

POINT OverlayWindowHelper::get_position(HWND hwnd) noexcept {
    RECT r{};
    if (hwnd && GetWindowRect(hwnd, &r)) {
        return {r.left, r.top};
    }
    return {0, 0};
}

std::pair<double, double> OverlayWindowHelper::get_monitor_dpi_scale(std::optional<POINT> anchor, HWND fallback_hwnd) noexcept {
    HMONITOR mon = anchor.has_value()
        ? MonitorFromPoint(*anchor, MONITOR_DEFAULTTONEAREST)
        : (fallback_hwnd ? MonitorFromWindow(fallback_hwnd, MONITOR_DEFAULTTONEAREST)
                         : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));
    if (mon) {
        UINT dpi_x = 96;
        UINT dpi_y = 96;
        if (SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) && dpi_x > 0 && dpi_y > 0) {
            return {static_cast<double>(dpi_x) / 96.0, static_cast<double>(dpi_y) / 96.0};
        }
    }
    return {1.0, 1.0};
}

OverlayPlacement OverlayWindowHelper::compute_placement(
    HWND hwnd,
    std::optional<POINT> compact_anchor,
    double compact_width_dip,
    double compact_height_dip,
    double target_width_dip,
    double target_height_dip) noexcept {

    auto [dpi_x, dpi_y] = get_monitor_dpi_scale(compact_anchor, hwnd);

    HMONITOR mon = compact_anchor.has_value()
        ? MonitorFromPoint(*compact_anchor, MONITOR_DEFAULTTONEAREST)
        : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT area{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (mon && GetMonitorInfoW(mon, &mi)) {
        area = mi.rcWork;
    }

    const int compact_w = std::max(1, static_cast<int>(std::llround(compact_width_dip * dpi_x)));
    const int compact_h = std::max(1, static_cast<int>(std::llround(compact_height_dip * dpi_y)));
    const int target_w = std::max(compact_w, static_cast<int>(std::llround(target_width_dip * dpi_x)));
    const int target_h = std::max(compact_h, static_cast<int>(std::llround(target_height_dip * dpi_y)));

    const int top_margin = static_cast<int>(std::llround(5.0 * dpi_y));
    const int area_w = area.right - area.left;
    POINT desired_compact = compact_anchor.value_or(
        POINT{area.left + (area_w - compact_w) / 2, area.top + top_margin});

    const int compact_left = std::clamp(static_cast<int>(desired_compact.x), static_cast<int>(area.left),
                                        std::max(static_cast<int>(area.left), static_cast<int>(area.right) - compact_w));
    const int compact_top = std::clamp(static_cast<int>(desired_compact.y), static_cast<int>(area.top),
                                       std::max(static_cast<int>(area.top), static_cast<int>(area.bottom) - compact_h));

    const int desired_target_left = compact_left - (target_w - compact_w) / 2;
    const int desired_target_top = compact_top;

    const int target_left = std::clamp(desired_target_left, static_cast<int>(area.left),
                                       std::max(static_cast<int>(area.left), static_cast<int>(area.right) - target_w));
    const int target_top = std::clamp(desired_target_top, static_cast<int>(area.top),
                                      std::max(static_cast<int>(area.top), static_cast<int>(area.bottom) - target_h));

    const int pad_l = static_cast<int>(std::llround(kShadowPadLeft * dpi_x));
    const int pad_t = static_cast<int>(std::llround(kShadowPadTop * dpi_y));
    const int pad_r = static_cast<int>(std::llround(kShadowPadRight * dpi_x));
    const int pad_b = static_cast<int>(std::llround(kShadowPadBottom * dpi_y));

    const int stage_left = target_left - pad_l;
    int stage_top = target_top - pad_t;
    const int stage_w = target_w + pad_l + pad_r;
    int stage_h = target_h + pad_t + pad_b;

    const bool near_top_edge = (static_cast<double>(compact_top - area.top) / dpi_y) <= kTopEdgeZoneDip;
    double extra_top_dip = 0.0;
    if (near_top_edge && stage_top > area.top) {
        const int extra_top_px = stage_top - static_cast<int>(area.top);
        stage_top = static_cast<int>(area.top);
        stage_h += extra_top_px;
        extra_top_dip = static_cast<double>(extra_top_px) / dpi_y;
    }

    const double compact_off_x = kShadowPadLeft +
        std::clamp(static_cast<double>(compact_left - target_left) / dpi_x, 0.0,
                   std::max(0.0, target_width_dip - compact_width_dip));
    const double compact_off_y = kShadowPadTop + extra_top_dip +
        std::clamp(static_cast<double>(compact_top - target_top) / dpi_y, 0.0,
                   std::max(0.0, target_height_dip - compact_height_dip));
    const double top_edge_off_y = std::max(0.0, static_cast<double>(area.top - stage_top) / dpi_y);

    OverlayPlacement p{};
    p.compact_visual = {compact_left, compact_top, compact_left + compact_w, compact_top + compact_h};
    p.target_visual = {target_left, target_top, target_left + target_w, target_top + target_h};
    p.stage_window = {stage_left, stage_top, stage_left + stage_w, stage_top + stage_h};
    p.monitor_work = area;
    p.compact_offset_x = compact_off_x;
    p.compact_offset_y = compact_off_y;
    p.target_offset_x = kShadowPadLeft;
    p.target_offset_y = kShadowPadTop + extra_top_dip;
    p.top_edge_offset_y = top_edge_off_y;
    p.near_top_edge = near_top_edge;
    p.dpi_scale_x = dpi_x;
    p.dpi_scale_y = dpi_y;
    return p;
}

void OverlayWindowHelper::set_bounds_topmost(HWND hwnd, int left, int top, int width, int height) noexcept {
    if (!hwnd) return;
    SetWindowPos(hwnd, HWND_TOPMOST, left, top, width, height, SWP_NOACTIVATE);
}

void OverlayWindowHelper::move_topmost(HWND hwnd, int left, int top) noexcept {
    if (!hwnd) return;
    SetWindowPos(hwnd, HWND_TOPMOST, left, top, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

bool OverlayWindowHelper::is_fullscreen_app_active() noexcept {
    QUERY_USER_NOTIFICATION_STATE state{};
    if (FAILED(SHQueryUserNotificationState(&state))) {
        return false;
    }
    if (state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE) {
        return true;
    }
    if (state != QUNS_BUSY) {
        return false;
    }

    // QUNS_BUSY can fire for Windows Shell surfaces (Win+Tab, taskbar preview, desktop).
    // Only treat it as fullscreen if a non-shell foreground window covers its entire monitor.
    HWND fg = GetForegroundWindow();
    if (!fg || !IsWindowVisible(fg)) return false;

    wchar_t cls[128]{};
    if (GetClassNameW(fg, cls, static_cast<int>(_countof(cls))) > 0) {
        if (_wcsicmp(cls, L"Shell_TrayWnd") == 0 ||
            _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
            _wcsicmp(cls, L"Progman") == 0 ||
            _wcsicmp(cls, L"WorkerW") == 0 ||
            _wcsicmp(cls, L"MultitaskingViewFrame") == 0 ||
            _wcsicmp(cls, L"XamlExplorerHostIslandWindow") == 0 ||
            _wcsicmp(cls, L"TaskListThumbnailWnd") == 0 ||
            _wcsicmp(cls, L"Windows.UI.Core.CoreWindow") == 0) {
            return false;
        }
    }

    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT wr{};
    if (!mon || !GetMonitorInfoW(mon, &mi) || !GetWindowRect(fg, &wr)) {
        return false;
    }

    return wr.left <= mi.rcMonitor.left + 2 &&
           wr.top <= mi.rcMonitor.top + 2 &&
           wr.right >= mi.rcMonitor.right - 2 &&
           wr.bottom >= mi.rcMonitor.bottom - 2;
}

bool OverlayWindowHelper::has_overlapping_window(HWND self_hwnd, const RECT& screen_rect) noexcept {
    if (screen_rect.right <= screen_rect.left || screen_rect.bottom <= screen_rect.top) {
        return false;
    }

    struct OverlapQuery {
        HWND self;
        RECT rect;
        bool found;
    } query{self_hwnd, screen_rect, false};

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* q = reinterpret_cast<OverlapQuery*>(lp);
        if (!hwnd || hwnd == q->self) return TRUE;
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;

        const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if ((ex & WS_EX_TRANSPARENT) != 0) return TRUE;
        if ((ex & WS_EX_TOOLWINDOW) != 0 && (ex & WS_EX_APPWINDOW) == 0) return TRUE;
        if ((ex & WS_EX_LAYERED) != 0) {
            BYTE alpha = 255;
            DWORD flags = 0;
            if (GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &flags) &&
                (flags & LWA_ALPHA) != 0 && alpha == 0) {
                return TRUE;
            }
        }

        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
            return TRUE;
        }

        wchar_t cls[128]{};
        if (GetClassNameW(hwnd, cls, static_cast<int>(_countof(cls))) > 0) {
            if (_wcsicmp(cls, L"Progman") == 0 ||
                _wcsicmp(cls, L"WorkerW") == 0 ||
                _wcsicmp(cls, L"Shell_TrayWnd") == 0 ||
                _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
                _wcsicmp(cls, L"MultitaskingViewFrame") == 0 ||
                _wcsicmp(cls, L"TaskListThumbnailWnd") == 0 ||
                _wcsicmp(cls, L"XamlExplorerHostIslandWindow") == 0 ||
                _wcsicmp(cls, L"Xaml_WindowedPopupClass") == 0 ||
                _wcsicmp(cls, L"NotifyIconOverflowWindow") == 0 ||
                _wcsicmp(cls, L"EdgeUiInputTopWndClass") == 0 ||
                _wcsicmp(cls, L"DummyDWMListenerWindow") == 0 ||
                _wcsicmp(cls, L"tooltips_class32") == 0 ||
                _wcsicmp(cls, L"IME") == 0 ||
                _wcsicmp(cls, L"MSCTFIME UI") == 0 ||
                _wcsicmp(cls, L"Windows.UI.Core.CoreWindow") == 0) {
                return TRUE;
            }
        }

        RECT wr{};
        if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &wr, sizeof(wr)))) {
            if (!GetWindowRect(hwnd, &wr)) return TRUE;
        }
        if (wr.right <= wr.left || wr.bottom <= wr.top) return TRUE;

        if (wr.left < q->rect.right && wr.right > q->rect.left &&
            wr.top < q->rect.bottom && wr.bottom > q->rect.top) {
            q->found = true;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&query));

    return query.found;
}

} // namespace mmoni
