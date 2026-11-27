#pragma once

#include <windows.h>
#include <string>

namespace mmoni {

constexpr UINT WM_MMONI_SHOW = WM_APP + 1;
constexpr UINT WM_MMONI_EXIT = WM_APP + 2;
constexpr UINT WM_MMONI_SNAPSHOT = WM_APP + 3;
constexpr UINT WM_MMONI_TRAY = WM_APP + 4;
constexpr UINT WM_MMONI_ANIM_TICK = WM_APP + 5;
constexpr UINT WM_MMONI_HOVER_TIMER = WM_APP + 6;

class SingleInstance {
public:
    SingleInstance() = default;
    ~SingleInstance();

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    static std::wstring mutex_name();
    static bool try_signal_show();
    static void signal_running_instance_to_exit();

    bool acquire(DWORD timeout_ms);
    void register_signals(HWND target_hwnd);
    void shutdown();

private:
    static void CALLBACK on_show_signal(PVOID param, BOOLEAN timed_out);
    static void CALLBACK on_exit_signal(PVOID param, BOOLEAN timed_out);
    static bool try_signal_event(const wchar_t* name);

    HANDLE mutex_ = nullptr;
    bool owns_mutex_ = false;
    HANDLE show_event_ = nullptr;
    HANDLE exit_event_ = nullptr;
    HANDLE show_wait_ = nullptr;
    HANDLE exit_wait_ = nullptr;
    HWND target_hwnd_ = nullptr;
};

} // namespace mmoni
