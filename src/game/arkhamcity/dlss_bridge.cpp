#include "game/arkhamcity/dlss_bridge.h"
#include "core/ui/overlay.h"

#include "core/gfx/frame_inspector.h"
#include "core/gfx/gfx_hud.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"

#include "../../../helper64/dlss_shared.h"
#include "../../../helper64/gpu_finish.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <windows.h>

#include <MinHook.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace bvr::ac::dlss_bridge {
namespace {

std::atomic<bool> g_on{false};       // D0 test switch: route eye pictures through the helper (copy)
std::atomic<int> g_mode{0};          // DLSS: 0 off, 1 DLAA, 2 ultra quality, 3 quality, 4 balanced, 5 performance
std::atomic<int> g_depthInv{0};      // NGX DepthInverted (test switch; which way Arkham City's depth runs)
std::atomic<int> g_preset{0};
std::atomic<bool> g_jitterOn{true};
// FINISHFIRST (DLSS_PLAN.md "Freeze analysis"): 3 = OVERLAP (default: both eyes in flight, every wait on the CPU,
// one wait per frame), 2 = both sides' work has run on the card before each hand-over, 1 = only the helper's, 0 = the
// old way (the card's contexts wait on each other mid-stream). Not saved.
std::atomic<int> g_sync{3};
bcdlss::GpuFinish g_finish; // render thread
std::atomic<uint32_t> g_finishTimeouts{0};
std::atomic<float> g_finishAvgMs{0.0f};
float g_finishSum = 0.0f;
HANDLE g_map = nullptr, g_req = nullptr, g_done = nullptr, g_job = nullptr, g_helper = nullptr;
bcdlss::Shared* g_sh = nullptr;
bool g_luidSet = false;
uint32_t g_serial = 0;

struct Set {
    ID3D11Texture2D* tex[bcdlss::kTexCount] = {};
    IDXGIKeyedMutex* km[bcdlss::kTexCount] = {};
    ID3D11UnorderedAccessView* uav[bcdlss::kTexCount] = {};
    UINT rw = 0, rh = 0, ow = 0, oh = 0;
    // camera motion: last frame's jitter-free view x lens of this eye
    double prevVP[16] = {};
    bool prevOk = false;
    bool reset = true;
};
Set g_set[2];

// Per pass (game thread writes at the FSceneView ctor, render thread reads at the eye's present).
constexpr uint32_t kSlots = 64;
struct Pass {
    std::atomic<uint32_t> seq{0};
    int eye;
    double vpJ[16]; // the view x lens the engine drew with (jittered)
    double vpU[16]; // the same without the jitter
    float jx, jy;   // jitter in render pixels
};
Pass g_pass[kSlots];
std::atomic<uint32_t> g_renderSlot{0};

// Counters (render thread writes, any thread reads).
std::atomic<uint32_t> g_trips{0}, g_fallbacks{0}, g_timeouts{0}, g_noDepth{0}, g_noPass{0};
std::atomic<uint32_t> g_failIn{0}, g_failBusy{0}, g_failStatus{0}, g_failOut{0};
std::atomic<float> g_waitAvgMs{0.0f}, g_helperAvgMs{0.0f};
float g_waitSum = 0.0f, g_helperSum = 0.0f;
uint32_t g_waitN = 0;

// --- camera motion + depth pass (game device) --------------------------------------------------------------
ID3D11ComputeShader* g_cs = nullptr;
ID3D11Buffer* g_cb = nullptr;
ID3DDeviceContextState* g_state = nullptr;
int g_csState = 0;
ID3D11Resource* g_depthRes = nullptr; // weak (cache key)
ID3D11ShaderResourceView* g_depthSrv = nullptr;

void mul4(const double a[16], const double b[16], double o[16]) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double v = 0;
            for (int k = 0; k < 4; ++k) v += a[i * 4 + k] * b[k * 4 + j];
            o[i * 4 + j] = v;
        }
}
bool inv4(const double m[16], double o[16]) {
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
float halton(int i, int b) {
    float f = 1, r = 0;
    while (i > 0) { f /= float(b); r += f * float(i % b); i /= b; }
    return r;
}

bool cs_build(ID3D11Device* dev) {
    if (g_csState) return g_csState == 1;
    g_csState = -1;
    // CAMMV (AKN D1m, JJ: "so much better"): every pixel's point from this eye's depth and camera, projected with
    // the same eye's camera of the previous frame = the camera motion, in render pixels, jitter removed. The same
    // pass copies the depth into the shared R32F texture for the helper.
    static const char cs[] =
        "Texture2D<float> depthT : register(t0); RWTexture2D<float> outDepth : register(u0); RWTexture2D<float2> outMv : register(u1);"
        "cbuffer C : register(b0) { float4 r0, r1, r2, r3; float4 sz; float4 jit; };"
        "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {"
        " if (id.x >= (uint)sz.x || id.y >= (uint)sz.y) return;"
        " float d = depthT.Load(int3(id.xy, 0));"
        " outDepth[id.xy] = d;"
        " float2 ndc = float2((id.x + 0.5) * sz.z * 2.0 - 1.0, 1.0 - (id.y + 0.5) * sz.w * 2.0);"
        " float4 p = ndc.x * r0 + ndc.y * r1 + d * r2 + r3;"
        " float2 prev = p.xy / p.w;"
        " float2 mvN = prev - (ndc - jit.xy);"
        " outMv[id.xy] = float2(mvN.x * sz.x * 0.5, -mvN.y * sz.y * 0.5); }";
    ID3DBlob* b = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(D3DCompile(cs, sizeof(cs) - 1, "bc_cammv", nullptr, nullptr, "main", "cs_5_0", 0, 0, &b, &err)) || !b) {
        BVR_LOG("[dlss] camera motion shader FAILED: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        if (err) err->Release();
        return false;
    }
    if (err) err->Release();
    const bool ok = SUCCEEDED(dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_cs));
    b->Release();
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 96;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (!ok || FAILED(dev->CreateBuffer(&bd, nullptr, &g_cb))) return false;
    ID3D11Device1* d1 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d1))) && d1) {
        // a private context state: the pass runs inside it, the game's state is swapped back untouched
        const D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
        d1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &g_state);
        d1->Release();
    }
    g_csState = 1;
    BVR_LOG("[dlss] camera motion + depth pass ready (private state %s)", g_state ? "yes" : "NO");
    return true;
}

ID3D11ShaderResourceView* depth_srv(ID3D11Device* dev, ID3D11Resource* r) {
    if (r == g_depthRes && g_depthSrv) return g_depthSrv;
    if (g_depthSrv) { g_depthSrv->Release(); g_depthSrv = nullptr; }
    g_depthRes = r;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(r, &sd, &g_depthSrv))) g_depthSrv = nullptr;
    return g_depthSrv;
}

