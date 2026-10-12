#include "core/gfx/gfx_hud.h"
#include "core/gfx/blit.h"

#include <cmath>

#include <cstdio>
#include <cstring>
#include <vector>

#include "core/gfx/frame_inspector.h"
#include "core/vr/openxr_runtime.h"
#include "core/util/log.h"

#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <map>
#include <atomic>

namespace bvr::gfx_hud {
namespace {

std::atomic<bool> g_armed{false};
std::atomic<bool> g_redirect{false};
// s52: full-screen-effect pass-through. A GFx damage flash / vignette is a
// tiny-count full-screen quad (<= 12 indices/vertices); leaving those on the
// backbuffer keeps effects across the WHOLE eye image while widgets go to
// the quad. Toggleable because the same shape could be a legit small widget
// - the in-headset A/B decides (bsihud fx on|off).
std::atomic<bool> g_fxPassthrough{true};
constexpr unsigned kFxMaxCount = 12;

// Render-thread state (the draw path is single-threaded per context).
// The backbuffer identity is the CANONICAL IUnknown pointer (COM identity
// rule - interface pointers of the same object are not comparable across
// interfaces). Weak: refreshed per present, never dereferenced.
IUnknown* g_bbUnk = nullptr;
unsigned g_bbW = 0, g_bbH = 0;

IUnknown* canonical(ID3D11Resource* r) {
    IUnknown* u = nullptr;
    if (r && SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&u))) && u) u->Release();
    return u; // weak identity only
}
bool g_onBackbuffer = false;             // current RTV0 is the backbuffer
ID3D11DepthStencilView* g_curDsv = nullptr; // weak, as last bound by the game
ID3D11Resource* g_sceneDepth = nullptr;      // weak: the last full-size readable R24G8 depth the game bound (DLSS)
ID3D11RenderTargetView* g_gameRtv = nullptr; // weak, the game's own rtv0
bool g_pastBoundary = false;             // the eye blit landed this frame
// MAPBLIT (ported from Origins 2026-10-11, MAPTRACE: every leaking eye picture on the map was the scene blit itself,
// carrying the map's own art left in the shared scene picture by the other eye's pass; JJ: City's map flickers the same
// way). While MAPSCREEN says the map is up, the eye is cleared black and the blit goes on the HUD sheet. Default on.
ULONGLONG g_mapUntilMs = 0; // MAPSCREEN: map screen up until (render thread)
std::atomic<bool> g_mapBlit{true};
std::atomic<uint32_t> g_cMapBlit{0};
// MAPBLITTAIL (JJ 2026-10-11 22:16: after closing the map "an overlay of the gameplay in 2D over the 3D for a couple of
// seconds in the location where the map was"): MAPBLIT followed MAPSCREEN's 1.5 s hold, so for 1.5 s after the map
// closed the gameplay picture went on the menu sheet. MAPBLIT now needs a map shape within the last mapblithold ms (the
// map draws them every frame) and a stopped game clock (the map pauses the game; set by the adapter).
ULONGLONG g_mapShapeMs = 0;                 // render thread: the last big untextured shape
constexpr ULONGLONG kMapBlitHoldMs = 250;
std::atomic<bool> g_clockStopped{false};    // adapter: the game clock stands (menus, the map)
std::atomic<ULONGLONG> g_resumedMs{0};      // adapter: the game clock started running again (UNPAUSECUT)
std::atomic<uint32_t> g_unpauseCuts{0};
bool g_ourBound = false;                 // our capture RT is currently bound
bool g_clearedThisFrame = false;
// HUDDSV (ported from AKN 2026-10-09, VR_ESSENTIALS 6; JJ 2026-10-10 SteamVR: the compass "only seems to disappear when
// I'm facing a certain direction ... a big neon sign there ... and then its teal glow ... gets displaced and put above
// it"). The HUD was redirected into our picture WITH the game's scene depth-stencil: Scaleform clips with stencil
// masks, so whatever the 3D scene left in the stencil (the neon sign) broke the compass's clip there. The HUD now
// draws with its own depth-stencil (same size and format as the game's), cleared once per frame at the first
// redirected draw (depth to the game's own scene clear value, stencil 0); the game's clears of its depth-stencil
// during the HUD are mirrored to it. `bc huddsv 0|1`, default on.
std::atomic<bool> g_hudDsvOn{true};
ID3D11Texture2D* g_hudDsvTex = nullptr;
ID3D11DepthStencilView* g_hudDsv = nullptr;
ID3D11DepthStencilView* g_hudDsvSrc = nullptr; // the game's view it mirrors (weak)
bool g_hudDsvCleared = false;                  // this frame
float g_sceneClearDepth = 1.0f;                // the game's last full-size depth clear value

// The capture RT (owned).
ID3D11Texture2D* g_capTex = nullptr;
ID3D11RenderTargetView* g_capRtv = nullptr;
// PAUSEHALO (JJ 2026-10-09: "when pausing ... because of the black-and-white effect ... the menu buttons along the
// bottom are almost impossible to see. ... some sort of soft feathered background for the buttons"): while the game
// is paused the panel shows a copy of the capture with a soft dark halo under everything drawn on it (blit
// alpha_down + halo_under), so text and button prompts keep a dark surround over the grey paused picture.
std::atomic<float> g_halo{0.0f}; // strength now (the adapter: the user's strength while paused, else 0)
std::atomic<float> g_haloRadius{0.035f};
ID3D11ShaderResourceView* g_capSrv = nullptr;
ID3D11Texture2D* g_haloTex = nullptr;
ID3D11RenderTargetView* g_haloRtv = nullptr;
ID3D11Texture2D* g_smallTex = nullptr;
ID3D11RenderTargetView* g_smallRtv = nullptr;
ID3D11ShaderResourceView* g_smallSrv = nullptr;
bool g_haloValid = false; // this frame's panel = g_haloTex
unsigned g_capW = 0, g_capH = 0;
uint32_t g_lastHudFrame = 0; // present counter value of the last frame with HUD draws
uint32_t g_presents = 0;

std::atomic<uint32_t> g_cFrames{0}, g_cBoundaries{0}, g_cHudDraws{0}, g_cRedirected{0},
    g_cBigPost{0}, g_cLastFrameHud{0};
uint32_t g_frameHud = 0;

// Batman City diagnostics (JJ 2026-10-04: the compass and a bar sit on BLACK on the panel):
// `bc huddump` lists every HUD draw of the next frame with its blend state; a capture (F2)
// also saves the panel picture as a 32-bit BMP (alpha kept) for side-by-side checks.
std::atomic<int> g_dumpLeft{0};
// Timeline (render thread): per frame, the distinct textures drawn (w, h, fmt, count) and the largest placement.
std::atomic<int> g_lineLeft{0};
// HUDRING (2026-10-09, JJ tests the exit-to-menu bat wipe by hand; "add a function to the overlay which records the
// last X amount of frames, so that once it happens, I can then go and capture it"): the same per-frame lines are
// always kept for the last 360 HUD frames; a capture (F2 / panel button) saves them as hud_last_frames.txt. With
// settings debug_hud_ring=1 they are also written to the log whenever a full-screen movie starts.
std::atomic<bool> g_ringOn{true};
std::atomic<bool> g_ringLogOnMovie{false};
constexpr int kRing = 1500; // ~15 s (JJ needs time to reach the capture button)
char g_ring[kRing][700];
int g_ringHead = 0, g_ringCount = 0;
// 2026-10-09: per texture also its largest placement and how many of its pieces stayed in the eye image (BATCUTS).
struct LineTex { unsigned w, h; int fmt; unsigned n; float sc; unsigned eye; unsigned hid; }; // hid: not drawn (WIPEMAIN2)
LineTex g_lineTex[24];
unsigned g_lineTexN = 0, g_lineDraws = 0;
float g_lineMaxScale = 0.0f;
int g_lineCur = -1; // this draw's g_lineTex entry
void line_note(ID3D11DeviceContext* ctx) {
    ++g_lineDraws;
    unsigned w = 0, h = 0;
    int fmt = 0;
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    if (srv) {
        ID3D11Resource* r = nullptr;
        srv->GetResource(&r);
        ID3D11Texture2D* t = nullptr;
        if (r && SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&t))) && t) {
            D3D11_TEXTURE2D_DESC td{};
            t->GetDesc(&td);
            w = td.Width; h = td.Height; fmt = int(td.Format);
            t->Release();
        }
        if (r) r->Release();
        srv->Release();
    }
    unsigned i = 0;
    for (; i < g_lineTexN; ++i)
        if (g_lineTex[i].w == w && g_lineTex[i].h == h && g_lineTex[i].fmt == fmt) break;
    if (i == g_lineTexN && g_lineTexN < 24) g_lineTex[g_lineTexN++] = {w, h, fmt, 0, 0.0f, 0, 0};
    if (i < g_lineTexN) ++g_lineTex[i].n;
    g_lineCur = i < g_lineTexN ? int(i) : -1;
    float cb[16] = {};
    ID3D11Buffer* vcb = nullptr;
    ctx->VSGetConstantBuffers(0, 1, &vcb);
    if (vcb) { bvr::frame_inspector::cb_shadow(vcb, cb, sizeof(cb)); vcb->Release(); }
    const float sc = fabsf(cb[0]) + fabsf(cb[1]);
    if (sc > g_lineMaxScale) g_lineMaxScale = sc;
    if (g_lineCur >= 0 && sc > g_lineTex[g_lineCur].sc) g_lineTex[g_lineCur].sc = sc;
}
std::atomic<bool> g_dumpArm{false};
uint32_t g_dumpIndex = 0;
std::atomic<bool> g_saveReq{false};
wchar_t g_savePath[MAX_PATH] = {};
// Test (2026-10-09, the black box entering gameplay): the panel picture saved every HUD frame for N frames.
std::atomic<int> g_panelSeqLeft{0};
wchar_t g_panelSeqDir[MAX_PATH] = {};

void dump_draw(ID3D11DeviceContext* ctx, unsigned count, bool indexed, bool redirected) {
    ID3D11BlendState* bs = nullptr;
    FLOAT bf[4];
    UINT mask = 0;
    ctx->OMGetBlendState(&bs, bf, &mask);
    D3D11_BLEND_DESC d{};
    if (bs) { bs->GetDesc(&d); bs->Release(); }
    const D3D11_RENDER_TARGET_BLEND_DESC& rt = d.RenderTarget[0];
    unsigned sw = 0, sh = 0;
    int fmt = 0;
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    if (srv) {
        ID3D11Resource* r = nullptr;
        srv->GetResource(&r);
        ID3D11Texture2D* t = nullptr;
        if (r && SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&t))) && t) {
            D3D11_TEXTURE2D_DESC td{};
            t->GetDesc(&td);
            sw = td.Width; sh = td.Height; fmt = static_cast<int>(td.Format);
            t->Release();
        }
        if (r) r->Release();
        srv->Release();
    }
    ID3D11PixelShader* ps = nullptr;
    ctx->PSGetShader(&ps, nullptr, nullptr);
    float cb[16] = {};
    {
        ID3D11Buffer* vcb = nullptr;
        ctx->VSGetConstantBuffers(0, 1, &vcb);
        if (vcb) { bvr::frame_inspector::cb_shadow(vcb, cb, sizeof(cb)); vcb->Release(); }
    }
    BVR_LOG("[huddump] #%u %s %u -> %s | blend %s src %d dst %d op %d | alpha src %d dst %d | write 0x%X | "
            "tex %ux%u fmt %d | ps %p | vscb %.4g %.4g %.4g %.4g / %.4g %.4g %.4g %.4g / %.4g %.4g %.4g %.4g / %.4g %.4g %.4g %.4g",
            g_dumpIndex++, indexed ? "DrawIndexed" : "Draw", count, redirected ? "PANEL" : "eye",
            bs ? (rt.BlendEnable ? "on" : "off") : "none", rt.SrcBlend, rt.DestBlend, rt.BlendOp,
            rt.SrcBlendAlpha, rt.DestBlendAlpha, rt.RenderTargetWriteMask, sw, sh, fmt, ps, cb[0], cb[1], cb[2],
            cb[3], cb[4], cb[5], cb[6], cb[7], cb[8], cb[9], cb[10], cb[11], cb[12], cb[13], cb[14], cb[15]);
    if (ps) ps->Release();
}

} // namespace
bool save_texture_bmp(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const wchar_t* path) {
    if (!tex || !ctx) return false;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc = {1, 0};
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    ID3D11Texture2D* st = nullptr;
    if (dev) { dev->CreateTexture2D(&td, nullptr, &st); dev->Release(); }
    if (!st) return false;
    ctx->CopySubresourceRegion(st, 0, 0, 0, 0, tex, 0, nullptr);
    bool ok = false;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        if (FILE* f = _wfopen(path, L"wb")) {
            const uint32_t w = td.Width, h = td.Height, img = w * h * 4;
            uint8_t hdr[54] = {'B', 'M'};
            auto put = [&](int off, uint32_t v) { memcpy(hdr + off, &v, 4); };
            put(2, 54 + img); put(10, 54); put(14, 40); put(18, w); put(22, h);
            hdr[26] = 1; hdr[28] = 32; put(34, img);
            fwrite(hdr, 1, 54, f);
            const bool bgr = td.Format == DXGI_FORMAT_B8G8R8A8_UNORM || td.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            std::vector<uint8_t> row(w * 4);
            for (uint32_t y = 0; y < h; ++y) {
                const uint8_t* src = static_cast<const uint8_t*>(m.pData) + (h - 1 - y) * m.RowPitch;
                for (uint32_t x = 0; x < w; ++x) {
                    row[x * 4 + 0] = bgr ? src[x * 4 + 0] : src[x * 4 + 2];
                    row[x * 4 + 1] = src[x * 4 + 1];
                    row[x * 4 + 2] = bgr ? src[x * 4 + 2] : src[x * 4 + 0];
                    row[x * 4 + 3] = src[x * 4 + 3];
                }
                fwrite(row.data(), 1, row.size(), f);
            }
            fclose(f);
            ok = true;
            BVR_LOG("[hud] picture saved: %ls (%ux%u, format %d)", path, w, h, static_cast<int>(td.Format));
        }
        ctx->Unmap(st, 0);
    }
    st->Release();
    return ok;
}
namespace {

void release_capture() {
    if (g_capRtv) { g_capRtv->Release(); g_capRtv = nullptr; }
    if (g_capSrv) { g_capSrv->Release(); g_capSrv = nullptr; }
    if (g_haloRtv) { g_haloRtv->Release(); g_haloRtv = nullptr; }
    if (g_haloTex) { g_haloTex->Release(); g_haloTex = nullptr; }
    if (g_smallSrv) { g_smallSrv->Release(); g_smallSrv = nullptr; }
    if (g_smallRtv) { g_smallRtv->Release(); g_smallRtv = nullptr; }
    if (g_smallTex) { g_smallTex->Release(); g_smallTex = nullptr; }
    g_haloValid = false;
    if (g_capTex) { g_capTex->Release(); g_capTex = nullptr; }
    g_capW = g_capH = 0;
}

// Batman City: UE3 sizes its scene/stencil buffers rounded UP (Arkham City: backbuffer
// 1274x1363, depth-stencil 1280x1368), and its Scaleform draws keep that DSV bound for
// masking. D3D11 refuses an RTV/DSV pair of different sizes, so the capture RT is sized to the
// LARGEST of backbuffer and any DSV seen with UI draws (sticky - never shrinks mid-frame).
unsigned g_needW = 0, g_needH = 0;
void dsv_dims(ID3D11DepthStencilView* dsv, unsigned* w, unsigned* h) {
    *w = *h = 0;
    if (!dsv) return;
    ID3D11Resource* r = nullptr;
    dsv->GetResource(&r);
    if (!r) return;
    ID3D11Texture2D* tex = nullptr;
    r->QueryInterface(IID_PPV_ARGS(&tex));
    if (tex) {
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        *w = td.Width;
        *h = td.Height;
        tex->Release();
    }
    r->Release();
}

bool ensure_capture(ID3D11DeviceContext* ctx) {
    unsigned dw = 0, dh = 0;
    dsv_dims(g_curDsv, &dw, &dh);
    const unsigned wantW = (std::max)({g_bbW, dw, g_needW});
    const unsigned wantH = (std::max)({g_bbH, dh, g_needH});
    g_needW = wantW;
    g_needH = wantH;
    if (g_capTex && g_capW == wantW && g_capH == wantH) return true;
    release_capture();
    if (!wantW || !wantH) return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = wantW;
    td.Height = wantH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    bool ok = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g_capTex)) && g_capTex &&
              SUCCEEDED(dev->CreateRenderTargetView(g_capTex, nullptr, &g_capRtv)) && g_capRtv;
    dev->Release();
    if (!ok) {
        release_capture();
        static bool logged = false;
        if (!logged) {
            logged = true;
            BVR_LOG("gfx_hud: capture RT creation FAILED (%ux%u)", g_bbW, g_bbH);
        }
        return false;
    }
    g_capW = wantW;
    g_capH = wantH;
    BVR_LOG("gfx_hud: capture RT %ux%u created", g_capW, g_capH);
    return true;
}

