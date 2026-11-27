#include "platform/settings.h"

#include <windows.h>
#include <sddl.h>
#include <shlobj.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

namespace mmoni {

std::wstring utf8_to_wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

std::string wide_to_utf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

bool is_process_elevated() noexcept {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

std::wstring get_current_user_sid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return L"CurrentUser";
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size == 0) {
        CloseHandle(token);
        return L"CurrentUser";
    }
    std::vector<uint8_t> buf(size);
    std::wstring result = L"CurrentUser";
    if (GetTokenInformation(token, TokenUser, buf.data(), size, &size)) {
        auto* user = reinterpret_cast<TOKEN_USER*>(buf.data());
        LPWSTR sid_str = nullptr;
        if (ConvertSidToStringSidW(user->User.Sid, &sid_str) && sid_str) {
            result = sid_str;
            LocalFree(sid_str);
        }
    }
    CloseHandle(token);
    return result;
}

std::wstring get_executable_path() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD copied = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (copied == 0) return {};
        if (copied < path.size() - 1) {
            path.resize(copied);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::wstring UserSettings::settings_path() {
    PWSTR local_app_data = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local_app_data)) && local_app_data) {
        dir = local_app_data;
        CoTaskMemFree(local_app_data);
    } else {
        wchar_t env[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", env, MAX_PATH) > 0) {
            dir = env;
        }
    }
    if (dir.empty()) return L"settings.json";
    return dir + L"\\Mmoni\\settings.json";
}

namespace {

class JsonReader {
public:
    explicit JsonReader(const std::string& src) : s_(src) {}

    void skip_ws() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n')) {
            ++pos_;
        }
    }

    bool consume(char c) {
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool peek(char c) {
        skip_ws();
        return pos_ < s_.size() && s_[pos_] == c;
    }

    bool parse_string(std::string& out) {
        skip_ws();
        if (pos_ >= s_.size() || s_[pos_] != '"') return false;
        ++pos_;
        out.clear();
        while (pos_ < s_.size()) {
            char ch = s_[pos_++];
            if (ch == '"') return true;
            if (ch == '\\' && pos_ < s_.size()) {
                char esc = s_[pos_++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (pos_ + 4 <= s_.size()) {
                            unsigned int cp = 0;
                            for (int i = 0; i < 4; ++i) {
                                char h = s_[pos_++];
                                cp <<= 4;
                                if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                                else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                            }
                            if (cp < 0x80) out.push_back(static_cast<char>(cp));
                            else if (cp < 0x800) {
                                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                            } else {
                                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                            }
                        }
                        break;
                    }
                    default: out.push_back(esc); break;
                }
            } else {
                out.push_back(ch);
            }
        }
        return false;
    }

    bool parse_null() {
        skip_ws();
        if (s_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            return true;
        }
        return false;
    }

    bool parse_bool(bool& out) {
        skip_ws();
        if (s_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            out = true;
            return true;
        }
        if (s_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            out = false;
            return true;
        }
        return false;
    }

    bool parse_number(double& out) {
        skip_ws();
        if (pos_ >= s_.size()) return false;
        const char* start = s_.c_str() + pos_;
        char* end = nullptr;
        out = std::strtod(start, &end);
        if (end == start) return false;
        pos_ = static_cast<size_t>(end - s_.c_str());
        return true;
    }

    void skip_value() {
        skip_ws();
        if (pos_ >= s_.size()) return;
        char c = s_[pos_];
        if (c == '"') {
            std::string dummy;
            parse_string(dummy);
        } else if (c == '{') {
            ++pos_;
            while (pos_ < s_.size() && !consume('}')) {
                std::string k;
                if (!parse_string(k)) break;
                consume(':');
                skip_value();
                consume(',');
            }
        } else if (c == '[') {
            ++pos_;
            while (pos_ < s_.size() && !consume(']')) {
                skip_value();
                consume(',');
            }
        } else {
            while (pos_ < s_.size() && s_[pos_] != ',' && s_[pos_] != '}' && s_[pos_] != ']' &&
                   s_[pos_] != ' ' && s_[pos_] != '\r' && s_[pos_] != '\n' && s_[pos_] != '\t') {
                ++pos_;
            }
        }
    }

private:
    const std::string& s_;
    size_t pos_ = 0;
};

std::string json_escape(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    out.push_back('"');
    for (unsigned char c : input) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}