// --- shared textures -------------------------------------------------------------------------------------
void release_set(Set& s) {
    for (int i = 0; i < bcdlss::kTexCount; ++i) {
        if (s.uav[i]) { s.uav[i]->Release(); s.uav[i] = nullptr; }
        if (s.km[i]) { s.km[i]->Release(); s.km[i] = nullptr; }
        if (s.tex[i]) { s.tex[i]->Release(); s.tex[i] = nullptr; }
    }
    s.rw = s.rh = s.ow = s.oh = 0;
    s.prevOk = false;
    s.reset = true;
}

bool make_shared(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT f, bool uav, ID3D11Texture2D** t, IDXGIKeyedMutex** km,
                 ID3D11UnorderedAccessView** uv, uint64_t* handle) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    const HRESULT hc = dev->CreateTexture2D(&d, nullptr, t);
    if (FAILED(hc) || !*t) {
        BVR_LOG("[dlss] CreateTexture2D %ux%u fmt %d shared+keyedmutex FAILED 0x%08X", w, h, int(f), unsigned(hc));
        return false;
    }
    (*t)->QueryInterface(IID_PPV_ARGS(km));
    IDXGIResource* r = nullptr;
    HANDLE hs = nullptr;
    if (SUCCEEDED((*t)->QueryInterface(IID_PPV_ARGS(&r))) && r) {
        r->GetSharedHandle(&hs);
        r->Release();
    }
    *handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(hs));
    if (uav && uv) dev->CreateUnorderedAccessView(*t, nullptr, uv);
    return *km && hs && (!uav || !uv || *uv);
}

uint32_t g_setFailures = 0;
bool ensure_set(ID3D11Device* dev, int eye, UINT rw, UINT rh, UINT ow, UINT oh) {
    Set& s = g_set[eye];
    if (g_setFailures >= 2) return false; // no retry every frame
    if (s.tex[bcdlss::kColor] && s.rw == rw && s.rh == rh && s.ow == ow && s.oh == oh) return true;
    release_set(s);
    uint64_t h[bcdlss::kTexCount] = {};
    const bool ok =
        make_shared(dev, rw, rh, DXGI_FORMAT_R8G8B8A8_UNORM, false, &s.tex[bcdlss::kColor], &s.km[bcdlss::kColor], nullptr, &h[0]) &&
        make_shared(dev, rw, rh, DXGI_FORMAT_R32_FLOAT, true, &s.tex[bcdlss::kDepth], &s.km[bcdlss::kDepth], &s.uav[bcdlss::kDepth], &h[1]) &&
        make_shared(dev, rw, rh, DXGI_FORMAT_R16G16_FLOAT, true, &s.tex[bcdlss::kMotion], &s.km[bcdlss::kMotion], &s.uav[bcdlss::kMotion], &h[2]) &&
        make_shared(dev, ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, true, &s.tex[bcdlss::kOut], &s.km[bcdlss::kOut], nullptr, &h[3]);
    if (!ok) {
        release_set(s);
        ++g_setFailures;
        BVR_LOG("[dlss] eye %d: shared textures %ux%u -> %ux%u FAILED", eye, rw, rh, ow, oh);
        return false;
    }
    s.rw = rw; s.rh = rh; s.ow = ow; s.oh = oh;
    for (int i = 0; i < bcdlss::kTexCount; ++i) g_sh->handle[eye][i] = h[i];
    g_sh->renderW = rw; g_sh->renderH = rh; g_sh->outW = ow; g_sh->outH = oh;
    g_sh->gen = g_sh->gen + 1;
    BVR_LOG("[dlss] eye %d: shared textures %ux%u -> %ux%u (gen %u)", eye, rw, rh, ow, oh, g_sh->gen);
    return true;
}

void set_luid(ID3D11Device* dev) {
    if (g_luidSet) return;
    IDXGIDevice* dd = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dd))) && dd) {
        IDXGIAdapter* a = nullptr;
        if (SUCCEEDED(dd->GetAdapter(&a)) && a) {
            DXGI_ADAPTER_DESC d{};
            a->GetDesc(&d);
            g_sh->luidLow = d.AdapterLuid.LowPart;
            g_sh->luidHigh = d.AdapterLuid.HighPart;
            a->Release();
        }
        dd->Release();
    }
    g_luidSet = true;
}

bool g_startTried = false;
bool start_helper() {
    if (g_startTried) return false;
    g_startTried = true;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (!slash) return false;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - exe), L"BatmanCityDLSS\\bc_dlss64.exe");
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
        BVR_LOG("[dlss] helper not found: %ls", exe);
        return false;
    }
    wchar_t dir[MAX_PATH];
    wcscpy_s(dir, exe);
    *wcsrchr(dir, L'\\') = 0;
    // The helper ends with the game: a job object that kills its members when the game's handle closes.
    g_job = CreateJobObjectW(nullptr, nullptr);
    if (g_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    }
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, nullptr, nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, dir, &si, &pi)) {
        BVR_LOG("[dlss] helper start FAILED (%lu)", GetLastError());
        return false;
    }
    if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    g_helper = pi.hProcess;
    BVR_LOG("[dlss] helper started (pid %lu)", pi.dwProcessId);
    return true;
}

