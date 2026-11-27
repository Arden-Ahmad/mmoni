#include "ui/overlay_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace mmoni {

namespace {

constexpr D2D1_COLOR_F argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) noexcept {
    return {r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
}

constexpr D2D1_COLOR_F with_alpha(D2D1_COLOR_F c, float mul) noexcept {
    return {c.r, c.g, c.b, c.a * mul};
}

constexpr D2D1_COLOR_F lerp_color(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) noexcept {
    return {
        a.r + (b.r - a.r) * t,
        a.g + (b.g - a.g) * t,
        a.b + (b.b - a.b) * t,
        a.a + (b.a - a.a) * t
    };
}

constexpr D2D1_COLOR_F kTextColor = argb(255, 244, 246, 251);
constexpr D2D1_COLOR_F kPowerColor = argb(255, 220, 227, 242);
constexpr D2D1_COLOR_F kSubtleColor = argb(255, 195, 204, 224);
constexpr D2D1_COLOR_F kQuietColor = argb(255, 154, 164, 186);
constexpr D2D1_COLOR_F kCpuColor = argb(255, 111, 231, 207);
constexpr D2D1_COLOR_F kGpuColor = argb(255, 178, 184, 255);
constexpr D2D1_COLOR_F kWarmColor = argb(255, 251, 191, 36);
constexpr D2D1_COLOR_F kHotColor = argb(255, 251, 113, 133);
constexpr D2D1_COLOR_F kLiveGreen = argb(255, 74, 222, 128);
constexpr D2D1_COLOR_F kSleepDot = argb(255, 106, 116, 138);

// State-aware dark glass tints (preserves crisp contrast over bright white apps while letting DWM blur shine).
constexpr D2D1_COLOR_F kLiquidPillIdle = argb(200, 17, 21, 33);
constexpr D2D1_COLOR_F kLiquidPillHover = argb(210, 23, 28, 43);
constexpr D2D1_COLOR_F kLiquidPeek = argb(178, 16, 20, 32);
constexpr D2D1_COLOR_F kLiquidDash = argb(198, 14, 18, 29);

constexpr D2D1_COLOR_F kAcrylicPillIdle = argb(206, 22, 25, 34);
constexpr D2D1_COLOR_F kAcrylicPillHover = argb(216, 28, 32, 43);
constexpr D2D1_COLOR_F kAcrylicPeek = argb(186, 21, 24, 33);
constexpr D2D1_COLOR_F kAcrylicDash = argb(206, 18, 21, 30);

constexpr D2D1_COLOR_F kPrismPillIdle = argb(200, 14, 17, 28);
constexpr D2D1_COLOR_F kPrismPillHover = argb(210, 20, 24, 38);
constexpr D2D1_COLOR_F kPrismPeek = argb(176, 14, 17, 28);
constexpr D2D1_COLOR_F kPrismDash = argb(196, 12, 15, 25);

constexpr D2D1_COLOR_F kPrecisionSurface = argb(244, 18, 21, 28);

D2D1_COLOR_F resolve_surface_color(const OverlayRenderState& st, VisualTheme theme) noexcept {
    if (!is_glass_theme(theme)) {
        return kPrecisionSurface;
    }

    D2D1_COLOR_F pill_c = kLiquidPillIdle;
    D2D1_COLOR_F peek_c = kLiquidPeek;
    D2D1_COLOR_F dash_c = kLiquidDash;

    if (theme == VisualTheme::FluentAcrylic) {
        pill_c = (st.hovered_chrome && st.state == IslandState::Minimal) ? kAcrylicPillHover : kAcrylicPillIdle;
        peek_c = kAcrylicPeek;
        dash_c = kAcrylicDash;
    } else if (theme == VisualTheme::ChromaticPrism) {
        pill_c = (st.hovered_chrome && st.state == IslandState::Minimal) ? kPrismPillHover : kPrismPillIdle;
        peek_c = kPrismPeek;
        dash_c = kPrismDash;
    } else {
        pill_c = (st.hovered_chrome && st.state == IslandState::Minimal) ? kLiquidPillHover : kLiquidPillIdle;
    }

    const float w_min = std::max(0.0f, st.minimal_opacity);
    const float w_peek = std::max(0.0f, st.peek_opacity);
    const float w_dash = std::max(0.0f, st.dashboard_opacity);
    const float sum = w_min + w_peek + w_dash;
    if (sum <= 0.001f) {
        return (st.state == IslandState::Dashboard) ? dash_c : (st.state == IslandState::Peek ? peek_c : pill_c);
    }
    const float inv = 1.0f / sum;
    return {
        (pill_c.r * w_min + peek_c.r * w_peek + dash_c.r * w_dash) * inv,
        (pill_c.g * w_min + peek_c.g * w_peek + dash_c.g * w_dash) * inv,
        (pill_c.b * w_min + peek_c.b * w_peek + dash_c.b * w_dash) * inv,
        (pill_c.a * w_min + peek_c.a * w_peek + dash_c.a * w_dash) * inv
    };
}

std::wstring fmt_num(const std::optional<float>& v, const wchar_t* fmt, const wchar_t* unit) {
    if (!v.has_value() || !std::isfinite(*v)) return L"\x2014";
    wchar_t buf[32];
    swprintf_s(buf, fmt, *v);
    std::wstring out = buf;
    out += unit;
    return out;
}

std::wstring fmt_clock(const std::optional<float>& mhz) {
    if (!mhz.has_value() || !std::isfinite(*mhz)) return L"\x2014";
    wchar_t buf[32];
    if (*mhz >= 1000.0f) {
        swprintf_s(buf, L"%.2f GHz", *mhz / 1000.0f);
    } else {
        swprintf_s(buf, L"%.0f MHz", *mhz);
    }
    return buf;
}

} // namespace

