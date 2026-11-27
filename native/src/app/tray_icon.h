#pragma once

#include "platform/settings.h"
#include "sensors/sensor_types.h"

#include <windows.h>
#include <functional>
#include <vector>

namespace mmoni {

enum class TrayCommand : UINT {
    ToggleOverlay = 1001,
    ThemeLiquidGlass = 1002,
    ThemePrecisionDark = 1003,
    PillTempPower = 1004,
    PillTempLoad = 1005,
    PillTempOnly = 1006,
    Range60s = 1007,
    Range5m = 1008,
    Range15m = 1009,
    ToggleAutoExpand = 1010,
    ToggleAutoHideFs = 1011,
    ToggleClickThrough = 1012,
    ResetPosition = 1013,
    ResetPeaks = 1014,
    ToggleStartup = 1015,
    RestartElevated = 1016,
    ExitApp = 1017,
    GpuAuto = 1100,
    GpuFirst = 1101
};

class TrayIcon {
public:
    TrayIcon() = default;
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    bool create(HWND owner_hwnd, HINSTANCE instance, bool elevated);
    void recreate();
    void destroy();
    void show_warning(const std::wstring& message);

    void show_context_menu(
        const UserSettings& settings,
        bool overlay_visible_by_user,
        bool startup_enabled,
        const std::vector<GpuDevice>& gpus,
        const std::function<void(UINT)>& on_command);

    UINT taskbar_created_msg() const noexcept { return taskbar_created_msg_; }

private:
    HWND owner_ = nullptr;
    HICON icon_ = nullptr;
    bool elevated_ = false;
    bool added_ = false;
    UINT taskbar_created_msg_ = 0;
    std::wstring version_label_;
};

} // namespace mmoni