// Depth copy + camera motion for this eye, into the shared textures (which the game holds, key 0).
bool motion_pass(ID3D11DeviceContext* ctx, ID3D11Device* dev, Set& s, const Pass& p) {
    ID3D11Resource* depth = bvr::gfx_hud::scene_depth();
    if (!depth || !cs_build(dev)) { g_noDepth.fetch_add(1, std::memory_order_relaxed); return false; }
    ID3D11ShaderResourceView* dv = depth_srv(dev, depth);
    if (!dv || !s.uav[bcdlss::kDepth] || !s.uav[bcdlss::kMotion]) { g_noDepth.fetch_add(1, std::memory_order_relaxed); return false; }
    double inv[16], M[16];
    if (!inv4(p.vpJ, inv)) return false;
    if (s.prevOk) mul4(inv, s.prevVP, M);
    else mul4(inv, p.vpU, M); // first frame: no motion
    float cb[24];
    for (int i = 0; i < 16; ++i) cb[i] = float(M[i]);
    cb[16] = float(s.rw); cb[17] = float(s.rh); cb[18] = 1.0f / float(s.rw); cb[19] = 1.0f / float(s.rh);
    cb[20] = 2.0f * p.jx / float(s.rw); cb[21] = -2.0f * p.jy / float(s.rh); cb[22] = 0; cb[23] = 0;
    ID3D11DeviceContext1* c1 = nullptr;
    ID3DDeviceContextState* prev = nullptr;
    if (g_state && SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&c1))) && c1) c1->SwapDeviceContextState(g_state, &prev);
    // No private state (the game's device refused one): keep the game's compute bindings by hand.
    ID3D11ComputeShader* oCs = nullptr;
    ID3D11ShaderResourceView* oSrv = nullptr;
    ID3D11UnorderedAccessView* oUav[2] = {};
    ID3D11Buffer* oCb = nullptr;
    if (!prev) {
        ctx->CSGetShader(&oCs, nullptr, nullptr);
        ctx->CSGetShaderResources(0, 1, &oSrv);
        ctx->CSGetUnorderedAccessViews(0, 2, oUav);
        ctx->CSGetConstantBuffers(0, 1, &oCb);
    }
    ctx->UpdateSubresource(g_cb, 0, nullptr, cb, 0, 0);
    ID3D11UnorderedAccessView* uv[2] = {s.uav[bcdlss::kDepth], s.uav[bcdlss::kMotion]};
    ctx->CSSetShader(g_cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &dv);
    ctx->CSSetUnorderedAccessViews(0, 2, uv, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &g_cb);
    ctx->Dispatch((s.rw + 7) / 8, (s.rh + 7) / 8, 1);
    ID3D11ShaderResourceView* nsv = nullptr;
    ID3D11UnorderedAccessView* nuv[2] = {};
    ctx->CSSetShaderResources(0, 1, &nsv);
    ctx->CSSetUnorderedAccessViews(0, 2, nuv, nullptr);
    if (prev) {
        c1->SwapDeviceContextState(prev, nullptr);
        prev->Release();
    } else {
        ctx->CSSetShader(oCs, nullptr, 0);
        ctx->CSSetShaderResources(0, 1, &oSrv);
        ctx->CSSetUnorderedAccessViews(0, 2, oUav, nullptr);
        ctx->CSSetConstantBuffers(0, 1, &oCb);
        if (oCs) oCs->Release();
        if (oSrv) oSrv->Release();
        for (auto* u : oUav)
            if (u) u->Release();
        if (oCb) oCb->Release();
    }
    if (c1) c1->Release();
    memcpy(s.prevVP, p.vpU, sizeof(s.prevVP));
    s.prevOk = true;
    return true;
}

// --- OVERLAP (sync 3) ---------------------------------------------------------------------------------------
// The left eye is handed over at its present without waiting; the core keeps its headset image open (defer hook).
// The right eye is handed over the same way, then ONE CPU wait for the helper's fenceOut covers both eyes and both
// outputs are copied out (right here, left in the finish hook). Neither context ever waits on the other on the card.
ID3D11Fence* g_fenceIn = nullptr;   // we signal: this eye's copy + motion pass have run
ID3D11Fence* g_fenceOut = nullptr;  // the helper signals: its DLSS for that serial has been submitted after release
ID3D11DeviceContext4* g_c4 = nullptr;
HANDLE g_fenceEv = nullptr;
int g_ovState = 0;                  // 0 not tried, 1 ready, -1 unavailable / switched off after failures
uint32_t g_ovSerial = 0;
uint32_t g_ovFailRun = 0;           // consecutive failures after the first success
struct HeldLeft { bool valid = false; uint32_t serial = 0; };
HeldLeft g_heldL;                   // render thread
int mode_for_ratio(float r);        // below, with the quality steps

// The one-at-a-time request's answer. A loop, not one wait: the helper sets the shared "done" event for overlap
// answers too, so a single wakeup may belong to someone else.
bool wait_legacy(DWORD ms) {
    const ULONGLONG deadline = GetTickCount64() + ms;
    while (g_sh->answered != g_serial) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return false;
        WaitForSingleObject(g_done, DWORD(deadline - now));
    }
    return true;
}
std::atomic<uint32_t> g_ovTrips{0}, g_ovFallbacks{0}, g_ovTimeouts{0}, g_ovLeftLost{0};
std::atomic<float> g_ovWaitAvgMs{0.0f};
float g_ovWaitSum = 0.0f;
uint32_t g_ovWaitN = 0;

bool ov_ready(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (g_ovState) return g_ovState > 0;
    g_ovState = -1;
    if (!g_helper || !g_sh) return false;
    ID3D11Device5* d5 = nullptr;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&d5))) || !d5) {
        BVR_LOG("[dlss] overlap unavailable: no ID3D11Device5 (shared fences) - safe hand-off instead");
        return false;
    }
    ID3D11Fence* f[2] = {};
    HANDLE h[2] = {}, dup[2] = {};
    bool ok = true;
    for (int i = 0; i < 2 && ok; ++i)
        ok = SUCCEEDED(d5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&f[i]))) && f[i] &&
             SUCCEEDED(f[i]->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h[i])) && h[i];
    d5->Release();
    for (int i = 0; i < 2 && ok; ++i)
        ok = DuplicateHandle(GetCurrentProcess(), h[i], g_helper, &dup[i], 0, FALSE, DUPLICATE_SAME_ACCESS) != FALSE;
    for (HANDLE x : h)
        if (x) CloseHandle(x);
    ok = ok && SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&g_c4))) && g_c4;
    if (ok) g_fenceEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ok || !g_fenceEv) {
        for (ID3D11Fence* x : f)
            if (x) x->Release();
        if (g_c4) { g_c4->Release(); g_c4 = nullptr; }
        BVR_LOG("[dlss] overlap unavailable: shared fences could not be made - safe hand-off instead");
        return false;
    }
    g_fenceIn = f[0];
    g_fenceOut = f[1];
    g_sh->fenceInHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(dup[0]));
    g_sh->fenceOutHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(dup[1]));
    MemoryBarrier();
    g_ovState = 1;
    BVR_LOG("[dlss] overlap ready: both eyes in flight, waits on the CPU only (shared fences)");
    return true;
}

// CPU wait until the helper has passed `serial` (its DLSS for it is queued behind its release).
bool wait_out(uint32_t serial, DWORD ms) {
    const ULONGLONG deadline = GetTickCount64() + ms;
    while (g_fenceOut->GetCompletedValue() < serial) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline || FAILED(g_fenceOut->SetEventOnCompletion(serial, g_fenceEv))) return false;
        WaitForSingleObject(g_fenceEv, DWORD(deadline - now));
    }
    return true;
}

void ov_failed(const char* what) {
    g_ovFallbacks.fetch_add(1, std::memory_order_relaxed);
    if (g_ovFallbacks.load() <= 5) BVR_LOG("[dlss] overlap: %s", what);
    if (g_ovTrips.load(std::memory_order_relaxed) > 0 && ++g_ovFailRun >= 30) {
        g_ovState = -1;
        BVR_LOG("[dlss] overlap switched OFF after 30 failures in a row - safe hand-off instead");
    }
}

