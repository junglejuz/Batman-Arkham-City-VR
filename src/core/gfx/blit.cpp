#include "core/gfx/blit.h"

#include "core/util/log.h"

#include <d3dcompiler.h>

#include <cstring>

namespace bvr::blit {
namespace {

const char* kShader = R"(
Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);
cbuffer StretchCB : register(b0) { float4 uvRemap; } // x = vOffset, y = vScale, z = uOffset
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(uint id : SV_VertexID) {
    // Fullscreen triangle, no vertex buffer.
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}
// Session 22: sample only a horizontal BAND of the source, stretched across
// the full destination (the engine-cinematic letterbox unsqueeze).
VSOut vs_stretch(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = float2(uv.x + uvRemap.z, uvRemap.x + uv.y * uvRemap.y);
    return o;
}
// Batman City SCREENROOM: a flat picture on a screen hanging in the room, drawn into one eye's
// image. The four corners arrive already in that eye's clip space (TL, TR, BL, BR); the GPU's
// perspective-correct interpolation keeps the picture undistorted at any viewing angle.
cbuffer ScreenCB : register(b1) { float4 corner[4]; }
VSOut vs_screen(uint id : SV_VertexID) {
    VSOut o;
    o.pos = corner[id];
    o.uv = float2(id & 1, id >> 1);
    return o;
}
float4 ps_main(VSOut i) : SV_Target {
    return tex0.Sample(samp0, i.uv);
}
// Alpha safety floor: coverage now accumulates correctly (the redirect swaps
// gameswf blend states for alpha-corrected variants), so the luminance term
// is only a low floor against any state that slips through - bright pixels
// can never be fully invisible.
// SVRGLOW: premultiplied-valid alpha for SteamVR's quad compositing - additive pieces (rgb > 0, alpha 0)
// get alpha = their brightness, so they show as light instead of vanishing.
float4 ps_repairfull(VSOut i) : SV_Target {
    float4 c = tex0.Sample(samp0, i.uv);
    c.a = max(c.a, max(c.r, max(c.g, c.b)));
    return c;
}
float4 ps_process(VSOut i) : SV_Target {
    float4 c = tex0.Sample(samp0, i.uv);
    float lum = dot(c.rgb, float3(0.299, 0.587, 0.114));
    c.a = max(c.a, saturate(lum * 0.35));
    return c;
}
// Batman City PAUSEDIRT: the source picture on a sphere around the recentred head, per pixel. The view
// ray (rebuilt from this eye's clip space) meets the sphere of radius screen.x; the hit point's yaw and
// pitch, over the picture's angular width / height (screen.y / screen.z, radians), give the uv.
cbuffer WarpCB : register(b2) { row_major float4x4 invClip; float4 screen; }
float4 ps_warp(VSOut i) : SV_Target {
    float2 ndc = float2(i.uv.x * 2 - 1, 1 - i.uv.y * 2);
    float4 a = mul(float4(ndc, 0.5, 1), invClip);
    float4 b = mul(float4(ndc, 0.9, 1), invClip);
    a.xyz /= a.w;
    b.xyz /= b.w;
    float3 d = normalize(b.xyz - a.xyz);
    float3 o = a.xyz - float3(0, screen.w, 0);
    float bo = dot(o, d), cc = dot(o, o) - screen.x * screen.x;
    float t = -bo + sqrt(max(bo * bo - cc, 0));
    float3 p = o + d * t;
    float yaw = atan2(p.x, p.z), pitch = atan2(p.y, length(p.xz));
    float2 uv = float2(0.5 + yaw / screen.y, 0.5 - pitch / screen.z);
    return tex0.Sample(samp0, uv);
}
// Batman City ROOMGRUNGE: dst' = add + mul * dst (dual-source blend ONE / SRC1_COLOR).
Texture2D tex1 : register(t1);
struct OverlayOut { float4 add : SV_Target0; float4 mul : SV_Target1; };
OverlayOut ps_overlay(VSOut i) {
    OverlayOut o;
    float3 black = tex0.Sample(samp0, i.uv).rgb;
    float3 white = tex1.Sample(samp0, i.uv).rgb;
    // Feathered border (JJ: no hard edge when the head turns): fade to "no overlay" near the edges.
    float2 e = min(i.uv, 1 - i.uv);
    float f = smoothstep(0, 0.08, min(e.x, e.y));
    o.add = float4(black * f, 0);
    o.mul = float4(lerp(float3(1, 1, 1), saturate(white - black), f), 1);
    return o;
}
// Batman City PAUSEHALO: a soft dark halo under the HUD panel's content (premultiplied picture: black under it only
// raises alpha). Pass 1 averages the source alpha over 8x8 texel blocks (16 bilinear taps) into a small picture;
// pass 2 spreads that over three rings around each pixel (bilinear on the small picture = smooth) and lays the
// result under the content.
cbuffer HaloCB : register(b3) { float4 halo; } // down: xy = source texel size | under: xy = radius (uv), z gain, w strength
// Batman City WIPEREPLAY: the source drawn over the whole destination, enlarged by zoom.x about the centre (clamped
// edges continue the border - the wipe's black reaches every edge).
cbuffer ZoomCB : register(b4) { float4 zoom; }
VSOut vs_zoom(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = 0.5 + (uv - 0.5) / zoom.x;
    return o;
}
// WIPEREPLAY storage: the wipe's coverage (alpha) into a one-channel picture, and back as black with that coverage.
float4 ps_a2r(VSOut i) : SV_Target { return float4(tex0.Sample(samp0, i.uv).a, 0, 0, 0); }
float4 ps_maskblack(VSOut i) : SV_Target { return float4(0, 0, 0, tex0.Sample(samp0, i.uv).r); }
float4 ps_maskblackinv(VSOut i) : SV_Target { return float4(0, 0, 0, 1 - tex0.Sample(samp0, i.uv).r); }
float4 ps_alphadown(VSOut i) : SV_Target {
    float a = 0;
    [unroll] for (int y = 0; y < 4; ++y)
        [unroll] for (int x = 0; x < 4; ++x)
            a += tex0.SampleLevel(samp0, i.uv + float2(x * 2 - 3, y * 2 - 3) * halo.xy, 0).a;
    return float4(0, 0, 0, a / 16);
}
float4 ps_haloback(VSOut i) : SV_Target {
    float4 c = tex0.Sample(samp0, i.uv);
    float acc = tex1.SampleLevel(samp0, i.uv, 0).a * 1.2, wsum = 1.2;
    [unroll] for (int r = 1; r <= 3; ++r) {
        float w = 1.0 - (r - 1) * 0.25;
        [unroll] for (int k = 0; k < 12; ++k) {
            float ang = (k + 0.5 * r) * 0.5235988;
            acc += tex1.SampleLevel(samp0, i.uv + float2(cos(ang), sin(ang)) * (r / 3.0) * halo.xy, 0).a * w;
            wsum += w;
        }
    }
    float s = saturate(acc / wsum * halo.z) * halo.w;
    return float4(c.rgb, c.a + (1 - c.a) * s);
}
)";