// Is this draw the eye blit? Cheap gates first; the srv/viewport checks run
// only for depth-free a=6 candidates on the backbuffer (rare - one per frame).
bool is_boundary_blit(ID3D11DeviceContext* ctx, unsigned indexCount) {
    if (indexCount != 6 || g_curDsv) return false;
    UINT n = 1;
    D3D11_VIEWPORT vp{};
    ctx->RSGetViewports(&n, &vp);
    if (n < 1 || static_cast<unsigned>(vp.Width) != g_bbW ||
        static_cast<unsigned>(vp.Height) != g_bbH)
        return false;
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv) return false;
    bool ok = false;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    if (res) {
        ID3D11Texture2D* tex = nullptr;
        res->QueryInterface(IID_PPV_ARGS(&tex));
        if (tex) {
            D3D11_TEXTURE2D_DESC td{};
            tex->GetDesc(&td);
            // A rendered full-res RT, not a UI atlas: RT-bindable and
            // backbuffer-sized (the measured blit source: 2064x2208 fmt 26,
            // bind RT|SRV).
            // Batman City: UE3 rounds its buffers up (1280x1368 source for a 1274x1363
            // backbuffer), so accept up to 16 px over in each axis.
            ok = (td.BindFlags & D3D11_BIND_RENDER_TARGET) != 0 && td.Width >= g_bbW &&
                 td.Height >= g_bbH && td.Width <= g_bbW + 16 && td.Height <= g_bbH + 16;
            tex->Release();
        }
        res->Release();
    }
    srv->Release();
    return ok;
}

// Batman City (JJ 2026-10-03: the grapple reticle showed black boxes on the HUD panel): the
// capture RT starts transparent, so only "over" blending builds a correct alpha. Additive
// (glow) and multiply-type UI wrote alpha too and turned into black patches. Per redirected
// draw: keep the colour blend, and for the ALPHA channel use (ONE, INV_SRC_ALPHA) for normal
// "over" draws and (ZERO, ONE) - leave alpha alone - for everything else. With the quad's
// premultiplied compositing an additive draw then stays additive and black adds nothing.
std::map<ID3D11BlendState*, ID3D11BlendState*> g_fixedBlends; // game state -> corrected
ID3D11BlendState* g_fixedNull = nullptr;                      // for "no blend state bound"
std::atomic<uint32_t> g_cBlendFixed{0};

ID3D11BlendState* fixed_blend(ID3D11DeviceContext* ctx) {
    ID3D11BlendState* cur = nullptr;
    FLOAT bf[4];
    UINT mask = 0;
    ctx->OMGetBlendState(&cur, bf, &mask);
    auto it = cur ? g_fixedBlends.find(cur) : g_fixedBlends.end();
    if (cur && it != g_fixedBlends.end()) {
        cur->Release();
        return it->second; // may be null = no change needed
    }
    if (!cur && g_fixedNull) return g_fixedNull;
    D3D11_BLEND_DESC d{};
    if (cur) {
        cur->GetDesc(&d);
    } else {
        d.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        d.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
        d.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        d.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        d.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    }
    ID3D11BlendState* out = nullptr;
    D3D11_RENDER_TARGET_BLEND_DESC& rt = d.RenderTarget[0];
    if (rt.BlendEnable) {
        const bool over = rt.DestBlend == D3D11_BLEND_INV_SRC_ALPHA;
        d.IndependentBlendEnable = FALSE;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        if (over) {
            rt.SrcBlendAlpha = D3D11_BLEND_ONE;
            rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            // Only where the game writes colour. Scaleform CLIP MASKS are drawn with every
            // colour write off (stencil only); turning alpha on for them painted the compass's
            // clip rectangle and another one as solid black boxes on the panel (JJ 2026-10-04,
            // panel picture of capture 20261004_043343).
            if (rt.RenderTargetWriteMask & D3D11_COLOR_WRITE_ENABLE_ALL & ~D3D11_COLOR_WRITE_ENABLE_ALPHA)
                rt.RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
        } else {
            rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
            rt.DestBlendAlpha = D3D11_BLEND_ONE;
        }
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) {
            dev->CreateBlendState(&d, &out);
            dev->Release();
        }
    }
    if (cur) {
        g_fixedBlends[cur] = out; // the game's state objects live as long as the device
        cur->Release();
    } else {
        g_fixedNull = out;
    }
    return out;
}

std::atomic<MarkerFn> g_markerFn{nullptr};
std::atomic<bool> g_cutsToEye{false};
std::atomic<uint32_t> g_cutsKept{0};
// The wipe was laid out for 16:9; the tall eye picture showed bands above and below it. Cut pieces are scaled
// about the picture centre by this much (their own placement, VS cb0), and put back after the draw.
std::atomic<float> g_cutScale{1.6f};
// FADEHOLD (JJ 2026-10-09: "a big black box in the centre of the view when entering gameplay from the main menu"):
// panel pictures (fake headset runs 27/28) show an opaque black rectangle over ~83% of the panel, panel pixels
// (0,115)-(1438,1390), fading out over ~0.8 s after the loading movie ends = the game's fade-in from black, laid
// out on the HUD movie's stage, so it lands on the panel. Its pieces share their texture (4x4 / 256x1 fills) and
// placement scale (the stage's) with ordinary HUD pieces, so it cannot be told apart per draw. Instead: for up to
// 3 s after a full-screen movie ends, five points of the panel picture are read back (1 texel each, 2 frames late,
// never waiting); while all five are dark and covered the panel is not shown (texture() = none), so the world
// appears with no box. Mode 0 = off (the old behaviour).
std::atomic<int> g_fadeHoldMode{1};
std::atomic<ULONGLONG> g_fadeUntil{0};
std::atomic<bool> g_fadeHold{false};
ID3D11Texture2D* g_fadeProbe[3] = {};
bool g_fadeProbeUsed[3] = {};
uint32_t g_fadeSeq = 0;
ULONGLONG g_fadeStart = 0;
// JJ 2026-10-09 18:38: "gone (panel clear) after 46 ms" - probed before the box was first drawn (it starts a few
// frames after the movie), so the panel came back and showed it. Now: clear only after the box was seen covered,
// and on two probes in a row; no box seen within 0.7 s (a load into the menus) = panel back.
bool g_fadeSeen = false;
int g_fadeClear = 0;
constexpr int kFadePts = 5;
const float kFadePt[kFadePts][2] = {{0.25f, 0.27f}, {0.75f, 0.27f}, {0.5f, 0.5f}, {0.25f, 0.72f}, {0.75f, 0.72f}};

void fade_end(const char* why) {
    if (g_fadeHold.exchange(false))
        BVR_LOG("gfx_hud: fade-in box gone (%s) - panel shown again after %llu ms", why, GetTickCount64() - g_fadeStart);
    g_fadeUntil.store(0);
}

// Render thread, at present: probe this frame's panel picture, evaluate the one copied two frames ago.
void fade_tick(ID3D11DeviceContext* ctx) {
    if (!g_fadeHold.load(std::memory_order_relaxed)) return;
    if (GetTickCount64() > g_fadeUntil.load(std::memory_order_relaxed)) { fade_end("3 s limit"); return; }
    if (!ctx || !g_capTex || !g_frameHud) return; // no HUD drawn this frame: nothing to read yet
    const uint32_t slot = g_fadeSeq % 3;
    if (!g_fadeProbe[slot]) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kFadePts; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc = {1, 0};
        td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        dev->CreateTexture2D(&td, nullptr, &g_fadeProbe[slot]);
        dev->Release();
        if (!g_fadeProbe[slot]) return;
    }
    for (int i = 0; i < kFadePts; ++i) {
        const UINT x = static_cast<UINT>(kFadePt[i][0] * g_capW), y = static_cast<UINT>(kFadePt[i][1] * g_capH);
        const D3D11_BOX box{x, y, 0, x + 1, y + 1, 1};
        ctx->CopySubresourceRegion(g_fadeProbe[slot], 0, i, 0, 0, g_capTex, 0, &box);
    }
    g_fadeProbeUsed[slot] = true;
    ++g_fadeSeq;
    const uint32_t old = (g_fadeSeq + 1) % 3; // copied two frames ago
    if (!g_fadeProbeUsed[old] || !g_fadeProbe[old]) return;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_fadeProbe[old], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) return;
    const uint8_t* p = static_cast<const uint8_t*>(m.pData);
    int covered = 0;
    for (int i = 0; i < kFadePts; ++i) {
        const uint8_t* c = p + i * 4;
        if (c[3] > 12 && c[0] < 40 && c[1] < 40 && c[2] < 40) ++covered;
    }
    ctx->Unmap(g_fadeProbe[old], 0);
    g_fadeProbeUsed[old] = false;
    if (covered == kFadePts) { g_fadeSeen = true; g_fadeClear = 0; return; }
    if (g_fadeSeen) {
        if (++g_fadeClear >= 2) fade_end("panel clear");
    } else if (GetTickCount64() - g_fadeStart > 700) {
        fade_end("no box within 0.7 s");
    }
}

// PAUSEHALO, render thread at present (after the frame's HUD is complete).
void halo_tick(ID3D11DeviceContext* ctx) {
    const float strength = g_halo.load(std::memory_order_relaxed);
    if (strength <= 0.0f || !ctx || !g_capTex || !g_capW || !g_capH) { g_haloValid = false; return; }
    if (!g_frameHud) return; // no UI this frame: keep the last picture
    if (!g_haloTex) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = g_capW; td.Height = g_capH; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        D3D11_TEXTURE2D_DESC sd = td;
        sd.Width = (g_capW + 7) / 8; sd.Height = (g_capH + 7) / 8;
        bool ok = SUCCEEDED(dev->CreateShaderResourceView(g_capTex, nullptr, &g_capSrv)) &&
                  SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g_haloTex)) &&
                  SUCCEEDED(dev->CreateRenderTargetView(g_haloTex, nullptr, &g_haloRtv)) &&
                  SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &g_smallTex)) &&
                  SUCCEEDED(dev->CreateRenderTargetView(g_smallTex, nullptr, &g_smallRtv)) &&
                  SUCCEEDED(dev->CreateShaderResourceView(g_smallTex, nullptr, &g_smallSrv));
        dev->Release();
        if (!ok) {
            BVR_LOG("gfx_hud: pause halo textures FAILED");
            g_halo.store(0.0f);
            return;
        }
        BVR_LOG("gfx_hud: pause halo ready (%ux%u, small %ux%u)", g_capW, g_capH, sd.Width, sd.Height);
    }
    const UINT sw = (g_capW + 7) / 8, sh = (g_capH + 7) / 8;
    const float r = g_haloRadius.load(std::memory_order_relaxed);
    const float p[4] = {r, r * float(g_capW) / float(g_capH), 6.0f, strength};
    g_haloValid = bvr::blit::alpha_down(ctx, g_smallRtv, sw, sh, g_capSrv, g_capW, g_capH) &&
                  bvr::blit::halo_under(ctx, g_haloRtv, g_capW, g_capH, g_capSrv, g_smallSrv, p);
}
// Which untextured / screen-picture pieces are the wipe (fake headset 2026-10-08): the title logo and bat symbol
// are untextured too, but sit in frames with ordinary HUD pieces at normal size. A cut piece is the wipe when it
// is drawn well over screen size, or when the previous HUD frame held nothing but cut-type pieces.
uint32_t g_frameCut = 0, g_frameOther = 0; // render thread, this frame
// WIPEALL (JJ 2026-10-09: restarting / exiting from the pause menu, or B back to "Press to continue": "the black screen
// with the bats flying across it appears in the middle of the field of view ... needs to be scaled and put at the depth
// of the very first one"). HUD ring 15:55:03: that wipe is ONE untextured black piece drawn at the menu movie's scale
// (placement 0.14, kept in the eye by BATCUTS) with the bats as six 256x256 BC3 sprites (+ a 64x64 one) - textured, so
// they went to the panel. Once a frame holds a cut piece at a menu-movie scale (> 1e-3; gameplay HUD pieces sit at
// ~2e-4 and below), every piece after it in that frame, and every piece of the next frame, stays in the eye with it.
bool g_wipeFrame = false, g_wipePrev = false;
std::atomic<bool> g_menuCut{false};
// STAGEBOX / FIRSTCUT / FADESKIP (JJ 2026-10-09 captures 184851 / 184932 / 184953 / 185004: "a black square appear
// briefly in the middle of your field of view, and then it goes to a full-size black screen, and then back ... the
// smaller black square briefly"; the same when the main menu appears after leaving the game; "a lot bigger" box when
// first entering gameplay). The rings: the square is a FILL piece (4x4 fill texture or untextured) at the HUD
// stage's own scale (placement 1.9e-4 .. 6.7e-4) just before / after the wipe frames - not a cut piece, so it went
// to the panel; the menus' own fill pieces sit at 9.8e-5 (save select), 0.14 (pause), 1.9 (main menu), the
// gameplay HUD's at <= 1.1e-4. And the first frame of the save-select -> title wipe is one untextured piece that
// went to the panel (the frame before it was not a "pure cut" frame).
//  STAGEBOX: outside gameplay, or within 1 s of a wipe frame, a stage-scale fill piece stays in the eye with the wipe.
//  FIRSTCUT: outside gameplay, a cut piece that is a frame's FIRST draw stays in the eye.
//  FADESKIP: in gameplay, for 3 s after a full-screen movie ends, stage-scale fill pieces (the fade-in box - its
//  zoom-out defeated FADEHOLD's probes, 18:49:50) are not drawn at all.
std::atomic<bool> g_stageBox{true};
// HIDEWIPES (JJ 2026-10-09 22:04: "small black square, full screen black, then small black square with bats coming past.
// So we just need to get rid of that black bloody square"; the exit-to-menu bats "in the same size small square").
// Capture 220419 (save select -> title, t-stamped ring): 1 frame of the untextured wipe piece on the panel, 1.47 s of
// it in the eye (the game animates it as a square that grows and shrinks - enlarged, it still only fills the view at
// its peak), then 0.6 s of the 1024x256 logo halves in the eye before they move to the panel. Outside gameplay (menus,
// title, pause) and for wipe-scale pieces (> 1e-3, the menu movies) everything the wipe logic would keep in the eye,
// and every untextured piece, is not drawn at all. Default on; off = the old full-view handling.
std::atomic<bool> g_hideWipes{true};
std::atomic<bool> g_mapScreenOn{true}; // MAPSCREEN: no wipe hiding while the map is up
bool g_wipeFromGameplay = false; // MENUOPEN2 (log 03:35:53: the clock stopped 4 ms before the replay began, so "gameplay" was already false)
// MS2 (JJ 2026-10-11 22:41: the upgrade screen "takes a little bit longer than it probably should"; LIVE: its opening
// replay ran the full recording + 700 ms hold = 1875 ms - the map ends it early by its big shapes, upgrades has none).
std::atomic<int> g_menuOpenHoldMs{250};
std::atomic<ULONGLONG> g_menuWipeAtMs{0}; // MS2: a wipe that started inside the menus (closing / leaving)
std::atomic<bool> g_openFadeOn{true}; // OPENFADE: the menu-opening fade pieces are not drawn
std::atomic<uint32_t> g_openFadeSkips{0}; // MENUOPEN: hold after the recorded wipe when a menu opens from gameplay
// JJ 2026-10-09 (later): "the bat wipe transition works fine going from the menu into the game, so I'd like to keep it".
// That wipe always starts from the MAIN MENU, whose own pieces sit at placement ~1.9 (rings 191956 / 184932); the
// square wipes start from save select (largest piece ~1e-4) or the pause menu (~0.19). The largest non-wipe piece of the
// last ordinary UI frame identifies the screen a wipe starts from: >= 1 (main menu) = keep the old full-view handling.
// WIPEREPLAY (JJ 2026-10-09: "remove it and replace it with the one that we've got going from the menu to the game"):
// the game cannot be asked to play that wipe elsewhere, so the mod RECORDS it: while the main menu -> game wipe plays,
// its pieces are drawn into a recording target instead of the eye image; before the eye pictures are taken that target
// is laid over the whole view (enlarged like cut_scale) and a quarter-size copy is stored with its time. When a square
// wipe starts (save select -> title, pause menu exit / restart), its pieces are not drawn (HIDEWIPES) and the stored
// frames are played over the view instead, at the recorded speed. No recording yet in this session = just hidden.
constexpr int kRecMax = 360;
ID3D11Texture2D* g_recTex = nullptr;          // full size, this frame's wipe pieces (premultiplied)
ID3D11RenderTargetView* g_recRtv = nullptr;
ID3D11ShaderResourceView* g_recSrv = nullptr;
ID3D11Texture2D* g_recFrames[kRecMax] = {};   // stored frames (full size since WIPEHD; v1 files were quarter size)
ID3D11RenderTargetView* g_recFrameRtv[kRecMax] = {};
ID3D11ShaderResourceView* g_recFrameSrv[kRecMax] = {};
ULONGLONG g_recTime[kRecMax] = {};            // ms since the recording started
int g_recCount = 0, g_recWriting = 0;         // frames of the finished recording / of the one being written
ULONGLONG g_recStart = 0, g_recLastMs = 0;
bool g_recThisFrame = false, g_recCleared = false;
std::atomic<bool> g_replayOn{false};
ULONGLONG g_replayStart = 0, g_squareSeenMs = 0, g_replayEndMs = 0;
bool g_replayLocked = false;                  // one replay per square wipe
std::atomic<uint32_t> g_replays{0};
// WIPEMAIN (JJ 2026-10-09 22:48: exit from the pause menu still a small box; save select -> title "a black box ... no
// bats at all"). Log 22:44-22:45: the pause menu's own pieces also sit at ~1.9 (huddump 224514 #7), so its exit wipe
// counted as the main menu one: shown small AND recorded over the good recording (54 frames, 22:45:10); an after-loading
// piece in gameplay was recorded too (5 frames, 22:45:00). The save select -> title replay then played that small box.
// Now: a wipe counts as the main menu one only while the main menu's grunge is on screen (not the pause screen); the
// mode a wipe starts in holds until 400 ms without wipe pieces; a recording is kept only if the loading movie starts
// during it or within 1.5 s after it; once a recording is kept (or loaded from disk) nothing overwrites it; no replay in
// the 3 s after a loading movie (gameplay fade pieces).
std::atomic<uint64_t> g_mainMenuAt{0};       // last frame the main menu's grunge was on screen (game thread)
std::atomic<uint64_t> g_movieStartMs{0}, g_movieEndMs{0};
std::atomic<uint32_t> g_movieEnds{0}; // MENUOPEN3: full-screen movies ended this session (1 = the logos)
ULONGLONG g_mainWipeUntil = 0, g_lastKeepMs = 0;
bool g_recKeep = false;                       // a good recording is held: never overwritten this session
std::atomic<bool> g_replayHidesAll{true};     // WIPEMAIN2: no HUD piece drawn while the recorded wipe plays
// WIPEMAIN3 (JJ 23:10: "still a smaller box" on both; captures 230928 / 230935): no box in the eyes - both on the PANEL.
// Title return: hidden frames never cleared the panel, so it kept the save-select backdrop (84 % opaque black). Pause
// exit: the recording (1.14 s) ends before the game's own wipe (~0.5 s more of the untextured piece + 256x256 bats), whose
// rest went to the panel. Now a hidden piece clears the panel once per frame, and the replay holds its last frame while
// untextured wipe pieces keep coming (300 ms after the last, at most 4 s from the start).
ULONGLONG g_hideAllUntil = 0, g_hideAllCap = 0;

