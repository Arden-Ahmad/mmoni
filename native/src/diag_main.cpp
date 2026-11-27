#include "platform/settings.h"
#include "platform/startup_task.h"
#include "sensors/sensor_monitor.h"

#include <windows.h>
#include <objbase.h>
#include <psapi.h>

#include <cstdio>
#include <string>

static std::string fmt_opt(const std::optional<float>& v, const char* unit) {
    if (!v.has_value()) return "--";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f%s", *v, unit);
    return buf;
}

int wmain(int argc, wchar_t** argv) {
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int samples = 3;
    bool test_intel = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--samples") == 0 && i + 1 < argc) {
            samples = _wtoi(argv[++i]);
        } else if (_wcsicmp(argv[i], L"--gpu-intel") == 0) {
            test_intel = true;
        }
    }

    auto settings = mmoni::UserSettings::load();
    std::printf("Settings loaded: theme=%d, collapsed=%d, range=%d, autoExpand=%d, autoHideFs=%d, clickThrough=%d, startupTask=%d\n",
                static_cast<int>(settings.theme),
                static_cast<int>(settings.collapsed_mode),
                static_cast<int>(settings.history_range),
                settings.auto_expand_on_hover ? 1 : 0,
                settings.auto_hide_in_fullscreen ? 1 : 0,
                settings.click_through ? 1 : 0,
                mmoni::StartupTask::is_enabled() ? 1 : 0);

    mmoni::SensorMonitor mon(std::nullopt, nullptr);
    mon.init_sync();
    auto first = mon.read_once_sync();

    std::printf("Diagnostics: elevated=%d, pawnio_installed=%d\n",
                mmoni::is_process_elevated() ? 1 : 0,
                first.pawnio_installed ? 1 : 0);
    std::printf("CPU: %s\n", first.cpu_name.value_or("unknown").c_str());
    std::printf("GPUs (%zu):\n", first.gpus.size());
    for (const auto& g : first.gpus) {
        std::printf("  - [%s] %s (vendor=0x%04X, dev=0x%04X, discrete=%d)\n",
                    g.id.c_str(), g.name.c_str(), g.vendor_id, g.device_id, g.is_discrete ? 1 : 0);
        if (test_intel && g.vendor_id == 0x8086) {
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
    }

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        std::printf("Memory: WorkingSet=%.2f MB, PrivateBytes=%.2f MB\n",
                    pmc.WorkingSetSize / (1024.0 * 1024.0),
                    pmc.PrivateUsage / (1024.0 * 1024.0));
    }

    mon.shutdown_sync();
    CoUninitialize();
    return 0;
}