ID3D11VertexShader* g_vs = nullptr;
ID3D11VertexShader* g_vsStretch = nullptr;
ID3D11VertexShader* g_vsScreen = nullptr;
ID3D11Buffer* g_screenCb = nullptr;
ID3D11Buffer* g_stretchCb = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11PixelShader* g_psProcess = nullptr;
ID3D11BlendState* g_blend = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3D11SamplerState* g_samplerMirror = nullptr; // PAUSEDIRT: the dirt continues mirrored past the screen
ID3D11RasterizerState* g_raster = nullptr;
ID3D11DepthStencilState* g_depth = nullptr;
bool g_failed = false;

bool ensure_pipeline(ID3D11DeviceContext* ctx) {
    if (g_vs) return true;
    if (g_failed) return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;

    ID3DBlob* vsb = nullptr;
    ID3DBlob* vstb = nullptr;
    ID3DBlob* vssb = nullptr;
    ID3DBlob* psb = nullptr;
    ID3DBlob* ppb = nullptr;
    ID3DBlob* err = nullptr;
    bool ok = SUCCEEDED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr,
                                   "vs_main", "vs_4_0", 0, 0, &vsb, &err)) &&
              SUCCEEDED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr,
                                   "vs_stretch", "vs_4_0", 0, 0, &vstb, &err)) &&
              SUCCEEDED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr,
                                   "vs_screen", "vs_4_0", 0, 0, &vssb, &err)) &&
              SUCCEEDED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr,
                                   "ps_main", "ps_4_0", 0, 0, &psb, &err)) &&
              SUCCEEDED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr,
                                   "ps_process", "ps_4_0", 0, 0, &ppb, &err));
    if (ok)
        ok = SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(),
                                               nullptr, &g_vs)) &&
             SUCCEEDED(dev->CreateVertexShader(vstb->GetBufferPointer(), vstb->GetBufferSize(),
                                               nullptr, &g_vsStretch)) &&
             SUCCEEDED(dev->CreateVertexShader(vssb->GetBufferPointer(), vssb->GetBufferSize(),
                                               nullptr, &g_vsScreen)) &&
             SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(),
                                              nullptr, &g_ps)) &&
             SUCCEEDED(dev->CreatePixelShader(ppb->GetBufferPointer(), ppb->GetBufferSize(),
                                              nullptr, &g_psProcess));
    if (ok) {
        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = 16;
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = SUCCEEDED(dev->CreateBuffer(&cbd, nullptr, &g_stretchCb));
        cbd.ByteWidth = 64;
        ok = ok && SUCCEEDED(dev->CreateBuffer(&cbd, nullptr, &g_screenCb));
    }
    if (vsb) vsb->Release();
    if (vstb) vstb->Release();
    if (vssb) vssb->Release();
    if (psb) psb->Release();
    if (ppb) ppb->Release();
    if (err) {
        BVR_LOG("[blit] shader compile: %s", static_cast<const char*>(err->GetBufferPointer()));
        err->Release();
    }

    if (ok) {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        // Premultiplied: the source rgb is already scaled by coverage.
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = FALSE;
        dd.StencilEnable = FALSE;
        ok = SUCCEEDED(dev->CreateBlendState(&bd, &g_blend)) &&
             SUCCEEDED(dev->CreateSamplerState(&sd, &g_sampler)) &&
             [&] {
                 D3D11_SAMPLER_DESC md = sd;
                 md.AddressU = md.AddressV = md.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR;
                 return SUCCEEDED(dev->CreateSamplerState(&md, &g_samplerMirror));
             }() &&
             SUCCEEDED(dev->CreateRasterizerState(&rd, &g_raster)) &&
             SUCCEEDED(dev->CreateDepthStencilState(&dd, &g_depth));
    }
    dev->Release();
    if (!ok) {
        release();
        g_failed = true; // log once, stay quiet after
        BVR_LOG("[blit] pipeline creation FAILED - window HUD composite disabled");
    }
    return ok;
}

} // namespace

