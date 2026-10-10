#pragma once
// Batman City DLSS bridge (DLSS_PLAN.md): the 32-bit side of the hand-off to the 64-bit helper bc_dlss64.exe
// (NVIDIA's DLSS library exists only as 64-bit). Stage D0: every eye picture goes through the helper as a plain
// copy, to prove the hand-off and measure its cost.

#include <cstdint>

struct ID3D11Device;

namespace bvr::ac::dlss_bridge {

void install_early();                    // before the game makes its device: DXGI 1.1 factory (shared textures)
void install();                          // shared block + events + start the helper (once, VR launches)
void set_enabled(bool on);               // route eye pictures through the helper
bool enabled();
// DLSS (D1): mode 0 off, 1 DLAA, 2 ultra quality, 3 quality, 4 balanced, 5 performance.
int mode();
void set_mode(int m);
// The render size is chosen at launch (window.cpp): launch size x render_scale(saved mode); the headset images keep
// the launch size and DLSS fills them. A live mode change switches DLSS on/off; the size follows on the next launch.
float render_scale(int m);
void set_launch(int m, unsigned outW, unsigned outH);
int launch_mode();
bool available(); // helper + NVIDIA's DLSS library next to the game, first graphics card NVIDIA
bool launched();  // a VR launch chose the render size (flat launches: the saved mode is only kept)
int saved_mode();             // the choice saved in settings.ini (applies at the next launch)
void set_saved_mode(int m);
void draw_ui();               // the panel's DLSS section
// Game thread, per eye pass: this frame's sub-pixel jitter (render pixels; false = none) ...
bool jitter(uint32_t frame, int renderW, int renderH, float* jx, float* jy);
// ... and, after the FSceneView is built, its view x lens (jittered, view+0x250) and the eye view matrix.
void set_pass(uint32_t slot, int eye, const float vpJ[16], const float eyeView[16], float jx, float jy, int renderW,
              int renderH);
void set_render_slot(uint32_t slot); // render thread, BeginDrawing
bool command(const char* args);      // "bc dlss ..."
void status_line(char* out, unsigned n);

} // namespace bvr::ac::dlss_bridge
