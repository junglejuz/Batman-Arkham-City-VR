#include "game/arkhamcity/scene.h"
#include "core/ui/overlay.h"

#include "core/hooks/d3d11_hook.h"
#include "core/gfx/frame_inspector.h"
#include "core/gfx/gfx_hud.h"
#include "core/gfx/hud_capture.h"
#include "core/hooks/pattern_scan.h"
#include "core/input/dinput_block.h"
#include "core/input/xinput_bridge.h"
#include "core/util/crash.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/arkhamcity/addresses.h"
#include "game/arkhamcity/afw.h"
#include "game/arkhamcity/billboard.h"
#include "game/arkhamcity/glow.h"
#include "game/arkhamcity/dlss_bridge.h"
#include "game/arkhamcity/markers.h"
#include "game/arkhamcity/menu_state.h"
#include "game/arkhamcity/reflect.h"
#include "game/arkhamcity/room_overlay.h"
#include "game/arkhamcity/screen_hd.h"
#include "game/arkhamcity/vision.h"
#include "game/arkhamcity/window.h"

#include <d3d11.h>

#include <MinHook.h>
#include <imgui.h>
#include <intrin.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace bvr::ac::scene {
namespace {

constexpr float kPi = 3.14159265358979f;

// --- hook types --------------------------------------------------------------
// thiscall targets hooked as __fastcall with a dummy EDX (BioShock VR's trick).
using DrawFn = void(__fastcall*)(void* self, void* edx, uint32_t a1);
using CalcFn = void*(__fastcall*)(void* self, void* edx, void* family, float* loc,
                                  int32_t* rot, void* viewport, void* drawer);
// FSceneView ctor: 27 stack dwords (ret 0x6C). a2 = ViewState, a10..a13 = X,Y,W,H
// (floats), a14 = &ViewMatrix, a15 = &ProjectionMatrix (RE_NOTES "LIVE results").
#define BC_A27 uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, \
    uint32_t a7, uint32_t a8, uint32_t a9, uint32_t a10, uint32_t a11, uint32_t a12, uint32_t a13, \
    uint32_t a14, uint32_t a15, uint32_t a16, uint32_t a17, uint32_t a18, uint32_t a19, uint32_t a20, \
    uint32_t a21, uint32_t a22, uint32_t a23, uint32_t a24, uint32_t a25, uint32_t a26, uint32_t a27
#define BC_P27(v14, v15) a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, v14, v15, a16, \
    a17, a18, a19, a20, a21, a22, a23, a24, a25, a26, a27
using CtorFn = void*(__fastcall*)(void* self, void* edx, BC_A27);
// UGameViewportClient::Draw(viewport, canvas): thiscall, ret 8. One call per BeginDrawing.
using ClientDrawFn = void(__fastcall*)(void* self, void* edx, void* viewport, void* canvas);
// BeginDrawing render command Execute (render thread): thiscall(cmd), returns cmd size.
using BeginExecFn = uint32_t(__fastcall*)(void* self, void* edx);

DrawFn g_drawOrig = nullptr;
CalcFn g_calcOrig = nullptr;
CtorFn g_ctorOrig = nullptr;
ClientDrawFn g_clientDrawOrig = nullptr;
BeginExecFn g_beginExecOrig = nullptr;

// Eye attribution, render side (fixes the converge/diverge flicker JJ saw 2026-10-03: the
// game-thread tag ring skewed under real GPU load). Pass k's eye is written into slot k
// BEFORE the pass enqueues its BeginDrawing; the render thread reads slot k when it executes
// BeginDrawing k and labels the frame it is about to present.
constexpr uint32_t kEyeSlots = 64;
// slot = (pose generation << 3) | (AFW frame << 2) | (eye sign + 1): the eye AND the locate it was drawn from.
std::atomic<uint32_t> g_eyeOfBegin[kEyeSlots] = {};
uint32_t g_poseGen = 0; // game thread: locate id of g_pose
std::atomic<uint32_t> g_gameBegins{0}; // client Draw calls (game thread)
std::atomic<uint32_t> g_rtBegins{0};   // BeginDrawing executions (render thread)
std::atomic<uint32_t> g_rtLabelled{0};
std::atomic<bool> g_live{false};

// --- census ------------------------------------------------------------------
struct Census {
    static constexpr int kSlots = 8;
    std::atomic<uint32_t> ra[kSlots]{};
    std::atomic<uint32_t> n[kSlots]{};
    void note(uint32_t r) {
        for (int i = 0; i < kSlots; ++i) {
            uint32_t cur = ra[i].load(std::memory_order_relaxed);
            if (cur == r) { n[i].fetch_add(1, std::memory_order_relaxed); return; }
            if (cur == 0 && ra[i].compare_exchange_strong(cur, r)) {
                n[i].fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    }
    void print(const char* tag) const {
        char line[512];
        int len = sprintf_s(line, "[bc] callers %s:", tag);
        for (int i = 0; i < kSlots; ++i) {
            const uint32_t r = ra[i].load();
            if (!r) break;
            len += sprintf_s(line + len, sizeof(line) - len, " %08X x%u", r, n[i].load());
        }
        BVR_LOG("%s", line);
    }
};
Census g_drawCallers, g_calcCallers, g_ctorCallers;

std::atomic<uint32_t> g_draws{0}, g_secondDraws{0}, g_calcs{0}, g_viewEdits{0};
std::atomic<uint32_t> g_skipForeign{0}, g_skipNoView{0}, g_skipNoPresent{0};
std::atomic<uint32_t> g_lastExcCode{0}, g_lastExcAddr{0}, g_call2Us{0};

// --- VR state (game thread) ---------------------------------------------------
std::atomic<bool> g_camera{false};
std::atomic<bool> g_stereo{false};
std::atomic<bool> g_poisoned{false};
std::atomic<bool> g_posOn{true};
// The game letterboxes its scene to 16:9 in a non-16:9 window (AKVR's aspect lock). The
// letterboxed rect arrives as ctor args a10..a13; in VR we widen it to the whole backbuffer.
std::atomic<bool> g_fullRect{true};
// 2D on a quad (JJ 2026-10-03: menus/titles warped with the head and menu art would not
// converge): every backbuffer draw after the scene's final full-screen pass is UI and goes to
// the core's head-locked HUD quad; the eye images stay pure 3D. Core gfx_hud classifier.
std::atomic<bool> g_hudQuad{true};
// Right eye gets its OWN view state (temporal history, occlusion results, eye adaptation),
// allocated once with the engine's own AllocateViewState. Sharing one between the eyes is the
// prime suspect for per-object micro-flicker (one eye's occlusion verdict applied to the other).
std::atomic<bool> g_state2On{true};
void* g_eye2State = nullptr;
// STATE1 (2026-10-07, JJ capture 20261007_235945: the neon sign and its coloured light on the wet road
// were missing in the LEFT eye only; that eye used the game's own view state, the right eye ours). The
// left eye gets a private view state too, so nothing else that renders with the game's state (a scene
// capture drew a 256x240 view in the left pass) can mix its occlusion history into the left eye.
std::atomic<bool> g_state1On{false}; // fake headset 2026-10-08: no effect on the sign or the puddles - off
void* g_eye1State = nullptr;
// Diagnostics: the game's own view state, and whether another FSceneView was built with it.
std::atomic<uint32_t> g_gameState{0}, g_foreignSameState{0};
using AllocStateFn = void*(__cdecl*)();
float g_lastRect[4] = {};
// First-look default (JJ 2026-10-03): arm camera + stereo once the session first reaches
// FOCUSED, so a launch is all that is needed. `bc auto off` / F10 to opt out.
std::atomic<bool> g_autoVr{true};
bool g_autoDone = false;
std::atomic<float> g_ipdMm{63.0f};
// Unreal units per metre of head motion. AKVR's measured 1:1 was ~89 UU/m (2.8 UU half-eye),
// UE3 nominal is 50; re-measure for Arkham City in the headset.
std::atomic<float> g_uuPerM{90.0f};

int g_depth = 0;                  // per-frame draw nesting (game thread)
int g_eyeSign = 0;                // 0 mono, -1 left pass, +1 right pass
// AFW (BATMAN_CITY_PLAN.md §11): this draw is an AFW frame (one eye drawn, the other warped at its present), and the
// eye the next AFW frame draws (0 left, 1 right) - one clock on the game thread, carried with the label (STR-012).
bool g_afwFrame = false;
int g_afwNext = 0;
std::atomic<uint32_t> g_afwVpLogs{0};
bvr::vr::HeadPose g_pose{};       // one head sample per frame, reused by both passes
bool g_poseValid = false;
bool g_viewThisDraw = false;      // the CalcSceneView ctor fired inside this draw
bool g_viewLastDraw = false;
uint64_t g_lastDrawPresent = 0;

float g_loc[3] = {};
int32_t g_rot[3] = {};
float g_lastHfov = 0.0f;
std::atomic<int> g_dumpRequested{0}; // views still to save (2 = both eyes of one stereo frame)

// Recenter (F12 / panel button): the head pose is made relative to a reference yaw + position,
// so "forward" is wherever you faced when you recentred. Captured automatically on the first
// tracked frame too.
std::atomic<bool> g_recenterReq{false};
bool g_haveRef = false;
float g_refYaw = 0.0f, g_refPos[3] = {};

// Title-screen grunge: a full-screen triangle (Draw 3) sampling a 2048x1024 BC1 sRGB texture,
// painted into the scene colour before the final pass (frame dump event 930). Drawn on the face it
// sits at no sensible depth (JJ 2026-10-03: "really close to my face"); room_overlay hangs it on a
// screen in the room instead (JJ 2026-10-07).

// Frame-rate lock (AKVR FPSLOCK): hold the game at a fixed rate, every game frame shown for the
// same number of headset refreshes. 0 = off (as fast as it can).
std::atomic<int> g_fpsLock{0};
std::atomic<uint32_t> g_engineBlocked{0}; // game inputs swallowed while the VR menu was open
// Between game frames (AKVR FPSLOCK-SSW, JJ 2026-10-03): true = repeat the frame (core re-shows
// the previous layers, the hold happens at the headset submit); false = Virtual Desktop SSW (the
// game thread sleeps so the headset sees a half-rate app and VD synthesises the gaps).
std::atomic<bool> g_fpsRepeat{true};
uint64_t g_lockLastQpc = 0;
std::atomic<uint32_t> g_grungeSkipped{0};

// Telemetry: one row per game frame, the last ~22 s kept (AKVR's "save last 20 s").
struct TeleRow {
    uint32_t ms;
    uint32_t frameUs;
    float head[7];   // recentred head pose: px py pz qx qy qz qw (XR metres)
    float loc[3];    // game camera location (UU)
    int32_t rot[3];  // game camera rotator
    uint32_t draws, second, presents, labelled;
    int32_t inflight;
    float hfov;
    uint8_t stereo, poseValid;
};
constexpr int kTeleRows = 2048;
TeleRow g_tele[kTeleRows];
uint32_t g_teleHead = 0;
uint64_t g_lastFrameQpc = 0;
float g_rateDraws = 0, g_rateSecond = 0, g_ratePresents = 0;
float g_dumpOrigin[3] = {};
int g_dumpOriginEye = 0;

// --- small math (UE3: row vectors, row-major FMatrix, translation in row 3) ---
struct M4 { float m[4][4]; };

M4 mul(const M4& a, const M4& b) {
    M4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] +
                        a.m[i][3] * b.m[3][j];
    return r;
}

// Head rotation in UE view space (x right, y up, z forward; left-handed) from an
// OpenXR LOCAL quaternion (x right, y up, -z forward; right-handed): R_view = S R_xr S
// with S = diag(1,1,-1). Returned as the 3x3 "column convention" matrix R, which is
// exactly the row-vector matrix that maps view space -> head space (p_head = p R).
void head_rotation_view(const bvr::vr::HeadPose& p, float R[3][3]) {
    const float x = p.qx, y = p.qy, z = p.qz, w = p.qw;
    const float Rx[3][3] = {
        {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
        {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
        {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)},
    };
    const float s[3] = {1, 1, -1};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) R[i][j] = s[i] * Rx[i][j] * s[j];
}

// Delta = T(-t_eye) * R: game-camera view space -> this eye's view space.
M4 eye_delta(const bvr::vr::HeadPose& p, int eyeSign) {
    float R[3][3];
    head_rotation_view(p, R);
    const float uu = g_uuPerM.load();
    float t[3] = {0, 0, 0};
    if (g_posOn.load()) {
        t[0] = p.px * uu;
        t[1] = p.py * uu;
        t[2] = -p.pz * uu;
    }
    // Eye offset along the HEAD's right axis (column 0 of R in column convention).
    const float half = eyeSign * g_ipdMm.load() * 0.0005f * uu;
    for (int i = 0; i < 3; ++i) t[i] += R[i][0] * half;
    M4 d{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) d.m[i][j] = R[i][j];
    for (int j = 0; j < 3; ++j) d.m[3][j] = -(t[0] * R[0][j] + t[1] * R[1][j] + t[2] * R[2][j]);
    d.m[3][3] = 1.0f;
    return d;
}

float quat_yaw(float qx, float qy, float qz, float qw) {
    const float fx = -2.0f * (qx * qz + qw * qy);           // forward = q*(0,0,-1)
    const float fz = -(1.0f - 2.0f * (qx * qx + qy * qy));
    return atan2f(-fx, -fz);
}
// Make a raw XR head pose relative to the recentre reference (yaw about +Y, then position).
void apply_recenter(bvr::vr::HeadPose& p) {
    if (g_recenterReq.exchange(false) || !g_haveRef) {
        g_refYaw = quat_yaw(p.qx, p.qy, p.qz, p.qw);
        g_refPos[0] = p.px; g_refPos[1] = p.py; g_refPos[2] = p.pz;
        g_haveRef = true;
        bvr::vr::recenter_panel();
        BVR_LOG("[bc] recentred: yaw %.1f deg, head at (%.2f, %.2f, %.2f) m", g_refYaw * 57.29578f, p.px, p.py, p.pz);
    }
    const float h = -g_refYaw * 0.5f;
    const float aw = cosf(h), ay = sinf(h); // qy(-yaw0)
    const float bx = p.qx, by = p.qy, bz = p.qz, bw = p.qw;
    p.qw = aw * bw - ay * by;
    p.qx = aw * bx + ay * bz;
    p.qy = aw * by + ay * bw;
    p.qz = aw * bz - ay * bx;
    const float th = -g_refYaw, c = cosf(th), s = sinf(th);
    const float dx = p.px - g_refPos[0], dy = p.py - g_refPos[1], dz = p.pz - g_refPos[2];
    p.px = c * dx + s * dz;
    p.py = dy;
    p.pz = -s * dx + c * dz;
}

uint32_t g_teleLastDraws = 0;
void tele_record() {
    LARGE_INTEGER q, f;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    TeleRow& r = g_tele[g_teleHead % kTeleRows];
    r.ms = static_cast<uint32_t>(GetTickCount64());
    r.frameUs = g_lastFrameQpc ? static_cast<uint32_t>((q.QuadPart - g_lastFrameQpc) * 1000000 / f.QuadPart) : 0;
    g_lastFrameQpc = q.QuadPart;
    r.head[0] = g_pose.px; r.head[1] = g_pose.py; r.head[2] = g_pose.pz;
    r.head[3] = g_pose.qx; r.head[4] = g_pose.qy; r.head[5] = g_pose.qz; r.head[6] = g_pose.qw;
    memcpy(r.loc, g_loc, sizeof(r.loc));
    memcpy(r.rot, g_rot, sizeof(r.rot));
    r.draws = g_draws.load(); r.second = g_secondDraws.load();
    r.presents = static_cast<uint32_t>(bvr::d3d11_hook::present_count());
    r.labelled = g_rtLabelled.load();
    r.inflight = static_cast<int32_t>(g_gameBegins.load() - g_rtBegins.load());
    r.hfov = g_lastHfov;
    r.stereo = g_stereo.load() ? 1 : 0;
    r.poseValid = g_poseValid ? 1 : 0;
    ++g_teleHead;
}

void capture_dir(wchar_t* out, size_t n) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t base[MAX_PATH];
    swprintf_s(base, L"%s\\captures", bvr::log::data_dir());
    CreateDirectoryW(base, nullptr);
    swprintf_s(out, n, L"%s\\%04d%02d%02d_%02d%02d%02d", base, st.wYear, st.wMonth, st.wDay, st.wHour,
               st.wMinute, st.wSecond);
    CreateDirectoryW(out, nullptr);
}

// "Save capture" (F2): settings + counters, the last ~20 s of per-frame telemetry, both eyes'
// FSceneView and one full frame dump - everything a later session needs to diagnose a moment.
void save_targets(const wchar_t* dir); // marker probe, below
std::atomic<bool> g_captureReq{false};
void save_capture() {
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    capture_dir(dir, MAX_PATH);
    // Pictures (JJ 2026-10-04: "should be seen clearly in the capture"): each eye as shown, and
    // the HUD panel with its transparency.
    bvr::vr::save_eye_pictures(dir);
    save_targets(dir);
    reflect::request_pictures(dir); // the right eye's puddle reflection before / after its shift
    // room_overlay::request_replay_pictures(dir) found the pause dirt (2026-10-07); off in normal captures.
    bvr::gfx_hud::request_dump(); // every HUD draw's placement, into the log
    swprintf_s(path, L"%s\\hud_panel.bmp", dir);
    bvr::gfx_hud::save_panel_picture(path);
    swprintf_s(path, L"%s\\hud_last_frames.txt", dir); // HUDRING: what the HUD drew over the last ~5 s
    bvr::gfx_hud::save_hud_ring(path);
    swprintf_s(path, L"%s\\status.txt", dir);
    if (FILE* f = _wfopen(path, L"w")) {
        float hd, hw, hu; bvr::vr::get_hud_quad(&hd, &hw, &hu);
        fprintf(f, "Batman City capture\ncamera %d stereo %d poisoned %d pos %d ipd %.1f mm scale %.1f UU/m\n",
                g_camera.load(), g_stereo.load(), g_poisoned.load(), g_posOn.load(), g_ipdMm.load(), g_uuPerM.load());
        fprintf(f, "pose lag %d asym %d state2 %d hudQuad %d hudHead %d | grunge draws seen %u\n",
                bvr::vr::get_pose_lag(), bvr::vr::asym_fov(), g_state2On.load(), g_hudQuad.load(),
                bvr::vr::hud_head_locked(), g_grungeSkipped.load());
        {
            char ro[1024];
            room_overlay::status_line(ro, sizeof(ro));
            fprintf(f, "%s\n", ro);
            reflect::status_line(ro, sizeof(ro));
            fprintf(f, "%s\n", ro);
            menu_state::status_line(ro, sizeof(ro));
            fprintf(f, "%s | UNPAUSECUT %u\n", ro, bvr::gfx_hud::unpause_cuts());
            dlss_bridge::status_line(ro, sizeof(ro));
            fprintf(f, "%s\n", ro);
            fprintf(f, "BILLBOARD on %d: particle axes patched %u | BATCUTS on %d: cut pieces kept in the eye %u\n",
                    billboard::enabled() ? 1 : 0, billboard::patched(), bvr::gfx_hud::cuts_to_eye() ? 1 : 0,
                    bvr::gfx_hud::cuts_kept());
        }
        fprintf(f, "hud quad %.2f m wide at %.2f m, up %.2f | hfov %.1f | rates draws %.0f second %.0f presents %.0f /s\n",
                hw, hd, hu, g_lastHfov, g_rateDraws, g_rateSecond, g_ratePresents);
        fprintf(f, "draws %u second %u edits %u | begins game %u render %u labelled %u | skips foreign %u noView %u noPresent %u\n",
                g_draws.load(), g_secondDraws.load(), g_viewEdits.load(), g_gameBegins.load(), g_rtBegins.load(),
                g_rtLabelled.load(), g_skipForeign.load(), g_skipNoView.load(), g_skipNoPresent.load());
        {
            uint32_t eh = 0, em = 0;
            bvr::vr::exact_pose_tag_stats(&eh, &em);
            uint32_t rpt = 0, nf = 0, late = 0;
            bvr::vr::repeat_stats(&rpt, &nf, &late);
            fprintf(f, "exact pose tags %s: matched %u, missed %u | fps lock %d (%s: repeated %u new %u late %u) | pad reads blocked %u (DirectInput %u, engine %u)\n",
                    bvr::vr::exact_pose_tag() ? "ON" : "off", eh, em, g_fpsLock.load(),
                    g_fpsRepeat.load() ? "repeat the frame" : "VD SSW", rpt, nf, late, bvr::input::blocked_reads(),
                    bvr::input::dinput_block::blocked_reads(), g_engineBlocked.load());
        }
        fprintf(f, "game camera loc (%.1f, %.1f, %.1f) rot (%d, %d, %d) | xr %s\n", g_loc[0], g_loc[1], g_loc[2],
                g_rot[0], g_rot[1], g_rot[2], bvr::vr::session_state_name());
        fclose(f);
    }
    swprintf_s(path, L"%s\\last20s.csv", dir);
    if (FILE* f = _wfopen(path, L"w")) {
        fprintf(f, "ms,frame_us,hx,hy,hz,qx,qy,qz,qw,locx,locy,locz,pitch,yaw,roll,draws,second,presents,labelled,inflight,hfov,stereo,pose_valid\n");
        const uint32_t n = g_teleHead < kTeleRows ? g_teleHead : kTeleRows;
        const uint32_t nowMs = static_cast<uint32_t>(GetTickCount64());
        for (uint32_t i = 0; i < n; ++i) {
            const TeleRow& r = g_tele[(g_teleHead - n + i) % kTeleRows];
            if (nowMs - r.ms > 20000) continue;
            fprintf(f, "%u,%u,%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.5f,%.1f,%.1f,%.1f,%d,%d,%d,%u,%u,%u,%u,%d,%.1f,%u,%u\n",
                    r.ms, r.frameUs, r.head[0], r.head[1], r.head[2], r.head[3], r.head[4], r.head[5], r.head[6],
                    r.loc[0], r.loc[1], r.loc[2], r.rot[0], r.rot[1], r.rot[2], r.draws, r.second, r.presents,
                    r.labelled, r.inflight, r.hfov, r.stereo, r.poseValid);
        }
        fclose(f);
    }
    if (bvr::overlay::show_tests()) { // PANELTIDY: the heavy dumps (3 MB each) only with test options on
        g_dumpRequested.store(g_stereo.load() ? 2 : 1); // both eyes' FSceneView (own timestamped folder)
        g_dumpOriginEye = 0;
        bvr::frame_inspector::arm(3, 3);                  // 3 passes (both eyes of one frame), every PS texture slot (data folder)
    }
    BVR_LOG("[bc] CAPTURE saved -> %ls (status, last 20 s telemetry%s)", dir, bvr::overlay::show_tests() ? "; view + frame dumps follow" : "");
}

// DRAW CENSUS (2026-10-07, JJ: the pause menu's dirt overlay sits on the face): every distinct
// texture a small non-indexed Draw samples is logged once, with the time, so the draw that appears
// when the pause menu opens can be named.
void draw_census(unsigned vertexCount, const D3D11_TEXTURE2D_DESC& d) {
    static uint64_t s_seen[64];
    static unsigned s_n = 0;
    const uint64_t key = (uint64_t(d.Width) << 40) ^ (uint64_t(d.Height) << 20) ^ (uint64_t(d.Format) << 8) ^
                         (uint64_t(d.BindFlags) << 1) ^ vertexCount;
    for (unsigned i = 0; i < s_n; ++i)
        if (s_seen[i] == key) return;
    if (s_n >= 64) return;
    s_seen[s_n++] = key;
    BVR_LOG("[bc] draw census: new Draw(%u) texture %ux%u fmt %d mips %u bind 0x%X", vertexCount, d.Width, d.Height,
            int(d.Format), d.MipLevels, d.BindFlags);
}

bool grunge_skip_inner(ID3D11DeviceContext* ctx, unsigned vertexCount);
bool __cdecl grunge_skip(ID3D11DeviceContext* ctx, unsigned vertexCount) { // + HD SCREENS for non-indexed draws
    screen_hd::on_draw(ctx, vision::movie_playing());
    const bool skip = grunge_skip_inner(ctx, vertexCount);
    if (skip) screen_hd::after_draw(ctx);
    return skip;
}
bool grunge_skip_inner(ID3D11DeviceContext* ctx, unsigned vertexCount) {
    if (vision::draw_skip(ctx, vertexCount)) return true; // detective-mode circle
    if (vertexCount > 6 || !g_camera.load(std::memory_order_relaxed)) return false;
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv) return false;
    bool hit = false;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    if (res) {
        ID3D11Texture2D* tex = nullptr;
        res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
        if (tex) {
            D3D11_TEXTURE2D_DESC d{};
            tex->GetDesc(&d);
            draw_census(vertexCount, d);
            // GRUNGEPACK (JJ 2026-10-10: after a high-resolution texture pack "the grime textures are back on the menu and
            // pause screens"; log 17:28:51: the title grime is now 4096x2048 BC1 sRGB, was 2048x1024): match the SHAPE -
            // a full-screen triangle sampling a 2:1 BC1 picture at least 2048 wide - not one exact size.
            hit = vertexCount == 3 && d.Width >= 2048 && d.Width == 2 * d.Height &&
                  (d.Format == DXGI_FORMAT_BC1_UNORM_SRGB || d.Format == DXGI_FORMAT_BC1_UNORM) &&
                  !(d.BindFlags & D3D11_BIND_RENDER_TARGET);
            static UINT s_loggedW = 0;
            if (hit && d.Width != s_loggedW) { s_loggedW = d.Width; BVR_LOG("[bc] title grime picture %ux%u format %d", d.Width, d.Height, int(d.Format)); }
            tex->Release();
        }
        res->Release();
    }
    srv->Release();
    if (!hit) {
        if (vertexCount != 3) return false;
        if (room_overlay::on_pause_dirt(ctx)) return true; // PAUSEDIRT: drawn with the dirt in the room
        room_overlay::probe_draw(ctx);
        return false;
    }
    g_grungeSkipped.fetch_add(1, std::memory_order_relaxed);
    return room_overlay::on_grunge_draw(ctx); // ROOMGRUNGE: on a screen in the room (or hidden)
}

