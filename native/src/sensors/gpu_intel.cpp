#include "sensors/gpu_intel.h"
#include "sensors/cpu_provider.h"
#include "vendor/igcl_api.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mmoni {

namespace {

constexpr uint64_t kRetryBackoffMs = 15000ULL;

using pfn_ctlInit = ctl_result_t(CTL_APICALL*)(ctl_init_args_t*, ctl_api_handle_t*);
using pfn_ctlClose = ctl_result_t(CTL_APICALL*)(ctl_api_handle_t);
using pfn_ctlEnumerateDevices = ctl_result_t(CTL_APICALL*)(ctl_api_handle_t, uint32_t*, ctl_device_adapter_handle_t*);
using pfn_ctlGetDeviceProperties = ctl_result_t(CTL_APICALL*)(ctl_device_adapter_handle_t, ctl_device_adapter_properties_t*);
using pfn_ctlPowerTelemetryGet = ctl_result_t(CTL_APICALL*)(ctl_device_adapter_handle_t, ctl_power_telemetry_t*);

std::optional<double> read_telemetry_item(const ctl_oc_telemetry_item_t& item) {
    if (!item.bSupported) return std::nullopt;
    double v = 0.0;
    switch (item.type) {
        case CTL_DATA_TYPE_INT8: v = static_cast<double>(item.value.data8); break;
        case CTL_DATA_TYPE_UINT8: v = static_cast<double>(item.value.datau8); break;
        case CTL_DATA_TYPE_INT16: v = static_cast<double>(item.value.data16); break;
        case CTL_DATA_TYPE_UINT16: v = static_cast<double>(item.value.datau16); break;
        case CTL_DATA_TYPE_INT32: v = static_cast<double>(item.value.data32); break;
        case CTL_DATA_TYPE_UINT32: v = static_cast<double>(item.value.datau32); break;
        case CTL_DATA_TYPE_INT64: v = static_cast<double>(item.value.data64); break;
        case CTL_DATA_TYPE_UINT64: v = static_cast<double>(item.value.datau64); break;
        case CTL_DATA_TYPE_FLOAT: v = static_cast<double>(item.value.datafloat); break;
        case CTL_DATA_TYPE_DOUBLE: v = item.value.datadouble; break;
        default: return std::nullopt;
    }
    if (!std::isfinite(v)) return std::nullopt;
    return v;
}

} // namespace

IntelGpuProvider::~IntelGpuProvider() {
    shutdown();
}

void IntelGpuProvider::reset_deltas() noexcept {
    for (auto& d : devices_) {
        d.has_prev = false;
    }
}

void IntelGpuProvider::shutdown() {
    if (ready_ && api_handle_ && fn_close_) {
        auto close_fn = reinterpret_cast<pfn_ctlClose>(fn_close_);
        close_fn(static_cast<ctl_api_handle_t>(api_handle_));
    }
    devices_.clear();
    api_handle_ = nullptr;
    fn_close_ = nullptr;
    fn_telemetry_get_ = nullptr;
    ready_ = false;
    last_attempt_ms_ = 0;
    if (dll_) {
        FreeLibrary(dll_);
        dll_ = nullptr;
    }
}

