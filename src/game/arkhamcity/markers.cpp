#include "game/arkhamcity/markers.h"
#include "core/ui/overlay.h"

#include "core/gfx/frame_inspector.h"
#include "core/gfx/blit.h"
#include "core/gfx/gfx_hud.h"
#include "core/util/log.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace bvr::ac::markers {
namespace {

// --- settings (saved) ----------------------------------------------------------------------
std::atomic<bool> g_on{true};
std::atomic<float> g_radius{0.12f}; // NDC distance from a reference point that still counts as its piece
// Which frame's layout the HUD movie shows: 0 = the frame being drawn, 1 = the one before.
std::atomic<int> g_lag{0};
// MARKERCARD (JJ 2026-10-07: markers "spin on their own axis to keep facing the camera" on head turns; that
// should only happen when the camera turns with the mouse / pad): true = each marker is a card facing the
// GAME camera at its object's distance, seen from each eye; false = flat in each eye's picture (facing
// the eye, turning and rolling with the head).
std::atomic<bool> g_card{true};
// MARKERUPRIGHT (JJ 2026-10-11: "the RB icon next to the grappling hook reticle rotates a little bit with your head
// movements"; port of AKN MKUPRIGHT + MKGS, VR_WORLD_MARKERS_GUIDE §5a/§5b): the MARKERCARD card leaned back with the
// camera (plane at the object's base depth) and was moved AFFINELY (first-order), so off-centre pieces such as the RB
// prompt swung on head turns. Now the card stands upright (normal = camera heading, up = world up) and every vertex is
// placed in exact perspective: City's Scaleform vertex shader uses all four cb0 columns (the pause menu is a perspective
// movie, capture 191956), so the homography goes straight into cb0 - no geometry shader needed.
std::atomic<bool> g_upright{true};
// MARKERFACE: the upright card tilts to face the game camera's position (see upright_card).
std::atomic<bool> g_face{true};

// --- per-frame marker sets (game thread writes, render thread reads) -------------------------
constexpr int kMaxPts = 16;
constexpr uint32_t kSets = 8;
struct Set {
    std::atomic<uint32_t> frame{0xFFFFFFFF};
    std::atomic<int> n{0};
    float world[kMaxPts][3];
    float ref[kMaxPts][2]; // reference screen point (game camera of that frame, no head), NDC y up
    bool onScreen[kMaxPts];
    float baseVp[16];    // the game camera's view-projection of that frame (the reference view)
    float baseInv[16];   // its inverse
    bool baseOk = false;
};
Set g_sets[kSets];

// --- per-pass views ----------------------------------------------------------------------------
constexpr uint32_t kSlots = 64;
struct Pass {
    std::atomic<uint32_t> seq{0};
    int eye;
    uint32_t frame;
    float vp[16];
};
Pass g_pass[kSlots];
std::atomic<uint32_t> g_renderSlot{0};
// Game thread: the game camera's own matrix of the current frame (the head-free reference view).
float g_baseVp[16];
uint32_t g_baseFrame = 0xFFFFFFFF;

// --- counters -----------------------------------------------------------------------------------
std::atomic<uint32_t> g_projects{0}, g_forced{0}, g_pieces{0}, g_lastPieces{0}, g_noPass{0};
std::atomic<float> g_lastDx{0}, g_lastDy{0};
std::atomic<uint32_t> g_cards{0}, g_uprights{0}, g_uprightFails{0}, g_faced{0};

// --- render thread: the cb0 to put back after a moved draw ------------------------------------
ID3D11Buffer* g_patchedBuf = nullptr;
uint8_t g_origBytes[640];
unsigned g_origLen = 0;

bool invert4(const float m[16], float inv[16]) {
    double a[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
    for (int col = 0; col < 4; ++col) {
        int piv = col;
        for (int r = col + 1; r < 4; ++r)
            if (fabs(a[r][col]) > fabs(a[piv][col])) piv = r;
        if (fabs(a[piv][col]) < 1e-15) return false;
        if (piv != col)
            for (int j = 0; j < 8; ++j) { const double t = a[col][j]; a[col][j] = a[piv][j]; a[piv][j] = t; }
        const double d = a[col][col];
        for (int j = 0; j < 8; ++j) a[col][j] /= d;
        for (int r = 0; r < 4; ++r) {
            if (r == col) continue;
            const double f = a[r][col];
            if (f != 0.0)
                for (int j = 0; j < 8; ++j) a[r][j] -= f * a[col][j];
        }
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) inv[i * 4 + j] = static_cast<float>(a[i][4 + j]);
    return true;
}

bool project(const float vp[16], const float w[3], float out[4]) {
    float c[4];
    for (int j = 0; j < 4; ++j) c[j] = w[0] * vp[j] + w[1] * vp[4 + j] + w[2] * vp[8 + j] + vp[12 + j];
    if (!(c[3] > 1e-3f)) return false;
    out[0] = c[0] / c[3];
    out[1] = c[1] / c[3];
    out[2] = c[2] / c[3];
    out[3] = c[3];
    return true;
}

// MARKERUPRIGHT: an upright card at W, the size the game laid the marker out at, seen from this eye. Returns the map
// from a reference-picture point (NDC, y up; the head-free game camera's picture) to this eye's clip space,
// clip = K0 + x*Kx + y*Ky - exact perspective (a plane maps linearly to clip space).
bool upright_card(const float bVp[16], const float eVp[16], const float W[3], float rx, float ry, float K0[4], float Kx[4],
                  float Ky[4]) {
    // Gradients of the base clip x / y / w over world position = P00 * camera right, P11 * camera up, camera forward
    // (the base lens is symmetric).
    const float gx[3] = {bVp[0], bVp[4], bVp[8]}, gy[3] = {bVp[1], bVp[5], bVp[9]}, gw[3] = {bVp[3], bVp[7], bVp[11]};
    const float p00 = sqrtf(gx[0] * gx[0] + gx[1] * gx[1] + gx[2] * gx[2]);
    const float p11 = sqrtf(gy[0] * gy[0] + gy[1] * gy[1] + gy[2] * gy[2]);
    const float fl = sqrtf(gw[0] * gw[0] + gw[1] * gw[1] + gw[2] * gw[2]);
    const float hl = sqrtf(gw[0] * gw[0] + gw[1] * gw[1]);
    if (!(p00 > 1e-6f) || !(p11 > 1e-6f) || !(hl > 0.2f * fl)) return false; // looking almost straight up or down
    // Card axes: right = horizontal, perpendicular to the heading (sign: the game view's right); up = world up (UE3 +Z).
    float right[3] = {gw[1] / hl, -gw[0] / hl, 0.0f};
    if (right[0] * gx[0] + right[1] * gx[1] < 0.0f) { right[0] = -right[0]; right[1] = -right[1]; }
    float up[3] = {0.0f, 0.0f, gy[2] >= 0.0f ? 1.0f : -1.0f};
    // World units per reference NDC unit at W's depth (the game's own layout scale there).
    const float bw = W[0] * gw[0] + W[1] * gw[1] + W[2] * gw[2] + bVp[15];
    if (!(bw > 1e-3f)) return false;
    float sx = bw / p00, sy = bw / p11;
    // MARKERFACE (JJ 2026-10-11: "shouldn't the markers and the reticles face the camera rather than ... facing straight
    // out like the rest of the HUD?"): the card tilts to face the GAME camera's position (head-free, so head moves still
    // change nothing); its right edge stays level (no roll). Size fitted so the game camera sees exactly the game's layout.
    if (g_face.load(std::memory_order_relaxed)) {
        // Camera position: where base clip x, y and w are all zero.
        const float* a = gx; const float* b = gy; const float* c = gw;
        const float bc[3] = {b[1] * c[2] - b[2] * c[1], b[2] * c[0] - b[0] * c[2], b[0] * c[1] - b[1] * c[0]};
        const float ca[3] = {c[1] * a[2] - c[2] * a[1], c[2] * a[0] - c[0] * a[2], c[0] * a[1] - c[1] * a[0]};
        const float ab[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        const float det = a[0] * bc[0] + a[1] * bc[1] + a[2] * bc[2];
        if (fabsf(det) > 1e-12f) {
            const float r0 = -bVp[12], r1 = -bVp[13], r2 = -bVp[15];
            float n[3];
            for (int i = 0; i < 3; ++i) n[i] = (r0 * bc[i] + r1 * ca[i] + r2 * ab[i]) / det - W[i]; // W -> camera
            const float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            const float nh = sqrtf(n[0] * n[0] + n[1] * n[1]);
            if (nl > 1e-3f && nh > 0.2f * nl) { // not with the camera almost straight above / below the marker
                for (int i = 0; i < 3; ++i) n[i] /= nl;
                float rt[3] = {-n[1] / (nh / nl), n[0] / (nh / nl), 0.0f};
                if (rt[0] * gx[0] + rt[1] * gx[1] < 0.0f) { rt[0] = -rt[0]; rt[1] = -rt[1]; }
                float u[3] = {n[1] * rt[2] - n[2] * rt[1], n[2] * rt[0] - n[0] * rt[2], n[0] * rt[1] - n[1] * rt[0]};
                if (u[2] < 0.0f) { u[0] = -u[0]; u[1] = -u[1]; u[2] = -u[2]; }
                // Base NDC change per world unit along each axis at W.
                const float cx = W[0] * gx[0] + W[1] * gx[1] + W[2] * gx[2] + bVp[12];
                const float cy = W[0] * gy[0] + W[1] * gy[1] + W[2] * gy[2] + bVp[13];
                const float dx = ((rt[0] * gx[0] + rt[1] * gx[1] + rt[2] * gx[2]) * bw -
                                  cx * (rt[0] * gw[0] + rt[1] * gw[1] + rt[2] * gw[2])) / (bw * bw);
                const float dy = ((u[0] * gy[0] + u[1] * gy[1] + u[2] * gy[2]) * bw -
                                  cy * (u[0] * gw[0] + u[1] * gw[1] + u[2] * gw[2])) / (bw * bw);
                if (dx > 1e-9f && dy > 1e-9f) {
                    memcpy(right, rt, sizeof(right));
                    memcpy(up, u, sizeof(up));
                    sx = 1.0f / dx;
                    sy = 1.0f / dy;
                    g_faced.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }
    float e0[4];
    for (int j = 0; j < 4; ++j) {
        e0[j] = W[0] * eVp[j] + W[1] * eVp[4 + j] + W[2] * eVp[8 + j] + eVp[12 + j];
        Kx[j] = sx * (right[0] * eVp[j] + right[1] * eVp[4 + j] + right[2] * eVp[8 + j]);
        Ky[j] = sy * (up[0] * eVp[j] + up[1] * eVp[4 + j] + up[2] * eVp[8 + j]);
    }
    if (!(e0[3] > 1e-3f)) return false;
    // Scale so clip w = 1 at W (same picture; and should a vertex shader force w = 1, W still lands exactly and the
    // piece degrades to the affine card instead of leaving the screen).
    const float iw = 1.0f / e0[3];
    for (int j = 0; j < 4; ++j) {
        Kx[j] *= iw;
        Ky[j] *= iw;
        K0[j] = e0[j] * iw - rx * Kx[j] - ry * Ky[j];
    }
    return true;
}

// --- AIMDEPTH (JJ 2026-10-08: "the bat claw and batarang target reticles should be at world object depth, not at
// HUD depth") ----------------------------------------------------------------------------------------------------------
// The aim reticle is six Scaleform pieces at the centre of the game camera's picture (HUD dump, fake headset run 19:
// 32x32 / 128x128 sprites at translation (0, 0) +- 0.053, the first HUD draws of the frame; explosive gel has none).
// No world projection the game makes places it, so its target is found from the picture: a one-thread compute pass
// marches the head-free game camera's centre ray (kAimN distances, geometric) through THIS eye's depth buffer and
// returns the first sample behind a surface. Read back a few passes later (no stall). Surfaces nearer than g_aimMin
// are skipped: the ray passes Batman's own arm. The pieces are then placed as a marker card at that point.
std::atomic<bool> g_aimOn{true};
std::atomic<bool> g_aimNoPass{false}; // test: place the pieces from the last distance, no compute pass
std::atomic<float> g_aimMin{250.0f};  // game units
std::atomic<float> g_aimReach{0.09f}; // NDC radius around the picture centre that counts as reticle
constexpr int kAimN = 96;
constexpr float kAimSpan = 200.0f;    // last sample = g_aimMin x kAimSpan
std::atomic<float> g_aimDist{0.0f};   // last hit distance from the near plane (game units); 0 = none yet
std::atomic<uint32_t> g_aimDispatches{0}, g_aimReads{0}, g_aimSky{0}, g_aimPieces{0};

struct Base {
    std::atomic<uint32_t> frame{0xFFFFFFFF};
    float vp[16];
    float inv[16];
    bool ok = false;
};
Base g_bases[kSets]; // the head-free game camera per game frame (written once per frame by the first eye pass)

bool unproject(const float inv[16], float x, float y, float z, float out[3]) {
    float c[4];
    for (int j = 0; j < 4; ++j) c[j] = x * inv[j] + y * inv[4 + j] + z * inv[8 + j] + inv[12 + j];
    if (!(fabsf(c[3]) > 1e-12f)) return false;
    out[0] = c[0] / c[3];
    out[1] = c[1] / c[3];
    out[2] = c[2] / c[3];
    return true;
}
bool aim_ray(const Base& b, float o[3], float dir[3]) {
    float f[3];
    if (!b.ok || !unproject(b.inv, 0.0f, 0.0f, 0.0f, o) || !unproject(b.inv, 0.0f, 0.0f, 0.5f, f)) return false;
    const float d[3] = {f[0] - o[0], f[1] - o[1], f[2] - o[2]};
    const float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(l > 1e-6f)) return false;
    for (int i = 0; i < 3; ++i) dir[i] = d[i] / l;
    return true;
}
float aim_sample_dist(float minD, int i) { return minD * powf(kAimSpan, float(i) / float(kAimN - 1)); }

// The reticle's sprites (run 19 HUD dump): 32x32 / 128x128 block-compressed (BC1 71, BC3 77). Anything else at the
// picture centre (title / loading pieces, prompts) stays where it is.
std::atomic<uint32_t> g_aimRejects{0};
bool reticle_texture(ID3D11DeviceContext* ctx) {
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv) return false;
    bool ok = false;
    ID3D11Resource* r = nullptr;
    srv->GetResource(&r);
    ID3D11Texture2D* t = nullptr;
    D3D11_TEXTURE2D_DESC d{};
    if (r && SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&t))) && t) {
        t->GetDesc(&d);
        ok = (d.Width == 32 || d.Width == 128) && d.Width == d.Height &&
             (d.Format == DXGI_FORMAT_BC1_UNORM || d.Format == DXGI_FORMAT_BC3_UNORM);
        t->Release();
    }
    if (r) r->Release();
    srv->Release();
    if (!ok && g_aimRejects.fetch_add(1, std::memory_order_relaxed) < 8)
        BVR_LOG("[bc] aim depth: centre piece left alone (texture %ux%u format %d)", d.Width, d.Height, int(d.Format));
    return ok;
}

// RETICLEHD (JJ 2026-10-09: "recreate the grappling hook reticle in high resolution; the existing one is low resolution
// and poorly compressed"). The first time a grapple-marker piece uses a texture, the texture is drawn into a plain RGBA8
// picture (the originals are block compressed), read back once and fingerprinted (FNV-1a over the RGBA rows, top-down);
// tools/reticle_hd.py made sharp 8x versions named by the same fingerprint (<game>\BatmanCityData\reticle\<key>.rgba).
// When one exists it is loaded (with a full mip chain) and bound in place of the original for that piece's draw.
// The decoded pictures are also saved to %LOCALAPPDATA%\Batman_City\reticle\ (the designs, for redrawing).
std::atomic<bool> g_hdOn{true};
std::atomic<uint32_t> g_hdSwaps{0};
struct HdEntry { ID3D11ShaderResourceView* game; ID3D11ShaderResourceView* hd; uint32_t key; bool keyed; };
constexpr int kHdMax = 96;
HdEntry g_hd[kHdMax] = {};
int g_hdN = 0;
int g_savedN = 0;
ID3D11ShaderResourceView* g_hdRestore = nullptr; // the game's texture while ours is bound
bool g_hdBound = false;

ID3D11ShaderResourceView* load_hd(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint32_t key) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* sl = wcsrchr(path, L'\\')) *sl = 0;
    wchar_t file[MAX_PATH];
    swprintf_s(file, L"%s\\BatmanCityData\\reticle\\%08x.rgba", path, key);
    FILE* f = _wfopen(file, L"rb");
    if (!f) return nullptr;
    uint32_t wh[2] = {};
    std::vector<uint8_t> px;
    if (fread(wh, 4, 2, f) == 2 && wh[0] && wh[1] && wh[0] <= 2048 && wh[1] <= 2048) {
        px.resize(size_t(wh[0]) * wh[1] * 4);
        if (fread(px.data(), 1, px.size(), f) != px.size()) px.clear();
    }
    fclose(f);
    if (px.empty()) return nullptr;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = wh[0];
    d.Height = wh[1];
    d.MipLevels = 0; // full chain, generated below
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ID3D11Texture2D* t = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    if (SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &t)) && SUCCEEDED(dev->CreateShaderResourceView(t, nullptr, &srv))) {
        bvr::frame_inspector::ScopedSuppress quiet;
        ctx->UpdateSubresource(t, 0, nullptr, px.data(), wh[0] * 4, 0);
        ctx->GenerateMips(srv);
    }
    if (t) t->Release();
    return srv;
}

