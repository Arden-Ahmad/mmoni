#include "app/app_controller.h"
#include "platform/settings.h"
#include "platform/single_instance.h"
#include "platform/startup_task.h"
#include "sensors/sensor_monitor.h"

#include <windows.h>
#include <objbase.h>
#include <roapi.h>
#include <shellapi.h>
#include <winrt/base.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

bool has_arg(const std::vector<std::wstring>& args, const wchar_t* flag) {
    for (const auto& a : args) {
        if (_wcsicmp(a.c_str(), flag) == 0) return true;
    }
    return false;
}

std::string fmt_opt(const std::optional<float>& v, const char* unit) {
    if (!v.has_value()) return "--";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f%s", *v, unit);
    return buf;
}

int run_sensor_test(const std::vector<std::wstring>& args) {
    if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
        FILE* dummy = nullptr;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONOUT$", "w", stderr);
    }

    int samples = 3;
    bool gpu_intel = false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (_wcsicmp(args[i].c_str(), L"--samples") == 0 && i + 1 < args.size()) {
            samples = _wtoi(args[++i].c_str());
            if (samples < 1) samples = 1;
            if (samples > 30) samples = 30;
        } else if (_wcsicmp(args[i].c_str(), L"--gpu-intel") == 0) {
            gpu_intel = true;
        }
    }

    mmoni::SensorMonitor mon(std::nullopt, nullptr);
    mon.init_sync();
    auto first = mon.read_once_sync();

    std::printf("Mmoni Native v2 Sensor Diagnostics (elevated=%d, pawnio=%d)\n",
                mmoni::is_process_elevated() ? 1 : 0,
                first.pawnio_installed ? 1 : 0);
    std::printf("CPU: %s\n", first.cpu_name.value_or("unknown").c_str());
    for (const auto& g : first.gpus) {
        std::printf("  - [%s] %s (vendor=0x%04X, dev=0x%04X, discrete=%d)\n",
                    g.id.c_str(), g.name.c_str(), g.vendor_id, g.device_id, g.is_discrete ? 1 : 0);
        if (gpu_intel && g.vendor_id == 0x8086) {
            mon.select_gpu(g.id);
        }
    }

    for (int i = 0; i < samples; ++i) {
        Sleep(1000);
        auto snap = mon.read_once_sync();
        const auto& c = snap.cpu;
        const auto& g = snap.gpu;
        std::printf("[%d] CPU: %s | %s | %s | %s (thrm=%u) [%.2f ms]  ||  GPU(%s): %s | %s | %s | %s (sleep=%d, thrm=%u) [%.2f ms]\n",
                    i + 1,
                    fmt_opt(c ? c->temperature : std::nullopt, "C").c_str(),
                    fmt_opt(c ? c->power : std::nullopt, "W").c_str(),
                    fmt_opt(c ? c->load : std::nullopt, "%").c_str(),
                    fmt_opt(c ? c->clock : std::nullopt, "MHz").c_str(),
                    c ? c->throttle : 0u,
                    snap.cpu_poll_ms,
                    snap.selected_gpu ? snap.selected_gpu->name.c_str() : "none",
                    fmt_opt(g ? g->temperature : std::nullopt, "C").c_str(),
                    fmt_opt(g ? g->power : std::nullopt, "W").c_str(),
                    fmt_opt(g ? g->load : std::nullopt, "%").c_str(),
                    fmt_opt(g ? g->clock : std::nullopt, "MHz").c_str(),
                    (g && g->is_sleeping) ? 1 : 0,
                    g ? g->throttle : 0u,
                    snap.gpu_poll_ms);
        std::fflush(stdout);
    }

    mon.shutdown_sync();
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // Restrict default DLL search path to System32 to prevent DLL hijacking when elevated.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    if (argv) {
        for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
        LocalFree(argv);
    }

    int exit_code = 0;
    if (has_arg(args, L"--enable-startup")) {
        exit_code = mmoni::StartupTask::set_enabled(true) ? 0 : 1;
    } else if (has_arg(args, L"--disable-startup")) {
        exit_code = mmoni::StartupTask::set_enabled(false) ? 0 : 1;
    } else if (has_arg(args, L"--exit")) {
        mmoni::SingleInstance::signal_running_instance_to_exit();
        exit_code = 0;
    } else if (has_arg(args, L"--sensor-test")) {
        exit_code = run_sensor_test(args);
    } else {
        mmoni::AppController app;
        exit_code = app.run(instance, has_arg(args, L"--wait-for-instance"));
    }

    winrt::uninit_apartment();
    return exit_code;
}
