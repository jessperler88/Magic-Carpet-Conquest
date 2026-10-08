# Round 11 task A: terrain generation passes, painting and features in an N-cell world

Agent A, 2026-10-08, branch `round11` (session 1 of 3: the call-site sweep). Files: `src/mcengine/terrain_gen.cpp`,
`terrain_paint.{h,cpp}`, `level_features.{h,cpp}`, new test `src/tests/world_terrain_test.{cpp,cmake}`. Build dir
`build_A`. Briefing: `docs/port/BRIEFING_round11.md`.

**Result.** Every local generation pass, the run-time painting / retexturing / spiral walk and all level
features (footprints, walls, paths, canyons, ridges, castle site tests and stamps) now address cells through
`world.h` and run on any N x N torus. At N = 256 every converted expression is the original's (all 60 ctest
gates pass, references byte / pixel identical). The three generator stages that only make sense for 256 cells
(fractal fill, normalisation, river source pick) stay 256-only on purpose, and `terrain_build` refuses a wider world.
`world_terrain_test` passes (exit 0, zero warnings, 25-35 s).

## 1. Sites converted

### terrain_gen.cpp (~45 sites)

| what | change |
|---|---|
| `H / HR / F / T` helpers | `mc_cell(x, y)` -> `world_cell(x, y)`, int arguments (the unused `FR` dropped) |
| `cross_range`, `block_range`, `steep_to_cliff_edge` | int coordinates; `steep_to_cliff_edge(i)` derives x / y itself |
| 16 sweeps `for (i < MC_MAP_CELLS)` with `x = (uint8_t)i, y = (uint8_t)(i >> 8)` | `for (i < g_world.cells)`, `world_cell_x(i)`, `world_cell_y(i)` (flatten water quads, classify_flat x2, mark_lowland, insert_transitions, mark_interior, mark_steep x2, mark_snow x2, fill_holes, smooth_spikes, fix_shore_quads, assign_textures, mark_water_anim, build_lightmap) |
| quad index arrays `uint16_t q[4] = {mc_cell...}` (classify_flat, insert_transitions) | `uint32_t`, `world_cell` |
| 7 memset / memcpy of `MC_MAP_CELLS` bytes (mark_lowland / interior / steep / snow copies, river visited map, `g_map_type := 0xff / 0`) | `g_world.cells` |
| `terrain_trace_river` | cell / cursor / best `uint16_t` -> `uint32_t`, neighbours via `world_cell` |
| `terrain_carve_rivers` | class reset over `g_world.cells`; the source pick is **unchanged** (`rng % 0xffff`, 1000 tries) with a 256-only comment |
| `terrain_build` | the two `g_cell_things` memsets over `g_world.cells`; **new guard**: `if (world_wide())` prints to stderr and returns (see 4.) |
| `terrain_fractal_fill`, `S()`, `terrain_generate` | **unchanged, documented 256-only** (8-bit coordinates, eight levels, 16-bit noise, `MC_MAP_CELLS`) |

### terrain_paint.cpp / .h (~45 sites)

- The private `cell_xy(uint8_t, uint8_t)` is now `world_cell(int, int)`. The new `cell_wrap(c) = c & (cells - 1)`
  replaces the original's `& 0xffff` on cell arguments (`terrain_paint_cell`, both `terrain_retexture_rect*`,
  `terrain_cell_is_nonland`).
- `terrain_slope_orientation`, `terrain_paint_cell` (the checker parity is `(x + y) & 1`, which needs no byte wrap),
  `light_slope`, `terrain_set_quad_texture`, `clear_class_if_surrounded`, `terrain_modify_cell`,
  `terrain_set_cell_height`: x / y come from `world_cell_x/y`, and every `(uint8_t)(x +- 1)` is gone (`world_cell` wraps).
- `retexture_rect` (non-mechanical): the width / height are the original's **byte** differences
  `(uint8_t)(x1 - x0 + 1)`. They are now `world_wrap_cell(...)`, so a rectangle across the seam works, and the
  `w == 0` / `w + 1 == 256` behaviour of the byte counters is kept at N = 256 (every `w++`, `cx++`, `cx - w` is
  wrapped). The loop counters became `int`, and since the counts are already wrapped they behave like the original's.
- `spiral_origin`: `((int16_t)t->x + 0x80) >> 8` -> `(world_signed(t->x) + 0x80) >> 8`. The centre is the
  signed coordinate's cell (-N/2 .. N/2 - 1), and `terrain_modify_cell` wraps centre + offset. The original's
  `origin = (int16_t)x == 0 && (int16_t)y == 0` test of the *argument* is kept. For the spiral callers the
  argument stays within |N/2 + 16| < 32768, so the test still means world cell (0, 0) (comment in the code).
- Header comment: cells are `world_cell` indices.

### level_features.cpp / .h (~95 sites)