// Render thread, inside a grapple-marker piece's draw decision: our sharp texture bound in place of the game's (or not).
// The texture of the piece being drawn: its fingerprint (computed once, by a read-back) and sharp version.
// Returns nullptr when the piece has no texture or the table is full.
const HdEntry* piece_entry(ID3D11DeviceContext* ctx, bool saveDesign) {
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv) return nullptr;
    for (int i = 0; i < g_hdN; ++i)
        if (g_hd[i].game == srv) {
            srv->Release();
            return &g_hd[i];
        }
    if (g_hdN >= kHdMax) { srv->Release(); return nullptr; }
    HdEntry& e = g_hd[g_hdN++];
    e.game = srv; // kept: the pointer identifies the texture
    e.hd = nullptr;
    e.key = 0;
    e.keyed = false;
    ID3D11Resource* r = nullptr;
    srv->GetResource(&r);
    ID3D11Texture2D* t = nullptr;
    D3D11_TEXTURE2D_DESC d{};
    if (r && SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&t))) && t) { t->GetDesc(&d); t->Release(); }
    if (r) r->Release();
    if (!d.Width || d.Width > 2048 || d.Height > 2048) return &e;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return &e;
    D3D11_TEXTURE2D_DESC od{};
    od.Width = d.Width;
    od.Height = d.Height;
    od.MipLevels = 1;
    od.ArraySize = 1;
    od.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    od.SampleDesc.Count = 1;
    od.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_TEXTURE2D_DESC sd = od;
    sd.BindFlags = 0;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* out = nullptr;
    ID3D11Texture2D* stage = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    uint32_t key = 0;
    bool keyed = false;
    if (SUCCEEDED(dev->CreateTexture2D(&od, nullptr, &out)) && SUCCEEDED(dev->CreateRenderTargetView(out, nullptr, &rtv)) &&
        SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &stage))) {
        bvr::frame_inspector::ScopedSuppress quiet;
        if (bvr::blit::shift_copy(ctx, rtv, d.Width, d.Height, srv, 0.0f)) {
            ctx->CopyResource(stage, out);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m))) {
                uint32_t h = 0x811C9DC5u;
                for (UINT y = 0; y < d.Height; ++y) {
                    const uint8_t* row = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
                    for (UINT x = 0; x < d.Width * 4; ++x) h = (h ^ row[x]) * 0x01000193u;
                }
                ctx->Unmap(stage, 0);
                key = h;
                keyed = true;
            }
            if (saveDesign && g_savedN < 24) { // the decoded design, for redrawing
                wchar_t dir[MAX_PATH], path[MAX_PATH];
                swprintf_s(dir, L"%s\\reticle", bvr::log::data_dir());
                CreateDirectoryW(dir, nullptr);
                swprintf_s(path, L"%s\\piece_%02d_%ux%u_fmt%d.bmp", dir, g_savedN++, d.Width, d.Height, int(d.Format));
                bvr::gfx_hud::save_texture_bmp(ctx, out, path);
            }
        }
    }
    e.key = key;
    e.keyed = keyed;
    if (keyed && d.Width <= 1024 && d.Height <= 1024) e.hd = load_hd(dev, ctx, key);
    BVR_LOG("[bc] HUD piece texture %ux%u format %d: key %08x, sharp version %s", d.Width, d.Height, int(d.Format), key,
            e.hd ? "LOADED" : "none");
    if (stage) stage->Release();
    if (rtv) rtv->Release();
    if (out) out->Release();
    dev->Release();
    return &e;
}

