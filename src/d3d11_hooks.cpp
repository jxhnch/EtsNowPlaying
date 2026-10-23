#include "d3d11_hooks.h"

#include "engine_hooks.h"
#include "log.h"
#include "renderer.h"
#include "screen_assets.h"
#include "settings.h"
#include "touch.h"

#include <MinHook.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ep {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using PSSetSRVFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using CreateSRVFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*,
                                                const D3D11_SHADER_RESOURCE_VIEW_DESC*,
                                                ID3D11ShaderResourceView**);
using CreateTex2DFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
                                                  const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);

PresentFn g_present;
Present1Fn g_present1;
PSSetSRVFn g_psSetSrv;
CreateSRVFn g_createSrv;
CreateTex2DFn g_createTex2D;
std::atomic<bool> g_off{false};

// ---------------------------------------------------------------------------------------------
// Cabin screen. The game loads our 64x2048 BC3 placeholder (see screen_assets.h) in place of its GPS
// texture and fills it itself. We leave that texture completely alone and only swap the view at bind
// time: whenever the game binds a shader resource view of the placeholder, the pixel shader gets a view
// of our own BGRA texture instead. The renderer's frames are copied into that texture from Present.
// ---------------------------------------------------------------------------------------------
std::mutex g_screenMx;
ID3D11Texture2D* g_placeholder = nullptr;  // the game's texture (we keep one reference to compare pointers safely)
ID3D11Texture2D* g_ourTex = nullptr;       // our private BGRA texture
ID3D11ShaderResourceView* g_ourSrv = nullptr;
std::atomic<ID3D11ShaderResourceView*> g_ourSrvAtomic{nullptr};
bool g_placeholderSrgb = false;

constexpr int kMaxSwap = 8;
std::atomic<ID3D11ShaderResourceView*> g_swap[kMaxSwap];  // the game's views of the placeholder

uint64_t g_screenUploaded = 0;
ID3D11Texture2D* g_screenUploadedTo = nullptr;
bool g_replaceTexture = true;
bool g_uploadFrames = true;
uint64_t g_uploadCount = 0;
ULONGLONG g_lastHeartbeat = 0;
bool g_removedLogged = false;

const char* RemovedReasonName(HRESULT hr) {
    switch (hr) {
        case S_OK: return "S_OK";
        case DXGI_ERROR_DEVICE_HUNG: return "DEVICE_HUNG";
        case DXGI_ERROR_DEVICE_REMOVED: return "DEVICE_REMOVED";
        case DXGI_ERROR_DEVICE_RESET: return "DEVICE_RESET";
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DRIVER_INTERNAL_ERROR";
        case DXGI_ERROR_INVALID_CALL: return "INVALID_CALL";
        default: return "other";
    }
}

void LogDesc(const char* what, const D3D11_TEXTURE2D_DESC& d, bool hasInit) {
    Logf("SCREEN %s: %ux%u fmt=%u mips=%u array=%u samples=%u usage=%u bind=0x%x cpu=0x%x misc=0x%x init=%d", what, d.Width,
         d.Height, static_cast<unsigned>(d.Format), d.MipLevels, d.ArraySize, d.SampleDesc.Count,
         static_cast<unsigned>(d.Usage), d.BindFlags, d.CPUAccessFlags, d.MiscFlags, hasInit ? 1 : 0);
}

bool IsPlaceholder(const D3D11_TEXTURE2D_DESC* d) {
    return d->Width == kPlaceholderWidth && d->Height == kPlaceholderHeight && d->ArraySize == 1 &&
           (d->Format == DXGI_FORMAT_BC3_UNORM || d->Format == DXGI_FORMAT_BC3_UNORM_SRGB ||
            d->Format == DXGI_FORMAT_BC3_TYPELESS);
}

bool IsSwapSrv(ID3D11ShaderResourceView* s) {
    for (auto& slot : g_swap) {
        if (slot.load(std::memory_order_relaxed) == s) return true;
    }
    return false;
}

void AddSwapSrv(ID3D11ShaderResourceView* s) {
    for (auto& slot : g_swap) {
        if (slot.load() == s) return;
    }
    for (auto& slot : g_swap) {
        ID3D11ShaderResourceView* expected = nullptr;
        if (slot.compare_exchange_strong(expected, s)) return;
    }
    Logf("SCREEN too many views of the placeholder, ignoring one");
}

