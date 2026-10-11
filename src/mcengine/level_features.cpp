// Level-start feature generation, terrain-shaping effects and castle footprints.
// Translated from the disassembly of carpet.exe (addresses in the comments).
#include "level_features.h"
#include "terrain_paint.h"
#include "spatial.h"
#include "mc_math.h"
#include "terrain.h"
#include "mcfile.h"
#include "settings.h"
#include "world.h"
#include "mode_rules.h"
#include "mode.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

void (*g_hook_player_note_ridge_distance)(Thing *t) = nullptr;

namespace {

// Round 11: cells and cell coordinates through world.h. Cell coordinates are ints kept in 0 .. N-1
// (world_wrap_cell wherever the original wrapped a byte); at N = 256 every expression is the original's.
inline unsigned cell_xy(int x, int y) { return world_cell(x, y); }
inline int wrap(int c) { return world_wrap_cell(c); }
inline unsigned cell_wrap(unsigned cell) { return cell & (g_world.cells - 1); }
// Cell of a thing as the effect handlers compute it: (coordinate + 0x80) >> 8 on the signed word, as a
// byte (== world_cx(x + 0x80) at N = 256).
inline int thing_cell_x(const Thing *t) { return world_cx(t->x + 0x80); }
inline int thing_cell_y(const Thing *t) { return world_cx(t->y + 0x80); }
inline uint32_t thing_rng_next(Thing *t) { t->rng = mc_lcg(t->rng); return t->rng; }
inline uint16_t rd16(const uint8_t *p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
inline void     wr16(uint8_t *p, uint16_t v) { std::memcpy(p, &v, 2); }

// ---- castle footprint maps (data/building.tab + building.dat, pointer at 0xadfb0) ---------------

constexpr unsigned kMaxFootprints = 256;        // Thing.castle_size is a byte
CastleFootprint s_footprints[kMaxFootprints];
mc_blob         s_building_dat = { nullptr, 0 };
bool            s_data_loaded = false, s_data_failed = false;
const uint8_t   s_empty_map[4] = { 0, 0, 0, 0 };

bool data_ready() {
    if (s_data_loaded) return true;
    if (s_data_failed) return false;
    return level_features_load_data(MC_DEFAULT_GAME_DIR);
}

// ---- port (Conquest; user, 2026-10-11): castle levels 8..11 --------------------------------------------------
// The original's castle ends at level 7: footprints 1..7, each drawing only what its level adds (2 / 4 / 6: the
// ground and the towers of the next ring, 3 / 5 / 7: the walls between them, with spurs to the ring inside; 0 =
// leave the cell). Slots 8..16 of building.tab are 1 x 1 dummies nothing refers to. Levels 8..11 are two more rings
// made from the last one: footprint 6 (towers) and 7 (walls), both 48 x 48, pulled 8 cells further out on every
// side - 8 = 6 stretched, 9 = 7 stretched (64 x 64), 10 and 11 the same again (80 x 80). "Stretched" repeats four
// columns (and rows) twice more in each half, in the part of the walls where the battlements repeat every four
// cells and that no tower or spur crosses, so the corners, the gate towers and the spurs keep their shape.
using FootGrid = std::vector<std::vector<uint8_t>>;
std::vector<uint8_t> s_extra_maps[4];

FootGrid footprint_decode(const CastleFootprint &f) {
    FootGrid g(f.h, std::vector<uint8_t>(f.w, 0));
    uint32_t i = 0;
    unsigned row = 0, x = 0;
    while (row < f.h && i < f.map_len) {
        const int8_t c = (int8_t)f.map[i++];
        if (c == 0) { row++; x = 0; }
        else if (c < 0) x += (unsigned)-c;
        else for (int n = c; n != 0 && i < f.map_len; n--, x++) { const uint8_t b = f.map[i++]; if (x < f.w) g[row][x] = b; }
    }
    return g;
}

// `prev` > 0 (a tower level): the cells left alone are exactly the square of the level below, centred - the rest
// of the stretched hole becomes level ground like the other cells around the towers.
FootGrid footprint_stretch(const FootGrid &g, int prev) {
    const int n = (int)g.size();
    std::vector<int> from;
    for (int i = 0; i < n; i++) {
        from.push_back(i);
        if (i == 17 || i == n - 15) for (int k = 0; k < 8; k++) from.push_back(i - 3 + k % 4);
    }
    const int m = (int)from.size(), off = (m - prev) / 2;
    FootGrid out((size_t)m, std::vector<uint8_t>((size_t)m, 0));
    for (int r = 0; r < m; r++)
        for (int c = 0; c < m; c++) {
            uint8_t b = g[(size_t)from[(size_t)r]][(size_t)from[(size_t)c]];
            if (prev > 0 && b == 0 && (r < off || r >= off + prev || c < off || c >= off + prev)) b = 7;
            out[(size_t)r][(size_t)c] = b;
        }
    return out;
}

void footprint_install(unsigned slot, const FootGrid &g, std::vector<uint8_t> &store) {
    store.clear();
    for (const auto &row : g) {
        for (size_t c = 0; c < row.size(); c += 100) {
            const size_t len = std::min<size_t>(100, row.size() - c);
            store.push_back((uint8_t)len);
            store.insert(store.end(), row.begin() + (long)c, row.begin() + (long)(c + len));
        }
        store.push_back(0);
    }
    s_footprints[slot].map = store.data();
    s_footprints[slot].map_len = (uint32_t)store.size();
    s_footprints[slot].w = s_footprints[slot].h = (uint8_t)g.size();
}

void footprints_add_levels_8_to_11() {
    if (s_footprints[6].w != 48 || s_footprints[6].h != 48 || s_footprints[7].w != 48 || s_footprints[7].h != 48) return;
    const FootGrid g8 = footprint_stretch(footprint_decode(s_footprints[6]), 48);
    const FootGrid g9 = footprint_stretch(footprint_decode(s_footprints[7]), 0);
    const FootGrid g10 = footprint_stretch(g8, 64), g11 = footprint_stretch(g9, 0);
    footprint_install(8, g8, s_extra_maps[0]);
    footprint_install(9, g9, s_extra_maps[1]);
    footprint_install(10, g10, s_extra_maps[2]);
    footprint_install(11, g11, s_extra_maps[3]);
    g_castle_footprints_to_11 = true;       // castle_max_level (player.h)
}

}  // namespace

bool level_features_load_data(const char *game_dir) {
    char path[1024];
    mc_blob tab = { nullptr, 0 }, dat = { nullptr, 0 };
    bool ok = terrain_paint_load_data(game_dir);
    mc_path_join(path, sizeof path, game_dir, "data/building.tab");
    bool have_tab = mc_read_unpacked(path, &tab) != 0;
    mc_path_join(path, sizeof path, game_dir, "data/building.dat");
    bool have_dat = mc_read_unpacked(path, &dat) != 0;
    for (auto &f : s_footprints) { f.map = s_empty_map; f.map_len = sizeof s_empty_map; f.w = f.h = 0; }
    if (have_tab && have_dat) {
        mc_blob_free(&s_building_dat);
        s_building_dat = dat;
        size_t n = tab.len / 6;
        for (size_t i = 0; i < n && i < kMaxFootprints; i++) {
            const uint8_t *e = tab.data + i * 6;
            uint32_t off;
            std::memcpy(&off, e, 4);
            if (off >= dat.len) continue;
            s_footprints[i].map = dat.data + off;
            s_footprints[i].map_len = (uint32_t)(dat.len - off);
            s_footprints[i].w = e[4];
            s_footprints[i].h = e[5];
        }
        footprints_add_levels_8_to_11();
    } else {
        std::fprintf(stderr, "level_features: data/building.tab / building.dat missing in %s\n", game_dir);
        if (have_dat) mc_blob_free(&dat);
        ok = false;
    }
    if (have_tab) mc_blob_free(&tab);
    s_data_loaded = ok;
    s_data_failed = !ok;
    return ok;
}

// The original keeps data/building.tab in the tab list 0x9744c that ui_sprite_lists_relocate_49de0
// relocates at start-up and after every resolution switch: in 320x200 (video flags bit 0)
// relocate_tab_double_62a80 doubles every entry's w and h (`shl byte [e+4], 1`, wrapping at 256), which
// the castle code halves again (`== 1` tests). Net effect: the same footprints in both modes, but
// effect_wizard_init_35090 computes the capacity w * h >> 4 before halving, i.e. 4x in 320x200.
// The port keeps the raw table and applies the doubling on access.
const CastleFootprint *castle_footprint(unsigned size) {
    if (!data_ready()) {
        static const CastleFootprint none = { s_empty_map, sizeof s_empty_map, 0, 0 };
        return &none;
    }
    const CastleFootprint *raw = &s_footprints[size < kMaxFootprints ? size : 0];
    if (!(g_video_mode_flags & 1)) return raw;
    static CastleFootprint doubled[4];   // a few live results (callers use the pointer immediately)
    static unsigned next = 0;
    CastleFootprint &d = doubled[next++ & 3];
    d = *raw;
    d.w = (uint8_t)(raw->w << 1);
    d.h = (uint8_t)(raw->h << 1);
    return &d;
}

namespace {

// Walk a span-encoded footprint map: `rows` rows starting at (x0, y0); fn(cell, instruction byte,
// read position after the byte) for every instruction byte.
template <class F>
void footprint_walk(const CastleFootprint *fp, uint32_t rows, int x0, int y0, F &&fn) {
    uint32_t i = 0;
    int x = x0, y = y0;
    while (rows != 0 && i < fp->map_len) {
        int8_t c = (int8_t)fp->map[i++];
        if (c == 0) {
            y = wrap(y + 1);
            rows--;
            x = x0;
        } else if (c < 0) {
            x = wrap(x - c);
        } else {
            for (int n = c; n != 0 && i < fp->map_len; n--) {
                uint8_t b = fp->map[i++];
                fn(cell_xy(x, y), b, i);
                x = wrap(x + 1);
            }
        }
    }
}

// Paint kind of a footprint instruction byte (second pass of 26320 / 26680); -1 = leave the cell.
int footprint_paint_kind(uint8_t b) {
    unsigned nib = b >> 4;
    if (nib == 0) {
        unsigned m = b % 7u;
        return m == 0 ? -1 : (int)m - 1;
    }
    if (nib <= 2) return (int)nib + 7;
    if (nib == 3) return (int)((10 + (b % 16u) / 3u) & 0xff);
    return (int)((nib + 0xb) & 0xff);
}

// DL at the terrain_paint_cell call of 26320 / 26680: the last idiv remainder ((b % 16) % 3 for the
// terrace bytes, b % 7 for the class bytes) or 0 after the sign extension of the other paths. CL holds
// the instruction byte or the paint kind there, never a corner index 0..3, hence the default 0xff.
unsigned footprint_paint_dl(uint8_t b) {
    unsigned nib = b >> 4;
    if (nib == 0) return b % 7u;
    if (nib == 3) return (b % 16u) % 3u;
    return 0;
}

// Height a footprint instruction byte asks for, relative to the castle base (first pass of 26320 /
// 26680); false = the byte does not touch the height.
bool footprint_height(uint8_t b, int *offset) {
    if (b >= 0xf) {
        unsigned nib = b >> 4, low = b % 16u;
        if (nib == 3) {
            unsigned m = low % 3u;
            if (m == 0) return false;
            *offset = m == 1 ? 0xc : 0x10;
            return true;
        }
        if (low == 0) return false;
        *offset = (int)((low - 1) & 0xff) * 4;
        return true;
    }
    if (b <= 6) return false;
    *offset = 0;
    return true;
}

}  // namespace

// ---- helpers 0x34250..0x34bb0 ----------------------------------------------------------------

// math_wrap_diff_34250 (16-bit arithmetic: right for moduli up to 0x8000 and |b - a| < 0x8000, i.e. cell
// coordinates of any world up to 4096 cells; callers pass g_world.size where the modulus is the world)
int math_wrap_diff(int a, int b, int modulus) {
    int32_t d = b - a;
    int32_t half = (int16_t)modulus >> 1;
    if ((int16_t)d > half) return (int16_t)(d - modulus);
    if ((int16_t)d < -half) d += modulus;
    return (int16_t)d;
}

// terrain_set_pos_scratch_34280
void terrain_set_pos_scratch(int x0, int y0, int x1, int y1) {
    uint8_t h = g_map_height[cell_xy(x0, y0)];
    uint8_t h1 = g_map_height[cell_xy(x1, y1)];
    if (h1 > h) h = h1;
    g_pos_scratch.x = (int32_t)((uint32_t)x0 << 8);      // WCoord wraps (the original's u16 store)
    g_pos_scratch.y = (int32_t)((uint32_t)y0 << 8);
    g_pos_scratch.z = (int16_t)(h << 5);
}

// terrain_rect_height_range_34820
int terrain_rect_height_range(int x, int y, int dy, int dx) {
    unsigned hi = g_map_height[cell_xy(x, y)], lo = hi;
    auto probe = [&](unsigned v) { if (v > hi) hi = v; else if (v < lo) lo = v; };
    probe(g_map_height[cell_xy(x + dx, y)]);
    probe(g_map_height[cell_xy(x + dx, y + dy)]);
    probe(g_map_height[cell_xy(x, y + dy)]);
    return (int)(hi - lo);
}

// terrain_height_avg_34b40
int terrain_height_avg(int x, int y, int dy, int dx) {
    unsigned sum = g_map_height[cell_xy(x, y)];
    sum += g_map_height[cell_xy(x + dx, y)];
    sum += g_map_height[cell_xy(x + dx, y + dy)];
    sum += g_map_height[cell_xy(x, y + dy)];
    return (int)(sum >> 2);
}

// terrain_rect_min_height_34bb0 (the two counters are the original's 16-bit loop counts)
int terrain_rect_min_height(int x_in, int y_in, int dy, int dx) {
    int x = wrap(x_in), y = wrap(y_in);
    uint16_t lo = 0xfa;
    auto probe = [&](int px, int py) { uint16_t v = g_map_height[cell_xy(px, py)]; if (v < lo) lo = v; };
    for (uint16_t n = (uint16_t)dx; n != 0; n--) {
        probe(x, y);
        probe(x, y + dy);
        x = wrap(x + 1);
    }
    for (uint16_t n = (uint16_t)dy; n != 0; n--) {
        probe(x, y);
        probe(x - dx, y);
        y = wrap(y + 1);
    }
    return lo;
}

// The texture map read at a linear (not byte-wrapped) offset, as terrain_smooth_cell_34a40 does for
// the three quad neighbours: below cell 0x101 the original reads the end of the tables.dat image
// that sits in front of the texture map. Round 11: the same linear quirk on any world (row stride N,
// below cell N + 1 the tables image; at the x = 0 column the "west" neighbour is the previous row's last
// cell), so only the world's first row and column see it.
static uint8_t map_type_linear(int idx) {
    if (idx >= 0) return g_map_type[cell_wrap((unsigned)idx)];
    return g_tables_image[MC_TABLES_SIZE + idx];
}
static inline bool is_castle_texture(uint8_t tex) { return tex > 5 && tex <= 0x22; }

// terrain_smooth_cell_34a40
void terrain_smooth_cell(unsigned cell_in) {
    const int cell = (int)cell_wrap(cell_in);
    const int stride = g_world.size;                 // 0x100 in the original
    if ((g_map_flags[cell] & 7) == 0) return;
    if (g_map_height[cell] == 0) return;
    if (is_castle_texture(map_type_linear(cell - stride - 1))) return;
    if (is_castle_texture(map_type_linear(cell - stride))) return;
    if (is_castle_texture(map_type_linear(cell - 1))) return;
    if (is_castle_texture(g_map_type[cell])) return;
    uint32_t sum = 0, count = 0;
    uint32_t p = (uint32_t)(cell - stride - 1);
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            unsigned q = cell_wrap(p);               // the original's 16-bit index: wraps over the map ends
            if (!is_castle_texture(g_map_type[q])) {
                count++;
                sum += g_map_height[q];
            }
            p++;
        }
        p += (uint32_t)(stride - 3);
    }
    if (count == 0) return;
    g_map_height[cell] = (uint8_t)(sum / count);
}

