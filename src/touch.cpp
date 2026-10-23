#include "touch.h"

#include "log.h"
#include "media.h"
#include "renderer.h"
#include "screen_fit.h"
#include "settings.h"
#include "touch_layout.h"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace ep {
namespace {

constexpr int kProbeW = 512, kProbeH = 256;  // probe texture: texel (i,j) holds ((i+.5)/W, (j+.5)/H, 1, 1)
constexpr int kRing = 4;
constexpr int kScanW = 384, kScanH = 216;  // picture of the whole view: texture coordinates of the screen
constexpr int kScanRing = 3;

PsSetSrvFn g_psSet = nullptr;
bool g_enabled = false;
bool g_failed = false;

ID3D11DeviceContext* g_imm = nullptr;  // immediate context of the game's device (no reference kept)
HWND g_hwnd = nullptr;
ID3D11ShaderResourceView* g_ours = nullptr;

// Resources (created lazily on the game's device).
ID3D11ShaderResourceView* g_probeSrv = nullptr;
ID3D11Texture2D* g_rt = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
ID3D11Texture2D* g_staging[kRing] = {};
bool g_stagingPending[kRing] = {};
ID3D11BlendState* g_noBlend = nullptr;
ID3D11DepthStencilState* g_noDepth = nullptr;
ID3D11Texture2D* g_scanRt = nullptr;
ID3D11RenderTargetView* g_scanRtv = nullptr;
ID3D11Texture2D* g_scanStaging[kScanRing] = {};
bool g_scanPending[kScanRing] = {};
int g_scanIndex = 0;
bool g_scanDrawnThisFrame = false;
bool g_scanFrameActive = false;  // a scan picture is being built this frame (it may take several draw calls)
int g_scanDraws = 0;
ULONGLONG g_lastScanTick = 0;
int g_scanLogged = 0;
double g_scanAspect = 16.0 / 9.0;  // width / height of the picture the player sees (the scan squeezes it into 384x216)

std::bitset<128> g_mask;  // slots of the immediate context that currently hold our screen view
std::atomic<bool> g_maskAny{false};
thread_local bool t_replaying = false;

struct Cursor {
    float x = 0, y = 0, w = 1, h = 1;
    bool inside = false;
} g_cursor;

bool g_clearedThisFrame = false;
bool g_replayedThisFrame = false;
int g_ringIndex = 0;
int g_logged = 0;
int g_replays = 0;
uint64_t g_hits = 0, g_misses = 0;
float g_lastRaw[4] = {};
float g_lastPixelX = 0, g_lastPixelY = 0, g_lastRtW = 0, g_lastRtH = 0;
ULONGLONG g_lastStats = 0;

std::mutex g_stateMx;
TouchState g_state;
uint64_t g_hoverTick = 0;
std::atomic<bool> g_buttonsVisible{false};
bool g_lmbDown = false;
bool g_dragging = false;       // the progress bar is being dragged
ULONGLONG g_lastSeekSend = 0;

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

bool CreateResources(ID3D11Device* dev) {
    // Probe texture.
    std::vector<float> texels(static_cast<size_t>(kProbeW) * kProbeH * 4);
    for (int y = 0; y < kProbeH; ++y) {
        for (int x = 0; x < kProbeW; ++x) {
            float* t = &texels[(static_cast<size_t>(y) * kProbeW + x) * 4];
            t[0] = (x + 0.5f) / kProbeW;
            t[1] = (y + 0.5f) / kProbeH;
            t[2] = 1.0f;
            t[3] = 1.0f;
        }
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kProbeW;
    td.Height = kProbeH;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{texels.data(), kProbeW * 16u, 0};
    ID3D11Texture2D* probe = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, &sd, &probe))) return false;
    HRESULT hr = dev->CreateShaderResourceView(probe, nullptr, &g_probeSrv);
    probe->Release();
    if (FAILED(hr)) return false;

    // 1x1 render target and its read-back ring.
    D3D11_TEXTURE2D_DESC rd{};
    rd.Width = rd.Height = 1;
    rd.MipLevels = rd.ArraySize = 1;
    rd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Usage = D3D11_USAGE_DEFAULT;
    rd.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(dev->CreateTexture2D(&rd, nullptr, &g_rt)) || FAILED(dev->CreateRenderTargetView(g_rt, nullptr, &g_rtv))) return false;
    D3D11_TEXTURE2D_DESC st = rd;
    st.Usage = D3D11_USAGE_STAGING;
    st.BindFlags = 0;
    st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto& s : g_staging) {
        if (FAILED(dev->CreateTexture2D(&st, nullptr, &s))) return false;
    }

    // Coarse scan target (whole view, texture coordinates of the screen) and its read-back ring.
    D3D11_TEXTURE2D_DESC sc = rd;
    sc.Width = kScanW;
    sc.Height = kScanH;
    if (FAILED(dev->CreateTexture2D(&sc, nullptr, &g_scanRt)) || FAILED(dev->CreateRenderTargetView(g_scanRt, nullptr, &g_scanRtv))) {
        return false;
    }
    sc.Usage = D3D11_USAGE_STAGING;
    sc.BindFlags = 0;
    sc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto& s : g_scanStaging) {
        if (FAILED(dev->CreateTexture2D(&sc, nullptr, &s))) return false;
    }

    // Fixed-function state for the probe pass: no blending, no depth test.
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_DEPTH_STENCIL_DESC dd{};
    if (FAILED(dev->CreateBlendState(&bd, &g_noBlend)) || FAILED(dev->CreateDepthStencilState(&dd, &g_noDepth))) return false;
    return true;
}