bool OverlayRenderer::init(D2DContext* ctx) {
    ctx_ = ctx;
    noise_bitmap_.Reset();
    noise_brush_.Reset();
    if (!ctx_ || !ctx_->is_ready()) return false;

    fmt_caption_ = ctx_->create_text_format(9.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    fmt_pill_temp_ = ctx_->create_text_format(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);
    fmt_pill_sec_ = ctx_->create_text_format(11.5f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_TRAILING);
    fmt_peek_temp_ = ctx_->create_text_format(19.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    fmt_peek_sub_left_ = ctx_->create_text_format(10.2f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    fmt_peek_sub_right_ = ctx_->create_text_format(10.2f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    fmt_peek_sub_center_ = ctx_->create_text_format(10.2f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt_peek_sub_ellipsis_ = ctx_->create_text_format(10.2f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    fmt_dash_temp_ = ctx_->create_text_format(15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    fmt_dash_power_ = ctx_->create_text_format(12.5f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_FAR);
    fmt_dash_sub_left_ = ctx_->create_text_format(9.8f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    fmt_dash_sub_center_ = ctx_->create_text_format(9.8f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt_dash_sub_ellipsis_ = ctx_->create_text_format(9.8f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    fmt_seg_tab_ = ctx_->create_text_format(11.0f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt_seg_tab_bold_ = ctx_->create_text_format(11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt_range_ = ctx_->create_text_format(11.0f, DWRITE_FONT_WEIGHT_MEDIUM);
    fmt_card_title_ = ctx_->create_text_format(10.5f, DWRITE_FONT_WEIGHT_BOLD);
    fmt_card_model_ = ctx_->create_text_format(10.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    fmt_stat_label_ = ctx_->create_text_format(10.0f, DWRITE_FONT_WEIGHT_NORMAL);
    fmt_stat_val_ = ctx_->create_text_format(10.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    fmt_stat_val_right_ = ctx_->create_text_format(10.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);
    fmt_scale_right_ = ctx_->create_text_format(9.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    fmt_empty_center_ = ctx_->create_text_format(10.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt_badge_ = ctx_->create_text_format(8.0f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt_tooltip_ = ctx_->create_text_format(9.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt_banner_ = ctx_->create_text_format(10.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);

    return fmt_pill_temp_ != nullptr;
}

std::wstring OverlayRenderer::compute_warning_text(const SensorSnapshot* snapshot, bool elevated) {
    if (!snapshot) return {};
    if (snapshot->error.has_value()) {
        return L"Sensor polling interrupted \x00B7 retrying\x2026";
    }
    const bool cpu_ok = snapshot->cpu.has_value() &&
                        snapshot->cpu->temperature.has_value() &&
                        snapshot->cpu->power.has_value();
    if (!cpu_ok) {
        if (!snapshot->pawnio_installed) {
            return L"Install PawnIO to enable Intel/AMD CPU sensors";
        }
        if (!elevated) {
            return L"CPU sensors need administrator access (Tray \x2192 Restart as admin)";
        }
        return L"Some CPU sensors are unavailable";
    }
    const bool gpu_ok = snapshot->gpu.has_value() &&
                        (snapshot->gpu->is_sleeping ||
                         (snapshot->gpu->temperature.has_value() && snapshot->gpu->power.has_value()));
    if (!gpu_ok) {
        return L"Some GPU sensors are unavailable";
    }
    return {};
}

DashboardLayout OverlayRenderer::compute_dashboard_layout(const D2D1_RECT_F& chrome, bool has_banner) noexcept {
    DashboardLayout lay{};
    lay.chrome = chrome;
    const float l = chrome.left;
    const float t = chrome.top;
    const float w = chrome.right - chrome.left;
    const float h = chrome.bottom - chrome.top;

    // Header row: margin (12, 9, 10, 6), height ~52
    const float btn_y = t + 11.0f;
    lay.collapse_btn = {l + w - 10.0f - 24.0f, btn_y, l + w - 10.0f, btn_y + 24.0f};
    lay.pin_btn = {lay.collapse_btn.left - 3.0f - 24.0f, btn_y, lay.collapse_btn.left - 3.0f, btn_y + 24.0f};
    lay.header = {l + 12.0f, t + 9.0f, lay.pin_btn.left - 6.0f, t + 61.0f};

    // Segmented control + range button row
    const float seg_y = t + 65.0f;
    lay.seg_track = {l + 12.0f, seg_y, l + 12.0f + 238.0f, seg_y + 26.0f};
    const float tab_w = (238.0f - 4.0f) / 4.0f;
    for (int i = 0; i < 4; ++i) {
        const float tx = lay.seg_track.left + 2.0f + i * tab_w;
        lay.tab_rects[i] = {tx, lay.seg_track.top + 2.0f, tx + tab_w, lay.seg_track.bottom - 2.0f};
    }
    lay.range_btn = {l + w - 12.0f - 54.0f, seg_y + 2.0f, l + w - 12.0f, seg_y + 24.0f};

    // Status banner (optional at bottom)
    const float banner_h = has_banner ? 24.0f : 0.0f;
    const float cards_bottom = has_banner ? (t + h - 10.0f - banner_h - 6.0f) : (t + h - 11.0f);
    if (has_banner) {
        lay.status_banner = {l + 12.0f, t + h - 10.0f - banner_h, l + w - 12.0f, t + h - 10.0f};
    }

    // Dual cards
    const float cards_top = seg_y + 35.0f;
    const float total_cards_w = w - 24.0f;
    const float gap = 9.0f;
    const float card_w = (total_cards_w - gap) * 0.5f;
    lay.cpu_card = {l + 12.0f, cards_top, l + 12.0f + card_w, cards_bottom};
    lay.gpu_card = {lay.cpu_card.right + gap, cards_top, l + w - 12.0f, cards_bottom};

    lay.cpu_chart = {lay.cpu_card.left + 10.0f, lay.cpu_card.top + 54.0f, lay.cpu_card.right - 10.0f, lay.cpu_card.bottom - 8.0f};
    lay.gpu_chart = {lay.gpu_card.left + 10.0f, lay.gpu_card.top + 54.0f, lay.gpu_card.right - 10.0f, lay.gpu_card.bottom - 8.0f};
    return lay;
}

D2D1_COLOR_F OverlayRenderer::thermal_color(const std::optional<float>& temp, bool is_gpu, const UserSettings& s) const noexcept {
    if (!temp.has_value()) return kQuietColor;
    const float hot = is_gpu ? s.gpu_hot_temp : s.cpu_hot_temp;
    const float warm = is_gpu ? s.gpu_warm_temp : s.cpu_warm_temp;
    if (*temp >= hot) return kHotColor;
    if (*temp >= warm) return kWarmColor;
    return is_gpu ? kGpuColor : kCpuColor;
}

OverlayRenderer::ThermalAccent OverlayRenderer::thermal_edge_accent(
    const std::optional<float>& temp,
    bool is_gpu,
    bool sleeping,
    const UserSettings& s) const noexcept {

    if (sleeping) {
        return {argb(255, 118, 128, 152), 0.0f};
    }

    const D2D1_COLOR_F base = is_gpu ? kGpuColor : kCpuColor;
    if (!temp.has_value() || !std::isfinite(*temp)) {
        return {base, 0.0f};
    }

    const float warm = is_gpu ? s.gpu_warm_temp : s.cpu_warm_temp;
    const float hot = std::max(warm + 1.0f, is_gpu ? s.gpu_hot_temp : s.cpu_hot_temp);
    const float t = *temp;
    const float ramp_start = warm - 10.0f;

    if (t <= ramp_start) {
        return {base, 0.0f};
    }
    if (t < warm) {
        const float u = std::clamp((t - ramp_start) / 10.0f, 0.0f, 1.0f);
        // Smooth subtle warming shift as temp approaches warm threshold.
        return {lerp_color(base, kWarmColor, u * u * 0.72f), u * 0.45f};
    }
    if (t < hot) {
        const float u = std::clamp((t - warm) / (hot - warm), 0.0f, 1.0f);
        return {lerp_color(kWarmColor, kHotColor, u), 0.45f + u * 0.55f};
    }
    return {kHotColor, 1.0f};
}

std::wstring OverlayRenderer::format_metric(const std::optional<float>& v, Metric m) const {
    switch (m) {
        case Metric::Temperature: return fmt_num(v, L"%.0f", L"\u00B0C");
        case Metric::Power: return fmt_num(v, L"%.1f", L"W");
        case Metric::Load: return fmt_num(v, L"%.0f", L"%");
        case Metric::Clock: return fmt_clock(v);
    }
    return L"\x2014";
}

std::wstring OverlayRenderer::format_secondary(const std::optional<ProcessorReading>& r, CollapsedDisplayMode mode) const {
    if (!r.has_value()) return mode == CollapsedDisplayMode::TempOnly ? L"" : L"\x2014";
    switch (mode) {
        case CollapsedDisplayMode::TempLoad: return fmt_num(r->load, L"%.0f", L"%");
        case CollapsedDisplayMode::TempOnly: return L"";
        default: return fmt_num(r->power, L"%.1f", L"W");
    }
}

void OverlayRenderer::draw_text(
    ID2D1DeviceContext* dc,
    const std::wstring& text,
    IDWriteTextFormat* fmt,
    const D2D1_RECT_F& rect,
    const D2D1_COLOR_F& color,
    float opacity,
    bool tabular) const {

    if (text.empty() || !fmt || opacity <= 0.005f) return;
    ComPtr<ID2D1SolidColorBrush> brush;
    dc->CreateSolidColorBrush(with_alpha(color, opacity), &brush);
    if (!brush) return;

    if (tabular) {
        const float w = std::max(1.0f, rect.right - rect.left);
        const float h = std::max(1.0f, rect.bottom - rect.top);
        if (auto layout = ctx_->create_tabular_layout(text.c_str(), fmt, w, h)) {
            dc->DrawTextLayout({rect.left, rect.top}, layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            return;
        }
    }
    dc->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), fmt, rect, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void OverlayRenderer::draw_shadow(
    ID2D1DeviceContext* dc,
    const D2D1_RECT_F& rect,
    float radius,
    bool is_glass,
    IslandState state) const {

    // Fast, crisp analytical multi-layer soft drop shadow (zero blur-shader pass overhead).
    const float max_spread = (state == IslandState::Minimal) ? (is_glass ? 9.0f : 6.0f)
                           : (state == IslandState::Peek)    ? (is_glass ? 13.0f : 9.0f)
                                                             : (is_glass ? 16.0f : 11.0f);
    const float y_off = (state == IslandState::Minimal) ? 3.5f : 7.0f;
    const float base_opacity = (state == IslandState::Minimal) ? 0.34f : 0.44f;

    constexpr int kLayers = 6;
    for (int i = kLayers; i >= 1; --i) {
        const float t = static_cast<float>(i) / static_cast<float>(kLayers);
        const float spread = max_spread * t;
        const float alpha = base_opacity * (1.0f - t) * (1.0f - t) * 0.42f;
        if (alpha <= 0.002f) continue;

        D2D1_ROUNDED_RECT rr{};
        rr.rect = {
            rect.left - spread * 0.65f,
            rect.top + y_off * t - spread * 0.35f,
            rect.right + spread * 0.65f,
            rect.bottom + y_off * t + spread * 0.75f
        };
        rr.radiusX = radius + spread * 0.55f;
        rr.radiusY = radius + spread * 0.55f;

        ComPtr<ID2D1SolidColorBrush> sb;
        dc->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, alpha), &sb);
        if (sb) {
            dc->FillRoundedRectangle(rr, sb.Get());
        }
    }
}

void OverlayRenderer::draw_throttle_badge(ID2D1DeviceContext* dc, float x, float y, uint32_t flags) const {
    if (flags == ThrottleNone) return;
    const bool is_thermal = (flags & ThrottleThermal) != 0;
    const std::wstring text = is_thermal ? L"THRM" : L"PWR";
    const float w = is_thermal ? 31.0f : 26.0f;
    const float h = 13.0f;
    const D2D1_RECT_F r{x, y, x + w, y + h};

    ComPtr<ID2D1SolidColorBrush> bg, border;
    dc->CreateSolidColorBrush(is_thermal ? argb(48, 251, 113, 133) : argb(44, 251, 191, 36), &bg);
    dc->CreateSolidColorBrush(is_thermal ? argb(104, 251, 113, 133) : argb(96, 251, 191, 36), &border);
    if (bg) dc->FillRoundedRectangle(D2D1::RoundedRect(r, 4.0f, 4.0f), bg.Get());
    if (border) dc->DrawRoundedRectangle(D2D1::RoundedRect(r, 4.0f, 4.0f), border.Get(), 1.0f);
    draw_text(dc, text, fmt_badge_.Get(), r, is_thermal ? kHotColor : kWarmColor, 1.0f, false);
}

void OverlayRenderer::draw_acrylic_grain(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, float opacity) const {
    if (opacity <= 0.002f) return;

    if (!noise_brush_) {
        constexpr UINT32 kTile = 128;
        std::array<uint32_t, kTile * kTile> pixels{};
        // Deterministic fast xorshift32 monochrome fine grain (premultiplied BGRA, zero external assets).
        uint32_t state = 0x5A17C93Du;
        for (UINT32 i = 0; i < kTile * kTile; ++i) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            const uint8_t n = static_cast<uint8_t>(state & 0xFFu);
            // Subtle crisp white speckles for tactile Win11 Fluent Acrylic texture without dark muddying.
            const uint8_t a = (n > 152u) ? static_cast<uint8_t>((n - 152u) * 2u) : 0u;
            pixels[i] = (static_cast<uint32_t>(a) << 24) |
                        (static_cast<uint32_t>(a) << 16) |
                        (static_cast<uint32_t>(a) << 8) |
                        static_cast<uint32_t>(a);
        }

        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(kTile, kTile), pixels.data(), kTile * 4u, props, &noise_bitmap_)) &&
            noise_bitmap_) {
            D2D1_BITMAP_BRUSH_PROPERTIES bprops = D2D1::BitmapBrushProperties(
                D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            dc->CreateBitmapBrush(noise_bitmap_.Get(), bprops, &noise_brush_);
        }
    }

    if (noise_brush_) {
        noise_brush_->SetOpacity(opacity);
        dc->FillRectangle(rect, noise_brush_.Get());
    }
}

void OverlayRenderer::draw_curved_double_rim(
    ID2D1DeviceContext* dc,
    const OverlayRenderState& st,
    VisualTheme theme) const {

    const D2D1_RECT_F cr = st.chrome_rect;
    const float cw = cr.right - cr.left;
    const float ch = cr.bottom - cr.top;
    if (cw <= 8.0f || ch <= 8.0f) return;

    // Animates seamlessly with existing morph transition (0 in Minimal -> 1 in Peek, ~0.88 in Dashboard).
    const float peek_weight = std::clamp(st.peek_opacity + st.dashboard_opacity * 0.88f, 0.0f, 1.0f);
    const bool is_acrylic = (theme == VisualTheme::FluentAcrylic);
    const bool is_prism = (theme == VisualTheme::ChromaticPrism);

    const float top_alpha = is_acrylic
        ? (0.20f + 0.16f * peek_weight)
        : (is_prism ? (0.28f + 0.24f * peek_weight) : (0.26f + 0.26f * peek_weight));
    const float corner_alpha = top_alpha * 0.56f;
    const float bot_alpha = is_acrylic
        ? (0.05f + 0.08f * peek_weight)
        : (0.06f + 0.14f * peek_weight);

    // Primary curved specular rim following exact animated rounded corners (top caustic arc + thin bottom bounce).
    {
        const float inset = 1.25f;
        const float rad = std::max(0.5f, st.corner_radius - inset);
        const D2D1_ROUNDED_RECT rim_rr = D2D1::RoundedRect(
            {cr.left + inset, cr.top + inset, cr.right - inset, cr.bottom - inset},
            rad, rad);

        const float top_wrap = std::clamp((rad * 0.95f) / ch, 0.10f, 0.34f);
        const float top_fade = std::min(0.52f, top_wrap + 0.18f);
        const float bot_start = std::max(0.68f, 1.0f - top_wrap - 0.12f);

        D2D1_GRADIENT_STOP stops[6] = {
            {0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, top_alpha)},
            {top_wrap, D2D1::ColorF(1.0f, 1.0f, 1.0f, corner_alpha)},
            {top_fade, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
            {bot_start, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
            {std::min(0.97f, 1.0f - top_wrap * 0.45f), D2D1::ColorF(1.0f, 1.0f, 1.0f, bot_alpha * 0.55f)},
            {1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, bot_alpha)}
        };
        ComPtr<ID2D1GradientStopCollection> scol;
        if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 6, &scol)) && scol) {
            ComPtr<ID2D1LinearGradientBrush> brush;
            if (SUCCEEDED(dc->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties({cr.left, cr.top}, {cr.left, cr.bottom}),
                    scol.Get(), &brush)) && brush) {
                dc->DrawRoundedRectangle(rim_rr, brush.Get(), 1.0f);
            }
        }
    }

    // Secondary optical inner rim (curved double rim that blooms smoothly as Peek / Dashboard opens).
    if (peek_weight > 0.02f && ch > 32.0f) {
        const float inset2 = 2.85f;
        const float rad2 = std::max(0.5f, st.corner_radius - inset2);
        const D2D1_ROUNDED_RECT inner_rr = D2D1::RoundedRect(
            {cr.left + inset2, cr.top + inset2, cr.right - inset2, cr.bottom - inset2},
            rad2, rad2);

        const float second_top = (is_acrylic ? 0.09f : 0.17f) * peek_weight;
        const float second_bot = (is_acrylic ? 0.04f : 0.08f) * peek_weight;
        const float top_wrap = std::clamp((rad2 * 0.85f) / ch, 0.08f, 0.28f);

        D2D1_GRADIENT_STOP stops2[5] = {
            {0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, second_top)},
            {top_wrap, D2D1::ColorF(1.0f, 1.0f, 1.0f, second_top * 0.45f)},
            {std::min(0.45f, top_wrap + 0.14f), D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
            {0.82f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
            {1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, second_bot)}
        };
        ComPtr<ID2D1GradientStopCollection> scol2;
        if (SUCCEEDED(dc->CreateGradientStopCollection(stops2, 5, &scol2)) && scol2) {
            ComPtr<ID2D1LinearGradientBrush> brush2;
            if (SUCCEEDED(dc->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties({cr.left, cr.top}, {cr.left, cr.bottom}),
                    scol2.Get(), &brush2)) && brush2) {
                dc->DrawRoundedRectangle(inner_rr, brush2.Get(), 1.35f);
            }
        }
    }
}

void OverlayRenderer::draw_glass_border(
    ID2D1DeviceContext* dc,
    const OverlayRenderState& st,
    VisualTheme theme,
    const ThermalAccent& cpu_accent,
    const ThermalAccent& gpu_accent) const {

    const D2D1_RECT_F cr = st.chrome_rect;
    const D2D1_ROUNDED_RECT border_rr = D2D1::RoundedRect(
        {cr.left + 0.5f, cr.top + 0.5f, cr.right - 0.5f, cr.bottom - 0.5f},
        st.corner_radius, st.corner_radius);

    if (!is_glass_theme(theme)) {
        ComPtr<ID2D1SolidColorBrush> bbrush;
        dc->CreateSolidColorBrush(argb(255, 62, 70, 88), &bbrush);
        if (bbrush) {
            dc->DrawRoundedRectangle(border_rr, bbrush.Get(), 1.0f);
        }
        return;
    }

    const bool hover_min = (st.state == IslandState::Minimal && st.hovered_chrome);
    const float expand = std::clamp(st.peek_opacity + st.dashboard_opacity, 0.0f, 1.0f);

    // Layer A: Crisp base specular glass border.
    {
        const uint8_t top_a = (theme == VisualTheme::FluentAcrylic)
            ? (hover_min ? 116 : 86)
            : (hover_min ? 112 : 76);
        const uint8_t mid_a = hover_min ? 48 : 30;
        const uint8_t bot_a = static_cast<uint8_t>((hover_min ? 34 : 20) + static_cast<int>(10.0f * expand));

        D2D1_GRADIENT_STOP bstops[3] = {
            {0.0f, argb(top_a, 255, 255, 255)},
            {0.46f, argb(mid_a, 255, 255, 255)},
            {1.0f, argb(bot_a, 255, 255, 255)}
        };
        ComPtr<ID2D1GradientStopCollection> bcol;
        if (SUCCEEDED(dc->CreateGradientStopCollection(bstops, 3, &bcol)) && bcol) {
            ComPtr<ID2D1LinearGradientBrush> bbrush;
            if (SUCCEEDED(dc->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties({cr.left, cr.top}, {cr.left, cr.bottom}),
                    bcol.Get(), &bbrush)) && bbrush) {
                dc->DrawRoundedRectangle(border_rr, bbrush.Get(), 1.0f);
            }
        }
    }

    // Layer B: Theme-specific Chromatic Prism ring OR Thermal-Reactive Mint/Violet -> Amber/Rose edge accent.
    if (theme == VisualTheme::ChromaticPrism) {
        // Iridescent prism spectrum blended smoothly with live CPU/GPU thermal colors.
        const D2D1_COLOR_F c0 = lerp_color(argb(255, 255, 96, 168), cpu_accent.color, 0.55f + 0.45f * cpu_accent.heat);
        const D2D1_COLOR_F c1 = lerp_color(argb(255, 94, 234, 212), cpu_accent.color, 0.35f + 0.65f * cpu_accent.heat);
        const D2D1_COLOR_F c2 = argb(255, 130, 215, 255);
        const D2D1_COLOR_F c3 = lerp_color(argb(255, 184, 132, 255), gpu_accent.color, 0.40f + 0.60f * gpu_accent.heat);
        const D2D1_COLOR_F c4 = lerp_color(argb(255, 244, 114, 182), gpu_accent.color, 0.55f + 0.45f * gpu_accent.heat);

        const float peak_heat = std::max(cpu_accent.heat, gpu_accent.heat);
        const float prism_alpha = std::clamp(0.56f + 0.14f * expand + 0.22f * peak_heat + (hover_min ? 0.10f : 0.0f), 0.0f, 0.92f);

        D2D1_GRADIENT_STOP pstops[5] = {
            {0.00f, with_alpha(c0, prism_alpha)},
            {0.26f, with_alpha(c1, prism_alpha * 0.92f)},
            {0.50f, with_alpha(c2, prism_alpha * 0.78f)},
            {0.74f, with_alpha(c3, prism_alpha * 0.92f)},
            {1.00f, with_alpha(c4, prism_alpha)}
        };
        ComPtr<ID2D1GradientStopCollection> pcol;
        if (SUCCEEDED(dc->CreateGradientStopCollection(pstops, 5, &pcol)) && pcol) {
            ComPtr<ID2D1LinearGradientBrush> pbrush;
            if (SUCCEEDED(dc->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties({cr.left, cr.top}, {cr.right, cr.bottom}),
                    pcol.Get(), &pbrush)) && pbrush) {
                dc->DrawRoundedRectangle(border_rr, pbrush.Get(), 1.25f);
            }
        }
    } else {
        // Liquid Glass & Fluent Acrylic: functional thermal-reactive CPU (left) <-> GPU (right) edge accent.
        const float base_scale = (theme == VisualTheme::FluentAcrylic) ? 0.76f : 1.0f;
        const float cpu_a = std::clamp((0.28f + 0.36f * cpu_accent.heat + 0.06f * expand) * base_scale, 0.0f, 0.72f);
        const float gpu_a = std::clamp((0.28f + 0.36f * gpu_accent.heat + 0.06f * expand) * base_scale, 0.0f, 0.72f);

        D2D1_GRADIENT_STOP tstops[5] = {
            {0.00f, with_alpha(cpu_accent.color, cpu_a)},
            {0.34f, with_alpha(cpu_accent.color, cpu_a * 0.45f)},
            {0.50f, argb(14, 255, 255, 255)},
            {0.66f, with_alpha(gpu_accent.color, gpu_a * 0.45f)},
            {1.00f, with_alpha(gpu_accent.color, gpu_a)}
        };
        ComPtr<ID2D1GradientStopCollection> tcol;
        if (SUCCEEDED(dc->CreateGradientStopCollection(tstops, 5, &tcol)) && tcol) {
            ComPtr<ID2D1LinearGradientBrush> tbrush;
            if (SUCCEEDED(dc->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties({cr.left, cr.top}, {cr.right, cr.top}),
                    tcol.Get(), &tbrush)) && tbrush) {
                const float stroke_w = (std::max(cpu_accent.heat, gpu_accent.heat) > 0.55f) ? 1.2f : 1.0f;
                dc->DrawRoundedRectangle(border_rr, tbrush.Get(), stroke_w);
            }
        }
    }
}

void OverlayRenderer::render(ID2D1DeviceContext* dc, const OverlayRenderState& st, DashboardLayout& out_layout) const {
    if (!dc || !ctx_ || !st.settings) return;

    const UserSettings& s = *st.settings;
    const VisualTheme theme = s.theme;
    const bool is_glass = is_glass_theme(theme);
    const D2D1_RECT_F cr = st.chrome_rect;
    const float cw = cr.right - cr.left;
    const float ch = cr.bottom - cr.top;
    if (cw <= 4.0f || ch <= 4.0f) return;

    out_layout = compute_dashboard_layout(cr, st.has_status_banner);

    const auto cpu_temp = (st.snapshot && st.snapshot->cpu) ? st.snapshot->cpu->temperature : std::nullopt;
    const bool gpu_sleep = (st.snapshot && st.snapshot->gpu && st.snapshot->gpu->is_sleeping);
    const auto gpu_temp = (st.snapshot && st.snapshot->gpu && !gpu_sleep) ? st.snapshot->gpu->temperature : std::nullopt;
    const ThermalAccent cpu_accent = thermal_edge_accent(cpu_temp, false, false, s);
    const ThermalAccent gpu_accent = thermal_edge_accent(gpu_temp, true, gpu_sleep, s);

    // 1) Soft outer drop shadow
    draw_shadow(dc, cr, st.corner_radius, is_glass, st.state);

    // 2) Draw the state-aware tint over the compositor backdrop or the Precision Dark surface.
    const D2D1_COLOR_F surface_color = resolve_surface_color(st, theme);
    const D2D1_ROUNDED_RECT outer_rr = D2D1::RoundedRect(cr, st.corner_radius, st.corner_radius);
    ComPtr<ID2D1SolidColorBrush> surface_brush;
    dc->CreateSolidColorBrush(surface_color, &surface_brush);
    if (surface_brush) {
        dc->FillRoundedRectangle(outer_rr, surface_brush.Get());
    }

    // 3) Clipped inner content (ambient thermal glows, acrylic grain, curved double rim, and 3 state surfaces)
    ComPtr<ID2D1RoundedRectangleGeometry> clip_geom;
    const D2D1_RECT_F inner_r{cr.left + 1.0f, cr.top + 1.0f, cr.right - 1.0f, cr.bottom - 1.0f};
    const float inner_rad = std::max(0.5f, st.corner_radius - 1.0f);
    ctx_->d2d_factory()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(inner_r, inner_rad, inner_rad), &clip_geom);

    if (clip_geom) {
        dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip_geom.Get()), nullptr);
    }

    if (is_glass) {
        auto draw_corner_glow = [&](float cx_frac, D2D1_COLOR_F base, float alpha) {
            D2D1_GRADIENT_STOP stops[2] = {
                {0.0f, {base.r, base.g, base.b, alpha}},
                {1.0f, {base.r, base.g, base.b, 0.0f}}
            };
            ComPtr<ID2D1GradientStopCollection> scol;
            if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &scol)) && scol) {
                const D2D1_POINT_2F center{cr.left + cw * cx_frac, cr.top + ch * 0.16f};
                ComPtr<ID2D1RadialGradientBrush> rbrush;
                if (SUCCEEDED(dc->CreateRadialGradientBrush(
                        D2D1::RadialGradientBrushProperties(center, {0, 0}, cw * 0.56f, ch * 0.72f),
                        scol.Get(), &rbrush)) && rbrush) {
                    dc->FillRectangle(cr, rbrush.Get());
                }
            }
        };

        const float theme_glow = (theme == VisualTheme::FluentAcrylic) ? 0.65f
                               : (theme == VisualTheme::ChromaticPrism ? 1.15f : 1.0f);
        const float glow_mul = theme_glow * (1.0f - 0.26f * st.minimal_opacity);
        const float cpu_glow_a = ((44.0f + 24.0f * cpu_accent.heat) / 255.0f) * glow_mul;
        const float gpu_glow_a = gpu_sleep
            ? ((20.0f / 255.0f) * glow_mul)
            : (((46.0f + 24.0f * gpu_accent.heat) / 255.0f) * glow_mul);

        draw_corner_glow(0.14f, cpu_accent.color, cpu_glow_a);
        draw_corner_glow(0.86f, gpu_accent.color, gpu_glow_a);

        // Tactile micro-grain texture for Fluent Acrylic (cached GPU tile, 0% idle cost).
        if (theme == VisualTheme::FluentAcrylic) {
            draw_acrylic_grain(dc, cr, 0.085f);
        }

        // Animated curved double specular rim (blooms smoothly as Peek / Dashboard opens).
        draw_curved_double_rim(dc, st, theme);
    }

    // Draw visible state surfaces (with crossfade opacity during transitions)
    if (st.minimal_opacity > 0.01f) {
        draw_minimal(dc, st);
    }
    if (st.peek_opacity > 0.01f) {
        draw_peek(dc, st);
    }
    if (st.dashboard_opacity > 0.01f) {
        draw_dashboard(dc, st, out_layout);
    }

    if (clip_geom) {
        dc->PopLayer();
    }

    // 4) Crisp 1px glass border + thermal-reactive or iridescent prism edge accent
    draw_glass_border(dc, st, theme, cpu_accent, gpu_accent);
}

