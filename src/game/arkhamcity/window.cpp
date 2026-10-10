#include "game/arkhamcity/window.h"
#include "core/ui/overlay.h"

#include "core/gfx/hud_capture.h"
#include "core/gfx/gfx_hud.h"
#include "core/input/xinput_bridge.h"
#include "core/util/crash.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/arkhamcity/dlss_bridge.h"

#include <MinHook.h>
#include <imgui.h>
#include <windows.h>
#include <Xinput.h>
#include <intrin.h>

#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bvr::ac::window {
namespace {

using CreateWindowExWFn = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND,
                                         HMENU, HINSTANCE, LPVOID);
CreateWindowExWFn g_createOrig = nullptr;
WNDPROC g_prevProc = nullptr;
HWND g_gameWnd = nullptr;
std::atomic<bool> g_done{false};

// KEEP RUNNING UNFOCUSED (AKVR's focus keeper, akvr/src/hooks.cpp). In the headset the game
// window is rarely the desktop's front window (Virtual Desktop, a click elsewhere), and Arkham
// City pauses the moment it hears it lost focus (LIVE 2026-10-03: a scripted test sat on the
// pause menu after a focus change). Windowed VR launches only; switch in the VIEW drawer.
std::atomic<bool> g_keepFocus{false};
DWORD g_gameThread = 0;

using GetWndFn = HWND(WINAPI*)();
GetWndFn g_gfwOrig = nullptr, g_gawOrig = nullptr, g_gfOrig = nullptr;
// Only the game's own thread is told it is in front; every other thread sees the truth.
bool lie_to_caller() { return g_keepFocus.load(std::memory_order_relaxed) && g_gameWnd && GetCurrentThreadId() == g_gameThread; }
// FOCUSAUDIO3 (JJ 2026-10-10 01:15: "the audio fix doesn't work" - log: SetForegroundWindow REFUSED by Windows on all 4
// tries, SteamVR keeps the front): the game's sound (Wwise) is muted by a check of the FRONT window made outside the
// game thread, so only the game thread being told the truth-bending answer let the sound blip and stop. The front-
// window question is now answered "the game" for EVERY thread in the process; the mod's own checks call g_gfwOrig.
// (GetActiveWindow / GetFocus stay per thread - they are per-thread by nature.)
HWND WINAPI gfw_detour() {
    return g_keepFocus.load(std::memory_order_relaxed) && g_gameWnd ? g_gameWnd : g_gfwOrig();
}
HWND WINAPI gaw_detour() { return lie_to_caller() ? g_gameWnd : g_gawOrig(); }
HWND WINAPI gf_detour() { return lie_to_caller() ? g_gameWnd : g_gfOrig(); }

// Windows caps a captioned window at the monitor size through WM_GETMINMAXINFO. Raise the cap
// so the game's own requested size (its ResX/ResY) is honoured; UE3 then sizes its backbuffer
// to the full client area.
LRESULT CALLBACK minmax_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SIZE)
        BVR_LOG("[bc] game window WM_SIZE %ux%u (type %u)", LOWORD(lp), HIWORD(lp), unsigned(wp));
    if (g_keepFocus.load(std::memory_order_relaxed)) {
        switch (msg) {
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_MINIMIZE) return 0; // a minimized window loses the pad (AKVR)
            break;
        case WM_ACTIVATE: wp = MAKEWPARAM(WA_ACTIVE, 0); break;
        case WM_ACTIVATEAPP: wp = TRUE; break;
        case WM_NCACTIVATE: wp = TRUE; break;
        case WM_KILLFOCUS: return 0;
        case WM_MOUSEACTIVATE: return MA_ACTIVATE;
        }
    }
    LRESULT r = CallWindowProcW(g_prevProc, hwnd, msg, wp, lp);
    if (msg == WM_GETMINMAXINFO && lp) {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMaxTrackSize.x = mm->ptMaxSize.x = 16384;
        mm->ptMaxTrackSize.y = mm->ptMaxSize.y = 16384;
    }
    return r;
}

HWND WINAPI create_detour(DWORD exStyle, LPCWSTR cls, LPCWSTR title, DWORD style, int x, int y,
                          int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    HWND hwnd = g_createOrig(exStyle, cls, title, style, x, y, w, h, parent, menu, inst, param);
    // The game's main window: top-level, captioned, titled. (Core's hidden probe window for
    // the D3D11 vtable has an empty title.)
    if (hwnd && !g_done.load() && !parent && (style & WS_CAPTION) == WS_CAPTION && title &&
        title[0]) {
        g_done.store(true);
        g_gameThread = GetCurrentThreadId();
        g_gameWnd = hwnd;
        g_prevProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&minmax_proc)));
        RECT rc{};
        GetWindowRect(hwnd, &rc);
        const int gotW = rc.right - rc.left, gotH = rc.bottom - rc.top;
        // Creation itself was already clamped; re-apply the size the game asked for.
        if (w != CW_USEDEFAULT && h > 0 && (w > gotW || h > gotH))
            SetWindowPos(hwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        GetWindowRect(hwnd, &rc);
        BVR_LOG("[bc] game window '%ls': requested %dx%d, created %dx%d, now %dx%d (monitor cap lifted)",
                title, w, h, gotW, gotH, rc.right - rc.left, rc.bottom - rc.top);
    }
    return hwnd;
}

