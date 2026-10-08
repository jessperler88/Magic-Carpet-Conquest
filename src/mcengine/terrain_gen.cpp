// Terrain generation of carpet.exe: terrain_build_303f0 and the passes it calls (0x303f0-0x31f71 and
// the late-placed fractal code at 0x71e00-0x72147). Translated from the disassembly; the decompiled
// C of the fractal functions is unusable (register arguments, 8/16-bit arithmetic).
//
// Map addressing follows the original: a cell index is (y << 8) | x, neighbours are formed by
// incrementing / decrementing the two bytes separately, so everything wraps on a 256 x 256 torus.
// During generation the low 3 bits of g_map_flags hold the terrain *class* (0 water, 1 cliff edge,
// 2 interior lowland, 3 flat land, 4 transition, 5 default land, 6 steep), bit 3 is the water
// animation flag and bits 4-6 the texture rotation code written by terrain_assign_textures.
#include "terrain.h"
#include "mc_globals.h"
#include "mc_math.h"
#include "world_set.h"
#include "world.h"
#include "gen/terrain_tables.h"
#include <cstring>

// DAT_000b58b0: compact corner-class -> {texture, rotation} table, index = c0*0x157 + c1*0x31 + c2*7 + c3.
uint8_t g_corner_tex_table[2401][2];

// ---------------------------------------------------------------------------------------------
// helpers

static inline uint8_t  H(unsigned x, unsigned y) { return g_map_height[mc_cell(x, y)]; }
static inline uint8_t &HR(unsigned x, unsigned y) { return g_map_height[mc_cell(x, y)]; }
static inline uint8_t  F(unsigned x, unsigned y) { return g_map_flags[mc_cell(x, y)]; }
static inline uint8_t &FR(unsigned x, unsigned y) { return g_map_flags[mc_cell(x, y)]; }
static inline uint8_t  T(unsigned x, unsigned y) { return g_map_type[mc_cell(x, y)]; }

// The 16-bit scratch heightfield shared with the cell->thing map (0x10dfb0).
static inline int16_t &S(unsigned x, unsigned y) {
    return reinterpret_cast<int16_t *>(g_cell_things)[mc_cell(x, y)];
}

// ---------------------------------------------------------------------------------------------
// Diamond-square heightfield (terrain_fractal_fill_71f08 + the two step routines)

// The fractal keeps its own copy of the LCG in SI (seeded from g_rng16 by the caller); g_rng16 itself
// is not advanced by the fractal. GNARL is the second noise amplitude ([ebp+0x14]).
static uint16_t s_frac_rng;
static uint16_t s_frac_gnarl;

// Common tail of 71f92 / 72027: avg - step*32 + r % (step*64 + 1) - gnarl + r % (2*gnarl + 1),
// all in 16-bit arithmetic, one LCG step per call (r is the same for both modulo terms).
static int16_t fractal_noise(int16_t avg, unsigned step) {
    s_frac_rng = (uint16_t)(s_frac_rng * 0x24a1 + 0x24df);
    uint16_t r  = s_frac_rng;
    uint16_t di = (uint16_t)avg;
    uint16_t cx = (uint16_t)(step << 5);
    di = (uint16_t)(di - cx);
    cx = (uint16_t)(cx * 2 + 1);
    di = (uint16_t)(di + r % cx);
    cx = s_frac_gnarl;
    di = (uint16_t)(di - cx);
    cx = (uint16_t)(cx * 2 + 1);
    if (cx) di = (uint16_t)(di + r % cx);   // cx == 0 only for gnarl 0x7fff/0xffff (the original would fault)
    return (int16_t)di;
}

// terrain_fractal_square_step_71f92: centre of the square (x,y)..(x+2s,y+2s) from its 4 corners.
static void fractal_square_step(uint8_t x, uint8_t y, uint8_t s) {
    uint8_t s2 = (uint8_t)(s * 2);
    int16_t sum = (int16_t)(uint16_t)(S(x, y) + S((uint8_t)(x + s2), y) +
                                      S((uint8_t)(x + s2), (uint8_t)(y + s2)) + S(x, (uint8_t)(y + s2)));
    int16_t v = fractal_noise((int16_t)(sum >> 2), s);
    int16_t &c = S((uint8_t)(x + s), (uint8_t)(y + s));
    if (c == 0) c = v;
}