// Keys (AKVR's): F12 recenter, F11 head tracking, F2 capture. Game thread, edge-triggered.
void poll_keys() {
    static bool was[3] = {};
    const int keys[3] = {VK_F12, VK_F11, VK_F2};
    for (int i = 0; i < 3; ++i) {
        const bool down = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        if (down && !was[i]) {
            if (i == 0) g_recenterReq.store(true);
            else if (i == 1) { if (g_camera.load()) set_camera(false); else set_stereo(true); }
            else g_captureReq.store(true);
        }
        was[i] = down;
    }
}

bool vr_view_active() {
    return g_camera.load(std::memory_order_relaxed) && bvr::vr::vr_camera_mode();
}
bool stereo_active() {
    return vr_view_active() && g_stereo.load(std::memory_order_relaxed) &&
           !g_poisoned.load(std::memory_order_relaxed) && !bvr::crash::teardown_seen();
}

// --- dump (read-only, on request) ---------------------------------------------
void save_dump(const void* view) {
    constexpr size_t kBytes = 0x900;
    if (!pattern_scan::is_memory_valid(view, kBytes)) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    swprintf_s(dir, L"%s\\captures", bvr::log::data_dir());
    CreateDirectoryW(dir, nullptr);
    swprintf_s(dir, L"%s\\captures\\%04d%02d%02d_%02d%02d%02d", bvr::log::data_dir(), st.wYear,
               st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    CreateDirectoryW(dir, nullptr);
    swprintf_s(path, L"%s\\sceneview_eye%+d.bin", dir, g_eyeSign);
    if (FILE* f = _wfopen(path, L"wb")) {
        fwrite(view, 1, kBytes, f);
        fclose(f);
    }
    BVR_LOG("[bc] dump: FSceneView %p eye %+d -> %ls | game loc=(%.1f,%.1f,%.1f) rot=(%d,%d,%d)",
            view, g_eyeSign, path, g_loc[0], g_loc[1], g_loc[2], g_rot[0], g_rot[1], g_rot[2]);
}

// --- detours -------------------------------------------------------------------
int reentry_filter(unsigned code, EXCEPTION_POINTERS* ep) {
    if (code == 0xE06D7363u || code == EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
    g_lastExcCode.store(code);
    g_lastExcAddr.store(ep ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
                                 ep->ExceptionRecord->ExceptionAddress))
                           : 0);
    return EXCEPTION_EXECUTE_HANDLER;
}

bool call_draw_guarded(void* self, void* edx, uint32_t a1) {
    __try {
        g_drawOrig(self, edx, a1);
        return true;
    } __except (reentry_filter(GetExceptionCode(), GetExceptionInformation())) {
        return false;
    }
}

uint64_t g_lastBeat = 0;
uint32_t g_beatDraws = 0, g_beatSecond = 0, g_beatEdits = 0;
uint64_t g_beatPresents = 0;

void settings_tick(); // saved settings, below

void heartbeat() {
    const uint64_t now = GetTickCount64();
    if (!g_lastBeat) { g_lastBeat = now; return; }
    if (now - g_lastBeat < 5000) return;
    const double dt = (now - g_lastBeat) / 1000.0;
    const uint32_t d = g_draws.load(), s = g_secondDraws.load(), e = g_viewEdits.load();
    const uint64_t p = bvr::d3d11_hook::present_count();
    BVR_LOG("[bc] beat: draws %.0f/s 2nd %.0f/s presents %.0f/s view-edits %.0f/s | cam %s stereo %s%s | skips foreign %u noView %u noPresent %u | call2 %u us | hfov %.1f | xr %s | begins game %u render %u (in flight %d) labelled %u",
            (d - g_beatDraws) / dt, (s - g_beatSecond) / dt, (p - g_beatPresents) / dt,
            (e - g_beatEdits) / dt, vr_view_active() ? "ON" : "off",
            g_stereo.load() ? "ARMED" : "off", g_poisoned.load() ? " (POISONED)" : "",
            g_skipForeign.load(), g_skipNoView.load(), g_skipNoPresent.load(), g_call2Us.load(),
            g_lastHfov, bvr::vr::session_state_name(), g_gameBegins.load(), g_rtBegins.load(),
            int(g_gameBegins.load() - g_rtBegins.load()), g_rtLabelled.load());
    {   // AFW (§11): frames per eye, pairs warped / flat, the warp's cost and holes (rates since the last beat)
        static bool s_afwSeen = false;
        if (afw::on()) s_afwSeen = true;
        if (s_afwSeen) {
            char al[600];
            afw::status_line(al, sizeof(al));
            BVR_LOG("[bc] %s", al);
        }
    }
    g_rateDraws = static_cast<float>((d - g_beatDraws) / dt);
    g_rateSecond = static_cast<float>((s - g_beatSecond) / dt);
    g_ratePresents = static_cast<float>((p - g_beatPresents) / dt);
    g_beatDraws = d;
    g_beatSecond = s;
    g_beatEdits = e;
    g_beatPresents = p;
    g_lastBeat = now;
}

// Write the eye for the NEXT BeginDrawing (and the one after, for the rare path that issues
// two in one viewport draw) before the pass starts, so the render thread can never read a
// slot the game thread has not written yet.
void label_next_begin(int sign, bool afwFrame = false) {
    const uint32_t k = g_gameBegins.load(std::memory_order_relaxed);
    const uint32_t v = (g_poseValid ? (g_poseGen << 3) : 0u) | (afwFrame ? 4u : 0u) | static_cast<uint32_t>(sign + 1);
    g_eyeOfBegin[k % kEyeSlots].store(v, std::memory_order_relaxed);
    g_eyeOfBegin[(k + 1) % kEyeSlots].store(v, std::memory_order_release);
}

bool gameplayCaller0(uint32_t ra) { return ra == addr::kWrapperCallerTick; }

