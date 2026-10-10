#include "game/arkhamcity/glow.h"
#include "core/ui/overlay.h"

#include "core/gfx/frame_inspector.h"
#include "core/hooks/d3d11_hook.h"
#include "core/util/log.h"

#include <d3d11.h>
#include <dxgi.h>
#include <MinHook.h>
#include <windows.h>
#include <d3dcompiler.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace bvr::ac::glow {
namespace {

// --- settings (saved) ----------------------------------------------------------------------------------------------
// 0 = the game's glow, 1 = no glow, 2 = ours (game's removed), 3 = ours on top of the game's (comparison)
std::atomic<int> g_mode{0}; // the game's until ours is tuned (depth of field, not bloom, was the blocky "glow")
std::atomic<float> g_intensity{0.08f}; // added scene light per unit of bloom
std::atomic<float> g_threshold{1.0f};  // scene brightness where glow starts (HDR units)
std::atomic<float> g_knee{0.5f};       // soft start below the threshold
std::atomic<float> g_radius{1.0f};     // upsample tent radius (texels)
std::atomic<int> g_levels{6};
// NODOF (JJ 2026-10-08): the "glow" around distant lights is the game's depth of field - the combine pass blends each
// pixel toward a quarter-size blurred copy by how far it is from the focus distance (ps_16 disassembly, notes in
// BATMAN_CITY_PLAN). Default: the combine pass runs our copy of its shader without that blend.
std::atomic<bool> g_dofOff{true};
std::atomic<float> g_limit{1.0f}; // highlight limit start (1 = off); applied in the combine pass copy
std::atomic<float> g_shaftGlow{1.0f};  // GAMEGLOW: strength of the light-shaft buffer (the glow around bright lights)
std::atomic<bool> g_shaftSmooth{true}; // GAMEGLOW: smooth enlargement of that quarter-size buffer
ID3D11Buffer* g_limCb = nullptr;
ID3D11Buffer* g_origCb5 = nullptr;
ID3D11ShaderResourceView* g_origSrv5 = nullptr;

// --- counters ------------------------------------------------------------------------------------------------------
std::atomic<uint32_t> g_found{0}, g_done{0}, g_failed{0};
std::atomic<uint32_t> g_sceneW{0}, g_sceneH{0};

constexpr int kMaxLevels = 7;

const char kHlsl[] = R"(
cbuffer P : register(b0) { float4 texel; float4 prm; float4 clampUV; float4 comp; };
// texel.xy = 1 / source size; prm = (threshold, knee, intensity, radius); clampUV.xy = largest valid source uv;
// comp.xy = 1 / bloom source size (composite), comp.z = 1 / levels
Texture2D S0 : register(t0);
Texture2D S1 : register(t1);
SamplerState L : register(s0);
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V vs_main(uint id : SV_VertexID) {
    V o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.uv = t;
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float3 tap(float2 uv) {
    float3 c = S0.SampleLevel(L, min(uv, clampUV.xy), 0).rgb;
    return (c == c) ? min(c, 64000.0) : 0; // NaN / inf guard
}
float3 down13(float2 uv, bool karis) {
    float2 t = texel.xy;
    float3 a = tap(uv + t * float2(-2, -2)), b = tap(uv + t * float2(0, -2)), c = tap(uv + t * float2(2, -2));
    float3 d = tap(uv + t * float2(-2, 0)), e = tap(uv), f = tap(uv + t * float2(2, 0));
    float3 g = tap(uv + t * float2(-2, 2)), h = tap(uv + t * float2(0, 2)), i = tap(uv + t * float2(2, 2));
    float3 j = tap(uv + t * float2(-1, -1)), k = tap(uv + t * float2(1, -1));
    float3 l = tap(uv + t * float2(-1, 1)), m = tap(uv + t * float2(1, 1));
    float3 g0 = (j + k + l + m) * 0.25, g1 = (a + b + d + e) * 0.25, g2 = (b + c + e + f) * 0.25;
    float3 g3 = (d + e + g + h) * 0.25, g4 = (e + f + h + i) * 0.25;
    if (!karis) return g0 * 0.5 + (g1 + g2 + g3 + g4) * 0.125;
    // Karis average: each group weighted by 1 / (1 + brightness), so one tiny bright pixel cannot flash the glow.
    float w0 = 0.5 / (1 + luma(g0)), w1 = 0.125 / (1 + luma(g1)), w2 = 0.125 / (1 + luma(g2));
    float w3 = 0.125 / (1 + luma(g3)), w4 = 0.125 / (1 + luma(g4));
    return (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / (w0 + w1 + w2 + w3 + w4);
}
float4 ps_prefilter(V i) : SV_Target {
    float3 c = down13(i.uv, true);
    float br = max(c.r, max(c.g, c.b));
    float kn = max(prm.y, 1e-4);
    float rq = clamp(br - prm.x + kn, 0, 2 * kn);
    rq = rq * rq / (4 * kn);
    float w = max(rq, br - prm.x) / max(br, 1e-4);
    return float4(c * w, 1);
}
float4 ps_down(V i) : SV_Target { return float4(down13(i.uv, false), 1); }
float3 tent(Texture2D T, float2 uv, float2 t) {
    float3 s = T.SampleLevel(L, uv, 0).rgb * 4;
    s += (T.SampleLevel(L, uv + float2(-t.x, 0), 0).rgb + T.SampleLevel(L, uv + float2(t.x, 0), 0).rgb +
          T.SampleLevel(L, uv + float2(0, -t.y), 0).rgb + T.SampleLevel(L, uv + float2(0, t.y), 0).rgb) * 2;
    s += T.SampleLevel(L, uv - t, 0).rgb + T.SampleLevel(L, uv + t, 0).rgb +
         T.SampleLevel(L, uv + float2(t.x, -t.y), 0).rgb + T.SampleLevel(L, uv + float2(-t.x, t.y), 0).rgb;
    return s / 16;
}
float4 ps_up(V i) : SV_Target { return float4(tent(S0, i.uv, texel.xy * prm.w), 1); } // added onto the level below
float4 ps_composite(V i) : SV_Target {
    float4 scene = S0.Load(int3(i.p.xy, 0));
    float3 bloom = tent(S1, i.uv, comp.xy * prm.w) * comp.z;
    return float4(scene.rgb + bloom * prm.z, scene.a);
}
float4 ps_noglow(V i) : SV_Target { return float4(0, 0, 0, S0.Load(int3(i.p.xy, 0)).a); }
)";

// The game's combine pass (GlobalShaderCache ps recorded in-game, 1704 bytes, disassembled 2026-10-08) minus its depth of
// field: scene (t0) + light shafts (t2), gamma 1/2.2, clamp, 16x16x16 colour-grading LUT unrolled in 256x16 (t3), luma
// in alpha for FXAA. Same input signature as the original (TEXCOORD0.zw scene uv, TEXCOORD1.xy shaft / LUT uv).
const char kHlslNoDof[] = R"(
Texture2D T0 : register(t0);
Texture2D T2 : register(t2);
Texture2D T3 : register(t3);
SamplerState S0 : register(s0);
SamplerState S2 : register(s2);
SamplerState S3 : register(s3);
cbuffer Lim : register(b5) { float4 lim; }; // lim.x = highlight limit start (1 = off), lim.y = glow sprite strength
Texture2D TA : register(t5);                  // the game's glow sprites, drawn on their own (SPRITEDIM)
float4 main(float4 uv0 : TEXCOORD0, float2 uv1 : TEXCOORD1) : SV_Target {
    // GAMEGLOW (JJ 2026-10-09: no scene draw held the glow): the light-shaft buffer t2 (quarter size) carries the glow
    // around bright lights too. lim.z = its strength; lim.w > 0.5 = smooth (bicubic B-spline) enlargement instead of
    // the game's bilinear one, which shows the quarter-size texels as blocks.
    float3 sh;
    if (lim.w > 0.5) {
        float2 sz;
        T2.GetDimensions(sz.x, sz.y);
        float2 p = uv1 * sz - 0.5, f = frac(p), b = floor(p);
        float2 w0 = (1 - f) * (1 - f) * (1 - f) / 6, w1 = (4 - 6 * f * f + 3 * f * f * f) / 6;
        float2 w2 = (1 + 3 * f + 3 * f * f - 3 * f * f * f) / 6, w3 = f * f * f / 6;
        float2 g0 = w0 + w1, g1 = w2 + w3, h0 = (b - 0.5 + w1 / g0) / sz, h1 = (b + 1.5 + w3 / g1) / sz;
        sh = g0.y * (g0.x * T2.SampleLevel(S2, float2(h0.x, h0.y), 0).rgb + g1.x * T2.SampleLevel(S2, float2(h1.x, h0.y), 0).rgb) +
             g1.y * (g0.x * T2.SampleLevel(S2, float2(h0.x, h1.y), 0).rgb + g1.x * T2.SampleLevel(S2, float2(h1.x, h1.y), 0).rgb);
    } else {
        sh = T2.Sample(S2, uv1).rgb;
    }
    float3 c = T0.Sample(S0, uv0.zw).rgb + sh * lim.z + TA.Sample(S0, uv0.zw).rgb * lim.y;
    // HIGHLIGHT LIMIT (JJ: spotlights / neon too intense): above lim.x the brightest channel rolls off smoothly toward
    // 1 (white) instead of clipping, hue kept, so blown-out halos shrink and keep their colour.
    float m = max(c.r, max(c.g, c.b));
    if (lim.x < 0.999 && m > lim.x) {
        float k = 1 - lim.x;
        c *= (lim.x + k * (1 - exp(-(m - lim.x) / k))) / m;
    }
    c = min(exp(log(max(c, 0)) * 0.454545), 1);
    float bi = floor(c.b * 14.9999);
    float2 a = float2(bi * 0.0625 + c.r * 0.058594 + 0.001953, c.g * 0.9375 + 0.03125);
    float3 l0 = T3.Sample(S3, a).rgb;
    float3 l1 = T3.Sample(S3, a + float2(0.0625, 0)).rgb;
    float3 o = l0 + (c.b * 15.0 - bi) * (l1 - l0);
    return float4(o, dot(o, float3(0.299, 0.587, 0.114)));
}
)";

struct Cb {
    float texel[4];
    float prm[4];
    float clampUV[4];
    float comp[4];
};

// Pipeline objects (render thread).
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_psPre = nullptr;
ID3D11PixelShader* g_psDown = nullptr;
ID3D11PixelShader* g_psUp = nullptr;
ID3D11PixelShader* g_psComp = nullptr;
ID3D11PixelShader* g_psNo = nullptr;
ID3D11PixelShader* g_psNoDof = nullptr; // the game's combine pass without its depth of field (NODOF)
ID3D11Buffer* g_cb = nullptr;
ID3D11SamplerState* g_samp = nullptr;
ID3D11BlendState* g_add = nullptr;
ID3D11BlendState* g_opaque = nullptr;
ID3D11RasterizerState* g_rs = nullptr;
ID3D11DepthStencilState* g_ds = nullptr;
bool g_pipeFailed = false;

struct Tex {
    ID3D11Texture2D* t = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    UINT w = 0, h = 0;
    void release() {
        if (srv) srv->Release();
        if (rtv) rtv->Release();
        if (t) t->Release();
        srv = nullptr; rtv = nullptr; t = nullptr; w = h = 0;
    }
};
Tex g_level[kMaxLevels];
Tex g_sceneOut;  // scene + our glow (the combine pass reads this instead of the game's scene)
Tex g_noGlow;    // the game's glow input with its colour removed (alpha kept)

// The game's inputs while our replacements are bound (put back after the draw).
ID3D11ShaderResourceView* g_origSrv[2] = {};
ID3D11PixelShader* g_origPs = nullptr;
bool g_swapped = false;

bool ensure_pipeline(ID3D11Device* dev) {
    if (g_vs) return true;
    if (g_pipeFailed) return false;
    auto compile = [&](const char* entry, const char* target, ID3DBlob** out) {
        ID3DBlob* err = nullptr;
        const HRESULT hr = D3DCompile(kHlsl, sizeof(kHlsl) - 1, "bc_glow", nullptr, nullptr, entry, target, 0, 0, out, &err);
        if (err) {
            BVR_LOG("[glow] shader %s: %s", entry, static_cast<const char*>(err->GetBufferPointer()));
            err->Release();
        }
        return SUCCEEDED(hr) && *out;
    };
    bool ok = true;
    ID3DBlob* b = nullptr;
    if ((ok = compile("vs_main", "vs_4_0", &b))) ok = SUCCEEDED(dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_vs));
    if (b) { b->Release(); b = nullptr; }
    struct { const char* e; ID3D11PixelShader** p; } ps[] = {
        {"ps_prefilter", &g_psPre}, {"ps_down", &g_psDown}, {"ps_up", &g_psUp}, {"ps_composite", &g_psComp}, {"ps_noglow", &g_psNo}};
    for (auto& s : ps) {
        if (!ok) break;
        if ((ok = compile(s.e, "ps_4_0", &b))) ok = SUCCEEDED(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, s.p));
        if (b) { b->Release(); b = nullptr; }
    }
    if (ok) {
        ID3DBlob* err = nullptr;
        if (SUCCEEDED(D3DCompile(kHlslNoDof, sizeof(kHlslNoDof) - 1, "bc_nodof", nullptr, nullptr, "main", "ps_5_0", 0, 0, &b,
                                 &err)) && b)
            ok = SUCCEEDED(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psNoDof));
        else
            ok = false;
        if (err) {
            BVR_LOG("[glow] no-depth-of-field shader: %s", static_cast<const char*>(err->GetBufferPointer()));
            err->Release();
        }
        if (b) { b->Release(); b = nullptr; }
    }
    if (ok) {
        D3D11_BUFFER_DESC cd{sizeof(Cb), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        D3D11_BLEND_DESC ad{};
        ad.RenderTarget[0].BlendEnable = TRUE;
        ad.RenderTarget[0].SrcBlend = ad.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        ad.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        ad.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        ad.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        ad.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        ad.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        D3D11_BLEND_DESC od{};
        od.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        D3D11_DEPTH_STENCIL_DESC dd{};
        ok = SUCCEEDED(dev->CreateBuffer(&cd, nullptr, &g_cb)) && SUCCEEDED(dev->CreateSamplerState(&sd, &g_samp)) &&
             SUCCEEDED(dev->CreateBlendState(&ad, &g_add)) && SUCCEEDED(dev->CreateBlendState(&od, &g_opaque)) &&
             SUCCEEDED(dev->CreateRasterizerState(&rd, &g_rs)) && SUCCEEDED(dev->CreateDepthStencilState(&dd, &g_ds));
    }
    if (!ok) {
        g_pipeFailed = true;
        BVR_LOG("[glow] pipeline setup FAILED - the game's glow stays");
    } else {
        BVR_LOG("[glow] pipeline ready");
    }
    return ok;
}