bool EnsureResources(ID3D11DeviceContext* ctx) {
    if (g_probeSrv) return true;
    if (g_failed) return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    bool ok = dev && CreateResources(dev);
    if (dev) dev->Release();
    if (!ok) {
        g_failed = true;
        Logf("TOUCH could not create the probe resources, touch disabled");
        return false;
    }
    Logf("TOUCH probe resources ready");
    return true;
}

bool ForegroundIsUs() {
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

void DecodeResult(const float* px) {
    const float r = px[0], g = px[1], b = px[2];
    std::lock_guard<std::mutex> lock(g_stateMx);
    memcpy(g_lastRaw, px, sizeof(g_lastRaw));
    // The target is cleared to 0, so anything above 0 was drawn by the screen. How bright that is depends on the
    // time of day and the game's exposure (at night the values are tiny), so no brightness threshold here:
    // only the ratios r/b and g/b matter.
    const bool drawn = std::isfinite(r) && std::isfinite(g) && std::isfinite(b) && b > 1e-9f;
    if (drawn) ++g_hits;
    else ++g_misses;
    if (g_logged < 25 && (drawn || g_logged < 6)) {
        ++g_logged;
        Logf("TOUCH probe pixel %s r=%g g=%g b=%g a=%g", drawn ? "HIT" : "miss", r, g, b, px[3]);
    }
    if (!drawn) {  // nothing was drawn at the cursor
        g_state.hover = false;
        return;
    }
    float u = r / b, v = g / b;
    if (u < -0.02f || u > 1.02f || v < -0.02f || v > 1.02f) {
        g_state.hover = false;
        return;
    }
    u = std::clamp(u, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);
    const ScreenFit fit = FitGet();
    if (fit.valid) {  // texture coordinate -> texel -> layout canvas, through the inverse of the fit transform
        const double tx = u * kScreenWidth, ty = v * kScreenHeight;
        g_state.x = static_cast<float>(fit.texelToCanvas[0] * tx + fit.texelToCanvas[2] * ty + fit.texelToCanvas[4]);
        // The buttons live in the 800x480 design area, which sits at designOffsetY inside the (taller) canvas.
        g_state.y = static_cast<float>(fit.texelToCanvas[1] * tx + fit.texelToCanvas[3] * ty + fit.texelToCanvas[5] - fit.designOffsetY);
    } else {
        g_state.x = u * kCanvasWidth;
        g_state.y = (GetSettings().flipVertical ? 1.0f - v : v) * kCanvasHeight;
    }
    g_state.hover = true;
    g_hoverTick = GetTickCount64();
}

// Works out which way the screen's texture runs on the screen from the coarse scan: a least-squares plane through
// u(x,y) and v(x,y) over all covered cells. The sign and dominant axis of the slopes give rotation and mirroring.
void DecodeScan(const float* px, UINT rowPitchFloats) {
    double n = 0, sx = 0, sy = 0, su = 0, sv = 0;
    struct Cell { double x, y, u, v; };
    std::vector<Cell> cells;
    for (int y = 0; y < kScanH; ++y) {
        for (int x = 0; x < kScanW; ++x) {
            const float* p = px + static_cast<size_t>(y) * rowPitchFloats + static_cast<size_t>(x) * 4;
            if (!(std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2])) || p[2] <= 1e-9f) continue;
            double u = p[0] / p[2], v = p[1] / p[2];
            if (u < -0.05 || u > 1.05 || v < -0.05 || v > 1.05) continue;
            cells.push_back({static_cast<double>(x), static_cast<double>(y), u, v});
            n += 1;
            sx += x;
            sy += y;
            su += u;
            sv += v;
        }
    }
    ScreenOrientation o;
    if (cells.size() < 60) {
        if (g_scanLogged < 6) {
            ++g_scanLogged;
            Logf("FIT scan: only %zu covered cells, screen too small or not in view", cells.size());
        }
        return;
    }
    // Which way does the texture run on the screen? Compare where, on the screen, the cells with high and with low
    // texture coordinates are. (A plane fit over all cells is thrown off by frame and bezel cells.)
    std::vector<double> us, vs;
    for (const Cell& c : cells) {
        us.push_back(c.u);
        vs.push_back(c.v);
    }
    std::sort(us.begin(), us.end());
    std::sort(vs.begin(), vs.end());
    // The visible part of the texture is what the cells show, apart from a few stray cells at the edges.
    const size_t trim = std::max<size_t>(1, us.size() * 3 / 1000);
    ScanWindow w;
    w.uMin = us[trim];
    w.uMax = us[us.size() - 1 - trim];
    w.vMin = vs[trim];
    w.vMax = vs[vs.size() - 1 - trim];
    w.ok = cells.size() >= 150 && w.uMax - w.uMin > 0.02 && w.vMax - w.vMin > 0.02;
    char shapeNote[160] = "";
    if (w.ok) {
        // Real proportions of that window, from how the camera sees it.
        std::vector<ShapeCell> sc;
        sc.reserve(cells.size());
        for (const Cell& c : cells) {
            sc.push_back({((c.x + 0.5) / kScanW * 2.0 - 1.0) * g_scanAspect, 1.0 - (c.y + 0.5) / kScanH * 2.0, c.u, c.v});
        }
        double aspect = 1, focal = 0, residual = 0;
        bool focalMeasured = false;
        if (FitEstimateShape(sc, w, aspect, focal, focalMeasured, residual)) {
            w.shapeOk = true;
            w.uvAspect = aspect;
            snprintf(shapeNote, sizeof(shapeNote), " | shape u:v %.3f (focal %.2f %s, fit error %.2f cells)", aspect, focal,
                     focalMeasured ? "measured" : "assumed", residual);
        } else {
            snprintf(shapeNote, sizeof(shapeNote), " | shape not measurable from this view");
        }
    }
    if (g_scanLogged < 12) {
        ++g_scanLogged;
        Logf("FIT scan: %zu cells show texture window u %.3f..%.3f, v %.3f..%.3f%s%s", cells.size(), w.uMin, w.uMax, w.vMin, w.vMax,
             w.ok ? "" : " (too few cells or too small, ignored)", shapeNote);
    }
    if (w.ok) FitNoteScanWindow(w);
}

