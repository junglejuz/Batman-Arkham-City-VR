#include "game/arkhamcity/room_overlay.h"
#include "core/ui/overlay.h"

#include "core/gfx/blit.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"

#include <d3d11.h>
#include <imgui.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>

namespace bvr::ac::room_overlay {
namespace {

// --- settings (saved) ----------------------------------------------------------------------
// JJ 2026-10-07 (later): "better without those grime maps on the menu screen and the pause screen" -
// both removed by default; the room versions stay as options.
std::atomic<int> g_mode{kHidden};
// PAUSEDIRT (JJ 2026-10-07: the pause menu's dirt "should be further away from your face and attached in
// world space"): true = the dirt hangs on the same room screen as the title grunge.
std::atomic<bool> g_dirtRoom{false};
std::atomic<uint32_t> g_dirtRemoved{0};
std::atomic<uint32_t> g_dirtDrawn{0}, g_dirtFailed{0};
// The dirt's angular width on its sphere: wide enough that its dark paper border stays out of view
// looking ahead and on ordinary head turns.
std::atomic<float> g_dirtSpanDeg{150.0f};
std::atomic<float> g_distM{3.0f};  // straight ahead of the recentred head
std::atomic<float> g_widthM{9.0f}; // ~112 deg wide at 3 m: edges just outside the view looking ahead
std::atomic<float> g_upM{0.0f};    // centre height above the recentred head

// --- per-pass matrices (game thread writes, render thread reads) ----------------------------
// 64 slots like the eye labels: the game is at most a few passes ahead of the render thread.
constexpr uint32_t kSlots = 64;
struct Pass {
    std::atomic<uint32_t> seq{0}; // slot + 1 once m is complete for that slot
    float m[16];
    float uu;
};
Pass g_pass[kSlots];
std::atomic<uint32_t> g_renderSlot{0};

// --- counters --------------------------------------------------------------------------------
std::atomic<uint32_t> g_drawn{0}, g_noPass{0}, g_failed{0}, g_hidden{0};

// --- the two replay targets (render thread only) ----------------------------------------------
ID3D11Texture2D* g_tex[2] = {};
ID3D11RenderTargetView* g_rtv[2] = {};
ID3D11ShaderResourceView* g_srv[2] = {};
UINT g_w = 0, g_h = 0;
DXGI_FORMAT g_texFmt = DXGI_FORMAT_UNKNOWN, g_viewFmt = DXGI_FORMAT_UNKNOWN;
bool g_loggedFirst = false;

void release_targets() {
    for (int i = 0; i < 2; ++i) {
        if (g_srv[i]) { g_srv[i]->Release(); g_srv[i] = nullptr; }
        if (g_rtv[i]) { g_rtv[i]->Release(); g_rtv[i] = nullptr; }
        if (g_tex[i]) { g_tex[i]->Release(); g_tex[i] = nullptr; }
    }
    g_w = g_h = 0;
}

bool ensure_targets(ID3D11DeviceContext* ctx, const D3D11_TEXTURE2D_DESC& td, DXGI_FORMAT viewFmt) {
    if (g_tex[0] && g_w == td.Width && g_h == td.Height && g_texFmt == td.Format && g_viewFmt == viewFmt)
        return true;
    release_targets();
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = td.Width;
    d.Height = td.Height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = td.Format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    bool ok = true;
    for (int i = 0; i < 2 && ok; ++i) {
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = viewFmt;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = viewFmt;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &g_tex[i])) &&
             SUCCEEDED(dev->CreateRenderTargetView(g_tex[i], &rd, &g_rtv[i])) &&
             SUCCEEDED(dev->CreateShaderResourceView(g_tex[i], &sd, &g_srv[i]));
    }
    dev->Release();
    if (!ok) {
        release_targets();
        BVR_LOG("[bc] room overlay: replay targets %ux%u fmt %d FAILED", td.Width, td.Height, int(viewFmt));
        return false;
    }
    g_w = td.Width;
    g_h = td.Height;
    g_texFmt = td.Format;
    g_viewFmt = viewFmt;
    BVR_LOG("[bc] room overlay: replay targets %ux%u fmt %d", g_w, g_h, int(viewFmt));
    return true;
}