// RETICLERUN (JJ 2026-10-09: the grapple reticle "breaking the text that was in front of it" - letters of the location
// title near a grapple point were taken into the eye image): a piece near a grapple point belongs to the reticle only
// when it is one of the reticle's own pictures (fingerprints from the RETICLEHD census) or is drawn straight after
// one in the same run (the RB prompt, the 4x4 fills). Anything else near the point stays on the HUD panel.
// 0x9174B635 (512x64, a long teal blob on white, drawn MULTIPLY) was in this list and is NOT the reticle's: it is the
// compass's underlay, drawn right after the reticle and right before the compass's first clip mask (huddumps 01:29:16
// and 01:34:19, #12). As a "reticle picture" it went onto the marker card and tinted the neon sign behind it (JJ
// 2026-10-10: "the teal strip ... appears when ... the compass overlays on top of the neon sign"). Removed.
constexpr uint32_t kReticleKeys[] = {0x4A8A08A5, 0xD4366941, 0x02323DAC, 0xBA1AC3BB, 0x39E417FE,
                                     0x09549989, 0x73B6291B, 0xA6824695, 0xE49CC051};
bool g_runOn = false;
uint32_t g_runSlot = 0xFFFFFFFF;
std::atomic<uint32_t> g_runRejected{0};
std::atomic<uint32_t> g_maskStops{0}; // MASKSTOP: clip masks seen (never marker pieces, end a run)
bool is_reticle_key(const HdEntry* e) {
    if (!e || !e->keyed) return false;
    for (uint32_t k : kReticleKeys)
        if (k == e->key) return true;
    return false;
}

