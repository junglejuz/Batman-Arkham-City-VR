# DLSS in Batman City (Arkham City VR) — plan

Written 2026-10-08. Goal (JJ): NVIDIA DLSS upscaling per eye — each eye rendered smaller and upscaled to the
headset size, for frame rate and clean anti-aliasing. Read with `BATMAN_CITY_PLAN.md` §10 round 9 and the
Arkham Knight record it reuses: `akvr_native/DLSS_PLAN.md` + `akvr_native/NOTES.md` (DLSS D0..MIPBIAS).

## 1. Why a helper program

- Arkham City is **32-bit**; NVIDIA's DLSS library (`nvngx_dlss.dll`, NGX SDK) exists **only as 64-bit**
  (checked: machine 0x8664; the SDK ships x64 libraries only). A 32-bit game cannot load it.
- So DLSS runs in a small **64-bit helper program** (`bc_dlss64.exe`) on the same graphics card. The game and the
  helper share textures (D3D11 shared NT handles + keyed mutexes); a shared memory block carries per-frame
  numbers (jitter, matrices, sizes, reset); two events hand control back and forth.
- Measured first (2026-10-08, `tools/tests/perf-measure-run10.ps1`): at 2480x2648 per eye the graphics card is
  92-100 % busy at 90 fps (the game's own 90 cap), the processor has room (second eye draw ~0.4 ms) -> upscaling
  buys frame rate. In the headset Virtual Desktop's encoding adds to the graphics-card load (JJ sees 75-80).

## 2. Design (reusing Arkham Knight's working method)

| Part | Arkham Knight (AKN, in-process) | Batman City (helper) |
|---|---|---|
| Where DLSS runs | end of each eye render: finished picture -> full-size eye | same: the eye's present, where the mod copies the eye into the headset image (`capture_frame`) |
| Jitter | AKN's lens, Halton(2,3), same index both eyes | our FSceneView ctor projection (`ctor_detour`), m20/m21 offset |
| Motion vectors | own camera motion per eye (CAMMV mode 1: depth + this eye's camera vs its previous camera, no jitter) | same compute pass, in the game process, written into a shared R16G16F |
| Depth | the game's SMAA input | the eye's scene depth buffer at its present (to find in D0), copied to a shared R32F |
| Render size | engine renders the eye small; window sized from the headset eye | the backbuffer (launch.ini VR size) = render size; headset swapchains = output size (decouple in D1) |
| Texture sharpness | MIPBIAS: sampler twins with bias log2(scale) - 1, incl. tiling linear samplers | port |
| HUD | HUDFULL: Scaleform movies drawn at headset size | port if the HUD looks soft |
| Desktop window | DLSSMIRROR | port |
| DLSS DLL | 310.9.1 (DLSS 4.x SR, transformer; JJ: no DLSS 5, no CNN) | the same files, in the helper's folder |

Credit: Luma Framework (Filippo Tarpini, Garamond; Custom MIT) for the Arkham Knight method AKN was built on.

## 3. Stages (each: one launch on the fake headset, measured, then JJ in the headset)

### D0 — the hand-off, no DLSS (this stage)
- `helper64/` (separate x64 CMake project): `bc_dlss64.exe` opens the shared textures and, per eye, copies input
  -> output. The game side (`dlss_bridge`): starts the helper, creates the shared textures, and at each eye's
  present sends the eye picture through the helper and copies the result into the headset image.
- PASS: the picture is unchanged (eye images identical to the direct copy), no stalls, round-trip cost per eye
  measured (game-thread wait and frame rate vs direct copy). FAIL -> try D3D11.4 shared fences instead of keyed
  mutexes, or batch both eyes into one hand-off at the right eye.
- Also find the eye's depth buffer at its present (size, format) for D1.

### D0 RESULT (2026-10-08, fake headset, `tools/tests/dlss-d0-run11.ps1`) — PASS
- First try: the game's device refused shared keyed-mutex textures (CreateTexture2D 0x80070057); a fresh 32-bit
  device on the same card accepts every variant (scratch test). Cause: the game makes its device through the DXGI 1.0
  `CreateDXGIFactory` (memory steamvr-dxgi11-shared-textures, BioShock VR). Fix DXGI11: `dlss_bridge::install_early`
  answers it with `CreateDXGIFactory1` before the game makes its device (2 calls answered). No geo-11 here.
- Hand-off through `BatmanCityDLSSc_dlss64.exe` (copy): 0.32 ms per eye on the render thread (helper 0.07 ms),
  3175 trips, 0 fallbacks, 0 timeouts, frame rate unchanged (90 at the game's cap). Picture intact (vis_h1_helper).
- Switch: `bc dlss0 on|off` (test only, not saved). Core hook: `bvr::vr::set_eye_capture_hook`.

### D1 — DLSS per eye
- NGX + one DLSS feature per eye in the helper (AKN dlss.cpp ported); jitter in the projection; CAMMV motion
  vectors; render size x scale (DLAA / Quality / Balanced / Performance); swapchains at the output size.
- Tests in one launch: frame rate per scale vs native; sharpness crops; head-sweep trails; no leak between eyes.

### D1a RESULT (2026-10-08, fake headset, `tools/tests/dlss-d1a-run12.ps1`) — DLSS WORKS, DLAA too costly
- Helper: NGX 310.9.1 ready, one feature per eye (2480x2648 -> 2480x2648, flags MVLowRes | AutoExposure), first
  evaluates OK; game side: jitter (Halton 8) in the FSceneView lens, CAMMV + depth copy compute pass (R24 depth ->
  shared R32F, motion -> shared R16G16F), vpU = vpJ minus the jitter terms.
- Fixes on the way: (1) a timed-out request was overwritten by the next one -> cascading failures (trips 0) ->
  one request at a time, 100 ms limits, per-step failure counters; (2) CreateDeviceContextState is refused on the
  game's device -> the pass saves / restores the game's compute bindings by hand.
- Measured: 2031 hand-offs, 7 fallbacks (start-up), hand-off 0.5 ms per eye (render thread), helper CPU 0.06 ms,
  **DLSS on the card 7.7 ms per eye** at DLAA 2480x2648 -> 90 -> ~37 fps. Picture correct (vis_d1_dlaa), depth
  inverted 0 / 1 alike on a still scene. => The gain needs D1b (render smaller); AKN: Performance 2.8 ms per eye.
- Switches: `bc dlss mode 0..5`, `bc dlss depthinv 0|1`, `bc dlss preset N`, `bc dlss jitter 0|1`.

### D1b — render smaller (next)
- Core: headset swapchains at the OUTPUT size (setting), independent of the backbuffer (= render size, launch.ini
  VR window); every consumer that assumed swapchain == backbuffer checked (plain copy -> stretch when sizes differ).
- Render size = output x scale (UQ 0.77, Q 0.667, B 0.58, P 0.5) written into launch.ini (restart, like AKN).
- Jitter phases 8 x (out/in)^2; MIPBIAS port; measure fps per scale vs native (90 cap: uncap the game for the test).

### D1b RESULT (2026-10-08, fake headset, runs 13-17 in `tools/tests/dlss-*-run1[3-7].ps1`) — WORKS, small gain only
- Built: `bvr::vr::set_output_size` (core) keeps the headset images at the launch size while the game draws
  launch x scale (window.cpp reads `dlss_mode` from settings.ini at launch; the panel/`bc dlss mode` value is saved
  and takes effect on the next launch). Without DLSS (mode 0, helper failure) the core stretches the frame
  (`capture_frame` -> `flat_copy` + `blit::shift_copy`). Jitter phases 8 x (out/in)^2. The quality name sent to NGX
  follows the real size ratio. Test switch `bc dlss repeat N` (extra evaluates per eye).
- Measured at the game's 90 cap (headset images 2480x2648 per eye; frames/s from `[bc] beat: draws`):

| Setting | Game draws | DLSS per eye (card) | frames/s | card busy |
|---|---|---|---|---|
| Native, no DLSS | 2480x2648 | - | 90 | 92-99 % |
| DLSS Quality | 1654x1766 | 4.1 ms | **74** | 98 % |
| same, DLSS off (stretched) | 1654x1766 | - | 90 | ~50 % |
| DLSS Performance | 1240x1324 | 2.8 ms | 90 | **84-86 %** |
| same, DLSS off (stretched) | 1240x1324 | - | 90 | ~40 % |
| DLAA (D1a) | 2480x2648 | 7.7 ms | 37 | - |

- The cost is DLSS itself, not the hand-off: 1 / 2 extra evaluates per eye added 4.1 ms each (74 -> 46 -> 33 fps).
  Presets 0 / E / F / J / K all cost the same (4.1-4.2 ms; NGX 310.9.1 seems to run one model whatever the hint).
- Arkham City is light: its whole scene at Quality size costs ~5.5 ms per frame (both eyes), less than DLSS's
  ~8 ms. Only Performance beats native, by ~13 % headroom. Pictures: Performance clearly sharper than the plain
  half-size stretch; next to native, about as sharp, with smoother edges (vis_f1_perf vs vis_n1_native).
- Found: the panel's capture saves the GAME frame (render size) under DLSS, not the headset image -> D2.

### D2 — built 2026-10-08 (JJ: "we've come this far. may as well at least have better AA")
- Default: no `dlss_mode` saved + DLSS available (helper files + first graphics card NVIDIA) -> **Performance**;
  NGX failing at run time -> DLSS off and saved off (next launch full size). Flat launches keep the saved choice
  untouched (`launched()` guards jitter, FXAA and the save).
- NOFXAA: the game's FXAA-HI is the last scene pass (frame dump: full-screen quad, finished picture -> backbuffer,
  same size/format, caller 0x5A9780); under DLSS it is replaced by a plain copy (vision.cpp `fxaa_pass`, test
  `bc vis nofxaa -1|0|1`). No ini change.
- MIPBIAS (AKN port): surface samplers swapped at bind time (new core PSSetSamplers tap) for twins with bias
  log2(scale) - 1 (full, default) / log2(scale) (mild), 16x on anisotropic ones; `bc dlss mipbias 0|1|2`.
- Panel: "DLSS (SMOOTHER EDGES)" - off / quality / balanced / performance at the next start, sharper textures, cost.
- Capture: saves the headset image when it differs from the game frame.
- Test (run 18, `tools/tests/dlss-d2-run18.ps1`, fake headset, three passes): default came up Performance
  (available yes), 90 fps throughout, capture 2480x2648. Fixes on the way: (1) the scene target is PADDED
  (1240x1328 source for the 1240x1324 backbuffer) -> size rule allows +16, copy a box; (2) dropping the FXAA draw
  moved the tracker gadget (the HUD classifier keys on that draw) -> the draw runs and the unfiltered picture is
  copied back right after it (new core `set_draw_indexed_after`); (3) our own state save/restore re-bound twins and
  they got twinned again (bias -2 -> -4 -> -6) -> twins map to themselves. Pictures: no-FXAA + sharper textures is
  the crispest (bricks, lamp grill); FXAA kept is visibly softer.

### First headset session with DLSS (JJ 2026-10-08 22:45-22:50)
- "Glows very flickery, distant city lights": suspects (1) sharper textures (bias -2 at Performance: distant lit
  windows alias, bloom amplifies), (2) jitter at half size: sub-pixel lights pop in and out of the jittered picture
  and the bloom is built BEFORE DLSS from it. Live A/B in the panel: sharper textures full/mild/off, new checkbox
  "sub-pixel shake"; DLSS off needs a restart.
- "VR froze after ~5 min, game kept running": Windows System log 22:49:34 nvlddmkm Xid 13 "Graphics Exception:
  SAVE_RESTORE_ADDR_OOB" (+ Xid 153) - a GPU context-switch fault; the game and the DLSS helper carried on (helper
  status 0, 90 fps), Virtual Desktop's side died (its OpenXR.log: xrCreateSwapchain RUNTIME_FAILURE ovr -7000 from
  22:49:58). This PC logged nvlddmkm errors / TDRs for weeks before DLSS (other mods too); this variant is new and
  coincides with a third GPU client (the helper). Watch: does it recur with DLSS off? The panel swapchain now
  retries at most every 2 s (it tried 568 times in 6 s).