bool ensure_tex(ID3D11Device* dev, Tex& t, UINT w, UINT h, DXGI_FORMAT f) {
    if (t.t && t.w == w && t.h == h) return true;
    t.release();
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &t.t)) || FAILED(dev->CreateRenderTargetView(t.t, nullptr, &t.rtv)) ||
        FAILED(dev->CreateShaderResourceView(t.t, nullptr, &t.srv))) {
        t.release();
        return false;
    }
    t.w = w;
    t.h = h;
    return true;
}

bool tex_desc(ID3D11ShaderResourceView* v, D3D11_TEXTURE2D_DESC* d) {
    if (!v) return false;
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    ID3D11Texture2D* t = nullptr;
    const bool ok = r && SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&t))) && t;
    if (ok) {
        t->GetDesc(d);
        t->Release();
    }
    if (r) r->Release();
    return ok;
}

// The game's state we touch, saved once around all our passes.
struct Saved {
    ID3D11RenderTargetView* rtv[1] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT bf[4] = {};
    UINT mask = 0;
    ID3D11DepthStencilState* depth = nullptr;
    UINT ref = 0;
    ID3D11RasterizerState* rs = nullptr;
    UINT nvp = 1;
    D3D11_VIEWPORT vp{};
    ID3D11VertexShader* vs = nullptr;
    ID3D11HullShader* hs = nullptr;
    ID3D11DomainShader* ds = nullptr;
    ID3D11GeometryShader* gs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* pcb = nullptr;
    ID3D11ShaderResourceView* srv[2] = {};
    ID3D11SamplerState* samp = nullptr;
    ID3D11InputLayout* il = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topo{};
    void save(ID3D11DeviceContext* c) {
        c->OMGetRenderTargets(1, rtv, &dsv);
        c->OMGetBlendState(&blend, bf, &mask);
        c->OMGetDepthStencilState(&depth, &ref);
        c->RSGetState(&rs);
        c->RSGetViewports(&nvp, &vp);
        c->VSGetShader(&vs, nullptr, nullptr);
        c->HSGetShader(&hs, nullptr, nullptr);
        c->DSGetShader(&ds, nullptr, nullptr);
        c->GSGetShader(&gs, nullptr, nullptr);
        c->PSGetShader(&ps, nullptr, nullptr);
        c->PSGetConstantBuffers(0, 1, &pcb);
        c->PSGetShaderResources(0, 2, srv);
        c->PSGetSamplers(0, 1, &samp);
        c->IAGetInputLayout(&il);
        c->IAGetPrimitiveTopology(&topo);
    }
    void restore(ID3D11DeviceContext* c) {
        c->OMSetRenderTargets(1, rtv, dsv);
        c->OMSetBlendState(blend, bf, mask);
        c->OMSetDepthStencilState(depth, ref);
        c->RSSetState(rs);
        if (nvp) c->RSSetViewports(1, &vp);
        c->VSSetShader(vs, nullptr, 0);
        c->HSSetShader(hs, nullptr, 0);
        c->DSSetShader(ds, nullptr, 0);
        c->GSSetShader(gs, nullptr, 0);
        c->PSSetShader(ps, nullptr, 0);
        c->PSSetConstantBuffers(0, 1, &pcb);
        c->PSSetShaderResources(0, 2, srv);
        c->PSSetSamplers(0, 1, &samp);
        c->IASetInputLayout(il);
        c->IASetPrimitiveTopology(topo);
        IUnknown* all[] = {rtv[0], dsv, blend, depth, rs, vs, hs, ds, gs, ps, pcb, srv[0], srv[1], samp, il};
        for (IUnknown* u : all)
            if (u) u->Release();
    }
};

