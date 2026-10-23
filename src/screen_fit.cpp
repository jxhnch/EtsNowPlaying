#include "screen_fit.h"

#include "log.h"
#include "renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ep {
namespace {

struct Elem {
    std::string semantic;
    UINT index = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT slot = 0;
    UINT offset = 0;
};

struct Geometry {
    bool tried = false;
    bool ok = false;
    double uvMin[2] = {0, 0}, uvMax[2] = {1, 1};
    double lenU = 1, lenV = 1;  // physical length of one texture unit along u and v
};

std::mutex g_mx;
bool g_enabled = false;
std::unordered_map<ID3D11InputLayout*, std::vector<Elem>> g_layouts;
Geometry g_geo;
ScreenOrientation g_orient;      // accepted orientation
ScreenOrientation g_lastOrient;  // last measurement, accepted after three equal ones
int g_orientVotes = 0;
bool g_orientStable = false;
ScreenFit g_fit;

// ---- vertex formats -------------------------------------------------------------------------------------------

float HalfToFloat(uint16_t h) {
    uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, mant = h & 1023;
    float v;
    if (exp == 0) v = std::ldexp(static_cast<float>(mant), -24);
    else if (exp == 31) v = mant ? NAN : INFINITY;
    else v = std::ldexp(static_cast<float>(mant | 1024), static_cast<int>(exp) - 25);
    return sign ? -v : v;
}

UINT FormatBytes(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT: return 16;
        case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32B32_UINT: case DXGI_FORMAT_R32G32B32_SINT: return 12;
        case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT:
        case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SINT: return 8;
        case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SINT: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_SNORM: return 2;
        case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: return 1;
        default: return 4;  // 32-bit single values, R16G16_*, R8G8B8A8_*, R10G10B10A2_*, R11G11B10_FLOAT ...
    }
}

// Decodes one vertex attribute into floats. Normalised formats give -1..1 / 0..1, integer formats raw values.
bool DecodeAttribute(const uint8_t* p, DXGI_FORMAT f, float out[4], int& n) {
    out[0] = out[1] = out[2] = out[3] = 0;
    auto s16 = [&](int i) { int16_t v; memcpy(&v, p + 2 * i, 2); return v; };
    auto u16 = [&](int i) { uint16_t v; memcpy(&v, p + 2 * i, 2); return v; };
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: n = 4; memcpy(out, p, 16); return true;
        case DXGI_FORMAT_R32G32B32_FLOAT: n = 3; memcpy(out, p, 12); return true;
        case DXGI_FORMAT_R32G32_FLOAT: n = 2; memcpy(out, p, 8); return true;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: n = 4; for (int i = 0; i < 4; ++i) out[i] = HalfToFloat(u16(i)); return true;
        case DXGI_FORMAT_R16G16_FLOAT: n = 2; for (int i = 0; i < 2; ++i) out[i] = HalfToFloat(u16(i)); return true;
        case DXGI_FORMAT_R16G16B16A16_SNORM: n = 4; for (int i = 0; i < 4; ++i) out[i] = std::max(s16(i) / 32767.0f, -1.0f); return true;
        case DXGI_FORMAT_R16G16_SNORM: n = 2; for (int i = 0; i < 2; ++i) out[i] = std::max(s16(i) / 32767.0f, -1.0f); return true;
        case DXGI_FORMAT_R16G16B16A16_UNORM: n = 4; for (int i = 0; i < 4; ++i) out[i] = u16(i) / 65535.0f; return true;
        case DXGI_FORMAT_R16G16_UNORM: n = 2; for (int i = 0; i < 2; ++i) out[i] = u16(i) / 65535.0f; return true;
        case DXGI_FORMAT_R16G16B16A16_SINT: n = 4; for (int i = 0; i < 4; ++i) out[i] = s16(i); return true;
        case DXGI_FORMAT_R16G16_SINT: n = 2; for (int i = 0; i < 2; ++i) out[i] = s16(i); return true;
        case DXGI_FORMAT_R16G16B16A16_UINT: n = 4; for (int i = 0; i < 4; ++i) out[i] = u16(i); return true;
        case DXGI_FORMAT_R16G16_UINT: n = 2; for (int i = 0; i < 2; ++i) out[i] = u16(i); return true;
        case DXGI_FORMAT_R8G8B8A8_UNORM: n = 4; for (int i = 0; i < 4; ++i) out[i] = p[i] / 255.0f; return true;
        case DXGI_FORMAT_R8G8B8A8_SNORM: n = 4; for (int i = 0; i < 4; ++i) out[i] = std::max(static_cast<int8_t>(p[i]) / 127.0f, -1.0f); return true;
        default: return false;
    }
}