// Copies this eye in and hands it to the helper without waiting. Returns its serial (0 = nothing sent).
uint32_t ov_send(ID3D11DeviceContext* ctx, ID3D11Device* dev, int eye, ID3D11Texture2D* dst, ID3D11Texture2D* backbuffer) {
    D3D11_TEXTURE2D_DESC bd{}, od{};
    backbuffer->GetDesc(&bd);
    dst->GetDesc(&od);
    if (!ensure_set(dev, eye, bd.Width, bd.Height, od.Width, od.Height)) return 0;
    Set& s = g_set[eye];
    const uint32_t slot = g_renderSlot.load(std::memory_order_relaxed);
    const Pass& p = g_pass[slot % kSlots];
    if (p.seq.load(std::memory_order_acquire) != slot + 1) { g_noPass.fetch_add(1, std::memory_order_relaxed); return 0; }
    bcdlss::Shared::EyeReq& rq = g_sh->req[eye];
    // This eye's previous request (normally long done), and any one-at-a-time request, must be finished.
    if (rq.serial && !wait_out(rq.serial, 100)) { g_ovTimeouts.fetch_add(1, std::memory_order_relaxed); return 0; }
    if (g_serial && !wait_legacy(100)) {
        g_failBusy.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    int got = 0;
    while (got < bcdlss::kTexCount && s.km[got]->AcquireSync(0, 100) == S_OK) ++got;
    if (got < bcdlss::kTexCount) {
        for (int i = 0; i < got; ++i) s.km[i]->ReleaseSync(0);
        g_failIn.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    ctx->CopyResource(s.tex[bcdlss::kColor], backbuffer);
    const bool mv = motion_pass(ctx, dev, s, p);
    for (int i = 0; i < bcdlss::kTexCount; ++i) s.km[i]->ReleaseSync(1); // hand everything to the helper
    const bool sameSize = bd.Width == od.Width && bd.Height == od.Height;
    g_sh->depthInverted = g_depthInv.load(std::memory_order_relaxed);
    g_sh->preset = g_preset.load(std::memory_order_relaxed);
    g_sh->mvScaleX = 1.0f; // our motion is in render pixels
    g_sh->mvScaleY = 1.0f;
    g_sh->syncMode = 3;
    rq.jitterX = mv ? p.jx : 0.0f;
    rq.jitterY = mv ? p.jy : 0.0f;
    rq.reset = s.reset ? 1 : 0;
    s.reset = false;
    rq.dlssMode = !mv ? 0 : sameSize ? 1 : mode_for_ratio(float(bd.Width) / float(od.Width));
    rq.status = 0;
    const uint32_t serial = ++g_ovSerial;
    g_c4->Signal(g_fenceIn, serial); // after the copy, the motion pass and the release
    ctx->Flush();
    MemoryBarrier();
    rq.serial = serial; // last: the helper reads the slot once it sees a new serial
    SetEvent(g_req);
    return serial;
}

// Waits (CPU) for this eye's answer and copies its output into the headset image.
bool ov_receive(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* dst, uint32_t serial) {
    if (!wait_out(serial, 100)) {
        g_ovTimeouts.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const bcdlss::Shared::EyeReq& rq = g_sh->req[eye];
    if (rq.answered != serial || rq.status != 0) {
        g_failStatus.fetch_add(1, std::memory_order_relaxed);
        if (g_failStatus.load() <= 3) BVR_LOG("[dlss] overlap eye %d: helper status %d", eye, rq.status);
        return false;
    }
    Set& s = g_set[eye];
    if (s.km[bcdlss::kOut]->AcquireSync(0, 100) != S_OK) { // already released by the helper: no card-side wait
        g_failOut.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    ctx->CopyResource(dst, s.tex[bcdlss::kOut]);
    s.km[bcdlss::kOut]->ReleaseSync(0);
    return true;
}

// Defer hook (core, the left eye of a held pair): true = taken, delivered in on_finish.
bool on_defer(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, ID3D11Texture2D* backbuffer) {
    g_heldL.valid = false;
    if (g_mode.load(std::memory_order_relaxed) <= 0 || g_sync.load(std::memory_order_relaxed) < 3 || !g_sh ||
        !g_sh->helperReady || g_sh->ngxState < 0 || g_ovState < 0)
        return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    const uint32_t serial = ov_ready(dev, ctx) ? ov_send(ctx, dev, 0, dst, backbuffer) : 0;
    dev->Release();
    if (!serial) return false; // the normal one-eye hand-off takes the left eye now
    g_heldL.valid = true;
    g_heldL.serial = serial;
    return true;
}

// The right eye of an overlapped pair: send, then one wait covers both eyes. false = not handled (normal path).
bool ov_right(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, ID3D11Texture2D* backbuffer) {
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    const uint32_t serial = ov_send(ctx, dev, 1, dst, backbuffer);
    dev->Release();
    if (!serial) { ov_failed("right eye not sent"); return false; }
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    const bool ok = ov_receive(ctx, 1, dst, serial);
    QueryPerformanceCounter(&t1);
    if (!ok) { ov_failed("right eye answer missing"); return false; }
    g_ovFailRun = 0;
    g_ovTrips.fetch_add(1, std::memory_order_relaxed);
    g_ovWaitSum += static_cast<float>((t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    if (++g_ovWaitN == 200) {
        uint32_t done = 0, early = 0;
        bvr::vr::held_left_counts(&done, &early);
        g_ovWaitAvgMs.store(g_ovWaitSum / 200.0f);
        BVR_LOG("[dlss] overlap: one wait per frame %.3f ms (both eyes), DLSS on the card %.2f ms per eye, pairs %u, "
                "fallbacks %u, timeouts %u, left lost %u | held left images %u (%u released early) | helper waits for "
                "our copies that ran out %u",
                g_ovWaitSum / 200.0f, g_sh->gpuMs, g_ovTrips.load(), g_ovFallbacks.load(), g_ovTimeouts.load(),
                g_ovLeftLost.load(), done, early, g_sh->fenceWaitTimeouts);
        g_ovWaitSum = 0.0f;
        g_ovWaitN = 0;
    }
    return true;
}

// Finish hook (core): writes the held left image - its DLSS finished with (or before) the right eye's.
void on_finish(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst) {
    if (!g_heldL.valid) return;
    g_heldL.valid = false;
    if (!ov_receive(ctx, 0, dst, g_heldL.serial)) {
        g_ovLeftLost.fetch_add(1, std::memory_order_relaxed); // the left image keeps its old picture this once
        ov_failed("left eye answer missing");
    }
}

// DLSS quality steps and their render scale (NVIDIA's ratios).
const float kScale[6] = {1.0f, 1.0f, 0.77f, 0.667f, 0.58f, 0.5f};
int mode_for_ratio(float r) { return r > 0.95f ? 1 : r > 0.72f ? 2 : r > 0.62f ? 3 : r > 0.54f ? 4 : 5; }
std::atomic<uint32_t> g_outW{0}, g_outH{0}, g_launchMode{0};

// MIPBIAS (AKN port, akvr_native/NOTES.md "MIPBIAS"; Luma core.hpp CreateCustomSampler mode 4): drawn smaller, the
// world's textures are picked for the small picture (one mip level blurrier at Performance) and DLSS does not bring
// that detail back. Surface samplers (anisotropic, or linear-mip ones that tile) are swapped at bind time for twins
// with MipLODBias += log2(render / output) - 1 (mode 1, default) or log2(render / output) (mode 2, milder); anisotropic
// ones also get 16x. Screen passes clamp and keep their sampler. Only while the launch drew smaller (mode >= 2).
std::atomic<int> g_mipBias{1};
std::atomic<uint32_t> g_mipTwinsMade{0}, g_mipSwaps{0};
struct MipTwin { ID3D11SamplerState* t[3]; bool surface; };
std::unordered_map<ID3D11SamplerState*, MipTwin> g_mipTwins; // render thread only

ID3D11SamplerState* mip_twin(ID3D11SamplerState* s, int m) {
    auto it = g_mipTwins.find(s);
    if (it != g_mipTwins.end()) return it->second.surface ? it->second.t[m] : s;
    D3D11_SAMPLER_DESC d{};
    s->GetDesc(&d);
    const bool aniso = d.Filter == D3D11_FILTER_ANISOTROPIC || d.Filter == D3D11_FILTER_COMPARISON_ANISOTROPIC;
    const bool tiles = d.AddressU == D3D11_TEXTURE_ADDRESS_WRAP || d.AddressU == D3D11_TEXTURE_ADDRESS_MIRROR ||
                       d.AddressV == D3D11_TEXTURE_ADDRESS_WRAP || d.AddressV == D3D11_TEXTURE_ADDRESS_MIRROR;
    const bool linMip = d.Filter == D3D11_FILTER_MIN_MAG_MIP_LINEAR || d.Filter == D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR ||
                        d.Filter == D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT || d.Filter == D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    MipTwin tw{{nullptr, nullptr, nullptr}, aniso || (linMip && tiles)};
    if (tw.surface) {
        const float base = log2f(kScale[std::clamp(int(g_launchMode.load()), 0, 5)]);
        ID3D11Device* dev = nullptr;
        s->GetDevice(&dev);
        for (int k = 1; k <= 2 && dev; ++k) {
            D3D11_SAMPLER_DESC b = d;
            b.MipLODBias = std::clamp(d.MipLODBias + base - (k == 1 ? 1.0f : 0.0f), D3D11_MIP_LOD_BIAS_MIN, D3D11_MIP_LOD_BIAS_MAX);
            if (aniso) b.MaxAnisotropy = D3D11_REQ_MAXANISOTROPY;
            b.MinLOD = (std::max)(d.MinLOD + (std::min)(b.MipLODBias - d.MipLODBias, 0.0f), 0.0f);
            if (FAILED(dev->CreateSamplerState(&b, &tw.t[k]))) tw.t[k] = nullptr;
        }
        if (dev) dev->Release();
        // Our own state save / restore (blit, HUD) re-binds twins: they must map to themselves, never get a twin.
        for (int k = 1; k <= 2; ++k)
            if (tw.t[k]) g_mipTwins.emplace(tw.t[k], MipTwin{{nullptr, nullptr, nullptr}, false});
        if (!tw.t[1] || !tw.t[2]) {
            for (int k = 1; k <= 2; ++k) if (tw.t[k]) tw.t[k]->Release();
            tw.surface = false;
        } else if (g_mipTwinsMade.fetch_add(1) < 16) {
            BVR_LOG("[dlss] sharper textures: sampler filter 0x%x aniso %u address %d/%d bias %.2f -> %.2f / %.2f",
                    unsigned(d.Filter), d.MaxAnisotropy, int(d.AddressU), int(d.AddressV), d.MipLODBias,
                    d.MipLODBias + base - 1.0f, d.MipLODBias + base);
        }
    }
    g_mipTwins.emplace(s, tw);
    return tw.surface ? tw.t[m] : s;
}

bool sampler_tap(ID3D11SamplerState** v, unsigned n) {
    const int m = g_mipBias.load(std::memory_order_relaxed);
    if (m <= 0 || m > 2 || g_launchMode.load(std::memory_order_relaxed) < 2 || g_mode.load(std::memory_order_relaxed) <= 0)
        return false;
    bool any = false;
    for (unsigned i = 0; i < n; ++i) {
        if (!v[i]) continue;
        ID3D11SamplerState* t = mip_twin(v[i], m);
        any |= t != v[i];
        v[i] = t;
    }
    if (any) g_mipSwaps.fetch_add(1, std::memory_order_relaxed);
    return any;
}

// The eye copy, through the helper (copy in D0, DLSS from D1). Render thread.
bool on_eye(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* dst, ID3D11Texture2D* backbuffer) {
    // OVERLAP: this pair's left eye is already with the helper - send the right one and collect both.
    if (eye == 1 && g_heldL.valid && ov_right(ctx, dst, backbuffer)) return true;
    const int mode = g_mode.load(std::memory_order_relaxed);
    if ((!g_on.load(std::memory_order_relaxed) && mode <= 0) || !g_sh || eye < 0 || eye > 1) return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    set_luid(dev);
    if (!g_helper) start_helper(); // after the adapter is known, so the helper uses the same graphics card
    if (!g_sh->helperReady) { dev->Release(); return false; }
    if (g_sh->ngxState < 0 && mode > 0) { // no DLSS on this PC: off (saved, so the next launch draws full size)
        BVR_LOG("[dlss] NVIDIA's DLSS could not start - DLSS off");
        set_mode(0);
    }
    D3D11_TEXTURE2D_DESC bd{}, od{};
    backbuffer->GetDesc(&bd);
    dst->GetDesc(&od);
    bool ok = ensure_set(dev, eye, bd.Width, bd.Height, od.Width, od.Height);
    Set& s = g_set[eye];
    // DLSS needs this pass's cameras (render thread: the pass whose present this is).
    const uint32_t slot = g_renderSlot.load(std::memory_order_relaxed);
    const Pass& p = g_pass[slot % kSlots];
    const bool haveCam = p.seq.load(std::memory_order_acquire) == slot + 1;
    bool dlss = ok && mode > 0;
    if (dlss && !haveCam) { g_noPass.fetch_add(1, std::memory_order_relaxed); dlss = false; }
    const bool sameSize = bd.Width == od.Width && bd.Height == od.Height;
    if (!dlss && !sameSize) { // the helper's plain copy cannot resize: the core stretches instead
        dev->Release();
        g_fallbacks.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    bool good = false;
    // One request at a time: a request still in the helper's hands would be overwritten.
    if (ok && g_serial && !wait_legacy(100)) {
        g_failBusy.fetch_add(1, std::memory_order_relaxed);
        ok = false;
    }
    if (ok) {
        IDXGIKeyedMutex** km = s.km;
        bool held = true;
        int got = 0;
        for (int i = 0; i < bcdlss::kTexCount && held; ++i) {
            held = km[i]->AcquireSync(0, 100) == S_OK;
            if (held) ++got;
        }
        if (!held) {
            for (int i = 0; i < got; ++i) km[i]->ReleaseSync(0);
            g_failIn.fetch_add(1, std::memory_order_relaxed);
        }
        if (held) {
            ctx->CopyResource(s.tex[bcdlss::kColor], backbuffer);
            if (dlss) dlss = motion_pass(ctx, dev, s, p);
            for (int i = 0; i < bcdlss::kTexCount; ++i) km[i]->ReleaseSync(1); // hand everything to the helper
            // FINISHFIRST: the copy, the motion pass and the release have run on the card before the helper
            // starts, so its wait on our pictures is already satisfied.
            const int sync = (std::min)(g_sync.load(std::memory_order_relaxed), 2); // overlap's fallback = safe
            if (sync >= 2) {
                LARGE_INTEGER ta, tb;
                QueryPerformanceCounter(&ta);
                if (!g_finish.wait(dev, ctx, 100)) g_finishTimeouts.fetch_add(1, std::memory_order_relaxed);
                QueryPerformanceCounter(&tb);
                g_finishSum += static_cast<float>((tb.QuadPart - ta.QuadPart) * 1000.0 / f.QuadPart);
                static bool logged = false;
                if (!logged) { logged = true; BVR_LOG("[dlss] finish-first hand-off (sync %d) with a %s", sync, g_finish.kind()); }
            }
            g_sh->syncMode = sync;
            g_sh->eye = eye;
            g_sh->mode = dlss ? 1 : 0;
            // The picture sizes decide the scale; the quality name only has to agree with it.
            g_sh->dlssMode = !dlss ? 0 : sameSize ? 1 : mode_for_ratio(float(bd.Width) / float(od.Width));
            g_sh->depthInverted = g_depthInv.load(std::memory_order_relaxed);
            g_sh->preset = g_preset.load(std::memory_order_relaxed);
            g_sh->jitterX = dlss ? p.jx : 0.0f;
            g_sh->jitterY = dlss ? p.jy : 0.0f;
            g_sh->mvScaleX = 1.0f; // our motion is in render pixels
            g_sh->mvScaleY = 1.0f;
            g_sh->reset = s.reset ? 1 : 0;
            s.reset = false;
            g_sh->serial = ++g_serial;
            SetEvent(g_req);
            const bool answered = wait_legacy(100);
            if (answered && g_sh->status == 0) {
                if (km[bcdlss::kOut]->AcquireSync(0, 100) == S_OK) {
                    ctx->CopyResource(dst, s.tex[bcdlss::kOut]);
                    km[bcdlss::kOut]->ReleaseSync(0);
                    good = true;
                } else {
                    g_failOut.fetch_add(1, std::memory_order_relaxed);
                }
            } else if (answered) {
                g_failStatus.fetch_add(1, std::memory_order_relaxed);
                if (g_failStatus.load() <= 3) BVR_LOG("[dlss] eye %d: helper status %d", eye, g_sh->status);
            } else {
                g_timeouts.fetch_add(1, std::memory_order_relaxed);
                if (g_timeouts.load() <= 3)
                    BVR_LOG("[dlss] eye %d: helper did not answer in time (status %d)", eye, g_sh->status);
            }
        }
    }
    dev->Release();
    QueryPerformanceCounter(&t1);
    if (!good) {
        g_fallbacks.fetch_add(1, std::memory_order_relaxed);
        if (!sameSize) return false; // the core stretches
        ctx->CopyResource(dst, backbuffer);
        return true;
    }
    g_trips.fetch_add(1, std::memory_order_relaxed);
    g_waitSum += static_cast<float>((t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    g_helperSum += g_sh->helperMs;
    if (++g_waitN == 200) {
        g_waitAvgMs.store(g_waitSum / 200.0f);
        g_helperAvgMs.store(g_helperSum / 200.0f);
        g_finishAvgMs.store(g_finishSum / 200.0f);
        BVR_LOG("[dlss] hand-off: %.3f ms per eye on the render thread (helper %.3f ms, DLSS on the card %.2f ms), mode %d, "
                "trips %u, fallbacks %u, no depth %u, no camera %u | sync %d: game finish wait %.3f ms (%u timeouts), "
                "helper finish wait %.3f ms (%u timeouts)",
                g_waitSum / 200.0f, g_helperSum / 200.0f, g_sh->gpuMs, mode, g_trips.load(), g_fallbacks.load(),
                g_noDepth.load(), g_noPass.load(), g_sync.load(), g_finishSum / 200.0f, g_finishTimeouts.load(),
                g_sh->finishMs, g_sh->finishTimeouts);
        g_waitSum = g_helperSum = g_finishSum = 0.0f;
        g_waitN = 0;
    }
    return true;
}

// DXGI11 (BioShock VR / AKVR lesson, memory steamvr-dxgi11-shared-textures): the game makes its device through the
// DXGI 1.0 CreateDXGIFactory, and such a device refuses shared keyed-mutex textures (E_INVALIDARG, 2026-10-08; a fresh
// 32-bit device on the same card accepts them). Answer the game's CreateDXGIFactory with CreateDXGIFactory1 - an
// IDXGIFactory1 is an IDXGIFactory. No geo-11 here, so AKVR's clash does not apply.
using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
CreateFactoryFn g_createFactoryOrig = nullptr, g_createFactory1 = nullptr;
std::atomic<uint32_t> g_dxgi11Calls{0};
HRESULT WINAPI create_factory_detour(REFIID riid, void** pp) {
    g_dxgi11Calls.fetch_add(1, std::memory_order_relaxed);
    if (g_createFactory1) return g_createFactory1(riid, pp);
    return g_createFactoryOrig(riid, pp);
}

} // namespace

void install_early() {
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) return;
    void* f0 = reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory"));
    g_createFactory1 = reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    if (!f0 || !g_createFactory1) return;
    const MH_STATUS a = MH_CreateHook(f0, reinterpret_cast<void*>(&create_factory_detour),
                                      reinterpret_cast<void**>(&g_createFactoryOrig));
    const MH_STATUS b = a == MH_OK ? MH_EnableHook(f0) : a;
    BVR_LOG("[dlss] game's CreateDXGIFactory answered with CreateDXGIFactory1 (shared textures): %s", MH_StatusToString(b));
}

void install() {
    g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(bcdlss::Shared), bcdlss::kMapName);
    if (!g_map) { BVR_LOG("[dlss] shared block FAILED (%lu)", GetLastError()); return; }
    g_sh = static_cast<bcdlss::Shared*>(MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(bcdlss::Shared)));
    if (!g_sh) return;
    memset(g_sh, 0, sizeof(*g_sh));
    g_sh->version = bcdlss::kVersion;
    g_sh->gamePid = GetCurrentProcessId();
    g_req = CreateEventW(nullptr, FALSE, FALSE, bcdlss::kReqName);
    g_done = CreateEventW(nullptr, FALSE, FALSE, bcdlss::kDoneName);
    bvr::vr::set_eye_capture_hook(&on_eye);
    bvr::vr::set_eye_defer_hooks(&on_defer, &on_finish);
    bvr::frame_inspector::set_sampler_tap(&sampler_tap);
}

void set_enabled(bool on) {
    g_on.store(on);
    BVR_LOG("[dlss] eye pictures through the helper: %s", on ? "ON" : "off");
}
bool enabled() { return g_on.load(); }

int mode() { return g_mode.load(); }

bool available() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (!slash) return false;
    bool files = true;
    for (const wchar_t* f : {L"BatmanCityDLSS\\bc_dlss64.exe", L"BatmanCityDLSS\\nvngx_dlss.dll"}) {
        wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - exe), f);
        files = files && GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES;
    }
    bool nvidia = false;
    IDXGIFactory1* fac = nullptr;
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    auto mk = dxgi ? reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    if (mk && SUCCEEDED(mk(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&fac))) && fac) {
        IDXGIAdapter1* ad = nullptr;
        if (SUCCEEDED(fac->EnumAdapters1(0, &ad)) && ad) {
            DXGI_ADAPTER_DESC1 d{};
            nvidia = SUCCEEDED(ad->GetDesc1(&d)) && d.VendorId == 0x10DE;
            ad->Release();
        }
        fac->Release();
    }
    BVR_LOG("[dlss] available: helper files %s, NVIDIA graphics card %s", files ? "yes" : "NO", nvidia ? "yes" : "NO");
    return files && nvidia;
}
float render_scale(int m) { return kScale[std::clamp(m, 0, 5)]; }
std::atomic<bool> g_launched{false};
std::atomic<int> g_next{0}; // the saved choice (the next launch's render size)
bool launched() { return g_launched.load(std::memory_order_relaxed); }
int saved_mode() { return g_next.load(); }
void set_saved_mode(int m) { g_next.store(std::clamp(m, 0, 5)); }
void set_launch(int m, unsigned outW, unsigned outH) {
    g_launched.store(true);
    g_next.store(std::clamp(m, 0, 5));
    g_launchMode.store(m);
    g_outW.store(outW);
    g_outH.store(outH);
    g_mode.store(std::clamp(m, 0, 5));
}
int launch_mode() { return g_launchMode.load(); }
void set_mode(int m) {
    m = m < 0 ? 0 : (m > 5 ? 5 : m);
    g_next.store(m);
    if (m != g_mode.exchange(m)) {
        for (Set& s : g_set) { s.reset = true; s.prevOk = false; }
        BVR_LOG("[dlss] mode %d (0 off, 1 DLAA, 2 ultra quality, 3 quality, 4 balanced, 5 performance)", m);
    }
}

bool jitter(uint32_t frame, int renderW, int renderH, float* jx, float* jy) {
    *jx = *jy = 0.0f;
    if (g_mode.load(std::memory_order_relaxed) <= 0 || !g_jitterOn.load(std::memory_order_relaxed) || renderW <= 0 ||
        !launched())
        return false;
    // Halton(2,3) in render pixels, -0.5..0.5; NVIDIA's guide: 8 x (out/in)^2 phases.
    const uint32_t ow = g_outW.load(std::memory_order_relaxed);
    const float k = ow ? float(ow) / float(renderW) : 1.0f;
    const uint32_t phases = std::clamp(uint32_t(8.0f * k * k + 0.5f), 8u, 64u);
    const int i = int(frame % phases) + 1;
    *jx = halton(i, 2) - 0.5f;
    *jy = halton(i, 3) - 0.5f;
    return true;
}

void set_pass(uint32_t slot, int eye, const float vpJ[16], const float eyeView[16], float jx, float jy, int renderW,
              int renderH) {
    Pass& p = g_pass[slot % kSlots];
    p.seq.store(0, std::memory_order_relaxed);
    p.eye = eye;
    p.jx = jx;
    p.jy = jy;
    // The jitter went into the lens as m20 += 2 jx / w, m21 += -2 jy / h (row vectors), so the jittered view x lens
    // differs from the plain one by V[i][2] * those offsets in columns 0 and 1.
    const double dx = renderW > 0 ? 2.0 * jx / renderW : 0.0, dy = renderH > 0 ? -2.0 * jy / renderH : 0.0;
    for (int i = 0; i < 16; ++i) { p.vpJ[i] = vpJ[i]; p.vpU[i] = vpJ[i]; }
    for (int i = 0; i < 4; ++i) {
        p.vpU[i * 4 + 0] -= eyeView[i * 4 + 2] * dx;
        p.vpU[i * 4 + 1] -= eyeView[i * 4 + 2] * dy;
    }
    p.seq.store(slot + 1, std::memory_order_release);
}

void set_render_slot(uint32_t slot) { g_renderSlot.store(slot, std::memory_order_relaxed); }

bool command(const char* args) {
    // "dlss mode N" | "dlss depthinv 0|1" | "dlss preset N" | "dlss jitter 0|1" | "dlss reset"
    if (!strncmp(args, "mode ", 5)) set_mode(atoi(args + 5));
    else if (!strncmp(args, "depthinv ", 9)) { g_depthInv.store(atoi(args + 9)); BVR_LOG("[dlss] depth inverted %d", g_depthInv.load()); }
    else if (!strncmp(args, "preset ", 7)) { g_preset.store(atoi(args + 7)); BVR_LOG("[dlss] preset %d", g_preset.load()); }
    else if (!strncmp(args, "jitter ", 7)) { g_jitterOn.store(atoi(args + 7) != 0); BVR_LOG("[dlss] jitter %d", g_jitterOn.load() ? 1 : 0); }
    else if (!strncmp(args, "reset", 5)) { for (Set& s : g_set) s.reset = true; }
    else if (!strncmp(args, "mipbias ", 8)) { g_mipBias.store(atoi(args + 8)); BVR_LOG("[dlss] sharper textures %d (0 off, 1 full, 2 mild), %u twins, %u swaps", g_mipBias.load(), g_mipTwinsMade.load(), g_mipSwaps.load()); }
    else if (!strncmp(args, "status", 6)) { char s[1024]; status_line(s, sizeof(s)); BVR_LOG("[dlss] status: %s", s); }
    else if (!strncmp(args, "sync ", 5)) { g_sync.store(std::clamp(atoi(args + 5), 0, 3)); BVR_LOG("[dlss] hand-off sync %d (3 overlap, 2 both finish first, 1 helper finishes first, 0 old: card-side waits)", g_sync.load()); }
    else if (!strncmp(args, "repeat ", 7) && g_sh) { g_sh->evalRepeat = atoi(args + 7); BVR_LOG("[dlss] extra evaluates %d (test)", g_sh->evalRepeat); }
    else return false;
    return true;
}

void draw_ui() {
    static const char* const kNames[6] = {"off", "DLAA", "ultra quality", "quality", "balanced", "performance"};
    const int lm = g_launchMode.load(), live = g_mode.load(), next = g_next.load();
    if (!launched()) {
        ImGui::TextDisabled("DLSS works in VR launches only");
        return;
    }
    ImGui::TextWrapped("NVIDIA DLSS smooths the edges: the game draws a smaller picture and DLSS rebuilds it at full "
                       "headset sharpness. Performance is the fastest and is the recommended setting here.");
    ImGui::TextUnformatted("at the next start:");
    for (int m : {0, 3, 4, 5}) {
        ImGui::SameLine();
        if (ImGui::RadioButton(kNames[m], next == m)) g_next.store(m);
    }
    if (next != lm) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "   restart the game to switch to %s", kNames[next]);
    const unsigned ow = g_outW.load(), oh = g_outH.load();
    const float k = kScale[std::clamp(lm, 0, 5)];
    if (lm > 0)
        ImGui::TextDisabled("   now: %s - the game draws %ux%u, DLSS fills %ux%u per eye%s", kNames[lm],
                            (unsigned(float(ow) * k + 1.0f) & ~1u), unsigned(float(oh) * k + 1.0f) & ~1u, ow, oh,
                            live > 0 ? "" : " (DLSS stopped: picture stretched)");
    else
        ImGui::TextDisabled("   now: off - the game draws %ux%u with its own edge smoothing (FXAA)", ow, oh);
    if (lm >= 2) {
        int mb = g_mipBias.load();
        ImGui::TextUnformatted("sharper textures:");
        ImGui::SameLine(); if (ImGui::RadioButton("full##mb", mb == 1)) g_mipBias.store(1);
        ImGui::SameLine(); if (ImGui::RadioButton("mild##mb", mb == 2)) g_mipBias.store(2);
        ImGui::SameLine(); if (ImGui::RadioButton("off##mb", mb == 0)) g_mipBias.store(0);
    }
    if (live > 0 && bvr::overlay::show_tests()) {
        // JJ 2026-10-08: "the in-game glows are very flickery ... distant city lights". Live A/B for the two suspects.
        bool jit = g_jitterOn.load();
        if (ImGui::Checkbox("sub-pixel shake (DLSS needs it for smooth edges; test: off = do glows stop flickering?)", &jit)) {
            g_jitterOn.store(jit);
            for (Set& s : g_set) s.reset = true;
            BVR_LOG("[dlss] jitter %d (panel)", jit ? 1 : 0);
        }
    }
    if (live > 0 && bvr::overlay::show_tests()) {
        // FINISHFIRST (2026-10-09): the headset froze twice with DLSS (graphics card fault). Live A/B of the hand-off.
        int sy = g_sync.load();
        ImGui::TextUnformatted("hand-off to DLSS:");
        ImGui::SameLine(); if (ImGui::RadioButton("overlap##sy", sy == 3)) g_sync.store(3);
        ImGui::SameLine(); if (ImGui::RadioButton("safe##sy", sy == 2)) g_sync.store(2);
        ImGui::SameLine(); if (ImGui::RadioButton("half##sy", sy == 1)) g_sync.store(1);
        ImGui::SameLine(); if (ImGui::RadioButton("old (froze)##sy", sy == 0)) g_sync.store(0);
    }
    if (live > 0 && g_sh)
        ImGui::TextDisabled("   DLSS costs %.1f ms per eye on the graphics card", g_sh->gpuMs);
    if (g_sh && g_sh->ngxState < 0) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "   DLSS could not start on this PC");
}