// terrain_fractal_diamond_step_72027: the two edge midpoints (x+s,y) and (x,y+s) of the same square,
// each from its 4 diamond neighbours (the square centre written by the previous phase included).
static void fractal_diamond_step(uint8_t x, uint8_t y, uint8_t s) {
    uint8_t s2 = (uint8_t)(s * 2);
    int16_t orig = S(x, y);
    int16_t sum = (int16_t)(uint16_t)(orig + S((uint8_t)(x + s), (uint8_t)(y - s)) +
                                      S((uint8_t)(x + s2), y) + S((uint8_t)(x + s), (uint8_t)(y + s)));
    int16_t v = fractal_noise((int16_t)(sum >> 2), s);
    {
        int16_t &m = S((uint8_t)(x + s), y);
        if (m == 0) m = v;
    }
    sum = (int16_t)(uint16_t)(S((uint8_t)(x + s), (uint8_t)(y + s)) + orig +
                              S((uint8_t)(x - s), (uint8_t)(y + s)) + S(x, (uint8_t)(y + s2)));
    v = fractal_noise((int16_t)(sum >> 2), s);
    {
        int16_t &m = S(x, (uint8_t)(y + s));
        if (m == 0) m = v;
    }
}

// terrain_fractal_fill_71f08(seed, off, raise, gnarl): all four arguments are read as 16-bit words.
// OFF is the start cell, RAISE its initial value; the scratch must be zero on entry (cells are only
// written while still 0). Eight levels, step 128 .. 1: a full pass of square steps then a full
// pass of diamond steps per level, both starting at the start cell and wrapping around the map.
void terrain_fractal_fill(uint16_t seed, int off, int raise, int gnarl) {
    uint16_t start = (uint16_t)off;
    reinterpret_cast<int16_t *>(g_cell_things)[start] = (int16_t)(uint16_t)raise;
    s_frac_rng   = seed;
    s_frac_gnarl = (uint16_t)gnarl;
    uint8_t x0 = (uint8_t)start, y0 = (uint8_t)(start >> 8);
    for (int level = 7; level >= 0; --level) {
        unsigned count = 1u << (7 - level);
        uint8_t  s     = (uint8_t)(1u << level);
        uint8_t  s2    = (uint8_t)(s * 2);
        uint8_t y = y0;
        for (unsigned r = 0; r < count; ++r) {
            uint8_t x = x0;
            for (unsigned c = 0; c < count; ++c) { fractal_square_step(x, y, s); x = (uint8_t)(x + s2); }
            y = (uint8_t)(y + s2);
        }
        y = y0;   // count * 2s == 256: the original's BH has wrapped back to the start row here
        for (unsigned r = 0; r < count; ++r) {
            uint8_t x = x0;
            for (unsigned c = 0; c < count; ++c) { fractal_diamond_step(x, y, s); x = (uint8_t)(x + s2); }
            y = (uint8_t)(y + s2);
        }
    }
}

// terrain_generate_313a0: scale the signed 16-bit scratch so that its maximum maps to 0xc4, clamp
// to 0..0xc4 into g_map_height and zero the scratch (it becomes the cell->thing map).
void terrain_generate() {
    int16_t *map = reinterpret_cast<int16_t *>(g_cell_things);
    int16_t mx = -32000;
    for (unsigned i = 0; i < MC_MAP_CELLS; ++i)
        if (map[i] > mx) mx = map[i];
    // (the minimum is tracked too in the original - into ECX, which is then overwritten - unused)
    int32_t scale = mx ? (int32_t)(0xc40000 / (int32_t)mx) : 0;   // idiv, truncating
    for (unsigned i = 0; i < MC_MAP_CELLS; ++i) {
        int32_t p = ((int32_t)map[i] * scale) >> 16;
        map[i] = 0;
        int v = (int16_t)p;            // the original tests AX, not EAX
        if (v < 0) v = 0;
        if (v > 0xc4) v = 0xc4;
        g_map_height[i] = (uint8_t)v;
    }
}

// ---------------------------------------------------------------------------------------------
// Rivers