void OverlayRenderer::draw_minimal(ID2D1DeviceContext* dc, const OverlayRenderState& st) const {
    const UserSettings& s = *st.settings;
    const bool is_glass = is_glass_theme(s.theme);
    const float op = st.minimal_opacity;
    const D2D1_RECT_F cr = st.chrome_rect;

    if (!is_glass && st.hovered_chrome && st.state == IslandState::Minimal) {
        ComPtr<ID2D1SolidColorBrush> hov;
        dc->CreateSolidColorBrush(argb(static_cast<uint8_t>(14 * op), 255, 255, 255), &hov);
        if (hov) dc->FillRectangle(cr, hov.Get());
    }

    const auto cpu = st.snapshot ? st.snapshot->cpu : std::nullopt;
    const auto gpu = st.snapshot ? st.snapshot->gpu : std::nullopt;
    const bool gpu_sleep = gpu.has_value() && gpu->is_sleeping;
    const auto pill_text = [is_glass](D2D1_COLOR_F color) {
        return is_glass ? lerp_color(color, kTextColor, 0.38f) : color;
    };
    const auto temperature_text_color = [&](const std::optional<float>& value, bool is_gpu) {
        return value && std::isfinite(*value)
            ? pill_text(thermal_color(value, is_gpu, s))
            : kPowerColor;
    };

    const auto cpu_value = cpu ? cpu->temperature : std::nullopt;
    const auto gpu_value = gpu ? gpu->temperature : std::nullopt;
    const std::wstring cpu_temp = fmt_num(cpu_value, L"%.0f", L"\u00B0C");
    const std::wstring cpu_sec = format_secondary(cpu, s.collapsed_mode);
    const D2D1_COLOR_F cpu_c = temperature_text_color(cpu_value, false);

    const std::wstring gpu_temp = gpu_sleep ? L"Idle" : fmt_num(gpu_value, L"%.0f", L"\u00B0C");
    const std::wstring gpu_sec = gpu_sleep ? L"D3" : format_secondary(gpu, s.collapsed_mode);
    const D2D1_COLOR_F gpu_c = gpu_sleep ? pill_text(kQuietColor) : temperature_text_color(gpu_value, true);

    const float cy = (cr.top + cr.bottom) * 0.5f;
    const float cx = (cr.left + cr.right) * 0.5f;

    // Vertical divider
    ComPtr<ID2D1SolidColorBrush> div_b;
    dc->CreateSolidColorBrush(with_alpha(argb(36, 255, 255, 255), op), &div_b);
    if (div_b) {
        dc->DrawLine({cx, cy - 5.5f}, {cx, cy + 5.5f}, div_b.Get(), 1.0f);
    }

    const bool show_sec = (s.collapsed_mode != CollapsedDisplayMode::TempOnly);
    const float sec_w = (s.collapsed_mode == CollapsedDisplayMode::TempLoad) ? 33.0f : (show_sec ? 45.0f : 0.0f);
    const float block_w = 5.5f + 4.0f + 34.0f + (show_sec ? (4.0f + sec_w) : 0.0f);

    auto draw_side = [&](float side_left, float side_right, D2D1_COLOR_F dot_c,
                         const std::wstring& temp, D2D1_COLOR_F temp_c,
                         const std::wstring& sec, D2D1_COLOR_F sec_c) {
        const float start_x = side_left + ((side_right - side_left) - block_w) * 0.5f;
        ComPtr<ID2D1SolidColorBrush> db;
        dc->CreateSolidColorBrush(with_alpha(dot_c, op), &db);
        if (db) {
            dc->FillEllipse(D2D1::Ellipse({start_x + 2.75f, cy}, 2.75f, 2.75f), db.Get());
        }
        const float temp_x = start_x + 9.5f;
        draw_text(dc, temp, fmt_pill_temp_.Get(), {temp_x, cr.top, temp_x + 34.0f, cr.bottom}, temp_c, op);
        if (show_sec) {
            const float sec_x = temp_x + 34.0f + 4.0f;
            draw_text(dc, sec, fmt_pill_sec_.Get(), {sec_x, cr.top, sec_x + sec_w, cr.bottom}, sec_c, op);
        }
    };

    draw_side(cr.left + 11.0f, cx - 6.0f, kCpuColor, cpu_temp, cpu_c, cpu_sec, kPowerColor);
    draw_side(cx + 6.0f, cr.right - 11.0f, gpu_sleep ? kSleepDot : kGpuColor,
              gpu_temp, gpu_c, gpu_sec, gpu_sleep ? pill_text(kQuietColor) : kPowerColor);
}