// Is a full-screen movie playing (opening logos, loading screens)? JJ 2026-10-04: the loading
// screen started "stuck to your face" - the game keeps drawing the world behind it for a few
// seconds, so the gameplay view looked live. The engine's own movie player answers directly.
// Game thread (UE3's GameThreadIsMoviePlaying); while the game thread is blocked loading a map
// the last answer (playing) stands.
using IsMoviePlayingFn = int(__thiscall*)(void*, const wchar_t*);
std::atomic<bool> g_moviePlaying{false};
int g_hudlineAfterMovie = 0; // test switch, read from settings.ini (not written back)
int g_panelseqAfterMovie = 0; // test switch: panel pictures for N HUD frames after a movie ends (not written back)
bool movie_playing_now() {
    void* fsm = *reinterpret_cast<void**>(addr::kGFullScreenMovie);
    if (!fsm || !pattern_scan::is_memory_valid(fsm, 4)) return false;
    void** vt = *reinterpret_cast<void***>(fsm);
    if (!vt || !pattern_scan::is_memory_valid(vt + addr::kMovieIsPlayingSlot, 4)) return false;
    __try {
        return reinterpret_cast<IsMoviePlayingFn>(vt[addr::kMovieIsPlayingSlot])(fsm, L"") != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// ---- the VR menu owns the controls while it is open (JJ 2026-10-04: "still letting button
// presses through", with 0 pad reads blocked: the game's controller reads were not passing
// through the XInput block at all - most likely Steam Input serving them). Blocked at the
// engine instead: every key, button and stick goes through UGameViewportClient::InputKey /
// InputAxis, whoever read it. Releases still pass so nothing stays held.
// Plus the menu gesture: a lone stick click is held back kStickWaitMs; if the other stick
// joins it is the menu gesture and the game never sees either click; if not it is delivered
// late (a quick tap is delivered on release, so it is never lost).
using InputKeyFn = int(__fastcall*)(void*, void*, void*, int, int, int, int, float, int);
using InputAxisFn = int(__fastcall*)(void*, void*, void*, int, int, int, float, float, int);
InputKeyFn g_inputKeyOrig = nullptr;
InputAxisFn g_inputAxisOrig = nullptr;
constexpr uint64_t kStickWaitMs = 120;
constexpr int kIE_Pressed = 0, kIE_Released = 1;
struct StickKey {
    bool down = false;       // physically down (as the events say)
    bool delivered = false;  // the game has seen its press
    bool pending = false;    // press held back, deciding
    uint64_t since = 0;
    void* self = nullptr; void* vp = nullptr; int ctrl = 0, n0 = 0, n1 = 0; float amt = 1.0f; int pad = 1;
};
StickKey g_stick[2];
bool g_stickChord = false;
uint64_t g_swallowed[16] = {}; // keys (name, controller) whose press the menu swallowed

int stick_slot(int nameIndex) {
    if (nameIndex == *reinterpret_cast<const int*>(addr::kNameLeftThumbstick)) return 0;
    if (nameIndex == *reinterpret_cast<const int*>(addr::kNameRightThumbstick)) return 1;
    return -1;
}
void deliver_press(StickKey& s) {
    s.pending = false;
    s.delivered = true;
    g_inputKeyOrig(s.self, nullptr, s.vp, s.ctrl, s.n0, s.n1, kIE_Pressed, s.amt, s.pad);
}
// Game thread, every frame: a held-back click whose wait ran out goes to the game now.
void stick_flush() {
    const uint64_t now = GetTickCount64();
    for (StickKey& s : g_stick)
        if (s.pending && now - s.since >= kStickWaitMs && !bvr::input::game_pad_blocked()) deliver_press(s);
}
int __fastcall input_key_detour(void* self, void* edx, void* vp, int ctrl, int n0, int n1, int ev, float amt, int pad) {
    const int k = stick_slot(n0);
    if (k >= 0) {
        StickKey& s = g_stick[k];
        StickKey& o = g_stick[k ^ 1];
        if (ev == kIE_Pressed) {
            s.down = true;
            if (o.down || o.pending) {
                g_stickChord = true; // the menu gesture: neither click is the game's
                s.pending = o.pending = false;
                return 1;
            }
            if (g_stickChord || bvr::input::game_pad_blocked()) return 1;
            s.pending = true;
            s.since = GetTickCount64();
            s.self = self; s.vp = vp; s.ctrl = ctrl; s.n0 = n0; s.n1 = n1; s.amt = amt; s.pad = pad;
            return 1;
        }
        if (ev == kIE_Released) {
            s.down = false;
            if (s.pending) deliver_press(s); // a quick tap: press now, release right after
            if (!s.down && !o.down) g_stickChord = false;
            if (!s.delivered) return 1;      // the game never saw this press
            s.delivered = false;
            return g_inputKeyOrig(self, edx, vp, ctrl, n0, n1, ev, amt, pad);
        }
        if (s.pending || !s.delivered) return 1; // repeats of a press the game has not seen
    }
    // Keys whose press the menu swallowed: their release (and repeats) are swallowed too. Arkham
    // City acts on some RELEASES (tap vs hold - detective mode toggled from a release that got
    // through, fake headset 2026-10-04). Releases of keys pressed before the menu opened pass.
    const uint64_t keyId = (static_cast<uint64_t>(static_cast<uint32_t>(n0)) << 32) | static_cast<uint32_t>(ctrl & 0xFF);
    if (ev == kIE_Released) {
        for (uint64_t& s : g_swallowed)
            if (s == keyId) { s = 0; return 1; }
        return g_inputKeyOrig(self, edx, vp, ctrl, n0, n1, ev, amt, pad);
    }
    if (bvr::input::game_pad_blocked()) {
        g_engineBlocked.fetch_add(1, std::memory_order_relaxed);
        bool known = false;
        for (uint64_t s : g_swallowed) known |= s == keyId;
        for (uint64_t& s : g_swallowed)
            if (!known && s == 0) { s = keyId; known = true; }
        return 1; // consumed: the menu is open
    }
    for (uint64_t s : g_swallowed)
        if (s == keyId) return 1; // a repeat of a swallowed press
    return g_inputKeyOrig(self, edx, vp, ctrl, n0, n1, ev, amt, pad);
}
int __fastcall input_axis_detour(void* self, void* edx, void* vp, int ctrl, int n0, int n1, float d, float dt, int pad) {
    if (bvr::input::game_pad_blocked()) {
        g_engineBlocked.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    return g_inputAxisOrig(self, edx, vp, ctrl, n0, n1, d, dt, pad);
}

// LOADING-SCREEN LAYOUT (JJ 2026-10-04: a black box over the objectives text at the top left).
// The loading screen lays itself out from the visible area it is given; in the tall VR picture
// (2480x2648) part of that layout lands over the text, at 16:9 (2560x1440) it is correct
// (fake headset, same save, both sizes). While a full-screen movie plays, Stage rectangles
// are trimmed to a centred 16:9 band, so the movie lays out as on a normal screen.
using StageRectFn = void*(__cdecl*)(void*, void*, const float*);
StageRectFn g_stageRectOrig = nullptr;
std::atomic<uint32_t> g_stageRectClamps{0};
void* __cdecl stage_rect_detour(void* out, void* env, const float* r) {
    if (r && movie_playing_now()) {
        const float w = r[2] - r[0], h = r[3] - r[1];
        if (w > 0.0f && h > w * 9.0f / 16.0f + 1.0f) {
            float c[4] = {r[0], r[1], r[2], r[3]};
            const float cy = (r[1] + r[3]) * 0.5f, half = w * 9.0f / 32.0f;
            c[1] = cy - half;
            c[3] = cy + half;
            if (g_stageRectClamps.fetch_add(1) < 3)
                BVR_LOG("[bc] movie layout: visible area %.0fx%.0f -> %.0fx%.0f (16:9)", w, h, w, c[3] - c[1]);
            return g_stageRectOrig(out, env, c);
        }
    }
    return g_stageRectOrig(out, env, r);
}

// MARKER PROBE (2026-10-04, the grapple reticle): every world point the game projects for its
// HUD this frame, with the screen point it got. Saved on F2 (targets.txt) next to a listing of
// every HUD draw's placement, to match markers to their targets.
struct ProjPt { float in[3]; float out[3]; uint32_t frame; };
ProjPt g_proj[256];
std::atomic<uint32_t> g_projN{0}, g_projTotal{0};
using V2SFn = float*(__fastcall*)(void*, void*, float*, float, float, float, void*);
V2SFn g_v2sOrig = nullptr;
float* __fastcall v2s_detour(void* self, void* edx, float* out, float x, float y, float z, void* p) {
    float* r = g_v2sOrig(self, edx, out, x, y, z, p);
    const uint32_t k = g_projN.fetch_add(1) % 256;
    g_projTotal.fetch_add(1, std::memory_order_relaxed);
    ProjPt& e = g_proj[k];
    e.in[0] = x; e.in[1] = y; e.in[2] = z;
    if (out) { e.out[0] = out[0]; e.out[1] = out[1]; e.out[2] = out[2]; }
    e.frame = g_draws.load(std::memory_order_relaxed);
    return r;
}
// Canvas.Project / Canvas.WorldToPixel script natives (UCanvas::execProject 0x8FDF60,
// execWorldToPixel 0x681F80): the result FVector (screen) is recorded; the world input stays 0.
using ExecFn = void(__fastcall*)(void*, void*, void*, void*);
ExecFn g_canvasProjOrig = nullptr, g_canvasW2POrig = nullptr;
void record_screen_only(const float* out, int kind) {
    if (!out) return;
    const uint32_t k = g_projN.fetch_add(1) % 256;
    g_projTotal.fetch_add(1, std::memory_order_relaxed);
    ProjPt& e = g_proj[k];
    e.in[0] = static_cast<float>(kind); e.in[1] = e.in[2] = 0.0f;
    e.out[0] = out[0]; e.out[1] = out[1]; e.out[2] = out[2];
    e.frame = g_draws.load(std::memory_order_relaxed);
}
void __fastcall canvas_proj_detour(void* self, void* edx, void* stack, void* result) {
    g_canvasProjOrig(self, edx, stack, result);
    record_screen_only(static_cast<const float*>(result), -1);
}
void __fastcall canvas_w2p_detour(void* self, void* edx, void* stack, void* result) {
    g_canvasW2POrig(self, edx, stack, result);
    record_screen_only(static_cast<const float*>(result), -2);
}

// VIEW PROJECTION PROBE (2026-10-07, the grapple reticle): the probes above saw 0 calls with a
// grapple point on screen. 0x972FB0 projects a world point through an FSceneView's
// ViewProjectionMatrix (view+0x250, row vectors): thiscall(view; FVector4* out, const FVector* in),
// ret 8, out = (x/w, y/w, z/w, w). Its Batman-side caller at 0xC57D0E projects object locations
// into on-screen / off-screen lists (the marker gatherer, to be confirmed here). Every call is
// recorded with its caller and the pass (eye) it ran in, plus each pass's own view matrix.
struct ViewProjCall { uint32_t ra; int eye; void* view; float in[3]; float out[4]; uint32_t frame; };
constexpr uint32_t kVpCalls = 1024;
ViewProjCall g_vpCalls[kVpCalls];
std::atomic<uint32_t> g_vpN{0};
using ViewProjFn = float*(__fastcall*)(void*, void*, float*, const float*);
ViewProjFn g_viewProjOrig = nullptr;
float* __fastcall view_proj_detour(void* view, void* edx, float* out, const float* in) {
    float* r = g_viewProjOrig(view, edx, out, in);
    // MARKEREYE: the HUD script's Canvas.Project (UCanvas::execProject's call returns here).
    if (_ReturnAddress() == reinterpret_cast<void*>(addr::kCanvasProjectRet))
        markers::on_project(g_eyeSign, g_draws.load(std::memory_order_relaxed), in, out);
    ViewProjCall& e = g_vpCalls[g_vpN.fetch_add(1, std::memory_order_relaxed) % kVpCalls];
    e.ra = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    e.eye = g_eyeSign;
    e.view = view;
    memcpy(e.in, in, 12);
    memcpy(e.out, out, 16);
    e.frame = g_draws.load(std::memory_order_relaxed);
    return r;
}
// FSceneView::WorldToScreen 0x971CE0 (same convention, no divide: out = clip x y z w). Its
// Batman-side caller 0xC582A1 sits in the same marker routine as 0xC57D0E.
ViewProjFn g_w2sOrig = nullptr;
float* __fastcall w2s_detour(void* view, void* edx, float* out, const float* in) {
    float* r = g_w2sOrig(view, edx, out, in);
    ViewProjCall& e = g_vpCalls[g_vpN.fetch_add(1, std::memory_order_relaxed) % kVpCalls];
    e.ra = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    e.eye = g_eyeSign;
    e.view = view;
    memcpy(e.in, in, 12);
    memcpy(e.out, out, 16);
    if (e.out[3] != 0.0f) { e.out[0] /= e.out[3]; e.out[1] /= e.out[3]; e.out[2] /= e.out[3]; }
    e.frame = g_draws.load(std::memory_order_relaxed);
    return r;
}
struct PassView { void* view; int eye; uint32_t frame; float vp[16]; float origin[3]; };
PassView g_passViews[8];
std::atomic<uint32_t> g_passViewN{0};
void record_pass_view(void* view, int eye) {
    PassView& p = g_passViews[g_passViewN.fetch_add(1, std::memory_order_relaxed) % 8];
    p.view = view;
    p.eye = eye;
    p.frame = g_draws.load(std::memory_order_relaxed);
    memcpy(p.vp, static_cast<const uint8_t*>(view) + 0x250, 64);
    memcpy(p.origin, static_cast<const uint8_t*>(view) + 0x3E0, 12);
}

void save_targets(const wchar_t* dir) {
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\targets.txt", dir);
    FILE* f = _wfopen(path, L"w");
    if (!f) return;
    const uint32_t n = g_projN.load(), now = g_draws.load();
    fprintf(f, "world -> screen projections, last 3 game frames (world -1 = Canvas.Project, -2 = WorldToPixel: screen only); total so far %u\n",
            g_projTotal.load());
    fprintf(f, "game camera loc (%.1f, %.1f, %.1f) rot (%d, %d, %d)\n", g_loc[0], g_loc[1], g_loc[2], g_rot[0],
            g_rot[1], g_rot[2]);
    for (uint32_t i = (n > 256 ? n - 256 : 0); i < n; ++i) {
        const ProjPt& e = g_proj[i % 256];
        if (now - e.frame > 3) continue;
        fprintf(f, "frame %u  world (%.1f, %.1f, %.1f)  ->  screen (%.2f, %.2f, %.3f)\n", e.frame, e.in[0], e.in[1],
                e.in[2], e.out[0], e.out[1], e.out[2]);
    }
    const uint32_t vn = g_vpN.load();
    fprintf(f, "\nFSceneView projection 0x972FB0, last 2 game frames (total so far %u); eye -1 left, +1 right, 0 mono\n", vn);
    for (uint32_t i = (vn > kVpCalls ? vn - kVpCalls : 0); i < vn; ++i) {
        const ViewProjCall& e = g_vpCalls[i % kVpCalls];
        if (now - e.frame > 2) continue;
        fprintf(f, "frame %u eye %+d caller %08X view %p  world (%.1f, %.1f, %.1f) -> ndc (%.4f, %.4f, %.5f) w %.1f\n",
                e.frame, e.eye, e.ra, e.view, e.in[0], e.in[1], e.in[2], e.out[0], e.out[1], e.out[2], e.out[3]);
    }
    {
        std::string ms;
        markers::status_text(ms);
        glow::status_text(ms);
        fputs(ms.c_str(), f);
    }
    fprintf(f, "\npass views (ViewProjectionMatrix at view+0x250, rows)\n");
    const uint32_t pn = g_passViewN.load();
    for (uint32_t i = (pn > 8 ? pn - 8 : 0); i < pn; ++i) {
        const PassView& p = g_passViews[i % 8];
        fprintf(f, "frame %u eye %+d view %p origin (%.2f, %.2f, %.2f)\n", p.frame, p.eye, p.view, p.origin[0],
                p.origin[1], p.origin[2]);
        for (int r = 0; r < 4; ++r)
            fprintf(f, "   %.6g %.6g %.6g %.6g\n", p.vp[r * 4], p.vp[r * 4 + 1], p.vp[r * 4 + 2], p.vp[r * 4 + 3]);
    }
    fclose(f);
}

void poll_movie() {
    const bool mp = movie_playing_now();
    if (mp != g_moviePlaying.exchange(mp)) {
        BVR_LOG("[bc] full-screen movie %s", mp ? "STARTED (logos / loading screen: flat screen in the room)" : "ended");
        // Test switch (settings debug_hudline_after_movie=N): a HUD timeline from the moment a movie ends.
        if (mp) bvr::gfx_hud::dump_hud_ring("full-screen movie started"); // HUDRING (test)
        if (mp) bvr::gfx_hud::note_movie_started(); // WIPEMAIN: the main menu -> game wipe is followed by the loading movie
        if (!mp) bvr::gfx_hud::note_movie_ended(); // FADEHOLD: the fade-in from black follows the loading movie
        if (!mp && g_hudlineAfterMovie > 0) bvr::gfx_hud::request_timeline(g_hudlineAfterMovie);
        if (!mp && g_panelseqAfterMovie > 0) {
            wchar_t dir[MAX_PATH];
            swprintf_s(dir, L"%s\\panelseq", bvr::log::data_dir());
            bvr::gfx_hud::request_panel_sequence(dir, g_panelseqAfterMovie);
        }
        bvr::vr::set_flat_screen(mp);
        vision::set_movie_playing(mp);
    }
}

// Hold the game at g_fpsLock: wait until a whole number of headset refreshes has passed since
// the last game frame (sleep, then spin the last 1.5 ms).
void fps_lock_wait() {
    const int lock = g_fpsLock.load(std::memory_order_relaxed);
    const bool repeat = g_fpsRepeat.load(std::memory_order_relaxed);
    bvr::vr::set_repeat_lock(repeat ? lock : 0);
    if (lock <= 0 || repeat) { g_lockLastQpc = 0; return; }
    const int64_t periodNs = bvr::vr::display_period_ns();
    if (periodNs <= 0) return;
    const double hz = 1e9 / double(periodNs);
    int div = static_cast<int>(hz / lock + 0.5);
    if (div < 1) div = 1;
    LARGE_INTEGER f, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&now);
    const int64_t slot = static_cast<int64_t>(double(periodNs) * div * f.QuadPart / 1e9);
    if (g_lockLastQpc && now.QuadPart - g_lockLastQpc < slot) {
        const int64_t target = g_lockLastQpc + slot;
        for (;;) {
            QueryPerformanceCounter(&now);
            const int64_t left = target - now.QuadPart;
            if (left <= 0) break;
            const double ms = double(left) * 1000.0 / f.QuadPart;
            if (ms > 1.5) Sleep(static_cast<DWORD>(ms - 1.0));
            else YieldProcessor();
        }
        g_lockLastQpc = target;
    } else {
        g_lockLastQpc = now.QuadPart;
    }
}

void __fastcall client_draw_detour(void* self, void* edx, void* viewport, void* canvas) {
    g_gameBegins.fetch_add(1, std::memory_order_release); // this pass's BeginDrawing is queued
    g_clientDrawOrig(self, edx, viewport, canvas);
}

uint32_t __fastcall begin_exec_detour(void* self, void* edx) {
    const uint32_t k = g_rtBegins.fetch_add(1, std::memory_order_acq_rel);
    const uint32_t v = g_eyeOfBegin[k % kEyeSlots].load(std::memory_order_acquire);
    const int sign = static_cast<int>(v & 3u) - 1;
    bvr::vr::sr_set_present_gen(v >> 3); // exact pose tag: the locate this frame was drawn from
    bvr::vr::sr_set_present_afw(sign != 0 && (v & 4u) != 0); // AFW: this present carries the whole pair
    bvr::vr::sr_set_present_eye(sign);
    afw::set_render_slot(k);
    room_overlay::set_render_slot(k);
    reflect::set_render_pass(k, sign);
    vision::set_render_pass(sign); // DETVR
    markers::set_render_slot(k);
    billboard::set_render_slot(k);
    dlss_bridge::set_render_slot(k);
    if (sign) g_rtLabelled.fetch_add(1, std::memory_order_relaxed);
    return g_beginExecOrig(self, edx);
}

void __fastcall draw_detour(void* self, void* edx, uint32_t a1) {    const uint32_t ra = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    if (g_depth > 0) { // nested (should not happen) - pass straight through
        g_drawOrig(self, edx, a1);
        return;
    }
    g_drawCallers.note(ra);
    g_draws.fetch_add(1, std::memory_order_relaxed);
    if (g_autoVr.load() && !g_autoDone && bvr::vr::ever_focused()) {
        g_autoDone = true;
        BVR_LOG("[bc] auto VR: session focused - arming camera + stereo");
        set_stereo(true); // arms the camera too
    }
    // Not under a full-screen movie (JJ 2026-10-04: a black box at the top left of the loading
    // screen): the loading screen is 2D art the HUD panel would cut out of the picture.
    bvr::gfx_hud::set_redirect(g_hudQuad.load(std::memory_order_relaxed) && bvr::vr::session_live() &&
                               !g_moviePlaying.load(std::memory_order_relaxed));
    ++g_depth;

    // One head sample per frame, shared by both eyes (re-sampling between the
    // passes gives vertical disparity - BioShock Infinite, ENGINE_NOTES).
    poll_keys();
    window::tick(); // opening logos: a pad button skips them
    poll_movie();
    stick_flush();
    if (gameplayCaller0(ra)) fps_lock_wait();
    {
        // Meta button held: the runtime recentred its space - take the current head as straight ahead.
        static uint32_t s_rtRecenters = 0;
        const uint32_t n = bvr::vr::runtime_recenter_count();
        if (n != s_rtRecenters) {
            s_rtRecenters = n;
            g_recenterReq.store(true);
            BVR_LOG("[bc] runtime recentre (Meta button) - view recentred");
        }
    }
    g_poseValid = vr_view_active() && bvr::vr::get_head_pose_gen(g_pose, &g_poseGen);
    if (g_poseValid) apply_recenter(g_pose);
    {   // WIPEMAIN2 test (JJ tests by hand): a capture 0.4 s into each of the first 3 wipe replays of a session.
        static uint32_t s_replays = 0, s_autoCaps = 0;
        static ULONGLONG s_capAt = 0;
        const uint32_t r = bvr::gfx_hud::replays_started();
        if (r != s_replays) {
            s_replays = r;
            if (s_autoCaps < 3 && bvr::gfx_hud::wipe_debug()) s_capAt = GetTickCount64() + 400; // WIPEDEBUG
        }
        if (s_capAt && GetTickCount64() >= s_capAt) {
            s_capAt = 0;
            ++s_autoCaps;
            BVR_LOG("[bc] auto capture 0.4 s into the wipe replay (%u of 3)", s_autoCaps);
            g_captureReq.store(true);
        }
    }
    if (g_captureReq.exchange(false)) save_capture();

    // Gate both tags and the doubling identically so they can never go one-sided.
    const bool gameplayCaller = ra == addr::kWrapperCallerTick;
    // No second eye under a full-screen movie: the picture shown is the movie, on a flat screen.
    const bool doubling = stereo_active() && g_poseValid && gameplayCaller && g_viewLastDraw &&
                          !g_moviePlaying.load(std::memory_order_relaxed);
    if (stereo_active() && !gameplayCaller) g_skipForeign.fetch_add(1, std::memory_order_relaxed);
    if (stereo_active() && gameplayCaller && !g_viewLastDraw)
        g_skipNoView.fetch_add(1, std::memory_order_relaxed);

    if (doubling && g_state1On.load(std::memory_order_relaxed) && !g_eye1State) {
        g_eye1State = reinterpret_cast<AllocStateFn>(addr::kAllocateViewState)();
        BVR_LOG("[bc] left-eye view state allocated: %p", g_eye1State);
    }
    if (doubling && g_state2On.load(std::memory_order_relaxed) && !g_eye2State) {
        g_eye2State = reinterpret_cast<AllocStateFn>(addr::kAllocateViewState)();
        BVR_LOG("[bc] right-eye view state allocated: %p", g_eye2State);
    }
    g_viewThisDraw = false;
    // AFW: no second draw - this frame's one draw is eye g_afwNext; its present submits the whole pair.
    g_afwFrame = doubling && afw::on();
    g_eyeSign = g_afwFrame ? (g_afwNext ? +1 : -1) : doubling ? -1 : 0;
    label_next_begin(g_eyeSign, g_afwFrame);
    g_drawOrig(self, edx, a1);
    if (g_afwFrame && g_viewThisDraw) {
        afw::note_drawn(g_afwNext);
        g_afwNext ^= 1;
    }

    const uint64_t present = bvr::d3d11_hook::present_count();
    const bool presented = present != g_lastDrawPresent;
    g_lastDrawPresent = present;

    if (doubling && !g_afwFrame && g_viewThisDraw) {
        if (!presented) {
            // No present since the last draw (hitch / unfocused). The left tag is
            // already queued; the core self-heals a lone tag, but count it.
            g_skipNoPresent.fetch_add(1, std::memory_order_relaxed);
        }
        g_eyeSign = +1;
        label_next_begin(+1);
        LARGE_INTEGER t0, t1, f;
        QueryPerformanceCounter(&t0);
        bvr::vr::set_draw_stage("bcSecondDraw");
        const bool ok = call_draw_guarded(self, edx, a1);
        bvr::vr::set_draw_stage(nullptr);
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        g_call2Us.store(static_cast<uint32_t>((t1.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart));
        if (ok) {
            g_secondDraws.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_poisoned.store(true);
            BVR_LOG("[bc] second draw FAULTED code=0x%08X at %08X - stereo POISONED (doubling off; `bc stereo reset`)",
                    g_lastExcCode.load(), g_lastExcAddr.load());
        }
    }
    g_eyeSign = 0;
    g_afwFrame = false;
    g_viewLastDraw = g_viewThisDraw;
    --g_depth;
    if (gameplayCaller) {
        tele_record();
        vision::tick();
    }
    heartbeat();
    settings_tick();
}

// MENUCLOCK (JJ 2026-10-09: HUD group adjustments broke menu screens - "only in the main game, not the main menu,
// pause menu, map or any other screen"; the map is not caught by the pause-dirt signal). The view family the engine
// builds every frame carries its clocks (world time stops in any in-game menu, real time runs on). Their offsets
// are not known, so every dword of the family's first 192 bytes is watched as a float: a field that keeps rising
// by a frame-sized step (0.0005 .. 0.25 s) for 30+ frames is a clock. "In a menu" = one established clock has stood
// still for 3+ frames while another one still rises.
struct ClockField { float prev = 0; int up = 0; int still = 0; bool clock = false; };
ClockField g_clock[48];
uint32_t g_clockFrame = 0xFFFFFFFF;
std::atomic<bool> g_gamePaused{false};
std::atomic<float> g_pauseHalo{0.8f}; // PAUSEHALO strength while paused (panel slider, saved pause_text_halo)
std::atomic<int> g_clockFields{0};
void watch_clocks(const void* family) {
    const uint32_t frame = g_draws.load(std::memory_order_relaxed);
    if (frame == g_clockFrame || !family || !pattern_scan::is_memory_valid(family, 192)) return;
    g_clockFrame = frame;
    float f[48];
    memcpy(f, family, sizeof(f));
    int clocks = 0, running = 0, stopped = 0;
    for (int i = 0; i < 48; ++i) {
        ClockField& c = g_clock[i];
        const float d = f[i] - c.prev;
        if (std::isfinite(f[i]) && d > 0.0005f && d < 0.25f) {
            c.still = 0;
            if (++c.up >= 30) c.clock = true;
        } else if (std::isfinite(f[i]) && d == 0.0f && c.clock) {
            ++c.still;
        } else {
            c.up = 0;
            c.still = 0;
            c.clock = false;
        }
        c.prev = f[i];
        if (c.clock) {
            ++clocks;
            if (c.still == 0) ++running;
            else if (c.still >= 3) ++stopped;
        }
    }
    g_clockFields.store(clocks, std::memory_order_relaxed);
    const bool paused = running > 0 && stopped > 0;
    bvr::gfx_hud::set_clock_stopped(paused); // MAPBLITTAIL + UNPAUSECUT
    if (paused != g_gamePaused.exchange(paused))
        BVR_LOG("[bc] game clock %s (clock fields %d, running %d, stopped %d)", paused ? "STOPPED - in a menu" : "running",
                clocks, running, stopped);
}

// WIPESOUND (JJ 2026-10-11: the reveal after quitting to the main menu has "no audio ... There should be" - the bats on
// the way to black are the game's own wipe (sound included, only replaced on screen); the reveal has no game wipe).
// Try 1 (URGFxMovie PlaySound 0xE8F090) heard nothing (log 04:16: no menu sound at all). Try 2: Wwise is linked into the
// exe and exported: AK::SoundEngine::PostEvent(AkUniqueID event, AkGameObjectID object, flags, callback, cookie,
// numExternals, externals) = 0x1057640 (__cdecl, returns the playing id; the string versions 0x1056D40 / 0x1056CB0 hash
// and call it). Every posted event is noted; events posted when no wipe is starting (menu clicks, music, gameplay) count as
// "common". The wipe's sound = an event posted within 0.7 s before / 0.4 s after a wipe to black starts that is not
// common. The reveal posts it again (game thread) on the game object of the newest event (the main menu's own, still
// registered after the level change). Events around wipes are logged (first 80).
// Try 2 hooked the id version only and heard nothing at the wipe (log 04:21): the name versions 0x1056D40 / 0x1056CB0 do
// not call it. All three end in 0x1056BD0 (cdecl: event id, game object, flags, callback, cookie, externals - id first:
// the name versions push the hashed name last; externals may be null, checked at +0x4C) - hooked there now.
using AkPostEventFn = uint32_t(__cdecl*)(uint32_t eventId, uint32_t gameObject, uint32_t flags, void* callback,
                                         void* cookie, void* externals);
AkPostEventFn g_akPostOrig = nullptr;
constexpr uintptr_t kAkPostEvent = 0x1056BD0;
SRWLOCK g_soundLock = SRWLOCK_INIT;
// WIPESOUND5 (JJ 2026-10-11 22:16: after the unpause bat transition the audio was gone - the learning had picked the
// menu-open sound, 2209210028, and the reveal posted it in gameplay). The bat sound is exact: 3904589100 = FNV-1 hash of
// "play_ui_fader_bats" (Origins posts Play_UI_Fader_Bats by name). The reveal posts it on the newest game object.
constexpr uint32_t kBatSoundId = 3904589100u; // Play_UI_Fader_Bats
uint32_t g_wipeSoundId = kBatSoundId, g_newestObj = 0;
std::atomic<bool> g_playWipeSound{false}, g_wipeSoundOn{true};
uint32_t __cdecl ak_post_detour(uint32_t id, uint32_t obj, uint32_t flags, void* cb, void* cookie, void* ext) {
    AcquireSRWLockExclusive(&g_soundLock);
    g_newestObj = obj;
    ReleaseSRWLockExclusive(&g_soundLock);
    return g_akPostOrig(id, obj, flags, cb, cookie, ext);
}
void wipe_sound_event(int kind) { // render thread (gfx_hud): 1 = the reveal starts
    if (kind == 1 && g_wipeSoundOn.load(std::memory_order_relaxed)) g_playWipeSound.store(true);
}
void play_wipe_sound_now() { // game thread
    AcquireSRWLockExclusive(&g_soundLock);
    const uint32_t id = g_wipeSoundId, obj = g_newestObj;
    ReleaseSRWLockExclusive(&g_soundLock);
    if (!id || !g_akPostOrig) return;
    uint32_t r = 0;
    __try {
        r = g_akPostOrig(id, obj, 0, nullptr, nullptr, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BVR_LOG("[bc] WIPESOUND: posting event %u FAULTED - switched off", id);
        g_wipeSoundOn.store(false);
        return;
    }
    BVR_LOG("[bc] WIPESOUND: event %u posted with the reveal on object %u -> playing id %u%s", id, obj, r,
            r ? "" : " (REFUSED by Wwise)");
}

void* g_localPlayer = nullptr; // game thread (FIRSTPERSON)
void fp_update(); // FIRSTPERSON, below

void* __fastcall calc_detour(void* self, void* edx, void* family, float* loc, int32_t* rot,
                             void* viewport, void* drawer) {
    g_calcCallers.note(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())));
    g_calcs.fetch_add(1, std::memory_order_relaxed);
    watch_clocks(family);
    {   // MENUCLOCK: HUD groups only in gameplay; menus, the map and the title screen take the menu panel size
        const bool title = room_overlay::title_screen_now();
        const bool paused = g_gamePaused.load(std::memory_order_relaxed);
        // MS2 (JJ 2026-10-11 22:41: after unpausing "the HUD elements are scaled down for about a second and then they
        // scale back out"; the old unpause bat replay used to cover it). LIVE 22:40: the pause screen's grey pass held
        // "menu" 500 ms past the menu's Close, which comes ~340 ms after its closing wipe starts. The game's own calls
        // (MENUSTATE) say when a menu opens and closes: the menu counts from its opening to its Close, and from the moment
        // its closing wipe starts the HUD is the gameplay HUD again (`bc closezones 0` = only from the Close).
        const ULONGLONG nowMs = GetTickCount64();
        static ULONGLONG s_runningSince = 0;
        if (paused) s_runningSince = 0; else if (!s_runningSince) s_runningSince = nowMs;
        const bool probe = menu_state::live();
        // MS9 (MS2 RESULTS 22:51: the safety dropped almost every menu the moment it opened - it counted from the last
        // unpause, minutes earlier - so the closing-wipe rule never ran and the HUD still shrank). It counts from the later
        // of the menu's opening and the clock running again.
        const ULONGLONG openAt = menu_state::menu_open_at();
        const ULONGLONG runFrom = s_runningSince > openAt ? s_runningSince : openAt;
        if (probe && menu_state::menu_open() && s_runningSince && nowMs - runFrom > 2500)
            menu_state::clear_stale("the game has run for 2.5 s"); // a missed Close must not keep the menu state
        // MS9 (JJ: the upgrades screen "sometimes takes several seconds"; LIVE 22:52:53 / 22:53:07: the game's opening wipe
        // starts ~40 ms after the menu takes focus; counting the menu at once made that wipe an in-menu one = the 4 s hold
        // cap). A menu counts for the HUD from 300 ms after it opens; its opening wipe still starts "from gameplay".
        const bool menuUp = probe ? (menu_state::menu_open() && nowMs - openAt >= 300) : room_overlay::pause_screen_now();
        const ULONGLONG wipeAt = bvr::gfx_hud::menu_wipe_at();
        // From the closing wipe's start until the game runs again (the Close comes ~340 ms after the wipe, the clock
        // ~30-90 ms after the Close) - capped at 1.5 s.
        const bool closing = probe && wipeAt > openAt &&
                             nowMs - wipeAt < 1500 && (menu_state::menu_open() || menu_state::menu_closed_at() >= wipeAt);
        const bool gameplayHud = (closing || (!paused && !menuUp)) && !title && !movie_playing_now();
        // HIDEWIPES: the pause screen's own grey pass counts at once (the clock signal can lag).
        bvr::gfx_hud::set_hud_zones_gameplay(gameplayHud);
        bvr::vr::set_hud_menu_now(((paused || menuUp) && !closing) || title);
        bvr::vr::set_hud_menu_hint(!probe || (menu_state::menu_open() && !closing)); // MS11
        // WIPEMAIN: the pause menu's pieces sit at the main menu's placement (~1.9), so the wipe logic needs this too.
        bvr::gfx_hud::set_main_menu_now(title && !paused && !room_overlay::pause_screen_now());
        // PAUSEHALO: the pause screen's own grey pass shows at once; the clock signal lags it by seconds (run 32: 7 s).
        const bool pauseScreen = (paused || room_overlay::pause_screen_now()) && !closing;
        bvr::gfx_hud::set_halo(pauseScreen && !title ? g_pauseHalo.load(std::memory_order_relaxed) : 0.0f);
    }
    if (g_playWipeSound.exchange(false)) play_wipe_sound_now(); // WIPESOUND
    g_localPlayer = self; // ULocalPlayer (FIRSTPERSON: +0x30 = the player controller)
    fp_update();          // before the view is built (the FSceneView ctor runs inside)
    void* view = g_calcOrig(self, edx, family, loc, rot, viewport, drawer);
    if (view) {
        if (loc && pattern_scan::is_memory_valid(loc, 12)) memcpy(g_loc, loc, 12);
        if (rot && pattern_scan::is_memory_valid(rot, 12)) memcpy(g_rot, rot, 12);
        // In stereo, start a two-view dump on the LEFT pass so both eyes of one frame are saved.
        int want = g_dumpRequested.load();
        if (want > 0 && (want == 1 || g_eyeSign <= 0)) {
            g_dumpRequested.store(want - 1);
            save_dump(view);
            const float* o = reinterpret_cast<const float*>(static_cast<const uint8_t*>(view) + 0x3E0);
            if (g_eyeSign != 0 && g_dumpOriginEye == -g_eyeSign) {
                const float dx = o[0] - g_dumpOrigin[0], dy = o[1] - g_dumpOrigin[1], dz = o[2] - g_dumpOrigin[2];
                const float d = sqrtf(dx * dx + dy * dy + dz * dz);
                BVR_LOG("[bc] dump: eye ViewOrigin separation %.3f UU (expected ipd %.1f mm x %.1f UU/m = %.3f UU)",
                        d, g_ipdMm.load(), g_uuPerM.load(), g_ipdMm.load() * 0.001f * g_uuPerM.load());
            }
            memcpy(g_dumpOrigin, o, 12);
            g_dumpOriginEye = g_eyeSign;
        }
    }
    return view;
}

// FIRSTPERSON (JJ 2026-10-09: "a first person mode with a controller button combination ... everywhere, but only as
// an option in the overlay or with a button combo. For now just do the camera"; "put the camera in front of his face
// for now" - his head is not hidden yet). The look direction stays the game camera's (right stick, head on top); only
// the camera's POSITION moves: Batman's eye point (APawn Location +0x48 + (0, 0, EyeHeight +0x2A8) - the engine's own
// APawn::GetPawnViewLocation 0x7A6970, vtbl+0x440; a collision-cylinder point, so no animation bob) plus fp_forward
// along the camera's level forward (his head always behind the lens) and fp_up. Static RE 2026-10-09: LocalPlayer
// (CalcSceneView's this) +0x30 = PlayerController (0x7ED830 GetALocalPlayerController); APawn +0x1CC = Controller
// (APawn::GetViewRotation 0x7BA650). The controller's own Pawn field is found LIVE: the pointer in the controller
// whose +0x1CC points back at it. No pawn (menus, some cutscenes) = the game's camera as before.
std::atomic<bool> g_fp{false};
std::atomic<float> g_fpFwdCm{25.0f};
std::atomic<float> g_fpUpCm{0.0f};
int g_fpPawnOff = 0;           // controller field that holds the pawn (found live)
float g_fpEye[3] = {};
bool g_fpEyeValid = false;     // this frame's eye point (game thread, before the ctor)
std::atomic<uint32_t> g_fpFrames{0};
constexpr int kPCPawnScanBegin = 0x1AC, kPCSize = 0x590, kPawnController = 0x1CC, kPawnLocation = 0x48,
              kPawnEyeHeight = 0x2A8, kPawnSizeMin = 0x444;
// HEADCAM (JJ 2026-10-11: "we need the camera attached to the head ... when flying the head comes forward and it goes
// through the camera ... attached at a fixed distance from the head"): the eye point follows Batman's head bone. Static RE
// (re/BatmanAC.exe.unpacked.exe): USkeletalMeshComponent::GetBoneLocation 0x850EE0 (thiscall: FVector* out, FName,
// int space 0 = world; ret 0x10; zero vector when the bone is not found; called by execGetBoneLocation 0x8540B0);
// "Bip01_Head" is hard-coded FName 0x620 (name table 0x47C5C1, Bip01_Neck1 0x61F); the game itself calls it with
// pawn +0x36C (= Pawn.Mesh, 0xD505B7); SkeletalMeshComponent vtable 0x13158C8, size 0x6C0, its SkeletalMesh +0x1A0.
// References: MELE VR convo_fp.cpp (head bone used only within a box around the steady eye point, smoothed: "a head bone
// bobs"), BS Infinite VR bones.h; playbook CAM-001 (head pose once). The head - eye-point offset is smoothed (fp_head_smooth
// seconds, real time - the two eye passes of a frame get the same point) and capped; outside the cap = the steady eye.
std::atomic<bool> g_fpHead{true};
std::atomic<float> g_fpHeadSmoothS{0.08f};
float g_fpHeadOff[3] = {};
bool g_fpHeadOffValid = false;
LARGE_INTEGER g_fpHeadLastT = {};
std::atomic<uint32_t> g_fpHeadFrames{0}, g_fpHeadMisses{0};
constexpr uintptr_t kGetBoneLocation = 0x850EE0;
constexpr int kPawnMesh = 0x36C, kMeshSkelMesh = 0x1A0, kSkelMeshCompSize = 0x6C0;
constexpr uint32_t kNameBip01Head = 0x620;
constexpr float kHeadCapUU = 140.0f; // ~1.5 m: anything further is not his head
using GetBoneLocationFn = float*(__thiscall*)(void* mesh, float* out, uint32_t nameIndex, uint32_t nameNumber, int space);
void fp_head_follow(uintptr_t pawn); // below

bool fp_object(uintptr_t p, unsigned bytes) {
    if (p < 0x10000 || (p & 3) || !pattern_scan::is_memory_valid(reinterpret_cast<void*>(p), bytes)) return false;
    const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(p);
    return vt >= 0x401000 && vt < 0x1800000; // a C++ object of the exe (vtable in its image)
}

void fp_update() {
    g_fpEyeValid = false;
    if (!g_fp.load(std::memory_order_relaxed) || !g_localPlayer) return;
    __try {
        const uintptr_t pc = *reinterpret_cast<const uintptr_t*>(static_cast<const uint8_t*>(g_localPlayer) + 0x30);
        if (!fp_object(pc, kPCSize)) return;
        auto pawn_at = [pc](int off) -> uintptr_t {
            const uintptr_t q = *reinterpret_cast<const uintptr_t*>(pc + off);
            if (q == pc || !fp_object(q, kPawnSizeMin)) return 0;
            return *reinterpret_cast<const uintptr_t*>(q + kPawnController) == pc ? q : 0;
        };
        uintptr_t pawn = g_fpPawnOff ? pawn_at(g_fpPawnOff) : 0;
        if (!pawn) {
            static ULONGLONG s_lastScan = 0;
            const ULONGLONG now = GetTickCount64();
            if (now - s_lastScan < 500) return;
            s_lastScan = now;
            for (int off = kPCPawnScanBegin; off < kPCSize && !pawn; off += 4)
                if ((pawn = pawn_at(off)) != 0) {
                    if (off != g_fpPawnOff)
                        BVR_LOG("[bc] first person: Batman = controller +0x%X (pawn %08X)", off, unsigned(pawn));
                    g_fpPawnOff = off;
                }
            if (!pawn) return;
        }
        const float* loc = reinterpret_cast<const float*>(pawn + kPawnLocation);
        const float eyeH = *reinterpret_cast<const float*>(pawn + kPawnEyeHeight);
        if (!std::isfinite(loc[0]) || !std::isfinite(loc[1]) || !std::isfinite(loc[2]) || !std::isfinite(eyeH) ||
            eyeH < -50.0f || eyeH > 300.0f)
            return;
        g_fpEye[0] = loc[0]; g_fpEye[1] = loc[1]; g_fpEye[2] = loc[2] + eyeH;
        // FPGATE (JJ 2026-10-11 02:24: "no 3D geometry is showing on the main menu anymore" with first person on): the
        // main menu has a stand-in pawn of its own (log: eye -3632, 864, -80, no mesh) far from the menu scene's camera,
        // and first person moved the camera there. First person only in gameplay: not on the title screen, not in
        // movies, and only while the game's camera is near Batman (third person sits ~2-4 m behind him).
        {
            const float dx = g_fpEye[0] - g_loc[0], dy = g_fpEye[1] - g_loc[1], dz = g_fpEye[2] - g_loc[2];
            const float dist = sqrtf(dx * dx + dy * dy + dz * dz), maxUU = 12.0f * g_uuPerM.load(std::memory_order_relaxed);
            // JJ 02:2x: "must always be disabled when going into main menu ... or any other loading screen and menus"
            const char* why = room_overlay::title_screen_now() ? "title screen / main menu"
                              : movie_playing_now()             ? "movie / loading screen"
                              : g_gamePaused.load(std::memory_order_relaxed) || room_overlay::pause_screen_now()
                                  ? "menu (pause, map, upgrades)"
                              : !(dist < maxUU)                 ? "game camera far from Batman"
                                                                : nullptr;
            static const char* s_why = "";
            if ((why ? why : "") != s_why) {
                s_why = why ? why : "";
                BVR_LOG("[bc] first person %s%s (game camera %.1f m from his eyes)", why ? "paused: " : "", why ? why : "active again",
                        dist / g_uuPerM.load(std::memory_order_relaxed));
            }
            if (why) return; // the game's own camera this frame
        }
        g_fpEyeValid = true;
        fp_head_follow(pawn); // HEADCAM: g_fpEye moved with his head
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            BVR_LOG("[bc] first person: eye (%.1f, %.1f, %.1f) = location + eye height %.1f; game camera (%.1f, %.1f, %.1f)",
                    g_fpEye[0], g_fpEye[1], g_fpEye[2], eyeH, g_loc[0], g_loc[1], g_loc[2]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_fpEyeValid = false;
    }
}

void fp_head_follow(uintptr_t pawn) { // game thread, inside fp_update's __try
    if (!g_fpHead.load(std::memory_order_relaxed)) { g_fpHeadOffValid = false; return; }
    auto miss = [](const char* why) {
        g_fpHeadOffValid = false;
        if (g_fpHeadMisses.fetch_add(1, std::memory_order_relaxed) < 6) BVR_LOG("[bc] first person head: %s - steady eye point", why);
    };
    const uintptr_t mesh = *reinterpret_cast<const uintptr_t*>(pawn + kPawnMesh);
    if (!fp_object(mesh, kSkelMeshCompSize)) return miss("no mesh at pawn +0x36C");
    const uintptr_t skel = *reinterpret_cast<const uintptr_t*>(mesh + kMeshSkelMesh);
    if (!fp_object(skel, 0x100)) return miss("mesh has no skeleton");
    float head[3] = {};
    reinterpret_cast<GetBoneLocationFn>(kGetBoneLocation)(reinterpret_cast<void*>(mesh), head, kNameBip01Head, 0, 0);
    if (!std::isfinite(head[0]) || !std::isfinite(head[1]) || !std::isfinite(head[2]) ||
        (head[0] == 0.0f && head[1] == 0.0f && head[2] == 0.0f))
        return miss("no Bip01_Head bone");
    const float d[3] = {head[0] - g_fpEye[0], head[1] - g_fpEye[1], head[2] - g_fpEye[2]};
    if (fabsf(d[0]) > kHeadCapUU || fabsf(d[1]) > kHeadCapUU || fabsf(d[2]) > kHeadCapUU) {
        char b[96];
        snprintf(b, sizeof(b), "head %.0f, %.0f, %.0f UU from the eye point (cap %.0f)", d[0], d[1], d[2], kHeadCapUU);
        return miss(b);
    }
    LARGE_INTEGER now, f;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    const float dt = g_fpHeadLastT.QuadPart ? float(double(now.QuadPart - g_fpHeadLastT.QuadPart) / double(f.QuadPart)) : 0.0f;
    g_fpHeadLastT = now;
    const float tau = g_fpHeadSmoothS.load(std::memory_order_relaxed);
    const float a = !g_fpHeadOffValid || tau <= 0.001f ? 1.0f : 1.0f - expf(-(dt > 0.1f ? 0.1f : dt) / tau);
    for (int i = 0; i < 3; ++i) {
        g_fpHeadOff[i] += (d[i] - g_fpHeadOff[i]) * a;
        g_fpEye[i] += g_fpHeadOff[i];
    }
    g_fpHeadOffValid = true;
    if (g_fpHeadFrames.fetch_add(1, std::memory_order_relaxed) == 0)
        BVR_LOG("[bc] first person head: mesh %08X (vtable %08X), Bip01_Head - eye point = %.1f, %.1f, %.1f UU (%u frames)",
                unsigned(mesh), unsigned(*reinterpret_cast<const uintptr_t*>(mesh)), d[0], d[1], d[2],
                g_fpHeadFrames.load(std::memory_order_relaxed));
}

// The game camera's view matrix (row vectors: v_view = (v - origin) * R, translation row = -origin * R) moved to the
// eye point. Columns of R: 0 right, 1 up, 2 forward (world).
void fp_apply(M4& view) {
    if (!g_fpEyeValid) return;
    float fx = view.m[0][2], fy = view.m[1][2];
    const float len = sqrtf(fx * fx + fy * fy);
    if (len > 1e-3f) { fx /= len; fy /= len; } else { fx = fy = 0.0f; } // straight down / up: no forward push
    const float uu = g_uuPerM.load(std::memory_order_relaxed) * 0.01f; // per cm
    const float fwd = g_fpFwdCm.load(std::memory_order_relaxed) * uu, up = g_fpUpCm.load(std::memory_order_relaxed) * uu;
    const float o[3] = {g_fpEye[0] + fx * fwd, g_fpEye[1] + fy * fwd, g_fpEye[2] + up};
    for (int j = 0; j < 3; ++j) view.m[3][j] = -(o[0] * view.m[0][j] + o[1] * view.m[1][j] + o[2] * view.m[2][j]);
    g_fpFrames.fetch_add(1, std::memory_order_relaxed);
}
void set_first_person(bool on) {
    if (g_fp.exchange(on) != on) BVR_LOG("[bc] first person %s", on ? "ON" : "off");
}
void fp_toggle() { set_first_person(!g_fp.load()); } // both sticks clicked twice quickly

void* __fastcall ctor_detour(void* self, void* edx, BC_A27) {
    const uint32_t ra = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    g_ctorCallers.note(ra);
    if (ra != addr::kCtorRetFromCalcSceneView || g_depth == 0) {
        // Another view (scene capture, ...) built with the game's own player view state?
        const uint32_t gs = g_gameState.load(std::memory_order_relaxed);
        if (gs && a2 == gs && g_foreignSameState.fetch_add(1, std::memory_order_relaxed) < 3)
            BVR_LOG("[bc] view state %08X (the game's own, the left eye's unless STATE1) also used by a view built "
                    "from %08X", gs, ra);
        return g_ctorOrig(self, edx, BC_P27(a14, a15));
    }
    if (a2) g_gameState.store(a2 == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_eye1State)) ||
                                      a2 == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_eye2State))
                                  ? g_gameState.load(std::memory_order_relaxed)
                                  : a2,
                              std::memory_order_relaxed);
    g_viewThisDraw = true;
    if (!g_poseValid || !pattern_scan::is_memory_valid(reinterpret_cast<void*>(a14), 64) ||
        !pattern_scan::is_memory_valid(reinterpret_cast<void*>(a15), 64))
        return g_ctorOrig(self, edx, BC_P27(a14, a15));

    // View: game camera view * (head pose + eye offset), in the camera's own view space.
    M4 view, proj;
    memcpy(&view, reinterpret_cast<void*>(a14), 64);
    memcpy(&proj, reinterpret_cast<void*>(a15), 64);
    fp_apply(view); // FIRSTPERSON: the camera at Batman's eyes (everything below follows the moved camera)
    if ((g_eyeSign <= 0 || g_afwFrame) && proj.m[0][0] > 0.0f) // AFW: every frame's only pass
        vision::on_game_fov(2.0f * atanf(1.0f / proj.m[0][0]) * 180.0f / kPi); // the game's OWN fov (zoom)
    const M4 delta = eye_delta(g_pose, g_eyeSign);
    const M4 eyeView = mul(view, delta);
    // BILLBOARD: particles get the game camera's axes instead of this eye's.
    billboard::set_pass(g_gameBegins.load(std::memory_order_relaxed) - 1, &eyeView.m[0][0], &view.m[0][0]);

    // Projection: symmetric frustum circumscribing the headset eye at the view's
    // aspect (core computes it from the backbuffer); keep the engine's depth terms.
    float rx = *reinterpret_cast<const float*>(&a10), ry = *reinterpret_cast<const float*>(&a11);
    float w = *reinterpret_cast<const float*>(&a12);
    float h = *reinterpret_cast<const float*>(&a13);
    if (rx != g_lastRect[0] || ry != g_lastRect[1] || w != g_lastRect[2] || h != g_lastRect[3]) {
        unsigned bw = 0, bh = 0;
        bvr::hud::backbuffer_dims(&bw, &bh);
        BVR_LOG("[bc] game view rect %.0f,%.0f %.0fx%.0f (backbuffer %ux%u)%s", rx, ry, w, h, bw, bh,
                g_fullRect.load() ? " - widening to the full backbuffer" : "");
        g_lastRect[0] = rx; g_lastRect[1] = ry; g_lastRect[2] = w; g_lastRect[3] = h;
    }
    unsigned bw = 0, bh = 0;
    if (g_fullRect.load() && bvr::hud::backbuffer_dims(&bw, &bh) && bw && bh) {
        rx = 0.0f; ry = 0.0f; w = float(bw); h = float(bh);
        a10 = *reinterpret_cast<const uint32_t*>(&rx);
        a11 = *reinterpret_cast<const uint32_t*>(&ry);
        a12 = *reinterpret_cast<const uint32_t*>(&w);
        a13 = *reinterpret_cast<const uint32_t*>(&h);
    }
    const float aspect = (w > 0 && h > 0) ? w / h : 1.0f;
    float hfov = bvr::vr::suggested_hfov_deg();
    if (!(hfov > 10.0f && hfov < 170.0f)) hfov = 100.0f;
    const float tanH = tanf(hfov * 0.5f * kPi / 180.0f);
    const float tanV = tanH / aspect;
    proj.m[0][0] = 1.0f / tanH;
    proj.m[1][1] = 1.0f / tanV;
    proj.m[2][0] = 0.0f;
    proj.m[2][1] = 0.0f;
    // MARKERBASE: the game camera's own view-projection (no head, no eye, symmetric) - the head-free
    // reference that HUD markers are placed from.
    const M4 baseVp = mul(view, proj);
    const M4 projSym = proj; // AFW: the other eye's lens starts from the same symmetric lens
    // Asymmetric per-eye frustum (stereo passes only): the runtime's own eye angles, the
    // same ones core tags the layer with. Row-vector UE3 projection: clip.x = x*m00 + z*m20,
    // clip.w = z, so x_ndc = (2*x/z - (r+l)) / (r-l) with l,r,u,d as tangents. Depth row kept.
    float ang[4];
    if (g_eyeSign != 0 && bvr::vr::asym_fov() && bvr::vr::eye_fov_angles(g_eyeSign < 0 ? 0 : 1, ang)) {
        const float l = tanf(ang[0]), r = tanf(ang[1]), u = tanf(ang[2]), d = tanf(ang[3]);
        proj.m[0][0] = 2.0f / (r - l);
        proj.m[2][0] = -(r + l) / (r - l);
        proj.m[1][1] = 2.0f / (u - d);
        proj.m[2][1] = -(u + d) / (u - d);
    }
    g_lastHfov = hfov;
    bvr::vr::set_rendered_hfov(hfov);
    bvr::vr::publish_gameplay_view(true);
    g_viewEdits.fetch_add(1, std::memory_order_relaxed);

    // DLSS (DLSS_PLAN.md D1): sub-pixel jitter in the lens, the same for both eyes of a frame.
    float dlssJx = 0.0f, dlssJy = 0.0f;
    const M4 projU = proj; // AFW: the drawn eye's lens without the jitter
    // AFW: each eye is drawn every other frame - half the frame count, so each eye walks the whole jitter sequence
    const uint32_t jitterFrame = g_draws.load(std::memory_order_relaxed) >> (g_afwFrame ? 1 : 0);
    if (g_eyeSign != 0 && dlss_bridge::jitter(jitterFrame, int(w), int(h), &dlssJx, &dlssJy)) {
        proj.m[2][0] += 2.0f * dlssJx / w;
        proj.m[2][1] += -2.0f * dlssJy / h;
    }
    // Room overlays (ROOMGRUNGE): game-camera view space -> this pass's clip space. This pass's
    // BeginDrawing index is the one client Draw just counted.
    const M4 baseToClip = mul(delta, proj);
    room_overlay::set_pass(g_gameBegins.load(std::memory_order_relaxed) - 1, &baseToClip.m[0][0], g_uuPerM.load());

    if (g_eyeSign > 0 && g_eye2State && g_state2On.load(std::memory_order_relaxed))
        a2 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_eye2State));
    if (g_eyeSign < 0 && g_eye1State && g_state1On.load(std::memory_order_relaxed))
        a2 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_eye1State));
    void* r = g_ctorOrig(self, edx,
                         BC_P27(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&eyeView)),
                                static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&proj))));
    record_pass_view(self, g_eyeSign);
    dlss_bridge::set_pass(g_gameBegins.load(std::memory_order_relaxed) - 1, g_eyeSign,
                          reinterpret_cast<const float*>(static_cast<const uint8_t*>(self) + 0x250), &eyeView.m[0][0],
                          dlssJx, dlssJy, int(w), int(h));
    markers::set_pass(g_gameBegins.load(std::memory_order_relaxed) - 1, g_eyeSign,
                      g_draws.load(std::memory_order_relaxed),
                      reinterpret_cast<const float*>(static_cast<const uint8_t*>(self) + 0x250), &baseVp.m[0][0]);
    if (g_afwFrame && g_eyeSign != 0) {
        // AFW: the other eye from the SAME head sample (its eye offset, its own lens angles, no jitter) = where the warp
        // puts this eye's picture. All cameras from our own matrices (the ones handed to the engine), one space.
        const int os = -g_eyeSign;
        const M4 oView = mul(view, eye_delta(g_pose, os));
        M4 oProj = projSym;
        if (bvr::vr::asym_fov() && bvr::vr::eye_fov_angles(os < 0 ? 0 : 1, ang)) {
            const float l = tanf(ang[0]), rr = tanf(ang[1]), u = tanf(ang[2]), d = tanf(ang[3]);
            oProj.m[0][0] = 2.0f / (rr - l);
            oProj.m[2][0] = -(rr + l) / (rr - l);
            oProj.m[1][1] = 2.0f / (u - d);
            oProj.m[2][1] = -(u + d) / (u - d);
        }
        const M4 vpJ = mul(eyeView, proj), vpU = mul(eyeView, projU), dstVp = mul(oView, oProj);
        afw::set_pass(g_gameBegins.load(std::memory_order_relaxed) - 1, g_eyeSign < 0 ? 0 : 1, &vpJ.m[0][0], &vpU.m[0][0],
                      &dstVp.m[0][0], oProj.m[2][2], oProj.m[3][2], int(w), int(h));
        if (g_afwVpLogs.fetch_add(1, std::memory_order_relaxed) < 2) {
            // the engine's own view x lens (view+0x250) against ours: equal = the warp's cameras are the ones drawn with
            const float* e = reinterpret_cast<const float*>(static_cast<const uint8_t*>(self) + 0x250);
            float dmax = 0, rel = 0;
            for (int i = 0; i < 16; ++i) {
                const float dv = fabsf(e[i] - (&vpJ.m[0][0])[i]);
                if (dv > dmax) { dmax = dv; rel = dv / (fabsf(e[i]) + 1e-6f); }
            }
            BVR_LOG("[afw] eye %c camera check: engine view x lens vs ours, largest difference %g (relative %g); other eye "
                    "lens P22 %g P32 %g, render %dx%d", g_eyeSign < 0 ? 'L' : 'R', dmax, rel, oProj.m[2][2], oProj.m[3][2],
                    int(w), int(h));
        }
    }
    return r;
}

