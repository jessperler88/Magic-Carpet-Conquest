// Landscape renderer of carpet.exe: render_landscape_29050 (vertex grid, projection, fog, terrain
// quads, mirrored second surface), render_build_roll_table_28580, render_sky_2f080,
// render_set_view_window_2f3c0 and the mono path of render_view_2f6e0.
//
// Sources: disassembly of 0x29050..0x2ace8 / 0x28580 / 0x2f080 / 0x2f3c0 / 0x2f6e0 (see
// docs/analysis/port_render.md for the argument-order and UV-corner findings).
#include "render.h"
#include "thing.h"
#include "raster.h"
#include "mc_globals.h"
#include "mc_math.h"
#include "world.h"
#include "gen/core_tables.h"
#include "gen/render_tables.h"
#include <cstring>

RenderViewFn   g_render_view_override = nullptr;             // render.h Phase 3 seams
RenderInterp   g_render_interp{};
CellThingsFn   g_render_cell_things = nullptr;
CellThingsFn   g_render_cell_things_mirrored = nullptr;
uint32_t       g_anim_tick = 0;
bool           g_wave_phase_on = false;
int32_t        g_wave_phase = 0;
RenderCamState g_rcam{};
int32_t        g_eye_offset = g_eye_offset_default[0];        // DAT_00093b1c
RollEntry      g_roll_table[MC_ROLL_MAX + 1];                 // DAT_000b3a10 (+ leading guard)
RollState      g_roll{};

static int32_t g_slope_x = g_slope_smooth_default[0];         // DAT_00093f7c
static int32_t g_slope_y = g_slope_smooth_default[1];         // DAT_00093f80
static int32_t g_view_window_offset = 0;                      // DAT_000b5810

// Port: access to render_view's carried-over slope low-pass (DAT_00093f7c / 80) for
// render_reference_test, which seeds it from the original's frame dump (declared there; requested
// for render.h).
void render_get_slope_state(int32_t *x, int32_t *y) { *x = g_slope_x; *y = g_slope_y; }
void render_set_slope_state(int32_t x, int32_t y) { g_slope_x = x; g_slope_y = y; }

// 32-bit wrap-around multiply (the original's imul truncates; avoid signed-overflow UB).
static inline int32_t mul32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }

// Texture property lookup (tables have 164 entries; the original indexes them with the raw byte).
static inline uint8_t tex_prop(const uint8_t *table, unsigned t) { return t < 164 ? table[t] : 0; }

// ---------------------------------------------------------------------------------------------
// render_build_roll_table_28580(roll)
// ---------------------------------------------------------------------------------------------

static inline int32_t *roll_list() { return reinterpret_cast<int32_t *>(g_work_buf + MC_ROLL_LIST_OFFSET); }

// One Bresenham walk along the major axis: `count` entries, `major_add` bytes per entry, `carry_add`
// bytes (and one list entry) whenever the 16-bit accumulator of the slope wraps. `diag_add` is the
// per-entry byte step on the exact 45-degree angles where every entry is also a minor step.
static void roll_walk(int count, int32_t num, int32_t den, int32_t major_add, int32_t carry_add,
                      bool diagonal, int32_t diag_add) {
    RollEntry *e = g_roll_entries();
    int32_t *list = roll_list();
    int32_t offset = 0, steps = 0, index = 1;
    if (diagonal) {
        g_roll.step = 0x10000;
        for (int i = 0; i < count; ++i) {
            e[i].offset = offset;
            e[i].steps = steps;
            steps++;
            *list++ = index;
            offset += diag_add;
            index++;
        }
    } else {
        g_roll.step = (num << 8) / (den >> 8);
        const uint16_t step16 = (uint16_t)g_roll.step;
        uint16_t acc = 0;
        for (int i = 0; i < count; ++i) {
            e[i].offset = offset;
            e[i].steps = steps;
            uint32_t sum = (uint32_t)acc + step16;      // add bx, step16 ; jae
            acc = (uint16_t)sum;
            if (sum > 0xffff) {
                offset += carry_add;
                steps++;
                *list++ = index;
            }
            offset += major_add;
            index++;
        }
    }
    g_roll.steps = steps;                                // DAT_000b5878
}