// LOADHOLD (JJ 2026-10-11: after quitting to the main menu "it takes ages for Batman to then come into view" - "if it's a
// loading issue because there's more data to load, then maybe it should pause longer at the black screen after the bat
// wipe"): a wipe replay that ends on black (not the flipped ones that reveal a menu) keeps its black last frame while the
// game reads from disk faster than 20 MB/s (measured over the last 0.5 s, sampled every 0.1 s during the replay, so a
// wipe with no loading behind it ends exactly as before), at most 20 s. `bc wipehold 0|1`.
std::atomic<bool> g_loadHoldOn{true};
struct DiskSample { ULONGLONG ms; uint64_t bytes; };
DiskSample g_disk[16] = {};
int g_diskN = 0;
ULONGLONG g_lhStart = 0;
uint64_t g_lhStartBytes = 0;
void disk_sample(ULONGLONG now) {
    if (g_diskN && now - g_disk[(g_diskN - 1) & 15].ms < 100) return;
    IO_COUNTERS io{};
    if (!GetProcessIoCounters(GetCurrentProcess(), &io)) return;
    g_disk[g_diskN & 15] = DiskSample{now, io.ReadTransferCount};
    ++g_diskN;
}
float disk_rate_mbs(ULONGLONG now) { // over the last ~0.5 s; 0 = not enough samples
    if (g_diskN < 2) return 0.0f;
    const DiskSample& last = g_disk[(g_diskN - 1) & 15];
    int i = g_diskN - 2;
    while (i > 0 && i > g_diskN - 16 && now - g_disk[i & 15].ms < 500) --i;
    const DiskSample& first = g_disk[i & 15];
    if (last.ms <= first.ms || last.bytes < first.bytes) return 0.0f;
    return float(double(last.bytes - first.bytes) / 1e6 / (double(last.ms - first.ms) / 1000.0));
}
// MENUARRIVE (JJ 2026-10-11: the reveal "sometimes arrives at Batman, and sometimes it arrives before Batman"). MENUTRACE
// 04:06-04:07, 3 quits to the main menu (draw calls per present + the game's frame every 0.5 s): during the wipe the
// old level draws ~720-800; at ~1.5 s the frontend's first shot (a close-up of the moon) draws ~95; Batman's shot (on
// the gargoyle, bats, the city) jumps to ~290 at +4.2 / +4.9 s - frames 2_04000 (moon) / 2_04500 (Batman). The disk
// hold had ended 0.7-2 s before that. So the black now holds until that jump: the level during the wipe (B), then a low
// phase (< 0.5 B, its average L), then 3 presents at >= max(1.8 L, L + 80) = Batman's shot -> reveal. Fallbacks: no low
// phase and the disk quiet for 1 s = nothing loading after all; 15 s cap.
uint64_t g_dsLastCensus = 0;
ULONGLONG g_dsLastMs = 0;     // MENUARRIVE2: a sample after a gap is not one present (log 04:11: "923821 during the wipe")
ULONGLONG g_dsReplay = 0;     // the replay the counts belong to (its start time)
uint32_t g_dsWipeMax = 0;     // B: the most draws per present during the wipe
uint32_t g_dsLast = 0;        // draws in the last present
void draw_sample() { // every present while a replay plays
    const uint64_t c = bvr::frame_inspector::draw_call_census();
    const ULONGLONG ms = GetTickCount64();
    g_dsLast = g_dsLastCensus && c >= g_dsLastCensus && ms - g_dsLastMs < 200 ? uint32_t(c - g_dsLastCensus) : 0;
    g_dsLastCensus = c;
    g_dsLastMs = ms;
    if (g_dsReplay != g_replayStart) { g_dsReplay = g_replayStart; g_dsWipeMax = 0; }
    if (!g_lhStart && g_dsLast > g_dsWipeMax) g_dsWipeMax = g_dsLast;
}
bool g_lhLowSeen = false;
int g_lhLowN = 0, g_lhHighN = 0;
float g_lhLowAvg = 0.0f;
ULONGLONG g_lhQuietSince = 0;
std::atomic<int> g_preRevealMs{500}; // PREREVEAL, below
bool load_hold(ULONGLONG now, bool toBlack, ULONGLONG recLen) {
    (void)recLen;
    if (!toBlack || !g_loadHoldOn.load(std::memory_order_relaxed)) return false;
    disk_sample(now);
    const float rate = disk_rate_mbs(now);
    const uint64_t bytes = g_diskN ? g_disk[(g_diskN - 1) & 15].bytes : 0;
    const uint32_t d = g_dsLast;
    if (!g_lhStart) {
        if (rate < 20.0f) return false; // nothing loading: the wipe ends as before
        g_lhStart = now;
        g_lhStartBytes = bytes;
        g_lhLowSeen = false; g_lhLowN = 0; g_lhHighN = 0; g_lhLowAvg = 0.0f; g_lhQuietSince = 0;
        BVR_LOG("gfx_hud: LOADHOLD: the game is loading (%.0f MB/s, %u draws per present, %u during the wipe) - black held "
                "until Batman's shot", rate, d, g_dsWipeMax);
        return true;
    }
    const char* end = nullptr;
    const float B = float(g_dsWipeMax);
    // MENUARRIVE3 (log 04:16:50: low phase 162, released at 352): Batman's shot (~290) is also under half the wipe's
    // level, so it fed the low-phase average and lifted the bar above itself. Once the low phase is known, only presents
    // under the bar update it; the first low presents start it from their smallest value.
    const float need = g_lhLowAvg * 1.8f > g_lhLowAvg + 80.0f ? g_lhLowAvg * 1.8f : g_lhLowAvg + 80.0f;
    if (g_lhLowSeen && float(d) >= need) {
        if (++g_lhHighN >= 1) end = "Batman's shot is up"; // JJ 04:21: "a little bit late" - the first present
    } else if (d && d < 0.5f * B) { // the low phase: the frontend's first shot (a close-up of the moon)
        g_lhHighN = 0;
        if (++g_lhLowN >= 3) g_lhLowSeen = true;
        g_lhLowAvg = g_lhLowAvg > 0.0f ? (float(d) < g_lhLowAvg ? float(d) : g_lhLowAvg * 0.9f + float(d) * 0.1f) : float(d);
    } else {
        g_lhHighN = 0;
    }
    if (rate < 12.0f) { if (!g_lhQuietSince) g_lhQuietSince = now; }
    else g_lhQuietSince = 0;
    if (!g_lhLowSeen) {
        if (g_lhQuietSince && now - g_lhQuietSince > 1000) end = "nothing more loading";
    } else if (!end && g_lhQuietSince && now - g_lhQuietSince >= ULONGLONG(g_preRevealMs.load(std::memory_order_relaxed))) {
        // PREREVEAL (JJ 2026-10-11, asked: the bats "just before Batman"): the cut to Batman's shot comes 1-2 s after the
        // loading goes quiet (traces 04:06: quiet ~3.5 / 3.3 s, cut 4.9 / 4.3 s; 04:25: quiet ~12.0, cut 14.1) - the reveal
        // starts g_preRevealMs after it, so the bats are already flying when he appears (the moon shot's end may show).
        end = "loading finished - just before Batman's shot";
    }
    if (!end && now - g_lhStart > 15000) end = "15 s cap reached";
    if (!end) return true;
    BVR_LOG("gfx_hud: LOADHOLD: black held %llu ms - %s (draws %u, low phase %.0f, wipe %u; %.0f MB read)", now - g_lhStart,
            end, d, g_lhLowAvg, g_dsWipeMax, double(bytes - g_lhStartBytes) / 1e6);
    g_lhStart = 0;
    return false;
}
// WIPESOUND: the adapter hears when a wipe to black starts (0) and when the reveal starts (1).
std::atomic<WipeEventFn> g_wipeEventFn{nullptr};
void wipe_event(int kind) { if (WipeEventFn f = g_wipeEventFn.load(std::memory_order_relaxed)) f(kind); }
bool reveal_after_black(ULONGLONG now); // WIPEREVEAL, after start_exit_replay
std::atomic<bool> g_revealOn{true};
std::atomic<int> g_revealSkipMs{0}; // JJ chose the bats before Batman (PREREVEAL): the full recording again
// BLACKFILL (JJ 2026-10-09 23:45: at game start "a zoomed view of the moon ... that should be black. And then the bats
// come across it and reveal Batman sitting on the gargoyle"). Ring 232042: after the logo movie the game covers its HUD
// stage with untextured fills at the stage's own scale (0x0, placement 1.9e-4, ~2.4 s) = a full-screen black in 2D;
// HIDEWIPES hid them, so the moon zoom showed. Now such a hidden fill turns the whole view black (both eyes), held while
// further hidden wipe pieces follow within 1 s (max 6 s), until the replay takes over. Panel switch, saved black_fill.
ULONGLONG g_blackUntil = 0, g_blackCap = 0;
// FADEALPHA (JJ 2026-10-10 00:00: "when returning to the menu from gameplay, the menu appears and then I get a full-screen
// black for a split second"): log 23:58:17 / 23:59:11 / 23:59:43 - the main menu's fade-in from black, first probed at
// alpha ~35 (14 %), was turned into a fully black view. Now the view is darkened by the panel's own fade amount; the
// game-start stage fill (BLACKFILL) stays fully black. Drawn with a 1x1 mask (mask_black, inverted).
ULONGLONG g_blackFillAt = 0;   // last BLACKFILL extension (fully black)
float g_mfAlpha = 1.0f;        // MENUFADE: the panel fade's opacity (5-point average)
ID3D11Texture2D* g_dimTex = nullptr;
ID3D11ShaderResourceView* g_dimSrv = nullptr;
uint8_t g_dimVal = 0xFF;
std::atomic<bool> g_blackFillOn{true};
std::atomic<uint32_t> g_squareStarts{0}; // WIPESEQ3: square wipes in menus (also with the replay off)
// WIPEFLIP (JJ 2026-10-09 23:20: a wipe that ARRIVES at the first menu - at game start, and second menu -> first menu -
// "would look better if the alpha channel was reversed ... black bats on the screen that's already there"). Those wipes
// start from no menu or save select (placement < 1e-3; the pause menu sits at ~0.19) and play the recording inverted:
// black, then the menu opens up through bat-shaped gaps, ending as black bats over it. Nothing is drawn while the last
// frame would be held (inverted it is clear). `bc wipeflip 0|1`, default on.
std::atomic<bool> g_wipeFlipOn{true};
// WIPEPANEL (JJ 23:27: a black box "the same size as the bat cutscene used to be", no menu, no scene, as the bats first
// appear; WIPESEQ 232610/232618: eye images = exactly what goes to the headset, all clean; panel pictures transparent):
// the HUD panel layer is not sent to the headset at all while a replay (incl. its hold) runs. `bc wipepanel 0|1`.
std::atomic<bool> g_wipePanelOff{true};
std::atomic<bool> g_replayEnabled{true}; // WIPEMAIN: square wipes replaced by the recorded one (off = only hidden)
// WIPEDEBUG (JJ 2026-10-10 00:18: "very stuttery" on every exit - the WIPESEQ eye/panel BMPs and the scene's auto
// captures with frame dumps): both test recorders run only with this on. Panel "test: record captures during bat
// wipes (stutters)", saved wipe_debug, default OFF.
std::atomic<bool> g_wipeDebug{false};
bool g_replayInvert = false;
bool ensure_rec(ID3D11DeviceContext* ctx) {
    if (g_recTex) return true;
    if (!g_capW || !g_capH) return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = g_capW; td.Height = g_capH; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    const bool ok = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g_recTex)) &&
                    SUCCEEDED(dev->CreateRenderTargetView(g_recTex, nullptr, &g_recRtv)) &&
                    SUCCEEDED(dev->CreateShaderResourceView(g_recTex, nullptr, &g_recSrv));
    dev->Release();
    if (!ok) BVR_LOG("gfx_hud: wipe recording target FAILED");
    return ok;
}
// Stored frames: the wipe's coverage only (it is black), one channel, quarter size.
UINT g_recW = 0, g_recH = 0;
void drop_rec_frame(int i) {
    if (g_recFrameSrv[i]) { g_recFrameSrv[i]->Release(); g_recFrameSrv[i] = nullptr; }
    if (g_recFrameRtv[i]) { g_recFrameRtv[i]->Release(); g_recFrameRtv[i] = nullptr; }
    if (g_recFrames[i]) { g_recFrames[i]->Release(); g_recFrames[i] = nullptr; }
}
bool make_rec_frame(ID3D11DeviceContext* ctx, int i, UINT w, UINT h, const uint8_t* init) {
    drop_rec_frame(i);
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{init, w, 0};
    const bool ok = SUCCEEDED(dev->CreateTexture2D(&td, init ? &sd : nullptr, &g_recFrames[i])) &&
                    SUCCEEDED(dev->CreateRenderTargetView(g_recFrames[i], nullptr, &g_recFrameRtv[i])) &&
                    SUCCEEDED(dev->CreateShaderResourceView(g_recFrames[i], nullptr, &g_recFrameSrv[i]));
    dev->Release();
    return ok;
}
// Disk (JJ: "can't you just save a recording to disk?"): %LOCALAPPDATA%\Batman_City\menu_wipe.bin, written once
// when the first recording completes, read at the first present - so the replacement works from the start of every
// session. Format: "BCWP" v1, w, h, n, n x time(ms), then n x (bytes, run-length pairs count/value of the coverage).
wchar_t g_wipeFile[MAX_PATH] = {};
bool g_wipeLoadTried = false;
// WIPEHD (JJ 2026-10-09 23:40: "the bat sequence needs to be high resolution. It looks really pixelated"): frames are
// stored at the HUD capture's full size (1440x1536 = about 1:1 with the eye after the 1.6 zoom), one per 12 ms at most,
// at most 160. File v2 = full size; a v1 (quarter-size) file still plays, but the next main menu -> game wipe records
// a full-size one and replaces it.
int g_recFileVer = 0;
void save_recording(ID3D11DeviceContext* ctx) {
    if (!g_wipeFile[0] || g_recCount < 8) return;
    if (g_recFileVer >= 2 && GetFileAttributesW(g_wipeFile) != INVALID_FILE_ATTRIBUTES) return; // never overwrite v2
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = g_recW; td.Height = g_recH; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    dev->CreateTexture2D(&td, nullptr, &st);
    dev->Release();
    if (!st) return;
    FILE* f = _wfopen(g_wipeFile, L"wb");
    if (!f) { st->Release(); return; }
    const uint32_t hdr[5] = {0x50574342u, 2u, g_recW, g_recH, static_cast<uint32_t>(g_recCount)};
    fwrite(hdr, sizeof(hdr), 1, f);
    for (int i = 0; i < g_recCount; ++i) { const uint32_t t = static_cast<uint32_t>(g_recTime[i]); fwrite(&t, 4, 1, f); }
    std::vector<uint8_t> rle;
    for (int i = 0; i < g_recCount; ++i) {
        rle.clear();
        ctx->CopyResource(st, g_recFrames[i]);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            for (UINT y = 0; y < g_recH; ++y) {
                const uint8_t* row = static_cast<const uint8_t*>(m.pData) + y * m.RowPitch;
                for (UINT x = 0; x < g_recW;) {
                    const uint8_t v = row[x];
                    uint8_t n = 1;
                    while (x + n < g_recW && n < 255 && row[x + n] == v) ++n;
                    rle.push_back(n);
                    rle.push_back(v);
                    x += n;
                }
            }
            ctx->Unmap(st, 0);
        }
        const uint32_t len = static_cast<uint32_t>(rle.size());
        fwrite(&len, 4, 1, f);
        if (len) fwrite(rle.data(), 1, len, f);
    }
    fclose(f);
    st->Release();
    g_recFileVer = 2;
    BVR_LOG("gfx_hud: menu wipe saved to disk (%d frames %ux%u, full size)", g_recCount, g_recW, g_recH);
}
void load_recording(ID3D11DeviceContext* ctx) {
    g_wipeLoadTried = true;
    FILE* f = g_wipeFile[0] ? _wfopen(g_wipeFile, L"rb") : nullptr;
    if (!f) return;
    uint32_t hdr[5] = {};
    bool ok = fread(hdr, sizeof(hdr), 1, f) == 1 && hdr[0] == 0x50574342u && (hdr[1] == 1u || hdr[1] == 2u) && hdr[2] && hdr[3] &&
              hdr[2] <= 4096 && hdr[3] <= 4096 && hdr[4] >= 8 && hdr[4] <= static_cast<uint32_t>(kRecMax);
    const UINT w = ok ? hdr[2] : 0, h = ok ? hdr[3] : 0;
    const int n = ok ? static_cast<int>(hdr[4]) : 0;
    for (int i = 0; ok && i < n; ++i) { uint32_t t = 0; ok = fread(&t, 4, 1, f) == 1; g_recTime[i] = t; }
    std::vector<uint8_t> rle, px(static_cast<size_t>(w) * h);
    int loaded = 0;
    for (int i = 0; ok && i < n; ++i) {
        uint32_t len = 0;
        ok = fread(&len, 4, 1, f) == 1 && len <= px.size() * 2 + 16;
        if (!ok) break;
        rle.resize(len);
        ok = len == 0 || fread(rle.data(), 1, len, f) == len;
        size_t o = 0;
        for (size_t k = 0; ok && k + 1 < rle.size(); k += 2)
            for (uint8_t c = 0; c < rle[k] && o < px.size(); ++c) px[o++] = rle[k + 1];
        ok = ok && o == px.size() && make_rec_frame(ctx, i, w, h, px.data());
        if (ok) ++loaded;
    }
    fclose(f);
    if (ok && loaded == n) {
        g_recW = w; g_recH = h; g_recCount = n;
        g_recFileVer = static_cast<int>(hdr[1]);
        g_recKeep = g_recFileVer >= 2; // WIPEHD: a quarter-size v1 recording gets replaced by the next good recording
        BVR_LOG("gfx_hud: menu wipe loaded from disk (%d frames %ux%u, %llu ms)", n, w, h, g_recTime[n - 1]);
    } else {
        g_recCount = 0;
        BVR_LOG("gfx_hud: menu wipe file unreadable - ignored");
    }
}
float g_frameMaxScale = 0.0f; // render thread: largest non-cut piece this frame
float g_menuScale = 0.0f;     // ... of the last ordinary (non-wipe) UI frame
std::atomic<uint32_t> g_wipesHidden{0};
bool untextured(ID3D11DeviceContext* ctx) {
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    if (srv) { srv->Release(); return false; }
    return true;
}
std::atomic<ULONGLONG> g_lastWipeMs{0};
std::atomic<ULONGLONG> g_fadeSkipUntil{0};
std::atomic<uint32_t> g_stageMoves{0}, g_fadeSkips{0};
bool fill_piece(ID3D11DeviceContext* ctx) {
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv) return true;
    bool fill = false;
    ID3D11Resource* r = nullptr;
    srv->GetResource(&r);
    ID3D11Texture2D* t = nullptr;
    if (r && SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&t))) && t) {
        D3D11_TEXTURE2D_DESC td{};
        t->GetDesc(&td);
        fill = (td.Width == 4 && td.Height == 4) || (td.Height == 1 && td.Width <= 1024);
        t->Release();
    }
    if (r) r->Release();
    srv->Release();
    return fill;
}
bool g_prevPureCut = false;
float placement_scale(ID3D11DeviceContext* ctx) {
    ID3D11Buffer* cb = nullptr;
    ctx->VSGetConstantBuffers(0, 1, &cb);
    if (!cb) return 0.0f;
    float m[16] = {};
    const unsigned len = bvr::frame_inspector::cb_shadow(cb, m, sizeof(m));
    cb->Release();
    return len >= 64 ? fabsf(m[0]) + fabsf(m[1]) : 0.0f;
}
ID3D11Buffer* g_cutBuf = nullptr;
uint8_t g_cutOrig[640];

