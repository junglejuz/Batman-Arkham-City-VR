#pragma once
// Batman City ROOMGRUNGE (JJ 2026-10-07: "the main menu dirt layer ... put back where it was, but
// attached in world space and at a distance"). The game paints full-screen overlays (the title
// screen's grunge) into each eye's scene colour at the same pixels, so in the headset they sit on
// the face. Here the game's own draw is replayed onto black and onto white render targets, and the
// result is laid onto a big screen hanging in the room, drawn into each eye with that eye's view.

#include <cstdint>
#include <string>

struct ID3D11DeviceContext;

namespace bvr::ac::room_overlay {

enum Mode : int { kOriginal = 0, kRoom = 1, kHidden = 2 };

// Game thread, per stereo/mono pass (FSceneView ctor): the matrix that takes a point in the game
// camera's own view space (UU; x right, y up, z forward, before the head) to this pass's clip
// space - eye_delta * projection, row vectors. slot = the pass's BeginDrawing index.
void set_pass(uint32_t slot, const float baseToClip[16], float uuPerM);
// Render thread (BeginDrawing execute): the pass whose draws follow.
void set_render_slot(uint32_t slot);

// Render thread, from the Draw veto with the grunge texture bound: true = the game's own draw is
// dropped (room screen drawn instead, or hidden).
bool on_grunge_draw(::ID3D11DeviceContext* ctx);

// Render thread, any full-screen Draw(3) into the scene: true = it was the pause menu's dirt pass and has
// been drawn with the dirt hung on a screen in the room (PAUSEDIRT); the game's own draw is dropped.
bool on_pause_dirt(::ID3D11DeviceContext* ctx);

// Probe (pause-menu dirt hunt): every full-screen Draw(3) into the HDR scene colour that is not the
// grunge. Logs each distinct texture set once; after request_replay_pictures, replays the next 8
// such draws onto black and white and saves both pictures (quarter size) into dir.
void probe_draw(::ID3D11DeviceContext* ctx);
void request_replay_pictures(const wchar_t* dir);

int mode();
bool title_screen_now(); // the main menu's grunge was drawn within the last half second
bool pause_screen_now(); // the pause / map screen's grey pass ran within the last half second (no clock wait)
void set_mode(int m);
void draw_ui();
void status_line(char* out, unsigned n);
void settings_write(std::string& out);
bool settings_read(const char* key, const char* val);

} // namespace bvr::ac::room_overlay
