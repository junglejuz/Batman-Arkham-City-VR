# Batman City — Batman: Arkham City in VR (native same-frame stereo + 6DOF)

> **HUD / menus / panels:** read `../VR_HUD_GUIDE.md` first (HUD depth from per-eye quads,
> menus over the live world, dark backings). Packaging: `../PACKAGING_GUIDE.md`.

**Status 2026-10-03 (night): same-frame stereo + head tracking + asymmetric frusta working on the FAKE headset; first real-headset look is next (JJ).** See §10. Every claim below is graded
`STATIC` (read in Arkham City's binary/files), `SOURCE` (read in another mod's source or docs),
`LOG` (another mod's runtime log on JJ's PC) or `INFERENCE`. Nothing is `LIVE` or `HEADSET`.

**Name:** Batman City (folder `batman_city/`, mod file `batman_city.dll`). JJ 2026-10-03: not "ACVR" (too close to AC6VR); "CITYVR" was used for an hour, then JJ chose Batman City / Batman_City.

**JJ's brief (2026-10-03):** Mass Effect LE's VR mods prove a UE3 game can render two real
cameras per frame. Do the same for a Batman game, **without geo-11**, by hooking UE3's camera
and render path natively. Target: Arkham City. Use what AKVR (Arkham Knight) and SKVR (Sekiro)
taught us, plus the Mass Effect and BioShock mods and all workspace references.

## 0. Plain-language summary (for JJ)

- **Can it be done? Very likely yes.** Two working mods already do exactly this on the same
  engine family: Mass Effect LE VR (64-bit UE3) and BioShock Infinite VR (**32-bit** UE3, like
  Arkham City). Both draw the full picture twice every frame, once per eye, from two real
  cameras. Infinite VR ran a clean 15-minute test at 80 pairs/second on a Quest 3 via Virtual
  Desktop.
- **How it works:** each frame, the game draws its normal picture (left eye); then we ask the
  engine to draw the whole picture *again* with the camera moved one eye-width to the right
  (right eye), without letting the world advance. Both pictures go to the headset together.
  Real depth, every frame, no shader fixes, no flicker from alternating eyes.
- **What's different from Arkham Knight:** Arkham City is a 32-bit game. That's fine with
  Virtual Desktop (it ships a 32-bit OpenXR runtime; BioShock VR uses it on your PC), but our
  AKVR camera code is 64-bit-only and has to be rewritten. Addresses never carry over; the
  *lessons* do.
- **The cost:** the game's CPU work for drawing roughly doubles. Arkham City is an open city, so
  frame rate is the main risk. We'd target 72-80 Hz pairs and tune from there.
- **JJ's decisions (§9):** DirectX 11 on; BioShock VR's MIT code + our own; third person.

## 1. The target

