#include "touch.h"

#include "log.h"
#include "media.h"
#include "renderer.h"
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

std::mutex g_stateMx;
TouchState g_state;
uint64_t g_hoverTick = 0;
std::atomic<bool> g_buttonsVisible{false};
bool g_lmbDown = false;

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
    if (g_logged < 40) {
        ++g_logged;
        Logf("TOUCH probe pixel r=%.4f g=%.4f b=%.4f a=%.4f", r, g, b, px[3]);
    }
    if (b < 0.05f) {  // nothing was drawn at the cursor
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
    g_state.x = u * kScreenWidth;
    g_state.y = (GetSettings().flipVertical ? 1.0f - v : v) * kScreenHeight;
    g_state.hover = true;
    g_hoverTick = GetTickCount64();
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

void HandleClick() {
    bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 && !(GetAsyncKeyState(VK_RBUTTON) & 0x8000);
    bool edge = down && !g_lmbDown;
    g_lmbDown = down;
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
    if (!g_enabled || ctx != g_imm) return;
    for (UINT i = 0; i < count; ++i) {
        UINT slot = start + i;
        if (slot < g_mask.size()) g_mask.set(slot, isScreen[i]);
    }
    g_maskAny.store(g_mask.any(), std::memory_order_relaxed);
}

bool TouchNeedsReplay(ID3D11DeviceContext* ctx) {
    return g_enabled && !g_failed && !t_replaying && g_maskAny.load(std::memory_order_relaxed) && ctx == g_imm &&
           g_cursor.inside && g_ours;
}

void TouchReplay(ID3D11DeviceContext* ctx, const std::function<void()>& drawOnce) {
    if (!EnsureResources(ctx)) return;

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

    if (nvp > 0 && rtW > 0 && rtH > 0) {
        float px = std::floor(g_cursor.x * rtW / g_cursor.w);
        float py = std::floor(g_cursor.y * rtH / g_cursor.h);

        t_replaying = true;
        const FLOAT zero[4] = {0, 0, 0, 0};
        if (!g_clearedThisFrame) {
            ctx->ClearRenderTargetView(g_rtv, zero);
            g_clearedThisFrame = true;
        }
        ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        ctx->OMSetBlendState(g_noBlend, nullptr, 0xFFFFFFFF);
        ctx->OMSetDepthStencilState(g_noDepth, 0);
        // Shift the viewport so that the mouse pixel lands on pixel (0,0) of the 1x1 target.
        D3D11_VIEWPORT vp = vps[0];
        vp.TopLeftX -= px;
        vp.TopLeftY -= py;
        ctx->RSSetViewports(1, &vp);
        D3D11_RECT sc{0, 0, 1, 1};
        ctx->RSSetScissorRects(1, &sc);
        for (int i = 0; i < nSlots; ++i) g_psSet(ctx, slots[i], 1, &g_probeSrv);

        drawOnce();
        g_replayedThisFrame = true;
        ++g_replays;
        if (g_replays == 1) Logf("TOUCH first probe draw (cursor pixel %.0f,%.0f in a %.0fx%.0f target)", px, py, rtW, rtH);

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
    if (!g_enabled || g_failed) return;

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
    g_replayedThisFrame = false;
    g_clearedThisFrame = false;

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
