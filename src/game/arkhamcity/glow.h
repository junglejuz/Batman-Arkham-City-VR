#pragma once
// Batman City GLOW (JJ 2026-10-08: "the in-game glows are very flickery ... the glow is very low resolution ... is it
// possible to remove the glows and add one of our own"). The game builds its bloom at a quarter of the picture with
// a hard threshold on the coarse picture, so tiny distant lights pop on and off and every glow is blocky. At the
// final combine pass (the "uber" pass: scene HDR + bloom + light shafts + exposure -> LDR) the game's bloom input is
// replaced by an empty one and the scene input by the scene with our own bloom added: built from the full-size HDR
// scene, Karis-averaged first downsample (no fireflies), six-level pyramid, tent upsample.

struct ID3D11DeviceContext;
#include <string>

namespace bvr::ac::glow {

// Render thread, every DrawIndexed(6) before it runs: true when this was the combine pass and our inputs are bound
// (after_draw puts the game's back).
bool on_draw(ID3D11DeviceContext* ctx);
// SHADERDUMP (diagnostic): hook device creation before the game makes its device, record pixel shader bytecode, and save
// the shaders of full-screen passes once (%LOCALAPPDATA%\Batman_City\shaders, listed in the log).
void install_early();
void note_fullscreen_pass(ID3D11DeviceContext* ctx);
void after_draw(ID3D11DeviceContext* ctx);
// SPRITEDIM: every non-indexed Draw, before / after it runs (additive scene sprite census + dimmer).
bool on_draw_plain(ID3D11DeviceContext* ctx, unsigned vertexCount); // true = skip the draw (glow sprites off)
void after_draw_plain(ID3D11DeviceContext* ctx);

bool command(const char* args); // "bc glow ..."
void draw_ui();
void settings_write(std::string& out);
bool settings_read(const char* key, const char* val);
void status_text(std::string& out);

} // namespace bvr::ac::glow