- Clones: `cell_xy` -> `world_cell`, `thing_cell_x/y` -> `world_cx(t->x + 0x80)`. That is identical at 256:
  `((int16_t)x + 0x80) >> 8` and `(x + 0x80) >> 8` agree mod 256. New `wrap()` = `world_wrap_cell` and `cell_wrap()`.
- `footprint_walk`, `strip_built`, `terrain_rect_min_height`, `terrain_smooth_rect`,
  `terrain_smooth_castle_border`, `castle_stamp_footprint`, `effect_castle_build_update`,
  `effect_wizard_castle_collapse_update`, `effect_wizard_init`, `castle_footprint_clear`,
  `castle_site_clear_at_pos`, the four wall / path updates: the `uint8_t` cell cursors became `int` wrapped with
  `wrap()` wherever the original's byte wrapped. The u16 *counts* (rows / cols / thickness, "a negative count
  walks 64K cells") stay u16.
- `iabs16(c->x - t->x)` -> `iabsw` = |`world_signed`(difference)| (castle_near_thing, castle_crush_wizards,
  castle_site_clear_at_pos: 6 sites).
- Position writes: `(uint16_t)(x0 << 8)` -> `(int32_t)((uint32_t)x0 << 8)` into the WCoord (terrain_set_pos_scratch,
  level_build_path x2, canyon / ridge x4, level_spawn_effect_record). The WCoord wraps, and a negative or u16
  cell number lands on the same cell mod N because N * 256 divides 2^24. `((int16_t)t->x >> 8) << 8` -> `t->x & ~0xff`.
  Collapse spawn position `(cell & 0xff) << 8` -> `world_cell_x(cell) << 8`. The villager / archer spawn offset
  `(uint16_t)(x + (uint16_t)ext_x)` -> `x + ext_x` (same value mod 65536, and correct in a wide world).
- `math_wrap_diff(.., 0x100)` -> `math_wrap_diff(.., g_world.size)` in `level_build_wall` / `level_build_path`.
  The function keeps its 16-bit arithmetic, which is correct for moduli <= 4096. No other file calls it.
- **Linear cell cursors** (non-mechanical, kept as quirks): `wall_row` / `effect_wall_east_update`'s `uint16_t c++`
  is now `cell_next(c) = (c + 1) & (cells - 1)`. `terrain_smooth_cell`'s `cell - 0x101 / - 0x100 / - 1` neighbours
  and `p += 0xfd` use row stride `g_world.size`, and `map_type_linear` still reads the tables image below cell 0.
  The canyon / ridge start height `((y & 0xffff) << 8) + (x & 0xffff)) & 0xffff` is now `height_linear`
  (shift `g_world.shift`, mask `cells - 1`). All of these are exactly the original at 256. In a wide world they
  generalise the original's linear-memory behaviour: the "x overflows into the next row" step happens at the
  world's x seam, which is where the 256 world has it.
- `effect_path_update`: the per-step `(uint8_t)(x + (uint8_t)yaw)` -> `wrap(x + (int8_t)(uint8_t)yaw)`. Same mod 256;
  in a wide world the -1 step is -1, not +255.
- `level_build_canyon / ridge`: `dist & 0xffff` -> `dist & g_world.unit_mask` (identical at 256; a torus distance
  never reaches the world width).
- Header: `math_wrap_diff` comment (modulus = `g_world.size` for cells), a note that cell arguments wrap on the torus.

## 2. What world_terrain_test checks (level 38, 256 vs the 1024 tiling)

1. **Generation passes.** The generator is replayed step by step in the 256 world, and the replay equals
   `terrain_build` (0 cells differ). Each local pass then runs on the tiling of its 256 input and is compared
   with the tiling of its 256 output:

   | pass | cells differing in 1M |
   |---|---|
   | flatten_water_quads, classify_flat, mark_lowland, insert_transitions, mark_interior, mark_steep, fill_holes, clear, fix_shore_quads, mark_water_anim | **0** |
   | smooth_spikes (in place) | 216, all within 1 cell of an old tile boundary |
   | assign_textures | 1,048,576 random cells, all a valid (texture, rotation) of their corner-class bucket |
   | build_lightmap | 0 on sloped cells; 177,776 flat random cells all in 0x1c..0x24 |
   | mark_snow (Hidden Worlds, snlin 0x50 / snflt 6) | 0 (950,320 cells changed) |

   `smooth_spikes` writes heights in place while it sweeps. In the 256 world, cell (0, y) reads (255, y) before
   that cell is smoothed. In the tiling, (256, y) reads (255, y) after it is smoothed. So it is the original
   algorithm on a larger torus, not a wrap error (the test bounds the differences to 8 cells from a boundary; the
   measured maximum is 1). The other in-place passes happen to match exactly on level 38.
