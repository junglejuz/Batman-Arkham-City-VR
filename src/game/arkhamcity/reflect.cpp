#include "game/arkhamcity/reflect.h"

#include "core/gfx/blit.h"
#include "core/gfx/frame_inspector.h"
#include "core/gfx/gfx_hud.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"

#include <d3d11.h>
#include <imgui.h>
#include <MinHook.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bvr::ac::reflect {
namespace {

std::atomic<bool> g_on{true};      // PERCAPTURE: the capture renders for each eye
std::atomic<bool> g_shift{false};  // the first attempt (shift the left picture) - off
std::atomic<int> g_eye{0};
std::atomic<uint32_t> g_pass{0};
uint32_t g_shiftedPass = 0xFFFFFFFF; // render thread: the pass whose picture is already shifted
std::atomic<uint32_t> g_shifts{0}, g_failed{0};
std::atomic<float> g_lastDu{0.0f};
bool g_logged = false;

// --- PERCAPTURE ----------------------------------------------------------------------------------------
using UpdateFn = int(__fastcall*)(void* probe, void* edx, const void* family);
UpdateFn g_updateOrig = nullptr;
constexpr uintptr_t kReflectUpdateRequired = 0x0080A2B0;
struct LeftUpdate { void* probe; uint32_t pass; };
LeftUpdate g_leftUpdates[16]; // render thread only
unsigned g_leftUpdateN = 0;
std::atomic<uint32_t> g_forced{0}, g_leftCaptures{0}, g_rightSkips{0};

int __fastcall update_detour(void* probe, void* edx, const void* family) {
    const int r = g_updateOrig(probe, edx, family);
    const int eye = g_eye.load(std::memory_order_relaxed);
    const uint32_t pass = g_pass.load(std::memory_order_relaxed);
    if (eye < 0) {
        if (r) {
            g_leftCaptures.fetch_add(1, std::memory_order_relaxed);
            g_leftUpdates[g_leftUpdateN++ % 16] = {probe, pass};
        }
        return r;
    }
    if (eye > 0 && !r && g_on.load(std::memory_order_relaxed)) {
        // Updated in this frame's left pass (the pass just before) -> render it again for the right eye.
        for (const LeftUpdate& u : g_leftUpdates)
            if (u.probe == probe && pass - u.pass <= 2) {
                g_forced.fetch_add(1, std::memory_order_relaxed);
                return 1;
            }
        g_rightSkips.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
}
std::atomic<bool> g_picReq{false};
wchar_t g_picDir[260] = {};

// The reflection picture (identified by its texture) and our working copy of it.
ID3D11Texture2D* g_reflTex = nullptr; // weak: compared only
ID3D11Texture2D* g_copy = nullptr;
ID3D11ShaderResourceView* g_copySrv = nullptr;
ID3D11RenderTargetView* g_reflRtv = nullptr;

void release_views() {
    if (g_copySrv) { g_copySrv->Release(); g_copySrv = nullptr; }
    if (g_copy) { g_copy->Release(); g_copy = nullptr; }
    if (g_reflRtv) { g_reflRtv->Release(); g_reflRtv = nullptr; }
}

// The engine's planar reflection target: 256x240, render target + shader resource (LIVE 2026-10-08).
bool is_reflection(const D3D11_TEXTURE2D_DESC& d) {
    return d.Width == 256 && d.Height == 240 && d.SampleDesc.Count == 1 && d.ArraySize == 1 &&
           (d.BindFlags & D3D11_BIND_RENDER_TARGET) && (d.BindFlags & D3D11_BIND_SHADER_RESOURCE);
}

DXGI_FORMAT plain_format(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return f;
    }
}

bool ensure_views(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC& d) {
    if (tex == g_reflTex && g_copy) return true;
    release_views();
    g_reflTex = tex;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    const DXGI_FORMAT vf = plain_format(d.Format);
    D3D11_TEXTURE2D_DESC cd = d;
    cd.MipLevels = 1;
    cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    cd.CPUAccessFlags = 0;
    cd.MiscFlags = 0;
    cd.Usage = D3D11_USAGE_DEFAULT;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = vf;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = vf;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    const bool ok = SUCCEEDED(dev->CreateTexture2D(&cd, nullptr, &g_copy)) &&
                    SUCCEEDED(dev->CreateShaderResourceView(g_copy, &sd, &g_copySrv)) &&
                    SUCCEEDED(dev->CreateRenderTargetView(tex, &rd, &g_reflRtv));
    dev->Release();
    if (!ok) {
        release_views();
        BVR_LOG("[bc] reflections: views on the %ux%u fmt %d reflection FAILED", d.Width, d.Height, int(d.Format));
    }
    return ok;
}

// Horizontal offset between the eyes' pictures for distant points, in texture u: both eyes share the head's
// rotation, so only the lens offsets differ. ndc = (x/z) * m00 + m20, m20 = -(r + l) / (r - l) (tangents).
bool eye_shift_u(float* du) {
    float a[4], b[4];
    if (!bvr::vr::asym_fov() || !bvr::vr::eye_fov_angles(0, a) || !bvr::vr::eye_fov_angles(1, b)) return false;
    const float lL = tanf(a[0]), rL = tanf(a[1]), lR = tanf(b[0]), rR = tanf(b[1]);
    const float m20L = -(rL + lL) / (rL - lL), m20R = -(rR + lR) / (rR - lR);
    // right-eye ndc = left-eye ndc + (m20R - m20L); the right eye at u reads the left picture at u - du_ndc/2
    *du = -(m20R - m20L) * 0.5f;
    return true;
}

} // namespace

