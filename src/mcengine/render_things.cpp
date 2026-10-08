// Thing (billboard sprite) renderer of carpet.exe: render_cell_things_2c600 (things of one terrain
// cell with their shadows), render_cell_things_mirrored_2e5a0 (reflections of the second-surface
// pass) and the scaled / rolled sprite blitter render_sprite_scaled_2ad60.
//
// Sources: disassembly of 0x2ad60..0x2c4cc (blitter, jump tables 0x2ace8 / 0x2ad10 / 0x2ad30),
// 0x2c600..0x2da23 and 0x2e5a0..0x2f07d. Findings are in docs/analysis/port_sprites.md.
//
// The blitter draws a sprite as `rows` lines that run along the rolled screen x axis. Each line
// walks the roll table of render_build_roll_table (one entry per pixel of the major screen axis:
// byte offset of that pixel on the rolled line through the origin), so a line is a Bresenham line
// and consecutive lines are one minor-axis step apart. Per octant of the roll angle the code only
// differs in how the anchor maps to (first line, first table entry) and in the clipping against the
// screen edges, which uses the list of table entries at which the line steps along the minor axis.
#include "sprites.h"
#include "raster.h"
#include "terrain.h"
#include "mc_math.h"
#include "world.h"
#include "gen/render_tables.h"
#include "gen/sprites_tables.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

SpriteBlit g_sprite_blit{};
uint32_t   g_sprite_oob_reads = 0, g_sprite_oob_writes = 0;
void     (*g_sprite_blit_probe)(unsigned anchor) = nullptr;

// 32-bit wrap-around arithmetic of the original (imul / shl truncate; avoid signed-overflow UB).
static inline int32_t mul32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }
static inline int32_t shl16(int32_t a) { return (int32_t)((uint32_t)a << 16); }

// ---------------------------------------------------------------------------------------------
// Work-buffer tables of the blitter
// ---------------------------------------------------------------------------------------------

struct ClipTriple { int32_t x0, count, skip; };          // g_work_buf + 0xb360
static_assert(sizeof(ClipTriple) == 12);

// Column table at g_work_buf + 0x9060: int32 pairs {delta to the previous source column, source
// column}. Kept as raw dwords because the exe reads the dword in front of the table (the last bytes
// of the vertex grid) and, for sprites wider than the table, the entries behind it.
static inline int32_t *coltab() { return reinterpret_cast<int32_t *>(g_work_buf + MC_SPRITE_COLTAB_OFFSET); }
static inline ClipTriple *triples() { return reinterpret_cast<ClipTriple *>(g_work_buf + MC_SPRITE_TRIPLES_OFFSET); }
constexpr int32_t kColTabLimit = (int32_t)((MC_WORK_SIZE - MC_SPRITE_COLTAB_OFFSET) / 8) - 1;

// Entry `i` of the minor-step list at g_work_buf + 0xe7e0 (1-based major index of the i-th step).
// The clip code can index a few entries below the list when a line lies completely outside; the exe
// then reads the unused tail of the triple table in front of the list, and so does the port.
static inline int32_t roll_list_at(int32_t i) {
    const int64_t at = (int64_t)MC_ROLL_LIST_OFFSET + (int64_t)i * 4;
    if (at < 0 || at + 4 > (int64_t)MC_WORK_SIZE) return 0;
    int32_t v;
    std::memcpy(&v, g_work_buf + at, 4);
    return v;
}

// ---------------------------------------------------------------------------------------------
// Pixel loops. `src_off` walks the source row through the column deltas exactly like the exe's
// "mov al,[ebx] / add ebx,[esi+8k]" chains; `put(texel, dest)` is the pixel mode.
// ---------------------------------------------------------------------------------------------

struct RowCtx {
    const uint8_t *pix;       // sprite pixels
    uint32_t       pix_size;  // readable bytes
    uint8_t       *dest;      // g_rt_dest
    uint32_t       dest_size; // pitch * height: writes outside are refused
};

static inline uint8_t src_texel(const RowCtx &c, int32_t off) {
    if ((uint32_t)off < c.pix_size) return c.pix[off];
    g_sprite_oob_reads++;
    return 0;
}

// Axis-aligned row (upright path): n pixels at dest offset `doff`, column records from `col`.
template <class Put> static inline void row_straight(const RowCtx &c, int32_t src_off, const int32_t *col,
                                                     int32_t doff, int32_t n, Put put) {
    for (int32_t i = 0; i < n; i++) {
        const uint8_t t = src_texel(c, src_off);
        src_off += col[2 * (i + 1)];                         // delta of the next column
        if (t != 0) {
            const uint32_t at = (uint32_t)(doff + i);
            if (at < c.dest_size) put(t, c.dest[at]); else g_sprite_oob_writes++;
        }
    }
}

// Rolled row: n pixels along the roll table starting at entry `roll`.
template <class Put> static inline void row_rolled(const RowCtx &c, int32_t src_off, const int32_t *col,
                                                   int32_t row_off, const RollEntry *roll, int32_t n, Put put) {
    for (int32_t i = 0; i < n; i++) {
        const uint8_t t = src_texel(c, src_off);
        src_off += col[2 * (i + 1)];
        if (t != 0) {
            const uint32_t at = (uint32_t)(row_off + roll[i].offset);
            if (at < c.dest_size) put(t, c.dest[at]); else g_sprite_oob_writes++;
        }
    }
}

