#include "sensors/sensor_monitor.h"
#include "platform/settings.h"
#include "sensors/cpu_provider.h"
#include "sensors/gpu_amd.h"
#include "sensors/gpu_intel.h"
#include "sensors/gpu_nvidia.h"
#include "sensors/gpu_sleep_guard.h"

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace mmoni {

namespace {

constexpr uint32_t kVendorNvidia = 0x10DE;
constexpr uint32_t kVendorAmd = 0x1002;
constexpr uint32_t kVendorIntel = 0x8086;

const GpuDevice* match_preferred_gpu(const std::vector<GpuDevice>& gpus, const std::string& pref_id) {
    // 1) Exact persistent ID match
    for (const auto& g : gpus) {
        if (g.id == pref_id) return &g;
    }

    // 2) Migrate legacy WPF LibreHardwareMonitor IDs (/gpu-nvidia/0, /gpu-amd/0, /gpu-intel/0)
    auto match_legacy = [&](const char* prefix, uint32_t vendor) -> const GpuDevice* {
        const size_t plen = strlen(prefix);
        if (pref_id.size() > plen && _strnicmp(pref_id.c_str(), prefix, plen) == 0) {
            const int ord = std::atoi(pref_id.c_str() + plen);
            for (const auto& g : gpus) {
                if (g.vendor_id == vendor && g.vendor_ordinal == ord) {
                    return &g;
                }
            }
        }
        return nullptr;
    };

    if (auto* g = match_legacy("/gpu-nvidia/", kVendorNvidia)) return g;
    if (auto* g = match_legacy("/gpu-amd/", kVendorAmd)) return g;
    if (auto* g = match_legacy("/gpu-intel/", kVendorIntel)) return g;

    // 3) Migrate earlier v2 LUID-based IDs ("pci/VVVV:DDDD/...") if unique vendor+device match exists
    if (pref_id.rfind("pci/", 0) == 0 && pref_id.find("/bdf/") == std::string::npos) {
        unsigned int vid = 0, did = 0;
        if (sscanf_s(pref_id.c_str(), "pci/%x:%x", &vid, &did) == 2) {
            const GpuDevice* candidate = nullptr;
            int matches = 0;
            for (const auto& g : gpus) {
                if (g.vendor_id == vid && g.device_id == did) {
                    candidate = &g;
                    ++matches;
                }
            }
            if (matches == 1) return candidate;
        }
    }

    return nullptr;
}

} // namespace

struct SensorMonitor::WorkerState {
    PublishCallback publish;
    std::mutex gpu_id_mutex;
    std::optional<std::string> preferred_gpu_id;

    std::atomic<bool> stop_requested{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> reset_deltas_pending{false};
    std::atomic<bool> refresh_topology_pending{false};
    HANDLE wake_event = nullptr;

    bool initialized = false;
    CpuProvider cpu;
    GpuSleepGuard sleep_guard;
    NvidiaGpuProvider nvidia;
    AmdGpuProvider amd;
    IntelGpuProvider intel_gpu;
    std::vector<GpuDevice> gpus;
    bool has_hybrid_gpus = false;
    uint64_t qpc_freq = 1;

    ~WorkerState() {
        shutdown();
        if (wake_event) {
            CloseHandle(wake_event);
            wake_event = nullptr;
        }
    }

    void discover_gpus() {
        gpus.clear();
        has_hybrid_gpus = false;

        ComPtr<IDXGIFactory6> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) {
            return;
        }

        auto pci_nodes = sleep_guard.enumerate_display_nodes();
        std::vector<bool> node_claimed(pci_nodes.size(), false);

        int nvidia_ord = 0;
        int amd_ord = 0;
        int intel_ord = 0;

        for (UINT idx = 0;; ++idx) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(idx, &adapter) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (!adapter) continue;

            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(adapter->GetDesc1(&desc))) continue;
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;
            if (desc.VendorId != kVendorNvidia && desc.VendorId != kVendorAmd && desc.VendorId != kVendorIntel) {
                continue;
            }

            const uint64_t luid = (static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
                                  static_cast<uint64_t>(desc.AdapterLuid.LowPart);
            bool exists = false;
            for (const auto& existing : gpus) {
                if (existing.luid == luid) {
                    exists = true;
                    break;
                }
            }
            if (exists) continue;

            GpuDevice dev{};
            dev.name = wide_to_utf8(desc.Description);
            dev.vendor_id = desc.VendorId;
            dev.device_id = desc.DeviceId;
            dev.subsys_id = desc.SubSysId;
            dev.luid = luid;

            if (desc.VendorId == kVendorNvidia) {
                dev.is_discrete = true;
                dev.vendor_ordinal = nvidia_ord++;
            } else if (desc.VendorId == kVendorAmd) {
                dev.is_discrete = (desc.DedicatedVideoMemory >= (1024ULL * 1024ULL * 1024ULL));
                dev.vendor_ordinal = amd_ord++;
            } else {
                dev.is_discrete = (desc.DedicatedVideoMemory >= (2048ULL * 1024ULL * 1024ULL));
                dev.vendor_ordinal = intel_ord++;
            }

            // Pair with CfgMgr32 display node to attach persistent PCI Bus/Device/Function.
            for (size_t ni = 0; ni < pci_nodes.size(); ++ni) {
                if (node_claimed[ni]) continue;
                const auto& pn = pci_nodes[ni];
                if (pn.vendor_id == dev.vendor_id && pn.device_id == dev.device_id &&
                    (pn.subsys_id == 0 || dev.subsys_id == 0 || pn.subsys_id == dev.subsys_id)) {
                    dev.pci_bus = pn.pci_bus;
                    dev.pci_dev = pn.pci_dev;
                    dev.pci_func = pn.pci_func;
                    node_claimed[ni] = true;
                    break;
                }
            }

            // Persistent reboot-stable GPU ID (never depends on ephemeral session LUID).
            char id_buf[96];
            if (dev.pci_bus >= 0 && dev.pci_dev >= 0) {
                std::snprintf(id_buf, sizeof(id_buf), "pci/%04x:%04x:%08x/bdf/%02x:%02x:%x",
                              dev.vendor_id, dev.device_id, dev.subsys_id,
                              dev.pci_bus, dev.pci_dev, std::max(0, dev.pci_func));
            } else {
                std::snprintf(id_buf, sizeof(id_buf), "pci/%04x:%04x:%08x/ord/%d",
                              dev.vendor_id, dev.device_id, dev.subsys_id, dev.vendor_ordinal);
            }
            dev.id = id_buf;

            gpus.push_back(std::move(dev));
        }

        std::stable_sort(gpus.begin(), gpus.end(), [](const GpuDevice& a, const GpuDevice& b) {
            if (a.is_discrete != b.is_discrete) return a.is_discrete > b.is_discrete;
            return a.vendor_id != kVendorIntel && b.vendor_id == kVendorIntel;
        });

        bool any_discrete = false;
        bool any_integrated = false;
        for (const auto& g : gpus) {
            if (g.is_discrete) any_discrete = true;
            else any_integrated = true;
        }
        has_hybrid_gpus = (gpus.size() > 1) && any_discrete && any_integrated;
    }

    void init() {
        if (initialized) return;
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        qpc_freq = f.QuadPart > 0 ? static_cast<uint64_t>(f.QuadPart) : 1;

        cpu.init();
        discover_gpus();
        initialized = true;
    }

    void shutdown() {
        if (!initialized) return;
        nvidia.shutdown();
        amd.shutdown();
        intel_gpu.shutdown();
        cpu.shutdown();
        gpus.clear();
        initialized = false;
    }

    SensorSnapshot read_snapshot() {
        if (refresh_topology_pending.exchange(false, std::memory_order_acq_rel)) {
            nvidia.reset_backoff();
            amd.reset_backoff();
            intel_gpu.reset_backoff();
            discover_gpus();
        }
        if (reset_deltas_pending.exchange(false, std::memory_order_acq_rel)) {
            cpu.reset_deltas();
            intel_gpu.reset_deltas();
        }

        SensorSnapshot snap{};
        snap.timestamp_ms = GetTickCount64();
        snap.cpu_name = cpu.name();
        snap.gpus = gpus;
        snap.pawnio_installed = cpu.pawnio_installed();

        std::optional<std::string> pref_id;
        {
            std::lock_guard lock(gpu_id_mutex);
            pref_id = preferred_gpu_id;
        }

        const GpuDevice* selected = nullptr;
        if (pref_id.has_value()) {
            selected = match_preferred_gpu(gpus, *pref_id);
        }
        if (!selected && !gpus.empty()) {
            selected = &gpus[0];
        }

        LARGE_INTEGER t0{}, t1{}, t2{};
        QueryPerformanceCounter(&t0);
        snap.cpu = cpu.read();
        QueryPerformanceCounter(&t1);

        if (selected) {
            snap.selected_gpu = *selected;
            const bool check_sleep = has_hybrid_gpus && selected->is_discrete;
            if (check_sleep && sleep_guard.is_adapter_sleeping(*selected)) {
                ProcessorReading sleeping{};
                sleeping.is_sleeping = true;
                snap.gpu = sleeping;
            } else {
                if (selected->vendor_id == kVendorNvidia) {
                    snap.gpu = nvidia.read(*selected);
                } else if (selected->vendor_id == kVendorAmd) {
                    snap.gpu = amd.read(*selected);
                } else if (selected->vendor_id == kVendorIntel) {
                    snap.gpu = intel_gpu.read(*selected, &cpu);
                }
            }
        }
        QueryPerformanceCounter(&t2);

        snap.cpu_poll_ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(qpc_freq);
        snap.gpu_poll_ms = static_cast<double>(t2.QuadPart - t1.QuadPart) * 1000.0 / static_cast<double>(qpc_freq);
        return snap;
    }
};