// First grunge draw: what the game's draw looks like, to check the "linear in the destination"
// assumption (a pixel shader that reads the scene itself would break it).
void log_first(ID3D11DeviceContext* ctx, const D3D11_VIEWPORT& vp, const D3D11_TEXTURE2D_DESC& td) {
    ID3D11BlendState* bs = nullptr;
    FLOAT bf[4];
    UINT mask = 0;
    ctx->OMGetBlendState(&bs, bf, &mask);
    D3D11_BLEND_DESC d{};
    if (bs) { bs->GetDesc(&d); bs->Release(); }
    const D3D11_RENDER_TARGET_BLEND_DESC& rt = d.RenderTarget[0];
    ID3D11ShaderResourceView* srvs[8] = {};
    ctx->PSGetShaderResources(0, 8, srvs);
    for (int i = 0; i < 8; ++i) {
        if (!srvs[i]) continue;
        ID3D11Resource* r = nullptr;
        srvs[i]->GetResource(&r);
        ID3D11Texture2D* t = nullptr;
        if (r && SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&t))) && t) {
            D3D11_TEXTURE2D_DESC s{};
            t->GetDesc(&s);
            BVR_LOG("[bc] room overlay: game draw reads t%d = %ux%u fmt %d bind 0x%X", i, s.Width, s.Height,
                    int(s.Format), s.BindFlags);
            t->Release();
        }
        if (r) r->Release();
        srvs[i]->Release();
    }
    BVR_LOG("[bc] room overlay: game draw into %ux%u fmt %d, viewport %.0f,%.0f %.0fx%.0f | blend %s src %d dst %d "
            "op %d | alpha src %d dst %d | write 0x%X",
            td.Width, td.Height, int(td.Format), vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height,
            bs ? (rt.BlendEnable ? "on" : "off") : "none", rt.SrcBlend, rt.DestBlend, rt.BlendOp, rt.SrcBlendAlpha,
            rt.DestBlendAlpha, rt.RenderTargetWriteMask);
}

// --- probe (2026-10-07, pause-menu dirt): full-screen Draw(3)s into the HDR scene colour -------
std::atomic<int> g_replayLeft{0};
wchar_t g_replayDir[260] = {};
int g_replayIndex = 0;

float half_to_float(uint16_t h) {
    const uint32_t sg = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = ldexpf(float(m), -24);
    else if (e == 31) v = 65504.0f;
    else v = ldexpf(float(m | 1024), int(e) - 25);
    return sg ? -v : v;
}

// fp16 RGBA texture -> quarter-size 24-bit BMP (gamma 2.2, clamped). Render thread; stalls on Map.
void save_half_bmp(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const wchar_t* path) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &st))) {
        ctx->CopyResource(st, tex);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            const int w = int(d.Width) / 4, h = int(d.Height) / 4, row = (w * 3 + 3) & ~3;
            if (FILE* f = _wfopen(path, L"wb")) {
                const uint32_t size = 54 + row * h;
                uint8_t hdr[54] = {'B', 'M'};
                memcpy(hdr + 2, &size, 4);
                const uint32_t off = 54, ih = 40, planes = 1 | (24 << 16);
                memcpy(hdr + 10, &off, 4);
                memcpy(hdr + 14, &ih, 4);
                memcpy(hdr + 18, &w, 4);
                memcpy(hdr + 22, &h, 4);
                memcpy(hdr + 26, &planes, 4);
                fwrite(hdr, 1, 54, f);
                uint8_t* line = static_cast<uint8_t*>(calloc(row, 1));
                for (int y = h - 1; y >= 0; --y) {
                    const uint16_t* src = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(m.pData) +
                                                                            size_t(y) * 4 * m.RowPitch);
                    for (int x = 0; x < w; ++x)
                        for (int c = 0; c < 3; ++c) {
                            float v = half_to_float(src[x * 4 * 4 + c]);
                            v = v < 0 ? 0 : (v > 1 ? 1 : v);
                            line[x * 3 + 2 - c] = uint8_t(powf(v, 1.0f / 2.2f) * 255.0f + 0.5f);
                        }
                    fwrite(line, 1, row, f);
                }
                free(line);
                fclose(f);
            }
            ctx->Unmap(st, 0);
        }
        st->Release();
    }
    dev->Release();
}

} // namespace

