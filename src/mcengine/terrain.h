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
void terrain_mark_steep(int min_range);                                    // 31ad0 (Hidden Worlds: 0x33570)
void terrain_mark_snow(int snlin, int snflt);                              // Hidden Worlds only: HIDDEN.EXE 0x31c10
void terrain_flags_fill_holes();                                           // 308f0
void terrain_smooth_spikes();                                              // 30500
void terrain_fix_shore_quads();                                            // 30810
void terrain_assign_textures();                                            // 30eb0 (also builds the compact corner-class table)
void terrain_mark_water_anim();                                            // 30690
void terrain_build_lightmap();                                             // 31310

// terrain_sample_height_71e00: height at a world position interpolated over the quad's triangle
// (height byte * 0x20 scale).
int terrain_sample_height(int32_t x, int32_t y);   // round 11: world coordinates (world.h)

// DAT_000b58b0: compact corner-class -> {texture, rotation code} table built by
// terrain_assign_textures, index = c0*0x157 + c1*0x31 + c2*7 + c3 (corner classes (x,y), (x+1,y),
// (x+1,y+1), (x,y+1)); {1, 0} where no texture matches. Used by the run-time retexture functions.
extern uint8_t g_corner_tex_table[2401][2];
