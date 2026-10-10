#pragma once
// Arkham City GOTY (Steam) addresses. Every value is a fixed VA: BatmanAC.exe has
// no DYNAMIC_BASE (DllCharacteristics 0x8100), image base 0x400000. They are
// trusted ONLY when the build fingerprint below matches; otherwise every hook
// stays off and the game runs flat. Derivations: batman_city/RE_NOTES.md.

#include <cstdint>

namespace bvr::ac::addr {

// Build fingerprint of the shipped (SteamStub-packed) exe as mapped in memory.
constexpr uint32_t kTimeDateStamp = 0x520000F7;
constexpr uint32_t kSizeOfImage = 0x0194F000;
constexpr uintptr_t kImageBase = 0x00400000;

// THE PER-FRAME VIEWPORT DRAW (LIVE 2026-10-03): thiscall(viewport, arg), ret 4. Its own
// canvas ctor -> client Draw -> flush. Doubled for stereo from kWrapperCallerTick only.
constexpr uintptr_t kViewportDraw = 0x0065FD80;
// FSceneView ctor return address inside CalcSceneView (LIVE: every frame).
constexpr uintptr_t kCtorRetFromCalcSceneView = 0x007CAD40;

// FViewport::Draw core variant: thiscall(viewport, bShouldPresent), ret 4. NOT called in play (LIVE). Canvas ctor ->
// ViewportClient vtbl[+8] Draw -> canvas flush, with Begin/EndDrawing commands.
constexpr uintptr_t kViewportDrawRoot = 0x0065F1E0;
// Its only static caller (wrapper), thiscall(viewport, arg), ret 4.
constexpr uintptr_t kViewportDrawWrapper = 0x0065FD80;
// Wrapper call sites (return address = site + 5).
constexpr uintptr_t kWrapperCallerTick = 0x006C3710;    // per-frame viewport loop (gameplay?)
constexpr uintptr_t kWrapperCallerBrowse = 0x006CC371;  // map browse ("Game=")
constexpr uintptr_t kWrapperCallerLoadMap = 0x006CDA08; // LoadMap
constexpr uintptr_t kWrapperCallerRedraw = 0x0051D82B;  // FViewportClient default redraw

// UGameViewportClient::Draw(FViewport*, FCanvas*) = FViewportClient vtable slot 2.
constexpr uintptr_t kGameViewportClientDraw = 0x007CCA30;
constexpr uintptr_t kFViewportClientVtbl = 0x012F8FE0;
// BeginDrawing render command Execute (render thread), shared by both BeginDrawing command
// vtables (0x12C0130 slot 1, 0x12C0154 slot 1): calls Viewport->vtbl[0x64] (begin frame).
constexpr uintptr_t kBeginDrawingExecute = 0x00645810;

// ULocalPlayer::CalcSceneView(family, FVector& loc, FRotator& rot, viewport, drawer),
// thiscall, ret 0x14, returns FSceneView* (expected, verified live by the probe).
constexpr uintptr_t kCalcSceneView = 0x007CA020;
// FSceneView constructor, ret 0x6C.
constexpr uintptr_t kSceneViewCtor = 0x009865B0;
// AllocateViewState(): cdecl, no args, returns a new FSceneViewState (0x390 bytes). The game
// stores the result at ULocalPlayer+0x7C (0x7CBD32); scene captures (0x809940) use it too.
constexpr uintptr_t kAllocateViewState = 0x0098B3B0;

// GFullScreenMovie (FFullScreenMovieSupport*): the full-screen movie player (opening logos,
// loading screens). Seen in the load-map-movie routine 0x69AD40: vtbl[+0x20](L"") = UE3's
// GameThreadIsMoviePlaying(name; "" = any), vtbl[+0x10](mode 0x81, name, 0, -1, -1) =
// GameThreadPlayMovie. thiscall.
constexpr uintptr_t kGFullScreenMovie = 0x016CB074;
constexpr unsigned kMovieIsPlayingSlot = 0x20 / 4;

// UGameViewportClient's FViewportClient input virtuals (vtbl 0x12F8FE0 slots 5 and 7, both
// ret 0x1C), thiscall: InputKey(FViewport*, INT ControllerId, FName Key, EInputEvent Event,
// FLOAT Amount, UBOOL bGamepad) and InputAxis(FViewport*, INT ControllerId, FName Key,
// FLOAT Delta, FLOAT DeltaTime, UBOOL bGamepad). EVERY key/button/stick reaches the game here,
// whatever read it (XInput, Steam Input, DirectInput, keyboard).
constexpr uintptr_t kInputKey = 0x007BDD50;
constexpr uintptr_t kInputAxis = 0x007BDF20;
// The engine's key FNames for the stick clicks (built at 0x6A96CA / 0x6A96ED; index at +0).
constexpr uintptr_t kNameLeftThumbstick = 0x01662B50;
constexpr uintptr_t kNameRightThumbstick = 0x01662B58;

// Scaleform: builds the ActionScript Rectangle for Stage.visibleRect (and two sibling Stage
// properties) from a GRectF {left, top, right, bottom}. cdecl (out, env, const GRectF*). The
// movie's own visible-area query is the vtbl+0x7C call just before it (0xF8C8F9).
constexpr uintptr_t kStageRectToValue = 0x00F8C7D0;

// ARPlayerController vtbl+0x574 (both controller classes: 0xD87D70), behind the script native
// VectorToScreen 0xC03D20: thiscall (FVector* out, FVector world, void* p), ret 0x14 - the game's
// world -> screen projection for HUD markers (to be confirmed with a grapple point in view).
constexpr uintptr_t kVectorToScreen = 0x00D87D70;

// FSceneView world -> screen: thiscall(view; FVector4* out, const FVector* in), ret 8; out = (x/w, y/w, z/w, w)
// through the view's ViewProjectionMatrix at +0x250 (row vectors). Callers incl. the Batman-side
// marker gatherer at 0xC57D0E (canvas SceneView -> on-screen / off-screen lists) and VectorToScreen.
constexpr uintptr_t kSceneViewProject = 0x00972FB0;
// FSceneView::WorldToScreen (no divide): same signature, out = clip (x, y, z, w).
constexpr uintptr_t kSceneViewWorldToScreen = 0x00971CE0;
// Return address of UCanvas::execProject's call into kSceneViewProject (the HUD script's Canvas.Project).
constexpr uintptr_t kCanvasProjectRet = 0x008FDFD2;

} // namespace bvr::ac::addr
