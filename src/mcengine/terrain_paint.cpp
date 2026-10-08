// Run-time terrain painting (0x31f90..0x329c0, 0x3d5f0..0x3da00, 0x23f20) and the spiral area
// search (0x10080..0x102b0). Translated from the disassembly of carpet.exe.
#include "terrain_paint.h"
#include "mc_math.h"
#include "terrain.h"
#include "world.h"
#include "gen/features_tables.h"
#include "mcfile.h"
#include <cstdio>
#include <cstring>

int32_t g_terrain_nearly_flat = 0;

// Round 11: cells through world.h (the original's (y << 8) | x on 8-bit coordinates at N = 256). A cell
// index handed in is wrapped onto the map (the original's `& 0xffff`).
static inline unsigned cell_xy(int x, int y) { return world_cell(x, y); }
static inline unsigned cell_wrap(unsigned cell) { return cell & (g_world.cells - 1); }

// ---- spiral search (spiral_search_init_101b0 / _begin_10080 / _next_10120 / _end_10100) ---------
// search.dat is a 32x32 image of ring numbers (0 = the 2x2 centre quad, 15 = outermost, 254 = unused).
// The original converts it once into a list of 4-byte entries {dx, dy, ring, -} sorted by ring (scan
// order inside a ring) in the '*SearchD' buffer plus a 32-entry ring table {first entry, count}; a
// search is a cursor over that list. The 100 search slots of the original (0xac160, 0x18 bytes each)
// only exist to hand out cursors, so the port keeps the cursor on the caller's stack.
namespace {

struct SpiralEntry { int8_t dx, dy; uint8_t ring, pad; };
struct SpiralRing  { uint32_t first; uint16_t count; };

constexpr int kSpiralCells = 32 * 32;
SpiralEntry s_spiral_entries[kSpiralCells + 4];   // zero entries behind the list (the original reads its heap there)
SpiralRing  s_spiral_rings[33];                   // [32] = the word behind the table (count 0)
bool        s_spiral_loaded = false;
bool        s_spiral_failed = false;

// spiral_search_init_101b0
void spiral_search_init(const uint8_t *image) {
    std::memset(s_spiral_entries, 0, sizeof s_spiral_entries);
    uint32_t n = 0;
    int centre_x = 0, centre_y = 0;
    for (int ring = 0; ring < 32; ring++) {
        s_spiral_rings[ring].first = n;
        s_spiral_rings[ring].count = 0;
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 32; x++) {
                if (image[y * 32 + x] != ring) continue;
                if (ring == 0 && n == 0) { centre_x = x; centre_y = y; }
                s_spiral_entries[n].dx = (int8_t)(x - centre_x);
                s_spiral_entries[n].dy = (int8_t)(y - centre_y);
                s_spiral_entries[n].ring = (uint8_t)ring;
                s_spiral_rings[ring].count++;
                n++;
            }
    }
    s_spiral_rings[32].first = n;
    s_spiral_rings[32].count = 0;
}

bool spiral_search_ready() {
    if (s_spiral_loaded) return true;
    if (s_spiral_failed) return false;
    return terrain_paint_load_data(MC_DEFAULT_GAME_DIR);
}

}  // namespace

// spiral_search_begin_10080(start ring, end ring)
bool spiral_search_begin(SpiralSearch *s, int start, int end) {
    if (!spiral_search_ready()) return false;
    if (start < 0) start = 0;
    if (start > 32) start = 32;
    s->cur = start;
    s->end = end;
    s->idx = 0;
    s->ring = start;
    s->entry = (int)s_spiral_rings[start].first;
    return true;
}

// spiral_search_next_10120: 1 = the offset is valid and more follow, 2 = the walk is over. The
// offset returned together with 2 (the last cell of the last ring) is dropped by every caller.
int spiral_search_next(SpiralSearch *s, int *dx, int *dy) {
    const SpiralEntry &e = s_spiral_entries[s->entry < kSpiralCells + 4 ? s->entry : kSpiralCells + 3];
    *dx = e.dx;
    *dy = e.dy;
    s->entry++;
    s->idx++;
    if ((int)s_spiral_rings[s->ring].count > s->idx) return 1;
    if (s->ring < 32) s->ring++;
    s->idx = 0;
    s->cur++;
    return s->cur <= s->end ? 1 : 2;
}