// Dispatch on the pixel mode (jump tables 0x2ace8 for the upright path, 0x2ad30 for the rolled
// path). `row(put)` runs one row with the given pixel operation. The two paths differ in modes 4, 5
// (the constant in the blend index is whatever the register held: the mode number in the upright
// path, 0 in the rolled path) and 8 (no-op in the upright path).
template <class Row> static inline void draw_row_mode(int32_t mode, int32_t shade, bool upright, Row row) {
    const uint8_t *const blend = g_blend_table();
    // SHADE[(shade & ~0xff) | c] with the full 32-bit `shade` as the exe indexes 0xb99b0.
    const uint32_t shade_base = (uint32_t)shade & 0xffffff00u;
    const bool shade_ok = shade_base <= MC_TABLES_SIZE - 0x100;
    const uint8_t *const sh = g_tables_image + (shade_ok ? shade_base : 0);
    switch (mode) {
    case 0: row([](uint8_t t, uint8_t &d) { d = t; }); break;
    case 1: if (shade_ok) row([sh](uint8_t t, uint8_t &d) { d = sh[t]; }); break;
    case 2: row([blend](uint8_t t, uint8_t &d) { d = blend[((unsigned)t << 8) | d]; }); break;
    case 3: row([blend](uint8_t t, uint8_t &d) { d = blend[((unsigned)d << 8) | t]; }); break;
    case 4: {
        const unsigned k = upright ? 4u : 0u;
        row([blend, k](uint8_t t, uint8_t &d) { d = blend[(k << 8) | t]; });
        break;
    }
    case 5: {
        const unsigned k = upright ? 5u : 0u;
        row([blend, k](uint8_t t, uint8_t &d) { d = blend[((unsigned)t << 8) | k]; });
        break;
    }
    case 6: if (shade_ok) row([blend, sh](uint8_t t, uint8_t &d) { d = sh[blend[((unsigned)t << 8) | d]]; }); break;
    case 7: if (shade_ok) row([blend, sh](uint8_t t, uint8_t &d) { d = sh[blend[((unsigned)d << 8) | t]]; }); break;
    case 8: if (!upright && shade_ok) row([sh](uint8_t, uint8_t &d) { d = sh[d]; }); break;
    case 9: {
        const uint8_t colour = (uint8_t)((uint32_t)shade >> 16);
        row([colour](uint8_t, uint8_t &d) { d = colour; });
        break;
    }
    default: break;                                          // modes > 9: nothing is drawn
    }
}

// ---------------------------------------------------------------------------------------------
// render_sprite_scaled_2ad60, upright path (DAT_000b58ae != 0): plain scaled blit, no rotation.
// ---------------------------------------------------------------------------------------------

static void sprite_blit_upright(unsigned anchor, const RowCtx &ctx) {
    SpriteBlit &b = g_sprite_blit;
    // The sprite's box is centred half its size "above" the anchor along the rolled up direction
    // (anchor 1) or below it (anchor 2); anchor 0 leaves (x, y) as the top-left corner.
    const int32_t q = (b.dst_w + b.dst_h) >> 2;
    if (anchor >= 1) {
        const int32_t dy = mul32(g_rcam.cos_roll, q) >> 16;
        const int32_t dx = mul32(g_rcam.sin_roll, q) >> 16;
        if (anchor == 1)      { b.x += -dx - q; b.y += -dy - q; }
        else if (anchor == 2) { b.x += dx - q;  b.y += dy - q; }
    }

    const int32_t rt_w = (uint16_t)g_rt_width;                  // word at 0x9b600
    if (rt_w <= b.x) return;
    const int32_t xstep = shl16(b.src_w) / b.dst_w;
    if (-b.x > 0) {
        const int32_t cut = -b.x;
        b.dst_w -= cut;
        if (b.dst_w <= 0) return;
        b.src_x = mul32(cut, xstep);
        b.x = 0;
        if (rt_w <= b.dst_w) b.dst_w = rt_w;
    } else {
        b.src_x = 0;
        const int32_t over = b.x + b.dst_w - rt_w;
        if (over > 0) b.dst_w -= over;
    }

    const int32_t rt_h = (uint16_t)g_rt_height;                 // word at 0x9b604
    if (rt_h <= b.y) return;
    const int32_t ystep = shl16(b.src_h) / b.dst_h;
    if (-b.y > 0) {
        const int32_t cut = -b.y;
        b.dst_h -= cut;
        if (b.dst_h <= 0) return;
        b.src_y = mul32(ystep, cut);
        b.y = 0;
        if (rt_h <= b.dst_h) b.dst_h = rt_h;
    } else {
        b.src_y = 0;
        const int32_t over = b.y + b.dst_h - rt_h;
        if (over > 0) b.dst_h -= over;
    }

    // Column table: entry i = {source column i - source column i-1, source column i}; the delta of
    // entry 0 is the constant 0x16 (never used).
    int32_t *const col = coltab();
    for (int32_t i = 0; i < b.dst_w; i++) {
        col[2 * i + 1] = b.src_x >> 16;
        col[2 * i] = i == 0 ? 0x16 : col[2 * i + 1] - col[2 * i - 1];
        b.src_x += xstep;
    }

    int32_t doff = b.x + mul32(b.y, g_rt_pitch);
    if (b.dst_h == 0) return;
    do {
        const int32_t src_off = mul32(b.src_y >> 16, b.src_stride) + col[1];
        const int32_t n = b.dst_w;
        draw_row_mode(b.mode, b.shade, true, [&](auto put) { row_straight(ctx, src_off, col, doff, n, put); });
        b.src_y += ystep;
        doff += g_rt_pitch;
    } while (--b.dst_h != 0);
}

// ---------------------------------------------------------------------------------------------
// render_sprite_scaled_2ad60
// ---------------------------------------------------------------------------------------------