// --- install ---------------------------------------------------------------------
bool wait_for_plaintext(uintptr_t va, const uint8_t* expect, size_t n, const char* name) {
    for (int i = 0; i < 600; ++i) {
        if (pattern_scan::is_memory_valid(reinterpret_cast<const void*>(va), n) &&
            memcmp(reinterpret_cast<const void*>(va), expect, n) == 0) {
            if (i) BVR_LOG("[bc] %s plaintext after %d ms", name, i * 50);
            return true;
        }
        Sleep(50);
    }
    BVR_LOG("[bc] %s at %08X never showed its expected prologue - NOT hooked", name, unsigned(va));
    return false;
}

bool hook(uintptr_t va, void* detour, void** orig, const char* name) {
    const MH_STATUS c = MH_CreateHook(reinterpret_cast<void*>(va), detour, orig);
    const MH_STATUS e = c == MH_OK ? MH_EnableHook(reinterpret_cast<void*>(va)) : c;
    BVR_LOG("[bc] hook %s @%08X: %s", name, unsigned(va), MH_StatusToString(e));
    return e == MH_OK;
}

void set_ipd_tag() { bvr::vr::set_eye_tag_ipd_mm(g_ipdMm.load()); }

// SAVED SETTINGS (JJ 2026-10-03: "make it so any changes to the overlay are saved").
// %LOCALAPPDATA%\Batman_City\settings.ini, one "key=value" per line. Not wired per widget: the
// whole set is rendered to text a few times a second and written when it differs from what was
// last written, half a second after the last change - so every overlay control, F-key and `bc`
// command is covered, and a slider drag writes once. VR resolution stays in launch.ini (the
// launcher reads it before the game starts). Head tracking on/off is not saved on purpose: every
// launch starts in VR.
// 2 (2026-10-04): wider zoom vignette defaults - older files' vignette size is not carried over.
constexpr int kSettingsVersion = 2;
std::string settings_text() {
    char buf[1024];
    float hd = 0, hw = 0, hu = 0, sd = 0, sw = 0, pw = 0, pd = 0;
    bvr::vr::get_hud_quad(&hd, &hw, &hu);
    bvr::vr::get_screen_quad(&sd, &sw);
    bvr::vr::get_panel_size(&pw, &pd);
    sprintf_s(buf,
              "world_scale=%.2f\neye_distance_mm=%.2f\nright_eye_history=%d\nleft_eye_history=%d\nhead_position=%d\n"
              "eye_lens_shape=%d\nkeep_focus=%d\nhud_in_world=%d\nhud_on_head=%d\n"
              "hud_distance_m=%.3f\nhud_size_m=%.3f\nhud_height_m=%.3f\n"
              "screen_distance_m=%.3f\nscreen_size_m=%.3f\nmenu_distance_m=%.3f\nmenu_size_m=%.3f\n"
              "fps_lock=%d\nfps_repeat=%d\nexact_pose=%d\npose_delay=%d\nwiden_letterbox=%d\nauto_vr=%d\n"
              "opening_logos=%d\nroom_screen=%d\nsettings_version=%d\n",
              g_uuPerM.load(), g_ipdMm.load(), g_state2On.load() ? 1 : 0, g_state1On.load() ? 1 : 0, g_posOn.load() ? 1 : 0,
              bvr::vr::asym_fov() ? 1 : 0, window::keep_focus() ? 1 : 0, g_hudQuad.load() ? 0 : 1,
              bvr::vr::hud_head_locked() ? 1 : 0, hd, hw, hu, sd, sw, pd, pw,
              g_fpsLock.load(), g_fpsRepeat.load() ? 1 : 0, bvr::vr::exact_pose_tag() ? 1 : 0,
              bvr::vr::get_pose_lag(), g_fullRect.load() ? 1 : 0, g_autoVr.load() ? 1 : 0,
              window::show_logos() ? 1 : 0, bvr::vr::screen_room() ? 1 : 0, kSettingsVersion);
    std::string s = "; Batman City settings - written by the mod whenever the overlay changes\n";
    s += buf;
    vision::settings_write(s);
    room_overlay::settings_write(s);
    markers::settings_write(s);
    reflect::settings_write(s);
    s += billboard::enabled() ? "particles_face_game_camera=1\n" : "particles_face_game_camera=0\n";
    glow::settings_write(s);
    afw::settings_write(s);
    s += screen_hd::settings_line(); // LOADTIME: HD loading pictures on / off
    // TIDY: the bat-wipe / menu-fade switches are no longer saved (fixed on).
    {
        char hb[48];
        snprintf(hb, sizeof(hb), "pause_text_halo=%.2f\n", g_pauseHalo.load());
        s += hb;
    }
    {
        char fb[192];
        snprintf(fb, sizeof(fb), "first_person=%d\nfp_forward_cm=%.1f\nfp_up_cm=%.1f\nfp_head=%d\nfp_head_smooth=%.3f\n",
                 g_fp.load() ? 1 : 0, g_fpFwdCm.load(), g_fpUpCm.load(), g_fpHead.load() ? 1 : 0, g_fpHeadSmoothS.load());
        s += fb;
    }
    s += "hud_eye_depth=" + std::to_string(bvr::vr::hud_eyes()) + "\n";
    s += bvr::gfx_hud::hud_zones_on() ? "hud_zones_on=1\n" : "hud_zones_on=0\n";
    {
        float md = 0, mw = 0;
        bvr::vr::get_hud_menu_quad(&md, &mw);
        char mb[96];
        snprintf(mb, sizeof(mb), "hud_map_distance_m=%.3f\nhud_map_size_m=%.3f\n", md, mw);
        s += mb;
    }
    for (int z = 0; z < bvr::gfx_hud::kHudZones; ++z) {
        float zs = 1, zx = 0, zy = 0;
        bvr::gfx_hud::get_hud_zone(z, &zs, &zx, &zy);
        char zb[80];
        snprintf(zb, sizeof(zb), "hud_zone_%d=%.2f,%.2f,%.2f\n", z, zs, zx, zy);
        s += zb;
    }
    s += "dlss_mode=" + std::to_string(dlss_bridge::saved_mode()) + "\n";
    return s;
}

