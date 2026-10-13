#include "overlay.h"

#include "core/framework/framework.h"
#include "core/gfx/frame_inspector.h"
#include "core/input/xinput_bridge.h"
#include "core/util/crash.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/igame_adapter.h"

#include <windows.h>
#include <d3d11.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <Xinput.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace bvr::overlay {
namespace {

bool g_initialized = false;
bool g_visible = false;
std::atomic<int> g_visibleRequest{-1}; // session 22: seam toggle (-1 = none)
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
ID3D11Texture2D* g_rtvBackbuffer = nullptr; // identity only, never deref'd
HWND g_window = nullptr;
WNDPROC g_originalWndProc = nullptr;
// The game window's messages arrive on the game thread, the panel frame runs on the render thread: both touch ImGui's
// input queue (PANELMOUSE). RECURSIVE (JJ 2026-10-13: a mouse click opening a drawer crashed the game): the backend's
// ReleaseCapture on button-up sends WM_CAPTURECHANGED straight back into WndProc on the same thread - a plain mutex
// locked twice threw (0xE06D7363).
std::recursive_mutex g_inputMutex;

// Batman City: the VR menu panel. While an XR session is live the overlay renders into its own
// texture (kPanelPx square) that core shows as a room-fixed quad (AKVR's panel pattern), instead
// of painting into the eye images where it could not fuse.
constexpr unsigned kPanelPx = 1440; // AKVR's panel: 1440 px shown 0.984 m wide, 1.30 m ahead
ID3D11Texture2D* g_panelTex = nullptr;
ID3D11RenderTargetView* g_panelRtv = nullptr;

bool ensure_panel() {
    if (g_panelTex) return true;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kPanelPx;
    td.Height = kPanelPx;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_device->CreateTexture2D(&td, nullptr, &g_panelTex)) ||
        FAILED(g_device->CreateRenderTargetView(g_panelTex, nullptr, &g_panelRtv))) {
        if (g_panelTex) { g_panelTex->Release(); g_panelTex = nullptr; }
        BVR_LOG("overlay: VR panel texture creation FAILED");
        return false;
    }
    BVR_LOG("overlay: VR menu panel %ux%u", kPanelPx, kPanelPx);
    return true;
}

// Gamepad chord: both sticks clicked together toggles the menu (AKVR's open gesture). Read
// through xinput1_4 directly with the menu-poll flag so the game-pad block never hides it.
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetStateFn g_xiGetState = nullptr;
std::atomic<ChordTapFn> g_chordDoubleTap{nullptr};
bool pad_chord_down() {
    if (!g_xiGetState) {
        static bool tried = false;
        if (tried) return false;
        tried = true;
        HMODULE m = LoadLibraryW(L"xinput1_4.dll");
        if (m) g_xiGetState = reinterpret_cast<XInputGetStateFn>(GetProcAddress(m, "XInputGetState"));
        if (!g_xiGetState) return false;
    }
    XINPUT_STATE s{};
    input::set_menu_poll(true);
    const DWORD r = g_xiGetState(0, &s);
    input::set_menu_poll(false);
    if (r != ERROR_SUCCESS) return false;
    const WORD both = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
    return (s.Gamepad.wButtons & both) == both;
}

// ONESTEP state (render thread): the overlay frame on which a step slider was last controller-active.
int g_frameNo = 0;
int g_sliderOwnFrame = -100;
// STICKNAV (JJ 2026-10-10: the panel "won't let me go any higher than the DLSS shelf. It just keeps looping back to
// down low"): ImGui scrolls the page freely with the left stick, but the D-pad moves the highlight and every move
// scrolls the page back to wherever the highlight is - two controls fighting over the page. On: stick up / down moves
// the highlight like the D-pad (the page follows it), the stick's free scroll is off.
std::atomic<bool> g_stickNav{true};
std::atomic<bool> g_showTests{false}; // PANELTIDY
float g_panelScroll = 0, g_panelScrollMax = 0; // PANELLOG: the page's scroll position last frame