// render_build_roll_table_28580
void render_build_roll_table(int roll) {
    const int32_t pitch = g_rt_pitch;
    const int32_t width = g_rt_width, height = g_rt_height;
    const unsigned a = (unsigned)roll & 0x7ff;
    g_roll.octant = (int32_t)a >> 8;                     // DAT_000b587c
    if (g_roll.octant > 7) return;                        // unreachable (a <= 0x7ff)

    // Per octant the original picks the sine/cosine of the angle relative to the octant's base
    // axis, the major extent, the byte steps and (cases 1/3/5/7) the exact diagonal special case.
    // (Ghidra's g_trig_table starts 0x100 entries before the sine table, so g_trig_table[a+k] is
    // sin(a + k - 0x100); mc_sin masks the index, which never leaves 0..0x7ff here anyway.)
    int32_t sin_a = 0, cos_a = 0, ext_major = 0, ext_minor = 0;
    switch (g_roll.octant) {
    case 0:
        sin_a = mc_sin(a); cos_a = mc_cos(a);                 // g_trig_table[a+0x100] = sin(a), [a+0x300] = cos(a)
        roll_walk(width, sin_a, cos_a, 1, pitch, false, 0);
        ext_major = width; ext_minor = height; g_roll.minor_step = pitch;
        break;
    case 1:
        sin_a = mc_sin(a); cos_a = mc_cos(a);
        roll_walk(height, cos_a, sin_a, pitch, 1, a == 0x100, 1 + pitch);
        ext_major = height; ext_minor = width; g_roll.minor_step = -1;
        break;
    case 2:
        sin_a = mc_sin(a - 0x200); cos_a = mc_sin(a);                       // g_trig_table[a-0x100] = -cos(a), [a+0x100] = sin(a)
        roll_walk(height, sin_a, cos_a, pitch, -1, false, 0);
        ext_major = height; ext_minor = width; g_roll.minor_step = -1;
        break;
    case 3:
        sin_a = mc_sin(a - 0x200); cos_a = mc_sin(a);
        roll_walk(width, cos_a, sin_a, -1, pitch, a == 0x300, pitch - 1);
        ext_major = width; ext_minor = height; g_roll.minor_step = -pitch;
        break;
    case 4:
        sin_a = mc_sin(a - 0x400); cos_a = mc_sin(a - 0x200);               // g_trig_table[a-0x300] = -sin(a), [a-0x100] = -cos(a)
        roll_walk(width, sin_a, cos_a, -1, -pitch, false, 0);
        ext_major = width; ext_minor = height; g_roll.minor_step = -pitch;
        break;
    case 5:
        sin_a = mc_sin(a - 0x400); cos_a = mc_sin(a - 0x200);
        roll_walk(height, cos_a, sin_a, -pitch, -1, a == 0x500, -1 - pitch);
        ext_major = height; ext_minor = width; g_roll.minor_step = 1;
        break;
    case 6:
        sin_a = mc_sin(a - 0x600); cos_a = mc_sin(a - 0x400);               // g_trig_table[a-0x500] = cos(a), [a-0x300] = -sin(a)
        roll_walk(height, sin_a, cos_a, -pitch, 1, false, 0);
        ext_major = height; ext_minor = width; g_roll.minor_step = 1;
        break;
    case 7:
        sin_a = mc_sin(a - 0x600); cos_a = mc_sin(a - 0x400);
        roll_walk(width, cos_a, sin_a, 1, -pitch, a == 0x700, 1 - pitch);
        ext_major = width; ext_minor = height; g_roll.minor_step = pitch;
        break;
    }
    g_roll.sin_a = sin_a;                                 // DAT_000b58a4
    g_roll.cos_a = cos_a;                                 // DAT_000b5868
    g_roll.neg_steps = -g_roll.steps;                     // DAT_000b5888
    g_roll.extent_minor = ext_minor;                      // DAT_000b5898
    g_roll.clip_a = ext_minor + g_roll.neg_steps;         // DAT_000b5858
    g_roll.extent_major = ext_major;                      // DAT_000b589c
    g_roll.extent_sum = width + height;                   // DAT_000b5874
    g_roll.list_last = -1 - g_roll.neg_steps;             // DAT_000b5860 = list + (steps-1)*4

    // Deltas: entry[i].delta = entry[i].offset - entry[i-1].offset (entry -1 is the zero guard;
    // the original reads the dword before DAT_000b3a10 there).
    RollEntry *e = g_roll_entries();
    for (int i = 0; i < g_roll.extent_major; ++i) e[i].delta = e[i].offset - e[i - 1].offset;
}