// render_sprite_scaled_2ad60
void render_sprite_scaled(unsigned anchor) {
    SpriteBlit &b = g_sprite_blit;
    if (g_sprite_blit_probe) g_sprite_blit_probe(anchor);
    RowCtx ctx;
    ctx.pix = b.pixels;
    ctx.pix_size = b.pixels_size;
    ctx.dest = g_rt_dest;
    ctx.dest_size = (uint32_t)(g_rt_pitch * (g_rt_height - 1) + g_rt_width);
    if (!ctx.pix || !ctx.dest || g_rt_height <= 0) return;

    if (b.upright) {
        sprite_blit_upright(anchor, ctx);
        return;
    }

    // Anchor -> top-left corner of the rolled rectangle. "Right" on screen is (cos, -sin) of the
    // camera roll and "down" is (sin, cos).
    const int32_t sin_roll = g_rcam.sin_roll, cos_roll = g_rcam.cos_roll;
    if (anchor == 1) {                                          // (x, y) = bottom centre
        const int32_t ax = (mul32(b.dst_h, sin_roll) + (mul32(cos_roll, b.dst_w) >> 1)) >> 16;
        const int32_t ay = (mul32(b.dst_h, cos_roll) - (mul32(b.dst_w, sin_roll) >> 1)) >> 16;
        b.x -= ax;
        b.y -= ay;
    } else if (anchor == 0 || anchor == 2) {                    // (x, y) = top centre
        b.x -= mul32(b.dst_w, cos_roll) >> 17;
        b.y -= (-mul32(b.dst_w, sin_roll)) >> 17;
    }

    const int32_t octant = g_roll.octant;                       // DAT_000b587c
    if ((uint32_t)octant > 7) return;
    const bool odd = (octant & 1) != 0;
    const int32_t pitch = g_rt_pitch;
    const int32_t step = g_roll.step;                           // DAT_000b5880 minor / major slope
    const int32_t neg_steps = g_roll.neg_steps;                 // DAT_000b5888
    const int32_t ext_minor = g_roll.extent_minor;              // DAT_000b5898
    const int32_t ext_major = g_roll.extent_major;              // DAT_000b589c
    // Even octants: the lines run mostly along the major axis with cos_a, odd octants with sin_a
    // (g_roll.sin_a / cos_a are the sine / cosine relative to the octant pair's base axis).
    const int32_t major_c = odd ? g_roll.sin_a : g_roll.cos_a;  // DAT_000b58a4 / DAT_000b5868
    const int32_t minor_c = odd ? g_roll.cos_a : g_roll.sin_a;

    const int32_t cols = mul32(b.dst_w, major_c) >> 16;         // pixels per line (edi)
    if (cols <= 0) return;
    int32_t rows = shl16(b.dst_h) / major_c;                    // lines (esi)
    if (rows <= 0) return;
    if (octant == 7 && b.x >= ext_major) return;
    int32_t ystep = shl16(b.src_h) / rows;                      // [ebp-0x30]
    if (anchor != 1) {                                          // shadows and reflections are upside down
        b.src_y = mul32(ystep, rows - 1);
        ystep = -ystep;
    } else {
        b.src_y = 0;
    }
    const int32_t shift = mul32(b.dst_h, minor_c) / rows;       // 16.16 start shift per line ([ebp-0x38] / [ebp-0x3c])

    ClipTriple *const tri = triples();
    int32_t acc;        // 16.16 position of the line start along the major axis ([ebp-0x18] / ecx)
    int32_t line0;      // index of the first line along the minor axis (edx / [ebp-0x34])
    int32_t row_off;    // byte offset of that line's origin ([ebp-8])
    int32_t min_skip = 0x98967f;                                // [ebp-0x10]

    if (!odd) {
        switch (octant) {
        case 0:  acc = shl16(b.x); line0 = b.y - (mul32(b.x, step) >> 16); break;
        case 2:  acc = shl16(b.y); line0 = ext_minor - b.x - (mul32(b.y, step) >> 16); break;
        case 4:  { const int32_t t = ext_major - b.x; acc = shl16(t); line0 = ext_minor - b.y - (mul32(step, t) >> 16); break; }
        default: { const int32_t t = ext_major - b.y; acc = shl16(t); line0 = b.x - (mul32(t, step) >> 16); break; }   // 6
        }
        if (line0 < neg_steps) {                                // lines before the first visible one
            const int32_t cut = neg_steps - line0;
            rows -= cut;
            if (rows <= 0) return;
            b.src_y += mul32(ystep, cut);
            acc -= mul32(cut, shift);
            line0 = neg_steps;
        }
        switch (octant) {
        case 0:  row_off = mul32(pitch, line0); break;
        case 2:  row_off = ext_minor - 1 - line0; break;
        case 4:  row_off = mul32(ext_minor - line0 - 1, pitch) + ext_major - 1; break;
        default: row_off = mul32(ext_major - 1, pitch) + line0; break;
        }

        // 0x2b50e: last line
        const int32_t lim = ext_minor - neg_steps;
        if (line0 > 0) {
            if (lim < rows + line0) {
                rows = lim - line0;
                if (rows <= 0) return;
            }
        } else if (rows > lim) {
            rows = lim;
        }
        if (rows > MC_SPRITE_TRIPLE_MAX) rows = MC_SPRITE_TRIPLE_MAX;   // port: table capacity (unreachable up to 640x480)

        // One triple per line: first table entry, pixel count, source columns skipped on the left.
        {
            ClipTriple *tp = tri;
            for (int32_t left = rows; left != 0; left--, tp++) {
                const int32_t xs = acc >> 16;
                int32_t sk;
                bool lower = true;
                if (xs >= 0) {
                    tp->skip = 0; tp->x0 = xs; sk = 0; tp->count = cols;
                } else {
                    sk = -xs;
                    tp->x0 = 0;
                    tp->count = cols - sk;
                    if (tp->count <= 0) { rows -= left; break; }    // this and all further lines start left of the table
                    tp->skip = sk;
                    lower = sk < min_skip;
                }
                if (lower) min_skip = sk;
                if (tp->x0 + tp->count > ext_major) tp->count = ext_major - tp->x0;
                acc -= shift;
            }
        }

        // 0x2b5ba: lines beyond clip_a leave the screen at the far minor edge before the table ends.
        const int32_t clip_a = g_roll.clip_a;                   // DAT_000b5858
        if (rows + line0 > clip_a) {
            const int32_t before = clip_a - line0;
            const int32_t beyond = rows + line0 - clip_a;
            ClipTriple *p;
            int32_t left, li;
            if (before > 0) { p = tri + before; left = beyond; li = g_roll.list_last; }
            else            { p = tri; left = beyond + before; li = g_roll.list_last + before; }
            for (; left != 0; left--, p++, li--) {
                const int32_t end = roll_list_at(li);
                if (p->count + p->x0 > end) {
                    if (p->x0 >= end) { rows -= left; break; }
                    p->count = end - p->x0;
                }
            }
        }
        // 0x2b66a: lines with a negative index enter the screen at the near minor edge.
        if (line0 < 0) {
            ClipTriple *p = tri;
            int32_t li = -1 - line0;
            for (int32_t r = line0; r != 0; r++, p++, li--) {
                const int32_t first = roll_list_at(li);
                if (first > p->x0) {
                    if (first > p->x0 + p->count) {
                        p->count = 0;
                    } else {
                        const int32_t d = first - p->x0;
                        p->x0 += d; p->skip += d; p->count -= d;
                    }
                }
            }
        }
    } else {
        switch (octant) {
        case 1:  acc = shl16(b.y); line0 = b.x - (mul32(b.y, step) >> 16); break;
        case 3:  { const int32_t t = ext_major - b.x; acc = shl16(t); line0 = b.y - (mul32(t, step) >> 16); break; }
        case 5:  { const int32_t t = ext_major - b.y; acc = shl16(t); line0 = ext_minor - b.x - (mul32(t, step) >> 16); break; }
        default: acc = shl16(b.x); line0 = ext_minor - b.y - (mul32(step, b.x) >> 16); break;   // 7
        }
        if (line0 >= ext_minor) {                               // lines past the far minor edge
            const int32_t cut = line0 - ext_minor;
            rows -= cut;
            if (rows <= 0) return;
            b.src_y += mul32(ystep, cut);
            acc += mul32(cut, shift);
            line0 = ext_minor;
        } else if (line0 < neg_steps) {
            return;
        }
        switch (octant) {
        case 1:  row_off = line0; break;
        case 3:  row_off = mul32(line0, pitch) + ext_major - 1; break;
        case 5:  row_off = ext_minor - 1 + mul32(pitch, ext_major - 1) - line0; break;
        default: row_off = mul32(ext_minor - 1 - line0, pitch); break;
        }

        // 0x2bce1
        const int32_t lim = ext_minor - neg_steps;
        if (line0 >= ext_minor) {
            if (lim < line0 + rows - ext_minor) {
                rows = ext_minor + (lim - line0);
                if (rows <= 0) return;
            }
        } else if (rows > lim) {
            rows = lim;
        }
        if (rows > MC_SPRITE_TRIPLE_MAX) rows = MC_SPRITE_TRIPLE_MAX;   // port: table capacity

        {
            ClipTriple *tp = tri;
            for (int32_t left = rows; left != 0; left--, tp++) {
                const int32_t xs = acc >> 16;
                int32_t sk;
                bool lower = true;
                if (xs >= 0) {
                    tp->skip = 0; tp->x0 = xs; sk = 0; tp->count = cols;
                } else {
                    sk = -xs;
                    tp->x0 = 0;
                    tp->count = cols - sk;
                    tp->skip = sk;
                    lower = sk < min_skip;
                }
                if (lower) min_skip = sk;
                if (tp->x0 + tp->count > ext_major) tp->count = ext_major - tp->x0;
                acc += shift;
            }
        }

        // 0x2bd8c: the first lines leave the screen at the far minor edge.
        {
            const int32_t e = g_roll.steps + line0 - ext_minor + 1;
            if (e > 0) {
                int32_t li = g_roll.list_last;
                for (int32_t pi = e - 1, k = e + 1; k != 0 && pi >= 0; k--, pi--, li--) {
                    if (pi >= MC_SPRITE_TRIPLE_MAX) continue;       // port: stay inside the table
                    ClipTriple *p = tri + pi;
                    const int32_t over = p->x0 + p->count - roll_list_at(li);
                    if (over > 0) {
                        p->count -= over;
                        if (p->count < 0) p->count = 0;
                    }
                }
            }
        }
        // 0x2be00: lines whose index runs below zero enter the screen at the near minor edge.
        {
            int32_t under = line0 - rows;
            if (under < 0) {
                int32_t n = -under;
                under -= neg_steps;
                if (under < 0) {
                    rows += under;
                    if (rows <= 0) return;
                    n += under;
                }
                if (n > 0) {
                    int32_t pi = line0, li = 0;
                    for (int32_t left = n - 1; left != 0; left--, li++) {
                        pi++;
                        if (pi < 0 || pi >= MC_SPRITE_TRIPLE_MAX) continue;
                        ClipTriple *p = tri + pi;
                        const int32_t d = roll_list_at(li) - p->x0;
                        if (d > 0) {
                            p->x0 += d;
                            p->count -= d;
                            if (p->count < 0) p->count = 0;
                            p->skip += d;
                        }
                    }
                }
            }
        }
    }

    // 0x2b6d5: column table from the smallest skip on. A mirrored sprite (negative source width)
    // starts at the last column and steps backwards.
    const int32_t xstep = shl16(b.src_w) / cols;
    b.src_x = 0;
    if (b.src_w < 0) b.src_x = -mul32(cols - 1, xstep);
    int32_t ncol = cols - min_skip;
    if (ncol <= 0) return;
    if (ncol > g_roll.extent_sum) ncol = g_roll.extent_sum;     // DAT_000b5874
    if (ncol > kColTabLimit) ncol = kColTabLimit;               // port: stay inside g_work_buf
    b.src_x += mul32(min_skip, xstep);
    int32_t *const col = coltab();
    for (int32_t i = 0; i <= ncol; i++) {                       // ncol + 1 entries
        const int32_t v = b.src_x >> 16;
        const int32_t prev = col[2 * i - 1];                    // entry -1 = the dword in front of the table
        col[2 * i + 1] = v;
        col[2 * i] = v - prev;
        b.src_x += xstep;
    }

    // 0x2b77c: the lines.
    if (rows == 0) return;
    const RollEntry *const roll = g_roll_entries();
    const ClipTriple *tp = tri;
    for (int32_t left = rows;; tp++) {
        const int32_t n = tp->count;
        if (n > 0) {
            const int32_t ci = tp->skip - min_skip;             // first column record of this line
            const int32_t ri = tp->x0;                          // first roll-table entry
            // Port: refuse lines whose records lie outside the tables (the exe would read stray memory).
            if (ci >= 0 && ci + n <= kColTabLimit && ri >= 0 && ri + n <= MC_ROLL_MAX) {
                const int32_t *c = col + 2 * ci;
                const int32_t src_off = mul32(b.src_y >> 16, b.src_stride) + c[1];
                const int32_t ro = row_off;
                draw_row_mode(b.mode, b.shade, false,
                              [&](auto put) { row_rolled(ctx, src_off, c, ro, roll + ri, n, put); });
            } else {
                g_sprite_oob_writes++;
            }
        }
        b.src_y += ystep;
        row_off += g_roll.minor_step;                           // DAT_000b5890
        if (--left == 0) break;
    }
}