namespace {

bool draw_internal(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst,
                   ID3D11ShaderResourceView* src, UINT dstW, UINT dstH,
                   ID3D11PixelShader* ps, ID3D11BlendState* blend,
                   ID3D11VertexShader* vs = nullptr, ID3D11SamplerState* samp = nullptr) {
    if (!ctx || !dst || !src || !dstW || !dstH) return false;
    if (!ensure_pipeline(ctx)) return false;
    if (!vs) vs = g_vs;

    // Backup the slices of state we touch (the game re-binds most of this per
    // frame, but the present chain must be transparent regardless).
    ID3D11RenderTargetView* oldRtv = nullptr;
    ID3D11DepthStencilView* oldDsv = nullptr;
    ctx->OMGetRenderTargets(1, &oldRtv, &oldDsv);
    ID3D11BlendState* oldBlend = nullptr;
    FLOAT oldBf[4];
    UINT oldMask = 0;
    ctx->OMGetBlendState(&oldBlend, oldBf, &oldMask);
    ID3D11DepthStencilState* oldDepth = nullptr;
    UINT oldStencilRef = 0;
    ctx->OMGetDepthStencilState(&oldDepth, &oldStencilRef);
    ID3D11RasterizerState* oldRaster = nullptr;
    ctx->RSGetState(&oldRaster);
    UINT nVp = 1;
    D3D11_VIEWPORT oldVp{};
    ctx->RSGetViewports(&nVp, &oldVp);
    ID3D11VertexShader* oldVs = nullptr;
    // Batman City REFLECTEYE: blits can now run mid-scene, where the game may have tessellation (hull /
    // domain) or geometry shaders bound - a triangle-list draw through them is rejected. Clear and restore.
    ID3D11HullShader* oldHs = nullptr;
    ID3D11DomainShader* oldDs = nullptr;
    ID3D11GeometryShader* oldGs = nullptr;
    ctx->HSGetShader(&oldHs, nullptr, nullptr);
    ctx->DSGetShader(&oldDs, nullptr, nullptr);
    ctx->GSGetShader(&oldGs, nullptr, nullptr);
    ID3D11PixelShader* oldPs = nullptr;
    ctx->VSGetShader(&oldVs, nullptr, nullptr);
    ctx->PSGetShader(&oldPs, nullptr, nullptr);
    ID3D11Buffer* oldVsCb = nullptr;
    ctx->VSGetConstantBuffers(0, 1, &oldVsCb); // the engine lives in VS b0
    ID3D11Buffer* oldVsCb1 = nullptr;
    ctx->VSGetConstantBuffers(1, 1, &oldVsCb1);
    ID3D11ShaderResourceView* oldSrv = nullptr;
    ctx->PSGetShaderResources(0, 1, &oldSrv);
    ID3D11SamplerState* oldSamp = nullptr;
    ctx->PSGetSamplers(0, 1, &oldSamp);
    ID3D11InputLayout* oldLayout = nullptr;
    ctx->IAGetInputLayout(&oldLayout);
    D3D11_PRIMITIVE_TOPOLOGY oldTopo{};
    ctx->IAGetPrimitiveTopology(&oldTopo);

    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<FLOAT>(dstW);
    vp.Height = static_cast<FLOAT>(dstH);
    vp.MaxDepth = 1.0f;
    ctx->OMSetRenderTargets(1, &dst, nullptr);
    ctx->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g_depth, 0);
    ctx->RSSetState(g_raster);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(vs == g_vsScreen ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
                                                 : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    if (vs == g_vsStretch) ctx->VSSetConstantBuffers(0, 1, &g_stretchCb);
    if (vs == g_vsScreen) ctx->VSSetConstantBuffers(1, 1, &g_screenCb);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &src);
    ctx->PSSetSamplers(0, 1, samp ? &samp : &g_sampler);
    ctx->Draw(vs == g_vsScreen ? 4 : 3, 0);

