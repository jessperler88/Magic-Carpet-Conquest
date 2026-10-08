// Terrain generation of carpet.exe (terrain_build_303f0 and its passes). Owner: terrain_gen.cpp.
// Runtime terrain helpers (painting, retexturing, spiral walks) are in terrain_paint.h.
#pragma once
#include <cstdint>
#include "mc_types.h"

// terrain_build_303f0(level header): seeds g_rng16 and g_state->rng from gen.seed, generates the
// 256x256 height map (fractal, or the cached file when present - the port never has one), then runs
// the classification, texturing and light-map passes. Writes g_map_height, g_map_type, g_map_flags,
// g_map_light and clears g_cell_things.
void terrain_build(const GenMap &gen);

// The individual passes, exposed for the regression test (same order as terrain_build).
void terrain_fractal_fill(uint16_t seed, int off, int raise, int gnarl);   // 71f08 (into g_cell_things as i16)
void terrain_generate();                                                   // 313a0: i16 scratch -> g_map_height
void terrain_carve_rivers(int count, int min_height);                      // 31430
void terrain_flatten_water_quads();                                        // 31e50
void terrain_classify_flat(int threshold);                                 // 309f0
void terrain_mark_lowland(int max_h, int max_range);                       // 31650
void terrain_insert_transitions();                                         // 30c50
void terrain_mark_interior(int max_h, int max_range);                      // 31800
// hw_classes: Hidden Worlds' rule (steep = class 1, class 6 free for snow); default = world_hidden()
void terrain_mark_steep(int min_range, int hw_classes = -1);               // 31ad0 (Hidden Worlds: 0x33570)
void terrain_mark_snow(int snlin, int snflt);                              // Hidden Worlds only: HIDDEN.EXE 0x31c10
void terrain_flags_fill_holes();                                           // 308f0
void terrain_smooth_spikes();                                              // 30500
void terrain_fix_shore_quads();                                            // 30810
void terrain_assign_textures();                                            // 30eb0 (also builds the compact corner-class table)
void terrain_mark_water_anim();                                            // 30690
void terrain_build_lightmap();                                             // 31310

// The passes of terrain_build after the rivers (flatten water quads .. light map), in the original's order.
// snow: Hidden Worlds' classification and snow pass (gen.snlin / snflt) on any data set - a generated Conquest
// world caps its peaks with snow (the snow tiles come from the Hidden Worlds textures, world_art.h). The campaign
// uses world_hidden() as before.
void terrain_build_passes(const GenMap &gen, bool snow = false);

// ---- round 11 session 2 (B4): a generated N-cell world (port-only, driven by worldgen.cpp) ----------------
struct TerrainWorldGen {
    uint32_t seed = 0;          // the field's own LCG (g_rng16 is not touched, like the original's fractal)
    int      start_x = 0, start_y = 0;   // the seeded lattice point (the original's OFF)
    int32_t  raise = 0;         // its value (the original's RAISE)
    int32_t  gnarl = 0;         // second noise amplitude (the original's GNARL), 0..0x7ffe
    int32_t  coarse_pct = 50;   // amplitude of the levels above step 128 in % of the step-128 level (4096)
};
// Torus diamond-square over N x N int32 cells (FIELD has g_world.cells entries): levels step N/2 .. 1, the
// original's square-then-diamond order per level; the noise per level is the original's step * 32 up to step
// 128 and coarse_pct % of 4096 above, so a wide world keeps the campaign's local relief and gains continents.
void terrain_fractal_world(int32_t *field, const TerrainWorldGen &p);
// FIELD -> g_map_height: the maximum maps to 0xc4, then heights above 0x60 get a quadratic lift (0xc4 -> 0xec:
// taller peaks); sea level = 0 (the original's rule) for water_pct 0, else the value below which water_pct % of
// the cells lie.
void terrain_normalise_world(const int32_t *field, int water_pct);
// Rivers on any N: begin (classes from the heights, all cells unvisited), trace from chosen sources (false =
// the source is protected: g_map_type 0 set by the caller), end (g_map_type := 0xff as the original).
void terrain_rivers_begin_world();
bool terrain_trace_river_world(uint32_t cell);
void terrain_rivers_end_world();

// terrain_sample_height_71e00: height at a world position interpolated over the quad's triangle
// (height byte * 0x20 scale).
int terrain_sample_height(int32_t x, int32_t y);   // round 11: world coordinates (world.h)

// DAT_000b58b0: compact corner-class -> {texture, rotation code} table built by
// terrain_assign_textures, index = c0*0x157 + c1*0x31 + c2*7 + c3 (corner classes (x,y), (x+1,y),
// (x+1,y+1), (x,y+1)); {1, 0} where no texture matches. Used by the run-time retexture functions.
extern uint8_t g_corner_tex_table[2401][2];
