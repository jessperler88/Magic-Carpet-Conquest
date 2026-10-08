# Port round 11 briefing (2026-10-08): Phase 4 round 2 - the big world, session 1 of 3

Read first: `docs/port/PORTING.md` (conventions, report format), `docs/ROADMAP_PHASE4.md` sections 2 and 3 / round 11,
`docs/analysis/phase4_code_world.md` (the study this round implements: sections 1.3, 3, 4.1, 5, 7 - it lists the
256-dependent sites per file with line numbers; the lines have moved a little since), then **`src/mcengine/world.h`**
and **`src/mcengine/mc_image.h`** (the API you sweep onto), then the files of your task.

## Round 11 in three sessions

| session | content |
|---|---|
| **1 (this one)** | B1 + B2 + B3: int32 positions, original-layout images, world descriptor and accessors (integrator, done), **the call-site sweep (tasks A-E below)** so that every subsystem works in an N-cell world (N = 512 .. 4096, power of two); the per-tick references stay byte-identical at N = 256 |
| 2 | B4 generator on N (int32 fractal / coarse lattice, rivers scaled), B5 seeded feature placer (`worldgen.cpp`, `WorldParams`), `mcport rts --size N` |
| 3 | B6 wake-near-any-wizard under the mode flag, B9 `world_test`, `MC_REF_WORLD`, two-instance sync at 1024, the round gate |

## What the integrator already did (commit `b8cf714` on branch `round11`, 60/60 ctest, references identical)

- **`WCoord`** (`mc_types.h`): `Thing.x / .y / .home.x / .home.y` and `Pos.x / .y` are int32 whose every store wraps
  into `0 .. N*256-1` (`g_world_unit_mask`). At N = 256 a WCoord is exactly the original's u16 field (same value
  after every store, same int promotion on every read). `Thing` is now 0xac bytes, `Pos` 10.
- **Images** (`mc_image.h`): `ThingImage` / `PosImage` / `GameStateImage` are the original's byte layouts (one field
  list, templated on the coordinate type). `state_to_image` / `state_from_image` / `thing_to_image` /
  `thing_from_image` / `state_image_read`; `ThingHi` carries the high 16 bits of the four coordinates. Movies
  (`gam*.dat`, `gax`), save states (`GAME` / `POOL` chunks), `net_state_checksum` (+ the high halves when N > 256),
  the reference tests and the original's unchecked over-reads go through them. `thing_load_snapshot_image`.
- **`world.h`**: `g_world` (size, shift, cell_mask, units, unit_mask, cells), `world_set_size(n)` (maps are now
  **pointers**: the static 256 arrays, or one heap block for a wider world; `sizeof g_map_type` is 8 now - use
  `g_world.cells`), accessors, `world_signed`, `world_dist_sq` / `world_dist_sq3` (int64; the original's int32 wrap
  at 256), `world_isqrt`, start positions (`world_start_pos` / `world_set_start_pos`) and the camera log
  (`world_log_x/y`, `world_log_set_xy`) with their high halves in `g_world_hi`, and **`world_widen_tiled(n)`**:
  the loaded 256 level repeated as an n-cell world (Things stay in the first copy, relinked) - your test bed.
- Core helpers already converted (thing.cpp): `terrain_sample_height(int32, int32)`, `terrain_height_at_offset`,
  `terrain_cell_flag_bit`, `terrain_type_mask_at`, `terrain_slope_vector`, `math_atan2_w`, `math_rotate_offset`,
  `pos_angle_to`, `pos_pitch_to`, `pos_dist_*` (**`pos_dist_sq_xy` / `_xyz` return int64 now**), `thing_link_cell` /
  `thing_unlink_cell`, `player_log_position`, `player_camera`, `player_spawn` / the flyer start constructors.
- Level reset calls `world_set_size(256)` (sim.cpp): every level is the original's world until session 2.
- `cmake -DMC_WARN_NARROWING=ON` turns C4244 back on (it is off project-wide): a diagnostic build lists every
  *implicit* narrowing (only ~10 in mcengine today). The bulk of the work is **explicit** casts, which no compiler
  flags: find them with grep.

## The sweep rules (world.h header, repeated)