### Freeze analysis (2026-10-09, Windows System log, 40 days)
- Both DLSS freezes (10-08 22:49:34, 23:05:54) carry the SAME signature, seen nowhere else: Xid 13 x8 "Graphics
  Exception: ESR 0x520900=0x80000001" + Xid 153. Older Xid 13s are different kinds (09-11 ESR 0x526fb0 x180;
  09-21 FECS logging error). No Xid 13 since DLSS was set off. NVIDIA: Xid 13 is usually an application-side fault
  (out-of-bounds access), rarely driver/hardware (docs.nvidia.com/deploy/xid-errors).
- AKN runs the same CAMMV compute pass (akvr_native/src/dlss.cpp:303) and DLSS in-process, under VD, with no Xid
  13 -> "our compute passes" is the weaker suspect. What only Batman City has: a SECOND GPU program (the helper)
  working on keyed-mutex shared textures, handed back and forth twice per frame.
- Ranked fixes: (1) shared fences instead of keyed mutexes + both eyes in one hand-off per frame; (2) helper never
  creates/frees shared textures while the game runs (check every recreate path); (3) CAMMV/AIMDEPTH as pixel
  shaders; (4) NVIDIA driver update (617.14). Proof needs long soaks: the faults came ~4-20 min into a session.
- Helper log of the 23:05 freeze session (dlss_helper.log): helper up 23:04:15, Balanced 1438x1536 -> 2480x2648, fault
  23:05:54 = ~1.5 min in; texture sets opened ONCE per eye at the start (gen 1, 2), never again -> suspect (2) cleared
  for that session; DLSS kept answering through and after the fault (2.8-3.6 ms per eye) - only VD's work died.