namespace {
// The dirt as seen from this eye: a quarter-area sRGB target the game's dirt pass samples instead.
ID3D11Texture2D* g_warpTex = nullptr;
ID3D11RenderTargetView* g_warpRtv = nullptr;
ID3D11ShaderResourceView* g_warpSrv = nullptr;
UINT g_warpW = 0, g_warpH = 0;

bool ensure_warp(ID3D11DeviceContext* ctx, UINT w, UINT h) {
    if (g_warpTex && g_warpW == w && g_warpH == h) return true;
    if (g_warpSrv) { g_warpSrv->Release(); g_warpSrv = nullptr; }
    if (g_warpRtv) { g_warpRtv->Release(); g_warpRtv = nullptr; }
    if (g_warpTex) { g_warpTex->Release(); g_warpTex = nullptr; }
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    const bool ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &g_warpTex)) &&
                    SUCCEEDED(dev->CreateRenderTargetView(g_warpTex, nullptr, &g_warpRtv)) &&
                    SUCCEEDED(dev->CreateShaderResourceView(g_warpTex, nullptr, &g_warpSrv));
    dev->Release();
    if (!ok) return false;
    g_warpW = w;
    g_warpH = h;
    BVR_LOG("[bc] pause dirt: room-screen picture %ux%u", w, h);
    return true;
}

bool tex_desc(ID3D11ShaderResourceView* v, D3D11_TEXTURE2D_DESC* out) {
    if (!v) return false;
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    ID3D11Texture2D* t = nullptr;
    const bool ok = r && SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&t))) && t;
    if (ok) { t->GetDesc(out); t->Release(); }
    if (r) r->Release();
    return ok;
}

bool invert4(const float m[16], float inv[16]) {
    float a[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.0f : 0.0f);
    for (int col = 0; col < 4; ++col) {
        int piv = col;
        for (int r = col + 1; r < 4; ++r)
            if (fabsf(a[r][col]) > fabsf(a[piv][col])) piv = r;
        if (fabsf(a[piv][col]) < 1e-12f) return false;
        if (piv != col)
            for (int j = 0; j < 8; ++j) { const float t = a[col][j]; a[col][j] = a[piv][j]; a[piv][j] = t; }
        const float d = a[col][col];
        for (int j = 0; j < 8; ++j) a[col][j] /= d;
        for (int r = 0; r < 4; ++r) {
            if (r == col) continue;
            const float f = a[r][col];
            if (f != 0.0f)
                for (int j = 0; j < 8; ++j) a[r][j] -= f * a[col][j];
        }
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) inv[i * 4 + j] = a[i][4 + j];
    return true;
}
} // namespace

// A plain white texture: the pause pass multiplies its greyed scene by the dirt, so white = no grime.
ID3D11ShaderResourceView* g_whiteSrv = nullptr;
bool ensure_white(ID3D11DeviceContext* ctx) {
    if (g_whiteSrv) return true;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    uint32_t px[16];
    for (auto& v : px) v = 0xFFFFFFFFu;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = d.Height = 4;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{px, 16, 0};
    ID3D11Texture2D* t = nullptr;
    const bool ok = SUCCEEDED(dev->CreateTexture2D(&d, &init, &t)) &&
                    SUCCEEDED(dev->CreateShaderResourceView(t, nullptr, &g_whiteSrv));
    if (t) t->Release();
    dev->Release();
    return ok;
}