void pass(ID3D11DeviceContext* c, Tex& dst, ID3D11PixelShader* ps, ID3D11ShaderResourceView* s0,
          ID3D11ShaderResourceView* s1, bool additive, const Cb& cb) {
    ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
    c->PSSetShaderResources(0, 2, none); // a level is read in one pass and written in the next
    c->OMSetRenderTargets(1, &dst.rtv, nullptr);
    D3D11_VIEWPORT vp{0, 0, float(dst.w), float(dst.h), 0, 1};
    c->RSSetViewports(1, &vp);
    c->OMSetBlendState(additive ? g_add : g_opaque, nullptr, 0xFFFFFFFF);
    c->UpdateSubresource(g_cb, 0, nullptr, &cb, 0, 0);
    c->PSSetShader(ps, nullptr, 0);
    ID3D11ShaderResourceView* in[2] = {s0, s1};
    c->PSSetShaderResources(0, 2, in);
    c->Draw(3, 0);
}

// The combine pass: scene HDR (full size, fp16) at t0, the game's bloom (quarter, 16-bit unorm) at t1, light shafts
// (quarter, fp16) at t2, exposure (256x16) at t3, PS constants 320 bytes (frame dump 2026-10-08, event 1838).
bool is_combine(ID3D11DeviceContext* ctx, D3D11_TEXTURE2D_DESC* scene, D3D11_TEXTURE2D_DESC* bloom) {
    ID3D11ShaderResourceView* v[4] = {};
    ctx->PSGetShaderResources(0, 4, v);
    D3D11_TEXTURE2D_DESC d2{}, d3{};
    const bool ok = tex_desc(v[0], scene) && tex_desc(v[1], bloom) && tex_desc(v[2], &d2) && tex_desc(v[3], &d3) &&
                    scene->Format == DXGI_FORMAT_R16G16B16A16_FLOAT && scene->Width >= 800 &&
                    bloom->Format == DXGI_FORMAT_R16G16B16A16_UNORM && bloom->Width * 4 >= scene->Width - 16 &&
                    bloom->Width * 4 <= scene->Width + 16 && d3.Width == 256 && d3.Height == 16;
    for (auto* x : v)
        if (x) x->Release();
    return ok;
}

