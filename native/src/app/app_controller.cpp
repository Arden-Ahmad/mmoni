#include "app/app_controller.h"
#include "platform/overlay_window.h"
#include "platform/startup_task.h"

#include <shellapi.h>
#include <wtsapi32.h>

namespace mmoni {

namespace {
constexpr UINT_PTR kTimerFullscreen = 101;
AppController* g_active_controller = nullptr;
} // namespace

AppController::~AppController() {
    if (g_active_controller == this) {
        g_active_controller = nullptr;
    }
}

void CALLBACK AppController::fullscreen_timer_proc(HWND, UINT, UINT_PTR, DWORD) {
    if (g_active_controller) {
        g_active_controller->check_fullscreen_auto_hide();
    }
}

int AppController::run(HINSTANCE instance, bool wait_for_instance) {
    instance_ = instance;
    g_active_controller = this;
    elevated_ = is_process_elevated();

    if (!single_instance_.acquire(wait_for_instance ? 15000u : 0u)) {
        SingleInstance::try_signal_show();
        return 0;
    }

    if (elevated_) {
        StartupTask::migrate_legacy_if_needed();
    }

    settings_ = UserSettings::load();

    OverlayView::Callbacks cbs{};
    cbs.on_moved = [this](POINT pt) {
        UserSettings next = settings_;
        next.left = pt.x;
        next.top = pt.y;
        save_settings(next);
    };
    cbs.on_range_changed = [this](TimeRange range) {
        UserSettings next = settings_;
        next.history_range = range;
        save_settings(next);
    };
    cbs.on_custom_message = [this](UINT msg, WPARAM wp, LPARAM lp) {
        on_custom_message(msg, wp, lp);
    };

    overlay_ = std::make_unique<OverlayView>(settings_, elevated_, std::move(cbs));
    if (!overlay_->create(instance_)) {
        single_instance_.shutdown();
        return 1;
    }

    HWND hwnd = overlay_->hwnd();
    single_instance_.register_signals(hwnd);
    WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
    tray_.create(hwnd, instance_, elevated_);

    overlay_->show();
    sync_fullscreen_timer();

    monitor_ = std::make_unique<SensorMonitor>(settings_.gpu_id, [this, hwnd](SensorSnapshot snap) {
        if (exiting_.load(std::memory_order_acquire)) return;
        {
            std::lock_guard lock(snap_mutex_);
            pending_snapshot_ = std::move(snap);
        }
        PostMessageW(hwnd, WM_MMONI_SNAPSHOT, 0, 0);
    });
    monitor_->start();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    exiting_.store(true, std::memory_order_release);
    if (fullscreen_timer_active_ && hwnd) {
        KillTimer(hwnd, kTimerFullscreen);
        fullscreen_timer_active_ = false;
    }
    if (hwnd) {
        WTSUnRegisterSessionNotification(hwnd);
    }
    if (monitor_) {
        monitor_->stop();
        monitor_.reset();
    }
    tray_.destroy();
    overlay_->destroy();
    overlay_.reset();
    single_instance_.shutdown();
    return 0;
}

void AppController::on_custom_message(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MMONI_SHOW) {
        if (!exiting_.load(std::memory_order_acquire)) {
            user_hidden_ = false;
            sync_visibility_and_polling();
            if (overlay_) overlay_->ensure_topmost();
        }
        return;
    }

    if (msg == WM_MMONI_EXIT) {
        request_exit();
        return;
    }

    if (msg == WM_MMONI_SNAPSHOT) {
        if (exiting_.load(std::memory_order_acquire) || !overlay_) return;
        std::optional<SensorSnapshot> snap;
        {
            std::lock_guard lock(snap_mutex_);
            snap = std::move(pending_snapshot_);
            pending_snapshot_.reset();
        }
        if (snap.has_value()) {
            if (!snap->error.has_value()) {
                gpus_ = snap->gpus;
            }
            overlay_->display_snapshot(*snap);
            if (!trimmed_working_set_) {
                trimmed_working_set_ = true;
                // Trim one-time cold DLL init pages (DX11/D2D/NVML) after first live render.
                SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
            }
        }
        return;
    }