std::string format_json_float(float v) {
    char buf[32];
    if (std::fabs(v - std::round(v)) < 1e-4f) {
        std::snprintf(buf, sizeof(buf), "%.0f", v);
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f", v);
    }
    return buf;
}

const char* theme_to_str(VisualTheme t) {
    return t == VisualTheme::PrecisionDark ? "PrecisionDark" : "LiquidGlass";
}

const char* layout_to_str(ChartLayoutMode m) {
    return m == ChartLayoutMode::Combined ? "Combined" : "Split";
}

const char* collapsed_to_str(CollapsedDisplayMode m) {
    switch (m) {
        case CollapsedDisplayMode::TempLoad: return "TempLoad";
        case CollapsedDisplayMode::TempOnly: return "TempOnly";
        case CollapsedDisplayMode::TempPowerSpark: return "TempPowerSpark";
        default: return "TempPower";
    }
}

const char* range_to_str(TimeRange r) {
    switch (r) {
        case TimeRange::Minutes5: return "Minutes5";
        case TimeRange::Minutes15: return "Minutes15";
        default: return "Seconds60";
    }
}

} // namespace

UserSettings UserSettings::load() {
    UserSettings s{};
    const auto path = settings_path();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return s;
    }

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(file, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 1024 * 1024) {
        CloseHandle(file);
        return s;
    }

    std::string content(static_cast<size_t>(sz.QuadPart), '\0');
    DWORD read = 0;
    BOOL ok = ReadFile(file, content.data(), static_cast<DWORD>(content.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok || read == 0) {
        return s;
    }
    content.resize(read);

    // Strip UTF-8 BOM if present.
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3);
    }

    JsonReader r(content);
    if (!r.consume('{')) return s;

    while (!r.peek('}')) {
        std::string key;
        if (!r.parse_string(key)) break;
        if (!r.consume(':')) break;

        if (key == "Left") {
            double v = 0;
            if (r.parse_null()) s.left.reset();
            else if (r.parse_number(v)) s.left = static_cast<int>(std::lround(v));
            else r.skip_value();
        } else if (key == "Top") {
            double v = 0;
            if (r.parse_null()) s.top.reset();
            else if (r.parse_number(v)) s.top = static_cast<int>(std::lround(v));
            else r.skip_value();
        } else if (key == "ClickThrough") {
            bool b = false;
            if (r.parse_bool(b)) s.click_through = b;
            else r.skip_value();
        } else if (key == "AutoExpandOnHover") {
            bool b = true;
            if (r.parse_bool(b)) s.auto_expand_on_hover = b;
            else r.skip_value();
        } else if (key == "AutoHideInFullscreen") {
            bool b = true;
            if (r.parse_bool(b)) s.auto_hide_in_fullscreen = b;
            else r.skip_value();
        } else if (key == "GpuId") {
            std::string v;
            if (r.parse_null()) s.gpu_id.reset();
            else if (r.parse_string(v)) s.gpu_id = v;
            else r.skip_value();
        } else if (key == "Theme") {
            std::string v;
            double n = 0;
            if (r.parse_string(v)) {
                s.theme = (v == "PrecisionDark") ? VisualTheme::PrecisionDark : VisualTheme::LiquidGlass;
            } else if (r.parse_number(n)) {
                s.theme = (static_cast<int>(n) == 1) ? VisualTheme::PrecisionDark : VisualTheme::LiquidGlass;
            } else r.skip_value();
        } else if (key == "ChartLayout") {
            std::string v;
            double n = 0;
            if (r.parse_string(v)) {
                s.chart_layout = (v == "Combined") ? ChartLayoutMode::Combined : ChartLayoutMode::Split;
            } else if (r.parse_number(n)) {
                s.chart_layout = (static_cast<int>(n) == 1) ? ChartLayoutMode::Combined : ChartLayoutMode::Split;
            } else r.skip_value();
        } else if (key == "CollapsedMode") {
            std::string v;
            double n = 0;
            if (r.parse_string(v)) {
                if (v == "TempLoad") s.collapsed_mode = CollapsedDisplayMode::TempLoad;
                else if (v == "TempOnly") s.collapsed_mode = CollapsedDisplayMode::TempOnly;
                else if (v == "TempPowerSpark") s.collapsed_mode = CollapsedDisplayMode::TempPowerSpark;
                else s.collapsed_mode = CollapsedDisplayMode::TempPower;
            } else if (r.parse_number(n)) {
                int iv = static_cast<int>(n);
                if (iv == 1) s.collapsed_mode = CollapsedDisplayMode::TempLoad;
                else if (iv == 2) s.collapsed_mode = CollapsedDisplayMode::TempOnly;
                else if (iv == 3) s.collapsed_mode = CollapsedDisplayMode::TempPowerSpark;
                else s.collapsed_mode = CollapsedDisplayMode::TempPower;
            } else r.skip_value();
        } else if (key == "HistoryRange") {
            std::string v;
            double n = 0;
            if (r.parse_string(v)) {
                if (v == "Minutes5") s.history_range = TimeRange::Minutes5;
                else if (v == "Minutes15") s.history_range = TimeRange::Minutes15;
                else s.history_range = TimeRange::Seconds60;
            } else if (r.parse_number(n)) {
                int iv = static_cast<int>(n);
                if (iv == 300) s.history_range = TimeRange::Minutes5;
                else if (iv == 900) s.history_range = TimeRange::Minutes15;
                else s.history_range = TimeRange::Seconds60;
            } else r.skip_value();
        } else if (key == "CpuWarmTemp") {
            double v = 0;
            if (r.parse_number(v) && std::isfinite(v)) s.cpu_warm_temp = static_cast<float>(v);
            else r.skip_value();
        } else if (key == "CpuHotTemp") {
            double v = 0;
            if (r.parse_number(v) && std::isfinite(v)) s.cpu_hot_temp = static_cast<float>(v);
            else r.skip_value();
        } else if (key == "GpuWarmTemp") {
            double v = 0;
            if (r.parse_number(v) && std::isfinite(v)) s.gpu_warm_temp = static_cast<float>(v);
            else r.skip_value();
        } else if (key == "GpuHotTemp") {
            double v = 0;
            if (r.parse_number(v) && std::isfinite(v)) s.gpu_hot_temp = static_cast<float>(v);
            else r.skip_value();
        } else {
            r.skip_value();
        }

        if (!r.consume(',')) break;
    }

    return s;
}

