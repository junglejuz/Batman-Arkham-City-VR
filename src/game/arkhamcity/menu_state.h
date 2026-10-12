#pragma once
// Menu signal (2026-10-11/12, JJ: the map copy and the menu leftovers; playbook b955f224 04 "Find the engine's
// authoritative UI state instead of inferring it"). The game's own menu functions (Scaleform movie Start / SetFocus /
// Close, the full-screen movie flag, the menus' RestoreMenuState) are found by name in the live native table and hooked:
// a menu is open from its movie starting with focus until its Close. Drives the HUD panel size and the menu-closing HUD
// (scene.cpp MS2 / MS9 / MS11).

namespace bvr::ac::menu_state {

void install();                              // after the code is plaintext (scene install)
void status_line(char* out, unsigned n);
bool live();                                 // Start / Close / SetFocus hooked
bool menu_open();
unsigned long long menu_open_at();           // GetTickCount64 of the last opening / closing
unsigned long long menu_closed_at();
void clear_stale(const char* why);           // the adapter's safety: the game has run on for a while

} // namespace bvr::ac::menu_state