SensorMonitor::SensorMonitor(std::optional<std::string> preferred_gpu_id, PublishCallback publish)
    : state_(std::make_shared<WorkerState>()) {
    state_->publish = std::move(publish);
    state_->preferred_gpu_id = std::move(preferred_gpu_id);
}

SensorMonitor::~SensorMonitor() {
    stop();
}

DWORD WINAPI SensorMonitor::worker_thread_proc(LPVOID param) {
    // Take ownership of the heap-allocated shared_ptr reference so detaching on hung driver is memory-safe.
    std::unique_ptr<std::shared_ptr<WorkerState>> holder(static_cast<std::shared_ptr<WorkerState>*>(param));
    auto st = *holder;
    holder.reset();

    st->init();

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {
        timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }

    if (timer) {
        // Wait 150 ms after init so PDH and RAPL have a valid first delta, then tick strictly every 1000 ms.
        LARGE_INTEGER due{};
        due.QuadPart = -1500000LL;
        SetWaitableTimer(timer, &due, 1000, nullptr, nullptr, FALSE);
    }

    HANDLE waits[2] = {st->wake_event, timer};
    const DWORD wait_count = timer ? 2u : 1u;

    while (!st->stop_requested.load(std::memory_order_acquire)) {
        DWORD res = WaitForMultipleObjects(wait_count, waits, FALSE, timer ? INFINITE : 1000);
        if (res == WAIT_OBJECT_0 || st->stop_requested.load(std::memory_order_acquire)) {
            break;
        }

        if (!st->paused.load(std::memory_order_acquire)) {
            auto snap = st->read_snapshot();
            if (st->publish && !st->stop_requested.load(std::memory_order_acquire)) {
                st->publish(std::move(snap));
            }
        }
    }

    if (timer) {
        CancelWaitableTimer(timer);
        CloseHandle(timer);
    }
    st->shutdown();
    return 0;
}