// ---------------------------------------------------------------------------------------------
// render_sky_2f080(roll): rotated + scrolled 256x256 sky into the render target.
// ---------------------------------------------------------------------------------------------

// render_sky_2f080
void render_sky(int roll) {
    const unsigned a = (unsigned)roll & 0x7ff;
    const int width = g_rt_width;
    if (width <= 0) return;
    const int32_t d_sin = (mc_sin(a) << 8) / width;       // local_24: 256 texels across the width
    const int32_t d_cos = (mc_cos(a) << 8) / width;       // local_28

    // Per-column (du, dv) byte deltas of the rotated x axis (u follows cos, v follows sin).
    static int8_t deltas[MC_ROLL_MAX * 2];
    {
        int32_t acc_u = 0, acc_v = 0;
        uint8_t prev_u = 0, prev_v = 0;
        for (int i = 0; i < width && i < MC_ROLL_MAX; ++i) {
            uint8_t cu = (uint8_t)(acc_u >> 16), cv = (uint8_t)(acc_v >> 16);
            deltas[2 * i]     = (int8_t)(uint8_t)(cu - prev_u);
            deltas[2 * i + 1] = (int8_t)(uint8_t)(cv - prev_v);
            prev_u = cu; prev_v = cv;
            acc_v += d_sin;
            acc_u += d_cos;
        }
    }

    // Start texel of the top-left pixel: rotate the horizon point back and apply the yaw scroll
    // (u scroll = yaw * 0x8000 >> 16 texels).
    const int32_t px = g_rcam.screen_cx + ((-(g_rcam.horizon * g_rcam.sin_roll)) >> 16);
    const int32_t py = g_rcam.screen_cy - ((g_rcam.horizon * g_rcam.cos_roll) >> 16);
    int32_t u = (int32_t)((uint32_t)g_rcam.yaw16 << 15) - (d_cos * px - d_sin * py);   // local_38
    int32_t v = -(py * d_cos + px * d_sin);                                              // local_20

    uint8_t *row = g_rt_dest;
    for (int y = 0; y < g_rt_height; ++y) {
        uint8_t cu = (uint8_t)(u >> 16), cv = (uint8_t)(v >> 16);
        const int n = (width >> 2) << 2;                  // the original writes width/4 dwords
        for (int x = 0; x < n; ++x) {
            row[x] = g_sky[((unsigned)cv << 8) | cu];
            cu = (uint8_t)(cu + deltas[2 * x]);            // the delta of column x is applied
            cv = (uint8_t)(cv + deltas[2 * x + 1]);        // after sampling pixel x (as the exe)
        }
        row += g_rt_pitch;
        u -= d_sin;
        v += d_cos;
    }
}

// ---------------------------------------------------------------------------------------------
// render_landscape_29050
// ---------------------------------------------------------------------------------------------

static inline void tri(const PolyVertex &a, const PolyVertex &b, const PolyVertex &c) {
    poly_fill_triangle(&a, &b, &c);
}