// ---------------------------------------------------------------------------------------------
// Sprite selection shared by the three per-thing code blocks (shadow, thing, reflection): draw
// type -> sprite number, cache / LRU bookkeeping, on-screen size and pixel mode.
// ---------------------------------------------------------------------------------------------

// Distance fog -> DAT_000b5818 (light level << 8).
static inline int32_t thing_fog(int32_t d2) {
    if (d2 <= g_rcam.fog_near2) return 0x2000;
    if (d2 >= g_rcam.fog_far2) return 0;
    return (int32_t)((uint32_t)(((int32_t)((uint32_t)(g_rcam.fog_far2 - d2) << 5)) / g_rcam.fog_div) << 8);
}

// Returns false when the thing must be skipped (sprite not loadable). `shadow` selects the variant
// of the first switch of 2c600, which does not mark draw type 0x15 as upright.
static bool thing_select_sprite(const Thing *t, const SpriteDesc *d, int32_t z, bool shadow) {
    SpriteBlit &b = g_sprite_blit;
    b.upright = 0;
    const unsigned base = d->base_sprite;
    // Facing relative to the camera in 16 steps: ((yaw - cam_yaw) >> 3 & 0xf0) >> 4.
    const unsigned dir = (unsigned)((((int32_t)t->yaw - (int32_t)g_rcam.yaw16) >> 3) & 0xf0) >> 4;
    unsigned n;
    bool mirror = false, upright = false;
    const unsigned dt = d->draw_type;
    if (dt > 0x24) return false;            // the exe would reuse the previous thing's sprite pointer; no such type in the data
    switch (dt) {
    case 0: case 1:
        n = base;
        break;
    case 0x15:
        n = base;
        upright = !shadow;
        break;
    case 0x11:                              // 8 views, the other half mirrored
        if (dir < 8) n = base + dir;
        else { n = base + 0xf - dir; mirror = true; }
        break;
    case 0x12:                              // 16 views
        n = base + dir;
        break;
    case 0x13:                              // 5 views
        n = base + g_sprite_dir_remap_13[dir];
        mirror = dir >= 8;
        break;
    case 0x14:                              // 3 views
        n = base + g_sprite_dir_remap_14[dir];
        mirror = dir >= 8;
        break;
    default:                                // 2..0x10 animation frame; 0x16..0x24 the same, drawn upright
        if (dt >= 0x16) b.upright = 1;
        n = base + t->frame;
        break;
    }
    if (n >= MC_SPRITE_SLOTS) return false; // port: the exe indexes DAT_000b8d3c unchecked; + port sprites (world_art.h)
    if (!g_sprite_ptr[n] && !sprite_ensure_loaded(n)) return false;
    g_sprite_group_stamp[sprite_group_of(n)] = g_cfg->tick;      // DAT_000b7cb0[group] = cfg+4
    if (upright) b.upright = 1;
    uint8_t *s = g_sprite_ptr[n];
    const int32_t w = (int32_t)((unsigned)s[2] | ((unsigned)s[3] << 8));
    const int32_t h = (int32_t)((unsigned)s[4] | ((unsigned)s[5] << 8));
    if (h == 0) return false;               // port: the exe would divide by zero
    b.src_stride = w;
    b.src_h = h;
    b.dst_h = (int32_t)(((int64_t)(uint16_t)d->half_z * (int64_t)g_rcam.focal) / (int64_t)z);
    b.dst_w = mul32(w, b.dst_h) / h;
    b.src_w = mirror ? -w : w;
    b.pixels = s + 6;
    const size_t bytes = sprite_loaded_bytes(n);
    b.pixels_size = bytes > 6 ? (uint32_t)(bytes - 6) : 0;
    if ((uint32_t)(w * h) < b.pixels_size) b.pixels_size = (uint32_t)(w * h);   // the image only, not the animation data
    s[0] |= 8;                              // "drawn" flag, cleared by texture_anim_update
    const unsigned sg = d->shade_group < 6 ? d->shade_group : 0;
    b.mode = b.shade == 0x2000 ? g_sprite_mode_lit[sg] : g_sprite_mode_fogged[sg];
    return true;
}

