# Port round 11, task E: renderers, HUD, map screen and debug views in an N-cell world

Phase 4 round 11 session 1 (B7 + the render part of B1/B2): every renderer-side 256 assumption goes through
`world.h`, so the extended renderer, the faithful renderer, the radar / map screen, the projection export, the
debug overlays and the debug camera work in a world of N = 512 .. 4096 cells. Every rule is the identity at
N = 256: the render references (`render_reference*`, pixel-exact) and the per-tick references are unchanged.

## Sites converted

| file | sites | what |
|---|---|---|
| `render_ext.cpp` | 22 | `kWin` / `kVN` per frame = `min(N/2, 256)` (vertex cache resized with it); camera `world_wrap` instead of `(uint16_t)`; draw distance clamp `4 .. kWin-1` (127 at 256); `vertex()`, `emit_quad` (cell + merged-quad colour loop), `count_cells`, `emit_cell_things` (cell, thing delta `world_signed`, shadow ground probe without the u16 cast), both skirt colours: `world_cell` / `world_wrap_cell`; `slope_camera` (5 cell sites); `thing_pos_lerp` (2) and the segment layout (4: `world_signed` deltas, `world_wrap` results) |
| `render_landscape.cpp` | 15 | `cx0 / cy0`, the grid walk (`cx / cy` int + `world_wrap_cell` steps, `world_cell` for the map / texture / thing cells), `render_view`'s slope probe (5). `g_rcam.cam_x16` stays u16 (layout), with a comment |
| `render_things.cpp` | 11 | `capture_view` records the full camera (`v.cam_x / cam_y`), `proj_f` uses it; `render_project_world` (faithful: `world_signed` + an early "not in front" beyond +-128 cells instead of an int32 overflow; extended: `world_signed`); `render_pick_ground` (ray samples `world_wrap`, cell `world_cx`, `wx / wy / wz` wrapped). `render_thing` / mirrored keep the original's `(int16_t)(x - cam_x16)` (exact for anything within 128 cells; the grid is 20) |
| `hud.cpp` | 11 | `ui_draw_radar` cell (`world_cell` of `u >> 8, v >> 8`), the castle line and every blip (`world_signed`, 4 sites), `ui_draw_map` (`world_wrap_cell` / `world_cell`, 4), the flight radar and the map screen read the camera log through `world_log_x / y` (2) |
| `debug_overlay.cpp` | 17 | grid (cells, corners, the picked-cell outline: `world_cx` / `world_wrap_cell` / `world_cell` / `world_wrap`), occupancy (loop over `g_world.cells`, `world_cell_x/y`, `world_signed`), radar geometry via `world_log_x/y`, inspector cell (`world_cell`), `debug_inspect_set_cell` (`world_wrap_cell`), follow camera (interpolation as render_ext, `world_wrap` camera, no u16 on the ground probe), inspector lines print int32 (`pos`, `home`, `cell`, `cell list` via `world_cx` / `world_cell_of`) |
| `mcport/main.cpp` | 9 | free camera step, `dcam_set`, follow step, `cam to` (`world_wrap(_cell)`, `world_cell_of`), Shift+Delete teleport passes the wrapped camera (no `& 0xffff`), log lines |
| `render.h`, `render_ext.h`, `debug_camera.h` | comments | Camera coordinates / projection doc (no layout change), cell-count hook size = `g_world.cells`, debug camera wrap rule |
| `render_ext_raster.cpp`, `mcport/inspect_tool.*`, `hud.h`, `debug_overlay.h` | 0 | nothing world-sized (the inspector stores the cell ints `render_pick_ground` returns) |

Non-mechanical ones:

- **Interpolation with a 16-bit `prev_pos`.** `RenderInterp.prev_pos` (render.h, filled by mcport/pacing.cpp)
  is `int16_t[3]` per slot and stays so (no layout change). The renderer now goes *back* from the current
  int32 position by the tick's 16-bit movement: `d = (int16_t)(t->x - prev16)`, `x = world_wrap(t->x - d +
  d*alpha >> 16)`. That is bit-identical to the old `(uint16_t)(prev + d*alpha >> 16)` at 256 and exact in any
  world (a Thing moves far less than 128 cells per tick). Same in `debug_inspect_follow_camera`.
- **The faithful renderer's camera.** `RenderCamState.cam_x16 / cam_y16` remain the original's 16 bits.
  Within the 40 x 21 grid `(int16_t)(t->x - cam_x16)` is the exact difference for any N, so the sprite
  projection needs nothing. The projection export (`render_view_info().cam_x`) needs the full position:
  `capture_view` rebuilds it as `world_wrap(cam.cam_x + (int16_t)(cam_x16 - cam.cam_x))` (the slope nudge is
  at most 100 units), which is `cam_x16` at 256.
- **Draw distance.** `R = min(dd, kWin - smax)` with `dd = clamp(draw_distance, 4, kWin - 1)`: unchanged at
  256 (4..127, minus the coarsest LOD size). From N = 512 the window is 256 cells, and render_ext accepts up to
  255 if the settings deliver it.
- **Map screen (decision):** the map screen keeps its scale (0xaa over 0x17e px = 254 cells) and in a wider
  world shows the **254-cell window centred on the player**, not the whole world scaled down. Why: it is
  the same code (the radar painter is already centred on the camera and wraps on the torus, the blips use the
  shortest torus difference), it stays readable (one sample per 0.66 cell, symbols at their size), nothing
  aliases, and the overview of the whole world is a job for the RTS minimap (B8). The castle line to a castle
  outside the window runs to the edge of the map in the castle's direction (the original's `break` on the
  first dot outside the radar).

## world_render_test (new, `src/tests/world_render_test.{cpp,cmake}`)

Level 38 after 50 ticks, `world_widen_tiled(1024)`; the 256 frames are taken first with the cell lists rebuilt
in slot order (as `world_widen_tiled` does) so both worlds list a cell's Things in the same order.

| check | result |
|---|---|
| terrain only (cell lists emptied): 4 cameras (tile interior, looking +x across x = 65535 -> 0, looking -y across y = 0, the corner of both) x 6 tiles ((0,0) (1,0) (0,1) (2,3) (3,3) (3,0) - i.e. across the old 255 -> 256 boundary and the new seam 1023 -> 0) x {extended lod 0, extended lod 1 (dd 100, 480x300), faithful 320x200, flight radar, map screen} | **120 frames, 0 pixels differ** from the 256 world at (x mod 65536) |
| with the Things (they live in tile (0,0)): extended, faithful and the flight radar with its blips at the camera in the middle of tile (0,0) | 0 / 0 / 0 pixels differ (153 blip pixels drawn) |
| wide window: dd 200, lod 2, camera in tile (3,0) looking across the seam, `g_render_ext_cell_count` over N*N | radius 200, 38639 cells drawn, 17953 of them > 128 cells away, **0 drawn twice** |
| projection export, camera in tile (3,0): `render_view_info().cam_x` (ext and faithful) is the full position (261760); a probe point 4.5 cells past the seam projects to the same pixel / depth as in the 256 world (both renderers); the ground pick returns the cell in the right tile (cell x 23 of 1024, same in-tile cell as at 256); a point 400 cells away: faithful "not in front" (no overflow), extended depth > 390 cells (no torus alias) | pass |
| interpolation across the seam: the follow camera of a creature at x = 262104 (N*256 - 40) with `prev_pos` = 80: alpha 0 = the camera of the Thing at 80, alpha 0.5 = at 20 | pass |
| drawing changes no game state: `net_state_checksum` before / after all of the above in the wide world | equal |

The map screen *with blips* is not compared: it reaches 127 cells, and a Thing ~128 cells from the camera is
the copy on the far side of the 256 torus but has no copy there in the tiled world (Things live in tile 0
only) - one pixel of difference, a true one.

Images (scratch, looked at): `w1024_seam_x_tile30_ext.png` (the seam frame: terrain continuous across
x = 262143 -> 0, the castle wall on the far side), `w1024_dd200_seam.png` (200-cell view across the seam),
`w1024_seam_x_tile30_map.png` (map screen at the seam: the window wraps, the four copies of the river visible).

## Gate

`build_E`, Release (`--parallel 2 -p:CL_MPCount=1`): zero warnings. `ctest -C Release -j 2`: 65 tests (the 60
existing + world_render_test + the other agents' new world_* tests), 63 passed in the full run. The two others
passed on rerun and are not caused by this task. `timectl_test` failed only because the agents' concurrent runs
share its default directory `%TEMP%/mc_timectl_test` (one run's mcport logs and screenshot overwrite
another's); it passes with `MC_TIMECTL_DIR` set to a private directory. `world_persist_test` (task D,
mid-edit at the time) passes when run alone. All render references (`render_reference*`, `render_ext*`,
`overlay_test`, `hud_test`) and the per-tick references pass, so the 256 world is byte- and pixel-identical.
Suggestion for the integrator: make timectl_test's default directory unique per build dir.

## Open sites / deviations (still 256-only or needing a decision)

- `RenderCamState.cam_x16 / cam_y16` (render.h) stay u16 - correct by locality (comment at the store).
- `g_render_interp.camera` is filled by **mcport/pacing.cpp** with `pace_lerp16`, which returns an int16:
  in a world wider than 256 cells the interpolated camera loses its high bits (the view jumps to tile 0) -
  see the request below. Same for `pace_lerp_camera(..., masked_xy = true)` (the debug camera) and the
  `PaceSlot` jump test (`abs16`, fine: a per-tick movement).
- Draw distance 255: render_ext allows `kWin - 1` = 255 for N >= 512, but `mcport/config.cpp`
  (`{"render", "draw_distance", ..., 8, 127}`), `game_menu.cpp` (slider 8..127) and the `settings.h` comment
  cap it at 127. For dd 255 they need `max = 255` (only meaningful when `g_world.size >= 512`; render_ext clamps
  to 127 in the 256 world by itself). Cost: the vertex cache is 513^2 entries (~10 MB), LOD stays at 3 coarser
  levels (8x8 quads beyond D0*4), the build time roughly doubles vs 127 (the coarse rings dominate). A fourth
  LOD level (16x16) would keep the triangle count flat - a session-2/3 or B8 decision.
- Fog constants, `thing_min_px` and the 20-cell faithful grid are game / look decisions, unchanged.
- Shift+Delete teleport passes the full camera to `debug_cmd_teleport(int x, int y, ...)`; what the 10-byte
  packet keeps of a wide position is task D's (debug_cmd.h).

## Requested shared-file changes (exact code)

**mcport/pacing.cpp** (nobody's this round): interpolate the camera on the N-cell torus and keep the full value.
Replace `pace_lerp16` (line ~19) by

```cpp
// round 11: a coordinate on the N-cell torus (world.h); with 256 cells the result equals the old 16-bit
// lerp's low 16 bits (the renderers wrap the camera), but it is no longer sign-extended
static int pace_lerp_w(int a, int b, uint32_t alpha) {
    if (alpha == 0) return world_wrap(a);
    if (alpha >= 0x10000) return world_wrap(b);
    const int d = world_signed(b - a);                    // shortest way round the torus
    return world_wrap(a + (int)(((int64_t)d * alpha) >> 16));
}
```

and in `pace_lerp_camera`:

```cpp
    c.cam_x = pace_lerp_w(a.cam_x, b.cam_x, alpha);
    c.cam_y = pace_lerp_w(a.cam_y, b.cam_y, alpha);
    (void)masked_xy;                                      // pace_lerp_w always wraps (was: &= 0xffff)