// terrain_trace_river_314e0(cell): g_map_type is the visited map (3 unvisited, 0 visited). From the
// start cell repeatedly step to the lowest unvisited of the 8 neighbours (scan order N, NE, E, SE,
// S, SW, W, NW; strict less-than, so the first lowest wins), lowering it to the running minimum
// height, until the next cell is water (class 0), there is no unvisited neighbour, or height 0 is
// reached. Every visited cell becomes class 0.
static void terrain_trace_river(uint16_t cell) {
    std::memset(g_map_type, 3, MC_MAP_CELLS);
    uint8_t  cur_h = g_map_height[cell];
    uint16_t cur   = cell;
    uint16_t best  = 0;        // stale when no neighbour qualifies (EBX in the original); harmless, see below
    for (;;) {
        g_map_type[cur] = 0;
        uint8_t x = (uint8_t)cur, y = (uint8_t)(cur >> 8);
        uint8_t best_h = 0xff;
        auto consider = [&](unsigned nx, unsigned ny) {
            uint16_t n = mc_cell(nx, ny);
            if (g_map_type[n] != 0 && g_map_height[n] < best_h) { best_h = g_map_height[n]; best = n; }
        };
        consider(x, y - 1); consider(x + 1, y - 1); consider(x + 1, y); consider(x + 1, y + 1);
        consider(x, y + 1); consider(x - 1, y + 1); consider(x - 1, y); consider(x - 1, y - 1);
        // Both exits below take no action, so a stale BEST only ever leads to one of them.
        if (g_map_flags[best] == 0) break;   // reached the sea
        if (best_h == 0xff) break;           // dead end
        if (best_h > cur_h) g_map_height[best] = cur_h;
        cur_h = g_map_height[best];
        cur   = best;
        if (cur_h == 0) break;
    }
    for (unsigned i = 0; i < MC_MAP_CELLS; ++i)
        if (g_map_type[i] == 0) g_map_flags[i] = 0;
}

// terrain_carve_rivers_31430(count, min_height): class = 5 where height != 0 else 0; COUNT rivers
// from random land cells above MIN_HEIGHT (at most 999 candidates per river - `mov ecx, 0x3e8` at
// 0x31461 is inside the river loop; running out ends the whole pass); finally g_map_type := 0xff.
// Uses g_rng16; the cell is rng % 0xffff (so 0xffff maps to cell 0).
// Round 5 fix (per-tick reference of level 44: 39 rivers above height 132): the try counter was
// shared by all rivers, so the port stopped carving early on levels with many high sources.
void terrain_carve_rivers(int count, int min_height) {
    for (unsigned i = 0; i < MC_MAP_CELLS; ++i)
        g_map_flags[i] = g_map_height[i] ? 5 : 0;
    while (count > 0) {
        int tries = 1000;
        uint16_t cell;
        for (;;) {
            cell = (uint16_t)(mc_rng16_next() % 0xffff);
            uint8_t h = g_map_height[cell];
            if (--tries == 0) goto done;
            if (h <= (uint8_t)min_height) continue;
            if (g_map_flags[cell] == 0) continue;
            break;
        }
        --count;
        terrain_trace_river(cell);
    }
done:
    std::memset(g_map_type, 0xff, MC_MAP_CELLS);
}

// terrain_flatten_water_quads_31e50: until stable, every 2x2 quad of four class-0 cells with unequal
// heights is set to its minimum height.
void terrain_flatten_water_quads() {
    bool changed;
    do {
        changed = false;
        for (unsigned i = 0; i < MC_MAP_CELLS; ++i) {
            uint8_t x = (uint8_t)i, y = (uint8_t)(i >> 8);
            int water = (F(x, y) == 0) + (F(x + 1, y) == 0) + (F(x + 1, y + 1) == 0) + (F(x, y + 1) == 0);
            uint8_t h0 = H(x, y), h1 = H(x + 1, y), h2 = H(x + 1, y + 1), h3 = H(x, y + 1);
            uint8_t mn = h0, mx = h0;
            if (h1 < mn) mn = h1; if (h1 > mx) mx = h1;
            if (h2 < mn) mn = h2; if (h2 > mx) mx = h2;
            if (h3 < mn) mn = h3; if (h3 > mx) mx = h3;
            if (mx != mn && water == 4) {
                changed = true;
                HR(x, y) = HR(x + 1, y) = HR(x + 1, y + 1) = HR(x, y + 1) = mn;
            }
        }
    } while (changed);
}

// ---------------------------------------------------------------------------------------------
// Classification passes