// terrain_smooth_rect_34a00
void terrain_smooth_rect(int x_in, int y_in, int rows, int cols) {
    int x = wrap(x_in), y = wrap(y_in);
    for (uint16_t r = (uint16_t)rows; r != 0; r--) {
        for (uint16_t c = (uint16_t)cols; c != 0; c--) {
            terrain_smooth_cell(cell_xy(x, y));
            x = wrap(x + 1);
        }
        x = wrap(x_in);
        y = wrap(y + 1);
    }
}

// terrain_smooth_castle_border_348b0: two strips of size + 1 cells on the left / right of the
// footprint, then two above / below it (corners included), each cell smoothed in place.
void terrain_smooth_castle_border(int cx, int cy, int half_h, int half_w, int size_in) {
    uint8_t size = (uint8_t)size_in;
    int x0 = wrap(cx - half_w), y0 = wrap(cy - half_h);
    int x1 = wrap(x0 + (uint8_t)half_w * 2);
    int y1 = wrap(y0 + (uint8_t)half_h * 2);
    {
        int ly = y0, ry = y0;
        for (uint16_t n = (uint16_t)(half_h * 2); n != 0; n--) {
            int lx = wrap(x0 - size), rx = x1;
            for (unsigned k = 0; k <= size; k++) {
                terrain_smooth_cell(cell_xy(lx, ly));
                terrain_smooth_cell(cell_xy(rx, ry));
                rx = wrap(rx + 1);
                lx = wrap(lx + 1);
            }
            ry = wrap(ry + 1);
            ly = wrap(ly + 1);
        }
    }
    {
        int tx = wrap(x0 - size), bx = wrap(x0 - size);
        for (uint16_t n = (uint16_t)(half_w * 2 + size * 2); n != 0; n--) {
            int ty = wrap(y0 - size), by = y1;
            for (unsigned k = 0; k <= size; k++) {
                terrain_smooth_cell(cell_xy(tx, ty));
                terrain_smooth_cell(cell_xy(bx, by));
                by = wrap(by + 1);
                ty = wrap(ty + 1);
            }
            bx = wrap(bx + 1);
            tx = wrap(tx + 1);
        }
    }
}