// Render thread, inside a grapple-marker piece's draw decision: our sharp texture bound in place of the game's (or not).
void swap_piece_texture(ID3D11DeviceContext* ctx, const HdEntry* e) {
    if (!e || !e->hd || !g_hdOn.load(std::memory_order_relaxed)) return;
    bvr::frame_inspector::ScopedSuppress quiet;
    ctx->PSGetShaderResources(0, 1, &g_hdRestore); // our reference, released after the draw
    ctx->PSSetShaderResources(0, 1, &e->hd);
    g_hdBound = true;
    g_hdSwaps.fetch_add(1, std::memory_order_relaxed);
}

void hd_after(ID3D11DeviceContext* ctx) {
    if (!g_hdBound) return;
    g_hdBound = false;
    {
        bvr::frame_inspector::ScopedSuppress quiet;
        ctx->PSSetShaderResources(0, 1, &g_hdRestore);
    }
    if (g_hdRestore) g_hdRestore->Release();
    g_hdRestore = nullptr;
}

// Render thread.
ID3D11ComputeShader* g_aimCs = nullptr;
ID3D11Buffer* g_aimCb = nullptr;
ID3D11Buffer* g_aimOut = nullptr;
ID3D11UnorderedAccessView* g_aimUav = nullptr;
ID3D11Buffer* g_aimStage[4] = {};
float g_aimStageMin[4] = {};
uint32_t g_aimK = 0;
ID3D11Resource* g_aimDepthRes = nullptr;
ID3D11ShaderResourceView* g_aimDepthSrv = nullptr;
bool g_aimFailed = false;
uint32_t g_aimDoneSlot = 0xFFFFFFFF;

bool aim_ensure(ID3D11Device* dev) {
    if (g_aimCs) return true;
    if (g_aimFailed) return false;
    static const char cs[] =
        "Texture2D<float> D : register(t0);\n"
        "cbuffer C : register(b0) { float4 P; float4 S[96]; };\n"
        "RWBuffer<uint> R : register(u0);\n"
        "[numthreads(1, 1, 1)] void main() {\n"
        "  uint hit = 0xFFFF;\n"
        "  for (uint i = 0; i < (uint)P.z; ++i) {\n"
        "    float4 q = S[i];\n"
        "    if (q.w < 0.5 || q.x < 0 || q.y < 0 || q.x >= P.x || q.y >= P.y) continue;\n"
        "    if (q.z >= D.Load(int3(int2(q.xy), 0))) { hit = i; break; }\n"
        "  }\n"
        "  R[0] = hit;\n"
        "}\n";
    ID3DBlob* b = nullptr;
    ID3DBlob* err = nullptr;
    bool ok = SUCCEEDED(D3DCompile(cs, sizeof(cs) - 1, "bc_aimdepth", nullptr, nullptr, "main", "cs_5_0", 0, 0, &b, &err)) && b &&
              SUCCEEDED(dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_aimCs));
    if (b) b->Release();
    if (err) {
        BVR_LOG("[bc] aim depth shader: %s", static_cast<const char*>(err->GetBufferPointer()));
        err->Release();
    }
    D3D11_BUFFER_DESC cd{16 + kAimN * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_BUFFER_DESC od{4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, 0, 0};
    D3D11_BUFFER_DESC sd{4, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    ok = ok && SUCCEEDED(dev->CreateBuffer(&cd, nullptr, &g_aimCb)) && SUCCEEDED(dev->CreateBuffer(&od, nullptr, &g_aimOut));
    if (ok) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_UINT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = 1;
        ok = SUCCEEDED(dev->CreateUnorderedAccessView(g_aimOut, &ud, &g_aimUav));
    }
    for (int i = 0; i < 4 && ok; ++i) ok = SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &g_aimStage[i]));
    if (!ok) {
        g_aimFailed = true;
        BVR_LOG("[bc] aim depth pass: setup FAILED - reticles stay on the HUD panel");
        return false;
    }
    BVR_LOG("[bc] aim depth pass ready");
    return true;
}

