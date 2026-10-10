#pragma once
// Batman City AFW - alternate frame warping, a choice beside native stereo (JJ 2026-10-10; BATMAN_CITY_PLAN.md §11).
// Each game frame draws ONE eye (left, right, left ...); the other eye is built from that same frame's picture and
// depth, so both eyes always show the same moment (not plain alternate-eye rendering). Port of Arkham Knight's AFW
// (akvr_native/src/afw.cpp, JJ: "it works really well"); idea from PureDark's UEVR AFW. Three warp modes:
//   1 this frame:     the drawn eye moved into the other eye's view through its depth; holes stretched from the background
//   2 previous frame: the other eye's own last real picture, brought to now through this frame's depth
//   3 combined:       1, its holes (behind objects, the other eye's outer edge) filled from 2   (default)

#include <cstdint>
#include <string>

namespace bvr::ac::afw {

void install();          // registers the core's eye-synth hook
bool on();               // AFW chosen (live switch; the game thread reads it per frame)
void set_on(bool on);
int mode();
// Game thread, FSceneView ctor of an AFW frame: the drawn eye's camera with its jitter (vpJ) and without (vpU), the
// other eye's camera from the same head sample (dstVp, no jitter) and its lens depth terms; render size.
void set_pass(uint32_t slot, int drawnEye, const float vpJ[16], const float vpU[16], const float dstVp[16], float dP22,
              float dP32, int renderW, int renderH);
void set_render_slot(uint32_t slot); // render thread, BeginDrawing
void note_drawn(int eye);            // game thread: an AFW frame drew this eye (0 left, 1 right)
bool command(const char* args);      // "bc afw ..."
void draw_ui();
void settings_write(std::string& out);
bool settings_read(const char* key, const char* val);
void status_line(char* out, unsigned n); // the heartbeat's AFW line (rates since the last call)

} // namespace bvr::ac::afw
