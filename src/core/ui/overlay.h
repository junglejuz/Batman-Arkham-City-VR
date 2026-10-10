#pragma once

struct IDXGISwapChain;

namespace bvr::overlay {

// ImGui debug/config overlay drawn from the Present hook. Toggled with F10.
// Initializes itself lazily on the first Present (that's when the game's
// device and window are known).
void on_present(IDXGISwapChain* swapchain);
void on_resize(); // drop backbuffer references before ResizeBuffers

// Session 22: programmatic visibility (any thread; applied on the next
// present). The `vroverlay on|off` seam command - the harness cannot press
// F10, and it doubles as the user's recovery if a keyboard state wedges.
void set_visible(bool on);

// Two quick clicks of both sticks (the 0.6 s hold stays the menu): calls fn (overlay thread). Batman City: first person.
using ChordTapFn = void (*)();
void set_chord_double_tap(ChordTapFn fn);
// STICKNAV (JJ 2026-10-10, panel scroll looping back down): stick up/down moves the highlight like the D-pad (default
// on); off = ImGui's own free scroll with the stick.
void set_stick_nav(bool on);
// PANELTIDY (JJ 2026-10-10, before release 0.1.1): settled tests, test buttons and diagnostics are hidden unless the
// panel's last checkbox "show test options" is ticked (not saved: every start hides them). Captures then also arm the
// heavy frame / view dumps.
bool show_tests();
void set_show_tests(bool on);

// ONESTEP (AKVR 2026-09-27, ported 2026-10-09 - JJ: "the slider should move 0.01 for every click on the D-pad;
// currently it moves several units at a time"): ImGui's own gamepad tweak moves a slider 1% of its RANGE per press.
// These sliders move exactly one displayed step per D-pad / left-stick press (%.2f -> 0.01, %.0f / int -> 1), with a
// repeat while held (300 ms, then every 70 ms; x5 after 1 s, x25 after 2.5 s), and ImGui's left/right is muted while
// one of them is controller-active so there is one writer only. Mouse drags and typed values work as usual.
bool SliderStep(const char* label, float* v, float lo, float hi, const char* fmt = "%.2f", int flags = 0);
bool SliderStepInt(const char* label, int* v, int lo, int hi, const char* fmt = "%d", int flags = 0);

} // namespace bvr::overlay
