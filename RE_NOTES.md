# Batman City — reverse-engineering notes (Arkham City GOTY, BatmanAC.exe 32-bit)

Exe: sha256 `b63ab98f5463d5a636597f1e20e615fb33631d06e344cd507ecd9aab51f0a332` (Steam, 2023-02-26),
image base 0x400000 (addresses below are VAs, no ASLR relocation expected; verify at run time).
All findings are **STATIC** (read from `re/BatmanAC.exe.unpacked.exe` with `re/ac.py`) unless marked.
Rocksteady build path in assert strings: `e:\projects\amadeus\ac\bm2gotypc\unrealengine3\development\src\...`
(assert/`check()` text survives, which makes functions easy to name).

## Engine object model
| Thing | Value | How found |
|---|---|---|
| UClass constructor | `0x458C50` (2,010 call sites = 2,010 native classes) | class getters `push size, flags, cast, name+2, pkg, cfg, InternalConstructor` |
| Class table | `re/classes.json` (name → size, InternalConstructor, site) | scan of all `call 0x458C50` |
| Native table | 2,630 `{name, impl}` pairs → `re/natives.txt` | DWORD pairs, name = `<Class>exec<Func>` |
| UGameViewportClient | size 0x118, ctor `0x7C9730` (InternalConstructor `0x7CBBF0`) | class table |
| URGFxGameViewportClient (the live one) | size 0x12C, InternalConstructor `0xEA91B0` | class table |
| ULocalPlayer | size 0x3EC, InternalConstructor `0x7CC930` | class table |
| UGameEngine / UEngine | 0x760 / 0x610 | class table |
| APlayerController / ACamera | 0x590 / 0x4D0 | class table |
| Rocksteady cameras | R3rdPersonCamera, RGlideKickCamera, RBatarangCamera, RDialogueCamera, RCinematicCameraActor, RMultiTargetCamera, … | class table |

## Viewport client sub-objects (UGameViewportClient layout)
- `+0x00` UObject vtable: `0x12F9050` (UGameViewportClient), `0x14C30B8` (URGFx…)
- `+0x2C` **FViewportClient** vtable `0x12F8FE0` (base FViewportClient vtable `0x1254E80`); URGFx keeps it
- `+0x30` FExec vtable `0x12F8FD8`

FViewportClient slots (derived vs base; OVR = overridden):

| Slot | Derived | Notes |
|---|---|---|
| 0 | 0x7C6FE0 (OVR) | 40 insns |
| 1 | 0x7E6BB0 (OVR) | stub |
| **2** | **0x7CCA30 = UGameViewportClient::Draw(FViewport*, FCanvas*)** | 1,616 insns, 107 calls; same slot as ME (slot 2) and BS Infinite (+0x8) |
| 5 / 7 / 9 / 12 / 19 | 0x7BDD50 / 0x7BDF20 / 0x7C3300 / 0x7BE0B0 / 0x7A8A00 | input/etc., unidentified |

## The viewport-draw root (what we double)
- **`0x65F1E0` = FViewport::Draw core**, thiscall, `ret 4` (one stack arg = bShouldPresent), same
  shape as BS Infinite's doubled root:
  - installs BeginDrawing (`0x12C0130`) and EndDrawing (`0x12C013C`) render-command vtables
    (DescribeCommand strings at 0x6456D0 / 0x645700),
  - `0x65F511 call 0x911720` = **FCanvas ctor** (canvas on the stack, `this = viewport` in esi),
  - `0x65F51D..0x65F533` = `ViewportClient->vtbl[+8]` → **Draw(viewport, &canvas)**,
  - `0x65F53D call 0x9122B0` = canvas flush,
  - `0x65F55E call 0x6556B0` → `0x655440` (frame begin/end helper with the command vtable 0x12C0124).
  - Huge stack frame (~0x80960 bytes).
- Only static caller: **`0x65FFB7` inside `0x65FD80`** (wrapper; also calls CalcSceneView at
  0x65FEA6 and has its own canvas path; `ret 4`).
- Callers of the wrapper `0x65FD80`:

| Call site (return addr = +5) | Containing fn | Context (strings nearby) | Double it? |
|---|---|---|---|
| **0x6C370B** | 0x6C2EB0 region | per-frame viewport array loop (Array.h checks) | **YES: likely the gameplay caller**. Confirm by live caller census |
| 0x6CC36C | 0x6C79F0 | `Game=` (map browse) | no |
| 0x6CDA03 | 0x6C79F0 | `DefaultPlayer`, `FullScreenMovie`, `LoadMapTimeLimit` (LoadMap) | no |
| 0x51D826 | 0x51D7A0 | FViewportClient default redraw | no |

## View building
- **`0x7CA020` = ULocalPlayer::CalcSceneView**, thiscall, `ret 0x14` (5 args: ViewFamily,
  ViewLocation&, ViewRotation&, Viewport, ViewDrawer). Uses 0.008727 (= pi/360, FOV half-angle),
  a "Cast of %s to %s failed" check. Callers: Draw (0x7CCFAB), the wrapper (0x65FEA6), 0x7CB0C4,
  0xCAB747.
- **`0x9865B0` = FSceneView constructor** (`ret 0x6C`, 27 stack dwords). Called by CalcSceneView
  (0x7CAD3B), Draw (0x7CCCB3), scene captures (0x809940 x4), 0x9AB950, 0xA68350. Contains the
  0.999 / -0.999 projection constants. FSceneView layout (ViewOrigin, PreViewTranslation, cached
  inverses, State) is to be read at run time with the ME inverse-scan method.
