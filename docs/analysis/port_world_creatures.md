# Round 11 task C: creatures, wizard AI, projectiles in an N-cell world

Files: `creature_common.cpp`, `creatures.cpp`, `creatures2.cpp`, `creatures3.cpp`, `ai_wizard.cpp`, `projectiles.cpp`
(headers `creatures*.h`, `ai_wizard.h`, `projectiles.h` needed no change: they carry no position type), new test
`src/tests/world_creatures_test.{cpp,cmake}`. Build dir `build_C`, Release, zero warnings. A diagnostic build with
`-DMC_WARN_NARROWING=ON` (`build_C_narrow`, my six objects rebuilt) reports **no** C4244 in these files.

## Gate

- `ctest -C Release -j 2` in `build_C`: 64 / 65 passed. **All 60 existing tests pass**, the per-tick references
  included (byte-identical at N = 256). The new `world_creatures_test` passes (84 s on the loaded machine). The one
  failure is `world_persist_test` (task D, being written while I ran).
- Every rewrite below is the identity at N = 256: `world_signed` is `(int16_t)`, WCoord stores wrap at 0xffff,
  `world_dist_sq*` keep the original's int32 wrap, and the unsigned 64-bit squared distances have the same value
  as the original's uint32 sums (at most 2^31).

## Sites converted

| file | sites | what |
|---|---|---|
| creature_common.cpp | 14 | `dist_sq_xy16` -> uint64 of `world_signed` differences, `sight_sq` uint64, `kFarSq`; `find_leader` and `creature_ai_step` searches (limit / best_d / d as uint64); `terrain_slope_at` (4 `mc_cell` + 4 `(uint8_t)` -> `world_cx` / `world_cell`); follow-leader separation (2); wake timer (dx, dy, d2, dy^2) |
| creatures.cpp | 25 | the same helpers; `nearest_in_list`; the archer's two searches; the skeleton's castle search ("anywhere": limit `kFarSq`) and enemy search; `villager_walk_to_wizard`; builder castle placement: 8 `(uint16_t)((int16_t)x + off)` writes -> WCoord sums, `sx, sy` -> int32, the `touches` differences -> `world_signed`; `skeleton_convert_villager` 3-D distance -> `world_dist_sq3` + `world_isqrt`; `terrain_rect_is_flat` cell arithmetic -> `world_cx` / `world_wrap_cell` |
| creatures2.cpp | 9 | helpers; `nearest_player_thing`, `nearest_leader`, the crab's mana-ball search |
| creatures3.cpp | 15 | helpers; `nearest_enemy_in_fov`; genie puffs (2) and teleport (2) -> WCoord sums; wyvern castle search; `genie_steal_mana`; `type15_move` cell-centre snap (2: `((int16)v >> 8 << 8) + 0x80` -> `(v & ~0xff) + 0x80`) and separation (2) |
| ai_wizard.cpp | 27 | `dist_sq_u` (unsigned view of `pos_dist_sq_xy`: the original's uint32 at 256, int64 as is otherwise) + `kFarSq`: 11 `best_d`, 13 `(uint32_t)pos_dist_sq_xy` -> `dist_sq_u`; **`ai_find_castle_site`** generalised (below) |
| projectiles.cpp | 2 | lightning: `g_pos_scratch.x/.y = (uint16_t)(... + delta)` -> WCoord sums |

The ones that are not mechanical:

- **Nearest searches** (about 40): the original starts `best_d` at 0xffffffff and some limits are 0xffffffff
  ("anywhere": skeleton -> enemy castle). Past 256 cells a squared distance is > 2^32, so with a uint32 start
  value far candidates would never be picked, and a truncated square ranks them wrongly. Both are now uint64
  with `~0ull`. No 256-world distance reaches 0xffffffff, so this is exact there.
- **`ai_find_castle_site`**: quadrant = `g_world.units / 4` per axis. The start quadrant is
  `world_signed(t->x) / quad`, which is the original's signed `(int16_t)x / 0x4000` with truncation (-2 / -1 / 0
  in the upper half). The corner is `(q & 3) * quad` and the centre cell `(q & 3) * (size/4) + size/8 - 1`
  (0x1f at 256). The 0x3000 Chebyshev clearance stays a game range. At 256 the result is the same 16 corners. At
  1024 the centres (127 cells in) are clear of the corners, so 32 sites.
- **Separation tests** in `creature_follow_leader` and `type15_move`: the original subtracts *separately
  sign-extended* coordinates (no wrap). This is kept as `world_signed(a) - world_signed(b)`, so it misses at the
  half-world line exactly as the original misses at x = 0x8000.