int tweak_dir() {
    int dir = 0;
    if (!g_xiGetState) pad_chord_down(); // loads xinput1_4 once
    if (g_xiGetState) {
        XINPUT_STATE s{};
        input::set_menu_poll(true);
        const DWORD r = g_xiGetState ? g_xiGetState(0, &s) : ERROR_DEVICE_NOT_CONNECTED;
        input::set_menu_poll(false);
        if (r == ERROR_SUCCESS) {
            if ((s.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) || s.Gamepad.sThumbLX > 16000) dir += 1;
            if ((s.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) || s.Gamepad.sThumbLX < -16000) dir -= 1;
        }
    }
    if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) dir += 1;
    if (ImGui::IsKeyDown(ImGuiKey_LeftArrow)) dir -= 1;
    return dir > 0 ? 1 : (dir < 0 ? -1 : 0);
}

// One displayed step per press, before the widget draws; returns the value to show.
float step_value(ImGuiID id, float v, float lo, float hi, float step, bool* owned) {
    static ImGuiID s_id = 0;
    static int s_dir = 0;
    static ULONGLONG s_next = 0, s_held = 0;
    const bool mouse = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool typing = ImGui::TempInputIsActive(id);
    *owned = ImGui::GetActiveID() == id && !mouse && !typing;
    if (!*owned) return v;
    g_sliderOwnFrame = g_frameNo;
    const int dir = tweak_dir();
    const ULONGLONG now = GetTickCount64();
    bool stepNow = false;
    if (dir == 0) s_dir = 0;
    else if (s_id != id || s_dir != dir) { s_id = id; s_dir = dir; s_next = now + 300; s_held = now; stepNow = true; }
    else if (now >= s_next) { s_next = now + 70; stepNow = true; }
    const ULONGLONG held = now - s_held;
    const float mult = held > 2500 ? 25.0f : (held > 1000 ? 5.0f : 1.0f);
    if (stepNow) v = roundf(v / step) * step + float(dir) * step * mult;
    return v < lo ? lo : (v > hi ? hi : v);
}

float fmt_step(const char* fmt) {
    int dec = 2;
    const char* dot = strchr(fmt, '.');
    if (dot && dot[1] >= '0' && dot[1] <= '9') dec = dot[1] - '0';
    else if (strstr(fmt, "%d") || strstr(fmt, "%.0f")) dec = 0;
    return powf(10.0f, float(-dec));
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    // Session 38: the subclass is on the GAME's main window, so it is the
    // earliest game-agnostic sight of a close. BS2's engine faults on its own
    // exit path (hook-free-proven); noting teardown here turns that into a
    // quiet fast exit instead of a dump per close. WM_ENDSESSION covers
    // logoff/shutdown. Always forwarded - observation only.
    if (msg == WM_CLOSE || msg == WM_DESTROY || (msg == WM_ENDSESSION && wparam))
        crash::note_teardown(msg == WM_CLOSE     ? "WM_CLOSE"
                             : msg == WM_DESTROY ? "WM_DESTROY"
                                                 : "WM_ENDSESSION");
    if (g_visible) {
        {
            std::lock_guard<std::recursive_mutex> lk(g_inputMutex);
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
        }
        // Session 22 (user report: overlay unusable while scrolling): while
        // ImGui owns the mouse/keyboard, CONSUME those messages instead of
        // letting the game fight the overlay for them (the wheel doubled as
        // weapon-cycle, clicks re-captured the cursor mid-drag).
        ImGuiIO& io = ImGui::GetIO();
        bool mouseMsg = msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST;
        bool keyMsg = msg >= WM_KEYFIRST && msg <= WM_KEYLAST;
        if ((mouseMsg && io.WantCaptureMouse) || (keyMsg && io.WantCaptureKeyboard))
            return TRUE;
    }
    return CallWindowProcW(g_originalWndProc, hwnd, msg, wparam, lparam);
}

