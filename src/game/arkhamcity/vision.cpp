#include "game/arkhamcity/vision.h"
#include "core/ui/overlay.h"

#include "game/arkhamcity/billboard.h"
#include "game/arkhamcity/dlss_bridge.h"
#include "game/arkhamcity/glow.h"
#include "game/arkhamcity/screen_hd.h"
#include "game/arkhamcity/reflect.h"

#include "core/gfx/frame_inspector.h"
#include "core/hooks/d3d11_hook.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <imgui.h>
#include <intrin.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace bvr::ac::vision {
namespace {

// --- settings ---------------------------------------------------------------------------
std::atomic<bool> g_vigOnZoom{true}, g_vigOnDetective{true};
std::atomic<bool> g_hideZoomStencil{true};
std::atomic<bool> g_hideDetOverlay{false}; // test switch: `bc vis overlay hide|show`
std::atomic<float> g_zoomBelowDeg{50.0f}; // zoom = the game's own FOV narrower than this

// --- state ---------------------------------------------------------------------------------
std::atomic<float> g_gameHfov{0.0f}, g_gameHfovMin{0.0f}, g_gameHfovMax{0.0f};
std::atomic<bool> g_zoom{false}; // the game's fov is below the zoom threshold
// The zoom frame's two 2D draws were seen at this present. The menu camera is also narrow
// (LIVE 2026-10-03: "zoom ON (game fov 37)" on the title screen), so zoom needs BOTH.
std::atomic<uint64_t> g_stencilSeenPresent{0};
bool g_zoomWas = false; // game thread, for the log
std::atomic<uint64_t> g_detectiveSeenPresent{0}; // present count when the detective pass last ran
std::atomic<uint32_t> g_detectivePasses{0}, g_stencilSkips{0}, g_patches{0};
float g_amt = 0.0f; // fade 0..1 (game thread)

// Detective final pass: its pixel-shader constants (40 floats, 160 bytes), remembered from the
// game's own upload each frame so they can be re-uploaded with chosen values changed.
ID3D11Buffer* g_detBuf = nullptr; // weak identity
float g_detCache[40] = {};
std::atomic<bool> g_detCacheValid{false};
struct Override { int idx; float val; };
Override g_overrides[8];
std::atomic<int> g_overrideCount{0};

bool zoom_active() {
    const uint64_t seen = g_stencilSeenPresent.load(std::memory_order_relaxed);
    return g_zoom.load(std::memory_order_relaxed) && seen && bvr::d3d11_hook::present_count() - seen < 6;
}
// DETSTEADY (JJ 2026-10-08: "random vignettes appearing around the edges ... when looking around"; the
// vignette layer came up at the start of gameplay with no zoom): something else matched the detective
// signature now and then. Real detective mode runs its pass every frame, so the vignette now needs the
// pass in a run of presents (kDetRun), not a single one.
constexpr uint32_t kDetRun = 12;
std::atomic<uint64_t> g_detRunStart{0}; // present count where the current unbroken run of detections began
std::atomic<uint32_t> g_detW{0}, g_detH{0};
bool g_detWas = false; // game thread, for the log
bool detective_active() {
    const uint64_t seen = g_detectiveSeenPresent.load(std::memory_order_relaxed);
    const uint64_t start = g_detRunStart.load(std::memory_order_relaxed);
    return seen && bvr::d3d11_hook::present_count() - seen < 6 && seen - start >= kDetRun;
}

uint32_t buffer_bytes(ID3D11Buffer* b) {
    if (!b) return 0;
    D3D11_BUFFER_DESC d{};
    b->GetDesc(&d);
    return d.ByteWidth;
}

void __cdecl upload_tap(ID3D11Resource* dst, const void* data, unsigned) {
    billboard::on_upload(dst, data); // BILLBOARD: particle sprite axes -> the game camera's
    if (dst && dst == static_cast<ID3D11Resource*>(g_detBuf)) {
        memcpy(g_detCache, data, sizeof(g_detCache));
        g_detCacheValid.store(true, std::memory_order_relaxed);
    }
}

// Detective mode's own screen overlay (the dark circle + arcs along the top): one Draw of 78
// vertices straight onto the finished picture, after the detective final pass (frame dump
// 2026-10-03 15:53; absent from every non-detective dump). Screen-fixed in each eye, so with
// each eye's own lens shape the two circles land in different directions and never fuse (JJ
// 2026-10-03). UNCONFIRMED: skipping it (and, before that, a camera-attached 256x128 quad)
// left the circle in place on the fake headset; off by default until the real source is found.
std::atomic<uint32_t> g_detOverlaySkips{0};

// JJ 2026-10-04: a black box over the top left of the loading screen (the objectives text cut
// off). Frame dump 044250 (loading): the only UI draw on the final picture is an UNTEXTURED
// DrawIndexed 36 with depth - present in no gameplay dump. Skipped while a full-screen movie
// (loading screen, logos) plays.
std::atomic<bool> g_moviePlaying{false};
std::atomic<uint32_t> g_loadBoxSkips{0};

// LOADBOX2 (JJ 2026-10-09 capture 20261009_153608: "that piece of the loading screen up in the top left is still being
// cut out by some sort of mask or a box"). Frame dumps 153608 q0..q2: after ALL the objectives text the loading movie
// draws one more Scaleform piece anchored at the stage's top-left corner - VS cb0 translation (-1.0, +0.702) - a
// solid-fill rectangle (DrawIndexed 30, 4x4 fill texture; in another frame the 64x64 bat sprite) plus, before the
// text, a filter composite of it (Draw 4 from a 128x128 offscreen target, same translation). Opaque black on top of
// the text = the box. While a full-screen movie plays, Scaleform pieces at that anchor (x = -1, upper half) are not
// drawn. 1 = on (default), 0 = off (`bc vis loadbox2 0|1`).
std::atomic<int> g_loadBox2{1};
std::atomic<uint32_t> g_loadBox2Skips{0};
bool top_left_piece(ID3D11DeviceContext* ctx) {
    ID3D11Buffer* cb = nullptr;
    ctx->VSGetConstantBuffers(0, 1, &cb);
    if (!cb) return false;
    float m[16] = {};
    const unsigned len = bvr::frame_inspector::cb_shadow(cb, m, sizeof(m));
    cb->Release();
    return len >= 64 && m[15] == 1.0f && m[3] == 0.0f && m[7] == 0.0f && fabsf(m[12] + 1.0f) < 0.002f &&
           m[13] > 0.3f && m[13] < 1.0f;
}
bool loadbox2_skip(ID3D11DeviceContext* ctx, unsigned count, bool indexed) {
    if (!g_loadBox2.load(std::memory_order_relaxed) || !g_moviePlaying.load(std::memory_order_relaxed)) return false;
    if (indexed ? count > 30 : count != 4) return false;
    if (!top_left_piece(ctx)) return false;
    if (g_loadBox2Skips.fetch_add(1) < 4)
        BVR_LOG("[bc] loading screen: top-left piece not drawn (%s %u)", indexed ? "DrawIndexed" : "Draw", count);
    return true;
}

// Test switch (`bc vis movieskip N`, -1 = off): skip the Nth untextured-or-RT-sourced
// DrawIndexed(30) rectangle of each movie frame - to find the loading screen's black box.
std::atomic<int> g_movieSkipIdx{-1};
uint64_t g_movieRectPresent = 0;
int g_movieRectK = 0;
bool is_plain_rect(ID3D11DeviceContext* ctx) {
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv) return true;
    bool rt = false;
    ID3D11Resource* r = nullptr;
    srv->GetResource(&r);
    ID3D11Texture2D* t = nullptr;
    if (r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) && t) {
        D3D11_TEXTURE2D_DESC d{};
        t->GetDesc(&d);
        rt = (d.BindFlags & D3D11_BIND_RENDER_TARGET) != 0;
        t->Release();
    }
    if (r) r->Release();
    srv->Release();
    return rt;
}