void RemoveSwapSrv(ID3D11ShaderResourceView* s) {
    for (auto& slot : g_swap) {
        ID3D11ShaderResourceView* expected = s;
        slot.compare_exchange_strong(expected, nullptr);
    }
}

// The game created its placeholder texture: make our private texture next to it.
void OnPlaceholderCreated(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& d, ID3D11Texture2D* placeholder, bool hasInit) {
    LogDesc("placeholder created by the game", d, hasInit);

    D3D11_TEXTURE2D_DESC nd{};
    nd.Width = kScreenWidth;
    nd.Height = kScreenHeight;
    nd.MipLevels = 1;
    nd.ArraySize = 1;
    nd.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
    nd.SampleDesc.Count = 1;
    nd.Usage = D3D11_USAGE_DEFAULT;
    nd.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = g_createTex2D(dev, &nd, nullptr, &tex);  // the original function: do not re-enter our hook
    if (FAILED(hr) || !tex) {
        Logf("SCREEN could not create our texture: 0x%08lx", static_cast<unsigned long>(hr));
        return;
    }
    std::lock_guard<std::mutex> lock(g_screenMx);
    if (g_ourSrv) {
        g_ourSrvAtomic = nullptr;
        g_ourSrv->Release();
        g_ourSrv = nullptr;
    }
    if (g_ourTex) g_ourTex->Release();
    if (g_placeholder) g_placeholder->Release();
    g_ourTex = tex;
    g_placeholder = placeholder;
    g_placeholder->AddRef();
    g_placeholderSrgb = d.Format == DXGI_FORMAT_BC3_UNORM_SRGB;
    Logf("SCREEN our %ux%u texture created next to the game's placeholder %p (thread %lu)", nd.Width, nd.Height,
         static_cast<void*>(placeholder), GetCurrentThreadId());
}

// The game made a view of the placeholder: make the matching view of our texture.
void OnPlaceholderViewCreated(ID3D11Device* dev, ID3D11ShaderResourceView* gameView,
                              const D3D11_SHADER_RESOURCE_VIEW_DESC* desc) {
    std::lock_guard<std::mutex> lock(g_screenMx);
    if (!g_ourTex) return;
    bool srgb = desc ? desc->Format == DXGI_FORMAT_BC3_UNORM_SRGB : g_placeholderSrgb;

    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* ours = nullptr;
    HRESULT hr = g_createSrv(dev, g_ourTex, &vd, &ours);
    if (FAILED(hr) || !ours) {
        Logf("SCREEN could not create our view: 0x%08lx", static_cast<unsigned long>(hr));
        return;
    }
    ID3D11ShaderResourceView* old = g_ourSrv;
    g_ourSrv = ours;
    g_ourSrvAtomic = ours;
    TouchSetScreenView(ours);
    if (old) old->Release();  // leaked bind slots keep their own reference
    AddSwapSrv(gameView);
    Logf("SCREEN game view %p of the placeholder will show our texture (%s)", static_cast<void*>(gameView),
         srgb ? "sRGB" : "linear");
}

HRESULT STDMETHODCALLTYPE HookCreateTexture2D(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC* desc,
                                              const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
    HRESULT hr = g_createTex2D(dev, desc, init, out);
    if (g_off || !desc || !out) return hr;
    if (FAILED(hr)) {
        static std::atomic<int> failures{0};
        if (failures.fetch_add(1) < 12) {
            LogDesc("CreateTexture2D FAILED", *desc, init != nullptr);
            Logf("SCREEN   hr=0x%08lx deviceRemovedReason=%s", static_cast<unsigned long>(hr),
                 RemovedReasonName(dev->GetDeviceRemovedReason()));
        }
        return hr;
    }
    if (g_replaceTexture && *out && IsPlaceholder(desc)) OnPlaceholderCreated(dev, *desc, *out, init != nullptr);
    return hr;
}

