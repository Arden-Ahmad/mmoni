#include "ui/compositor_host.h"

#include <DispatcherQueue.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.UI.Composition.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;
namespace wuc = winrt::Windows::UI::Composition;
namespace wucd = winrt::Windows::UI::Composition::Desktop;
namespace wfn = winrt::Windows::Foundation::Numerics;

namespace mmoni {

namespace {

using pfn_CreateDispatcherQueueController = HRESULT(WINAPI*)(
    DispatcherQueueOptions options,
    ABI::Windows::System::IDispatcherQueueController** dispatcherQueueController);

bool is_device_lost_hr(HRESULT hr) noexcept {
    return hr == DXGI_ERROR_DEVICE_REMOVED ||
           hr == DXGI_ERROR_DEVICE_RESET ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR ||
           hr == D2DERR_RECREATE_TARGET ||
           hr == RO_E_CLOSED;
}

} // namespace

struct CompositorHost::Impl {
    HWND hwnd = nullptr;
    D2DContext* d2d = nullptr;
    HMODULE core_messaging_dll = nullptr;
    winrt::Windows::System::DispatcherQueueController dq_controller{nullptr};

    wuc::Compositor compositor{nullptr};
    wucd::DesktopWindowTarget target{nullptr};
    wuc::ContainerVisual root{nullptr};

    bool backdrop_supported = false;
    wuc::SpriteVisual backdrop_visual{nullptr};
    wuc::CompositionRoundedRectangleGeometry backdrop_geom{nullptr};
    wuc::CompositionGeometricClip backdrop_clip{nullptr};

    wuc::CompositionGraphicsDevice graphics_device{nullptr};
    wuc::CompositionDrawingSurface drawing_surface{nullptr};
    wuc::CompositionSurfaceBrush surface_brush{nullptr};
    wuc::SpriteVisual surface_visual{nullptr};

    int surface_w = 0;
    int surface_h = 0;
    float dpi_x = 96.0f;
    float dpi_y = 96.0f;