    ctx->OMSetRenderTargets(1, &oldRtv, oldDsv);
    ctx->OMSetBlendState(oldBlend, oldBf, oldMask);
    ctx->OMSetDepthStencilState(oldDepth, oldStencilRef);
    ctx->RSSetState(oldRaster);
    if (nVp) ctx->RSSetViewports(1, &oldVp);
    ctx->VSSetShader(oldVs, nullptr, 0);
    ctx->HSSetShader(oldHs, nullptr, 0);
    ctx->DSSetShader(oldDs, nullptr, 0);
    ctx->GSSetShader(oldGs, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &oldVsCb);
    ctx->VSSetConstantBuffers(1, 1, &oldVsCb1);
    ctx->PSSetShader(oldPs, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &oldSrv);
    ctx->PSSetSamplers(0, 1, &oldSamp);
    ctx->IASetInputLayout(oldLayout);
    ctx->IASetPrimitiveTopology(oldTopo);
    if (oldRtv) oldRtv->Release();
    if (oldDsv) oldDsv->Release();
    if (oldVsCb) oldVsCb->Release();
    if (oldVsCb1) oldVsCb1->Release();
    if (oldBlend) oldBlend->Release();
    if (oldDepth) oldDepth->Release();
    if (oldRaster) oldRaster->Release();
    if (oldVs) oldVs->Release();
    if (oldHs) oldHs->Release();
    if (oldDs) oldDs->Release();
    if (oldGs) oldGs->Release();
    if (oldPs) oldPs->Release();
    if (oldSrv) oldSrv->Release();
    if (oldSamp) oldSamp->Release();
    if (oldLayout) oldLayout->Release();
    return true;
}

} // namespace

bool alpha_premul(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst,
                  ID3D11ShaderResourceView* src, UINT dstW, UINT dstH) {
    return draw_internal(ctx, dst, src, dstW, dstH, g_ps, g_blend);
}

