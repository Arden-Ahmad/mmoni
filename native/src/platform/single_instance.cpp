#include "platform/single_instance.h"
#include "platform/settings.h"

namespace mmoni {

namespace {
constexpr wchar_t kShowSignalName[] = L"Local\\Mmoni-Signal-Show";
constexpr wchar_t kExitSignalName[] = L"Local\\Mmoni-Signal-Exit";
} // namespace

SingleInstance::~SingleInstance() {
    shutdown();
}

std::wstring SingleInstance::mutex_name() {
    return L"Local\\Mmoni-" + get_current_user_sid();
}

bool SingleInstance::try_signal_event(const wchar_t* name) {
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (!ev) return false;
    BOOL ok = SetEvent(ev);
    CloseHandle(ev);
    return ok != FALSE;
}

bool SingleInstance::try_signal_show() {
    return try_signal_event(kShowSignalName);
}

void SingleInstance::signal_running_instance_to_exit() {
    try_signal_event(kExitSignalName);

    const auto name = mutex_name();
    HANDLE mtx = CreateMutexW(nullptr, FALSE, name.c_str());
    if (!mtx) return;
    DWORD res = WaitForSingleObject(mtx, 5000);
    if (res == WAIT_OBJECT_0 || res == WAIT_ABANDONED) {
        ReleaseMutex(mtx);
    }
    CloseHandle(mtx);
}

bool SingleInstance::acquire(DWORD timeout_ms) {
    const auto name = mutex_name();
    mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
    if (!mutex_) return false;

    DWORD res = WaitForSingleObject(mutex_, timeout_ms);
    if (res == WAIT_OBJECT_0 || res == WAIT_ABANDONED) {
        owns_mutex_ = true;
        return true;
    }

    CloseHandle(mutex_);
    mutex_ = nullptr;
    return false;
}

void SingleInstance::register_signals(HWND target_hwnd) {
    target_hwnd_ = target_hwnd;
    show_event_ = CreateEventW(nullptr, FALSE, FALSE, kShowSignalName);
    if (show_event_) {
        RegisterWaitForSingleObject(&show_wait_, show_event_, &SingleInstance::on_show_signal,
                                    this, INFINITE, WT_EXECUTEDEFAULT);
    }
    exit_event_ = CreateEventW(nullptr, FALSE, FALSE, kExitSignalName);
    if (exit_event_) {
        RegisterWaitForSingleObject(&exit_wait_, exit_event_, &SingleInstance::on_exit_signal,
                                    this, INFINITE, WT_EXECUTEDEFAULT);
    }
}

void SingleInstance::shutdown() {
    if (show_wait_) {
        UnregisterWaitEx(show_wait_, INVALID_HANDLE_VALUE);
        show_wait_ = nullptr;
    }
    if (exit_wait_) {
        UnregisterWaitEx(exit_wait_, INVALID_HANDLE_VALUE);
        exit_wait_ = nullptr;
    }
    if (show_event_) {
        CloseHandle(show_event_);
        show_event_ = nullptr;
    }
    if (exit_event_) {
        CloseHandle(exit_event_);
        exit_event_ = nullptr;
    }
    if (mutex_) {
        if (owns_mutex_) {
            ReleaseMutex(mutex_);
            owns_mutex_ = false;
        }
        CloseHandle(mutex_);
        mutex_ = nullptr;
    }
}

void CALLBACK SingleInstance::on_show_signal(PVOID param, BOOLEAN timed_out) {
    if (timed_out) return;
    auto* self = static_cast<SingleInstance*>(param);
    if (self && self->target_hwnd_) {
        PostMessageW(self->target_hwnd_, WM_MMONI_SHOW, 0, 0);
    }
}

void CALLBACK SingleInstance::on_exit_signal(PVOID param, BOOLEAN timed_out) {
    if (timed_out) return;
    auto* self = static_cast<SingleInstance*>(param);
    if (self && self->target_hwnd_) {
        PostMessageW(self->target_hwnd_, WM_MMONI_EXIT, 0, 0);
    }
}

} // namespace mmoni