void UploadScreenFrame() {
    ID3D11Texture2D* tex = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_screenMx);
        tex = g_ourTex;
        if (tex) tex->AddRef();
    }
    if (!tex) return;

    ID3D11Device* dev = nullptr;
    tex->GetDevice(&dev);
    if (dev) {
        if (!g_removedLogged) {
            HRESULT reason = dev->GetDeviceRemovedReason();
            if (reason != S_OK) {
                g_removedLogged = true;
                Logf("SCREEN !!! the graphics device was removed: %s (0x%08lx) after %llu uploads", RemovedReasonName(reason),
                     static_cast<unsigned long>(reason), static_cast<unsigned long long>(g_uploadCount));
            }
        }
        ULONGLONG now = GetTickCount64();
        if (now - g_lastHeartbeat > 5000) {
            g_lastHeartbeat = now;
            Logf("SCREEN alive: %llu frames uploaded so far", static_cast<unsigned long long>(g_uploadCount));
        }
    }

    if (g_uploadFrames && dev && !g_removedLogged) {
        if (tex != g_screenUploadedTo) {
            g_screenUploadedTo = tex;
            g_screenUploaded = 0;
        }
        FrameView frame;
        if (RendererLockFrame(g_screenUploaded, &frame)) {
            ID3D11DeviceContext* ctx = nullptr;
            dev->GetImmediateContext(&ctx);
            if (ctx) {
                ctx->UpdateSubresource(tex, 0, nullptr, frame.data, frame.pitch, 0);
                ctx->Release();
                if (g_uploadCount == 0) Logf("SCREEN first frame uploaded (%ux%u)", frame.width, frame.height);
                ++g_uploadCount;
            }
            g_screenUploaded = frame.version;
            RendererUnlockFrame();
        }
    }
    if (dev) dev->Release();
    tex->Release();
}

// ---------------------------------------------------------------------------------------------
// Texture finder (diagnostic).
//
// Goal: find out which texture the cabin GPS screen samples, so later milestones can swap it.
// Method: record every 2D texture the game binds for ~2.5 s, then bisect: paint half of the
// candidates magenta, the user tells us whether the GPS turned magenta, repeat.
//   Ctrl+Shift+F6  start (GPS must be visible in the cabin)
//   Ctrl+Shift+F7  "the GPS screen is magenta"
//   Ctrl+Shift+F8  "the GPS screen is NOT magenta"
//   Ctrl+Shift+F9  stop
// ---------------------------------------------------------------------------------------------
struct Cand {
    ID3D11Texture2D* tex;
    D3D11_TEXTURE2D_DESC desc;
};

enum Mode { kIdle = 0, kRecording = 1, kTinting = 2 };

std::mutex g_mx;
std::unordered_map<ID3D11ShaderResourceView*, ID3D11Texture2D*> g_srvCache;
std::unordered_map<ID3D11Texture2D*, size_t> g_seen;
std::vector<Cand> g_cands;
std::unordered_set<ID3D11Texture2D*> g_tint;
size_t g_lo = 0, g_hi = 0, g_mid = 0;
ID3D11ShaderResourceView* g_tintSrv = nullptr;
std::atomic<int> g_mode{kIdle};
ULONGLONG g_recordUntil = 0;

const char* FormatName(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "RGBA8_SRGB";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "BGRA8_SRGB";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16F";
        case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10F";
        case DXGI_FORMAT_BC1_UNORM: return "BC1";
        case DXGI_FORMAT_BC1_UNORM_SRGB: return "BC1_SRGB";
        case DXGI_FORMAT_BC2_UNORM: return "BC2";
        case DXGI_FORMAT_BC3_UNORM: return "BC3";
        case DXGI_FORMAT_BC3_UNORM_SRGB: return "BC3_SRGB";
        case DXGI_FORMAT_BC4_UNORM: return "BC4";
        case DXGI_FORMAT_BC5_UNORM: return "BC5";
        case DXGI_FORMAT_BC7_UNORM: return "BC7";
        case DXGI_FORMAT_BC7_UNORM_SRGB: return "BC7_SRGB";
        case DXGI_FORMAT_R8_UNORM: return "R8";
        case DXGI_FORMAT_R8G8_UNORM: return "RG8";
        case DXGI_FORMAT_R16_TYPELESS: return "R16_TYPELESS";
        case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
        case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
        default: return "?";
    }
}

void LogCand(const char* prefix, size_t index, const Cand& c) {
    const D3D11_TEXTURE2D_DESC& d = c.desc;
    Logf("%s #%zu tex=%p %ux%u fmt=%u(%s) mips=%u usage=%u bind=0x%x cpu=0x%x misc=0x%x", prefix, index,
         static_cast<void*>(c.tex), d.Width, d.Height, static_cast<unsigned>(d.Format), FormatName(d.Format),
         d.MipLevels, static_cast<unsigned>(d.Usage), d.BindFlags, d.CPUAccessFlags, d.MiscFlags);
}

