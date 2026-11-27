#pragma once

#include "app/tray_icon.h"
#include "platform/settings.h"
#include "platform/single_instance.h"
#include "sensors/sensor_monitor.h"
#include "ui/overlay_view.h"

#include <windows.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace mmoni {

class AppController {
public:
    AppController() = default;
    ~AppController();

    int run(HINSTANCE instance, bool wait_for_instance);

private:
    void on_custom_message(UINT msg, WPARAM wp, LPARAM lp);
    void on_tray_command(UINT cmd);
    void update_settings(const UserSettings& updated);
    void save_settings(const UserSettings& updated);
    void sync_visibility_and_polling();
    void sync_fullscreen_timer();
    void check_fullscreen_auto_hide();
    void restart_elevated();
    void request_exit();

    static void CALLBACK fullscreen_timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time);

    HINSTANCE instance_ = nullptr;
    bool elevated_ = false;
    std::atomic<bool> exiting_{false};
    bool user_hidden_ = false;
    bool fullscreen_hidden_ = false;
    bool session_locked_ = false;
    bool fullscreen_timer_active_ = false;

    UserSettings settings_;
    SingleInstance single_instance_;
    TrayIcon tray_;
    std::unique_ptr<OverlayView> overlay_;
    std::unique_ptr<SensorMonitor> monitor_;

    std::mutex snap_mutex_;
    std::optional<SensorSnapshot> pending_snapshot_;
    std::vector<GpuDevice> gpus_;
    bool trimmed_working_set_ = false;
};

} // namespace mmoni