void map_mouse_to_panel() {
    // PANELMOUSE (player report 2026-10-13: F10 cursor offset from the items, "pulls in a certain direction",
    // "renders double"): the old code scaled io.MousePos, last frame's value - the backend's queued mouse events
    // (window pixels) replaced it whenever the mouse moved, and every still frame scaled it again, so the cursor
    // jumped between two places and crept to a corner. Map the QUEUED events instead, from the part of the window
    // the cursor can actually reach (the VR window is taller than most monitors) onto the whole panel.
    RECT cr{};
    if (GetClientRect(g_window, &cr)) {
        POINT tl{cr.left, cr.top}, br{cr.right, cr.bottom};
        ClientToScreen(g_window, &tl);
        ClientToScreen(g_window, &br);
        RECT vis{tl.x, tl.y, br.x, br.y};
        MONITORINFO mi{sizeof(mi)};
        RECT on{};
        if (GetMonitorInfoW(MonitorFromWindow(g_window, MONITOR_DEFAULTTONEAREST), &mi) &&
            IntersectRect(&on, &vis, &mi.rcMonitor))
            vis = on;
        const float vx = float(vis.left - tl.x), vy = float(vis.top - tl.y); // in client pixels
        const float vw = float(vis.right - vis.left), vh = float(vis.bottom - vis.top);
        if (vw > 0 && vh > 0) {
            for (ImGuiInputEvent& e : GImGui->InputEventsQueue) {
                if (e.Type != ImGuiInputEventType_MousePos || e.MousePos.PosX <= -FLT_MAX) continue;
                e.MousePos.PosX = ImClamp((e.MousePos.PosX - vx) / vw, 0.0f, 1.0f) * float(kPanelPx);
                e.MousePos.PosY = ImClamp((e.MousePos.PosY - vy) / vh, 0.0f, 1.0f) * float(kPanelPx);
            }
        }
    }
}

bool CreateRenderTarget(IDXGISwapChain* swapchain) {
    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer))))
        return false;
    HRESULT hr = g_device->CreateRenderTargetView(backbuffer, nullptr, &g_rtv);
    g_rtvBackbuffer = SUCCEEDED(hr) ? backbuffer : nullptr;
    backbuffer->Release();
    return SUCCEEDED(hr);
}

bool Init(IDXGISwapChain* swapchain) {
    if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&g_device))))
        return false;
    g_device->GetImmediateContext(&g_context);

    DXGI_SWAP_CHAIN_DESC desc{};
    swapchain->GetDesc(&desc);
    g_window = desc.OutputWindow;

    if (!CreateRenderTarget(swapchain)) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // don't scatter imgui.ini into the game folder
    ImGui::StyleColorsDark();
    { // AKVR's font: ImGui default at 26 px, shown at 0.78 on the VR panel (hooks.cpp)
        ImFontConfig fcfg;
        fcfg.SizePixels = 26.0f;
        io.Fonts->AddFontDefault(&fcfg);
    }
    ImGui_ImplWin32_Init(g_window);
    ImGui_ImplDX11_Init(g_device, g_context);

    g_originalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));

    BVR_LOG("overlay initialized (hwnd=%p) - F10 toggles it", g_window);
    return true;
}

void DrawUi(bool vrPanel) {
    ImGuiIO& io = ImGui::GetIO();
    // AKVR's panel format (hooks.cpp draw_panel): the panel IS the window, fixed text size,
    // sliders at 42% of the width with the label beside them, all text wrapped.
    ImGui::SetNextWindowPos(ImVec2(0, 0), vrPanel ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
    if (vrPanel) ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
    else ImGui::SetNextWindowSize(ImVec2(760, 900), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.96f); // JJ 2026-10-11: "a bit less transparent" (was 0.86)
    ImGui::Begin("Batman City VR", nullptr,
                 vrPanel ? (ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar)
                         : ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.42f);
    ImGui::PushTextWrapPos(0.0f);
    {
        // First open: focus the window so the first controller press moves the highlight.
        static bool s_was = false;
        if (!s_was) ImGui::SetWindowFocus();
        s_was = true;
    }
    if (auto* adapter = game::adapter()) {
        adapter->drawDebugUi(); // the game's own panel (AKVR layout)
    } else {
        ImGui::Text("%.1f fps", io.Framerate);
    }
    ImGui::PopTextWrapPos();
    ImGui::PopItemWidth();
    g_panelScroll = ImGui::GetScrollY();
    g_panelScrollMax = ImGui::GetScrollMaxY();
    ImGui::End();
}

} // namespace