bool terrain_paint_load_data(const char *game_dir) {
    char path[1024];
    uint8_t image[kSpiralCells];
    mc_path_join(path, sizeof path, game_dir, "data/search.dat");
    long got = mc_load_rnc_into(path, image, sizeof image);
    if (got != (long)sizeof image) {
        std::fprintf(stderr, "terrain_paint: %s: expected %d bytes, got %ld\n", path, kSpiralCells, got);
        s_spiral_failed = true;
        return false;
    }
    spiral_search_init(image);
    s_spiral_loaded = true;
    s_spiral_failed = false;
    return true;
}

// ---- slope orientation / paint kinds ------------------------------------------------------------

// terrain_slope_orientation_31f90
int terrain_slope_orientation(unsigned cell, unsigned dl_in, unsigned cl_in) {
    const int x = world_cell_x(cell), y = world_cell_y(cell);
    const uint8_t h[4] = {
        g_map_height[cell_xy(x, y)],
        g_map_height[cell_xy(x + 1, y)],
        g_map_height[cell_xy(x + 1, y + 1)],
        g_map_height[cell_xy(x, y + 1)],
    };
    uint8_t lo = 0xff, hi = 0;
    uint8_t hi_idx = (uint8_t)dl_in;        // DL: only written when a corner is above 0
    for (int i = 0; i < 4; i++) {
        if (h[i] > hi) { hi = h[i]; hi_idx = (uint8_t)i; }
        if (h[i] < lo) lo = h[i];
    }
    uint8_t second = 0;
    uint8_t second_idx = (uint8_t)cl_in;    // CL: only written when another corner is above 0
    for (int i = 0; i < 4; i++) {
        if (hi_idx == i) continue;
        if (h[i] > second) { second = h[i]; second_idx = (uint8_t)i; }
    }
    g_terrain_nearly_flat = (hi - lo <= 8) ? 1 : 0;
    if (hi - second >= 8) return hi_idx;
    switch (hi_idx) {
    case 0:  return second_idx == 1 ? 4 : 7;
    case 1:  return second_idx == 2 ? 5 : 4;
    case 2:  return second_idx == 3 ? 6 : 5;
    case 3:  return second_idx == 0 ? 7 : 6;
    default: return 0;
    }
}