    if (msg == WM_MMONI_TRAY) {
        const UINT ev = LOWORD(lp);
        if (ev == WM_LBUTTONDBLCLK) {
            user_hidden_ = !user_hidden_;
            sync_visibility_and_polling();
        } else if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
            const bool startup = StartupTask::is_enabled();
            tray_.show_context_menu(settings_, !user_hidden_, startup, gpus_, [this](UINT cmd) {
                on_tray_command(cmd);
            });
        }
        return;
    }

    if (tray_.taskbar_created_msg() != 0 && msg == tray_.taskbar_created_msg()) {
        tray_.recreate();
        return;
    }

    if (msg == WM_WTSSESSION_CHANGE) {
        if (wp == WTS_SESSION_LOCK || wp == WTS_CONSOLE_DISCONNECT) {
            session_locked_ = true;
            sync_visibility_and_polling();
        } else if (wp == WTS_SESSION_UNLOCK || wp == WTS_CONSOLE_CONNECT) {
            session_locked_ = false;
            if (monitor_) monitor_->request_topology_refresh();
            sync_visibility_and_polling();
            if (overlay_) overlay_->ensure_topmost();
        }
        return;
    }

    if (msg == WM_DISPLAYCHANGE) {
        if (monitor_) monitor_->request_topology_refresh();
        if (overlay_) {
            overlay_->handle_display_change();
        }
    }
}

void AppController::on_tray_command(UINT cmd) {
    if (cmd == static_cast<UINT>(TrayCommand::GpuAuto)) {
        UserSettings next = settings_;
        next.gpu_id.reset();
        if (monitor_) monitor_->select_gpu(std::nullopt);
        save_settings(next);
        return;
    }
    if (cmd >= static_cast<UINT>(TrayCommand::GpuFirst)) {
        const size_t idx = static_cast<size_t>(cmd - static_cast<UINT>(TrayCommand::GpuFirst));
        if (idx < gpus_.size()) {
            UserSettings next = settings_;
            next.gpu_id = gpus_[idx].id;
            if (monitor_) monitor_->select_gpu(next.gpu_id);
            save_settings(next);
        }
        return;
    }

    switch (static_cast<TrayCommand>(cmd)) {
        case TrayCommand::ToggleOverlay:
            user_hidden_ = !user_hidden_;
            sync_visibility_and_polling();
            break;
        case TrayCommand::ThemeLiquidGlass: {
            UserSettings next = settings_;
            next.theme = VisualTheme::LiquidGlass;
            update_settings(next);
            break;
        }
        case TrayCommand::ThemePrecisionDark: {
            UserSettings next = settings_;
            next.theme = VisualTheme::PrecisionDark;
            update_settings(next);
            break;
        }
        case TrayCommand::PillTempPower: {
            UserSettings next = settings_;
            next.collapsed_mode = CollapsedDisplayMode::TempPower;
            update_settings(next);
            break;
        }
        case TrayCommand::PillTempLoad: {
            UserSettings next = settings_;
            next.collapsed_mode = CollapsedDisplayMode::TempLoad;
            update_settings(next);
            break;
        }
        case TrayCommand::PillTempOnly: {
            UserSettings next = settings_;
            next.collapsed_mode = CollapsedDisplayMode::TempOnly;
            update_settings(next);
            break;
        }
        case TrayCommand::Range60s: {
            UserSettings next = settings_;
            next.history_range = TimeRange::Seconds60;
            update_settings(next);
            break;
        }
        case TrayCommand::Range5m: {
            UserSettings next = settings_;
            next.history_range = TimeRange::Minutes5;
            update_settings(next);
            break;
        }
        case TrayCommand::Range15m: {
            UserSettings next = settings_;
            next.history_range = TimeRange::Minutes15;
            update_settings(next);
            break;
        }
        case TrayCommand::ToggleAutoExpand: {
            UserSettings next = settings_;
            next.auto_expand_on_hover = !settings_.auto_expand_on_hover;
            update_settings(next);
            break;
        }
        case TrayCommand::ToggleAutoHideFs: {
            UserSettings next = settings_;
            next.auto_hide_in_fullscreen = !settings_.auto_hide_in_fullscreen;
            update_settings(next);
            check_fullscreen_auto_hide();
            sync_fullscreen_timer();
            break;
        }
        case TrayCommand::ToggleClickThrough: {
            UserSettings next = settings_;
            next.click_through = !settings_.click_through;
            update_settings(next);
            break;
        }
        case TrayCommand::ResetPosition: {
            if (overlay_) overlay_->reset_position();
            UserSettings next = settings_;
            next.left.reset();
            next.top.reset();
            save_settings(next);
            break;
        }
        case TrayCommand::ResetPeaks:
            if (overlay_) overlay_->reset_peaks();
            break;
        case TrayCommand::ToggleStartup: {
            const bool target = !StartupTask::is_enabled();
            std::wstring err;
            if (!StartupTask::set_enabled(target, &err)) {
                tray_.show_warning(L"Could not change Windows startup. " + err);
            }
            break;
        }
        case TrayCommand::RestartElevated:
            restart_elevated();
            break;
        case TrayCommand::ExitApp:
            request_exit();
            break;
        default:
            break;
    }
}