// Same near-square shape as the Quest 3 eye via Virtual Desktop (2064x2208), scaled.
const Preset kPresets[] = {
    {"Low (0.8x)", 1648, 1768},
    {"Quest 3 native (1.0x)", 2064, 2208},
    {"High (1.2x)", 2480, 2648},
    {"Very high (1.4x)", 2888, 3088},
    {"Ultra (1.6x)", 3304, 3536},
};

// --- desktop-size spoof for the game's own calls (windowed VR launches only) ----------------
// UE3 shrinks a windowed game to fit the desktop work area, keeping the aspect (2064x2208 ->
// 1274x1363 on a 1440p screen; LIVE 2026-10-03). The game asks GetMonitorInfoW,
// SystemParametersInfoW(SPI_GETWORKAREA) and GetSystemMetrics; only ITS import slots are
// patched, so Windows and every other module still see the real monitor (MELE VR spoofs the
// same three for the same reason).
constexpr LONG kFakeDesktop = 8192;
using GetSystemMetricsFn = int(WINAPI*)(int);
using SystemParametersInfoWFn = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT);
using GetMonitorInfoWFn = BOOL(WINAPI*)(HMONITOR, LPMONITORINFO);
GetSystemMetricsFn g_gsmOrig = nullptr;
SystemParametersInfoWFn g_spiOrig = nullptr;
GetMonitorInfoWFn g_gmiOrig = nullptr;
std::atomic<uint32_t> g_spoofHits{0};

void grow(RECT& r) {
    if (r.right - r.left < kFakeDesktop) r.right = r.left + kFakeDesktop;
    if (r.bottom - r.top < kFakeDesktop) r.bottom = r.top + kFakeDesktop;
}
int WINAPI gsm_detour(int i) {
    const int v = g_gsmOrig(i);
    switch (i) {
    case SM_CXSCREEN: case SM_CYSCREEN: case SM_CXFULLSCREEN: case SM_CYFULLSCREEN:
    case SM_CXMAXIMIZED: case SM_CYMAXIMIZED: case SM_CXMAXTRACK: case SM_CYMAXTRACK:
    case SM_CXVIRTUALSCREEN: case SM_CYVIRTUALSCREEN:
        g_spoofHits.fetch_add(1, std::memory_order_relaxed);
        return v < kFakeDesktop ? kFakeDesktop : v;
    default:
        return v;
    }
}
BOOL WINAPI spi_detour(UINT action, UINT ui, PVOID pv, UINT win) {
    const BOOL r = g_spiOrig(action, ui, pv, win);
    if (r && action == SPI_GETWORKAREA && pv) {
        grow(*static_cast<RECT*>(pv));
        g_spoofHits.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
}
BOOL WINAPI gmi_detour(HMONITOR m, LPMONITORINFO mi) {
    const BOOL r = g_gmiOrig(m, mi);
    if (r && mi) {
        grow(mi->rcMonitor);
        grow(mi->rcWork);
        g_spoofHits.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
}

// Patch one import slot of the host exe (by DLL + function name). Returns the old pointer.
void* patch_import(const char* dll, const char* fn, void* replacement) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(base + d->Name), dll) != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) continue;
            auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(ibn->Name), fn) != 0) continue;
            DWORD old = 0;
            VirtualProtect(&slots->u1.Function, sizeof(DWORD), PAGE_READWRITE, &old);
            void* prev = reinterpret_cast<void*>(static_cast<uintptr_t>(slots->u1.Function));
            slots->u1.Function = static_cast<DWORD>(reinterpret_cast<uintptr_t>(replacement));
            VirtualProtect(&slots->u1.Function, sizeof(DWORD), old, &old);
            return prev;
        }
    }
    return nullptr;
}

using SetWindowPosFn = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
SetWindowPosFn g_swpOrig = nullptr;
unsigned g_targetW = 0, g_targetH = 0; // VR client size for this launch (launch.ini / default)

// THE size lever (AKVR lesson: force the size where the game sets it, don't chase every
// desktop query). The game sizes its window from the monitor (LIVE: SetWindowPos 2070x1469
// from 0x00BAC1EC on a 1440p screen) and UE3 then sizes its backbuffer to the client area.
// For the game's main window we substitute the VR client size, framed by its own style.
BOOL WINAPI swp_detour(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags) {
    if (!(flags & SWP_NOSIZE) && h == g_gameWnd && g_targetW && g_targetH) {
        RECT r{0, 0, static_cast<LONG>(g_targetW), static_cast<LONG>(g_targetH)};
        AdjustWindowRectEx(&r, static_cast<DWORD>(GetWindowLongW(h, GWL_STYLE)), FALSE,
                           static_cast<DWORD>(GetWindowLongW(h, GWL_EXSTYLE)));
        const int wantW = r.right - r.left, wantH = r.bottom - r.top;
        if (cx != wantW || cy != wantH) {
            BVR_LOG("[bc] game SetWindowPos %dx%d (from %p) -> forced %dx%d (client %ux%u)", cx, cy,
                    _ReturnAddress(), wantW, wantH, g_targetW, g_targetH);
            // Stack scrape: every dword on the stack that points just after a CALL in the game's
            // .text (the game omits frame pointers, so the API backtrace stops at 2 frames).
            const uint32_t* sp = reinterpret_cast<const uint32_t*>(_AddressOfReturnAddress());
            char line[600];
            int len = sprintf_s(line, "[bc]   resize call chain:");
            int found = 0;
            for (int i = 0; i < 2048 && found < 14 && len < 560; ++i) {
                const uint32_t v = sp[i];
                if (v < 0x00401000 || v > 0x01202000) continue;
                const auto* b = reinterpret_cast<const uint8_t*>(v);
                if (b[-5] == 0xE8 || (b[-2] == 0xFF && (b[-1] & 0xF8) == 0xD0) || (b[-6] == 0xFF && b[-5] == 0x15) || (b[-3] == 0xFF && (b[-2] & 0x38) == 0x10)) {
                    len += sprintf_s(line + len, sizeof(line) - len, " %08X", v);
                    ++found;
                }
            }
            BVR_LOG("%s", line);
            cx = wantW;
            cy = wantH;
        }
    }
    return g_swpOrig(h, after, x, y, cx, cy, flags);
}