// Height range (max - min) over the 5-cell cross centred on (x,y).
static int cross_range(uint8_t x, uint8_t y, uint8_t *out_max = nullptr, uint8_t *out_min = nullptr) {
    uint8_t h = H(x, y), mx = h, mn = h;
    auto acc = [&](unsigned nx, unsigned ny) { uint8_t v = H(nx, ny); if (v > mx) mx = v; if (v < mn) mn = v; };
    acc(x, y - 1); acc(x + 1, y); acc(x, y + 1); acc(x - 1, y);
    if (out_max) *out_max = mx;
    if (out_min) *out_min = mn;
    return (int)mx - (int)mn;
}

// Max / min height over the 3x3 block centred on (x,y).
static void block_range(uint8_t x, uint8_t y, uint8_t &mx, uint8_t &mn) {
    mx = mn = H(x, y);
    auto acc = [&](unsigned nx, unsigned ny) { uint8_t v = H(nx, ny); if (v > mx) mx = v; if (v < mn) mn = v; };
    acc(x, y - 1); acc(x + 1, y - 1); acc(x + 1, y); acc(x + 1, y + 1);
    acc(x, y + 1); acc(x - 1, y + 1); acc(x - 1, y); acc(x - 1, y - 1);
}

// terrain_classify_flat_309f0(threshold): class-5 cells with cross range < threshold become 3,
// == threshold become 4. Then every 2x2 quad containing both 3 and 5 but no 2 turns its 3s into 4.
void terrain_classify_flat(int threshold) {
    for (unsigned i = 0; i < MC_MAP_CELLS; ++i) {
        if (g_map_flags[i] != 5) continue;
        int range = cross_range((uint8_t)i, (uint8_t)(i >> 8));
        if (range > threshold) continue;
        g_map_flags[i] = (range == threshold) ? 4 : 3;
    }
    for (unsigned i = 0; i < MC_MAP_CELLS; ++i) {
        uint8_t x = (uint8_t)i, y = (uint8_t)(i >> 8);
        const uint16_t q[4] = { mc_cell(x, y), mc_cell(x + 1, y), mc_cell(x + 1, y + 1), mc_cell(x, y + 1) };
        int c3 = 0, c2 = 0, c5 = 0;
        for (uint16_t c : q) { c3 += g_map_flags[c] == 3; c2 += g_map_flags[c] == 2; c5 += g_map_flags[c] == 5; }
        if (c2 == 0 && c3 != 0 && c5 != 0)
            for (uint16_t c : q) if (g_map_flags[c] == 3) g_map_flags[c] = 4;
    }
}

// terrain_mark_lowland_31650(max_height, max_range): copies the flags into g_map_type (scratch, not
// read again), then any non-water cell whose 3x3 max < max_height and range <= max_range becomes 5.
// Both arguments are compared as bytes.
void terrain_mark_lowland(int max_h, int max_range) {
    std::memcpy(g_map_type, g_map_flags, MC_MAP_CELLS);
    for (unsigned i = 0; i < MC_MAP_CELLS; ++i) {
        uint8_t mx, mn;
        block_range((uint8_t)i, (uint8_t)(i >> 8), mx, mn);
        if (mx < (uint8_t)max_h && (int)mx - (int)mn <= (int)(uint8_t)max_range && g_map_flags[i] != 0)
            g_map_flags[i] = 5;
    }
}

// terrain_insert_transitions_30c50: per 2x2 quad count classes 0 / 5 / 3. 3 with 5 -> the 5s become
// 4; 3 with 0 -> the 3s become 4; 0 with 5 -> every non-water cell becomes 4. The three rewrites run
// in that order on the live flags (the counts are taken once).
void terrain_insert_transitions() {
    for (unsigned i = 0; i < MC_MAP_CELLS; ++i) {
        uint8_t x = (uint8_t)i, y = (uint8_t)(i >> 8);
        const uint16_t q[4] = { mc_cell(x, y), mc_cell(x + 1, y), mc_cell(x + 1, y + 1), mc_cell(x, y + 1) };
        int c0 = 0, c5 = 0, c3 = 0;
        for (uint16_t c : q) { c0 += g_map_flags[c] == 0; c5 += g_map_flags[c] == 5; c3 += g_map_flags[c] == 3; }
        if (c3 && c5) for (uint16_t c : q) if (g_map_flags[c] == 5) g_map_flags[c] = 4;
        if (c3 && c0) for (uint16_t c : q) if (g_map_flags[c] == 3) g_map_flags[c] = 4;
        if (c0 && c5) for (uint16_t c : q) if (g_map_flags[c] != 0) g_map_flags[c] = 4;
    }
}

