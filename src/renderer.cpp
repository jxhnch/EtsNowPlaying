#include "renderer.h"

#include "cover_lookup.h"
#include "log.h"
#include "media.h"
#include "radio_tracker.h"
#include "settings.h"
#include "text_util.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace ep {
namespace {

using Microsoft::WRL::ComPtr;

D2D1_COLOR_F Rgb(float r, float g, float b, float a = 1.0f) { return D2D1::ColorF(r, g, b, a); }

std::wstring Clock() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t buf[16];
    swprintf(buf, 16, L"%02d:%02d", t.wHour, t.wMinute);
    return buf;
}

std::wstring Duration(double seconds) {
    int s = static_cast<int>(std::max(seconds, 0.0));
    wchar_t buf[24];
    if (s >= 3600) swprintf(buf, 24, L"%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else swprintf(buf, 24, L"%d:%02d", s / 60, s % 60);
    return buf;
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

float Lerp(float a, float b, float t) { return a + (b - a) * t; }

using CoverBytes = std::shared_ptr<const std::vector<uint8_t>>;

constexpr int kBgSize = 96;  // the blurred background is made from a 96x96 copy of the cover

// Box blur on a BGRA image, repeated three times (close to a Gaussian). Edges are clamped.
void Blur(std::vector<uint8_t>& px, int w, int h, int r) {
    std::vector<uint8_t> tmp(px.size());
    for (int round = 0; round < 3; ++round) {
        for (int pass = 0; pass < 2; ++pass) {
            const std::vector<uint8_t>& src = pass == 0 ? px : tmp;
            std::vector<uint8_t>& dst = pass == 0 ? tmp : px;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    int sum[4] = {};
                    for (int k = -r; k <= r; ++k) {
                        int sx = pass == 0 ? std::clamp(x + k, 0, w - 1) : x;
                        int sy = pass == 0 ? y : std::clamp(y + k, 0, h - 1);
                        const uint8_t* p = &src[(static_cast<size_t>(sy) * w + sx) * 4];
                        for (int c = 0; c < 4; ++c) sum[c] += p[c];
                    }
                    uint8_t* d = &dst[(static_cast<size_t>(y) * w + x) * 4];
                    for (int c = 0; c < 4; ++c) d[c] = static_cast<uint8_t>(sum[c] / (2 * r + 1));
                }
            }
        }
    }
}

// Pushes the colours away from grey so the blurred background is clearly tinted by the cover.
void Saturate(std::vector<uint8_t>& px, float amount) {
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        float b = px[i], g = px[i + 1], r = px[i + 2];
        float luma = 0.299f * r + 0.587f * g + 0.114f * b;
        px[i] = static_cast<uint8_t>(std::clamp(luma + (b - luma) * amount, 0.0f, 255.0f));
        px[i + 1] = static_cast<uint8_t>(std::clamp(luma + (g - luma) * amount, 0.0f, 255.0f));
        px[i + 2] = static_cast<uint8_t>(std::clamp(luma + (r - luma) * amount, 0.0f, 255.0f));
    }
}