std::string Upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}

// ---- reading buffers back --------------------------------------------------------------------------------------

// Copies [start, start+len) of a GPU buffer to the CPU. Blocks until the GPU is done, which is fine: it happens
// once per screen mesh.
bool ReadRange(ID3D11DeviceContext* ctx, ID3D11Buffer* src, UINT start, UINT len, std::vector<uint8_t>& out) {
    D3D11_BUFFER_DESC sd{};
    src->GetDesc(&sd);
    if (len == 0 || len > 16u * 1024 * 1024 || start > sd.ByteWidth || len > sd.ByteWidth - start) return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = len;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* staging = nullptr;
    bool ok = SUCCEEDED(dev->CreateBuffer(&d, nullptr, &staging));
    dev->Release();
    if (!ok) return false;
    D3D11_BOX box{start, 0, 0, start + len, 1, 1};
    ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, src, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m{};
    ok = SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m));
    if (ok) {
        out.assign(static_cast<const uint8_t*>(m.pData), static_cast<const uint8_t*>(m.pData) + len);
        ctx->Unmap(staging, 0);
    }
    staging->Release();
    return ok;
}

// ---- least squares: p = c0 + c1*u + c2*v ------------------------------------------------------------------------

bool Solve3(double a[3][4]) {  // Gauss-Jordan on an augmented 3x4 matrix, result in column 3
    for (int i = 0; i < 3; ++i) {
        int piv = i;
        for (int r = i + 1; r < 3; ++r) {
            if (std::fabs(a[r][i]) > std::fabs(a[piv][i])) piv = r;
        }
        if (std::fabs(a[piv][i]) < 1e-12) return false;
        if (piv != i) {
            for (int c = 0; c < 4; ++c) std::swap(a[i][c], a[piv][c]);
        }
        for (int r = 0; r < 3; ++r) {
            if (r == i) continue;
            double f = a[r][i] / a[i][i];
            for (int c = i; c < 4; ++c) a[r][c] -= f * a[i][c];
        }
    }
    for (int i = 0; i < 3; ++i) a[i][3] /= a[i][i];
    return true;
}

struct Vertex {
    double p[3];
    double uv[2];
};

struct Surface {
    double uvMin[2], uvMax[2];
    double lenU, lenV;  // physical length of a texture unit along u / v
    double error;       // RMS distance of the vertices from the fitted flat surface, relative to its size
};