// terrain_mark_interior_31800(max_height, max_range): flags copied to g_map_type (scratch); a class-5
// cell whose 3x3 max < max_height, range <= max_range and whose 8 neighbours are all class 5 or 2
// becomes 2.
void terrain_mark_interior(int max_h, int max_range) {
    std::memcpy(g_map_type, g_map_flags, MC_MAP_CELLS);
    for (unsigned i = 0; i < MC_MAP_CELLS; ++i) {
        uint8_t x = (uint8_t)i, y = (uint8_t)(i >> 8);
        uint8_t mx, mn;
        block_range(x, y, mx, mn);
        int c5 = 0, c2 = 0;
        auto cnt = [&](unsigned nx, unsigned ny) { uint8_t f = F(nx, ny); c5 += f == 5; c2 += f == 2; };
        cnt(x, y - 1); cnt(x + 1, y - 1); cnt(x + 1, y); cnt(x + 1, y + 1);
        cnt(x, y + 1); cnt(x - 1, y + 1); cnt(x - 1, y); cnt(x - 1, y - 1);
        if (mx < (uint8_t)max_h && (int)mx - (int)mn <= (int)(uint8_t)max_range &&
            g_map_flags[i] == 5 && c5 + c2 == 8)
            g_map_flags[i] = 2;
    }
}

// terrain_mark_steep_31ad0(min_range): flags copied to g_map_type (scratch); non-water cells with a
// cross range >= min_range become 6. Then a class-6 cell whose 8 neighbours mix classes becomes 1:
// with a 3 present any 2/5/4 neighbour qualifies, without a 3 either a 2 or both 5 and 4.
// Hidden Worlds (HIDDEN.EXE 0x33570, world_hidden()): steep cells become 1 instead of 6 (class 6 is
// snow there, terrain_mark_snow) and the second pass, which then selects cells == 1 and writes 1,
// changes nothing.
static void steep_to_cliff_edge(unsigned i, uint8_t x, uint8_t y);

void terrain_mark_steep(int min_range) {
    const bool hidden = world_hidden();
    std::memcpy(g_map_type, g_map_flags, MC_MAP_CELLS);
    for (unsigned i = 0; i < MC_MAP_CELLS; ++i) {
        int range = cross_range((uint8_t)i, (uint8_t)(i >> 8));
        if (g_map_flags[i] != 0 && range >= (int)(uint8_t)min_range) g_map_flags[i] = hidden ? 1 : 6;
    }
    if (hidden) return;
    for (unsigned i = 0; i < MC_MAP_CELLS; ++i) {
        if (g_map_flags[i] != 6) continue;
        steep_to_cliff_edge(i, (uint8_t)i, (uint8_t)(i >> 8));
    }
}

// The second pass of terrain_mark_steep_31ad0 (and of Hidden Worlds' terrain_mark_snow) for one class-6
// cell: it becomes 1 when its 8 neighbours mix classes.
static void steep_to_cliff_edge(unsigned i, uint8_t x, uint8_t y) {
    int c3 = 0, c2 = 0, c5 = 0, c4 = 0;
    auto cnt = [&](unsigned nx, unsigned ny) { uint8_t f = F(nx, ny); c3 += f == 3; c2 += f == 2; c5 += f == 5; c4 += f == 4; };
    cnt(x, y - 1); cnt(x + 1, y - 1); cnt(x + 1, y); cnt(x + 1, y + 1);
    cnt(x, y + 1); cnt(x - 1, y + 1); cnt(x - 1, y); cnt(x - 1, y - 1);
    bool set;
    if (c3) set = c2 || c5 || c4;
    else    set = c2 || (c5 && c4);
    if (set) g_map_flags[i] = 1;
}

// Hidden Worlds only (HIDDEN.EXE 0x31c10, called by terrain_build right after terrain_mark_steep):
// cells higher than snlin whose cross range is below snflt become class 6 (snow); then snow cells
// whose neighbours mix classes become 1, with terrain_mark_steep's rule. No RNG. The copy into
// g_map_type is dead (cleared two calls later) but kept as the original does it.
void terrain_mark_snow(int snlin, int snflt) {
    std::memcpy(g_map_type, g_map_flags, MC_MAP_CELLS);
    for (unsigned i = 0; i < MC_MAP_CELLS; ++i) {
        if (g_map_height[i] <= (uint8_t)snlin) continue;
        int range = cross_range((uint8_t)i, (uint8_t)(i >> 8));
        if (g_map_flags[i] != 0 && range < (int)(uint8_t)snflt) g_map_flags[i] = 6;
    }
    for (unsigned i = 0; i < MC_MAP_CELLS; ++i) {
        if (g_map_flags[i] != 6) continue;
        steep_to_cliff_edge(i, (uint8_t)i, (uint8_t)(i >> 8));
    }
}