void status_line(char* out, unsigned n) {
    snprintf(out, n,
             "DLSS mode %d (launched with %d: headset images %ux%u), route %s, DXGI 1.1 factory calls %u: helper %s, NGX %d, trips %u, fallbacks %u, timeouts %u, "
             "no depth %u, no camera %u, %.3f ms per eye (helper %.3f, DLSS on the card %.2f), depth inverted %d, preset %d | "
             "failed at: inputs %u, busy %u, helper status %u, output %u | sharper textures %d (%u twins, %u swaps) | "
             "sync %d (%s): finish wait game %.3f ms / %u timeouts, helper %.3f ms / %u timeouts, texture set gen %u | "
             "overlap %d: pairs %u, wait %.3f ms, fallbacks %u, timeouts %u, left lost %u, helper copy waits ran out %u",
             g_mode.load(), g_launchMode.load(), g_outW.load(), g_outH.load(), g_on.load() ? "ON" : "off", g_dxgi11Calls.load(), g_sh && g_sh->helperReady ? "ready" : "not ready",
             g_sh ? g_sh->ngxState : 0, g_trips.load(), g_fallbacks.load(), g_timeouts.load(), g_noDepth.load(),
             g_noPass.load(), g_waitAvgMs.load(), g_helperAvgMs.load(), g_sh ? g_sh->gpuMs : 0.0f, g_depthInv.load(),
             g_preset.load(), g_failIn.load(), g_failBusy.load(), g_failStatus.load(), g_failOut.load(),
             g_mipBias.load(), g_mipTwinsMade.load(), g_mipSwaps.load(), g_sync.load(), g_finish.kind(),
             g_finishAvgMs.load(), g_finishTimeouts.load(), g_sh ? g_sh->finishMs : 0.0f, g_sh ? g_sh->finishTimeouts : 0u,
             g_sh ? g_sh->gen : 0u, g_ovState, g_ovTrips.load(), g_ovWaitAvgMs.load(), g_ovFallbacks.load(),
             g_ovTimeouts.load(), g_ovLeftLost.load(), g_sh ? g_sh->fenceWaitTimeouts : 0u);
}

} // namespace bvr::ac::dlss_bridge