// NOFXAA (DLSS_PLAN.md D2): the game's FXAA (MultisampleMode=FXAA-HI) is the last scene pass - a full-screen quad
// from the finished picture (same size and format, frame dump 2026-10-08 event 1842, caller 0x5A9780) into the
// swapchain backbuffer (render-target-only). DLSS needs the picture before FXAA, so under DLSS the pass becomes a
// plain copy. Switch: -1 = with DLSS (default), 0 = never, 1 = always (test).
std::atomic<int> g_noFxaa{-1};
std::atomic<uint32_t> g_fxaaSkips{0};
// The FXAA draw itself still runs (the HUD classifier keys on it: skipping it moved the tracker gadget, run 18);
// right after it, the unfiltered picture is copied back over its result.
ID3D11Texture2D* g_fxaaDst = nullptr;
ID3D11Texture2D* g_fxaaSrc = nullptr;
D3D11_BOX g_fxaaBox{};
void fxaa_after(ID3D11DeviceContext* ctx) {
    if (!g_fxaaDst) return;
    ctx->CopySubresourceRegion(g_fxaaDst, 0, 0, 0, 0, g_fxaaSrc, 0, &g_fxaaBox);
    g_fxaaDst->Release();
    g_fxaaSrc->Release();
    g_fxaaDst = g_fxaaSrc = nullptr;
}
bool fxaa_pass(ID3D11DeviceContext* ctx) {
    const int sw = g_noFxaa.load(std::memory_order_relaxed);
    if (sw == 0 || (sw < 0 && (dlss_bridge::mode() <= 0 || !dlss_bridge::launched()))) return false;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, &dsv);
    if (dsv) dsv->Release();
    if (!rtv) return false;
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    ID3D11Resource* dr = nullptr;
    ID3D11Resource* sr = nullptr;
    rtv->GetResource(&dr);
    if (srv) srv->GetResource(&sr);
    bool hit = false;
    ID3D11Texture2D* dt = nullptr;
    ID3D11Texture2D* st = nullptr;
    if (dr && sr && dr != sr && SUCCEEDED(dr->QueryInterface(IID_PPV_ARGS(&dt))) &&
        SUCCEEDED(sr->QueryInterface(IID_PPV_ARGS(&st)))) {
        D3D11_TEXTURE2D_DESC a{}, b{};
        dt->GetDesc(&a);
        st->GetDesc(&b);
        ID3D11Buffer* vcb = nullptr;
        ctx->VSGetConstantBuffers(0, 1, &vcb);
        const uint32_t vb = buffer_bytes(vcb);
        if (vcb) vcb->Release();
        static std::atomic<int> s_logged{0};
        if (a.Width >= 1000 && !(a.BindFlags & D3D11_BIND_SHADER_RESOURCE) && s_logged.fetch_add(1) < 6)
            BVR_LOG("[bc] FXAA candidate: target %ux%u fmt %d bind 0x%x samples %u <- source %ux%u fmt %d bind 0x%x "
                    "samples %u, VS constants %u bytes",
                    a.Width, a.Height, int(a.Format), a.BindFlags, a.SampleDesc.Count, b.Width, b.Height, int(b.Format),
                    b.BindFlags, b.SampleDesc.Count, vb);
        // The game's scene targets are padded (run 18: 1240x1328 source for a 1240x1324 backbuffer).
        hit = !(a.BindFlags & D3D11_BIND_SHADER_RESOURCE) && a.Width >= 1000 && b.Width >= a.Width &&
              b.Width <= a.Width + 16 && b.Height >= a.Height && b.Height <= a.Height + 16 && a.Format == b.Format &&
              a.SampleDesc.Count == 1 && b.SampleDesc.Count == 1 && vb != 640; // never detective mode's final pass
        if (hit) {
            const D3D11_BOX box{0, 0, 0, a.Width, a.Height, 1};
            if (g_fxaaDst) { g_fxaaDst->Release(); g_fxaaSrc->Release(); }
            g_fxaaDst = dt; g_fxaaDst->AddRef();
            g_fxaaSrc = st; g_fxaaSrc->AddRef();
            g_fxaaBox = box;
            if (g_fxaaSkips.fetch_add(1, std::memory_order_relaxed) == 0)
                BVR_LOG("[bc] FXAA pass %ux%u replaced by a plain copy (DLSS does the anti-aliasing)", a.Width, a.Height);
        }
    }
    if (st) st->Release();
    if (dt) dt->Release();
    if (sr) sr->Release();
    if (dr) dr->Release();
    if (srv) srv->Release();
    rtv->Release();
    return hit;
}