// ---- wizard castles / castle footprints ----------------------------------------------------------

// thing_set_castle_extents_353f0
void thing_set_castle_extents(Thing *t, int size) {
    const CastleFootprint *fp = castle_footprint((unsigned)(int16_t)size);
    uint32_t h = fp->h, w = fp->w;
    if (g_video_mode_flags == 1) { h >>= 1; w >>= 1; }
    t->ext_x = (int16_t)(((w << 8) + 0x500) >> 1);
    t->ext_z0 = -0x2000;        // 0xe000
    t->ext_h = 0x4000;
    t->ext_y = (int16_t)(((h << 8) + 0x500) >> 1);
}

// effect_wizard_init_35090(thing, castle size)
void effect_wizard_init(Thing *t, int size_in) {
    uint16_t size = (uint16_t)size_in;
    const CastleFootprint *fp = castle_footprint(size);
    uint32_t h = fp->h, w = fp->w;
    t->aux = 2;
    t->speed_base = (int16_t)((int32_t)(h * w) >> 4);
    if (g_video_mode_flags == 1) { h >>= 1; w >>= 1; }
    // snap to the cell corner
    g_pos_scratch = *thing_pos(t);
    g_pos_scratch.x = t->x & ~0xff;          // the original's ((int16_t)x >> 8) << 8 as a u16
    g_pos_scratch.y = t->y & ~0xff;
    thing_move_to(t, &g_pos_scratch);
    int bx = world_cx(t->x);
    int by = world_cx(t->y);
    bx = wrap(bx - (int)(w >> 1));
    by = wrap(by - (int)(h >> 1));
    if (((by + bx) & 1) != 0) {
        // the footprint corner must be on an even (x + y) cell: move one cell in +x
        g_pos_scratch = *thing_pos(t);
        g_pos_scratch.x = g_pos_scratch.x + 0x100;
        thing_move_to(t, &g_pos_scratch);
        bx = wrap(bx + 1);
    }
    thing_set_castle_extents(t, size);
    int avg = terrain_height_avg(bx, by, (int)h, (int)w);
    t->health = 0x1e;
    t->damage = 0x7d0;
    t->prop_flags |= 2;
    t->z = (int16_t)(avg << 5);
    t->castle_size = (uint8_t)size;
}

// ---- castle site tests (0x11820..0x11be0) ----------------------------------------------------------

namespace {
// |torus difference| of two coordinates (the original's abs of the int16 difference at N = 256)
inline int iabsw(int32_t v) { v = world_signed(v); return v < 0 ? -v : v; }
template <typename F> Thing *first_castle(F match) {
    int guard = 0;
    for (uint32_t i = g_cfg->player_list; i != 0 && i < (uint32_t)thing_pool_slots() && guard < thing_pool_slots(); guard++) {
        Thing *p = thing_at(i);
        if (p->type == 2 && match(p)) return p;
        i = p->next;
    }
    return nullptr;
}
// One strip of castle_footprint_clear: `rows` x `cols` cells from (x0, y0); both counters are 16-bit
// do-while loops in the original (a negative count walks 64K cells), x restarts at `x_restart`.
bool strip_built(int x0, int y0, uint16_t rows, uint16_t cols, int x_restart) {
    if (rows == 0) return false;
    int x = wrap(x0), y = wrap(y0);
    do {
        if (cols != 0) {
            uint16_t n = cols;
            do {
                if (g_map_flags[cell_xy(x, y)] & 0x80) return true;
                x = wrap(x + 1);
            } while (--n != 0);
        }
        x = wrap(x_restart);
        y = wrap(y + 1);
    } while (--rows != 0);
    return false;
}
}  // namespace

// castle_near_thing_11820 (dead code in retail)
int castle_near_thing(const Thing *t) {
    return first_castle([&](Thing *c) {
        return iabsw(c->x - t->x) <= c->ext_x + t->ext_x + 0x300
            && iabsw(c->y - t->y) <= c->ext_y + t->ext_y + 0x300;
    }) != nullptr;
}

// castle_crush_wizards_118c0
void castle_crush_wizards(Thing *castle) {
    thing_set_castle_extents(castle, (int16_t)castle->aux + 1);
    int ex = castle->ext_x, ey = castle->ext_y;
    int guard = 0;
    for (uint32_t i = g_cfg->wizard_list; i != 0 && i < (uint32_t)thing_pool_slots() && guard < thing_pool_slots(); guard++) {
        Thing *w = thing_at(i);
        if (iabsw(w->x - castle->x) <= ex + 0x100 + w->ext_x && iabsw(w->y - castle->y) <= w->ext_y + ey + 0x100)
            w->health = -1;
        i = w->next;
    }
    thing_set_castle_extents(castle, (int16_t)castle->aux);
}

// castle_footprint_clear_11980: can the castle grow to level aux + 1? No other castle may touch the
// grown box and no cell of the four border strips may be built on (flags bit 7).
int castle_footprint_clear(Thing *castle) {
    int16_t old_ex = (int16_t)(castle->ext_x >> 8), old_ey = (int16_t)(castle->ext_y >> 8);
    thing_set_castle_extents(castle, (int16_t)castle->aux + 1);
    if (first_castle([&](Thing *c) { return c != castle && thing_collide(c, castle); })) {
        thing_set_castle_extents(castle, (int16_t)castle->aux);
        return 0;
    }
    int16_t new_ex = (int16_t)(castle->ext_x >> 8), new_ey = (int16_t)(castle->ext_y >> 8);
    const int x0 = wrap(world_cx(castle->x + 0x80) - new_ex);
    const int y0 = wrap(world_cx(castle->y + 0x80) - new_ey);
    uint16_t dx = (uint16_t)(new_ex - old_ex), dy = (uint16_t)(new_ey - old_ey);
    // (dx / dy are the original's u16 counts; as cell offsets they are the same values mod N)
    bool built =
           strip_built(x0, y0, dy, (uint16_t)(new_ex * 2), x0)
        || strip_built(x0, new_ey * 2 + y0 - dy, dy, (uint16_t)(new_ex * 2), x0)
        || strip_built(x0, y0 + dy, dy, dx, x0)
        // the last strip starts at the right edge but restarts every row at x0, as the original does
        || strip_built(x0 + new_ex - dx, y0 + dy, dy, dx, x0);
    thing_set_castle_extents(castle, (int16_t)castle->aux);
    return built ? 0 : 1;
}

