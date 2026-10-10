// Batman City AFW - alternate frame warping (BATMAN_CITY_PLAN.md §11). Port of akvr_native/src/afw.cpp (AKN AFW1,
// JJ 2026-10-10 "it works really well", FEATHER edge blend). Differences here:
// - depth comes from the scene depth the game bound (gfx_hud::scene_depth), so AFW works with DLSS on or off;
// - this device refuses a private context state (DLSS_PLAN D1a): the game's compute bindings are saved by hand;
// - the core calls synth() at the drawn eye's present with both headset images acquired (one present = one pair).
// Pass 1 (render size): every depth pixel of the drawn eye -> its point in the world -> the other eye's pixel; the
// nearest point wins (InterlockedMin on the distance). Pass 2 (headset size): each pixel of the other eye takes that
// distance (or, in a hole, the farthest one nearby = the background), goes back into the world and picks its colour
// from the drawn eye (or from the other eye's own previous picture).
#include "game/arkhamcity/afw.h"
#include "game/arkhamcity/dlss_bridge.h"
#include "core/gfx/gfx_hud.h"
#include "core/vr/openxr_runtime.h"
#include "core/util/log.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <imgui.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bvr::ac::afw {
namespace {

std::atomic<bool> g_on{false};
std::atomic<int> g_mode{3}, g_win{24}, g_dbg{0}, g_log{0};
std::atomic<int> g_feather{120}; // FEATHER: blend zone at the drawn eye's left / right edge, thousandths of its width

// Cameras per BeginDrawing slot (game thread writes at the ctor, render thread reads at the present).
constexpr uint32_t kSlots = 64;
struct Pass {
    std::atomic<uint32_t> seq{0}; // slot + 1 when complete
    int eye = 0;
    float vpJ[16], vpU[16], dst[16];
    float p22 = 0, p32 = 0;
    int rw = 0, rh = 0;
};
Pass g_pass[kSlots];
std::atomic<uint32_t> g_renderSlot{0};

// GPU objects (render thread only)
int g_built = 0; // 0 not tried, 1 ready, -1 failed
ID3D11Device* g_dev = nullptr;
ID3D11ComputeShader* g_csSplat = nullptr;
ID3D11ComputeShader* g_csGather = nullptr;
ID3D11Buffer* g_cbSplat = nullptr;
ID3D11Buffer* g_cbGather = nullptr;
ID3D11SamplerState* g_lin = nullptr;
// the other eye's distance per render pixel (R32_UINT = float bits; 0xFFFFFFFF = nothing landed here)
ID3D11Texture2D* g_key = nullptr;
ID3D11UnorderedAccessView* g_keyU = nullptr;
ID3D11ShaderResourceView* g_keyS = nullptr;
UINT g_rw = 0, g_rh = 0;
// each eye's last real picture (headset size) + its view x lens without the jitter
ID3D11Texture2D* g_real[2] = {};
ID3D11ShaderResourceView* g_realS[2] = {};
double g_realVP[2][16] = {};
unsigned long long g_realAt[2] = {};
bool g_realOk[2] = {};
ID3D11Texture2D* g_out = nullptr;
ID3D11UnorderedAccessView* g_outU = nullptr;
UINT g_ow = 0, g_oh = 0;
DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
// scene depth view (weak resource pointer, as dlss_bridge)
ID3D11Resource* g_depthRes = nullptr;
ID3D11ShaderResourceView* g_depthSrv = nullptr;
// hole counts (every 16th pixel): [0] sampled, [1] behind objects, [2] outer edge, [3] from the previous picture
ID3D11Buffer* g_stats = nullptr;
ID3D11UnorderedAccessView* g_statsU = nullptr;
ID3D11Buffer* g_statsRb[3] = {};
int g_statsI = 0;
bool g_statsPend[3] = {};
float g_holePct = 0, g_edgePct = 0, g_prevPct = 0;
struct Tq { ID3D11Query* dj = nullptr; ID3D11Query* a = nullptr; ID3D11Query* b = nullptr; bool pending = false; } g_tq[4];
int g_tqI = 0;
double g_tqSum = 0;
long g_tqN = 0;
float g_ms = 0;
unsigned long long g_n = 0; // warps done (this module's frame clock)

std::atomic<uint32_t> g_warps{0}, g_fails{0}, g_noPass{0}, g_noDepth{0}, g_noPrev{0};
std::atomic<uint32_t> g_drawn[2] = {};

void mul4(const double a[16], const double b[16], double o[16]) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double v = 0;
            for (int k = 0; k < 4; ++k) v += a[i * 4 + k] * b[k * 4 + j];
            o[i * 4 + j] = v;
        }
}
bool inv4(const double m[16], double o[16]) { // Gauss-Jordan with partial pivoting
    double a[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r)
            if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-30) return false;
        if (p != c)
            for (int j = 0; j < 8; ++j) { const double t = a[c][j]; a[c][j] = a[p][j]; a[p][j] = t; }
        const double d = a[c][c];
        for (int j = 0; j < 8; ++j) a[c][j] /= d;
        for (int r = 0; r < 4; ++r)
            if (r != c) {
                const double f = a[r][c];
                for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
            }
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) o[i * 4 + j] = a[i][j + 4];
    return true;
}
void to_d(const float* f, double* d) { for (int i = 0; i < 16; ++i) d[i] = f[i]; }