ID3D11Texture2D* ResolveTexLocked(ID3D11ShaderResourceView* srv) {
    auto it = g_srvCache.find(srv);
    if (it != g_srvCache.end()) return it->second;

    ID3D11Texture2D* tex = nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    srv->GetDesc(&vd);
    if (vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) {
        ID3D11Resource* res = nullptr;
        srv->GetResource(&res);
        if (res) {
            res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
            res->Release();
            if (tex) tex->Release();  // the SRV keeps the texture alive; we only keep a key
        }
    }
    g_srvCache[srv] = tex;
    return tex;
}

void EnsureTintSrvLocked(ID3D11DeviceContext* ctx) {
    if (g_tintSrv) return;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    uint32_t pixels[16];
    std::fill(std::begin(pixels), std::end(pixels), 0xFFFF00FFu);  // ABGR: opaque magenta
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 4;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{pixels, 16, 0};
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(dev->CreateTexture2D(&td, &sd, &tex))) {
        // Call the original: going through the vtable would enter HookCreateSRV and re-lock g_mx.
        g_createSrv(dev, tex, nullptr, &g_tintSrv);
        tex->Release();
    }
    dev->Release();
}

void STDMETHODCALLTYPE HookPSSetSRV(ID3D11DeviceContext* ctx, UINT start, UINT n, ID3D11ShaderResourceView* const* srvsIn) {
    if (g_off || !srvsIn || n == 0 || n > 128) return g_psSetSrv(ctx, start, n, srvsIn);

    // Cabin screen: the game binds its view of the placeholder, the shader gets ours.
    ID3D11ShaderResourceView* swapped[128];
    ID3D11ShaderResourceView* const* srvs = srvsIn;
    if (ID3D11ShaderResourceView* ours = g_ourSrvAtomic.load(std::memory_order_relaxed)) {
        bool any = false;
        bool isScreen[128];
        for (UINT i = 0; i < n; ++i) {
            ID3D11ShaderResourceView* s = srvsIn[i];
            isScreen[i] = s && IsSwapSrv(s);
            if (isScreen[i]) {
                if (!any) {
                    memcpy(swapped, srvsIn, n * sizeof(swapped[0]));
                    any = true;
                }
                swapped[i] = ours;
            }
        }
        if (any) srvs = swapped;
        if (any || TouchHasBindings()) TouchNoteBind(ctx, start, n, isScreen);  // beta: where is the screen bound?
    }

    int mode = g_mode.load(std::memory_order_relaxed);
    if (mode == kIdle) return g_psSetSrv(ctx, start, n, srvs);

    ID3D11ShaderResourceView* local[128];
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_mx);
        if (mode == kTinting) EnsureTintSrvLocked(ctx);
        for (UINT i = 0; i < n; ++i) {
            local[i] = srvs[i];
            if (!srvs[i]) continue;
            ID3D11Texture2D* tex = ResolveTexLocked(srvs[i]);
            if (!tex) continue;
            if (mode == kRecording) {
                if (g_seen.find(tex) == g_seen.end()) {
                    D3D11_TEXTURE2D_DESC d{};
                    tex->GetDesc(&d);
                    bool plain = d.ArraySize == 1 && !(d.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) &&
                                 d.SampleDesc.Count == 1 && d.Width >= 32 && d.Height >= 32;
                    g_seen[tex] = plain ? g_cands.size() : SIZE_MAX;
                    if (plain) g_cands.push_back({tex, d});
                }
            } else if (g_tintSrv && g_tint.count(tex)) {
                local[i] = g_tintSrv;
                changed = true;
            }
        }
    }
    g_psSetSrv(ctx, start, n, changed ? local : srvs);
}

HRESULT STDMETHODCALLTYPE HookCreateSRV(ID3D11Device* dev, ID3D11Resource* res,
                                        const D3D11_SHADER_RESOURCE_VIEW_DESC* desc, ID3D11ShaderResourceView** out) {
    HRESULT hr = g_createSrv(dev, res, desc, out);
    if (g_off) return hr;
    static std::atomic<int> srvFailures{0};
    if (FAILED(hr) && srvFailures.fetch_add(1) < 12) {
        Logf("SCREEN CreateShaderResourceView FAILED hr=0x%08lx deviceRemovedReason=%s", static_cast<unsigned long>(hr),
             RemovedReasonName(dev->GetDeviceRemovedReason()));
    }
    if (SUCCEEDED(hr) && out && *out) {
        // A new view may reuse the address of a released one: forget what we knew about it.
        RemoveSwapSrv(*out);
        {
            std::lock_guard<std::mutex> lock(g_mx);
            g_srvCache.erase(*out);
        }
        bool isPlaceholder = false;
        if (res) {
            std::lock_guard<std::mutex> lock(g_screenMx);
            isPlaceholder = res == g_placeholder;
        }
        if (isPlaceholder) OnPlaceholderViewCreated(dev, *out, desc);
    }
    return hr;
}