// castle_site_clear_at_pos_11be0: no castle within 0x800 (+ its extent) on both axes and no built-on
// cell in the 8x8 block that starts 8 cells up-left of pos.
int castle_site_clear_at_pos(const Pos *pos) {
    if (first_castle([&](Thing *c) {
            return iabsw(c->x - pos->x) <= c->ext_x + 0x800 && iabsw(c->y - pos->y) <= c->ext_y + 0x800;
        }))
        return 0;
    const int x0 = world_cx(pos->x) - 8, y0 = world_cx(pos->y) - 8;
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++)
            if (g_map_flags[cell_xy(x0 + col, y0 + row)] & 0x80) return 0;
    return 1;
}

// castle_stamp_footprint_26320
void castle_stamp_footprint(Thing *t) {
    const int cx = thing_cell_x(t), cy = thing_cell_y(t);
    uint8_t base = (uint8_t)(t->z >> 5);
    const CastleFootprint *fp = castle_footprint(t->castle_size);
    uint32_t h = fp->h, w = fp->w;
    if (g_video_mode_flags == 1) { h >>= 1; w >>= 1; }
    const int x0 = wrap(cx - (int)(w >> 1)), y0 = wrap(cy - (int)(h >> 1));
    footprint_walk(fp, h, x0, y0, [&](unsigned cell, uint8_t b, uint32_t) {
        int off;
        if (!footprint_height(b, &off)) return;
        uint8_t f = g_map_flags[cell];
        g_map_height[cell] = (uint8_t)(base + off);
        if (f & 7) return;
        g_map_flags[cell] = (uint8_t)((f & 0xf8) | 1);
        terrain_retexture_rect(cell, cell);
    });
    footprint_walk(fp, h, x0, y0, [&](unsigned cell, uint8_t b, uint32_t) {
        int kind = footprint_paint_kind(b);
        if (kind >= 0) terrain_paint_cell(cell, (unsigned)kind, footprint_paint_dl(b));
    });
}

