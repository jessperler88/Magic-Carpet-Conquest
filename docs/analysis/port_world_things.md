# Round 11 task B: Thing core, spatial index, effects, castles, constructors, spells, scenery on an N-cell world

Agent B, session 1 of round 11 (briefing `docs/port/BRIEFING_round11.md`). Every site below goes through
the `world.h` accessors and is the identity at N = 256. Zero warnings at /W4.

**ctest in `build_B` (65 tests incl. the other agents' new ones, `-j 2`).** Every reference, render-reference,
pool, net, effects, castle, spatial and constructors gate passes, and so does `world_things_test`. Four tests
failed in that run:

- `replay_check_test` failed under load but passes when re-run alone.
- `timectl_test` fails in its mcport log-rotation check: `mcport.1.log` is missing the time-control lines. Its
  300-tick checksums are identical (0 differ), so the simulation is unaffected. It is mcport / main.cpp
  territory, not a file of task B.
- `world_persist_test` and `world_terrain_test` (not run) belong to agents D and A and are mid-edit.

## Sites converted

| file | sites | what |
|---|---|---|
| `spatial.cpp` | 19 | `walk_cell` / `cell_kill_things` mask `cells - 1`; `spiral_find`, `area_cells_slot0`, `thing_exists_near_pos`, `thing_area_damage` (slots 1..5): `(int16_t)t->x` -> `world_signed`, the hand-packed `((cy & 0xff) << 8) \| (cx & 0xff)` -> `world_cell`; `terrain_max_corner_level` -> `world_cx` / `world_cell` |
| `thing.cpp` | 6 | `level_spawn_thing_record`: spawn position and the teleport destination (`home`) from ThingInit cells without the `(uint16_t)` (WCoord wraps); `math_bbox_overlap` (that is, `thing_collide` and every collision test) `(int16_t)(pb->x - pa->x)` -> `world_signed` |
| `effects.cpp` | 33 | `cell_xy` / `thing_cell_x/y` clones on ints through `world_cell` / `world_wrap_cell` + `world_signed`; `scatter_pos`; `terrain_max_drop_around`; `terrain_ring_find_height_ne8`; castle levelling (0x29) and castle raise (0x2a): footprint origin, sub-footprint walk, the rect loops; lava blob, mana ball, boulder positions (see "velocity" below) |
| `castle.cpp` | 3 | `castle_find_free_mana_ball`: nearest ball by int64 square (below); guard spawn offset without `(uint16_t)` |
| `constructors.cpp` | 7 | castle constructor (`player_create_type2`) cell-corner snap without the u16 casts; tree jitter `(int16_t)p.x + j` -> `p.x + j` |
| `spells.cpp`, `scenery.cpp` | 0 | nothing 256-specific: every position goes through `math_rotate_offset` / `thing_move_to` / `pos_*` (converted by the integrator) |
| `*.h` of the task | 0 | no interface change (`terrain_ring_find_height_ne8(unsigned cell, ...)` already took a packed cell) |

### The ones that were not mechanical

- **Velocity in `Thing.home`.** Mana balls, lava blobs and the boulder keep a signed 16-bit x / y velocity in
  `home.x / home.y` (an original trick). The original moved them with `(uint16_t)(t->x + t->home.x)`, which only
  works because both are u16. With a wider world a negative velocity stored through WCoord is `units - v`, and
  `x + home.x` jumps 256 cells. The position update now adds the sign-extended velocity (`t->x + vel_x(t)`), the
  same thing mod 65536. Every read of a velocity already went through `(int16_t)`, and the stores stay 16-bit,
  so the velocity is still a 16-bit quantity at any N. The test's westward mana ball checks this (it would jump
  0xffd0 units per tick without the fix). Side effect worth knowing: velocities written by
  `math_rotate_offset(&home, ...)` from 0 (thing_drop_mana_ball, effect_create_type16) are stored as
  `units - v` when N > 256, so their `ThingHi.home_x/home_y` are non-zero. That is deterministic and harmless,
  but it is part of the high-half checksum.
- **`castle_find_free_mana_ball`.** The original compares the int32 square *unsigned*, so a square that wraps
  negative at 128 cells counts as "very far". At 256 the code keeps exactly that (`(uint32_t)sq`, best =
  0xffffffff). When N > 256 it compares the exact int64 square (best = UINT64_MAX).
- **`area_cells_slot0`.** The window centre is `idiv((int16_t)x - 0x80, 256)`, which truncates toward zero, so in
  the upper half of the world (negative signed coordinate) the centre is one cell further east / south than the
  floor would give. This quirk is kept through `world_signed`. When N > 256 it applies to the upper half of the
  wider world. It is harmless, because the radius always covers the box.
- The cell coordinates of the two castle effects were `uint8_t` walkers. They are now ints, wrapped by
  `world_cell` (and `world_wrap_cell` where a value is stored or handed to `level_features`).