- How the old hand-off waits: keyed mutexes only. Each AcquireSync puts a WAIT INTO THE GRAPHICS CARD's stream: the
  helper's context stalls on the game's copy, the game's context stalls on the helper's DLSS = 4 card-side stalls of
  one context on another per frame (360/s), each a context switch with state save/restore (the faulting step).
- FINISHFIRST (built + installed 2026-10-09, not yet run; old builds in backups/build-before-finishfirst-20261009):
  `helper64/gpu_finish.h` waits on the CPU until a context's work has run (ID3D11Fence + event, else an event
  query). sync 2 (default): the game releases its pictures, waits until its copy + motion pass have run, then asks;
  the helper releases the output, waits until its DLSS has run, then answers -> every keyed-mutex wait is already
  satisfied, no context stalls mid-stream. sync 1: helper side only. sync 0: the old way. Live: `bc dlss sync 0|1|2`,
  panel "hand-off to DLSS: safe / half / old (froze)" (not saved). `bc dlss status` logs the full status line.
  Shared block version 3 (game + helper must match). Cost to measure: the render thread now waits for the card each
  eye (CPU-GPU overlap lost there).
- Test: `tools/tests/dlss-finishfirst-run24.ps1` - one launch at Performance: A/B/C 2 min each (sync 2/1/0, frame
  rate + waits), then a 25-min soak in sync 2 with head sweeps, System log checked every 30 s. Then JJ: 20-30 min
  in the headset with DLSS on (the fake headset has no Virtual Desktop, whose work was the one that died).