2. **Features at the seams.** Each case runs in a fresh copy of the level, with the level's own effects flushed by
   `level_run_terrain_effects`. The terrain change is recorded as torus offsets from the anchor cell. Anchors:
   x seam (256: (252,120); 1024: (1020,376) across 1023 -> 0, (252,120) and (508,632) across old boundaries), y seam
   (60,251), corner (253,253) / (1021,1021), and inside a tile (130,90). The features are: a wizard castle (spawn
   record, built to completion including border smoothing), a player castle stamp (size 3), a spiral walk
   (`terrain_modify_cells_spiral` rings 0..9 plus `terrain_find_cell_spiral`), two walls, two paths, a canyon and
   a ridge. `castle_site_clear_at_pos` is also checked on a 13 x 13 grid around each anchor.
   - **Every world-seam case equals the 256 world exactly** (all 28 feature x anchor pairs).
   - Old-boundary cases equal the 256 world for the stamp, the spiral, paths, canyons and ridges. They differ
     only for the wizard castle (20 / 14 cells) and the wall (16 / 20 cells, plus the site test). These two use
     the original's linear cursors (above), which in the 256 world step from (255, y) to (0, y + 1). At an old
     boundary inside the wide world they step to the true neighbour (256, y). The two old-boundary placements equal
     each other, and y-seam cases match exactly (the linear quirk is horizontal only).
3. **Long segments.** The canyon / ridge builders in the 1024 world are checked over 400 cells (+x, -y across the
   seam, and a 565-cell diagonal across both seams). Yaw equals the 10-cell segment's (0x200 / 0 / 0x300), the
   length is 400 / 400 / 565 cells, and the ridge takes 100 / 100 / 141 steps. There is no torus alias and no
   u16 truncation.
4. **Determinism.** Two runs of 2000 ticks of the tiled 1024 world (`game_tick_sim`, AI wizards) give identical
   `net_state_checksum` sequences (b86ba739 .. 0d897338).

## 3. ctest

Full Release build in `build_A`: zero warnings (/W4). `ctest -C Release -j 2`: **65/65 passed**. That is the 60
existing gates (references byte / pixel identical at N = 256) plus the five new `world_*_test`s present in the tree at
the time, mine among them (25 s under ctest).

## 4. Deviations / open sites (still 256-only, on purpose)

- `terrain_fractal_fill` (+ `fractal_square_step` / `fractal_diamond_step` / `S()`): 8-bit coordinates, levels 7..0,
  16-bit noise, the first 0x10000 scratch entries. `terrain_generate`: `MC_MAP_CELLS` loops. `terrain_carve_rivers`'
  source pick: `rng % 0xffff`, 1000 tries (in a wide world it would only reach cell indices < 0xffff = the first
  64 rows of a 1024 world). Session 2 (B4) replaces them.
- `terrain_build` in a wide world prints `terrain_build: the generator only builds 256-cell worlds yet` and returns
  without touching the maps. A level reset always sizes the world to 256 first (sim.cpp), so no current path hits it.
- The linear-cursor quirks (wall rows, `terrain_smooth_cell`, canyon / ridge start height) are kept as linear-memory
  behaviour, with the row stride N and the "next row" step at the world's x seam (2.). If the Conquest mode prefers
  a quirk-free torus, swap `cell_next` for a row-wrapping step and the smooth-cell neighbours for `world_cell_step`
  under the mode flag.
- `terrain_modify_cell`'s clamp-at-(0,0) result stays a test of the argument (world cell (0,0) for the spiral callers).

**Game-design notes for a big world** (not in code): the river count (`GenMap.river`, 39 on level 44) and the
1000-try source pick should scale with area. The spiral walks are limited to 15 rings (search.dat) and castle
footprints to 256 sizes, which does not depend on N. A wizard castle's villager spawns use `x + ext_x`, fine on
any N.

## 5. Requested shared-file changes

None needed. My files compile against the shared headers as they are (`world.h`, `mc_image.h`, `thing.h`).

## 6. For sessions 2 / 3

- B4 can call the local passes unchanged on any N. They are verified on the tiling, and the costs are linear
  (13 passes on 1M cells run in well under a second each in Release).
- The new generator must fill `g_map_height` over `g_world.cells` itself (or call a widened fractal), pick river
  sources over `g_world.cells`, and then lift the `world_wide()` guard in `terrain_build` (or add
  `terrain_build(const GenMap &, const WorldParams &)` beside it).
- `level_spawn_effect_record` takes `ThingInit` u16 cells, so features can be placed up to 65535 cells. The
  builders take int cell coordinates and wrap them, and the segments compute headings / lengths without aliasing
  (3. above).
- `terrain_assign_textures` / `terrain_build_lightmap` consume `g_rng16` in cell order, so a wide world's textures
  depend on N (deterministic per N, fine for lockstep).
