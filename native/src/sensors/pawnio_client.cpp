#include "sensors/pawnio_client.h"

#include <shlobj.h>
#include <cwctype>
#include <mutex>
#include <string>

namespace mmoni {

namespace {

using pfn_pawnio_open = HRESULT(STDAPICALLTYPE*)(PHANDLE handle);
using pfn_pawnio_load = HRESULT(STDAPICALLTYPE*)(HANDLE handle, const UCHAR* blob, SIZE_T size);
using pfn_pawnio_execute = HRESULT(STDAPICALLTYPE*)(
    HANDLE handle,
    PCSTR name,
    const ULONG64* in,
    SIZE_T in_size,
    PULONG64 out,
    SIZE_T out_size,
    PSIZE_T return_size);
using pfn_pawnio_close = HRESULT(STDAPICALLTYPE*)(HANDLE handle);

struct PawnIoApi {
    HMODULE dll = nullptr;
    pfn_pawnio_open open = nullptr;
    pfn_pawnio_load load = nullptr;
    pfn_pawnio_execute execute = nullptr;
    pfn_pawnio_close close = nullptr;
    uint64_t last_attempt_tick = 0;
};

std::mutex g_api_mutex;
PawnIoApi g_api{};

bool is_under_trusted_root(const std::wstring& candidate, REFKNOWNFOLDERID folder_id) {
    PWSTR root = nullptr;
    if (FAILED(SHGetKnownFolderPath(folder_id, 0, nullptr, &root)) || !root) {
        return false;
    }
    std::wstring prefix(root);
    CoTaskMemFree(root);
    if (!prefix.empty() && prefix.back() != L'\\') prefix.push_back(L'\\');
    if (candidate.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (towupper(candidate[i]) != towupper(prefix[i])) return false;
    }
    return true;
}

bool is_trusted_pawnio_path(const std::wstring& path) {
    if (path.empty()) return false;
    if (path.find(L"..") != std::wstring::npos) return false;
    return is_under_trusted_root(path, FOLDERID_ProgramFiles) ||
           is_under_trusted_root(path, FOLDERID_ProgramFilesX86) ||
           is_under_trusted_root(path, FOLDERID_System);
}

std::wstring find_pawnio_dll_path() {
    HKEY key = nullptr;
    const wchar_t* reg_paths[] = {
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO"
    };

    for (const wchar_t* reg_path : reg_paths) {
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, reg_path, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
            wchar_t dir[MAX_PATH]{};
            DWORD size = sizeof(dir);
            DWORD type = 0;
            LONG res = RegQueryValueExW(key, L"InstallLocation", nullptr, &type, reinterpret_cast<LPBYTE>(dir), &size);
            RegCloseKey(key);
            if (res == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && dir[0] != L'\0') {
                std::wstring candidate = dir;
                if (!candidate.empty() && candidate.back() != L'\\') candidate.push_back(L'\\');
                candidate += L"PawnIOLib.dll";
                if (is_trusted_pawnio_path(candidate) &&
                    GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    return candidate;
                }
            }
        }
    }

    PWSTR pf = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &pf)) && pf) {
        std::wstring candidate = std::wstring(pf) + L"\\PawnIO\\PawnIOLib.dll";
        CoTaskMemFree(pf);
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return candidate;
        }
    }

    // Never fall back to a bare filename in an elevated process (prevents DLL hijacking).
    return {};
}

} // namespace

PawnIoClient::~PawnIoClient() {
    close();
}

bool PawnIoClient::ensure_library_loaded() {
    std::lock_guard lock(g_api_mutex);
    if (g_api.dll && g_api.open && g_api.load && g_api.execute && g_api.close) {
        return true;
    }

    const uint64_t now = GetTickCount64();
    if (g_api.last_attempt_tick != 0 && now - g_api.last_attempt_tick < 15000ULL) {
        return false;
    }
    g_api.last_attempt_tick = now;

    const std::wstring path = find_pawnio_dll_path();
    if (path.empty()) return false;

    g_api.dll = LoadLibraryExW(
        path.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_api.dll) return false;

    g_api.open = reinterpret_cast<pfn_pawnio_open>(GetProcAddress(g_api.dll, "pawnio_open"));
    g_api.load = reinterpret_cast<pfn_pawnio_load>(GetProcAddress(g_api.dll, "pawnio_load"));
    g_api.execute = reinterpret_cast<pfn_pawnio_execute>(GetProcAddress(g_api.dll, "pawnio_execute"));
    g_api.close = reinterpret_cast<pfn_pawnio_close>(GetProcAddress(g_api.dll, "pawnio_close"));

    if (!g_api.open || !g_api.load || !g_api.execute || !g_api.close) {
        FreeLibrary(g_api.dll);
        g_api = {};
        return false;
    }
    return true;
}

bool PawnIoClient::is_installed() {
    if (!ensure_library_loaded()) return false;
    HANDLE h = nullptr;
    HRESULT hr = g_api.open(&h);
    if (SUCCEEDED(hr) && h) {
        g_api.close(h);
        return true;
    }
    // Check service presence as secondary signal if non-admin couldn't open device.
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm) {
        SC_HANDLE svc = OpenServiceW(scm, L"PawnIO", SERVICE_QUERY_STATUS);
        bool exists = (svc != nullptr);
        if (svc) CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        if (exists) return true;
    }
    return false;
}

bool PawnIoClient::load_embedded_module(UINT resource_id) {
    close();
    if (!ensure_library_loaded()) return false;

    HMODULE self = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(self, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    if (!res) return false;

    DWORD size = SizeofResource(self, res);
    HGLOBAL loaded = LoadResource(self, res);
    if (!loaded || size == 0) return false;

    const auto* blob = static_cast<const UCHAR*>(LockResource(loaded));
    if (!blob) return false;

    HANDLE h = nullptr;
    if (FAILED(g_api.open(&h)) || !h) {
        return false;
    }

    if (FAILED(g_api.load(h, blob, size))) {
        g_api.close(h);
        return false;
    }

    handle_ = h;
    return true;
}

void PawnIoClient::close() {
    if (handle_ && g_api.close) {
        g_api.close(handle_);
        handle_ = nullptr;
    }
}

std::optional<uint64_t> PawnIoClient::read_msr(uint32_t index) const {
    if (!handle_ || !g_api.execute) return std::nullopt;
    ULONG64 in[1] = {static_cast<ULONG64>(index)};
    ULONG64 out[1] = {0};
    SIZE_T returned = 0;
    HRESULT hr = g_api.execute(handle_, "ioctl_read_msr", in, 1, out, 1, &returned);
    if (FAILED(hr) || returned < 1) return std::nullopt;
    return out[0];
}

std::optional<uint32_t> PawnIoClient::read_smn(uint32_t address) const {
    if (!handle_ || !g_api.execute) return std::nullopt;
    ULONG64 in[1] = {static_cast<ULONG64>(address)};
    ULONG64 out[1] = {0};
    SIZE_T returned = 0;
    HRESULT hr = g_api.execute(handle_, "ioctl_read_smn", in, 1, out, 1, &returned);
    if (FAILED(hr) || returned < 1) return std::nullopt;
    return static_cast<uint32_t>(out[0] & 0xFFFFFFFFULL);
}

} // namespace mmoni
