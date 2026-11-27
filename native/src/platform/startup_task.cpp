#include "platform/startup_task.h"
#include "platform/settings.h"

#include <windows.h>
#include <comdef.h>
#include <taskschd.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace mmoni {

namespace {

constexpr wchar_t kTaskName[] = L"Mmoni";
constexpr wchar_t kLegacyRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kLegacyRunValue[] = L"Mmoni";

void remove_legacy_run_entry() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kLegacyRunKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, kLegacyRunValue);
        RegCloseKey(key);
    }
}

HRESULT connect_root_folder(ComPtr<ITaskService>& service, ComPtr<ITaskFolder>& root) {
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&service));
    if (FAILED(hr)) return hr;

    VARIANT empty{};
    VariantInit(&empty);
    hr = service->Connect(empty, empty, empty, empty);
    if (FAILED(hr)) return hr;

    _bstr_t root_path(L"\\");
    return service->GetFolder(root_path, &root);
}

std::wstring format_hr(HRESULT hr) {
    wchar_t buf[64];
    swprintf_s(buf, L"Task Scheduler error (0x%08X)", static_cast<unsigned>(hr));
    return buf;
}

} // namespace

bool StartupTask::is_enabled() {
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    if (FAILED(connect_root_folder(service, root))) {
        return false;
    }

    ComPtr<IRegisteredTask> task;
    _bstr_t name(kTaskName);
    HRESULT hr = root->GetTask(name, &task);
    if (FAILED(hr) || !task) {
        return false;
    }

    VARIANT_BOOL enabled = VARIANT_FALSE;
    if (SUCCEEDED(task->get_Enabled(&enabled))) {
        return enabled == VARIANT_TRUE;
    }
    return true;
}

bool StartupTask::set_enabled(bool enabled, std::wstring* error_out) {
    remove_legacy_run_entry();

    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    HRESULT hr = connect_root_folder(service, root);
    if (FAILED(hr)) {
        if (error_out) *error_out = format_hr(hr);
        return false;
    }

    _bstr_t task_name(kTaskName);

    if (!enabled) {
        hr = root->DeleteTask(task_name, 0);
        if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) &&
            hr != HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) {
            if (error_out) *error_out = format_hr(hr);
            return false;
        }
        return true;
    }

    const std::wstring exe = get_executable_path();
    if (exe.empty()) {
        if (error_out) *error_out = L"Executable path unavailable.";
        return false;
    }
    const auto slash = exe.find_last_of(L"\\/");
    const std::wstring work_dir = (slash != std::wstring::npos) ? exe.substr(0, slash) : L"";
    const std::wstring user_sid = get_current_user_sid();

    ComPtr<ITaskDefinition> def;
    hr = service->NewTask(0, &def);
    if (FAILED(hr)) {
        if (error_out) *error_out = format_hr(hr);
        return false;
    }

    ComPtr<IRegistrationInfo> reg_info;
    if (SUCCEEDED(def->get_RegistrationInfo(&reg_info)) && reg_info) {
        reg_info->put_Description(_bstr_t(L"Starts Mmoni hardware telemetry overlay with administrator privileges at user sign-in."));
    }

    ComPtr<IPrincipal> principal;
    if (SUCCEEDED(def->get_Principal(&principal)) && principal) {
        principal->put_UserId(_bstr_t(user_sid.c_str()));
        principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
        principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
    }

    ComPtr<ITaskSettings> settings;
    if (SUCCEEDED(def->get_Settings(&settings)) && settings) {
        settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
        settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        settings->put_AllowHardTerminate(VARIANT_FALSE);
        settings->put_StartWhenAvailable(VARIANT_TRUE);
        settings->put_RunOnlyIfNetworkAvailable(VARIANT_FALSE);
        settings->put_AllowDemandStart(VARIANT_TRUE);
        settings->put_Enabled(VARIANT_TRUE);
        settings->put_Hidden(VARIANT_FALSE);
        settings->put_RunOnlyIfIdle(VARIANT_FALSE);
        settings->put_WakeToRun(VARIANT_FALSE);
        settings->put_ExecutionTimeLimit(_bstr_t(L"PT0S"));
        settings->put_Priority(5);

        ComPtr<IIdleSettings> idle;
        if (SUCCEEDED(settings->get_IdleSettings(&idle)) && idle) {
            idle->put_StopOnIdleEnd(VARIANT_FALSE);
            idle->put_RestartOnIdle(VARIANT_FALSE);
        }
    }

    ComPtr<ITriggerCollection> triggers;
    if (SUCCEEDED(def->get_Triggers(&triggers)) && triggers) {
        ComPtr<ITrigger> trigger;
        if (SUCCEEDED(triggers->Create(TASK_TRIGGER_LOGON, &trigger)) && trigger) {
            ComPtr<ILogonTrigger> logon;
            if (SUCCEEDED(trigger.As(&logon)) && logon) {
                logon->put_Enabled(VARIANT_TRUE);
                logon->put_UserId(_bstr_t(user_sid.c_str()));
                logon->put_Delay(_bstr_t(L"PT3S"));
            }
        }
    }

    ComPtr<IActionCollection> actions;
    if (SUCCEEDED(def->get_Actions(&actions)) && actions) {
        ComPtr<IAction> action;
        if (SUCCEEDED(actions->Create(TASK_ACTION_EXEC, &action)) && action) {
            ComPtr<IExecAction> exec_action;
            if (SUCCEEDED(action.As(&exec_action)) && exec_action) {
                exec_action->put_Path(_bstr_t(exe.c_str()));
                if (!work_dir.empty()) {
                    exec_action->put_WorkingDirectory(_bstr_t(work_dir.c_str()));
                }
            }
        }
    }

    VARIANT empty{};
    VariantInit(&empty);
    ComPtr<IRegisteredTask> registered;
    hr = root->RegisterTaskDefinition(
        task_name,
        def.Get(),
        TASK_CREATE_OR_UPDATE,
        empty,
        empty,
        TASK_LOGON_INTERACTIVE_TOKEN,
        empty,
        &registered);

    if (FAILED(hr)) {
        if (error_out) *error_out = format_hr(hr);
        return false;
    }

    return true;
}

void StartupTask::migrate_legacy_if_needed() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kLegacyRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return;
    }
    DWORD type = 0;
    DWORD size = 0;
    bool has_legacy = (RegQueryValueExW(key, kLegacyRunValue, nullptr, &type, nullptr, &size) == ERROR_SUCCESS) &&
                      (type == REG_SZ || type == REG_EXPAND_SZ);
    RegCloseKey(key);
    if (has_legacy) {
        set_enabled(true);
    }
}

} // namespace mmoni