// Fits "position = C0 + C1*u + C2*v" to the vertices. A flat display with plain texture mapping fits almost exactly;
// anything else (wrong guess of the vertex format, curved or tiled surfaces) gives a large error.
bool FitSurface(const std::vector<Vertex>& verts, Surface& s) {
    if (verts.size() < 3) return false;
    s.uvMin[0] = s.uvMin[1] = 1e30;
    s.uvMax[0] = s.uvMax[1] = -1e30;
    double n[3][3] = {}, rhs[3][3] = {};
    for (const Vertex& v : verts) {
        for (int k = 0; k < 2; ++k) {
            s.uvMin[k] = std::min(s.uvMin[k], v.uv[k]);
            s.uvMax[k] = std::max(s.uvMax[k], v.uv[k]);
        }
        const double basis[3] = {1.0, v.uv[0], v.uv[1]};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) n[r][c] += basis[r] * basis[c];
            for (int k = 0; k < 3; ++k) rhs[k][r] += basis[r] * v.p[k];
        }
    }
    double coef[3][3];  // coef[k][0..2] = c0,c1,c2 of position component k
    for (int k = 0; k < 3; ++k) {
        double a[3][4];
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) a[r][c] = n[r][c];
            a[r][3] = rhs[k][r];
        }
        if (!Solve3(a)) return false;
        for (int c = 0; c < 3; ++c) coef[k][c] = a[c][3];
    }
    s.lenU = std::sqrt(coef[0][1] * coef[0][1] + coef[1][1] * coef[1][1] + coef[2][1] * coef[2][1]);
    s.lenV = std::sqrt(coef[0][2] * coef[0][2] + coef[1][2] * coef[1][2] + coef[2][2] * coef[2][2]);
    double err = 0;
    for (const Vertex& v : verts) {
        for (int k = 0; k < 3; ++k) {
            double d = v.p[k] - (coef[k][0] + coef[k][1] * v.uv[0] + coef[k][2] * v.uv[1]);
            err += d * d;
        }
    }
    const double pu = s.lenU * (s.uvMax[0] - s.uvMin[0]), pv = s.lenV * (s.uvMax[1] - s.uvMin[1]);
    const double diag = std::sqrt(pu * pu + pv * pv);
    if (!(diag > 0) || !std::isfinite(diag)) return false;
    s.error = std::sqrt(err / verts.size()) / diag;
    return std::isfinite(s.error);
}

bool PlausibleSurface(const Surface& s) {
    const double du = s.uvMax[0] - s.uvMin[0], dv = s.uvMax[1] - s.uvMin[1];
    if (!(du > 0.02 && dv > 0.02 && s.lenU > 0 && s.lenV > 0)) return false;
    if (s.uvMin[0] < -0.05 || s.uvMin[1] < -0.05 || s.uvMax[0] > 1.05 || s.uvMax[1] > 1.05) return false;
    const double aspect = (s.lenU * du) / (s.lenV * dv);
    return aspect > 0.1 && aspect < 10.0;
}

// ---- decoding a draw call's vertices -------------------------------------------------------------------------------

struct Source {
    const std::vector<uint8_t>* bytes = nullptr;  // vertices minV..maxV of one vertex buffer slot
    UINT stride = 0;
};

bool DecodeVertices(const std::vector<uint32_t>& unique, uint32_t minV, const Source& pos, UINT posOffset, DXGI_FORMAT posFormat,
                    const Source& uv, UINT uvOffset, DXGI_FORMAT uvFormat, std::vector<Vertex>& out) {
    out.clear();
    for (uint32_t v : unique) {
        size_t po = static_cast<size_t>(v - minV) * pos.stride + posOffset;
        size_t uo = static_cast<size_t>(v - minV) * uv.stride + uvOffset;
        if (po + FormatBytes(posFormat) > pos.bytes->size() || uo + FormatBytes(uvFormat) > uv.bytes->size()) return false;
        float a[4], b[4];
        int na = 0, nb = 0;
        if (!DecodeAttribute(pos.bytes->data() + po, posFormat, a, na) || na < 3 ||
            !DecodeAttribute(uv.bytes->data() + uo, uvFormat, b, nb) || nb < 2) {
            return false;
        }
        Vertex vx{};
        for (int k = 0; k < 3; ++k) vx.p[k] = a[k];
        vx.uv[0] = b[0];
        vx.uv[1] = b[1];
        if (!std::isfinite(vx.p[0] + vx.p[1] + vx.p[2] + vx.uv[0] + vx.uv[1])) return false;
        if (std::fabs(vx.p[0]) > 1e5 || std::fabs(vx.p[1]) > 1e5 || std::fabs(vx.p[2]) > 1e5) return false;
        out.push_back(vx);
    }
    return out.size() >= 3;
}