// The most colourful colour of the picture: pixels vote for their hue sector with saturation * brightness,
// the winning sector's average is returned. Plain averaging turns most covers into brown/grey mud.
D2D1_COLOR_F VibrantColor(const std::vector<uint8_t>& px) {
    double weight[12] = {}, sr[12] = {}, sg[12] = {}, sb[12] = {};
    double total = 0, grey = 0;
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        float b = px[i] / 255.0f, g = px[i + 1] / 255.0f, r = px[i + 2] / 255.0f;
        float mx = std::max({r, g, b}), mn = std::min({r, g, b});
        float v = mx, s = mx > 0 ? (mx - mn) / mx : 0;
        total += 1;
        if (v < 0.18f || s < 0.12f) {
            grey += v;
            continue;
        }
        float delta = mx - mn, hue;
        if (mx == r) hue = std::fmod((g - b) / delta, 6.0f);
        else if (mx == g) hue = (b - r) / delta + 2.0f;
        else hue = (r - g) / delta + 4.0f;
        if (hue < 0) hue += 6.0f;
        int bin = std::clamp(static_cast<int>(hue * 2.0f), 0, 11);
        double w = static_cast<double>(s) * s * (0.3 + v);
        weight[bin] += w;
        sr[bin] += r * w;
        sg[bin] += g * w;
        sb[bin] += b * w;
    }
    int best = 0;
    for (int i = 1; i < 12; ++i) {
        if (weight[i] > weight[best]) best = i;
    }
    if (weight[best] < total * 0.01) {  // a (nearly) grey cover: a soft light grey instead of a made-up tint
        float l = total > 0 ? static_cast<float>(grey / total) : 0.5f;
        l = std::clamp(0.45f + l * 0.4f, 0.5f, 0.85f);
        return Rgb(l, l, l * 1.04f);
    }
    float r = static_cast<float>(sr[best] / weight[best]);
    float g = static_cast<float>(sg[best] / weight[best]);
    float b = static_cast<float>(sb[best] / weight[best]);
    float mx = std::max({r, g, b, 0.001f});
    float lift = mx < 0.6f ? 0.6f / mx : 1.0f;
    return Rgb(std::min(r * lift, 1.0f), std::min(g * lift, 1.0f), std::min(b * lift, 1.0f));
}

class Renderer {
public:
    bool Init();
    void Frame(double t);
    const std::vector<uint8_t>& Pixels() const { return pixels_; }

private:
    ComPtr<IDWriteTextFormat> Format(float size, DWRITE_FONT_WEIGHT weight, bool wrap);
    void Text(const std::wstring& s, float x, float y, float w, float h, float size, DWRITE_FONT_WEIGHT weight,
              D2D1_COLOR_F color, bool wrap = false, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
              DWRITE_PARAGRAPH_ALIGNMENT palign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    void Fill(const D2D1_ROUNDED_RECT& r, D2D1_COLOR_F c);
    void Background(D2D1_COLOR_F accent);
    void SetCover(const CoverBytes& bytes, uint64_t key);
    void DrawCover(float x, float y, float size);
    void DrawClock();
    void DrawMedia(const MediaSnapshot& m, double t);
    void DrawRadio(const RadioSnapshot& r, double t);
    void DrawIdle(double t);
    void CopyOut();

    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IDWriteFactory> dw_;
    ComPtr<IWICImagingFactory> wic_;
    ComPtr<IWICBitmap> bitmap_;
    ComPtr<ID2D1RenderTarget> rt_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    std::map<std::tuple<int, int, bool>, ComPtr<IDWriteTextFormat>> formats_;
    std::vector<uint8_t> pixels_;

    ComPtr<ID2D1Bitmap> cover_;
    ComPtr<ID2D1Bitmap> bg_;  // blurred, saturated copy of the cover for the background
    uint64_t coverKey_ = ~0ull;
    D2D1_COLOR_F coverColor_ = Rgb(0.11f, 0.73f, 0.33f);
    D2D1_COLOR_F accent_ = Rgb(0.11f, 0.73f, 0.33f);

    RadioSnapshot lastRadio_;
    std::chrono::steady_clock::time_point lastRadioTime_{};
    std::string lastCoverRequest_;

    // Soft transitions: new covers and new titles fade in instead of popping.
    double now_ = 0;
    double coverSetAt_ = -10;
    std::string titleKey_;
    double titleChangedAt_ = -10;
    float textAlpha_ = 1.0f;  // multiplied into every text colour
    float CoverFade() const { return Ease(static_cast<float>((now_ - coverSetAt_) / 0.7)); }
    static float Ease(float x) {
        x = std::clamp(x, 0.0f, 1.0f);
        return x * x * (3.0f - 2.0f * x);
    }
};

bool Renderer::Init() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dw_.GetAddressOf())))) return false;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(wic_.GetAddressOf())))) return false;
    if (FAILED(wic_->CreateBitmap(kScreenWidth, kScreenHeight, GUID_WICPixelFormat32bppPBGRA,
                                  WICBitmapCacheOnDemand, bitmap_.GetAddressOf()))) return false;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(d2d_->CreateWicBitmapRenderTarget(bitmap_.Get(), props, rt_.GetAddressOf()))) return false;
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (FAILED(rt_->CreateSolidColorBrush(Rgb(1, 1, 1), brush_.GetAddressOf()))) return false;
    pixels_.assign(static_cast<size_t>(kScreenWidth) * kScreenHeight * 4, 0);
    return true;
}

