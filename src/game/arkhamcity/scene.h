#pragma once
// Batman City engine seam (replaces the stage-1 probe).
//
//  - Per-frame draw 0x65FD80 (caller 0x6C3710): census, heartbeat and, with stereo
//    on, the doubling (pass 1 = left eye, pass 2 = right eye, SEH-guarded).
//  - FSceneView ctor 0x9865B0 when called from CalcSceneView (0x7CAD40): the VR view.
//    Its INPUT ViewMatrix (a14) is multiplied by the head pose (+ eye offset) and its
//    INPUT ProjectionMatrix (a15) gets the headset FOV, so the engine derives every
//    inverse, ViewOrigin, PreViewTranslation and culling plane itself. The game's
//    camera (gameplay, aim, audio, transitions) is never written.
//  - CalcSceneView 0x7CA020: census + the game camera location (read-only).

namespace bvr::ac::scene {

bool install(); // fail-soft; waits for SteamStub plaintext; logs every step
bool hook_live();

void set_camera(bool on); // head drive + headset FOV + projection layer
void set_stereo(bool on); // doubled draw (needs camera on)
bool handle_command(const char* args); // "bc <args>"
void draw_ui();
void log_status();

} // namespace bvr::ac::scene