// --- SHADERDUMP: the pixel shaders of the post passes, saved once for offline disassembly -----------------------------
// The engine creates its shaders before our first-Present hooks, so D3D11CreateDevice(AndSwapChain) are hooked early and
// the returned device's CreatePixelShader keeps a copy of every shader's bytecode.
using CreatePsFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
CreatePsFn g_createPsOrig = nullptr;
SRWLOCK g_psLock = SRWLOCK_INIT;
std::unordered_map<ID3D11PixelShader*, std::string> g_psCode;
HRESULT STDMETHODCALLTYPE create_ps_detour(ID3D11Device* d, const void* code, SIZE_T len, ID3D11ClassLinkage* cl,
                                           ID3D11PixelShader** out) {
    const HRESULT hr = g_createPsOrig(d, code, len, cl, out);
    if (SUCCEEDED(hr) && out && *out && code && len < (1u << 20)) {
        AcquireSRWLockExclusive(&g_psLock);
        g_psCode[*out].assign(static_cast<const char*>(code), len);
        ReleaseSRWLockExclusive(&g_psLock);
    }
    return hr;
}
void hook_device(ID3D11Device* dev) {
    if (g_createPsOrig || !dev) return;
    void* fn = (*reinterpret_cast<void***>(dev))[15]; // ID3D11Device::CreatePixelShader
    const MH_STATUS a = MH_CreateHook(fn, reinterpret_cast<void*>(&create_ps_detour), reinterpret_cast<void**>(&g_createPsOrig));
    const MH_STATUS b = a == MH_OK ? MH_EnableHook(fn) : a;
    BVR_LOG("[glow] pixel shader recorder on the game's device: %s", MH_StatusToString(b));
}
using CreateDevFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                     ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using CreateDevSwFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                       const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*,
                                       ID3D11DeviceContext**);
CreateDevFn g_createDevOrig = nullptr;
CreateDevSwFn g_createDevSwOrig = nullptr;
HRESULT WINAPI create_dev_detour(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE s, UINT f, const D3D_FEATURE_LEVEL* fl, UINT n,
                                 UINT v, ID3D11Device** d, D3D_FEATURE_LEVEL* o, ID3D11DeviceContext** c) {
    const HRESULT hr = g_createDevOrig(a, t, s, f, fl, n, v, d, o, c);
    if (SUCCEEDED(hr) && d && *d) hook_device(*d);
    return hr;
}
HRESULT WINAPI create_dev_sw_detour(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE s, UINT f, const D3D_FEATURE_LEVEL* fl,
                                    UINT n, UINT v, const DXGI_SWAP_CHAIN_DESC* sd, IDXGISwapChain** sw, ID3D11Device** d,
                                    D3D_FEATURE_LEVEL* o, ID3D11DeviceContext** c) {
    const HRESULT hr = g_createDevSwOrig(a, t, s, f, fl, n, v, sd, sw, d, o, c);
    if (SUCCEEDED(hr) && d && *d) hook_device(*d);
    return hr;
}

