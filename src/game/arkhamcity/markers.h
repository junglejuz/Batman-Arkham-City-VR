#pragma once
// Batman City MARKEREYE (JJ 2026-10-07: the grapple reticle "should be attached in world space to the
// object it refers to ... and be at the same depth as that object"; AKVR TARGETMOVE/TARGETDEPTH lesson).
//
// The HUD script places world markers with Canvas.Project (UCanvas::execProject -> FSceneView
// project 0x972FB0, return address 0x8FDFD2), once in each eye pass, but the HUD movie shows ONE
// layout in both eyes (LIVE 2026-10-04: the reticle on the left eye's gargoyle, same pixel in the
// right eye). Here:
//  1. every Canvas.Project result in a stereo pass is replaced by the projection through the GAME
//     CAMERA's own view of the same frame - no head, no eye (MARKERBASE, 2026-10-07: the first version
//     used the left eye's view, and the movie's layout lagging the eye images by a frame turned every
//     head movement into reticle jitter, JJ). Whichever pass or frame the movie takes its layout from,
//     the reference point then only moves with the game camera, never with the head;
//  2. each HUD draw whose placement lies near a reference point this frame is a marker piece: it is
//     kept out of the HUD panel and moved, per eye, by (this eye's projection of the world point -
//     the reference point), so it sits on its object at the object's depth.

#include <cstdint>
#include <string>

struct ID3D11DeviceContext;

namespace bvr::ac::markers {

// Game thread, after each pass's FSceneView is built: slot = BeginDrawing index, eye -1/0/+1,
// frame = the game frame counter, vp = the view's ViewProjectionMatrix (view+0x250, row vectors),
// baseVp = the game camera's own view-projection for this frame (no head, no eye offset).
void set_pass(uint32_t slot, int eye, uint32_t frame, const float vp[16], const float baseVp[16]);
// Game thread, Canvas.Project's world point and result (x/w, y/w, z/w, w); may rewrite out.
void on_project(int eye, uint32_t frame, const float world[3], float out[4]);
// Render thread (BeginDrawing execute).
void set_render_slot(uint32_t slot);
// HEADAIM2 (2026-10-27, Origins AIMDEPTH): while head aim steers Batman's controller, the depth march and the aim reticle follow
// the HEAD's centre ray instead of the head-free game camera's. Game thread, a pair's first eye; frame = set_pass's frame;
// origin null = no head ray this frame.
void set_aim_ray(uint32_t frame, const float origin[3], const float dir[3]);
// Game thread: the point under the head's ray, if read within the last 150 ms.
bool aim_point(float out[3]);
// RETICLESPHERE: the HUD panel's distance in game units (game thread, every frame) - the aim reticle's sphere radius.
void set_reticle_sphere(float radiusUU);

// Render thread, once per HUD draw (after the scene): true = a world-marker piece; its VS cb0 has
// been moved for this eye and the draw belongs in the eye image. after_hud_draw puts cb0 back.
bool on_hud_draw(::ID3D11DeviceContext* ctx);
void after_hud_draw(::ID3D11DeviceContext* ctx);

void draw_ui();
void status_text(std::string& out); // for captures (targets.txt)
void settings_write(std::string& out);
bool settings_read(const char* key, const char* val);

} // namespace bvr::ac::markers