void OverlayRenderer::draw_peek(ID2D1DeviceContext* dc, const OverlayRenderState& st) const {
    const UserSettings& s = *st.settings;
    const float op = st.peek_opacity;
    const D2D1_RECT_F cr = st.chrome_rect;

    const auto cpu = st.snapshot ? st.snapshot->cpu : std::nullopt;
    const auto gpu = st.snapshot ? st.snapshot->gpu : std::nullopt;
    const bool gpu_sleep = gpu.has_value() && gpu->is_sleeping;

    const float pad_x = 14.0f;
    const float pad_y = 10.0f;
    const float cx = (cr.left + cr.right) * 0.5f;

    ComPtr<ID2D1SolidColorBrush> div_b;
    dc->CreateSolidColorBrush(with_alpha(argb(30, 255, 255, 255), op), &div_b);
    if (div_b) {
        dc->DrawLine({cx, cr.top + 16.0f}, {cx, cr.bottom - 16.0f}, div_b.Get(), 1.0f);
    }

    auto draw_half = [&](float left, float right, bool is_gpu, const std::optional<ProcessorReading>& r, bool sleeping) {
        const float y0 = cr.top + pad_y;
        ComPtr<ID2D1SolidColorBrush> db;
        dc->CreateSolidColorBrush(with_alpha(sleeping ? kSleepDot : (is_gpu ? kGpuColor : kCpuColor), op), &db);
        if (db) {
            dc->FillEllipse(D2D1::Ellipse({left + 2.75f, y0 + 6.0f}, 2.75f, 2.75f), db.Get());
        }
        draw_text(dc, is_gpu ? L"GPU" : L"CPU", fmt_caption_.Get(), {left + 10.5f, y0, left + 40.0f, y0 + 12.0f}, kQuietColor, op, false);
        if (!sleeping && r.has_value() && r->throttle != ThrottleNone && op > 0.7f) {
            draw_throttle_badge(dc, left + 36.0f, y0 - 0.5f, r->throttle);
        }

        const std::wstring temp_str = sleeping ? L"Idle" : fmt_num(r ? r->temperature : std::nullopt, L"%.0f", L"\u00B0C");
        const D2D1_COLOR_F temp_c = sleeping ? kQuietColor : thermal_color(r ? r->temperature : std::nullopt, is_gpu, s);
        draw_text(dc, temp_str, fmt_peek_temp_.Get(), {left, y0 + 13.0f, right, y0 + 38.0f}, temp_c, op);

        const float sub_y = y0 + 39.0f;
        if (sleeping) {
            draw_text(dc, L"dGPU sleeping (D3)", fmt_peek_sub_left_.Get(), {left, sub_y, right, sub_y + 15.0f}, kQuietColor, op, false);
        } else {
            draw_text(dc, fmt_num(r ? r->power : std::nullopt, L"%.1f", L"W"), fmt_peek_sub_left_.Get(), {left, sub_y, left + 38.0f, sub_y + 15.0f}, kSubtleColor, op);
            draw_text(dc, L"\x00B7", fmt_peek_sub_center_.Get(), {left + 38.0f, sub_y, left + 47.0f, sub_y + 15.0f}, kQuietColor, op, false);
            draw_text(dc, fmt_num(r ? r->load : std::nullopt, L"%.0f", L"%"), fmt_peek_sub_right_.Get(), {left + 47.0f, sub_y, left + 73.0f, sub_y + 15.0f}, kSubtleColor, op);
            draw_text(dc, L"\x00B7", fmt_peek_sub_center_.Get(), {left + 73.0f, sub_y, left + 82.0f, sub_y + 15.0f}, kQuietColor, op, false);
            draw_text(dc, fmt_clock(r ? r->clock : std::nullopt), fmt_peek_sub_ellipsis_.Get(), {left + 82.0f, sub_y, right, sub_y + 15.0f}, kSubtleColor, op);
        }
    };

    draw_half(cr.left + pad_x, cx - 11.0f, false, cpu, false);
    draw_half(cx + 11.0f, cr.right - pad_x, true, gpu, gpu_sleep);
}