void SelectTintRangeLocked() {
    g_tint.clear();
    for (size_t i = g_lo; i < g_mid; ++i) g_tint.insert(g_cands[i].tex);
}

void ReportStepLocked() {
    if (g_hi - g_lo <= 1) {
        Logf("FINDER ====== RESULT: texture candidate #%zu ======", g_lo);
        LogCand("FINDER result", g_lo, g_cands[g_lo]);
        size_t same = 0;
        for (const Cand& c : g_cands) {
            if (c.desc.Width == g_cands[g_lo].desc.Width && c.desc.Height == g_cands[g_lo].desc.Height &&
                c.desc.Format == g_cands[g_lo].desc.Format)
                ++same;
        }
        Logf("FINDER %zu of %zu recorded textures share this width/height/format", same, g_cands.size());
        g_mid = g_hi;
        SelectTintRangeLocked();
        MessageBeep(MB_ICONASTERISK);
        return;
    }
    g_mid = g_lo + (g_hi - g_lo) / 2;
    SelectTintRangeLocked();
    Logf("FINDER range [%zu,%zu) -> painting [%zu,%zu) magenta. F7 = GPS is magenta, F8 = not magenta", g_lo, g_hi,
         g_lo, g_mid);
    MessageBeep(MB_OK);
}

void FinishRecording() {
    std::lock_guard<std::mutex> lock(g_mx);
    if (g_mode.load() != kRecording) return;
    if (g_cands.size() < 2) {
        Logf("FINDER recorded only %zu textures - is the game rendering? Stopping.", g_cands.size());
        g_mode = kIdle;
        return;
    }
    Logf("FINDER recorded %zu candidate textures", g_cands.size());
    g_lo = 0;
    g_hi = g_cands.size();
    g_mode = kTinting;
    ReportStepLocked();
}

void FinderKey(int vk) {
    switch (vk) {
        case VK_F6: {
            std::lock_guard<std::mutex> lock(g_mx);
            g_seen.clear();
            g_cands.clear();
            g_tint.clear();
            g_recordUntil = GetTickCount64() + 2500;
            g_mode = kRecording;
            Logf("FINDER recording textures for 2.5 s - keep the GPS screen in view");
            break;
        }
        case VK_F7:
        case VK_F8: {
            std::lock_guard<std::mutex> lock(g_mx);
            if (g_mode.load() != kTinting) break;
            if (g_hi - g_lo <= 1) {
                Logf("FINDER already at a single candidate; press Ctrl+Shift+F6 to restart");
                break;
            }
            if (vk == VK_F7) g_hi = g_mid;  // GPS was painted: it is in the painted half
            else g_lo = g_mid;              // not painted: it is in the other half
            ReportStepLocked();
            break;
        }
        case VK_F9: {
            std::lock_guard<std::mutex> lock(g_mx);
            g_mode = kIdle;
            g_tint.clear();
            Logf("FINDER stopped");
            break;
        }
    }
}

bool ForegroundIsUs() {
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

void OnFrame(IDXGISwapChain* sc) {
    static bool down[4] = {};
    static const int keys[4] = {VK_F6, VK_F7, VK_F8, VK_F9};

    if (g_mode.load() == kRecording && GetTickCount64() >= g_recordUntil) FinishRecording();
    UploadScreenFrame();
    TouchOnPresent(sc);

    if (!GetSettings().textureFinder) return;  // developer tool, off unless [debug] texture_finder=1
    bool chord = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && ForegroundIsUs();
    for (int i = 0; i < 4; ++i) {
        bool now = chord && (GetAsyncKeyState(keys[i]) & 0x8000);
        if (now && !down[i]) FinderKey(keys[i]);
        down[i] = now;
    }
}

thread_local int t_presentDepth = 0;

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!g_off && t_presentDepth == 0) OnFrame(sc);
    ++t_presentDepth;
    HRESULT hr = g_present(sc, sync, flags);
    --t_presentDepth;
    return hr;
}