void ReadScanBack(ID3D11DeviceContext* ctx) {
    for (int i = 0; i < kScanRing; ++i) {
        int slot = (g_scanIndex + i) % kScanRing;  // oldest first
        if (!g_scanPending[slot]) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        HRESULT hr = ctx->Map(g_scanStaging[slot], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) break;
        g_scanPending[slot] = false;
        if (FAILED(hr)) continue;
        DecodeScan(static_cast<const float*>(m.pData), m.RowPitch / sizeof(float));
        ctx->Unmap(g_scanStaging[slot], 0);
    }
}

void ReadBack(ID3D11DeviceContext* ctx) {
    for (int i = 0; i < kRing; ++i) {
        // Oldest first: the slot we will write next is the oldest one.
        int slot = (g_ringIndex + i) % kRing;
        if (!g_stagingPending[slot]) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        HRESULT hr = ctx->Map(g_staging[slot], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) break;
        g_stagingPending[slot] = false;
        if (FAILED(hr)) continue;
        float px[4];
        memcpy(px, m.pData, sizeof(px));
        ctx->Unmap(g_staging[slot], 0);
        DecodeResult(px);
    }
}

float CanvasWidthNow() {
    const ScreenFit fit = FitGet();
    return fit.valid ? static_cast<float>(fit.canvasW) : static_cast<float>(kCanvasWidth);
}