// Once per eye pass while the reticle shows: read an older answer, then queue this pass's march.
void aim_pass(ID3D11DeviceContext* ctx, const float eyeVp[16], const Base& base, float vpW, float vpH) {
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    const bool ready = aim_ensure(dev);
    // The answer from three passes ago (the copy has long finished; never wait).
    if (ready && g_aimK >= 3) {
        const uint32_t r = (g_aimK - 3) % 4;
        D3D11_MAPPED_SUBRESOURCE ms{};
        if (ctx->Map(g_aimStage[r], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms) == S_OK) {
            const uint32_t hit = *static_cast<const uint32_t*>(ms.pData);
            ctx->Unmap(g_aimStage[r], 0);
            g_aimReads.fetch_add(1, std::memory_order_relaxed);
            if (hit < uint32_t(kAimN)) {
                g_aimDist.store(aim_sample_dist(g_aimStageMin[r], int(hit)), std::memory_order_relaxed);
            } else {
                g_aimSky.fetch_add(1, std::memory_order_relaxed);
                g_aimDist.store(aim_sample_dist(g_aimStageMin[r], kAimN - 1), std::memory_order_relaxed);
            }
        }
    }
    ID3D11Resource* depth = bvr::gfx_hud::scene_depth();
    float o[3], dir[3];
    if (!ready || !depth || !aim_ray(base, o, dir)) {
        dev->Release();
        return;
    }
    if (depth != g_aimDepthRes) {
        if (g_aimDepthSrv) g_aimDepthSrv->Release();
        g_aimDepthSrv = nullptr;
        g_aimDepthRes = depth;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateShaderResourceView(depth, &sd, &g_aimDepthSrv))) g_aimDepthSrv = nullptr;
    }
    dev->Release();
    if (!g_aimDepthSrv) return;
    // The ray's samples, seen by this eye: pixel position and depth.
    struct { float p[4]; float s[kAimN][4]; } c{};
    const float minD = g_aimMin.load(std::memory_order_relaxed);
    c.p[0] = vpW;
    c.p[1] = vpH;
    c.p[2] = float(kAimN);
    for (int i = 0; i < kAimN; ++i) {
        const float d = aim_sample_dist(minD, i);
        const float w[3] = {o[0] + dir[0] * d, o[1] + dir[1] * d, o[2] + dir[2] * d};
        float e[4];
        if (!project(eyeVp, w, e)) continue; // s[i].w stays 0: skipped
        c.s[i][0] = (e[0] * 0.5f + 0.5f) * vpW;
        c.s[i][1] = (0.5f - e[1] * 0.5f) * vpH;
        c.s[i][2] = e[2];
        c.s[i][3] = 1.0f;
    }
    bvr::frame_inspector::ScopedSuppress quiet;
    ctx->UpdateSubresource(g_aimCb, 0, nullptr, &c, 0, 0);
    // The scene depth may still be bound for depth-tested HUD drawing: unbind the targets around the pass.
    ID3D11RenderTargetView* oRtv = nullptr;
    ID3D11DepthStencilView* oDsv = nullptr;
    ctx->OMGetRenderTargets(1, &oRtv, &oDsv);
    ID3D11ComputeShader* oCs = nullptr;
    ID3D11ShaderResourceView* oSrv = nullptr;
    ID3D11UnorderedAccessView* oUav = nullptr;
    ID3D11Buffer* oCb = nullptr;
    ctx->CSGetShader(&oCs, nullptr, nullptr);
    ctx->CSGetShaderResources(0, 1, &oSrv);
    ctx->CSGetUnorderedAccessViews(0, 1, &oUav);
    ctx->CSGetConstantBuffers(0, 1, &oCb);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ctx->CSSetShader(g_aimCs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &g_aimDepthSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &g_aimUav, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &g_aimCb);
    ctx->Dispatch(1, 1, 1);
    ID3D11ShaderResourceView* nsv = nullptr;
    ID3D11UnorderedAccessView* nuv = nullptr;
    ctx->CSSetShaderResources(0, 1, &nsv);
    ctx->CSSetUnorderedAccessViews(0, 1, &nuv, nullptr);
    const uint32_t w = g_aimK % 4;
    ctx->CopyResource(g_aimStage[w], g_aimOut);
    g_aimStageMin[w] = minD;
    ++g_aimK;
    g_aimDispatches.fetch_add(1, std::memory_order_relaxed);
    ctx->CSSetShader(oCs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &oSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &oUav, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &oCb);
    ctx->OMSetRenderTargets(1, &oRtv, oDsv);
    if (oCs) oCs->Release();
    if (oSrv) oSrv->Release();
    if (oUav) oUav->Release();
    if (oCb) oCb->Release();
    if (oRtv) oRtv->Release();
    if (oDsv) oDsv->Release();
}

} // namespace

void set_pass(uint32_t slot, int eye, uint32_t frame, const float vp[16], const float baseVp[16]) {
    Pass& p = g_pass[slot % kSlots];
    p.seq.store(0, std::memory_order_relaxed);
    p.eye = eye;
    p.frame = frame;
    memcpy(p.vp, vp, sizeof(p.vp));
    p.seq.store(slot + 1, std::memory_order_release);
    if (eye != 0) {
        memcpy(g_baseVp, baseVp, sizeof(g_baseVp));
        g_baseFrame = frame;
        Base& b = g_bases[frame % kSets];
        if (b.frame.load(std::memory_order_relaxed) != frame) {
            b.frame.store(0xFFFFFFFF, std::memory_order_relaxed);
            memcpy(b.vp, baseVp, sizeof(b.vp));
            b.ok = invert4(b.vp, b.inv);
            b.frame.store(frame, std::memory_order_release);
        }
    }
}