// Render thread: save the bound pixel shader of a full-screen pass once (by shader), with a note of its targets.
std::atomic<int> g_dumped{0};
ID3D11PixelShader* g_dumpedPs[48] = {};
void dump_ps(ID3D11DeviceContext* ctx, const char* what) {
    if (!g_createPsOrig || g_dumped.load() >= 48) return;
    ID3D11PixelShader* ps = nullptr;
    ctx->PSGetShader(&ps, nullptr, nullptr);
    if (!ps) return;
    const int n = g_dumped.load();
    bool seen = false;
    for (int i = 0; i < n; ++i) seen |= g_dumpedPs[i] == ps;
    std::string code;
    if (!seen) {
        AcquireSRWLockShared(&g_psLock);
        auto it = g_psCode.find(ps);
        if (it != g_psCode.end()) code = it->second;
        ReleaseSRWLockShared(&g_psLock);
    }
    if (!seen && !code.empty()) {
        g_dumpedPs[n] = ps;
        g_dumped.store(n + 1);
        wchar_t dir[MAX_PATH], path[MAX_PATH];
        swprintf_s(dir, L"%s\\shaders", bvr::log::data_dir());
        CreateDirectoryW(dir, nullptr);
        swprintf_s(path, L"%s\\ps_%02d.dxbc", dir, n);
        if (FILE* f = _wfopen(path, L"wb")) {
            fwrite(code.data(), 1, code.size(), f);
            fclose(f);
        }
        ID3D11ShaderResourceView* v[4] = {};
        ctx->PSGetShaderResources(0, 4, v);
        ID3D11RenderTargetView* rtv = nullptr;
        ctx->OMGetRenderTargets(1, &rtv, nullptr);
        char srvs[160] = {};
        int len = 0;
        for (auto* x : v) {
            D3D11_TEXTURE2D_DESC d{};
            if (tex_desc(x, &d)) len += snprintf(srvs + len, sizeof(srvs) - len, " %ux%u/%d", d.Width, d.Height, int(d.Format));
            else len += snprintf(srvs + len, sizeof(srvs) - len, " -");
            if (x) x->Release();
        }
        D3D11_TEXTURE2D_DESC rd{};
        if (rtv) {
            ID3D11Resource* r = nullptr;
            rtv->GetResource(&r);
            ID3D11Texture2D* t = nullptr;
            if (r && SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&t))) && t) { t->GetDesc(&rd); t->Release(); }
            if (r) r->Release();
            rtv->Release();
        }
        BVR_LOG("[glow] shader ps_%02d.dxbc (%u bytes) %s: into %ux%u/%d, inputs%s", n, unsigned(code.size()), what, rd.Width,
                rd.Height, int(rd.Format), srvs);
    }
    ps->Release();
}

// --- SPRITEDIM (JJ 2026-10-08: the glow on the perimeter spotlights and some neon signs is too intense) --------------
// Those halos are the game's own additive sprites in the translucent pass (frame dump of JJ's capture: indexed draws on
// the sprite path into the fp16 scene, 256x256 textures), not a post effect. Census: every distinct additive blend /
// texture combination drawn into the scene is logged once. Off: the draw is skipped. Strength < 1: the draw goes into
// a layer of its own (cleared per eye pass, same depth buffer), and our combine-pass copy adds that layer x strength -
// works for every additive blend (source factor ONE or SRC_ALPHA alike).
std::atomic<float> g_spriteDim{1.0f};
std::atomic<bool> g_spritesOn{true};
std::atomic<uint32_t> g_spriteSkipped{0};
std::atomic<uint32_t> g_spriteSeen{0}, g_spriteDimmed{0}, g_spriteOther{0};
Tex g_accum;                        // the glow sprites' own layer (scene size, fp16)
uint64_t g_accumClearedAt = ~0ull;  // present count of the last clear
uint64_t g_accumUsedAt = ~0ull;     // present count of the last sprite drawn into it
ID3D11RenderTargetView* g_redirRtv = nullptr; // the game's target while a sprite draws into the layer
ID3D11DepthStencilView* g_redirDsv = nullptr;
bool g_redirected = false;
// GLOWHUNT (JJ: the "glow sprites" were the volumetric light beams, not the glow): every blended draw into the scene
// is put in a group by blend and textures; the panel hides one group at a time while JJ looks at a glowing sign.
constexpr int kGroups = 64;
uint32_t g_censusKeys[kGroups] = {};
char g_groupDesc[kGroups][112] = {};
std::atomic<uint32_t> g_groupDraws[kGroups];
int g_censusN = 0;
std::atomic<int> g_hideGroup{-1};