| original | port (any N) |
|---|---|
| `mc_cell(cx, cy)`, the `cell_xy` clones | `world_cell(cx, cy)` (uint32 index) |
| `mc_cell_of(x, y)` | `world_cell_of(x, y)` |
| `(uint8_t)(cx + 1)`, `cx & 0xff` on a cell coordinate | `world_wrap_cell(cx + 1)` |
| `(uint8_t)(x >> 8)` | `world_cx(x)` |
| `cell & 0xff`, `cell >> 8`, `(cell + 0x100) & 0xffff` | `world_cell_x(cell)`, `world_cell_y(cell)`, `world_cell_step(cell, dx, dy)` |
| `for (i = 0; i < MC_MAP_CELLS ...)`, `0x10000` cell counts | `g_world.cells` |
| `(int16_t)(b->x - a->x)`, `(int16_t)p->x` | `world_signed(b->x - a->x)`, `world_signed(p->x)` |
| `p->x = (uint16_t)(expr)` | `p->x = expr` (WCoord wraps) |
| `uint16_t x = t->x` / `int16_t dx = ...` for a position / difference | `int32_t` |
| `dx * dx + dy * dy` (int32) | `world_dist_sq(dx, dy)` (int64), results stored in int64 |
| `math_atan2((int16_t)dx, (int16_t)dy)` of a world difference | `math_atan2_w(dx, dy)` |
| `0x10000` / `0xffff` as "the world's width in units" | `g_world.units` / `g_world.unit_mask` |
| 256 / 255 / 128 as "the world's width in cells" | `g_world.size` / `g_world.cell_mask` / `g_world.size / 2` |

Every rule is the identity at N = 256 (that is why the references stay exact). Keep the original's quirks where
they are not about the world size (an (int16_t) on a *height*, an angle, a speed, a z stays as it is; only x / y
positions, their differences and cell coordinates change). When a site is ambiguous, keep it exact for 256 and
write a short comment saying what it does for N > 256. Things you find that need a game-design decision for a big
world (e.g. a constant range that should scale) go into your report, not into code.

## Rules (as in rounds 6-10)