void settings_path(wchar_t* out, size_t n) { swprintf_s(out, n, L"%s\\settings.ini", bvr::log::data_dir()); }

std::string g_settingsSaved;    // what the file holds (game thread)
std::string g_settingsPending;  // changed text waiting for the quiet period
ULONGLONG g_settingsChangedMs = 0, g_settingsCheckMs = 0;

void settings_load() {
    wchar_t path[MAX_PATH];
    settings_path(path, MAX_PATH);
    FILE* f = _wfopen(path, L"r");
    if (!f) {
        BVR_LOG("[bc] settings: none saved yet - defaults");
        g_settingsSaved = settings_text();
        return;
    }
    char line[256];
    int n = 0;
    float hd = 0, hw = 0, hu = 0, sd = 0, sw = 0, pw = 0, pd = 0, mapD = 0, mapW = 0;
    bvr::vr::get_hud_quad(&hd, &hw, &hu);
    bvr::vr::get_screen_quad(&sd, &sw);
    bvr::vr::get_panel_size(&pw, &pd);
    int asym = -1;
    int version = 1;
    {
        // settings_version is written near the end; look it up first so older keys can be judged.
        char vl[256];
        while (fgets(vl, sizeof(vl), f))
            if (!strncmp(vl, "settings_version=", 17)) version = atoi(vl + 17);
        rewind(f);
    }
    std::string raw; // SAVEALL: the file as it was, to see whether it holds every current setting
    while (fgets(line, sizeof(line), f)) {
        raw += line;
        char* eq = strchr(line, '=');
        if (line[0] == ';' || !eq) continue;
        *eq = 0;
        char* val = eq + 1;
        val[strcspn(val, "\r\n")] = 0;
        const char* k = line;
        const float v = static_cast<float>(atof(val));
        const bool on = atoi(val) != 0;
        ++n;
        if (!strcmp(k, "world_scale")) { if (v >= 10.0f && v <= 400.0f) g_uuPerM.store(v); }
        else if (!strcmp(k, "eye_distance_mm")) { if (v >= 40.0f && v <= 90.0f) g_ipdMm.store(v); }
        else if (!strcmp(k, "right_eye_history")) g_state2On.store(on);
        else if (!strcmp(k, "left_eye_history")) g_state1On.store(on);
        else if (!strcmp(k, "head_position")) g_posOn.store(on);
        else if (!strcmp(k, "eye_lens_shape")) asym = on ? 1 : 0;
        else if (!strcmp(k, "keep_focus")) window::set_keep_focus(on);
        else if (!strcmp(k, "hud_in_world")) g_hudQuad.store(!on);
        else if (!strcmp(k, "hud_on_head")) bvr::vr::set_hud_head_locked(on);
        else if (!strcmp(k, "hud_eye_depth")) bvr::vr::set_hud_eyes(atoi(val));
        else if (!strcmp(k, "hud_zones_on")) bvr::gfx_hud::set_hud_zones_on(on);
        else if (!strcmp(k, "hud_map_distance_m")) mapD = v;
        else if (!strcmp(k, "hud_map_size_m")) mapW = v;
        else if (!strcmp(k, "hud_distance_m")) hd = v;
        else if (!strcmp(k, "hud_size_m")) hw = v;
        else if (!strcmp(k, "hud_height_m")) hu = v;
        else if (!strcmp(k, "hide_grunge")) {} // old switch: JJ 2026-10-07 wants the grunge back, in the room
        else if (!strcmp(k, "screen_distance_m")) sd = v;
        else if (!strcmp(k, "screen_size_m")) sw = v;
        else if (!strcmp(k, "menu_distance_m")) pd = v;
        else if (!strcmp(k, "menu_size_m")) pw = v;
        else if (!strcmp(k, "fps_lock")) { const int l = atoi(val); g_fpsLock.store(l == 45 || l == 40 || l == 30 ? l : 0); }
        else if (!strcmp(k, "fps_repeat")) g_fpsRepeat.store(on);
        else if (!strcmp(k, "exact_pose")) bvr::vr::set_exact_pose_tag(on);
        else if (!strcmp(k, "pose_delay")) bvr::vr::set_pose_lag(atoi(val));
        else if (!strcmp(k, "widen_letterbox")) g_fullRect.store(on);
        else if (!strcmp(k, "auto_vr")) g_autoVr.store(on);
        else if (!strcmp(k, "opening_logos")) window::set_show_logos(on);
        else if (!strcmp(k, "room_screen")) bvr::vr::set_screen_room(on);
        else if (!strcmp(k, "settings_version")) {}
        else if (version < 2 && (!strcmp(k, "vig_clear_deg") || !strcmp(k, "vig_fade_deg"))) {} // new defaults win
        else if (room_overlay::settings_read(k, val)) {}
        else if (markers::settings_read(k, val)) {}
        else if (reflect::settings_read(k, val)) {}
        else if (glow::settings_read(k, val)) {}
        else if (afw::settings_read(k, val)) {}
        else if (screen_hd::settings_read(k, val)) {}
        else if (!strcmp(k, "particles_face_game_camera")) billboard::set_enabled(on);
        else if (!strcmp(k, "transition_cuts_full_view") || !strcmp(k, "hide_loading_fade_box") ||
                 !strcmp(k, "hide_menu_wipes") || !strcmp(k, "wipe_replay") || !strcmp(k, "wipe_flip") ||
                 !strcmp(k, "wipe_panel_off") || !strcmp(k, "black_fill") || !strcmp(k, "wipe_debug") ||
                 !strcmp(k, "menu_fade_black")) {} // TIDY: fixed on (wipe_debug off) - old values ignored
        else if (!strcmp(k, "pause_text_halo")) { if (v >= 0.0f && v <= 1.0f) g_pauseHalo.store(v); }
        else if (!strcmp(k, "first_person")) g_fp.store(on);
        else if (!strcmp(k, "fp_forward_cm")) { if (v >= -20.0f && v <= 80.0f) g_fpFwdCm.store(v); }
        else if (!strcmp(k, "fp_up_cm")) { if (v >= -40.0f && v <= 40.0f) g_fpUpCm.store(v); }
        else if (!strcmp(k, "fp_head")) g_fpHead.store(on); // HEADCAM
        else if (!strcmp(k, "fp_head_smooth")) { if (v >= 0.0f && v <= 1.0f) g_fpHeadSmoothS.store(v); }
        else if (!strncmp(k, "hud_zone_", 9)) {
            float zs = 1, zx = 0, zy = 0;
            if (sscanf_s(val, "%f,%f,%f", &zs, &zx, &zy) == 3 && zs >= 0.3f && zs <= 1.5f && fabsf(zx) <= 0.6f && fabsf(zy) <= 0.6f)
                bvr::gfx_hud::set_hud_zone(atoi(k + 9), zs, zx, zy);
        }
        else if (!strcmp(k, "debug_hudline_after_movie")) g_hudlineAfterMovie = atoi(val);
        else if (!strcmp(k, "debug_panelseq_after_movie")) g_panelseqAfterMovie = atoi(val);
        else if (!strcmp(k, "debug_hud_ring")) bvr::gfx_hud::set_hud_ring(atoi(val) != 0);
        else if (!strcmp(k, "dlss_mode")) { if (!dlss_bridge::launched()) dlss_bridge::set_saved_mode(atoi(val)); } // VR: window.cpp
        else if (!vision::settings_read(k, val)) { --n; BVR_LOG("[bc] settings: unknown key '%s' ignored", k); }
    }
    fclose(f);
    if (hd > 0.1f && hw > 0.1f) bvr::vr::set_hud_quad(hd, hw, hu);
    if (mapD > 0.1f && mapW > 0.1f) bvr::vr::set_hud_menu_quad(mapD, mapW);
    if (sd > 0.1f && sw > 0.1f) bvr::vr::set_screen_quad(sd, sw);
    if (pd > 0.1f && pw > 0.1f) bvr::vr::set_panel_size(pw, pd);
    if (asym >= 0) bvr::vr::set_asym_fov(asym != 0);
    set_ipd_tag();
    // SAVEALL (2026-10-12, packaging: JJ "make sure all the settings are saved"): the file was only written on a change,
    // so settings added in a newer build (marker_upright / marker_face) never reached it. A file that is not exactly
    // the current set is rewritten once, half a second after the start (settings_tick), with every setting in it.
    const std::string now = settings_text();
    g_settingsSaved = raw == now ? now : std::string();
    BVR_LOG("[bc] settings: %d loaded from %ls%s", n, path, raw == now ? "" : " - rewriting it with every current setting");
}