void on_project(int eye, uint32_t frame, const float world[3], float out[4]) {
    g_projects.fetch_add(1, std::memory_order_relaxed);
    if (!g_on.load(std::memory_order_relaxed)) return;
    float ref[4] = {out[0], out[1], out[2], out[3]};
    if (eye != 0 && g_baseFrame == frame && project(g_baseVp, world, ref)) {
        memcpy(out, ref, sizeof(ref)); // both eye passes now place the marker from the head-free game camera
        g_forced.fetch_add(1, std::memory_order_relaxed);
    }
    Set& s = g_sets[frame % kSets];
    if (s.frame.load(std::memory_order_relaxed) != frame) {
        s.n.store(0, std::memory_order_relaxed);
        s.baseOk = g_baseFrame == frame && invert4(g_baseVp, s.baseInv);
        if (s.baseOk) memcpy(s.baseVp, g_baseVp, sizeof(s.baseVp));
        s.frame.store(frame, std::memory_order_release);
    }
    const int n = s.n.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
        if (fabsf(s.world[i][0] - world[0]) < 0.5f && fabsf(s.world[i][1] - world[1]) < 0.5f &&
            fabsf(s.world[i][2] - world[2]) < 0.5f)
            return; // the other eye's pass projected the same point
    if (n >= kMaxPts) return;
    memcpy(s.world[n], world, 12);
    s.ref[n][0] = ref[0];
    s.ref[n][1] = ref[1];
    s.onScreen[n] = ref[3] > 0.0f && fabsf(ref[0]) < 1.2f && fabsf(ref[1]) < 1.2f;
    s.n.store(n + 1, std::memory_order_release);
}

void set_render_slot(uint32_t slot) { g_renderSlot.store(slot, std::memory_order_relaxed); }

