#pragma once
// Batman City HD SCREENS (JJ 2026-10-10: "replace ... all the loading screens [with] higher resolution AI upscaled
// versions. We can use my magnific account"; BATMAN_CITY_PLAN.md "HD SCREENS"). While a full-screen movie plays (the
// loading screens), every picture a draw samples is fingerprinted once (the RETICLEHD method: decoded to RGBA8, read
// back, FNV-1a) and saved as %LOCALAPPDATA%\Batman_City\hd_dump\<key>_<w>x<h>.png; tools/hd_upscale.py sends those to
// Magnific and writes <game>\BatmanCityData\hd\<key>.png, which is then bound in place of the game's picture.
// The upscaled art is the game's (WB): it stays on this PC, never in the public repository.

struct ID3D11DeviceContext;
struct IDXGISwapChain;

namespace bvr::ac::screen_hd {

// Render thread, before / after every game draw. active = a full-screen movie (loading screen) is up.
void on_draw(ID3D11DeviceContext* ctx, bool active);
void after_draw(ID3D11DeviceContext* ctx);
// HD LOGO (JJ 2026-10-10: "the very first batman logo with the game name"): the game's video player cannot play a larger
// bac_logo.usm (2560x1440 froze it, picture cut and stretched), so while that video plays the still title card
// (frames 11-164) and the tech-logos card (165-312) are painted over the frame from
// <game>\BatmanCityData\hd\logo_title.png / logo_tech.png (Magnific 2x). Present thread, before the eye capture.
void logo_frame(ID3D11DeviceContext* ctx, IDXGISwapChain* sc);
bool command(const char* args); // "bc hd on|off|dump 0|1|status"
void status_line(char* out, unsigned n);

} // namespace bvr::ac::screen_hd