ComPtr<IDWriteTextFormat> Renderer::Format(float size, DWRITE_FONT_WEIGHT weight, bool wrap) {
    auto key = std::make_tuple(static_cast<int>(size * 10), static_cast<int>(weight), wrap);
    auto it = formats_.find(key);
    if (it != formats_.end()) return it->second;

    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dw_->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                     size, L"en-us", fmt.GetAddressOf()))) return nullptr;
    fmt->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    ComPtr<IDWriteInlineObject> sign;
    if (SUCCEEDED(dw_->CreateEllipsisTrimmingSign(fmt.Get(), sign.GetAddressOf()))) {
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        fmt->SetTrimming(&trim, sign.Get());
    }
    formats_[key] = fmt;
    return fmt;
}

void Renderer::Text(const std::wstring& s, float x, float y, float w, float h, float size, DWRITE_FONT_WEIGHT weight,
                    D2D1_COLOR_F color, bool wrap, DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT palign) {
    if (s.empty()) return;
    auto fmt = Format(size, weight, wrap);
    if (!fmt) return;
    fmt->SetTextAlignment(align);
    fmt->SetParagraphAlignment(palign);
    color.a *= textAlpha_;
    brush_->SetColor(color);
    rt_->DrawText(s.c_str(), static_cast<UINT32>(s.size()), fmt.Get(), D2D1::RectF(x, y, x + w, y + h), brush_.Get(),
                  D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Renderer::Fill(const D2D1_ROUNDED_RECT& r, D2D1_COLOR_F c) {
    brush_->SetColor(c);
    rt_->FillRoundedRectangle(r, brush_.Get());
}

void Renderer::Background(D2D1_COLOR_F accent) {
    rt_->Clear(Rgb(0.04f, 0.045f, 0.055f));
    // Plain tinted gradient: the background without a cover, and what a new cover fades in over.
    D2D1_GRADIENT_STOP stops[2] = {{0.0f, Rgb(accent.r * 0.42f, accent.g * 0.42f, accent.b * 0.42f)},
                                   {1.0f, Rgb(0.04f, 0.045f, 0.055f)}};
    ComPtr<ID2D1GradientStopCollection> coll;
    ComPtr<ID2D1LinearGradientBrush> grad;
    if (SUCCEEDED(rt_->CreateGradientStopCollection(stops, 2, coll.GetAddressOf())) &&
        SUCCEEDED(rt_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(kScreenWidth * 0.8f, kScreenHeight)),
            coll.Get(), grad.GetAddressOf()))) {
        rt_->FillRectangle(D2D1::RectF(0, 0, kScreenWidth, kScreenHeight), grad.Get());
    }
    if (!bg_) return;

    // The cover, blurred and saturated, covers the whole screen.
    const float fade = CoverFade();
    rt_->DrawBitmap(bg_.Get(), D2D1::RectF(0, 0, kScreenWidth, kScreenHeight), fade, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    // Darken a little so the white text stays readable on bright covers.
    D2D1_GRADIENT_STOP shade[2] = {{0.0f, Rgb(0, 0, 0, 0.28f * fade)}, {1.0f, Rgb(0, 0, 0, 0.60f * fade)}};
    ComPtr<ID2D1GradientStopCollection> sc;
    ComPtr<ID2D1LinearGradientBrush> sb;
    if (SUCCEEDED(rt_->CreateGradientStopCollection(shade, 2, sc.GetAddressOf())) &&
        SUCCEEDED(rt_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(kScreenWidth * 0.7f, kScreenHeight)),
            sc.Get(), sb.GetAddressOf()))) {
        rt_->FillRectangle(D2D1::RectF(0, 0, kScreenWidth, kScreenHeight), sb.Get());
    }
}