// Fills a quad whose far-left vertex record is r0: r1 = next column (same row), r2 = next column
// of the nearer row, r3 = same column of the nearer row. UV corners: dwords 0/1 -> r0, 2/3 -> r1,
// 4/5 -> r2, 6/7 -> r3 of g_uv_table[uv_sel*8]. Diagonal from flags bit0: 0 -> r0-r2, 1 -> r1-r3.
// The mirrored pass uses the mirrored screen coordinates and the opposite winding.
static void draw_quad_tris(const VertexRec *r0, bool mirrored) {
    const VertexRec *r1 = r0 + 1, *r2 = r0 - (MC_GRID_COLS - 1), *r3 = r0 - MC_GRID_COLS;
    const int32_t *uv = g_uv_table + (unsigned)r0->uv_sel * 8;
    PolyVertex v0, v1, v2, v3;
    if (!mirrored) {
        v0 = {r0->sx, r0->sy, uv[0], uv[1], r0->shade};
        v1 = {r1->sx, r1->sy, uv[2], uv[3], r1->shade};
        v2 = {r2->sx, r2->sy, uv[4], uv[5], r2->shade};
        v3 = {r3->sx, r3->sy, uv[6], uv[7], r3->shade};
    } else {
        v0 = {r0->sx_mirror, r0->sy_mirror, uv[0], uv[1], r0->shade};
        v1 = {r1->sx_mirror, r1->sy_mirror, uv[2], uv[3], r1->shade};
        v2 = {r2->sx_mirror, r2->sy_mirror, uv[4], uv[5], r2->shade};
        v3 = {r3->sx_mirror, r3->sy_mirror, uv[6], uv[7], r3->shade};
    }
    g_texture_ptr = g_texture_table[r0->texture];         // DAT_0009b5f8 = DAT_0009afec[tex]
    if (!mirrored) {
        if (!(r0->flags & 1)) { tri(v0, v1, v2); tri(v0, v2, v3); }
        else                  { tri(v0, v1, v3); tri(v3, v1, v2); }
    } else {
        if (!(r0->flags & 1)) { tri(v0, v2, v1); tri(v0, v3, v2); }
        else                  { tri(v0, v3, v1); tri(v3, v2, v1); }
    }
}

// Terrain quad of the main pass. `water` = the second-surface variant of the loop, which draws
// translucent textures (flags bit7) with mode 0x1a; the plain variant never sets that bit.
static void draw_quad_terrain(VertexRec *r0, bool water) {
    const VertexRec *r1 = r0 + 1, *r2 = r0 - (MC_GRID_COLS - 1), *r3 = r0 - MC_GRID_COLS;
    const uint8_t or_flags  = r0->flags | r1->flags | r2->flags | r3->flags;
    const uint8_t and_flags = r0->flags & r1->flags & r2->flags & r3->flags;
    if (water && (r0->flags & 0x80)) {
        g_fill_mode = 0x1a;
    } else if (r0->flags2 & 0x10) {
        g_fill_mode = 7;
        g_fill_colour = (uint8_t)((r0->shade + r1->shade + r2->shade + r3->shade) >> 18);
    } else {
        g_fill_mode = 5;
    }
    if (!(or_flags & 2) && !(and_flags & 0x78)) draw_quad_tris(r0, false);
    if (r0->first_thing != 0 && g_render_cell_things)      // render_cell_things_2c600
        g_render_cell_things(r0->first_thing, r0);
}

// Mirrored (second surface) quad: the terrain reflected in the z = 0 plane, drawn before the
// terrain so it shows through the translucent water. Texture 0 cells are skipped; the mode-7
// selection is overridden to 5 right before drawing (as the exe does); there is no off-screen test
// (the exe tests a register it has just zeroed).
static void draw_quad_mirrored(VertexRec *r0) {
    if (r0->texture != 0) {
        const VertexRec *r1 = r0 + 1, *r2 = r0 - (MC_GRID_COLS - 1), *r3 = r0 - MC_GRID_COLS;
        const uint8_t or_flags = r0->flags | r1->flags | r2->flags | r3->flags;
        if (r0->flags2 & 0x10) {
            g_fill_mode = 7;
            g_fill_colour = (uint8_t)((r0->shade + r1->shade + r2->shade + r3->shade) >> 18);
        } else {
            g_fill_mode = 5;
        }
        if (!(or_flags & 2)) {
            g_fill_mode = 5;
            draw_quad_tris(r0, true);
        }
    }
    if (r0->first_thing != 0 && g_render_cell_things_mirrored)   // render_cell_things_mirrored_2e5a0 (0x29ae6 / 0x29aef)
        g_render_cell_things_mirrored(r0->first_thing, r0);
}

