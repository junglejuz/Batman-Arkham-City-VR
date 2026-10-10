// bc_dlss64.exe - Batman City's 64-bit DLSS helper (DLSS_PLAN.md). Arkham City is 32-bit and NVIDIA's DLSS library
// is 64-bit only, so the eye pictures come here through shared textures. Started by the game (batman_city.dll),
// ends with it. D0: copy input -> output. D1: one DLSS Super Resolution feature per eye (NGX), after AKN's dlss.cpp
// (akvr_native/src/dlss.cpp; method from Luma's Arkham Knight mod - Luma Framework, Filippo Tarpini / Garamond,
// "Custom MIT": reuse with credit).

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <cstdarg>
#include <cstdio>

#include "dlss_shared.h"
#include "gpu_finish.h"
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"

namespace {

FILE* g_log = nullptr;
void logf(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
wchar_t g_dir[MAX_PATH] = L"";

struct Set {
    ID3D11Texture2D* tex[bcdlss::kTexCount] = {};
    IDXGIKeyedMutex* km[bcdlss::kTexCount] = {};
};
Set g_set[2];
uint32_t g_gen = 0xFFFFFFFF;

// --- DLSS (NGX) ------------------------------------------------------------------------------------------
bool g_ngxTried = false, g_ngxOk = false;
NVSDK_NGX_Parameter* g_caps = nullptr;
struct Feature {
    NVSDK_NGX_Handle* h = nullptr;
    NVSDK_NGX_Parameter* p = nullptr;
    UINT rw = 0, rh = 0, ow = 0, oh = 0;
    int mode = -1, depthInv = -1, preset = -1;
    bool reset = true, failed = false;
    long evals = 0;
};
Feature g_feat[2];

// Evaluate cost on the graphics card (timestamp queries, read a few frames later).
struct Tq { ID3D11Query* dj = nullptr; ID3D11Query* a = nullptr; ID3D11Query* b = nullptr; bool pending = false; };
Tq g_tq[8];
int g_tqI = 0;
double g_tqSum = 0;
long g_tqN = 0;

void tq_collect() {
    for (Tq& q : g_tq) {
        if (!q.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 a = 0, b = 0;
        if (g_ctx->GetData(q.dj, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (g_ctx->GetData(q.a, &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            g_ctx->GetData(q.b, &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        q.pending = false;
        if (!dj.Disjoint && dj.Frequency && b > a) {
            g_tqSum += double(b - a) * 1000.0 / double(dj.Frequency);
            ++g_tqN;
        }
    }
}
Tq* tq_begin() {
    tq_collect();
    Tq& q = g_tq[g_tqI++ % 8];
    if (q.pending) return nullptr;
    if (!q.dj) {
        D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        g_dev->CreateQuery(&d, &q.dj);
        d.Query = D3D11_QUERY_TIMESTAMP;
        g_dev->CreateQuery(&d, &q.a);
        g_dev->CreateQuery(&d, &q.b);
    }
    if (!q.dj || !q.a || !q.b) return nullptr;
    g_ctx->Begin(q.dj);
    g_ctx->End(q.a);
    return &q;
}
void tq_end(Tq* q) {
    if (!q) return;
    g_ctx->End(q->b);
    g_ctx->End(q->dj);
    q->pending = true;
}

bool ngx_init() {
    if (g_ngxTried) return g_ngxOk;
    g_ngxTried = true;
    const wchar_t* paths[1] = {g_dir};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    // our own project id (unregistered with NVIDIA, as Luma / AKN)
    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID("8f3a1c62-4e9b-4d27-b1c5-6a2e9d4f0b31", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                             "1.0", g_dir, g_dev, &info);
    if (NVSDK_NGX_FAILED(r)) {
        logf("NGX init FAILED 0x%08x (nvngx_dlss.dll in %ls?)", unsigned(r), g_dir);
        return false;
    }
    r = NVSDK_NGX_D3D11_GetCapabilityParameters(&g_caps);
    int avail = 0;
    if (g_caps) g_caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
    g_ngxOk = NVSDK_NGX_SUCCEED(r) && avail;
    logf("NGX %s (super resolution available %d)", g_ngxOk ? "ready" : "NOT usable", avail);
    return g_ngxOk;
}

bool make_feature(Feature& f, const bcdlss::Shared* sh, int m) {
    if (f.h) { NVSDK_NGX_D3D11_ReleaseFeature(f.h); f.h = nullptr; }
    if (!f.p && NVSDK_NGX_FAILED(NVSDK_NGX_D3D11_AllocateParameters(&f.p))) return false;
    const int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
                      (sh->depthInverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0);
    if (sh->preset > 0) {
        const char* const keys[] = {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                                    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality,
                                    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                                    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                                    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                                    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance};
        for (const char* k : keys) NVSDK_NGX_Parameter_SetUI(f.p, k, unsigned(sh->preset));
    }
    NVSDK_NGX_DLSS_Create_Params cp{};
    cp.Feature.InWidth = sh->renderW;
    cp.Feature.InHeight = sh->renderH;
    cp.Feature.InTargetWidth = sh->outW;
    cp.Feature.InTargetHeight = sh->outH;
    cp.Feature.InPerfQualityValue = m == 1 ? NVSDK_NGX_PerfQuality_Value_DLAA
                                  : m == 2 ? NVSDK_NGX_PerfQuality_Value_UltraQuality
                                  : m == 3 ? NVSDK_NGX_PerfQuality_Value_MaxQuality
                                  : m == 4 ? NVSDK_NGX_PerfQuality_Value_Balanced
                                           : NVSDK_NGX_PerfQuality_Value_MaxPerf;
    cp.InFeatureCreateFlags = flags;
    NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(g_ctx, &f.h, f.p, &cp);
    if (NVSDK_NGX_FAILED(r)) {
        cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_Balanced;
        r = NGX_D3D11_CREATE_DLSS_EXT(g_ctx, &f.h, f.p, &cp);
    }
    logf("DLSS feature %ux%u -> %ux%u, mode %d, flags 0x%x, preset %d: %s (0x%08x)", sh->renderW, sh->renderH, sh->outW,
         sh->outH, m, flags, sh->preset, NVSDK_NGX_SUCCEED(r) ? "created" : "FAILED", unsigned(r));
    f.rw = sh->renderW; f.rh = sh->renderH; f.ow = sh->outW; f.oh = sh->outH;
    f.mode = m; f.depthInv = sh->depthInverted; f.preset = sh->preset;
    f.reset = true;
    f.failed = !(NVSDK_NGX_SUCCEED(r) && f.h);
    return !f.failed;
}

// --- shared textures -------------------------------------------------------------------------------------
void release_set(Set& s) {
    for (int i = 0; i < bcdlss::kTexCount; ++i) {
        if (s.km[i]) { s.km[i]->Release(); s.km[i] = nullptr; }
        if (s.tex[i]) { s.tex[i]->Release(); s.tex[i] = nullptr; }
    }
}

bool open_sets(const bcdlss::Shared* sh) {
    for (int e = 0; e < 2; ++e) {
        release_set(g_set[e]);
        for (int i = 0; i < bcdlss::kTexCount; ++i) {
            const uint64_t h = sh->handle[e][i];
            if (!h) continue;
            ID3D11Texture2D* t = nullptr;
            HRESULT hr = g_dev->OpenSharedResource(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(h)), IID_PPV_ARGS(&t));
            if (FAILED(hr) || !t) {
                logf("open shared texture eye %d slot %d handle %llx FAILED 0x%08X", e, i, (unsigned long long)h, hr);
                return false;
            }
            g_set[e].tex[i] = t;
            t->QueryInterface(IID_PPV_ARGS(&g_set[e].km[i]));
        }
    }
    logf("texture sets opened (gen %u): render %ux%u -> out %ux%u", sh->gen, sh->renderW, sh->renderH, sh->outW, sh->outH);
    return true;
}

bool make_device(const bcdlss::Shared* sh) {
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return false;
    IDXGIAdapter1* chosen = nullptr;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* a = nullptr;
        if (f->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        if (d.AdapterLuid.LowPart == sh->luidLow && d.AdapterLuid.HighPart == sh->luidHigh) {
            chosen = a;
            logf("adapter: %ls", d.Description);
            break;
        }
        a->Release();
    }
    f->Release();
    const D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const HRESULT hr = D3D11CreateDevice(chosen, chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                         fl, 2, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx);
    if (chosen) chosen->Release();
    if (FAILED(hr)) {
        logf("D3D11CreateDevice FAILED 0x%08X", hr);
        return false;
    }
    return true;
}

// One eye: copy (D0) or DLSS (D1). The game handed every slot over (key 1).
double g_tAcq = 0, g_tEval = 0;
long g_tN = 0;
bcdlss::GpuFinish g_finish;
bool g_finishLogged = false;
// One request's own numbers (the single legacy request, or an OVERLAP eye slot).
struct Req {
    float jx, jy;
    uint32_t reset;
    int32_t dlssMode;
    bool overlap; // the game waits for fenceOut instead of our finish-first wait
};
int serve(bcdlss::Shared* sh, int e, const Req& rq) {
    Set& s = g_set[e & 1];
    LARGE_INTEGER qf, ta, tb, tc;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&ta);
    IDXGIKeyedMutex* km[bcdlss::kTexCount] = {};
    int held[bcdlss::kTexCount] = {};
    int status = 0;
    for (int i = 0; i < bcdlss::kTexCount; ++i) {
        if (!s.tex[i]) continue;
        km[i] = s.km[i];
        if (!km[i] || km[i]->AcquireSync(1, 100) != S_OK) { status = -2; break; }
        held[i] = 1;
    }
    if (status == 0 && (!s.tex[bcdlss::kColor] || !s.tex[bcdlss::kOut])) status = -4;
    QueryPerformanceCounter(&tb);
    if (status == 0) {
        const bool dlss = rq.dlssMode > 0 && s.tex[bcdlss::kDepth] && s.tex[bcdlss::kMotion] && ngx_init();
        if (!dlss) {
            if (sh->renderW == sh->outW && sh->renderH == sh->outH) g_ctx->CopyResource(s.tex[bcdlss::kOut], s.tex[bcdlss::kColor]);
            else status = -5;
        } else {
            Feature& f = g_feat[e & 1];
            if (!f.h || f.rw != sh->renderW || f.rh != sh->renderH || f.ow != sh->outW || f.oh != sh->outH ||
                f.mode != rq.dlssMode || f.depthInv != sh->depthInverted || f.preset != sh->preset)
                make_feature(f, sh, rq.dlssMode);
            if (f.failed) {
                status = -6;
            } else {
                NVSDK_NGX_D3D11_DLSS_Eval_Params ep{};
                ep.Feature.pInColor = s.tex[bcdlss::kColor];
                ep.Feature.pInOutput = s.tex[bcdlss::kOut];
                ep.pInDepth = s.tex[bcdlss::kDepth];
                ep.pInMotionVectors = s.tex[bcdlss::kMotion];
                ep.InJitterOffsetX = rq.jx;
                ep.InJitterOffsetY = rq.jy;
                ep.InRenderSubrectDimensions.Width = f.rw;
                ep.InRenderSubrectDimensions.Height = f.rh;
                ep.InReset = (f.reset || rq.reset) ? 1 : 0;
                f.reset = false;
                ep.InMVScaleX = sh->mvScaleX;
                ep.InMVScaleY = sh->mvScaleY;
                Tq* q = tq_begin();
                NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT(g_ctx, f.h, f.p, &ep);
                for (int k = 0; k < sh->evalRepeat && k < 4 && NVSDK_NGX_SUCCEED(r); ++k)
                    r = NGX_D3D11_EVALUATE_DLSS_EXT(g_ctx, f.h, f.p, &ep);
                tq_end(q);
                if (NVSDK_NGX_FAILED(r)) {
                    status = -7;
                    if ((f.evals++ % 300) == 0) logf("eye %d evaluate FAILED 0x%08x", e, unsigned(r));
                } else if (++f.evals == 1) {
                    logf("eye %d: first DLSS evaluate", e);
                }
                if (g_tqN >= 50) {
                    sh->gpuMs = float(g_tqSum / g_tqN);
                    g_tqSum = 0;
                    g_tqN = 0;
                }
            }
        }
    }
    QueryPerformanceCounter(&tc);
    g_tAcq += double(tb.QuadPart - ta.QuadPart) * 1000.0 / double(qf.QuadPart);
    g_tEval += double(tc.QuadPart - tb.QuadPart) * 1000.0 / double(qf.QuadPart);
    if (++g_tN == 120) {
        logf("timing over 120 requests: waiting for the game's pictures %.3f ms, copy/DLSS call %.3f ms (CPU), DLSS on the card %.2f ms, status %d, sync %d (finish wait %.2f ms, %u timeouts)",
             g_tAcq / 120.0, g_tEval / 120.0, sh->gpuMs, status, sh->syncMode, sh->finishMs, sh->finishTimeouts);
        g_tAcq = g_tEval = 0;
        g_tN = 0;
    }
    for (int i = 0; i < bcdlss::kTexCount; ++i)
        if (held[i]) km[i]->ReleaseSync(0);
    // FINISHFIRST: answer only once the DLSS (and the release above) has run on the card, so the game's wait for
    // the output is already satisfied and its context never stalls on ours.
    if (sh->syncMode >= 1 && !rq.overlap) {
        LARGE_INTEGER td;
        QueryPerformanceCounter(&td);
        if (!g_finish.wait(g_dev, g_ctx, 100)) ++sh->finishTimeouts;
        LARGE_INTEGER te;
        QueryPerformanceCounter(&te);
        sh->finishMs = float(double(te.QuadPart - td.QuadPart) * 1000.0 / double(qf.QuadPart));
        if (!g_finishLogged) {
            g_finishLogged = true;
            logf("finish-first hand-off (sync %d) with a %s", sh->syncMode, g_finish.kind());
        }
    } else {
        sh->finishMs = 0.0f;
    }
    sh->ngxState = g_ngxTried ? (g_ngxOk ? 1 : -1) : 0;
    return status;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wchar_t path[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
    if (n && n < MAX_PATH - 64) {
        wcscat_s(path, L"\\Batman_City\\dlss_helper.log");
        g_log = _wfopen(path, L"w");
    }
    GetModuleFileNameW(nullptr, g_dir, MAX_PATH);
    if (wchar_t* sl = wcsrchr(g_dir, L'\\')) *sl = 0; // nvngx_dlss.dll sits next to the helper
    logf("bc_dlss64 starting (pid %lu), folder %ls", GetCurrentProcessId(), g_dir);

    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, bcdlss::kMapName);
    if (!map) { logf("no shared block - exiting"); return 1; }
    auto* sh = static_cast<bcdlss::Shared*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(bcdlss::Shared)));
    HANDLE req = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, bcdlss::kReqName);
    HANDLE done = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, bcdlss::kDoneName);
    if (!sh || !req || !done || sh->version != bcdlss::kVersion) { logf("shared block / events missing or old version - exiting"); return 1; }
    HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, sh->gamePid);
    if (!make_device(sh)) return 1;
    sh->helperPid = GetCurrentProcessId();
    sh->helperReady = 1;
    logf("ready");

    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    uint32_t served = 0, ovServed = 0;
    // OVERLAP: the game's two shared fences, opened once it has published them.
    ID3D11Fence* fenceIn = nullptr;
    ID3D11Fence* fenceOut = nullptr;
    ID3D11DeviceContext4* c4 = nullptr;
    bool fencesTried = false;
    HANDLE fenceEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    uint32_t handled[2] = {0, 0};
    auto open_fences = [&]() -> bool {
        if (fenceIn && fenceOut && c4) return true;
        if (fencesTried || !sh->fenceInHandle || !sh->fenceOutHandle) return false;
        fencesTried = true;
        ID3D11Device5* d5 = nullptr;
        HRESULT a = E_NOINTERFACE, b = E_NOINTERFACE;
        if (SUCCEEDED(g_dev->QueryInterface(IID_PPV_ARGS(&d5))) && d5) {
            a = d5->OpenSharedFence(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(sh->fenceInHandle)), IID_PPV_ARGS(&fenceIn));
            b = d5->OpenSharedFence(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(sh->fenceOutHandle)), IID_PPV_ARGS(&fenceOut));
            d5->Release();
        }
        g_ctx->QueryInterface(IID_PPV_ARGS(&c4));
        logf("overlap fences: in 0x%08X, out 0x%08X, context4 %s", unsigned(a), unsigned(b), c4 ? "yes" : "NO");
        return fenceIn && fenceOut && c4;
    };
    for (;;) {
        const DWORD w = WaitForSingleObject(req, 1000);
        if (game && WaitForSingleObject(game, 0) == WAIT_OBJECT_0) { logf("game ended - exiting"); break; }
        if (w != WAIT_OBJECT_0) continue;
        // OVERLAP eye slots, oldest serial first (left before right).
        for (;;) {
            int e = -1;
            for (int k = 0; k < 2; ++k) {
                const uint32_t s = sh->req[k].serial;
                if (s != handled[k] && (e < 0 || s < sh->req[e].serial)) e = k;
            }
            if (e < 0) break;
            const uint32_t serial = sh->req[e].serial;
            handled[e] = serial;
            MemoryBarrier(); // the slot's numbers were written before its serial
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);
            // Without the fences we still serve (the keyed mutexes order it and hand the pictures back); the game
            // then times out on fenceOut and stops asking for overlap.
            const bool fences = open_fences();
            int status = 0;
            if (sh->gen != g_gen) {
                g_gen = sh->gen;
                if (!open_sets(sh)) status = -1;
            }
            if (status == 0) {
                // Wait here, on the CPU, until the game's copy has run - never on the graphics card. On a timeout
                // serve anyway: the keyed mutexes still order it (a card-side wait, counted).
                const ULONGLONG deadline = GetTickCount64() + 100;
                while (fences && fenceIn->GetCompletedValue() < serial) {
                    const ULONGLONG now = GetTickCount64();
                    if (now >= deadline || FAILED(fenceIn->SetEventOnCompletion(serial, fenceEv))) { ++sh->fenceWaitTimeouts; break; }
                    WaitForSingleObject(fenceEv, DWORD(deadline - now));
                }
                const Req rq{sh->req[e].jitterX, sh->req[e].jitterY, sh->req[e].reset, sh->req[e].dlssMode, true};
                status = serve(sh, e, rq);
                if (!fences && status == 0) status = -10;
            }
            QueryPerformanceCounter(&t1);
            sh->req[e].status = status;
            sh->req[e].helperMs = static_cast<float>((t1.QuadPart - t0.QuadPart) * 1000.0 / qf.QuadPart);
            MemoryBarrier();
            sh->req[e].answered = serial; // before the fence: the game reads it once the fence has passed
            if (c4) {
                c4->Signal(fenceOut, serial); // after our release, so the game's next acquire is already satisfied
                g_ctx->Flush();
            }
            SetEvent(done);
            if (++ovServed == 1 || (status && (ovServed % 300) == 1))
                logf("overlap request %u eye %d status %d (%.3f ms)", serial, e, status, sh->req[e].helperMs);
        }
        // The one-at-a-time request (sync 0..2).
        if (sh->serial == sh->answered) continue;
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        int status = 0;
        if (sh->gen != g_gen) {
            g_gen = sh->gen;
            if (!open_sets(sh)) status = -1;
        }
        if (status == 0) {
            const Req rq{sh->jitterX, sh->jitterY, sh->reset, sh->dlssMode, false};
            status = serve(sh, sh->eye, rq);
        }
        QueryPerformanceCounter(&t1);
        sh->status = status;
        sh->helperMs = static_cast<float>((t1.QuadPart - t0.QuadPart) * 1000.0 / qf.QuadPart);
        sh->answered = sh->serial;
        SetEvent(done);
        if (++served == 1 || (status && (served % 300) == 1))
            logf("request %u eye %d status %d (%.3f ms)", sh->serial, sh->eye, status, sh->helperMs);
    }
    if (c4) c4->Release();
    if (fenceIn) fenceIn->Release();
    if (fenceOut) fenceOut->Release();
    for (Feature& f : g_feat)
        if (f.h) NVSDK_NGX_D3D11_ReleaseFeature(f.h);
    if (g_ngxOk) NVSDK_NGX_D3D11_Shutdown1(g_dev);
    release_set(g_set[0]);
    release_set(g_set[1]);
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
    return 0;
}
