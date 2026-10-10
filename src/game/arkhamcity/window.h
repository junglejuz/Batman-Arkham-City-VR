#pragma once
// Batman City: let the game's window be larger than the monitor, so the VR render resolution
// is not capped by the desktop (Windows clamped a 2064x2208 request to 1274x1363 on JJ's 1440p
// screen). Plus the VR resolution presets: chosen in the F10 menu, saved to
// %LOCALAPPDATA%\Batman_City\launch.ini, applied by the launcher on the next start.

namespace bvr::ac::window {

void install(); // hooks CreateWindowExW (call before the game creates its window)
void apply_size_clamp_patch(); // after SteamStub plaintext; windowed (VR) launches only

struct Preset {
    const char* name;
    unsigned w, h;
};
const Preset* presets(int* count);
void current_launch_res(unsigned* w, unsigned* h); // what launch.ini holds (0 = none)
bool save_launch_res(unsigned w, unsigned h);
bool keep_focus();             // game never hears it lost focus (no pause on click-away)
void set_keep_focus(bool on);
bool logo_video_opened();       // HD LOGO: the movie being played is the opening-logo video
bool show_logos();             // play the opening logos (settings.ini opening_logos, read at launch)
void set_show_logos(bool on);  // next launch
void skip_logos();             // a pad button during the logos: end them now
bool logo_skip_requested();
void set_movie_band(bool on); // loading-screen layout test: full-screen movies in a centred 16:9 band
void tick();                   // game thread, every frame: the logo skip button check
void draw_ui();

} // namespace bvr::ac::window