bool sprite_before(ID3D11DeviceContext* ctx, unsigned vertexCount) {
    ID3D11BlendState* bs = nullptr;
    FLOAT bf[4];
    UINT mask = 0;
    ctx->OMGetBlendState(&bs, bf, &mask);
    if (!bs) return false;
    D3D11_BLEND_DESC bd{};
    bs->GetDesc(&bd);
    bs->Release();
    const D3D11_RENDER_TARGET_BLEND_DESC& r = bd.RenderTarget[0];
    if (!r.BlendEnable) return false;
    const bool beam = r.DestBlend == D3D11_BLEND_ONE && r.BlendOp == D3D11_BLEND_OP_ADD; // the volumetric light beams
    D3D11_TEXTURE2D_DESC td{};
    {
        ID3D11RenderTargetView* rtv = nullptr;
        ctx->OMGetRenderTargets(1, &rtv, nullptr);
        if (!rtv) return false;
        ID3D11Resource* res = nullptr;
        rtv->GetResource(&res);
        ID3D11Texture2D* t = nullptr;
        if (res && SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&t))) && t) { t->GetDesc(&td); t->Release(); }
        if (res) res->Release();
        rtv->Release();
    }
    if (!(td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && td.Width >= 800)) return false;
    ID3D11ShaderResourceView* v[2] = {};
    ctx->PSGetShaderResources(0, 2, v);
    D3D11_TEXTURE2D_DESC t0{}, t1{};
    tex_desc(v[0], &t0);
    tex_desc(v[1], &t1);
    for (auto* x : v)
        if (x) x->Release();
    const uint32_t key = (uint32_t(r.SrcBlend) << 27) ^ (uint32_t(r.DestBlend) << 22) ^ (t0.Width << 12) ^ t0.Height ^
                         (uint32_t(t0.Format) << 17) ^ (t1.Width << 6) ^ (t1.Height << 2) ^ uint32_t(t1.Format);
    int group = -1;
    for (int i = 0; i < g_censusN; ++i)
        if (g_censusKeys[i] == key) group = i;
    if (group < 0 && g_censusN < kGroups) {
        group = g_censusN;
        snprintf(g_groupDesc[group], sizeof(g_groupDesc[group]), "blend %d/%d, pictures %ux%u (%d) + %ux%u (%d), first %u vertices",
                 int(r.SrcBlend), int(r.DestBlend), t0.Width, t0.Height, int(t0.Format), t1.Width, t1.Height,
                 int(t1.Format), vertexCount);
        g_censusKeys[group] = key;
        g_censusN = group + 1;
        BVR_LOG("[glow] see-through scene group %d: %s", group, g_groupDesc[group]);
    }
    if (group >= 0) {
        g_groupDraws[group].fetch_add(1, std::memory_order_relaxed);
        if (group == g_hideGroup.load(std::memory_order_relaxed)) return true; // GLOWHUNT: hidden while JJ looks
    }
    if (!beam) return false;
    g_spriteSeen.fetch_add(1, std::memory_order_relaxed);
    if (!g_spritesOn.load(std::memory_order_relaxed)) { // JJ: "a checkbox to turn on and off the existing glow"
        g_spriteSkipped.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    const float k = g_spriteDim.load(std::memory_order_relaxed);
    // The layer is only added back by our combine-pass copy (game depth of field off).
    if (k >= 0.999f || !g_dofOff.load(std::memory_order_relaxed)) {
        if (k < 0.999f) g_spriteOther.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    const bool ok = dev && ensure_tex(dev, g_accum, td.Width, td.Height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (dev) dev->Release();
    if (!ok) return false;
    bvr::frame_inspector::ScopedSuppress quiet;
    const uint64_t pc = bvr::d3d11_hook::present_count();
    if (g_accumClearedAt != pc) {
        const float zero[4] = {0, 0, 0, 0};
        ctx->ClearRenderTargetView(g_accum.rtv, zero);
        g_accumClearedAt = pc;
    }
    g_accumUsedAt = pc;
    ctx->OMGetRenderTargets(1, &g_redirRtv, &g_redirDsv);
    ctx->OMSetRenderTargets(1, &g_accum.rtv, g_redirDsv);
    g_redirected = true;
    g_spriteDimmed.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void sprite_after(ID3D11DeviceContext* ctx) {
    if (!g_redirected) return;
    g_redirected = false;
    {
        bvr::frame_inspector::ScopedSuppress quiet;
        ctx->OMSetRenderTargets(1, &g_redirRtv, g_redirDsv);
    }
    if (g_redirRtv) g_redirRtv->Release();
    if (g_redirDsv) g_redirDsv->Release();
    g_redirRtv = nullptr;
    g_redirDsv = nullptr;
}

} // namespace

bool on_draw_plain(ID3D11DeviceContext* ctx, unsigned vertexCount) { return sprite_before(ctx, vertexCount); }
void after_draw_plain(ID3D11DeviceContext* ctx) { sprite_after(ctx); }

void install_early() {
    HMODULE d3d = LoadLibraryW(L"d3d11.dll");
    if (!d3d) return;
    void* a = reinterpret_cast<void*>(GetProcAddress(d3d, "D3D11CreateDevice"));
    void* b = reinterpret_cast<void*>(GetProcAddress(d3d, "D3D11CreateDeviceAndSwapChain"));
    MH_STATUS sa = a ? MH_CreateHook(a, reinterpret_cast<void*>(&create_dev_detour), reinterpret_cast<void**>(&g_createDevOrig)) : MH_ERROR_FUNCTION_NOT_FOUND;
    MH_STATUS sb = b ? MH_CreateHook(b, reinterpret_cast<void*>(&create_dev_sw_detour), reinterpret_cast<void**>(&g_createDevSwOrig)) : MH_ERROR_FUNCTION_NOT_FOUND;
    if (sa == MH_OK) sa = MH_EnableHook(a);
    if (sb == MH_OK) sb = MH_EnableHook(b);
    BVR_LOG("[glow] device creation hooks (shader recorder): %s / %s", MH_StatusToString(sa), MH_StatusToString(sb));
}

void note_fullscreen_pass(ID3D11DeviceContext* ctx) { dump_ps(ctx, "full-screen pass"); }

bool on_draw(ID3D11DeviceContext* ctx) {
    const int mode = g_mode.load(std::memory_order_relaxed) >= 2 ? 2 : 0;
    const bool noDof = g_dofOff.load(std::memory_order_relaxed);
    if ((mode == 0 && !noDof) || g_pipeFailed) return false;
    D3D11_TEXTURE2D_DESC sd{}, bd{};
    if (!is_combine(ctx, &sd, &bd)) return false;
    g_found.fetch_add(1, std::memory_order_relaxed);
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    bool ok = ensure_pipeline(dev);
    const int levels = std::max(1, std::min(g_levels.load(), kMaxLevels));
    UINT w = sd.Width, h = sd.Height;
    for (int i = 0; ok && i < levels; ++i) {
        w = std::max(1u, (w + 1) / 2);
        h = std::max(1u, (h + 1) / 2);
        ok = ensure_tex(dev, g_level[i], w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
    }
    ok = ok && ensure_tex(dev, g_sceneOut, sd.Width, sd.Height, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
         ensure_tex(dev, g_noGlow, bd.Width, bd.Height, DXGI_FORMAT_R16G16B16A16_UNORM);
    dev->Release();
    if (!ok) {
        if (g_failed.fetch_add(1) < 3) BVR_LOG("[glow] textures FAILED (%ux%u)", sd.Width, sd.Height);
        return false;
    }
    g_sceneW.store(sd.Width, std::memory_order_relaxed);
    g_sceneH.store(sd.Height, std::memory_order_relaxed);

    bvr::frame_inspector::ScopedSuppress quiet;
    Saved s;
    s.save(ctx);
    // The valid part of the scene texture is the combine pass's viewport (the game pads its targets).
    const float vw = s.nvp ? s.vp.Width : float(sd.Width), vh = s.nvp ? s.vp.Height : float(sd.Height);
    ctx->OMSetDepthStencilState(g_ds, 0);
    ctx->RSSetState(g_rs);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->PSSetSamplers(0, 1, &g_samp);
    ctx->PSSetConstantBuffers(0, 1, &g_cb);
    const float prm[4] = {g_threshold.load(), g_knee.load(), g_intensity.load(), g_radius.load()};
    Cb cb{};
    memcpy(cb.prm, prm, sizeof(prm));
    cb.clampUV[0] = cb.clampUV[1] = 1.0f;
    const bool ours = mode >= 2;
    if (ours) {
        // Pyramid: prefilter (Karis) into level 0, then down; then up, adding each level onto the one below.
        cb.texel[0] = 1.0f / float(sd.Width);
        cb.texel[1] = 1.0f / float(sd.Height);
        cb.clampUV[0] = (vw - 0.5f) / float(sd.Width);
        cb.clampUV[1] = (vh - 0.5f) / float(sd.Height);
        pass(ctx, g_level[0], g_psPre, s.srv[0], nullptr, false, cb);
        cb.clampUV[0] = cb.clampUV[1] = 1.0f;
        for (int i = 1; i < levels; ++i) {
            cb.texel[0] = 1.0f / float(g_level[i - 1].w);
            cb.texel[1] = 1.0f / float(g_level[i - 1].h);
            pass(ctx, g_level[i], g_psDown, g_level[i - 1].srv, nullptr, false, cb);
        }
        for (int i = levels - 1; i >= 1; --i) {
            cb.texel[0] = 1.0f / float(g_level[i].w);
            cb.texel[1] = 1.0f / float(g_level[i].h);
            pass(ctx, g_level[i - 1], g_psUp, g_level[i].srv, nullptr, true, cb);
        }
        cb.comp[0] = 1.0f / float(g_level[0].w);
        cb.comp[1] = 1.0f / float(g_level[0].h);
        cb.comp[2] = 1.0f / float(levels);
        pass(ctx, g_sceneOut, g_psComp, s.srv[0], g_level[0].srv, false, cb);
    }
    // Keep the game's two inputs for after_draw (save() holds a reference; take our own).
    g_origSrv[0] = s.srv[0];
    g_origSrv[1] = s.srv[1];
    if (g_origSrv[0]) g_origSrv[0]->AddRef();
    if (g_origSrv[1]) g_origSrv[1]->AddRef();
    s.restore(ctx);
    ID3D11ShaderResourceView* in[2] = {ours ? g_sceneOut.srv : g_origSrv[0], g_origSrv[1]};
    ctx->PSSetShaderResources(0, 2, in);
    if (noDof && g_psNoDof) {
        ctx->PSGetShader(&g_origPs, nullptr, nullptr);
        ctx->PSSetShader(g_psNoDof, nullptr, 0);
        if (!g_limCb) {
            ID3D11Device* d2 = nullptr;
            ctx->GetDevice(&d2);
            D3D11_BUFFER_DESC ld{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
            if (d2) { d2->CreateBuffer(&ld, nullptr, &g_limCb); d2->Release(); }
        }
        if (g_limCb) {
            // SPRITEDIM: this eye pass's glow sprites went into their own layer - add it back x strength.
            const bool layer = g_accum.srv && g_accumUsedAt == bvr::d3d11_hook::present_count();
            const float lim[4] = {g_limit.load(std::memory_order_relaxed), layer ? g_spriteDim.load(std::memory_order_relaxed) : 0.0f,
                                  g_shaftGlow.load(std::memory_order_relaxed), g_shaftSmooth.load(std::memory_order_relaxed) ? 1.0f : 0.0f};
            ctx->PSGetShaderResources(5, 1, &g_origSrv5);
            ID3D11ShaderResourceView* a5 = layer ? g_accum.srv : nullptr;
            ctx->PSSetShaderResources(5, 1, &a5);
            ctx->UpdateSubresource(g_limCb, 0, nullptr, lim, 0, 0);
            ctx->PSGetConstantBuffers(5, 1, &g_origCb5);
            ctx->PSSetConstantBuffers(5, 1, &g_limCb);
        }
    }
    g_swapped = true;
    if (g_done.fetch_add(1, std::memory_order_relaxed) == 0)
        BVR_LOG("[glow] combine pass found: scene %ux%u (viewport %.0fx%.0f), depth-of-field buffer %ux%u; our glow %d, "
                "game depth of field %s", sd.Width, sd.Height, vw, vh, bd.Width, bd.Height, mode, noDof ? "REMOVED" : "kept");
    return true;
}

void after_draw(ID3D11DeviceContext* ctx) {
    if (!g_swapped) return;
    g_swapped = false;
    {
        bvr::frame_inspector::ScopedSuppress quiet;
        ctx->PSSetShaderResources(0, 2, g_origSrv);
        if (g_origPs) {
            ctx->PSSetShader(g_origPs, nullptr, 0);
            ctx->PSSetConstantBuffers(5, 1, &g_origCb5);
            ctx->PSSetShaderResources(5, 1, &g_origSrv5);
            if (g_origSrv5) g_origSrv5->Release();
            g_origSrv5 = nullptr;
            if (g_origCb5) g_origCb5->Release();
            g_origCb5 = nullptr;
            g_origPs->Release();
            g_origPs = nullptr;
        }
    }
    for (auto*& v : g_origSrv) {
        if (v) v->Release();
        v = nullptr;
    }
}

bool command(const char* args) {
    // "glow mode N" | "glow intensity F" | "glow threshold F" | "glow knee F" | "glow radius F" | "glow levels N"
    char key[32] = {};
    float v = 0;
    if (sscanf_s(args, "%31s %f", key, unsigned(sizeof(key)), &v) != 2) return false;
    if (!strcmp(key, "game")) {
        g_shaftGlow.store(std::max(0.0f, std::min(v, 1.5f)));
        BVR_LOG("[glow] game glow (light-shaft buffer) x%.2f", g_shaftGlow.load());
        return true;
    }
    if (!strcmp(key, "smooth")) {
        g_shaftSmooth.store(v != 0.0f);
        return true;
    }
    if (!strcmp(key, "hide")) {
        g_hideGroup.store(int(v));
        BVR_LOG("[glow] hiding see-through group %d", int(v));
        return true;
    }
    if (!strcmp(key, "sprites")) {
        g_spritesOn.store(v != 0.0f);
        BVR_LOG("[glow] game glow sprites %s (seen %u, skipped %u)", v != 0.0f ? "ON" : "OFF", g_spriteSeen.load(),
                g_spriteSkipped.load());
        return true;
    }
    if (!strcmp(key, "limit")) {
        g_limit.store(std::max(0.3f, std::min(v, 1.0f)));
        BVR_LOG("[glow] bright-light limit %.2f", g_limit.load());
        return true;
    }
    if (!strcmp(key, "spritedim")) {
        g_spriteDim.store(std::max(0.0f, std::min(v, 1.0f)));
        BVR_LOG("[glow] scene glow sprites x%.2f (seen %u, dimmed %u, other blend %u)", g_spriteDim.load(), g_spriteSeen.load(),
                g_spriteDimmed.load(), g_spriteOther.load());
        return true;
    }
    if (!strcmp(key, "dof")) {
        g_dofOff.store(v == 0.0f);
        BVR_LOG("[glow] game depth of field %s", v == 0.0f ? "REMOVED" : "kept");
        return true;
    }
    if (!settings_read((std::string("glow_") + key).c_str(), args + strlen(key) + 1)) return false;
    BVR_LOG("[glow] %s %.3f", key, v);
    return true;
}

void draw_ui() {
    bool dof = !g_dofOff.load();
    if (ImGui::Checkbox("game's depth of field (the blocky blur / glow around distant lights)", &dof)) g_dofOff.store(!dof);
    float gg = g_shaftGlow.load();
    if (bvr::overlay::SliderStep("game's glow strength (around spotlights, neon, lamps)", &gg, 0.0f, 1.5f, "%.2f")) g_shaftGlow.store(gg);
    bool sm = g_shaftSmooth.load();
    if (ImGui::Checkbox("smooth glow edges (the game's glow is quarter size - blocky without this)", &sm)) g_shaftSmooth.store(sm);
    float lim = g_limit.load();
    if (bvr::overlay::SliderStep("bright-light limit (1 = off; lower = softer spotlights / neon)", &lim, 0.3f, 1.0f, "%.2f"))
        g_limit.store(lim);
    bool spr = g_spritesOn.load();
    if (ImGui::Checkbox("light beams (volumetric light from spotlights and lamps)", &spr)) g_spritesOn.store(spr);
    float sd = g_spriteDim.load();
    if (bvr::overlay::SliderStep("light beams strength", &sd, 0.0f, 1.0f, "%.2f")) g_spriteDim.store(sd);
    ImGui::TextDisabled("   beam draws %u, on their own layer %u, kept (depth of field on) %u", g_spriteSeen.load(), g_spriteDimmed.load(),
                        g_spriteOther.load());
    ImGui::TextUnformatted("find the glow (test): hide one group of see-through effects while you look at a glowing sign");
    int hg = g_hideGroup.load();
    if (bvr::overlay::SliderStepInt("   hidden group (-1 = none)", &hg, -1, std::max(0, g_censusN - 1))) g_hideGroup.store(hg);
    if (hg >= 0 && hg < g_censusN)
        ImGui::TextDisabled("   group %d: %s, drawn %u times", hg, g_groupDesc[hg], g_groupDraws[hg].load());
    ImGui::TextDisabled("   %d groups seen so far", g_censusN);
    // Our own bloom (glow_mode 2) is kept in the code but not offered: the game has its own glow sprites; JJ 2026-10-08:
    // "we don't need our own one".
    ImGui::TextDisabled("   combine passes changed %u", g_done.load());
}

void settings_write(std::string& out) {
    char buf[256];
    snprintf(buf, sizeof(buf), "game_dof=%d\nglow_mode=%d\nglow_intensity=%.4f\nglow_threshold=%.3f\nglow_knee=%.3f\nglow_radius=%.3f\nglow_levels=%d\n",
             g_dofOff.load() ? 0 : 1, g_mode.load(), g_intensity.load(), g_threshold.load(), g_knee.load(), g_radius.load(), g_levels.load());
    out += buf;
    snprintf(buf, sizeof(buf), "glow_limit=%.3f\nglow_sprites=%.3f\n", g_limit.load(), g_spriteDim.load());
    out += buf;
    snprintf(buf, sizeof(buf), "glow_sprites_on=%d\nglow_game=%.3f\nglow_smooth=%d\n", g_spritesOn.load() ? 1 : 0,
             g_shaftGlow.load(), g_shaftSmooth.load() ? 1 : 0);
    out += buf;
}

bool settings_read(const char* key, const char* val) {
    const float f = static_cast<float>(atof(val));
    if (!strcmp(key, "game_dof")) g_dofOff.store(atoi(val) == 0);
    else if (!strcmp(key, "glow_game")) { if (f >= 0.0f && f <= 1.5f) g_shaftGlow.store(f); }
    else if (!strcmp(key, "glow_smooth")) g_shaftSmooth.store(atoi(val) != 0);
    else if (!strcmp(key, "glow_limit")) { if (f >= 0.3f && f <= 1.0f) g_limit.store(f); }
    else if (!strcmp(key, "glow_sprites_on")) g_spritesOn.store(atoi(val) != 0);
    else if (!strcmp(key, "glow_sprites")) { if (f >= 0.0f && f <= 1.0f) g_spriteDim.store(f); }
    else if (!strcmp(key, "glow_mode")) g_mode.store(0); // not offered any more (see draw_ui)
    else if (!strcmp(key, "glow_intensity")) { if (f >= 0.0f && f <= 2.0f) g_intensity.store(f); }
    else if (!strcmp(key, "glow_threshold")) { if (f >= 0.0f && f <= 64.0f) g_threshold.store(f); }
    else if (!strcmp(key, "glow_knee")) { if (f >= 0.0f && f <= 8.0f) g_knee.store(f); }
    else if (!strcmp(key, "glow_radius")) { if (f >= 0.25f && f <= 4.0f) g_radius.store(f); }
    else if (!strcmp(key, "glow_levels")) { const int l = atoi(val); if (l >= 1 && l <= kMaxLevels) g_levels.store(l); }
    else return false;
    return true;
}

void status_text(std::string& out) {
    char buf[256];
    snprintf(buf, sizeof(buf), "GLOW mode %d intensity %.3f threshold %.2f knee %.2f radius %.2f levels %d | combine passes "
             "found %u changed %u failed %u | scene %ux%u\n",
             g_mode.load(), g_intensity.load(), g_threshold.load(), g_knee.load(), g_radius.load(), g_levels.load(),
             g_found.load(), g_done.load(), g_failed.load(), g_sceneW.load(), g_sceneH.load());
    out += buf;
}

} // namespace bvr::ac::glow
