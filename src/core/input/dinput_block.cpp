#include "core/input/dinput_block.h"

#include "core/input/xinput_bridge.h"
#include "core/util/log.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <MinHook.h>
#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")

// Batman City (JJ 2026-10-04: "the VR overlay is still passing through gamepad controls"). The
// game reads controllers through XInput (blocked in xinput_bridge) AND DirectInput: it has
// UE3's AllowJoystickInput=1 and filters Xbox pads out of DirectInput with the WMI "IG_" check.
// If that filter misses a pad, the game reads it twice and the XInput block alone leaks. While
// the menu is open, every DirectInput GAME CONTROLLER read here returns the state from just
// before the menu opened (frozen - nothing changes, so nothing is pressed) and buffered events
// are dropped. Keyboard and mouse devices are never touched.

namespace bvr::input::dinput_block {
namespace {

using GetDeviceStateFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPVOID);
using GetDeviceDataFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
GetDeviceStateFn g_stateOrig[2] = {};
GetDeviceDataFn g_dataOrig[2] = {};

struct Dev {
    void* ptr = nullptr;
    int kind = -1; // -1 unknown, 0 not a controller, 1 controller
    DWORD bytes = 0;
    unsigned char frozen[512] = {};
};
Dev g_devs[8];
std::mutex g_mutex;
std::atomic<uint32_t> g_blockedReads{0};

bool is_controller_type(DWORD devType) {
    const BYTE t = GET_DIDEVICE_TYPE(devType);
    return (t >= DI8DEVTYPE_JOYSTICK && t <= DI8DEVTYPE_1STPERSON) || t == DI8DEVTYPE_SUPPLEMENTAL;
}

// Caller holds g_mutex. GetCapabilities is vtable slot 3 in both the A and W interfaces.
Dev* lookup(void* dev) {
    for (Dev& d : g_devs)
        if (d.ptr == dev) return &d;
    for (Dev& d : g_devs) {
        if (d.ptr) continue;
        d.ptr = dev;
        DIDEVCAPS caps{};
        caps.dwSize = sizeof(caps);
        auto* w = static_cast<IDirectInputDevice8W*>(dev);
        d.kind = SUCCEEDED(w->GetCapabilities(&caps)) && is_controller_type(caps.dwDevType) ? 1 : 0;
        if (d.kind == 1)
            BVR_LOG("input: the game reads a game controller through DirectInput too (type 0x%02X) - "
                    "frozen while the VR menu is open", GET_DIDEVICE_TYPE(caps.dwDevType));
        return &d;
    }
    return nullptr;
}

template <int I>
HRESULT STDMETHODCALLTYPE state_detour(IDirectInputDevice8W* self, DWORD cb, LPVOID data) {
    const HRESULT hr = g_stateOrig[I](self, cb, data);
    if (FAILED(hr) || !data || cb == 0 || cb > sizeof(Dev::frozen)) return hr;
    std::lock_guard<std::mutex> lk(g_mutex);
    Dev* d = lookup(self);
    if (!d || d->kind != 1) return hr;
    if (game_pad_blocked()) {
        if (d->bytes == cb) memcpy(data, d->frozen, cb);
        else memset(data, 0, cb);
        g_blockedReads.fetch_add(1, std::memory_order_relaxed);
    } else {
        memcpy(d->frozen, data, cb);
        d->bytes = cb;
    }
    return hr;
}

template <int I>
HRESULT STDMETHODCALLTYPE data_detour(IDirectInputDevice8W* self, DWORD cbObj, LPDIDEVICEOBJECTDATA rg,
                                      LPDWORD inOut, DWORD flags) {
    const HRESULT hr = g_dataOrig[I](self, cbObj, rg, inOut, flags);
    if (FAILED(hr) || !inOut) return hr;
    std::lock_guard<std::mutex> lk(g_mutex);
    Dev* d = lookup(self);
    if (d && d->kind == 1 && game_pad_blocked() && *inOut) {
        *inOut = 0; // events while the menu is open are the menu's
        g_blockedReads.fetch_add(1, std::memory_order_relaxed);
    }
    return hr;
}

bool hook_slot(void* target, void* detour, void** orig, const char* what) {
    const MH_STATUS c = MH_CreateHook(target, detour, orig);
    if (c == MH_ERROR_ALREADY_CREATED) return true;
    const MH_STATUS e = c == MH_OK ? MH_EnableHook(target) : c;
    if (e != MH_OK) BVR_LOG("input: DirectInput %s hook failed: %s", what, MH_StatusToString(e));
    return e == MH_OK;
}

} // namespace

void install() {
    MH_Initialize(); // idempotent
    HINSTANCE inst = GetModuleHandleW(nullptr);
    int hooked = 0;
    // The A and W device interfaces have their own vtables; hook both (slot 9 = GetDeviceState,
    // slot 10 = GetDeviceData). A throwaway keyboard device yields the shared implementation.
    for (int i = 0; i < 2; ++i) {
        IUnknown* di = nullptr;
        const IID& iid = i == 0 ? IID_IDirectInput8W : IID_IDirectInput8A;
        if (FAILED(DirectInput8Create(inst, DIRECTINPUT_VERSION, iid, reinterpret_cast<void**>(&di), nullptr)) || !di)
            continue;
        IUnknown* dev = nullptr;
        HRESULT hr = i == 0 ? static_cast<IDirectInput8W*>(static_cast<void*>(di))
                                  ->CreateDevice(GUID_SysKeyboard, reinterpret_cast<IDirectInputDevice8W**>(&dev), nullptr)
                            : static_cast<IDirectInput8A*>(static_cast<void*>(di))
                                  ->CreateDevice(GUID_SysKeyboard, reinterpret_cast<IDirectInputDevice8A**>(&dev), nullptr);
        if (SUCCEEDED(hr) && dev) {
            void** vt = *reinterpret_cast<void***>(dev);
            if (i == 0) {
                hooked += hook_slot(vt[9], reinterpret_cast<void*>(&state_detour<0>), reinterpret_cast<void**>(&g_stateOrig[0]), "GetDeviceState");
                hooked += hook_slot(vt[10], reinterpret_cast<void*>(&data_detour<0>), reinterpret_cast<void**>(&g_dataOrig[0]), "GetDeviceData");
            } else { // shared with W: MinHook reports it already hooked, nothing to do
                hooked += hook_slot(vt[9], reinterpret_cast<void*>(&state_detour<1>), reinterpret_cast<void**>(&g_stateOrig[1]), "GetDeviceState(A)");
                hooked += hook_slot(vt[10], reinterpret_cast<void*>(&data_detour<1>), reinterpret_cast<void**>(&g_dataOrig[1]), "GetDeviceData(A)");
            }
            dev->Release();
        }
        di->Release();
    }
    BVR_LOG("input: DirectInput controller block armed (%d hooks)", hooked);
}

uint32_t blocked_reads() { return g_blockedReads.load(std::memory_order_relaxed); }

} // namespace bvr::input::dinput_block