### Run 24 (2026-10-09, fake headset, stopped after 10 min by JJ: "safe" costs too much)
- Same session, DLSS Performance, 2 min each: old (sync 0) **90** fps steady, half (sync 1) 82, safe (sync 2) 73
  (render thread 4.1 ms per eye: game finish wait 0.87 + helper 3.0). 0 timeouts, fence used on both sides, no
  driver events in the 10 min (no VD in the loop - not proof). The cost = the CPU-GPU pipeline drained per wait.
- Script lesson: the old Escape (with the Alt tap) left the game paused; JJ unpaused, then my extra Escape re-paused
  (memory no-blind-toggle-keys). Fixed in the script.

### OVERLAP (sync 3, built 2026-10-09, default) - full speed without card-side waits
- Left eye: copied in, `fenceIn` (shared ID3D11Fence, game -> helper) signalled with the slot's serial, request slot
  `req[0]` filled, NO wait - the core keeps the left headset image acquired (core `set_eye_defer_hooks`: defer at the
  left capture, finish after the right capture). The helper waits for fenceIn ON THE CPU, runs DLSS while the game
  draws the right eye, writes status/answered, signals `fenceOut` (helper -> game). Right eye: same, then ONE CPU
  wait on fenceOut (>= right serial = both done), right output copied, then the core's finish hook copies the left.