bool on_hud_draw(ID3D11DeviceContext* ctx) {
    if (!g_on.load(std::memory_order_relaxed)) return false;
    const uint32_t slot = g_renderSlot.load(std::memory_order_relaxed);
    const Pass& p = g_pass[slot % kSlots];
    if (p.seq.load(std::memory_order_acquire) != slot + 1) {
        g_noPass.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    ID3D11Buffer* cb = nullptr;
    ctx->VSGetConstantBuffers(0, 1, &cb);
    if (!cb) return false;
    float m[160];
    const unsigned len = bvr::frame_inspector::cb_shadow(cb, m, sizeof(m));
    // A flat Scaleform piece: 4x4 placement, translation in row 3 (x, y, 0, 1) in NDC.
    if (len < 64 || m[15] != 1.0f || m[14] != 0.0f || m[3] != 0.0f || m[7] != 0.0f) {
        cb->Release();
        return false;
    }
    const float tx = m[12], ty = m[13];
    // MASKSTOP (JJ 2026-10-10: the compass vanishes, its teal glow "displaced and put above it", whenever the camera is
    // on the GCR neon sign; capture 012149: the grapple point at (-0.05, 0.66), the compass's clip mask anchored at
    // (0.01, 0.65) - inside the 0.12 reach, drawn right after the reticle, so RETICLERUN's run took the mask and the
    // pieces after it into the eye; the compass left on the panel had no mask). A clip mask (every colour write off)
    // starts a new clipped group: it ends any run and is never a marker piece.
    // MASKSTOP2 (JJ 01:30: "the teal underlay for it is still popping out and appearing above it"; capture 012916: the
    // run's 13th piece, a 512x64 MULTIPLY strip (DEST_COLOR, ZERO) right after the reticle, went to the marker card):
    // a multiply piece also ends a run unless it is one of the reticle's own pictures.
    bool multiply = false;
    {
        ID3D11BlendState* bs = nullptr;
        FLOAT bf[4];
        UINT sm = 0;
        ctx->OMGetBlendState(&bs, bf, &sm);
        UINT8 wm = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (bs) {
            D3D11_BLEND_DESC bd{};
            bs->GetDesc(&bd);
            const D3D11_RENDER_TARGET_BLEND_DESC& rt = bd.RenderTarget[0];
            wm = rt.RenderTargetWriteMask;
            multiply = rt.BlendEnable && (rt.SrcBlend == D3D11_BLEND_DEST_COLOR || rt.DestBlend == D3D11_BLEND_SRC_COLOR);
            bs->Release();
        }
        if ((wm & D3D11_COLOR_WRITE_ENABLE_ALL) == 0) {
            if (slot == g_runSlot) g_runOn = false;
            g_maskStops.fetch_add(1, std::memory_order_relaxed);
            cb->Release();
            return false;
        }
    }
    // Where the piece belongs: a world point, its reference screen point, and the head-free camera that saw it.
    float world[3] = {0, 0, 0}, rx = 0.0f, ry = 0.0f;
    const float* bVp = nullptr;
    const float* bInv = nullptr;
    bool have = false;
    bool grapple = false; // RETICLEHD: a grapple-marker piece (sharp texture swapped in when it is drawn)
    const HdEntry* entry = nullptr;
    if (slot != g_runSlot) { g_runSlot = slot; g_runOn = false; } // RETICLERUN: runs never cross a pass
    // 1) World markers: points the HUD script projected this frame (Canvas.Project).
    const uint32_t f = p.frame - static_cast<uint32_t>(g_lag.load(std::memory_order_relaxed));
    const Set& s = g_sets[f % kSets];
    if (s.frame.load(std::memory_order_acquire) == f) {
        const int n = s.n.load(std::memory_order_acquire);
        const float r = g_radius.load(std::memory_order_relaxed);
        int best = -1;
        float bestD = r * r;
        for (int i = 0; i < n; ++i) {
            if (!s.onScreen[i]) continue;
            const float dx = tx - s.ref[i][0], dy = ty - s.ref[i][1], d = dx * dx + dy * dy;
            if (d <= bestD) { bestD = d; best = i; }
        }
        if (best >= 0) {
            // RETICLERUN: only the reticle's own pictures start a run; other pieces only continue one.
            entry = piece_entry(ctx, false);
            // MASKSTOP: a run carries at most 8 pieces past the last reticle picture (RB prompt + its fills), so a
            // HUD group drawn after the reticle near the same point (the compass) never rides along.
            static int s_runLeft = 0;
            if (is_reticle_key(entry)) { g_runOn = true; s_runLeft = 8; }
            else if (g_runOn && (multiply || --s_runLeft < 0)) g_runOn = false; // MASKSTOP2: multiply ends a run
            if (!g_runOn) {
                best = -1;
                g_runRejected.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            g_runOn = false;
        }
        if (best >= 0) {
            memcpy(world, s.world[best], sizeof(world));
            rx = s.ref[best][0];
            ry = s.ref[best][1];
            if (s.baseOk) { bVp = s.baseVp; bInv = s.baseInv; }
            grapple = true;
            have = true;
        }
    }
    // 2) AIMDEPTH: the aim reticle at the picture centre, on the surface the game camera looks at.
    const float ar = g_aimReach.load(std::memory_order_relaxed);
    if (!have && g_aimOn.load(std::memory_order_relaxed) && tx * tx + ty * ty <= ar * ar && reticle_texture(ctx)) {
        const Base& b = g_bases[p.frame % kSets];
        if (b.frame.load(std::memory_order_acquire) == p.frame && b.ok) {
            if (g_aimDoneSlot != slot && !g_aimNoPass.load(std::memory_order_relaxed)) {
                g_aimDoneSlot = slot;
                D3D11_VIEWPORT vp{};
                UINT nv = 1;
                ctx->RSGetViewports(&nv, &vp);
                if (nv == 1 && vp.Width > 0.0f) aim_pass(ctx, p.vp, b, vp.Width, vp.Height);
            }
            const float dist = g_aimDist.load(std::memory_order_relaxed);
            float o[3], dir[3];
            if (dist > 0.0f && aim_ray(b, o, dir)) {
                for (int i = 0; i < 3; ++i) world[i] = o[i] + dir[i] * dist;
                bVp = b.vp;
                bInv = b.inv;
                have = true;
                if (g_aimPieces.fetch_add(1, std::memory_order_relaxed) % 2000 == 0)
                    BVR_LOG("[bc] aim depth: reticle piece placed at %.0f game units (piece %u)", dist, g_aimPieces.load());
            }
        }
    }
    if (!have) {
        cb->Release();
        return false;
    }
    float eye[4];
    if (!project(p.vp, world, eye)) {
        cb->Release();
        return false;
    }
    const float dx = eye[0] - rx, dy = eye[1] - ry;
    g_lastDx.store(dx, std::memory_order_relaxed);
    g_lastDy.store(dy, std::memory_order_relaxed);
    g_pieces.fetch_add(1, std::memory_order_relaxed);
    memcpy(g_origBytes, m, len);
    g_origLen = len;
    // MARKERCARD: the marker as a card facing the game camera at its object's view depth. F maps a point
    // of the reference picture onto that card (same base clip z and w as the object) and then into this
    // eye; the piece's placement is replaced by F's first-order expansion about the reference point
    // (markers are small, so the linear part is exact to well under a pixel).
    // MARKERUPRIGHT: the piece's reference point q = (x*m0 + y*m4 + m12, x*m1 + y*m5 + m13) goes through
    // clip = K0 + q.x*Kx + q.y*Ky, folded into cb0's rows 0 / 1 / 3 (all four columns: w becomes perspective). z stays 0.
    bool upright = false;
    if (g_card.load(std::memory_order_relaxed) && g_upright.load(std::memory_order_relaxed) && bVp) {
        float K0[4], Kx[4], Ky[4];
        if (upright_card(bVp, p.vp, world, rx, ry, K0, Kx, Ky)) {
            float r0[4], r1[4], r3[4];
            for (int j = 0; j < 4; ++j) {
                r0[j] = m[0] * Kx[j] + m[1] * Ky[j];
                r1[j] = m[4] * Kx[j] + m[5] * Ky[j];
                r3[j] = K0[j] + m[12] * Kx[j] + m[13] * Ky[j];
            }
            r0[2] = r1[2] = r3[2] = 0.0f;
            memcpy(m, r0, 16);
            memcpy(m + 4, r1, 16);
            memcpy(m + 12, r3, 16);
            upright = true;
            g_uprights.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_uprightFails.fetch_add(1, std::memory_order_relaxed);
        }
    }
    float Jx[2] = {1, 0}, Jy[2] = {0, 1};
    bool card = false;
    if (!upright && g_card.load(std::memory_order_relaxed) && bVp && bInv) {
        float bc[4];
        for (int j = 0; j < 4; ++j)
            bc[j] = world[0] * bVp[j] + world[1] * bVp[4 + j] + world[2] * bVp[8 + j] + bVp[12 + j];
        auto F = [&](float x, float y, float o[2]) {
            const float c[4] = {x * bc[3], y * bc[3], bc[2], bc[3]};
            float w[4];
            for (int j = 0; j < 4; ++j)
                w[j] = c[0] * bInv[j] + c[1] * bInv[4 + j] + c[2] * bInv[8 + j] + c[3] * bInv[12 + j];
            if (!(fabsf(w[3]) > 1e-9f)) return false;
            const float pw[3] = {w[0] / w[3], w[1] / w[3], w[2] / w[3]};
            float e[4];
            if (!project(p.vp, pw, e)) return false;
            o[0] = e[0];
            o[1] = e[1];
            return true;
        };
        const float h = 0.01f;
        float f0[2], fx[2], fy[2];
        if (bc[3] > 1e-3f && F(rx, ry, f0) && F(rx + h, ry, fx) && F(rx, ry + h, fy)) {
            Jx[0] = (fx[0] - f0[0]) / h; Jx[1] = (fx[1] - f0[1]) / h;
            Jy[0] = (fy[0] - f0[0]) / h; Jy[1] = (fy[1] - f0[1]) / h;
            card = true;
        }
    }
    if (card) {
        // Rows 0 / 1 (local x / y -> NDC) and the translation, through the 2x2 linear part.
        const float r0x = m[0], r0y = m[1], r1x = m[4], r1y = m[5], ox = m[12] - rx, oy = m[13] - ry;
        m[0] = r0x * Jx[0] + r0y * Jy[0];
        m[1] = r0x * Jx[1] + r0y * Jy[1];
        m[4] = r1x * Jx[0] + r1y * Jy[0];
        m[5] = r1x * Jx[1] + r1y * Jy[1];
        m[12] = eye[0] + ox * Jx[0] + oy * Jy[0];
        m[13] = eye[1] + ox * Jx[1] + oy * Jy[1];
        g_cards.fetch_add(1, std::memory_order_relaxed);
    } else if (!upright) {
        m[12] += dx;
        m[13] += dy;
    }
    {
        bvr::frame_inspector::ScopedSuppress quiet;
        ctx->UpdateSubresource(cb, 0, nullptr, m, 0, 0);
    }
    g_patchedBuf = cb; // reference kept until after_hud_draw
    if (grapple) swap_piece_texture(ctx, entry);
    return true;
}

void after_hud_draw(ID3D11DeviceContext* ctx) {
    hd_after(ctx);
    if (!g_patchedBuf) return;
    {
        bvr::frame_inspector::ScopedSuppress quiet;
        ctx->UpdateSubresource(g_patchedBuf, 0, nullptr, g_origBytes, 0, 0);
    }
    g_patchedBuf->Release();
    g_patchedBuf = nullptr;
}

void draw_ui() {
    bool on = g_on.load();
    if (ImGui::Checkbox("grapple reticle and markers sit on their targets (each eye)", &on)) g_on.store(on);
    if (g_on.load()) {
        float r = g_radius.load();
        if (bvr::overlay::show_tests() && bvr::overlay::SliderStep("   marker piece reach", &r, 0.03f, 0.4f, "%.2f")) g_radius.store(r);
        bool card = g_card.load();
        if (bvr::overlay::show_tests() && ImGui::Checkbox("   markers turn with the game camera, not your head", &card)) g_card.store(card);
        bool upr = g_upright.load();
        if (bvr::overlay::show_tests() && ImGui::Checkbox("   markers stand upright in exact perspective (Arkham Knight fix)", &upr)) g_upright.store(upr);
        bool face = g_face.load();
        if (bvr::overlay::show_tests() && ImGui::Checkbox("   ... tilted to face the game camera", &face)) g_face.store(face);
        int lag = g_lag.load();
        if (bvr::overlay::show_tests() && bvr::overlay::SliderStepInt("   marker frame lag", &lag, 0, 1)) g_lag.store(lag);
        if (bvr::overlay::show_tests()) ImGui::TextDisabled("   moved pieces %u, last shift %.3f, %.3f", g_pieces.load(), g_lastDx.load(), g_lastDy.load());
    }
    bool hd = g_hdOn.load();
    if (ImGui::Checkbox("sharp grapple reticle (redrawn at high resolution)", &hd)) g_hdOn.store(hd);
    int loaded = 0;
    for (int i = 0; i < g_hdN; ++i) loaded += g_hd[i].hd ? 1 : 0;
    if (bvr::overlay::show_tests()) ImGui::TextDisabled("   sharp pictures in use %d of %d seen, swaps %u | other pieces left on the panel %u, clip masks %u",
                        loaded, g_hdN, g_hdSwaps.load(), g_runRejected.load(), g_maskStops.load());
    bool aim = g_aimOn.load();
    if (ImGui::Checkbox("batarang / batclaw aim reticle sits on what it points at", &aim)) g_aimOn.store(aim);
    if (g_aimOn.load()) {
        float mn = g_aimMin.load();
        if (bvr::overlay::show_tests() && bvr::overlay::SliderStep("   ignore surfaces nearer than (game units)", &mn, 50.0f, 800.0f, "%.0f")) g_aimMin.store(mn);
        if (bvr::overlay::show_tests()) ImGui::TextDisabled("   last target distance %.0f game units, reticle pieces placed %u", g_aimDist.load(),
                            g_aimPieces.load());
    }
}

void status_text(std::string& out) {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "\nMARKEREYE on %d reach %.2f lag %d | Canvas.Project calls %u, forced to the game camera %u | marker pieces "
             "moved %u (no pass %u, as cards %u, upright %d: placed %u, fell back %u, face %d: faced %u), last shift "
             "(%.4f, %.4f) | reference = game camera, no head\n",
             g_on.load() ? 1 : 0, g_radius.load(), g_lag.load(), g_projects.load(), g_forced.load(), g_pieces.load(),
             g_noPass.load(), g_cards.load(), g_upright.load() ? 1 : 0, g_uprights.load(), g_uprightFails.load(),
             g_face.load() ? 1 : 0, g_faced.load(),
             g_lastDx.load(), g_lastDy.load());
    out += buf;
    snprintf(buf, sizeof(buf),
             "AIMDEPTH on %d min %.0f reach %.2f | passes %u, answers %u (no surface %u), last distance %.1f, reticle pieces "
             "placed %u%s\n",
             g_aimOn.load() ? 1 : 0, g_aimMin.load(), g_aimReach.load(), g_aimDispatches.load(), g_aimReads.load(),
             g_aimSky.load(), g_aimDist.load(), g_aimPieces.load(), g_aimFailed ? " | SETUP FAILED" : "");
    out += buf;
    for (uint32_t k = 0; k < kSets; ++k) {
        const Set& s = g_sets[k];
        const int n = s.n.load();
        if (!n) continue;
        snprintf(buf, sizeof(buf), "set frame %u: %d point(s)\n", s.frame.load(), n);
        out += buf;
        for (int i = 0; i < n && i < kMaxPts; ++i) {
            snprintf(buf, sizeof(buf), "   world (%.1f, %.1f, %.1f) ref (%.4f, %.4f)%s\n", s.world[i][0], s.world[i][1],
                     s.world[i][2], s.ref[i][0], s.ref[i][1], s.onScreen[i] ? "" : " off-screen");
            out += buf;
        }
    }
}

void settings_write(std::string& out) {
    char buf[160];
    snprintf(buf, sizeof(buf), "marker_fix=%d\nmarker_reach=%.3f\nmarker_lag=%d\nmarker_card=%d\naim_depth=%d\naim_min=%.0f\n",
             g_on.load() ? 1 : 0, g_radius.load(), g_lag.load(), g_card.load() ? 1 : 0, g_aimOn.load() ? 1 : 0,
             g_aimMin.load());
    out += buf;
    out += g_hdOn.load() ? "reticle_hd=1\n" : "reticle_hd=0\n";
    out += g_upright.load() ? "marker_upright=1\n" : "marker_upright=0\n";
    out += g_face.load() ? "marker_face=1\n" : "marker_face=0\n";
}

bool settings_read(const char* key, const char* val) {
    const float f = static_cast<float>(atof(val));
    if (!strcmp(key, "marker_fix")) g_on.store(atoi(val) != 0);
    else if (!strcmp(key, "marker_reach")) { if (f >= 0.03f && f <= 0.4f) g_radius.store(f); }
    else if (!strcmp(key, "marker_card")) g_card.store(atoi(val) != 0);
    else if (!strcmp(key, "marker_upright")) g_upright.store(atoi(val) != 0);
    else if (!strcmp(key, "marker_face")) g_face.store(atoi(val) != 0);
    else if (!strcmp(key, "marker_lag")) { const int l = atoi(val); g_lag.store(l == 1 ? 1 : 0); }
    else if (!strcmp(key, "aim_depth")) g_aimOn.store(atoi(val) != 0);
    else if (!strcmp(key, "reticle_hd")) g_hdOn.store(atoi(val) != 0);
    else if (!strcmp(key, "aim_nopass")) { g_aimNoPass.store(atoi(val) != 0); if (g_aimDist.load() <= 0.0f) g_aimDist.store(850.0f); }
    else if (!strcmp(key, "aim_min")) { if (f >= 50.0f && f <= 800.0f) g_aimMin.store(f); }
    else return false;
    return true;
}

} // namespace bvr::ac::markers
