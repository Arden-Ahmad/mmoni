#include "ui/d2d_context.h"

#include <vector>

using Microsoft::WRL::ComPtr;

namespace mmoni {

namespace {

bool is_integrated_adapter(const DXGI_ADAPTER_DESC1& desc) noexcept {
    if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) return false;
    if (desc.VendorId == 0x10DE) return false; // NVIDIA is always discrete on Windows laptops/desktops
    if (desc.VendorId == 0x8086) return desc.DedicatedVideoMemory < (2048ULL * 1024ULL * 1024ULL);
    if (desc.VendorId == 0x1002) return desc.DedicatedVideoMemory < (1024ULL * 1024ULL * 1024ULL);
    return false;
}

bool adapter_has_attached_output(IDXGIAdapter1* adapter) noexcept {
    if (!adapter) return false;
    ComPtr<IDXGIOutput> output;
    return adapter->EnumOutputs(0, &output) != DXGI_ERROR_NOT_FOUND && output != nullptr;
}

bool same_luid(const LUID& a, const LUID& b) noexcept {
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

struct AdapterCandidate {
    ComPtr<IDXGIAdapter1> adapter;
    LUID luid{};
    bool integrated = false;
    bool has_output = false;
};

// Enumerates hardware adapters ordered so Optimus/MSHybrid laptops stay on the display-driving iGPU
// (zero dGPU wake), while MUX / Advanced Optimus / dGPU-wired display setups pick the active display
// GPU instead of a sleeping headless iGPU (avoiding 150-480 ms cross-adapter wake stalls).
std::vector<AdapterCandidate> enumerate_preferred_adapters() {
    std::vector<AdapterCandidate> raw;
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) {
        return raw;
    }

    for (UINT idx = 0;; ++idx) {
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapterByGpuPreference(idx, DXGI_GPU_PREFERENCE_MINIMUM_POWER, IID_PPV_ARGS(&adapter)))) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue;
        }
        AdapterCandidate cand{};
        cand.adapter = std::move(adapter);
        cand.luid = desc.AdapterLuid;
        cand.integrated = is_integrated_adapter(desc);
        cand.has_output = adapter_has_attached_output(cand.adapter.Get());
        raw.push_back(std::move(cand));
    }

    std::vector<AdapterCandidate> ordered;
    ordered.reserve(raw.size());
    auto append_matching = [&](bool need_integrated, bool need_output) {
        for (const auto& c : raw) {
            if (c.integrated == need_integrated && c.has_output == need_output) {
                ordered.push_back(c);
            }
        }
    };

    append_matching(true, true);   // Pass 1: iGPU driving an active display (Optimus / MSHybrid)
    append_matching(false, true);  // Pass 2: dGPU driving an active display (MUX / Advanced Optimus / dGPU monitor)
    append_matching(true, false);  // Pass 3: headless iGPU fallback (RDP / display transition)
    append_matching(false, false); // Pass 4: headless dGPU fallback
    return ordered;
}

} // namespace

bool D2DContext::create_d3d_device() {
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

    const auto candidates = enumerate_preferred_adapters();
    for (const auto& cand : candidates) {
        if (try_create_on_adapter(cand.adapter.Get())) {
            adapter_luid_ = cand.luid;
            using_warp_ = false;
            return true;
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
    if (SUCCEEDED(hr) && d3d_device_ != nullptr) {
        adapter_luid_ = {};
        using_warp_ = true;
        return true;
    }
    return false;
}

bool D2DContext::preferred_adapter_changed() const noexcept {
    if (!d3d_device_) return false;
    const auto candidates = enumerate_preferred_adapters();
    if (candidates.empty()) {
        return !using_warp_;
    }
    return using_warp_ || !same_luid(adapter_luid_, candidates.front().luid);
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

    if (!create_d3d_device()) {
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
    adapter_luid_ = {};
    using_warp_ = false;
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
