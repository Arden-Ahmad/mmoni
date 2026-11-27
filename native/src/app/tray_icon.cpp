#include "app/tray_icon.h"
#include "platform/single_instance.h"
#include "resource.h"

#include <shellapi.h>

namespace mmoni {

namespace {
constexpr UINT kTrayIconId = 1;
} // namespace

TrayIcon::~TrayIcon() {
    destroy();
}

bool TrayIcon::create(HWND owner_hwnd, HINSTANCE instance, bool elevated) {
    owner_ = owner_hwnd;
    elevated_ = elevated;
    taskbar_created_msg_ = RegisterWindowMessageW(L"TaskbarCreated");

    wchar_t ver[64];
    swprintf_s(ver, L"Mmoni v%d.%d.%d", MMONI_VERSION_MAJOR, MMONI_VERSION_MINOR, MMONI_VERSION_PATCH);
    version_label_ = ver;

    icon_ = static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(IDI_MMONI_APP),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR));

    recreate();
    return added_;
}

void TrayIcon::recreate() {
    if (!owner_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = owner_;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_MMONI_TRAY;
    nid.hIcon = icon_;
    wcsncpy_s(nid.szTip, version_label_.c_str(), _TRUNCATE);

    added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (added_) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
}

void TrayIcon::destroy() {
    if (added_ && owner_) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = owner_;
        nid.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        added_ = false;
    }
    if (icon_) {
        DestroyIcon(icon_);
        icon_ = nullptr;
    }
}

void TrayIcon::show_warning(const std::wstring& message) {
    if (!added_ || !owner_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = owner_;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_WARNING;
    wcsncpy_s(nid.szInfoTitle, L"Mmoni", _TRUNCATE);
    wcsncpy_s(nid.szInfo, message.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::show_context_menu(
    const UserSettings& settings,
    bool overlay_visible_by_user,
    bool startup_enabled,
    const std::vector<GpuDevice>& gpus,
    const std::function<void(UINT)>& on_command) {

    if (!owner_) return;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    std::wstring header = elevated_ ? (version_label_ + L" (Admin)") : version_label_;
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, header.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING | (overlay_visible_by_user ? MF_CHECKED : 0),
                static_cast<UINT>(TrayCommand::ToggleOverlay), L"Show overlay");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU theme_menu = CreatePopupMenu();
    AppendMenuW(theme_menu, MF_STRING | (settings.theme == VisualTheme::LiquidGlass ? MF_CHECKED : 0),
                static_cast<UINT>(TrayCommand::ThemeLiquidGlass), L"Liquid Glass (Blur)");
    AppendMenuW(theme_menu, MF_STRING | (settings.theme == VisualTheme::PrecisionDark ? MF_CHECKED : 0),
                static_cast<UINT>(TrayCommand::ThemePrecisionDark), L"Precision Dark (No Blur)");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(theme_menu), L"Visual style");

    HMENU pill_menu = CreatePopupMenu();
    AppendMenuW(pill_menu, MF_STRING | (settings.collapsed_mode == CollapsedDisplayMode::TempPower ? MF_CHECKED : 0),
                static_cast<UINT>(TrayCommand::PillTempPower), L"Temp + Power");
    AppendMenuW(pill_menu, MF_STRING | (settings.collapsed_mode == CollapsedDisplayMode::TempLoad ? MF_CHECKED : 0),
                static_cast<UINT>(TrayCommand::PillTempLoad), L"Temp + Load");
    AppendMenuW(pill_menu, MF_STRING | (settings.collapsed_mode == CollapsedDisplayMode::TempOnly ? MF_CHECKED : 0),
                static_cast<UINT>(TrayCommand::PillTempOnly), L"Temp only (Compact)");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(pill_menu), L"Minimal pill metrics");

    HMENU range_menu = CreatePopupMenu();
    AppendMenuW(range_menu, MF_STRING | (settings.history_range == TimeRange::Seconds60 ? MF_CHECKED : 0),
                static_cast<UINT>(TrayCommand::Range60s), L"60 seconds");
    AppendMenuW(range_menu, MF_STRING | (settings.history_range == TimeRange::Minutes5 ? MF_CHECKED : 0),
                static_cast<UINT>(TrayCommand::Range5m), L"5 minutes");
    AppendMenuW(range_menu, MF_STRING | (settings.history_range == TimeRange::Minutes15 ? MF_CHECKED : 0),
                static_cast<UINT>(TrayCommand::Range15m), L"15 minutes");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(range_menu), L"History range");

    HMENU gpu_menu = CreatePopupMenu();
    bool matched_gpu = false;
    for (size_t i = 0; i < gpus.size(); ++i) {
        if (settings.gpu_id.has_value() && gpus[i].id == *settings.gpu_id) {
            matched_gpu = true;
            break;
        }
    }
    AppendMenuW(gpu_menu, MF_STRING | (!matched_gpu ? MF_CHECKED : 0),
                static_cast<UINT>(TrayCommand::GpuAuto), L"Automatic");
    for (size_t i = 0; i < gpus.size(); ++i) {
        bool selected = settings.gpu_id.has_value() && gpus[i].id == *settings.gpu_id;
        std::wstring label = utf8_to_wide(gpus[i].name);
        AppendMenuW(gpu_menu, MF_STRING | (selected ? MF_CHECKED : 0),
                    static_cast<UINT>(TrayCommand::GpuFirst) + static_cast<UINT>(i), label.c_str());
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(gpu_menu), L"GPU");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (settings.auto_expand_on_hover ? MF_CHECKED : 0),
                static_cast<UINT>(TrayCommand::ToggleAutoExpand), L"Hover Peek (Auto-expand)");
    AppendMenuW(menu, MF_STRING | (settings.auto_hide_in_fullscreen ? MF_CHECKED : 0),
                static_cast<UINT>(TrayCommand::ToggleAutoHideFs), L"Auto-hide in fullscreen");
    AppendMenuW(menu, MF_STRING | (settings.click_through ? MF_CHECKED : 0),
                static_cast<UINT>(TrayCommand::ToggleClickThrough), L"Click-through");
    AppendMenuW(menu, MF_STRING, static_cast<UINT>(TrayCommand::ResetPosition), L"Reset position");
    AppendMenuW(menu, MF_STRING, static_cast<UINT>(TrayCommand::ResetPeaks), L"Reset session peaks");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING | (startup_enabled ? MF_CHECKED : 0),
                static_cast<UINT>(TrayCommand::ToggleStartup), L"Start with Windows (Admin)");
    if (!elevated_) {
        AppendMenuW(menu, MF_STRING, static_cast<UINT>(TrayCommand::RestartElevated), L"Restart as administrator");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, static_cast<UINT>(TrayCommand::ExitApp), L"Exit Mmoni");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(owner_);
    UINT cmd = TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, owner_, nullptr);
    PostMessageW(owner_, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (cmd != 0 && on_command) {
        on_command(cmd);
    }
}

} // namespace mmoni