// pass 1 - render size: the drawn eye's depth pixels splatted into the other eye's view (nearest wins)
const char kSplat[] =
    "Texture2D<float> depthT : register(t0); RWTexture2D<uint> keyU : register(u0);"
    "cbuffer C : register(b0) { float4 m0, m1, m2, m3; float4 a3; float4 rs; };" // m = inverse(drawn VP, jittered) x other VP; a3 = inverse's w column
    "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {"
    " if (id.x >= (uint)rs.x || id.y >= (uint)rs.y) return;"
    " float d = depthT.Load(int3(id.xy, 0));"
    " float4 p = float4((id.x + 0.5) * rs.z * 2.0 - 1.0, 1.0 - (id.y + 0.5) * rs.w * 2.0, d, 1.0);"
    " float4 c = p.x * m0 + p.y * m1 + p.z * m2 + m3;"
    " float hw = dot(p, a3);"
    " if (hw < 0) { c = -c; hw = -hw; }"
    " if (c.w <= 1e-12) return;"
    " float z = hw > 1e-20 ? c.w / hw : 1e30;" // the point's distance in the other eye's view (w = view z)
    " float2 n = c.xy / c.w;"
    " int2 q = int2(floor((n.x * 0.5 + 0.5) * rs.x), floor((0.5 - n.y * 0.5) * rs.y));"
    " if (q.x < 0 || q.y < 0 || q.x >= (int)rs.x || q.y >= (int)rs.y) return;"
    " uint o; InterlockedMin(keyU[q], asuint(min(z, 1e30)), o); }";