// Decodes `bytes` (PNG/JPEG) into the shared cover bitmap and derives an accent colour from it.
// `key` identifies the picture: nothing happens while it stays the same.
void Renderer::SetCover(const CoverBytes& bytes, uint64_t key) {
    if (key == coverKey_) return;
    coverKey_ = key;
    coverSetAt_ = now_;
    cover_.Reset();
    bg_.Reset();
    coverColor_ = Rgb(0.11f, 0.73f, 0.33f);
    if (!bytes || bytes->empty()) return;

    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic_->CreateStream(stream.GetAddressOf())) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes->data()), static_cast<DWORD>(bytes->size()))) ||
        FAILED(wic_->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf())) ||
        FAILED(decoder->GetFrame(0, frame.GetAddressOf())) || FAILED(wic_->CreateFormatConverter(conv.GetAddressOf())) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom))) {
        Logf("RENDER cover decode failed");
        return;
    }
    if (FAILED(rt_->CreateBitmapFromWicBitmap(conv.Get(), cover_.GetAddressOf()))) {
        Logf("RENDER cover bitmap failed");
        return;
    }

    // Shrink the cover to 96x96: the accent colour comes from it, the background is its blurred copy.
    ComPtr<IWICBitmapScaler> scaler;
    std::vector<uint8_t> px(static_cast<size_t>(kBgSize) * kBgSize * 4);
    if (SUCCEEDED(wic_->CreateBitmapScaler(scaler.GetAddressOf())) &&
        SUCCEEDED(scaler->Initialize(conv.Get(), kBgSize, kBgSize, WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(scaler->CopyPixels(nullptr, kBgSize * 4, static_cast<UINT>(px.size()), px.data()))) {
        coverColor_ = VibrantColor(px);
        Blur(px, kBgSize, kBgSize, 4);
        Saturate(px, 1.35f);
        rt_->CreateBitmap(D2D1::SizeU(kBgSize, kBgSize), px.data(), kBgSize * 4,
                          D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
                          bg_.GetAddressOf());
    }
}

void Renderer::DrawCover(float cx, float cy, float cs) {
    Fill(D2D1::RoundedRect(D2D1::RectF(cx + 4, cy + 10, cx + cs + 4, cy + cs + 10), 24, 24), Rgb(0, 0, 0, 0.35f));
    if (cover_) {
        ComPtr<ID2D1BitmapBrush> bb;
        if (SUCCEEDED(rt_->CreateBitmapBrush(cover_.Get(),
                                             D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                                                                         D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),
                                             bb.GetAddressOf()))) {
            D2D1_SIZE_F sz = cover_->GetSize();
            bb->SetTransform(D2D1::Matrix3x2F::Scale(cs / sz.width, cs / sz.height) * D2D1::Matrix3x2F::Translation(cx, cy));
            bb->SetOpacity(CoverFade());
            Fill(D2D1::RoundedRect(D2D1::RectF(cx, cy, cx + cs, cy + cs), 24, 24), Rgb(1, 1, 1, 0.08f));
            rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx, cy, cx + cs, cy + cs), 24, 24), bb.Get());
        }
    } else {
        Fill(D2D1::RoundedRect(D2D1::RectF(cx, cy, cx + cs, cy + cs), 24, 24), Rgb(1, 1, 1, 0.08f));
        Text(L"♪", cx, cy, cs, cs, 110, DWRITE_FONT_WEIGHT_NORMAL, Rgb(1, 1, 1, 0.35f), false,
             DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
}

void Renderer::DrawClock() {
    Text(Clock(), kScreenWidth - 140, 22, 100, 28, 20, DWRITE_FONT_WEIGHT_NORMAL, Rgb(1, 1, 1, 0.55f), false,
         DWRITE_TEXT_ALIGNMENT_TRAILING);
}

void Renderer::DrawMedia(const MediaSnapshot& m, double t) {
    SetCover(m.cover, m.coverSerial);
    accent_.r = Lerp(accent_.r, coverColor_.r, 0.12f);
    accent_.g = Lerp(accent_.g, coverColor_.g, 0.12f);
    accent_.b = Lerp(accent_.b, coverColor_.b, 0.12f);
    Background(accent_);
    DrawClock();

    std::string app = Lower(m.app);
    std::wstring source = app.find("spotify") != std::string::npos ? L"SPOTIFY" : ToWide(m.app);
    if (source.size() > 4 && source.compare(source.size() - 4, 4, L".exe") == 0) source.resize(source.size() - 4);
    for (auto& c : source) c = static_cast<wchar_t>(towupper(c));
    Text(source, 40, 24, 300, 24, 15, DWRITE_FONT_WEIGHT_SEMI_BOLD, Rgb(accent_.r, accent_.g, accent_.b, 0.95f));

    DrawCover(40, 80, 280);

    const float x0 = 360, w = 400;
    Text(ToWide(m.title), x0, 84, w, 112, 38, DWRITE_FONT_WEIGHT_SEMI_BOLD, Rgb(1, 1, 1), true);
    Text(ToWide(m.artist), x0, 202, w, 36, 24, DWRITE_FONT_WEIGHT_NORMAL, Rgb(1, 1, 1, 0.82f));
    Text(ToWide(m.album), x0, 240, w, 28, 18, DWRITE_FONT_WEIGHT_NORMAL, Rgb(1, 1, 1, 0.5f));

    if (m.duration > 1.0) {
        double pos = m.PositionNow();
        float frac = static_cast<float>(std::clamp(pos / m.duration, 0.0, 1.0));
        const float py = 330, ph = 8;
        Fill(D2D1::RoundedRect(D2D1::RectF(x0, py, x0 + w, py + ph), 4, 4), Rgb(1, 1, 1, 0.16f));
        if (frac > 0.002f) {
            Fill(D2D1::RoundedRect(D2D1::RectF(x0, py, x0 + std::max(w * frac, ph), py + ph), 4, 4),
                 Rgb(std::min(accent_.r * 1.1f, 1.0f), std::min(accent_.g * 1.1f, 1.0f), std::min(accent_.b * 1.1f, 1.0f)));
        }
        brush_->SetColor(Rgb(1, 1, 1));
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x0 + w * frac, py + ph / 2), 8, 8), brush_.Get());
        Text(Duration(pos), x0, py + 18, 100, 24, 16, DWRITE_FONT_WEIGHT_NORMAL, Rgb(1, 1, 1, 0.6f));
        Text(Duration(m.duration), x0 + w - 100, py + 18, 100, 24, 16, DWRITE_FONT_WEIGHT_NORMAL, Rgb(1, 1, 1, 0.6f), false,
             DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    // Play / pause badge.
    const float bx = x0 + 24, by = 420, br = 24;
    brush_->SetColor(Rgb(1, 1, 1));
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(bx, by), br, br), brush_.Get());
    brush_->SetColor(Rgb(0.05f, 0.05f, 0.06f));
    if (m.playing) {
        rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx - 9, by - 11, bx - 3, by + 11), 2, 2), brush_.Get());
        rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx + 3, by - 11, bx + 9, by + 11), 2, 2), brush_.Get());
    } else {
        ComPtr<ID2D1PathGeometry> tri;
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(d2d_->CreatePathGeometry(tri.GetAddressOf())) && SUCCEEDED(tri->Open(sink.GetAddressOf()))) {
            sink->BeginFigure(D2D1::Point2F(bx - 7, by - 12), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(bx + 12, by));
            sink->AddLine(D2D1::Point2F(bx - 7, by + 12));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            rt_->FillGeometry(tri.Get(), brush_.Get());
        }
    }
    Text(m.playing ? L"Playing" : L"Paused", bx + 40, by - 14, 200, 28, 18, DWRITE_FONT_WEIGHT_NORMAL, Rgb(1, 1, 1, 0.55f));
    (void)t;
}