void draw_after(ID3D11DeviceContext* ctx) {
    screen_hd::after_draw(ctx); // HD SCREENS: the game's picture back
    fxaa_after(ctx);
    glow::after_draw(ctx);
    glow::after_draw_plain(ctx);
}

void detvr_after(ID3D11DeviceContext* ctx); // DETVR, below
void detvr_draw(ID3D11DeviceContext* ctx, unsigned count, bool indexed);
void __cdecl draw_after_plain(ID3D11DeviceContext* ctx) { detvr_after(ctx); screen_hd::after_draw(ctx); glow::after_draw_plain(ctx); }
bool draw_indexed_inner(ID3D11DeviceContext* ctx, unsigned indexCount);
// HD SCREENS: the upscaled loading-screen picture bound for this draw (put back in draw_after, or at once when skipped)
bool __cdecl draw_indexed_hook(ID3D11DeviceContext* ctx, unsigned indexCount) {
    screen_hd::on_draw(ctx, g_moviePlaying.load(std::memory_order_relaxed));
    const bool skip = draw_indexed_inner(ctx, indexCount);
    if (skip) screen_hd::after_draw(ctx);
    return skip;
}
bool draw_indexed_inner(ID3D11DeviceContext* ctx, unsigned indexCount) {
    reflect::on_draw_indexed(ctx); // REFLECTEYE: the right eye's puddle reflection (never drops the draw)
    if (glow::on_draw_plain(ctx, indexCount)) return true; // the game's glow sprites (they are indexed draws)
    if (loadbox2_skip(ctx, indexCount, true)) return true;
    if (indexCount == 30 && g_moviePlaying.load(std::memory_order_relaxed) && g_movieSkipIdx.load() >= 0 &&
        is_plain_rect(ctx)) {
        const uint64_t pc = bvr::d3d11_hook::present_count();
        if (pc != g_movieRectPresent) { g_movieRectPresent = pc; g_movieRectK = 0; }
        if (g_movieRectK++ == g_movieSkipIdx.load()) return true;
    }
    if (indexCount == 36 && g_moviePlaying.load(std::memory_order_relaxed)) {
        ID3D11ShaderResourceView* srv = nullptr;
        ctx->PSGetShaderResources(0, 1, &srv);
        if (srv) { srv->Release(); return false; }
        if (g_loadBoxSkips.fetch_add(1) == 0) BVR_LOG("[bc] loading screen: untextured box draw skipped");
        return true;
    }
    // Zoom stencil: two 2D draws sampling a 128x128 BC3 texture used nowhere else (frame dump
    // 2026-10-03: DrawIndexed 30 + 90 after the scene, zoom only).
    if ((indexCount == 30 || indexCount == 90) && g_zoom.load(std::memory_order_relaxed)) {
        ID3D11ShaderResourceView* srv = nullptr;
        ctx->PSGetShaderResources(0, 1, &srv);
        bool hit = false;
        if (srv) {
            ID3D11Resource* r = nullptr;
            srv->GetResource(&r);
            if (r) {
                ID3D11Texture2D* t = nullptr;
                r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t));
                if (t) {
                    D3D11_TEXTURE2D_DESC d{};
                    t->GetDesc(&d);
                    hit = d.Width == 128 && d.Height == 128 && d.Format == DXGI_FORMAT_BC3_UNORM;
                    t->Release();
                }
                r->Release();
            }
            srv->Release();
        }
        if (!hit) return false;
        const uint64_t pc = bvr::d3d11_hook::present_count();
        g_stencilSeenPresent.store(pc ? pc : 1, std::memory_order_relaxed);
        if (!g_hideZoomStencil.load(std::memory_order_relaxed)) return false;
        g_stencilSkips.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    // Detective mode's final pass: a full-screen quad with no depth buffer, VS constants 640
    // bytes and PS constants 160 bytes (the normal final pass has 160 / 160).
    if (indexCount != 6) return false;
    glow::note_fullscreen_pass(ctx);
    if (glow::on_draw(ctx)) return false; // GLOW: our scene / glow inputs bound, put back after the draw
    if (fxaa_pass(ctx)) return false; // restored right after the draw (fxaa_after)
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, &dsv);
    if (rtv) rtv->Release();
    if (dsv) { dsv->Release(); return false; }
    ID3D11Buffer* vcb = nullptr;
    ctx->VSGetConstantBuffers(0, 1, &vcb);
    const uint32_t vbytes = buffer_bytes(vcb);
    if (vcb) vcb->Release();
    if (vbytes != 640) return false;
    ID3D11Buffer* pcb = nullptr;
    ctx->PSGetConstantBuffers(0, 1, &pcb);
    if (!pcb) return false;
    // DETSIZE (2026-10-08 log): the false match drew into a 256x16 target (the exposure measure, which changes
    // with where you look). Detective mode's final pass covers the screen.
    {
        ID3D11RenderTargetView* cur = nullptr;
        ID3D11DepthStencilView* dd = nullptr;
        ctx->OMGetRenderTargets(1, &cur, &dd);
        if (dd) dd->Release();
        UINT w = 0;
        if (cur) {
            ID3D11Resource* rr = nullptr;
            cur->GetResource(&rr);
            ID3D11Texture2D* tt = nullptr;
            if (rr && SUCCEEDED(rr->QueryInterface(IID_PPV_ARGS(&tt))) && tt) {
                D3D11_TEXTURE2D_DESC td{};
                tt->GetDesc(&td);
                w = td.Width;
                tt->Release();
            }
            if (rr) rr->Release();
            cur->Release();
        }
        if (w < 1000) {
            pcb->Release();
            return false;
        }
    }
    if (buffer_bytes(pcb) == 160) {
        g_detBuf = pcb;
        const uint64_t pc = bvr::d3d11_hook::present_count() ? bvr::d3d11_hook::present_count() : 1;
        // A gap of more than 4 presents since the last detection starts a new run (both eyes draw it).
        if (pc - g_detectiveSeenPresent.load(std::memory_order_relaxed) > 4) g_detRunStart.store(pc, std::memory_order_relaxed);
        if (rtv) {
            ID3D11Resource* rr = nullptr;
            ID3D11RenderTargetView* cur = nullptr;
            ID3D11DepthStencilView* dd = nullptr;
            ctx->OMGetRenderTargets(1, &cur, &dd);
            if (dd) dd->Release();
            if (cur) {
                cur->GetResource(&rr);
                ID3D11Texture2D* tt = nullptr;
                if (rr && SUCCEEDED(rr->QueryInterface(IID_PPV_ARGS(&tt))) && tt) {
                    D3D11_TEXTURE2D_DESC td{};
                    tt->GetDesc(&td);
                    g_detW.store(td.Width, std::memory_order_relaxed);
                    g_detH.store(td.Height, std::memory_order_relaxed);
                    tt->Release();
                }
                if (rr) rr->Release();
                cur->Release();
            }
        }
        g_detectiveSeenPresent.store(pc, std::memory_order_relaxed);
        g_detectivePasses.fetch_add(1, std::memory_order_relaxed);
        const int n = g_overrideCount.load(std::memory_order_relaxed);
        if (n > 0 && g_detCacheValid.load(std::memory_order_relaxed)) {
            float patched[40];
            memcpy(patched, g_detCache, sizeof(patched));
            for (int i = 0; i < n; ++i)
                if (g_overrides[i].idx >= 0 && g_overrides[i].idx < 40) patched[g_overrides[i].idx] = g_overrides[i].val;
            ctx->UpdateSubresource(pcb, 0, nullptr, patched, 0, 0);
            g_patches.fetch_add(1, std::memory_order_relaxed);
        }
    }
    pcb->Release();
    return false;
}