    bool last_bd_vis = false;
    float last_bd_l = -1.0f, last_bd_t = -1.0f, last_bd_w = -1.0f, last_bd_h = -1.0f, last_bd_r = -1.0f;
};

CompositorHost::CompositorHost() : impl_(std::make_unique<Impl>()) {}

CompositorHost::~CompositorHost() {
    shutdown();
}

bool CompositorHost::init(HWND hwnd, D2DContext* d2d) {
    shutdown();
    if (!hwnd || !d2d || !d2d->is_ready()) return false;

    impl_->hwnd = hwnd;
    impl_->d2d = d2d;

    try {
        if (!winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) {
            impl_->core_messaging_dll = LoadLibraryExW(L"CoreMessaging.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!impl_->core_messaging_dll) return false;

            auto create_dq = reinterpret_cast<pfn_CreateDispatcherQueueController>(
                GetProcAddress(impl_->core_messaging_dll, "CreateDispatcherQueueController"));
            if (!create_dq) return false;

            DispatcherQueueOptions opts{};
            opts.dwSize = sizeof(opts);
            opts.threadType = DQTYPE_THREAD_CURRENT;
            opts.apartmentType = DQTAT_COM_NONE;

            ABI::Windows::System::IDispatcherQueueController* raw_ctrl = nullptr;
            if (FAILED(create_dq(opts, &raw_ctrl)) || !raw_ctrl) {
                return false;
            }
            winrt::attach_abi(impl_->dq_controller, raw_ctrl);
        }

        impl_->compositor = wuc::Compositor();

        auto desktop_interop = impl_->compositor.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
        ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget* raw_target = nullptr;
        if (FAILED(desktop_interop->CreateDesktopWindowTarget(hwnd, TRUE, &raw_target)) || !raw_target) {
            return false;
        }
        winrt::attach_abi(impl_->target, raw_target);

        impl_->root = impl_->compositor.CreateContainerVisual();
        impl_->target.Root(impl_->root);

        // Layer 1 (optional): Native Win11 HostBackdropBrush. Non-fatal if unavailable so Precision Dark always works.
        try {
            auto brush = impl_->compositor.CreateHostBackdropBrush();
            impl_->backdrop_visual = impl_->compositor.CreateSpriteVisual();
            impl_->backdrop_visual.Brush(brush);
            impl_->backdrop_geom = impl_->compositor.CreateRoundedRectangleGeometry();
            impl_->backdrop_clip = impl_->compositor.CreateGeometricClip(impl_->backdrop_geom);
            impl_->backdrop_visual.Clip(impl_->backdrop_clip);
            impl_->backdrop_visual.IsVisible(false);
            impl_->root.Children().InsertAtBottom(impl_->backdrop_visual);
            impl_->backdrop_supported = true;
        } catch (...) {
            impl_->backdrop_supported = false;
            impl_->backdrop_clip = nullptr;
            impl_->backdrop_geom = nullptr;
            impl_->backdrop_visual = nullptr;
        }

        // Layer 2: Direct2D / DirectWrite retained drawing surface on iGPU-pinned DX11 device.
        auto comp_interop = impl_->compositor.as<ABI::Windows::UI::Composition::ICompositorInterop>();
        ABI::Windows::UI::Composition::ICompositionGraphicsDevice* raw_gfx = nullptr;
        if (FAILED(comp_interop->CreateGraphicsDevice(d2d->d2d_device(), &raw_gfx)) || !raw_gfx) {
            return false;
        }
        winrt::attach_abi(impl_->graphics_device, raw_gfx);

        impl_->surface_visual = impl_->compositor.CreateSpriteVisual();
        impl_->surface_brush = impl_->compositor.CreateSurfaceBrush();
        impl_->surface_brush.Stretch(wuc::CompositionStretch::None);
        impl_->surface_brush.HorizontalAlignmentRatio(0.0f);
        impl_->surface_brush.VerticalAlignmentRatio(0.0f);
        impl_->surface_visual.Brush(impl_->surface_brush);
        impl_->root.Children().InsertAtTop(impl_->surface_visual);

        return true;
    } catch (...) {
        shutdown();
        return false;
    }
}

void CompositorHost::shutdown() {
    if (!impl_) return;
    try {
        impl_->surface_visual = nullptr;
        impl_->surface_brush = nullptr;
        impl_->drawing_surface = nullptr;
        impl_->graphics_device = nullptr;
        impl_->backdrop_clip = nullptr;
        impl_->backdrop_geom = nullptr;
        impl_->backdrop_visual = nullptr;
        impl_->root = nullptr;
        impl_->target = nullptr;
        impl_->compositor = nullptr;
    } catch (...) {
    }
    impl_->backdrop_supported = false;
    impl_->surface_w = 0;
    impl_->surface_h = 0;
    impl_->last_bd_vis = false;
    impl_->last_bd_l = -1.0f;
    impl_->hwnd = nullptr;
    impl_->d2d = nullptr;
}

bool CompositorHost::is_ready() const noexcept {
    return impl_ && impl_->compositor != nullptr && impl_->graphics_device != nullptr;
}

bool CompositorHost::backdrop_supported() const noexcept {
    return impl_ && impl_->backdrop_supported;
}

RenderStatus CompositorHost::resize_surface(int pixel_width, int pixel_height, float dpi_x, float dpi_y) {
    if (!is_ready()) return RenderStatus::DeviceLost;
    pixel_width = std::max(1, pixel_width);
    pixel_height = std::max(1, pixel_height);
    impl_->dpi_x = dpi_x;
    impl_->dpi_y = dpi_y;

    if (impl_->drawing_surface && impl_->surface_w == pixel_width && impl_->surface_h == pixel_height) {
        return RenderStatus::Ok;
    }

    try {
        const winrt::Windows::Foundation::Size sz{static_cast<float>(pixel_width), static_cast<float>(pixel_height)};
        if (!impl_->drawing_surface) {
            impl_->drawing_surface = impl_->graphics_device.CreateDrawingSurface(
                sz,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
            impl_->surface_brush.Surface(impl_->drawing_surface);
        } else {
            auto surface_interop = impl_->drawing_surface.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>();
            SIZE new_sz{pixel_width, pixel_height};
            HRESULT hr = surface_interop->Resize(new_sz);
            if (is_device_lost_hr(hr)) {
                return RenderStatus::DeviceLost;
            }
            if (FAILED(hr)) {
                impl_->drawing_surface = impl_->graphics_device.CreateDrawingSurface(
                    sz,
                    winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                    winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
                impl_->surface_brush.Surface(impl_->drawing_surface);
            }
        }

        const wfn::float2 fsz{static_cast<float>(pixel_width), static_cast<float>(pixel_height)};
        impl_->root.Size(fsz);
        impl_->surface_visual.Size(fsz);
        impl_->surface_w = pixel_width;
        impl_->surface_h = pixel_height;
        return RenderStatus::Ok;
    } catch (const winrt::hresult_error& e) {
        return is_device_lost_hr(e.code()) ? RenderStatus::DeviceLost : RenderStatus::Failed;
    } catch (...) {
        return RenderStatus::Failed;
    }
}

void CompositorHost::update_backdrop(bool visible, float left_px, float top_px, float width_px, float height_px, float radius_px) {
    if (!is_ready() || !impl_->backdrop_supported || !impl_->backdrop_visual) return;

    if (visible == impl_->last_bd_vis &&
        (!visible || (std::fabs(left_px - impl_->last_bd_l) < 0.25f &&
                      std::fabs(top_px - impl_->last_bd_t) < 0.25f &&
                      std::fabs(width_px - impl_->last_bd_w) < 0.25f &&
                      std::fabs(height_px - impl_->last_bd_h) < 0.25f &&
                      std::fabs(radius_px - impl_->last_bd_r) < 0.25f))) {
        return;
    }

    impl_->last_bd_vis = visible;
    impl_->last_bd_l = left_px;
    impl_->last_bd_t = top_px;
    impl_->last_bd_w = width_px;
    impl_->last_bd_h = height_px;
    impl_->last_bd_r = radius_px;

    try {
        impl_->backdrop_visual.IsVisible(visible);
        if (visible && width_px > 2.0f && height_px > 2.0f) {
            const float l = left_px + 0.5f;
            const float t = top_px + 0.5f;
            const float w = std::max(1.0f, width_px - 1.0f);
            const float h = std::max(1.0f, height_px - 1.0f);
            const float r = std::clamp(radius_px - 0.5f, 0.5f, std::min(w, h) * 0.5f);

            impl_->backdrop_visual.Offset({l, t, 0.0f});
            impl_->backdrop_visual.Size({w, h});
            impl_->backdrop_geom.Offset({0.0f, 0.0f});
            impl_->backdrop_geom.Size({w, h});
            impl_->backdrop_geom.CornerRadius({r, r});
        }
    } catch (...) {
    }
}

void CompositorHost::set_opacity(float opacity) {
    if (!is_ready() || !impl_->root) return;
    try {
        impl_->root.Opacity(std::clamp(opacity, 0.0f, 1.0f));
    } catch (...) {
    }
}

RenderStatus CompositorHost::render(const std::function<void(ID2D1DeviceContext*, float dpi_x, float dpi_y)>& draw_fn) {
    if (!is_ready() || !impl_->drawing_surface || !draw_fn) return RenderStatus::DeviceLost;

    try {
        auto surface_interop = impl_->drawing_surface.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>();
        ComPtr<ID2D1DeviceContext> dc;
        POINT offset{};
        HRESULT hr = surface_interop->BeginDraw(nullptr, IID_PPV_ARGS(&dc), &offset);
        if (is_device_lost_hr(hr)) {
            return RenderStatus::DeviceLost;
        }
        if (FAILED(hr) || !dc) {
            return RenderStatus::Failed;
        }

        dc->SetDpi(impl_->dpi_x, impl_->dpi_y);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(
            static_cast<float>(offset.x) * 96.0f / impl_->dpi_x,
            static_cast<float>(offset.y) * 96.0f / impl_->dpi_y));
        dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        draw_fn(dc.Get(), impl_->dpi_x, impl_->dpi_y);

        HRESULT end_hr = surface_interop->EndDraw();
        if (is_device_lost_hr(end_hr)) {
            return RenderStatus::DeviceLost;
        }
        return SUCCEEDED(end_hr) ? RenderStatus::Ok : RenderStatus::Failed;
    } catch (const winrt::hresult_error& e) {
        return is_device_lost_hr(e.code()) ? RenderStatus::DeviceLost : RenderStatus::Failed;
    } catch (...) {
        return RenderStatus::Failed;
    }
}

} // namespace mmoni