bool UserSettings::save(std::wstring* error_out) const {
    const auto path = settings_path();
    const auto slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        const auto dir = path.substr(0, slash);
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    }

    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"Left\": " << (left.has_value() ? std::to_string(*left) : "null") << ",\n";
    oss << "  \"Top\": " << (top.has_value() ? std::to_string(*top) : "null") << ",\n";
    oss << "  \"ClickThrough\": " << (click_through ? "true" : "false") << ",\n";
    oss << "  \"AutoExpandOnHover\": " << (auto_expand_on_hover ? "true" : "false") << ",\n";
    oss << "  \"AutoHideInFullscreen\": " << (auto_hide_in_fullscreen ? "true" : "false") << ",\n";
    oss << "  \"GpuId\": " << (gpu_id.has_value() ? json_escape(*gpu_id) : "null") << ",\n";
    oss << "  \"Theme\": \"" << theme_to_str(theme) << "\",\n";
    oss << "  \"ChartLayout\": \"" << layout_to_str(chart_layout) << "\",\n";
    oss << "  \"CollapsedMode\": \"" << collapsed_to_str(collapsed_mode) << "\",\n";
    oss << "  \"HistoryRange\": \"" << range_to_str(history_range) << "\",\n";
    oss << "  \"CpuWarmTemp\": " << format_json_float(cpu_warm_temp) << ",\n";
    oss << "  \"CpuHotTemp\": " << format_json_float(cpu_hot_temp) << ",\n";
    oss << "  \"GpuWarmTemp\": " << format_json_float(gpu_warm_temp) << ",\n";
    oss << "  \"GpuHotTemp\": " << format_json_float(gpu_hot_temp) << "\n";
    oss << "}\n";

    const std::string json = oss.str();
    const std::wstring tmp_path = path + L".tmp";

    HANDLE file = CreateFileW(tmp_path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error_out) *error_out = L"Could not open settings file for writing.";
        return false;
    }

    DWORD written = 0;
    BOOL ok = WriteFile(file, json.data(), static_cast<DWORD>(json.size()), &written, nullptr);
    CloseHandle(file);
    if (!ok || written != json.size()) {
        DeleteFileW(tmp_path.c_str());
        if (error_out) *error_out = L"Could not write settings data.";
        return false;
    }

    if (!MoveFileExW(tmp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp_path.c_str());
        if (error_out) *error_out = L"Could not replace settings file.";
        return false;
    }

    return true;
}

} // namespace mmoni