// Game thread, every frame; does real work at most 4x a second.
void settings_tick() {
    const ULONGLONG now = GetTickCount64();
    if (now - g_settingsCheckMs < 250) return;
    g_settingsCheckMs = now;
    std::string cur = settings_text();
    if (cur != g_settingsPending) {
        g_settingsPending = cur;
        g_settingsChangedMs = now;
        return;
    }
    if (cur == g_settingsSaved || now - g_settingsChangedMs < 500) return;
    wchar_t path[MAX_PATH], tmp[MAX_PATH];
    settings_path(path, MAX_PATH);
    swprintf_s(tmp, L"%s.tmp", path);
    if (FILE* f = _wfopen(tmp, L"w")) {
        fputs(cur.c_str(), f);
        fclose(f);
        if (MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
            g_settingsSaved = cur;
            BVR_LOG("[bc] settings saved");
            return;
        }
    }
    g_settingsSaved = cur; // never retry a failing write every 250 ms
    BVR_LOG("[bc] settings: SAVE FAILED (%ls)", path);
}

} // namespace

bool install() {
    static const uint8_t kDraw[] = {0x6A, 0xFF, 0x68, 0x22, 0x8F, 0x15, 0x01, 0x64, 0xA1, 0x00, 0x00, 0x00};
    static const uint8_t kCalc[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x6A, 0xFF, 0x68, 0x99, 0x22, 0x16};
    static const uint8_t kCtor[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x6A, 0xFF, 0x68, 0x43, 0xF4, 0x16};
    if (!wait_for_plaintext(addr::kViewportDraw, kDraw, sizeof(kDraw), "viewport draw") ||
        !wait_for_plaintext(addr::kCalcSceneView, kCalc, sizeof(kCalc), "CalcSceneView") ||
        !wait_for_plaintext(addr::kSceneViewCtor, kCtor, sizeof(kCtor), "FSceneView ctor"))
        return false;
    bool ok = hook(addr::kViewportDraw, reinterpret_cast<void*>(&draw_detour),
                   reinterpret_cast<void**>(&g_drawOrig), "viewport draw");
    ok = hook(addr::kCalcSceneView, reinterpret_cast<void*>(&calc_detour),
              reinterpret_cast<void**>(&g_calcOrig), "CalcSceneView") && ok;
    ok = hook(addr::kSceneViewCtor, reinterpret_cast<void*>(&ctor_detour),
              reinterpret_cast<void**>(&g_ctorOrig), "FSceneView ctor") && ok;
    static const uint8_t kClient[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x6A, 0xFF, 0x68, 0xFF, 0x24, 0x16};
    static const uint8_t kBegin[] = {0x8B, 0x49, 0x04, 0x8B, 0x01, 0x8B, 0x50, 0x64, 0xFF, 0xD2, 0xB8, 0x08};
    if (wait_for_plaintext(addr::kGameViewportClientDraw, kClient, sizeof(kClient), "client Draw") &&
        wait_for_plaintext(addr::kBeginDrawingExecute, kBegin, sizeof(kBegin), "BeginDrawing execute")) {
        ok = hook(addr::kGameViewportClientDraw, reinterpret_cast<void*>(&client_draw_detour),
                  reinterpret_cast<void**>(&g_clientDrawOrig), "client Draw") && ok;
        ok = hook(addr::kBeginDrawingExecute, reinterpret_cast<void*>(&begin_exec_detour),
                  reinterpret_cast<void**>(&g_beginExecOrig), "BeginDrawing execute") && ok;
        static const uint8_t kV2S[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x54, 0x8B};
        if (wait_for_plaintext(addr::kVectorToScreen, kV2S, sizeof(kV2S), "VectorToScreen"))
            hook(addr::kVectorToScreen, reinterpret_cast<void*>(&v2s_detour), reinterpret_cast<void**>(&g_v2sOrig),
                 "VectorToScreen (marker probe)");
        static const uint8_t kCanvasProj[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x38, 0x56};
        static const uint8_t kCanvasW2P[] = {0x83, 0xEC, 0x18, 0x53, 0x56, 0x8B, 0x74, 0x24, 0x24, 0x8B};
        if (wait_for_plaintext(0x008FDF60, kCanvasProj, sizeof(kCanvasProj), "Canvas.Project"))
            hook(0x008FDF60, reinterpret_cast<void*>(&canvas_proj_detour), reinterpret_cast<void**>(&g_canvasProjOrig),
                 "Canvas.Project (marker probe)");
        if (wait_for_plaintext(0x00681F80, kCanvasW2P, sizeof(kCanvasW2P), "Canvas.WorldToPixel"))
            hook(0x00681F80, reinterpret_cast<void*>(&canvas_w2p_detour), reinterpret_cast<void**>(&g_canvasW2POrig),
                 "Canvas.WorldToPixel (marker probe)");
        static const uint8_t kViewProj[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x45, 0x0C, 0xF3, 0x0F, 0x10};
        if (wait_for_plaintext(addr::kSceneViewProject, kViewProj, sizeof(kViewProj), "FSceneView project"))
            hook(addr::kSceneViewProject, reinterpret_cast<void*>(&view_proj_detour),
                 reinterpret_cast<void**>(&g_viewProjOrig), "FSceneView project (marker probe)");
        static const uint8_t kW2S[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x45, 0x0C, 0xF3, 0x0F, 0x10};
        if (wait_for_plaintext(addr::kSceneViewWorldToScreen, kW2S, sizeof(kW2S), "FSceneView WorldToScreen"))
            hook(addr::kSceneViewWorldToScreen, reinterpret_cast<void*>(&w2s_detour),
                 reinterpret_cast<void**>(&g_w2sOrig), "FSceneView WorldToScreen (marker probe)");
        static const uint8_t kAkPost[] = {0x83, 0xEC, 0x30, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x56}; // sub esp,30; mov eax,1; push esi
        if (wait_for_plaintext(kAkPostEvent, kAkPost, sizeof(kAkPost), "Wwise PostEvent"))
            hook(kAkPostEvent, reinterpret_cast<void*>(&ak_post_detour), reinterpret_cast<void**>(&g_akPostOrig),
                 "Wwise PostEvent (WIPESOUND)");
        bvr::gfx_hud::set_wipe_event_hook(&wipe_sound_event);
        static const uint8_t kStageRect[] = {0x83, 0xEC, 0x20, 0x53, 0x8B, 0x5C, 0x24, 0x28, 0x56, 0x57};
        if (wait_for_plaintext(addr::kStageRectToValue, kStageRect, sizeof(kStageRect), "Stage rect"))
            hook(addr::kStageRectToValue, reinterpret_cast<void*>(&stage_rect_detour),
                 reinterpret_cast<void**>(&g_stageRectOrig), "Stage rect (movie layout 16:9)");
        static const uint8_t kKey[] = {0x55, 0x56, 0x57, 0x8B, 0xE9, 0x33, 0xFF, 0x8D, 0x75, 0xD4};
        static const uint8_t kAxis[] = {0x53, 0x55, 0x56, 0x57, 0x8B, 0xE9, 0x33, 0xFF, 0x8D, 0x75};
        if (wait_for_plaintext(addr::kInputKey, kKey, sizeof(kKey), "InputKey") &&
            wait_for_plaintext(addr::kInputAxis, kAxis, sizeof(kAxis), "InputAxis")) {
            hook(addr::kInputKey, reinterpret_cast<void*>(&input_key_detour), reinterpret_cast<void**>(&g_inputKeyOrig), "InputKey (VR menu input block)");
            hook(addr::kInputAxis, reinterpret_cast<void*>(&input_axis_detour), reinterpret_cast<void**>(&g_inputAxisOrig), "InputAxis (VR menu input block)");
        }
    } else {
        ok = false;
    }
    g_live.store(ok);
    bvr::vr::sr_use_render_thread_eyes(true);
    bvr::gfx_hud::set_armed(true);
    // JJ 2026-10-04 (a HUD piece "too far apart" in the two eyes, capture 035226): BioShock's
    // "few vertices = full-screen flash, leave it in the eye image" rule caught Arkham City's
    // ordinary HUD pieces (4-12 vertex shapes, e.g. a 90x83 rendered widget) - drawn into both
    // eyes at one screen position, so they never fuse. Every UI piece goes to the HUD panel.
    bvr::gfx_hud::set_fx_passthrough(false);
    bvr::frame_inspector::set_draw_skip(&grunge_skip);
    bvr::frame_inspector::set_cb_shadow(true); // MARKEREYE reads each HUD piece's placement
    reflect::install();                         // REFLECTEYE: the reflection capture renders for each eye
    menu_state::install();                      // MENUSTATE probe: the game's own menu calls, logged by name
    dlss_bridge::install();                     // DLSS hand-off to the 64-bit helper (off until switched on)
    afw::install();                             // AFW: the core asks it for the undrawn eye (off until chosen)
    bvr::gfx_hud::set_before_capture_hook(&screen_hd::logo_frame); // HD LOGO: HD title / tech cards over the logo video
    bvr::gfx_hud::set_marker_fn(&markers::on_hud_draw, &markers::after_hud_draw);
    bvr::gfx_hud::set_cuts_to_eye(true); // BATCUTS: the bat-wipe cuts fill the whole view
    vision::install();
    bvr::vr::set_hud_texture_provider(&bvr::gfx_hud::texture);
    bvr::overlay::set_chord_double_tap(&fp_toggle);

    // UE3 threaded renderer: displayed content is two locate generations old
    // (BioShock Infinite s43b; AKVR posedelay=2). Rendered-pose eye tags: the layer
    // describes the parallel pair we render (base +- ipd/2 along head right).
    bvr::vr::set_pose_lag(2);
    bvr::vr::set_eye_tag_rendered(true);
    set_ipd_tag();
    // Asymmetric per-eye frusta (sim-verified 2026-10-03: far-object disparity matches the
    // eye geometry). F10 / `bc asym off` for the A/B.
    bvr::vr::set_asym_fov(true);
    settings_load(); // JJ's saved overlay choices, over the defaults above
    return ok;
}

