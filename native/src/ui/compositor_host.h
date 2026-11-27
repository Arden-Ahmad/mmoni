#pragma once

#include "ui/d2d_context.h"

#include <windows.h>
#include <d2d1_1.h>
#include <functional>
#include <memory>

namespace mmoni {

enum class RenderStatus : uint8_t {
    Ok = 0,
    DeviceLost = 1,
    Failed = 2
};

class CompositorHost {
public:
    CompositorHost();
    ~CompositorHost();

    CompositorHost(const CompositorHost&) = delete;
    CompositorHost& operator=(const CompositorHost&) = delete;

    bool init(HWND hwnd, D2DContext* d2d);
    void shutdown();

    RenderStatus resize_surface(int pixel_width, int pixel_height, float dpi_x, float dpi_y);

    // Positions & clips the optional Win11 HostBackdropBrush behind the exact active rounded chrome rect.
    void update_backdrop(bool visible, float left_px, float top_px, float width_px, float height_px, float radius_px);

    // Begins a retained Direct2D draw pass into the DirectComposition surface and checks EndDraw HRESULT.
    RenderStatus render(const std::function<void(ID2D1DeviceContext*, float dpi_x, float dpi_y)>& draw_fn);

    void set_opacity(float opacity);
    bool is_ready() const noexcept;
    bool backdrop_supported() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mmoni