void on_present(IDXGISwapChain* swapchain) {
    if (!g_initialized) {
        g_initialized = Init(swapchain);
        if (!g_initialized) return;
    }
    // Session 22 (user report: overlay gone until an alt-tab): if the game
    // swaps its backbuffer object WITHOUT a ResizeBuffers (fullscreen-state
    // churn), a held RTV keeps drawing into the dead buffer - F10 toggles an
    // overlay nobody can see. Track the buffer identity and re-create.
    {
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&bb)))) {
            if (g_rtv && bb != g_rtvBackbuffer) {
                g_rtv->Release();
                g_rtv = nullptr;
                BVR_LOG("overlay: backbuffer identity changed - RTV re-created");
            }
            bb->Release();
        }
    }
    if (!g_rtv && !CreateRenderTarget(swapchain)) return;

    // F10, edge-triggered (Insert was the original choice, but not every
    // keyboard has it); the seam request lane covers harness toggling.
    static bool wasDown = false;
    bool isDown = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (isDown && !wasDown) {
        g_visible = !g_visible;
        ImGui::GetIO().MouseDrawCursor = g_visible;
    }
    wasDown = isDown;
    // Both stick clicks must be HELD 0.6 s (AKVR's akvr_gamepad_menu_chord_pressed, HOLD_MS 600;
    // JJ 2026-10-04: "after a couple of seconds, not straight away"). Fires once per hold.
    {
        static ULONGLONG chordSince = 0, lastTap = 0;
        static bool chordFired = false;
        const ULONGLONG now = GetTickCount64();
        if (pad_chord_down()) {
            if (!chordSince) chordSince = now;
            if (!chordFired && now - chordSince >= 600) {
                chordFired = true;
                g_visible = !g_visible;
                ImGui::GetIO().MouseDrawCursor = g_visible;
            }
        } else {
            // Batman City FIRSTPERSON (JJ 2026-10-09): two quick clicks of both sticks (each under 0.35 s, the second
            // within 0.6 s of the first) = the double-tap action; the 0.6 s hold stays the menu.
            if (chordSince && !chordFired && now - chordSince < 350) {
                if (lastTap && now - lastTap < 600) {
                    lastTap = 0;
                    if (ChordTapFn f = g_chordDoubleTap.load()) f();
                } else {
                    lastTap = now;
                }
            }
            chordSince = 0;
            chordFired = false;
        }
    }
    int req = g_visibleRequest.exchange(-1, std::memory_order_relaxed);
    if (req >= 0) {
        g_visible = req != 0;
        ImGui::GetIO().MouseDrawCursor = g_visible;
    }

    const bool vrPanel = vr::session_live() && ensure_panel();
    input::set_block_game_pad(g_visible && vrPanel);
    if (!g_visible) {
        vr::set_panel_texture(nullptr);
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    if (vrPanel) io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    else io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    io.FontGlobalScale = vrPanel ? 0.78f : 0.5f; // AKVR: 26 px font x 0.78 on the 1440 px panel

    ImGui_ImplDX11_NewFrame();
    std::unique_lock<std::recursive_mutex> inputLock(g_inputMutex); // until NewFrame has taken the queued input
    input::set_menu_poll(true); // the backend's own XInput poll must see the pad
    ImGui_ImplWin32_NewFrame();
    input::set_menu_poll(false);
    if (vrPanel) io.DisplaySize = ImVec2(float(kPanelPx), float(kPanelPx));
    // ONESTEP: while one of our step sliders was controller-active last frame, ImGui must not see left/right (its own
    // tweak would be a second writer). The backend queued this frame's pad state above; queue "released" after it and
    // let the whole queue apply at once (no trickling), so the last state wins.
    io.ConfigInputTrickleEventQueue = false;
    if (g_frameNo - g_sliderOwnFrame <= 1) {
        io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, false);
        io.AddKeyEvent(ImGuiKey_GamepadDpadRight, false);
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, false, 0.0f);
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, false, 0.0f);
    }
    if (vrPanel) {
        XINPUT_STATE s{};
        if (!g_xiGetState) pad_chord_down(); // loads xinput1_4 once
        input::set_menu_poll(true);
        const bool padOk = g_xiGetState && g_xiGetState(0, &s) == ERROR_SUCCESS;
        input::set_menu_poll(false);
        const bool dU = padOk && (s.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP);
        const bool dD = padOk && (s.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN);
        const bool sU = padOk && s.Gamepad.sThumbLY > 16000, sD = padOk && s.Gamepad.sThumbLY < -16000;
        if (g_stickNav.load(std::memory_order_relaxed)) {
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, false, 0.0f);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, false, 0.0f);
            io.AddKeyEvent(ImGuiKey_GamepadDpadUp, dU || sU);
            io.AddKeyEvent(ImGuiKey_GamepadDpadDown, dD || sD);
        }
        // PANELLOG: what the controller does to the page, once a second while it is pushed up or down
        static ULONGLONG s_logAt = 0;
        static int s_logs = 0;
        const ULONGLONG now = GetTickCount64();
        if ((dU || dD || sU || sD) && now - s_logAt >= 1000 && s_logs < 120) {
            s_logAt = now;
            ++s_logs;
            BVR_LOG("overlay: pad on the panel - d-pad %s%s, stick y %d | page scroll %.0f of %.0f | stick moves %s",
                    dU ? "UP" : "", dD ? "DOWN" : "", padOk ? int(s.Gamepad.sThumbLY) : 0, g_panelScroll,
                    g_panelScrollMax, g_stickNav.load() ? "the highlight" : "the page");
        }
    }
    ++g_frameNo;
    if (vrPanel) map_mouse_to_panel();
    ImGui::NewFrame();
    inputLock.unlock();
    DrawUi(vrPanel);
    ImGui::Render();
    if (vrPanel) {
        const float clear[4] = {0, 0, 0, 0};
        g_context->ClearRenderTargetView(g_panelRtv, clear);
        g_context->OMSetRenderTargets(1, &g_panelRtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        vr::set_panel_texture(g_panelTex);
    } else {
        vr::set_panel_texture(nullptr);
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
}

void on_resize() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
    g_rtvBackbuffer = nullptr;
}