bool hook_live() { return g_live.load(); }

void set_camera(bool on) {
    if (on) {
        bvr::vr::set_enabled(true);
        bvr::vr::set_camera_mode(true);
    } else {
        set_stereo(false);
        bvr::vr::set_camera_mode(false);
    }
    g_camera.store(on);
    BVR_LOG("[bc] camera %s (head drive + headset FOV + projection layer)", on ? "ON" : "off");
}

void set_stereo(bool on) {
    if (on) {
        if (!g_camera.load()) set_camera(true);
        bvr::vr::set_sr_pair_pacing(true);
        bvr::vr::set_pace_sync(true);
    } else {
        bvr::vr::set_pace_sync(false);
        bvr::vr::set_alternate_eye(false);
    }
    g_stereo.store(on);
    BVR_LOG("[bc] stereo %s (same-frame: per-frame draw doubled, L then R)%s", on ? "ARMED" : "off",
            g_poisoned.load() ? " - NOTE: poisoned, `bc stereo reset` first" : "");
}

bool handle_command(const char* args) {
    if (!args) args = "";
    auto starts = [&](const char* w) { return _strnicmp(args, w, strlen(w)) == 0; };
    if (starts("cam on")) set_camera(true);
    else if (starts("cam off")) set_camera(false);
    else if (starts("stereo on")) set_stereo(true);
    else if (starts("stereo off")) set_stereo(false);
    else if (starts("stereo reset")) { g_poisoned.store(false); BVR_LOG("[bc] stereo poison cleared"); }
    else if (starts("dlss ")) dlss_bridge::command(args + 5);
    else if (starts("sticknav ")) bvr::overlay::set_stick_nav(atoi(args + 9) != 0); // STICKNAV (panel scroll)
    else if (starts("mapscreen ")) bvr::gfx_hud::set_map_screen(atoi(args + 10) != 0); // MAPSCREEN
    else if (starts("menuhold ")) bvr::gfx_hud::set_menu_open_hold(atoi(args + 9)); // MENUOPEN
    else if (starts("openfade ")) bvr::gfx_hud::set_open_fade(atoi(args + 9) != 0); // OPENFADE
    else if (starts("glowzone ")) bvr::gfx_hud::set_glow_zone(atoi(args + 9)); // GLOWZONE / PICZONE
    else if (starts("wipehold ")) bvr::gfx_hud::set_load_hold(atoi(args + 9) != 0); // LOADHOLD
    else if (starts("wipereveal skip ")) bvr::gfx_hud::set_wipe_reveal_skip(atoi(args + 16)); // WIPEREVEAL
    else if (starts("wipereveal pre ")) bvr::gfx_hud::set_pre_reveal(atoi(args + 15)); // PREREVEAL
    else if (starts("wipereveal ")) bvr::gfx_hud::set_wipe_reveal(atoi(args + 11) != 0); // WIPEREVEAL
    else if (starts("wipesound play")) g_playWipeSound.store(true); // test: the learned sound now
    else if (starts("wipesound id ")) { AcquireSRWLockExclusive(&g_soundLock); g_wipeSoundId = uint32_t(strtoul(args + 13, nullptr, 10)); ReleaseSRWLockExclusive(&g_soundLock); BVR_LOG("[bc] WIPESOUND: event set to %s", args + 13); }
    else if (starts("wipesound ")) { g_wipeSoundOn.store(atoi(args + 10) != 0); BVR_LOG("[bc] WIPESOUND %s", g_wipeSoundOn.load() ? "on" : "off"); }
    else if (starts("wipedebug ")) bvr::gfx_hud::set_wipe_debug(atoi(args + 10) != 0); // auto captures 0.4 s into wipe replays
    else if (starts("hidewipes ")) { bvr::gfx_hud::set_hide_wipes(atoi(args + 10) != 0); BVR_LOG("[bc] hide menu wipes %d", atoi(args + 10)); }
    else if (starts("hd ")) screen_hd::command(args + 3); // HD SCREENS
    else if (starts("afw ")) { if (!afw::command(args + 4)) BVR_LOG("[bc] afw: on|off|mode N|win N|dbg N|feather N|log N|status"); }
    else if (starts("dlss0 on")) dlss_bridge::set_enabled(true);
    else if (starts("dlss0 off")) dlss_bridge::set_enabled(false);
    else if (starts("reflect on")) reflect::settings_read("reflect_per_eye", "1");
    else if (starts("reflect off")) reflect::settings_read("reflect_per_eye", "0");
    else if (starts("state1 on")) { g_state1On.store(true); BVR_LOG("[bc] left eye: own view state"); }
    else if (starts("state1 off")) { g_state1On.store(false); BVR_LOG("[bc] left eye: the game's view state (A/B)"); }
    else if (starts("state2 on")) { g_state2On.store(true); BVR_LOG("[bc] right eye: own view state"); }
    else if (starts("state2 off")) { g_state2On.store(false); BVR_LOG("[bc] right eye: SHARED view state (A/B)"); }
    else if (starts("lag ")) { bvr::vr::set_pose_lag(atoi(args + 4)); BVR_LOG("[bc] pose lag %d", bvr::vr::get_pose_lag()); }
    else if (starts("vis")) vision::handle_command(args[3] ? args + 4 : "");
    else if (starts("recenter")) g_recenterReq.store(true);
    else if (starts("capture")) g_captureReq.store(true);
    else if (starts("grunge game")) room_overlay::set_mode(room_overlay::kOriginal);
    else if (starts("grunge room")) room_overlay::set_mode(room_overlay::kRoom);
    else if (starts("grunge hide")) room_overlay::set_mode(room_overlay::kHidden);
    else if (starts("hudhead on")) bvr::vr::set_hud_head_locked(true);
    else if (starts("hudhead off")) bvr::vr::set_hud_head_locked(false);
    else if (starts("fps repeat")) { g_fpsRepeat.store(true); BVR_LOG("[bc] between frames: repeat the frame"); }
    else if (starts("fps ssw")) { g_fpsRepeat.store(false); BVR_LOG("[bc] between frames: Virtual Desktop SSW"); }
    else if (starts("fps ")) { g_fpsLock.store(atoi(args + 4)); BVR_LOG("[bc] fps lock %d", g_fpsLock.load()); }
    else if (starts("exact on")) bvr::vr::set_exact_pose_tag(true);
    else if (starts("exact off")) bvr::vr::set_exact_pose_tag(false);
    else if (starts("huddump")) bvr::gfx_hud::request_dump();
    else if (starts("hudline ")) bvr::gfx_hud::request_timeline(atoi(args + 8));
    else if (starts("movieband on")) window::set_movie_band(true);
    else if (starts("fadehold ")) bvr::gfx_hud::set_fade_hold(atoi(args + 9));
    else if (starts("menucut ")) bvr::gfx_hud::set_menu_cut(atoi(args + 8) != 0);
    else if (starts("stagebox ")) bvr::gfx_hud::set_stage_box(atoi(args + 9) != 0);
    else if (starts("huddsv ")) bvr::gfx_hud::set_hud_dsv(atoi(args + 7) != 0); // HUDDSV
    else if (starts("replayall ")) bvr::gfx_hud::set_replay_hides_all(atoi(args + 10) != 0); // WIPEMAIN2
    else if (starts("wipeflip ")) bvr::gfx_hud::set_wipe_flip(atoi(args + 9) != 0);          // WIPEFLIP
    else if (starts("wipepanel ")) bvr::gfx_hud::set_wipe_panel_off(atoi(args + 10) != 0);   // WIPEPANEL
    else if (starts("panelseq ")) {
        wchar_t dir[MAX_PATH];
        swprintf_s(dir, L"%s\\panelseq", bvr::log::data_dir());
        bvr::gfx_hud::request_panel_sequence(dir, atoi(args + 9));
    }
    else if (starts("halo ")) { g_pauseHalo.store(static_cast<float>(atof(args + 5))); BVR_LOG("[bc] pause halo %.2f", g_pauseHalo.load()); }
    else if (starts("fp on")) set_first_person(true);
    else if (starts("fp off")) set_first_person(false);
    else if (starts("fp fwd ")) { g_fpFwdCm.store(static_cast<float>(atof(args + 7))); BVR_LOG("[bc] first person forward %.1f cm", g_fpFwdCm.load()); }
    else if (starts("fp head ")) { g_fpHead.store(atoi(args + 8) != 0); BVR_LOG("[bc] first person head follow %s", g_fpHead.load() ? "ON" : "off"); }
    else if (starts("fp smooth ")) { g_fpHeadSmoothS.store(static_cast<float>(atof(args + 10))); BVR_LOG("[bc] first person head smoothing %.3f s", g_fpHeadSmoothS.load()); }
    else if (starts("fp up ")) { g_fpUpCm.store(static_cast<float>(atof(args + 6))); BVR_LOG("[bc] first person up %.1f cm", g_fpUpCm.load()); }
    else if (starts("movieband off")) window::set_movie_band(false);
    else if (starts("glow ")) glow::command(args + 5);
    else if (starts("hudzone ")) { // "hudzone Z size dx dy" (test)
        int z = 0;
        float zs = 1, zx = 0, zy = 0;
        if (sscanf_s(args + 8, "%d %f %f %f", &z, &zs, &zx, &zy) == 4) bvr::gfx_hud::set_hud_zone(z, zs, zx, zy);
        BVR_LOG("[bc] HUD group %d: size %.2f offset %.2f, %.2f", z, zs, zx, zy);
    }
    else if (starts("aimdepth ")) { markers::settings_read("aim_depth", args + 9); BVR_LOG("[bc] aim depth %s", args + 9); }
    else if (starts("markerupright ")) { markers::settings_read("marker_upright", args + 14); BVR_LOG("[bc] upright marker card %s", args + 14); }
    else if (starts("markerface ")) { markers::settings_read("marker_face", args + 11); BVR_LOG("[bc] marker card faces the game camera %s", args + 11); }
    else if (starts("aimnopass ")) { markers::settings_read("aim_nopass", args + 10); BVR_LOG("[bc] aim depth without the pass %s", args + 10); }
    else if (starts("hudfx on")) bvr::gfx_hud::set_fx_passthrough(true);
    else if (starts("hudfx off")) bvr::gfx_hud::set_fx_passthrough(false);
    else if (starts("hud off")) { g_hudQuad.store(false); bvr::gfx_hud::set_redirect(false); BVR_LOG("[bc] 2D-to-quad off"); }
    else if (starts("hud on")) { g_hudQuad.store(true); BVR_LOG("[bc] 2D-to-quad on"); }
    else if (starts("hud size ")) { float d, w, u; bvr::vr::get_hud_quad(&d, &w, &u); w = static_cast<float>(atof(args + 9)); bvr::vr::set_hud_quad(d, w, u); BVR_LOG("[bc] hud quad width %.2f m", w); }
    else if (starts("hud dist ")) { float d, w, u; bvr::vr::get_hud_quad(&d, &w, &u); d = static_cast<float>(atof(args + 9)); bvr::vr::set_hud_quad(d, w, u); BVR_LOG("[bc] hud quad distance %.2f m", d); }
    else if (starts("hud")) {
        const bvr::gfx_hud::Census c = bvr::gfx_hud::census();
        float d, w, u; bvr::vr::get_hud_quad(&d, &w, &u);
        BVR_LOG("[bc] hud: %s redirect %s | frames %u boundaries %u hudDraws %u (last frame %u) redirected %u bigPost %u | quad %.2f m wide at %.2f m, up %.2f",
                g_hudQuad.load() ? "ON" : "off", bvr::gfx_hud::redirect_on() ? "ENGAGED" : "idle", c.frames, c.boundaries,
                c.hudDraws, c.lastFrameHud, c.redirected, c.bigPostDraws, w, d, u);
    }
    else if (starts("fullrect on")) g_fullRect.store(true);
    else if (starts("fullrect off")) g_fullRect.store(false);
    else if (starts("asym on")) bvr::vr::set_asym_fov(true);
    else if (starts("asym off")) bvr::vr::set_asym_fov(false);
    else if (starts("auto off")) { g_autoVr.store(false); BVR_LOG("[bc] auto VR off"); }
    else if (starts("auto on")) { g_autoVr.store(true); g_autoDone = false; BVR_LOG("[bc] auto VR on"); }
    else if (starts("pos on")) g_posOn.store(true);
    else if (starts("pos off")) g_posOn.store(false);
    else if (starts("ipd ")) { g_ipdMm.store(static_cast<float>(atof(args + 4))); set_ipd_tag(); BVR_LOG("[bc] ipd %.1f mm", g_ipdMm.load()); }
    else if (starts("scale ")) { g_uuPerM.store(static_cast<float>(atof(args + 6))); BVR_LOG("[bc] world scale %.1f UU/m", g_uuPerM.load()); }
    else if (starts("dump")) { g_dumpRequested.store(g_stereo.load() ? 2 : 1); g_dumpOriginEye = 0; BVR_LOG("[bc] dump requested"); }
    else log_status();
    return true;
}

void log_status() {
    BVR_LOG("[bc] status: hooks %s | cam %s stereo %s%s | draws %u 2nd %u calcs %u edits %u | ipd %.1f mm scale %.1f UU/m pos %s | game loc (%.0f,%.0f,%.0f) rot (%d,%d,%d) | lastExc 0x%08X@%08X",
            g_live.load() ? "LIVE" : "off", vr_view_active() ? "ON" : (g_camera.load() ? "requested" : "off"),
            g_stereo.load() ? "ARMED" : "off", g_poisoned.load() ? " POISONED" : "", g_draws.load(),
            g_secondDraws.load(), g_calcs.load(), g_viewEdits.load(), g_ipdMm.load(), g_uuPerM.load(),
            g_posOn.load() ? "on" : "off", g_loc[0], g_loc[1], g_loc[2], g_rot[0], g_rot[1], g_rot[2],
            g_lastExcCode.load(), g_lastExcAddr.load());
    g_drawCallers.print("viewport draw");
    g_calcCallers.print("CalcSceneView");
    g_ctorCallers.print("FSceneView ctor");
}