// Projects a camera-space point to the rolled screen: DAT_000b5828 / DAT_000b5824.
static inline void thing_project(int32_t xc, int32_t h_rel, int32_t z) {
    SpriteBlit &b = g_sprite_blit;
    const int32_t sx = mul32(g_rcam.focal, xc) / z;
    const int32_t sy = g_rcam.horizon + mul32(g_rcam.focal, h_rel) / z;
    b.x = g_rcam.screen_cx + ((mul32(g_rcam.cos_roll, sx) - mul32(g_rcam.sin_roll, sy)) >> 16);
    b.y = g_rcam.screen_cy - ((mul32(sx, g_rcam.sin_roll) + mul32(sy, g_rcam.cos_roll)) >> 16);
}

// ---------------------------------------------------------------------------------------------
// render_cell_things_2c600
// ---------------------------------------------------------------------------------------------

// Port (round 10, task C): the faithful anchors of the frame being drawn inside a capture (render.h
// projection export). Reads g_sprite_blit / the Thing only.
static std::vector<RenderAnchor> *s_anchor_stage = nullptr;
static void anchor_stage_push(const Thing *t, int32_t zc) {
    const SpriteBlit &b = g_sprite_blit;
    RenderAnchor a;
    a.slot = thing_index(t);
    a.generation = thing_slot_generation(a.slot);
    a.sx = b.x; a.sy = b.y; a.w = b.dst_w; a.h = b.dst_h; a.depth = zc;
    a.x = t->x; a.y = t->y; a.z = t->z;
    s_anchor_stage->push_back(a);
}

static void render_thing(const Thing *t, const VertexRec *cell) {
    SpriteBlit &b = g_sprite_blit;
    b.thing = t;
    // round 11: the 16-bit difference against the camera's low 16 bits is the exact difference in any world
    // size (the Thing is on a cell of the 20-cell grid, far inside +-128 cells)
    const int32_t dx = (int16_t)(uint16_t)(t->x - g_rcam.cam_x16);
    const int32_t dy = (int16_t)(uint16_t)(g_rcam.cam_y16 - t->y);
    // Camera space: x to the right, z forward.
    const int32_t xc = (mul32(g_rcam.cos_yaw, dx) - mul32(dy, g_rcam.sin_yaw)) >> 16;
    const int32_t zc = (mul32(dy, g_rcam.cos_yaw) + mul32(dx, g_rcam.sin_yaw)) >> 16;
    const int32_t d2 = mul32(xc, xc) + mul32(zc, zc);
    const bool visible = zc > 0x40 && d2 < g_rcam.cull_dist2;
    const SpriteDesc *d = mc_sprite_desc((unsigned)(int32_t)(int16_t)t->sprite);

    // Shadow: the same sprite, a quarter as high, upside down below the ground point, darkening
    // the terrain. Not on textures flagged in DAT_00093930 and not for shade groups != 0.
    if (g_rcam.shadows != 0 && cell->tex_prop == 0) {
        const int32_t ground = terrain_sample_height(t->x, t->y);   // terrain_height_at_71e00
        if (visible) {
            b.shade = thing_fog(d2);
            if (d->shade_group == 0) {
                thing_project(xc, ground - g_rcam.cam_z, zc);
                if (!thing_select_sprite(t, d, zc, true)) return;
                b.dst_h >>= 2;
                if (b.dst_w > 0 && b.dst_h > 0) {
                    b.shade = (b.shade >> 2) + 0x2000;              // level 0x20 (identity) .. 0x28 (darker)
                    b.mode = 8;
                    render_sprite_scaled(0);
                }
            }
        }
    }

    if (!visible) return;
    b.shade = thing_fog(d2);
    thing_project(xc, (int32_t)t->z - g_rcam.cam_z, zc);
    if (!thing_select_sprite(t, d, zc, false)) return;
    b.dst_w++;
    b.dst_h++;
    if (s_anchor_stage) anchor_stage_push(t, zc);      // port (round 10): render-only, a capture is open
    render_sprite_scaled(1);
}