void cut_scale(ID3D11DeviceContext* ctx, Decision& d) {
    const float k = g_cutScale.load(std::memory_order_relaxed);
    if (k == 1.0f) return;
    ID3D11Buffer* cb = nullptr;
    ctx->VSGetConstantBuffers(0, 1, &cb);
    if (!cb) return;
    float m[160];
    const unsigned len = bvr::frame_inspector::cb_shadow(cb, m, sizeof(m));
    // JJ 2026-10-09 ("exiting the game from the pause menu, the bat cutscene still appears in the small window"): the
    // pause menu is a PERSPECTIVE Scaleform movie - capture 191956 huddump: rows (0.14 0 0 0 / 0 -0.13 0 0 / 0 0 1 1 /
    // -1.5e4 -3108 1.97e4 1.97e4), w = z + 19670 - so the old flat-only test (m15 == 1, m3 == m7 == 0) skipped its wipe
    // and the wipe stayed unscaled in the middle of each eye. Scaling the clip x and y COLUMNS (row vectors: x = col 0,
    // y = col 1) scales x/w and y/w about the centre for flat and perspective placements alike.
    if (len < 64 || !std::isfinite(m[15]) || m[15] == 0.0f) { cb->Release(); return; }
    memcpy(g_cutOrig, m, len);
    for (int i : {0, 4, 8, 12, 1, 5, 9, 13}) m[i] *= k;
    {
        bvr::frame_inspector::ScopedSuppress quiet;
        ctx->UpdateSubresource(cb, 0, nullptr, m, 0, 0);
    }
    g_cutBuf = cb; // reference kept until after_draw
    d.after = true;
}

// HUDZONES (JJ 2026-10-09: "some of the HUD elements are just too big ... the ability to resize them, and sliders to
// position them - move the compass up, the other stuff right and down"). Every panel piece is put in a zone by where
// its placement puts it (VS cb0 translation, NDC), then scaled about the zone's anchor (its screen corner / edge) and
// moved; the game's placement is put back after the draw (after_draw).
struct HudZone { const char* name; float ax, ay; std::atomic<float> s, dx, dy; };
HudZone g_zones[kHudZones] = {
    {"top left (health)", -1.0f, 1.0f, {1.0f}, {0.0f}, {0.0f}},
    {"top centre (compass)", 0.0f, 1.0f, {1.0f}, {0.0f}, {0.0f}},
    {"top right", 1.0f, 1.0f, {1.0f}, {0.0f}, {0.0f}},
    {"right side (tracker, prompts)", 1.0f, 0.0f, {1.0f}, {0.0f}, {0.0f}},
    {"left side", -1.0f, 0.0f, {1.0f}, {0.0f}, {0.0f}},
    {"bottom", 0.0f, -1.0f, {1.0f}, {0.0f}, {0.0f}},
    {"centre", 0.0f, 0.0f, {1.0f}, {0.0f}, {0.0f}},
};
std::atomic<uint32_t> g_zoneDraws[kHudZones];
std::atomic<bool> g_zonesOn{true}; // JJ 2026-10-09: one switch for all the group adjustments
std::atomic<bool> g_zonesGameplay{true}; // MENUCLOCK: false in menus, the title screen and movies - groups untouched

// GLOWZONE state (render thread): per frame, the k-th render-target paste -> the zone of the ordinary piece after it.
constexpr int kPasteMax = 64;
int g_pasteZoneCur[kPasteMax], g_pasteZonePrev[kPasteMax];
int g_pastePending[kPasteMax];
int g_pasteIdx = 0, g_pastePendingN = 0;
bool g_pasteInit = false;
// PICZONE (JJ 2026-10-11: the glow under "LEVEL UP" still sits below it, GLOWZONE did not catch it): capture 173005 fits
// the glow exactly to the LEVEL UP text moved by the top-left group (0.9x toward the corner, 0.1 down) while the text
// is top centre - the glow is an ordinary picture piece whose ANCHOR lies in the top-left zone while its picture lies
// under the text. A picture piece carries its texture mapping in the vertex constants (floats 16..31, laid out like the
// placement: framedump 173004, a 1024x256 picture has v scale 0.0002 = 1 / (256 px * 20 twips)), so the middle of its
// picture (uv 0.5, 0.5) can be put on screen. glowzone mode: 0 off; 1 render-target pastes only (GLOWZONE); 2 + a picture
// piece goes to the zone of its picture's middle when the next piece (same order, last HUD frame) agrees (default);
// 3 + the picture's middle alone; 4 + a piece unlike both neighbours takes the next piece's zone.
constexpr int kPieceMax = 512;
int g_anchorCur[kPieceMax], g_anchorPrev[kPieceMax]; // anchor zone of the k-th ordinary piece, this / last HUD frame
int g_pieceIdx = 0, g_prevAnchor = -1;
std::atomic<int> g_glowZoneMode{2};
std::atomic<uint32_t> g_picZoneMoved{0};
struct PicSeen { unsigned w, h; int fmt, from, to; bool moved; };
PicSeen g_picSeen[32];
int g_picSeenN = 0;
void paste_frame_end() {
    if (!g_pasteInit) {
        for (int i = 0; i < kPasteMax; ++i) g_pasteZoneCur[i] = g_pasteZonePrev[i] = -1;
        for (int i = 0; i < kPieceMax; ++i) g_anchorCur[i] = g_anchorPrev[i] = -1;
        g_pasteInit = true;
    }
    for (int i = 0; i < kPasteMax; ++i) { g_pasteZonePrev[i] = g_pasteZoneCur[i]; g_pasteZoneCur[i] = -1; }
    g_pasteIdx = 0;
    g_pastePendingN = 0;
    for (int i = 0; i < kPieceMax; ++i) { g_anchorPrev[i] = g_anchorCur[i]; g_anchorCur[i] = -1; }
    g_pieceIdx = 0;
    g_prevAnchor = -1;
}

int zone_of(float x, float y) {
    if (y > 0.35f) return x < -0.5f ? 0 : (x > 0.5f ? 2 : 1);
    if (x > 0.3f) return 3;
    if (x < -0.3f) return 4;
    if (y < -0.45f) return 5;
    return 6;
}

// The zone of the middle of this piece's picture, or -1 (no usable texture mapping / off screen).
int picture_zone(const float* m, unsigned lenBytes, float& cx, float& cy) {
    if (lenBytes < 128) return -1;
    const float a = m[16], b = m[17], c = m[20], d = m[21], det = a * d - b * c;
    if (!(fabsf(det) > 1e-14f) || m[31] != 1.0f) return -1;
    const float r0 = 0.5f - m[28], r1 = 0.5f - m[29];
    const float x = (r0 * d - r1 * c) / det, y = (r1 * a - r0 * b) / det; // local point of uv (0.5, 0.5)
    cx = x * m[0] + y * m[4] + m[12];
    cy = x * m[1] + y * m[5] + m[13];
    if (!(fabsf(cx) <= 1.05f && fabsf(cy) <= 1.05f)) return -1;
    return zone_of(cx, cy);
}

void zone_adjust(ID3D11DeviceContext* ctx, Decision& d) {
    ID3D11Buffer* cb = nullptr;
    ctx->VSGetConstantBuffers(0, 1, &cb);
    if (!cb) return;
    float m[160];
    const unsigned len = bvr::frame_inspector::cb_shadow(cb, m, sizeof(m));
    // A flat Scaleform piece only (4x4 placement, translation in row 3).
    if (len < 64 || m[15] != 1.0f || m[3] != 0.0f || m[7] != 0.0f || m[14] != 0.0f) { cb->Release(); return; }
    int z = zone_of(m[12], m[13]);
    // GLOWZONE (JJ 2026-10-10, capture 173005: the glow under "LEVEL UP" sits below and left of the text): Scaleform
    // pastes a glow / blur result (a small render target) anchored at the stage's top-left corner, so its anchor says
    // zone 0 (top left: moved and shrunk) while its text is in another zone. Each paste is drawn just before its own
    // text, so it takes the zone of the next ordinary piece - learned in the previous HUD frame (frames repeat); not
    // known yet = not moved.
    {
        ID3D11ShaderResourceView* srv = nullptr;
        ctx->PSGetShaderResources(0, 1, &srv);
        bool rtPaste = false;
        D3D11_TEXTURE2D_DESC td{};
        if (srv) {
            ID3D11Resource* r = nullptr;
            srv->GetResource(&r);
            ID3D11Texture2D* t = nullptr;
            if (r && SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&t))) && t) {
                t->GetDesc(&td);
                rtPaste = (td.BindFlags & D3D11_BIND_RENDER_TARGET) && td.Width < 1000;
                t->Release();
            }
            if (r) r->Release();
            srv->Release();
        }
        const int mode = g_glowZoneMode.load(std::memory_order_relaxed);
        if (rtPaste && mode >= 1) {
            const int k = g_pasteIdx++;
            if (k < kPasteMax) g_pastePending[g_pastePendingN < kPasteMax ? g_pastePendingN++ : kPasteMax - 1] = k;
            z = k < kPasteMax ? g_pasteZonePrev[k] : -1;
            if (z < 0) { cb->Release(); return; } // not learned yet: leave it where the game put it
        } else {
            // PICZONE (modes 2-4, above): the anchor zone is z; neighbours by draw order, the next one from the last frame
            const int a = z, k = g_pieceIdx++;
            const int prev = g_prevAnchor;
            // last frame lined up with this one around piece k (its k-1 and k match): else a widget fading in / out shifted
            // the order and "the piece after" is another piece (log 01:38: a LEVEL UP letter sent to group 0 once)
            const bool aligned = k + 1 < kPieceMax && g_anchorPrev[k] == a && (k == 0 || g_anchorPrev[k - 1] == prev);
            const int next = aligned ? g_anchorPrev[k + 1] : -1;
            if (k < kPieceMax) g_anchorCur[k] = a;
            g_prevAnchor = a;
            if (mode >= 2 && g_pasteInit) {
                float cx = 0.0f, cy = 0.0f;
                const bool picture = td.Width >= 64 && td.Height >= 16; // a real picture, not a font page / solid fill
                const int pz = mode <= 3 && picture ? picture_zone(m, len, cx, cy) : -1;
                int to = a;
                // SANDWICH (JJ 2026-10-11 01:3x: PICZONE did not catch it; framedump 173004: the glow is a 42-point shape,
                // anchor -0.543,0.600 = group 0, drawn between the LEVEL UP chevrons and letters, all group 1): a piece
                // between two pieces of one other group belongs to that group.
                if (mode == 2 && prev >= 0 && prev == next && a != prev) to = prev;
                else if (mode == 2 && pz >= 0 && pz != a && pz == next) to = pz;
                else if (mode == 3 && pz >= 0 && pz != a) to = pz;
                else if (mode == 4 && prev >= 0 && next >= 0 && a != prev && a != next) to = next;
                if (to != a) g_picZoneMoved.fetch_add(1, std::memory_order_relaxed);
                // log each kind of piece once: regrouped, or its picture's middle in another zone but left alone
                if ((to != a || (pz >= 0 && pz != a)) && g_picSeenN < 32) {
                    bool seen = false;
                    for (int i = 0; i < g_picSeenN; ++i) {
                        const PicSeen& p = g_picSeen[i];
                        seen |= p.w == td.Width && p.h == td.Height && p.fmt == int(td.Format) && p.from == a && p.to == pz;
                    }
                    if (!seen) {
                        g_picSeen[g_picSeenN++] = PicSeen{td.Width, td.Height, int(td.Format), a, pz, to != a};
                        BVR_LOG("gfx_hud: piece %ux%u fmt %d bind 0x%x: anchor %.3f,%.3f = group %d, picture middle "
                                "%.3f,%.3f = group %d, pieces before / after: group %d / %d -> %s %d (glowzone mode %d)",
                                td.Width, td.Height, int(td.Format), td.BindFlags, m[12], m[13], a, cx, cy, pz, prev, next,
                                to != a ? "MOVED to group" : "left in group", to, mode);
                    }
                }
                z = to;
            }
            for (int i = 0; i < g_pastePendingN; ++i) g_pasteZoneCur[g_pastePending[i]] = z;
            g_pastePendingN = 0;
        }
    }
    g_zoneDraws[z].fetch_add(1, std::memory_order_relaxed);
    if (!g_zonesOn.load(std::memory_order_relaxed) || !g_zonesGameplay.load(std::memory_order_relaxed)) {
        cb->Release();
        return;
    }
    const HudZone& Z = g_zones[z];
    const float s = Z.s.load(std::memory_order_relaxed), dx = Z.dx.load(std::memory_order_relaxed),
                dy = Z.dy.load(std::memory_order_relaxed);
    if (s == 1.0f && dx == 0.0f && dy == 0.0f) { cb->Release(); return; }
    memcpy(g_cutOrig, m, len);
    for (int i : {0, 1, 4, 5}) m[i] *= s;
    m[12] = Z.ax + (m[12] - Z.ax) * s + dx;
    m[13] = Z.ay + (m[13] - Z.ay) * s + dy;
    {
        bvr::frame_inspector::ScopedSuppress quiet;
        ctx->UpdateSubresource(cb, 0, nullptr, m, 0, 0);
    }
    g_cutBuf = cb; // reference kept until after_draw
    d.after = true;
}

