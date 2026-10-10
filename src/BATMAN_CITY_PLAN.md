
### JJ 03:36: "much better, but still flickering sometimes when opening the level up window" (capture 033614)
- LIVE log: two upgrade-screen openings still "wipe replay ended after 4000 ms": the replay starts on a later piece of
  the wipe, 4 ms AFTER the game clock stopped, so MENUOPEN's "gameplay" test was already false -> the 4 s cap.
- HUD ring 033614: from t168109 every piece of the WayneTech screen drawn but hidden (h) for exactly 4.0 s, then shown;
  during the opening some frames carry only 75 / 120 of ~180 pieces (the screen's own build-up) - the edge flicker JJ saw
  lines up with this hidden / partial period.
- MENUOPEN2 (built + deployed, backup before-menuopen2): "started in gameplay" is remembered when the wipe STARTS
  (g_wipeFromGameplay) and the replay's cap uses it. Expected log: "wipe replay ended after ~1870 ms" on openings.