// --- DETVR: detective vision's screen overlay, fused for VR ------------------------------------------------------
// JJ 2026-10-12: "detective mode still has a separate vignette for each eye, whereas they should fuse into one". The
// detective look is one screen material (DXBC checksum 9f1079e5fc4a7d14; found by a pass-hiding hunt: hiding it lifted the
// picture corners x5.6). Disassembled (re/detvig_9f1079e5fc4a7d14.dxbc): it lays its overlay layers - t2 scanlines (c2.xy =
// their scroll), t3 honeycomb + vignette (at UV x2, mirrored), t4 / t5 grain - on each eye's own off-centre screen, so none
// fuses. Skipping the pass flashed (the scroll offset was mistaken for a detective flag) and whitening t3 lost the honeycomb.
// The fix: the shader is rebuilt (re/detvr.hlsl, identical signature) with the overlay layers sampled at a SHARED view
// direction - each eye's screen position -> tangent of its own frustum -> position in the union frustum of both eyes - so
// both eyes see one overlay (JJ: "This is perfect"). Scene and depth outlines stay the game's. Swapped for that one draw.
constexpr unsigned long long kDetVrShader = 0x9f1079e5fc4a7d14ull;
std::atomic<bool> g_detVrOn{true};
const char kDetVrHlsl[] = R"HLSL(// DETVR: Arkham City's detective-vision screen material (DXBC 9f1079e5fc4a7d14), rebuilt from its disassembly, with the
// overlay textures (t2 scanlines, t3 honeycomb + vignette, t4 / t5 grain) sampled at a SHARED view direction instead of
// each eye's own screen position, so both eyes see the same overlay and it fuses. vrRemap: uv' = uv * xy + zw.
cbuffer CB0 : register(b0) { float4 c0[15]; };
cbuffer CB2 : register(b2) { float4 c2[5]; };
cbuffer VR : register(b4) { float4 vrRemap; };
Texture2D t0 : register(t0);
Texture2D t1 : register(t1);
Texture2D t2 : register(t2);
Texture2D t3 : register(t3);
Texture2D t4 : register(t4);
Texture2D t5 : register(t5);
Texture2D t6 : register(t6);
SamplerState s0 : register(s0);
SamplerState s1 : register(s1);
SamplerState s2 : register(s2);
SamplerState s3 : register(s3);
SamplerState s4 : register(s4);
SamplerState s5 : register(s5);
SamplerState s6 : register(s6);