// BATCUTS: no texture at slot 0, or a screen-size one (width >= 1000).
bool is_cut_piece(ID3D11DeviceContext* ctx) {
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv) return true;
    bool big = false;
    ID3D11Resource* r = nullptr;
    srv->GetResource(&r);
    ID3D11Texture2D* t = nullptr;
    if (r && SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&t))) && t) {
        D3D11_TEXTURE2D_DESC td{};
        t->GetDesc(&td);
        // 2026-10-09: HUD headings ("Joker Radio Signal Strength") are drawn from a large square font atlas and were
        // caught as wipe pieces (pushed into the eye image and scaled 1.6x). The wipe's screen-size picture is a
        // rendered target or a non-square movie frame; a square power-of-two atlas is never one.
        const bool square = td.Width == td.Height;
        big = td.Width >= 1000 && ((td.BindFlags & D3D11_BIND_RENDER_TARGET) || !square);
        static int s_logged = 0;
        if (td.Width >= 1000 && s_logged < 6) {
            ++s_logged;
            BVR_LOG("gfx_hud: large HUD texture %ux%u fmt %d bind 0x%x -> %s", td.Width, td.Height, int(td.Format),
                    td.BindFlags, big ? "wipe picture" : "not a wipe (font / atlas)");
        }
        t->Release();
    }
    if (r) r->Release();
    srv->Release();
    return big;
}
std::atomic<MarkerAfterFn> g_markerAfterFn{nullptr};

// HUDDSV: the HUD's own depth-stencil for this frame (made / remade to match the game's), or the game's on failure.
ID3D11DepthStencilView* hud_dsv(ID3D11DeviceContext* ctx) {
    ID3D11DepthStencilView* game = g_curDsv;
    if (!g_hudDsvOn.load(std::memory_order_relaxed) || !game) return game;
    ID3D11Resource* r = nullptr;
    game->GetResource(&r);
    ID3D11Texture2D* t = nullptr;
    if (r) { r->QueryInterface(IID_PPV_ARGS(&t)); r->Release(); }
    if (!t) return game;
    D3D11_TEXTURE2D_DESC td{};
    t->GetDesc(&td);
    t->Release();
    if (g_hudDsvTex) {
        D3D11_TEXTURE2D_DESC od{};
        g_hudDsvTex->GetDesc(&od);
        if (od.Width != td.Width || od.Height != td.Height || od.Format != td.Format ||
            od.SampleDesc.Count != td.SampleDesc.Count) {
            if (g_hudDsv) { g_hudDsv->Release(); g_hudDsv = nullptr; }
            g_hudDsvTex->Release(); g_hudDsvTex = nullptr;
        }
    }
    if (!g_hudDsvTex) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return game;
        D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
        game->GetDesc(&vd);
        D3D11_TEXTURE2D_DESC nd = td;
        nd.MipLevels = 1; nd.ArraySize = 1; nd.Usage = D3D11_USAGE_DEFAULT;
        nd.BindFlags = D3D11_BIND_DEPTH_STENCIL; nd.CPUAccessFlags = 0; nd.MiscFlags = 0;
        const bool ok = SUCCEEDED(dev->CreateTexture2D(&nd, nullptr, &g_hudDsvTex)) &&
                        SUCCEEDED(dev->CreateDepthStencilView(g_hudDsvTex, &vd, &g_hudDsv));
        dev->Release();
        BVR_LOG("gfx_hud: HUD's own depth-stencil %ux%u fmt %d %s", td.Width, td.Height, int(td.Format),
                ok ? "ready" : "FAILED - the game's is used");
        if (!ok) {
            if (g_hudDsv) { g_hudDsv->Release(); g_hudDsv = nullptr; }
            if (g_hudDsvTex) { g_hudDsvTex->Release(); g_hudDsvTex = nullptr; }
            g_hudDsvOn.store(false);
            return game;
        }
    }
    g_hudDsvSrc = game;
    if (!g_hudDsvCleared) {
        g_hudDsvCleared = true;
        bvr::frame_inspector::ScopedSuppress quiet;
        ctx->ClearDepthStencilView(g_hudDsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, g_sceneClearDepth, 0);
    }
    return g_hudDsv;
}

Decision classify_inner(ID3D11DeviceContext* ctx, unsigned count, bool indexed);
Decision classify(ID3D11DeviceContext* ctx, unsigned count, bool indexed) { return classify_inner(ctx, count, indexed); }
Decision classify_inner(ID3D11DeviceContext* ctx, unsigned count, bool indexed) {
    Decision d{};
    if (!g_armed.load(std::memory_order_relaxed)) return d;
    if (!g_onBackbuffer) return d;
    if (!g_pastBoundary) {
        if (indexed && is_boundary_blit(ctx, count)) {
            g_pastBoundary = true;
            g_cBoundaries.fetch_add(1, std::memory_order_relaxed);
            if (g_mapBlit.load(std::memory_order_relaxed) && GetTickCount64() < g_mapUntilMs &&
                GetTickCount64() - g_mapShapeMs < kMapBlitHoldMs && g_clockStopped.load(std::memory_order_relaxed) && // MAPBLITTAIL
                g_redirect.load(std::memory_order_relaxed) && g_gameRtv && ensure_capture(ctx)) { // MAPBLIT
                g_cMapBlit.fetch_add(1, std::memory_order_relaxed);
                const float black[4] = {0, 0, 0, 1};
                ctx->ClearRenderTargetView(g_gameRtv, black);
                if (!g_clearedThisFrame) {
                    g_clearedThisFrame = true;
                    const float clear[4] = {0, 0, 0, 0};
                    ctx->ClearRenderTargetView(g_capRtv, clear);
                }
                g_cRedirected.fetch_add(1, std::memory_order_relaxed);
                g_ourBound = true;
                d.redirectRtv = g_capRtv;
                d.redirectDsv = hud_dsv(ctx);
                d.blend = fixed_blend(ctx);
            }
        } else if (g_ourBound) {
            // A stale redirect binding survived into a pre-boundary draw
            // (present rolled while our RT was bound and the game has not
            // rebound yet). Hand the game's binding back.
            g_ourBound = false;
            d.restoreRtv = g_gameRtv;
            d.restoreDsv = g_curDsv;
        }
        return d;
    }
    // Post-boundary backbuffer draw = UI-run content.
    g_cHudDraws.fetch_add(1, std::memory_order_relaxed);
    ++g_frameHud;
    const bool dumping = g_dumpLeft.load(std::memory_order_relaxed) > 0;
    g_lineCur = -1;
    if (g_lineLeft.load(std::memory_order_relaxed) > 0 || g_ringOn.load(std::memory_order_relaxed)) line_note(ctx);
    // MARKEREYE: world markers stay in the eye image, moved for this eye by the adapter.
    const bool cutType = g_cutsToEye.load(std::memory_order_relaxed) && is_cut_piece(ctx);
    if (cutType) ++g_frameCut; else ++g_frameOther;
    const float pieceScale = placement_scale(ctx);
    const float cutScale = cutType ? pieceScale : 0.0f;
    const bool gameplay = g_zonesGameplay.load(std::memory_order_relaxed);
    const ULONGLONG nowMs = GetTickCount64();
    const bool stageFill = g_stageBox.load(std::memory_order_relaxed) && g_cutsToEye.load(std::memory_order_relaxed) &&
                           pieceScale >= 1.5e-4f && pieceScale < 1.0e-3f && fill_piece(ctx);
    // MENUFADESKIP (JJ 2026-10-10 00:15: after FADEMIN "the faint border of another window that covers maybe 70 % of my
    // field of view ... when the menu appears" after quitting to the menu): the main menu's fade-in from black is drawn
    // as stage-scale fill pieces (STAGEBOX notes: 1.9e-4 .. 6.7e-4; the main menu's own pieces sit at ~1e-4 and ~1.9),
    // so for 2 s after a wipe replay ends those pieces are not drawn - the menu itself stays.
    if (stageFill && g_replayEndMs && nowMs - g_replayEndMs < 2000) {
        static uint32_t s_skips = 0;
        if (++s_skips <= 3) BVR_LOG("gfx_hud: menu fade-in piece after a wipe not drawn (placement %.3g)", pieceScale);
        if (g_ourBound) { g_ourBound = false; d.restoreRtv = g_gameRtv; d.restoreDsv = g_curDsv; }
        d.skip = true;
        return d;
    }
    if (stageFill && gameplay && nowMs < g_fadeSkipUntil.load(std::memory_order_relaxed)) {
        if (g_fadeSkips.fetch_add(1, std::memory_order_relaxed) < 3)
            BVR_LOG("gfx_hud: after-loading fade piece not drawn (placement %.3g)", pieceScale);
        if (g_ourBound) { g_ourBound = false; d.restoreRtv = g_gameRtv; d.restoreDsv = g_curDsv; }
        d.skip = true;
        return d;
    }
    // JJ 2026-10-09 19:18: "the logo that says Batman Arkham City is now stuck to my face and huge" - the title logo is
    // a 1024x256 picture (a cut-type piece) drawn FIRST, and stage-scale fills: FIRSTCUT and STAGEBOX's "outside
    // gameplay" branch took it into the eye. Both removed; STAGEBOX only next to a wipe now.
    const bool stageBox = stageFill && nowMs - g_lastWipeMs.load(std::memory_order_relaxed) < 1000;
    const bool firstCut = false;
    if (stageBox && g_stageMoves.fetch_add(1, std::memory_order_relaxed) < 6)
        BVR_LOG("gfx_hud: stage-size fill piece kept with the wipe (placement %.3g, %s)", pieceScale,
                gameplay ? "just after a wipe" : "menus");
    if (cutType && cutScale > 1.0e-3f) g_wipeFrame = true;
    // WIPEALL only takes pieces at the wipe's (menu movie's) scale: capture 161059 showed the first gameplay HUD frame
    // after a pause wipe (pieces ~1e-4) pushed whole into the eye by the "next frame" rule.
    const bool wipeRest = g_cutsToEye.load(std::memory_order_relaxed) && (g_wipeFrame || g_wipePrev) &&
                          pieceScale > 1.0e-3f;
    // MENUCUT (test switch, JJ 2026-10-09: a small black square on the panel as the menu wipes start and end - their
    // first / last frames are not "pure cut" frames): outside gameplay every cut-type piece stays in the eye.
    const bool menuCut = cutType && g_menuCut.load(std::memory_order_relaxed) &&
                         !g_zonesGameplay.load(std::memory_order_relaxed);
    if (!cutType && pieceScale > g_frameMaxScale) g_frameMaxScale = pieceScale;
    const bool keep = wipeRest || menuCut || stageBox || firstCut || (cutType && (g_prevPureCut || cutScale > 1.5e-4f));
    // WIPEMAIN: the main menu -> game wipe (kept as it was, and recorded) - decided when a wipe starts, held through it.
    const bool squareRunning = nowMs - g_squareSeenMs < 400;
    const bool mainWipe = nowMs < g_mainWipeUntil ||
                          (!squareRunning && g_menuScale >= 1.0f && nowMs - g_mainMenuAt.load(std::memory_order_relaxed) < 500);
    static uint32_t s_wipeLogs = 0;
    if (keep && nowMs - g_lastKeepMs > 400 && !mainWipe &&
        !(gameplay && nowMs - g_movieEndMs.load(std::memory_order_relaxed) < 3000))
        g_squareStarts.fetch_add(1, std::memory_order_relaxed);
    if (keep && nowMs - g_lastKeepMs > 400) {
        wipe_event(2); // WIPESOUND4: every bat wipe of the game starts here (its sound plays now)
        if (++s_wipeLogs <= 60)
            BVR_LOG("gfx_hud: wipe starts (%s, start screen placement %.3g, %s) t%llu", mainWipe ? (g_recKeep ? "main menu -> game: shown (saved copy kept)" : "main menu -> game: shown, recorded") :
                    "square: hidden, replayed", g_menuScale, gameplay ? "gameplay" : "menus", nowMs % 1000000ULL);
    }
    // MENUOPEN3 (JJ 2026-10-10 17:28: "back to the zoomed moon" logos -> first menu; the title was not recognised under a
    // texture pack, so the boot transition counted as gameplay): only after a loading screen (the second full-screen movie: the first is the logos).
    const bool inWorld = g_movieEnds.load(std::memory_order_relaxed) >= 2; // the logos, then a loading screen
    if (keep && nowMs - g_lastKeepMs > 400) g_wipeFromGameplay = gameplay && inWorld; // MENUOPEN2: where this wipe STARTED
    if (keep && nowMs - g_lastKeepMs > 400 && !gameplay) g_menuWipeAtMs.store(nowMs, std::memory_order_relaxed); // MS2
    if (keep) g_lastKeepMs = nowMs;
    if (keep && mainWipe) g_mainWipeUntil = nowMs + 400;
    // WIPEMAIN2 (JJ 2026-10-09 23:00: save select -> title still "a small black box as well as the full screen bats"):
    // while the recorded wipe plays, no HUD piece is drawn at all (eye or panel).
    const bool replayAll = g_replayHidesAll.load(std::memory_order_relaxed) &&
                           (g_replayOn.load(std::memory_order_relaxed) || nowMs < g_hideAllUntil);
    if (replayAll && nowMs < g_hideAllCap && untextured(ctx) && nowMs + 300 > g_hideAllUntil)
        g_hideAllUntil = nowMs + 300 < g_hideAllCap ? nowMs + 300 : g_hideAllCap; // the game's wipe still runs
    // MAPSCREEN (JJ 2026-10-10: the map showed its text but not the city; MAPTEST 03:23: only "menu-wipe hiding off"
    // brought it back, panel 92 % lit vs 6 %): the map draws its city as very large untextured Scaleform shapes
    // (frame dump 031117: 1008-13455 indices each, every frame, clock stopped = "not gameplay"), which the rule "outside
    // gameplay every untextured piece is a wipe" hid. Wipe squares / rings / bat sprites are small or textured. A
    // big untextured shape = the map screen: no wipe hiding (and no replay over it) while it is up, 1.5 s hold.
    ULONGLONG& s_mapUntil = g_mapUntilMs; // shared with MAPBLIT
    if (indexed && count >= 2000 && g_mapScreenOn.load(std::memory_order_relaxed) && untextured(ctx)) {
        if (nowMs >= s_mapUntil) {
            BVR_LOG("gfx_hud: map screen (untextured shape of %u indices) - wipe hiding paused while it is up", count);
            if (g_replayOn.load(std::memory_order_relaxed)) { g_replayOn.store(false); g_replayEndMs = nowMs; }
            g_hideAllUntil = 0;
        }
        s_mapUntil = nowMs + 1500;
        g_mapShapeMs = nowMs; // MAPBLITTAIL
    }
    const bool mapScreen = nowMs < s_mapUntil;
    if (!mapScreen && g_hideWipes.load(std::memory_order_relaxed) && g_cutsToEye.load(std::memory_order_relaxed) &&
        !mainWipe && // the main menu -> game wipe stays as it was
        ((keep && (!gameplay || pieceScale > 1.0e-3f || g_wipeFrame || g_wipePrev)) ||
         (!gameplay && untextured(ctx)) || replayAll)) {
        if (g_lineCur >= 0) ++g_lineTex[g_lineCur].hid;
        if (g_blackFillOn.load(std::memory_order_relaxed) && !g_replayOn.load(std::memory_order_relaxed)) { // BLACKFILL
            if (untextured(ctx) && pieceScale >= 1.5e-4f && pieceScale < 1.0e-3f) {
                if (nowMs >= g_blackUntil) {
                    g_blackCap = nowMs + 6000;
                    BVR_LOG("gfx_hud: the game's full-screen black (stage fill, placement %.3g) -> whole view black", pieceScale);
                }
                g_blackUntil = nowMs + 150 < g_blackCap ? nowMs + 150 : g_blackCap;
                g_blackFillAt = nowMs;
            } else if (nowMs < g_blackUntil + 1000 && nowMs < g_blackCap && nowMs - g_blackFillAt < 1200) {
                g_blackUntil = nowMs + 150 < g_blackCap ? nowMs + 150 : g_blackCap; // the wipe that follows it
                g_blackFillAt = nowMs;
            }
        }
        if (!g_clearedThisFrame && ensure_capture(ctx)) { // WIPEMAIN3: the panel must not keep an older picture
            g_clearedThisFrame = true;
            const float clear[4] = {0, 0, 0, 0};
            ctx->ClearRenderTargetView(g_capRtv, clear);
        }
        if (cutType || wipeRest) g_lastWipeMs.store(nowMs, std::memory_order_relaxed);
        if (g_wipesHidden.fetch_add(1, std::memory_order_relaxed) < 3)
            BVR_LOG("gfx_hud: menu wipe piece not drawn (placement %.3g, %s)", pieceScale, gameplay ? "wipe scale" : "menus");
        if (keep) { // WIPEREPLAY: a square wipe is running - play the recorded one over the view
            g_squareSeenMs = nowMs;
            if (!g_replayLocked && g_recCount > 8 && g_replayEnabled.load(std::memory_order_relaxed) &&
                nowMs - g_movieEndMs.load(std::memory_order_relaxed) > 3000) {
                g_replayLocked = true;
                g_replayStart = nowMs;
                g_hideAllUntil = nowMs + g_recTime[g_recCount - 1] + 50;
                // MENUOPEN (JJ 2026-10-10: the map / upgrade screen "takes quite a long time to appear"; log 03:31: every
                // opening from gameplay held "wipe replay ended after 4000 ms"): the arriving screen keeps drawing untextured
                // pieces, which kept extending the hold to the 4 s cap with no HUD shown. Opening a menu from gameplay: the
                // hold ends g_menuOpenHoldMs after the recording; wipes inside the menus keep the 4 s cap (WIPEMAIN3).
                g_hideAllCap = nowMs + (g_wipeFromGameplay ? g_recTime[g_recCount - 1] + ULONGLONG(g_menuOpenHoldMs.load(std::memory_order_relaxed)) : 4000);
                g_replayInvert = g_wipeFlipOn.load(std::memory_order_relaxed) && g_menuScale < 1.0e-3f && !gameplay;
                g_replayOn.store(true);
                g_replays.fetch_add(1, std::memory_order_relaxed);
                if (!g_replayInvert) wipe_event(0); // WIPESOUND: a wipe to black - the game's own bat sound plays now
                BVR_LOG("gfx_hud: square wipe replaced by the recorded menu wipe (%d frames, %llu ms%s)", g_recCount,
                        g_recTime[g_recCount - 1], g_replayInvert ? ", FLIPPED: black bats over the arriving menu" : "");
            }
        }
        if (g_ourBound) { g_ourBound = false; d.restoreRtv = g_gameRtv; d.restoreDsv = g_curDsv; }
        d.skip = true;
        return d;
    }
    // OPENFADE (JJ 2026-10-10 03:38: "still flickering on occasion" opening the upgrade / map screen; HUD rings 033614 and
    // 033847): every opening starts with two untextured pieces at placement ~5.3e-5 in gameplay - one frame on the panel
    // (a dark box the size of the panel), the next sent full-view into the eye as a "pure cut" frame, then the wipe
    // hiding takes over (here: only pieces the wipe hiding above let through). Gameplay HUD untextured pieces sit at ~2.7e-5, after-loading fades at 1.9e-4. Not drawn.
    if (gameplay && inWorld && g_openFadeOn.load(std::memory_order_relaxed) && pieceScale > 4.8e-5f && pieceScale < 5.8e-5f &&
        untextured(ctx)) {
        if (g_openFadeSkips.fetch_add(1, std::memory_order_relaxed) < 3)
            BVR_LOG("gfx_hud: menu-opening fade piece not drawn (placement %.3g)", pieceScale);
        if (g_ourBound) { g_ourBound = false; d.restoreRtv = g_gameRtv; d.restoreDsv = g_curDsv; }
        d.skip = true;
        return d;
    }
    if (keep && g_hideWipes.load(std::memory_order_relaxed) && mainWipe && ensure_rec(ctx)) {
        // WIPEREPLAY: the main menu -> game wipe - drawn into the recording target (shown over the view at present).
        if (cutType || wipeRest) g_lastWipeMs.store(nowMs, std::memory_order_relaxed);
        g_cutsKept.fetch_add(1, std::memory_order_relaxed);
        if (g_lineCur >= 0) ++g_lineTex[g_lineCur].eye;
        if (!g_recCleared) {
            g_recCleared = true;
            const float clear[4] = {0, 0, 0, 0};
            ctx->ClearRenderTargetView(g_recRtv, clear);
        }
        g_recThisFrame = true;
        g_ourBound = true; // our target is bound: the next game-bound draw gets the game's own back
        d.redirectRtv = g_recRtv;
        d.redirectDsv = hud_dsv(ctx); // HUDDSV
        d.blend = fixed_blend(ctx);
        return d;
    }
    if (keep) {
        if (cutType || wipeRest) g_lastWipeMs.store(nowMs, std::memory_order_relaxed);
        g_cutsKept.fetch_add(1, std::memory_order_relaxed);
        if (g_lineCur >= 0) ++g_lineTex[g_lineCur].eye;
        if (g_ourBound) { // an earlier piece went to the panel: give the game its target back
            g_ourBound = false;
            d.restoreRtv = g_gameRtv;
            d.restoreDsv = g_curDsv;
        }
        if (dumping) BVR_LOG("[huddump] #%u (transition cut piece, kept in the eye image)", g_dumpIndex);
        cut_scale(ctx, d);
        return d;
    }
    if (MarkerFn mf = g_markerFn.load(std::memory_order_relaxed)) {
        if (mf(ctx)) {
            d.after = true;
            if (g_ourBound) { // an earlier piece went to the panel: give the game its target back
                g_ourBound = false;
                d.restoreRtv = g_gameRtv;
                d.restoreDsv = g_curDsv;
            }
            if (dumping) BVR_LOG("[huddump] #%u (marker piece, moved for this eye, kept in the eye image)", g_dumpIndex);
            if (dumping) dump_draw(ctx, count, indexed, false);
            return d;
        }
    }
    if (dumping && (count <= kFxMaxCount && g_fxPassthrough.load(std::memory_order_relaxed) ||
                    !g_redirect.load(std::memory_order_relaxed)))
        dump_draw(ctx, count, indexed, false);
    // The effects census + pass-through: a full-screen-ish overlay (few
    // indices/vertices) is what a GFx damage flash / vignette looks like -
    // measured live s52: ~0.3/frame at rest, ~3.6/frame during an RPG
    // self-hit. Passing them through keeps effects across the whole view.
    if (count <= kFxMaxCount) {
        g_cBigPost.fetch_add(1, std::memory_order_relaxed);
        if (g_fxPassthrough.load(std::memory_order_relaxed)) return d;
    }
    zone_adjust(ctx, d); // HUDZONES: this piece resized / moved with its zone
    if (!g_redirect.load(std::memory_order_relaxed)) return d;
    if (!ensure_capture(ctx)) return d;
    if (!g_clearedThisFrame) {
        g_clearedThisFrame = true;
        const float clear[4] = {0, 0, 0, 0};
        ctx->ClearRenderTargetView(g_capRtv, clear);
    }
    g_cRedirected.fetch_add(1, std::memory_order_relaxed);
    g_ourBound = true;
    d.redirectRtv = g_capRtv;
    d.redirectDsv = hud_dsv(ctx); // HUDDSV: its own depth-stencil (the scene's stencil broke the clip masks)
    d.blend = fixed_blend(ctx);
    if (d.blend) g_cBlendFixed.fetch_add(1, std::memory_order_relaxed);
    if (dumping) dump_draw(ctx, count, indexed, true);
    return d;
}

} // namespace