// Row loop of the draw passes: rows from far (20) to near (1); within a row the columns on the near
// side of the view axis (flags bit2 clear) left to right, then the far side right to left down to
// the column where the sweep stopped, so the quad on the axis is drawn last (painter's order).
template <class F> static void for_each_quad_far_to_near(VertexRec *V, F &&quad) {
    for (int row = MC_GRID_ROWS - 1; row >= 1; --row) {
        VertexRec *base = V + row * MC_GRID_COLS;
        int c = 0;
        for (; c < MC_GRID_COLS - 1; ++c) {
            if (base[c + 1].flags & 4) break;
            quad(base + c);
        }
        if (c < MC_GRID_COLS - 1)
            for (int k = MC_GRID_COLS - 2; k >= c; --k) quad(base + k);
    }
}

// render_landscape_29050(cam_x, cam_y, yaw, cam_z, pitch, roll, zoom)
void render_landscape(const Camera &cam) {
    VertexRec *V = g_work_vertices();
    const int width = g_rt_width, height = g_rt_height;

    g_rcam.shadows   = g_state->opt_shadows;                       // DAT_000b58af
    g_rcam.screen_cx = (width >> 1) + g_eye_offset;                // DAT_000b5894
    g_rcam.screen_cy = height >> 1;                                // DAT_000b586c
    const unsigned yaw = (unsigned)cam.yaw & 0x7ff;
    // The low 16 bits only (the original's field): the Thing renderer's (int16_t)(x - cam_x16) is the exact
    // difference in any world size for anything within 128 cells, and this grid reaches 20.
    g_rcam.cam_x16 = (uint16_t)cam.cam_x;                          // DAT_000b58ac
    g_rcam.yaw16   = (uint16_t)yaw;                                // DAT_000b58a8
    g_rcam.cam_y16 = (uint16_t)cam.cam_y;                          // DAT_000b58aa
    g_rcam.cam_z   = cam.cam_z;                                    // DAT_000b5884
    g_rcam.cos_yaw = mc_cos(yaw);                                  // DAT_000b5864 (0x98bec[yaw+0x100])
    g_rcam.sin_yaw = mc_sin(yaw);                                  // DAT_000b58a0 (0x983ec[yaw+0x100])

    // View quadrant and the angle inside it (-0x100..0xff).
    const unsigned yq = yaw + 0x100;
    const int q = (int)(yq >> 9) & 3;
    const unsigned aq = ((yq & 0x1ff) - 0x100) & 0x7ff;
    const int32_t sin_q = mc_sin(aq), cos_q = mc_cos(aq);

    render_build_roll_table((-cam.roll) & 0x7ff);

    // Focal length: screen diagonal scaled by zoom (0x100 = 1.0).
    g_rcam.focal = (int32_t)((mc_isqrt((uint32_t)(width * width + height * height)) * (uint32_t)cam.zoom) >> 8);

    const QuadStep &qs = mc_quad_steps()[q];
    const int fx = cam.cam_x & 0xff, fy = cam.cam_y & 0xff;
    int lat0 = 0, fwd0 = 0;         // lateral coordinate of column 0, forward coordinate of row 0
    switch (q) {
    case 0: fwd0 = fy + kRowStart; lat0 = kColStart - fx; break;   // looking -y
    case 1: fwd0 = -fx;            lat0 = kColStart - fy; break;   // looking +x
    case 2: fwd0 = -fy;            lat0 = kColStart + fx; break;   // looking +y
    case 3: fwd0 = fx + kRowStart; lat0 = kColStart + fy; break;   // looking -x
    }

    // Vertex grid: rotate (lateral, forward) by the in-quadrant angle into camera space.
    // x_cam = lat*cos - fwd*sin, z_cam = lat*sin + fwd*cos (each product shifted separately).
    for (int c = 0; c < MC_GRID_COLS; ++c) {
        const int32_t lat = lat0 + c * 0x100;
        const int32_t xl = (cos_q * lat) >> 16, zl = (sin_q * lat) >> 16;
        const uint8_t fl = lat < 0 ? 0 : 4;                 // bit2: far side of the view axis
        for (int r = 0; r < MC_GRID_ROWS; ++r) {
            VertexRec &v = V[r * MC_GRID_COLS + c];
            v.x_cam = xl; v.z_cam = zl; v.flags = fl; v.flags2 = 0;
        }
    }
    for (int r = 0; r < MC_GRID_ROWS; ++r) {
        const int32_t fwd = fwd0 + r * 0x100;
        const int32_t xf = (sin_q * fwd) >> 16, zf = (cos_q * fwd) >> 16;
        for (int c = 0; c < MC_GRID_COLS; ++c) {
            VertexRec &v = V[r * MC_GRID_COLS + c];
            v.x_cam -= xf; v.z_cam += zf;
        }
    }

    g_rcam.fog_far2   = kFogFar2;                                  // DAT_000b5850
    g_rcam.fog_near2  = kFogNear2;                                 // DAT_000b5848
    g_rcam.cull_dist2 = kCullDist2;                                // DAT_000b584c
    g_rcam.fog_div    = kFogDiv;                                   // DAT_000b5844
    g_rcam.horizon    = (cam.pitch * width) >> 8;                  // DAT_000b588c
    // round 11: cell coordinates wrap at the world size (world.h; the original's uint8_t at 256 cells)
    const int cx0 = world_wrap_cell((cam.cam_x >> 8) + qs.start_dx);
    const int cy0 = world_wrap_cell((cam.cam_y >> 8) + qs.start_dy);
    g_rcam.sin_roll = mc_sin((unsigned)cam.roll & 0x7ff);          // DAT_000b585c
    g_rcam.cos_roll = mc_cos((unsigned)cam.roll & 0x7ff);          // DAT_000b5870

    // Sky: SIRDS clears to 0x40, textured sky, or clear to 0xff.
    const bool sirds = g_state->mode_3d == 2 && g_state->opt_interlaced == 0;
    if (sirds || g_state->opt_textured_sky == 0) {
        const uint8_t colour = sirds ? 0x40 : 0xff;
        uint8_t *row = g_rt_dest;
        const int n = (width >> 2) << 2;
        for (int y = 0; y < height; ++y, row += g_rt_pitch) std::memset(row, colour, (size_t)n);
    } else {
        render_sky(cam.roll);
    }

    // The second-surface variant of the vertex pass also computes the mirrored height and marks
    // translucent / flat-shaded textures; the SIRDS path uses the plain variant.
    const bool second = g_state->opt_second_surface != 0 && !sirds;
    const int32_t phase = g_wave_phase_on ? g_wave_phase : (int32_t)(g_anim_tick << 6);   // player tick * 0x40

    int cx = cx0, cy = cy0;
    VertexRec *rec = V;
    for (int r = 0; r < MC_GRID_ROWS; ++r) {
        for (int c = 0; c < MC_GRID_COLS; ++c, ++rec) {
            const uint32_t cell = world_cell(cx, cy);
            const int32_t light = (int32_t)g_map_light[cell] * 0x100 + 0x80;
            int32_t z = rec->z_cam;
            rec->first_thing = 0;
            const int32_t d2 = rec->x_cam * rec->x_cam + z * z;
            if (z < -0xff || d2 >= kCullDist2) {
                rec->flags |= 2;
            } else {
                if (z < 0x80) z = 0x80;
                rec->sx = mul32(rec->x_cam, g_rcam.focal) / z;
                rec->h_rel = (int32_t)g_map_height[cell] * 0x20 - cam.cam_z;
                // Water wave: sin(phase + x*0x80) * sin(phase + y*0x80) in 8.8 * 8.8.
                int32_t wave = (mc_sin((unsigned)(phase + cx * 0x80) & 0x7ff) >> 8) *
                               (mc_sin((unsigned)(cy * 0x80 + phase) & 0x7ff) >> 8);
                if (second)
                    rec->h_mirror = -cam.cam_z - ((((wave >> 4) + 0x8000) * (int32_t)g_map_height[cell]) >> 10);
                if (g_map_flags[cell] & 8) rec->h_rel -= wave >> 10;
                else wave = 0;
                int32_t shade = wave * 8 + (light << 8);
                if (d2 > kFogNear2) {
                    if (d2 < kFogFar2) shade = (int32_t)(((int64_t)(kFogFar2 - d2) * shade) / kFogDiv);
                    else shade = 0;
                }
                rec->shade = shade;
                rec->sy = mul32(rec->h_rel, g_rcam.focal) / z + g_rcam.horizon;
                if (second) rec->sy_mirror = mul32(rec->h_mirror, g_rcam.focal) / z + g_rcam.horizon;

                const int tx = world_wrap_cell(cx + qs.tex_dx), ty = world_wrap_cell(cy + qs.tex_dy);
                const uint32_t tcell = world_cell(tx, ty);
                rec->texture = g_map_type[tcell];
                if (second) {
                    if (tex_prop(g_tex_prop_water, rec->texture)) rec->flags |= 0x80;
                    if (tex_prop(g_tex_prop_flat, rec->texture))  rec->flags2 |= 0x10;
                }
                rec->tex_prop = tex_prop(g_tex_prop_shadow, rec->texture);
                rec->uv_sel = (uint8_t)(((g_map_flags[tcell] >> 2) & 0x1c) + q);
                rec->first_thing = g_cell_things[world_cell(tx + qs.thing_dx, ty + qs.thing_dy)];
            }
            rec->flags |= (uint8_t)((cx + cy) & 1);              // bit0: which diagonal splits the quad
            cx = world_wrap_cell(cx + qs.col_dx);
            cy = world_wrap_cell(cy + qs.col_dy);
        }
        cx = world_wrap_cell(cx + qs.row_dx);
        cy = world_wrap_cell(cy + qs.row_dy);
    }

    // Screen roll about the screen centre and the off-screen flags.
    {
        const int32_t sr = g_rcam.sin_roll, cr = g_rcam.cos_roll;
        rec = V;
        for (int i = 0; i < MC_GRID_CELLS; ++i, ++rec) {
            const int32_t sx = rec->sx, sy = rec->sy;
            rec->sx = g_rcam.screen_cx + ((mul32(cr, sx) - mul32(sy, sr)) >> 16);
            rec->sy = g_rcam.screen_cy - ((mul32(sy, cr) + mul32(sx, sr)) >> 16);
            if (second) {
                const int32_t sym = rec->sy_mirror;
                rec->sx_mirror = g_rcam.screen_cx + ((mul32(cr, sx) - mul32(sym, sr)) >> 16);
                rec->sy_mirror = g_rcam.screen_cy - ((mul32(sym, cr) + mul32(sx, sr)) >> 16);
            }
            if (rec->sx < 0) rec->flags |= 8; else if (rec->sx >= width) rec->flags |= 0x10;
            if (rec->sy < 0) rec->flags |= 0x20; else if (rec->sy >= height) rec->flags |= 0x40;
            if (second) {
                if (rec->sx_mirror < 0) rec->flags2 |= 1; else if (rec->sx_mirror >= width) rec->flags2 |= 2;
                if (rec->sy_mirror < 0) rec->flags2 |= 4; else if (rec->sy_mirror >= height) rec->flags2 |= 8;
            }
        }
    }

    if (sirds) {
        // SIRDS depth pass (fill mode 1, depth = (0x1400 - z_cam) * 0x15e, render_cell_things_sirds_2dac0)
        // is not ported.
        return;
    }
    if (second && cam.cam_z < 0x1000)
        for_each_quad_far_to_near(V, [](VertexRec *r0) { draw_quad_mirrored(r0); });
    for_each_quad_far_to_near(V, [second](VertexRec *r0) { draw_quad_terrain(r0, second); });
}