// pass 2 - headset size: each pixel of the other eye picks its colour
const char kGather[] =
    "Texture2D<uint> keyT : register(t0); Texture2D<float4> srcC : register(t1); Texture2D<float4> prevC : register(t2);"
    "RWTexture2D<float4> outC : register(u0); RWByteAddressBuffer stats : register(u1); SamplerState lin : register(s0);"
    "cbuffer C : register(b0) { float4 g0, g1, g2, g3; float4 h0, h1, h2, h3; float4 os; float4 rs; float4 pz; float4 fl; };"
    // g = inverse(other VP) x drawn VP (no jitter); h = inverse(other VP) x the other eye's last real VP
    // pz = lens depth terms P22, P32, hole search width, mode; fl = previous picture usable, debug, edge blend width (source uv)
    "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {"
    " if (id.x >= (uint)os.x || id.y >= (uint)os.y) return;"
    " float2 uv = (id.xy + 0.5) * os.zw;"
    " float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);"
    " int2 kp = min(int2(uv * rs.xy), int2(rs.xy) - 1);"
    " uint k = keyT.Load(int3(kp, 0));"
    " int hole = 0;"
    " if (k == 0xFFFFFFFF) {"
    "  int win = (int)pz.z; int st = max(1, win / 12); uint best = 0;"
    "  for (int dy = -1; dy <= 1; ++dy) for (int dx = -win; dx <= win; dx += st) {"
    "   int2 q = kp + int2(dx, dy); if (q.x < 0 || q.y < 0 || q.x >= (int)rs.x || q.y >= (int)rs.y) continue;"
    "   uint v = keyT.Load(int3(q, 0)); if (v != 0xFFFFFFFF && v > best) best = v; }" // farthest = the background
    "  if (best != 0) { k = best; hole = 1; } else { k = asuint(1e30); hole = 2; } }"
    " float z = asfloat(k);"
    " float4 p = float4(ndc, pz.x + pz.y / max(z, 1e-6), 1.0);"
    " float4 cs = p.x * g0 + p.y * g1 + p.z * g2 + g3;"
    " float2 su = cs.w > 1e-12 ? float2(cs.x / cs.w * 0.5 + 0.5, 0.5 - cs.y / cs.w * 0.5) : uv;"
    " float4 col = srcC.SampleLevel(lin, su, 0);"
    " int mode = (int)pz.w; bool fromPrev = false;"
    " if (fl.x > 0.5 && (mode == 2 || (mode == 3 && hole != 0))) {"
    "  float4 cp = p.x * h0 + p.y * h1 + p.z * h2 + h3;"
    "  if (cp.w > 1e-12) { float2 pu = float2(cp.x / cp.w * 0.5 + 0.5, 0.5 - cp.y / cp.w * 0.5);"
    "   if (all(pu >= 0.0) && all(pu <= 1.0)) { col = prevC.SampleLevel(lin, pu, 0); fromPrev = true; } } }"
    // FEATHER (AKN, JJ: vertical lines at the very left and right): near the drawn eye's left / right edge, blend over to
    // the other eye's own previous picture, so the two sources meet without a line
    " else if (fl.x > 0.5 && mode == 3 && fl.z > 0.0) {"
    "  float e = saturate(min(su.x, 1.0 - su.x) / fl.z);"
    "  if (e < 1.0) { float4 cp = p.x * h0 + p.y * h1 + p.z * h2 + h3;"
    "   if (cp.w > 1e-12) { float2 pu = float2(cp.x / cp.w * 0.5 + 0.5, 0.5 - cp.y / cp.w * 0.5);"
    "    if (all(pu >= 0.0) && all(pu <= 1.0)) { float w = e * e * (3.0 - 2.0 * e); col = lerp(prevC.SampleLevel(lin, pu, 0), col, w); fromPrev = w < 0.5; } } } }"
    " if (fl.y > 0.5 && hole != 0) col = hole == 1 ? float4(1, 0, 1, 1) : float4(0, 1, 1, 1);"
    " if (fl.y > 1.5 && fromPrev) col = float4(col.r * 0.5, col.g * 0.5 + 0.5, col.b * 0.5, col.a);"
    " outC[id.xy] = col;"
    " if (((id.x | id.y) & 3) == 0) { uint o; stats.InterlockedAdd(0, 1, o);"
    "  if (hole == 1) stats.InterlockedAdd(4, 1, o); if (hole == 2) stats.InterlockedAdd(8, 1, o); if (fromPrev) stats.InterlockedAdd(12, 1, o); } }";

bool build(ID3D11Device* dev) {
    if (g_built) return g_built > 0;
    g_built = -1;
    g_dev = dev;
    auto cs = [&](const char* src, size_t n, const char* name, ID3D11ComputeShader** out) {
        ID3DBlob* b = nullptr;
        ID3DBlob* err = nullptr;
        if (FAILED(D3DCompile(src, n, name, nullptr, nullptr, "main", "cs_5_0", 0, 0, &b, &err)) || !b) {
            BVR_LOG("[afw] %s shader FAILED: %s", name, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            if (err) err->Release();
            return false;
        }
        if (err) err->Release();
        const bool ok = SUCCEEDED(dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, out));
        b->Release();
        return ok;
    };
    if (!cs(kSplat, sizeof(kSplat) - 1, "afw_splat", &g_csSplat) || !cs(kGather, sizeof(kGather) - 1, "afw_gather", &g_csGather))
        return false;
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.ByteWidth = 96;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_cbSplat))) return false;
    bd.ByteWidth = 192;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_cbGather))) return false;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, &g_lin))) return false;
    D3D11_BUFFER_DESC sb{};
    sb.ByteWidth = 16;
    sb.Usage = D3D11_USAGE_DEFAULT;
    sb.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    sb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (FAILED(dev->CreateBuffer(&sb, nullptr, &g_stats))) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = 4;
    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(dev->CreateUnorderedAccessView(g_stats, &ud, &g_statsU))) return false;
    D3D11_BUFFER_DESC rb{};
    rb.ByteWidth = 16;
    rb.Usage = D3D11_USAGE_STAGING;
    rb.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto& r : g_statsRb) dev->CreateBuffer(&rb, nullptr, &r);
    g_built = 1;
    BVR_LOG("[afw] warp passes ready");
    return true;
}

