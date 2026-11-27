#include "ui/compositor_host.h"

#include <DispatcherQueue.h>
#include <d2d1effects.h>
#include <dwmapi.h>
#include <windows.graphics.effects.interop.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.UI.Composition.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

using Microsoft::WRL::ComPtr;
namespace wge = winrt::Windows::Graphics::Effects;
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

struct BackdropTuning {
    float blur_sigma;
    float saturation;
};

BackdropTuning tuning_for_theme(VisualTheme theme, float dpi_scale) noexcept {
    const float scale = std::clamp(dpi_scale, 1.0f, 2.5f);
    return (theme == VisualTheme::FluentAcrylic)
        ? BackdropTuning{24.0f * scale, 1.50f}
        : BackdropTuning{16.0f * scale, 1.95f};
}

// Zero-dependency D2D GaussianBlur wrapper for Windows.UI.Composition (no Win2D required).
struct DwmGaussianBlurEffect
    : winrt::implements<
          DwmGaussianBlurEffect,
          wge::IGraphicsEffect,
          wge::IGraphicsEffectSource,
          ABI::Windows::Graphics::Effects::IGraphicsEffectD2D1Interop> {
    winrt::hstring name_{L"Blur"};
    wge::IGraphicsEffectSource source_{nullptr};
    float blur_amount_ = 14.0f;

    winrt::hstring Name() const { return name_; }
    void Name(winrt::hstring const& value) { name_ = value; }

    IFACEMETHODIMP GetEffectId(GUID* id) noexcept override {
        if (!id) return E_POINTER;
        *id = CLSID_D2D1GaussianBlur;
        return S_OK;
    }

    IFACEMETHODIMP GetNamedPropertyMapping(
        LPCWSTR name,
        UINT* index,
        ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING* mapping) noexcept override {
        if (!name || !index || !mapping) return E_INVALIDARG;
        if (_wcsicmp(name, L"BlurAmount") == 0) {
            *index = D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION;
            *mapping = ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT;
            return S_OK;
        }
        return E_INVALIDARG;
    }

    IFACEMETHODIMP GetPropertyCount(UINT* count) noexcept override {
        if (!count) return E_POINTER;
        *count = 3;
        return S_OK;
    }

    IFACEMETHODIMP GetProperty(UINT index, ABI::Windows::Foundation::IPropertyValue** value) noexcept override {
        if (!value) return E_POINTER;
        *value = nullptr;
        try {
            switch (index) {
                case D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION: {
                    auto pv = winrt::box_value(blur_amount_).as<ABI::Windows::Foundation::IPropertyValue>();
                    *value = pv.detach();
                    return S_OK;
                }
                case D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION: {
                    auto pv = winrt::box_value(static_cast<uint32_t>(D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED))
                                  .as<ABI::Windows::Foundation::IPropertyValue>();
                    *value = pv.detach();
                    return S_OK;
                }
                case D2D1_GAUSSIANBLUR_PROP_BORDER_MODE: {
                    // Hard border mode prevents dark/transparent halo bleed at the rounded clip boundary.
                    auto pv = winrt::box_value(static_cast<uint32_t>(D2D1_BORDER_MODE_HARD))
                                  .as<ABI::Windows::Foundation::IPropertyValue>();
                    *value = pv.detach();
                    return S_OK;
                }
                default:
                    return E_BOUNDS;
            }
        } catch (...) {
            return E_FAIL;
        }
    }

    IFACEMETHODIMP GetSource(UINT index, ABI::Windows::Graphics::Effects::IGraphicsEffectSource** source) noexcept override {
        if (!source) return E_POINTER;
        *source = nullptr;
        if (index != 0 || !source_) return E_BOUNDS;
        try {
            auto abi_src = source_.as<ABI::Windows::Graphics::Effects::IGraphicsEffectSource>();
            *source = abi_src.detach();
            return S_OK;
        } catch (...) {
            return E_FAIL;
        }
    }

    IFACEMETHODIMP GetSourceCount(UINT* count) noexcept override {
        if (!count) return E_POINTER;
        *count = 1;
        return S_OK;
    }
};