const DXGI_FORMAT kPositionGuesses[] = {DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_SNORM,
                                        DXGI_FORMAT_R32G32B32A32_FLOAT};
const DXGI_FORMAT kUvGuesses[] = {DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_SNORM};

struct Layout {
    UINT posOffset = 0, uvOffset = 0;
    DXGI_FORMAT posFormat = DXGI_FORMAT_UNKNOWN, uvFormat = DXGI_FORMAT_UNKNOWN;
    bool guessed = false;
    double error = 0;
};

// The game's vertex layout was created before the plugin started, so it is unknown. Try the usual formats and
// offsets; only a combination in which position and texture coordinate fit together almost exactly is accepted.
bool GuessLayout(const std::vector<uint32_t>& unique, uint32_t minV, const Source& src, Layout& best) {
    best.error = 1e30;
    std::vector<Vertex> verts;
    int tried = 0, plausible = 0;
    for (DXGI_FORMAT pf : kPositionGuesses) {
        const UINT psz = FormatBytes(pf);
        for (UINT po = 0; po + psz <= src.stride; po += 4) {
            for (DXGI_FORMAT uf : kUvGuesses) {
                const UINT usz = FormatBytes(uf);
                for (UINT uo = 0; uo + usz <= src.stride; uo += 2) {
                    if (uo < po + psz && po < uo + usz) continue;  // overlapping fields
                    if (!DecodeVertices(unique, minV, src, po, pf, src, uo, uf, verts)) continue;
                    ++tried;
                    Surface s;
                    if (!FitSurface(verts, s) || !PlausibleSurface(s)) continue;
                    ++plausible;
                    if (s.error < best.error) {
                        best = Layout{po, uo, pf, uf, true, s.error};
                    }
                }
            }
        }
    }
    Logf("FIT layout guess: %d combinations decoded, %d plausible, best error %.4f%%", tried, plausible,
         plausible ? best.error * 100 : -1.0);
    return plausible > 0 && best.error < 0.005;  // better than 0.5% of the display's size
}