// render_cell_things_2c600
void render_cell_things(int first_thing, const VertexRec *cell) {
    unsigned idx = (unsigned)first_thing & 0xffff;
    // Port: a cycle in the cell list (the original's quick load can leave one) must not hang the
    // renderer - no list is longer than the pool, so stop after thing_pool_slots() links.
    int budget = thing_pool_slots();
    do {
        if (idx >= (unsigned)thing_pool_slots()) break;             // port: the exe indexes the pool unchecked
        const Thing *t = thing_at(idx);
        if ((t->flags & 0x21) == 0) render_thing(t, cell);          // ret_stub_1fa80(thing) was a debug hook
        idx = t->cell_next;
    } while (idx != 0 && --budget > 0);
}

// ---------------------------------------------------------------------------------------------
// render_cell_things_mirrored_2e5a0: the reflection in the z = 0 plane, no shadows.
// ---------------------------------------------------------------------------------------------

// render_cell_things_mirrored_2e5a0
void render_cell_things_mirrored(int first_thing, const VertexRec *) {
    SpriteBlit &b = g_sprite_blit;
    unsigned idx = (unsigned)first_thing & 0xffff;
    int budget = thing_pool_slots();                                // port: bound the walk (cycles), as above
    do {
        // The exe skips an index >= 1000 but then reads the next link through an uninitialised
        // (first thing) or stale pointer; the port ends the list.
        if (idx >= (unsigned)thing_pool_slots()) break;
        const Thing *t = thing_at(idx);
        if ((t->flags & 0x21) == 0) {
            b.thing = t;
            const int32_t dx = (int16_t)(uint16_t)(t->x - g_rcam.cam_x16);
            const int32_t dy = (int16_t)(uint16_t)(g_rcam.cam_y16 - t->y);
            const int32_t xc = (mul32(g_rcam.cos_yaw, dx) - mul32(g_rcam.sin_yaw, dy)) >> 16;
            const int32_t zc = (mul32(g_rcam.cos_yaw, dy) + mul32(dx, g_rcam.sin_yaw)) >> 16;
            const int32_t d2 = mul32(xc, xc) + mul32(zc, zc);
            if (zc > 0x40 && d2 < g_rcam.cull_dist2) {
                b.shade = thing_fog(d2);
                const SpriteDesc *d = mc_sprite_desc((unsigned)(int32_t)(int16_t)t->sprite);
                thing_project(xc, -(int32_t)t->z - g_rcam.cam_z, zc);
                if (thing_select_sprite(t, d, zc, false)) {
                    b.dst_w++;
                    b.dst_h++;
                    render_sprite_scaled(2);
                }
            }
        }
        idx = t->cell_next;
    } while (idx != 0 && --budget > 0);
}

// ---------------------------------------------------------------------------------------------

static void hook_sprite_mark_needed(const ThingInit *rec) {
    sprite_mark_needed_for_model(rec->cls, rec->model, -1);         // switch_activate_356e0 pushes (class, model, -1)
}

void render_things_install() {
    g_render_cell_things = render_cell_things;
    g_render_cell_things_mirrored = render_cell_things_mirrored;
    g_hook_sprite_group_priorities_clear = sprite_group_priorities_clear;
    g_hook_sprite_mark_needed_for_model = hook_sprite_mark_needed;
    g_hook_sprite_groups_reload_by_priority = sprite_groups_reload_by_priority;
}

// ---------------------------------------------------------------------------------------------
// Port (Phase 4 round 10, task C): projection export (render.h). Render-only: reads the camera state the
// renderers left behind (g_rcam / the extended renderer's record) and the maps; writes no game state.
// docs/analysis/port_inspect.md.
// ---------------------------------------------------------------------------------------------