- Card-side waits: none in normal running (each keyed-mutex acquire happens after its fence passed on the CPU).
  Waits per frame: 1 (safe had 4). Expected near the old 90 fps.
- Core safety (`openxr_runtime.cpp flush_held_left`): the held left image is written + released on EVERY early end:
  swapchain destroy / session teardown, session stopped, detached, pair hold expired, headset idle, leaked frame,
  room screen, left captured again, no frame open, and a catch-all before xrEndFrame. Counted ("released early").
- Fallbacks: no ID3D11Device5 / shared fences -> safe (sync 2); 30 failures in a row after the first success ->
  overlap off (logged); right eye not sent -> one-eye path; left answer missing -> left keeps its old picture once
  (counted "left lost"). The one-at-a-time wait now loops (overlap answers share the "done" event).
- Shared block version 4 (per-eye request slots + fence handles duplicated into the helper).
- Switch: `bc dlss sync 0..3`, panel "hand-off to DLSS: overlap / safe / half / old (froze)".
- Test: `tools/tests/dlss-overlap-run25.ps1` (A overlap 2 min + eye pictures, B old 1 min, C live switches, D 10 min
  overlap with the System log).
- RUN 25 RESULT (2026-10-09 03:25-03:41, fake headset, Performance 1240x1324 -> 2480x2648): overlap **88.4** fps
  (84-90) vs old 90 in the same session (safe was 73, half 82); soak 10 min overlap 88.7 (75-90). 71,600 pairs:
  0 fallbacks, 0 timeouts, 0 left lost, 0 held images released early, 0 helper copy waits ran out; one wait per frame
  3.7-4.2 ms (DLSS 3.0 ms per eye on the card). Helper opened both shared fences (S_OK). Live switches 3->2->3->1->3
  clean. Eye pictures (vis_a_still / vis_a_turned): both eyes same moment, correct stereo during a head turn. No
  driver events in the whole run. NEXT: JJ 20-30 min in the headset with DLSS Performance + overlap (VD in the loop).

### D2 — still open
- The HUD panel is now captured at the render size (1240x1324 at Performance): check its text in the headset; if
  soft, draw the HUD at the output size.
- MIPBIAS, HUD at full size if soft, desktop mirror, panel (DLSS mode, model, texture sharpness), saved settings.

## 4. Risks

| Risk | What we would see | Answer |
|---|---|---|
| Cross-process sync too slow | lower frame rate than without | fences, batch both eyes, or pipeline one frame |
| Virtual Desktop's in-process device | wrong device / mono (AKN lesson) | everything on the game's own device and immediate context |
| Motion vectors wrong | trails on head turns | CAMMV checks (AKN D1m), jitter sign |
| Helper missing / crashes | black eyes | game falls back to the plain copy after a short timeout |