// terrain_paint_cell_32150
void terrain_paint_cell(unsigned cell_in, unsigned kind_in, unsigned dl_in, unsigned cl_in) {
    unsigned cell = cell_wrap(cell_in);
    const int x = world_cell_x(cell), y = world_cell_y(cell);
    uint8_t kind = (uint8_t)kind_in;
    unsigned checker = ((unsigned)(x + y) & 1) << 3;     // parity: the same with or without the byte wrap
    if (kind < 8) {
        g_map_flags[cell] = (uint8_t)((g_map_flags[cell] & 0xf0) | kind);
        terrain_retexture_rect(cell, cell);
        return;
    }
    const uint8_t *tab = nullptr;           // {texture, rotation}
    const uint8_t *t218 = g_slope_tex_tables;             // 0x94218
    const uint8_t *t228 = g_slope_tex_tables + 0x10;      // 0x94228
    const uint8_t *t258 = g_slope_tex_tables + 0x40;      // 0x94258
    const uint8_t *t268 = g_slope_tex_tables + 0x50;      // 0x94268
    const uint8_t *t298 = g_slope_tex_tables + 0x80;      // 0x94298
    switch (kind) {
    case 0x08: g_map_type[cell] = 8; break;
    case 0x09: g_map_type[cell] = 9; break;
    case 0x0f: g_map_type[cell] = 0xb; break;
    case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x0e: {
        unsigned o = (unsigned)terrain_slope_orientation(cell, dl_in, cl_in);
        o = (o + (kind - 0x0au) * 0x10u) & 0xff;
        if (g_terrain_nearly_flat) o = (o + 8) & 0xff;
        tab = t298 + o * 2;
        break;
    }
    case 0x10: {
        uint8_t tex = g_map_type[cell];
        if (tex == 0xa || tex == 0xb || tex == 0xc) break;
        tab = t218 + (unsigned)terrain_slope_orientation(cell, tex, cl_in) * 2;   // DL = the cell's texture here
        break;
    }
    case 0x11: tab = t258 + (unsigned)terrain_slope_orientation(cell, dl_in, cl_in) * 2; break;
    case 0x12: tab = t268 + (checker + (unsigned)terrain_slope_orientation(cell, dl_in, cl_in)) * 2; break;
    case 0x13: tab = t268 + 0x10 + (checker + (unsigned)terrain_slope_orientation(cell, dl_in, cl_in)) * 2; break;
    case 0x14: tab = t228 + (unsigned)terrain_slope_orientation(cell, dl_in, cl_in) * 2; break;
    case 0x15: tab = t228 + 0x10 + (unsigned)terrain_slope_orientation(cell, dl_in, cl_in) * 2; break;
    case 0x16: tab = t228 + 0x20 + (unsigned)terrain_slope_orientation(cell, dl_in, cl_in) * 2; break;
    default: break;                          // kinds above 0x16 only mark the cell
    }
    if (tab) {
        g_map_type[cell] = tab[0];
        g_map_flags[cell] = (uint8_t)(tab[1] | (g_map_flags[cell] & 0x8f));
    }
    g_map_flags[cell] = (uint8_t)((g_map_flags[cell] | 0x80) & 0xf7);
    g_map_flags[cell_xy(x + 1, y)] &= 0xf7;
    g_map_flags[cell_xy(x + 1, y + 1)] &= 0xf7;
    g_map_flags[cell_xy(x, y + 1)] &= 0xf7;
}

// Light value of one cell as the two retexture functions compute it (8-bit arithmetic on the heights).
static inline int8_t light_slope(int x, int y) {
    uint8_t d = (uint8_t)(g_map_height[cell_xy(x + 1, y + 1)] - g_map_height[cell_xy(x - 1, y - 1)]);
    return (int8_t)(uint8_t)(0x20 - d);
}

// terrain_set_quad_texture_32430
void terrain_set_quad_texture(unsigned cell, unsigned texture) {
    const int x = world_cell_x(cell), y = world_cell_y(cell);
    const uint8_t tex = (uint8_t)texture;
    g_map_type[cell_xy(x, y)] = tex;
    g_map_type[cell_xy(x - 1, y)] = tex;
    g_map_type[cell_xy(x - 1, y - 1)] = tex;
    g_map_type[cell_xy(x, y - 1)] = tex;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            const int px = world_wrap_cell(x - 1 + c), py = world_wrap_cell(y - 1 + r);
            int8_t v = light_slope(px, py);
            if (v < 0x20) v = 0x20;
            else if (v > 0x28) v = (int8_t)((v & 7) + 0x28);
            unsigned p = cell_xy(px, py);
            g_map_light[p] = (uint8_t)v;
            g_map_flags[p] &= 0xf7;
        }
}

