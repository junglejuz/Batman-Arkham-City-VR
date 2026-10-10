# Batman City — changes to game / third-party files

Every change Batman City makes outside its own folder is listed here so the installer can apply it
and the uninstaller can undo it. Originals are backed up under `batman_city/backups/`.

## 2026-10-03 — DirectX 11 on (JJ approved)

Config folder: `D:\Documents\WB Games\Batman Arkham City GOTY\BmGame\Config\`
Backup: `batman_city/backups/config-original-20261003/` (BmEngine.ini, UserEngine.ini as found)

| File | Encoding | Line | Before | After |
|---|---|---|---|---|
| BmEngine.ini | ASCII, CRLF | 1780 (`[SystemSettings]`) | `AllowD3D11=False` | `AllowD3D11=True` |
| UserEngine.ini | **UTF-16 LE with BOM**, CRLF | 8 (`[SystemSettings]`) | `AllowD3D11=False` | `AllowD3D11=True` |

Why: OpenXR has no D3D9 binding; same-frame stereo needs the game's D3D11 device.
Installer note: write UserEngine.ini back as UTF-16 LE with BOM (a plain-text write breaks it).
The game's launcher (BmLauncher "DirectX 11" option) writes the same keys.
Undo: copy the two backup files back, or set both lines to `False`.

## 2026-10-03 — mod files in the game folder (development install)

Game folder: `E:\Games\Steam\steamapps\common\Batman Arkham City GOTY\Binaries\Win32\`

| File | What | Undo |
|---|---|---|
| `xinput1_3.dll` (new; none existed before) | loader: forwards to the real SysWOW64 xinput1_3.dll, loads batman_city.dll | delete it |
| `batman_city.dll` (new) | the mod | delete it |
| `Batman City VR (test).bat` (new, 2026-10-03, JJ asked for it here) | launcher: `-windowed ResX=2064 ResY=2208` from its own folder; desktop shortcut "Batman City VR (test)" points at it | delete it (and the shortcut) |

Logs, captures and the test-only simulator live under `%LOCALAPPDATA%\Batman_City\`, never in the game folder.

## 2026-10-03 — VR comfort settings (first look)

Same config folder and backup as above.

| File | Line | Before | After | Why |
|---|---|---|---|---|
| BmEngine.ini | 161, 203 (`bSmoothFrameRate` blocks) | `MaxSmoothedFrameRate=62` | `MaxSmoothedFrameRate=90` | the game capped itself at 62 fps; the Quest runs 72-90 Hz |
| BmEngine.ini | 1749 (`[SystemSettings]`) | `MotionBlur=True` | `MotionBlur=False` | both eyes share one view history for now, so blur would smear between eyes; also a VR comfort rule |
| UserEngine.ini (UTF-16) | 17 | `MotionBlur=True` | `MotionBlur=False` | same |

These also apply to flat play until per-mode profiles exist (AKVR's VRLAUNCH pattern).

## 2026-10-03 — frame cap in the install's defaults

The game rewrites `MaxSmoothedFrameRate` in BmEngine.ini from the install defaults at every
launch (MotionBlur is kept). So the cap is raised at the source too.

| File | Line | Before | After |
|---|---|---|---|
| `E:\...\Batman Arkham City GOTY\BmGame\Config\DefaultEngine.ini` (ASCII, CRLF) | 700 (`[Engine.GameEngine]`) | `MaxSmoothedFrameRate=62` | `MaxSmoothedFrameRate=90` |

Backup: `batman_city/backups/install-config-original-20261003/DefaultEngine.ini`.

## 2026-10-03 — opening logo movies: ini route REVERTED

`StartupMovies` was commented out in DefaultEngine.ini and BmEngine.ini, but the logos still
played (the NVIDIA build uses `BmGame\Movies\StartupNV.swf`, not the ini list). Both lines were
restored the same day; those files are back to the earlier state. The logos are now skipped by
the mod itself, in VR launches only (it reports `Startup.swf` / `StartupNV.swf` as missing to the
game - no game file is changed).

## 2026-10-08 — lens flares off (JJ: "lens flares are not fusing in both eyes")

Config folder: `D:\Documents\WB Games\Batman Arkham City GOTY\BmGame\Config\`
Backup: `batman_city/backups/config-before-lensflares-20261008/` (BmEngine.ini, UserEngine.ini as found)

| File | Encoding | Line | Before | After |
|---|---|---|---|---|
| BmEngine.ini | ASCII, CRLF | 1772 (`[SystemSettings]`) | `LensFlares=True` | `LensFlares=False` |
| UserEngine.ini | **UTF-16 LE with BOM**, CRLF | 14 (`[SystemSettings]`) | `LensFlares=True` | `LensFlares=False` |

Why: lens flares are built in screen space from each view's centre; with the asymmetric eye frusta the two
eyes place them far apart (VR_ESSENTIALS §7: force lens flares off). Also affects flat play until per-mode
profiles exist. Undo: copy the two backup files back, or set both lines to `True`.

## 2026-10-08 — depth of field off (JJ: "the glow is very low resolution ... blockiness around distant windows")

Config folder: `D:\Documents\WB Games\Batman Arkham City GOTY\BmGame\Config\`
Backup: `batman_city/backups/config-before-dof-20261008/BmEngine.ini`

| File | Encoding | Line | Before | After |
|---|---|---|---|---|
| BmEngine.ini | ASCII, CRLF | 1752 (`[SystemSettings]`) | `DepthOfField=True` | `DepthOfField=False` |

Why: the "glow" over distant windows and the sky is the game's depth of field - a far-distance blur built at a
quarter of the picture (frame dump: 622x664 buffer blended in by the final combine pass); measured on the fake
headset it adds +55 to the far sky and 0 to bright near snow, so it is distance-based, not brightness-based. Blocky
and flickering in VR; VR_ESSENTIALS §7 forces depth of field off. UserEngine.ini has no DepthOfField line. Also
affects flat play until per-mode profiles exist. Undo: copy the backup back, or set the line to `True`.

## 2026-10-08 (later) — depth of field change REVERTED

`DepthOfField=False` made the engine skip its whole final combine pass (frame dump with DoF off: the scene goes
straight to the backbuffer - no bloom, no light shafts, no exposure strip). JJ noticed the game's glow was gone.
BmEngine.ini line 1752 is back to `DepthOfField=True`; the file is byte-identical to the backup
`backups/config-before-dof-20261008/BmEngine.ini`. The far-distance blur is now handled in the mod instead (glow.cpp).

## 2026-10-09 — mod data folder in the game folder

| Path | What | Undo |
|---|---|---|
| `Binaries\Win32\BatmanCityData\reticle\*.rgba` (new) | sharp 8x versions of the grapple reticle's pictures, made by `tools/reticle_hd.py`, named by a fingerprint of the original's pixels; the mod swaps them in for the reticle's draws | delete the folder (the game's own pictures are untouched) |

## 2026-10-10 HD SCREENS: new folder in the game folder
- `Binaries\Win32\BatmanCityData\hd\` created (empty). tools/hd_upscale.ps1 writes Magnific-upscaled loading-screen
  pictures there as `<key>.png`; the mod loads them. Game files are not changed. Remove the folder to undo. These are
  the game's art (WB): never commit or ship them.
## 2026-10-10 HD LOGO test: BmGame\Movies\bac_logo.usm replaced
- The opening-logo video (Batman Arkham City title card, tech logos, WB / DC / Rocksteady) rebuilt at 2560x1440 by
  tools/usm_rebuild.py (same container, audio, frame timing and keyframes; MPEG-1). Test build = local lanczos enlarge.
- Original backed up: batman_city\backups\game_files\bac_logo.usm.orig (24,940,992 bytes). To undo: copy it back over
  BmGame\Movies\bac_logo.usm (Steam "verify files" also restores it). Game art (WB): never commit or ship.- 2026-10-10 18:05: the 2560x1440 bac_logo.usm FROZE the game -> original restored (hash checked). Replaced instead by
  the mod painting `BatmanCityData\hd\logo_title.png` / `logo_tech.png` (Magnific upscales, WB art - never commit or
  ship) over the original video. Game files unchanged.- 2026-10-10: `Binaries\Win32\BatmanCityData\hd\synopsis\` - 87 Magnific-upscaled loading-screen pictures (WB art) +
  synopsis_index.txt; drawn by the mod in place of the game's. Game files unchanged. Delete the folder to undo.