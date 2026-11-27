#pragma once

#include "sensors/sensor_types.h"
#include <optional>
#include <string>

namespace mmoni {

struct UserSettings {
    std::optional<int> left;
    std::optional<int> top;
    bool click_through = false;
    bool auto_expand_on_hover = true;
    bool auto_hide_in_fullscreen = true;
    std::optional<std::string> gpu_id;

    VisualTheme theme = VisualTheme::LiquidGlass;
    ChartLayoutMode chart_layout = ChartLayoutMode::Split;
    CollapsedDisplayMode collapsed_mode = CollapsedDisplayMode::TempPower;
    TimeRange history_range = TimeRange::Seconds60;

    float cpu_warm_temp = 80.0f;
    float cpu_hot_temp = 90.0f;
    float gpu_warm_temp = 78.0f;
    float gpu_hot_temp = 87.0f;

    static std::wstring settings_path();
    static UserSettings load();
    bool save(std::wstring* error_out = nullptr) const;
};

bool is_process_elevated() noexcept;
std::wstring get_current_user_sid();
std::wstring get_executable_path();
std::wstring utf8_to_wide(const std::string& utf8);
std::string wide_to_utf8(const std::wstring& wide);

} // namespace mmoni