// Zero-dependency D2D Saturation wrapper for Windows.UI.Composition vibrancy.
struct DwmSaturationEffect
    : winrt::implements<
          DwmSaturationEffect,
          wge::IGraphicsEffect,
          wge::IGraphicsEffectSource,
          ABI::Windows::Graphics::Effects::IGraphicsEffectD2D1Interop> {
    winrt::hstring name_{L"Sat"};
    wge::IGraphicsEffectSource source_{nullptr};
    float saturation_ = 1.85f;

    winrt::hstring Name() const { return name_; }
    void Name(winrt::hstring const& value) { name_ = value; }

    IFACEMETHODIMP GetEffectId(GUID* id) noexcept override {
        if (!id) return E_POINTER;
        *id = CLSID_D2D1Saturation;
        return S_OK;
    }

    IFACEMETHODIMP GetNamedPropertyMapping(
        LPCWSTR name,
        UINT* index,
        ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING* mapping) noexcept override {
        if (!name || !index || !mapping) return E_INVALIDARG;
        if (_wcsicmp(name, L"Saturation") == 0) {
            *index = D2D1_SATURATION_PROP_SATURATION;
            *mapping = ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT;
            return S_OK;
        }
        return E_INVALIDARG;
    }

    IFACEMETHODIMP GetPropertyCount(UINT* count) noexcept override {
        if (!count) return E_POINTER;
        *count = 1;
        return S_OK;
    }

    IFACEMETHODIMP GetProperty(UINT index, ABI::Windows::Foundation::IPropertyValue** value) noexcept override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (index != D2D1_SATURATION_PROP_SATURATION) return E_BOUNDS;
        try {
            auto pv = winrt::box_value(saturation_).as<ABI::Windows::Foundation::IPropertyValue>();
            *value = pv.detach();
            return S_OK;
        } catch (...) {
            return E_FAIL;
        }
    }

    IFACEMETHODIMP GetSource(UINT index, ABI::Windows::Graphics::Effects::IGraphicsEffectSource** source) noexcept override {
        if (!source) return E_POINTER;
        *source = nullptr;
        if (index != 0 || !source_) return E_BOUNDS;
        try {
            auto abi_src = source_.as<ABI::Windows::Graphics::Effects::IGraphicsEffectSource>();
            *source = abi_src.detach();
            return S_OK;
        } catch (...) {
            return E_FAIL;
        }
    }

    IFACEMETHODIMP GetSourceCount(UINT* count) noexcept override {
        if (!count) return E_POINTER;
        *count = 1;
        return S_OK;
    }
};

wuc::CompositionEffectBrush create_backdrop_effect(
    const wuc::Compositor& compositor,
    const wuc::CompositionBackdropBrush& backdrop,
    bool with_saturation) {

    try {
        auto blur = winrt::make_self<DwmGaussianBlurEffect>();
        blur->source_ = wuc::CompositionEffectSourceParameter(L"backdrop");
        auto effect = blur.as<wge::IGraphicsEffect>();
        auto anim_props = winrt::single_threaded_vector<winrt::hstring>({L"Blur.BlurAmount"});

        if (with_saturation) {
            auto sat = winrt::make_self<DwmSaturationEffect>();
            sat->source_ = blur.as<wge::IGraphicsEffectSource>();
            effect = sat.as<wge::IGraphicsEffect>();
            anim_props.Append(L"Sat.Saturation");
        }

        auto factory = compositor.CreateEffectFactory(effect, anim_props);
        const auto status = factory.LoadStatus();
        if (status != wuc::CompositionEffectFactoryLoadStatus::Success &&
            status != wuc::CompositionEffectFactoryLoadStatus::Pending) {
            return nullptr;
        }

        auto brush = factory.CreateBrush();
        brush.SetSourceParameter(L"backdrop", backdrop);
        return brush;
    } catch (...) {
        return nullptr;
    }
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
    bool backdrop_effect_attempted = false;
    bool effect_has_saturation = false;
    wuc::CompositionBackdropBrush host_backdrop{nullptr};
    wuc::CompositionEffectBrush backdrop_effect_brush{nullptr};
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
    VisualTheme last_theme = VisualTheme::PrecisionDark;
    float last_dpi_scale = -1.0f;
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

        // Layer 1 (optional): Native Win11 backdrop. Enhanced themes load their effect on demand.
        try {
            const BOOL enable_host_backdrop = TRUE;
            winrt::check_hresult(DwmSetWindowAttribute(hwnd, DWMWA_USE_HOSTBACKDROPBRUSH,
                &enable_host_backdrop, sizeof(enable_host_backdrop)));
            impl_->host_backdrop = impl_->compositor.CreateHostBackdropBrush();
            impl_->backdrop_visual = impl_->compositor.CreateSpriteVisual();
            impl_->backdrop_visual.Brush(impl_->host_backdrop);
            impl_->backdrop_geom = impl_->compositor.CreateRoundedRectangleGeometry();
            impl_->backdrop_clip = impl_->compositor.CreateGeometricClip(impl_->backdrop_geom);
            impl_->backdrop_visual.Clip(impl_->backdrop_clip);
            impl_->backdrop_visual.IsVisible(false);
            impl_->root.Children().InsertAtBottom(impl_->backdrop_visual);
            impl_->backdrop_supported = true;
        } catch (...) {
            impl_->backdrop_supported = false;
            impl_->host_backdrop = nullptr;
            impl_->backdrop_clip = nullptr;
            impl_->backdrop_geom = nullptr;
            impl_->backdrop_visual = nullptr;
        }

        // Layer 2: Direct2D / DirectWrite retained drawing surface on active-display DX11 device.
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
        impl_->backdrop_effect_brush = nullptr;
        impl_->host_backdrop = nullptr;
        impl_->root = nullptr;
        impl_->target = nullptr;
        impl_->compositor = nullptr;
    } catch (...) {
    }
    impl_->backdrop_supported = false;
    impl_->backdrop_effect_attempted = false;
    impl_->effect_has_saturation = false;
    impl_->surface_w = 0;
    impl_->surface_h = 0;
    impl_->last_bd_vis = false;
    impl_->last_theme = VisualTheme::PrecisionDark;
    impl_->last_dpi_scale = -1.0f;
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