extern ULONGLONG g_exitUntil; // EXITFADE, defined below (UNPAUSECUT reads it)

ID3D11DepthStencilView* game_dsv() { return g_curDsv; }
ID3D11Resource* scene_depth() { return g_sceneDepth; }
const char* hud_zone_name(int i) { return i >= 0 && i < kHudZones ? g_zones[i].name : ""; }
void get_hud_zone(int i, float* s, float* dx, float* dy) {
    if (i < 0 || i >= kHudZones) return;
    *s = g_zones[i].s.load(); *dx = g_zones[i].dx.load(); *dy = g_zones[i].dy.load();
}
void set_hud_zone(int i, float s, float dx, float dy) {
    if (i < 0 || i >= kHudZones) return;
    g_zones[i].s.store(s); g_zones[i].dx.store(dx); g_zones[i].dy.store(dy);
}
void set_hud_zones_on(bool on) { g_zonesOn.store(on); }
void set_hud_zones_gameplay(bool on) { g_zonesGameplay.store(on, std::memory_order_relaxed); }
bool hud_zones_gameplay() { return g_zonesGameplay.load(std::memory_order_relaxed); }
bool hud_zones_on() { return g_zonesOn.load(); }
uint32_t hud_zone_draws(int i) { return i >= 0 && i < kHudZones ? g_zoneDraws[i].load() : 0; }
void set_cuts_to_eye(bool on) { g_cutsToEye.store(on, std::memory_order_relaxed); }
// WIPEREPLAY, at present BEFORE the eye pictures are taken (d3d11_hook): lay this frame's recorded wipe, or the replayed
// one, over the whole view; store recorded frames.
std::atomic<BeforeCaptureHook> g_beforeCaptureHook{nullptr};
void set_before_capture_hook(BeforeCaptureHook h) { g_beforeCaptureHook.store(h); }
void before_eye_capture(ID3D11DeviceContext* ctx, IDXGISwapChain* sc) {
    if (BeforeCaptureHook h = g_beforeCaptureHook.load(std::memory_order_relaxed)) h(ctx, sc); // HD LOGO
    const ULONGLONG now = GetTickCount64();
    {   // WIPESEQ test (JJ 23:25: "still briefly showing a smaller box when the bats first appear" - shorter than the
        // 0.4 s auto capture): for the first 3 replays of a session, both eyes at presents 0/2/4/8/16 of the replay and
        // the panel for its first 12 HUD frames -> %LOCALAPPDATA%\Batman_City\wipeseq_<time>\.
        static uint32_t s_seen = 0, s_seqs = 0;
        static int s_step = -1;
        static ULONGLONG s_nextLong = 0;
        static wchar_t s_dir[MAX_PATH] = {};
        static ULONGLONG s_t0 = 0;
        // WIPESEQ3 (JJ 23:45: the box stays with the replay switched off): every square wipe in menus starts a sequence
        // (first 3): eyes at presents 0/4/16, then eyes + panel every 250 ms from 0.5 to 2.5 s.
        const uint32_t r = g_squareStarts.load(std::memory_order_relaxed);
        if (r != s_seen) {
            s_seen = r;
            if (s_seqs < 3 && g_wipeDebug.load(std::memory_order_relaxed)) {
                ++s_seqs;
                s_t0 = now;
                SYSTEMTIME st; GetLocalTime(&st);
                swprintf_s(s_dir, L"%s\\wipeseq_%02u%02u%02u", bvr::log::data_dir(), st.wHour, st.wMinute, st.wSecond);
                request_panel_sequence(s_dir, 12);
                s_step = 0;
            }
        }
        if (s_step >= 0) {
            if (s_step == 0 || s_step == 4 || s_step == 16) {
                wchar_t d[MAX_PATH];
                swprintf_s(d, L"%s\\eyes_present%02d_t%llu", s_dir, s_step, now - s_t0);
                CreateDirectoryW(d, nullptr);
                bvr::vr::save_eye_pictures(d);
            }
            if (++s_step > 16) {
                s_step = -1;
                s_nextLong = s_t0 + 500; // WIPESEQ2/3: eyes + panel every 250 ms until 2.5 s
            }
        }
        if (s_nextLong && now >= s_nextLong) {
            wchar_t d[MAX_PATH];
            swprintf_s(d, L"%s\\eyes_t%04llu", s_dir, now - s_t0);
            CreateDirectoryW(d, nullptr);
            bvr::vr::save_eye_pictures(d);
            wchar_t pp[MAX_PATH];
            swprintf_s(pp, L"%s\\hud_panel.bmp", d);
            if (g_capTex) save_texture_bmp(ctx, g_capTex, pp);
            s_nextLong = now - s_t0 >= 2500 ? 0 : now + 250;
        }
    }
    // UNPAUSECUT (JJ 2026-10-11 22:16: unpausing gave a black screen and a bat transition, "it should just go straight
    // back", and then the audio was gone). Log 22:16:36: closing the pause menu draws a square-wipe piece (placement
    // 0.189, menus) -> the recorded replay started, held black to the 4 s cap, then WIPEREVEAL (with its sound). Quitting
    // starts the same way, but the game clock stays stopped into the load. So the clock running again within 2 s of a
    // menu-side replay start = unpause: the replay is dropped at once, no reveal, the game shows straight away.
    if (const ULONGLONG resumed = g_resumedMs.exchange(0, std::memory_order_relaxed)) {
        if (g_replayOn.load(std::memory_order_relaxed) &&
            !g_wipeFromGameplay && resumed >= g_exitUntil && resumed >= g_replayStart && resumed - g_replayStart < 2000) {
            g_replayOn.store(false);
            g_replayInvert = false;
            g_hideAllUntil = 0;
            g_hideAllCap = 0;
            g_blackUntil = 0;
            g_replayEndMs = now;
            g_unpauseCuts.fetch_add(1, std::memory_order_relaxed);
            BVR_LOG("gfx_hud: UNPAUSECUT: the game runs again %llu ms after the menu wipe started - replay dropped, no reveal",
                    resumed - g_replayStart);
        }
    }
    const bool rec = g_recThisFrame;
    const bool replay = g_replayOn.load(std::memory_order_relaxed);
    g_recThisFrame = false;
    g_recCleared = false;
    if (!g_wipeLoadTried && ctx) {
        swprintf_s(g_wipeFile, L"%s\\menu_wipe.bin", bvr::log::data_dir());
        load_recording(ctx);
    }
    if (g_replayLocked && now - g_squareSeenMs > 400) g_replayLocked = false; // the square wipe is over
    const bool black = now < g_blackUntil && !replay && !rec; // BLACKFILL
    if ((!rec && !replay && !black) || !ctx || !sc) {
        if (g_recWriting && now - g_recLastMs > 500) { // the recording ended
            // WIPEMAIN: only the main menu -> game wipe is followed by the loading movie - anything else is discarded.
            const uint64_t ms = g_movieStartMs.load(std::memory_order_relaxed);
            const ULONGLONG len = g_recTime[g_recWriting - 1];
            if (ms >= g_recStart && ms <= g_recLastMs + 1500 && g_recWriting >= 8) { // it becomes the replay source
                g_recCount = g_recWriting;
                g_recWriting = 0;
                g_recKeep = true;
                BVR_LOG("gfx_hud: menu wipe recorded (%d frames, %llu ms)", g_recCount, len);
                if (ctx) save_recording(ctx);
            } else if (now - g_recLastMs > 2000 || g_recWriting < 8) {
                BVR_LOG("gfx_hud: wipe recording discarded (%d frames, %llu ms - no loading movie after it)", g_recWriting, len);
                g_recWriting = 0;
            }
        }
        return;
    }
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&bb))) || !bb) return;
    D3D11_TEXTURE2D_DESC bd{};
    bb->GetDesc(&bd);
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    ID3D11RenderTargetView* rtv = nullptr;
    if (dev) { dev->CreateRenderTargetView(bb, nullptr, &rtv); dev->Release(); }
    const float zoom = g_cutScale.load(std::memory_order_relaxed);
    if (rtv && rec) {
        bvr::blit::zoom_premul(ctx, rtv, bd.Width, bd.Height, g_recSrv, zoom);
        // Store a quarter-size copy (one per game frame: the second eye's present repeats the same pieces).
        if (!g_recKeep && (now - g_recLastMs > 12 || !g_recWriting)) { // WIPEMAIN: a held recording is never overwritten
            if (!g_recWriting || now - g_recLastMs > 500) { g_recWriting = 0; g_recStart = now; }
            const UINT w = g_capW, h = g_capH; // WIPEHD: full size
            if (g_recWriting < 160 && make_rec_frame(ctx, g_recWriting, w, h, nullptr)) {
                if (g_recWriting == 0) g_recCount = 0; // a new recording replaces the old one (its textures are reused)
                bvr::blit::alpha_to_r8(ctx, g_recFrameRtv[g_recWriting], w, h, g_recSrv);
                g_recTime[g_recWriting] = now - g_recStart;
                g_recW = w; g_recH = h;
                ++g_recWriting;
            }
        }
        g_recLastMs = now;
    } else if (rtv && replay) {
        disk_sample(now); // LOADHOLD: the game's disk reading through the wipe
        draw_sample();    // MENUARRIVE: draw calls per present through the wipe
        const ULONGLONG t = now - g_replayStart;
        int k = 0;
        while (k + 1 < g_recCount && g_recTime[k + 1] <= t) ++k;
        if (t > g_recTime[g_recCount - 1] + 50 && now >= g_hideAllUntil &&
            !load_hold(now, !g_replayInvert, g_recTime[g_recCount - 1]) && // WIPEMAIN3: last frame held meanwhile
            !reveal_after_black(now)) {
            g_replayOn.store(false);
            BVR_LOG("gfx_hud: wipe replay ended after %llu ms", t);
            g_replayEndMs = now; // MENUFADESKIP
        } else if (g_recFrameSrv[k] && !(g_replayInvert && t > g_recTime[g_recCount - 1])) {
            bvr::blit::mask_black(ctx, rtv, bd.Width, bd.Height, g_recFrameSrv[k], zoom, g_replayInvert);
        }
    } else if (rtv && black) {
        const float a = now - g_blackFillAt < 200 ? 1.0f : g_mfAlpha; // FADEALPHA
        if (a >= 0.99f) {
            const float k[4] = {0, 0, 0, 1};
            ctx->ClearRenderTargetView(rtv, k); // BLACKFILL: the game's full-screen black, over the whole view
        } else {
            if (!g_dimTex) {
                ID3D11Device* d2 = nullptr;
                ctx->GetDevice(&d2);
                if (d2) {
                    D3D11_TEXTURE2D_DESC td{};
                    td.Width = 1; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
                    td.Format = DXGI_FORMAT_R8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
                    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    if (SUCCEEDED(d2->CreateTexture2D(&td, nullptr, &g_dimTex)))
                        d2->CreateShaderResourceView(g_dimTex, nullptr, &g_dimSrv);
                    d2->Release();
                }
            }
            if (g_dimTex && g_dimSrv) {
                const uint8_t v = static_cast<uint8_t>(255.0f * (1.0f - (a < 0.0f ? 0.0f : a)) + 0.5f); // mask = 1 - a
                if (v != g_dimVal) { g_dimVal = v; ctx->UpdateSubresource(g_dimTex, 0, nullptr, &v, 1, 1); }
                bvr::blit::mask_black(ctx, rtv, bd.Width, bd.Height, g_dimSrv, 1.0f, true); // black at opacity a
            }
        }
    }
    if (rtv) rtv->Release();
    bb->Release();
}