namespace {

RenderViewInfo s_view;
std::vector<RenderAnchor> s_anchors;        // the last drawn view's anchors
std::vector<RenderAnchor> s_stage;          // the faithful renderer's while it draws inside a capture
bool s_capture = false, s_want_anchors = false;
RenderViewFn s_cap_prev = nullptr;
const uint8_t *s_ext_target = nullptr;      // where the extended renderer drew last
uint32_t s_ext_count = 0;                   // ++ per extended view (render_view_info_ext_drawn)

// The projection of a view as doubles (both renderers; the faithful one projects with integers below).
struct ProjF { double focal, horizon, scx, scy, cr, sr, cy, sy; int cam_x, cam_y, cam_z; double radius; };
ProjF proj_f(const RenderViewInfo &v) {
    ProjF p{};
    if (v.kind == RENDER_VIEW_FAITHFUL) {
        const RenderCamState &r = v.rcam;
        p.focal = r.focal; p.horizon = r.horizon; p.scx = r.screen_cx; p.scy = r.screen_cy;
        p.cr = r.cos_roll / 65536.0; p.sr = r.sin_roll / 65536.0; p.cy = r.cos_yaw / 65536.0; p.sy = r.sin_yaw / 65536.0;
        p.cam_x = v.cam_x; p.cam_y = v.cam_y; p.cam_z = r.cam_z;     // the full world position (capture_view)
        p.radius = std::sqrt((double)(uint32_t)r.cull_dist2);
    } else {
        p.focal = v.focal; p.horizon = v.horizon; p.scx = v.scx; p.scy = v.scy;
        p.cr = v.cr; p.sr = v.sr; p.cy = v.cyaw; p.sy = v.syaw;
        p.cam_x = v.cam_x; p.cam_y = v.cam_y; p.cam_z = v.cam_z;
        p.radius = std::sqrt(v.cull2);
    }
    if (p.radius < 256.0) p.radius = 20 * 256.0;
    return p;
}

// The wrapper render_capture_begin installs as g_render_view_override.
void capture_view(const FrameBuffer &fb, const Camera &cam) {
    const RenderViewFn prev = s_cap_prev;
    const uint32_t ext0 = s_ext_count;
    // render_landscape_29050 writes g_rcam.fog_far2 (= kFogFar2) in every frame before anything reads it:
    // a zero here afterwards means the faithful renderer did not run (the extended one did, or nothing - the
    // compositor's second pass); the value is put back then.
    const int32_t saved_far = g_rcam.fog_far2;
    g_rcam.fog_far2 = 0;
    s_stage.clear();
    s_anchor_stage = s_want_anchors ? &s_stage : nullptr;
    if (prev) prev(fb, cam); else render_view(fb, cam);
    s_anchor_stage = nullptr;
    RenderViewInfo &v = s_view;
    if (g_rcam.fog_far2 != 0) {
        v.kind = RENDER_VIEW_FAITHFUL;
        v.serial++;
        v.view_w = g_rt_width; v.view_h = g_rt_height;
        v.rcam = g_rcam;
        // g_rcam keeps the original's low 16 bits; the full position is the camera passed in plus the slope
        // nudge render_view added (|nudge| <= 100 units)
        v.cam_x = world_wrap(cam.cam_x + (int16_t)(uint16_t)(g_rcam.cam_x16 - cam.cam_x));
        v.cam_y = world_wrap(cam.cam_y + (int16_t)(uint16_t)(g_rcam.cam_y16 - cam.cam_y));
        v.cam_z = g_rcam.cam_z; v.yaw = g_rcam.yaw16;
        v.cull2 = (double)(uint32_t)g_rcam.cull_dist2;
        v.composed = false;
        v.frame_known = false;
        const intptr_t off = fb.pixels ? g_rt_dest - fb.pixels : -1;
        if (fb.pixels && g_rt_pitch == fb.width && off >= 0 && off < (intptr_t)fb.width * fb.height) {
            v.frame_known = true;
            v.frame_x0 = (double)(off % fb.width); v.frame_y0 = (double)(off / fb.width);
            v.frame_sx = v.frame_sy = 1.0;
            v.frame_w = fb.width; v.frame_h = fb.height;
        }
        s_anchors.swap(s_stage);
        s_stage.clear();
    } else {
        g_rcam.fog_far2 = saved_far;
        if (s_ext_count != ext0 && !v.frame_known && s_ext_target && fb.pixels) {
            const intptr_t off = s_ext_target - fb.pixels;     // render_view_ext_target: the window inside fb
            if (off >= 0 && off < (intptr_t)fb.width * fb.height) {
                v.frame_known = true;
                v.frame_x0 = (double)(off % fb.width); v.frame_y0 = (double)(off / fb.width);
                v.frame_sx = v.frame_sy = 1.0;
                v.frame_w = fb.width; v.frame_h = fb.height;
            }
        }
    }
}

} // namespace

const RenderViewInfo &render_view_info() { return s_view; }
RenderViewInfo *render_view_info_edit() { return &s_view; }

void render_view_info_ext_drawn(const uint8_t *target) {
    s_view.kind = RENDER_VIEW_EXT;
    s_view.serial++;
    s_view.frame_known = false;
    s_view.composed = false;
    s_ext_target = target;
    s_ext_count++;
}

void render_view_info_set_frame_mapping(double x0, double y0, double sx, double sy, int frame_w, int frame_h, bool composed) {
    s_view.frame_known = true;
    s_view.composed = composed;
    s_view.frame_x0 = x0; s_view.frame_y0 = y0; s_view.frame_sx = sx; s_view.frame_sy = sy;
    s_view.frame_w = frame_w; s_view.frame_h = frame_h;
}

bool render_anchors_wanted() { return s_capture && s_want_anchors; }
void render_anchors_clear() { s_anchors.clear(); }
void render_anchor_push(const RenderAnchor &a) { s_anchors.push_back(a); }
const RenderAnchor *render_anchors(int *count) {
    *count = (int)s_anchors.size();
    return s_anchors.data();
}

void render_capture_begin(const FrameBuffer &, bool anchors) {
    if (s_capture) render_capture_end();
    s_capture = true;
    s_want_anchors = anchors;
    s_cap_prev = g_render_view_override;
    g_render_view_override = capture_view;
}

void render_capture_end() {
    if (!s_capture) return;
    if (g_render_view_override == capture_view) g_render_view_override = s_cap_prev;
    s_cap_prev = nullptr;
    s_capture = false;
    s_want_anchors = false;
    s_anchor_stage = nullptr;
}

bool render_capture_active() { return s_capture; }