void SensorMonitor::start() {
    if (worker_handle_) return;
    state_->stop_requested.store(false, std::memory_order_release);
    if (!state_->wake_event) {
        state_->wake_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    } else {
        ResetEvent(state_->wake_event);
    }

    auto* arg = new std::shared_ptr<WorkerState>(state_);
    worker_handle_ = CreateThread(nullptr, 0, &SensorMonitor::worker_thread_proc, arg, 0, nullptr);
    if (!worker_handle_) {
        delete arg;
    }
}

void SensorMonitor::stop() {
    state_->stop_requested.store(true, std::memory_order_release);
    if (state_->wake_event) {
        SetEvent(state_->wake_event);
    }
    if (worker_handle_) {
        // Bounded shutdown wait (1500 ms) so a hung GPU/PawnIO driver can never freeze app exit.
        WaitForSingleObject(worker_handle_, 1500);
        CloseHandle(worker_handle_);
        worker_handle_ = nullptr;
    }
}

void SensorMonitor::select_gpu(std::optional<std::string> id) {
    std::lock_guard lock(state_->gpu_id_mutex);
    state_->preferred_gpu_id = std::move(id);
}

void SensorMonitor::set_paused(bool paused) noexcept {
    const bool prev = state_->paused.exchange(paused, std::memory_order_acq_rel);
    if (prev && !paused) {
        // Resuming after a pause: reset RAPL/IGCL energy delta baselines so stale spans are ignored.
        state_->reset_deltas_pending.store(true, std::memory_order_release);
    }
}

void SensorMonitor::request_topology_refresh() noexcept {
    state_->refresh_topology_pending.store(true, std::memory_order_release);
}

void SensorMonitor::init_sync() {
    state_->init();
}

SensorSnapshot SensorMonitor::read_once_sync() {
    if (!state_->initialized) state_->init();
    return state_->read_snapshot();
}

void SensorMonitor::shutdown_sync() {
    state_->shutdown();
}

} // namespace mmoni
