#pragma once
// Batman City BILLBOARD (JJ 2026-10-08: main-menu water droplets "are rotating to face the way your head is
// facing ... they need to be fixed so that they don't swivel on their axes").
//
// Particle sprites face the view they are drawn for: their vertex constants carry the view's right and up
// axes (title capture 2026-10-08, head straight vs turned + rolled: floats 12-14 and 16-18 of the 320-byte VS
// buffer of the sprite draws, and of the 640-byte buffer of their lighting draws). Per eye those are the eye's
// axes, head included. Here every such upload is caught (UpdateSubresource tap) and both axes are mapped from
// the eye's frame to the game camera's own frame, so sprites face the game camera and keep their place and
// roll when the head moves - the MARKERCARD rule for particles.

#include <cstdint>

struct ID3D11Resource;

namespace bvr::ac::billboard {

// Game thread, per pass (FSceneView ctor): the eye view's and the game camera's world->view rotations (3x3 of
// the row-vector view matrices, rows of 4 floats as in M4).
void set_pass(uint32_t slot, const float eyeView[16], const float baseView[16]);
void set_render_slot(uint32_t slot); // render thread
// Render thread, before every UpdateSubresource the game makes: may rewrite the sprite axes in data.
void on_upload(::ID3D11Resource* dst, const void* data);

bool enabled();
void set_enabled(bool on);
uint32_t patched();

} // namespace bvr::ac::billboard