void AppController::update_settings(const UserSettings& updated) {
    save_settings(updated);
    if (overlay_) overlay_->apply_settings(updated);
}

void AppController::save_settings(const UserSettings& updated) {
    settings_ = updated;
    std::wstring err;
    if (!settings_.save(&err)) {
        tray_.show_warning(L"Could not save overlay settings. " + err);
    }
}

void AppController::sync_visibility_and_polling() {
    if (exiting_.load(std::memory_order_acquire) || !overlay_) return;

    if (!user_hidden_ && !session_locked_ && !fullscreen_timer_active_) {
        fullscreen_hidden_ = settings_.auto_hide_in_fullscreen && OverlayWindowHelper::is_fullscreen_app_active();
    }

    const bool should_be_visible = !user_hidden_ && !fullscreen_hidden_ && !session_locked_;
    if (monitor_) {
        monitor_->set_paused(!should_be_visible);
    }

    if (should_be_visible) {
        if (!overlay_->is_visible()) {
            overlay_->show();
        } else {
            overlay_->ensure_topmost();
        }
    } else if (overlay_->is_visible()) {
        overlay_->hide();
    }

    sync_fullscreen_timer();
}

void AppController::sync_fullscreen_timer() {
    if (!overlay_ || !overlay_->hwnd()) return;
    const bool should_monitor = !exiting_.load(std::memory_order_acquire) && settings_.auto_hide_in_fullscreen && !user_hidden_ && !session_locked_;
    if (should_monitor && !fullscreen_timer_active_) {
        SetTimer(overlay_->hwnd(), kTimerFullscreen, 1500, &AppController::fullscreen_timer_proc);
        fullscreen_timer_active_ = true;
    } else if (!should_monitor && fullscreen_timer_active_) {
        KillTimer(overlay_->hwnd(), kTimerFullscreen);
        fullscreen_timer_active_ = false;
    }
}

void AppController::check_fullscreen_auto_hide() {
    if (exiting_.load(std::memory_order_acquire) || !overlay_) return;
    const bool should_hide = settings_.auto_hide_in_fullscreen && OverlayWindowHelper::is_fullscreen_app_active();
    if (should_hide == fullscreen_hidden_) return;
    fullscreen_hidden_ = should_hide;
    sync_visibility_and_polling();
}

void AppController::restart_elevated() {
    const std::wstring exe = get_executable_path();
    if (exe.empty()) return;
    HINSTANCE res = ShellExecuteW(nullptr, L"runas", exe.c_str(), L"--wait-for-instance", nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(res) > 32) {
        request_exit();
    }
}

void AppController::request_exit() {
    if (exiting_.exchange(true, std::memory_order_acq_rel)) return;
    PostQuitMessage(0);
}

} // namespace mmoni