namespace {

// effect_castle_build_update_26680 (class 10 states 0x30 and 0x33): the footprint heights approach
// their targets over `health` ticks, the textures are painted every fifth tick; on the last tick the
// whole footprint is retextured and a wizard castle (state 0x33) becomes a living castle (0x34).
void effect_castle_build_update(Thing *t) {
    const int cx = thing_cell_x(t), cy = thing_cell_y(t);
    int32_t base = t->z >> 5;
    const CastleFootprint *fp = castle_footprint(t->castle_size);
    uint32_t h = fp->h, w = fp->w;
    if (g_video_mode_flags == 1) { h >>= 1; w >>= 1; }
    uint32_t half_w = w >> 1, half_h = h >> 1;
    t->health--;
    if (t->health != 0) {
        const int x0 = wrap(cx - (int)half_w), y0 = wrap(cy - (int)half_h);
        footprint_walk(fp, h, x0, y0, [&](unsigned cell, uint8_t b, uint32_t) {
            int off;
            if (!footprint_height(b, &off)) return;
            int32_t cur = g_map_height[cell];
            int32_t target = (int16_t)base + off;
            g_map_height[cell] = (uint8_t)((target - cur) / t->health + cur);
            uint8_t f = g_map_flags[cell];
            if (f & 7) return;
            g_map_flags[cell] = (uint8_t)((f & 0xf0) | 1);
            terrain_retexture_rect(cell, cell);
        });
        if (t->health % 5 != 0 && t->health != 1) return;
        footprint_walk(fp, h, x0, y0, [&](unsigned cell, uint8_t b, uint32_t) {
            int kind = footprint_paint_kind(b);
            if (kind >= 0) terrain_paint_cell(cell, (unsigned)kind, footprint_paint_dl(b));
        });
        return;
    }
    terrain_retexture_rect(cell_xy(cx - (int)half_w, cy - (int)half_h),
                           cell_xy(cx + (int)half_w, cy + (int)half_h));
    if (t->state == 0x30) {
        thing_at(thing_wrap(t->caster))->cast_ticks = 2;
        thing_mark_delete(t);
    } else if (t->state == 0x33) {
        t->health = t->damage;
        t->flags |= 1;
        t->state = 0x34;
        t->z = (int16_t)terrain_height_at(thing_pos(t));
        terrain_smooth_castle_border(cx, cy, (int)half_h, (int)(half_w & 0xffff), 2);
        terrain_smooth_castle_border(cx, cy, (int)half_h, (int)(half_w & 0xffff), 5);
    }
}

// port (mode runs; always true otherwise): no more archers than two a building. Nothing thins them out in a match
// (they only fight wanted wizards and skeletons) while every hit on a building sends one out and the buildings'
// people grow back: 30 buildings had 313 archers after 20 minutes.
// The same for the villagers (builders, townies, traders): four a building - see villager_room.
static bool town_room(bool archer) {
    int people = 0, buildings = 0;
    for (int type = archer ? 4 : 0xc; type <= (archer ? 4 : 0xe); type++)
        for (uint32_t i = g_cfg->creature_lists[type]; i != 0 && i < (uint32_t)thing_pool_slots(); i = thing_at(i)->next) people++;
    for (uint32_t i = g_cfg->wizard_list; i != 0 && i < (uint32_t)thing_pool_slots(); i = thing_at(i)->next) buildings++;
    return people < (archer ? 2 : 4) * buildings;
}
static bool archer_room() { return !mode_active() || town_room(true); }

// creature_spawn_random_villager_27660(thing, pos): 2/12 Archer, 2/12 Trader, 5/12 Townie, 3/12
// Builder, each forced into its "villager" state. The constructors belong to the creature port.
Thing *creature_spawn_random_villager(Thing *t, const Pos *pos) {
    static const uint8_t type_of[12]  = { 4, 4, 0xe, 0xe, 0xd, 0xd, 0xd, 0xd, 0xd, 0xc, 0xc, 0xc };
    static const uint8_t state_of[12] = { 0x19, 0x19, 0x55, 0x55, 0x4f, 0x4f, 0x4f, 0x4f, 0x4f, 0x49, 0x49, 0x49 };
    uint32_t r = thing_rng_next(t) % 12u;
    if (mode_active()) {        // port: no room for this kind -> an archer / a townie instead
        const bool archers = town_room(true);
        if (type_of[r] == 4 ? !archers : !town_room(false)) r = type_of[r] != 4 && archers ? 0 : 4;
    }
    Thing *c = thing_create(pos, 5, type_of[r]);
    if (c) c->state = state_of[r];
    return c;
}

// thing_apply_pending_damage_27f90: 0 = nothing pending, 1 = hit (last_attacker set), 2 = dead.
int thing_apply_pending_damage(Thing *t) {
    t->last_attacker = 0;
    if (t->health < 0) return 2;
    DamageSlot &s = t->damage_slots[0];
    if (s.attacker == 0) return 0;
    t->health -= s.amount;
    if (t->health < 0) {
        t->killer = s.attacker;
        return 2;
    }
    t->last_attacker = s.attacker;
    s.amount = 0;
    s.attacker = 0;
    return 1;
}

// effect_ridge_node_s52_update_27710 (class 10 state 0x34): the living wizard castle. The table name
// is a leftover, the handler has nothing to do with ridges.
//
// port (round 7, port_pool.md): a full town sends out a villager now and then, and nothing else limits
// them - the original's 1000 slots do. With an extended pool (PortSettings::thing_cap_villagers) a town
// sends nobody while 999 slots or more are in use, exactly when the original's pool would have refused the
// villager; without it the towns of level 34 grow to 8500 Things in 30000 ticks. The original pool is
// never affected (thing_pool_slots() == 1000: no test).
//
// port (mode runs): four villagers (builders, townies, traders) a building instead. A generated world holds
// thousands of Things, so the 999-slot test above never let a town send anybody out: the people a world starts with
// walked into the buildings and the towns stood empty (user, 2026-10-11).
static bool villager_room() {
    if (mode_active()) return town_room(false) || town_room(true);
    if (thing_pool_slots() == MC_THING_SLOTS || !g_settings.thing_cap_villagers) return true;
    return thing_pool_slots() - 1 - thing_free_count() < MC_THING_SLOTS - 1;
}

void effect_wizard_castle_update(Thing *t) {
    int r = thing_apply_pending_damage(t);
    if (r == 2) { t->state = 0x35; return; }
    if (r != 0 && r != 1) return;
    if (r == 1 && t->aux > 2) {
        // a hit costs one inhabitant, who comes out as an archer
        t->aux--;
        g_pos_scratch = *thing_pos(t);
        g_pos_scratch.x = g_pos_scratch.x + t->ext_x;     // the original's u16 sum (same value mod 65536)
        Thing *c = archer_room() ? thing_create(&g_pos_scratch, 5, 4) : nullptr;    // port: archer_room (mode runs)
        if (c) c->state = 0x19;
        Thing *a = thing_at(thing_wrap(t->last_attacker));
        if (a->type == 0 || a->type == 1) wr16(thing_player_block(a) + 0x210, 0xc8);
    }
    uint16_t claimer = t->damage_slots[1].attacker;
    if (claimer != 0) {
        if (!same_side(claimer, t->mana_owner)) {     // round 12: an ally's village stays his
            t->mana_owner = claimer;
            sound_request(claimer, -1, 4);
            t->flags &= ~1u;
            thing_set_sprite(t, 0xb1);
            t->sprite = (uint16_t)(t->sprite + rd16(thing_player_block(thing_at(thing_wrap(t->damage_slots[1].attacker))) + 0x30));
        }
        t->damage_slots[1].attacker = 0;
        t->damage_slots[1].amount = 0;
    }
    if (t->tick % 0x28 == 0) {
        // port (mode runs): a building's people grow by one every 16 s up to its capacity. The original's only
        // grow by villagers walking in, its levels start with full towns; a generated world's start with two people
        // a building, so none ever filled up and sent out the builders that make a town spread (user, 2026-10-10).
        if (mode_active() && t->aux < t->speed_base && t->tick % (0x28 * 10) == 0) t->aux++;
        // port (mode runs): a quarter of the original's 256 mana a head. A generated world's buildings are full (the
        // town castle held 13312), and a wizard who claimed the towns had 45 % of all mana - most of a mana win and
        // every big spell - without collecting any (user decision, 2026-10-11: count town mana at a fraction).
        t->mana = (int32_t)t->aux << (mode_active() ? 6 : 8);
        int16_t cap = t->speed_base;
        if (cap > 5 && cap == t->aux) {
            uint32_t roll = thing_rng_next(t) % (uint32_t)(int32_t)cap;
            int32_t limit = cap - (cap >> 4) - 2;
            if ((int32_t)roll > limit && villager_room()) {
                g_pos_scratch = *thing_pos(t);
                g_pos_scratch.x = g_pos_scratch.x + t->ext_x;
                creature_spawn_random_villager(t, &g_pos_scratch);
            }
        }
    }
    if (g_hook_player_note_ridge_distance) g_hook_player_note_ridge_distance(t);
}

// effect_type51_s53_update_27930 (class 10 state 0x35): a destroyed wizard castle. The inhabitants
// leave, the footprint is un-built (flag 0x80 cleared, walls collapse) and smoothed.
void effect_wizard_castle_collapse_update(Thing *t) {
    const int cx = thing_cell_x(t), cy = thing_cell_y(t);
    const CastleFootprint *fp = castle_footprint(t->castle_size);
    uint32_t h = fp->h, w = fp->w;
    if (g_video_mode_flags == 1) { h >>= 1; w >>= 1; }
    const int x0 = wrap(cx - (int)(w >> 1)), y0 = wrap(cy - (int)(h >> 1));
    int32_t base;
    if (t->type != 0) base = terrain_height_avg(x0, y0, (int)h, (int)w);
    else              base = t->z >> 5;
    int32_t z_hi = base << 5, z_lo = (base - 10) << 5;
    footprint_walk(fp, h, x0, y0, [&](unsigned cell, uint8_t b, uint32_t pos) {
        if (b == 0) return;
        g_pos_scratch.x = world_cell_x(cell) << 8;
        g_pos_scratch.y = world_cell_y(cell) << 8;
        g_pos_scratch.z = (int16_t)((pos & 7) == 0 ? z_lo : z_hi);
        if (t->aux > 0) {
            t->aux--;
            if (t->aux == 0) {
                Thing *c = thing_create(&g_pos_scratch, 5, 0xc);
                if (c) c->state = 0x49;
            } else if (t->aux < 4 && archer_room()) {           // port: archer_room (mode runs; else a random villager)
                Thing *c = thing_create(&g_pos_scratch, 5, 4);
                if (c) c->state = 0x19;
            } else {
                creature_spawn_random_villager(t, &g_pos_scratch);
            }
        }
        unsigned nib = b >> 4;
        if (nib == 0) {
            g_map_flags[cell] &= 0x7f;
        } else if (nib == 3) {
            g_map_flags[cell] &= 0x7f;
            unsigned m = (b % 16u) % 3u;
            if (m == 1 && g_map_height[cell] > 0xc) g_map_height[cell] -= 0xc;
            if ((m == 1 || m == 2) && g_map_height[cell] > 0x10) g_map_height[cell] -= 0x10;
            terrain_retexture_rect_force(cell, cell);
        } else {
            g_map_flags[cell] = (uint8_t)((g_map_flags[cell] & 0x70) | 1);
            terrain_retexture_rect_force(cell, cell);
            unsigned low = b % 16u;
            if (low != 0) {
                low = (low - 1) & 0xff;
                int32_t drop = (int32_t)low * 4;
                int32_t cur = g_map_height[cell];
                if (cur > drop) {
                    if (thing_rng_next(t) % 0x32u > 0x14) {
                        uint32_t r20 = thing_rng_next(t) % 0x14u;
                        g_map_height[cell] = (uint8_t)(g_map_height[cell] - (drop - (int32_t)r20));
                    } else {
                        g_map_height[cell] = (uint8_t)(g_map_height[cell] - (uint8_t)(low << 2));
                    }
                } else {
                    g_map_height[cell] = 0;
                }
            }
        }
    });
    terrain_smooth_rect(x0, y0, (int)(h & 0xffff), (int)(w & 0xffff));
    thing_mark_delete(t);
}

// ---- terrain-shaping effect handlers --------------------------------------------------------------

// effect_update_shared_23d30 (states 7, 8, 0x1e, 0x1f, 0x21, 0x36): delete.
void effect_delete_update(Thing *t) { thing_mark_delete(t); }

// effect_volcano_update_23dc0 (state 9)
void effect_volcano_update(Thing *t) {
    t->aux++;
    int32_t life = t->health;
    t->health = life - 1;
    if (life >= 0) {
        int r = (int16_t)(thing_rng_next(t) % 9u);
        if (!terrain_find_cell_spiral(t, 0, (int16_t)(t->aux / 6), r, 0)) {
            thing_area_damage_quake(t, 0, t->damage);    // thing_area_damage_11450
            sound_request(thing_index(t), -1, 10);
            return;
        }
    }
    terrain_find_cell_spiral(t, 0, 0, -0x28, 0);
    g_pos_scratch = *thing_pos(t);
    g_pos_scratch.z = (int16_t)terrain_height_at(&g_pos_scratch);
    Thing *c = thing_create(&g_pos_scratch, 10, 0x12);       // constructor 0x39050 (not in this file)
    if (c) c->owner = t->owner;
    thing_mark_delete(t);
}

// effect_type10_s10_update_23ec0 (state 0xa): one random dent that spares built-on cells.
void effect_dent_update(Thing *t) {
    if (!(terrain_cell_flag_bit(thing_pos(t)) & 1)) {
        int r = (int16_t)(thing_rng_next(t) % 7u);
        terrain_find_cell_spiral(t, 0, (int16_t)(t->ext_x / 256), (int16_t)-r, 1);
    }
    thing_mark_delete(t);
}

// effect_crater_s11_update_23fc0 (state 0xb)
void effect_crater_update(Thing *t) {
    if (t->tick % 3 == 0) t->aux++;
    int32_t life = t->health;
    t->health = life - 1;
    if (life < 0 || (terrain_cell_flag_bit(thing_pos(t)) & 1)) {
        thing_mark_delete(t);
        return;
    }
    unsigned dmg = (t->flags & 2) ? t->damage / 0x19u : t->damage;
    thing_area_damage_quake(t, 0, dmg);                      // thing_area_damage_11450
    int rim = t->ext_x / 256 - 1;
    int16_t ring = t->aux;
    if (t->aux > rim) {
        ring = (int16_t)rim;
        if (!(t->flags & 2)) terrain_modify_cells_spiral(t, (int16_t)(rim + 1), (int16_t)(rim + 1));
    }
    t->flags |= 2;
    terrain_modify_cells_spiral(t, 0, ring);
    sound_request(thing_index(t), -1, 10);
}

// terrain_cell_raise_needed_24e20: 0 when the cell is already part of a raised wall (the x-1
// neighbour has the wall texture and no 4-neighbour stands more than 0x1e above the cell).
int terrain_cell_raise_needed(unsigned cell) {
    cell = cell_wrap(cell);
    const int x = world_cell_x(cell), y = world_cell_y(cell);
    unsigned limit = (g_map_height[cell] + 0x1eu) & 0xffff;
    unsigned west = cell_xy(x - 1, y);
    if (g_map_type[west] != 8) return 1;
    if (g_map_height[west] > limit) return 1;
    if (g_map_height[cell_xy(x + 1, y)] > limit) return 1;
    if (g_map_height[cell_xy(x, y + 1)] > limit) return 1;
    if (g_map_height[cell_xy(x, y - 1)] > limit) return 1;
    return 0;
}

// One wall cell: raise by 0x30 unless it already is wall, then the wall texture on its quad.
inline void wall_raise_cell(unsigned cell) {
    if (g_map_type[cell] != 8 || terrain_cell_raise_needed(cell)) g_map_height[cell] = (uint8_t)(g_map_height[cell] + 0x30);
    terrain_set_quad_texture(cell, 8);
}

// The original's 16-bit linear cell cursor (x overflows into the next row, the last cell into cell 0):
// cell + 1 wrapped over the whole map.
inline unsigned cell_next(unsigned c) { return cell_wrap(c + 1); }

// One row of a north / south wall piece: `width` cells from `cell`, flag 0x80 on both sides. The
// cursor is a linear cell index (x overflows into y, as in the original).
inline void wall_row(int x, int y, uint16_t width) {
    g_map_flags[cell_xy(x - 1, y)] |= 0x80;
    unsigned c = cell_xy(x, y);
    for (uint16_t n = width; n != 0; n--) { wall_raise_cell(c); c = cell_next(c); }
    g_map_flags[c] |= 0x80;
}

// effect_steal_mana_s27_update_24fc0 (state 0x1b; the table name is wrong): wall piece running
// towards -y, `aux` + thickness rows from two cells south of the thing.
void effect_wall_north_update(Thing *t) {
    int x = thing_cell_x(t), y = wrap(thing_cell_y(t) + 2);
    for (uint16_t rows = (uint16_t)(t->aux + (uint16_t)t->health); rows != 0; rows--) {
        wall_row(x, y, (uint16_t)t->health);
        y = wrap(y - 1);
    }
    thing_mark_delete(t);
}

// effect_type26_s28_update_24eb0 (state 0x1c): wall piece running towards +y.
void effect_wall_south_update(Thing *t) {
    int x = thing_cell_x(t), y = thing_cell_y(t);
    if (x & 1) x = wrap(x + 1);
    x = wrap(x - (uint8_t)t->health + 1);     // (the thickness as a byte, as the original subtracts it)
    for (uint16_t rows = (uint16_t)(t->aux + (uint16_t)t->health); rows != 0; rows--) {
        wall_row(x, y, (uint16_t)t->health);
        y = wrap(y + 1);
    }
    thing_mark_delete(t);
}

// effect_type27_s29_update_250b0 (state 0x1d): wall piece running towards +x, `aux` cells long.
void effect_wall_east_update(Thing *t) {
    int x = thing_cell_x(t), y = thing_cell_y(t);
    if ((x + y) & 1) x = wrap(x + 1);
    unsigned c = cell_xy(x, y - 1);
    for (uint16_t n = (uint16_t)t->aux; n != 0; n--) { g_map_flags[c] |= 0x80; c = cell_next(c); }
    for (uint16_t rows = (uint16_t)t->health; rows != 0; rows--) {
        c = cell_xy(x, y);
        for (uint16_t n = (uint16_t)t->aux; n != 0; n--) { wall_raise_cell(c); c = cell_next(c); }
        y = wrap(y + 1);
    }
    c = cell_xy(x, y);
    for (uint16_t n = (uint16_t)t->aux; n != 0; n--) { g_map_flags[c] |= 0x80; c = cell_next(c); }
    thing_mark_delete(t);
}

// effect_type30_s32_update_251e0 (state 0x20, type 0x1e): path piece, `aux` cells stepping by
// (yaw, pitch) get class 1 and are retextured. The steps are the low bytes of yaw / pitch (-1, 0, 1
// from level_build_path), added as signed bytes: the same mod 256 as the original's byte add.
void effect_path_update(Thing *t) {
    int x = thing_cell_x(t), y = thing_cell_y(t);
    const int dx = (int8_t)(uint8_t)t->yaw, dy = (int8_t)(uint8_t)t->pitch;
    for (uint16_t n = (uint16_t)t->aux; n != 0; n--) {
        unsigned c = cell_xy(x, y);
        g_map_flags[c] = (uint8_t)((g_map_flags[c] & 0xf0) | 1);
        terrain_retexture_rect(c, c);
        x = wrap(x + dx);
        y = wrap(y + dy);
    }
    thing_mark_delete(t);
}

// effect_type32_s34_update_25270 (state 0x22, type 0x20): canyon digger, drops a short-lived crater
// every cell along its heading until its life runs out or it reaches water.
void effect_canyon_digger_update(Thing *t) {
    int32_t life = t->health;
    t->health = life - 1;
    if (life < 0 || (terrain_cell_flag_bit(thing_pos(t)) & 1)) {
        thing_mark_delete(t);
        return;
    }
    Thing *c = thing_create(thing_pos(t), 10, 0xb);
    if (c) {
        c->health = 2;
        c->ext_h = t->ext_h;
        c->owner = t->owner;
    }
    math_rotate_offset(thing_pos(t), t->yaw, 0, t->speed_cur);
}

// effect_type53_s55_update_252f0 (state 0x37, type 0x33): ridge raiser, lifts the terrain around it
// by 10..24 every step along its heading.
void effect_ridge_raiser_update(Thing *t) {
    int32_t life = t->health;
    t->health = life - 1;
    if (life < 0 || (terrain_cell_flag_bit(thing_pos(t)) & 1)) {
        thing_mark_delete(t);
        return;
    }
    int r = (int16_t)(thing_rng_next(t) % 0xfu + 0xa);
    if (!terrain_find_cell_spiral(t, 0, 0x400, r, 0)) {
        thing_area_damage(t, 0, t->damage);                  // thing_area_damage_10d20
        sound_request(thing_index(t), -1, 10);
    }
    math_rotate_offset(thing_pos(t), t->yaw, 0, t->speed_cur);
}

// ---- constructors (Table B, class 10) ---------------------------------------------------------------

Thing *effect_alloc(const Pos *pos, int type, int state, int max_health, bool copy_pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->max_health = max_health;
    t->state = (uint8_t)state;
    t->cls = 10;
    t->type = (uint8_t)type;
    if (copy_pos) { t->x = pos->x; t->y = pos->y; t->z = pos->z; }
    return t;
}

// effect_create_volcano_38b70
Thing *effect_create_volcano(const Pos *pos) {
    Thing *t = effect_alloc(pos, 9, 9, 0x11, true);
    if (!t) return nullptr;
    t->damage = 0x7d0;
    t->flags &= ~8u;
    thing_restore_health(t);
    thing_set_extents(t, 0x300, 0x2000);
    return t;
}

// effect_create_type10_38bd0
Thing *effect_create_dent(const Pos *pos) {
    Thing *t = effect_alloc(pos, 0xa, 0xa, 1, true);
    if (!t) return nullptr;
    t->damage = 0x64;
    t->flags = (t->flags & 0xfffdfff7u) | 0x20000u;
    thing_restore_health(t);
    thing_set_extents(t, 0x80, 0x80);
    return t;
}

// effect_create_crater_38c40
Thing *effect_create_crater(const Pos *pos) {
    Thing *t = effect_alloc(pos, 0xb, 0xb, 0x28, true);
    if (!t) return nullptr;
    t->damage = 0xc8;
    t->flags = (t->flags & 0xfffdfff7u) | 0x20000u;
    thing_restore_health(t);
    thing_set_extents(t, 0x900, 0x2000);
    return t;
}

// effect_create_type27_392a0: wall piece (level_build_wall sets the direction state and length).
Thing *effect_create_wall_piece(const Pos *pos) {
    Thing *t = effect_alloc(pos, 0x1b, 0x1b, 2, false);
    if (!t) return nullptr;
    t->damage = (uint16_t)((pos->z >> 5) + 0x30);
    t->aux = 10;
    t->flags &= ~8u;
    thing_link_cell(t, pos);
    thing_restore_health(t);
    return t;
}

// The four level markers (wall 0x39300, path 0x393c0, canyon 0x39470, ridge node 0x39540) only
// differ in type and state; their state handler is the shared delete.
Thing *effect_create_marker(const Pos *pos, int type, int state) {
    Thing *t = effect_alloc(pos, type, state, 0, true);
    if (!t) return nullptr;
    t->flags &= ~8u;
    thing_link_cell(t, pos);
    thing_restore_health(t);
    return t;
}
Thing *effect_create_wall(const Pos *pos)       { return effect_create_marker(pos, 0x1c, 0x1e); }   // effect_create_wall_39300
Thing *effect_create_path(const Pos *pos)       { return effect_create_marker(pos, 0x1d, 0x1f); }   // effect_create_path_393c0
Thing *effect_create_canyon(const Pos *pos)     { return effect_create_marker(pos, 0x1f, 0x21); }   // effect_create_canyon_39470
Thing *effect_create_ridge_node(const Pos *pos) { return effect_create_marker(pos, 0x32, 0x36); }   // effect_create_ridge_node_39540
// effect_create_type30_39360: path piece (same body, state 0x20 = effect_path_update)
Thing *effect_create_path_piece(const Pos *pos) { return effect_create_marker(pos, 0x1e, 0x20); }

// effect_create_type32_39420: canyon digger
Thing *effect_create_canyon_digger(const Pos *pos) {
    Thing *t = effect_alloc(pos, 0x20, 0x22, 0, true);
    if (!t) return nullptr;
    t->flags &= ~8u;
    t->speed_cur = 0x100;
    thing_restore_health(t);
    return t;
}

// effect_create_type51_394d0: ridge raiser
Thing *effect_create_ridge_raiser(const Pos *pos) {
    Thing *t = effect_alloc(pos, 0x33, 0x37, 0, true);
    if (!t) return nullptr;
    t->aux = 0x100;
    t->flags &= ~8u;
    t->speed_cur = 0x400;
    thing_set_extents(t, 0x300, 0x300);
    thing_restore_health(t);
    return t;
}

// effect_create_wizard_39930: wizard castle (health stays 0 until effect_wizard_init).
Thing *effect_create_wizard(const Pos *pos) {
    Thing *t = effect_alloc(pos, 0x2d, 0x33, 0x1e, false);
    if (!t) return nullptr;
    t->damage = 0x64;
    t->aux = 4;
    t->flags = 9;
    t->prop_flags = 0x21;
    thing_link_cell(t, pos);
    thing_set_sprite(t, 0xb1);
    return t;
}

}  // namespace