bool IntelGpuProvider::ensure_init() {
    if (ready_) return true;
    const uint64_t now = GetTickCount64();
    if (last_attempt_ms_ != 0 && now - last_attempt_ms_ < kRetryBackoffMs) {
        return false;
    }
    last_attempt_ms_ = now;

    dll_ = LoadLibraryExW(L"ControlLib.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll_) return false;

    auto init_fn = reinterpret_cast<pfn_ctlInit>(GetProcAddress(dll_, "ctlInit"));
    auto close_fn = reinterpret_cast<pfn_ctlClose>(GetProcAddress(dll_, "ctlClose"));
    auto enum_fn = reinterpret_cast<pfn_ctlEnumerateDevices>(GetProcAddress(dll_, "ctlEnumerateDevices"));
    auto props_fn = reinterpret_cast<pfn_ctlGetDeviceProperties>(GetProcAddress(dll_, "ctlGetDeviceProperties"));
    auto telem_fn = reinterpret_cast<pfn_ctlPowerTelemetryGet>(GetProcAddress(dll_, "ctlPowerTelemetryGet"));

    if (!init_fn || !close_fn || !enum_fn || !props_fn || !telem_fn) {
        FreeLibrary(dll_);
        dll_ = nullptr;
        return false;
    }

    ctl_init_args_t args{};
    args.Size = sizeof(args);
    args.Version = 0;
    args.AppVersion = CTL_MAKE_VERSION(CTL_IMPL_MAJOR_VERSION, CTL_IMPL_MINOR_VERSION);
    args.flags = 0;

    ctl_api_handle_t handle = nullptr;
    ctl_result_t res = init_fn(&args, &handle);
    if (res != CTL_RESULT_SUCCESS || !handle) {
        args.flags = CTL_INIT_FLAG_USE_LEVEL_ZERO;
        res = init_fn(&args, &handle);
        if (res != CTL_RESULT_SUCCESS || !handle) {
            FreeLibrary(dll_);
            dll_ = nullptr;
            return false;
        }
    }

    uint32_t count = 0;
    if (enum_fn(handle, &count, nullptr) == CTL_RESULT_SUCCESS && count > 0) {
        std::vector<ctl_device_adapter_handle_t> raw(count);
        if (enum_fn(handle, &count, raw.data()) == CTL_RESULT_SUCCESS) {
            for (uint32_t i = 0; i < count; ++i) {
                ctl_device_adapter_properties_t props{};
                props.Size = sizeof(props);
                props.Version = 2; // Request Version 2 for adapter_bdf when supported
                LUID luid{};
                props.pDeviceID = &luid;
                props.device_id_size = sizeof(luid);
                if (props_fn(raw[i], &props) != CTL_RESULT_SUCCESS) {
                    props = {};
                    props.Size = sizeof(props);
                    props.Version = 0;
                    luid = {};
                    props.pDeviceID = &luid;
                    props.device_id_size = sizeof(luid);
                    if (props_fn(raw[i], &props) != CTL_RESULT_SUCCESS) {
                        continue;
                    }
                }
                if (props.device_type == CTL_DEVICE_TYPE_GRAPHICS) {
                    DeviceDeltaState st{};
                    st.handle = raw[i];
                    st.pci_device_id = props.pci_device_id;
                    st.luid = (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32) |
                              static_cast<uint64_t>(luid.LowPart);
                    if (props.Version >= 2) {
                        st.pci_bus = static_cast<int>(props.adapter_bdf.bus);
                        st.pci_dev = static_cast<int>(props.adapter_bdf.device);
                    }
                    st.name = props.name;
                    devices_.push_back(std::move(st));
                }
            }
        }
    }

    if (devices_.empty()) {
        close_fn(handle);
        FreeLibrary(dll_);
        dll_ = nullptr;
        return false;
    }

    api_handle_ = handle;
    fn_close_ = reinterpret_cast<void*>(close_fn);
    fn_telemetry_get_ = reinterpret_cast<void*>(telem_fn);
    ready_ = true;
    return true;
}

std::optional<ProcessorReading> IntelGpuProvider::read(const GpuDevice& gpu, CpuProvider* cpu_rapl_fallback) {
    ProcessorReading r{};
    bool got_any = false;

    if (ensure_init() && !devices_.empty()) {
        DeviceDeltaState* chosen = nullptr;
        DeviceDeltaState* model_match = nullptr;
        int model_matches = 0;

        for (auto& d : devices_) {
            // 1) Exact 1:1 session LUID match.
            if (gpu.luid != 0 && d.luid != 0 && d.luid == gpu.luid) {
                chosen = &d;
                break;
            }
            // 2) Exact PCI BDF match.
            if (gpu.pci_bus >= 0 && d.pci_bus >= 0 && d.pci_bus == gpu.pci_bus &&
                (gpu.pci_dev < 0 || d.pci_dev < 0 || d.pci_dev == gpu.pci_dev)) {
                chosen = &d;
                break;
            }
            // 3) Model match (only accepted if unambiguous).
            if (gpu.device_id != 0 && d.pci_device_id == gpu.device_id) {
                model_match = &d;
                ++model_matches;
            }
        }

        if (!chosen) {
            if (model_matches == 1) chosen = model_match;
            else if (devices_.size() == 1 && gpu.luid == 0 && gpu.pci_bus < 0) chosen = &devices_[0];
        }

        if (chosen) {
            auto telem_fn = reinterpret_cast<pfn_ctlPowerTelemetryGet>(fn_telemetry_get_);
            ctl_power_telemetry_t t{};
            t.Size = sizeof(t);
            t.Version = 0;

            if (telem_fn(static_cast<ctl_device_adapter_handle_t>(chosen->handle), &t) == CTL_RESULT_SUCCESS) {
                if (auto temp = read_telemetry_item(t.gpuCurrentTemperature)) {
                    if (*temp >= -20.0 && *temp <= 140.0) {
                        r.temperature = static_cast<float>(*temp);
                        got_any = true;
                    }
                }
                if (auto freq = read_telemetry_item(t.gpuCurrentClockFrequency)) {
                    if (*freq >= 0.0 && *freq < 10000.0) {
                        r.clock = static_cast<float>(*freq);
                        got_any = true;
                    }
                }

                if (t.gpuTemperatureLimited) r.throttle |= ThrottleThermal;
                if (t.gpuPowerLimited) r.throttle |= ThrottlePower;

                const auto ts_opt = read_telemetry_item(t.timeStamp);
                const auto energy_opt = read_telemetry_item(t.gpuEnergyCounter);
                const auto act_opt = read_telemetry_item(t.globalActivityCounter).has_value()
                    ? read_telemetry_item(t.globalActivityCounter)
                    : read_telemetry_item(t.renderComputeActivityCounter);

                if (ts_opt.has_value()) {
                    const double ts = *ts_opt;
                    if (chosen->has_prev && ts > chosen->prev_ts) {
                        const double dt = ts - chosen->prev_ts;
                        if (dt > 0.05 && dt < 30.0) {
                            if (energy_opt.has_value() && *energy_opt >= chosen->prev_energy) {
                                const double watts = (*energy_opt - chosen->prev_energy) / dt;
                                if (std::isfinite(watts) && watts >= 0.0 && watts < 600.0) {
                                    r.power = static_cast<float>(watts);
                                    got_any = true;
                                }
                            }
                            if (act_opt.has_value() && *act_opt >= chosen->prev_activity) {
                                const double util = ((*act_opt - chosen->prev_activity) / dt) * 100.0;
                                if (std::isfinite(util)) {
                                    r.load = std::clamp(static_cast<float>(util), 0.0f, 100.0f);
                                    got_any = true;
                                }
                            }
                        }
                    }
                    chosen->prev_ts = ts;
                    if (energy_opt.has_value()) chosen->prev_energy = *energy_opt;
                    if (act_opt.has_value()) chosen->prev_activity = *act_opt;
                    chosen->has_prev = true;
                }
            }
        }
    }

    // RAPL PP1 (0x641) measures CPU-package integrated GT power ONLY — never apply to discrete Intel Arc GPUs.
    if (!r.power.has_value() && !gpu.is_discrete && cpu_rapl_fallback) {
        if (auto rapl_gt = cpu_rapl_fallback->read_intel_igpu_rapl_watts()) {
            r.power = rapl_gt;
            got_any = true;
        }
    }

    if (!got_any) return std::nullopt;
    return r;
}

} // namespace mmoni
