#pragma once
// s52 (Infinite I9): the Scaleform-GFx HUD capture - a SECOND hud machine,
// deliberately not a modification of hud_capture.cpp (that is BS1's gameswf
// worked example; its fingerprints are Vengeance-specific).
//
// Classifier: the POSITIONAL rule (DR-I7, ENGINE_NOTES). Within one present
// window, the moment a full-screen depth-free DrawIndexed a=6 sampling a
// rendered render-target lands on the BACKBUFFER (the tonemap/eye blit),
// every LATER backbuffer draw is UI-run content. No fingerprint needed - the
// eye image is HUD-free by construction once those draws are redirected.
// Frames with no such blit (Bink movies, loading screens) classify NOTHING,
// which is what keeps this lane cinematics-safe: movie frames stay untouched.
//
// Default OFF in core. The Infinite adapter arms it; BS1/BS2 never call any
// setter here, and the armed-off cost on their draw stream is one relaxed
// load per draw.

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;
struct ID3D11Resource;
struct ID3D11Texture2D;
struct ID3D11BlendState;
struct IDXGISwapChain;

namespace bvr::gfx_hud {

// What the frame_inspector detour should do with the current draw. At most
// one of redirectRtv / restoreRtv is set; both null = pass through.
struct Decision {
    ID3D11RenderTargetView* redirectRtv = nullptr; // bind this (+ dsv) via ORIGINAL SetRT
    ID3D11DepthStencilView* redirectDsv = nullptr;
    ID3D11RenderTargetView* restoreRtv = nullptr;  // hand the game's binding back
    ID3D11DepthStencilView* restoreDsv = nullptr;
    // Batman City: for a redirected draw, the blend state to use INSTEAD of the game's for this
    // one draw (alpha channel corrected for the transparent capture); the caller restores the
    // game's own state right after the draw. Null = leave the game's state alone.
    ::ID3D11BlendState* blend = nullptr;
    // Batman City MARKEREYE: the adapter moved this draw (a world marker kept in the eye image);
    // the caller calls after_draw() once the draw is issued.
    bool after = false;
    // Batman City FADESKIP: drop this draw (the caller returns without drawing, after any restore binding).
    bool skip = false;
};

// Batman City MARKEREYE: asked once per post-boundary HUD draw. true = a world-marker piece: it stays in
// the eye image (never the panel) and the adapter has already adjusted it; after_draw undoes that.
using MarkerFn = bool (*)(::ID3D11DeviceContext* ctx);
using MarkerAfterFn = void (*)(::ID3D11DeviceContext* ctx);
void set_marker_fn(MarkerFn fn, MarkerAfterFn after);
// Batman City BATCUTS (JJ 2026-10-08: the bat-wipe transition cuts show as a black box on the panel - "make it
// much bigger so it fills the entire field of view"): HUD draws with no texture, or sampling a screen-size
// picture, are the wipe (never in normal HUD frames, hudline timelines 2026-10-08); they stay in the eye image.
// Batman City DLSS: the depth-stencil view the game bound last (weak; render thread).
::ID3D11DepthStencilView* game_dsv();
::ID3D11Resource* scene_depth(); // the last full-size readable R24G8 depth the game bound (weak)
// HUDZONES: the panel HUD in zones by screen position, each with its own size (about its corner / edge) and offset (NDC).
constexpr int kHudZones = 7;
const char* hud_zone_name(int i);
void get_hud_zone(int i, float* s, float* dx, float* dy);
void set_hud_zone(int i, float s, float dx, float dy);
uint32_t hud_zone_draws(int i);
void set_hud_zones_on(bool on); // all group adjustments on / off at once
bool hud_zones_on();
void set_hud_zones_gameplay(bool on); // the adapter: false while a menu / title / movie is up
bool hud_zones_gameplay();
void set_cuts_to_eye(bool on);
// HUDDSV: the game cleared a depth-stencil view (mirrored to the HUD's own one during the HUD).
void on_clear_dsv(::ID3D11DeviceContext* ctx, ::ID3D11DepthStencilView* dsv, unsigned flags, float depth,
                  unsigned char stencil);
void before_eye_capture(::ID3D11DeviceContext* ctx, ::IDXGISwapChain* sc); // WIPEREPLAY (present, before the eye capture)
// HD LOGO: the adapter draws into the game's frame right before the eye capture (present thread).
using BeforeCaptureHook = void (*)(::ID3D11DeviceContext* ctx, ::IDXGISwapChain* sc);
void set_before_capture_hook(BeforeCaptureHook h);
void set_hide_wipes(bool on);
void set_hud_dsv(bool on); // HUDDSV: the HUD draws with its own depth-stencil (default on) // HIDEWIPES: menu / pause bat-wipe transitions not drawn (default on)
bool hide_wipes();
void set_menu_open_hold(int ms); // MENUOPEN: hold after the recorded wipe when a menu opens from gameplay (700)
void set_open_fade(bool on); // OPENFADE: menu-opening fade pieces (placement ~5.3e-5, gameplay) not drawn (default on)
void set_load_hold(bool on);  // LOADHOLD: black after a bat wipe while the game loads (default on)
// WIPESOUND: render thread; kind 0 = a recorded wipe to black starts (the game's own wipe, with its sound), 1 = the
// reveal (flipped wipe after a wipe to black) starts - it has no game wipe and so no sound of its own; 2 = any bat wipe of
// the game starts (WIPESOUND4: the sound posted at every wipe start is the bat sound).
using WipeEventFn = void (*)(int kind);
void set_wipe_event_hook(WipeEventFn fn);
void set_wipe_reveal(bool on); // WIPEREVEAL: bats clear off the arriving screen after a wipe to black (default on)
void set_pre_reveal(int ms);  // PREREVEAL: the reveal starts this long after loading goes quiet (default 500)
void set_wipe_reveal_skip(int ms); // WIPEREVEAL: start that far into the recording (default 300)
void set_glow_zone(int mode); // GLOWZONE / PICZONE: 0 anchor only, 1 glow pastes, 2 picture middle (default), 3, 4
// The adapter's game-clock verdict (menus / map stop it). Stopped -> running also feeds UNPAUSECUT.
void set_clock_stopped(bool stopped);
uint32_t unpause_cuts();
unsigned long long menu_wipe_at(); // MS2: GetTickCount64 of the last wipe that started inside the menus
void set_map_screen(bool on); // MAPSCREEN: big untextured shapes = the map: wipe hiding paused (default on)
void set_stage_box(bool on); // STAGEBOX / FIRSTCUT / FADESKIP (default on)
void set_menu_cut(bool on); // MENUCUT test: outside gameplay every cut-type piece stays in the eye image
void set_halo(float strength); // PAUSEHALO: soft dark halo under the panel's content (0 = off), set every frame
void set_fade_hold(int mode); // FADEHOLD: 1 = no HUD panel while the after-loading fade-in box covers it, 0 = off
int fade_hold();
void note_movie_ended(); // FADEHOLD: start watching the panel (up to 3 s)
void note_movie_started(); // WIPEMAIN: a wipe recording is kept only if the loading movie follows it
uint32_t replays_started(); // WIPEMAIN2: how many recorded-wipe replays have started this session
void set_replay_hides_all(bool on);
void set_wipe_panel_off(bool on); // WIPEPANEL: no HUD panel layer while a wipe replay runs (default on)
void set_wipe_flip(bool on);
bool wipe_flip();
bool wipe_panel_off();
void set_replay_enabled(bool on); // WIPEMAIN: square wipes replaced by the recorded bat wipe (default on)
bool replay_enabled();
void set_black_fill(bool on); // BLACKFILL: the game's full-screen black stage fill -> whole view black (default on)
bool black_fill();
void set_wipe_debug(bool on); // WIPEDEBUG: test captures / eye sequences during bat wipes (default off - they stutter)
bool wipe_debug();
void set_menu_fade_black(bool on); // MENUFADE: a menu's fade to black on the panel -> whole view black (default on)
bool menu_fade_black(); // WIPEFLIP: wipes arriving at the first menu play the recording inverted (default on) // WIPEMAIN2: no HUD piece drawn while the recorded wipe plays (default on)
void set_main_menu_now(bool on); // WIPEMAIN: the main menu's own grunge is on screen (not the pause screen), every frame
void request_panel_sequence(const wchar_t* dir, int frames); // test: save the panel picture each HUD frame
bool cuts_to_eye();
uint32_t cuts_kept();
void after_draw(::ID3D11DeviceContext* ctx);

void set_armed(bool on);    // classifier + census
void request_dump();        // Batman City: log the next frame's HUD draws + blend states
void set_hud_ring(bool on);           // HUDRING test: keep the last per-frame HUD lines in memory
void dump_hud_ring(const char* why);  // ... and write them to the log (settings debug_hud_ring=1)
bool save_hud_ring(const wchar_t* path); // ... or to a file (captures)
void request_timeline(int frames); // Batman City: one compact log line per frame (draws, textures, biggest piece)
void save_panel_picture(const wchar_t* path); // next HUD frame -> 32-bit BMP
bool save_texture_bmp(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const wchar_t* path); // any 8-bit RGBA texture
void set_redirect(bool on); // classified draws feed the capture RT (the quad)
bool armed();
bool redirect_on();
// Full-screen-effect pass-through (default ON): post-boundary draws with a
// tiny index/vertex count (a GFx flash/vignette quad) stay on the eye image
// instead of the quad. In-headset A/B: bsihud fx on|off.
void set_fx_passthrough(bool on);
bool fx_passthrough();

// Detour taps (render thread). on_draw_* returns the decision; the detour
// owns the actual SetRT call so this module never re-enters the hooks.
void on_setrt(unsigned numViews, ID3D11RenderTargetView* const* rtvs,
              ID3D11DepthStencilView* dsv);
Decision on_draw_indexed(ID3D11DeviceContext* ctx, unsigned indexCount);
Decision on_draw(ID3D11DeviceContext* ctx, unsigned vertexCount);
void on_present(ID3D11DeviceContext* ctx, IDXGISwapChain* swapchain);

// The captured HUD texture (transparent-cleared, UI-run content only), or
// null when stale/empty. The XR HUD-quad consumer prefers this via the
// provider seam in openxr_runtime.
ID3D11Texture2D* texture(ID3D11DeviceContext* ctx);

// Census counters for the adapter's status verb (relaxed reads, any thread).
struct Census {
    uint32_t frames = 0;        // presents seen while armed
    uint32_t boundaries = 0;    // frames where the eye blit was found
    uint32_t hudDraws = 0;      // post-boundary backbuffer draws
    uint32_t redirected = 0;    // of those, redirected to the capture RT
    uint32_t bigPostDraws = 0;  // post-boundary FULL-SCREEN-ish draws (the
                                // effects census: a damage flash would land
                                // here if it is a GFx overlay)
    uint32_t lastFrameHud = 0;  // hud draws in the last completed frame
};
Census census();

} // namespace bvr::gfx_hud