// ---- feature spawning -----------------------------------------------------------------------------

// level_build_wall_342e0: the wall from (x0, y0) to (x1, y1) as a staircase of at most 10-cell runs,
// each run one east piece (state 0x1d) plus one north / south piece (0x1b / 0x1c).
void level_build_wall(int x0, int y0, int x1, int y1) {
    int32_t x = x0, y = y0, xe = x1, ye = y1;
    int32_t dx = math_wrap_diff((int16_t)x, (int16_t)xe, g_world.size);    // 0x100 in the original
    int32_t dy = math_wrap_diff((int16_t)y, (int16_t)ye, g_world.size);
    if (dx == 0 && dy == 0) return;
    if (dx < 0) {
        dx = -dx;
        dy = -dy;
        int32_t s = x; x = xe; xe = s;
        s = y; y = ye; ye = s;
    }
    // (the original passes the words; a u16 cell number is the same cell mod N for any N <= 4096)
    auto scratch = [&]() { terrain_set_pos_scratch((uint16_t)x, (uint16_t)y, (uint16_t)xe, (uint16_t)ye); };
    auto east_piece = [&](int32_t len) {
        scratch();
        Thing *t = thing_create(&g_pos_scratch, 10, 0x1b);     // (the original does not test for null)
        if (t) { t->aux = (int16_t)len; t->state = 0x1d; }
    };
    auto y_piece = [&](int32_t step, int32_t rem) {
        scratch();
        Thing *t = thing_create(&g_pos_scratch, 10, 0x1b);
        if (!t) return;
        if (step < 0) { t->state = 0x1b; t->aux = (int16_t)(-step - rem); }
        else          { t->state = 0x1c; t->aux = (int16_t)(step + rem); }
    };
    if (std::abs(dy) < dx) {
        int32_t n = dx / 10 + 1;
        int32_t step_x = dx / n, rem_x = dx - step_x * n;
        int32_t step_y = dy / n, rem_y = dy - step_y * n;
        for (; n != 0; n--) {
            int32_t len = step_x + rem_x;
            east_piece(len);
            x += len;
            y_piece(step_y, rem_y);
            y += rem_y + step_y;
            rem_y = 0;
            rem_x = 0;
        }
    } else {
        int32_t n = std::abs(dy / 10) + 1;
        int32_t step_y = dy / n, rem_y = dy - step_y * n;
        int32_t step_x = dx / n, rem_x = dx - step_x * n;
        do {
            y_piece(step_y, rem_y);
            y += rem_y + step_y;
            rem_y = 0;
            int32_t len = step_x + rem_x;
            east_piece(len);
            rem_x = 0;
            x += len;
        } while (--n != 0);
    }
}