// THE clamp (AKVR-style direct patch, 2026-10-03). FWindowsViewport's open/resize function
// (0xBAF0A0) caps the requested client size to a cached monitor/work-area RECT and then
// shrinks to fit keeping the aspect:
//   0xBAF615  cmovg edi, ebp   ; width  = min(width,  rect width)    0F 4F FD
//   0xBAF63F  cmovg esi, ecx   ; height = min(height, rect height)   0F 4F F1
// NOP both (0F 1F 00) and the requested size survives; the shrink-to-fit compares the
// result with the request and is skipped when they match. Windowed (VR) launches only.
bool patch3(uintptr_t va, const uint8_t (&expect)[3]) {
    auto* p = reinterpret_cast<uint8_t*>(va);
    if (memcmp(p, expect, 3) != 0) {
        BVR_LOG("[bc] clamp patch @%08X: unexpected bytes %02X %02X %02X - NOT patched", unsigned(va), p[0], p[1], p[2]);
        return false;
    }
    DWORD old = 0;
    VirtualProtect(p, 3, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0x0F; p[1] = 0x1F; p[2] = 0x00;
    VirtualProtect(p, 3, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 3);
    return true;
}

// FWindowsViewport resize (0xBAF0A0): thiscall(SizeX, SizeY, bFullscreen, PosX, PosY), ret 0x14.
// THE size point (AKVR-style): every windowed resize request becomes the chosen VR size, so the
// window, swap chain and engine viewport all follow it. The "match the window" check (0xBAC8C0)
// then sees client == viewport and stays quiet.
using ViewportResizeFn = void(__fastcall*)(void* self, void* edx, int sx, int sy, int fs, int px, int py);
ViewportResizeFn g_vpResizeOrig = nullptr;
void __fastcall vp_resize_detour(void* self, void* edx, int sx, int sy, int fs, int px, int py) {
    if (!fs && g_targetW && g_targetH && (sx != int(g_targetW) || sy != int(g_targetH))) {
        BVR_LOG("[bc] viewport resize %dx%d -> %ux%u (VR size)", sx, sy, g_targetW, g_targetH);
        sx = int(g_targetW);
        sy = int(g_targetH);
    }
    g_vpResizeOrig(self, edx, sx, sy, fs, px, py);
}

// Opening logos (JJ 2026-10-03: skip them on launch). They are CRI .usm videos sequenced by
// BmGame\Movies\Startup.swf / StartupNV.swf (the NVIDIA build uses the NV one, so the ini's
// StartupMovies list does not control it). In VR launches the game is told those two files do
// not exist - the community "delete Startup*.swf" fix without touching the game folder.
bool is_startup_swf(LPCWSTR path) {
    if (!path) return false;
    const wchar_t* slash = wcsrchr(path, L'\\');
    const wchar_t* name = slash ? slash + 1 : path;
    if (_wcsicmp(name, L"Startup.swf") != 0 && _wcsicmp(name, L"StartupNV.swf") != 0) return false;
    return wcsstr(path, L"Movies") != nullptr || wcsstr(path, L"movies") != nullptr;
}
using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using GetFileAttributesWFn = DWORD(WINAPI*)(LPCWSTR);
using FindFirstFileWFn = HANDLE(WINAPI*)(LPCWSTR, LPWIN32_FIND_DATAW);
CreateFileWFn g_cfOrig = nullptr;
GetFileAttributesWFn g_gfaOrig = nullptr;
FindFirstFileWFn g_fffOrig = nullptr;
std::atomic<uint32_t> g_logoHides{0};
// JJ 2026-10-04: the logos SHOULD play, skippable with a pad button. `opening_logos=0` in
// settings.ini restores the old always-skip. Read at install (before the game opens them).
bool g_showLogos = true;
bool hide_logos() { return !g_showLogos; }

// Logo video files (*.usm), for the skip: their handles, and whether a skip was asked for.
std::atomic<HANDLE> g_usmHandle[4] = {};
std::atomic<bool> g_logoSkip{false};
std::atomic<uint32_t> g_usmReads{0}, g_usmSkippedReads{0};
std::atomic<ULONGLONG> g_usmLastReadMs{0};
ULONGLONG g_installMs = 0;
bool is_usm(LPCWSTR path) {
    const size_t n = path ? wcslen(path) : 0;
    return n > 4 && _wcsicmp(path + n - 4, L".usm") == 0;
}
bool is_logo_usm(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE) return false;
    for (auto& s : g_usmHandle)
        if (s.load(std::memory_order_relaxed) == h) return true;
    return false;
}