// Beta touch: the draw calls that show the screen are repeated once into a 1x1 probe target.
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
DrawIndexedFn g_drawIndexed;
DrawIndexedInstancedFn g_drawIndexedInstanced;

void STDMETHODCALLTYPE HookDrawIndexed(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base) {
    if (!g_off && TouchNeedsReplay(ctx)) TouchReplay(ctx, [&] { g_drawIndexed(ctx, count, start, base); });
    g_drawIndexed(ctx, count, start, base);
}

void STDMETHODCALLTYPE HookDrawIndexedInstanced(ID3D11DeviceContext* ctx, UINT count, UINT instances, UINT start, INT base,
                                                UINT startInstance) {
    if (!g_off && TouchNeedsReplay(ctx)) {
        TouchReplay(ctx, [&] { g_drawIndexedInstanced(ctx, count, instances, start, base, startInstance); });
    }
    g_drawIndexedInstanced(ctx, count, instances, start, base, startInstance);
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) {
    if (!g_off && t_presentDepth == 0) OnFrame(sc);
    ++t_presentDepth;
    HRESULT hr = g_present1(sc, sync, flags, pp);
    --t_presentDepth;
    return hr;
}

LRESULT CALLBACK DummyWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

}  // namespace

bool D3D11HooksInstall(bool replaceTexture, bool uploadFrames, bool touch) {
    g_replaceTexture = replaceTexture;
    g_uploadFrames = uploadFrames;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DummyWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"EtsNowPlayingDummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 16, 16, nullptr, nullptr,
                                wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = sd.BufferDesc.Height = 16;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr)) {
        Logf("D3D11 dummy device failed: 0x%08lx", static_cast<unsigned long>(hr));
        if (hwnd) DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    auto vt = [](void* obj) { return *reinterpret_cast<void***>(obj); };
    void* present = vt(sc)[8];
    void* createTex2D = vt(dev)[5];
    void* createSrv = vt(dev)[7];
    void* psSetSrv = vt(ctx)[8];
    void* drawIndexed = vt(ctx)[12];
    void* drawIndexedInstanced = vt(ctx)[20];
    void* present1 = nullptr;
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&sc1)))) {
        present1 = vt(sc1)[22];
        sc1->Release();
    }

    ctx->Release();
    dev->Release();
    sc->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    bool ok = true;
    auto hook = [&](const char* name, void* target, void* detour, void** orig) {
        if (!target) return;
        MH_STATUS st = MH_CreateHook(target, detour, orig);
        if (st != MH_OK) {
            Logf("D3D11 hook %s failed: %s", name, MH_StatusToString(st));
            ok = false;
        }
    };
    hook("Present", present, reinterpret_cast<void*>(&HookPresent), reinterpret_cast<void**>(&g_present));
    hook("Present1", present1, reinterpret_cast<void*>(&HookPresent1), reinterpret_cast<void**>(&g_present1));
    hook("CreateTexture2D", createTex2D, reinterpret_cast<void*>(&HookCreateTexture2D),
         reinterpret_cast<void**>(&g_createTex2D));
    hook("CreateShaderResourceView", createSrv, reinterpret_cast<void*>(&HookCreateSRV),
         reinterpret_cast<void**>(&g_createSrv));
    hook("PSSetShaderResources", psSetSrv, reinterpret_cast<void*>(&HookPSSetSRV),
         reinterpret_cast<void**>(&g_psSetSrv));
    if (touch) {  // draw hooks cost a little on every draw call, so only when the beta feature is on
        hook("DrawIndexed", drawIndexed, reinterpret_cast<void*>(&HookDrawIndexed), reinterpret_cast<void**>(&g_drawIndexed));
        hook("DrawIndexedInstanced", drawIndexedInstanced, reinterpret_cast<void*>(&HookDrawIndexedInstanced),
             reinterpret_cast<void**>(&g_drawIndexedInstanced));
    }
    if (!ok) return false;
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        Logf("D3D11 MH_EnableHook failed");
        return false;
    }
    TouchInit(touch, g_psSetSrv);
    Logf("D3D11 hooks installed (Present=%p Present1=%p CreateTex2D=%p CreateSRV=%p PSSetSRV=%p)", present, present1,
         createTex2D, createSrv, psSetSrv);
    return true;
}

void D3D11HooksDisable() {
    g_off = true;
    g_mode = kIdle;
}

}  // namespace ep
