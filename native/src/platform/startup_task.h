#pragma once

#include <string>

namespace mmoni {

class StartupTask {
public:
    static bool is_enabled();
    static bool set_enabled(bool enabled, std::wstring* error_out = nullptr);
    static void migrate_legacy_if_needed();
};

} // namespace mmoni