- **Five agents (A..E) work in the same tree at the same time.** Only modify the files your task names. A change
  you need elsewhere goes into your report as **exact code (file, place)**; the integrator applies it. Shared files
  nobody edits this round: `mc_types.h`, `mc_image.h`, `world.h`, `mc_globals.*`, `sim.*`, `sim_all.cpp`, `thing.h`,
  `engine.*`, `settings.h`, `mode.*`, `src/CMakeLists.txt`, `docs/ROADMAP*.md`, `README.md`, `docs/ENGINE.md`. New
  test files and their `.cmake` are yours (tests/*.cmake files are picked up by CMake automatically - reconfigure).
- **Faithful by default, the references are the gate.** With N = 256 every reference must stay byte / pixel
  identical: after your changes run the whole `ctest -C Release` in your build dir (60 tests, ~30 s with `-j 8`).
  If a test fails in a file you do not own, another agent is mid-edit: wait and rerun; never edit it.
- **Determinism**: integers only in the tick, nothing new reads the local player, the camera or real time.
- **Bash pitfall (bit the integrator again in this round's first hour):** never edit source with python / sed /
  cat in a Bash heredoc - `\n` reaches the file as a real newline and `replace()` calls silently miss. **All
  source edits through the Edit / Write tools.** Throwaway scripts go in your scratch dir via Write and run with
  `python <file>`; never run python with `C:\Magic Carpet` as the working directory (`ghidra/` shadows a package).
- Build in **your own build directory** `build_X`: `cd "C:/Magic Carpet/src" && VCPKG_ROOT=C:/vcpkg cmake --preset
  msvc-x64 -B ../build_X` (once; reconfigure after adding a test), then `cmake --build ../build_X --config Release
  --parallel` and `cd ../build_X && ctest -C Release -j 8`. **Zero warnings (/W4).** Never build in `build/`.
- Scratch: `<scratchpad>/round11_X/`. Headless mcport runs: `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
  MC_SAVE_DIR=<your scratch>`. TCP listeners in tests bind `127.0.0.1` (`MC_NET_BIND`), or the user gets
  Windows Firewall prompts.
- The original game files under `MagicCarpet/` are never modified. GPL: no code from remc2 / mgcarpet / DOSBox.

## Verification of the wide world (every task)

Write `src/tests/world_<x>_test.cpp` + `.cmake` (`mc_unit_test` over `${MC_SIM_ALL}` or the sources you need) that
loads a level (e.g. 38 via `sim_load_level`), calls `world_widen_tiled(1024)` and checks your subsystem in the
wide world. The tiled world is a strong oracle: **any terrain-only query at (x + k*65536, y + m*65536) must give
what the 256 world gives at (x, y)**, and a Thing that crosses x = 65535 -> 65536 must move on smoothly (cell lists,
distances, angles, collisions across the old boundary and across the new seam at N*256 -> 0). Also probe distances
and angles between Things 300-500 cells apart (no int32 overflow, no torus-alias error) and run the sim for a few
thousand ticks in the tiled world twice in one process with identical `net_state_checksum` sequences. Exit 0 =
pass. Look at any image you produce.

## Deliverable

Your edits, your test (exit 0, zero warnings), all 60 existing ctest gates passing, and the report
`docs/analysis/port_world_<x>.md`: sites converted per file (counts and the non-mechanical ones explained), what
the wide-world test checks and its numbers, deviations / open sites (anything still 256-only and why),
**requested shared-file changes (exact code)**, and what sessions 2 / 3 must know. Final message: a short summary.

## Tasks

### A - terrain generation passes, painting, features (`terrain_gen.cpp`, `terrain_paint.{h,cpp}`, `level_features.{h,cpp}`, test `world_terrain_test`, report `port_world_terrain.md`, build `build_A`)

- `terrain_gen.cpp`: every local pass (flatten water quads, the six class passes, holes / spikes / shore quads,
  `terrain_assign_textures`, `terrain_mark_water_anim`, `terrain_build_lightmap`, the snow pass, the `H/HR/F/FR/T/S`
  helpers, `cross_range` / `block_range`) through the accessors over `g_world.cells`. **Leave the fractal fill,
  `terrain_generate`'s normalisation and the river source pick (`rng % 0xffff`, 1000 tries) 256-only** (session 2
  replaces them for N > 256): make them explicitly 256 (a comment + use `MC_MAP_CELLS` / the 256 constants where
  they are), so nothing silently half-works. Note: the fractal reuses `g_cell_things` as 16-bit scratch.
- `terrain_paint.cpp`: paint / retexture / smooth / the spiral cell walk (`spiral_search_begin` origin and ring
  limit) through the accessors.
- `level_features.cpp`: footprints, walls / paths / canyons / ridges, castle site tests and stamps, the `cell_xy` /
  `thing_cell_x/y` clones, `math_wrap_diff` uses (modulus = the world size where it means the world).
- Test: in a tiled 1024 world, the class / texture / light passes run on the tiled maps give the tiled result of
  the 256 run; a castle footprint stamped across x = 1023 -> 0 and across the old 255 -> 256 boundary equals the
  same footprint stamped at the same offset inside a tile; spiral searches around a seam cell visit the same
  relative cells as in the 256 world.

### B - Thing core, spatial index, effects, castles, constructors, spells, scenery (`thing.cpp` (the rest; the converted core helpers stay), `spatial.{h,cpp}`, `effects.{h,cpp}`, `castle.{h,cpp}`, `constructors.{h,cpp}`, `spells.{h,cpp}`, `scenery.{h,cpp}`, test `world_things_test`, report `port_world_things.md`, build `build_B`)

- `spatial.cpp`: `walk_cell`, `spiral_find`, `area_cells_slot0`, `thing_exists_near_pos`, `thing_area_damage`,
  `terrain_max_corner_level`, the pending-damage searches - every `& 0xff` / `mc_cell*`.
- `effects.cpp`: castle collapse / raise, terrain effects (craters, ridges, volcano, earthquake...), the `cell_xy` /
  `thing_cell_x/y` clones; `castle.cpp` growth rect and courtyard tests; constructors' and spells' position
  arithmetic (`(uint16_t)` writes, `(int16_t)` differences), `pos_dist_sq_*` results stored in `int` -> int64.
- `thing.cpp`: whatever is left (`thing_create` spawn positions, `level_*` spawning from ThingInit cells, the
  remaining `mc_cell*`, `demo_repair`-style walks if any, `thing_find_in_sight_of_class`).
- Test: spiral / area queries and area damage across both seams hit the right Things; an earthquake / crater /
  castle raise across x = 1023 -> 0 changes the right cells; a projectile or meteor flying across the seam keeps
  a continuous track; distances / angles 500 cells apart.

### C - creatures, wizard AI, projectiles (`creature_common.cpp`, `creatures.{h,cpp}`, `creatures2.{h,cpp}`, `creatures3.{h,cpp}`, `ai_wizard.{h,cpp}`, `projectiles.{h,cpp}`, test `world_creatures_test`, report `port_world_creatures.md`, build `build_C`)

- Every `(int16_t)` torus difference, `(uint16_t)` position write, `dist_sq_xy16` / `sight_sq` (int64), nearest-target
  searches (`best_d` in int64), wander targets, segment (worm / dragon) follow arithmetic, villager / builder
  castle placement offsets (`creatures.cpp` ~650-720, `creatures3.cpp`).
- `ai_wizard.cpp`: `ai_goal_build_castle`'s **4x4 grid of 64-cell quadrants** (`t->x / 0x4000`, `(qx & 3) << 14`,
  ...) generalised: quadrant = world / 4 per axis (`g_world.units / 4`), identical at 256; the squared-range
  constants stay (they are game ranges), comparisons in int64; aim / lead arithmetic.
- The creature **wake** rule (`creature_proximity_wake_timer`, distance to the local player) only gets the
  int64 / `world_signed` treatment now; the deterministic wake-near-any-wizard is session 3 - do not change it.
- Test: a creature (pick a walker and a flyer) and an AI wizard placed near x = 65535 and near N*256-1 walk / fly
  across and keep their targets; an AI wizard 400 cells from its target computes the right angle (not the torus
  alias of a 256 world); the castle-site quadrant search in a 1024 world returns sites in all 4x4 quadrants of the
  big world.

### D - player, input, debug commands, sound, persistence, checksum (`player.{h,cpp}` (except the camera log / start-position code already converted), `input.{h,cpp}`, `debug_cmd.{h,cpp}`, `scenario.{h,cpp}`, `sound.{h,cpp}`, `demo.{h,cpp}`, `savegame.{h,cpp}`, `net.cpp` (the checksum part only), `mode_dump.{h,cpp}`, `replay_check.{h,cpp}`, `mode_level.{h,cpp}`, `mcport/rts_run.{h,cpp}`, test `world_persist_test`, report `port_world_persist.md`, build `build_D`)

- Position arithmetic in `player.cpp` (flyer movement, push, tether, possession, castle home), `input.cpp`,
  `debug_cmd.cpp` (teleport / spawn take int32 coordinates; packet layout: document what a wide teleport needs -
  the 10-byte packet may not hold two int32; propose), `scenario.cpp` (positions in asserts / queries), the
  sound cull (`0x9000000` from `pos_dist_*`) and pan.
- **Wide-world persistence**: save states write a `WRLD` chunk when N > 256 (size, the five maps at N*N, the
  `ThingHi` of every slot (GameState and pool extension), `g_world_hi`) and refuse / handle a missing one; the
  `MAPS` chunk stays the 256 format for the original world. Movie v3 snapshots (`gax` + `max`): the same for N >
  256 (design it, document it in demo.h; v1 / v2 / original unchanged). `sim_load_snapshot` stays 256. The
  checksum's map parts hash `g_world.cells` cells; `net.h` part names unchanged. `mode_dump` JSON: world size,
  positions as int32.
- Test: a tiled 1024 world: save state at T, play N ticks, load, play N again -> identical checksums incl. the
  high halves; a Thing moved to x = 900 cells survives the round trip; the checksum changes when only a high half
  changes; a recorded v3 movie of a 1024 run replays identically (if `rts` can start a tiled 1024 world through
  `mode_level` - add a `ModeParams.world_size` path there that loads the template map and calls
  `world_widen_tiled(size)` after the level start, behind the mode flag; report the hook placement you need from
  the integrator if it must live in sim.cpp).

### E - renderers, HUD, map screen, debug overlays (`render_ext.{h,cpp}`, `render_ext_raster.cpp`, `render_landscape.cpp`, `render_things.cpp`, `render.h` (no layout changes), `hud.{h,cpp}`, `debug_overlay.{h,cpp}`, `debug_camera.h`, `mcport/inspect_tool.*`, `mcport/main.cpp` (only the debug-camera / viewer coordinate code), test `world_render_test`, report `port_world_render.md`, build `build_E`)

- `render_ext.cpp` (step B7): `kWin = min(N/2, 256)` (and the vertex cache sized to it), the camera no longer
  truncated to u16 (`F.cam_x = (uint16_t)...`), `fx = cam_x & 0xff`, `slope_camera`, every `mc_cell` / `(uint8_t)`
  cell walk, `thing_pos` interpolation deltas (`world_signed`); draw distance up to `min(dd, kWin - smax)` -
  report what dd 255 would need. `render_landscape.cpp` (faithful renderer): the cell walk through the
  accessors so it draws correctly in a wide world too (pixels at 256 unchanged: `render_reference*`).
  `render_things.cpp`: projection deltas.
- `hud.cpp`: `ui_draw_radar` / `ui_draw_map` cell lookups, radar blips (`(int16_t)(T->x - cam_x)`), the map screen
  (scale `0xaa` over `0x17e` px = the whole 256 world: for N > 256 show a window around the player at the same
  scale, or the whole world scaled down - pick one, explain), the castle line; the camera log readers via
  `world_log_x/y`. `debug_overlay.cpp` / `inspect_tool`: grid / labels / pick-ground across the seam, positions
  printed as int32. Debug camera (`debug_camera.h`, main.cpp free camera): coordinates wrap at `g_world.units`.
- Test: in a tiled 1024 world with the Things hidden (or a terrain-only render), the extended renderer at the
  same camera offset inside different tiles and straddling both seams produces **pixel-identical** frames to the
  256 world at (x mod 65536); the radar too. Faithful renderer the same at its 20-cell view. Write the PNGs to
  your scratch dir and look at one seam frame.

## Session 2 (2026-10-08, done by the integrator alone - no agents): the generated world

B4 + B5 in one coherent piece (generator and placer depend on each other: plateaus before rivers, features after
the class passes), so no agent split. Result and design: `docs/analysis/port_world_gen.md`. What later work must know:

- **Entry point**: `sim.cpp level_start_from_state` calls `g_hook_level_world` (worldgen.h) before `terrain_build`;
  `mode_level.cpp` installs it; `worldgen_build(params, &g_state->level)` sizes the world, generates the maps and
  *replaces* `level.things` with placed records, which the original's level start then spawns. Nothing else knows a
  world was generated: save states / movies store maps and records; a v3 movie regenerates from its ModeParams.
- **Which runs generate**: `mode_params_generated(p)` (mode.h): world_size > 256 unless `MODE_PARAM_TILED`
  (`--tiled`, the session 1 test bed), or 256 with `MODE_PARAM_GENERATE`. Template mode levels and the campaign are
  untouched (references identical).
- **WorldParams** (mode.h, 8 bytes in ModeParams where round 10 had reserved zeros; 0 = default): symmetry, roughness,
  water, continent, springs, wild, lairs, features (triggers | portals nibbles). Full - grow it with a ModeState v2.
- **Mode-run rule changes**: switches fire for computer wizards (`scenery.cpp`); a generated world's pool is at least
  `mode_generated_world_slots(N)` = 16384 (32768 from 2048 cells; `thing.cpp thing_pool_wanted_slots`). Scenery and
  wild creatures go into `level_extra_records_set` (thing.h), not the level file's 1999 records. Draw distance up to
  255 in a wide world (ini 8..255, pause menu, render_ext caps 127 at 256 cells).
- `worldgen_last_layout()` = what was placed where (seats, springs, lairs, triggers + DisIds, portal pairs, towns,
  groves); not saved. Tests: `world_gen_test` (PNG maps in `MC_WORLD_GEN_OUT`).
- Options: `rts --size N --symmetry S --water % --rough % --continent % --springs N --wild % --lairs N --triggers N
  --portals N` (`none` for the counts); scenario header `rts ... size N symmetry S water % rough % triggers N portals N
  [tiled] [generate]`.