// terrain_retexture_rect_324e0 / terrain_retexture_rect_force_32760
// The rectangle's width / height are cell-coordinate differences + 1 wrapped onto the world like the
// original's bytes (a rectangle across the seam works; w == 0 walks nothing, as the byte counters do).
static void retexture_rect(unsigned c0, unsigned c1, bool force) {
    int x0 = world_cell_x(c0), y0 = world_cell_y(c0);
    int w = world_wrap_cell(world_cell_x(c1) - x0 + 1);
    int h = world_wrap_cell(world_cell_y(c1) - y0 + 1);
    auto mark = [force](int x, int y) {
        unsigned c = cell_xy(x, y);
        if (force || !(g_map_flags[c] & 0x80)) g_map_type[c] = 1;
    };
    // pass 1: mark the quads touching the rectangle
    int cx = x0, cy = y0;
    for (int r = h; r != 0; r--) {
        for (int c = w; c != 0; c--) {
            mark(cx, cy);
            mark(cx - 1, cy);
            mark(cx - 1, cy - 1);
            mark(cx, cy - 1);
            cx = world_wrap_cell(cx + 1);
        }
        cx = world_wrap_cell(cx - w);
        cy = world_wrap_cell(cy + 1);
    }
    // pass 2: marked cells take the texture of their corner-class tuple
    x0 = world_wrap_cell(x0 - 1); y0 = world_wrap_cell(y0 - 1); w = world_wrap_cell(w + 1); h = world_wrap_cell(h + 1);
    cx = x0; cy = y0;
    for (int r = h; r != 0; r--) {
        for (int c = w; c != 0; c--) {
            unsigned p = cell_xy(cx, cy);
            if (g_map_type[p] == 1) {
                unsigned idx = (g_map_flags[p] & 7u) * 0x157u
                             + (g_map_flags[cell_xy(cx + 1, cy)] & 7u) * 0x31u
                             + (g_map_flags[cell_xy(cx + 1, cy + 1)] & 7u) * 7u
                             + (g_map_flags[cell_xy(cx, cy + 1)] & 7u);
                uint8_t tex = g_corner_tex_table[idx][0];
                g_map_type[p] = tex;
                if (tex < 8)
                    g_map_flags[p] = (uint8_t)((g_map_flags[p] & 0x87) + (uint8_t)((mc_rng16_next() % 7) << 4));
                else
                    g_map_flags[p] = (uint8_t)((g_map_flags[p] & 0x87) + g_corner_tex_table[idx][1]);
            }
            cx = world_wrap_cell(cx + 1);
        }
        cx = world_wrap_cell(cx - w);
        cy = world_wrap_cell(cy + 1);
    }
    // pass 3: light map and water-animation bit over the rectangle + 1
    w = world_wrap_cell(w + 1); h = world_wrap_cell(h + 1);
    cx = x0; cy = y0;
    for (int r = h; r != 0; r--) {
        for (int c = w; c != 0; c--) {
            int8_t v = light_slope(cx, cy);
            if (v < 0x1c) v = (int8_t)((v & 3) + 0x1c);
            else if (v > 0x28) v = (int8_t)((v & 7) + 0x28);
            unsigned p = cell_xy(cx, cy);
            g_map_light[p] = (uint8_t)v;
            g_map_flags[p] &= 0xf7;
            cx = world_wrap_cell(cx + 1);
        }
        cx = world_wrap_cell(cx - w);
        cy = world_wrap_cell(cy + 1);
    }
}

void terrain_retexture_rect(unsigned c0, unsigned c1)       { retexture_rect(cell_wrap(c0), cell_wrap(c1), false); }
void terrain_retexture_rect_force(unsigned c0, unsigned c1) { retexture_rect(cell_wrap(c0), cell_wrap(c1), true); }

// ---- per-cell height modifiers ------------------------------------------------------------------

// terrain_cell_is_nonland_3d5f0
int terrain_cell_is_nonland(unsigned cell) {
    uint8_t c = g_map_flags[cell_wrap(cell)] & 7;
    return (c == 5 || c == 2 || c == 3) ? 0 : 1;
}

// Shared tail of 3d620 / 3d7d0 for a cell that went down to height 0: the class nibble is cleared
// (water) only when none of the 8 neighbours is a land class; otherwise the flags stay as they are.
static void clear_class_if_surrounded(unsigned cell) {
    const int x = world_cell_x(cell), y = world_cell_y(cell);
    static const int8_t order[8][2] = { {-1,-1}, {0,-1}, {1,-1}, {1,0}, {-1,0}, {-1,1}, {0,1}, {1,1} };
    for (const auto &o : order)
        if (!terrain_cell_is_nonland(cell_xy(x + o[0], y + o[1]))) return;
    g_map_flags[cell] &= 0xf0;
}