// While the progress bar is dragged: the position under the mouse is sent to the player a few times a second and once
// more when the button goes up.
void UpdateDrag(bool finish) {
    const MediaSnapshot m = MediaGetSnapshot();
    float x;
    {
        std::lock_guard<std::mutex> lock(g_stateMx);
        x = g_state.x;
    }
    if (m.duration <= 1.0 || !g_buttonsVisible) {
        g_dragging = false;
        std::lock_guard<std::mutex> lock(g_stateMx);
        g_state.dragging = false;
        return;
    }
    const float frac = std::clamp((x - kBarX) / BarWidth(CanvasWidthNow()), 0.0f, 1.0f);
    {
        std::lock_guard<std::mutex> lock(g_stateMx);
        g_state.dragging = !finish;
        g_state.dragFrac = frac;
    }
    const ULONGLONG now = GetTickCount64();
    if (finish || now - g_lastSeekSend >= 150) {
        g_lastSeekSend = now;
        MediaSeek(frac * m.duration);
    }
    if (finish) {
        g_dragging = false;
        Logf("TOUCH progress bar released at %.0f%%", frac * 100.0f);
    }
}

void HandleClick() {
    bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 && !(GetAsyncKeyState(VK_RBUTTON) & 0x8000);
    bool edge = down && !g_lmbDown;
    g_lmbDown = down;
    if (g_dragging) {
        UpdateDrag(!down || !ForegroundIsUs());
        return;
    }
    if (!edge || !ForegroundIsUs()) return;

    TouchState s;
    {
        std::lock_guard<std::mutex> lock(g_stateMx);
        if (!g_state.hover || GetTickCount64() - g_hoverTick > 300) return;
        g_state.clickTick = GetTickCount64();
        g_state.clickX = g_state.x;
        g_state.clickY = g_state.y;
        s = g_state;
    }
    if (g_buttonsVisible && HitProgressBar(s.x, s.y, CanvasWidthNow()) && MediaGetSnapshot().duration > 1.0) {
        g_dragging = true;
        g_lastSeekSend = 0;
        Logf("TOUCH progress bar pressed at %.0f,%.0f", s.x, s.y);
        UpdateDrag(false);
        return;
    }
    Button b = g_buttonsVisible ? HitTestButtons(s.x, s.y) : Button::None;
    Logf("TOUCH click at %.0f,%.0f -> %s", s.x, s.y,
         b == Button::Previous ? "previous" : b == Button::PlayPause ? "play/pause" : b == Button::Next ? "next" : "nothing");
    if (b == Button::Previous) MediaSendCommand(MediaCommand::Previous);
    else if (b == Button::PlayPause) MediaSendCommand(MediaCommand::PlayPause);
    else if (b == Button::Next) MediaSendCommand(MediaCommand::Next);
}

}  // namespace