ID3D11PixelShader* g_psRepairFull = nullptr;
bool g_repairFullFailed = false;
bool alpha_repair_full(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst,
                       ID3D11ShaderResourceView* src, UINT dstW, UINT dstH) {
    if (!g_psRepairFull) {
        if (g_repairFullFailed || !ensure_pipeline(ctx)) return false;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return false;
        ID3DBlob* pb = nullptr;
        ID3DBlob* err = nullptr;
        const bool ok = SUCCEEDED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "ps_repairfull",
                                             "ps_4_0", 0, 0, &pb, &err)) &&
                        SUCCEEDED(dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr,
                                                         &g_psRepairFull));
        if (err) { BVR_LOG("[blit] repair shader: %s", static_cast<const char*>(err->GetBufferPointer())); err->Release(); }
        if (pb) pb->Release();
        dev->Release();
        if (!ok) { g_repairFullFailed = true; BVR_LOG("[blit] full alpha repair FAILED"); return false; }
    }
    return draw_internal(ctx, dst, src, dstW, dstH, g_psRepairFull, nullptr);
}

bool process(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst,
             ID3D11ShaderResourceView* src, UINT dstW, UINT dstH) {
    // Blend OFF (null state = replace): rgb passes through, alpha repaired.
    return draw_internal(ctx, dst, src, dstW, dstH, g_psProcess, nullptr);
}

bool stretch_band(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst,
                  ID3D11ShaderResourceView* src, UINT dstW, UINT dstH,
                  float topFrac, float heightFrac) {
    if (!ensure_pipeline(ctx)) return false;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_stretchCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    float* f = static_cast<float*>(m.pData);
    f[0] = topFrac;    // vOffset
    f[1] = heightFrac; // vScale
    f[2] = 0.0f;
    f[3] = 0.0f;
    ctx->Unmap(g_stretchCb, 0);
    // Blend OFF (replace): the band overwrites the whole destination.
    return draw_internal(ctx, dst, src, dstW, dstH, g_ps, nullptr, g_vsStretch);
}

bool shift_copy(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst, UINT dstW, UINT dstH,
                ID3D11ShaderResourceView* src, float du) {
    if (!ensure_pipeline(ctx)) return false;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_stretchCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    float* f = static_cast<float*>(m.pData);
    f[0] = 0.0f;
    f[1] = 1.0f;
    f[2] = du;
    f[3] = 0.0f;
    ctx->Unmap(g_stretchCb, 0);
    return draw_internal(ctx, dst, src, dstW, dstH, g_ps, nullptr, g_vsStretch);
}

bool screen_in_eye(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst, UINT dstW, UINT dstH,
                   ID3D11ShaderResourceView* src, const float corners[4][4]) {
    if (!ensure_pipeline(ctx)) return false;
    const FLOAT black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    ctx->ClearRenderTargetView(dst, black);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_screenCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    memcpy(m.pData, corners, 64);
    ctx->Unmap(g_screenCb, 0);
    return draw_internal(ctx, dst, src, dstW, dstH, g_ps, nullptr, g_vsScreen);
}

namespace {
ID3D11PixelShader* g_psWarp = nullptr;
ID3D11Buffer* g_warpCb = nullptr;
bool g_warpFailed = false;
ID3D11PixelShader* g_psOverlay = nullptr;
ID3D11BlendState* g_blendOverlay = nullptr;
bool g_overlayFailed = false;

bool ensure_overlay(ID3D11DeviceContext* ctx) {
    if (g_psOverlay) return true;
    if (g_overlayFailed || !ensure_pipeline(ctx)) return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    ID3DBlob* psb = nullptr;
    ID3DBlob* err = nullptr;
    bool ok = SUCCEEDED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "ps_overlay", "ps_4_0", 0,
                                   0, &psb, &err)) &&
              SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_psOverlay));
    if (ok) {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_SRC1_COLOR;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        ok = SUCCEEDED(dev->CreateBlendState(&bd, &g_blendOverlay));
    }
    if (psb) psb->Release();
    if (err) {
        BVR_LOG("[blit] overlay shader compile: %s", static_cast<const char*>(err->GetBufferPointer()));
        err->Release();
    }
    dev->Release();
    if (!ok) {
        g_overlayFailed = true;
        BVR_LOG("[blit] overlay pipeline creation FAILED - room overlays off");
    }
    return ok;
}
} // namespace

