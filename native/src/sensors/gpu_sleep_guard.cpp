#include "sensors/gpu_sleep_guard.h"

#include <windows.h>
#include <cfgmgr32.h>
#include <devpropdef.h>
#include <initguid.h>
#include <devpkey.h>

#include <algorithm>
#include <cwctype>

namespace mmoni {

namespace {

bool parse_hex_after_token(const std::wstring& s, const wchar_t* token, size_t digits, uint32_t& out) {
    const size_t tlen = wcslen(token);
    for (size_t i = 0; i + tlen + digits <= s.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < tlen; ++j) {
            if (towupper(s[i + j]) != towupper(token[j])) {
                match = false;
                break;
            }
        }
        if (!match) continue;
        uint32_t val = 0;
        bool ok = true;
        for (size_t d = 0; d < digits; ++d) {
            wchar_t c = towupper(s[i + tlen + d]);
            val <<= 4;
            if (c >= L'0' && c <= L'9') val |= static_cast<uint32_t>(c - L'0');
            else if (c >= L'A' && c <= L'F') val |= static_cast<uint32_t>(c - L'A' + 10);
            else { ok = false; break; }
        }
        if (ok) {
            out = val;
            return true;
        }
    }
    return false;
}

} // namespace

const std::vector<DisplayPciNode>& GpuSleepGuard::get_cached_nodes() {
    const uint64_t now = GetTickCount64();
    if (!cached_nodes_.empty() && last_scan_tick_ != 0 && now - last_scan_tick_ < 15000ULL) {
        return cached_nodes_;
    }

    last_scan_tick_ = now;
    cached_nodes_.clear();

    constexpr wchar_t kDisplayClassGuid[] = L"{4d36e968-e325-11ce-bfc1-08002be10318}";
    constexpr ULONG kFlags = CM_GETIDLIST_FILTER_CLASS | CM_GETIDLIST_FILTER_PRESENT;

    ULONG len = 0;
    if (CM_Get_Device_ID_List_SizeW(&len, kDisplayClassGuid, kFlags) != CR_SUCCESS || len <= 1) {
        return cached_nodes_;
    }

    std::vector<wchar_t> buf(len);
    if (CM_Get_Device_ID_ListW(kDisplayClassGuid, buf.data(), len, kFlags) != CR_SUCCESS) {
        return cached_nodes_;
    }

    const wchar_t* p = buf.data();
    while (*p != L'\0') {
        DisplayPciNode node{};
        node.instance_id = p;
        parse_hex_after_token(node.instance_id, L"VEN_", 4, node.vendor_id);
        parse_hex_after_token(node.instance_id, L"DEV_", 4, node.device_id);
        parse_hex_after_token(node.instance_id, L"SUBSYS_", 8, node.subsys_id);

        DEVINST dev_inst = 0;
        if (CM_Locate_DevNodeW(&dev_inst, const_cast<DEVINSTID_W>(node.instance_id.c_str()), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {
            DEVPROPTYPE ptype = 0;
            uint32_t bus_num = 0;
            ULONG sz = sizeof(bus_num);
            if (CM_Get_DevNode_PropertyW(dev_inst, &DEVPKEY_Device_BusNumber, &ptype,
                                         reinterpret_cast<PBYTE>(&bus_num), &sz, 0) == CR_SUCCESS &&
                sz == sizeof(bus_num)) {
                node.pci_bus = static_cast<int>(bus_num);
            }

            uint32_t addr = 0;
            sz = sizeof(addr);
            if (CM_Get_DevNode_PropertyW(dev_inst, &DEVPKEY_Device_Address, &ptype,
                                         reinterpret_cast<PBYTE>(&addr), &sz, 0) == CR_SUCCESS &&
                sz == sizeof(addr)) {
                node.pci_dev = static_cast<int>((addr >> 16) & 0xFFFFu);
                node.pci_func = static_cast<int>(addr & 0xFFFFu);
            }
        }

        cached_nodes_.push_back(std::move(node));
        p += wcslen(p) + 1;
    }

    return cached_nodes_;
}

std::vector<DisplayPciNode> GpuSleepGuard::enumerate_display_nodes() {
    invalidate_cache();
    return get_cached_nodes();
}

bool GpuSleepGuard::is_adapter_sleeping(const GpuDevice& gpu) {
    const auto& nodes = get_cached_nodes();

    bool found_any = false;
    bool all_sleeping = true;

    for (const auto& node : nodes) {
        if (node.vendor_id != gpu.vendor_id) continue;
        if (gpu.device_id != 0 && node.device_id != 0 && node.device_id != gpu.device_id) continue;
        if (gpu.pci_bus >= 0 && node.pci_bus >= 0) {
            if (node.pci_bus != gpu.pci_bus) continue;
            if (gpu.pci_dev >= 0 && node.pci_dev >= 0 && node.pci_dev != gpu.pci_dev) continue;
        }

        found_any = true;

        DEVINST dev_inst = 0;
        if (CM_Locate_DevNodeW(&dev_inst, const_cast<DEVINSTID_W>(node.instance_id.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
            all_sleeping = false;
            continue;
        }

        BYTE prop_buf[64]{};
        ULONG prop_size = sizeof(prop_buf);
        DEVPROPTYPE prop_type = 0;
        CONFIGRET cr = CM_Get_DevNode_PropertyW(
            dev_inst,
            &DEVPKEY_Device_PowerData,
            &prop_type,
            prop_buf,
            &prop_size,
            0);

        if (cr != CR_SUCCESS || prop_size < 8) {
            all_sleeping = false;
            continue;
        }

        // CM_POWER_DATA: DWORD PD_Size (0..3), DEVICE_POWER_STATE PD_MostRecentPowerState (4..7)
        const int32_t power_state = *reinterpret_cast<const int32_t*>(prop_buf + 4);
        if (power_state < PowerDeviceD3) {
            all_sleeping = false;
        }
    }

    return found_any && all_sleeping;
}

} // namespace mmoni