Geometry Analyze(ID3D11DeviceContext* ctx, UINT indexCount, UINT startIndex, INT baseVertex) {
    Geometry g;
    g.tried = true;
    if (indexCount < 3 || indexCount > 6000) {
        Logf("FIT draw has %u indices, not a plain screen mesh", indexCount);
        return g;
    }

    ID3D11InputLayout* layout = nullptr;
    ctx->IAGetInputLayout(&layout);
    ID3D11Buffer* vbs[8] = {};
    UINT strides[8] = {}, offsets[8] = {};
    ctx->IAGetVertexBuffers(0, 8, vbs, strides, offsets);
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT ibFormat = DXGI_FORMAT_UNKNOWN;
    UINT ibOffset = 0;
    ctx->IAGetIndexBuffer(&ib, &ibFormat, &ibOffset);

    auto cleanup = [&] {
        if (layout) layout->Release();
        for (auto* b : vbs) {
            if (b) b->Release();
        }
        if (ib) ib->Release();
    };

    std::vector<Elem> elems;
    {
        std::lock_guard<std::mutex> lock(g_mx);
        auto it = g_layouts.find(layout);
        if (layout && it != g_layouts.end()) elems = it->second;
    }
    if (!ib) {
        Logf("FIT draw is not indexed");
        cleanup();
        return g;
    }

    // Indices of this draw call.
    const UINT isz = ibFormat == DXGI_FORMAT_R16_UINT ? 2 : ibFormat == DXGI_FORMAT_R32_UINT ? 4 : 0;
    std::vector<uint8_t> ibytes;
    if (!isz || !ReadRange(ctx, ib, ibOffset + startIndex * isz, indexCount * isz, ibytes)) {
        Logf("FIT could not read the index buffer (format %d)", static_cast<int>(ibFormat));
        cleanup();
        return g;
    }
    std::vector<uint32_t> idx(indexCount);
    for (UINT i = 0; i < indexCount; ++i) {
        if (isz == 2) {
            uint16_t v;
            memcpy(&v, &ibytes[i * 2], 2);
            idx[i] = v;
        } else {
            memcpy(&idx[i], &ibytes[i * 4], 4);
        }
    }
    const uint32_t minV = *std::min_element(idx.begin(), idx.end()), maxV = *std::max_element(idx.begin(), idx.end());
    if (static_cast<int64_t>(minV) + baseVertex < 0 || maxV - minV > 60000) {
        Logf("FIT odd index range %u..%u base %d", minV, maxV, baseVertex);
        cleanup();
        return g;
    }
    std::vector<uint32_t> unique = idx;
    std::sort(unique.begin(), unique.end());
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());

    // Vertex data of the slots we need (read back once per slot).
    std::vector<uint8_t> slotBytes[8];
    auto loadSlot = [&](UINT s) {
        if (s >= 8 || !vbs[s] || !strides[s]) return false;
        if (!slotBytes[s].empty()) return true;
        UINT first = offsets[s] + static_cast<UINT>(minV + baseVertex) * strides[s];
        UINT len = (maxV - minV + 1) * strides[s];
        return ReadRange(ctx, vbs[s], first, len, slotBytes[s]);
    };

    Layout chosen;
    UINT posSlot = 0, uvSlot = 0;
    if (!elems.empty()) {
        for (const Elem& e : elems) {
            Logf("FIT layout element %s%u format=%d slot=%u offset=%u", e.semantic.c_str(), e.index, static_cast<int>(e.format),
                 e.slot, e.offset);
        }
        const Elem *posEl = nullptr, *uvEl = nullptr;
        for (const Elem& e : elems) {
            std::string s = Upper(e.semantic);
            if (s == "POSITION" && e.index == 0 && !posEl) posEl = &e;
            if (s == "TEXCOORD" && e.index == 0 && !uvEl) uvEl = &e;
        }
        if (!posEl || !uvEl) {
            Logf("FIT layout has no POSITION0 / TEXCOORD0");
            cleanup();
            return g;
        }
        chosen = Layout{posEl->offset, uvEl->offset, posEl->format, uvEl->format, false, 0};
        posSlot = posEl->slot;
        uvSlot = uvEl->slot;
    } else {
        Logf("FIT input layout %p was created before the plugin started, guessing the vertex format (stride %u)",
             static_cast<void*>(layout), strides[0]);
        if (!loadSlot(0) || !GuessLayout(unique, minV, Source{&slotBytes[0], strides[0]}, chosen)) {
            Logf("FIT could not work out the vertex format");
            cleanup();
            return g;
        }
        Logf("FIT guessed: position format %d at offset %u, texcoord format %d at offset %u", static_cast<int>(chosen.posFormat),
             chosen.posOffset, static_cast<int>(chosen.uvFormat), chosen.uvOffset);
    }
    if (!loadSlot(posSlot) || !loadSlot(uvSlot)) {
        Logf("FIT could not read the vertex buffers (slots %u, %u)", posSlot, uvSlot);
        cleanup();
        return g;
    }
    std::vector<Vertex> verts;
    bool decoded = DecodeVertices(unique, minV, Source{&slotBytes[posSlot], strides[posSlot]}, chosen.posOffset, chosen.posFormat,
                                  Source{&slotBytes[uvSlot], strides[uvSlot]}, chosen.uvOffset, chosen.uvFormat, verts);
    cleanup();
    if (!decoded) {
        Logf("FIT cannot decode the vertex data");
        return g;
    }

    Surface s;
    if (!FitSurface(verts, s)) {
        Logf("FIT texture coordinates are degenerate");
        return g;
    }
    const double du = s.uvMax[0] - s.uvMin[0], dv = s.uvMax[1] - s.uvMin[1];
    const double pu = s.lenU * du, pv = s.lenV * dv;
    Logf("FIT mesh: %zu vertices, %u indices, uv u=%.4f..%.4f v=%.4f..%.4f, physical size %.5f x %.5f (aspect %.3f), "
         "flatness error %.3f%%",
         verts.size(), indexCount, s.uvMin[0], s.uvMax[0], s.uvMin[1], s.uvMax[1], pu, pv, pv > 0 ? pu / pv : 0.0, s.error * 100);
    if (!PlausibleSurface(s)) {
        Logf("FIT rejected: texture range or aspect ratio not plausible for a display");
        return g;
    }
    if (s.error > 0.03) {
        Logf("FIT rejected: the mesh is not a flat surface with simple texture mapping");
        return g;
    }
    g.ok = true;
    for (int k = 0; k < 2; ++k) {
        g.uvMin[k] = s.uvMin[k];
        g.uvMax[k] = s.uvMax[k];
    }
    g.lenU = s.lenU;
    g.lenV = s.lenV;
    return g;
}