bool overlay_in_eye(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst, UINT dstW, UINT dstH,
                    ID3D11ShaderResourceView* blackSrv, ID3D11ShaderResourceView* whiteSrv,
                    const float corners[4][4]) {
    if (!whiteSrv || !ensure_overlay(ctx)) return false;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_screenCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    memcpy(m.pData, corners, 64);
    ctx->Unmap(g_screenCb, 0);
    ID3D11ShaderResourceView* oldSrv1 = nullptr;
    ctx->PSGetShaderResources(1, 1, &oldSrv1);
    ctx->PSSetShaderResources(1, 1, &whiteSrv);
    const bool ok = draw_internal(ctx, dst, blackSrv, dstW, dstH, g_psOverlay, g_blendOverlay, g_vsScreen);
    ctx->PSSetShaderResources(1, 1, &oldSrv1);
    if (oldSrv1) oldSrv1->Release();
    return ok;
}

bool plane_warp(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst, UINT dstW, UINT dstH,
                ID3D11ShaderResourceView* src, const float invClip[16], const float screen[4]) {
    if (!g_psWarp) {
        if (g_warpFailed || !ensure_pipeline(ctx)) return false;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return false;
        ID3DBlob* psb = nullptr;
        ID3DBlob* err = nullptr;
        bool ok = SUCCEEDED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "ps_warp", "ps_4_0", 0, 0,
                                       &psb, &err)) &&
                  SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_psWarp));
        if (ok) {
            D3D11_BUFFER_DESC cbd{};
            cbd.ByteWidth = 80;
            cbd.Usage = D3D11_USAGE_DYNAMIC;
            cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            ok = SUCCEEDED(dev->CreateBuffer(&cbd, nullptr, &g_warpCb));
        }
        if (psb) psb->Release();
        if (err) {
            BVR_LOG("[blit] warp shader compile: %s", static_cast<const char*>(err->GetBufferPointer()));
            err->Release();
        }
        dev->Release();
        if (!ok) {
            g_warpFailed = true;
            BVR_LOG("[blit] warp pipeline creation FAILED");
            return false;
        }
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_warpCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    memcpy(m.pData, invClip, 64);
    memcpy(static_cast<unsigned char*>(m.pData) + 64, screen, 16);
    ctx->Unmap(g_warpCb, 0);
    ID3D11Buffer* oldCb = nullptr;
    ctx->PSGetConstantBuffers(2, 1, &oldCb);
    ctx->PSSetConstantBuffers(2, 1, &g_warpCb);
    const bool ok = draw_internal(ctx, dst, src, dstW, dstH, g_psWarp, nullptr, nullptr, g_samplerMirror);
    ctx->PSSetConstantBuffers(2, 1, &oldCb);
    if (oldCb) oldCb->Release();
    return ok;
}