std::atomic<bool> g_logoUsm{false}; // HD LOGO: the last movie file opened was bac_logo.usm
HANDLE WINAPI cf_detour(LPCWSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) {
    if (is_startup_swf(p) && (hide_logos() || g_logoSkip.load())) {
        if (g_logoHides.fetch_add(1) == 0) BVR_LOG("[bc] opening logos skipped (%ls reported missing)", p);
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    HANDLE h = g_cfOrig(p, a, s, sa, c, f, t);
    // Only the start-up phase's videos are logos (in-game .usm cutscenes come much later).
    if (is_usm(p) && h != INVALID_HANDLE_VALUE) // HD LOGO: the opening-logo video is the one on screen next
        g_logoUsm.store([p] { wchar_t low[MAX_PATH] = {}; wcsncpy_s(low, p, _TRUNCATE); _wcslwr_s(low); return wcsstr(low, L"bac_logo") != nullptr; }(), std::memory_order_relaxed);
    if (is_usm(p) && h != INVALID_HANDLE_VALUE && GetTickCount64() - g_installMs < 90000) {
        static std::atomic<uint32_t> slot{0};
        g_usmHandle[slot.fetch_add(1) % 4].store(h);
        BVR_LOG("[bc] movie opened: %ls (handle %p, flags 0x%08X)", p, h, f);
    }
    return h;
}
DWORD WINAPI gfa_detour(LPCWSTR p) {
    if (is_startup_swf(p) && (hide_logos() || g_logoSkip.load())) { g_logoHides.fetch_add(1); SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_FILE_ATTRIBUTES; }
    return g_gfaOrig(p);
}
HANDLE WINAPI fff_detour(LPCWSTR p, LPWIN32_FIND_DATAW d) {
    if (is_startup_swf(p) && (hide_logos() || g_logoSkip.load())) { g_logoHides.fetch_add(1); SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    return g_fffOrig(p, d);
}
using ReadFileFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
ReadFileFn g_rfOrig = nullptr;
void logo_tick(); // below
BOOL WINAPI rf_detour(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    if (is_logo_usm(h)) {
        const uint32_t k = g_usmReads.fetch_add(1);
        if (k < 3 || (k % 50) == 0) BVR_LOG("[bc] movie read #%u: %u bytes%s", k, n, ov ? " (overlapped)" : "");
        g_usmLastReadMs.store(GetTickCount64(), std::memory_order_relaxed);
        logo_tick();
    }
    return g_rfOrig(h, buf, n, got, ov);
}

// THE SKIP (2026-10-04). Ending the video's file early froze the game (the player waits for
// data forever), so the skip asks the engine's movie player to stop, as the engine itself does:
// GFullScreenMovie vtbl+0x14 = GameThreadStopMovie(delay 0.0, bWaitForMovie FALSE, bForceStop
// TRUE) - the game's own force-stop calls at 0x689A59 / 0x7A8D71 / 0xBADDDF. "GameThread" means
// it should run on the game thread - but the GFx player's stop (vtbl 0x13B3C58 slot 5 =
// 0xB462A0) is only an interlocked flag write, so it is issued at once from the thread that
// sees the press; the player's status queries (vtbl+0x1C IsMovieFinished, +0x20 IsMoviePlaying)
// are hooked as a backup. Hiding Startup*.swf after the skip stops the next logo from starting.
using MovieQueryFn = int(__thiscall*)(void*, const wchar_t*);
using MovieStopFn = void(__thiscall*)(void*, float, int, int);
MovieQueryFn g_finishedOrig = nullptr, g_playingOrig = nullptr;
std::atomic<bool> g_stopIssued{false};
std::atomic<bool> g_movieHooks{false};
void* fsm_object() {
    __try {
        void* fsm = *reinterpret_cast<void**>(0x016CB074); // addr::kGFullScreenMovie
        return fsm && *reinterpret_cast<void**>(fsm) ? fsm : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
void maybe_stop(void* self) {
    if (!g_logoSkip.load(std::memory_order_relaxed) || g_stopIssued.exchange(true)) return;
    void** vt = *reinterpret_cast<void***>(self);
    BVR_LOG("[bc] opening logos: stopping the start-up movie (game thread %lu)", GetCurrentThreadId());
    reinterpret_cast<MovieStopFn>(vt[0x14 / 4])(self, 0.0f, 0, 1);
}
int __fastcall finished_detour(void* self, void*, const wchar_t* name) {
    maybe_stop(self);
    return g_finishedOrig(self, name);
}
int __fastcall playing_detour(void* self, void*, const wchar_t* name) {
    maybe_stop(self);
    return g_playingOrig(self, name);
}
void install_movie_hooks() {
    if (g_movieHooks.exchange(true)) return;
    void* fsm = fsm_object();
    if (!fsm) { g_movieHooks.store(false); return; }
    void** vt = *reinterpret_cast<void***>(fsm);
    const MH_STATUS a = MH_CreateHook(vt[0x1C / 4], reinterpret_cast<void*>(&finished_detour), reinterpret_cast<void**>(&g_finishedOrig));
    const MH_STATUS c = MH_CreateHook(vt[0x20 / 4], reinterpret_cast<void*>(&playing_detour), reinterpret_cast<void**>(&g_playingOrig));
    if (a == MH_OK) MH_EnableHook(vt[0x1C / 4]);
    if (c == MH_OK) MH_EnableHook(vt[0x20 / 4]);
    BVR_LOG("[bc] movie player hooks for the logo skip: %s / %s", MH_StatusToString(a), MH_StatusToString(c));
}

// LOADING-SCREEN LAYOUT (JJ 2026-10-04: a black box over the objectives text at the top left).
// Full-screen movies lay themselves out for the display area they are given; in the tall VR
// picture (2480x2648) part of the loading screen lands over its own text - at 16:9 (2560x1440)
// it is correct (fake headset, same save). The movie view (GFxMovieView, at fsm+0x24 while a
// movie plays - the player's tick calls its SetVariable there) takes its area through vtbl+0x64
// SetViewport(const GViewport&) (UGFxMoviePlayer::SetViewport 0xB35040: +0x68 get, edit,
// +0x64 set). GViewport ints: [0] buffer w, [1] buffer h, [2] left, [3] top, [4] w, [5] h,
// [6..9] scissor, [10] scale, [11] aspect (floats), [12] flags. For the full-screen movie only,
// the area becomes a centred 16:9 band; above and below stay black.
using SetViewportFn = void(__thiscall*)(void*, const int*);
SetViewportFn g_setVpOrig = nullptr;
std::atomic<bool> g_vpHook{false};
std::atomic<bool> g_bandEnable{false}; // 2026-10-09: live switch (`bc movieband on|off`), off = the game's layout
std::atomic<uint32_t> g_vpBands{0};
void* fsm_movie_view() {
    void* fsm = fsm_object();
    if (!fsm) return nullptr;
    __try {
        return *reinterpret_cast<void**>(static_cast<char*>(fsm) + 0x24);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
// The loading screen is NOT the movie the player starts: it is one of the Scaleform movies made
// earlier (title time) and revealed during the load - banding the player's own new view changed
// nothing on screen (fake headset). So every view sized to the whole buffer is remembered; while
// a full-screen movie plays all of them get the 16:9 band, and their own area back after it.
// Before touching a remembered view its vtable must still be the movie view's (freed views).
constexpr uintptr_t kMovieViewSetViewport = 0x00F30BF0; // GFxMovieRoot::SetViewport (probe 2026-10-04)
struct KnownView {
    void* view = nullptr;
    void* vtbl = nullptr;
    int vp[13] = {};
    bool banded = false;
};
KnownView g_views[48];
std::mutex g_viewsMutex;
bool g_bandsOn = false; // game thread

bool full_buffer(const int* vp) { return vp[2] == 0 && vp[3] == 0 && vp[4] == vp[0] && vp[5] == vp[1] && vp[4] > 0; }
void to_band(const int* vp, int* out) {
    memcpy(out, vp, 13 * sizeof(int));
    const int band = vp[4] * 9 / 16;
    if (vp[5] > band + 2) {
        out[3] = vp[3] + (vp[5] - band) / 2;
        out[5] = band;
    }
}
bool view_alive(const KnownView& k) {
    __try {
        return k.view && *reinterpret_cast<void* const*>(k.view) == k.vtbl;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall setvp_detour(void* self, void*, const int* vp) {
    if (vp && self && full_buffer(vp)) {
        std::lock_guard<std::mutex> lk(g_viewsMutex);
        KnownView* slot = nullptr;
        for (KnownView& k : g_views)
            if (k.view == self) { slot = &k; break; }
        if (!slot)
            for (KnownView& k : g_views)
                if (!k.view || !view_alive(k)) { slot = &k; break; }
        if (slot) {
            slot->view = self;
            slot->vtbl = *reinterpret_cast<void**>(self);
            memcpy(slot->vp, vp, sizeof(slot->vp));
            slot->banded = g_bandsOn;
        }
        if (g_bandsOn) {
            int c[13];
            to_band(vp, c);
            g_setVpOrig(self, c);
            return;
        }
    }
    g_setVpOrig(self, vp);
}

bool fsm_playing_raw() {
    bool playing = false;
    if (void* fsm = fsm_object()) {
        __try { playing = g_playingOrig(fsm, L"") != 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return playing;
}

// Game thread, every frame: band / unband every remembered view when a full-screen movie
// starts / ends.
void movie_band_tick() {
    if (!g_setVpOrig || !g_playingOrig) return;
    const bool playing = g_bandEnable.load(std::memory_order_relaxed) && fsm_playing_raw();
    if (playing == g_bandsOn) return;
    std::lock_guard<std::mutex> lk(g_viewsMutex);
    g_bandsOn = playing;
    int n = 0;
    for (KnownView& k : g_views) {
        if (!k.view || !view_alive(k)) continue;
        if (playing && !k.banded) {
            int c[13];
            to_band(k.vp, c);
            g_setVpOrig(k.view, c);
            k.banded = true;
            ++n;
        } else if (!playing && k.banded) {
            g_setVpOrig(k.view, k.vp);
            k.banded = false;
            ++n;
        }
    }
    BVR_LOG("[bc] full-screen movie %s: %d Scaleform view(s) %s", playing ? "on" : "off", n,
            playing ? "laid out in a centred 16:9 band" : "given their full area back");
}

void install_movie_viewport_hook() {
    if (g_vpHook.exchange(true)) return;
    void* target = reinterpret_cast<void*>(kMovieViewSetViewport);
    const MH_STATUS c = MH_CreateHook(target, reinterpret_cast<void*>(&setvp_detour), reinterpret_cast<void**>(&g_setVpOrig));
    const MH_STATUS e = c == MH_OK ? MH_EnableHook(target) : c;
    BVR_LOG("[bc] Scaleform SetViewport hook @%p: %s", target, MH_StatusToString(e));
}

// Any pad face button / Start / Back, or Enter / Escape / Space, while a logo video streams.
using XIGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
void logo_tick() {
    if (g_logoSkip.load(std::memory_order_relaxed)) return;
    const ULONGLONG last = g_usmLastReadMs.load(std::memory_order_relaxed);
    if (!last || GetTickCount64() - last > 1500) return; // no logo playing
    install_movie_hooks(); // the player exists once a logo plays
    install_movie_viewport_hook(); // records views only; the band itself waits for `bc movieband on` (2026-10-09 test)
    static XIGetStateFn get = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (HMODULE m = LoadLibraryW(L"xinput1_4.dll")) get = reinterpret_cast<XIGetStateFn>(GetProcAddress(m, "XInputGetState"));
    }
    bool press = (GetAsyncKeyState(VK_RETURN) | GetAsyncKeyState(VK_ESCAPE) | GetAsyncKeyState(VK_SPACE)) & 0x8000;
    if (get) {
        XINPUT_STATE s{};
        const WORD kAny = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y |
                          XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK;
        for (DWORD i = 0; i < 4 && !press; ++i)
            if (get(i, &s) == ERROR_SUCCESS && (s.Gamepad.wButtons & kAny)) press = true;
    }
    if (bvr::input::test_buttons_down()) press = true; // `vrinput press a` (tests)
    if (press) {
        g_logoSkip.store(true);
        BVR_LOG("[bc] opening logos: skipped by a button press");
        // The GFx player's stop (0xB462A0) only does InterlockedExchange(this+0x3C, 2) and
        // clears this+0x7C - safe from any thread. The game thread waits for the movies only
        // at the END of start-up loading, so waiting for its queries made the skip ~6 s late.
        if (void* fsm = fsm_object()) maybe_stop(fsm);
    }
}

void launch_ini_path(wchar_t* out, size_t n) {
    swprintf_s(out, n, L"%s\\launch.ini", bvr::log::data_dir());
}

} // namespace

// EXITRING (JJ 2026-10-10: the bats on quitting from the first menu are windowed; no capture is possible while the
// game quits): the HUD record is written when the crash guard sees the window close (this window proc never did).
void save_exit_ring() {
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\hud_last_frames_at_exit.txt", bvr::log::data_dir());
    const bool ok = bvr::gfx_hud::save_hud_ring(path);
    BVR_LOG("[bc] game window closing - last HUD frames %s", ok ? "saved (hud_last_frames_at_exit.txt)" : "NOT saved");
}

void install() {
    bvr::crash::set_teardown_hook(&save_exit_ring);
    g_installMs = GetTickCount64();
    void* target = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "CreateWindowExW"));
    const MH_STATUS c = MH_CreateHook(target, reinterpret_cast<void*>(&create_detour),
                                      reinterpret_cast<void**>(&g_createOrig));
    const MH_STATUS e = c == MH_OK ? MH_EnableHook(target) : c;
    BVR_LOG("[bc] window size cap hook (CreateWindowExW): %s", MH_StatusToString(e));

    // Desktop-size spoof: windowed launches only (the VR launcher passes -windowed; a normal
    // Steam launch is fullscreen and must keep the real monitor).
    const wchar_t* cmd = GetCommandLineW();
    if (cmd && wcsstr(cmd, L"-windowed")) {
        g_gsmOrig = reinterpret_cast<GetSystemMetricsFn>(patch_import("USER32.dll", "GetSystemMetrics", reinterpret_cast<void*>(&gsm_detour)));
        g_spiOrig = reinterpret_cast<SystemParametersInfoWFn>(patch_import("USER32.dll", "SystemParametersInfoW", reinterpret_cast<void*>(&spi_detour)));
        g_gmiOrig = reinterpret_cast<GetMonitorInfoWFn>(patch_import("USER32.dll", "GetMonitorInfoW", reinterpret_cast<void*>(&gmi_detour)));
        g_swpOrig = reinterpret_cast<SetWindowPosFn>(patch_import("USER32.dll", "SetWindowPos", reinterpret_cast<void*>(&swp_detour)));
        // Inline hooks on kernel32 (not the exe's import table): the game opens movies through
        // its C runtime (msvcr90), whose calls never touch the exe's imports. The detours only
        // act on the two Startup*.swf names, everything else passes straight through.
        MH_CreateHookApi(L"kernel32", "CreateFileW", reinterpret_cast<void*>(&cf_detour), reinterpret_cast<void**>(&g_cfOrig));
        MH_CreateHookApi(L"kernel32", "GetFileAttributesW", reinterpret_cast<void*>(&gfa_detour), reinterpret_cast<void**>(&g_gfaOrig));
        MH_CreateHookApi(L"kernel32", "FindFirstFileW", reinterpret_cast<void*>(&fff_detour), reinterpret_cast<void**>(&g_fffOrig));
        MH_CreateHookApi(L"kernel32", "ReadFile", reinterpret_cast<void*>(&rf_detour), reinterpret_cast<void**>(&g_rfOrig));
        // Off unless chosen: both GPU faults that froze Virtual Desktop (2026-10-08 22:49, 23:05, Xid 13) ran with the
        // DLSS helper; none in a month of logs before it.
        int dlssMode = 0;
        {
            wchar_t sp[MAX_PATH];
            swprintf_s(sp, L"%s\\settings.ini", bvr::log::data_dir());
            if (FILE* f = _wfopen(sp, L"r")) {
                char line[128];
                while (fgets(line, sizeof(line), f)) {
                    if (!strncmp(line, "opening_logos=", 14)) g_showLogos = atoi(line + 14) != 0;
                    if (!strncmp(line, "dlss_mode=", 10)) dlssMode = atoi(line + 10);
                }
                fclose(f);
            }
            BVR_LOG("[bc] opening logos: %s", g_showLogos ? "play (any pad button skips)" : "skipped");
        }
        MH_CreateHookApi(L"user32", "GetForegroundWindow", reinterpret_cast<void*>(&gfw_detour), reinterpret_cast<void**>(&g_gfwOrig));
        MH_CreateHookApi(L"user32", "GetActiveWindow", reinterpret_cast<void*>(&gaw_detour), reinterpret_cast<void**>(&g_gawOrig));
        MH_CreateHookApi(L"user32", "GetFocus", reinterpret_cast<void*>(&gf_detour), reinterpret_cast<void**>(&g_gfOrig));
        MH_EnableHook(MH_ALL_HOOKS);
        g_keepFocus.store(g_gfwOrig && g_gawOrig && g_gfOrig);
        BVR_LOG("[bc] keep running unfocused: %s", g_keepFocus.load() ? "on" : "FAILED (hooks missing)");
        BVR_LOG("[bc] opening-logo skip armed: CreateFileW %s, GetFileAttributesW %s, FindFirstFileW %s",
                g_cfOrig ? "ok" : "MISSING", g_gfaOrig ? "ok" : "MISSING", g_fffOrig ? "ok" : "MISSING");
        unsigned w = 0, h = 0;
        current_launch_res(&w, &h);
        g_targetW = w ? w : 2064u;
        g_targetH = h ? h : 2208u;
        BVR_LOG("[bc] VR window client size for this launch: %ux%u", g_targetW, g_targetH);
        // DLSS (DLSS_PLAN.md D1b): the game draws at the launch size x the quality's scale, the headset images keep
        // the launch size and DLSS fills them.
        if (dlssMode != 0) {
            const bool ok = dlss_bridge::available();
            dlssMode = !ok ? 0 : dlssMode < 0 ? 5 : (dlssMode > 5 ? 5 : dlssMode);
        }
        dlss_bridge::set_launch(dlssMode, g_targetW, g_targetH);
        if (dlssMode >= 2) {
            const unsigned outW = g_targetW, outH = g_targetH;
            const float k = dlss_bridge::render_scale(dlssMode);
            g_targetW = (unsigned(float(outW) * k + 1.0f) & ~1u);
            g_targetH = (unsigned(float(g_targetW) * float(outH) / float(outW) + 1.0f) & ~1u);
            bvr::vr::set_output_size(outW, outH);
            BVR_LOG("[bc] DLSS mode %d: the game draws %ux%u, DLSS fills %ux%u headset images", dlssMode, g_targetW,
                    g_targetH, outW, outH);
        }
        BVR_LOG("[bc] desktop-size spoof for the game's own calls: GetSystemMetrics %s, SystemParametersInfoW %s, GetMonitorInfoW %s",
                g_gsmOrig ? "ok" : "MISSING", g_spiOrig ? "ok" : "MISSING", g_gmiOrig ? "ok" : "MISSING");
    } else {
        BVR_LOG("[bc] not a windowed launch - real desktop size kept");
    }
}

void apply_size_clamp_patch() {
    const wchar_t* cmd = GetCommandLineW();
    if (!cmd || !wcsstr(cmd, L"-windowed")) return;
    static const uint8_t kW[3] = {0x0F, 0x4F, 0xFD};
    static const uint8_t kH[3] = {0x0F, 0x4F, 0xF1};
    const bool w = patch3(0x00BAF615, kW);
    const bool h = patch3(0x00BAF63F, kH);
    BVR_LOG("[bc] window size clamp removed: width %s, height %s", w ? "ok" : "FAILED", h ? "ok" : "FAILED");
    static const uint8_t kVp[] = {0x6A, 0xFF, 0x68, 0xC8, 0x56, 0x18, 0x01, 0x64, 0xA1};
    if (memcmp(reinterpret_cast<void*>(0x00BAF0A0), kVp, sizeof(kVp)) == 0) {
        const MH_STATUS c = MH_CreateHook(reinterpret_cast<void*>(0x00BAF0A0), reinterpret_cast<void*>(&vp_resize_detour),
                                          reinterpret_cast<void**>(&g_vpResizeOrig));
        const MH_STATUS e = c == MH_OK ? MH_EnableHook(reinterpret_cast<void*>(0x00BAF0A0)) : c;
        BVR_LOG("[bc] viewport resize hook @00BAF0A0: %s", MH_StatusToString(e));
    } else {
        BVR_LOG("[bc] viewport resize @00BAF0A0: unexpected prologue - NOT hooked");
    }
}

const Preset* presets(int* count) {
    *count = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));
    return kPresets;
}

void current_launch_res(unsigned* w, unsigned* h) {
    wchar_t path[MAX_PATH];
    launch_ini_path(path, MAX_PATH);
    *w = GetPrivateProfileIntW(L"launch", L"ResX", 0, path);
    *h = GetPrivateProfileIntW(L"launch", L"ResY", 0, path);
}

bool show_logos() { return g_showLogos; }
bool logo_video_opened() { return g_logoUsm.load(std::memory_order_relaxed); }
void forget_logo_video() { g_logoUsm.store(false, std::memory_order_relaxed); }
void set_movie_band(bool on) {
    g_bandEnable.store(on);
    BVR_LOG("[bc] movie band %s", on ? "ON (full-screen movies laid out in a 16:9 band)" : "off");
}
void set_show_logos(bool on) { g_showLogos = on; } // takes effect at the next launch
// FOCUSAUDIO (JJ 2026-10-10: "when first launching the game, the window opens and it goes into VR, but there's no
// audio. I have to actually click on the window"): KEEP RUNNING UNFOCUSED rewrites the activation messages the window
// GETS, but a window that opens behind another (SteamVR's) never gets one, so the game never hears it is active and
// keeps its sound down until a click. Once, 2-30 s after the window exists, if it really is not in front: post the
// messages a click would bring (activate app, activate, focus).
// FOCUSAUDIO2 (JJ 00:55: sound "for a split second when it first launches into VR and then it stops"): the posted
// messages un-muted it, then the game muted again - it also checks the REAL front window outside the game thread
// (KEEP FOCUS only answers the game thread). A click works because it makes the window really the front one, so do
// that: attach to the front window's input queue, bring the game window to the front, detach. Up to 4 tries, >= 3 s
// apart, while the window is not really in front, in the first 40 s; the posted messages stay as the fallback.
ULONGLONG g_wndAt = 0, g_lastTry = 0;
int g_fgTries = 0;
void focus_audio_tick() {
    if (g_fgTries >= 4 || !g_gameWnd || !g_keepFocus.load(std::memory_order_relaxed)) return;
    const ULONGLONG now = GetTickCount64();
    if (!g_wndAt) { g_wndAt = now; return; }
    if (now - g_wndAt < 2000 || now - g_lastTry < 3000) return;
    if (now - g_wndAt > 40000) { g_fgTries = 4; return; }
    const HWND front = g_gfwOrig ? g_gfwOrig() : GetForegroundWindow(); // the real one, not our answer
    if (front == g_gameWnd) return; // really in front
    // FOCUSAUDIO3: Windows refused SetForegroundWindow every time (01:14 log) - no more attempts or input-queue
    // attaching; the posted activation (once) plus the whole-process front-window answer (gfw_detour) do the job.
    g_fgTries = 4;
    PostMessageW(g_gameWnd, WM_ACTIVATEAPP, TRUE, 0);
    PostMessageW(g_gameWnd, WM_ACTIVATE, MAKEWPARAM(WA_ACTIVE, 0), 0);
    PostMessageW(g_gameWnd, WM_SETFOCUS, 0, 0);
    BVR_LOG("[bc] game window not in front at start (%p in front) - told it it is active", static_cast<void*>(front));
}
void tick() {
    logo_tick();
    movie_band_tick();
    focus_audio_tick();
}
void skip_logos() {
    if (!g_logoSkip.exchange(true)) BVR_LOG("[bc] opening logos: skip requested (pad button)");
}
bool logo_skip_requested() { return g_logoSkip.load(); }

bool keep_focus() { return g_keepFocus.load(); }
void set_keep_focus(bool on) {
    if (on && !g_gfwOrig) return; // not a VR launch: hooks were never installed
    g_keepFocus.store(on);
    BVR_LOG("[bc] keep running unfocused: %s", on ? "on" : "off");
}

bool save_launch_res(unsigned w, unsigned h) {
    wchar_t path[MAX_PATH], v[32];
    launch_ini_path(path, MAX_PATH);
    swprintf_s(v, L"%u", w);
    bool ok = WritePrivateProfileStringW(L"launch", L"ResX", v, path) != 0;
    swprintf_s(v, L"%u", h);
    ok = WritePrivateProfileStringW(L"launch", L"ResY", v, path) != 0 && ok;
    BVR_LOG("[bc] VR resolution for the next launch: %ux%u (%s)", w, h, ok ? "saved" : "SAVE FAILED");
    return ok;
}

void draw_ui() {
    unsigned bw = 0, bh = 0;
    bvr::hud::backbuffer_dims(&bw, &bh);
    unsigned rw = 0, rh = 0;
    bvr::vr::recommended_eye_size(&rw, &rh);
    unsigned nw = 0, nh = 0;
    current_launch_res(&nw, &nh);
    ImGui::Text("Render size now: %ux%u per eye (headset recommends %ux%u)", bw, bh, rw, rh);
    ImGui::Text("Next launch: %ux%u   (desktop-size answers given: %u)", nw ? nw : 2064u, nh ? nh : 2208u, g_spoofHits.load());
    // Slider (JJ 2026-10-03): a scale of the Quest 3's native 2064x2208 eye, same shape,
    // rounded to a multiple of 8. Saved when the slider is released.
    static float s_scale = -1.0f;
    if (s_scale < 0.0f) s_scale = (nw ? float(nw) : 2064.0f) / 2064.0f;
    const unsigned pw = (static_cast<unsigned>(2064.0f * s_scale) + 4) / 8 * 8;
    const unsigned ph = (static_cast<unsigned>(2208.0f * s_scale) + 4) / 8 * 8;
    char fmt[64];
    sprintf_s(fmt, "%%.2fx  (%ux%u)", pw, ph);
    bvr::overlay::SliderStep("render resolution", &s_scale, 0.6f, 1.8f, fmt);
    if (ImGui::IsItemDeactivatedAfterEdit()) save_launch_res(pw, ph);
    ImGui::TextDisabled("1.00x = Quest 3 native through Virtual Desktop. Higher is sharper and slower.");
    ImGui::TextDisabled("Applies when the game is restarted from the Batman City VR launcher.");
}

} // namespace bvr::ac::window