// terrain_flags_fill_holes_308f0: if the north neighbour's class is non-zero and equal to the other 7
// neighbours, the centre takes it (removes single-cell holes).
void terrain_flags_fill_holes() {
    for (unsigned i = 0; i < MC_MAP_CELLS; ++i) {
        uint8_t x = (uint8_t)i, y = (uint8_t)(i >> 8);
        uint8_t n = F(x, y - 1);
        if (n == 0) continue;
        int same = (n == F(x + 1, y - 1)) + (n == F(x + 1, y)) + (n == F(x + 1, y + 1)) + (n == F(x, y + 1)) +
                   (n == F(x - 1, y + 1)) + (n == F(x - 1, y)) + (n == F(x - 1, y - 1));
        if (same == 7) g_map_flags[i] = n;
    }
}

// ---------------------------------------------------------------------------------------------
// Height clean-up

// terrain_smooth_spikes_30500: for cells with a non-zero class, min/max over the 3x3 block and the
// average of the 8 neighbours. A cell more than 4 above the min (spike) or more than 4 below the max
// (pit) becomes the average when the excess is > 10, else (h + avg) / 2.
void terrain_smooth_spikes() {
    for (unsigned i = 0; i < MC_MAP_CELLS; ++i) {
        if ((g_map_flags[i] & 7) == 0) continue;
        uint8_t x = (uint8_t)i, y = (uint8_t)(i >> 8);
        uint8_t h = g_map_height[i], mx = h, mn = h;
        unsigned sum = 0;
        auto acc = [&](unsigned nx, unsigned ny) { uint8_t v = H(nx, ny); sum += v; if (v > mx) mx = v; if (v < mn) mn = v; };
        acc(x, y - 1); acc(x + 1, y - 1); acc(x + 1, y); acc(x + 1, y + 1);
        acc(x, y + 1); acc(x - 1, y + 1); acc(x - 1, y); acc(x - 1, y - 1);
        unsigned avg = sum >> 3;
        uint8_t up = (uint8_t)(h - mn);
        if (up > 4) {
            g_map_height[i] = (uint8_t)(up > 10 ? avg : (h + avg) >> 1);
            continue;
        }
        uint8_t down = (uint8_t)(mx - h);
        if (down <= 4) continue;
        g_map_height[i] = (uint8_t)(down > 10 ? avg : (h + avg) >> 1);
    }
}

// terrain_fix_shore_quads_30810: per 2x2 quad, if the three cells other than (x,y) include both a
// class-0 and a class-4 cell and the minimum of (x,y) and the class-0 cells is 0, all four heights
// become 0.
void terrain_fix_shore_quads() {
    for (unsigned i = 0; i < MC_MAP_CELLS; ++i) {
        uint8_t x = (uint8_t)i, y = (uint8_t)(i >> 8);
        uint8_t mn = g_map_height[i];
        int water = 0, trans = 0;
        auto look = [&](unsigned nx, unsigned ny) {
            uint8_t f = F(nx, ny);
            if (f == 0) { ++water; uint8_t v = H(nx, ny); if (v < mn) mn = v; }
            else if (f == 4) ++trans;
        };
        look(x + 1, y); look(x + 1, y + 1); look(x, y + 1);
        if (trans && water && mn == 0)
            HR(x, y) = HR(x + 1, y) = HR(x + 1, y + 1) = HR(x, y + 1) = 0;
    }
}

// ---------------------------------------------------------------------------------------------
// Texturing

// terrain_assign_textures_30eb0. Bucket table: 7^4 keys x 0x19 bytes {count, tex[12], rot[12]}
// built in the frame buffer in the original (a static array here), then the compact first-match copy
// g_corner_tex_table (DAT_000b58b0). Finally every cell whose texture is 0 gets a random matching
// texture for its corner classes (x,y), (x+1,y), (x+1,y+1), (x,y+1), texture 1 when there is none;
// the rotation code is added to the class bits of the flag byte.
namespace {
constexpr int   kBucketSize = 0x19;
constexpr int   kBuckets    = 2401;
uint8_t         s_buckets[kBuckets * kBucketSize];    // DAT_0012ed74 scratch in the original

inline int tex_key(int c0, int c1, int c2, int c3) { return c0 * 0x157 + c1 * 0x31 + c2 * 7 + c3; }

inline void bucket_add(int key, uint8_t tex, uint8_t rot) {
    uint8_t *b = s_buckets + key * kBucketSize;
    uint8_t n = b[0];
    if (n < 0xc) { b[0] = (uint8_t)(n + 1); b[0xd + n] = rot; b[1 + n] = tex; }
}
}