| Fact | Value | Grade |
|---|---|---|
| Install | `E:\Games\Steam\steamapps\common\Batman Arkham City GOTY\Binaries\Win32\BatmanAC.exe` | STATIC |
| Exe | PE32 i386, 21,135,688 bytes, 2023-02-26, sha256 `b63ab98f…0a332`, image base 0x400000, **LAA already set** | STATIC |
| DRM | SteamStub **2.1** on `.text` (entropy 8.0, `.bind` section). Unpacked copy for study: `batman_city/re/BatmanAC.exe.unpacked.exe` (Steamless). The game itself is untouched and decrypts in memory at run time | STATIC |
| Renderer | Imports `d3d9.dll`; D3D11 loaded on demand. **JJ's config currently runs DX9** (`AllowD3D11=False` in BmEngine.ini and UserEngine.ini). OpenXR has no D3D9 binding → we need DX11 | STATIC |
| Config | `D:\Documents\WB Games\Batman Arkham City GOTY\BmGame\Config\` (BmEngine.ini, UserEngine.ini is UTF-16) | STATIC |
| Render thread | `OneFrameThreadLag=True`; threaded renderer | STATIC |
| Built-in stereo | NVIDIA 3D Vision Automatic hooks (`ALLOW_NVIDIA_STEREO_3D`, `NvStereoFixTexture`, `StereoToMonoClipPos` shader code) and TriOviz (`UDwTriovizImplEffect`). **Neither is two cameras**: one is driver duplication (geo-11-like), the other depth-based 2D→3D. Not our route | STATIC |
| Injection | Imports `DINPUT8.dll` and `XINPUT1_3.dll` → a 32-bit dinput8 proxy works (as AKVR/SKVR); BioShock VR uses xinput1_3 | STATIC |
| Native table | 2,630 `<Class>exec<Func>` `{name, impl}` pairs found (Infinite had 2,647). Dump: `batman_city/re/natives.txt`. e.g. `UGameViewportClientexecGetViewportSize` → 0x6832A0 | STATIC |
| Ghidra | Headless analysis done: project `ghidra/ArkhamCity` (unpacked exe, auto-analysis succeeded). Ghidra MCP bridge is still down (connection refused) → use headless + python/capstone | STATIC |
| HUD | Scaleform GFx (`URGFxGameViewportClient`, `UGFxMoviePlayer`) | STATIC |
| Settings worth noting | `MotionBlur=True`, `DepthOfField=True`, `bSmoothFrameRate` cap 62, `FXAA-HI`, `AspectRatioAxisConstraint=AspectRatio_MaintainXFOV`, `FOVAngle=70` | STATIC |

## 2. The precedents and what each one teaches

References cloned for this project (read-only, outside the playbook):

| Reference | Path | Commit | Licence |
|---|---|---|---|
| Mass Effect 1/2/3 VR (Halcyon / jrush64) | `references/mele-vr/MassEffect{1,2,3}-VR` | ME1 `8776619a`, ME2 `4784e906`, ME3 `cfd40019` | **GPL-3.0** on GitHub (the release zip's LICENSE.txt is stricter and forbids disassembly. **Never disassemble the installed dxgi.dll**; read the GPL source) |
| BioShock VR 0.8.3 (BS1/BS2/**Infinite**) | `references/bioshock-vr-latest` | `41f004bc` | **MIT** |
| BioShock VR 0.4.1 (BS1 only) | `references/bioshock-vr-0.4.1` | n/a | MIT |
| VR Modding Playbook | `references/vr-modding-playbook` | `b955f224` | see its LICENSE files |

### 2.1 Mass Effect LE VR (UE3 x64): "render the whole frame twice" — SOURCE + LOG
- Detours `FViewportClient::Draw(FViewport*, FCanvas*)` and calls the original **twice**; a
  thread-local flag marks the replay. Inside, the `CalcSceneView` detour shifts the eye
  (pass 0 = −half eye, pass 1 = +half eye). Game thread only; never touch D3D from it.
  (ME1 `render_hook.cpp:2597-2717`, `1751-1791`.)
- **Why two full renders, not two views in one frame:** UE3 runs bloom, DoF, light shafts and
  the lighting resolve only for the *first* view in a frame, so a second view loses bloom/DoF
  and characters go black. Fixing at composite time failed. (ME1
  `docs/STEREO_FIX_DOCUMENTATION.md:10-32`.) → **Do not try a two-view family.**
- Eye shift = real camera move: `PreViewTranslation -= w`, `ViewOrigin += w` along camera
  right. ME2 also fixes ViewMatrix translation and rebuilds ViewProjection, or lens flares and
  markers draw from the head centre. (`render_hook.cpp:1653-1686`, ME2 `calcview_hook.cpp:281-308`.)
- **Stale inverse matrices** (inverse ViewProj / Proj / TranslatedViewProj) cause head-locked fog
  and light-shaft bands under pitch. A one-shot scan finds them by numerically inverting known
  matrices, then they're recomputed after every edit. **Writes are whitelisted to the found
  offsets**; a loose scan corrupted memory. (`render_hook.cpp:1928-2101`.)
- **Separate view state per eye** (`FSceneView::State`, from `AllocateViewState(0)`): this is
  temporal history and occlusion. Never NULL. (ME2 `calcview_hook.cpp:1409-1459`.)
- FOV written at the **source** (the camera, before CalcSceneView), not patched into the
  projection afterwards, which smeared refraction. (ME2 `calcview_hook.cpp:1262-1318`.)
- Capture: pass 0 copied at the first scene-sized depth clear after the backbuffer was
  composited; pass 1 = backbuffer at Present. Stale pair: freeze the pose tag *with* the
  pixels (FREEZETAG), and fall back to mono after 45 presents. (ME1 `d3d_capture.cpp:4419-4463`,
  `xr_session.cpp:1836-1861`, `4759-4799`.)
- Guards: SEH + auto-disarm after 3 faults; replay only in gameplay modes (re-entering Draw
  at the **main menu crashed uncatchably**); the engine sometimes silently declines the replay
  (pass 1 < 100 µs) and they retry once.
- HUD: Scaleform draws once (into pass 1) and each UI draw is mirrored into the held pass-0
  image (`d3d_capture.cpp:4313-4417`).
- Cost: CPU submission ~doubles (ME3 hubs ~7k draws/frame, 17-25 ms, GPU 25%) (`docs/PERFORMANCE.md`).
- Finding things: GEngine → GamePlayers[0] → ViewportClient → FViewportClient vtable slot 2 =
  Draw; CalcSceneView by counting calls with `this == local player` among Draw's callees;
  AllocateViewState by the store to `ULocalPlayer::ViewState`. (ME2 `engine_probe.cpp`,
  `calcview_finder.cpp:71-157`, `allocviewstate_finder.cpp:52-152`.)

### 2.2 BioShock Infinite VR (UE3 build 6829, **32-bit**, DX11): the closest match — SOURCE
- Doubled function = the **viewport draw root** (`FViewport::Draw` equivalent): canvas setup →
  client Draw → canvas teardown → present kick. **Doubling only the client Draw is a recorded
  failure** (no second present, eye tags skew). Rule: the root is the smallest function that
  contains camera sample + scene build + present. Only the gameplay caller (return address) may
  double. (`docs/bioshockinfinite/ENGINE_NOTES.md:2904-2926`, `src/game/bioshockinf/scenedraw.cpp:132`.)
- Threaded renderer: doubling **just worked threaded**, no single-thread forcing needed
  (90 draws/s, 90 second draws/s, 180 presents/s; 15-minute soak). (`ENGINE_NOTES.md:2928-2948`.)
  BS1 by contrast needed the render drain moved onto the game thread to cure a deadlock.
- Camera seam: `APlayerController::GetPlayerViewPoint` implementation, out-params only, no
  engine memory written. Pass 2 replays the cached pass-1 base camera +IPD/2 (re-sampling the
  head between passes = vertical disparity). (`camera.cpp:708-716, 889-897, 1443-1463`.)
- **The UE3 trap:** ~13 callers ask for the player view. Giving everyone the VR view broke
  interaction and scripted "is the player looking" checks → a **deny list** of 10 callers get
  the game's own view. But denying the eyes-viewpoint wrapper caused a post-process smear that
  only showed in the headset. (`ENGINE_NOTES.md:1566-1690`.)
- Pose lag 2 (image is 2 locate generations old) fixed "jumpy camera" (`ENGINE_NOTES.md:2548-2558`), the same as AKVR's posedelay=2.
- 30-second GC hitch: `TimeBetweenPurgingPendingKillObjects` 30 → 300 (`ENGINE_NOTES.md:2522-2546`).
- FOV: `FOVAngle` and property writes did nothing; the camera FOV fields must be rewritten on
  every call. Symmetric frustum. (`ENGINE_NOTES.md:2798-2825`.)
- 32-bit OpenXR: VDXR's `virtualdesktop-openxr-32.json` works in-process (LOG on JJ's PC,
  2026-09-22: `xr: 32-bit ActiveRuntime: ...virtualdesktop-openxr-32.json`, session created).
  Static loader, /MT CRT. Don't recreate swapchains on same-size ResizeBuffers (VDXR crash).
  After focus loss xrWaitFrame blocks for seconds → pace guard. (0.4.1 `ENGINE_NOTES.md:117-131, 2196-2226`.)
- Reusable MIT core: `src/core/vr/openxr_runtime.cpp` (pair pacing, eye-tag ring,
  `set_pose_lag`, `set_eye_tag_rendered`), `core/gfx/gfx_hud.cpp` (HUD capture after the
  tonemap blit), `core/hooks/*`, `tools/pe-xref.ps1`.

### 2.3 Playbook @ `b955f224` — SOURCE
- **BL1GOTYVR (Borderlands UE3):** calling `GameViewportClient::Draw` twice **corrupted the heap
  and crashed later** (`0xC0000374`) → fell back to AER. (docs/17 #rung1-refusal.) Together
  with 2.2 this says: double the *viewport* root, gate by caller, run long soak tests (docs/17
  #reentry-endurance; a short burst is not an endurance test).
- Side-effect gate: what advances twice, what runs out, what gets overwritten between write and
  read (docs/17 #side-effect-gate; STR-005, STR-008).
- Occlusion: one eye's decision applied to both, a frame late. The UE3 (Singularity) fix
  forces queries to report visible (docs/14 #per-view-decisions, FAIL-STR-025). Per-eye view
  state should cover this; keep the fix in reserve.
- Motion blur off (FAIL-STR-036). Shared auto-exposure (STR-009). Disable lens flares and
  screen-centre effects (docs/14 "What to disable outright"). HUD/GFx drawn twice advances
  movie state (FAIL-HUD-014).
- Projection: UE3 has no off-centre helper. Hand-build it, change only the x/y terms, keep the
  depth row (a3 A3.1-A3.4). VDXR Quest 3 per-eye FOV is **asymmetric** (~11° vertical; docs/09
  #simpler-substitute). Start symmetric (circumscribed), as BioShock and ME do.
- 32-bit: WOW6432Node manifest, OpenXR handles are uint64 on x86, `__stdcall` exports, LAA
  (docs/09 "32-bit targets", #bitness-routes; FAIL-PACK-003/005).
- Reflection walk offsets on 32-bit UE3 (Singularity) as a starting guess, to re-verify (docs/11
  #reflection-walk). Test the second-call question in x32dbg before writing a hook (docs/06
  #double-call-in-debugger).
- Order: stereo ladder STR-006; PORT-00..13 (start-new-port.md); vrframework guides 08, 09, 10, 16.

### 2.4 Our own mods — what transfers from AKVR / SKVR
Engine **behaviour** (re-verify; addresses never transfer):
- UE3 int rotator (65536 = 360°); turn head yaw about world up; recenter yaw only; gimbal-safe
  heading (memory `ak-menu-camera-gimbal-roll`).
- **Roll sign:** our `build_basis` rolls opposite to UE3, so negate the roll; check against the
  engine's own matrix (memory `ak-roll-sign-vs-ue3`).
- Third-person shoulder offset ("standing left of Batman"), a separate slider (memory `ak-shoulder-camera-offset`).
- **The game reads its camera back** (CAMRESTORE): give it its own values after each frame or
  head motion is applied twice in transitions (memory `ak-camera-readback-double-head`). The
  Infinite deny list (2.2) is the same lesson from the other side.
- OneFrameThreadLag: world vs rain/Scaleform from different frames; pose delay 2-3 (memories
  `ak-one-frame-thread-lag-rain-hud`, `ak-hud-steady-world-jitters`). `akvr/src/frameid.cpp`
  finds UE3 frame counters at runtime, which is engine-generic.
- World scale: AK's 1:1 was 2.8 UU half-eye and 102 UU/m lean. Infinite shipped 150 (UU/m
  world scale), ME uses ~3.4 UU half-eye. **Measure Arkham City's own** (memory `ak-world-scale-calibrated`).
- Aspect lock: AK constrained the *camera* to 16:9. Check whether City does (§4 R5).

Display half (reuse, but port to x86):
- `skvr/src/xr.cpp` (cleanest): session on the game's device, sRGB swapchains, per-eye HUD quads
  (`hud_world_quads`), session back-off. ImGui panel + gamepad; settings ini; VRLAUNCH
  (VR only via shortcut, Steam = 2D); installers.
- **x64-only, must be rewritten:** the hand-assembled camera code caves, the MASM stubs, the
  0x140000000 base maths, `.pdata` function bounds, RIP-relative xref scripts, `lib/win64`.
  On x86: thiscall (`this` in ecx), absolute-address xrefs, hde32/capstone 32-bit mode.

Process rules from our history (all apply): never trust a read-back; print raw lists before
writing matchers; never gate on frame counts; run the head-vs-camera 1:1 regression first;
Virtual Desktop quirks (quads have no parallax, so use per-eye quads; one pose for all
projection layers; narrow projection crops layers); captures in one subfolder; diagnostics
behind `testtools=1`; every third-party/ini change recorded in `batman_city/FIX_CHANGES.md`; never
propose SteamVR.

## 3. Chosen architecture

```
BatmanAC.exe (32-bit, DX11)
 └─ dinput8.dll (32-bit proxy, our loader)  ── VRLAUNCH: VR only from the "(VR)" shortcut
     └─ batman_city.dll
         ├─ hooks: MinHook (x86); D3D11 Present/ResizeBuffers on the REAL swapchain
         ├─ engine: GNames/GObjects, native table, GEngine → LocalPlayer → ViewportClient
         ├─ camera: GetPlayerViewPoint-style seam, caller allow/deny lists, CAMRESTORE,
         │          FOV at source, roll sign, shoulder offset, world scale
         ├─ stereo: double the viewport-draw root (caller-gated, SEH, auto-disarm),
         │          per-eye ViewState, eye shift (PreViewTranslation/ViewOrigin/ViewMatrix),
         │          inverse-matrix repair, pass-0 capture, pass-1 at Present, eye-tag ring
         ├─ xr: OpenXR 32-bit in-process on VDXR, one xrWaitFrame per PAIR, pose lag,
         │      FREEZETAG on stale pairs, per-eye HUD quads, ImGui panel quad
         └─ hud: Scaleform drawn once → captured → per-eye quads at depth
```

Ladder (each rung shippable, switchable at run time, per STR-006): Mono screen → Mono tracked
→ (optional 1-day AER check) → **Same-frame stereo** → fallbacks.

## 4. Milestones

Each one ends with a named test and an evidence grade. Don't start the next until it passes.

**M0 — Setup (no code)**
- Switch the game to DX11 (`AllowD3D11=True` in BmEngine.ini **and** UserEngine.ini) and confirm
  it runs flat. Record in `batman_city/FIX_CHANGES.md`.
- Comfort lines forced on every bucket: MotionBlur off, lens flares off, `bSmoothFrameRate`
  cap raised, VSync off. Leave DepthOfField alone at first (in ME, turning it off killed
  tonemapping, so test it).
- `TimeBetweenPurgingPendingKillObjects=300` (Infinite's GC hitch fix).

**M1 — Reverse-engineering map (static, then x32dbg)** — Ghidra project ready.
- R1 GNames / GObjects / GEngine (native table + string xrefs; `pe-xref`-style caller census).
- R2 **Viewport draw root**: the function that calls canvas setup → `UGameViewportClient::Draw`
  → canvas teardown → present kick. List its static callers; identify the gameplay caller.
  Find `UGameViewportClient::Draw` via the `URGFxGameViewportClient`/`UGameViewportClient`
  vtables.
- R3 `ULocalPlayer::CalcSceneView`, `AllocateViewState`, the FSceneView layout on x86
  (ViewOrigin, PreViewTranslation, View/Proj/ViewProj, the cached inverses, `State`).
- R4 Camera producer (`GetPlayerViewPoint` impl / Rocksteady camera) and **all its callers**
  (print the raw list first).
- R5 Projection build and any camera aspect-ratio constraint (AK had one).
- R6 Where Scaleform draws relative to the viewport root (inside it = drawn twice).
- **Gate:** in x32dbg, call the root twice by hand at a gameplay breakpoint and play on for a
  minute without a crash (docs/06 #double-call-in-debugger).

**M2 — Mono screen in the headset**
- 32-bit OpenXR in-process on VDXR; game image on a floating screen; panel + recenter. Proves the
  32-bit display chain (statically linked loader).
- **Test (HEADSET):** screen visible and stable; desktop mirror OK; Steam launch stays 2D.

**M3 — Mono tracked (head drives the camera)**
- Headset pose into the camera via the render-only consumers (deny list for gameplay callers);
  FOV at source; pose lag; CAMRESTORE from day one.
- **Test:** the head-vs-camera 1:1 regression per axis (memory `measure-head-vs-camera-1to1`);
  world-scale calibration; Batman doesn't drift; grapple/glide/combat unaffected.

**M4 — Same-frame stereo (the goal)**
- Double the viewport root on the gameplay caller only; replay the cached pass-0 camera +eye;
  per-eye ViewState; eye shift incl. ViewMatrix/ViewProj; inverse repair (whitelisted);
  pass-0 capture at the scene/backbuffer boundary; pass 1 at Present; one xrWaitFrame per pair;
  SEH + auto-disarm; mono outside gameplay (menus, Bink movies, loading).
- **Tests:** (a) photo pair of a near and a far object: real parallax, correct eye order;
  (b) bloom, fog, light shafts, shadows identical in both eyes; no head-locked bands when looking
  up and down; (c) **30-minute soak** incl. gliding over the city and a level transition (the
  BL1 heap crash only showed later); (d) pairs/s vs flat fps logged.

**M5 — HUD and menus**
- Scaleform drawn once, captured, shown as per-eye quads at depth (VR_HUD_GUIDE); menus on a
  floating screen over the live world. Detective-mode, map and waypoints checked.

**M6 — Effects audit** (one subsystem per A/B): occlusion (per-eye state, else force-visible),
reflections/scene captures, rain/smoke particles (advance once), screen-space effects, DoF,
eye adaptation, PhysX/APEX cloth (the cape!), detective-vision overlay.

**M7 — Comfort, controls, packaging**: snap/smooth turn decisions with JJ, cutscene handling
(screen vs tracked), VRLAUNCH shortcut, installer + uninstaller, per-mode graphics profiles.

## 5. Risks (ranked)

1. **CPU cost.** Doubling the draw work in an open city on a 2011 engine. Mitigations: square
   per-eye resolution, PhysX lower, shadows tuned, 72/80 Hz pairs, stale-pair FREEZETAG.
2. **Re-entry instability** (BL1 heap crash; ME main-menu crash; BS2 deadlock). Mitigations:
   double only from the gameplay caller, SEH + auto-disarm, menu/movie/loading gate, debugger
   double-call test first, long soaks.
3. **Second-pass side effects** (things that advance twice: GFx movies, particles, sound
   triggers, anim). Mitigation: the replay renders only; audit with the side-effect gate.
4. **32-bit memory** (≤4 GB with LAA; two extra eye images + OpenXR). Infinite and BS1 were fine
   (peak 1.88 GB); measure City's working set early.
5. **The game reading the VR view** (aim, grapple targets, "looking at" checks): the deny list
   and CAMRESTORE.
6. **DX11 mode itself** (Arkham City's DX11 path historically stuttered more than DX9; it's
   required anyway).

## 6. Not doing (and why)
- geo-11/3Dmigoto: JJ's brief, and AKVR showed its limits.
- A two-view frame (two cameras in one view family): UE3 drops bloom/lighting from the second view (2.1).
- 3D Vision / TriOviz paths: not real cameras.
- AER as the product: AK measured 22-36 cm eye separation during spins (memory
  `ak-inter-eye-baseline-measured`). Only as a one-day correctness check.
- Disassembling the installed MELE dxgi.dll: its release licence forbids it, and the GPL source is public.
- SteamVR: no 32-bit OpenXR there anyway; VDXR is the target. (OUTDATED 2026-10-10: SteamVR ships steamxr_win32.json
  now - see STEAMVR in section 7.)

## 7. Files
- `batman_city/re/BatmanAC.exe` (copy), `BatmanAC.exe.unpacked.exe` (Steamless), `ghidra_import.log`, `natives.txt`.
- Ghidra project: `ghidra/ArkhamCity.gpr` / `.rep`.

## 10. Progress log
- 2026-10-03: renamed to Batman City; DirectX 11 switched on and confirmed (window title "32-bit, DX11").
- 2026-10-03: code base = fork of BioShock VR 0.8.3 core (MIT, `LICENSE-bioshock-vr.txt`), our own
  adapter in `src/game/arkhamcity/`. Builds clean (`build/src/RelWithDebInfo/`: `xinput1_3.dll`,
  `batman_city.dll`, test-only `bvr_xrsim32.dll`). Installed in the game folder (`FIX_CHANGES.md`).
- 2026-10-03: probe run LIVE: build verified, SteamStub wait needed (100 ms), 32-bit OpenXR session
  created on the game's DX11 device (fake headset), all four read-only hooks fire. Per-frame draw
  is `0x65FD80`; FSceneView layout and ctor inputs confirmed (RE_NOTES "LIVE results").

- 2026-10-03 (later): **scene seam built** (`src/game/arkhamcity/scene.cpp`, replaces the probe).
  On the fake headset (`bvr-xrsim`, Quest 3 shape) - grade LIVE/SIM, not HEADSET:
  - Mono-tracked: head yaw/pitch move the view; projection layer, headset FOV.
  - **Same-frame stereo:** per-frame draw doubled from caller 0x6C3710 only: 90 draws + 90 second
    draws/s = 179 eye images/s on the title screen; ran 17 min (menus + a level load) with no
    fault; second pass costs ~0.1-0.3 ms on the game thread.
  - Eye geometry verified from two dumps of one frame: ViewOrigin gap 5.670 UU = 63 mm x 90 UU/m,
    100% along camera right, rotations identical (parallel eyes).
  - **Asymmetric per-eye frusta** (JJ asked): core tags each eye with the runtime's own angles,
    the adapter renders the same off-centre frustum. Far-object disparity matched the prediction
    (181 px predicted, 175 sym / 185 asym measured). Default ON (`bc asym off` / F10).
  - **16:9 letterbox found and fixed:** in a non-16:9 window the game passes a 16:9 rect
    (0,323 1274x717 in a 1274x1363 window); widened to the full backbuffer in VR only.
  - Frame cap 62 came from the install's `DefaultEngine.ini` (rewritten into BmEngine.ini every
    launch); raised to 90 there (FIX_CHANGES). Motion blur off.
  - Window: `-windowed ResX=2064 ResY=2208` is shrunk by Windows to 1274x1363 (fits the 1440p
    monitor), shape kept. Resolution is the next quality lever.
  - Auto-VR: camera + stereo arm themselves when the session first reaches FOCUSED.
  - One launch exited after 80 ms (right after a force-kill of the previous run); next launch fine.
- **First headset look: ready for JJ** - `install/Batman City VR (test).bat`.

- 2026-10-03, **first HEADSET look (JJ, Quest 3 / VD)**: game "looks pretty good"; problems:
  (1) eyes flickered converge/diverge - game-thread eye-tag ring skewed constantly under real GPU
  load (render thread one frame behind). FIXED with render-thread labels: pass k's eye is written
  before its BeginDrawing is queued; the BeginDrawing render command (Execute 0x645810) labels the
  frame it precedes; client Draw 0x7CCA30 counts passes. JJ: "3D image is stable now".
  (2) title-screen 2D shears/warps with head motion; (3) menu items and HUD do not fuse (2D drawn
  into each asymmetric eye at the same pixels). Fix in progress: 2D -> core GFx HUD quad
  (gfx_hud). Its boundary rule (BS Infinite: DrawIndexed 6, no DSV, full viewport, backbuffer-
  sized RT source) found 0 boundaries in Arkham City - needs a frame dump to adapt.
  (4) residual micro-flicker in game: built A/B levers - right eye gets its own view state
  (AllocateViewState 0x98B3B0, game stores at LocalPlayer+0x7C), and head-pose delay slider.
- Launcher now in the game folder: `Binaries\Win32\Batman City VR (test).bat` + desktop shortcut.

- 2026-10-03 (later still), all on the fake headset unless noted:
  - **2D -> panel works:** gfx_hud boundary rule adapted for UE3 (scene source up to 16 px larger
    than the backbuffer; capture RT sized to the bound stencil, 1280x1368). Title logo + "Click
    to Start" now on the head-locked HUD quad; the title's grunge overlay is a pre-boundary scene
    effect and stays in the 3D image.
  - **VR menu panel** (JJ: overlay like AKVR/BioShock): F10 menu renders into its own 1024x1024
    texture shown as a ROOM-FIXED quad (placed in front of the head when opened); gamepad drives
    it (game pad blocked while open); **both sticks clicked** also toggles it.
  - **Render resolution:** the monitor cap is gone (viewport resize hook, see RE_NOTES "Window /
    render size"). Presets in F10 "VR resolution": 0.8x 1648x1768, 1.0x 2064x2208 (default, Quest 3
    native via VD), 1.2x 2480x2648, 1.4x 2888x3088, 1.6x 3304x3536; saved to
    `%LOCALAPPDATA%\Batman_City\launch.ini`, applied on the next launch by the launcher.
    2064x2208 and 2888x3088 both run 90 fps doubled on the title screen.
  - Per-eye view state (anti-flicker) + head-pose delay slider: built, awaiting JJ's A/B.
  - Still to do (JJ's list): telemetry - "capture now" + "save last 20 s" recorder (AKVR style,
    behind a test-tools switch); HUD in the room + per-eye depth (AKVR); world scale check.

- 2026-10-03 (JJ round 3: "format the overlay like Arkham Knight, I couldn't find head pose delay or
  screenshot; loading/title screens shear - lock them in world space; the title grunge is right
  in my face"):
  - Menu now matches AKVR: 1440 px panel, 0.984 m, 1.30 m ahead / 0.10 m down, head-facing,
    room-fixed, re-placed on open; default font at 26 px x 0.78; bg alpha 0.86; sliders 42%; AKVR
    order (status, Recenter F12 / Save capture F2 / Head tracking F11, VIEW, HUD, MENUS AND
    SCREENS, VR RESOLUTION, FRAME RATE, Advanced, Diagnostics, Keyboard shortcuts). Core's own
    controls moved to a closed "Core (BioShock VR base)" drawer.
  - 2D panel (title art / menus / HUD) ROOM-FIXED by default (AKVR HUDWORLD), "attach UI to head
    movement" option; movie/loading flat screen placed in front of the head each time it appears.
  - Title grunge = Draw(3) full-screen triangle sampling a 2048x1024 BC1 sRGB texture into the
    scene colour (frame dump event 930). Skipped in VR (core gets a Draw veto hook); toggle in
    MENUS AND SCREENS. Hanging it as a distant backdrop would need re-rendering - not done.
  - Recenter (yaw + position reference, re-places all panels; auto on first tracked frame);
    Save capture = status.txt + last20s.csv (per-frame head pose, camera, timing, eye labels) +
    both eyes' FSceneView + one frame dump, in captures\<date_time>\.

- 2026-10-03 (JJ round 4) - built, fake-headset checked:
  - Title/loading screens: the core head-locked "screen-only" flat screens (BioShock rule) - VD
    renders view-space quads badly (AKVR). Now always room-fixed, placed in front on appearance.
  - All-black flat frames (loading in/out) -> no layers at all = whole view black.
  - Opening logos skipped in VR launches: BmGame\Movies\Startup.swf / StartupNV.swf reported
    missing (inline hooks on kernel32 CreateFileW / GetFileAttributesW / FindFirstFileW; the game
    opens them through msvcr90, so the exe's import table was not enough). Title in ~2 s.
  - EXACT POSE TAGS (core): every locate numbered (16-deep history); the id rides with the eye
    label through BeginDrawing; the layer is tagged with that locate's views. 5862/5862 matched.
    Replaces the 0-2 delay guess (JJ: 1 best, still flickered; 3 impossible - core kept 2).
  - Pad: both-sticks chord never reaches the game; block held 350 ms after the menu closes; blocked
    reads counted in Diagnostics.
  - HUD panel black patches: redirected UI draws get an alpha-corrected blend (over -> ONE /
    INV_SRC_ALPHA; additive/multiply -> alpha untouched), game state restored after each draw.
  - "HUD drawn into the world" option (UI in each eye, symmetric eyes) for reticles on targets.
  - Menu: FRAME RATE drawer (hold game at off/45/40/30, exact pose match, fallback delay), BioShock
    core drawer removed, resolution as a 0.6x-1.8x slider.
  - OPEN (JJ asked): "repeat the frame vs VD SSW" needs a core resubmit loop; zoom (R3) and
    detective-mode vignettes -> one full-view vignette (AKVR ZOOM VIGNETTE pattern).
- 2026-10-03 (round 5):
  - REPEAT (AKVR FPSLOCK, akvr/src/xr.cpp): FRAME RATE "between game frames: repeat the frame |
    Virtual Desktop SSW". Repeat = core holds an early pair-opening present and re-ends the last
    frame's banked layers (projection + quads, deep-copied) on each in-between refresh; the
    xrWaitFrame step was lifted into `wait_frame_state()` so the loop can call it again. SSW = the
    old game-thread sleep (VD sees a half-rate app). Snapshot dropped whenever any swapchain dies.
    Commands: `bc fps 45|40|30|0`, `bc fps repeat|ssw`. Settings are NOT saved between launches yet.
  - KEEP FOCUS (AKVR focus keeper, akvr/src/hooks.cpp): the game paused on focus loss (scripted
    zoom/detective test sat on the pause menu). WndProc never reports deactivation; Get{Foreground,
    Active}Window/GetFocus answer the game window to the game thread only. VR launches only; VIEW
    drawer switch. Scripted tests still hit the pause menu at random moments (cause unknown -
    controller sleep or a stray key?); the test drivers now detect it and press Escape.
  - Fake headset results: repeat at lock 45 = 45 repeated + 45 new /s, 0 late; SSW mode = no
    repeats. Zoom: game fov 19 deg, the 2D zoom frame skipped, full-view vignette at full. Zoom now
    needs BOTH the fov drop and the zoom-frame draws (title camera is ~37 deg).
  - Detective circle (the game's own, one per eye, screen-fixed): NOT removed yet. Ruled out on
    the fake headset: all 40 PS constants of the detective final pass (only [8] and [0]/[1]/[10]
    change the image, none the circle); the camera-attached 6-index 256x128 BC1 quads; the
    detective-only Draw(78) onto the backbuffer. The final pass reads only the scene colour, so
    the circle is drawn INTO the scene before it. Our full-view vignette covers most of it.
  - SAVED SETTINGS (JJ): `%LOCALAPPDATA%\Batman_City\settings.ini`, 29 keys (scene.cpp
    settings_text/settings_load/settings_tick + vision settings_write/read). Change-detected text
    snapshot, written 0.5 s after the last change (atomic tmp+rename), loaded at the end of
    scene::install over the defaults. Not saved: head tracking on/off, test-only switches. VR
    resolution stays in launch.ini. Round trip verified on the fake headset.
- 2026-10-04 (round 6, JJ: vignette too tight; logos back but skippable; logos/loading squash +
  shear; loading screen starts stuck to the face; overlay still passes pad input):
  - Vignette default clear 18 -> 32 deg, fade 18 -> 16; settings_version=2 drops older files'
    vignette size.
  - LOGOS play again (`opening_logos`, MENUS AND SCREENS). Skip = any pad face button / Start /
    Back or Enter/Esc/Space while a logo .usm streams -> GFullScreenMovie (0x16CB074, GFx class
    vtbl 0x13B3C58) slot 5 GameThreadStopMovie(0.0, FALSE, TRUE) = 0xB462A0, which is only an
    InterlockedExchange(this+0x3C, 2) -> safe from any thread. Startup*.swf hidden after the skip.
    FAILED first: returning EOF on the .usm reads froze the game (player waits for data).
    Fake headset: press -> movie ended 0.6 s later, NVIDIA logo skipped, title, save loads.
  - MOVIE SIGNAL: slot 8 (vtbl+0x20) GameThreadIsMoviePlaying(L"") polled on the game thread
    each frame -> bvr::vr::set_flat_screen(); no eye doubling while it plays. The loading screen
    plays through it; the game keeps drawing the world for ~3.6 s under it, which is why the old
    stale-view rule left it "stuck to the face". Now flat within 40 ms of the movie starting.
  - SCREENROOM (core): flat screens are drawn by us onto the room-fixed screen pose into BOTH eye
    swapchains with each eye's located pose + fov (blit::screen_in_eye, clip corners on the CPU)
    and submitted as a full-view projection layer - a lone quad layer is what VD squashed/sheared
    (SKVR SCREENPAD lesson). Switch: MENUS AND SCREENS / `room_screen`.
  - PAD: lone stick clicks held back 120 ms (replayed if released early) so half of the
    both-sticks menu gesture never reaches the game; DirectInput game controllers frozen while
    the menu is open (core/input/dinput_block.cpp; the game has AllowJoystickInput=1 + the WMI
    IG_ filter). Log line "the game reads a game controller through DirectInput too" = that leak.
    Not testable here (no pad connected to this PC): JJ's headset session decides.
- 2026-10-04 (round 7, JJ: overlay STILL passes buttons; Meta-button recentre; a HUD piece too far
  apart in the two eyes, capture 20261004_035226):
  - JJ plays on Quest Touch controllers; his capture: XInput reads blocked 0, DirectInput 0 ->
    the game's pad input never passed our XInput seam (Steam Input suspected). ENGINE BLOCK:
    UGameViewportClient InputKey 0x7BDD50 / InputAxis 0x7BDF20 (vtbl 0x12F8FE0 slots 5/7, ret
    0x1C) hooked; while the menu owns the pad every press/axis is consumed; the release of a
    swallowed press is swallowed too (Arkham acts on releases: detective mode toggled from a
    passed-through release). Lone stick click held 120 ms at the engine (FNames
    XboxTypeS_Left/RightThumbstick at 0x1662B50 / 0x1662B58). Fake headset: X with the menu open
    no longer toggles detective mode; after closing it does.
  - RECENTRE: XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING handled (was ignored) -> adapter
    re-zeroes at changeTime, panels/screens re-placed; backup for VD: a >28 deg / >0.5 m one-frame
    head jump counts as a recentre (VD runtimes only). Sim `recenter` verified.
  - xrsim BUG fixed: projection-layer view poses were read as WORLD poses (ignored the layer
    space), so any sim recentre showed every 3D frame turned by the recentre yaw.
  - HUD: BioShock's "<=12 vertices = full-screen flash, leave in the eye image" passthrough
    caught Arkham's ordinary 4-12 vertex HUD pieces (incl. a 90x83 rendered widget) -> drawn into
    both eyes at one screen spot under asymmetric eyes = no fusion. Off for Batman City
    (`bc hudfx on|off`). Full-screen flashes now land on the HUD panel too.
  - Fake-headset caveat: `vrinput on` (synthetic pad) brings the core's pitch servo, which pitched
    the game camera ~80 deg after a recentre - test-only, JJ never enables it.
- 2026-10-04 (round 8):
  - Captures (F2) now save eye_left.bmp / eye_right.bmp (frame as copied for the headset) and
    hud_panel.bmp (32-bit, ALPHA KEPT - PIL reads BI_RGB 32-bit as opaque; read the bytes).
    `bc huddump` lists one frame's HUD draws + blend states.
  - Compass on black + black bar: Scaleform CLIP MASKS (colour writes off) got alpha writes turned
    on by the "over" alpha fix -> solid black rectangles on the panel. Alpha write is now added
    only where the draw writes colour. Verified: panel picture 20261004_044029 clean.
  - Menu: both sticks held 0.6 s (AKVR HOLD_MS); panels/screens placed yaw-only (hang level).
  - HUD redirect off while a full-screen movie plays.
  - OPEN, loading-screen black box over the objectives text (top left): drawn inside the movie
    player's own path (not in our draw stream - a loading-frame dump shows only the game frame
    under it, plus an untextured DrawIndexed 36 now skipped during movies, which did NOT remove the
    box). Present since round 5. Suspect the movie's layout/masks at our near-square size; a 16:9
    VR-window test went astray (menu navigation differs at 16:9) - redo with a 16:9 run that
    loads the save, or capture a flat 16:9 loading screen to compare.
  - Loading box, more (PARKED by JJ, "do this last"): 16:9 VR window (2560x1440) = loading screen
    CORRECT, so it is the game's layout at our tall size. Tried and FAILED: Stage.visibleRect clamp
    (0xF8C7D0, never called), skipping DrawIndexed 36 / the first 4 plain 30-index rects of the movie
    frame, banding the full-screen player's own new view (the loading screen is another, earlier
    Scaleform movie). Found: every movie view's SetViewport = 0xF30BF0 (GViewport ints: buf w/h,
    left, top, w, h, ...). Built but UNTESTED and switched off (window.cpp
    install_movie_viewport_hook): remember all full-buffer views, band them all to 16:9 while a
    full-screen movie plays, restore after.
  - OPEN, grapple reticle not on its target: with the HUD in the eye images the reticle sits at
    the SAME pixel in both eyes while its gargoyle is ~200 px apart - the game places markers once
    per frame from its own camera (no head, no eye). Plan: classify world markers (moves with
    the game camera, not with the HUD), re-project each per eye from game camera to eye camera,
    depth from the eye's depth buffer at the target (frame-delayed readback, AKVR PANELREAD ring),
    keep them in the eye images; fixed HUD stays on the panel.

- 2026-10-07 (round 9, JJ: grapple reticle on its object at its depth; pause-menu dirt and title grunge
  room-fixed at a distance; "refer to Arkham Knight"):
  - MARKEREYE (markers.cpp): the HUD script places world markers with Canvas.Project (UCanvas::execProject
    calls the FSceneView project 0x972FB0; return address 0x8FDFD2) ONCE PER EYE PASS with that pass's own
    view, but the movie shows one layout in both eyes. Every Canvas.Project result in a stereo pass is now
    forced to the LEFT eye's view of the same frame (known reference point); per HUD draw, a flat
    Scaleform piece whose cb0 translation is within "reach" (0.12 NDC) of a reference point is a marker
    piece: kept out of the HUD panel (gfx_hud marker callback, game RTV restored) and moved for this eye by
    (eye projection of the world point - reference point) via UpdateSubresource of its cb0, put back after
    the draw. Fake headset: the reticle (9 pieces incl. the F prompt) moved as one, ~10 pieces/pass, no
    faults. Not yet seen with its target visible (the grapple point was behind Batman); "marker frame lag"
    0/1 slider for JJ's A/B (is the movie's layout from this frame or the last?). AKVR lessons applied:
    one point per marker part (TARGETDEPTH), check our own geometry changes first (TARGETSTOCK).
  - Ruled out for markers: VectorToScreen, Canvas.WorldToPixel (0 calls); 0x972FB0's other callers are
    the render thread's sprite/flare code (0xA78E9C/0xA796A6, ~50 points/view); FSceneView::WorldToScreen
    0x971CE0 callers are culling/LOD (the Batman-side 0xC582A1 uses only w). Grapple gun target position
    = gun+0x460 (GetGrappleTargetPosition 0xDAB4F0), not needed now.
  - ROOMGRUNGE (room_overlay.cpp): the title grunge (Draw 3, multiply blend DEST_COLOR/ZERO into the fp16
    scene) is replayed onto black and white targets and laid onto a room screen (9 m wide at 3 m,
    feathered border) per eye with dual-source blending (dst' = black + (white - black) * dst). The
    texture also carries the title art (moon, bats, skyline). MENUS AND SCREENS: as the game draws it / in
    the room / hidden; distance, width, height sliders.
  - PAUSEDIRT: the pause look is two Draw(3) passes into the scene: an ink-outline pass (fine per eye) and
    the dirt pass (t0 4x4, t1 = scene copy fp16, t2 = 512x512 BC1 dirt, alpha blend) that re-draws the
    greyed scene times dirt sampled in SCREEN space. Fix: per eye, the dirt is pre-warped onto a sphere of
    radius 3 m around the recentred head (ps_warp, mirror addressing, spread 150 deg) into a half-size
    sRGB target that replaces t2 for the game's own draw. A flat-plane version streaked when the head
    turned (mirrored paper border in perspective) - replaced by the sphere; the sphere build is
    DEPLOYED but NOT yet run (JJ was playing Arkham Knight).
  - MARKERBASE (same day, JJ in the headset: "more locked to their objects now and at object depth, but
    they jitter a lot, especially when moving the head"): the reference was the LEFT EYE view, so any frame
    offset between the movie's layout and the eye image turned head motion into jitter (AKVR
    one-frame-thread-lag lesson). Reference is now the game camera's own view-projection (no head, no eye,
    symmetric; the head only ever enters the eye views). Built + deployed, untested (AK open). If camera
    (stick) turns still jitter, that is the remaining frame offset: "marker frame lag" 0/1.
  - MARKERCARD (same day, JJ: markers "spin on their own axis to keep facing the camera" on head turns -
    should only happen with mouse / pad camera turns): each piece was flat in each eye's picture (facing
    the eye). Now the marker is a card facing the GAME camera at its object's view depth: F = reference
    picture -> card (same base clip z, w as the object, inverse base VP) -> this eye; the piece's cb0
    rows 0/1 and translation go through F's 2x2 Jacobian at the reference point (finite differences,
    h = 0.01). HUD drawer switch "markers turn with the game camera, not your head" (marker_card).
    AKVR TARGETFACE did the opposite (faced the eye) - JJ's request here is game-camera facing.
    Built + deployed, untested (AK open).
  - GRIME OFF (same day, JJ: "better without those grime maps on the menu screen and the pause screen"):
    title grime default "removed" (Draw veto); pause grime removed by giving the game's own pause pass a
    4x4 white texture in place of the dirt (keeps its greyed look). Room versions stay as options; new keys
    title_grime / pause_grime (the old grunge_mode / pause_dirt_room are ignored). F2 no longer saves
    replay pictures; F2 frame dump now covers 2 presents (both eyes).
  - OPEN, puddle reflections differ per eye (JJ capture 20261007_235303: right eye bright blue streaks on the
    wet road, left dim). Config: Reflections=True, ImageReflectionTextureSize=1024 (UE3 image-based
    reflections), SceneCaptureReflectActor material set. The one-eye frame dump of that capture has NO
    1024 target -> suspect the reflection texture is built in one eye's pass only and reused (or stale) in
    the other. Ranked: (1) once-per-frame reflection build; (2) right eye's own view state (VIEW drawer A/B);
    (3) real specular difference (unlikely this strong). Next: two-eye F2 dump, then per-eye build or
    Reflections=False as the fallback.
  - Puddles, 2026-10-08 (JJ capture 20261007_235945, two-pass dump): q0 = RIGHT eye of frame N, q1 = LEFT eye of
    frame N+1 (told apart by the ViewProjection values in cb0 vs targets.txt pass views). The right eye
    showed the THEATRE neon sign and its red/green light on the wet road, the left eye neither. The left
    (first) pass draws ~300 more scene draws: extra light passes (stacks 0x784570/0x5AB6B6 +17,
    0x5AB85D +58) and a 512x512 R32 shadow map (0x5C8E2E, 16 draws) that the right pass does NOT draw -
    per-frame lighting work done in the first pass and reused by the second.
    STATE1 (left eye own view state) A/B on the fake headset: the sign showed in both eyes with it on AND
    off; no other view used the game's view state (diagnostic log stayed silent) -> NEGATIVE, default off.
    The sign itself flickers, so a sign-in-one-eye picture can be flicker timing. The right eye's glossier
    road remains. NEXT: F2 now dumps 3 passes (always one frame's L and R) - diff light passes and their
    constants within one frame; suspect per-frame light/shadow/reflection setup done for the left view only.
  - REFLECTEYE (2026-10-08, JJ: "the left eye is correct ... the reflection in the right eye is shifted over
    way too much to the right"; captures 20261008_004737/004827, 3-pass dumps): the wet road (DrawIndexed 5676,
    srv0) samples a 256x240 RT|SRV planar reflection that the engine's scene capture renders only in the
    LEFT pass (74 / 35 capture draws at vp 256x240 in the left passes, 0 in the right) - the probe updates
    once per game time, and both eye passes share one. Left/right passes told apart by our pair-submit
    draws (2688x2880) at the start of each left-pass window. Fix (reflect.cpp, called from vision's
    DrawIndexed hook): in the right pass, at the first draw with the reflection at srv0, copy it and draw it
    back shifted by du = -(m20R - m20L)/2 (lens offsets; ~+0.243 for Quest 3 via VD), clamp at the edge;
    the next left pass re-renders it. HUD drawer switch "puddle reflections lined up in the right eye"
    (reflect_fix). Built + deployed, untested (AK open). Proper fix later: make the capture render per eye.
  - REFLECTEYE headset result (JJ, captures 20261008_010434/010451): NO CHANGE, though the log shows 2493 right-pass
    shifts, du 0.243. Measured in 010451: the right eye's neon streaks sit ~540 px right of the left eye's (about
    the full lens-offset shift), i.e. the shift did not land. Mode-3 dumps (F2 now arm(3,3): all PS slots) of
    the fake-headset run 010819: the ONLY render target the right pass reads without writing it is the 256x240
    capture (road DrawIndexed 5676 at t0, a 4416-index surface at t2); road PS constants hold no matrix (screen-
    position sampling). Open: why the in-place shift has no effect - suspects: wrong pass (eye label at
    BeginDrawing vs the GPU pass), the shift drawn into the texture but overwritten / not seen, or RTV view-format
    issue. NEXT: fake-headset A/B with `bc reflect on|off` (tools/tests/reflect-ab-run6.ps1), compare BOTH eyes
    pixel-wise; log the shifted pass's eye + begin index next to the capture-draw pass.
  - JJ 2026-10-08 01:3x: toggling REFLECTEYE changed NOTHING in either eye; right-eye reflections slide with head
    turns. Likely cause of the no-op: blit draw_internal never cleared HS/DS/GS - mid-scene the game has
    tessellation bound, so our triangle-list draw was rejected (the title/pause blits ran where none was bound).
    BLITSTAGES: draw_internal now saves, clears and restores HS/DS/GS. Deployed, untested.
  - DETSTEADY: JJ saw "random vignettes ... when looking around"; the vignette layer was created at the start of
    gameplay (01:31:56) with no zoom logged -> the detective-pass signature (DrawIndexed 6, no DSV, VS cb 640 B,
    PS cb 160 B) matched something else. Detective now needs an unbroken run of 12 presents; ON/off transitions
    are logged with the pass's target size. Deployed, untested.
  - PERCAPTURE (2026-10-08 02:2x): the shift attempt had NO visible effect (fake headset: the shift itself works -
    before/after BMPs - and fires every right pass), and JJ described the right eye as "skewing and stretching"
    like a picture projected from another viewpoint: the ground maps the reflection through the capture's own
    mirrored camera. Found the engine path (frame-dump stack values are RVAs - add 0x400000): main render
    0x9B22E4 calls 0x99A890 (scene captures, if ShowFlags & 0x100) before the main scene 0x9B1E40; per capture
    probe->vtbl[1] CaptureScene(SceneRenderer). Reflect probe vtbl 0x1302414: [1] = 0x80A480 CaptureScene,
    [2] = 0x80A2B0 UpdateRequired(ViewFamily*): TimeBetweenCaptures +0x4C, LastCaptureTime +0x48 vs
    family CurrentWorldTime (+0x1C); CaptureScene stores LastCaptureTime = family time (0x80A51E). Both eye
    passes share one game time -> the right pass always skipped. Fix (reflect.cpp): hook 0x80A2B0; in the
    right pass, a probe that updated in this frame's left pass returns TRUE -> rendered mirrored from the
    right eye's own view. Shift kept as option reflect_shift=1 (off). Deployed, untested (AK open).
  - DETSIZE: the false detective match drew into 256x16 (the exposure measure -> "certain areas"); detection now
    needs a target >= 1000 wide. Deployed, untested in the headset.
  - PERCAPTURE CONFIRMED by JJ in the headset ("well done, you fixed it"). Same day: HUD panel widened to ~75 deg
    (JJ: "covers about 75 deg rather than the current 50-odd"): default width 2.0 m at 1.3 m, JJ's settings.ini
    hud_size_m 1.250 -> 1.995 (backup settings.ini.before-hud75); HUD drawer shows the panel's angle. Deployed.
  - JJ 2026-10-08 03:00, three new items:
    (a) LENS FLARES not fusing -> LensFlares=False in BmEngine.ini + UserEngine.ini (FIX_CHANGES 2026-10-08).
        Told JJ a proper fix is possible (build each flare from the head-free game camera centre, place per
        eye like MARKERCARD) but flares stay view-tied; his call.
    (b) BAT TRANSITION before the main menu: a black square with bat-shaped holes, shown as a box (on the HUD
        panel?) - JJ wants it to fill the whole view. Data needed: HUD dumps through the transition.
    (c) MAIN-MENU WATER DROPLETS around Batman turn to face the head (particle sprites billboarded per eye view).
        Plan: find the sprite draws' camera right/up constants (mode-3 dumps, head straight vs turned+rolled)
        and give them the head-free game camera's axes, like MARKERCARD.
    Test ready: tools/tests/title-droplets-bats-run7.ps1 (not run yet - AK open).
  - BILLBOARD (2026-10-08, JJ: main-menu droplets turn with the head): title captures head straight vs turned 30 +
    rolled 20 (mode-3 uploads): particle sprite draws (stack RVA 0x5EF772 path) get the view right/up axes at VS
    cb floats 16-18 / 12-14 (320-byte buffer; their lighting draws the 640-byte one). billboard.cpp maps both
    from the eye frame to the game camera (v * Eye3 * Base3^T) in the UpdateSubresource tap (vision upload_tap),
    only when they match this eye's axes. Fake headset: 83743 patches, no faults. Headset check pending.
  - BATCUTS (JJ: the bat-wipe "transition cuts" show as a black box): hudline timelines (`bc hudline N`, settings
    debug_hudline_after_movie=N) show the wipe as Scaleform draws with NO texture (or a screen-size picture),
    drawn 2-4x screen size, after the logos and when leaving the menu; never in normal HUD frames. gfx_hud keeps
    such cut pieces in the eye image (whole view) and scales them 1.6x about the centre (16:9 layout left bands
    in the tall eye) when the piece is > 1.5x screen size or the previous HUD frame was cut-pieces only (the
    title logo / bat symbol are untextured too, at normal size, among other pieces). Fake headset: logos-cut and
    menu-out cut fill the view; title logo normal. OPEN: right after the loading screen into gameplay a black
    box still shows on the panel (the wipe's zoom-out, or the parked loading box).
  - DLSS question (JJ 2026-10-08): Arkham City is 32-bit; NVIDIA NGX / nvngx_dlss.dll exist only as 64-bit -> no
    in-process DLSS. Options: 64-bit helper process (shared textures + fences on the same GPU, reuse
    akvr_native/src/dlss.cpp + its lessons: CAMMV, MIPBIAS, HUDFULL, DLSSMIRROR) or FSR (32-bit capable).
    PERF (fake headset, gameplay, 2480x2648 per eye, tools/tests/perf-measure-run10.ps1): 90 stereo fps at both
    90 Hz and "refresh 240" (the game's own MaxSmoothedFrameRate=90 caps it); GPU 92-100 % busy, 2835 MHz, ~183 W;
    second-eye draw ~0.4 ms on the game thread -> GPU-BOUND: an upscaler buys frame rate.
  - DLSS (2026-10-08): batman_city/DLSS_PLAN.md - 64-bit helper, default Performance where available, FXAA
    replaced, sharper textures, panel section "DLSS (SMOOTHER EDGES)".
  - AIMDEPTH (2026-10-08, JJ: "the bat claw and batarang target reticles should be at world object depth, not at
    HUD depth"): the aim reticle is six Scaleform pieces at the picture centre (HUD dump: 32x32 / 128x128 BC1/BC3
    sprites, translation (0,0) +- 0.053; explosive gel has none); no game projection places it. markers.cpp marches
    the head-free game camera's centre ray (96 geometric samples, 250 .. 50000 game units; nearer surfaces skipped:
    the ray passes Batman's arm) through THIS eye's depth buffer in a one-thread compute pass (scene_depth SRV,
    targets unbound around it), reads the answer back 3 passes later (no stall), and places the centre pieces as a
    marker card at that point. Fake headset (tools/tests/reticles-run19.ps1): reticle disparity = the wall lamp's
    in both eyes, stays on the wall with head yaw 12 / pitch -6, batclaw same; 3113 answers, 26564 pieces.
    Switches: panel "batarang / batclaw aim reticle sits on what it points at", slider "ignore surfaces nearer
    than"; `bc aimdepth 0|1`, `bc aimnopass 0|1` (test). Saved aim_depth / aim_min.
    Test lesson: a synthetic right click lands on whatever window is under the pointer -> Arkham City loses focus
    and pauses (looked like the new code; it paused with the fix OFF too). The script now moves the pointer over
    the game window, checks WindowFromPoint, clicks, puts it back. `vrinput test trig` did not make Batman aim.
  - GLOW / DOF (2026-10-08, JJ: glows flicker, "very low resolution ... blockiness around distant windows", also
    with DLSS off): the final combine pass (frame dump event 1838) reads scene HDR (fp16 full), a 622x664
    R16G16B16A16_UNORM "DOF and bloom" buffer, quarter-size light shafts and a 256x16 exposure strip. Measured
    (fake headset, paused title-less city scene): that buffer added +55 to the far sky and 0 to bright near
    surfaces -> it is DEPTH OF FIELD (far blur at quarter size), not a brightness glow. Fix: DepthOfField=False in
    BmEngine.ini (FIX_CHANGES 2026-10-08). glow.cpp (our own bloom: Karis prefilter, 6-level pyramid, tent
    upsample, swapped into the combine pass) stays as a panel option, default = the game's (mode 0); its
    threshold needs tuning (1.0 = nothing; 0.3 lights snow too).
    Test lesson: the pause screen is grey - is_paused.py was RIGHT; Escape sent through Key() (Alt tap) re-pauses
    on the focus loss -> send Escape without the Alt tap (tools/tests/glow-run20.ps1).
  - NODOF (2026-10-08, later): DepthOfField=False skipped the whole combine pass (no light shafts / grading) ->
    REVERTED (FIX_CHANGES). Shader recorder (glow.cpp: D3D11CreateDevice* hooked early, CreatePixelShader bytecode
    kept, full-screen pass shaders saved to %LOCALAPPDATA%\Batman_City\shaders): the combine pass (ps_16) =
    depth -> unfocused amount (cb0[8] focus distance / 1/radius / exponent, cb0[9] max near / far blur), lerp
    scene -> 4x quarter-size blurred copy by max(that, 4 x blurred alpha), + light shafts (t2), gamma 1/2.2, 16^3
    colour-grading LUT (t3, 256x16), luma in alpha. NO bloom at all: the "glow" was all depth of field. The mod
    binds its own copy of ps_16 without the DoF blend at that draw (default; panel "game's depth of field",
    setting game_dof). Fake headset (tools/tests/nodof-run21.ps1): in-focus pixels identical to the game's (diff
    0.00), only the far blur changes. Our bloom (glow_mode 2) stays an option, default none.
  - GAMEGLOW / GLOWHUNT / RETICLEHD (2026-10-09): JJ: the "glow sprites" (additive ONE/ONE scene draws) were the
    volumetric light BEAMS -> renamed "light beams" (on/off, strength via a layer added back in the combine copy).
    Hiding each of ~60 blended scene-draw groups never removed the glow -> it is the combine pass's light-shaft
    buffer t2 (quarter size, added after the DoF blend): "game's glow strength" scales it, "smooth glow edges" =
    bicubic B-spline enlargement in our ps_16 copy. Grapple reticle = ~9 tiny BC1/BC3 pictures (rings, arcs with
    ticks, arrow, bars); markers.cpp fingerprints each piece texture once (RGBA8 render + readback, FNV-1a) and
    swaps in tools/reticle_hd.py's 8x versions (rings redrawn as perfect circles, arrow core sharpened with its
    halo kept, arcs crisped) from <game>\BatmanCityData
eticle; panel "sharp grapple reticle".
  - HUD controls (2026-10-09, JJ: HUD "cramped into the middle", "some elements too big", "sliders to position
    them", "0.01 per D-pad click", "further away like Arkham Knight"): ONESTEP ported from AKVR (core
    overlay SliderStep / SliderStepInt: one displayed step per press, ImGui's left/right muted while a step slider
    is controller-active; all 38 panel sliders). HUDZONES (gfx_hud): panel pieces put in 7 zones by placement
    (top-left, top-centre, top-right, right, left, bottom, centre), each scaled about its corner / edge and moved
    via cb0, put back after the draw (after_draw now also runs on the blend-fixed redirect path); saved hud_zone_N.
    HUDEYES (VR_HUD_GUIDE section 1): the HUD quad submitted per eye, shifted by (eye - midpoint) from
    xrLocateViews, auto under Virtual Desktop. HUDDIST: panel set by distance + angle (moving keeps the size);
    JJ's settings moved to 4.0 m at his 88 deg (backup settings.ini.before-hud4m). BATCUTS fix: square font
    atlases (>= 1000) are not wipe pictures (the "Joker Radio Signal Strength" heading was thrown into the eye).
    All untested in the headset at writing (JJ declined the fake-headset run; Arkham Knight open).
  - FREEZES with DLSS (Xid 13 SAVE_RESTORE_ADDR_OOB at 22:49:34 and 23:05:54, both DLSS sessions; Windows
    hardware GPU scheduling ON, driver 617.14; JJ will not change scheduling): DLSS default now OFF (JJ's saved
    setting set to 0); JJ values DLSS's anti-aliasing (fences). Next: move our in-game compute passes (CAMMV,
    AIMDEPTH) to pixel shaders and retest DLSS.
  - Probes kept: scene Draw(3) census (every texture set into the fp16 scene, logged once) and F2 replay
    pictures (replayN_black/white.bmp, quarter size) - how the pause dirt was found.
  - Test script: tools/tests/marker-dirt-run3.ps1 (title, mouse sweep 10 x 150 counts, pause);
    dirt-edges-run4.ps1 (title + pause only). The game sometimes sits in its pause menu after loading;
    the scripts check is_paused.py and press Escape.
  - FADEHOLD (2026-10-09, JJ: "a big black box in the centre of the view when entering gameplay from the main
    menu", "maybe a wipe but I can't see the bats"): NOT the bat wipe. Panel pictures (new test switch
    debug_panelseq_after_movie=N -> %LOCALAPPDATA%\Batman_City\panelseq) show an opaque black rectangle over ~83 %
    of the panel (pixels 0,115-1438,1390 of 1440x1536) that fades out over ~0.8-0.95 s after the loading movie =
    the game's fade-in from black, laid out on the HUD movie's stage -> on the panel. Its pieces use the ordinary
    4x4 / 256x1 fill textures at the stage's placement scale (1.9e-4, same as normal HUD pieces), so no per-draw
    rule separates it (CUTGRAD - gradient pieces > 1.5e-4 / 6e-5 to the eye - tried in runs 27/28: it moved a
    real HUD piece and missed the box; removed). Fix: for up to 3 s after a full-screen movie ends, gfx_hud reads
    5 texels of the panel picture (async, 2 frames late) and gives the runtime no HUD texture while all 5 are dark
    and covered. Fake headset run 29 (tools/tests/fadehold-run29.ps1): held 953 ms while mean panel alpha went
    211 -> 38, shown at 5 (= normal HUD); title unaffected (no panel there for ~3 s anyway). Panel checkbox "hide
    the black fade box after loading screens", saved hide_loading_fade_box (default on), `bc fadehold 0|1`.
    Timeline lines now also give per texture the largest placement and how many pieces stayed in the eye (s.. e..).
  - LOADING BOX (JJ again 2026-10-09: small black box over the text at the top left of the loading screen): the
    parked 16:9 band (install_movie_viewport_hook) was tried live (`bc movieband on`, run 26): WRONG - the loading
    picture went small and off to the lower right, objectives text gone. Hook now installed (passive, records
    views); the band stays off.
    LOADBOX2 (JJ's capture 20261009_153608 + its frame dumps q0..q2): after all the objectives text the loading movie
    draws one more Scaleform piece anchored at the stage's top-left corner, VS cb0 translation (-1.0, +0.702): a
    solid-fill DrawIndexed(30) (in another frame the 64x64 bat sprite) plus a filter composite of it (Draw 4 from a
    128x128 offscreen target) before the text. While a full-screen movie plays, pieces at x = -1 / upper half
    (indexed <= 30 or Draw 4) are not drawn (vision.cpp loadbox2_skip, `bc vis loadbox2 0|1`, default on). Built,
    NOT yet seen working (JJ tests by hand).
  - EXIT WIPE (JJ 2026-10-09: the bats wipe when leaving the game for the main menu shows small on the panel, should
    fill the view like the menu one): no data yet. HUDRING test recorder (settings debug_hud_ring=1, set in JJ's
    settings.ini - backup settings.ini.before-hudring): the per-frame HUD lines of the last 360 frames are written to
    the log ([hudring]) when a full-screen movie starts (the load after the wipe). JJ: test runs must not wait over
    5 s in menus (memory test-plans-one-launch).
    JJ's session 15:54: LOADBOX2 fired (4 "top-left piece not drawn" lines on the loading screen). Ring at the pause-menu
    restart/exit (15:55:03): ~190 frames of one untextured piece at placement 0.14 (kept in the eye, e1) + six 256x256
    BC3 bat sprites (s0.1) + a 64x64 + 4x4/16x16 pieces, all on the panel (e0) = the bats in the middle. The main-menu
    wipe that works is untextured pieces only (s ~4e-5). WIPEALL (gfx_hud): a frame with a cut piece at placement
    > 1e-3 (menu-movie scale) sends every piece after it, and the whole next HUD frame, to the eye image (scaled 1.6x
    with the cut). HUDRING now always records; captures save hud_last_frames.txt (JJ: "a function ... which records
    the last X amount of frames, so that once it happens, I can then go and capture it"); debug_hud_ring=1 only adds
    the log dump on movie start. Built; deploys when JJ closes the game. Menu path (JJ): title -> A, wait ~10 s,
    A, A = gameplay (3 s before the third press was too short - run 32 ended on save select; 5 s works).
  - PAUSEHALO (JJ 2026-10-09: "when pausing ... because of the black-and-white effect ... the menu buttons along the
    bottom are almost impossible to see ... some sort of soft feathered background for the buttons"): while the pause /
    map screen is up, the panel shows a copy of the HUD capture with a soft dark halo under all its content (blit
    alpha_down: alpha averaged over 8x8 blocks into a 180x192 picture; halo_under: three rings of 12 taps, radius 3.5%
    of the panel width, gain 6, then alpha = a + (1 - a) * halo * strength - premultiplied, so only alpha rises).
    Trigger: the pause screen's own grey pass (room_overlay pause_screen_now, at once) or the stopped game clock.
    Panel slider "pause menu: dark glow behind text and buttons" (default 0.8, saved pause_text_halo), `bc halo N`.
    Fake headset runs 32/33: dark feathered surround behind the pause icons and the "Back / Select" prompts; halo
    on within ~50 ms of the pause. Test trap again: Key()'s Alt tap re-pauses / unpauses (focus) - Escape needs the
    no-Alt path (see the GLOW entry).
  - STAGEBOX / FIRSTCUT / FADESKIP (JJ 2026-10-09 captures 184851 / 184932 / 184953 / 185004 - HUD ring, 15 s): the
    "small black square" before / after menu wipes and when the main menu appears after leaving the game = FILL pieces
    (4x4 fill or untextured) at the HUD stage's scale (placement 1.9e-4 .. 6.7e-4) in the frames next to the wipe -
    not cut pieces, so panel. Menus' own fills: 9.8e-5 (save select), 0.14 (pause), 1.9 (main menu); gameplay HUD
    <= 1.1e-4. Save select -> title: the wipe's FIRST frame (one untextured piece) went to the panel. Gameplay entry:
    the fade box grows then shrinks to the centre, so FADEHOLD's probes fell outside it (18:49:50 "clear" after 593
    ms). Now (gfx_hud, `bc stagebox 0|1`, default on): outside gameplay or within 1 s of a wipe frame, stage-scale
    fill pieces stay in the eye with the wipe; outside gameplay a cut piece that is the frame's first draw stays in
    the eye; in gameplay for 3 s after a full-screen movie the stage-scale fills are not drawn (Decision.skip, new in
    frame_inspector). WIPEALL now only takes pieces at the wipe's scale (> 1e-3): capture 161059 had a whole gameplay
    HUD frame pushed into the eye after a pause wipe. FADEHOLD v2: clear only after the box was seen + 2 clear probes;
    no box within 0.7 s = panel back. Fake headset: run 36 fade pieces not drawn, panel 0% dark through the hold;
    run 38 exit to the main menu (path in memory test-plans-one-launch): panel never showed a square (max 1.2% =
    menu icons). Health bar absent in runs 36-37 with the switch OFF too = the game's own out-of-combat hide.
    Not reproduced on the fake headset: save select -> B -> title (scripted Escape never registered) - JJ tests.
  - JJ 19:18 session: (1) title "BATMAN ARKHAM CITY" logo "stuck to my face and huge" = FIRSTCUT (the logo is a
    1024x256 cut-type picture drawn first) + STAGEBOX's outside-gameplay branch -> both removed (STAGEBOX only within
    1 s of a wipe). (2) pause-menu exit wipe still "in the small window": NOT the flat screen (no cinematic-quad switch
    during the exit at ~19:19:33). Capture 191956 huddump: the pause menu is a PERSPECTIVE Scaleform movie (VS cb0 rows
    0.14 0 0 0 / 0 -0.13 0 0 / 0 0 1 1 / -1.5e4 -3108 1.97e4 1.97e4) - cut_scale only accepted flat placements
    (m15 == 1), so the wipe was kept in the eye but never enlarged = unscaled in the middle of each eye. cut_scale now
    scales the clip x / y columns (0,4,8,12 / 1,5,9,13), valid for both. Ring lines and the cinematic-quad / blackout
    log lines carry a t<ms> stamp. JJ tests by hand from now on (memory test-plans-one-launch).
  - HIDEWIPES (JJ 22:04: still "small black square, full screen black, then small black square with bats ... get rid of
    that black bloody square"; exit bats "in the same size small square"). Capture 220419 (save select -> title, t-ring):
    1 frame of the untextured wipe piece on the panel, then 1.47 s of it in the eye - the game animates the wipe as a
    square that grows and shrinks, so enlarged it still only fills the view at its peak - then 0.6 s of the 1024x256
    logo halves in the eye before they move to the panel. Now: outside gameplay (menus, title, pause - the gameplay flag
    also counts the pause screen's grey pass) and for wipe-scale pieces (> 1e-3) every piece the wipe logic would keep
    in the eye, and every untextured piece, is NOT drawn (Decision.skip). Panel "hide the bat-wipe transitions in menus
    (no black square)", saved hide_menu_wipes, default on. Possible side effect to watch: a cut piece kept in the eye in
    the pause menu (huddump 191956 #33) is now hidden. Not run on the fake headset (JJ tests by hand).
  - JJ: keep the menu -> game wipe ("works fine"), and "remove it and replace it with the one that we've got going from
    the menu to the game". HIDEWIPES now spares wipes that start from the MAIN MENU (largest piece of the last ordinary UI
    frame >= 1; main menu pieces ~1.9, save select ~1e-4, pause ~0.19). WIPEREPLAY: that main-menu wipe's pieces are
    drawn into a recording target (fixed_blend, premultiplied), laid over the whole view before the eye capture
    (d3d11_hook -> gfx_hud::before_eye_capture, blit::zoom_premul at cut_scale 1.6) and stored as quarter-size frames
    with times (max 360). When a square wipe's pieces are hidden, the stored frames are played over the view at the
    recorded speed (one replay per square wipe). Before the first menu -> game wipe of a session there is nothing
    recorded: the square is just hidden. Built + deployed, not run (JJ tests by hand).
    DISK (JJ: "can't you just save a recording to disk?"): frames are stored as one-channel coverage (R8, quarter size;
    the wipe is black - blit alpha_to_r8 / mask_black) and the first completed recording is written to
    %LOCALAPPDATA%\Batman_City\menu_wipe.bin ("BCWP" v1: w, h, n, times, run-length coverage per frame), read at the
    first present of every session - the replacement works from the start. Delete the file to record a new one.
  - WIPEMAIN (JJ 22:50: pause-menu exit wipe "still inside a box"; save select -> title "a black box ... no bats at all").
    Looked up: this plan's HIDEWIPES / WIPEREPLAY / DISK entries, memory batman-city-hud-overlays (nothing else in the
    fleet replays a wipe). LIVE log 22:44-22:45: the 68-frame main-menu wipe was recorded and saved (menu_wipe.bin
    decoded: bats sweep in from the left, 92 % of the frame covered at the end = good). Then the pause menu's own pieces
    sit at ~1.9 too (huddump 224514 #7), so its exit wipe counted as the main-menu one: shown small AND recorded over
    the good one (54 frames, 22:45:10); a 5-frame after-loading piece was recorded too (22:45:00). The save select ->
    title replay at 22:45:18 then played that small black square. Fix (gfx_hud + scene.cpp): main-menu wipe only while
    the main menu's grunge is on screen and not the pause screen (set_main_menu_now); the mode holds through the wipe
    (400 ms); a recording is kept only if the loading movie starts during it or 1.5 s after (note_movie_started), else
    "wipe recording discarded"; a held recording (disk or kept) is never overwritten; no replay in the 3 s after a
    loading movie. Log line "wipe starts (main menu -> game / square ...)" at each wipe. Built, deployed 22:54 (backup
    backups/batman_city.dll.before-wipemain), not run - JJ tests by hand.
    JJ 22:56 (HEADSET): "mostly better"; pause exit OK. Save select -> title: full-view bats now play, but "still a small
    black box as well". LIVE log 22:55:58: square wipe hidden + replay fired as planned, so the box is something the hide
    rule does not catch (ring 220419 shows only the untextured piece + 1024x256 logo halves in that wipe, both caught).
    WIPEMAIN2: while the replay plays, NO HUD piece is drawn (eye or panel; `bc replayall 0|1`, default on); ring lines
    now show pieces not drawn as h<N>; a capture is saved automatically 0.4 s into each of the first 3 replays of a
    session. If the box survives, it is not a HUD piece: look at the scene Draw(3) title pass (probe at 22:55:58.4
    switched textures exactly at the wipe) / the room screen. Deployed 22:59 (backup before-wipemain2).
    JJ 23:10 (HEADSET): small box still on both. Auto captures 230928 (pause exit) / 230935 (title return): eye images
    = full-view bats, NO box; the box is the PANEL. 230935 panel alpha 84 % opaque black (bbox 0-1432 x 120-1408) =
    the save-select backdrop left over: hidden frames never cleared the panel target. 230928 ring: replay ends at
    ~1.2 s, then ~0.4 s more of the game's wipe (untextured 0x0 s0.14 + six 256x256 bats) went to the panel unhidden.
    WIPEMAIN3: a hidden piece clears the panel once per frame; the replay holds its last (92 % black) frame while
    untextured pieces keep coming (300 ms after the last, max 4 s); log "wipe replay ended after N ms". Deployed 23:12
    (backup before-wipemain3), not run. Lesson: read panel BMP alpha (PIL drops it) before calling a box "in the eye".
    JJ 23:20 (HEADSET): "looks better" (boxes gone). The recording only makes sense where the bats lead INTO black; for
    wipes that ARRIVE at the first menu (game start, second menu -> first) JJ wants "the alpha channel reversed ... black
    bats on the screen that's already there". Recording contact sheet: bats fly in black over clear, merge, end solid
    black. WIPEFLIP: replays that start from no menu / save select (placement < 1e-3, not gameplay) play 1 - coverage
    (blit ps_maskblackinv): black, the menu opens up through bat-shaped gaps, ending as black bats over it; nothing drawn
    in the hold. Pause exit unchanged. `bc wipeflip 0|1`, default on. Built 23:2x, deploys when the game closes (backup
    before-wipeflip). Session start replay still starts ~3 s after the logo movie (the after-loading block) - JJ OK with it.
    JJ 23:23 (HEADSET): flip works, but "still briefly showing a smaller box when the bats first appear". Log 23:20-23:21:
    every wipe flipped + replaced as planned. Ring 232052 (title return): save-select pieces, then from the game wipe's
    FIRST frame all hidden + panel cleared; captures at 0.4 s: panel 0 % opaque, eyes correct -> the box is shorter than
    0.4 s. Suspects: the panel quad is copied at XR submit and may lag the eyes by a frame (old save-select backdrop over
    the black first replay frame); something outside the HUD. WIPESEQ test: for the first 3 replays, eyes at presents
    0/2/4/8/16 + panel for 12 HUD frames -> %LOCALAPPDATA%\Batman_City\wipeseq_<time>. Deployed 23:25 (before-wipeseq).
    JJ 23:27 (HEADSET): the box is BLACK, no menu, no scene, "the same size as what the bat cutscene animation used to
    be". WIPESEQ 232610 / 232618 / 232628: eye pictures (saved from the headset swapchain images = exactly what is
    submitted) uniformly black at t78-172 then bat gaps - no box; first present: left eye still the scene, right black
    (replay starts between the eyes, one frame); all 12 panel pictures 0 % opaque. Present-hook order: replay drawn ->
    eye capture + XR submit (panel copy) -> gfx_hud::on_present, so no panel lag. A black box on a black frame is only
    seen once the gaps open (t > 150 ms), so it lasts longer than a frame - in a layer not recorded, or after 0.4 s.
    WIPEPANEL: the HUD panel layer is not submitted while a replay / hold runs (`bc wipepanel 0|1`, default on).
    WIPESEQ2: the first flipped replay of a session saves eyes + panel every 250 ms from 0.5 to 2.5 s (eyes_tNNNN).
    Deployed 23:31 (before-wipepanel). Live A/B plan: title return with wipepanel 1, then `bc wipepanel 0`, then
    `bc wipeflip 0`.
    JJ 23:34 (HEADSET): box still there with the panel layer off. WIPESEQ2 233247 (session start, left eye every 250 ms
    0.5-2.5 s): black -> bat gaps -> title scene, NO box in any frame; panel 0 % (0.7 % after the replay = title icons).
    So the box is in neither the eye images nor the HUD panel - still unexplained. Asked JJ: centred + moves with the
    head, or room-fixed? one eye or both?
    WIPEHD (JJ: "pixelated ... can it be upscaled?"): recordings now full HUD-capture size (1440x1536, ~1:1 in the eye
    after the 1.6 zoom), one frame per >= 12 ms, max 160 frames (~2.2 MB VRAM each); file v2; a v1 (quarter) file still
    plays until the next main menu -> game wipe records a full-size one and overwrites it (v1 backed up to
    backups/menu_wipe_v1_quarter.bin).
    WIPE PANEL SWITCHES (JJ: "a button within the overlay that I could switch myself"): under "hide the bat-wipe
    transitions": "play the full-view bats in their place" (wipe_replay), "flip the bats when arriving at the first
    menu" (wipe_flip), "hide the floating panel while the bats play" (wipe_panel_off), all saved, default on.
    Deployed 23:36 (before-wipehd).
    JJ 23:43 (HEADSET): none of the switches changed the box. LIVE log 23:39:34 / 23:39:47: replay switched OFF, square
    wipes only hidden - box still seen -> the box is NOT the replay; something of the game's own still shows. Full-size
    recording made 23:38:05 (70 frames 1440x1536, v2 on disk).
    BLACKFILL (JJ: at game start the zoomed moon "should be black. And then the bats come across it and reveal Batman"):
    ring 232042 - after the logo movie the game covers its HUD stage with untextured fills at stage scale (0x0, 1.9e-4,
    ~2.4 s) = full-screen black in 2D, hidden by HIDEWIPES. Now a hidden stage-scale untextured fill clears the whole
    backbuffer (both eyes) black, held while hidden wipe pieces follow within 1 s (max 6 s), until the replay starts.
    Panel "game start: black until the bats reveal Batman" (black_fill). WIPESEQ3: the recorder now starts on every
    square wipe in menus (also with the replay off): eyes at presents 0/4/16 + eyes/panel every 250 ms to 2.5 s, first 3.
    Deployed 23:44 (before-blackfill).
    JJ 23:50 (HEADSET): game start "perfect now" (black, then the bats reveal Batman). Title return: square remains.
    WIPESEQ3 234732 (replay OFF): eyes + panel clean from the wipe's first frame on. Gap check (scratchpad
    hole_check.py: each eye picture vs the inverted zoomed recording at the same time) on 233827: every expected bat gap
    shows the scene to 360 ms - nothing masks the bats in the eyes. Vignette never on; cinematic quad only at the movie.
    JJ (asked): the square shows "before the screen goes black", "fixed in the room", same depth as the bats, "acts like
    a kind of a mask" -> the PANEL before the wipe starts: capture 230935's last panel picture before the wipe = 84 %
    opaque black, no text (ordinary menu panels <= 2 % opaque) = the save-select screen's fade to black on its HUD stage
    (2D: full-screen fade), shown on the panel. MENUFADE: outside gameplay the five FADEHOLD panel points are read back
    every frame (2 frames late); all five dark + covered -> panel not shown, whole view black (BLACKFILL path, +120 ms
    per covered probe), running into the flipped replay. Panel switch "menu fades to black over the whole view (no
    black square)" (menu_fade_black). Log: "menu fade to black on the panel". Deployed 23:55 (before-menufade).
    Watch: a fade before the main menu -> game wipe would now also go full-view black (2D-faithful).
    JJ 00:00 (HEADSET, 2026-10-10): "much better, we are nearly there" (title return fixed). Left: after quitting to the
    menu from gameplay, the menu appears, then a split-second full black, then the menu again. Log 23:58:17 / 23:59:11 /
    23:59:43: ~0.7 s after each pause-exit replay, "menu fade to black on the panel (alpha ~35)" for ~100 ms = the main
    menu's fade-in from black, already 86 % clear when probed, turned fully black. FADEALPHA: MENUFADE darkens the view
    by the fade's own opacity (lowest alpha of the 5 points; 1x1 R8 mask via mask_black inverted); BLACKFILL stays fully
    black; BLACKFILL's "wipe that follows" extension now only follows a BLACKFILL (within 1.2 s), not a menu fade.
    Deployed 00:01 (before-fadealpha), not run.
    JJ 00:05 (HEADSET): no black flash now, but "the menus flashing off briefly" - the panel is hidden while a fade is
    taken over. FADEMIN: MENUFADE takes over only near-opaque fades (all five points alpha >= 200; the save-select fade
    probes at ~255); the faint main-menu fade-in (~35) stays on the panel as before MENUFADE. Deployed (before-fademin).
    JJ 00:13 (HEADSET): now "the faint border of another window that covers maybe 70 % of my field of view" flashes when
    the menu appears = that faint fade-in on the panel (~75 deg panel). MENUFADESKIP: for 2 s after a wipe replay ends,
    stage-scale fill pieces (1.5e-4..1e-3, fill_piece - STAGEBOX notes: the boxes "when the main menu appears after
    leaving the game") are not drawn; the menu's own pieces (~1e-4, ~1.9) stay. Log "menu fade-in piece after a wipe not
    drawn". Deployed 00:15 (before-menufadeskip), not run. If the border stays, its pieces are at another scale: capture
    (HUD ring) right after the menu appears.
    JJ 00:18 (HEADSET): "much better" but "very stuttery" on every exit - thought the bats were re-recorded. LIVE log
    00:17-00:18: bats loaded once from disk (70 frames 1440x1536), never re-recorded; MENUFADESKIP fired (fade-in pieces at
    1.94e-4 skipped, 00:17:50). The stutter = the test recorders: auto capture + frame dumps 0.4 s into each replay and
    the WIPESEQ eye/panel BMPs. WIPEDEBUG: both run only with panel "test: record captures during bat wipes (stutters)"
    (wipe_debug, default OFF). Log text for the main wipe now says "saved copy kept" when nothing is recorded. Test
    output wipeseq_* deleted (~5 GB). Deployed 00:19 (before-wipedebug).
  - TIDY (JJ 2026-10-10: "clear out all other test captures and redundant options now in the VR overlay now that we have
    all the correct settings and fixes"): the HUD drawer's bat-wipe / menu-fade switches are gone from the panel and no
    longer saved - BATCUTS, HIDEWIPES, WIPEREPLAY, WIPEFLIP, WIPEPANEL, BLACKFILL, MENUFADE, FADEHOLD fixed ON, WIPEDEBUG
    fixed OFF; old keys in settings.ini are read and ignored (an old file cannot switch a fix off); `bc` commands remain
    for tests. Data folder %LOCALAPPDATA%\Batman_City cleared of test output (captures 13 GB, panelseq 8.6 GB, 517 frame
    dumps, shader / reticle dumps, pacetrace, perf_smi, xrsim\capture): 22 GB -> 3.5 MB. Kept: settings.ini (+ its small
    backups), menu_wipe.bin (v2 full-size bats), logs, launch.ini, command.txt, xrsim runtime files. Other panel drawers
    (VIEW, HUD placement, MENUS AND SCREENS, DLSS, FRAME RATE, Advanced, Diagnostics) untouched. Deployed 00:21
    (before-tidy).
  - STEAMVR (JJ 2026-10-10: "now we need it to work in Steam VR. Currently ... it's upside down. Refer to Arkham
    Knight's solutions"). Looked up: VR_ESSENTIALS 11 (STAGE space, DXGI 1.1 factory, own launcher, EXITING);
    akvr_native/HANDOFF.md "SteamVR" row + akvr/src/xr.cpp SVRSPACE / POSEJUMP code; AKN launcher
    AKVR-Launch-VR-EngineStereo-SteamVR.bat; SVRSTRAIGHT not needed (Batman City's eyes are off-centre lenses, not
    turned eyes). Found: SteamVR now ships a 32-bit OpenXR runtime (steamxr_win32.json -> bin\vrclient.dll), so the
    old line "SteamVR: no 32-bit OpenXR there" (section 5) and the bundled OpenVR shim are no longer needed. DXGI 1.1
    factory already always on (dlss_bridge install_early); EXITING / LOSS_PENDING already tear the session down.
    SVRSPACE port (openxr_runtime.cpp): LOCAL and STAGE both made; runtime name contains "SteamVR" -> g_space = STAGE,
    else LOCAL (VD unchanged); log "xr: tracking space STAGE|LOCAL"; the recentre event only counts for the space in
    use; the one-frame head-jump re-zero (was VD only) also runs under SteamVR (AKN POSEJUMP). Launcher
    install\Batman City VR (SteamVR).bat (copied beside BatmanAC.exe): XR_RUNTIME_JSON = SteamVR's steamxr_win32.json
    (Steam path from HKCU\Software\Valve\Steam, fallback Program Files (x86)), same ResX/ResY as the test launcher,
    nothing on the PC changed. Dry run: finds c:\program files (x86)\steam\...\steamxr_win32.json, res 2480x2648.
    Deployed 00:26 (before-svrspace), not run. Next if a problem: read Steam\logs\xrclient_BatmanAC.exe.txt first.
    JJ 00:35 (HEADSET, SteamVR via VD's SteamVR driver): "that's working" (right way up) but "the frame rate has tanked
    from up in the 80s to down in the 20s 30s", loading ~2x longer. Looked up: VR_ESSENTIALS 4 + 11 ("its xrWaitFrame
    is a poor pacer: check frame locks there"); AKN NOTES SVR (picture only, no frame-rate finding); playbook 09
    #wait-frame-is-not-a-pacer (b955f224): SteamVR reports a MULTIPLE of the true period when it thinks the caller is
    behind; pacing against the latest value spirals the app down. LIVE log 00:33: beats 27-32 pairs/s from the title
    screen on (VD run 00:22: 85), DLSS 3.6-4.1 ms/eye (VD 2.8-3.0) - not GPU; same 2480x2648; pacetrace: "lastWait 0 ms"
    (SteamVR's xrWaitFrame returns at once) while the present thread sits in "vrBegin" 14-49 ms = OUR pace_sync_gate
    (pair-rate sync, on in Batman City) sleeping to g_displayPeriodNs = the LATEST predictedDisplayPeriod. SteamVR
    compositor: HMD 2284x2448 90 Hz, "legacy frame submission". SVRPACE: g_displayPeriodNs keeps the SMALLEST period
    reported this session (reset at teardown); REPEAT (fps lock) uses it too; log "runtime reported a frame period of
    N ms (shortest M ms)" when SteamVR inflates it. Deployed (before-svrpace), not run. If still slow: the log line shows
    whether SteamVR inflates; next suspects SteamVR Motion Smoothing / per-app throttling, then `vrpace sync off`.
  - JJ 2026-10-10 00:45 (SteamVR runs): three asks.
    NONAME: "Remove all references to the BioShock VR mod ... It says that when I force close the game via Steam using
    Steam VR": OpenXR applicationName / engineName were "bioshock-vr" (SteamVR shows the app name) -> "Batman City VR";
    log texts ("BioshockHD.exe" in value_scan, "BioShock 1 re-routes" in xinput_bridge) and the pad-map name
    "bioshock1" -> "swapped". Built DLL scanned: 0 BioShock strings (ASCII + UTF-16). Kept (developer-only): source
    comments, CMake option names, and the MIT notice files LICENSE-bioshock-vr.txt / THIRD_PARTY_NOTICES (the licence
    requires them in a release; not shown in game).
    SVRGLOW: in SteamVR "the top compass is not there, all I can see is the markers for north, south, east and west":
    additive HUD pieces stay additive on the panel (premultiplied rgb, alpha 0 - fixed_blend 2026-10-03); VD adds that
    colour, SteamVR drops it, so only the "over"-drawn letters remain. Under SteamVR (runtime_is_steamvr) texture()
    returns a copy with alpha = max(alpha, max(r,g,b)) (blit alpha_repair_full, ps_repairfull); VD unchanged.
    FOCUSAUDIO: "no audio ... I have to actually click on the window": KEEP FOCUS rewrites activation messages the
    window gets, but one opening behind SteamVR never gets any, so UE3 keeps its sound down. window::tick: once, 2-30 s
    after the window exists, if the REAL foreground window is not the game's, post WM_ACTIVATEAPP / WM_ACTIVATE /
    WM_SETFOCUS (log "told it it is active"). Needs keep_focus=1 (JJ's settings: 1).
    Deployed 00:49 (before-svrglow), not run.
  - JJ 00:55 (SteamVR): SVRPACE CONFIRMED by the log - SteamVR reported 22-67 ms periods, pacing kept 11.1 ms, beats
    59-68 pairs/s (was 27-32). Sound: "for a very brief period ... then it stops" until the window is focused. Compass:
    "there, but ... masking all around it ... only a very small sliver" as head / camera turn.
    Compass analysis (captures 005356 / 005358): the panel picture SteamVR gets (simulated: alpha = max(a, rgb) over
    grey) holds the compass strip from N (x 391) to a fade at ~900 - NOTHING left of N; an earlier SteamVR capture
    (003952) had the full symmetric strip (150-685, centred on the caret). HUD groups: compass zone 1 identity, but zone
    0 (top left) 0.90 / dy -0.10 and zone 6 (centre) 0.90 / dx +0.10. zone_of() uses a piece's ANCHOR (cb0 row 3), and
    clip masks (write 0x0) can be anchored in another zone than what they clip (huddump 00:53:56 #37 DrawIndexed 6,
    anchor -0.70, 0.75 = zone 0). Hypothesis: a compass clip mask moved by another zone's adjustment -> window
    misplaced. Asked JJ to switch "HUD groups: use the group sizes ..." off once (one click) and to compare with VD.
    FOCUSAUDIO2: posted messages un-muted only briefly - the game also checks the REAL front window off the game thread.
    Now: up to 4 tries (>= 3 s apart, first 40 s) while the window is not really in front: AttachThreadInput to the
    front window's thread, BringWindowToTop + SetForegroundWindow, detach, plus the posted messages; log "brought to
    the front - OK / REFUSED". Deployed 01:00 (before-focusaudio2), not run.
  - JJ 01:05: the compass "only seems to disappear when I'm facing a certain direction ... a big neon sign there ...
    and then its teal glow behind it gets displaced and put above it" -> NOT the HUD groups: the scene's stencil.
    Looked up: VR_ESSENTIALS 6 + akvr_native/NOTES.md HUDDSV (AKN: the HUD sheet needs its own depth/stencil, cleared
    with the sheet, the game's HUD-time depth clears redirected to it). Batman City redirected the HUD into the panel
    picture WITH the game's scene DSV (comment "depth-tested UI keeps its own depth behaviour"); Scaleform's clip masks
    (write 0x0 pieces) test stencil, so the neon sign's scene stencil broke the compass clip (and moved its glow's).
    HUDDSV port (gfx_hud.cpp hud_dsv): own DSV, same size / format / view as the game's (remade if it changes),
    cleared once per frame at the first redirected draw (depth = the game's last full-size scene clear value, stencil
    0); game ClearDepthStencilView calls on that DSV after the UI boundary mirrored to ours (new frame_inspector ->
    gfx_hud::on_clear_dsv hook); used by the panel capture and the wipe recording. `bc huddsv 0|1`, default on. All
    runtimes (VD had the same flaw). Deployed 01:08 (before-huddsv), not run. The HUD-groups test asked of JJ is
    moot.
  - JJ 01:15: "the audio fix doesn't work". LIVE log 01:13-01:14: all 4 FOCUSAUDIO2 tries "REFUSED by Windows
    (SetForegroundWindow false)" - Windows' foreground lock (SteamVR keeps the front; AttachThreadInput did not help).
    The game's sound is Wwise (exe strings "wwmute"/"wwnomute", AK_*); no static DirectSound import. The brief blip
    after the posted activation means the game un-muted, then a front-window check OFF the game thread muted again
    (Wwise / DirectSound focus). FOCUSAUDIO3: gfw_detour now answers GetForegroundWindow = the game window for EVERY
    thread while keep_focus is on (GetActiveWindow / GetFocus stay game-thread only); the mod's own checks use
    g_gfwOrig; the only other in-mod caller is the core's pace-trace "fg" flag (diagnostic). The SetForegroundWindow /
    AttachThreadInput attempts are removed; one posted activation stays. Deployed 01:16 (before-focusaudio3), not run.
    Side note from that log: the game window requested 4096x4096 (launch.ini says 2480x2648) - check where that comes
    from if performance under SteamVR is still low.
  - JJ 01:22: "Compass still disappears when the camera moves over the neon sign" -> HUDDSV was NOT the cause (kept:
    still correct). Captures 012141 (compass complete) / 012149 (looking at the GCR sign: only the centre caret left
    on the panel picture; same 118 HUD draws, e0 everywhere - but marker-path pieces are not counted in "e").
    targets.txt 012149: grapple point ref (-0.049, 0.663) = ON the compass; compass clip mask anchor (0.014, 0.653)
    (huddump #36), 0.06 away < MARKEREYE reach 0.12. RETICLERUN's run (started by a reticle picture, continued by any
    piece within reach) took the compass's mask / pieces drawn after the reticle into the eye -> the compass on the
    panel lost its mask (gone) and its glow went to the marker card ("displaced and put above it").
    MASKSTOP (markers.cpp): a clip mask (all colour writes off) ends any run and is never a marker piece; a run carries
    at most 8 pieces past the last reticle picture. Panel marker line shows "clip masks N". Deployed 01:24
    (before-maskstop), not run. Lesson: a HUD piece missing from the panel picture with "e0" can still be the MARKER
    path - check targets.txt points against the missing piece's anchor.
  - JJ 01:30: compass "there properly" (MASKSTOP CONFIRMED) but "the teal underlay ... still popping out and appearing
    above it". Capture 012916 huddump: marker pieces #0-#12 into the eye, the 13th (#12) a 512x64 fmt 28 MULTIPLY strip
    (blend DEST_COLOR / ZERO) at (0.17, 0.39) right after the reticle; everything else on the panel (compass complete
    there). MASKSTOP2: a multiply piece that is not a reticle picture ends the run. Note: on the panel a multiply piece
    draws nothing (transparent capture, alpha untouched - fixed_blend), so the underlay is now simply absent in VR
    (VD too); if JJ wants it, approximate it on the panel as a dark band (alpha = 1 - texture luminance). Deployed 01:31
    (before-maskstop2), not run.
  - JJ 01:35: "The teal strip is still there and appears when you tilt the camera so that the compass overlays on top
    of the neon sign" (capture 013419). Huddump 01:34:19: #12 (512x64 multiply) still a marker piece - it is a RETICLE
    KEY (0x9174B635), so MASKSTOP2's run rule never applied. The 10 key pictures rendered (BatmanCityData/reticle
    *.rgba): arcs, rings, disc, pointer = reticle; 0x9174B635 = a long teal blob on white (multiply) = the COMPASS
    UNDERLAY (drawn right after the reticle, right before the compass's first clip mask). It joined the key list in
    the RETICLEHD census. On the marker card it tinted whatever was behind it - visible only over the bright neon.
    STRIPKEY: removed from kReticleKeys (its .rgba stays, unused). It now goes to the panel, where multiply draws
    nothing - the underlay is absent in VR (offer JJ a dark-band approximation). Deployed 01:37 (before-stripkey).
    Remaining keys (JJ asked "anything else?"): arcs 02323DAC / BA1AC3BB, rings 4A8A08A5 / A6824695, disc D4366941,
    pointer 39E417FE, RB-prompt backing 09549989 = reticle (all drawn in the 01:29 / 01:34 marker runs); 73B6291B
    (64x512 dark bar) and E49CC051 (64x128 teal bar with cyan corner brackets) never drawn in those runs - unverified
    ("bars" in the RETICLEHD note); offered JJ a per-key usage record before removing them.
  - EXIT WIPE (JJ 01:42: quitting the game from the first menu, "the bats appear again in a transition to black" -
    windowed). Log 01:40:12: MENUFADE fired (opaque panel) 0.4 s before WM_DESTROY, but NO "wipe starts" line - the
    exit wipe made no keep (cut-type) piece, so HIDEWIPES / the replay never engaged and its bats went to the panel.
    Probable shape (from the pause-exit and title-return rings): an untextured square at the first menu's stage
    scale (~5e-5, below the 1.5e-4 cut rule; not a pure-cut frame because bat sprites share it) + 256x256 bat
    sprites. Open: replay must be NORMAL (to black) here but is FLIPPED for second -> first menu, and both start at
    placement ~9.8e-5 - need a discriminator from data. EXITRING: the window proc writes the HUD ring (last ~1500
    frames) to %LOCALAPPDATA%\Batman_City\hud_last_frames_at_exit.txt on WM_CLOSE / WM_DESTROY. Deployed 01:42
    (before-exitring); JJ to quit once from the first menu.
    JJ 01:46: "Teal compass element is fixed" (STRIPKEY CONFIRMED in the headset). Quit run 01:45:18: no ring file -
    the adapter's window proc never saw WM_CLOSE / WM_DESTROY (the core crash guard did: "window teardown noted").
    Same pattern as before: menu fade 01:45:17.894, teardown 18.356, no "wipe starts". EXITRING2: crash.cpp gained
    set_teardown_hook (called once from note_teardown, before the watchdog); window::install registers
    save_exit_ring. Deployed (before-exitring2); JJ to quit once more from the first menu.
    Exit record 01:47:32 (hud_last_frames_at_exit.txt): quit prompt (16-24 draws) opens and closes, then ~1.8 s of only
    the first menu's own pieces (1024x256 logo, 1024x512/71, 4x4/28 at 9.8e-5) with the 4x4 fill count 1 -> 2 = a fade
    fill whose opacity rises on the panel (MENUFADE caught it only at alpha >= 200, 0.46 s before WM_DESTROY). NO bat
    pieces in the HUD - the bats are the menu's 3D background; the "window" is the panel fade. EXITFADE (menu_fade_tick,
    outside gameplay): the five points' lowest alpha rising (>= 4 rising probes, +10 over the run, from > 12, < 250) on
    a near-empty HUD frame (<= 8 draws; save select has 36-47) starts the recorded wipe NORMAL (to black), then holds
    the whole view black (alpha 1) while any fade covers the panel, up to 10 s. Log "menu fading to black ... full-view
    bat wipe to black". Deployed 01:49 (before-exitfade), not run. Watch: false triggers on other first-menu fades.
    JJ 02:12: "The exit from the game still plays the bats in a window. It should be the same full screen ... bats
    ... that plays when you enter gameplay". Log 02:11: EXITFADE never fired (the five probe points stay <= 12 until the
    last 0.1 s - the fade fill is too faint / not under the points; MENUFADE caught it at 255 only 0.46 s before
    WM_DESTROY); no movie, no room screen, no cinematic quad at the exit. Exit record 02:11:37: quit prompt closes
    (t95562 16 -> 6 -> 3 draws), then t95796-t96937 (1.14 s, 166 frames) ONLY "1024x256 logo + 1024x512/71 picture +
    4x4/28 x2" (idle first menu has 4x4 x1) until the window closes; no bat pieces in the HUD. EXITPATTERN: that frame
    pattern (draws <= 4, a 1024x512 fmt 71 piece, exactly two 4x4/28 fills; from the per-frame ring line, on_present)
    starts the recorded wipe NORMAL (start_exit_replay, outside gameplay, one per quit) and exit_hold keeps the whole
    view black (panel hidden) while it lasts, max 10 s. The rising-alpha EXITFADE trigger is removed (exit_hold also
    runs from menu_fade_tick when a fade covers the probes). Checked against the 1500-frame exit record: matches only
    the final 166 frames. Deployed (before-exitpattern), not run.
  - FIRSTPERSON (2026-10-09, JJ: first person "everywhere, but only as an option in the overlay or with a button
    combo. For now just do the camera ... we can do the head part later"; "put the camera in front of his face for
    now"). Sources read first: MELE VR (GPL, guide only) = fixed eye offset + SetOwnerNoSee head hiding; BS Infinite
    ENGINE_NOTES hiding tests; ds3fps; playbook CAM-001. Static RE: LocalPlayer (CalcSceneView this) +0x30 = player
    controller (0x7ED830, GEngine 0x1661800 +0x49C GamePlayers); APawn vtable 0x12AD370, +0x440 GetPawnViewLocation
    0x7A6970 = Location +0x48 + (0,0,EyeHeight +0x2A8); +0x43C GetViewRotation 0x7BA650 -> Controller +0x1CC,
    Rotation +0x54. LIVE: the controller's pawn field = +0x1B0 (found as the pointer whose +0x1CC points back; eye
    height 64 UU; eye ~2 m ahead of the game camera). scene.cpp fp_update / fp_apply: the game camera's view matrix
    keeps its rotation (right stick + head on top), its translation becomes eye point + fp_forward along the camera's
    level forward (head always behind the lens) + fp_up. Fake headset run 30 (tools/tests/firstperson-run30.ps1):
    fwd 25 cm clear view; fwd 0 shows the cowl ear in the top left (so the offset is needed until the head is
    hidden); walking moves with Batman; HUD and head turns fine. Panel: "FIRST PERSON" checkbox at the top + sliders
    (front of face, up/down), saved first_person / fp_forward_cm / fp_up_cm; controller: both sticks clicked twice
    quickly (each < 0.35 s, within 0.6 s; the 0.6 s hold stays the menu) - not testable on the fake headset (real
    pad only). `bc fp on|off`, `bc fp fwd N`, `bc fp up N`. Next: hide the head (SetOwnerNoSee / HideBoneByName via
    ProcessEvent - needs GObjects/GNames), body seen when looking down, cutscene / takedown behaviour.

## 8. Next session starts here
M0/M1 essentially done (JJ: a few minutes of flat play to confirm DX11 gameplay is fine).
DONE on the fake headset (see §10). Was: **first same-frame pair on the fake headset**: double `0x65FD80` from caller `0x6C3710`
only (SEH guard, auto-disarm, menus/loading excluded), shift the eye in the FSceneView ctor
inputs (a14) per pass, second ViewState for eye 2 (find AllocateViewState), and check the
present count doubles (two presents per frame) before wiring the core's eye-tag pairing.
Then mono-tracked head pose in the same seam, then the headset.

**After JJ's first look:** world scale (90 UU/m is a guess; AKVR 1:1 was ~89), HUD/menus
(currently drawn into each eye at screen depth - VR_HUD_GUIDE), render resolution (window capped
by the monitor), second ViewState for eye 2 (AllocateViewState) if temporal effects shimmer,
cutscene policy, per-mode graphics profiles (VRLAUNCH) so flat play is untouched.

## 9. Decisions (JJ, 2026-10-03)
1. **DirectX 11: YES.** Applied 2026-10-03, see `FIX_CHANGES.md`.
2. **Code: BioShock VR (MIT) + our own code.** Mass Effect VR (GPL-3.0) is a guide only: read
   it, don't copy it, so Batman City keeps its own licence choice. Credit BioShock VR in the release.
3. **Camera: third person** (Arkham's own camera, like AKVR). First person maybe later.

## 11. AFW - alternate frame warping as a choice beside native stereo (JJ 2026-10-10)

JJ: "implement AFW as an option alternative to native stereo ... we have done this successfully in Arkham Knight".

Looked up: AKN `akvr_native/NOTES.md` "AFW PLAN 2026-10-10" + AFW1 BUILD / RESULTS (JJ "it works really well"; FEATHER fix
for the edge lines) and `akvr_native/src/afw.cpp` (warp passes, ported here); memory afw-alternate-frame-warping (PureDark
UEVR AFW: one eye drawn per frame, the other synthesised from THIS frame - not AER). Playbook @ b955f224: 19
#composed-reprojection (SnowRunner: depth shift of the fresh eye, holes from the stale eye's own warp = mode 3), 19 DLSS
section ("alternate-eye fights temporal accumulators" -> own history per eye), 14 #velocity-not-temporal (motion blur off is
required; already off here), 14 temporal trap (history per eye, one exposure, deferred results), STR-012 / STR-014 (carry
the eye with the frame, one clock), STR-013 (one eye per frame culls from that eye only: the other eye's outer edge is never
drawn - holes, filled from that eye's previous picture), STR-015 (per-eye history bank), 13 #pair-serial (submit the views
rendered with; both eyes of a pair from one locate). Own code: DLSS_PLAN D1 (per-eye DLSS set, CAMMV previous camera per
eye, depth from `gfx_hud::scene_depth`), §10 STATE1 / right-eye view state, PERCAPTURE, MARKEREYE.

What changes against AK (AKN):
- Depth does NOT need DLSS here: the scene depth (`gfx_hud::scene_depth`, R24) is readable at every eye's present, so AFW
  works with DLSS off too (AKN needed DLSS on).
- No private context state on this device (DLSS D1a): the warp saves / restores the game's compute bindings by hand.
- The core pairs a LEFT present with a RIGHT present (pair hold). An AFW frame is ONE present carrying a whole pair: the
  core acquires both eye images at that present, writes the drawn eye, asks the warp hook for the other eye, tags both with
  the same locate and submits. No pair hold; pair-rate sync then gates every present (one pair each).

Design (live switch, no restart):
- `scene.cpp draw_detour`: AFW on -> no second draw; the one gameplay draw is eye `g_afwNext` (left, right, left ...), the
  right eye keeps its own view state; the label carries the eye + an AFW bit to the render thread (STR-012).
- `ctor_detour`: the drawn eye as now; the other eye's view + lens from the SAME head sample (eye_delta with the other sign,
  its asymmetric angles, no jitter) = the warp's destination; jitter index = frame / 2 so each eye walks the whole sequence.
- `afw.cpp` (new, port of AKN afw.cpp): per-slot cameras; core hook `set_eye_synth_hook`; splat (render size, nearest wins)
  + gather (headset size) + mode 1 / 2 / 3 + FEATHER; hole counts; graphics-card ms per warp.
- Core (`openxr_runtime.cpp`): `sr_set_present_afw`, AFW frames never hold a pair; at the present: drawn eye captured as now
  (DLSS hook included), the other eye's image acquired and filled by the synth hook (fallback: the drawn eye in both =
  flat, never a stale eye), both poses from the frame's own locate, F2 saves both eyes.
- Panel: FRAME RATE drawer "3D: both eyes drawn (native) / one eye drawn, the other warped (AFW)" + "warp from" + status.
  Saved `afw=` / `afw_mode=`. Commands `bc afw on|off|mode N|win N|dbg N|feather N|log N|status`.

Ranked risks (each with its check):
1. See-through things (glass, smoke, steam, rain) and REFLECTIONS (puddles, PERCAPTURE) have no depth of their own: in the
   warped eye they sit on the surface behind them. Check: wet street + neon, AFW vs native.
2. Disocclusion holes around Batman (nearest, largest parallax). Check: `bc afw dbg 1`, mode 3 vs 1.
3. Real / warped alternation per eye (view-dependent shine) -> shimmer. Check: JJ, still head, still camera.
4. Frame rate: a pair per frame needs the game at the headset rate; the gain is GPU only. Check: fps + GPU busy native vs
   AFW at the same render size; with DLSS one helper round trip per frame (not overlap) - check its wait.
5. Eye swap / wrong-eye pose (STR-012). Check: log L/R frames per second equal, eyes not swapped (JJ, close one eye).
6. Exposure / view-state history at half rate per eye (detective mode, glow). Check: detective on / off, brightness per eye.

## TEST PLAN AFW1 (JJ drives in the headset, Virtual Desktop; I trigger through command.txt and read the log)
| # | command / switch | what it proves | how to judge |
|---|---|---|---|
| T0 | `bc afw off` (control), 30 s still, then turn | native baseline | beat line: draws/s, presents/s; `nvidia-smi` GPU busy |
| T1 | `bc afw on` (mode 3) | AFW runs, one eye per frame | log: left = right frames/s, pairs warped = frames/s, 0 flat; JJ: 3D, eyes not swapped |
| T2 | `bc afw dbg 1` 5 s, `bc afw dbg 0` | where the holes are and that they are filled | JJ: magenta behind objects, cyan outer edge; hole % in the log |
| T3 | `bc afw mode 1`, `mode 2`, `mode 3` | which warp looks best | JJ: edges around Batman, outer edge, smearing |
| T4 | head turns, camera spins, glide, run | warp under motion | JJ: no doubling, tearing or lag vs native |
| T5 | neon sign + wet street, detective mode, map, pause | see-through / reflections / mode states | JJ: compare to native (switch `bc afw off` live) |
| T6 | DLSS Performance on (next launch) + AFW | AFW with DLSS | fps, DLSS wait, picture |
| T7 | 5 min soak in AFW | stability | 0 faults, pairs steady |
| T8 | panel switch AFW -> native -> AFW (last) | live switching | picture back each time |
Follow-ups ready: big holes -> `bc afw win 64`; edge lines -> `bc afw feather 200`; warped eye looks shifted -> `bc afw dbg 2`
(tints what came from the previous picture); eyes swapped -> `bc afw log 20` prints drawn / warped eye per frame.

### AFW1 BUILD 2026-10-10 02:38 (STATIC: built + deployed, game closed; not yet run; backup batman_city.dll.before-afw)
- New `src/game/arkhamcity/afw.cpp/.h`: AKN's splat + gather passes (cs_5_0, compiled at run time), modes 1/2/3, FEATHER,
  hole counts every 30 warps, graphics-card ms per warp; compute bindings saved and restored by hand; our copies of the
  headset images use the same format family, viewed as plain UNORM (typed UAV support checked, else flat + logged).
- Core `openxr_runtime.cpp/.h`: `sr_set_present_afw`, `set_eye_synth_hook`, `afw_counts`; an AFW present never holds a
  pair; after the drawn eye's capture the other eye's image is acquired, filled by the hook (fallback: copy of the drawn
  eye), released; both eyes tagged from the same locate (parallel_eye_tag); F2 saves both eyes. Native path unchanged.
- `scene.cpp`: label = gen << 3 | AFW << 2 | eye + 1; draw_detour draws one eye (g_afwNext flips only when a view was built),
  no second draw; ctor: other eye's view + lens from the same head sample, vpJ / vpU / dst from our own matrices, jitter
  index draws / 2, zoom FOV read on every AFW pass; log "[afw] eye L|R camera check" (ours vs the engine's view+0x250, first
  2 passes); beat line every 5 s with the AFW status; panel FRAME RATE: native / AFW + warp from; settings `afw=`,
  `afw_mode=`; commands `bc afw on|off|mode N|win N|dbg N|feather N|log N|status`.
- Works with DLSS off (depth from gfx_hud::scene_depth). With DLSS on, the drawn eye takes the single-eye hand-off (one
  helper round trip per frame, not overlap) - measure in T6.
### AFW1 RESULTS 2026-10-10 02:48-03:03 (JJ, HEADSET + LIVE log, DLSS Balanced 1438x1536 -> 2480x2648)
- LIVE: AFW on 02:49:48-02:50:07 (mode 3): left 44 / right 44-45 frames/s drawn, 87-89 pairs/s warped, 0 flat, 0 failures,
  warp 0.28 ms on the graphics card; holes behind objects ~3 %, outer edge ~23 %, filled from the previous picture ~30 %.
  Camera check: our view x lens == the engine's (difference 0) for both eyes. Native in the same session: 72-79 frames/s
  (DLSS overlap) -> AFW ~ +15 %. JJ has not yet said how it looks.
- JJ 03:0x: (1) panel: "won't let me go any higher than the DLSS shelf. It just keeps looping back to down low";
  (2) "opening the map ... takes quite a long time and only the HUD shows, the actual map doesn't show up" - AFW was OFF
  then (native). Log 03:01:30: map open = "gfx_hud: wipe starts (square: hidden, replayed, ..., gameplay)" + clock
  stopped, no replay line, next wipe 03:01:42 -> the menu-wipe hiding (HIDEWIPES / WIPEREPLAY, 2026-10-09) probably
  takes the map's opening for a square bat wipe and hides it. Not proven: no HUD ring was saved during the map.

Looked up (panel): ImGui 1.92.8 imgui.cpp NavUpdate - D-pad moves the highlight (and scrolls the page to it), the left
stick scrolls the page freely ("Manual scroll with LStick"); SKVR PANELFIT (panel taller than the headset panel - fit +
scroll, not the same symptom); memory akvr-slider-two-writers (two writers on one control). Ranked cause: stick scroll
and highlight-follow fight over the page (JJ's Touch: thumbstick = left stick; D-pad presses also exist via VD).

## TEST PLAN PANELMAP (next launch, JJ drives; built + deployed 03:09, backup batman_city.dll.before-sticknav)
| # | command / switch | what it proves | how to judge |
|---|---|---|---|
| P1 | STICKNAV on (default): open the panel, push the stick up to the top | the stick now moves the highlight, the page follows | JJ reaches the top; log "overlay: pad on the panel ... page scroll N of M" falls to 0 |
| P2 | `bc sticknav 0`, same | the old behaviour comes back = the cause | JJ: loops back down again; log scroll jumps |
| M1 | open the map, panel capture while it is wrong | which HUD pieces were hidden / sent to the eye | hud_last_frames.txt + eye / panel pictures |
| M2 | `bc hidewipes 0`, open the map again | the wipe hiding eats the map | JJ: map shows (bat wipes in menus come back as before) |
| M3 | `bc replayall 0` (with hidewipes 1) | only the replay's "hide all" is at fault | JJ: map shows |
| M4 | `bc afw on`, open the map | AFW with the map | JJ: map in 3D, nothing doubled |
Follow-ups: M2 positive -> tell the map's opening apart from a wipe in gfx_hud (piece census from M1); P1 negative -> log
the highlighted item's position too.
### PANELMAP RESULTS 2026-10-10 03:09-03:11 (JJ, HEADSET; no switches were fired)
- P1 STICKNAV: JJ "the overlay controls are fixed" (HEADSET) - stick moving the highlight cured the page loop-back.
- Map still wrong + "flickering something"; captures 031110 / 031117 (map open). Panel picture: the map's 2D layer
  complete (title, objectives, district names, crosshair lines, button prompts). BOTH eye pictures: black except one
  marker bracket = no 3D city at all. Frame dump q4: only Scaleform work (256x128 filter targets T33/T35, backbuffer T10)
  - no map scene pass in that frame. Log: the letterbox pixel watch flaps ON/off every ~30 ms while the map is up (the
  backbuffer is black except the crosshair lines, top ~710 / bottom ~770 rows black) - that verdict is not used for the
  eyes in projection mode, so it is a symptom (black frame), not the cause. FSceneView dump during the map = the normal
  player camera (loc 5253, 22247, 532), perspective lens, nothing unusual.
- Open: what draws the 3D map normally (another viewport-draw / CalcSceneView caller? a scene capture?), and whether the
  map ever worked in VR (ask JJ).

## TEST PLAN MAP2 (one launch, JJ opens the map and says "ready"; I fire, ~8 s apart, JJ says what changes)
| # | command | what it proves | how to judge |
|---|---|---|---|
| 1 | `bc status` | which draw / CalcSceneView / ctor callers run while the map is up | log "callers" lines vs gameplay |
| 2 | `bc hidewipes 0` | the menu-wipe hiding eats the map | map appears |
| 3 | `bc stereo off` | the doubled draw breaks the map scene | map appears (flat but head-tracked) |
| 4 | `bc cam off` | our camera / lens edit breaks the map camera | map appears (flat, not head-tracked) |
| 5 | `bc fullrect off` (with cam on) | widening the game's 16:9 view rect breaks the map | map appears |
| 6 | restore: `bc cam on`, `bc stereo on`, `bc fullrect on`, `bc hidewipes 1` | back to normal | gameplay as before |
### MAP2 replaced (JJ 2026-10-10 03:15): "Yes, the map used to work. I can't remember if it was only before DLSS ... I
can't exactly look through the headset and type to you in real time." -> no live A/B; the sequence runs inside the mod.
Frame dump q4 (map open) read further: the map's city = Scaleform 3D draws (DrawIndexed up to 13455 indices, 320-byte VS
constants, stack 0x765FAD / 0x76975C) into the backbuffer WITH the scene depth-stencil T12; the scene itself reaches the
backbuffer through a 6-index draw from the HDR target (T26, fmt 10) - no FXAA-shaped pass in that frame. Those big draws
are not in the 03:11:17 huddump. Ranked suspects: (1) HUDDSV (built 2026-10-10 01:08 - the redirected HUD gets its own
depth-stencil cleared to a fixed value; the map's 3D pieces depth-test), (2) NOFXAA copy-back under DLSS (fits "maybe
before DLSS"), (3) HIDEWIPES (the map opening logged as a square wipe), (4) doubled draw, (5) our camera / lens,
(6) 16:9 widening.

## TEST PLAN MAP3 (built + deployed 03:19, backup batman_city.dll.before-maptest; JJ: one button, then just the map)
Panel top: "Map test: press, close this panel, open the map, wait 1 minute" (or `bc maptest`). After 15 s, 7 steps x 6 s,
each suspect off on its own, capture 3 s in, switched back; log "[bc] MAPTEST step N: <name>" / "capture" / "done".
| # | step | proves | judge (captures, by me) |
|---|---|---|---|
| 1 | baseline | the fault is present in this run | panel / eyes without the city |
| 2 | `huddsv 0` | HUDDSV hides the city | city on the panel |
| 3 | `vis nofxaa 0` | NOFXAA copy-back | city appears |
| 4 | `hidewipes 0` | wipe hiding | city appears |
| 5 | `stereo off` | doubled draw | city appears |
| 6 | `cam off` | our camera / lens | city appears |
| 7 | `fullrect off` | 16:9 widening | city appears |
### MAP3 RESULTS 2026-10-10 03:23 (JJ ran it in the headset; captures 032319..032355, measured by me)
| step | eye lit | panel lit | city |
|---|---|---|---|
| 1 baseline | 0.1 % | 6.4 % | no |
| 2 HUDDSV off | 0.1 % | 6.5 % | no |
| 3 NOFXAA off | 0.1 % | 6.4 % | no |
| 4 HIDEWIPES off | 0.1 % | **92.0 %** | **yes** (JJ: "the map appeared halfway through") |
| 5 stereo off | 0.0 % | 6.5 % | no |
| 6 camera off | 0.0 % | 6.5 % | no |
| 7 16:9 widening off | 0.1 % | 6.4 % | no |
Cause: the menu-wipe hiding (2026-10-09). The map draws its city as large untextured Scaleform shapes on the HUD sheet
(frame dump: 1008-13455 indices per draw) with the game clock stopped, and "outside gameplay every untextured piece is a
wipe" hid them; the map opening also started a wipe replay (log 03:01:30) - the "takes a long time". Not DLSS.
MAPSCREEN (gfx_hud classify, built + deployed 03:27, backup before-mapscreen): an untextured indexed piece of >= 2000
indices = the map screen -> no wipe hiding (and any running replay stopped) while such pieces keep coming, 1.5 s hold;
log "gfx_hud: map screen (untextured shape of N indices) - wipe hiding paused"; `bc mapscreen 0|1` (default on).
Unverified: that no bat wipe draws an untextured shape that large (the square wipes are squares, rings and textured
bat sprites; the main menu -> game wipe is untextured but excluded by WIPEMAIN anyway). Check: map + the menu wipes.
### JJ 2026-10-10 03:31: MAPSCREEN "seems like it works"; the map button (= upgrade screen too) "takes quite a long time to
appear", and flickers "like it's not wanting to scale within the size window ... outside the bounds" before it settles.
- LIVE log: every opening from gameplay = "wipe starts (square: hidden, replayed, placement 0.000111, gameplay)" + menu
  fade to black + recorded wipe (1172 ms) + "wipe replay ended after 4000 ms" = the 4 s CAP: the arriving screen's own
  untextured pieces kept extending the hold (WIPEMAIN3) while no HUD piece was drawn. First map opening (03:30:54):
  MAPSCREEN fired 0.45 s in and stopped it; the upgrade-screen openings have no big shapes -> full 4 s.
- Captures 033045 / 033124: the settled WayneTech screen only (flicker not caught; it lasts ~1 s).
- MENUOPEN (built + deployed 03:3x, backup before-menuopen): a replay started from gameplay ends its hold
  `menuhold` ms (default 700) after the recording, not at 4 s; menu-side wipes unchanged. `bc menuhold N`.
- Flicker evidence next time without JJ reporting: I send `bc wipedebug 1` while he plays (auto capture 0.4 s into each
  of the first 3 replays + HUD ring), then compare.
### JJ 03:39: "Quicker to open, but still flickering on occasion" (captures 033839 / 033847)
- LIVE: openings now "wipe replay ended after 1875 ms" (MENUOPEN2 CONFIRMED).
- HUD ring 033847, opening t323296: frame 1 = two untextured pieces at placement 5.3e-5 drawn on the PANEL (a dark box the
  panel's size), frame 2 = the same two sent full-view into the EYE (pure-cut rule, e2), then the wipe hiding takes over
  (all h) for 1.9 s. Same pair at the start of the 033614 opening. Gameplay HUD untextured pieces sit at 2.7e-5.
- OPENFADE (gfx_hud classify, after the wipe-hiding branch so the replay still starts; built + deployed, backup
  before-openfade): in gameplay, untextured pieces at placement 4.8e-5..5.8e-5 are not drawn; log "menu-opening fade piece
  not drawn"; `bc openfade 0|1`. Untested.

## HD SCREENS (JJ 2026-10-10 03:40): "replace all the title screens at launch and all the loading screens [with] higher
resolution AI upscaled versions. We can use my magnific account"
Looked up: game folder - BmGame\Movies\bac_logo.usm = the title logo video, 1280x720 60 fps 14 s (840 frames), CRI USM
with a codec ffmpeg 6 cannot decode; Startup.swf / StartupNV.swf = publisher logos (skipped in VR by default); loading
screens = a Scaleform full-screen movie built from images inside the packages (no .usm opened for them; viewports 128x128
etc.). MoviesStereo\ holds 3D Vision stereo versions of the cutscenes (note for later: true 3D cutscenes in VR).
Own code: markers.cpp RETICLEHD - texture fingerprint by read-back, <game>\BatmanCityData\reticle\<key>.rgba sharp
version swapped at draw = the route for loading-screen images.
Plan (pending JJ's answers): (1) dump switch: every image a loading / title screen draws saved once as PNG named by its
fingerprint; (2) upscale (Magnific); (3) the mod loads <key>.png from BatmanCityData\hd\ and draws it instead.
Open: how Magnific is used (API key vs JJ uploads); the logo VIDEO (840 frames, image upscalers flicker frame to frame,
re-encoding to USM needs CRI tools -> the mod would play its own frames); the upscaled art is WB's - local only, never in
the public repo.
### HD SCREENS build 1 (2026-10-10 ~03:55; built + deployed, backup before-hdscreens; not yet run). JJ chose: API key, loading
screens first (logo video later).
- Magnific API (docs.magnific.com, read 2026-10-10): POST https://api.magnific.com/v1/ai/image-upscaler, header
  x-magnific-api-key, body image (base64), scale_factor 2x|4x|8x|16x, optimized_for (videogame_assets, ...), creativity /
  hdr / resemblance / fractality -10..10, engine; returns task_id + status (CREATED .. COMPLETED / FAILED) + generated[]
  URLs. Separate "precision" endpoint /v1/ai/image-upscaler-precision. Price by output area (EUR 0.10 for 640x480 2x,
  0.40 for 1280x720 4x). Poll path assumed /v1/ai/image-upscaler/<task_id> (not in the excerpt - check on first run).
- Mod: src/game/arkhamcity/screen_hd.cpp - while a full-screen movie is up (loading screens), every picture a draw samples
  (not a render target / dynamic / one-channel; long side 256..4096) is fingerprinted once (RETICLEHD method) and saved
  to %LOCALAPPDATA%\Batman_City\hd_dump\<key>_<w>x<h>.png (WIC); <game>\BatmanCityData\hd\<key>.png (WIC, full mips) is
  bound in its place for the draw (vision draw_indexed_hook / grunge_skip wrappers, restored in the after-draw hooks).
  Log "[hd] loading-screen picture WxH format F: key K, upscaled version LOADED|none"; `bc hd on|off|dump 0|1|status`.
- Tool: tools/hd_upscale.ps1 (-DryRun lists + rough cost; -Limit N): opaque colour to Magnific (videogame_assets,
  creativity 0, resemblance 3, factor up to a 4096 long side), the picture's alpha enlarged bicubic and put back; key from
  the user variable MAGNIFIC_API_KEY, never printed. Results local only (WB art, never in the public repo).
- Risks: pictures whose content changes after first use (font caches) would get a stale key - excluded by format / usage,
  watch the log; per-picture first-use read-back = a small stall once per new picture during loading screens.
### HD SCREENS: Magnific MCP connected (JJ 2026-10-10 ~04:05). Account: Premium, 5,730 credits left this period (216,000
plan). images_upscale cost (simulate_cost, ultra-sublime 4x): 90 / 180 / 270 / 1080 credits by source size tier (S..XL).
Modes: creative (presets subtle/vivid/wild, VideoGameAssets), ultra-sublime (precision, "best in most cases, shines at
4x"), ultra-photo / ultra-denoiser / ultra (2x only). Local files: creations_request_upload (presigned PUT) ->
creations_finalize_upload -> images_upscale -> creations_wait -> creations_get (url). tools/hd_upscale.ps1 gained -Prepare
(opaque copies) and -Finish <key> -From <file> (alpha back, into BatmanCityData\hd). Waiting on the first dump (JJ plays
through loading screens).
### JJ 2026-10-10 17:3x (after installing a high-resolution TEXTURE PACK): grime back on the menu / pause screens; the
glow under "LEVEL UP" sits below the text (capture 173005); logos -> first menu shows the zoomed moon again (should be
black + bat wipe). HD dump: 4 pictures, all 0-byte files.
- GRUNGEPACK: the title grime is now 4096x2048 BC1 sRGB (log census 17:28:51; was matched as exactly 2048x1024) and the
  pause dirt was matched as exactly 512x512. Now by shape: title = full-screen triangle sampling a 2:1 BC1 picture >= 2048
  wide; pause = t0 4x4 + fp16 scene + any square dirt >= 512. Logs "title / pause grime picture WxH format F".
  Root of the moon: the title was not recognised -> "gameplay" at the first menu -> OPENFADE removed the boot transition's
  black pieces (placement ~5e-5) and MENUOPEN shortened its hold. MENUOPEN3: both only after the 2nd full-screen movie
  (logos, then a loading screen).
- GLOWZONE: Scaleform pastes a glow / blur result (small render target) anchored at the stage top-left -> zone 0 (moved /
  shrunk) while its text is in zone 1 (identity). Each paste now takes the zone of the next ordinary piece, learned in
  the previous HUD frame (unknown = not moved). `bc glowzone 0|1`.
- HD dump: WIC PNG encoder refused 32bppRGBA -> BGRA now; failed saves deleted; 0-byte files removed.
- Checked: reticle fingerprints still match under the pack (4a8a08a5, 09549989, d4366941, a6824695 LOADED).
Built + deployed (backup before-grungepack), not yet run.
### HD LOGO (JJ 2026-10-10 17:4x: "the very first batman logo with the game name" + the other logo stills)
- bac_logo.usm = AC_Legal_ExtendedSFX_v7: 1280x720 MPEG-1 60 fps, 841 frames, ADX audio, keyframes listed in VIDEO_SEEKINFO.
  Frames 0-10 title card fading in from black, 11-164 still (Batman Arkham City logo + legal text), 165-312 tech-logos
  card (Unreal, PhysX, Scaleform, IDV, Agora, speedtree, Wwise), 313-840 WB Games / DC Comics / Rocksteady animations.
- tools/usm_rebuild.py rebuilds the container with a new MPEG-1 stream (self-test: byte-identical rebuild; filesize, seek
  offsets, ixsize, minbuf, avbps formulas proven on the original). A 2560x1440 rebuild FROZE the game's player on the
  first logo (JJ: picture stretched, half missing; log: reads stopped after #50) -> the 2011 CRI / Scaleform player only
  handles the original size. Original restored (hash 148293147028D5CC).
- Magnific (MCP, ultra-sublime 2x, grain 0; 90 credits each = 180): title + tech cards at 2560x1440. Title: the logo art
  gains real crack detail, but the small text was altered (TM under the bat -> "DA", (c) -> "@", a TM -> (R)) -> final
  title = Magnific with the legal block (y 1030-1370) and the TM (1255,700 90x50) from a lanczos enlarge. Tech card clean.
  Files: <game>\BatmanCityData\hd\logo_title.png / logo_tech.png.
- screen_hd logo_frame (gfx_hud before-capture hook): while the movie plays and the last opened .usm is bac_logo, the
  title card is painted over frames 11-161 and the tech card over 168-308 (60 fps from the movie start, 16:9 band,
  screen_in_eye blit; flat and VR). `bc hd logo 0|1`, `bc hd logooffset N` (ms). Built + deployed (backup
  before-hdlogo), not yet run. Risks: start-time offset at the cuts; band placement if the stage is not 16:9 centred.- JJ 18:12: "they look better but you can see them rescaling when they appear" (cards loaded 18:11:53.24 = ~frame 17 of the
  movie; painted over the guessed 16:9 band). LOGOFIT (built + deployed, backup before-logofit): at the first title-card
  frame the backbuffer is read back once, the bright content's box (luma > 60) is compared with the same box in the
  1280x720 source (253,73)-(1044,678) -> the video's real rect on screen, used for both cards; log "[hd] logo video
  measured on screen: ... (used)"; falls back to the band if the x / y scales disagree by > 6 %.
### SYNOPSIS SLIDESHOW (JJ 2026-10-10 18:2x-18:4x): the loading-screen slideshow in HD
- Source: LoadingMovie.upk runs RSeqAct_LoadSynopsisTextures; the pictures are CookedPCConsole\Synopsis<N>_SF.upk (87
  packages: 0-66, 100-114, 150-153, 999), one Texture2D "Image" each, 1024x512. Exported with UModel 2023-07
  (D:\Tools\umodel, gildor.org; `umodel_64 -path=<CookedPCConsole> -export -png -out=<dir> Synopsis<N>_SF`).
- Magnific (MCP, precision ultra-sublime 2x, grain 0, sharpness 0 - JJ chose 0 over the default 7, which aliased
  silhouettes; my local edge-soften was "too soft"): 4 pictures per request (2112x1088 tile, 16 px padding) = 180
  credits per 4 (S tier would be 90 per single picture). Black padding darkened the outer 8 px by ~5 % (tile 0, its 4
  pictures feathered to a lanczos enlarge over 12 px); edge-smear padding for the other 21 tiles. Total ~4,320 credits.
- Output: <game>\BatmanCityData\hd\synopsis\<SynopsisN>.png (2048x1024, 87 files, 213 MB) + synopsis_index.txt
  (32x16 mean-luma grid per source picture, Y = .299R+.587G+.114B; closest two pictures differ by 14.6 per block).
- Mod (screen_hd entry_for): a 1024x512 loading-screen picture -> same grid from the read-back -> nearest index entry,
  mean difference < 5 -> its HD png bound instead (else the old fingerprint route). Log "[hd] slideshow picture:
  nearest SynopsisN, mean difference D -> HD version | no match" and "slideshow index: 87 pictures". Built + deployed
  (backup before-synopsis), not yet run.
### RELEASE 0.1 (2026-10-10, JJ: "package this game up now and write up a Github page")
- Repo https://github.com/junglejuz/Batman-Arkham-City-VR (public, JJ). Release v0.1.0, asset BatmanCityVR-ArkhamCity.zip
  (291 MB, 115 files; downloaded back without login, sha256 identical). Built by tools/make_package.py from the game
  folder (tested DLLs, DLSS helper + nvngx_dlss.dll, BatmanCityData HD art - JJ chose to ship it) + JJ live settings.ini.
- install/Install-BatmanCityVR.ps1 + Uninstall-BatmanCityVR.ps1 (+ .bat, CRLF): Steam lookup, Documents lookup, first-install
  backup in <game>\BatmanCityVR_backup, config edits (every copy of a [section]), shortcuts (desktop + Start menu, SteamVR
  launcher, uninstall). Practice test (scratchpad): install, second install (backup kept), uninstall - all three config
  files byte-identical to the originals.
- Open for 0.2: VR-only launch (the mod also runs in a plain Steam launch), 2D/VR settings profiles (the config changes
  apply to flat play too), test tools behind one switch (frame dumps on capture), the logo-card fit and slideshow not yet
  seen in the headset.

### PANELTIDY (2026-10-10, JJ: "did we remove all active redundant tests and clean everything up in the overlay?" - no)
- One switch: the panel's last checkbox "show test options" (overlay::show_tests, not saved, off at every start). Hidden
  behind it: Map test button; VIEW right / left eye view history, particles turn with the game camera, each eye shaped
  for its lens; MENUS room-screen checkbox; HUD marker reach / turn-with-camera / frame lag / aim-depth near limit and
  their counters; glow depth-of-field checkbox, hidden group, beam / group / combine counters; DLSS sub-pixel shake and
  hand-off sync; FRAME RATE exact-pose / pose-delay block; vision game-view text; pause dirt spread + dirt screen
  sliders; puddle reflections per eye; the Advanced and Diagnostics drawers. Captures arm the 3 MB frame / view dumps
  only with test options on. Panel title "Batman City VR". Built + deployed (backup before-paneltidy); not in the
  GitHub release yet (0.1.1 after JJ looks).
## TEST PLAN LOADFIX (JJ 2026-10-11 ~01:15: loading screens into the game show the logo cards first; the slideshow does not
look higher resolution; the glow under "LEVEL UP" still sits under it; panel a bit less transparent; 3D options named
"Native" / "AFW" with the description in brackets)
Looked up: own notes HD SCREENS / HD LOGO / LOGOFIT / SYNOPSIS / GLOWZONE (this file); log 01:10 (LIVE); capture 173005 +
framedump 173004_q0 (LIVE); playbook @ b955f224 symptom-index / failure-atlas / pattern-catalog: nothing on loading-screen
resolution or HUD grouping; VR_HUD_GUIDE: only SKVR FULLSCREEN (grouped items), not this.
Causes (LIVE evidence):
1. Logo cards over loading screens: logo_frame ran while "the last .usm opened was bac_logo" - loading screens open no
   .usm, so it stayed true all session (log 01:10:39.554: "logo video measured on screen" on the loading screen, shapes
   disagree). Fix LOGOONCE: the flag is cleared when the logo playback ends (window::forget_logo_video).
2. Slideshow not sharper: the HD picture IS bound (log: Synopsis9/8/10 LOADED), but the game sets a 1024x512 viewport
   (game stack EED08D/F025A2) right before each picture's first draw and 256x128 before the 256x128 picture = it copies
   each picture into a render target of its own size and the loading screen shows that target -> our 2048x1024 went
   through a 1024x512 copy. Ceiling after that: the game window is 1438x1536 with DLSS Balanced; the room screen
   (2.4 m at 1.75 m, ~69 deg) gets ~1450 headset pixels across - the same, so the window is not the limit.
   Fix HDFOLLOW: a render target of the picture's size last drawn from an HD picture gets the HD picture wherever it is
   sampled (table cleared when the movie ends; a picture without HD drawn into it drops it). HD textures from load_named
   now have full mips (2048 shown ~1400 wide).
3. LEVEL UP glow: capture 173005 - the glow's place = the text moved by group 0 (top left: 0.9x toward the corner, 0.1
   down; JJ's hud_zone_0=0.90,0.00,-0.10), text in group 1 (identity): predicted -0.100,0.501 vs measured -0.106,0.505.
   The glow is NOT a render-target paste (no render-target texture on the HUD sheet in framedump 173004) - GLOWZONE never
   fired. Its anchor lies in the top-left zone, its picture under the text. Vertex constants floats 16..31 = the texture
   mapping (1024x256 picture: v scale 0.0002 = 1/(256*20)). Fix PICZONE: glowzone mode 2 (default) - a picture piece
   (>= 64x16) goes to the zone of its picture's middle when the next piece agrees; mode 3 the middle alone; mode 4 a piece
   unlike both neighbours takes the next piece's zone. Log "gfx_hud: picture piece ... -> MOVED / left" once per kind.
4. Panel: background 0.86 -> 0.96; radio buttons "Native (both eyes drawn every frame)" / "AFW (one eye drawn per frame,
   the other warped)".

| # | command / switch | what it proves | how to judge |
|---|---|---|---|
| T1 | main menu -> Continue (loading screen) | LOGOONCE | log "[hd] logo video ended" after the logos; no "logo video measured" during the loading screen; JJ: no logo cards |
| T2 | same loading screen | HDFOLLOW fired | log "HD picture copied by the game into a 1024x512 render target"; `bc hd status`: follow swaps > 0; JJ: sharper |
| T3 | `bc hd follow 0` / `1` on the next slideshow picture | the A/B | capture both; edge detail of the picture |
| T4 | gameplay with LEVEL UP on screen | PICZONE | log "picture piece ... MOVED" for the glow; JJ: glow under the text, nothing else moved |
| T5 | `bc glowzone 1` / `2` | A/B against the old rule | JJ / panel capture: glow jumps back down-left at 1 |
| T6 | panel: transparency + labels | JJ | readable |
Follow-ups: T2 no "copied" line -> the copy is not a plain draw (check the viewport-set stack EED08D); T4 glow still
wrong + log shows its middle in group 1 but next piece elsewhere -> `bc glowzone 3`; no picture line at all -> `bc
glowzone 4`, then `bc huddump` while LEVEL UP is shown; health / other widgets moved -> mode 1 and read the log lines.

### LOADFIX BUILD 2026-10-11 01:27 (STATIC: built clean + deployed; backup backups\batman_city.dll.before-hdfollow; not run)
Files: game/arkhamcity/screen_hd.cpp (LOGOONCE, HDFOLLOW, mips, `bc hd follow 0|1`, status line), window.cpp/.h
(forget_logo_video), core/gfx/gfx_hud.cpp/.h (PICZONE, set_glow_zone(int)), scene.cpp (`bc glowzone N`),
core/ui/overlay.cpp (bg 0.96), afw.cpp (labels).

### LOADFIX RESULTS 2026-10-11 01:29-01:33 (JJ HEADSET + LIVE log)
- T1 PASS (JJ): no logo cards on the loading screens; log "[hd] logo video ended" 01:29:26.
- T2 FAIL: JJ "still don't look that much better"; log has NO "copied into a render target" line - HDFOLLOW never fired
  (the picture's draw target is not a same-size texture with the shader bind flag, or the movie samples a copy of it).
- Loading time (JJ: "taking a bit longer, three screens instead of two"): LIVE both runs today show the same three pictures
  (Synopsis 9, 8, 10; the third at +6 s): 01:10 run 21.3 s, 01:29 run 19.7 s (after the change). Not the mod; the pictures
  change every ~6 s, so a load over ~12 s shows a third one.
- T4 FAIL (JJ): glow still displaced; fixed only with the HUD group moves off. Log: no "picture piece" line for it.
  framedump 173004_q0 re-read with each piece's anchor: the LEVEL UP bar is all group 1 except ONE piece - a Draw of 42
  points (4x4 solid texture), anchor -0.543,0.600 = group 0, drawn between the chevrons (03461, group 1) and the letters
  (03467, group 1). Not a picture piece -> PICZONE could not see it.
- huddump 01:32:46 caught a 17-piece frame (menu / pause, no LEVEL UP).
### LOADFIX2 BUILD 2026-10-11 01:36 (STATIC: built clean + deployed; backup backupsatman_city.dll.before-sandwich)
- SANDWICH (glowzone mode 2, default): a piece between two pieces of one other group takes that group. Replayed on
  framedump 173004_q0 (99 pieces): exactly one piece moves - 03464 group 0 -> 1 (the glow).
- HDFOLLOW2: the picture's draw target is followed whatever its bind flags; frame_inspector copy tap (CopyResource /
  CopySubresourceRegion by the game) carries the HD picture to the copy's destination. Log "[hd] picture WxH (...) drawn
  into ..." for the first 12 picture draws, "the game copied the HD picture's target ... - followed".
- LOADAB: automatic A/B on the first 2 slideshow pictures per loading screen (6 per session): 1.5 s after the HD picture is
  first used the window is saved as shown (<n>_hd.bmp), then one frame with the game's own picture (<n>_game.bmp) ->
  %LOCALAPPDATA%\Batman_City\captures\loadab_<time>\. `bc hd ab 0|1`. Judge offline: if the two are identical, the HD
  picture never reaches the screen; if they differ, compare detail.
- Ceiling note: the room screen gets ~1450 headset pixels across and the game window is 1438 wide, so a 1024 -> 2048
  picture can gain at most ~1.4x in the headset even when it reaches the screen.

### LOADFIX2 RESULTS 2026-10-11 01:38 (JJ HEADSET + LIVE log + A/B captures loadab_013831)
- SANDWICH PASS (JJ: "Level up is fixed"). Log showed a few one-off regroupings during HUD fades (a LEVEL UP letter to
  group 0 once): the draw order shifts while a widget appears, so "the piece after" from last frame was another piece.
  ALIGNED (built 01:40, deployed, backup before-aligned): the next piece is only used when last frame's pieces k-1 and k
  match this frame's.
- HD loading pictures: the picture is drawn STRAIGHT into the game window (1438x1536 format 28 bind 0x20) - no copy;
  the 1024x512 viewport seen earlier is something else. HDFOLLOW / HDFOLLOW2 were never needed (follow targets 0).
  The swap worked in every run tonight (2968 swaps by A/B 2). A/B (same frame, HD vs game's own): edge detail inside the
  picture 6.05 vs 4.13 and 6.75 vs 4.69 = ~46 % more; close-up visibly crisper (suit pattern, grille). Limit is the
  headset: the room screen gets ~1450 px across and the picture fills ~1250 px of the 1438 window, then the video stream.
  LOADAB now off by default (`bc hd ab 1`).
- Slideshow timing: Synopsis10 arrives 6.4 s after the first picture in all three runs tonight (01:10, 01:29, 01:38),
  loads 21.3 / 19.7 / 19.7 s -> three pictures. The mod does not touch the slideshow timing; no vanilla baseline measured.

## TEST PLAN LOADTIME (JJ 2026-10-11 01:4x: "still seeing three loading screens ... maybe the screens are just showing for
less time than the original" - "worth a test")
Looked up: LOADFIX2 RESULTS above (HD-on loads tonight 21.3 / 19.7 / 19.7 s, picture change every 6.4 s); memory
ak-first-run-stutter-hdd (game drives on E: = disk cache makes repeat loads faster - keep both loads warm).
One launch, same save, both loads warm (the game ran 3 times tonight). Measured from the log: "full-screen movie STARTED"
-> "ended" (load length) and the time between "slideshow picture" lines (picture length).
| # | command / switch | what it proves | how to judge |
|---|---|---|---|
| L1 | at the main menu: `bc hd off` + `bc hd dump 0`, then Continue | load + picture timing with the mod's HD work fully off (no swaps, no read-backs) | log times vs tonight's 19.7 s / 6.4 s |
| L2 | pause -> quit to main menu, `bc hd on` + `bc hd dump 1`, Continue | control in the same launch | same numbers = the HD pictures change nothing |
Trigger check: `bc hd status` after each switch (swaps count frozen in L1, rising in L2).
Follow-up: L1 clearly shorter -> per-picture first-use cost (PNG decode 30-100 ms, fingerprint read-back) is the suspect:
time each step in the log; equal -> the game's own pacing (no change needed).
### LOADTIME BUILD 2026-10-11 01:45 (JJ: "just make a button for me"; built clean + deployed, backup before-loadtime)
- Panel MENUS AND SCREENS: checkbox "HD loading pictures (AI-upscaled)" (off = no swaps and no read-backs; saved as
  hd_screens=) and under it the last 6 loading-screen lengths with HD on / off (logo video excluded). Log "[hd] loading
  screen took N s (HD pictures on|off)". JJ runs L1 / L2 himself from the panel; the command steps above are not needed.
### LOADTIME RESULTS 2026-10-11 ~01:50 (JJ HEADSET, panel switch): HD on and off took the SAME time -> the HD pictures
are not the cause. JJ: two loading pictures "up until today", three since. STATIC: JJ's high-resolution texture pack
rewrote BmGame\CookedPCConsole\*.upk + CompressionNoAlpha.tfc (860 MB) on 2026-10-10 17:19, the same day - more texture
data read from disk per load = longer loads = a third slideshow picture (6.4 s each). Not the mod; nothing to fix here.

## TEST PLAN HEADCAM (JJ 2026-10-11 ~01:50: first person - "we need the camera attached to the head ... when flying the head
comes forward and it goes through the camera ... attached at a fixed distance from the head"; and "when you move your
head ... he would also move his body a little bit ... it would work both ways")
Looked up: playbook @ b955f224 CAM-001 (head pose once), 12-torso-calculations "Body-heading evidence ladder" (HMD yaw is
not body yaw; thresholded, filtered latent heading), 01-camera-and-tracking (axis-follow body-follow disabled after
oscillation / body lag), FAIL-HAND-047 / FAIL-HAND-002 (bone writes from a camera hook are overwritten by the skeleton
evaluation), CAM-013 / FAIL-CAM-020 (head animation vs body); vrframework guide 13 -> REFramework FirstPerson.cpp (head
joint + smoothing, RotateBody); MELE VR ME2 convo_fp.cpp (head bone only inside a box around the steady eye point, EMA
smoothing, "a head bone bobs"), ME1 head_aim / SetOwnerNoSee + HideBoneByName; BS Infinite VR bones.h / hide.cpp (UE3
32-bit: SpaceBases = 32-byte FBoneAtoms, SetOwnerNoSee hides, HideBoneByName does not cascade); own memory
batman-city-first-person (steady eye point chosen 2026-10-09 because head-bone cameras bob - JJ now asks for the head).
Static RE (re/BatmanAC.exe.unpacked.exe, ac.py): USkeletalMeshComponent::GetBoneLocation 0x850EE0 (thiscall out, FName,
space; ret 0x10; zero vector if the bone is missing); hard-coded FName 0x620 = "Bip01_Head" (0x61F Bip01_Neck1, name
table 0x47C5C1); the game calls it on pawn +0x36C (0xD505B7) = Pawn.Mesh; SkeletalMeshComponent vtable 0x13158C8,
size 0x6C0, SkeletalMesh +0x1A0. Natives also found: execGetBoneMatrix 0x84EF50, execHideBoneByName 0x84C770,
execSetOwnerNoSee 0x58F3E0, execGetSocketWorldLocationAndRotation 0x84FB30.
Stage 1 HEADCAM (built): eye point = steady eye point + smoothed (head bone - steady eye point), capped at 140 UU per
axis (outside = steady eye point, logged); smoothing in real seconds (both eye passes of a frame get the same point);
fp_forward / fp_up still apply on top. Panel FIRST PERSON: "camera follows his head (gliding, animations)" (default on)
+ "head smoothing (s)" (default 0.08) + head-frame / fallback counters. Settings fp_head / fp_head_smooth;
`bc fp head 0|1`, `bc fp smooth N`.
| # | switch | what it proves | how to judge |
|---|---|---|---|
| H1 | first person on, stand still | the bone is found | log "[bc] first person head: mesh ... vtable 13158C8? Bip01_Head - eye point = ..." (~0 to 15 UU); panel fallbacks 0 |
| H2 | glide / dive | the camera moves with the head | JJ: head no longer passes through the view |
| H3 | run, fight | bob | JJ: steady enough? raise "head smoothing" if bouncy |
| H4 | "camera follows his head" off / on | A/B in the panel | JJ |
Follow-ups: vtable not 13158C8 but the bone found = a subclass (fine); "no Bip01_Head" = the bone name differs -> list
the mesh's bones (execGetBoneNames 0x858950); camera too high / low or inside the cowl -> the up / forward sliders.
Stage 2 BODYFOLLOW (plan only, after H1-H4): the body / head of Batman following the player's head. The playbook warns
(body-follow disabled after oscillation and body lag; bone writes must happen inside the skeleton update). Candidates:
(a) Batman's facing turns toward the headset's heading when it stays past a dead zone (thresholded, filtered - the
evidence-ladder rule), standing still only; (b) a small spine / neck lean toward the head's sideways offset, written
inside the skeleton update (hook after the animation pose, before SpaceBases are used) - needs the SpaceBases layout
(BS Infinite: 32-byte atoms; check live). Also open: hiding his head (SetOwnerNoSee 0x58F3E0 needs a ProcessEvent call or
the bOwnerNoSee bit).
### HEADCAM BUILD 2026-10-11 01:56 (STATIC: built clean + deployed; backup backups\batman_city.dll.before-headcam; not run)
### STEREOSECTION 2026-10-11 02:18 (JJ: copy Arkham Knight's own STEREO RENDERING section; built + deployed, backup
before-stereosection): panel section STEREO RENDERING (open the first time, above VIEW) holds 3D rendering Native / AFW +
warp from + status, as akvr/src/hooks.cpp; removed from FRAME RATE. Not seen by JJ yet.
### PACKAGE 0.1.1 2026-10-11 02:2x (JJ: "package up this new build"; NOT published)
- dist/BatmanCityVR-ArkhamCity.zip (115 files, 290.8 MB, sha256 84fbebfac09e15fb...); 0.1 kept as
  dist/BatmanCityVR-ArkhamCity-v0.1.0.zip. Against 0.1 only batman_city.dll and files/settings.ini differ; testzip clean,
  no backslash entries, Windows tar lists it.
- Settings = JJ's live file except first_person=0 (make_package SHIP_OVERRIDES: new players start in third person);
  new keys hd_screens=1, fp_head=1, fp_head_smooth=0.08.
- DLL: loading-picture saver (hd dump) now OFF by default (was on = a test tool writing PNGs for every player); the panel
  switch no longer turns it on. Contains HEADCAM + STEREOSECTION, which JJ has not run yet (build 02:20).
- PACKAGING_GUIDE section 9: unrun builds go into a release only for JJ's own test - the repo is PUBLIC, so publishing
  waits for one launch (HEADCAM H1-H4, panel STEREO RENDERING) or JJ's explicit go.
### FPGATE 2026-10-11 02:25 (JJ 02:24: "no 3D geometry is showing on the main menu anymore"; "first person must always be
disabled when going into main menu ... or any other loading screen and menus"). LIVE log 02:23: first person on (JJ's
setting) took the main menu's stand-in pawn (eye -3632, 864, -80; no mesh) and moved the camera away from the menu scene
- an old first-person bug, not HEADCAM. Fix: first person applies only in gameplay - not on the title screen / main menu
(room_overlay::title_screen_now), movies / loading screens, menus (game clock stopped or the pause screen), or while the
game camera is more than 12 m from his eyes. Log "[bc] first person paused: <why>" / "active again". The setting stays
on: gameplay comes back in first person. Note: the clock signal can lag the menu by seconds (PAUSEHALO run 32) - map /
upgrade screens may start in first person for a moment. Built + deployed (backup before-fpgate); package rebuilt.
### MENUBATMAN 2026-10-11 02:3x (JJ: after quitting to the main menu "the HUD elements appear, and you can see the moon, but
it takes ages for Batman to then come into view"). LIVE log 02:28:53-57: quit, bat wipe replay, menu up at 57.4; first
person already off (02:28:46, not FPGATE); MENUFADESKIP only drops the faint (~35 alpha) menu fade-in for 2 s - not a
cover. Suspect: the main-menu scene streams Batman in after the menu shows, slower since JJ's texture pack (10-10 17:19;
no such report in the many exit tests before it). DISKREAD (built + deployed 02:31, backup before-diskread, not in the
package): 1 Hz log "[bc] disk: game reading N MB/s (loading for S s so far)" while > 20 MB/s, then "finished reading
after S s". Test: quit to the main menu once, JJ says roughly when Batman appears; reading run ~ that long = loading
(texture pack); no reading = something else (then capture right after the menu appears).
- LOADHOLD (JJ: "if it's a loading issue ... maybe it should pause longer at the black screen after the bat wipe";
  built + deployed 03:14, backup before-loadhold, not run, not in the package): gfx_hud samples the game's disk reads
  every 0.1 s during a wipe replay; at the replay's end, a replay that ends on black (not flipped) keeps its black last
  frame while reading > 20 MB/s over the last 0.5 s, max 20 s. No reading = the wipe ends exactly as before (menu-open
  wipes unaffected). Log "LOADHOLD: the game is loading (N MB/s) - black held" / "black held N ms ... (N MB read)".
  Command: bc wipehold 0|1. Judge: quit to the main menu - black until Batman is there, then the menu appears with him.
- LOADHOLD RESULT 03:15 (JJ HEADSET + LIVE log): quit to the main menu -> "LOADHOLD: the game is loading (126 MB/s)" ->
  "black held 1375 ms (40 MB read)" -> menu. Reading went on at 21-25 MB/s for ~4 s more (0.5 s window dipped under 20).
  JJ: works (bats, black, wait, menu) but the menu "just appears" - wants the bat transition into the menu.
- WIPEREVEAL (built + deployed 03:18, backup before-wipereveal, not run): a replay that ended on black in the menus (not
  opened from gameplay, not quitting the game, recording > 8 frames) plays again FLIPPED (black bats clearing off the
  arriving screen, as after the logos); HUD pieces stay hidden while it plays. LOADHOLD now ends below 12 MB/s (starts
  above 20). Log "WIPEREVEAL: black -> bats clearing off the arriving screen". Command: bc wipereveal 0|1.
  Watch: other menu-to-menu wipes that end on black now also reveal (save select -> title).
- SCREEN25 2026-10-11 03:58 (JJ: "move the loading screens a bit further away"): room screen 1.75 -> 2.50 m, size kept
  2.4 m wide (now ~51 deg instead of ~69 deg). JJ's settings.ini edited (backup settings.ini.before-screen25) + code
  default g_screenDistM 2.5 (built + deployed, backup before-screen25). Same screen = logos and movies too. Panel MENUS
  AND SCREENS sliders still adjust both.

## TEST PLAN MENUARRIVE (JJ 2026-10-11 04:0x: the reveal after quitting "sometimes arrives at Batman, and sometimes it
arrives before Batman. Can we time it better so that it syncs up with his arrival")
Looked up: LOADHOLD / WIPEREVEAL above (disk reading = proxy only: log 04:01 hold ended 42.2 s, reading in bursts to 46.8 s);
framedump 173004 vs constant-buffer sizes (scene draws 160/320/640 bytes - no bone-palette signature visible, so no
'skinned draw' marker from old data); memory ak-first-run-stutter-hdd (load times vary with the disk cache - explains
'sometimes').
Causes: (1) Batman's mesh / textures stream in after the frontend map is up, later than the disk proxy says; (2) the
menu's own intro (Batman placed / faded in by the frontend script) at a fixed time after the menu opens.
MENUTRACE (built + deployed 04:04, backup before-menutrace, test build only): first 3 wipes to black in the menus per
session, 14 s each: [trace] lines every 0.1 s (draw calls per present min / avg / max, disk MB/s, black / REVEAL / menu
shown, LOADHOLD) + the game's own frame every 0.5 s (under our black) -> captures\menutrace_<time>\<n>_<ms>.bmp.
| # | step | what it proves | how to judge |
|---|---|---|---|
| A1 | quit to the main menu 3 times (pause -> quit) | when Batman appears vs draws / disk | first frame with Batman; the draw-count step at that moment; disk then |
| A2 | JJ: each exit, early or on time | ties the frames to what he saw | |
Next: draw count steps up when Batman appears -> release the hold on that step (with the disk rule as fallback and the
20 s cap); no step -> look at the frames for another sign (e.g. a picture change in the region where he stands).
### MENUARRIVE RESULTS 2026-10-11 04:06-04:07 (LIVE trace + frames, 3 quits) and fix
- Draw calls per present: wipe (old level) 755-820; frontend's first shot (moon close-up) ~95 from ~1.5 s; Batman's
  shot (gargoyle, bats, city) ~290 from +4.9 / +4.2 / +4.0 s (frames 2_04000 moon, 2_04500 Batman). The disk hold had
  released at 3.1 / 2.1 / 2.0 s = reveal before Batman each time. Drop to ~12 at +7-8 s = JJ pressing Continue.
- Fix (built + deployed 04:09, backup before-menuarrive): LOADHOLD now holds until Batman's shot: B = most draws during
  the wipe, low phase < 0.5 B (avg L), release after 3 presents >= max(1.8 L, L + 80); fallbacks: no low phase + disk quiet
  1 s; 15 s cap. Replayed on the 3 traces: releases at 4938 / 4312 / 4000 ms = the jump each time. MENUTRACE default off;
  its 716 MB of frames deleted.
- JJ 04:11 (HEADSET): reveal "a tiny bit too late", and no bat sound with it ("there should be"). LIVE log 04:11:37: the first
  draw sample after a gap counted every draw since the last replay ("923821 during the wipe") -> low-phase average 162 ->
  Batman's shot (~290) was under the 1.8 x threshold; released only at 353 draws = late.
- MENUARRIVE2 + WIPESOUND (built + deployed 04:15, backup before-wipesound, not run): draw samples > 200 ms apart count 0.
  Sound: URGFxMovie execPlaySound 0xC29A70 -> vtable +0x1B8 = 0xE8F090 (all RGFxMovie classes; thiscall const FString&,
  ret 4; GEngine -> 0x689A10 -> vtable +0x198), prologue 8B 0D 00 18 66 01 E8 checked on the unpacked exe. Hooked: logs
  every menu sound name (first 60), learns the one played within 0.7 s before / 0.4 s after a wipe to black starts
  (gfx_hud wipe event 0), plays that name on the game thread (next CalcSceneView) when the reveal starts (event 1).
  Commands: bc wipesound 0|1, bc wipesound play. If the bat sound is not a menu-movie sound, the log lists what is played
  instead ("no wipe sound heard yet").
- JJ 04:16 (HEADSET): no sound; the menu's audio started before the reveal. LIVE: no menu sound logged at all (the
  RGFxMovie PlaySound path is unused); low phase 162 again - Batman's shot (~290) is also < 0.5 x wipe level (820), so it
  fed the low average and lifted the bar above itself (released at 352).
- MENUARRIVE3 + WIPESOUND2 (built + deployed 04:19, backup before-wwise, not run): once the low phase is known only
  presents under the bar update it, and it starts from the smallest low value. Sound: Wwise is linked into the exe and
  EXPORTED - AK::SoundEngine::PostEvent(id, gameObject, flags, cb, cookie, n, ext) 0x1057640 (prologue 83 EC 10 53 57 8B
  7C 24 30 checked; name versions 0x1056D40 / 0x1056CB0). Hooked: events near a wipe start (0.7 s before .. 0.4 s after)
  that are not also posted on their own (clicks, music) = the wipe's sound; the reveal posts it on the newest event's
  game object (game thread). Log: "Wwise event N on object M (at the wipe start)" (first 80 near wipes), "WIPESOUND: the
  wipe's sound = Wwise event N", "posted with the reveal ... playing id" (0 = refused).
- JJ 04:21 (HEADSET): menu audio starts while still black; our bats silent; Batman already there = a little late. LIVE:
  no Wwise event within -0.7 / +0.4 s of the wipe start; menu events from +1.1 s (clock running 02.38 = the menu audio).
  Low phase 94 now right; release at the 3rd high present.
- WIPESOUND3 (built + deployed 04:23, backup before-wwise2, not run): the name versions of PostEvent (0x1056D40 / 0x1056CB0)
  never call the id version - all three end in 0x1056BD0 (cdecl id, object, flags, cb, cookie, externals; prologue 83 EC
  30 B8 01 00 00 00 56), hooked there. At each wipe start the events of the last 1.5 s are logged; learning window
  -1.5 / +0.6 s. Release on the FIRST present of Batman's shot; reveal starts 300 ms into the recording (bc wipereveal
  skip N, 0-900).
- JJ 04:25 (HEADSET): bats still too late, still silent. LIVE 04:25:08.791: at the quit wipe's start the game posted
  990593514 + 3809399880 (15 ms before) and 3904589100 (0 ms); before them alternating 172321384 / 2563718726 = pause-menu
  clicks. The not-common rule dropped the bat sound because it plays at EVERY game wipe (pause too).
- WIPESOUND4: gfx_hud wipe event 2 at every 'wipe starts'; each Wwise event within 60 ms of a wipe start scores one point
  per wipe; the bat sound = most wipe starts (ties: nearest). Command: bc wipesound id N (force), bc wipesound play.
- JJ chose (AskUserQuestion): reveal 'just before Batman' (bats already flying as his shot appears; the end of the moon
  shot may show). PREREVEAL: in the low phase the reveal starts 500 ms after the disk goes quiet (< 12 MB/s); traces: quiet
  1-2 s before the cut. Batman's shot first = immediate as before. Reveal skip back to 0. Commands: bc wipereveal pre N.
  Built + deployed 04:3x (backups before-wipesound4, before-prereveal), not run.
### PACKAGE 0.1.2 2026-10-11 04:3x (JJ: "package it up removing any tests and probes ... all the settings saved and installed
and then removed when uninstalled"; NOT published)
- LIVE 04:29-04:30 (JJ's run of the 04:28 build): bat sound learned = Wwise event 3904589100 (0 ms from the wipe start),
  posted with the reveal on object 7 -> playing id 152 (accepted); PREREVEAL released 'loading finished - just before
  Batman's shot'. BUG found: wipe_event(2) had been inserted as the body of a one-line if -> 'wipe starts' logged for
  every wipe piece every frame (358k log lines) and scoring capped at 60 wipes. Fixed with braces.
- Removed test code: HDFOLLOW / HDFOLLOW2 + the frame_inspector copy tap (never needed), LOADAB, LOADTIME readout, DISKREAD,
  MENUTRACE, the Wwise per-event logging and 1.5 s dumps. A slip in that cut (a duplicated 'else if (kind == 2)' that
  made scoring dead code) was caught by reading the result. Head-camera counters now behind 'show test options'.
- Install: first install also copies launch.ini (2480x2648, JJ's DLSS output size) and menu_wipe.bin (the recorded bat
  transition every reveal replays - a fresh install had none until a game was started from the main menu). Uninstall:
  always deletes %LOCALAPPDATA%\Batman_City (settings, launch.ini, recording, logs, captures); -KeepSettings keeps it,
  -KeepShortcuts for practice runs. README removal text says so.
- Practice run (scratchpad, LOCALAPPDATA redirected, copies of JJ's configs): install -> 3 data files; second install
  keeps a changed setting; uninstall from the game folder -> data folder gone, backup gone, BmEngine / UserEngine /
  DefaultEngine byte-identical to before. JJ's real files untouched (hash checked).
- dist/BatmanCityVR-ArkhamCity.zip 117 files, 292.4 MB, sha256 ea1482ae91f0c786... (changed vs 0.1: dll, both scripts,
  README, settings; new: launch.ini, menu_wipe.bin). Clean build (04:34) not yet run in the game.
- README (GitHub page + zip): download step now explains the size - upscaled loading pictures ~225 MB (87 at 2x), DLSS
  library ~60 MB, the mod a few MB (measured from the zip: 223.5 / 59.1 / 5.6 MB). Package rebuilt with it.
- README images (JJ: "the image ... sucks"): replaced images/stereo_gcr*.jpg (dark wall, Batman's back, HUD prompt) with
  images/rooftop_hero.jpg (left eye of capture 20261010_173004, 16:9 crop 2480x1395 from y 20, 1600x900: helicopter +
  searchlight, Joker balloons, Ferris wheel, cape) at the top and images/rooftop_stereo.jpg (both eyes, y 150-2350,
  800 px each) under 'Native stereo 3D'. Grade: gamma 0.82 lift, contrast 1.06, light unsharp after the downscale -
  nothing added. Package rebuilt (sha256 dd778c8e...). Not pushed.
- GitHub page (JJ: the top image should be the game's marketing image): copying WB's Steam library art into the repo was
  refused by the permission check (republishing WB art); JJ chose 'use Steam's'. README top = <img> of Steam's
  capsule_616x353.jpg straight from cdn.cloudflare.steamstatic.com (not in the repo) + credit line. Pushed: 544dbd6
  (rooftop images, size note), 1a17f22 (Steam artwork). The working README keeps the 0.1.2 removal text (pushed copies
  say the 0.1 text until the release).
