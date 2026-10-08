# Port round 11 session 3, task G: the per-tick references in a tiled wide world, and the wide renderer at dd 127 / 255

Two test extensions, no engine change:

- `reference_test` with **`MC_REF_WORLD=N`** (512 .. 4096) replays every per-tick reference of the original in a world
  tiled to N cells, and a new ctest **`reference_world`** gates movie 0 at N = 1024.
- `world_render_test` gets two parts: the 1024 world at draw distance 127 and 255 (kWin 256), and a generated 1024
  world (`rts --size 1024`) drawn at dd 255.

With `MC_REF_WORLD` unset, `reference_test` behaves byte-identically to before. Its output for `movie0_full` and
`level38` is diff-identical to build_F's binary, which has the pre-change source. All existing gates pass (see the
Gate section).

## MC_REF_WORLD: how the wide run is set up

- **When the world is widened.** `world_widen_tiled(N)` runs right after the playback loads the snapshot pair (the
  `gam` / `map` files). This happens in the first tick, before that tick's Thing updates. `reference_test` links no
  renderer, so it uses the free `g_hook_demo_textures_reload` hook, which the playback calls right after
  `demo_load_terrain`. For the level-generation check, the widening comes after `sim_load_level` and before the
  first `game_tick_sim`. A later `sim_load_level` sets the world back to 256, and the next playback widens again.
- **Cell-list order.** `world_widen_tiled` relinks every cell list in slot order. `reference_test` puts back the
  snapshot's own heads and `cell_next` / `cell_prev` links: 54 slots of movie 0 are relinked back. This way a cell's
  Things are visited in the original's order. `MC_REF_WORLD_RELINK=slot` keeps the slot order instead. On movie 0 up
  to tick 548, both orders give the same set of (slot, first tick), so the order does not matter there.
- **What is compared.** The port's state goes through `state_to_image`, so the low halves are compared, exactly as
  at 256. The maps are compared in tile (0,0). A cell-list head is "folded" over the tiles: it is the head of the one
  tile whose list is non-empty, or a difference when Things of that 256-cell sit in two tiles. `MC_REF_CELLCHECK`
  walks `g_world.cells`.
- **Extra output (only with `MC_REF_WORLD`):**
  - the first tick a Thing leaves tile (0,0);
  - at the first divergent tick: Things outside tile (0,0), terrain changed outside it, and where the local wizard is;
  - for every slot at its first divergence (`MC_REF_WORLD_WHERE=n` lines, default 40): its full position, its
    "edge" (distance in cells to the old boundary lines x/y = 0 of the 256 torus), and its target and the
    reference's target with their positions;
  - at each 100-tick map dump: how far from the boundary the differing terrain cells reach;
  - the `LEVEL` summary line gains the first tick a Thing leaves tile (0,0);
  - in the gen-check trace (`MC_REF_GEN_TRACE`): positions and targets.
- **`MC_REF_WORLD_GATE=K`.** This is the pass rule of the wide run (exit 0/1). It passes when all of these hold:
  - the RNG never differs;
  - no handler is missing;
  - every differing terrain cell is within K cells of the old boundary;
  - every slot that diverges is a *boundary case*: the Thing itself, its owner, its target or the reference's target
    is within K cells of the old boundary or outside tile (0,0), or the Thing and its target / owner are a pair whose
    256-torus difference `(int16_t)(b - a)` is not the tiled difference `world_signed(b - a)` (their shortest way in
    the original crosses the boundary). A spell sits at (0,0), so its owner stands in for it.

  Any other divergence is printed as `UNEXPLAINED`.

## Per-set divergence (N = 1024)

The global RNG (GameState+4) is **identical at every compared tick in every set**: in movie 0, all 8 levels, and over
the whole movie (8195 ticks of `movie0_full`). The per-tick RNG draws do not depend on the diverging Thing logic, the
same as at 256.