// ---- combining ---------------------------------------------------------------------------------------------------

// Caller holds g_mx.
void Recompute() {
    g_fit = ScreenFit{};
    if (!g_orientStable) return;
    const ScreenOrientation& o = g_orient;

    double umin = 0, vmin = 0, du = 1, dv = 1;
    if (g_geo.ok) {
        umin = g_geo.uvMin[0];
        vmin = g_geo.uvMin[1];
        du = g_geo.uvMax[0] - umin;
        dv = g_geo.uvMax[1] - vmin;
    }
    // Physical width (along the screen's right direction) and height (along up) of the visible part.
    const double pu = g_geo.ok ? g_geo.lenU * du : 1.0, pv = g_geo.ok ? g_geo.lenV * dv : 1.0;
    double W = o.rightAxis == 0 ? pu : pv, H = o.upAxis == 0 ? pu : pv;
    if (!g_geo.ok) {  // shape unknown: let the picture fill the visible area
        W = kCanvasWidth;
        H = kCanvasHeight;
    }
    const double s = std::min(W / kCanvasWidth, H / kCanvasHeight);  // physical units per canvas unit

    auto toTexel = [&](double cx, double cy, double& tx, double& ty) {
        double tr = 0.5 + (cx - kCanvasWidth / 2.0) * s / W;   // 0..1 along the screen's right direction
        double tu = 0.5 - (cy - kCanvasHeight / 2.0) * s / H;  // 0..1 along its up direction
        if (o.rightSign < 0) tr = 1.0 - tr;
        if (o.upSign < 0) tu = 1.0 - tu;
        double uv[2];
        const double mn[2] = {umin, vmin}, rg[2] = {du, dv};
        uv[o.rightAxis] = mn[o.rightAxis] + tr * rg[o.rightAxis];
        uv[o.upAxis] = mn[o.upAxis] + tu * rg[o.upAxis];
        tx = uv[0] * kScreenWidth;
        ty = uv[1] * kScreenHeight;
    };
    double x0, y0, x1, y1, x2, y2;
    toTexel(0, 0, x0, y0);
    toTexel(kCanvasWidth, 0, x1, y1);
    toTexel(0, kCanvasHeight, x2, y2);
    double* m = g_fit.canvasToTexel;
    m[0] = (x1 - x0) / kCanvasWidth;   // a
    m[1] = (y1 - y0) / kCanvasWidth;   // b
    m[2] = (x2 - x0) / kCanvasHeight;  // c
    m[3] = (y2 - y0) / kCanvasHeight;  // d
    m[4] = x0;                         // e
    m[5] = y0;                         // f
    const double det = m[0] * m[3] - m[1] * m[2];
    if (std::fabs(det) < 1e-9) return;
    double* inv = g_fit.texelToCanvas;
    inv[0] = m[3] / det;
    inv[1] = -m[1] / det;
    inv[2] = -m[2] / det;
    inv[3] = m[0] / det;
    inv[4] = -(inv[0] * m[4] + inv[2] * m[5]);
    inv[5] = -(inv[1] * m[4] + inv[3] * m[5]);
    g_fit.fromGeometry = g_geo.ok;
    g_fit.valid = true;
    Logf("FIT result: %s | screen right = texture %c%c, screen up = texture %c%c | canvas->texel [%.3f %.3f %.3f %.3f %.1f %.1f]",
         g_geo.ok ? "real display shape" : "orientation only (picture fills the display)", o.rightSign > 0 ? '+' : '-',
         o.rightAxis == 0 ? 'u' : 'v', o.upSign > 0 ? '+' : '-', o.upAxis == 0 ? 'u' : 'v', m[0], m[1], m[2], m[3], m[4],
         m[5]);
}

}  // namespace