// MENUFADE (JJ 2026-10-09 23:55: second menu -> first menu, "the black square" shows "before the screen goes black",
// "fixed in the room", acting "like a kind of a mask"): the save-select screen fades to black on its HUD stage before
// the bat wipe - in 2D a full-screen fade, in VR an opaque black box on the panel (capture 230935: last panel picture
// before the wipe 84 % opaque black, no text; ordinary menu panels are <= 2 % opaque). Outside gameplay the same five
// panel points are read back every frame (2 frames late, never waiting); while all five are dark and covered, the
// panel is not shown and the whole view is black (BLACKFILL's path) - it runs straight into the flipped bat replay.
// Panel switch "menu fades to black over the whole view" (menu_fade_black).
std::atomic<bool> g_menuFadeOn{true};
ID3D11Texture2D* g_mfProbe[3] = {};
bool g_mfProbeUsed[3] = {};
uint32_t g_mfSeq = 0;
bool g_mfCovered = false;

// EXITFADE / EXITPATTERN (JJ 2026-10-10: quitting from the first menu plays "the bats in a window"; "it should be the
// same full screen, full field of view, bats ... that plays when you enter gameplay from the main menu"). Exit HUD
// records 01:47:32 and 02:11:37: after the quit prompt closes the first menu's items fade out, then ~1.1 s of ONLY the
// menu background (1024x256 logo, the 1024x512 BC1 picture) + TWO 4x4 fills (the idle first menu has one; the second
// = the game's fade to black, too faint for the 5-point probes until the last 0.1 s), then the window closes. No bat
// piece is drawn in the HUD. That frame starts the recorded wipe NORMAL (to black); afterwards the whole view is held
// black (panel hidden) while the pattern / the fade lasts, up to 10 s.
ULONGLONG g_exitUntil = 0;
void start_exit_replay(ULONGLONG now, const char* why) {
    if (now < g_exitUntil || g_replayOn.load(std::memory_order_relaxed) || g_replayLocked || g_recCount <= 8 ||
        !g_replayEnabled.load(std::memory_order_relaxed) || g_zonesGameplay.load(std::memory_order_relaxed))
        return;
    g_exitUntil = now + 10000;
    g_replayLocked = true;
    g_squareSeenMs = now;
    g_replayStart = now;
    g_hideAllUntil = now + g_recTime[g_recCount - 1] + 50;
    g_hideAllCap = now + 4000;
    g_replayInvert = false; // to black
    g_replayOn.store(true);
    g_replays.fetch_add(1, std::memory_order_relaxed);
    BVR_LOG("gfx_hud: leaving the game (%s) - full-view bat wipe to black", why);
}
// WIPEREVEAL (JJ 2026-10-11: after quitting to the main menu "bats flying, then it turns black, then it waits, then a menu
// appears. Instead of just appearing, there should be the bat transition to the menu"): a replay that ended on black in
// the menus (not opened from gameplay, not quitting the game) plays again FLIPPED - black bats clearing off the arriving
// screen, as the first menu after the logos does. true = the replay goes on (the reveal started).
namespace { // the declaration above sits in this file's unnamed namespace
bool reveal_after_black(ULONGLONG now) {
    if (g_replayInvert || !g_revealOn.load(std::memory_order_relaxed) || g_wipeFromGameplay || now < g_exitUntil ||
        g_recCount <= 8)
        return false;
    g_replayInvert = true;
    // JJ 04:21: "Batman's already sitting there, so it's a little bit late": the reveal starts g_revealSkipMs into the
    // recording, past its nearly-solid black opening frames.
    g_replayStart = now - ULONGLONG(g_revealSkipMs.load(std::memory_order_relaxed));
    g_hideAllUntil = now + g_recTime[g_recCount - 1] + 50;
    g_hideAllCap = now + 4000;
    BVR_LOG("gfx_hud: WIPEREVEAL: black -> bats clearing off the arriving screen (%llu ms)", g_recTime[g_recCount - 1]);
    wipe_event(1); // WIPESOUND: the game's bat sound with the reveal
    return true;
}

} // namespace
void exit_hold(bool cond) {
    const ULONGLONG now = GetTickCount64();
    if (now >= g_exitUntil || !cond) return;
    g_mfAlpha = 1.0f;
    if (now >= g_blackUntil) g_blackCap = now + 6000;
    const ULONGLONG until = now + 150;
    if (until > g_blackUntil) g_blackUntil = until < g_blackCap ? until : g_blackCap;
    g_squareSeenMs = now; // keeps the replay locked (one per quit)
}
// This frame's HUD (from the per-frame line counts): the quit fade pattern?
bool exit_pattern_now() {
    if (g_lineDraws == 0 || g_lineDraws > 4) return false;
    bool picture = false;
    unsigned fills = 0;
    for (unsigned i = 0; i < g_lineTexN; ++i) {
        const LineTex& t = g_lineTex[i];
        if (t.w == 1024 && t.h == 512 && t.fmt == 71) picture = true;
        if (t.w == 4 && t.h == 4 && t.fmt == 28) fills += t.n;
    }
    return picture && fills == 2;
}

// MENUFADE, render thread at present: probe this frame's panel picture, evaluate the one copied two frames ago.
void menu_fade_tick(ID3D11DeviceContext* ctx) {
    if (!g_menuFadeOn.load(std::memory_order_relaxed) || g_zonesGameplay.load(std::memory_order_relaxed)) {
        g_mfCovered = false;
        return;
    }
    if (!ctx || !g_capTex || !g_frameHud) return;
    const uint32_t slot = g_mfSeq % 3;
    if (!g_mfProbe[slot]) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kFadePts; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc = {1, 0};
        td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        dev->CreateTexture2D(&td, nullptr, &g_mfProbe[slot]);
        dev->Release();
        if (!g_mfProbe[slot]) return;
    }
    for (int i = 0; i < kFadePts; ++i) {
        const UINT x = static_cast<UINT>(kFadePt[i][0] * g_capW), y = static_cast<UINT>(kFadePt[i][1] * g_capH);
        const D3D11_BOX box{x, y, 0, x + 1, y + 1, 1};
        ctx->CopySubresourceRegion(g_mfProbe[slot], 0, i, 0, 0, g_capTex, 0, &box);
    }
    g_mfProbeUsed[slot] = true;
    ++g_mfSeq;
    const uint32_t old = (g_mfSeq + 1) % 3; // copied two frames ago
    if (!g_mfProbeUsed[old] || !g_mfProbe[old]) return;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_mfProbe[old], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) return;
    const uint8_t* p = static_cast<const uint8_t*>(m.pData);
    int covered = 0;
    uint32_t alphaSum = 0, alphaMin = 255;
    for (int i = 0; i < kFadePts; ++i) {
        const uint8_t* c = p + i * 4;
        if (c[3] > 12 && c[0] < 40 && c[1] < 40 && c[2] < 40) ++covered;
        alphaSum += c[3];
        if (c[3] < alphaMin) alphaMin = c[3];
    }
    ctx->Unmap(g_mfProbe[old], 0);
    g_mfProbeUsed[old] = false;
    const ULONGLONG now = GetTickCount64();
    // FADEMIN (JJ 2026-10-10 00:05: with FADEALPHA "the menus flashing off briefly" - the panel was hidden for the
    // faint main-menu fade-in): only a near-opaque fade (all five points >= 200) is taken over; faint ones stay on the
    // panel as the game draws them (as before MENUFADE - not noticed there).
    if (covered == kFadePts && alphaMin >= 200) {
        g_mfAlpha = alphaMin / 255.0f; // FADEALPHA: the view gets the fade's own opacity
        if (!g_mfCovered) {
            g_mfCovered = true;
            if (now >= g_blackUntil) g_blackCap = now + 6000;
            BVR_LOG("gfx_hud: menu fade to black on the panel (alpha ~%u) -> whole view black, panel hidden", alphaSum / kFadePts);
        }
        const ULONGLONG until = now + 120;
        if (until > g_blackUntil) g_blackUntil = until < g_blackCap ? until : g_blackCap;
    } else if (g_mfCovered) {
        g_mfCovered = false;
        BVR_LOG("gfx_hud: menu fade box gone from the panel");
    }
    exit_hold(alphaMin > 12); // EXITFADE: keep the view black while a fade covers the panel during a quit
}

void on_clear_dsv(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, unsigned flags, float depth,
                  unsigned char stencil) {
    if (!g_armed.load(std::memory_order_relaxed) || !ctx || !dsv) return;
    if (dsv == g_hudDsv) return;
    if (!g_pastBoundary) {
        // The scene's own clear value (UE3 may clear depth to 0 or 1) - the HUD's depth starts the same way.
        if (flags & D3D11_CLEAR_DEPTH) { // full-size buffers only (not shadow maps)
            unsigned w = 0, h = 0;
            dsv_dims(dsv, &w, &h);
            if (w >= 1000) g_sceneClearDepth = depth;
        }
        return;
    }
    if (g_hudDsv && g_hudDsvCleared && dsv == g_hudDsvSrc) { // the HUD clears its masks: on its own buffer too
        bvr::frame_inspector::ScopedSuppress quiet;
        ctx->ClearDepthStencilView(g_hudDsv, flags, depth, stencil);
    }
}
void set_hud_dsv(bool on) { g_hudDsvOn.store(on); BVR_LOG("gfx_hud: HUD's own depth-stencil %s", on ? "ON" : "off (the game's)"); }
void set_hide_wipes(bool on) { g_hideWipes.store(on); BVR_LOG("gfx_hud: menu bat wipes %s", on ? "HIDDEN" : "shown (full view)"); }
bool hide_wipes() { return g_hideWipes.load(); }
void set_menu_open_hold(int ms) { g_menuOpenHoldMs.store(ms < 0 ? 0 : ms > 4000 ? 4000 : ms); BVR_LOG("gfx_hud: menu opening from gameplay: hold %d ms after the recorded wipe", g_menuOpenHoldMs.load()); }
void set_open_fade(bool on) { g_openFadeOn.store(on); BVR_LOG("gfx_hud: menu-opening fade pieces %s", on ? "not drawn" : "drawn (test)"); }
void set_wipe_event_hook(WipeEventFn fn) { g_wipeEventFn.store(fn); }
void set_load_hold(bool on) { g_loadHoldOn.store(on); BVR_LOG("gfx_hud: LOADHOLD %s", on ? "on" : "off"); }
void set_wipe_reveal_skip(int ms) { g_revealSkipMs.store(ms < 0 ? 0 : ms > 900 ? 900 : ms); BVR_LOG("gfx_hud: WIPEREVEAL skips %d ms", g_revealSkipMs.load()); }
void set_pre_reveal(int ms) { g_preRevealMs.store(ms < 0 ? 0 : ms > 3000 ? 3000 : ms); BVR_LOG("gfx_hud: PREREVEAL %d ms after loading goes quiet", g_preRevealMs.load()); }
void set_wipe_reveal(bool on) { g_revealOn.store(on); BVR_LOG("gfx_hud: WIPEREVEAL %s", on ? "on" : "off"); }
void set_glow_zone(int mode) {
    g_glowZoneMode.store(mode);
    BVR_LOG("gfx_hud: glowzone mode %d (0 anchor only, 1 + glow pastes, 2 + a piece between two of one group / picture middle with the next piece, 3 + picture "
            "middle alone, 4 + lone pieces take the next piece's group) | pieces moved so far %u", mode, g_picZoneMoved.load());
}
void set_clock_stopped(bool stopped) {
    if (g_clockStopped.exchange(stopped, std::memory_order_relaxed) && !stopped)
        g_resumedMs.store(GetTickCount64(), std::memory_order_relaxed); // UNPAUSECUT: handled at the next present
}
uint32_t unpause_cuts() { return g_unpauseCuts.load(); }
unsigned long long menu_wipe_at() { return g_menuWipeAtMs.load(std::memory_order_relaxed); }
void set_map_screen(bool on) { g_mapScreenOn.store(on); BVR_LOG("gfx_hud: map screen exception %s", on ? "ON" : "off (test)"); }
void set_stage_box(bool on) { g_stageBox.store(on); BVR_LOG("gfx_hud: stage box / first cut / fade skip %s", on ? "ON" : "off"); }
void set_menu_cut(bool on) { g_menuCut.store(on); BVR_LOG("gfx_hud: menu cut pieces %s", on ? "ALL in the eye (outside gameplay)" : "as before"); }
void set_halo(float strength) { g_halo.store(strength, std::memory_order_relaxed); }