| set | first divergent tick | first Thing outside tile (0,0) | first divergence and why |
|---|---|---|---|
| movie0 / movie0_full | **413** (the first tick) | 415 (projectile 491 into cell 256,256) | **Boundary.** An AI castle (slot 895) stands exactly at (0,0), a quadrant corner of the AI castle-site search, and the local wizard is at cell 2,117. Tick 413: creature 395 (14/85) at 234,114 aims at target 12 at 3,77. That is 25 cells across x = 0 in the original but 231 cells the other way in the tiled world: `target_yaw` 186 vs 1587. AI wizard 486 (27,249) takes castle 895 at (0,0) as its target in the original and 283 in the port. AI wizard 492 (48,1): `home` from the scaled quadrant rule (row 3 below). Tick 414 on: fireballs (13/13) at 255.x,255.x fly at castle 895. In the tiled world they leave into tile (1,1), the castle (only in tile 0) is not hit, so the castle health stays 12800 instead of 12400 and the live count differs from 415. Ticks 418-422: mana balls at x = 253..255 next to the local wizard, and creatures 9/56 at 244..253,248..252 heading for castle 895. Map dump 500: 60 terrain cells differ, all within 3 cells of the boundary. **First divergence away from the boundary: 549** (possession cast on mana ball 377 at 100,52, then wizard-class Thing 521 at 171,48 takes it as target): a consequence of the player records and owners that diverged before (player 2 from 413, player 3 from 416 - the camera-log bytes +0x3fc..+0x403 of a wizard that flies differently - player 0 from 458, player 1 from 513). End of the movie: 989 divergences (as many as at 512). |
| level00 | 41 | 13 (creature 1/7 across y = 0) | **Boundary.** Creature 37 (14/85) at 54,59 targets Thing 7 at 253,251: the shortest way crosses both seams (about 4 / 8 cells in the original, 199 / 192 in the tiled world). Ticks 73-80: the same for 8 more 14/85 creatures with target 7. From tick 1120, creatures that had left tile (0,0) earlier (tile (1,3), cell 289,863) diverge: every neighbour they had in the original is in tile (0,0). |
| level01 | 2010 | 544 | **Half-world line, not the 255 -> 256 boundary.** Vultures 67 / 202 (1/9, `creature_follow_leader`) at x = 0x7fdb .. 0x8001 (cell 127.9 .. 128.0), y 236. The separation test compares coordinates that were sign-extended before the subtraction (`creature_common.cpp:362`). In the original, the test therefore misses across x = 0x8000. In the port that line is `world_signed`'s centre, so in a 1024 world it lies at cell 512. A pair straddling cell 128 turns away in the port and not in the original (`target_yaw` 789 vs 1070). This is the kept quirk generalised (see "Non-boundary divergence classes"), not a round 11 bug. From 2130: the same creatures at y 252 .. 269 (the boundary). |
| level12 | 2 | 2040 | **Scaled AI rule.** AI wizards 92 / 96: `home` (castle site) from the quadrant rule. Tick 122: the original builds the AI castle at 171,75 and the port does not (consequence). First boundary divergence: 123, effects 310 / 311 (45/53) at 54,8 / 57,1. |
| level16 | 2 | 5 (creature 2/13 into cell 256,201) | Scaled AI rule (`home` of wizards 327 / 331). Then the boundary at tick 4: creature 251 at 0.08,203 (edge 0), and a flock of 2/13 / 2/15 creatures at x = 252 .. 257, y ~200 crossing x = 0. |
| level24 | 2 | 40 | Scaled AI rule (wizards 528 / 553 / 578: `home`). Wizard 553 then flies to another site, and the effects 2/2 next to it (slot 29 at 99,85, tick 13) and its castle (slot 28, tick 16) follow. Boundary from tick 2 (wizard 578 at 0,255.f0, spell 581). |
| level38 | 2 | 10 | Scaled AI rule (wizard 492 at 0,255.f0: `home`). **Boundary** from tick 4: wizard 486 at 1,4 targets 383 at 0.8,8.8 (the original's 389 is at 255.8,1.8, across). Mana balls at 255,1 / 0,8; creature 329 (13/79) at 1,187 targets 15 at 7,181 vs the original's 14 at 253,181. |
| level44 | 49 | 137 (fireball across y = 255) | **Boundary.** AI wizard 160 at 207,42: the original's target 42 at 26,40 is 75 cells away across x = 0 (181 cells in the tiled world); the port takes 46 at 200,212. Ticks 137-169: effects 2/2 along y = 251 .. 255. |
| level49 | 10 | 2 (creature 11/67 across y = 255) | **Boundary.** Creature 446 (5/31) at 96,1: the original's target 87 at 112,245 is across y = 0. Ticks 14-15: mana balls at 209,105 (owners from that chain). Tick 30: creature 361 at 248,74, whose original target is 49 at 8,95 (across). |
| gen (69 levels: generation + tick 1) | - | - | **29 / 69 identical.** **29 levels: scaled AI rule** (`home.x` / `home.y` of an AI wizard: 16384 / 49152 in the original, 0 in tile coordinates of the 1024 world). **10 levels: a target pair across the boundary** (14/85 or 13/79 creatures and AI wizards whose original nearest target is reached across x/y = 0: levels 2, 3, 7, 28, 31, 40, 45, 69, 22 (`parent` at cell 8,1) and 38). **Level 46:** `z` of a segment creature 8/49 at 113,255 (edge 0). The tile-0 maps vs the tick-2 snapshot differ in 7 levels (23, 34, 35, 38, 40, 43, 45). Their first differing cells (0,30 / 0,247 / 157,249 / 255,0 / 253,140 / 61,0 / 67,253) are at the boundary: footprints / terrain effects of the first tick cut by y or x = 0 land in the far tile. Maps identical at 256 (baseline run). |
| player (4 recordings) | p1 41, p2 2, p3 49 (as level00 / level38 / level44); **p4_quicksave_l1: 400** | - | p4: the recording quick-loads at tick ~400. In a world wider than 256 cells, `demo_load_state(10000)` refuses the original's GameState-only quick save (demo.cpp: it holds no maps to go back to 256 with), so the port plays on where the original went back. This is a known limitation, not a boundary effect. |
| N = 512, movie 0 | 413 | 415 | Up to tick 548 the same (slot, first tick) set as at 1024. The gate passes at 512 too. |

### Non-boundary divergence classes (checked: none is a round 11 bug)

1. **The AI castle site, a deliberately scaled rule** (`ai_wizard.cpp ai_find_castle_site`, task C). The 4 x 4
   quadrant grid is a quarter of the *world* per axis. In a tiled 1024 world, the corner `(q & 3) * quad` is
   `k * 65536`, whose low half is 0, where the original has `k * 0x4000`. This gives the `home.x / home.y` divergences
   in tick 1-2 of 29 / 69 generated levels and in 4 of the 8 playbacks, and the castle built elsewhere. It is
   identical at 256 (the references pass) and is the intended big-world behaviour.
2. **Sign-extension quirks whose line moves from cell 128 to cell N/2.** Several places keep the original's
   "operands sign-extended before the subtraction" by centring each coordinate with `world_signed` (the agents'
   documented choice):
   - `creature_common.cpp:362` (follow-leader separation, level01's first divergence)
   - `creatures3.cpp:508`
   - `thing.cpp:778` (`pos_dist_manhattan`)
   - `spatial.cpp:72` (`area_cells_slot0`'s truncating window: the one-cell eastward shift for the upper half)
   - `ai_wizard.cpp:644` (start quadrant)

   At 256 these are exact. In a tiled world, a pair or Thing near cell 128 behaves differently from the original.
   This is not a bug against the decision taken, but a **game-design question for N > 256**: keeping a "seam" at
   N/2 in a big world has no purpose. If wanted, the natural form is the torus difference for N > 256, for example in
   `creature_follow_leader`:

   ```cpp
   int dx = world_wide() ? world_signed(t->x - o->x) : world_signed(t->x) - world_signed(o->x);
   ```

   (and the same for dy and in creatures3.cpp:508). It is identical at 256 and has no quirk line in a wide world.
   Not applied (not my files); for the integrator's range-scaling decisions.
3. **The quick load refused in a wide world** (p4), by design (demo.cpp / savegame).

Every other first divergence was checked through its `where` line: the Thing, its owner or its target is at the old
boundary, outside tile (0,0), or reached across it.

## ctest `reference_world`

```
reference_test <game> extracted/reference/movie0 548     MC_REF_WORLD=1024  MC_REF_WORLD_GATE=16
```

Movie 0 cannot be "identical up to the first boundary interaction": the first one is in its first tick (the AI
castle at (0,0), the local wizard at x = 2), so that window holds zero ticks. The gate is therefore the boundary rule
above, up to **tick 548**. That is the last tick before the first divergence that is a consequence away from the
boundary (549); 136 ticks are compared. Result: `OK: MC_REF_WORLD gate (boundary 16 cells): 59 divergence(s), 0
slot(s) away from the old boundary, 0 map cell(s) away from it, RNG identical`. The map dump of tick 500 has 60
terrain cells differing, all within 3 cells of the boundary. Running to 549 or later fails with the `UNEXPLAINED`
lines, so the rule really discriminates. It runs in a few seconds in Release.

## world_render_test: parts 7 and 8

Terrain only (cell lists emptied), the tiled 1024 world (kWin = 256):

| check | result |
|---|---|
| dd 127: 4 cameras (inside, across x seam, across y seam, corner) x lod 0 / 1 / 2 x 6 tiles ((0,0) (1,0) (0,1) (2,3) (3,3) (3,0)), each compared with tile (0,0) | **72 frames, 0 differ** |
| dd 127 lod 0 in tile (0,0) vs the 256 world's dd 127 lod 0 (no coarse level, so both radii are 127) | **0 of 4 differ** (at lod >= 1 the 256 world's radius is pulled in by the coarsest LOD size, 126 / 120, so no comparison there) |
| dd 255, seam camera (tile (3,0)) and corner camera (tile (3,3)), raised 0x800, lod 0 / 1 / 2: radius / cells drawn / drawn twice / far ring (cell centres in [R - 12, R]) / beyond 200 cells | lod 0: 255 / 57122 / **0** / 5210 / 22106; lod 1: 252 / 56981 / **0** / 5180 / 21842; lod 2: 248 / 56650 / **0** / 5148 / 21194 (corner: 57124..57191 drawn, 0 twice, ring 5137..5209). The farthest cell is at most radius + 10 (a coarse quad's corner) |
| generated 1024 world (seed 1, 3 bots, template 62), dd 255 lod 2, 960x600, camera 0x3000 above player 0's seat looking at the world centre | radius 252, 69046 cells drawn, **0 twice**, far ring 6295, 26417 beyond 200 cells, 117 colours, game state unchanged (checksum); `w1024_gen_dd255.ppm` |

**Frame time** (1280x720, Release, one raster thread, median of 5, camera in tile (1,1) raised 0x800; the machine is
shared, so the numbers vary by +-20 % between runs):

| lod | dd 127 | dd 255 | ratio | triangles 127 -> 255 |
|---|---|---|---|---|
| 1 (default) | 6.1 ms quiet (build 3.2, raster 2.5); 9.0 - 11.5 ms under load | 13.2 ms quiet (build 8.5, raster 4.5); 17.9 - 22.6 ms under load | x 2.0 - 2.2 | 20161 -> 50918 |
| 2 | 6.1 ms quiet; 8.7 - 9.9 ms under load | 9.1 ms quiet; 11.9 - 12.8 ms under load | x 1.3 - 1.5 | 17789 -> 27982 |

dd 255 costs about 2x at lod 1. At lod 2 the coarse rings keep it to +30-50 %. A fourth LOD level (16x16 beyond
D0 * 8) would flatten lod 1 as well, as task E noted.

Images looked at (scratch `round11_G/wr/*.png`):
- `w1024_gen_dd255`: the generated world to the horizon, with rivers, lakes, a canyon, trees and the horizon
  curvature; nothing missing at the edge of the 252-cell radius.
- `w1024_dd255_seam`: level 38 tiled, the 248-cell view across the N*256 -> 0 seam, continuous.
- `w1024_dd127_corner_tile33`: the corner of both seams.

**`render_ext_test` needs nothing.** It never widens (every level load sets the world back to 256), and its static
`counts[65536]` cell-count buffer and its dd 20..127 cases are the 256 world's. Any test that widens and uses the
cell-count hook must size it `g_world.cells`, as `world_render_test` does.

## Gate

Build: build_G, Release, `--parallel 2 -p:CL_MPCount=1`, the whole tree, **0 warnings**. Full `ctest -C Release -j 2`:
**68 / 69 pass**, including:
- `reference_world`
- `reference_test` (movie 0), `reference_levels`, `reference_gen`, `reference_player`
- `world_render_test`
- every render reference

The one failure is the integrator's new, untracked `world_net_test` (`world_net_test.cpp:280 awake == far_n (5 != 6)`,
part of the B6 wake-rule work in progress in the tree at the time). It also fails when run alone, and it is not
touched by this task. The B6 wake rule is under `mode_active()`, so it does not affect any reference run.

## Notes for the integrator / later work

- No shared-file change is needed for this task.
- Optional design change (class 2 above, exact code given) if the sign-extension seams at N/2 should go away in big
  worlds.
- `MC_REF_WORLD` works for every reference set (HW / 1995 sets not run here). The quick-load recording (p4) cannot
  match in a wide world by construction.
- `tools/reference/compare_reference.py` needs no change. The new output lines start with `MC_REF_WORLD`, `  where`
  or `  UNEXPLAINED` and appear only with the variable set.