void FitInit(bool enabled) {
    g_enabled = enabled;
    Logf("FIT %s", enabled ? "enabled" : "disabled");
}

bool FitEnabled() { return g_enabled; }

void FitReset(const char* reason) {
    if (!g_enabled) return;
    std::lock_guard<std::mutex> lock(g_mx);
    g_geo = Geometry{};
    g_orient = ScreenOrientation{};
    g_lastOrient = ScreenOrientation{};
    g_orientVotes = 0;
    g_orientStable = false;
    g_fit = ScreenFit{};
    Logf("FIT reset (%s)", reason);
}

void FitOnCreateInputLayout(ID3D11InputLayout* layout, const D3D11_INPUT_ELEMENT_DESC* elements, UINT count) {
    if (!g_enabled || !layout || !elements) return;
    std::vector<Elem> list;
    std::unordered_map<UINT, UINT> next;  // running offset per slot for D3D11_APPEND_ALIGNED_ELEMENT
    for (UINT i = 0; i < count; ++i) {
        Elem e;
        e.semantic = elements[i].SemanticName ? elements[i].SemanticName : "";
        e.index = elements[i].SemanticIndex;
        e.format = elements[i].Format;
        e.slot = elements[i].InputSlot;
        e.offset = elements[i].AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT ? next[e.slot] : elements[i].AlignedByteOffset;
        next[e.slot] = e.offset + FormatBytes(e.format);
        list.push_back(std::move(e));
    }
    std::lock_guard<std::mutex> lock(g_mx);
    g_layouts[layout] = std::move(list);
}

bool FitNeedsGeometry() {
    if (!g_enabled) return false;
    std::lock_guard<std::mutex> lock(g_mx);
    return !g_geo.tried;
}

void FitCaptureGeometry(ID3D11DeviceContext* ctx, UINT indexCount, UINT startIndex, INT baseVertex) {
    {
        std::lock_guard<std::mutex> lock(g_mx);
        if (g_geo.tried) return;
        g_geo.tried = true;  // one attempt per screen, whatever happens
    }
    Geometry g = Analyze(ctx, indexCount, startIndex, baseVertex);
    std::lock_guard<std::mutex> lock(g_mx);
    g_geo = g;
    Recompute();
}

bool FitNeedsOrientation() {
    if (!g_enabled) return false;
    std::lock_guard<std::mutex> lock(g_mx);
    return !g_orientStable;
}

void FitNoteOrientation(const ScreenOrientation& o) {
    if (!g_enabled || !o.ok) return;
    std::lock_guard<std::mutex> lock(g_mx);
    if (g_orientStable) return;
    bool same = g_lastOrient.ok && g_lastOrient.rightAxis == o.rightAxis && g_lastOrient.rightSign == o.rightSign &&
                g_lastOrient.upAxis == o.upAxis && g_lastOrient.upSign == o.upSign;
    g_orientVotes = same ? g_orientVotes + 1 : 1;
    g_lastOrient = o;
    if (g_orientVotes >= 3) {  // the same answer three times in a row
        g_orient = o;
        g_orientStable = true;
        Recompute();
    }
}

ScreenFit FitGet() {
    std::lock_guard<std::mutex> lock(g_mx);
    return g_fit;
}

}  // namespace ep