struct PSIn {
    float4 col : COLOR0;
    float4 tc0 : TEXCOORD0;
    float4 v2 : TEXCOORD4;
    float4 tc6 : TEXCOORD6;
    float4 v4 : TEXCOORD5;
    bool ff : SV_IsFrontFace;
};
struct PSOut {
    float4 o0 : SV_Target0;
    float4 o1 : SV_Target1;
    float4 o2 : SV_Target2;
};

PSOut main(PSIn i) {
    const bool flag = 0.0 != c2[4].x;
    float3 p = i.v4.xyw; // (x, y, w)
    if (flag) {
        const float2 a = t0.Sample(s1, float2(0.0625, 0.0)).xy;
        const float x = a.x * (i.v4.w - a.y) + i.v4.x;
        p = float3(i.v4.w != 1.0 ? x : i.v4.x, i.v4.y, i.v4.w);
    }
    float4 r1 = float4(p.xy / p.z, p.xy / p.z);
    r1 = r1 * c2[0].xyxy + c2[0].wzwz;
    const float2 uv = r1.zw;
    const float2 ov = uv * vrRemap.xy + vrRemap.zw; // DETVR: the shared overlay position

    float3 r2 = t1.Sample(s0, uv).xyz;
    float3 r3 = sqrt(r2) - r2;
    r2 = c0[5].x * r3 + r2;
    const float lum = dot(r2, float3(0.666, 0.25075, 0.08325));
    r3 = lum * c0[1].xyz - r2;
    r2 = c0[5].y * r3 + r2;
    const float scan = t2.SampleBias(s2, ov * float2(4.0, 12.0) + c0[2].xy, 0.0).x;
    r2 *= scan;
    const float3 hex = t3.SampleBias(s3, ov + ov, 0.0).xyz;
    float g = saturate(t4.SampleBias(s4, ov * float2(24.0, 18.0), 0.0).z + hex.x);
    g = g + g;
    r2 *= hex * g;
    float3 n = t5.SampleBias(s5, float2(ov.x * 4.0 + c0[12].w, ov.y * 4.0 + c0[13].x), 0.0).xyz;
    n = n * c0[13].y + 1.0;

    const float dz = c2[1].z, dw = c2[1].w;
    float4 r5;
    r5.x = 1.0 / (t1.SampleLevel(s0, float2(uv.x + 0.000587, uv.y), 0.0).w * dz - dw);
    r5.z = 1.0 / (t1.SampleLevel(s0, float2(uv.x, uv.y + 0.000695), 0.0).w * dz - dw);
    const float dc = 1.0 / (t1.SampleLevel(s0, uv, 0.0).w * dz - dw);
    r5.y = 1.0 / (t1.SampleLevel(s0, float2(uv.x - 0.000587, uv.y), 0.0).w * dz - dw);
    r5.w = 1.0 / (t1.SampleLevel(s0, float2(uv.x, uv.y - 0.000695), 0.0).w * dz - dw);

    const float k = dc / p.z;
    float qx = k * p.x, qy = k * p.z;
    if (flag) {
        const float2 a = t0.Sample(s1, float2(0.0625, 0.0)).xy;
        const float z = -a.x * (p.z * k - a.y) + qx;
        qx = qy != 1.0 ? z : qx;
    }
    const float ex = (qx / qy) * c2[0].x + c2[0].w;
    const float edge = t6.SampleBias(s6, float2(ex + c0[3].x, dc * 0.001 + c0[3].y), 0.0).y;
    const float4 m = min(r5, c0[14].wwww) - min(dc, c0[14].w);
    float e = dot(m, float4(0.25, 0.25, 0.25, 0.25)) * (c0[14].z / dc);
    e = saturate(edge * e);
    e = e * (1.0 - e);
    float3 r0 = c0[4].xyz * c0[4].w * e;
    r0 = r2 * n + r0;
    r0 += c0[0].xyz;
    PSOut o;
    o.o0 = float4(r0 * i.v2.w + i.v2.xyz, 1.0);
    o.o1 = 0;
    o.o2 = 0;
    return o;
}
)HLSL";
ID3D11PixelShader* g_detVrPs = nullptr;   // render thread
bool g_detVrTried = false;
ID3D11Buffer* g_detVrCb = nullptr;        // b4: the remap
ID3D11PixelShader* g_detVrGamePs = nullptr;
ID3D11Buffer* g_detVrGameCb = nullptr;
bool g_detVrSwapped = false;
std::atomic<int> g_passEye{0};            // render thread: -1 left, +1 right, 0 mono (scene begin_exec)
std::atomic<uint32_t> g_detVrDraws{0};
void detvr_remap(float out[4]) {
    out[0] = 1.0f; out[1] = 1.0f; out[2] = 0.0f; out[3] = 0.0f;
    const int sign = g_passEye.load(std::memory_order_relaxed);
    float a[2][4];
    if (!sign || !bvr::vr::eye_fov_angles(0, a[0]) || !bvr::vr::eye_fov_angles(1, a[1]))
        return;
    float t[2][4];
    for (int e = 0; e < 2; ++e)
        for (int k = 0; k < 4; ++k) t[e][k] = tanf(a[e][k]);
    const float L = (std::min)(t[0][0], t[1][0]), R = (std::max)(t[0][1], t[1][1]);
    const float U = (std::max)(t[0][2], t[1][2]), D = (std::min)(t[0][3], t[1][3]);
    const float* e = t[sign < 0 ? 0 : 1];
    if (R - L < 1e-4f || U - D < 1e-4f) return;
    out[0] = (e[1] - e[0]) / (R - L);
    out[2] = (e[0] - L) / (R - L);
    out[1] = (e[2] - e[3]) / (U - D);
    out[3] = (U - e[2]) / (U - D);
}
bool detvr_ready(ID3D11DeviceContext* ctx) {
    if (g_detVrPs && g_detVrCb) return true;
    if (g_detVrTried) return false;
    g_detVrTried = true;
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    const HRESULT hr = D3DCompile(kDetVrHlsl, sizeof(kDetVrHlsl) - 1, "detvr", nullptr, nullptr, "main", "ps_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr) || !code) {
        BVR_LOG("[bc] DETVR: compile FAILED %08X %s", unsigned(hr), err ? static_cast<const char*>(err->GetBufferPointer()) : "");
        if (err) err->Release();
        if (code) code->Release();
        return false;
    }
    if (err) err->Release();
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (dev) {
        dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_detVrPs);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        dev->CreateBuffer(&bd, nullptr, &g_detVrCb);
        dev->Release();
    }
    code->Release();
    BVR_LOG("[bc] DETVR: detective screen shader rebuilt %s", g_detVrPs && g_detVrCb ? "and ready" : "but NOT created");
    return g_detVrPs && g_detVrCb;
}
// Before a Draw(3): the detective screen material gets the rebuild (put back in detvr_after). Never drops the draw.
void detvr_draw(ID3D11DeviceContext* ctx, unsigned count, bool indexed) {
    if (indexed || count != 3 || !g_detVrOn.load(std::memory_order_relaxed)) return;
    ID3D11PixelShader* ps = nullptr;
    ctx->PSGetShader(&ps, nullptr, nullptr);
    if (!ps) return;
    static ID3D11PixelShader* s_ps[8] = {};
    static unsigned long long s_chk[8] = {};
    unsigned long long chk = 0;
    const unsigned slot = static_cast<unsigned>(reinterpret_cast<uintptr_t>(ps) >> 4) & 7;
    if (s_ps[slot] == ps) chk = s_chk[slot];
    else if (glow::ps_checksum(ps, &chk)) { s_ps[slot] = ps; s_chk[slot] = chk; }
    if (chk != kDetVrShader || !detvr_ready(ctx)) { ps->Release(); return; }
    float r[4];
    detvr_remap(r);
    ctx->UpdateSubresource(g_detVrCb, 0, nullptr, r, 0, 0);
    g_detVrGamePs = ps; // reference kept until detvr_after
    ctx->PSGetConstantBuffers(4, 1, &g_detVrGameCb);
    ctx->PSSetShader(g_detVrPs, nullptr, 0);
    ctx->PSSetConstantBuffers(4, 1, &g_detVrCb);
    g_detVrSwapped = true;
    g_detVrDraws.fetch_add(1, std::memory_order_relaxed);
}
void detvr_after(ID3D11DeviceContext* ctx) {
    if (!g_detVrSwapped) return;
    g_detVrSwapped = false;
    ctx->PSSetShader(g_detVrGamePs, nullptr, 0);
    ctx->PSSetConstantBuffers(4, 1, &g_detVrGameCb);
    if (g_detVrGamePs) { g_detVrGamePs->Release(); g_detVrGamePs = nullptr; }
    if (g_detVrGameCb) { g_detVrGameCb->Release(); g_detVrGameCb = nullptr; }
}

} // namespace