void OverlayRenderer::draw_dashboard(ID2D1DeviceContext* dc, const OverlayRenderState& st, const DashboardLayout& lay) const {
    const UserSettings& s = *st.settings;
    const bool is_glass = is_glass_theme(s.theme);
    const float op = st.dashboard_opacity;

    D2D1_MATRIX_3X2_F saved_xf{};
    dc->GetTransform(&saved_xf);
    if (std::fabs(st.dashboard_slide_y) > 0.05f) {
        dc->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, st.dashboard_slide_y) * saved_xf);
    }

    const auto cpu = st.snapshot ? st.snapshot->cpu : std::nullopt;
    const auto gpu = st.snapshot ? st.snapshot->gpu : std::nullopt;
    const bool gpu_sleep = gpu.has_value() && gpu->is_sleeping;

    // Header hover highlight
    if (st.hovered_region == HitRegion::DashboardHeader) {
        ComPtr<ID2D1SolidColorBrush> hb;
        dc->CreateSolidColorBrush(with_alpha(argb(10, 255, 255, 255), op), &hb);
        if (hb) dc->FillRoundedRectangle(D2D1::RoundedRect(lay.header, 10.0f, 10.0f), hb.Get());
    }

    // Header CPU & GPU halves
    const float hcx = (lay.header.left + lay.header.right) * 0.5f;
    ComPtr<ID2D1SolidColorBrush> div_b;
    dc->CreateSolidColorBrush(with_alpha(argb(28, 255, 255, 255), op), &div_b);
    if (div_b) {
        dc->DrawLine({hcx, lay.header.top + 10.0f}, {hcx, lay.header.bottom - 10.0f}, div_b.Get(), 1.0f);
    }

    auto draw_header_half = [&](float left, float right, bool is_gpu, const std::optional<ProcessorReading>& r, bool sleeping) {
        const float y0 = lay.header.top + 2.0f;
        ComPtr<ID2D1SolidColorBrush> db;
        dc->CreateSolidColorBrush(with_alpha(sleeping ? kSleepDot : (is_gpu ? kGpuColor : kCpuColor), op), &db);
        if (db) {
            dc->FillEllipse(D2D1::Ellipse({left + 2.75f, y0 + 5.5f}, 2.75f, 2.75f), db.Get());
        }
        draw_text(dc, is_gpu ? L"GPU" : L"CPU", fmt_caption_.Get(), {left + 10.0f, y0, left + 40.0f, y0 + 11.0f}, kQuietColor, op, false);
        if (!sleeping && r.has_value() && r->throttle != ThrottleNone && op > 0.7f) {
            draw_throttle_badge(dc, left + 35.0f, y0 - 1.0f, r->throttle);
        }

        const std::wstring temp_s = sleeping ? L"Idle" : fmt_num(r ? r->temperature : std::nullopt, L"%.0f", L"\u00B0C");
        const D2D1_COLOR_F temp_c = sleeping ? kQuietColor : thermal_color(r ? r->temperature : std::nullopt, is_gpu, s);
        const std::wstring pwr_s = sleeping ? L"0.0W" : fmt_num(r ? r->power : std::nullopt, L"%.1f", L"W");

        draw_text(dc, temp_s, fmt_dash_temp_.Get(), {left, y0 + 12.5f, left + 42.0f, y0 + 32.0f}, temp_c, op);
        draw_text(dc, pwr_s, fmt_dash_power_.Get(), {left + 45.0f, y0 + 12.5f, right, y0 + 31.0f}, kPowerColor, op);

        const float y2 = y0 + 33.5f;
        if (sleeping) {
            draw_text(dc, L"dGPU sleeping (D3)", fmt_dash_sub_left_.Get(), {left, y2, right, y2 + 13.0f}, kQuietColor, op, false);
        } else {
            draw_text(dc, fmt_num(r ? r->load : std::nullopt, L"%.0f", L"%"), fmt_dash_sub_left_.Get(), {left, y2, left + 28.0f, y2 + 13.0f}, kQuietColor, op);
            draw_text(dc, L"\x00B7", fmt_dash_sub_center_.Get(), {left + 28.0f, y2, left + 38.0f, y2 + 13.0f}, kQuietColor, op, false);
            draw_text(dc, fmt_clock(r ? r->clock : std::nullopt), fmt_dash_sub_ellipsis_.Get(), {left + 38.0f, y2, right, y2 + 13.0f}, kQuietColor, op);
        }
    };

    draw_header_half(lay.header.left + 2.0f, hcx - 10.0f, false, cpu, false);
    draw_header_half(hcx + 10.0f, lay.header.right - 2.0f, true, gpu, gpu_sleep);

    // Pin & Collapse circular glass icon buttons
    auto draw_icon_btn = [&](const D2D1_RECT_F& r, HitRegion reg, bool active_pin) {
        const bool hov = (st.hovered_region == reg);
        const bool prs = (st.pressed_region == reg);
        D2D1_COLOR_F bg = argb(0, 255, 255, 255);
        D2D1_COLOR_F brd = argb(0, 255, 255, 255);
        if (active_pin) {
            bg = argb(34, 111, 231, 207);
            brd = argb(70, 111, 231, 207);
        } else if (prs) {
            bg = argb(48, 255, 255, 255);
        } else if (hov) {
            bg = argb(30, 255, 255, 255);
        }
        if (bg.a > 0.0f) {
            ComPtr<ID2D1SolidColorBrush> b;
            dc->CreateSolidColorBrush(with_alpha(bg, op), &b);
            if (b) dc->FillRoundedRectangle(D2D1::RoundedRect(r, 12.0f, 12.0f), b.Get());
        }
        if (brd.a > 0.0f) {
            ComPtr<ID2D1SolidColorBrush> b;
            dc->CreateSolidColorBrush(with_alpha(brd, op), &b);
            if (b) dc->DrawRoundedRectangle(D2D1::RoundedRect(r, 12.0f, 12.0f), b.Get(), 1.0f);
        }
    };

    draw_icon_btn(lay.pin_btn, HitRegion::PinButton, st.pinned);
    {
        // Pin glyph centered in 24x24
        const float px = lay.pin_btn.left + 6.75f;
        const float py = lay.pin_btn.top + 6.25f;
        const D2D1_POINT_2F pts[] = {
            {px + 2.6f, py + 0.0f}, {px + 7.9f, py + 0.0f}, {px + 7.2f, py + 4.7f},
            {px + 10.5f, py + 6.8f}, {px + 5.9f, py + 6.8f}, {px + 5.25f, py + 11.5f},
            {px + 4.6f, py + 6.8f}, {px + 0.0f, py + 6.8f}, {px + 3.3f, py + 4.7f}
        };
        ComPtr<ID2D1PathGeometry> geom;
        if (SUCCEEDED(ctx_->d2d_factory()->CreatePathGeometry(&geom)) && geom) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(geom->Open(&sink)) && sink) {
                sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLines(pts + 1, static_cast<UINT32>(_countof(pts) - 1));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                ComPtr<ID2D1SolidColorBrush> pb;
                dc->CreateSolidColorBrush(with_alpha(st.pinned ? kCpuColor : kQuietColor, op), &pb);
                if (pb) dc->FillGeometry(geom.Get(), pb.Get());
            }
        }
    }

    draw_icon_btn(lay.collapse_btn, HitRegion::CollapseButton, false);
    {
        // Chevron-up collapse icon
        const float cx0 = lay.collapse_btn.left + 7.5f;
        const float cy0 = lay.collapse_btn.top + 9.75f;
        ComPtr<ID2D1SolidColorBrush> cb;
        dc->CreateSolidColorBrush(with_alpha(argb(255, 168, 178, 200), op), &cb);
        ComPtr<ID2D1StrokeStyle> round_cap;
        ctx_->d2d_factory()->CreateStrokeStyle(
            D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
            nullptr, 0, &round_cap);
        if (cb) {
            dc->DrawLine({cx0, cy0 + 4.5f}, {cx0 + 4.5f, cy0}, cb.Get(), 1.6f, round_cap.Get());
            dc->DrawLine({cx0 + 4.5f, cy0}, {cx0 + 9.0f, cy0 + 4.5f}, cb.Get(), 1.6f, round_cap.Get());
        }
    }

    // Segmented control track + sliding glass thumb
    {
        ComPtr<ID2D1SolidColorBrush> tbg, tbrd;
        dc->CreateSolidColorBrush(with_alpha(is_glass ? argb(150, 8, 10, 18) : argb(224, 11, 13, 18), op), &tbg);
        dc->CreateSolidColorBrush(with_alpha(argb(30, 255, 255, 255), op), &tbrd);
        const D2D1_ROUNDED_RECT trr = D2D1::RoundedRect(lay.seg_track, 13.0f, 13.0f);
        if (tbg) dc->FillRoundedRectangle(trr, tbg.Get());
        if (tbrd) dc->DrawRoundedRectangle(trr, tbrd.Get(), 1.0f);

        const float tab_w = (lay.seg_track.right - lay.seg_track.left - 4.0f) / 4.0f;
        const float thumb_l = lay.seg_track.left + 2.0f + st.seg_thumb_pos * tab_w;
        const D2D1_RECT_F thumb_r{thumb_l, lay.seg_track.top + 2.0f, thumb_l + tab_w, lay.seg_track.bottom - 2.0f};
        const D2D1_ROUNDED_RECT thumb_rr = D2D1::RoundedRect(thumb_r, 11.0f, 11.0f);

        if (is_glass) {
            D2D1_GRADIENT_STOP ts[2] = {
                {0.0f, with_alpha(argb(59, 255, 255, 255), op)},
                {1.0f, with_alpha(argb(30, 255, 255, 255), op)}
            };
            ComPtr<ID2D1GradientStopCollection> tc;
            if (SUCCEEDED(dc->CreateGradientStopCollection(ts, 2, &tc)) && tc) {
                ComPtr<ID2D1LinearGradientBrush> tb;
                if (SUCCEEDED(dc->CreateLinearGradientBrush(
                        D2D1::LinearGradientBrushProperties({thumb_r.left, thumb_r.top}, {thumb_r.left, thumb_r.bottom}),
                        tc.Get(), &tb)) && tb) {
                    dc->FillRoundedRectangle(thumb_rr, tb.Get());
                }
            }
        } else {
            ComPtr<ID2D1SolidColorBrush> tb;
            dc->CreateSolidColorBrush(with_alpha(argb(34, 255, 255, 255), op), &tb);
            if (tb) dc->FillRoundedRectangle(thumb_rr, tb.Get());
        }

        ComPtr<ID2D1SolidColorBrush> th_brd;
        dc->CreateSolidColorBrush(with_alpha(is_glass ? argb(54, 255, 255, 255) : argb(28, 255, 255, 255), op), &th_brd);
        if (th_brd) dc->DrawRoundedRectangle(thumb_rr, th_brd.Get(), 1.0f);

        const wchar_t* labels[4] = {L"Temp", L"Power", L"Load", L"Clock"};
        const HitRegion tab_regs[4] = {HitRegion::TabTemp, HitRegion::TabPower, HitRegion::TabLoad, HitRegion::TabClock};
        for (int i = 0; i < 4; ++i) {
            const bool active = (static_cast<int>(st.metric) == i);
            const bool hov = (st.hovered_region == tab_regs[i]);
            draw_text(dc, labels[i], active ? fmt_seg_tab_bold_.Get() : fmt_seg_tab_.Get(),
                      lay.tab_rects[i], (active || hov) ? kTextColor : kQuietColor, op, false);
        }
    }

    // Range pill button + live indicator dot
    {
        if (st.hovered_region == HitRegion::RangeButton) {
            ComPtr<ID2D1SolidColorBrush> rb, rbrd;
            dc->CreateSolidColorBrush(with_alpha(argb(20, 255, 255, 255), op), &rb);
            dc->CreateSolidColorBrush(with_alpha(argb(32, 255, 255, 255), op), &rbrd);
            const D2D1_ROUNDED_RECT rrr = D2D1::RoundedRect(lay.range_btn, 10.0f, 10.0f);
            if (rb) dc->FillRoundedRectangle(rrr, rb.Get());
            if (rbrd) dc->DrawRoundedRectangle(rrr, rbrd.Get(), 1.0f);
        }

        const bool has_err = (st.snapshot && st.snapshot->error.has_value());
        const D2D1_COLOR_F dot_c = has_err ? kWarmColor : kLiveGreen;
        const float dot_op = op * (st.live_heartbeat_bright ? 1.0f : 0.55f);
        const D2D1_POINT_2F dot_pt{lay.range_btn.left + 12.0f, (lay.range_btn.top + lay.range_btn.bottom) * 0.5f};

        D2D1_GRADIENT_STOP ds[2] = {
            {0.0f, with_alpha(dot_c, 0.6f * dot_op)},
            {1.0f, with_alpha(dot_c, 0.0f)}
        };
        ComPtr<ID2D1GradientStopCollection> dc_col;
        if (SUCCEEDED(dc->CreateGradientStopCollection(ds, 2, &dc_col)) && dc_col) {
            ComPtr<ID2D1RadialGradientBrush> gb;
            if (SUCCEEDED(dc->CreateRadialGradientBrush(
                    D2D1::RadialGradientBrushProperties(dot_pt, {0, 0}, 6.0f, 6.0f),
                    dc_col.Get(), &gb)) && gb) {
                dc->FillEllipse(D2D1::Ellipse(dot_pt, 6.0f, 6.0f), gb.Get());
            }
        }
        ComPtr<ID2D1SolidColorBrush> db;
        dc->CreateSolidColorBrush(with_alpha(dot_c, dot_op), &db);
        if (db) dc->FillEllipse(D2D1::Ellipse(dot_pt, 3.0f, 3.0f), db.Get());

        const wchar_t* range_str = (s.history_range == TimeRange::Minutes5) ? L"5m"
                                 : (s.history_range == TimeRange::Minutes15) ? L"15m" : L"60s";
        draw_text(dc, range_str, fmt_range_.Get(),
                  {lay.range_btn.left + 21.0f, lay.range_btn.top, lay.range_btn.right - 4.0f, lay.range_btn.bottom},
                  kQuietColor, op, false);
    }

    // Dual hardware glass cards
    draw_card(dc, st, lay.cpu_card, lay.cpu_chart, false);
    draw_card(dc, st, lay.gpu_card, lay.gpu_chart, true);

    // Conditional warning banner
    if (st.has_status_banner && !st.warning_text.empty()) {
        ComPtr<ID2D1SolidColorBrush> bbg, bbrd, bdot;
        dc->CreateSolidColorBrush(with_alpha(argb(38, 251, 191, 36), op), &bbg);
        dc->CreateSolidColorBrush(with_alpha(argb(72, 251, 191, 36), op), &bbrd);
        const bool err = (st.snapshot && st.snapshot->error.has_value());
        dc->CreateSolidColorBrush(with_alpha(err ? kHotColor : kWarmColor, op), &bdot);

        const D2D1_ROUNDED_RECT brr = D2D1::RoundedRect(lay.status_banner, 8.0f, 8.0f);
        if (bbg) dc->FillRoundedRectangle(brr, bbg.Get());
        if (bbrd) dc->DrawRoundedRectangle(brr, bbrd.Get(), 1.0f);
        const float bcy = (lay.status_banner.top + lay.status_banner.bottom) * 0.5f;
        if (bdot) dc->FillEllipse(D2D1::Ellipse({lay.status_banner.left + 11.0f, bcy}, 2.75f, 2.75f), bdot.Get());

        draw_text(dc, st.warning_text, fmt_banner_.Get(),
                  {lay.status_banner.left + 20.0f, lay.status_banner.top, lay.status_banner.right - 8.0f, lay.status_banner.bottom},
                  kTextColor, op, false);
    }

    dc->SetTransform(saved_xf);
}