- `0x9B2370` (called twice from Draw) references `NvStereoFixTexture`, `SceneColorTexture`,
  `SceneDepthTexture`, `MinZ_MaxZRatio` (3D Vision / post setup).

## Design consequence (INFERENCE)
Apply head pose **and** eye offset to the FSceneView (render-only: hook CalcSceneView's result or
the FSceneView ctor), not to the gameplay camera. Game logic, aim, audio and camera transitions
then never see our edits, which avoids AKVR's camera read-back (CAMRESTORE) and BS Infinite's
GetPlayerViewPoint deny list. ME VR does it this way. Check: UCanvas::Project (HUD markers) uses
the canvas's scene view, so markers should follow the edited view.

## LIVE results (2026-10-03, flat window 1280x720, main menu, fake headset `bvr-xrsim`)
Grade **LIVE** (runtime observation, not headset). Probe build `batman_city 0.0.1`.
- SteamStub: `.text` became plaintext 100 ms after our init started. **Hooks must wait for the
  plaintext prologue** (probe does); hooking at once would patch ciphertext.
- **Per-frame draw = `0x65FD80` (the "wrapper"), not `0x65F1E0`.** `0x65F1E0` was called 0 times.
  `0x65FD80` callers: `0x6C3710` x1846 (per-frame) and `0x6CDA08` x1 (LoadMap). It has its own
  canvas ctor (0x660114) -> client Draw (0x660132) -> flush (0x66013B). **This is the function to
  double**, gated on return address `0x6C3710`.
- 62 draws/s = 62 CalcSceneView/s = 62 presents/s (game cap `MaxSmoothedFrameRate=62`). One view
  per frame; every CalcSceneView ran inside the per-frame draw; same thread.
- CalcSceneView callers: `0x7CCFB0` (Draw) every frame, `0x65FEAB` once (LoadMap path).
- FSceneView ctor callers: `0x7CAD40` (CalcSceneView) every frame, `0x7CCCB8` once.
- **ctor args confirmed:** `a14` ViewMatrix -> view `+0x060` MATCH, `a15` ProjectionMatrix ->
  `+0x0A0` MATCH, `a2` ViewState stored at view `+0x004`, a10..a13 = X,Y,W,H (0,0,1280,720).

### FSceneView layout (32-bit, from `captures/20261003_011931/sceneview.bin`)
| Offset | Field |
|---|---|
| +0x004 | ViewState (per-eye temporal history / occlusion) |
| +0x060 | ViewMatrix (input a14) |
| +0x0A0 | ProjectionMatrix (input a15); m22=0.999 m32=-9.99 -> near plane 10 UU |
| +0x180 | TranslatedViewMatrix |
| +0x1C0 / +0x200 | TranslatedViewProjection / its inverse (pair verified) |
| +0x240 | PreViewTranslation = -ViewOrigin |
| +0x250 / +0x2D0 | ViewProjection / its inverse (pair verified) |
| +0x290 | inverse Projection (pair with +0x0A0) |
| +0x310, +0x350, +0x390 | further copies of Proj / TVP / VP |
| +0x3E0 | ViewOrigin (w=1) |
| +0x3F0.. | view frustum planes (normal, d), plus SIMD-permuted copies at +0x440 and +0x560 |

### Stereo seam decision (INFERENCE from the above)
Change the **inputs** of the FSceneView ctor (a14 ViewMatrix, a15 Projection, a2 ViewState) only
when the caller is CalcSceneView (RA `0x7CAD40`). The engine then derives ViewOrigin,
PreViewTranslation, every inverse and the culling planes itself, per eye. No inverse-matrix
repair (ME's INVMAT) and no gameplay-camera write (no CAMRESTORE / deny list).

## Next RE steps
1. ~~Caller census~~ and ~~FSceneView layout~~: done (above). Repeat the census in gameplay, cutscenes, loading.
2. Where does the present happen relative to `0x65FD80` (EndDrawing command)? Doubling must yield two presents (BS Infinite rule).
3. AllocateViewState (store to `ULocalPlayer::ViewState`) for per-eye view states.
4. GEngine global, GamePlayers, the URGFx Scaleform draw position relative to the root.

## Window / render size (LIVE 2026-10-03)
The game caps its windowed size at the monitor in several places; the one that matters:
- **`0xBAF0A0` = FWindowsViewport resize** (thiscall SizeX, SizeY, bFullscreen, PosX, PosY; ret 0x14;
  also creates the window). Hooked: windowed requests become the VR size (launch.ini). Window,
  swap chain and engine viewport all follow. This alone fixed it (2064x2208 and 2888x3088 LIVE).
- Inside it: `0xBAF615 cmovg edi,ebp` / `0xBAF63F cmovg esi,ecx` cap to a cached monitor or
  work-area RECT (`0x16B8F90..9C` / `0x16B8FA0..AC`), then an aspect-keeping shrink (the
  1274x1363 result). NOPed in windowed launches (belt and braces).
- `0xBAC8C0` = "match viewport to window client rect" (GetClientRect -> resize); it re-requested
  2064x1440 about 25 s in (title screen) - the hook converts that too.
- `0xBAC170` = set style + SetWindowPos helper (called from 0xBAF8E7/0xBAF909).
- Desktop size cache `0x16B8FE0/4` written in 0x4B1FE0. Desktop-query spoof (GetSystemMetrics /
  SystemParametersInfoW / GetMonitorInfoW, IAT of the exe, windowed only) stays in but was NOT
  the fix.
- ResizeBuffers caller in the RHI: FD3D11Viewport::Resize `0xB92250` (ret 0xC).