bool render_project_world(int32_t x, int32_t y, int32_t z, int *sx, int *sy, int *depth) {
    const RenderViewInfo &v = s_view;
    if (v.kind == RENDER_VIEW_FAITHFUL) {
        // render_thing + thing_project exactly (integer, the original's rounding)
        const RenderCamState &r = v.rcam;
        // the nearest copy on the torus (exactly the original's 16-bit difference with 256 cells); farther
        // than 128 cells - only possible in a wider world - is far outside the 20-cell view and would
        // overflow the integer projection
        const int32_t dx = world_signed(x - v.cam_x);
        const int32_t dy = world_signed(v.cam_y - y);
        if (dx < -0x8000 || dx > 0x7fff || dy < -0x8000 || dy > 0x7fff) {
            if (depth) *depth = 0;
            return false;
        }
        const int32_t xc = (mul32(r.cos_yaw, dx) - mul32(dy, r.sin_yaw)) >> 16;
        const int32_t zc = (mul32(dy, r.cos_yaw) + mul32(dx, r.sin_yaw)) >> 16;
        if (depth) *depth = zc;
        if (zc <= 0x40) return false;
        const int32_t px = mul32(r.focal, xc) / zc;
        const int32_t py = r.horizon + mul32(r.focal, z - r.cam_z) / zc;
        *sx = r.screen_cx + ((mul32(r.cos_roll, px) - mul32(r.sin_roll, py)) >> 16);
        *sy = r.screen_cy - ((mul32(px, r.sin_roll) + mul32(py, r.cos_roll)) >> 16);
        return true;
    }
    if (v.kind == RENDER_VIEW_EXT) {
        // render_ext.cpp emit_cell_things + project()
        const double dx = world_signed(x - v.cam_x);
        const double dy = world_signed(v.cam_y - y);
        const double xc = v.cyaw * dx - v.syaw * dy, zc = v.cyaw * dy + v.syaw * dx;
        if (depth) *depth = (int)std::lround(zc);
        if (!(zc > 0x40)) return false;
        const double X = xc * v.focal / zc, Y = (double)(z - v.cam_z) * v.focal / zc + v.horizon;
        const double px = v.scx + (v.cr * X - v.sr * Y), py = v.scy - (Y * v.cr + X * v.sr);
        *sx = (int)std::floor(std::clamp(px, -1.0e8, 1.0e8) + 0.5);
        *sy = (int)std::floor(std::clamp(py, -1.0e8, 1.0e8) + 0.5);
        return true;
    }
    return false;
}

bool render_view_to_frame(double vx, double vy, double *fx, double *fy) {
    const RenderViewInfo &v = s_view;
    if (v.kind == RENDER_VIEW_NONE || !v.frame_known) return false;
    *fx = v.frame_x0 + vx * v.frame_sx;
    *fy = v.frame_y0 + vy * v.frame_sy;
    return true;
}

bool render_frame_to_view(double fx, double fy, double *vx, double *vy) {
    const RenderViewInfo &v = s_view;
    if (v.kind == RENDER_VIEW_NONE || !v.frame_known || v.frame_sx == 0 || v.frame_sy == 0) return false;
    *vx = (fx - v.frame_x0) / v.frame_sx;
    *vy = (fy - v.frame_y0) / v.frame_sy;
    return true;
}

void render_frame_to_hud640(double fx, double fy, int *hx, int *hy) {
    const RenderViewInfo &v = s_view;
    const int fw = v.frame_w > 0 ? v.frame_w : 640, fh = v.frame_h > 0 ? v.frame_h : 480;
    const int vh = fw <= 320 ? 400 : 480;
    *hx = (int)std::floor(fx * 640.0 / fw);
    *hy = (int)std::floor(fy * vh / fh);
}

bool render_project_world_hud640(int32_t x, int32_t y, int32_t z, int *hx, int *hy, int *depth) {
    int sx, sy;
    if (!render_project_world(x, y, z, &sx, &sy, depth)) return false;
    double fx, fy;
    if (!render_view_to_frame(sx + 0.5, sy + 0.5, &fx, &fy)) return false;
    render_frame_to_hud640(fx, fy, hx, hy);
    return true;
}

bool render_pick_ground(int sx, int sy, int *cell_x, int *cell_y, int32_t *wx, int32_t *wy, int32_t *wz) {
    const RenderViewInfo &v = s_view;
    if (v.kind == RENDER_VIEW_NONE) return false;
    const ProjF p = proj_f(v);
    if (p.focal <= 0) return false;
    // Un-roll the pixel (the inverse of the projection's screen rotation), then the ray per unit of forward
    // (camera z) distance: camera x = kx, height = kh; world dx = cos*xc + sin*zc, dy = cos*zc - sin*xc
    // with world y = cam_y - dy (the renderers' dy = cam_y - y).
    const double a = (sx + 0.5) - p.scx, b = p.scy - (sy + 0.5);
    const double X = p.cr * a + p.sr * b, Y = -p.sr * a + p.cr * b;
    const double kx = X / p.focal, kh = (Y - p.horizon) / p.focal;
    const double ux = p.cy * kx + p.sy, uy = -(p.cy - p.sy * kx);
    double gx = 0, gy = 0;
    auto above = [&](double t) {
        gx = p.cam_x + ux * t; gy = p.cam_y + uy * t;
        const double h = p.cam_z + kh * t;
        const int ground = terrain_sample_height(world_wrap((int32_t)std::floor(gx)), world_wrap((int32_t)std::floor(gy)));
        return h > ground;
    };
    const double t_max = p.radius * 1.5;
    double t0 = 8.0, t1 = t0;
    if (!above(t0)) return false;                              // the camera is below the terrain
    bool hit = false;
    while (t1 < t_max) {
        const double tn = std::min(t_max, t1 + 8.0 + t1 * (1.0 / 64.0));
        if (!above(tn)) { t0 = t1; t1 = tn; hit = true; break; }
        t1 = tn;
    }
    if (!hit) return false;
    for (int i = 0; i < 24; i++) {                             // bisection: t0 above, t1 at / below the ground
        const double tm = 0.5 * (t0 + t1);
        if (above(tm)) t0 = tm; else t1 = tm;
    }
    above(t1);
    const int32_t ix = (int32_t)std::floor(gx), iy = (int32_t)std::floor(gy);
    *cell_x = world_cx(ix);
    *cell_y = world_cx(iy);
    if (wx) *wx = world_wrap(ix);
    if (wy) *wy = world_wrap(iy);
    if (wz) *wz = terrain_sample_height(world_wrap(ix), world_wrap(iy));
    return true;
}

int render_pick_thing(int sx, int sy, int max_dist) {
    int best_in = -1, best_near = -1;
    int32_t in_depth = 0;
    double near_d = (double)max_dist + 0.5;
    for (const RenderAnchor &a : s_anchors) {
        const double x0 = a.sx - a.w * 0.5, x1 = a.sx + a.w * 0.5, y0 = (double)a.sy - a.h, y1 = a.sy;
        const double ox = sx < x0 ? x0 - sx : sx > x1 ? sx - x1 : 0.0;
        const double oy = sy < y0 ? y0 - sy : sy > y1 ? sy - y1 : 0.0;
        if (ox == 0.0 && oy == 0.0) {
            if (best_in < 0 || a.depth < in_depth) { best_in = a.slot; in_depth = a.depth; }
        } else {
            const double d = std::sqrt(ox * ox + oy * oy);
            if (d < near_d) { near_d = d; best_near = a.slot; }
        }
    }
    return best_in >= 0 ? best_in : best_near;
}