// The headset images are typeless or a plain 8-bit format: our copies share the family, viewed as plain UNORM (bytes
// kept as they are - the warp moves pixels, it does not change their encoding).
void formats_for(DXGI_FORMAT f, DXGI_FORMAT* tex, DXGI_FORMAT* view) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        *tex = DXGI_FORMAT_R8G8B8A8_TYPELESS; *view = DXGI_FORMAT_R8G8B8A8_UNORM; return;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        *tex = DXGI_FORMAT_B8G8R8A8_TYPELESS; *view = DXGI_FORMAT_B8G8R8A8_UNORM; return;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
        *tex = DXGI_FORMAT_R10G10B10A2_TYPELESS; *view = DXGI_FORMAT_R10G10B10A2_UNORM; return;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        *tex = DXGI_FORMAT_R16G16B16A16_TYPELESS; *view = DXGI_FORMAT_R16G16B16A16_FLOAT; return;
    default: *tex = f; *view = f; return;
    }
}

void drop_sized() {
    for (int e = 0; e < 2; ++e) {
        if (g_realS[e]) g_realS[e]->Release();
        if (g_real[e]) g_real[e]->Release();
        g_realS[e] = nullptr;
        g_real[e] = nullptr;
        g_realOk[e] = false;
    }
    if (g_outU) g_outU->Release();
    if (g_out) g_out->Release();
    g_outU = nullptr;
    g_out = nullptr;
    g_ow = g_oh = 0;
    g_fmt = DXGI_FORMAT_UNKNOWN;
}

bool ensure(UINT rw, UINT rh, UINT ow, UINT oh, DXGI_FORMAT fmt) {
    if (rw != g_rw || rh != g_rh || !g_key) {
        if (g_keyU) g_keyU->Release();
        if (g_keyS) g_keyS->Release();
        if (g_key) g_key->Release();
        g_keyU = nullptr; g_keyS = nullptr; g_key = nullptr; g_rw = g_rh = 0;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = rw; d.Height = rh; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R32_UINT;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_key)) || FAILED(g_dev->CreateUnorderedAccessView(g_key, nullptr, &g_keyU)) ||
            FAILED(g_dev->CreateShaderResourceView(g_key, nullptr, &g_keyS))) {
            BVR_LOG("[afw] distance texture %ux%u FAILED", rw, rh);
            return false;
        }
        g_rw = rw; g_rh = rh;
        BVR_LOG("[afw] distance texture %ux%u", rw, rh);
    }
    if (ow != g_ow || oh != g_oh || fmt != g_fmt || !g_out) {
        drop_sized();
        DXGI_FORMAT tf, vf;
        formats_for(fmt, &tf, &vf);
        UINT sup = 0;
        if (FAILED(g_dev->CheckFormatSupport(vf, &sup)) || !(sup & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)) {
            BVR_LOG("[afw] headset image format %d cannot be written by the warp - AFW shows the drawn eye in both", int(fmt));
            return false;
        }
        D3D11_TEXTURE2D_DESC d{};
        d.Width = ow; d.Height = oh; d.MipLevels = 1; d.ArraySize = 1; d.Format = tf; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = vf; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        for (int e = 0; e < 2; ++e)
            if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_real[e])) || FAILED(g_dev->CreateShaderResourceView(g_real[e], &sv, &g_realS[e]))) {
                BVR_LOG("[afw] eye picture copy %ux%u format %d FAILED", ow, oh, int(fmt));
                drop_sized();
                return false;
            }
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = vf; uv.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_out)) || FAILED(g_dev->CreateUnorderedAccessView(g_out, &uv, &g_outU))) {
            BVR_LOG("[afw] warped eye %ux%u format %d FAILED", ow, oh, int(fmt));
            drop_sized();
            return false;
        }
        g_ow = ow; g_oh = oh; g_fmt = fmt;
        BVR_LOG("[afw] eye pictures %ux%u format %d (viewed as %d)", ow, oh, int(fmt), int(vf));
    }
    return true;
}