namespace {
ID3D11PixelShader* g_psDown = nullptr;
ID3D11PixelShader* g_psHalo = nullptr;
ID3D11Buffer* g_haloCb = nullptr;
bool g_haloFailed = false;
bool ensure_halo(ID3D11DeviceContext* ctx) {
    if (g_psHalo) return true;
    if (g_haloFailed || !ensure_pipeline(ctx)) return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    ID3DBlob* a = nullptr;
    ID3DBlob* b = nullptr;
    ID3DBlob* err = nullptr;
    bool ok = SUCCEEDED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "ps_alphadown", "ps_4_0", 0, 0,
                                   &a, &err)) &&
              SUCCEEDED(dev->CreatePixelShader(a->GetBufferPointer(), a->GetBufferSize(), nullptr, &g_psDown));
    if (err) { BVR_LOG("[blit] halo down shader: %s", static_cast<const char*>(err->GetBufferPointer())); err->Release(); err = nullptr; }
    ok = ok && SUCCEEDED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "ps_haloback", "ps_4_0", 0, 0,
                                    &b, &err)) &&
         SUCCEEDED(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psHalo));
    if (err) { BVR_LOG("[blit] halo shader: %s", static_cast<const char*>(err->GetBufferPointer())); err->Release(); }
    if (ok) {
        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = 16;
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = SUCCEEDED(dev->CreateBuffer(&cbd, nullptr, &g_haloCb));
    }
    if (a) a->Release();
    if (b) b->Release();
    dev->Release();
    if (!ok) {
        g_haloFailed = true;
        if (g_psHalo) { g_psHalo->Release(); g_psHalo = nullptr; }
        BVR_LOG("[blit] halo pipeline creation FAILED - pause halo off");
    }
    return ok;
}
bool halo_draw(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst, UINT dstW, UINT dstH,
               ID3D11ShaderResourceView* src, ID3D11ShaderResourceView* src1, ID3D11PixelShader* ps,
               const float p[4]) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_haloCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    memcpy(m.pData, p, 16);
    ctx->Unmap(g_haloCb, 0);
    ID3D11Buffer* oldCb = nullptr;
    ctx->PSGetConstantBuffers(3, 1, &oldCb);
    ctx->PSSetConstantBuffers(3, 1, &g_haloCb);
    ID3D11ShaderResourceView* oldSrv1 = nullptr;
    ctx->PSGetShaderResources(1, 1, &oldSrv1);
    ctx->PSSetShaderResources(1, 1, &src1);
    const bool ok = draw_internal(ctx, dst, src, dstW, dstH, ps, nullptr);
    ctx->PSSetShaderResources(1, 1, &oldSrv1);
    ctx->PSSetConstantBuffers(3, 1, &oldCb);
    if (oldSrv1) oldSrv1->Release();
    if (oldCb) oldCb->Release();
    return ok;
}
} // namespace

ID3D11VertexShader* g_vsZoom = nullptr;
ID3D11PixelShader* g_psA2R = nullptr;
ID3D11PixelShader* g_psMask = nullptr;
ID3D11PixelShader* g_psMaskInv = nullptr; // WIPEFLIP
ID3D11Buffer* g_zoomCb = nullptr;
bool g_zoomFailed = false;

bool zoom_draw(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst, UINT dstW, UINT dstH,
               ID3D11ShaderResourceView* src, float zoom, int mode) {
    if (!g_vsZoom) {
        if (g_zoomFailed || !ensure_pipeline(ctx)) return false;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return false;
        ID3DBlob* vb = nullptr;
        ID3DBlob* err = nullptr;
        bool ok = SUCCEEDED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "vs_zoom", "vs_4_0", 0, 0,
                                       &vb, &err)) &&
                  SUCCEEDED(dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &g_vsZoom));
        if (err) { BVR_LOG("[blit] zoom shader: %s", static_cast<const char*>(err->GetBufferPointer())); err->Release(); err = nullptr; }
        static const char* const kNames[3] = {"ps_a2r", "ps_maskblack", "ps_maskblackinv"};
        ID3D11PixelShader** const outs[3] = {&g_psA2R, &g_psMask, &g_psMaskInv};
        for (int k = 0; k < 3 && ok; ++k) {
            ID3DBlob* pb = nullptr;
            ok = SUCCEEDED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, kNames[k],
                                      "ps_4_0", 0, 0, &pb, &err)) &&
                 SUCCEEDED(dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, outs[k]));
            if (err) { BVR_LOG("[blit] mask shader: %s", static_cast<const char*>(err->GetBufferPointer())); err->Release(); err = nullptr; }
            if (pb) pb->Release();
        }
        if (ok) {
            D3D11_BUFFER_DESC cbd{};
            cbd.ByteWidth = 16;
            cbd.Usage = D3D11_USAGE_DYNAMIC;
            cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            ok = SUCCEEDED(dev->CreateBuffer(&cbd, nullptr, &g_zoomCb));
        }
        if (vb) vb->Release();
        dev->Release();
        if (!ok) {
            g_zoomFailed = true;
            if (g_vsZoom) { g_vsZoom->Release(); g_vsZoom = nullptr; }
            BVR_LOG("[blit] zoom pipeline creation FAILED");
            return false;
        }
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_zoomCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    const float z[4] = {zoom > 0.01f ? zoom : 1.0f, 0, 0, 0};
    memcpy(m.pData, z, 16);
    ctx->Unmap(g_zoomCb, 0);
    ID3D11Buffer* oldCb = nullptr;
    ctx->VSGetConstantBuffers(4, 1, &oldCb);
    ctx->VSSetConstantBuffers(4, 1, &g_zoomCb);
    ID3D11PixelShader* ps = mode == 1 ? g_psA2R : (mode == 2 ? g_psMask : (mode == 3 ? g_psMaskInv : g_ps));
    const bool ok = draw_internal(ctx, dst, src, dstW, dstH, ps, mode == 1 ? nullptr : g_blend, g_vsZoom);
    ctx->VSSetConstantBuffers(4, 1, &oldCb);
    if (oldCb) oldCb->Release();
    return ok;
}
bool zoom_premul(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst, UINT dstW, UINT dstH,
                 ID3D11ShaderResourceView* src, float zoom) {
    return zoom_draw(ctx, dst, dstW, dstH, src, zoom, 0);
}
bool alpha_to_r8(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst, UINT dstW, UINT dstH,
                 ID3D11ShaderResourceView* src) {
    return zoom_draw(ctx, dst, dstW, dstH, src, 1.0f, 1);
}
bool mask_black(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst, UINT dstW, UINT dstH,
                ID3D11ShaderResourceView* mask, float zoom, bool invert) {
    return zoom_draw(ctx, dst, dstW, dstH, mask, zoom, invert ? 3 : 2);
}