// ---------------------------------------------------------------------------------------------
// render_set_view_window_2f3c0(size)
// ---------------------------------------------------------------------------------------------

// render_set_view_window_2f3c0
void render_set_view_window(const FrameBuffer &fb, int size) {
    const int n = 0x28 - size;
    int w, h;
    if (fb.width == 320) {                                // DAT_0012edae bit0
        g_view_window_offset = n * 4 + ((n * 5) / 2) * fb.width;
        w = (size * 8) & 0xffff; h = (size * 5) & 0xffff;
    } else {
        g_view_window_offset = n * 8 + ((n * 0xc) / 2) * fb.width;
        w = (size * 16) & 0xffff; h = (size * 0xc) & 0xffff;
    }
    render_set_viewport(fb.pixels + g_view_window_offset, nullptr, fb.width, w, h);
}

// ---------------------------------------------------------------------------------------------
// render_view_2f6e0(fb, cam_x, cam_y, yaw, cam_z, pitch, roll, zoom) - mono path.
// ---------------------------------------------------------------------------------------------

// render_view_2f6e0
void render_view(const FrameBuffer &fb, const Camera &cam_in) {
    Camera cam = cam_in;

    // Terrain-slope camera offset: sample the height map 2 cells apart around the camera cell
    // (rounded down when the fraction is < 0x80), low-pass the clamped slope and add it to the
    // camera position.
    int cx = world_cx(cam.cam_x), cy = world_cx(cam.cam_y);
    if ((cam.cam_x & 0xff) < 0x80) cx--;
    if ((cam.cam_y & 0xff) < 0x80) cy--;
    const int h00 = g_map_height[world_cell(cx, cy)];
    const int h20 = g_map_height[world_cell(cx + 2, cy)];
    const int h22 = g_map_height[world_cell(cx + 2, cy + 2)];
    const int h02 = g_map_height[world_cell(cx, cy + 2)];
    int slope_x = (h00 - h20 - h22 + h02) * 2;
    int slope_y = (h00 + h20 - h22 - h02) * 2;
    if (slope_x > 100) slope_x = 100; else if (slope_x < -100) slope_x = -100;
    if (slope_y > 100) slope_y = 100; else if (slope_y < -100) slope_y = -100;
    g_slope_x += (slope_x - g_slope_x) >> 3;
    g_slope_y += (slope_y - g_slope_y) >> 3;
    cam.cam_x += g_slope_x;
    cam.cam_y += g_slope_y;
    cam.yaw &= 0x7ff;

    // Interlaced stereo (opt_interlaced && mode_3d && width == 640: two half-height passes with
    // g_eye_offset -5 / +5), SIRDS (mode_3d == 2: depth image into g_frame2 + random-dot pattern)
    // and anaglyph (mode_3d == 1: two passes with g_eye_offset +-pitch/40 into fb / g_frame2) are
    // not ported; only the mono path follows.

    const uint8_t saved_blur = g_state->opt_motion_blur;
    {
        // Automatic ghost blend in 320x200 single player when the player's thing is fast.
        const PlayerRec &pl = g_state->players[g_state->local_player];
        if (fb.width == 320 && pl.input_mode == 0 && g_cfg->pentium != 0 && g_frame2 != nullptr &&
            g_state->view_size == 0x28) {
            int speed = thing_at(pl.thing)->speed_cur;
            if (speed < 0) speed = -speed;
            if (speed > 0x50) g_state->opt_motion_blur = 1;
        }
    }

    uint8_t *dest = g_rt_dest;
    const int width = g_rt_width, height = g_rt_height, pitch = g_rt_pitch;
    const uint8_t *blend = g_blend_table();
    if (g_state->opt_motion_blur == 0 || g_frame2 == nullptr) {
        render_landscape(cam);
        if (g_state->opt_smooth != 0) {
            // 2x2 smoothing: p = BLEND[BLEND[p11][p01]][BLEND[p10][p00]] over (height-1) x (width-1).
            uint8_t *row = dest;
            for (int y = 0; y < height - 1; ++y, row += pitch) {
                for (int x = 0; x < width - 1; ++x) {
                    const uint8_t a = blend[((unsigned)row[x + pitch] << 8) | row[x]];
                    const uint8_t b = blend[((unsigned)row[x + pitch + 1] << 8) | row[x + 1]];
                    row[x] = blend[((unsigned)b << 8) | a];
                }
            }
        }
    } else {
        // Render into the second buffer, then blend it with the previous frame in the target.
        render_set_viewport(g_frame2, nullptr, 0, 0, 0);
        render_landscape(cam);
        render_set_viewport(dest, nullptr, 0, 0, 0);
        const int n = (width >> 2) << 2;
        uint8_t *d = dest;
        const uint8_t *s = g_frame2;
        if (g_state->opt_motion_blur == 1) {
            for (int y = 0; y < height; ++y, d += pitch, s += pitch)
                for (int x = 0; x < n; ++x) d[x] = blend[((unsigned)d[x] << 8) | s[x]];
        } else {
            for (int y = 0; y < height; ++y, d += pitch, s += pitch)
                for (int x = 0; x < n; ++x) d[x] = blend[((unsigned)s[x] << 8) | d[x]];
        }
    }
    g_state->opt_motion_blur = saved_blur;
}