## What `world_things_test` checks (level 38, `world_widen_tiled(1024)`, about 45 s)

1. **Terrain probes.** 569 sample positions (13x13 edge combinations around both seams plus 400 random
   ones), compared in all 16 tiles: `terrain_height_at`, `terrain_max_corner_level`, `terrain_max_drop_around`,
   `terrain_slope_vector`, `terrain_type_mask_at`, `terrain_cell_flag_bit` and `terrain_ring_find_height_ne8`
   (result relative to its tile). 9104 comparisons, 0 differ.
2. **Spatial queries.** A searcher 0x80 west of a creature, across x = 0x3ffff -> 0, across 0xffff -> 0x10000 and
   inside a tile, with a decoy at the creature's 256-world alias (+65536). `thing_collide`,
   `thing_find_collision`, `_other_owner`, `thing_find_mana_ball_touching`, `thing_exists_near_pos` (and not at
   the alias), `thing_area_damage` slots 0 and 1, `thing_area_damage_quake` and `cell_kill_things` all hit the
   neighbour and never the decoy.
3. **Distances / angles** at 300, 500, 511, 513, 600, -300 and -500 cells on x and y, plus 500 cells diagonally:
   exact `pos_dist_xy / _xyz / _sq_xy / _chebyshev` (int64 squares). Angles go the short way round (513 cells east
   = 511 west, 0x600). The diagonal is 181019 at 0x300. `castle_find_free_mana_ball` picks the ball 300 cells away
   over the one 450 cells away.
4. **Castle raise (0x2a) and levelling (0x29), castle size 3.** First run in the 256 world at cell (0, 0) (431 /
   664 cells changed). Then in the wide world at (0,0), (256,256), (512,0), (768,512), (0,768), so the footprint
   straddles the world seam on both axes and the old 255 -> 256 boundaries. The raise is identical to the 256
   result in a 49x49 window everywhere, and nothing outside the window changes. The levelling matches the 256
   result on the x seam, and all centres on old boundaries match each other. The two groups differ by 5 cells
   because of the original's *linear-index* quirk in `terrain_smooth_cell` (level_features.cpp, task A): at
   x = 0 the "west" neighbour is the previous row's last cell. That quirk is present only on the world's
   x = 0 column, so it is correct that the old boundaries do not show it.
5. **Tracks.**
   - Mana balls rolling east (+0x30) and west (-0x30) across the world seam, compared with two other tile
     boundaries: identical per-tick steps and heights. Each run moves +1793 / -1940 units with a maximum step of
     48 / 49 and exactly one wrap at the seam, and the ball stays in its cell list.
   - An earthquake crack heading east across the seam (fixed RNG): 24 steps of 0x100, one crater on the crack per
     step, identical at all three boundaries.
6. **3000 ticks of the tiled world, twice in one process.** Identical `net_state_checksum` sequences (last
   f6cdc935); 18 Things wander out of the first tile; 0 cell-list errors.

## Deviations, open sites, game-design notes

- Nothing in my files is still 256-only.
- Ranges that are game constants and stay as they are (they will feel different on a big world: a design
  decision for session 2 / 3):
  - the teleport gate's default destination, `rotate(-0x8000)` = 128 cells, which is "half the world" at 256;
  - the Teleport spell without a castle (0x4000 = 64 cells) and the projectile target distances (0x4000);
  - the mana magnet (0xe00), the blast (0xa00), the balloon's 0x400 pickup range;
  - the castle spill throw distance (0xf00..0x22ff);
  - the spiral search's 15-ring maximum.
- `effect_earthquake_update` moves its Thing without relinking (original). The Thing is never linked, so this
  causes no list corruption at any N. The test's cell-list check skips it anyway.
- I did not run a `MC_WARN_NARROWING` build: the CPU limit allowed one configuration. My edits add no implicit
  narrowing; every narrowing is an explicit cast on a non-position value.

## Shared-file changes requested

None. `thing.h`, `world.h`, `mc_types.h` and `mc_image.h` were enough as they are.

## For sessions 2 / 3

- `world_things_test` shows the pattern for an exact oracle on terrain effects: save the maps, run the effect
  with `g_rng16` fixed, restore. A generated world can reuse it (the levelling oracle must avoid the x = 0
  column, see 4).
- The checksum's `ThingHi` of velocity Things can be non-zero (above). If session 3 wants all-zero high halves for
  non-position uses of `home`, it is a one-line `(uint16_t)` around the two `math_rotate_offset(&home, ...)`
  velocity initialisations. That is not needed for determinism.
- With the wake rule still on the local player (task C / session 3), level 38 tiled for 3000 ticks already moves
  Things across the old 65535 -> 65536 boundary without any inconsistency.
