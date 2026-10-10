#pragma once
// Batman City: zoom (right stick / Z) and detective mode (left shoulder / X) in VR.
// The game's own effects are screen-space (a 2D zoom stencil, a per-eye detective vignette) and
// do not fuse in stereo; JJ asked (2026-10-03) for one vignette around the whole field of view
// instead - AKVR's ZOOM VIGNETTE pattern.

#include <string>

struct ID3D11DeviceContext;

namespace bvr::ac::vision {

void install();                  // render-thread hooks (draw-indexed, upload tap)
void on_game_fov(float hfovDeg); // game thread: the game's OWN horizontal FOV this frame
bool draw_skip(::ID3D11DeviceContext* ctx, unsigned vertexCount); // render thread, Draw(): detective circle
void set_movie_playing(bool on); // a full-screen movie (loading screen, logos) is up
bool movie_playing();
void tick();                     // game thread, once per frame: fade + publish the vignette
bool handle_command(const char* args); // "bc vis ..."
void draw_ui();
void status_line(char* out, unsigned n);
void settings_write(std::string& out);                 // saved settings, "key=value" lines
bool settings_read(const char* key, const char* val);  // false = not a vision key

} // namespace bvr::ac::vision