void TouchInit(bool enabled, PsSetSrvFn originalPsSetSrv) {
    g_enabled = enabled;
    g_psSet = originalPsSetSrv;
    Logf("TOUCH %s", enabled ? "enabled" : "disabled");
}

void TouchSetScreenView(ID3D11ShaderResourceView* ours) { g_ours = ours; }

bool TouchHasBindings() { return g_maskAny.load(std::memory_order_relaxed); }

void TouchNoteBind(ID3D11DeviceContext* ctx, UINT start, UINT count, const bool* isScreen) {
    if ((!g_enabled && !FitEnabled()) || ctx != g_imm) return;
    for (UINT i = 0; i < count; ++i) {
        UINT slot = start + i;
        if (slot < g_mask.size()) g_mask.set(slot, isScreen[i]);
    }
    g_maskAny.store(g_mask.any(), std::memory_order_relaxed);
}

namespace {
bool WantsPixelProbe() { return g_enabled && g_cursor.inside; }
bool WantsScan() {
    // One scan picture every quarter second. The screen may be several draw calls: all of them go into the picture.
    if (!FitEnabled() || !FitNeedsScan()) return false;
    if (g_scanFrameActive) return g_scanDraws < 6;
    return GetTickCount64() - g_lastScanTick > 250;
}
}  // namespace

bool TouchNeedsReplay(ID3D11DeviceContext* ctx) {
    return (g_enabled || FitEnabled()) && !g_failed && !t_replaying && g_maskAny.load(std::memory_order_relaxed) &&
           ctx == g_imm && g_ours && (WantsPixelProbe() || WantsScan());
}

bool TouchScreenBound(ID3D11DeviceContext* ctx) {
    if (ctx != g_imm || !g_ours || !g_maskAny.load(std::memory_order_relaxed)) return false;
    for (UINT s = 0; s < g_mask.size(); ++s) {
        if (!g_mask.test(s)) continue;
        ID3D11ShaderResourceView* cur = nullptr;
        ctx->PSGetShaderResources(s, 1, &cur);
        bool ok = cur == g_ours;
        SafeRelease(cur);
        if (ok) return true;
    }
    return false;
}

void TouchResetScan() {
    for (auto& p : g_scanPending) p = false;
    g_lastScanTick = 0;
    g_scanLogged = 0;
}