bool alpha_down(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst, UINT dstW, UINT dstH,
                ID3D11ShaderResourceView* src, UINT srcW, UINT srcH) {
    if (!ensure_halo(ctx) || !srcW || !srcH) return false;
    const float p[4] = {1.0f / srcW, 1.0f / srcH, 0, 0};
    return halo_draw(ctx, dst, dstW, dstH, src, nullptr, g_psDown, p);
}

bool halo_under(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst, UINT dstW, UINT dstH,
                ID3D11ShaderResourceView* src, ID3D11ShaderResourceView* smallAlpha, const float params[4]) {
    if (!ensure_halo(ctx) || !smallAlpha) return false;
    return halo_draw(ctx, dst, dstW, dstH, src, smallAlpha, g_psHalo, params);
}

void release() {
    if (g_vsZoom) { g_vsZoom->Release(); g_vsZoom = nullptr; }
    if (g_psA2R) { g_psA2R->Release(); g_psA2R = nullptr; }
    if (g_psMask) { g_psMask->Release(); g_psMask = nullptr; }
    if (g_psMaskInv) { g_psMaskInv->Release(); g_psMaskInv = nullptr; }
    if (g_zoomCb) { g_zoomCb->Release(); g_zoomCb = nullptr; }
    if (g_psDown) { g_psDown->Release(); g_psDown = nullptr; }
    if (g_psHalo) { g_psHalo->Release(); g_psHalo = nullptr; }
    if (g_haloCb) { g_haloCb->Release(); g_haloCb = nullptr; }
    if (g_psWarp) { g_psWarp->Release(); g_psWarp = nullptr; }
    if (g_warpCb) { g_warpCb->Release(); g_warpCb = nullptr; }
    if (g_psOverlay) { g_psOverlay->Release(); g_psOverlay = nullptr; }
    if (g_blendOverlay) { g_blendOverlay->Release(); g_blendOverlay = nullptr; }
    if (g_vs) { g_vs->Release(); g_vs = nullptr; }
    if (g_vsScreen) { g_vsScreen->Release(); g_vsScreen = nullptr; }
    if (g_screenCb) { g_screenCb->Release(); g_screenCb = nullptr; }
    if (g_vsStretch) { g_vsStretch->Release(); g_vsStretch = nullptr; }
    if (g_stretchCb) { g_stretchCb->Release(); g_stretchCb = nullptr; }
    if (g_ps) { g_ps->Release(); g_ps = nullptr; }
    if (g_psProcess) { g_psProcess->Release(); g_psProcess = nullptr; }
    if (g_psRepairFull) { g_psRepairFull->Release(); g_psRepairFull = nullptr; }
    if (g_blend) { g_blend->Release(); g_blend = nullptr; }
    if (g_sampler) { g_sampler->Release(); g_sampler = nullptr; }
    if (g_samplerMirror) { g_samplerMirror->Release(); g_samplerMirror = nullptr; }
    if (g_raster) { g_raster->Release(); g_raster = nullptr; }
    if (g_depth) { g_depth->Release(); g_depth = nullptr; }
}

} // namespace bvr::blit