std::atomic<int> g_vpLogReset{0};
bool movie_playing() { return g_moviePlaying.load(std::memory_order_relaxed); }
void set_movie_playing(bool on) {
    g_moviePlaying.store(on, std::memory_order_relaxed);
    if (on) g_vpLogReset.fetch_add(1);
}

// Probe (2026-10-04, the loading-screen black box): while a movie plays, log each distinct
// viewport the game sets and who set it - the movie player's own size decides its layout.
bool __cdecl viewport_tap(ID3D11DeviceContext*, unsigned n, void* vpsRaw) {
    D3D11_VIEWPORT* vps = static_cast<D3D11_VIEWPORT*>(vpsRaw);
    if (!g_moviePlaying.load(std::memory_order_relaxed)) return false;
    static uint32_t seen[32] = {};
    static int nSeen = 0;
    static int s_reset = 0;
    if (s_reset != g_vpLogReset.load()) { s_reset = g_vpLogReset.load(); nSeen = 0; }
    const uint32_t key = (static_cast<uint32_t>(vps[0].Width) << 16) ^ static_cast<uint32_t>(vps[0].Height) ^
                         (static_cast<uint32_t>(vps[0].TopLeftX) << 8) ^ (static_cast<uint32_t>(vps[0].TopLeftY) << 4);
    for (int i = 0; i < nSeen; ++i)
        if (seen[i] == key) return false;
    if (nSeen >= 32) return false;
    seen[nSeen++] = key;
    // Stack scan (the game is built without frame pointers): every dword that points into the exe's
    // code just after a CALL is a likely return address.
    char stk[400] = {};
    int len = 0, found = 0;
    const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(_AddressOfReturnAddress());
    for (int i = 0; i < 1500 && found < 24 && len < 380; ++i) {
        uintptr_t a = 0;
        __try { a = sp[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (a < 0x401005 || a >= 0x1201000) continue;
        bool call = false;
        __try {
            const uint8_t* c = reinterpret_cast<const uint8_t*>(a);
            call = c[-5] == 0xE8 || c[-2] == 0xFF || c[-3] == 0xFF || c[-6] == 0xFF;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (!call) continue;
        len += sprintf_s(stk + len, sizeof(stk) - len, " %06X", unsigned(a));
        ++found;
    }
    BVR_LOG("[bc] movie viewport %ux? %.0fx%.0f at (%.0f, %.0f) - set by%s", n, vps[0].Width, vps[0].Height,
            vps[0].TopLeftX, vps[0].TopLeftY, stk);
    return false;
}

bool draw_skip(ID3D11DeviceContext* ctx, unsigned vertexCount) {
    if (glow::on_draw_plain(ctx, vertexCount)) return true;
    if (loadbox2_skip(ctx, vertexCount, false)) return true;
    detvr_draw(ctx, vertexCount, false); // DETVR: after every rule that can drop the draw (it is put back after the draw)
    if (vertexCount != 78 || !g_hideDetOverlay.load(std::memory_order_relaxed) || !detective_active()) return false;
    g_detOverlaySkips.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Saved settings (scene owns the file): one "key=value" line each.
void settings_write(std::string& out) {
    char line[256];
    float s = 0, c = 0, r = 0;
    bvr::vr::get_vignette_look(&s, &c, &r);
    sprintf_s(line, "vig_on_zoom=%d\nvig_on_detective=%d\nhide_zoom_frame=%d\nzoom_below_deg=%.2f\n"
                    "vig_strength=%.3f\nvig_clear_deg=%.2f\nvig_fade_deg=%.2f\n",
              g_vigOnZoom.load() ? 1 : 0, g_vigOnDetective.load() ? 1 : 0, g_hideZoomStencil.load() ? 1 : 0,
              g_zoomBelowDeg.load(), s, c, r);
    out += line;
    sprintf_s(line, "detective_overlay_fused=%d\n", g_detVrOn.load() ? 1 : 0); // DETVR
    out += line;
}

bool settings_read(const char* key, const char* val) {
    const float f = static_cast<float>(atof(val));
    const bool on = atoi(val) != 0;
    float s = 0, c = 0, r = 0;
    bvr::vr::get_vignette_look(&s, &c, &r);
    if (!strcmp(key, "vig_on_zoom")) g_vigOnZoom.store(on);
    else if (!strcmp(key, "vig_on_detective")) g_vigOnDetective.store(on);
    else if (!strcmp(key, "detective_overlay_fused")) g_detVrOn.store(on); // DETVR
    else if (!strncmp(key, "det_vignette", 12)) {} // test builds of 2026-10-12 - not carried over
    else if (!strcmp(key, "hide_zoom_frame")) g_hideZoomStencil.store(on);
    else if (!strcmp(key, "zoom_below_deg")) { if (f > 5.0f && f < 120.0f) g_zoomBelowDeg.store(f); }
    else if (!strcmp(key, "vig_strength")) bvr::vr::set_vignette_look(f < 0 ? 0 : (f > 1 ? 1 : f), c, r);
    else if (!strcmp(key, "vig_clear_deg")) bvr::vr::set_vignette_look(s, f, r);
    else if (!strcmp(key, "vig_fade_deg")) bvr::vr::set_vignette_look(s, c, f);
    else return false;
    return true;
}

void install() {
    bvr::frame_inspector::set_draw_indexed_hook(&draw_indexed_hook);
    bvr::frame_inspector::set_draw_indexed_after(&draw_after);
    bvr::frame_inspector::set_draw_after(&draw_after_plain);
    bvr::frame_inspector::set_upload_tap(&upload_tap);
    bvr::frame_inspector::set_viewport_tap(&viewport_tap);
    BVR_LOG("[bc] vision: zoom / detective vignette hooks armed");
}

void on_game_fov(float hfovDeg) {
    if (!(hfovDeg > 1.0f && hfovDeg < 179.0f)) return;
    g_gameHfov.store(hfovDeg, std::memory_order_relaxed);
    if (g_gameHfovMin.load() <= 0.0f || hfovDeg < g_gameHfovMin.load()) g_gameHfovMin.store(hfovDeg);
    if (hfovDeg > g_gameHfovMax.load()) g_gameHfovMax.store(hfovDeg);
    const bool z = hfovDeg < g_zoomBelowDeg.load(std::memory_order_relaxed);
    g_zoom.store(z, std::memory_order_relaxed);
}

void tick() {
    if (detective_active() != g_detWas) {
        g_detWas = !g_detWas;
        BVR_LOG("[bc] vision: detective %s (pass into %ux%u, %u passes so far)", g_detWas ? "ON" : "off", g_detW.load(),
                g_detH.load(), g_detectivePasses.load());
    }
    if (zoom_active() != g_zoomWas) {
        g_zoomWas = !g_zoomWas;
        BVR_LOG("[bc] vision: zoom %s (game fov %.1f deg)", g_zoomWas ? "ON" : "off", g_gameHfov.load());
    }
    const bool want = (g_vigOnZoom.load() && zoom_active()) ||
                      (g_vigOnDetective.load() && detective_active());
    const float step = 1.0f / 8.0f; // ~0.1 s at 90 Hz
    g_amt = want ? (g_amt + step > 1.0f ? 1.0f : g_amt + step) : (g_amt - step < 0.0f ? 0.0f : g_amt - step);
    bvr::vr::set_vignette(g_amt);
}

void set_render_pass(int eyeSign) { g_passEye.store(eyeSign, std::memory_order_relaxed); } // DETVR
bool handle_command(const char* args) {
    // "vis set <idx> <val>" (detective constant override), "vis clear", "vis zoom <deg>", "vis detoverlay 0|1" (DETVR)
    if (!args) return false;
    if (_strnicmp(args, "detoverlay ", 11) == 0) {
        g_detVrOn.store(atoi(args + 11) != 0);
        BVR_LOG("[bc] DETVR: detective overlay %s (rebuilt draws %u)", g_detVrOn.load() ? "fused" : "as the game draws it",
                g_detVrDraws.load());
        return true;
    }
    if (_strnicmp(args, "set ", 4) == 0) {
        int idx = 0;
        float val = 0.0f;
        if (sscanf_s(args + 4, "%d %f", &idx, &val) == 2) {
            const int n = g_overrideCount.load();
            if (n < 8) {
                g_overrides[n] = {idx, val};
                g_overrideCount.store(n + 1);
            }
            BVR_LOG("[bc] vision: detective constant [%d] -> %.4f (was %.4f)", idx, val,
                    idx >= 0 && idx < 40 ? g_detCache[idx] : 0.0f);
        }
        return true;
    }
    if (_strnicmp(args, "overlay ", 8) == 0) { // "vis overlay hide|show" - the game's detective circle
        g_hideDetOverlay.store(_strnicmp(args + 8, "show", 4) != 0);
        return true;
    }
    if (_strnicmp(args, "nofxaa ", 7) == 0) {
        g_noFxaa.store(atoi(args + 7));
        BVR_LOG("[bc] vision: FXAA replaced %d (-1 with DLSS, 0 never, 1 always), %u so far", g_noFxaa.load(),
                g_fxaaSkips.load());
        return true;
    }
    if (_strnicmp(args, "loadbox2 ", 9) == 0) {
        g_loadBox2.store(atoi(args + 9));
        BVR_LOG("[bc] vision: loading-screen top-left piece %s (%u skipped so far)",
                g_loadBox2.load() ? "NOT drawn" : "drawn (game)", g_loadBox2Skips.load());
        return true;
    }
    if (_strnicmp(args, "movieskip ", 10) == 0) {
        g_movieSkipIdx.store(atoi(args + 10));
        BVR_LOG("[bc] vision: movie rect skip index %d", g_movieSkipIdx.load());
        return true;
    }
    if (_strnicmp(args, "clear", 5) == 0) {
        g_overrideCount.store(0);
        BVR_LOG("[bc] vision: detective overrides cleared");
        return true;
    }
    if (_strnicmp(args, "strength ", 9) == 0) { // our full-view vignette (0 = off), for tests
        float s = 0, c = 0, r = 0;
        bvr::vr::get_vignette_look(&s, &c, &r);
        bvr::vr::set_vignette_look(static_cast<float>(atof(args + 9)), c, r);
        return true;
    }
    if (_strnicmp(args, "zoom ", 5) == 0) {
        g_zoomBelowDeg.store(static_cast<float>(atof(args + 5)));
        return true;
    }
    char line[256];
    status_line(line, sizeof(line));
    BVR_LOG("%s", line);
    return true;
}

void status_line(char* out, unsigned n) {
    sprintf_s(out, n, "[bc] vision: game fov %.1f (min %.1f max %.1f, zoom below %.0f) zoom %s detective %s | vignette %.2f | detective passes %u stencil skips %u patches %u overlay skips %u",
              g_gameHfov.load(), g_gameHfovMin.load(), g_gameHfovMax.load(), g_zoomBelowDeg.load(),
              zoom_active() ? "ON" : "off", detective_active() ? "ON" : "off", g_amt, g_detectivePasses.load(),
              g_stencilSkips.load(), g_patches.load(), g_detOverlaySkips.load());
}

void draw_ui() {
    bool vz = g_vigOnZoom.load();
    if (ImGui::Checkbox("vignette while zoomed (right stick click)", &vz)) g_vigOnZoom.store(vz);
    bool vd = g_vigOnDetective.load();
    if (ImGui::Checkbox("vignette in detective mode (left shoulder)", &vd)) g_vigOnDetective.store(vd);
    bool hz = g_hideZoomStencil.load();
    if (ImGui::Checkbox("hide the game's own 2D zoom frame", &hz)) g_hideZoomStencil.store(hz);
    float s = 0, c = 0, r = 0;
    bvr::vr::get_vignette_look(&s, &c, &r);
    bool ch = bvr::overlay::SliderStep("strength  (0 = off)", &s, 0.0f, 1.0f, "%.2f");
    ch |= bvr::overlay::SliderStep("clear centre  (deg)", &c, 5.0f, 60.0f, "%.0f");
    ch |= bvr::overlay::SliderStep("fade width  (deg)", &r, 2.0f, 60.0f, "%.0f");
    if (ch) bvr::vr::set_vignette_look(s, c, r);
    if (bvr::overlay::show_tests()) {
        bool fo = g_detVrOn.load();
        if (ImGui::Checkbox("   detective overlay fused for both eyes", &fo)) g_detVrOn.store(fo); // DETVR
    }
    if (bvr::overlay::show_tests()) ImGui::TextDisabled("   game view %.0f deg - zoom %s, detective %s", g_gameHfov.load(), zoom_active() ? "ON" : "off",
                        detective_active() ? "ON" : "off");
}

} // namespace bvr::ac::vision