void TouchReplay(ID3D11DeviceContext* ctx, const std::function<void()>& drawOnce) {
    if (!EnsureResources(ctx)) return;
    const bool doPixel = WantsPixelProbe();
    const bool doScan = WantsScan();
    if (!doPixel && !doScan) return;

    // Slots that really hold our view right now (the bookkeeping can be stale after ClearState etc.).
    UINT slots[8];
    int nSlots = 0;
    for (UINT s = 0; s < g_mask.size() && nSlots < 8; ++s) {
        if (!g_mask.test(s)) continue;
        ID3D11ShaderResourceView* cur = nullptr;
        ctx->PSGetShaderResources(s, 1, &cur);
        bool ok = cur == g_ours;
        SafeRelease(cur);
        if (ok) slots[nSlots++] = s;
    }
    if (!nSlots) return;

    // Save the pipeline state the probe pass touches.
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    ctx->RSGetViewports(&nvp, vps);
    UINT nsc = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT scs[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    ctx->RSGetScissorRects(&nsc, scs);
    ID3D11BlendState* bs = nullptr;
    FLOAT bf[4] = {};
    UINT sampleMask = 0;
    ctx->OMGetBlendState(&bs, bf, &sampleMask);
    ID3D11DepthStencilState* dss = nullptr;
    UINT stencilRef = 0;
    ctx->OMGetDepthStencilState(&dss, &stencilRef);

    // Size of the target the game is drawing into: the mouse position is scaled onto it.
    float rtW = 0, rtH = 0;
    if (rtvs[0]) {
        ID3D11Resource* res = nullptr;
        rtvs[0]->GetResource(&res);
        if (res) {
            ID3D11Texture2D* t2 = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t2)))) {
                D3D11_TEXTURE2D_DESC d{};
                t2->GetDesc(&d);
                rtW = static_cast<float>(d.Width);
                rtH = static_cast<float>(d.Height);
                t2->Release();
            }
            res->Release();
        }
    }

    // Only the main view: reflection, shadow and cube-map passes draw the screen into small targets too.
    const bool mainPass = g_cursor.w <= 1 || (rtW >= 0.4f * g_cursor.w && rtH >= 0.4f * g_cursor.h);
    if (nvp > 0 && rtW > 0 && rtH > 0 && mainPass) {
        t_replaying = true;
        const FLOAT zero[4] = {0, 0, 0, 0};
        ctx->OMSetBlendState(g_noBlend, nullptr, 0xFFFFFFFF);
        ctx->OMSetDepthStencilState(g_noDepth, 0);
        for (int i = 0; i < nSlots; ++i) g_psSet(ctx, slots[i], 1, &g_probeSrv);

        if (doPixel) {
            float px = std::floor(g_cursor.x * rtW / g_cursor.w);
            float py = std::floor(g_cursor.y * rtH / g_cursor.h);
            g_lastPixelX = px;
            g_lastPixelY = py;
            g_lastRtW = rtW;
            g_lastRtH = rtH;
            if (!g_clearedThisFrame) {
                ctx->ClearRenderTargetView(g_rtv, zero);
                g_clearedThisFrame = true;
            }
            ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
            // Shift the viewport so that the mouse pixel lands on pixel (0,0) of the 1x1 target.
            D3D11_VIEWPORT vp = vps[0];
            vp.TopLeftX -= px;
            vp.TopLeftY -= py;
            ctx->RSSetViewports(1, &vp);
            D3D11_RECT sc{0, 0, 1, 1};
            ctx->RSSetScissorRects(1, &sc);
            drawOnce();
            g_replayedThisFrame = true;
            ++g_replays;
            if (g_replays == 1) Logf("TOUCH first probe draw (cursor pixel %.0f,%.0f in a %.0fx%.0f target)", px, py, rtW, rtH);
        }
        if (doScan) {
            // The whole view squeezed into a smaller target: every pixel of the screen shows its texture coordinates.
            if (!g_scanFrameActive) {
                ctx->ClearRenderTargetView(g_scanRtv, zero);
                g_scanFrameActive = true;
                g_scanDraws = 0;
                g_lastScanTick = GetTickCount64();
            }
            ctx->OMSetRenderTargets(1, &g_scanRtv, nullptr);
            g_scanAspect = g_cursor.w > 1 && g_cursor.h > 1 ? static_cast<double>(g_cursor.w) / g_cursor.h
                                                             : static_cast<double>(vps[0].Width) / std::max(1.0f, vps[0].Height);
            D3D11_VIEWPORT vp = vps[0];
            vp.TopLeftX = 0;
            vp.TopLeftY = 0;
            vp.Width = static_cast<float>(kScanW);
            vp.Height = static_cast<float>(kScanH);
            ctx->RSSetViewports(1, &vp);
            D3D11_RECT sc{0, 0, kScanW, kScanH};
            ctx->RSSetScissorRects(1, &sc);
            drawOnce();
            g_scanDrawnThisFrame = true;
            ++g_scanDraws;
        }

        for (int i = 0; i < nSlots; ++i) g_psSet(ctx, slots[i], 1, &g_ours);
        t_replaying = false;
    }

    // Restore.
    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
    ctx->OMSetBlendState(bs, bf, sampleMask);
    ctx->OMSetDepthStencilState(dss, stencilRef);
    if (nvp) ctx->RSSetViewports(nvp, vps);
    if (nsc) ctx->RSSetScissorRects(nsc, scs);
    for (auto*& r : rtvs) SafeRelease(r);
    SafeRelease(dsv);
    SafeRelease(bs);
    SafeRelease(dss);
}