```

(+ `#include "world.h"`). Pixel-neutral at 256 (both renderers and the radar mask / wrap the camera), but
`pacing_test` should be checked: if it compares `camera.cam_x` against a sign-extended value, its expectation
becomes `world_wrap(...)`. Note `pace_camera_jump` uses 16-bit differences too (a per-frame camera jump test:
fine). `prev_pos` (int16) can stay: the renderers reconstruct the full position (above).

**mcport/config.cpp / game_menu.cpp** (only when dd 255 is wanted): draw_distance max 127 -> 255, and the
`settings.h` comment "max 127, the map wraps at 256" -> "max 127 in the 256 world, 255 from 512 cells".

## For sessions 2 / 3

- The renderers take the world size from `g_world` per frame; nothing to call. A `--size N` run is drawn
  correctly as soon as the maps are filled; set the draw distance > 127 only through the settings change above.
- Interpolation stays correct with the 16-bit `prev_pos`; the camera needs the pacing.cpp change before a wide
  world is played with `interpolate` on (the default).
- Map screen = window around the player at the original scale; the RTS minimap (B8) is the place for a
  whole-world view (`ui_draw_radar` with scale `0xaa * N / 256` would draw it, point-sampled).
- `debug_overlay`'s occupancy overlay walks all `g_world.cells` per frame (1M at 1024): fine for a debug view,
  16M at 4096 would want a window walk.