void terrain_assign_textures() {
    for (int k = 0; k < kBuckets; ++k) s_buckets[k * kBucketSize] = 0;
    for (unsigned t = 0; t < 0x94; ++t) {
        const int8_t *e = g_corner_class_tuples + t * 4;
        int a = e[0], b = e[1], c = e[2], d = e[3];
        if (a < 0 || b < 0 || c < 0 || d < 0) continue;
        uint8_t tex = (uint8_t)t;
        // The 8 symmetries of the tuple, with the rotation code stored for each (order as 30eb0).
        bucket_add(tex_key(a, b, c, d), tex, 0x00);
        bucket_add(tex_key(b, a, d, c), tex, 0x10);
        bucket_add(tex_key(c, d, a, b), tex, 0x30);
        bucket_add(tex_key(d, c, b, a), tex, 0x20);
        bucket_add(tex_key(b, c, d, a), tex, 0x60);
        bucket_add(tex_key(c, b, a, d), tex, 0x70);
        bucket_add(tex_key(d, a, b, c), tex, 0x50);
        bucket_add(tex_key(a, d, c, b), tex, 0x40);
    }
    for (int c0 = 0; c0 < 7; ++c0)
        for (int c1 = 0; c1 < 7; ++c1)
            for (int c2 = 0; c2 < 7; ++c2)
                for (int c3 = 0; c3 < 7; ++c3) {
                    int key = tex_key(c0, c1, c2, c3);
                    const uint8_t *b = s_buckets + key * kBucketSize;
                    if (b[0]) { g_corner_tex_table[key][0] = b[1]; g_corner_tex_table[key][1] = b[0xd]; }
                    else      { g_corner_tex_table[key][0] = 1;    g_corner_tex_table[key][1] = 0; }
                }
    for (unsigned i = 0; i < MC_MAP_CELLS; ++i) {
        if (g_map_type[i] != 0) continue;
        uint8_t x = (uint8_t)i, y = (uint8_t)(i >> 8);
        int key = tex_key(F(x, y) & 7, F(x + 1, y) & 7, F(x + 1, y + 1) & 7, F(x, y + 1) & 7);
        const uint8_t *b = s_buckets + key * kBucketSize;
        uint8_t n = b[0];
        if (n == 0) { g_map_type[i] = 1; continue; }
        uint8_t r = (uint8_t)(mc_rng16_next() % (uint16_t)(n + 1));
        if (r >= n) r = 0;
        g_map_type[i]  = b[1 + r];
        g_map_flags[i] = (uint8_t)((g_map_flags[i] & 7) + b[0xd + r]);
    }
}

// terrain_mark_water_anim_30690: clear flag bit 3 everywhere; set it on height-0 cells whose 8
// neighbours are height 0 and whose 4 surrounding quads (x,y), (x-1,y), (x-1,y-1), (x,y-1) have
// texture 0.
void terrain_mark_water_anim() {
    for (unsigned i = 0; i < MC_MAP_CELLS; ++i) {
        g_map_flags[i] &= 0xf7;
        if (g_map_height[i] != 0) continue;
        uint8_t x = (uint8_t)i, y = (uint8_t)(i >> 8);
        int nz = (H(x, y - 1) != 0) + (H(x + 1, y - 1) != 0) + (H(x + 1, y) != 0) + (H(x + 1, y + 1) != 0) +
                 (H(x, y + 1) != 0) + (H(x - 1, y + 1) != 0) + (H(x - 1, y) != 0) + (H(x - 1, y - 1) != 0);
        if (nz) continue;
        int tz = (T(x, y) != 0) + (T(x - 1, y) != 0) + (T(x - 1, y - 1) != 0) + (T(x, y - 1) != 0);
        if (tz) continue;
        g_map_flags[i] |= 8;
    }
}