void set_chord_double_tap(ChordTapFn fn) { g_chordDoubleTap.store(fn); }
bool show_tests() { return g_showTests.load(std::memory_order_relaxed); }
void set_show_tests(bool on) { g_showTests.store(on); }
void set_stick_nav(bool on) {
    g_stickNav.store(on);
    BVR_LOG("overlay: stick up/down moves %s", on ? "the highlight (the page follows it)" : "the page (ImGui's own scroll)");
}

void set_visible(bool on) {
    g_visibleRequest.store(on ? 1 : 0, std::memory_order_relaxed);
}

bool SliderStep(const char* label, float* v, float lo, float hi, const char* fmt, int flags) {
    const float step = fmt_step(fmt);
    const ImGuiID id = ImGui::GetID(label);
    const float before = *v;
    bool owned = false;
    *v = step_value(id, *v, lo, hi, step, &owned);
    const float stepped = *v;
    const bool changed = ImGui::SliderFloat(label, v, lo, hi, fmt, flags);
    const bool mouse = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (owned || (ImGui::IsItemActive() && !mouse && !ImGui::TempInputIsActive(id)))
        *v = stepped; // ImGui's own keyboard / gamepad tweak discarded
    else if (changed)
        *v = roundf(*v / step) * step; // mouse drag / typed value lands on a displayed step
    if (*v < lo) *v = lo;
    if (*v > hi) *v = hi;
    return *v != before;
}

bool SliderStepInt(const char* label, int* v, int lo, int hi, const char* fmt, int flags) {
    float f = float(*v);
    const ImGuiID id = ImGui::GetID(label);
    const int before = *v;
    bool owned = false;
    f = step_value(id, f, float(lo), float(hi), 1.0f, &owned);
    int iv = int(lroundf(f));
    const int stepped = iv;
    ImGui::SliderInt(label, &iv, lo, hi, fmt, flags);
    if (owned || (ImGui::IsItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::TempInputIsActive(id)))
        iv = stepped;
    *v = iv < lo ? lo : (iv > hi ? hi : iv);
    return *v != before;
}

} // namespace bvr::overlay