void OverlayRenderer::draw_card(
    ID2D1DeviceContext* dc,
    const OverlayRenderState& st,
    const D2D1_RECT_F& card,
    const D2D1_RECT_F& chart_rect,
    bool is_gpu) const {

    const UserSettings& s = *st.settings;
    const VisualTheme theme = s.theme;
    const bool is_glass = is_glass_theme(theme);
    const float op = st.dashboard_opacity;

    const float rad = (theme == VisualTheme::PrecisionDark) ? 8.0f
                    : (theme == VisualTheme::FluentAcrylic ? 11.0f : 14.0f);

    // Darkened recessed glass card well so small labels & colored chart lines stay high-contrast over pure white windows.
    D2D1_COLOR_F card_fill = argb(255, 26, 30, 40);
    D2D1_COLOR_F card_border = argb(255, 49, 56, 71);
    if (is_glass) {
        if (theme == VisualTheme::FluentAcrylic) {
            card_fill = argb(132, 10, 12, 20);
            card_border = argb(40, 255, 255, 255);
        } else if (theme == VisualTheme::ChromaticPrism) {
            card_fill = argb(128, 7, 10, 18);
            card_border = argb(38, 255, 255, 255);
        } else {
            card_fill = argb(122, 8, 11, 20);
            card_border = argb(36, 255, 255, 255);
        }
    }

    ComPtr<ID2D1SolidColorBrush> cbg;
    dc->CreateSolidColorBrush(with_alpha(card_fill, op), &cbg);

    const D2D1_ROUNDED_RECT crr = D2D1::RoundedRect(card, rad, rad);
    if (cbg) dc->FillRoundedRectangle(crr, cbg.Get());

    if (is_glass && theme == VisualTheme::ChromaticPrism) {
        const auto reading = st.snapshot ? (is_gpu ? st.snapshot->gpu : st.snapshot->cpu) : std::nullopt;
        const bool sleeping = is_gpu && reading.has_value() && reading->is_sleeping;
        const auto accent = thermal_edge_accent(
            (reading && !sleeping) ? reading->temperature : std::nullopt, is_gpu, sleeping, s);
        D2D1_GRADIENT_STOP cstops[2] = {
            {0.0f, with_alpha(accent.color, (0.32f + 0.20f * accent.heat) * op)},
            {1.0f, with_alpha(argb(30, 255, 255, 255), op)}
        };
        ComPtr<ID2D1GradientStopCollection> ccol;
        if (SUCCEEDED(dc->CreateGradientStopCollection(cstops, 2, &ccol)) && ccol) {
            ComPtr<ID2D1LinearGradientBrush> cbrd;
            if (SUCCEEDED(dc->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties({card.left, card.top}, {card.right, card.bottom}),
                    ccol.Get(), &cbrd)) && cbrd) {
                dc->DrawRoundedRectangle(crr, cbrd.Get(), 1.0f);
            }
        }
    } else {
        ComPtr<ID2D1SolidColorBrush> cbrd;
        dc->CreateSolidColorBrush(with_alpha(card_border, op), &cbrd);
        if (cbrd) dc->DrawRoundedRectangle(crr, cbrd.Get(), 1.0f);
    }

    // Subtle top specular lip inside glass chart card
    if (is_glass) {
        const D2D1_ROUNDED_RECT inner_crr = D2D1::RoundedRect(
            {card.left + 1.0f, card.top + 1.0f, card.right - 1.0f, card.bottom - 1.0f},
            std::max(1.0f, rad - 1.0f), std::max(1.0f, rad - 1.0f));
        D2D1_GRADIENT_STOP lip_stops[3] = {
            {0.0f, with_alpha(argb(34, 255, 255, 255), op)},
            {0.18f, with_alpha(argb(8, 255, 255, 255), op)},
            {0.35f, with_alpha(argb(0, 255, 255, 255), op)}
        };
        ComPtr<ID2D1GradientStopCollection> lcol;
        if (SUCCEEDED(dc->CreateGradientStopCollection(lip_stops, 3, &lcol)) && lcol) {
            ComPtr<ID2D1LinearGradientBrush> lbrush;
            if (SUCCEEDED(dc->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties({card.left, card.top}, {card.left, card.bottom}),
                    lcol.Get(), &lbrush)) && lbrush) {
                dc->DrawRoundedRectangle(inner_crr, lbrush.Get(), 1.0f);
            }
        }
    }

    const float l = card.left + 10.0f;
    const float r = card.right - 10.0f;
    const float t = card.top + 8.0f;

    draw_text(dc, is_gpu ? L"GPU" : L"CPU", fmt_card_title_.Get(), {l, t, r, t + 13.0f},
              is_gpu ? kGpuColor : kCpuColor, op, false);

    std::wstring model;
    if (!st.snapshot) {
        model = L"Detecting\x2026";
    } else if (!is_gpu) {
        model = st.snapshot->cpu_name ? utf8_to_wide(*st.snapshot->cpu_name) : L"CPU unavailable";
    } else {
        model = st.snapshot->selected_gpu ? utf8_to_wide(st.snapshot->selected_gpu->name) : L"GPU unavailable";
    }
    draw_text(dc, model, fmt_card_model_.Get(), {l, t + 14.0f, r, t + 27.0f}, kQuietColor, op, false);

    const ChartStats& stats = is_gpu ? st.gpu_stats : st.cpu_stats;
    const RingBuffer* hist = is_gpu ? st.gpu_history : st.cpu_history;
    const auto peak = (hist && hist->session_peak(st.metric).has_value()) ? hist->session_peak(st.metric) : stats.max;

    const float stat_y = t + 31.0f;
    draw_text(dc, L"Peak", fmt_stat_label_.Get(), {l, stat_y, l + 25.0f, stat_y + 13.0f}, kQuietColor, op, false);
    draw_text(dc, format_metric(peak, st.metric), fmt_stat_val_.Get(), {l + 25.0f, stat_y, l + 82.0f, stat_y + 13.0f}, kPowerColor, op);

    const std::wstring avg_s = L"Avg " + format_metric(stats.avg, st.metric);
    draw_text(dc, avg_s, fmt_stat_val_right_.Get(), {l + 76.0f, stat_y, r, stat_y + 13.0f}, kPowerColor, op);

    // Chart
    const HistoryChart* chart = is_gpu ? st.gpu_chart : st.cpu_chart;
    if (chart && op > 0.25f) {
        chart->draw(dc, ctx_->d2d_factory(), chart_rect);
    }

    const bool gpu_sleep = is_gpu && st.snapshot && st.snapshot->gpu && st.snapshot->gpu->is_sleeping;
    if (stats.has_data) {
        draw_text(dc, format_metric(stats.max, st.metric), fmt_scale_right_.Get(),
                  {chart_rect.left, chart_rect.top + 1.0f, chart_rect.right - 2.0f, chart_rect.top + 13.0f},
                  argb(184, 154, 164, 186), op);
        draw_text(dc, format_metric(stats.min, st.metric), fmt_scale_right_.Get(),
                  {chart_rect.left, chart_rect.bottom - 13.0f, chart_rect.right - 2.0f, chart_rect.bottom - 1.0f},
                  argb(184, 154, 164, 186), op);
    } else {
        draw_text(dc, gpu_sleep ? L"dGPU sleeping" : L"No readings", fmt_empty_center_.Get(),
                  chart_rect, kQuietColor, op, false);
    }

    // Hover scrub tooltip pill
    if (chart) {
        if (auto scrub = chart->current_scrub()) {
            wchar_t tbuf[32];
            if (scrub->seconds_ago == 0) {
                wcscpy_s(tbuf, L"Now");
            } else if (scrub->seconds_ago >= 60) {
                swprintf_s(tbuf, L"-%dm%02ds", scrub->seconds_ago / 60, scrub->seconds_ago % 60);
            } else {
                swprintf_s(tbuf, L"-%ds", scrub->seconds_ago);
            }
            const std::wstring tip = std::wstring(tbuf) + L": " + format_metric(scrub->value, st.metric);
            const float tip_w = std::clamp(static_cast<float>(tip.size()) * 5.7f + 14.0f, 52.0f, chart_rect.right - chart_rect.left);
            const float tip_h = 17.0f;
            const float cw = std::max(1.0f, chart_rect.right - chart_rect.left);
            const float tx = chart_rect.left + std::clamp(scrub->relative_x * cw - tip_w * 0.5f, 0.0f, std::max(0.0f, cw - tip_w));
            const float ty = chart_rect.top - 16.0f;
            const D2D1_RECT_F tr{tx, ty, tx + tip_w, ty + tip_h};

            ComPtr<ID2D1SolidColorBrush> pbg, pbrd;
            dc->CreateSolidColorBrush(with_alpha(argb(238, 12, 15, 22), op), &pbg);
            dc->CreateSolidColorBrush(with_alpha(argb(42, 255, 255, 255), op), &pbrd);
            if (pbg) dc->FillRoundedRectangle(D2D1::RoundedRect(tr, 6.0f, 6.0f), pbg.Get());
            if (pbrd) dc->DrawRoundedRectangle(D2D1::RoundedRect(tr, 6.0f, 6.0f), pbrd.Get(), 1.0f);
            draw_text(dc, tip, fmt_tooltip_.Get(), tr, kTextColor, op);
        }
    }
}

} // namespace mmoni
