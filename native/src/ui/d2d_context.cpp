#include "ui/d2d_context.h"

using Microsoft::WRL::ComPtr;

namespace mmoni {

bool D2DContext::create_d3d_device_igpu() {
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED;
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    auto try_create_on_adapter = [&](IDXGIAdapter1* adapter) -> bool {
        HRESULT hr = D3D11CreateDevice(
            adapter,
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            flags,
            levels,
            static_cast<UINT>(_countof(levels)),
            D3D11_SDK_VERSION,
            &d3d_device_,
            nullptr,
            nullptr);
        return SUCCEEDED(hr) && d3d_device_ != nullptr;
    };

    auto is_integrated = [](const DXGI_ADAPTER_DESC1& desc) noexcept -> bool {
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) return false;
        if (desc.VendorId == 0x10DE) return false; // NVIDIA is always discrete on Windows laptops/desktops
        if (desc.VendorId == 0x8086) return desc.DedicatedVideoMemory < (2048ULL * 1024ULL * 1024ULL);
        if (desc.VendorId == 0x1002) return desc.DedicatedVideoMemory < (1024ULL * 1024ULL * 1024ULL);
        return false;
    };

    ComPtr<IDXGIFactory6> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory) {
        // Pass 1: Strictly pick an integrated GPU if present so hybrid laptops NEVER wake the dGPU.
        bool any_integrated_found = false;
        for (UINT idx = 0;; ++idx) {
            ComPtr<IDXGIAdapter1> adapter;
            if (FAILED(factory->EnumAdapterByGpuPreference(idx, DXGI_GPU_PREFERENCE_MINIMUM_POWER, IID_PPV_ARGS(&adapter)))) {
                break;
            }
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && is_integrated(desc)) {
                any_integrated_found = true;
                if (try_create_on_adapter(adapter.Get())) {
                    return true;
                }
            }
        }

        // Pass 2: Only if NO integrated GPU exists (e.g., dGPU-only desktop), use first hardware adapter.
        if (!any_integrated_found) {
            for (UINT idx = 0;; ++idx) {
                ComPtr<IDXGIAdapter1> adapter;
                if (FAILED(factory->EnumAdapterByGpuPreference(idx, DXGI_GPU_PREFERENCE_MINIMUM_POWER, IID_PPV_ARGS(&adapter)))) {
                    break;
                }
                DXGI_ADAPTER_DESC1 desc{};
                if (SUCCEEDED(adapter->GetDesc1(&desc)) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                    if (try_create_on_adapter(adapter.Get())) {
                        return true;
                    }
                }
            }
        }
    }

    // Fallback to WARP software rasterizer (never wakes a sleeping dGPU).
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_WARP,
        nullptr,
        flags,
        levels,
        static_cast<UINT>(_countof(levels)),
        D3D11_SDK_VERSION,
        &d3d_device_,
        nullptr,
        nullptr);
    return SUCCEEDED(hr) && d3d_device_ != nullptr;
}

bool D2DContext::init() {
    reset();

    if (!d2d_factory_) {
        D2D1_FACTORY_OPTIONS opts{};
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts, &d2d_factory_))) {
            return false;
        }
    }

    if (!dwrite_factory_) {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(dwrite_factory_.GetAddressOf())))) {
            return false;
        }
        if (SUCCEEDED(dwrite_factory_->CreateTypography(&tabular_typography_)) && tabular_typography_) {
            DWRITE_FONT_FEATURE feat{};
            feat.nameTag = DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES;
            feat.parameter = 1;
            tabular_typography_->AddFontFeature(feat);
        }
    }

    if (!create_d3d_device_igpu()) {
        return false;
    }

    if (FAILED(d3d_device_.As(&dxgi_device_)) || !dxgi_device_) {
        return false;
    }

    if (FAILED(d2d_factory_->CreateDevice(dxgi_device_.Get(), &d2d_device_)) || !d2d_device_) {
        return false;
    }

    return true;
}

void D2DContext::reset() {
    d2d_device_.Reset();
    dxgi_device_.Reset();
    d3d_device_.Reset();
}

ComPtr<IDWriteTextFormat> D2DContext::create_text_format(
    float size_dip,
    DWRITE_FONT_WEIGHT weight,
    DWRITE_TEXT_ALIGNMENT halign,
    DWRITE_PARAGRAPH_ALIGNMENT valign,
    bool ellipsis) const {

    if (!dwrite_factory_) return nullptr;

    ComPtr<IDWriteTextFormat> fmt;
    HRESULT hr = dwrite_factory_->CreateTextFormat(
        L"Segoe UI Variable Text",
        nullptr,
        weight,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        size_dip,
        L"",
        &fmt);
    if (FAILED(hr) || !fmt) {
        dwrite_factory_->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            weight,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            size_dip,
            L"",
            &fmt);
    }
    if (!fmt) return nullptr;

    fmt->SetTextAlignment(halign);
    fmt->SetParagraphAlignment(valign);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    if (ellipsis) {
        ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(dwrite_factory_->CreateEllipsisTrimmingSign(fmt.Get(), &sign)) && sign) {
            DWRITE_TRIMMING trimming{};
            trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
            fmt->SetTrimming(&trimming, sign.Get());
        }
    }

    return fmt;
}

ComPtr<IDWriteTextLayout> D2DContext::create_tabular_layout(
    const wchar_t* text,
    IDWriteTextFormat* format,
    float max_width,
    float max_height) const {

    if (!dwrite_factory_ || !format || !text) return nullptr;
    const UINT32 len = static_cast<UINT32>(wcslen(text));
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_factory_->CreateTextLayout(text, len, format, max_width, max_height, &layout)) || !layout) {
        return nullptr;
    }
    if (tabular_typography_ && len > 0) {
        DWRITE_TEXT_RANGE range{0, len};
        layout->SetTypography(tabular_typography_.Get(), range);
    }
    return layout;
}

} // namespace mmoni