void draw_ui() {
    // AKVR's panel layout (hooks.cpp draw_panel, TIDY4/DRAWERS): status, Recenter / Save capture /
    // Head tracking, controller hint, closed drawers; tests under Advanced, numbers under
    // Diagnostics, keys under Keyboard shortcuts.
    const ImVec4 kGreen(0.3f, 1.0f, 0.55f, 1.0f), kAmber(1.0f, 0.7f, 0.25f, 1.0f);
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "Batman City VR");
    ImGui::SameLine();
    ImGui::TextDisabled("   build: " __DATE__ " " __TIME__);
    {
        const bool vrOn = bvr::vr::session_live() && vr_view_active();
        ImGui::TextColored(vrOn ? kGreen : kAmber, "%s",
                           vrOn ? "VR on" : (bvr::vr::session_live() ? "VR connected, not showing yet" : "VR not connected"));
        ImGui::SameLine();
        ImGui::TextDisabled("|  %.0f fps  |  %s  |  head tracking %s", g_rateDraws,
                            stereo_active() ? "true 3D, both eyes every frame" : "3D off", g_camera.load() ? "on" : "OFF (F11)");
    }
    if (g_poisoned.load())
        ImGui::TextColored(kAmber, "3D switched itself off after a fault (0x%08X) - see Advanced", g_lastExcCode.load());
    if (ImGui::Button("Recenter  (F12)")) g_recenterReq.store(true);
    ImGui::SameLine();
    if (ImGui::Button("Save capture  (F2)")) g_captureReq.store(true);
    ImGui::SameLine();
    if (ImGui::Button(g_camera.load() ? "Head tracking: ON  (F11)" : "Head tracking: OFF  (F11)")) {
        if (g_camera.load()) set_camera(false); else set_stereo(true);
    }
    ImGui::TextDisabled("controller: left stick / d-pad = move, A = select / change, B = back, both stick clicks = close");

    ImGui::Separator();
    ImGui::SetNextItemOpen(false, ImGuiCond_Once);
    {   // FIRSTPERSON: at the top, it changes the whole view
        bool fp = g_fp.load();
        if (ImGui::Checkbox("FIRST PERSON  (or click both sticks twice quickly)", &fp)) set_first_person(fp);
        if (fp) {
            float f = g_fpFwdCm.load(), u = g_fpUpCm.load();
            if (bvr::overlay::SliderStep("   camera in front of his face (cm)", &f, -20.0f, 80.0f, "%.0f")) g_fpFwdCm.store(f);
            if (bvr::overlay::SliderStep("   camera up / down (cm)", &u, -40.0f, 40.0f, "%.0f")) g_fpUpCm.store(u);
            bool hf = g_fpHead.load(); // HEADCAM
            if (ImGui::Checkbox("   camera follows his head (gliding, animations)", &hf)) g_fpHead.store(hf);
            if (hf) {
                float sm = g_fpHeadSmoothS.load();
                if (bvr::overlay::SliderStep("   head smoothing (s) - higher = steadier, lags more", &sm, 0.0f, 0.5f, "%.2f"))
                    g_fpHeadSmoothS.store(sm);
                if (bvr::overlay::show_tests())
                    ImGui::TextDisabled("      head frames %u, fell back to the steady eye point %u", g_fpHeadFrames.load(),
                                        g_fpHeadMisses.load());
            }
        }
    }
    // STEREOSECTION (JJ 2026-10-11: "copy Batman Arkham Knight's implementation ... their own menu section called stereo
    // rendering"): AKVR hooks.cpp - its own section, open the first time, above VIEW (was inside FRAME RATE).
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::CollapsingHeader("STEREO RENDERING")) afw::draw_ui();
    if (ImGui::CollapsingHeader("VIEW")) {
        float uu = g_uuPerM.load();
        if (bvr::overlay::SliderStep("world scale  (game units per metre)", &uu, 30.0f, 200.0f, "%.0f")) g_uuPerM.store(uu);
        float ipd = g_ipdMm.load();
        if (bvr::overlay::SliderStep("eye distance (mm)", &ipd, 50.0f, 80.0f, "%.1f")) { g_ipdMm.store(ipd); set_ipd_tag(); }
        bool s2 = g_state2On.load();
        if (bvr::overlay::show_tests() && ImGui::Checkbox("right eye keeps its own view history (anti-flicker)", &s2)) g_state2On.store(s2);
        reflect::draw_ui();
        glow::draw_ui();
        bool bb = billboard::enabled();
        if (bvr::overlay::show_tests() && ImGui::Checkbox("particles (rain, droplets) turn with the game camera, not your head", &bb))
            billboard::set_enabled(bb);
        bool s1 = g_state1On.load();
        if (bvr::overlay::show_tests() && ImGui::Checkbox("left eye keeps its own view history (same lights and signs in both eyes)", &s1))
            g_state1On.store(s1);
        bool pos = g_posOn.load();
        if (ImGui::Checkbox("head position (lean / move)", &pos)) g_posOn.store(pos);
        bool asym = bvr::vr::asym_fov();
        if (bvr::overlay::show_tests() && ImGui::Checkbox("each eye shaped for its lens (sharper, less wasted)", &asym)) bvr::vr::set_asym_fov(asym);
        bool kf = window::keep_focus();
        if (ImGui::Checkbox("keep playing when the game window is not in front", &kf)) window::set_keep_focus(kf);
    }
    ImGui::SetNextItemOpen(false, ImGuiCond_Once);
    if (ImGui::CollapsingHeader("HUD")) {
        bool hh = bvr::vr::hud_head_locked();
        if (ImGui::Checkbox("attach UI to head movement", &hh)) bvr::vr::set_hud_head_locked(hh);
        // In-world HUD: UI stays in each eye's 3D image, where the game positioned markers (grapple
        // reticle, target marks) for THAT eye's view - so they sit on their objects with depth.
        // Needs symmetric eye shapes so flat HUD parts land on the same direction in both eyes
        // (they then read as far away instead of not fusing).
        bool inWorld = !g_hudQuad.load();
        if (ImGui::Checkbox("HUD drawn into the world (reticles stay on their targets)", &inWorld)) {
            g_hudQuad.store(!inWorld);
            if (inWorld) bvr::gfx_hud::set_redirect(false);
            bvr::vr::set_asym_fov(!inWorld);
        }
        ImGui::TextDisabled("   off = menus and HUD on the floating panel above");
        markers::draw_ui();
        // TIDY (JJ 2026-10-10: "clear out ... redundant options now in the VR overlay now that we have all the correct
        // settings and fixes"): the bat-wipe / menu-fade fixes are fixed ON (BATCUTS, HIDEWIPES, WIPEREPLAY, WIPEFLIP,
        // WIPEPANEL, BLACKFILL, MENUFADE, FADEHOLD) and the wipe test recorder OFF - no panel switches; `bc` commands remain.
        float ph = g_pauseHalo.load();
        if (bvr::overlay::SliderStep("pause menu: dark glow behind text and buttons (0 = off)", &ph, 0.0f, 1.0f, "%.2f"))
            g_pauseHalo.store(ph);
        float q[3];
        bvr::vr::get_hud_quad(&q[0], &q[1], &q[2]);
        // HUDDIST (JJ 2026-10-09: "move the HUD a little further away, similar to Arkham Knight HUDs, but give me
        // controls"): the panel is set by its distance and the angle it covers, so moving it nearer or further keeps
        // its size in view (width = 2 x distance x tan(angle / 2)); the height offset scales with the distance too.
        // JJ 2026-10-08: ~75 deg (was ~51); 2026-10-09: 88 deg. Arkham Knight's HUD hangs at 4 m.
        float deg = 2.0f * atanf(0.5f * q[1] / q[0]) * 180.0f / kPi;
        const float d0 = q[0];
        bool ch = bvr::overlay::SliderStep("HUD distance (m)", &q[0], 0.5f, 8.0f, "%.2f");
        ch |= bvr::overlay::SliderStep("HUD size (degrees of your view)", &deg, 30.0f, 120.0f, "%.0f");
        float upDeg = atanf(q[2] / d0) * 180.0f / kPi;
        ch |= bvr::overlay::SliderStep("HUD height (degrees, + up)", &upDeg, -30.0f, 30.0f, "%.0f");
        if (ch) {
            q[1] = 2.0f * q[0] * tanf(0.5f * deg * kPi / 180.0f);
            q[2] = q[0] * tanf(upDeg * kPi / 180.0f);
            bvr::vr::set_hud_quad(q[0], q[1], q[2]);
            bvr::vr::recenter_panel();
        }
        int he = bvr::vr::hud_eyes();
        ImGui::TextUnformatted("HUD depth for each eye (needed in Virtual Desktop):");
        ImGui::SameLine(); if (ImGui::RadioButton("auto##he", he < 0)) bvr::vr::set_hud_eyes(-1);
        ImGui::SameLine(); if (ImGui::RadioButton("on##he", he > 0)) bvr::vr::set_hud_eyes(1);
        ImGui::SameLine(); if (ImGui::RadioButton("off##he", he == 0)) bvr::vr::set_hud_eyes(0);
        ImGui::TextDisabled("   %s - off: the HUD looks far away whatever its distance",
                            bvr::vr::hud_eyes_active() ? "in use" : "not in use");
        // HUDZONES (JJ 2026-10-09): each part of the HUD resized and moved on its own.
        // HUDMENU (JJ 2026-10-09): the pause / map screen hangs at its own distance and size.
        {
            float md = 0, mw = 0;
            bvr::vr::get_hud_menu_quad(&md, &mw);
            float mdeg = 2.0f * atanf(0.5f * mw / md) * 180.0f / kPi;
            bool mc = bvr::overlay::SliderStep("pause / map screen distance (m)", &md, 0.5f, 8.0f, "%.2f");
            mc |= bvr::overlay::SliderStep("pause / map screen size (degrees)", &mdeg, 30.0f, 120.0f, "%.0f");
            if (mc) bvr::vr::set_hud_menu_quad(md, 2.0f * md * tanf(0.5f * mdeg * kPi / 180.0f));
            ImGui::TextDisabled("   menu / map screen %s | game clock %s (%d clock fields found)",
                                bvr::vr::hud_menu_now() ? "showing now" : "not showing",
                                g_gamePaused.load() ? "stopped" : "running", g_clockFields.load());
        }
        if (ImGui::TreeNode("HUD groups: size and position")) {
            bool zon = bvr::gfx_hud::hud_zones_on();
            if (ImGui::Checkbox("use the group sizes and positions below (off = the game's own layout)", &zon))
                bvr::gfx_hud::set_hud_zones_on(zon);
            for (int z = 0; z < bvr::gfx_hud::kHudZones; ++z) {
                ImGui::PushID(z);
                float s = 1, dx = 0, dy = 0;
                bvr::gfx_hud::get_hud_zone(z, &s, &dx, &dy);
                ImGui::Text("%s  (%u pieces drawn)", bvr::gfx_hud::hud_zone_name(z), bvr::gfx_hud::hud_zone_draws(z));
                bool ch = bvr::overlay::SliderStep("   size", &s, 0.30f, 1.50f, "%.2f");
                ch |= bvr::overlay::SliderStep("   left / right", &dx, -0.60f, 0.60f, "%.2f");
                ch |= bvr::overlay::SliderStep("   down / up", &dy, -0.60f, 0.60f, "%.2f");
                if (ch) bvr::gfx_hud::set_hud_zone(z, s, dx, dy);
                if (ImGui::Button("   reset this group")) bvr::gfx_hud::set_hud_zone(z, 1.0f, 0.0f, 0.0f);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
    }
    ImGui::SetNextItemOpen(false, ImGuiCond_Once);
    if (ImGui::CollapsingHeader("MENUS AND SCREENS")) {
        bool lg = window::show_logos();
        if (ImGui::Checkbox("play the opening logos (any pad button skips them)", &lg)) window::set_show_logos(lg);
        ImGui::TextDisabled("   takes effect the next time the game starts");
        room_overlay::draw_ui();
        bool rs = bvr::vr::screen_room();
        if (bvr::overlay::show_tests() && ImGui::Checkbox("logos / loading: screen drawn into the 3D view (steadier in Virtual Desktop)", &rs))
            bvr::vr::set_screen_room(rs);
        if (bvr::overlay::show_tests()) ImGui::TextDisabled("   off = the older floating panel");
        screen_hd::draw_ui(); // LOADTIME: HD loading pictures switch + loading-screen lengths
        float d = 0, w = 0;
        bvr::vr::get_screen_quad(&d, &w);
        bool ch = bvr::overlay::SliderStep("movie / loading screen distance (m)", &d, 0.8f, 6.0f, "%.2f");
        ch |= bvr::overlay::SliderStep("movie / loading screen size (m)", &w, 0.8f, 6.0f, "%.2f");
        if (ch) bvr::vr::set_screen_quad(d, w);
        float pw = 0, pd = 0;
        bvr::vr::get_panel_size(&pw, &pd);
        bool pc = bvr::overlay::SliderStep("this menu: distance (m)", &pd, 0.6f, 3.0f, "%.2f");
        pc |= bvr::overlay::SliderStep("this menu: size (m)", &pw, 0.5f, 2.0f, "%.2f");
        if (pc) bvr::vr::set_panel_size(pw, pd);
    }
    ImGui::SetNextItemOpen(false, ImGuiCond_Once);
    if (ImGui::CollapsingHeader("ZOOM AND DETECTIVE VISION")) vision::draw_ui();
    ImGui::SetNextItemOpen(false, ImGuiCond_Once);
    if (ImGui::CollapsingHeader("VR RESOLUTION")) window::draw_ui();
    ImGui::SetNextItemOpen(false, ImGuiCond_Once);
    if (ImGui::CollapsingHeader("DLSS (SMOOTHER EDGES)")) dlss_bridge::draw_ui();
    ImGui::SetNextItemOpen(false, ImGuiCond_Once);
    if (ImGui::CollapsingHeader("FRAME RATE")) {
        ImGui::Text("game frames %.0f /s, eye images %.0f /s (%s)", g_rateDraws, g_ratePresents,
                    stereo_active() && !afw::on() ? "two per frame" : "one per frame");
        ImGui::Separator(); // AFW's choice moved to its own STEREO RENDERING section (STEREOSECTION)
        int lock = g_fpsLock.load();
        ImGui::TextUnformatted("hold the game at:");
        ImGui::SameLine(); if (ImGui::RadioButton("off", lock == 0)) g_fpsLock.store(0);
        ImGui::SameLine(); if (ImGui::RadioButton("45", lock == 45)) g_fpsLock.store(45);
        ImGui::SameLine(); if (ImGui::RadioButton("40", lock == 40)) g_fpsLock.store(40);
        ImGui::SameLine(); if (ImGui::RadioButton("30", lock == 30)) g_fpsLock.store(30);
        {
            const bool rep = g_fpsRepeat.load();
            ImGui::TextUnformatted("between game frames:");
            ImGui::SameLine(); if (ImGui::RadioButton("repeat the frame", rep)) g_fpsRepeat.store(true);
            ImGui::SameLine(); if (ImGui::RadioButton("Virtual Desktop SSW", !rep)) g_fpsRepeat.store(false);
            const int64_t pn = bvr::vr::display_period_ns();
            const double hz = pn > 0 ? 1e9 / double(pn) : 0.0;
            const int lk = g_fpsLock.load();
            if (!lk) {
                ImGui::TextDisabled("   'off' lets the game run as fast as it can (capped at 90). The choice above is used");
                ImGui::TextDisabled("   only when the game is held at 45 / 40 / 30.");
            } else if (hz > 1.0) {
                const int div = static_cast<int>(hz / lk + 0.5);
                if (g_fpsRepeat.load()) {
                    uint32_t rpt = 0, nf = 0, late = 0;
                    bvr::vr::repeat_stats(&rpt, &nf, &late);
                    ImGui::TextDisabled("   headset %.0f Hz: each game frame shown %d times. repeated %u, new %u, late %u",
                                        hz, div, rpt, nf, late);
                    ImGui::TextDisabled("   back from SSW? set Virtual Desktop's SSW off Always, or VD keeps the game at half rate itself.");
                } else {
                    ImGui::TextDisabled("   headset %.0f Hz: a new frame every %d refreshes, SSW fills the rest.", hz, div);
                    ImGui::TextDisabled("   set Virtual Desktop's SSW to Always (Streaming tab) for this mode.");
                }
            }
        }
        if (bvr::overlay::show_tests()) { // PANELTIDY: settled (exact pose tags on)
        ImGui::Separator();
        bool exact = bvr::vr::exact_pose_tag();
        if (ImGui::Checkbox("exact head-pose match (recommended)", &exact)) bvr::vr::set_exact_pose_tag(exact);
        ImGui::TextDisabled("   each image is told the exact head position it was drawn from - no delay to guess.");
        if (!exact) {
            int lag = bvr::vr::get_pose_lag();
            if (bvr::overlay::SliderStepInt("head-pose delay (frames)", &lag, 0, 2)) bvr::vr::set_pose_lag(lag);
        }
        uint32_t hit = 0, miss = 0;
        bvr::vr::exact_pose_tag_stats(&hit, &miss);
        ImGui::TextDisabled("   exact matches %u, missed %u", hit, miss);
        }
    }
    ImGui::SetNextItemOpen(false, ImGuiCond_Once);
    if (bvr::overlay::show_tests() && ImGui::CollapsingHeader("Advanced  (tests - leave alone)")) {
        bool cam = g_camera.load();
        if (ImGui::Checkbox("VR camera", &cam)) set_camera(cam);
        bool st = g_stereo.load();
        if (ImGui::Checkbox("same-frame 3D (draw twice)", &st)) set_stereo(st);
        if (g_poisoned.load() && ImGui::Button("clear the 3D fault")) g_poisoned.store(false);
        bool fr = g_fullRect.load();
        if (ImGui::Checkbox("widen the game's 16:9 letterbox", &fr)) g_fullRect.store(fr);
        bool av = g_autoVr.load();
        if (ImGui::Checkbox("switch VR on automatically", &av)) g_autoVr.store(av);
        if (ImGui::Button("Save the view (both eyes)")) { g_dumpRequested.store(g_stereo.load() ? 2 : 1); g_dumpOriginEye = 0; }
    }
    ImGui::SetNextItemOpen(false, ImGuiCond_Once);
    if (bvr::overlay::show_tests() && ImGui::CollapsingHeader("Diagnostics")) {
        ImGui::Text("engine hooks %s | draws %u, second %u, view edits %u | hfov %.1f", g_live.load() ? "LIVE" : "off",
                    g_draws.load(), g_secondDraws.load(), g_viewEdits.load(), g_lastHfov);
        ImGui::Text("eye labels: game %u, render %u (in flight %d), labelled %u", g_gameBegins.load(), g_rtBegins.load(),
                    int(g_gameBegins.load() - g_rtBegins.load()), g_rtLabelled.load());
        ImGui::Text("skips: foreign %u, no view %u, no present %u | 2nd draw %u us | grunge skipped %u", g_skipForeign.load(),
                    g_skipNoView.load(), g_skipNoPresent.load(), g_call2Us.load(), g_grungeSkipped.load());
        const bvr::gfx_hud::Census c = bvr::gfx_hud::census();
        ImGui::Text("2D: frames found %u of %u, last frame %u UI draws", c.boundaries, c.frames, c.lastFrameHud);
        ImGui::Text("game camera (%.0f, %.0f, %.0f) yaw %d", g_loc[0], g_loc[1], g_loc[2], g_rot[1]);
        ImGui::Text("gamepad reads blocked while the menu was open: %u (DirectInput %u)", bvr::input::blocked_reads(),
                    bvr::input::dinput_block::blocked_reads());
    }
    ImGui::SetNextItemOpen(false, ImGuiCond_Once);
    if (ImGui::CollapsingHeader("Keyboard shortcuts")) {
        ImGui::TextDisabled("F10 or both stick clicks - this menu\nF12 - recenter\nF11 - head tracking on/off\nF2 - save a capture (both eyes, the HUD panel, settings)");
    }
    {   // PANELTIDY: everything for testing stays out of sight unless this is ticked (not saved)
        bool st = bvr::overlay::show_tests();
        if (ImGui::Checkbox("show test options", &st)) bvr::overlay::set_show_tests(st);
    }
}

} // namespace bvr::ac::scene