// terrain_build_lightmap_31310: g_rng16 := 0, then light = 0x20 - (h[x+1,y+1] - h[x-1,y-1]) as a
// byte; exactly 0x20 (flat) -> rng % 9 + 0x1c; signed < 0x1c -> (v & 3) + 0x1c; signed > 0x28 ->
// (v & 7) + 0x28.
void terrain_build_lightmap() {
    g_rng16 = 0;
    for (unsigned i = 0; i < MC_MAP_CELLS; ++i) {
        uint8_t x = (uint8_t)i, y = (uint8_t)(i >> 8);
        uint8_t v = (uint8_t)(0x20 - (uint8_t)(H(x + 1, y + 1) - H(x - 1, y - 1)));
        if (v == 0x20)               v = (uint8_t)(mc_rng16_next() % 9 + 0x1c);
        else if ((int8_t)v < 0x1c)   v = (uint8_t)((v & 3) + 0x1c);
        else if ((int8_t)v > 0x28)   v = (uint8_t)((v & 7) + 0x28);
        g_map_light[i] = v;
    }
}

// ---------------------------------------------------------------------------------------------

// terrain_build_303f0(level header)
void terrain_build(const GenMap &gen) {
    g_rng16       = (uint16_t)gen.seed;
    g_state->rng  = (uint32_t)gen.seed;
    // The original first tries to load the 64 KB cached height map "c:/carpet.cd/save/scanned.rmd"
    // (file_load_rnc_3cbe0 into g_map_height, taken when exactly 0x10000 bytes come back). The port
    // never has that file, so the fractal path is unconditional.
    // The fractal relies on a zeroed scratch; the original gets that from the per-level reset and
    // from terrain_generate. Clearing it here makes terrain_build self-contained (no observable
    // difference).
    std::memset(g_cell_things, 0, sizeof(uint16_t) * MC_MAP_CELLS);
    terrain_fractal_fill(g_rng16, (uint16_t)gen.off, (uint16_t)gen.raise, (uint16_t)gen.gnarl);
    terrain_generate();
    std::memset(g_cell_things, 0, sizeof(uint16_t) * MC_MAP_CELLS);
    terrain_carve_rivers(gen.river, (uint16_t)gen.sourc);
    terrain_flatten_water_quads();
    terrain_classify_flat((uint16_t)gen.snflt);
    terrain_mark_lowland((uint16_t)gen.bhlin, (uint16_t)gen.bhflt);
    terrain_insert_transitions();
    terrain_mark_interior((uint16_t)gen.bhlin, (uint16_t)gen.bhflt);
    terrain_mark_steep((uint16_t)gen.rkste);
    if (world_hidden()) terrain_mark_snow((uint16_t)gen.snlin, (uint16_t)gen.snflt);   // Hidden Worlds
    terrain_flags_fill_holes();
    std::memset(g_map_type, 0, MC_MAP_CELLS);
    terrain_smooth_spikes();
    terrain_fix_shore_quads();
    terrain_assign_textures();
    terrain_mark_water_anim();
    terrain_build_lightmap();
}

// terrain_sample_height_71e00(x, y): height at a world position, interpolated over the triangle of
// the cell quad that contains the point. Quads alternate their diagonal with the parity of
// (x_cell + y_cell): even quads split along the (x,y)-(x+1,y+1) diagonal, odd ones along
// (x+1,y)-(x,y+1). Result = corner height * 0x20 + (gradient terms) >> 3 (fractions are 0..255).
int terrain_sample_height(int32_t x, int32_t y) {
    const int cx = world_cx(x), cy = world_cx(y);
    int fx = x & 0xff, fy = y & 0xff;
    auto hw = [](int cx_, int cy_) -> int { return g_map_height[world_cell(cx_, cy_)]; };
    int h00 = hw(cx, cy), h10 = hw(cx + 1, cy), h01 = hw(cx, cy + 1), h11 = hw(cx + 1, cy + 1);
    if (((cx + cy) & 1) == 0) {
        if (fx > fy) return h00 * 0x20 + (((h11 - h10) * fy + (h10 - h00) * fx) >> 3);
        return h00 * 0x20 + (((h11 - h01) * fx + (h01 - h00) * fy) >> 3);
    }
    if (fx + fy < 256) return h00 * 0x20 + (((h10 - h00) * fx + (h01 - h00) * fy) >> 3);
    return h01 * 0x20 + (((h10 - h11) * (255 - fy) + (h11 - h01) * fx) >> 3);
}