void TouchOnPresent(IDXGISwapChain* sc) {
    if ((!g_enabled && !FitEnabled()) || g_failed) return;

    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) || !dev) return;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    dev->Release();
    if (!ctx) return;
    g_imm = ctx;  // the device keeps the immediate context alive
    ctx->Release();

    DXGI_SWAP_CHAIN_DESC desc{};
    if (SUCCEEDED(sc->GetDesc(&desc)) && desc.OutputWindow) g_hwnd = desc.OutputWindow;

    // Mouse position inside the game window.
    g_cursor.inside = false;
    if (g_hwnd) {
        POINT pt{};
        RECT rc{};
        if (GetCursorPos(&pt) && ScreenToClient(g_hwnd, &pt) && GetClientRect(g_hwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
            g_cursor.x = static_cast<float>(pt.x);
            g_cursor.y = static_cast<float>(pt.y);
            g_cursor.w = static_cast<float>(rc.right);
            g_cursor.h = static_cast<float>(rc.bottom);
            g_cursor.inside = pt.x >= 0 && pt.y >= 0 && pt.x < rc.right && pt.y < rc.bottom && ForegroundIsUs() &&
                              !(GetAsyncKeyState(VK_RBUTTON) & 0x8000);  // right button = looking around, cursor is hidden
        }
    }

    if (g_probeSrv) {
        // Results of earlier frames.
        ReadBack(g_imm);
        // This frame's probe pixel goes into the ring.
        if (g_replayedThisFrame) {
            int slot = g_ringIndex % kRing;
            if (!g_stagingPending[slot]) {
                g_imm->CopyResource(g_staging[slot], g_rt);
                g_stagingPending[slot] = true;
                g_ringIndex = (g_ringIndex + 1) % kRing;
            }
        }
    }
    if (g_scanRt) {
        ReadScanBack(g_imm);
        if (g_scanDrawnThisFrame) {
            int slot = g_scanIndex % kScanRing;
            if (!g_scanPending[slot]) {
                g_imm->CopyResource(g_scanStaging[slot], g_scanRt);
                g_scanPending[slot] = true;
                g_scanIndex = (g_scanIndex + 1) % kScanRing;
            }
        }
    }
    g_replayedThisFrame = false;
    g_clearedThisFrame = false;
    g_scanDrawnThisFrame = false;
    g_scanFrameActive = false;
    g_scanDraws = 0;

    // A short status line every 10 s while the screen is in use, so a report shows what the plugin saw.
    if (g_replays > 0 && GetTickCount64() - g_lastStats > 10000) {
        g_lastStats = GetTickCount64();
        Logf("TOUCH stats: probe draws=%d hits=%llu misses=%llu | mouse in window=(%.0f,%.0f of %.0fx%.0f) inside=%d -> "
             "target pixel (%.0f,%.0f of %.0fx%.0f) | last raw r=%g g=%g b=%g",
             g_replays, static_cast<unsigned long long>(g_hits), static_cast<unsigned long long>(g_misses), g_cursor.x,
             g_cursor.y, g_cursor.w, g_cursor.h, g_cursor.inside ? 1 : 0, g_lastPixelX, g_lastPixelY, g_lastRtW, g_lastRtH,
             g_lastRaw[0], g_lastRaw[1], g_lastRaw[2]);
    }

    {
        std::lock_guard<std::mutex> lock(g_stateMx);
        if (g_state.hover && (GetTickCount64() - g_hoverTick > 300 || !g_cursor.inside)) g_state.hover = false;
    }
    HandleClick();
}

void TouchSetButtonsVisible(bool visible) { g_buttonsVisible = visible; }

TouchState TouchGetState() {
    std::lock_guard<std::mutex> lock(g_stateMx);
    return g_state;
}

}  // namespace ep
