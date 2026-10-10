#pragma once
// Batman City REFLECTEYE (JJ 2026-10-08: puddle reflections differ per eye - "the left eye is correct", the right
// eye's are shifted, skewed and stretched, and slide when the head turns).
//
// The wet ground reads a 256x240 planar reflection that the engine's reflection capture renders from the
// view family's first view, mirrored. The capture asks "update required?" (FSceneCaptureProbeReflect vtbl[2]
// = 0x80A2B0, thiscall(probe, const FSceneViewFamily*)) with the family's game time: both eye passes share one
// game time, so it rendered for the left eye only and the right eye projected the left eye's reflection.
// PERCAPTURE: in the right pass, a capture that updated in this frame's left pass is told to update again, so
// it renders mirrored from the right eye's own view.
// (First attempt, kept as an option: shifting the left eye's picture sideways - no visible effect, because the
// ground maps the reflection through the capture's own camera, not by screen position.)

#include <cstdint>
#include <string>

struct ID3D11DeviceContext;

namespace bvr::ac::reflect {

void install();                                         // hooks the capture's update check
void set_render_pass(uint32_t beginIndex, int eyeSign); // render thread, BeginDrawing execute
void on_draw_indexed(::ID3D11DeviceContext* ctx);       // render thread, before every DrawIndexed (shift option)

// Capture (F2): the next right-pass shift saves the reflection before and after it, as BMPs into dir.
void request_pictures(const wchar_t* dir);

void draw_ui();
void status_line(char* out, unsigned n);
void settings_write(std::string& out);
bool settings_read(const char* key, const char* val);

} // namespace bvr::ac::reflect
