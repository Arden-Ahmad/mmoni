#pragma once

#include <windows.h>
#include <optional>
#include <utility>

namespace mmoni {

struct OverlayPlacement {
    RECT compact_visual{};
    RECT target_visual{};
    RECT stage_window{};
    double compact_offset_x = 0.0;
    double compact_offset_y = 0.0;
    double target_offset_x = 0.0;
    double target_offset_y = 0.0;
    double dpi_scale_x = 1.0;
    double dpi_scale_y = 1.0;
};

class OverlayWindowHelper {
public:
    static constexpr double kShadowPadLeft = 18.0;
    static constexpr double kShadowPadTop = 6.0;
    static constexpr double kShadowPadRight = 18.0;
    static constexpr double kShadowPadBottom = 26.0;

    static void set_click_through(HWND hwnd, bool enabled) noexcept;
    static POINT get_position(HWND hwnd) noexcept;
    static std::pair<double, double> get_monitor_dpi_scale(std::optional<POINT> anchor, HWND fallback_hwnd) noexcept;
    static OverlayPlacement compute_placement(
        HWND hwnd,
        std::optional<POINT> compact_anchor,
        double compact_width_dip,
        double compact_height_dip,
        double target_width_dip,
        double target_height_dip) noexcept;
    static void set_bounds_topmost(HWND hwnd, int left, int top, int width, int height) noexcept;
    static void move_topmost(HWND hwnd, int left, int top) noexcept;
    static bool is_fullscreen_app_active() noexcept;
};

} // namespace mmoni
