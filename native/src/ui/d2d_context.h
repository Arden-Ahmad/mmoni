#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite_1.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace mmoni {

class D2DContext {
public:
    D2DContext() = default;
    ~D2DContext() = default;

    bool init();
    void reset();
    bool is_ready() const noexcept { return d2d_device_ != nullptr && dwrite_factory_ != nullptr; }

    ID3D11Device* d3d_device() const noexcept { return d3d_device_.Get(); }
    IDXGIDevice* dxgi_device() const noexcept { return dxgi_device_.Get(); }
    ID2D1Factory1* d2d_factory() const noexcept { return d2d_factory_.Get(); }
    ID2D1Device* d2d_device() const noexcept { return d2d_device_.Get(); }
    IDWriteFactory* dwrite_factory() const noexcept { return dwrite_factory_.Get(); }

    Microsoft::WRL::ComPtr<IDWriteTextFormat> create_text_format(
        float size_dip,
        DWRITE_FONT_WEIGHT weight,
        DWRITE_TEXT_ALIGNMENT halign = DWRITE_TEXT_ALIGNMENT_LEADING,
        DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
        bool ellipsis = false) const;

    Microsoft::WRL::ComPtr<IDWriteTextLayout> create_tabular_layout(
        const wchar_t* text,
        IDWriteTextFormat* format,
        float max_width,
        float max_height) const;

private:
    bool create_d3d_device_igpu();

    Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device_;
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2d_factory_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2d_device_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_factory_;
    Microsoft::WRL::ComPtr<IDWriteTypography> tabular_typography_;
};

} // namespace mmoni