std::atomic<uint64_t> g_pausePassAt{0}; // when the pause / map screen's grey pass last ran

bool on_pause_dirt(ID3D11DeviceContext* ctx) {
    // The pause pass (LIVE 2026-10-07): t0 4x4, t1 = the scene colour copy (fp16, screen size), t2 = the
    // 512x512 dirt, alpha blended over the scene.
    ID3D11ShaderResourceView* srvs[3] = {};
    ctx->PSGetShaderResources(0, 3, srvs);
    D3D11_TEXTURE2D_DESC t0{}, t1{}, t2{};
    const bool match = tex_desc(srvs[0], &t0) && tex_desc(srvs[1], &t1) && tex_desc(srvs[2], &t2) &&
                       t0.Width == 4 && t0.Height == 4 && t1.Width >= 1000 && t1.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
                       t2.Width >= 512 && t2.Width == t2.Height; // GRUNGEPACK: any square dirt >= 512 (texture packs enlarge it)
    static UINT s_loggedW = 0;
    if (match && t2.Width != s_loggedW) { s_loggedW = t2.Width; BVR_LOG("[bc] pause grime picture %ux%u format %d", t2.Width, t2.Height, int(t2.Format)); }
    bool handled = false;
    if (match) bvr::vr::note_hud_menu(); // HUDMENU: the pause / map screen is up - the HUD panel takes its own size
    if (match) g_pausePassAt.store(GetTickCount64(), std::memory_order_relaxed); // PAUSEHALO: at once, no clock wait
    if (match && !g_dirtRoom.load(std::memory_order_relaxed)) {
        // Grime removed (default): the game's own pass, with white in place of the dirt.
        if (ensure_white(ctx)) {
            ctx->PSSetShaderResources(2, 1, &g_whiteSrv);
            ctx->Draw(3, 0);
            ctx->PSSetShaderResources(2, 1, &srvs[2]);
            handled = true;
            g_dirtRemoved.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (match) {
        const uint32_t slot = g_renderSlot.load(std::memory_order_relaxed);
        const Pass& p = g_pass[slot % kSlots];
        float inv[16];
        D3D11_VIEWPORT vp{};
        UINT nvp = 1;
        ctx->RSGetViewports(&nvp, &vp);
        if (p.seq.load(std::memory_order_acquire) == slot + 1 && invert4(p.m, inv) && nvp && vp.Width > 0 &&
            ensure_warp(ctx, static_cast<UINT>(vp.Width) / 2, static_cast<UINT>(vp.Height) / 2)) {
            const float span = g_dirtSpanDeg.load() * 3.14159265f / 180.0f;
            const float screen[4] = {g_distM.load() * p.uu, span, span * vp.Height / vp.Width, g_upM.load() * p.uu};
            if (bvr::blit::plane_warp(ctx, g_warpRtv, g_warpW, g_warpH, srvs[2], inv, screen)) {
                ctx->PSSetShaderResources(2, 1, &g_warpSrv);
                ctx->Draw(3, 0);
                ctx->PSSetShaderResources(2, 1, &srvs[2]);
                handled = true;
            }
        }
        (handled ? g_dirtDrawn : g_dirtFailed).fetch_add(1, std::memory_order_relaxed);
    }
    for (auto* v : srvs)
        if (v) v->Release();
    return handled;
}

void request_replay_pictures(const wchar_t* dir) {
    wcsncpy_s(g_replayDir, dir, _TRUNCATE);
    g_replayIndex = 0;
    g_replayLeft.store(8);
}

void probe_draw(ID3D11DeviceContext* ctx) {
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    if (rtvs[0]) {
        ID3D11Resource* r = nullptr;
        rtvs[0]->GetResource(&r);
        ID3D11Texture2D* t = nullptr;
        if (r) r->QueryInterface(IID_PPV_ARGS(&t));
        D3D11_TEXTURE2D_DESC td{};
        if (t) t->GetDesc(&td);
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rtvs[0]->GetDesc(&rd);
        const bool sceneHdr = t && td.Width >= 1000 && td.SampleDesc.Count == 1 &&
                              rd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
                              rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D;
        if (sceneHdr) {
            // Every texture slot, logged once per distinct set.
            ID3D11ShaderResourceView* srvs[8] = {};
            ctx->PSGetShaderResources(0, 8, srvs);
            char text[512] = {};
            int n = 0;
            uint64_t key = 1469598103934665603ull;
            for (int i = 0; i < 8; ++i) {
                D3D11_TEXTURE2D_DESC sd{};
                if (srvs[i]) {
                    ID3D11Resource* sr = nullptr;
                    srvs[i]->GetResource(&sr);
                    ID3D11Texture2D* stex = nullptr;
                    if (sr && SUCCEEDED(sr->QueryInterface(IID_PPV_ARGS(&stex))) && stex) {
                        stex->GetDesc(&sd);
                        stex->Release();
                    }
                    if (sr) sr->Release();
                    srvs[i]->Release();
                    if (n < int(sizeof(text)) - 40)
                        n += snprintf(text + n, sizeof(text) - n, " t%d %ux%u/%d", i, sd.Width, sd.Height, int(sd.Format));
                }
                key = (key ^ (uint64_t(sd.Width) << 32 | uint64_t(sd.Height) << 8 | uint64_t(sd.Format))) *
                      1099511628211ull;
            }
            static uint64_t s_seen[64];
            static unsigned s_n = 0;
            bool seen = false;
            for (unsigned i = 0; i < s_n; ++i) seen |= s_seen[i] == key;
            if (!seen && s_n < 64) {
                s_seen[s_n++] = key;
                ID3D11BlendState* bs = nullptr;
                FLOAT bf[4];
                UINT mask = 0;
                ctx->OMGetBlendState(&bs, bf, &mask);
                D3D11_BLEND_DESC bd{};
                if (bs) { bs->GetDesc(&bd); bs->Release(); }
                const D3D11_RENDER_TARGET_BLEND_DESC& b = bd.RenderTarget[0];
                BVR_LOG("[bc] scene Draw(3) probe: into %ux%u |%s | blend %s src %d dst %d op %d write 0x%X", td.Width,
                        td.Height, text, bs ? (b.BlendEnable ? "on" : "off") : "none", b.SrcBlend, b.DestBlend,
                        b.BlendOp, b.RenderTargetWriteMask);
            }
            // A capture asked for pictures: replay this draw onto black and white and save both.
            if (g_replayLeft.load() > 0 && ensure_targets(ctx, td, rd.Format)) {
                g_replayLeft.fetch_sub(1);
                static const FLOAT kBlack[4] = {0, 0, 0, 0}, kWhite[4] = {1, 1, 1, 1};
                for (int i = 0; i < 2; ++i) {
                    ctx->OMSetRenderTargets(1, &g_rtv[i], dsv);
                    ctx->ClearRenderTargetView(g_rtv[i], i ? kWhite : kBlack);
                    ctx->Draw(3, 0);
                }
                ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
                wchar_t path[300];
                for (int i = 0; i < 2; ++i) {
                    swprintf_s(path, L"%s\\replay%d_%s.bmp", g_replayDir, g_replayIndex, i ? L"white" : L"black");
                    save_half_bmp(ctx, g_tex[i], path);
                }
                BVR_LOG("[bc] scene Draw(3) probe: replay %d saved (%s )", g_replayIndex, text);
                ++g_replayIndex;
            }
        }
        if (t) t->Release();
        if (r) r->Release();
    }
    for (auto* v : rtvs)
        if (v) v->Release();
    if (dsv) dsv->Release();
}

void set_pass(uint32_t slot, const float baseToClip[16], float uuPerM) {
    Pass& p = g_pass[slot % kSlots];
    p.seq.store(0, std::memory_order_relaxed);
    memcpy(p.m, baseToClip, sizeof(p.m));
    p.uu = uuPerM;
    p.seq.store(slot + 1, std::memory_order_release);
}

void set_render_slot(uint32_t slot) { g_renderSlot.store(slot, std::memory_order_relaxed); }

std::atomic<uint64_t> g_titleAt{0}; // when the title screen's grunge was last drawn (MENUCLOCK: main menu)
bool on_grunge_draw(ID3D11DeviceContext* ctx) {
    g_titleAt.store(GetTickCount64(), std::memory_order_relaxed);
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (mode == kOriginal) return false;
    if (mode == kHidden) {
        g_hidden.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    const uint32_t slot = g_renderSlot.load(std::memory_order_relaxed);
    const Pass& p = g_pass[slot % kSlots];
    if (p.seq.load(std::memory_order_acquire) != slot + 1) {
        g_noPass.fetch_add(1, std::memory_order_relaxed); // no eye matrix for this pass: leave it out
        return true;
    }
    float M[16];
    memcpy(M, p.m, sizeof(M));
    const float uu = p.uu;

    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    bool ok = false;
    if (rtvs[0]) {
        D3D11_VIEWPORT vp{};
        UINT nvp = 1;
        ctx->RSGetViewports(&nvp, &vp);
        ID3D11Resource* r = nullptr;
        rtvs[0]->GetResource(&r);
        ID3D11Texture2D* t = nullptr;
        if (r) r->QueryInterface(IID_PPV_ARGS(&t));
        D3D11_TEXTURE2D_DESC td{};
        if (t) t->GetDesc(&td);
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rtvs[0]->GetDesc(&rd);
        if (!g_loggedFirst && t) {
            g_loggedFirst = true;
            log_first(ctx, vp, td);
        }
        if (t && nvp && vp.Width > 0 && vp.Height > 0 && td.SampleDesc.Count == 1 &&
            rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D && ensure_targets(ctx, td, rd.Format)) {
            // Replay the game's own draw (its shaders, texture, constants, blend) onto black and white.
            static const FLOAT kBlack[4] = {0, 0, 0, 0}, kWhite[4] = {1, 1, 1, 1};
            for (int i = 0; i < 2; ++i) {
                ctx->OMSetRenderTargets(1, &g_rtv[i], dsv);
                ctx->ClearRenderTargetView(g_rtv[i], i ? kWhite : kBlack);
                ctx->Draw(3, 0);
            }
            ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);

            // The room screen in the game camera's own view space, then this eye's clip space.
            const float d = g_distM.load(), w = g_widthM.load(), up = g_upM.load();
            const float h = w * vp.Height / vp.Width;
            const float sx[4] = {-1, 1, -1, 1}, sy[4] = {1, 1, -1, -1}; // TL TR BL BR
            float corners[4][4];
            for (int c = 0; c < 4; ++c) {
                const float v[4] = {sx[c] * 0.5f * w * uu, (up + sy[c] * 0.5f * h) * uu, d * uu, 1.0f};
                for (int j = 0; j < 4; ++j)
                    corners[c][j] = v[0] * M[0 * 4 + j] + v[1] * M[1 * 4 + j] + v[2] * M[2 * 4 + j] + v[3] * M[3 * 4 + j];
            }
            // overlay_in_eye draws with a (0,0)-based viewport (the game's is, see the first-draw log)
            // and restores the game's state.
            ok = bvr::blit::overlay_in_eye(ctx, rtvs[0], static_cast<UINT>(vp.Width), static_cast<UINT>(vp.Height),
                                           g_srv[0], g_srv[1], corners);
        }
        if (t) t->Release();
        if (r) r->Release();
    }
    for (auto* v : rtvs)
        if (v) v->Release();
    if (dsv) dsv->Release();
    (ok ? g_drawn : g_failed).fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool title_screen_now() { return GetTickCount64() - g_titleAt.load(std::memory_order_relaxed) < 500; }
bool pause_screen_now() { return GetTickCount64() - g_pausePassAt.load(std::memory_order_relaxed) < 500; }
int mode() { return g_mode.load(); }
void set_mode(int m) { g_mode.store(m < kOriginal || m > kHidden ? kRoom : m); }

void draw_ui() {
    static const char* kModes[] = {"as the game draws it (on your face)", "on a screen in the room", "removed"};
    int m = g_mode.load();
    if (ImGui::Combo("title-screen grime", &m, kModes, 3)) set_mode(m);
    bool dr = g_dirtRoom.load();
    if (ImGui::Checkbox("pause menu grime (off = removed)", &dr)) g_dirtRoom.store(dr);
    if (g_dirtRoom.load()) {
        float sp = g_dirtSpanDeg.load();
        if (bvr::overlay::show_tests() && bvr::overlay::SliderStep("pause dirt spread (deg)", &sp, 60.0f, 300.0f, "%.0f")) g_dirtSpanDeg.store(sp);
    }
    if (g_mode.load() == kRoom || g_dirtRoom.load()) {
        float d = g_distM.load(), w = g_widthM.load(), u = g_upM.load();
        if (bvr::overlay::show_tests() && bvr::overlay::SliderStep("dirt screen distance (m)", &d, 1.0f, 12.0f, "%.1f")) g_distM.store(d);
        if (bvr::overlay::show_tests() && bvr::overlay::SliderStep("dirt screen width (m)", &w, 1.0f, 30.0f, "%.1f")) g_widthM.store(w);
        if (bvr::overlay::show_tests() && bvr::overlay::SliderStep("dirt screen height (m)", &u, -3.0f, 3.0f, "%.2f")) g_upM.store(u);
    }
}

void status_line(char* out, unsigned n) {
    snprintf(out, n,
             "grunge mode %d (room drawn %u, no eye matrix %u, failed %u, hidden %u) | pause dirt room %d (drawn %u, "
             "failed %u, grime removed %u) | screen %.1f m wide at %.1f m",
             g_mode.load(), g_drawn.load(), g_noPass.load(), g_failed.load(), g_hidden.load(), g_dirtRoom.load() ? 1 : 0,
             g_dirtDrawn.load(), g_dirtFailed.load(), g_dirtRemoved.load(), g_widthM.load(), g_distM.load());
}

void settings_write(std::string& out) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "title_grime=%d\npause_grime=%d\npause_dirt_spread_deg=%.0f\ngrunge_distance_m=%.2f\n"
             "grunge_width_m=%.2f\ngrunge_height_m=%.2f\n",
             g_mode.load(), g_dirtRoom.load() ? 1 : 0, g_dirtSpanDeg.load(), g_distM.load(), g_widthM.load(),
             g_upM.load());
    out += buf;
}

bool settings_read(const char* key, const char* val) {
    const float f = static_cast<float>(atof(val));
    if (!strcmp(key, "title_grime")) set_mode(atoi(val));
    else if (!strcmp(key, "pause_grime")) g_dirtRoom.store(atoi(val) != 0);
    else if (!strcmp(key, "grunge_mode") || !strcmp(key, "pause_dirt_room")) {} // before JJ removed the grime
    else if (!strcmp(key, "pause_dirt_spread_deg")) { if (f >= 60.0f && f <= 300.0f) g_dirtSpanDeg.store(f); }
    else if (!strcmp(key, "grunge_distance_m")) { if (f >= 1.0f && f <= 12.0f) g_distM.store(f); }
    else if (!strcmp(key, "grunge_width_m")) { if (f >= 1.0f && f <= 30.0f) g_widthM.store(f); }
    else if (!strcmp(key, "grunge_height_m")) { if (f >= -3.0f && f <= 3.0f) g_upM.store(f); }
    else return false;
    return true;
}

} // namespace bvr::ac::room_overlay