// terrain_modify_cell_3d620. x / y are cell coordinates (wrapped here). `origin` is the original's test of
// the *argument* (0, 0), i.e. world cell (0, 0) for the spiral callers, whose arguments stay within
// -N/2 - 16 .. N/2 + 16 (spiral_origin), so the int16 test means the same on any N <= 4096.
int terrain_modify_cell(int x, int y, int delta, int keep_built) {
    unsigned cell = cell_xy(x, y);
    bool origin = (int16_t)x == 0 && (int16_t)y == 0;
    int result = 0;
    int16_t h = (int16_t)(g_map_height[cell] + delta);
    if (h > 0xc8) { h = 0xc8; if (origin) result = 1; }
    if (h < 0)    { h = 0;    if (origin) result = 1; }
    if ((uint8_t)keep_built != 0 && (g_map_flags[cell] & 0x80)) return 1;
    g_map_height[cell] = (uint8_t)h;
    if (h != 0) g_map_flags[cell] = (uint8_t)((g_map_flags[cell] & 0xf8) | 1);
    else        clear_class_if_surrounded(cell);
    if ((uint8_t)keep_built != 0) terrain_retexture_rect(cell, cell);
    else                          terrain_retexture_rect_force(cell, cell);
    return result;
}

// terrain_set_cell_height_3d7d0
int terrain_set_cell_height(int x, int y, int height) {
    unsigned cell = cell_xy(x, y);
    bool origin = (int16_t)x == 0 && (int16_t)y == 0;
    int result = 0;
    int16_t h = (int16_t)height;
    if (h > 0xff) { h = 0xff; if (origin) result = 1; }
    if (h < 0)    { h = 0;    if (origin) result = 1; }
    if (g_map_flags[cell] & 0x80) return 1;
    g_map_height[cell] = (uint8_t)h;
    if (h != 0) g_map_flags[cell] = (uint8_t)((g_map_flags[cell] & 0xf8) | 1);
    else        clear_class_if_surrounded(cell);
    terrain_retexture_rect(cell, cell);
    return result;
}

// Centre cell and ring limit shared by 3d940 / 23f20: cell = (pos + 0x80) >> 8, limit = ext_x / 256.
// The centre is the signed coordinate's cell (-N/2 .. N/2 - 1; the original's (int16_t) word at N = 256);
// terrain_modify_cell wraps centre + offset onto the world.
static void spiral_origin(const Thing *t, int *cx, int *cy, int *ring1) {
    *cx = (world_signed(t->x) + 0x80) >> 8;
    *cy = (world_signed(t->y) + 0x80) >> 8;
    int limit = t->ext_x / 256;
    if ((int16_t)*ring1 > limit) *ring1 = limit;
    *ring1 = (int16_t)*ring1;
}

// terrain_find_cell_spiral_3d940
int terrain_find_cell_spiral(Thing *t, int ring0, int ring1, int delta, int keep_built) {
    int cx, cy;
    spiral_origin(t, &cx, &cy, &ring1);
    SpiralSearch s;
    if (!spiral_search_begin(&s, (int16_t)ring0, ring1)) return 0;
    int dx, dy;
    while (spiral_search_next(&s, &dx, &dy) == 1)
        if ((uint8_t)terrain_modify_cell((int16_t)(dx + cx), (int16_t)(dy + cy), (int16_t)delta, keep_built & 0xff))
            return 1;
    return 0;
}

// terrain_modify_cells_spiral_23f20
void terrain_modify_cells_spiral(Thing *t, int ring0, int ring1) {
    int cx, cy;
    spiral_origin(t, &cx, &cy, &ring1);
    SpiralSearch s;
    if (!spiral_search_begin(&s, (int16_t)ring0, ring1)) return;
    int dx, dy;
    while (spiral_search_next(&s, &dx, &dy) == 1)
        terrain_modify_cell(dx + cx, dy + cy, -3, 0);
}