void set_fade_hold(int mode) {
    g_fadeHoldMode.store(mode);
    BVR_LOG("gfx_hud: fade-in box after loading %s", mode ? "HIDDEN (panel held while it covers the panel)" : "shown (old)");
}
int fade_hold() { return g_fadeHoldMode.load(); }
uint32_t replays_started() { return g_replays.load(std::memory_order_relaxed); }
bool wipe_panel_off() { return g_wipePanelOff.load(); }
bool black_fill() { return g_blackFillOn.load(); }
bool wipe_debug() { return g_wipeDebug.load(); }
void set_wipe_debug(bool on) { g_wipeDebug.store(on); BVR_LOG("gfx_hud: test captures during bat wipes %s", on ? "ON (stutters)" : "off"); }
bool menu_fade_black() { return g_menuFadeOn.load(); }
void set_menu_fade_black(bool on) { g_menuFadeOn.store(on); BVR_LOG("gfx_hud: menu fades to black %s", on ? "over the whole view" : "on the panel (old)"); }
void set_black_fill(bool on) { g_blackFillOn.store(on); BVR_LOG("gfx_hud: the game's full-screen black %s", on ? "covers the whole view" : "hidden (old)"); }
bool wipe_flip() { return g_wipeFlipOn.load(); }
bool replay_enabled() { return g_replayEnabled.load(); }
void set_replay_enabled(bool on) { g_replayEnabled.store(on); BVR_LOG("gfx_hud: square wipes %s", on ? "REPLACED by the recorded bat wipe" : "only hidden (no replay)"); }
void set_wipe_panel_off(bool on) { g_wipePanelOff.store(on); BVR_LOG("gfx_hud: HUD panel during wipe replays %s", on ? "NOT SENT" : "sent (old)"); }
void set_wipe_flip(bool on) { g_wipeFlipOn.store(on); BVR_LOG("gfx_hud: wipes into the first menu %s", on ? "FLIPPED (black bats over the menu)" : "as recorded"); }
void set_replay_hides_all(bool on) {
    g_replayHidesAll.store(on);
    BVR_LOG("gfx_hud: during the wipe replay %s", on ? "NO HUD piece is drawn" : "only wipe pieces are hidden (old)");
}
void note_movie_started() { g_movieStartMs.store(GetTickCount64(), std::memory_order_relaxed); }
void set_main_menu_now(bool on) { if (on) g_mainMenuAt.store(GetTickCount64(), std::memory_order_relaxed); }
void note_movie_ended() {
    g_movieEndMs.store(GetTickCount64(), std::memory_order_relaxed); // WIPEMAIN: no replay just after a loading movie
    g_movieEnds.fetch_add(1, std::memory_order_relaxed);
    if (!g_fadeHoldMode.load()) return;
    g_fadeStart = GetTickCount64();
    g_fadeSkipUntil.store(g_fadeStart + 3000); // FADESKIP
    g_fadeSeen = false;
    g_fadeClear = 0;
    g_fadeUntil.store(g_fadeStart + 3000);
    g_fadeHold.store(true);
}
void request_panel_sequence(const wchar_t* dir, int frames) {
    CreateDirectoryW(dir, nullptr);
    wcsncpy_s(g_panelSeqDir, dir, _TRUNCATE);
    g_panelSeqLeft.store(frames);
    BVR_LOG("gfx_hud: panel pictures for the next %d HUD frames", frames);
}
bool cuts_to_eye() { return g_cutsToEye.load(std::memory_order_relaxed); }
uint32_t cuts_kept() { return g_cutsKept.load(std::memory_order_relaxed); }

void set_marker_fn(MarkerFn fn, MarkerAfterFn after) {
    g_markerAfterFn.store(after, std::memory_order_relaxed);
    g_markerFn.store(fn, std::memory_order_relaxed);
}
void after_draw(ID3D11DeviceContext* ctx) {
    if (g_cutBuf) {
        {
            bvr::frame_inspector::ScopedSuppress quiet;
            ctx->UpdateSubresource(g_cutBuf, 0, nullptr, g_cutOrig, 0, 0);
        }
        g_cutBuf->Release();
        g_cutBuf = nullptr;
        return;
    }
    if (MarkerAfterFn f = g_markerAfterFn.load(std::memory_order_relaxed)) f(ctx);
}

void set_hud_ring(bool on) {
    g_ringLogOnMovie.store(on);
    BVR_LOG("gfx_hud: HUD ring %s", on ? "also written to the log when a full-screen movie starts" : "kept for captures only");
}
bool save_hud_ring(const wchar_t* path) {
    FILE* f = _wfopen(path, L"w");
    if (!f) return false;
    // Render-thread data read from the game thread: a torn line is possible and harmless (diagnostics only).
    fprintf(f, "Last %d HUD frames (per frame: draws, largest placement | per texture: WxH/format xCount sLargest "
               "eKeptInTheEyeImage)\n", g_ringCount);
    const int start = (g_ringHead - g_ringCount + kRing) % kRing;
    for (int i = 0; i < g_ringCount; ++i) fprintf(f, "%s\n", g_ring[(start + i) % kRing]);
    fclose(f);
    return true;
}
void dump_hud_ring(const char* why) {
    if (!g_ringLogOnMovie.load() || !g_ringCount) return;
    // Render-thread data read from the game thread: a torn line is possible and harmless (test log only).
    BVR_LOG("[hudring] last %d HUD frames before: %s", g_ringCount, why);
    const int start = (g_ringHead - g_ringCount + kRing) % kRing;
    for (int i = 0; i < g_ringCount; ++i) BVR_LOG("[hudring] %s", g_ring[(start + i) % kRing]);
}

void request_timeline(int frames) {
    bvr::frame_inspector::set_cb_shadow(true);
    g_lineLeft.store(frames);
    BVR_LOG("[hudline] next %d frames:", frames);
}

void request_dump() {
    bvr::frame_inspector::set_cb_shadow(true);
    g_dumpArm.store(true);
    BVR_LOG("[huddump] next frame's HUD draws:");
}
void save_panel_picture(const wchar_t* path) {
    wcsncpy_s(g_savePath, path, _TRUNCATE);
    g_saveReq.store(true);
}

void set_armed(bool on) {
    bool was = g_armed.exchange(on, std::memory_order_relaxed);
    if (was != on) BVR_LOG("gfx_hud: %s", on ? "ARMED (positional classifier live)" : "off");
}

void set_redirect(bool on) {
    bool was = g_redirect.exchange(on, std::memory_order_relaxed);
    if (was != on)
        BVR_LOG("gfx_hud: redirect %s", on ? "ON (UI run -> capture RT / quad)" : "off");
}

bool armed() { return g_armed.load(std::memory_order_relaxed); }
bool redirect_on() { return g_redirect.load(std::memory_order_relaxed); }

void set_fx_passthrough(bool on) {
    bool was = g_fxPassthrough.exchange(on, std::memory_order_relaxed);
    if (was != on)
        BVR_LOG("gfx_hud: fx passthrough %s (small-count full-screen draws %s)",
                on ? "ON" : "off", on ? "stay on the eye image" : "go to the quad");
}
bool fx_passthrough() { return g_fxPassthrough.load(std::memory_order_relaxed); }

void on_setrt(unsigned numViews, ID3D11RenderTargetView* const* rtvs,
              ID3D11DepthStencilView* dsv) {
    if (!g_armed.load(std::memory_order_relaxed)) return;
    g_curDsv = dsv;
    // DLSS: remember the scene depth (full size, R24G8, readable) - checked only when the view changes.
    static ID3D11DepthStencilView* s_lastChecked = nullptr;
    if (dsv && dsv != s_lastChecked) {
        s_lastChecked = dsv;
        ID3D11Resource* r = nullptr;
        dsv->GetResource(&r);
        ID3D11Texture2D* t = nullptr;
        if (r && SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&t))) && t) {
            D3D11_TEXTURE2D_DESC d{};
            t->GetDesc(&d);
            if (d.Format == DXGI_FORMAT_R24G8_TYPELESS && (d.BindFlags & D3D11_BIND_SHADER_RESOURCE) && d.Width >= 1000 &&
                d.SampleDesc.Count == 1)
                g_sceneDepth = r; // weak
            t->Release();
        }
        if (r) r->Release();
    }
    ID3D11RenderTargetView* rtv0 = (numViews && rtvs) ? rtvs[0] : nullptr;
    g_gameRtv = rtv0;
    g_ourBound = false; // the game rebinding always clears our substitution
    g_onBackbuffer = false;
    if (!rtv0 || !g_bbUnk) return;
    ID3D11Resource* res = nullptr;
    rtv0->GetResource(&res);
    if (res) {
        g_onBackbuffer = (canonical(res) == g_bbUnk);
        res->Release();
    }
}

Decision on_draw_indexed(ID3D11DeviceContext* ctx, unsigned indexCount) {
    return classify(ctx, indexCount, /*indexed=*/true);
}

Decision on_draw(ID3D11DeviceContext* ctx, unsigned vertexCount) {
    return classify(ctx, vertexCount, /*indexed=*/false);
}

void on_present(ID3D11DeviceContext* ctx, IDXGISwapChain* swapchain) {
    if (!g_armed.load(std::memory_order_relaxed)) return;
    if (g_frameHud && g_dumpLeft.load(std::memory_order_relaxed) > 0 && g_dumpLeft.fetch_sub(1) == 1)
        BVR_LOG("[huddump] end (%u draws)", g_dumpIndex);
    if (g_dumpArm.exchange(false)) { g_dumpIndex = 0; g_dumpLeft.store(1); } // whole frames only
    if (g_frameOther && !g_wipeFrame && !g_wipePrev) g_menuScale = g_frameMaxScale; // HIDEWIPES: where a wipe starts
    g_frameMaxScale = 0.0f;
    if (g_frameCut || g_frameOther) {
        g_prevPureCut = g_frameCut > 0 && g_frameOther == 0;
        g_wipePrev = g_wipeFrame; // WIPEALL: frames with HUD draws only (a frame without UI keeps the state)
    }
    g_wipeFrame = false;
    g_frameCut = g_frameOther = 0;
    const bool lineNow = g_lineLeft.load(std::memory_order_relaxed) > 0;
    if (lineNow || g_ringOn.load(std::memory_order_relaxed)) {
        char buf[900];
        int n = snprintf(buf, sizeof(buf), "[hudline] t%llu p%u draws %u maxscale %.3g |", GetTickCount64() % 1000000ULL,
                         g_presents, g_lineDraws, g_lineMaxScale);
        for (unsigned i = 0; i < g_lineTexN && n < int(sizeof(buf)) - 48; ++i)
            n += snprintf(buf + n, sizeof(buf) - n, g_lineTex[i].hid ? " %ux%u/%d x%u s%.2g e%u h%u" : " %ux%u/%d x%u s%.2g e%u",
                          g_lineTex[i].w, g_lineTex[i].h, g_lineTex[i].fmt, g_lineTex[i].n, g_lineTex[i].sc,
                          g_lineTex[i].eye, g_lineTex[i].hid);
        if (lineNow) {
            BVR_LOG("%s", buf);
            g_lineLeft.fetch_sub(1);
        }
        if (g_ringOn.load(std::memory_order_relaxed) && g_lineDraws) {
            strncpy_s(g_ring[g_ringHead], buf, _TRUNCATE);
            g_ringHead = (g_ringHead + 1) % kRing;
            if (g_ringCount < kRing) ++g_ringCount;
        }
        // EXITPATTERN: the quit fade frame starts the full-view wipe to black and keeps the view black.
        if (g_ringOn.load(std::memory_order_relaxed) && exit_pattern_now()) {
            start_exit_replay(GetTickCount64(), "quit fade on the first menu");
            exit_hold(true);
        }
        g_lineTexN = 0; g_lineDraws = 0; g_lineMaxScale = 0.0f;
    }
    if (ctx && g_frameHud && g_saveReq.exchange(false)) save_texture_bmp(ctx, g_capTex, g_savePath);
    if (ctx && g_frameHud && g_capTex && g_panelSeqLeft.load(std::memory_order_relaxed) > 0) {
        g_panelSeqLeft.fetch_sub(1);
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\p%05u_%s.bmp", g_panelSeqDir, g_presents,
                   g_fadeHold.load() ? L"held" : L"shown");
        save_texture_bmp(ctx, g_capTex, path);
    }
    fade_tick(ctx);
    menu_fade_tick(ctx);
    halo_tick(ctx);
    ++g_presents;
    g_cFrames.fetch_add(1, std::memory_order_relaxed);
    if (g_frameHud) g_lastHudFrame = g_presents;
    g_cLastFrameHud.store(g_frameHud, std::memory_order_relaxed);
    g_frameHud = 0;
    g_pastBoundary = false;
    g_clearedThisFrame = false;
    g_hudDsvCleared = false; // HUDDSV
    paste_frame_end(); // GLOWZONE
    // Refresh the backbuffer identity latch (weak - no ref held across
    // frames; a resize simply re-latches).
    g_bbUnk = nullptr;
    if (swapchain) {
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&bb))) && bb) {
            D3D11_TEXTURE2D_DESC td{};
            bb->GetDesc(&td);
            g_bbUnk = canonical(bb);
            g_bbW = td.Width;
            g_bbH = td.Height;
            bb->Release();
        }
    }
}

// SVRGLOW (JJ 2026-10-10: in SteamVR "the top compass is not there, all I can see is the markers for north, south,
// east and west"): additive HUD pieces (the compass bar's glow) are kept additive on the panel - premultiplied colour
// with alpha 0 (fixed_blend, 2026-10-03). Virtual Desktop adds that colour; SteamVR drops it, leaving only the
// "over"-drawn letters. Under SteamVR the panel picture is copied with alpha = max(alpha, brightness) first.
ID3D11Texture2D* g_svrTex = nullptr;
ID3D11RenderTargetView* g_svrRtv = nullptr;
ID3D11ShaderResourceView* g_svrSrcSrv[2] = {}; // capture, halo
ID3D11Texture2D* g_svrSrcOf[2] = {};            // the texture each view was made for (rebuilt if it changes)
ID3D11Texture2D* svr_repaired(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, int which) {
    if (!ctx || !src || !g_capW || !g_capH) return src;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return src;
    if (!g_svrTex) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = g_capW; td.Height = g_capH; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g_svrTex)))
            dev->CreateRenderTargetView(g_svrTex, nullptr, &g_svrRtv);
        BVR_LOG("gfx_hud: SteamVR panel copy with glow alpha %s", g_svrRtv ? "ready" : "FAILED");
    }
    if (g_svrSrcSrv[which] && g_svrSrcOf[which] != src) { g_svrSrcSrv[which]->Release(); g_svrSrcSrv[which] = nullptr; }
    if (!g_svrSrcSrv[which]) { dev->CreateShaderResourceView(src, nullptr, &g_svrSrcSrv[which]); g_svrSrcOf[which] = src; }
    dev->Release();
    if (!g_svrRtv || !g_svrSrcSrv[which]) return src;
    return bvr::blit::alpha_repair_full(ctx, g_svrRtv, g_svrSrcSrv[which], g_capW, g_capH) ? g_svrTex : src;
}

ID3D11Texture2D* texture(ID3D11DeviceContext* ctx) {
    if (!g_redirect.load(std::memory_order_relaxed) || !g_capTex) return nullptr;
    // Fresh = HUD content landed within the last few presents (menus and
    // pauses keep drawing UI; movies/loads go stale and the quad drops).
    if (g_presents - g_lastHudFrame > 8) return nullptr;
    if (g_fadeHold.load(std::memory_order_relaxed)) return nullptr; // FADEHOLD: the fade-in box covers the panel
    if (g_wipePanelOff.load(std::memory_order_relaxed) &&
        (g_replayOn.load(std::memory_order_relaxed) || GetTickCount64() < g_hideAllUntil))
        return nullptr; // WIPEPANEL: no panel layer during a wipe replay
    if (GetTickCount64() < g_blackUntil) return nullptr; // BLACKFILL / MENUFADE: the whole view is black
    ID3D11Texture2D* out = g_haloValid ? g_haloTex : g_capTex; // PAUSEHALO
    if (bvr::vr::runtime_is_steamvr()) out = svr_repaired(ctx, out, g_haloValid ? 1 : 0); // SVRGLOW
    return out;
}

Census census() {
    Census c;
    c.frames = g_cFrames.load(std::memory_order_relaxed);
    c.boundaries = g_cBoundaries.load(std::memory_order_relaxed);
    c.hudDraws = g_cHudDraws.load(std::memory_order_relaxed);
    c.redirected = g_cRedirected.load(std::memory_order_relaxed);
    c.bigPostDraws = g_cBigPost.load(std::memory_order_relaxed);
    c.lastFrameHud = g_cLastFrameHud.load(std::memory_order_relaxed);
    return c;
}

} // namespace bvr::gfx_hud
