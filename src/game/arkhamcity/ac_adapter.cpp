#include "game/arkhamcity/ac_adapter.h"

#include "core/framework/command.h"
#include "core/util/log.h"
#include "game/arkhamcity/addresses.h"
#include "game/arkhamcity/scene.h"
#include "core/input/dinput_block.h"
#include "game/arkhamcity/dlss_bridge.h"
#include "game/arkhamcity/glow.h"
#include "game/arkhamcity/window.h"

#include <imgui.h>
#include <windows.h>

#include <cstring>

namespace bvr::ac {
namespace {

bool g_buildVerified = false;

// Reads the mapped PE header of the host exe. The header is plaintext even while
// SteamStub still holds .text encrypted.
bool verify_build(const bvr::pattern_scan::ProcessImage& image) {
    const auto* base = image.base;
    if (!base) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const uint32_t stamp = nt->FileHeader.TimeDateStamp;
    const uint32_t size = nt->OptionalHeader.SizeOfImage;
    const bool baseOk = reinterpret_cast<uintptr_t>(base) == addr::kImageBase;
    BVR_LOG("[ac] host build: timestamp 0x%08X size 0x%08X base %p -> %s", stamp, size, base,
            (stamp == addr::kTimeDateStamp && size == addr::kSizeOfImage && baseOk)
                ? "VERIFIED (Arkham City GOTY Steam 2023-02-26)"
                : "NOT RECOGNISED - engine hooks stay off, game runs flat");
    return stamp == addr::kTimeDateStamp && size == addr::kSizeOfImage && baseOk;
}

} // namespace

uint32_t ArkhamCityAdapter::capabilities() const {
    return scene::hook_live() ? (bvr::game::CAP_CAMERA_OVERRIDE | bvr::game::CAP_SCENE_REENTRY) : 0u;
}

bool ArkhamCityAdapter::init(const bvr::pattern_scan::ProcessImage& image) {
    // Command surface from frame one (Present pump), before any engine hook fires.
    bvr::command::enable_present_pump();
    g_buildVerified = verify_build(image);
    if (!g_buildVerified) return false;
    glow::install_early();       // shader recorder (diagnostic): before the game makes its device
    dlss_bridge::install_early(); // before the game makes its device: DXGI 1.1 factory (shared textures)
    window::install(); // before the game creates its window: lift the monitor-size cap
    bvr::input::dinput_block::install(); // the VR menu also blocks DirectInput pads
    const bool ok = scene::install(); // waits for SteamStub plaintext
    window::apply_size_clamp_patch();
    BVR_LOG("[ac] engine seam %s", ok ? "installed (camera + stereo default OFF)" : "NOT installed");
    return ok;
}

void ArkhamCityAdapter::setFov(float) {}

void ArkhamCityAdapter::drawDebugUi() {
    if (!g_buildVerified) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "This Arkham City build is not recognised - VR is off.");
        return;
    }
    scene::draw_ui();
}

bool ArkhamCityAdapter::handleCommand(const char* cmd, const char* args) {
    if (_stricmp(cmd, "bc") != 0) return false;
    return scene::handle_command(args);
}

bvr::game::IGameAdapter* create_adapter() {
    static ArkhamCityAdapter instance;
    return &instance;
}

} // namespace bvr::ac