void CompositorHost::update_backdrop(
    bool visible,
    VisualTheme theme,
    float dpi_scale,
    float left_px,
    float top_px,
    float width_px,
    float height_px,
    float radius_px) {

    if (!is_ready() || !impl_->backdrop_supported || !impl_->backdrop_visual) return;

    const bool material_changed = visible &&
        (theme != impl_->last_theme || std::fabs(dpi_scale - impl_->last_dpi_scale) > 0.01f);

    const bool geom_changed =
        visible != impl_->last_bd_vis ||
        (visible && (std::fabs(left_px - impl_->last_bd_l) >= 0.25f ||
                     std::fabs(top_px - impl_->last_bd_t) >= 0.25f ||
                     std::fabs(width_px - impl_->last_bd_w) >= 0.25f ||
                     std::fabs(height_px - impl_->last_bd_h) >= 0.25f ||
                     std::fabs(radius_px - impl_->last_bd_r) >= 0.25f));

    if (!material_changed && !geom_changed) {
        return;
    }

    if (material_changed) {
        const bool uses_effect = theme == VisualTheme::FluentAcrylic || theme == VisualTheme::ChromaticPrism;
        if (uses_effect && !impl_->backdrop_effect_attempted) {
            impl_->backdrop_effect_attempted = true;
            impl_->backdrop_effect_brush = create_backdrop_effect(impl_->compositor, impl_->host_backdrop, true);
            impl_->effect_has_saturation = static_cast<bool>(impl_->backdrop_effect_brush);
            if (!impl_->backdrop_effect_brush) {
                impl_->backdrop_effect_brush = create_backdrop_effect(impl_->compositor, impl_->host_backdrop, false);
            }
        }

        wuc::CompositionBrush brush = impl_->host_backdrop;
        if (uses_effect && impl_->backdrop_effect_brush) {
            try {
                const BackdropTuning tuning = tuning_for_theme(theme, dpi_scale);
                auto props = impl_->backdrop_effect_brush.Properties();
                props.InsertScalar(L"Blur.BlurAmount", tuning.blur_sigma);
                if (impl_->effect_has_saturation) {
                    props.InsertScalar(L"Sat.Saturation", tuning.saturation);
                }
                brush = impl_->backdrop_effect_brush;
            } catch (...) {
            }
        }

        try {
            impl_->backdrop_visual.Brush(brush);
            impl_->last_theme = theme;
            impl_->last_dpi_scale = dpi_scale;
        } catch (...) {
        }
    }

    if (geom_changed) {
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