ID3D11ShaderResourceView* depth_srv(ID3D11Resource* r) {
    if (r == g_depthRes && g_depthSrv) return g_depthSrv;
    if (g_depthSrv) { g_depthSrv->Release(); g_depthSrv = nullptr; }
    g_depthRes = r;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; // scene_depth is R24G8 (gfx_hud on_setrt)
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (FAILED(g_dev->CreateShaderResourceView(r, &sd, &g_depthSrv))) g_depthSrv = nullptr;
    return g_depthSrv;
}

void tq_collect(ID3D11DeviceContext* c) {
    for (auto& q : g_tq) {
        if (!q.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 a = 0, b = 0;
        if (c->GetData(q.dj, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (c->GetData(q.a, &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            c->GetData(q.b, &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        q.pending = false;
        if (!dj.Disjoint && dj.Frequency && b > a) { g_tqSum += double(b - a) * 1000.0 / double(dj.Frequency); ++g_tqN; }
    }
}
Tq* tq_begin(ID3D11DeviceContext* c) {
    tq_collect(c);
    Tq& q = g_tq[g_tqI++ & 3];
    if (q.pending) return nullptr;
    if (!q.dj) {
        D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        g_dev->CreateQuery(&d, &q.dj);
        d.Query = D3D11_QUERY_TIMESTAMP;
        g_dev->CreateQuery(&d, &q.a);
        g_dev->CreateQuery(&d, &q.b);
    }
    if (!q.dj || !q.a || !q.b) return nullptr;
    c->Begin(q.dj);
    c->End(q.a);
    return &q;
}
void stats_collect(ID3D11DeviceContext* c) { // hole counts of a few frames ago (no stall: DO_NOT_WAIT)
    for (int i = 0; i < 3; ++i) {
        if (!g_statsPend[i] || !g_statsRb[i]) continue;
        D3D11_MAPPED_SUBRESOURCE ms{};
        if (c->Map(g_statsRb[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms) != S_OK) continue;
        const UINT* s = static_cast<const UINT*>(ms.pData);
        if (s[0]) { g_holePct = 100.0f * s[1] / s[0]; g_edgePct = 100.0f * s[2] / s[0]; g_prevPct = 100.0f * s[3] / s[0]; }
        c->Unmap(g_statsRb[i], 0);
        g_statsPend[i] = false;
    }
}

// The core's hook: the drawn eye's headset image (drawn) -> the other eye's (otherDst). Render thread.
bool synth(ID3D11DeviceContext* c, int eye, ID3D11Texture2D* drawn, ID3D11Texture2D* otherDst) {
    if (eye < 0 || eye > 1 || !drawn || !otherDst || !g_on.load(std::memory_order_relaxed)) return false;
    const uint32_t slot = g_renderSlot.load(std::memory_order_relaxed);
    const Pass& p = g_pass[slot % kSlots];
    if (p.seq.load(std::memory_order_acquire) != slot + 1 || p.eye != eye || p.rw <= 0 || p.rh <= 0) {
        g_noPass.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    ID3D11Device* dev = nullptr;
    c->GetDevice(&dev);
    if (!dev) return false;
    const bool built = build(dev);
    dev->Release();
    if (!built) return false;
    ID3D11Resource* depth = bvr::gfx_hud::scene_depth();
    ID3D11ShaderResourceView* depthS = depth ? depth_srv(depth) : nullptr;
    if (!depthS) {
        if ((g_noDepth.fetch_add(1, std::memory_order_relaxed) % 300) == 0)
            BVR_LOG("[afw] no scene depth at the eye's present - the drawn eye goes to both");
        return false;
    }
    D3D11_TEXTURE2D_DESC dd{}, pd{};
    {
        ID3D11Texture2D* t = nullptr;
        if (FAILED(depth->QueryInterface(IID_PPV_ARGS(&t))) || !t) return false;
        t->GetDesc(&dd);
        t->Release();
    }
    drawn->GetDesc(&pd);
    // the drawn area = the view's size; the depth target can be a little bigger (padded: its top-left part is used)
    const UINT rw = UINT(p.rw), rh = UINT(p.rh);
    if (rw > dd.Width || rh > dd.Height) { g_fails.fetch_add(1, std::memory_order_relaxed); return false; }
    if (!ensure(rw, rh, pd.Width, pd.Height, pd.Format)) { g_fails.fetch_add(1, std::memory_order_relaxed); return false; }
    const int o = eye ^ 1;
    double vpJ[16], vpU[16], vpD[16], invJ[16], invD[16], S[16], G[16], H[16];
    to_d(p.vpJ, vpJ); to_d(p.vpU, vpU); to_d(p.dst, vpD);
    if (!inv4(vpJ, invJ) || !inv4(vpD, invD)) { g_fails.fetch_add(1, std::memory_order_relaxed); return false; }
    mul4(invJ, vpD, S);
    mul4(invD, vpU, G);
    const bool prevOk = g_realOk[o] && g_n - g_realAt[o] <= 2; // the other eye was drawn on the last frame
    if (prevOk) mul4(invD, g_realVP[o], H);
    else { memcpy(H, G, sizeof(H)); g_noPrev.fetch_add(1, std::memory_order_relaxed); }
    c->CopyResource(g_real[eye], drawn);
    float cs[24], cg[48];
    for (int i = 0; i < 16; ++i) cs[i] = float(S[i]);
    for (int i = 0; i < 4; ++i) cs[16 + i] = float(invJ[i * 4 + 3]); // the inverse's w column
    cs[20] = float(rw); cs[21] = float(rh); cs[22] = 1.0f / rw; cs[23] = 1.0f / rh;
    for (int i = 0; i < 16; ++i) { cg[i] = float(G[i]); cg[16 + i] = float(H[i]); }
    cg[32] = float(pd.Width); cg[33] = float(pd.Height); cg[34] = 1.0f / pd.Width; cg[35] = 1.0f / pd.Height;
    cg[36] = float(rw); cg[37] = float(rh); cg[38] = 1.0f / rw; cg[39] = 1.0f / rh;
    cg[40] = p.p22; cg[41] = p.p32; cg[42] = float(g_win.load()); cg[43] = float(g_mode.load());
    cg[44] = prevOk ? 1.0f : 0.0f; cg[45] = float(g_dbg.load()); cg[46] = float(g_feather.load()) / 1000.0f; cg[47] = 0;

    // No private context state on this device (DLSS_PLAN D1a): keep the game's compute bindings by hand.
    ID3D11ComputeShader* oCs = nullptr;
    ID3D11ShaderResourceView* oSrv[3] = {};
    ID3D11UnorderedAccessView* oUav[2] = {};
    ID3D11Buffer* oCb = nullptr;
    ID3D11SamplerState* oSmp = nullptr;
    c->CSGetShader(&oCs, nullptr, nullptr);
    c->CSGetShaderResources(0, 3, oSrv);
    c->CSGetUnorderedAccessViews(0, 2, oUav);
    c->CSGetConstantBuffers(0, 1, &oCb);
    c->CSGetSamplers(0, 1, &oSmp);

    c->UpdateSubresource(g_cbSplat, 0, nullptr, cs, 0, 0);
    c->UpdateSubresource(g_cbGather, 0, nullptr, cg, 0, 0);
    Tq* tq = tq_begin(c);
    const UINT clr[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    c->ClearUnorderedAccessViewUint(g_keyU, clr);
    const bool statNow = (g_n % 30) == 0;
    if (statNow) { const UINT z[4] = {}; c->ClearUnorderedAccessViewUint(g_statsU, z); }
    ID3D11UnorderedAccessView* nu[2] = {};
    ID3D11ShaderResourceView* ns[3] = {};
    c->CSSetShaderResources(0, 3, ns);
    c->CSSetShader(g_csSplat, nullptr, 0);
    c->CSSetConstantBuffers(0, 1, &g_cbSplat);
    c->CSSetShaderResources(0, 1, &depthS);
    c->CSSetUnorderedAccessViews(0, 1, &g_keyU, nullptr);
    c->Dispatch((rw + 7) / 8, (rh + 7) / 8, 1);
    c->CSSetUnorderedAccessViews(0, 2, nu, nullptr);
    c->CSSetShaderResources(0, 3, ns);
    ID3D11ShaderResourceView* gs[3] = {g_keyS, g_realS[eye], g_realS[o]};
    ID3D11UnorderedAccessView* gu[2] = {g_outU, g_statsU};
    c->CSSetShader(g_csGather, nullptr, 0);
    c->CSSetConstantBuffers(0, 1, &g_cbGather);
    c->CSSetShaderResources(0, 3, gs);
    c->CSSetUnorderedAccessViews(0, 2, gu, nullptr);
    c->CSSetSamplers(0, 1, &g_lin);
    c->Dispatch((pd.Width + 7) / 8, (pd.Height + 7) / 8, 1);
    c->CSSetUnorderedAccessViews(0, 2, nu, nullptr);
    c->CSSetShaderResources(0, 3, ns);
    if (tq) { c->End(tq->b); c->End(tq->dj); tq->pending = true; }
    if (statNow && g_statsRb[g_statsI] && !g_statsPend[g_statsI]) {
        c->CopyResource(g_statsRb[g_statsI], g_stats);
        g_statsPend[g_statsI] = true;
        g_statsI = (g_statsI + 1) % 3;
    }
    stats_collect(c);

    c->CSSetShader(oCs, nullptr, 0);
    c->CSSetShaderResources(0, 3, oSrv);
    c->CSSetUnorderedAccessViews(0, 2, oUav, nullptr);
    c->CSSetConstantBuffers(0, 1, &oCb);
    c->CSSetSamplers(0, 1, &oSmp);
    if (oCs) oCs->Release();
    for (auto* s : oSrv) if (s) s->Release();
    for (auto* u : oUav) if (u) u->Release();
    if (oCb) oCb->Release();
    if (oSmp) oSmp->Release();

    c->CopyResource(otherDst, g_out);
    memcpy(g_realVP[eye], vpU, sizeof(vpU));
    g_realAt[eye] = g_n;
    g_realOk[eye] = true;
    if (g_log.load(std::memory_order_relaxed) > 0) {
        g_log.fetch_sub(1, std::memory_order_relaxed);
        BVR_LOG("[afw] frame %llu: drew eye %c, warped into eye %c, previous picture %s, depth %ux%u (target %ux%u) -> %ux%u",
                g_n, eye ? 'R' : 'L', o ? 'R' : 'L', prevOk ? "used" : "not usable", rw, rh, dd.Width, dd.Height, pd.Width, pd.Height);
    }
    ++g_n;
    g_warps.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void reset_history() { g_realOk[0] = g_realOk[1] = false; }

} // namespace

void install() { bvr::vr::set_eye_synth_hook(&synth); }
bool on() { return g_on.load(std::memory_order_relaxed); }
int mode() { return g_mode.load(std::memory_order_relaxed); }
void set_on(bool v) {
    if (g_on.exchange(v) == v) return;
    reset_history();
    dlss_bridge::command("reset"); // each eye's DLSS history restarts in the new rhythm
    BVR_LOG("[afw] %s (warp mode %d)", v ? "ON - one eye drawn per frame, the other warped" : "off - both eyes drawn every frame",
            g_mode.load());
}

void set_pass(uint32_t slot, int drawnEye, const float vpJ[16], const float vpU[16], const float dstVp[16], float dP22,
              float dP32, int renderW, int renderH) {
    Pass& p = g_pass[slot % kSlots];
    p.seq.store(0, std::memory_order_relaxed);
    p.eye = drawnEye;
    memcpy(p.vpJ, vpJ, sizeof(p.vpJ));
    memcpy(p.vpU, vpU, sizeof(p.vpU));
    memcpy(p.dst, dstVp, sizeof(p.dst));
    p.p22 = dP22;
    p.p32 = dP32;
    p.rw = renderW;
    p.rh = renderH;
    p.seq.store(slot + 1, std::memory_order_release);
}
void set_render_slot(uint32_t slot) { g_renderSlot.store(slot, std::memory_order_relaxed); }
void note_drawn(int eye) { if (eye == 0 || eye == 1) g_drawn[eye].fetch_add(1, std::memory_order_relaxed); }

bool command(const char* a) {
    // "afw on|off" | "afw mode N" | "afw win N" | "afw dbg N" | "afw feather N" | "afw log N" | "afw status"
    auto starts = [&](const char* w) { return _strnicmp(a, w, strlen(w)) == 0; };
    if (starts("on")) set_on(true);
    else if (starts("off")) set_on(false);
    else if (starts("mode ")) { const int m = atoi(a + 5); g_mode.store(m >= 1 && m <= 3 ? m : 3); BVR_LOG("[afw] warp mode %d (1 this frame, 2 previous frame, 3 combined)", g_mode.load()); }
    else if (starts("win ")) { const int v = atoi(a + 4); g_win.store(v < 1 ? 1 : v > 256 ? 256 : v); BVR_LOG("[afw] hole search %d render pixels", g_win.load()); }
    else if (starts("dbg ")) { g_dbg.store(atoi(a + 4)); BVR_LOG("[afw] debug %d (1 holes painted: magenta behind objects, cyan outer edge; 2 also tints what came from the previous picture)", g_dbg.load()); }
    else if (starts("feather ")) { const int v = atoi(a + 8); g_feather.store(v < 0 ? 0 : v > 400 ? 400 : v); BVR_LOG("[afw] edge blend %d / 1000 of the eye width", g_feather.load()); }
    else if (starts("log ")) { g_log.store(atoi(a + 4)); BVR_LOG("[afw] log the next %d warps", g_log.load()); }
    else if (starts("status")) { char s[400]; status_line(s, sizeof(s)); BVR_LOG("[afw] %s", s); }
    else return false;
    return true;
}

void status_line(char* out, unsigned n) {
    static ULONGLONG s_last = 0;
    static uint32_t s_d[2] = {}, s_w = 0, s_pw = 0, s_pf = 0;
    const ULONGLONG now = GetTickCount64();
    const float sec = s_last ? float(now - s_last) / 1000.0f : 0.0f;
    s_last = now;
    const uint32_t d0 = g_drawn[0].load(), d1 = g_drawn[1].load(), w = g_warps.load();
    uint32_t pw = 0, pf = 0;
    bvr::vr::afw_counts(&pw, &pf);
    auto rate = [&](uint32_t cur, uint32_t& last) { const float r = sec > 0 ? float(cur - last) / sec : 0.0f; last = cur; return r; };
    const float rL = rate(d0, s_d[0]), rR = rate(d1, s_d[1]), rW = rate(w, s_w), rP = rate(pw, s_pw), rF = rate(pf, s_pf);
    if (g_tqN) { g_ms = float(g_tqSum / g_tqN); g_tqSum = 0; g_tqN = 0; } // render thread writes these; a torn read only skews one line
    snprintf(out, n,
             "AFW %s mode %d | frames/s drawing left %.0f right %.0f | warps/s %.0f, pairs/s warped %.0f, drawn eye in both %.0f "
             "| warp %.2f ms on the graphics card | holes behind objects %.1f %%, outer edge %.1f %%, from the previous picture %.1f %% "
             "| no camera %u, no depth %u, failed %u, no previous picture %u",
             g_on.load() ? "on" : "off", g_mode.load(), rL, rR, rW, rP, rF, g_ms, g_holePct, g_edgePct, g_prevPct,
             g_noPass.load(), g_noDepth.load(), g_fails.load(), g_noPrev.load());
}

void draw_ui() {
    bool v = g_on.load();
    ImGui::TextUnformatted("3D rendering");
    if (ImGui::RadioButton("both eyes drawn every frame (native)", !v)) set_on(false);
    if (ImGui::RadioButton("one eye drawn per frame, the other warped (AFW)", v)) set_on(true);
    if (g_on.load()) {
        int m = g_mode.load();
        ImGui::TextUnformatted("warp from:");
        ImGui::SameLine();
        if (ImGui::RadioButton("this frame", m == 1)) g_mode.store(1);
        ImGui::SameLine();
        if (ImGui::RadioButton("previous frame", m == 2)) g_mode.store(2);
        ImGui::SameLine();
        if (ImGui::RadioButton("both (best)", m == 3)) g_mode.store(3);
        uint32_t pw = 0, pf = 0;
        bvr::vr::afw_counts(&pw, &pf);
        ImGui::Text("pairs warped %u, drawn eye in both %u | warp %.2f ms | holes %.1f %% + edge %.1f %%", pw, pf, g_ms,
                    g_holePct, g_edgePct);
    }
}

void settings_write(std::string& out) {
    char b[48];
    snprintf(b, sizeof(b), "afw=%d\nafw_mode=%d\n", g_on.load() ? 1 : 0, g_mode.load());
    out += b;
}
bool settings_read(const char* key, const char* val) {
    if (!strcmp(key, "afw")) { g_on.store(atoi(val) != 0); return true; }
    if (!strcmp(key, "afw_mode")) { const int m = atoi(val); g_mode.store(m >= 1 && m <= 3 ? m : 3); return true; }
    return false;
}

} // namespace bvr::ac::afw