// 0x34570: a path as one diagonal piece followed by one straight piece (type 0x1e things).
void level_build_path(int x0, int y0, int x1, int y1) {
    int32_t dx = math_wrap_diff((int16_t)x0, (int16_t)x1, g_world.size);   // 0x100 in the original
    int32_t dy = math_wrap_diff((int16_t)y0, (int16_t)y1, g_world.size);
    int32_t sx = dx == 0 ? 0 : (dx > 0 ? 1 : -1);
    int32_t sy = dy == 0 ? 0 : (dy > 0 ? 1 : -1);
    int32_t adx = std::abs(dx), ady = std::abs(dy);
    int32_t straight = std::abs(ady - adx), diag = adx;
    int32_t straight_dx, straight_dy;
    if (adx > ady) { diag = ady; straight_dx = sx; straight_dy = 0; }
    else           { straight_dx = 0; straight_dy = sy; }
    g_pos_scratch.x = (int32_t)((uint32_t)x0 << 8);      // WCoord wraps (the original's u16 store)
    g_pos_scratch.y = (int32_t)((uint32_t)y0 << 8);
    Thing *t = thing_create(&g_pos_scratch, 10, 0x1e);
    if (t) { t->yaw = (uint16_t)sx; t->pitch = (uint16_t)sy; t->aux = (int16_t)diag; }
    int32_t x = x0 + sx * diag, y = y0 + sy * diag;
    g_pos_scratch.x = (int32_t)((uint32_t)x << 8);
    g_pos_scratch.y = (int32_t)((uint32_t)y << 8);
    t = thing_create(&g_pos_scratch, 10, 0x1e);
    if (t) { t->yaw = (uint16_t)straight_dx; t->aux = (int16_t)straight; t->pitch = (uint16_t)straight_dy; }
}