void install() {
    static const uint8_t kPro[] = {0x83, 0xEC, 0x18, 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x83, 0x7E, 0x3C, 0x00};
    void* t = reinterpret_cast<void*>(kReflectUpdateRequired);
    if (memcmp(t, kPro, sizeof(kPro)) != 0) {
        BVR_LOG("[bc] reflections: capture update check at %08X has an unexpected prologue - per-eye capture OFF",
                unsigned(kReflectUpdateRequired));
        return;
    }
    const MH_STATUS a = MH_CreateHook(t, reinterpret_cast<void*>(&update_detour), reinterpret_cast<void**>(&g_updateOrig));
    const MH_STATUS b = a == MH_OK ? MH_EnableHook(t) : a;
    BVR_LOG("[bc] hook reflection capture update check @%08X: %s", unsigned(kReflectUpdateRequired), MH_StatusToString(b));
}

void set_render_pass(uint32_t beginIndex, int eyeSign) {
    g_pass.store(beginIndex, std::memory_order_relaxed);
    g_eye.store(eyeSign, std::memory_order_relaxed);
}

void on_draw_indexed(ID3D11DeviceContext* ctx) {
    if (!g_shift.load(std::memory_order_relaxed) || g_eye.load(std::memory_order_relaxed) <= 0) return;
    const uint32_t pass = g_pass.load(std::memory_order_relaxed);
    if (pass == g_shiftedPass) return;
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv) return;
    ID3D11Resource* r = nullptr;
    srv->GetResource(&r);
    srv->Release();
    if (!r) return;
    ID3D11Texture2D* t = nullptr;
    r->QueryInterface(IID_PPV_ARGS(&t));
    r->Release();
    if (!t) return;
    // Cheap check first: the texture we already know.
    bool hit = t == g_reflTex && g_copy;
    D3D11_TEXTURE2D_DESC d{};
    if (!hit) {
        t->GetDesc(&d);
        hit = is_reflection(d);
    }
    if (hit) {
        g_shiftedPass = pass; // at most once per right-eye pass
        if (!d.Width) t->GetDesc(&d);
        float du = 0.0f;
        bool ok = false;
        if (eye_shift_u(&du) && ensure_views(ctx, t, d)) {
            if (!g_logged) {
                g_logged = true;
                BVR_LOG("[bc] reflections: right-eye shift on the %ux%u fmt %d planar reflection, du %.3f", d.Width,
                        d.Height, int(d.Format), du);
            }
            // Copy, then draw the copy back shifted. The draw below then reads the shifted picture; the next
            // left pass renders the reflection again.
            ctx->CopyResource(g_copy, t);
            ok = bvr::blit::shift_copy(ctx, g_reflRtv, d.Width, d.Height, g_copySrv, du);
            if (g_picReq.exchange(false)) {
                wchar_t path[300];
                swprintf_s(path, L"%s\\reflection_before.bmp", g_picDir);
                const bool b1 = bvr::gfx_hud::save_texture_bmp(ctx, g_copy, path);
                swprintf_s(path, L"%s\\reflection_after.bmp", g_picDir);
                const bool b2 = bvr::gfx_hud::save_texture_bmp(ctx, t, path);
                BVR_LOG("[bc] reflections: before/after pictures saved %d/%d (shift ok %d, du %.3f, pass %u)", b1, b2, ok,
                        du, pass);
            }
            // shift_copy left the game's own bindings in place except what it restores; srv0 still points at
            // the reflection, which now holds the right eye's picture.
            g_lastDu.store(du, std::memory_order_relaxed);
        }
        (ok ? g_shifts : g_failed).fetch_add(1, std::memory_order_relaxed);
    }
    t->Release();
}

void request_pictures(const wchar_t* dir) {
    wcsncpy_s(g_picDir, dir, _TRUNCATE);
    g_picReq.store(true);
}

void draw_ui() {
    bool on = g_on.load();
    if (ImGui::Checkbox("puddle reflections rendered for each eye", &on)) g_on.store(on);
}

void status_line(char* out, unsigned n) {
    snprintf(out, n,
             "REFLECTEYE per-eye capture %d: left-pass captures %u, right-pass re-captures %u, right skips %u | shift "
             "option %d: shifts %u (failed %u), last du %.3f",
             g_on.load() ? 1 : 0, g_leftCaptures.load(), g_forced.load(), g_rightSkips.load(), g_shift.load() ? 1 : 0,
             g_shifts.load(), g_failed.load(), g_lastDu.load());
}

void settings_write(std::string& out) {
    char buf[64];
    snprintf(buf, sizeof(buf), "reflect_per_eye=%d\n", g_on.load() ? 1 : 0);
    out += buf;
}

bool settings_read(const char* key, const char* val) {
    if (!strcmp(key, "reflect_fix")) return true; // the shift attempt's switch - ignored
    if (!strcmp(key, "reflect_shift")) { g_shift.store(atoi(val) != 0); return true; }
    if (strcmp(key, "reflect_per_eye")) return false;
    g_on.store(atoi(val) != 0);
    return true;
}

} // namespace bvr::ac::reflect