void Renderer::DrawRadio(const RadioSnapshot& r, double t) {
    const std::string stationUtf8 = r.stationName.empty() ? r.icyName : r.stationName;
    const std::string raw = Trim(r.title);
    const bool titleIsStation = Lower(raw) == Lower(Trim(stationUtf8)) || Lower(raw) == Lower(Trim(r.icyName));
    const bool hasSong = !raw.empty() && !titleIsStation;

    std::wstring artist, song;
    SplitArtistTitle(raw, artist, song);  // without a separator `song` is the whole title

    // Song cover from the iTunes lookup (when the title looks like "Artist - Song").
    CoverBytes coverBytes;
    uint64_t coverKey = 0;
    if (hasSong && !artist.empty() && GetSettings().coverLookup) {
        if (raw != lastCoverRequest_) {
            lastCoverRequest_ = raw;
            CoverLookupRequest(raw);
        }
        CoverResult c = CoverLookupGet(raw);
        if (c.state == CoverState::Found) coverBytes = c.image;
        coverKey = (std::hash<std::string>()(raw) * 31 + (c.state == CoverState::Found ? 1 : 0)) | (1ull << 63);
    } else {
        coverKey = (std::hash<std::string>()(raw) * 31 + 7) | (1ull << 63);
    }
    SetCover(coverBytes, coverKey);

    D2D1_COLOR_F target = cover_ ? coverColor_ : Rgb(1.0f, 0.62f, 0.18f);
    accent_.r = Lerp(accent_.r, target.r, 0.12f);
    accent_.g = Lerp(accent_.g, target.g, 0.12f);
    accent_.b = Lerp(accent_.b, target.b, 0.12f);
    Background(accent_);
    DrawClock();

    // LIVE pill with a pulsing dot.
    Fill(D2D1::RoundedRect(D2D1::RectF(40, 22, 128, 50), 14, 14), Rgb(0.86f, 0.12f, 0.14f, 0.95f));
    float pulse = 0.55f + 0.45f * static_cast<float>(std::sin(t * 4.0));
    brush_->SetColor(Rgb(1, 1, 1, pulse));
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(58, 36), 5, 5), brush_.Get());
    Text(L"LIVE", 70, 22, 56, 28, 15, DWRITE_FONT_WEIGHT_SEMI_BOLD, Rgb(1, 1, 1), false, DWRITE_TEXT_ALIGNMENT_LEADING,
         DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    Text(L"RADIO", 140, 22, 100, 28, 15, DWRITE_FONT_WEIGHT_SEMI_BOLD, Rgb(1, 1, 1, 0.6f), false,
         DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // Left: the song's cover, or the station's initials while there is none.
    const float lx = 40, ly = 80, ls = 280;
    if (cover_) {
        DrawCover(lx, ly, ls);
    } else {
        std::wstring name = ToWide(stationUtf8);
        std::wstring initials;
        bool startWord = true;
        for (wchar_t c : name) {
            if (iswalnum(c)) {
                if (startWord && initials.size() < 2) initials.push_back(static_cast<wchar_t>(towupper(c)));
                startWord = false;
            } else {
                startWord = true;
            }
        }
        Fill(D2D1::RoundedRect(D2D1::RectF(lx + 4, ly + 10, lx + ls + 4, ly + ls + 10), 24, 24), Rgb(0, 0, 0, 0.35f));
        Fill(D2D1::RoundedRect(D2D1::RectF(lx, ly, lx + ls, ly + ls), 24, 24),
             Rgb(accent_.r * 0.55f, accent_.g * 0.55f, accent_.b * 0.55f));
        Text(initials.empty() ? L"♫" : initials, lx, ly, ls, ls, 110, DWRITE_FONT_WEIGHT_SEMI_BOLD, Rgb(1, 1, 1, 0.95f), false,
             DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // Right: song and artist big. While the stream has not told us the song yet (many stations only send it
    // after a few seconds), the station name stands in, with a moving "looking for the track" bar below it.
    // New text fades in when it changes.
    const float x0 = 360, w = 400;
    const std::string key = hasSong ? raw : "station:" + stationUtf8;
    if (key != titleKey_) {
        titleKey_ = key;
        titleChangedAt_ = t;
    }
    const float fade = Ease(static_cast<float>((t - titleChangedAt_) / 0.45));
    textAlpha_ = fade;
    const float rise = (1.0f - fade) * 10.0f;  // text slides up a little while it fades in
    if (hasSong) {
        Text(song, x0, 84 + rise, w, 112, 38, DWRITE_FONT_WEIGHT_SEMI_BOLD, Rgb(1, 1, 1), true);
        Text(artist, x0, 202 + rise, w, 36, 24, DWRITE_FONT_WEIGHT_NORMAL, Rgb(1, 1, 1, 0.82f));
    } else {
        Text(ToWide(stationUtf8), x0, 84 + rise, w, 112, 38, DWRITE_FONT_WEIGHT_SEMI_BOLD, Rgb(1, 1, 1), true);
        if (r.ageMs < 15000) {
            const int dots = static_cast<int>(t * 2.5) % 4;
            Text(std::wstring(L"Looking for the track") + std::wstring(static_cast<size_t>(dots), L'.'), x0, 202, w, 30, 20,
                 DWRITE_FONT_WEIGHT_NORMAL, Rgb(1, 1, 1, 0.55f));
            // Indeterminate progress bar: a soft highlight sweeping over a faint track.
            const float by = 242, bh = 4;
            Fill(D2D1::RoundedRect(D2D1::RectF(x0, by, x0 + w, by + bh), 2, 2), Rgb(1, 1, 1, 0.12f * textAlpha_));
            rt_->PushAxisAlignedClip(D2D1::RectF(x0, by, x0 + w, by + bh), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            float sweep = static_cast<float>(std::fmod(t * 0.7, 1.4)) - 0.2f;
            Fill(D2D1::RoundedRect(D2D1::RectF(x0 + sweep * w, by, x0 + sweep * w + 120, by + bh), 2, 2),
                 Rgb(accent_.r, accent_.g, accent_.b, 0.85f * textAlpha_));
            rt_->PopAxisAlignedClip();
        } else {
            Text(L"This station does not send track titles", x0, 202, w, 30, 18, DWRITE_FONT_WEIGHT_NORMAL,
                 Rgb(1, 1, 1, 0.45f));
        }
    }

    // Equalizer.
    for (int i = 0; i < 9; ++i) {
        float h = 8.0f + 26.0f * static_cast<float>(0.5 + 0.5 * std::sin(t * (3.0 + i * 0.7) + i * 1.3));
        float x = x0 + i * 22.0f;
        Fill(D2D1::RoundedRect(D2D1::RectF(x, 372 - h, x + 12, 372), 3, 3), Rgb(accent_.r, accent_.g, accent_.b, 0.9f));
    }

    // The station is the least important part: one small line at the bottom.
    std::wstring meta;
    if (hasSong) meta = ToWide(stationUtf8);
    auto add = [&](const std::wstring& part) {
        if (part.empty()) return;
        if (!meta.empty()) meta += L"  ·  ";
        meta += part;
    };
    add(ToWide(r.genre));
    add(ToWide(r.country));
    if (!r.bitrate.empty()) add(ToWide(r.bitrate) + L" kbps");
    Text(meta, x0, 408, w, 24, 16, DWRITE_FONT_WEIGHT_NORMAL, Rgb(1, 1, 1, 0.42f));
    textAlpha_ = 1.0f;
}

void Renderer::DrawIdle(double t) {
    SetCover(nullptr, 1ull << 62);  // drop any cover so the plain background is used
    D2D1_COLOR_F target = Rgb(0.35f, 0.42f, 0.55f);
    accent_.r = Lerp(accent_.r, target.r, 0.12f);
    accent_.g = Lerp(accent_.g, target.g, 0.12f);
    accent_.b = Lerp(accent_.b, target.b, 0.12f);
    Background(accent_);
    Text(Clock(), 0, 120, kScreenWidth, 150, 120, DWRITE_FONT_WEIGHT_LIGHT, Rgb(1, 1, 1, 0.92f), false,
         DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    Text(L"Nothing playing", 0, 290, kScreenWidth, 36, 22, DWRITE_FONT_WEIGHT_NORMAL, Rgb(1, 1, 1, 0.45f), false,
         DWRITE_TEXT_ALIGNMENT_CENTER);
    (void)t;
}

void Renderer::CopyOut() {
    ComPtr<IWICBitmapLock> lock;
    WICRect rc{0, 0, static_cast<INT>(kScreenWidth), static_cast<INT>(kScreenHeight)};
    if (FAILED(bitmap_->Lock(&rc, WICBitmapLockRead, lock.GetAddressOf()))) return;
    UINT stride = 0, size = 0;
    BYTE* src = nullptr;
    lock->GetStride(&stride);
    lock->GetDataPointer(&size, &src);
    if (!src) return;
    const size_t row = static_cast<size_t>(kScreenWidth) * 4;
    const bool flip = GetSettings().flipVertical;
    for (uint32_t y = 0; y < kScreenHeight; ++y) {
        uint32_t from = flip ? kScreenHeight - 1 - y : y;
        memcpy(&pixels_[y * row], src + static_cast<size_t>(from) * stride, row);
    }
}

void Renderer::Frame(double t) {
    now_ = t;
    RadioSnapshot radio = RadioGetSnapshot();
    MediaSnapshot media = MediaGetSnapshot();

    // While switching stations the stream is closed for a moment: keep showing the last radio screen.
    auto now = std::chrono::steady_clock::now();
    if (radio.active) {
        lastRadio_ = radio;
        lastRadioTime_ = now;
    }
    bool holdRadio = !radio.active && lastRadioTime_.time_since_epoch().count() != 0 &&
                     (radio.connecting || now - lastRadioTime_ < std::chrono::milliseconds(GetSettings().radioHoldMs));

    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    if (radio.active) DrawRadio(radio, t);
    else if (holdRadio) DrawRadio(lastRadio_, t);
    else if (media.hasSession && !media.title.empty()) DrawMedia(media, t);
    else DrawIdle(t);
    HRESULT hr = rt_->EndDraw();
    if (FAILED(hr)) {
        Logf("RENDER EndDraw failed: 0x%08lx", static_cast<unsigned long>(hr));
        return;
    }
    CopyOut();
}

// --- frame hand-over ----------------------------------------------------------------------

std::mutex g_frameMutex;
std::vector<uint8_t> g_front;
uint64_t g_version = 0;
std::thread g_thread;
std::atomic<bool> g_stop{false};

void Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        Renderer renderer;
        if (!renderer.Init()) {
            Logf("RENDER init failed");
        } else {
            Logf("RENDER ready (%ux%u)", kScreenWidth, kScreenHeight);
            const auto start = std::chrono::steady_clock::now();
            while (!g_stop) {
                auto frameStart = std::chrono::steady_clock::now();
                double t = std::chrono::duration<double>(frameStart - start).count();
                renderer.Frame(t);
                {
                    std::lock_guard<std::mutex> lock(g_frameMutex);
                    g_front = renderer.Pixels();
                    ++g_version;
                }
                auto spent = std::chrono::steady_clock::now() - frameStart;
                auto budget = std::chrono::milliseconds(66);
                if (spent < budget) std::this_thread::sleep_for(budget - spent);
            }
        }
    }
    CoUninitialize();
}

}  // namespace

void RendererStart() {
    g_stop = false;
    g_thread = std::thread(Run);
}

void RendererStop() {
    g_stop = true;
    if (g_thread.joinable()) g_thread.join();
}

bool RendererLockFrame(uint64_t afterVersion, FrameView* view) {
    g_frameMutex.lock();
    if (g_version <= afterVersion || g_front.empty()) {
        g_frameMutex.unlock();
        return false;
    }
    view->data = g_front.data();
    view->width = kScreenWidth;
    view->height = kScreenHeight;
    view->pitch = kScreenWidth * 4;
    view->version = g_version;
    return true;
}

void RendererUnlockFrame() { g_frameMutex.unlock(); }

}  // namespace ep