// The height byte the canyon / ridge builders read: the original's linear index (y << 8) + x on 16-bit
// words (x above 255 carries into the next row). Round 11: the same with row stride N, wrapped over the
// map; == world_cell(x, y) for 0 <= x < N.
static uint8_t height_linear(int x, int y) {
    return g_map_height[cell_wrap(((uint32_t)(y & 0xffff) << g_world.shift) + (uint32_t)(x & 0xffff))];
}

// 0x346b0: a canyon segment = one digger (type 0x20) heading for the far end, one step per cell.
// (dist & 0xffff: the original's 16-bit word; unit_mask is the same at N = 256 and keeps a wide world's
// distances, which never reach the world width.)
void level_build_canyon(int x0, int y0, int x1, int y1) {
    Pos a, b;
    a.x = (int32_t)((uint32_t)x0 << 8);
    a.y = (int32_t)((uint32_t)y0 << 8);
    a.z = (int16_t)(height_linear(x0, y0) << 5);
    b.x = (int32_t)((uint32_t)x1 << 8);
    b.y = (int32_t)((uint32_t)y1 << 8);
    b.z = 0;
    int yaw = pos_angle_to(&a, &b);
    int dist = pos_dist_xy(&a, &b);
    Thing *t = thing_create(&a, 10, 0x20);
    if (t) {
        t->yaw = (uint16_t)yaw;
        t->health = (int32_t)(dist & (int32_t)g_world.unit_mask) >> 8;
    }
}

// 0x34760: a ridge segment = one raiser (type 0x33) heading for the far end, one step per 4 cells.
void level_build_ridge(int x0, int y0, int x1, int y1) {
    Pos a, b;
    a.x = (int32_t)((uint32_t)x0 << 8);
    a.y = (int32_t)((uint32_t)y0 << 8);
    a.z = (int16_t)(height_linear(x0, y0) << 4);
    b.x = (int32_t)((uint32_t)x1 << 8);
    b.y = (int32_t)((uint32_t)y1 << 8);
    b.z = 0;
    int yaw = pos_angle_to(&a, &b);
    int dist = pos_dist_xy(&a, &b);
    Thing *t = thing_create(&a, 10, 0x33);
    if (t) {
        t->yaw = (uint16_t)yaw;
        t->health = t->speed_cur != 0 ? (int32_t)(dist & (int32_t)g_world.unit_mask) / (int32_t)t->speed_cur : 0;
    }
}

// level_build_linked_feature_34c40
void level_build_linked_feature(LevelData *level, ThingInit *rec) {
    uint16_t cls = rec->cls, model = rec->model;
    void (*build)(int, int, int, int) = nullptr;
    if (cls == 10) {
        if (model == 0x1c)      build = level_build_wall;
        else if (model == 0x1d) build = level_build_path;
        else if (model == 0x1f) build = level_build_canyon;
        else if (model == 0x32) build = level_build_ridge;
    }
    if (!build) return;
    const unsigned count = sizeof level->things / sizeof level->things[0];
    // record number n lives at level + 0x430 + n * 0x12 = things[n - 1]
    auto record = [&](unsigned n) -> ThingInit * { return (n >= 1 && n <= count) ? &level->things[n - 1] : nullptr; };
    // walk to the head of the chain (the original loops forever on a cyclic chain: bounded here)
    ThingInit *r = rec;
    for (unsigned guard = 0; r->parent != 0 && guard < count; guard++) {
        ThingInit *p = record(r->parent);
        if (!p) break;
        r = p;
    }
    for (unsigned guard = 0; r && guard <= count; guard++) {
        if (r->cls != cls || r->model != model) break;
        uint16_t child = r->child;
        r->swi_id = 0;
        if (child == 0) break;
        ThingInit *next = record(child);
        if (!next) break;
        build(r->x, r->y, next->x, next->y);
        r = next;
    }
}

// level_spawn_effect_record_34e00 (+ level_spawn_effect_direct_34ed7)
void level_spawn_effect_record(LevelData *level, ThingInit *rec) {
    uint16_t model = rec->model;
    bool linked = model == 0x1c || model == 0x1d || model == 0x1f || model == 0x32;
    if (linked && rec->swi_id != 0) {
        level_build_linked_feature(level, rec);
        return;
    }
    g_pos_scratch.x = (int32_t)((uint32_t)rec->x << 8);   // ThingInit cells are u16: up to 65535 cells
    g_pos_scratch.y = (int32_t)((uint32_t)rec->y << 8);
    g_pos_scratch.z = (int16_t)terrain_height_at(&g_pos_scratch);
    // the Table B record is called directly: no enabled / index check
    ThingCreateFn fn = thing_create_fn(10, rec->model);
    if (!fn) return;
    Thing *t = fn(&g_pos_scratch);
    if (model == 0x2d && t) {
        effect_wizard_init(t, (rec->parent + 0x10) & 0xffff);
        // port (mode runs): a generated world's buildings start full, so their people come out from the first minute
        // (two people in a castle for 52 took 13 minutes to fill up)
        if (mode_active()) t->aux = t->speed_base;
    }
}

// level_flag_terrain_effects_34f40
void level_flag_terrain_effects(LevelData *level) {
    for (ThingInit &r : level->things) {
        if (r.dis_id != 0xffff || r.cls != 10) continue;
        if (r.model == 0x1c || r.model == 0x1d || r.model == 0x1f || r.model == 0x32) r.swi_id = 1;
    }
}

// level_spawn_terrain_effects_34d60
void level_spawn_terrain_effects(LevelData *level) {
    models_initialise();
    for (ThingInit &r : level->things)
        if (r.dis_id == 0xffff && r.cls == 10) level_spawn_effect_record(level, &r);
    level_flag_terrain_effects(level);
    level_run_terrain_effects();
}

// terrain_generate_features_34db0
void terrain_generate_features() {
    LevelData *level = &g_state->level;
    for (ThingInit &r : level->things) {
        if (r.dis_id == 0xffff && r.cls == 10) {
            level_spawn_effect_record(level, &r);
            r.cls = 0;
        }
    }
    level_run_terrain_effects();
}

void level_features_register_handlers() {
    g_hook_effect_wizard_init = effect_wizard_init;
    // Table A
    thing_register_update(0x23d30, effect_delete_update);
    thing_register_update(0x23dc0, effect_volcano_update);
    thing_register_update(0x23ec0, effect_dent_update);
    thing_register_update(0x23fc0, effect_crater_update);
    thing_register_update(0x24fc0, effect_wall_north_update);
    thing_register_update(0x24eb0, effect_wall_south_update);
    thing_register_update(0x250b0, effect_wall_east_update);
    thing_register_update(0x251e0, effect_path_update);
    thing_register_update(0x25270, effect_canyon_digger_update);
    thing_register_update(0x252f0, effect_ridge_raiser_update);
    thing_register_update(0x26680, effect_castle_build_update);
    thing_register_update(0x27710, effect_wizard_castle_update);
    thing_register_update(0x27930, effect_wizard_castle_collapse_update);
    // Table B
    thing_register_create(0x38b70, effect_create_volcano);
    thing_register_create(0x38bd0, effect_create_dent);
    thing_register_create(0x38c40, effect_create_crater);
    thing_register_create(0x392a0, effect_create_wall_piece);
    thing_register_create(0x39300, effect_create_wall);
    thing_register_create(0x393c0, effect_create_path);
    thing_register_create(0x39360, effect_create_path_piece);
    thing_register_create(0x39470, effect_create_canyon);
    thing_register_create(0x39420, effect_create_canyon_digger);
    thing_register_create(0x39930, effect_create_wizard);
    thing_register_create(0x39540, effect_create_ridge_node);
    thing_register_create(0x394d0, effect_create_ridge_raiser);
}
