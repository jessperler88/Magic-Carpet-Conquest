# Round 11 task D: player, input, debug commands, sound, persistence, checksum in an N-cell world

Session 1 of round 11 (docs/port/BRIEFING_round11.md, world.h / mc_image.h). Build dir `build_D`, all
pre-existing ctest gates pass (references byte / pixel identical at N = 256), the new `world_persist_test`
passes, zero warnings (/W4).

## Sites converted per file

| file | sites | what |
|---|---|---|
| `player.cpp` | 4 | `player_flyer_move` push (`(uint16_t)(x + push_x)` -> WCoord wrap, x / y); `player_dying_update` dropped-spell scatter (`(uint16_t)((int16_t)x + r)` -> `x + r`, identical mod 65536). Everything else in the file is already int32-safe: `pos_dist_xyz` results are game ranges (fire / ridge distance, tether 0x1400 / 0x400..0xc00), `player_terrain_collide`'s `dist & 0xffff` is the length of one move step, `player_spawn`'s castle stamp copies `home` (WCoord to WCoord). Camera log / start position code was already converted by the integrator. |
| `input.cpp` / `input.h` | 0 | no position arithmetic (keys, mouse, controller -> packets only). |
| `sound.cpp` | 0 (comment) | the cull `pos_dist_sq_xy(...) > 0x9000000` already compares int64 (thing.h returns int64; the original's int32 wrap at 256); past the cull `dist <= 0x3000`, so the volume / pan arithmetic stays int32. Comment added. |
| `debug_cmd.{h,cpp}` | 9 | spawn "ahead": int32 position, `world_cx` (was `(x >> 8) & 0xff`); spawn at a cell: 12-bit cells; `ThingInit.x/.y` via `world_wrap_cell`; teleport: 20-bit coordinates; describe / say print int32. **Packet layout extension** below. |
| `scenario.cpp` | 6 | `parse_coord` masks to the largest world (`MC_WORLD_MAX` cells / 20 bits of units) instead of 256 cells (a line is parsed before its world exists; the store / packet wraps it - identical at 256); `assert_pos` differences `world_signed` (was `(int16_t)(uint16_t)`); `face_point` / `fly`: `Pos{x, y, z}` without `(uint16_t)`, differences `world_signed`. |
| `net.cpp` (checksum) | 6 | the five map parts hash `g_world.cells` cells (0x10000 at 256: the same bytes); a wide world additionally hashes `g_world.size` first in `map.type` (peers with different sizes differ at once). Part names unchanged. The Thing / start-position high halves were already hashed by the integrator; the camera log is not hashed at any size (its high halves neither). |
| `savegame.{h,cpp}` | WRLD chunk | below |
| `demo.{h,cpp}` | gax WRLD block, wide `max`, cell-list repair | below; `demo_repair_cell_lists` / `cell_list_problems`: `world_cell_of`, loops over `g_world.cells` (6 sites). |
| `mode_dump.{h,cpp}` | 3 | `world {size, units, cells}` member; player `x` / `y` explicitly int32. |
| `mode_level.{h,cpp}` | hook | `ModeParams.world_size` N > 256 (power of two <= 4096): `world_widen_tiled(N)` at the start of `g_hook_mode_level_start` (`level_start`). |
| `mcport/rts_run.{h,cpp}` | 1 | `--size` accepts powers of two 256..4096 (> 256 = the template tiled until session 2). |
| `replay_check.{h,cpp}` | 0 | goes through `savestate_*` (now wide-aware) and the movie code; nothing 256-specific. |

## Debug packet layout for wide positions (debug_cmd.h)

A world has at most 4096 cells = 2^20 units per side. Two 20-bit coordinates + a 16-bit z = 56 bits = exactly
the 7 data bytes b3..b9 of the 10-byte packet. b9 was 0 in every packet before, so:

- **teleport 0x40**: b3-4 x low 16, b5-6 y low 16, b7-8 z, **b9 = x bits 16..19 (low nibble) | y bits 16..19 (high nibble)**.
- **spawn 0x41**: b3 / b4 cell low 8 bits, **b9 = x cell bits 8..11 | y cell bits 8..11 << 4** (only with "at cell").

In a 256-cell world b9 stays 0: every packet (and `console_test`'s byte expectations) is unchanged. The
applied coordinates wrap onto the world (WCoord / `world_wrap_cell`). No second packet or packet-size change needed.

## Persistence design

**Save states (`.mcs`)**: N = 256 writes exactly what round 10 wrote (`MAPS`). N > 256 writes **`WRLD` instead of
`MAPS`**: u32 version 1, u32 N, u32 slots (= header thing_slots), the four byte maps at N*N, cell heads u16 N*N,
the corner table, `ThingHi[slots]` (GameState's 1000 then the pool extension), `WorldHi` (`g_world_hi`). GAME /
POOL keep the original 16-bit image. Loading: chunks validated first; then `world_set_size(N or 256)` **before**
any coordinate is stored (WCoord wraps at the current `unit_mask`), `state_from_image(img, g_state, hi)` /
`thing_from_image(..., &hi[1000 + i])`, maps of `g_world.cells`. Neither or both of MAPS / WRLD: refused. A reader
older than round 11 refuses a wide file ("required chunk missing") instead of loading 256 cells of it. A MAPS file
loaded into a wide world goes back to 256 (`g_world_hi` cleared).

**Movies (demo.h)**: v1 / v2 / v3 and the original's files are byte-identical for N = 256. For N > 256 (any mvx
version; a wide recording without a mode is version 2 because version 1 requires an extended pool):
- `gax%05d.dat`: after the snapshot (v1 / v2: after the extension's Things; v3: after the tail) the block
  `"WRLD", u32 1, u32 N, u32 slots, ThingHi[slots], WorldHi`. `demo_load_state` parses the v3 tail length
  exactly (`v3_tail_size`), then expects nothing or a valid block, sets the world size, then restores the state.
  (Round-10 v3 snapshots with trailing bytes that are no block still load, as `>= base` did.)
- `max%05d.dat`: `"MCWM", u32 N`, the maps at N*N, the corner table; expected exactly when the world is wide.
- A wide world always records mvx (`s_ext |= world_wide()`); the original's GameState-only `gam` save / load
  (also the non-full quick save 10000) is refused in a wide world (it cannot hold the high halves and has no maps).
- `sim_load_snapshot` stays 256 (not touched).

## The test (`world_persist_test`, ~55 s alone)

1. Level 38 tiled to 1024. `teleport 900 700` (console grammar -> packet, b9 = 0x23) puts the local wizard at
   230528,179328 (cell 900,700); `assert_pos p0 900 700` passes, its 256-alias `132 188` fails; `spawn dragon
   600 800` (b9 = 0x32) spawns at cell 600,800 (17 Things); a creature moved by hand to x = 900 cells; the dump
   has `"world":{"size":1024,...}` and the int32 x.
2. Checksum: a creature's x high half alone changes it (`814fc3d2` -> `951b92f5`), its home's high half changes
   part `things.creature`, a start position's high half part `player2`; all restored -> the original value.
3. Save at T (6.5 MB: WRLD, no MAPS), 2000 ticks; reload level 38 (**a 256-cell world**), load the state (back to
   1024, checksum = the one at the save, the 900-cell Things where they were, cell lists consistent), 2000 ticks:
   **identical checksums and all parts every tick**.
4. A 256-cell save (MAPS, no WRLD) loaded into a 512 world: back to 256 with the original checksum. The non-full
   quick save / load refuse in a wide world.
5. Sound: a source 20 cells away is heard, (128,128) and (428,128) cells away are culled (int64 squares).
6. v3 movie of an rts run with `world_size = 1024` (mode_level widened the template), with two wide teleports in
   the recording (one to cell 1000,300 next to the x = 1023 -> 0 seam): gax ends in the WRLD block, max is
   `MCWM` + 6 * 1024^2 + 0x12c2 bytes; played back from a 256-cell campaign level (5): **600 ticks, identical
   checksums every tick**, the mode stopped at the end.

ctest in build_D: 64 of 65 pass; the one "not run" is agent A's `world_terrain_test` (its file appeared after my
build). Every one of the 60 pre-existing gates passes.

## Deviations / open sites

- Game ranges stay constants (not scaled, a design decision for later): sound cull 48 cells (0x9000000), hearing
  range 0x3000, tether break 0x1400, fire / ridge proximity 0x600, the `spawn ahead` distance (<= 255 cells).
- `face_point` (scenario input generation) uses double, as before (host-side, recorded in the packet).
- `parse_coord` no longer masks to 256 cells: "teleport 300 10" in a 256 world produces cell 300 in the packet
  (b9 = 0x01) and lands at cell 44, exactly where the old `& 0xff` sent it; the describe text shows 300.
- The original's `gam` / `map` movie format and `sim_load_snapshot` remain 256-only by design.
- `MC_PORT_VERSION` was not bumped (the WRLD chunk is self-describing).

## Requested shared-file changes

None required. The world-size hook lives in `mode_level.cpp`'s `g_hook_mode_level_start` (called by
`mode_level_start()` at the end of `level_start_from_state`, after `switch_activate` / `players_init_records`,
before the first tick spawns the players), so sim.cpp needs nothing. `g_mode.params.world_size` is in the mode
block, hence in the mode checksum, the MODE chunk and the v3 header.

## For sessions 2 / 3

- Session 2's generator on N should replace the `world_widen_tiled` call in `mode_level.cpp level_start` (or run
  before it and skip it); `rts --size N` already accepts N. Save states / movies need no change for a generated
  world: they store the maps, not how they were made.
- Session 3's two-instance sync at 1024: the checksum already differs on the world size and every high half; the
  debug teleport / spawn packets carry wide positions; a desync dump (`replay_check`) of a wide world is a WRLD save.
- `MC_REF_WORLD` (session 3) can reuse `world_widen_tiled` exactly like `mode_level` does.