- **Wake timer** (`creature_proximity_wake_timer`): `world_signed` + `world_dist_sq` only. It is still measured
  against the local player (session 3 changes the rule). The `isqrt(dy^2)` argument is computed in 64 bits; it
  is at most 2^30 when the test passes.
- `terrain_rect_is_flat` passes wrapped int cell coordinates to `terrain_rect_height_range` (level_features.cpp,
  task A). The test's 16 000 tiled probes, which include the last row and column, show no difference.

## world_creatures_test (level 38, `world_widen_tiled(1024)`)

1. `terrain_slope_at` gives the 256 value at 16 000 probes in 4 tiles (last row and column included): 0
   differences. `terrain_rect_is_flat` also gives 0 differences (INFO only, because the function under it belongs
   to A).
2. Walks: an archer (walker) and a bee (flyer) are stepped by `creature_move_step` (terrain-only) for 220 steps
   east and south across 65535 in the 256 world. That gives 8 reference tracks (each moves more than 16 cells and
   survives). In the tiled world each track is replayed (a) across 65535 -> 65536, (b) across the seam
   N*256 -> 0, (c) inside tile (1,2): 24 runs. Each run matches its reference mod 65536 at every step (x, y, z,
   yaw, health), never moves more than one cell per step, ends in the next tile, and is in its own cell list at
   every step.
3. An archer in its attack state with a bee 3 cells away across the seam (0x3ff80 vs 0x180): the target and the
   state are kept for 64 / 64 ticks, and the archer aims east (target_yaw 0x200) on all 16 aim ticks.
4. AI wizard: with a target 400 cells east and 30 south, `ai_mode4_approach_target` gives target_yaw 0x218 (the
   true angle; the 16-bit alias would be 0x5ac). `player_find_nearest_by_type` picks the castle 200 cells west
   over the one 300 cells east, whose squared distance overflows 32 bits (both for type 2 and for 0xff).
   `ai_find_castle_site`: 256 world, 16 sites in all 16 quadrants of 64 cells; 1024 world, 32 sites
   (corners + centres) in all 16 quadrants of 256 cells, every one exactly a corner or a centre.
5. Determinism: level 38 tiled to 1024, 147 creatures moved one tile east or into the far corner tile, 3000
   ticks of `game_tick_sim` run twice: identical `net_state_checksum` sequences (final cfff2fee), cell lists
   consistent, 123 Things outside the first tile at the end.

`world_creatures_test.exe <game> <ticks>` changes the tick count of part 5.

## Deviations / open sites (all identical at 256)

None of these files is still 256-only. Ranges that need a game-design decision for a big world (left as they
are):

- **Unlimited searches become world-wide.** In a 1024+ world creatures may walk hundreds of cells:
  - skeletons walk toward the nearest enemy castle anywhere;
  - townies and traders go to the nearest wizard castle anywhere (traders: farther than 0xe100000 = 120 cells);
  - builders go to the nearest wizard castle at any 3-D distance;
  - the AI finders (`player_find_nearest_*`, `ai_goal_attack_*`, `ai_goal_hunt_creature`,
    `ai_find_mana_ball_target`) also take the nearest candidate anywhere. The attack goals are then limited by
    the sight radius; the mana-ball and creature hunts are not.

  This is correct torus behaviour, but a cap such as "within K cells" may be wanted.
- **`ai_find_castle_site`** scales with the world: in a 4096 world a quadrant is 1024 cells, so an AI's first
  castle can be up to 1000+ cells away. An alternative is a fixed 64-cell grid around the wizard (a mode
  parameter).
- Fixed game ranges stay fixed: `crab_target_nearest_mana_ball` < 0x10000 units (256 cells), the genie's
  teleport (50..109 cells), the AI constants 0x1900000 / 0x3840000 / 0x6400000 / 0x100000, projectile target
  ranges (0x1400 and sight radii; `aim_score` cannot overflow because dist <= 0x1400), the wake radius
  0x2400000.
- The separation quirk (no wrap) is kept (see above).

## Requested shared-file changes

None.

## For sessions 2 / 3

- Session 3 (B6): the wake rule is the one place still tied to the local player (`creature_proximity_wake_timer`,
  creature_common.cpp, the `local` line). The distance arithmetic is already int64 / torus-correct, so replacing
  `local` with a loop over the wizards is enough. In the tiled test creatures more than 24 cells from the local
  player never wake, as in the original.
- Session 2: the castle-site search depends only on `g_world.size`, so it works for any N = 512..4096 once the
  generator places castles. The creatures' tiled oracle in this test (walk tracks mod 65536) is reusable for any
  terrain-only movement code.
