#pragma once

#include <windows.h>
#include <cstdint>
#include <optional>

namespace mmoni {

class PawnIoClient {
public:
    PawnIoClient() = default;
    ~PawnIoClient();

    PawnIoClient(const PawnIoClient&) = delete;
    PawnIoClient& operator=(const PawnIoClient&) = delete;

    static bool is_installed();

    bool load_embedded_module(UINT resource_id);
    void close();

    bool is_ready() const noexcept { return handle_ != nullptr; }

    std::optional<uint64_t> read_msr(uint32_t index) const;
    std::optional<uint32_t> read_smn(uint32_t address) const;

private:
    static bool ensure_library_loaded();

    HANDLE handle_ = nullptr;
};

} // namespace mmoni
