// Run-time terrain painting of carpet.exe: slope orientation, paint kinds, quad / rectangle
// retexturing, the per-cell height modifiers and the spiral ("search.dat") area walkers.
// Owner: terrain_paint.cpp. Cells are world_cell(x, y) indices (world.h; (y << 8) | x in the original's
// 256-cell world); cell arguments are wrapped onto the map, cell coordinates wrap on the world torus.
#pragma once
#include <cstdint>
#include "thing.h"

// Loads data/search.dat (the 32x32 ring image spiral_search_init_101b0 turns into the spiral walk
// order). Called by level_features_load_data(); when nobody called it the first spiral walk loads it
// from MC_DEFAULT_GAME_DIR. Returns false when the file is missing or malformed.
bool terrain_paint_load_data(const char *game_dir);

// The spiral area walk (spiral_search_begin_10080 / _next_10120 / _end_10100): cell offsets (dx, dy)
// ring by ring around a centre, rings `start`..`end` (ring 0 = the 2x2 centre quad, 15 = outermost).
// The original hands out one of 100 cursor slots and frees it in _end; the port keeps the cursor in
// the caller's SpiralSearch, so there is no _end call.
//     SpiralSearch s; int dx, dy;
//     if (spiral_search_begin(&s, r0, r1)) while (spiral_search_next(&s, &dx, &dy) == 1) { ... }
struct SpiralSearch { int end, cur, idx, ring, entry; };
bool spiral_search_begin(SpiralSearch *s, int start, int end);   // false when search.dat is unavailable
int  spiral_search_next(SpiralSearch *s, int *dx, int *dy);      // 1 = offset valid, 2 = walk over (offset dropped)

// DAT_00093fc4: set by terrain_slope_orientation, 1 when the quad's height range is <= 8.
extern int32_t g_terrain_nearly_flat;

// terrain_slope_orientation_31f90(cell): highest corner 0..3 of the quad (x,y) (x+1,y) (x+1,y+1)
// (x,y+1), or the edge code 4..7 (corner pairs 0-1, 1-2, 2-3, 3-0) when the second highest corner
// is within 7 of it. `dl_in` / `cl_in` model the two registers the original leaves uninitialised
// when the quad is completely flat at height 0 (see port_features.md).
int  terrain_slope_orientation(unsigned cell, unsigned dl_in = 0xff, unsigned cl_in = 0xff);
// terrain_paint_cell_32150(cell, kind). `dl_in` / `cl_in`: what the caller has in DL / CL at the call, for
// terrain_slope_orientation's flat-quad corner case (the castle code leaves defined values there).
void terrain_paint_cell(unsigned cell, unsigned kind, unsigned dl_in = 0xff, unsigned cl_in = 0xff);
// terrain_set_quad_texture_32430(cell, texture)
void terrain_set_quad_texture(unsigned cell, unsigned texture);
// terrain_retexture_rect_324e0(c0, c1) / terrain_retexture_rect_force_32760(c0, c1)
void terrain_retexture_rect(unsigned c0, unsigned c1);
void terrain_retexture_rect_force(unsigned c0, unsigned c1);
// terrain_cell_is_nonland_3d5f0(cell): 1 when the class (flags & 7) is not 2, 3 or 5.
int  terrain_cell_is_nonland(unsigned cell);
// terrain_modify_cell_3d620(x, y, delta, keep_built): height += delta clamped to 0..200, class fix-up
// and retexture. Returns 1 when the walk that called it must stop (built-on cell with keep_built, or
// a clamp at cell (0,0)).
int  terrain_modify_cell(int x, int y, int delta, int keep_built);
// terrain_set_cell_height_3d7d0(x, y, height): absolute variant (clamp 0..255, refuses built-on cells).
int  terrain_set_cell_height(int x, int y, int height);
// terrain_find_cell_spiral_3d940(thing, ring0, ring1, delta, keep_built): terrain_modify_cell over the
// rings ring0..min(ring1, ext_x / 256) around the thing; 1 when a cell stopped the walk.
int  terrain_find_cell_spiral(Thing *t, int ring0, int ring1, int delta, int keep_built);
// terrain_modify_cells_spiral_23f20(thing, ring0, ring1): terrain_modify_cell(.., -3, 0) over the rings.
void terrain_modify_cells_spiral(Thing *t, int ring0, int ring1);
