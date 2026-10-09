// Band rasteriser of the extended renderer (round 7, task A; port-only, nothing here is in carpet.exe).
//
// Same pixel formulas as poly_fill_triangle_722e3 / render_sprite_scaled_2ad60 (raster.cpp,
// render_things.cpp): 8-bit palette indices, nearest texel, the shade table SHADE[level << 8 | colour]
// and the blend table BLEND[a << 8 | b], affine (not perspective-correct) texture mapping, the same
// fill rule (rows [top, bottom), pixels [floor(x_left), floor(x_right))). What differs is the set-up:
// edges and attribute planes in double precision (any vertex range, any target size - the original's
// 16.16 edge walk overflows beyond ~32000 px and its span table holds 480 rows), clipping to a band
// of rows so several threads can draw one frame, u / v / shade clamped to the texture / table per span,
// and a "haze" attribute that mixes the pixel towards the sky texel behind it (dithered 4x4 Bayer
// between five steps 0, 1/4, 1/2, 3/4, 1 through the g_ext_mix tables).
#include "render_ext.h"
#include "mc_globals.h"
#include <cmath>
#include <cstring>
#include <algorithm>

uint8_t g_ext_mix[3][65536];

namespace {

// 4x4 ordered dither thresholds (16.16, (b + 0.5) / 16).
constexpr int32_t kBayer[4][4] = {
    { 0 * 4096 + 2048,  8 * 4096 + 2048,  2 * 4096 + 2048, 10 * 4096 + 2048 },
    { 12 * 4096 + 2048, 4 * 4096 + 2048, 14 * 4096 + 2048,  6 * 4096 + 2048 },
    { 3 * 4096 + 2048, 11 * 4096 + 2048,  1 * 4096 + 2048,  9 * 4096 + 2048 },
    { 15 * 4096 + 2048, 7 * 4096 + 2048, 13 * 4096 + 2048,  5 * 4096 + 2048 },
};

inline uint8_t sky_texel(const ExtSky &sky, int32_t u, int32_t v) {
    return sky.textured ? g_sky[(((uint32_t)v >> 8) & 0xff00u) | (((uint32_t)u >> 16) & 0xffu)] : sky.colour;
}

// Mix pixel p towards the sky colour k by haze step 0..4.
inline uint8_t haze_mix(uint8_t p, uint8_t k, int step) {
    if (step <= 0) return p;
    if (step >= 4) return k;
    return g_ext_mix[step - 1][((unsigned)p << 8) | k];
}

// Clamps a linear attribute over a span of n pixels to [lo, hi]: start value and per-pixel step.
inline void span_attr(double a0, double dadx, int n, double lo, double hi, int32_t &start, int32_t &step) {
    double a1 = a0 + dadx * (n - 1);
    a0 = std::min(std::max(a0, lo), hi);
    a1 = std::min(std::max(a1, lo), hi);
    start = (int32_t)a0;
    step = n > 1 ? (int32_t)((a1 - a0) / (n - 1)) : 0;
}

struct TriSetup {
    const ExtVtx *T, *M, *B;     // sorted by y
    bool   mid_right;            // M lies right of the long edge T-B
    double du_dx, du_dy, dv_dx, dv_dy, ds_dx, ds_dy, dh_dx, dh_dy;
    double x0, y0, u0, v0, s0, h0;
};

inline double edge_x(const ExtVtx *P, const ExtVtx *Q, int y) {
    return (double)P->x + (double)(y - P->y) * (double)(Q->x - P->x) / (double)(Q->y - P->y);
}

template <int MODE, bool HAZE>
void fill_rows(const ExtBand &band, const ExtSky &sky, const ExtTri &t, const TriSetup &S, int ya, int yb) {
    const uint8_t *shade = g_shade_table();
    const uint8_t *blend = g_blend_table();
    const uint8_t *tex = t.tex;
    const uint32_t colour = t.colour;
    const double uv_hi = (double)t.uv_max;
    constexpr bool uses_tex = MODE != 4;
    constexpr bool uses_shade = MODE == 4 || MODE == 5 || MODE == 26 || MODE == 27;
    constexpr bool uses_sky = HAZE || MODE == 27;       // 27: water over the sky texel behind the pixel
    (void)blend; (void)tex; (void)colour; (void)uv_hi;
    for (int y = ya; y < yb; ++y) {
        const double xa = edge_x(S.T, S.B, y);
        const double xb = y < S.M->y ? edge_x(S.T, S.M, y) : edge_x(S.M, S.B, y);
        const double xl = S.mid_right ? xa : xb, xr = S.mid_right ? xb : xa;
        int x0 = (int)std::floor(xl), x1 = (int)std::floor(xr);
        if (x0 < 0) x0 = 0;
        if (x1 > band.width) x1 = band.width;
        const int n = x1 - x0;
        if (n <= 0) continue;
        const double dx = x0 - S.x0, dy = y - S.y0;
        int32_t u = 0, du = 0, v = 0, dv = 0, s = 0, ds = 0, h = 0, dh = 0;
        if (uses_tex) {
            span_attr(S.u0 + S.du_dx * dx + S.du_dy * dy, S.du_dx, n, 0.0, uv_hi, u, du);
            span_attr(S.v0 + S.dv_dx * dx + S.dv_dy * dy, S.dv_dx, n, 0.0, uv_hi, v, dv);
        }
        if (uses_shade) span_attr(S.s0 + S.ds_dx * dx + S.ds_dy * dy, S.ds_dx, n, 0.0, 63.0 * 65536.0, s, ds);
        if (HAZE) span_attr(S.h0 + S.dh_dx * dx + S.dh_dy * dy, S.dh_dx, n, 0.0, 65536.0, h, dh);
        uint8_t *p = band.dest + (ptrdiff_t)y * band.pitch + x0;
        const uint8_t *behind = MODE == 26 && band.refl ? band.refl + (ptrdiff_t)y * band.pitch + x0 : p;
        (void)behind;
        int32_t su = 0, sv = 0;
        if (uses_sky) { su = sky.u0 + x0 * sky.dux + y * sky.duy; sv = sky.v0 + x0 * sky.dvx + y * sky.dvy; }
        const int32_t *bay = kBayer[y & 3];
        for (int i = 0; i < n; ++i) {
            uint32_t tx = 0;
            if (uses_tex) tx = tex[(((uint32_t)v >> 8) & 0xff00u) | (((uint32_t)u >> 16) & 0xffu)];
            const uint32_t g = ((uint32_t)s >> 16) & 0xffu;
            uint8_t out;
            if (MODE == 4)       out = shade[(g << 8) | colour];
            else if (MODE == 5)  out = shade[(g << 8) | tx];
            else if (MODE == 7)  out = shade[(colour << 8) | tx];
            else {                                   // 26: water over the reflection, 27: over the sky
                const uint8_t sh = shade[(g << 8) | tx];
                const uint32_t back = MODE == 27 ? sky_texel(sky, su, sv) : behind[i];
                out = tx < 0xc ? blend[(back << 8) | sh] : sh;
            }
            if (HAZE) {
                const int step = (h * 4 + bay[(x0 + i) & 3]) >> 16;
                if (step > 0) out = haze_mix(out, sky_texel(sky, su, sv), step);
                h += dh;
            }
            if (uses_sky) { su += sky.dux; sv += sky.dvx; }
            p[i] = out;
            u += du; v += dv; s += ds;
        }
    }
}

template <int MODE>
void fill_mode(const ExtBand &band, const ExtSky &sky, const ExtTri &t, const TriSetup &S, int ya, int yb) {
    if (t.haze) fill_rows<MODE, true>(band, sky, t, S, ya, yb);
    else        fill_rows<MODE, false>(band, sky, t, S, ya, yb);
}

} // namespace

void ext_fill_triangle(const ExtBand &band, const ExtSky &sky, const ExtTri &t) {
    if (t.ymax <= band.y0 || t.ymin >= band.y1) return;
    const ExtVtx &a = t.v[0], &b = t.v[1], &c = t.v[2];
    const int64_t area = (int64_t)(b.x - a.x) * (c.y - a.y) - (int64_t)(c.x - a.x) * (b.y - a.y);
    if (area <= 0) return;                                   // counter-clockwise / degenerate: back face
    TriSetup S;
    const ExtVtx *p[3] = { &a, &b, &c };
    std::sort(p, p + 3, [](const ExtVtx *l, const ExtVtx *r) { return l->y < r->y; });
    S.T = p[0]; S.M = p[1]; S.B = p[2];
    if (S.T->y == S.B->y) return;
    int ya = std::max(S.T->y, band.y0), yb = std::min(S.B->y, band.y1);
    if (ya >= yb) return;
    // M right of the long edge T-B?
    {
        const int64_t cross = (int64_t)(S.B->x - S.T->x) * (S.M->y - S.T->y) - (int64_t)(S.M->x - S.T->x) * (S.B->y - S.T->y);
        S.mid_right = cross < 0;
    }
    const double inv = 1.0 / (double)area;
    const double e1x = b.x - a.x, e1y = b.y - a.y, e2x = c.x - a.x, e2y = c.y - a.y;
    auto grad = [&](double A0, double A1, double A2, double &ddx, double &ddy) {
        const double d1 = A1 - A0, d2 = A2 - A0;
        ddx = (d1 * e2y - d2 * e1y) * inv;
        ddy = (d2 * e1x - d1 * e2x) * inv;
    };
    grad(a.u, b.u, c.u, S.du_dx, S.du_dy);
    grad(a.v, b.v, c.v, S.dv_dx, S.dv_dy);
    grad(a.s, b.s, c.s, S.ds_dx, S.ds_dy);
    grad(a.hz, b.hz, c.hz, S.dh_dx, S.dh_dy);
    S.x0 = a.x; S.y0 = a.y; S.u0 = a.u; S.v0 = a.v; S.s0 = a.s; S.h0 = a.hz;
    switch (t.mode) {
    case EXT_FILL_FLAT_SHADED: fill_mode<4>(band, sky, t, S, ya, yb); break;
    case EXT_FILL_TEX_FLATLIT: fill_mode<7>(band, sky, t, S, ya, yb); break;
    case EXT_FILL_WATER:       fill_mode<26>(band, sky, t, S, ya, yb); break;
    case EXT_FILL_WATER_SKY:   fill_mode<27>(band, sky, t, S, ya, yb); break;
    default:                  fill_mode<5>(band, sky, t, S, ya, yb); break;
    }
}

// ---------------------------------------------------------------------------------------------
// Sprites
// ---------------------------------------------------------------------------------------------

namespace {

template <class Put>
void sprite_rows(const ExtBand &band, const ExtSky &sky, const ExtSprite &s, Put put) {
    const double det = (double)s.ax * s.by - (double)s.ay * s.bx;
    if (std::fabs(det) < 1e-9) return;
    // (c, r) of the pixel centre (x + 0.5, y + 0.5): c = ((px - ox) * by - (py - oy) * bx) / det,
    // r = ((py - oy) * ax - (px - ox) * ay) / det.
    const double dc_dx = s.by / det, dc_dy = -s.bx / det;
    const double dr_dx = -s.ay / det, dr_dy = s.ax / det;
    const int ya = std::max<int>(s.ymin, band.y0), yb = std::min<int>(s.ymax, band.y1);
    const int stride = s.sw;
    const int32_t *bayer_row = nullptr;
    const bool haze = s.hz != 0;
    for (int y = ya; y < yb; ++y) {
        const double py = y + 0.5 - s.oy;
        const double c0 = (0.5 - s.ox) * dc_dx + py * dc_dy;   // at x = 0
        const double r0 = (0.5 - s.ox) * dr_dx + py * dr_dy;
        // x range with 0 <= c < sw and 0 <= r < sh
        double lo = 0, hi = band.width;
        auto limit = [&](double v0, double dv, double vmax) {
            if (std::fabs(dv) < 1e-12) { if (v0 < 0 || v0 >= vmax) hi = lo - 1; return; }
            double xa = (0 - v0) / dv, xb = (vmax - v0) / dv;
            if (xa > xb) std::swap(xa, xb);
            lo = std::max(lo, xa); hi = std::min(hi, xb);
        };
        limit(c0, dc_dx, s.sw);
        limit(r0, dr_dx, s.sh);
        const int x0 = std::max(0, (int)std::ceil(lo - 0.5)), x1 = std::min(band.width, (int)std::ceil(hi - 0.5));
        const int n = x1 - x0;
        if (n <= 0) continue;
        int32_t c, dc, r, dr;
        span_attr((c0 + x0 * dc_dx) * 65536.0, dc_dx * 65536.0, n, 0.0, (s.sw - 1) * 65536.0 + 65535.0, c, dc);
        span_attr((r0 + x0 * dr_dx) * 65536.0, dr_dx * 65536.0, n, 0.0, (s.sh - 1) * 65536.0 + 65535.0, r, dr);
        uint8_t *p = band.dest + (ptrdiff_t)y * band.pitch + x0;
        int32_t su = 0, sv = 0;
        int step = 0;
        if (haze) {
            su = sky.u0 + x0 * sky.dux + y * sky.duy; sv = sky.v0 + x0 * sky.dvx + y * sky.dvy;
            bayer_row = kBayer[y & 3];
        }
        for (int i = 0; i < n; ++i) {
            const uint8_t t = s.pix[(r >> 16) * stride + (c >> 16)];
            if (t != 0) {
                uint8_t out = put(t, p[i]);
                if (haze) {
                    step = ((int32_t)s.hz * 4 + bayer_row[(x0 + i) & 3]) >> 16;
                    if (step > 0) out = haze_mix(out, sky_texel(sky, su, sv), step);
                }
                p[i] = out;
            }
            c += dc; r += dr;
            if (haze) { su += sky.dux; sv += sky.dvx; }
        }
    }
}

} // namespace

void ext_draw_sprite(const ExtBand &band, const ExtSky &sky, const ExtSprite &s) {
    if (s.ymax <= band.y0 || s.ymin >= band.y1 || !s.pix || s.sw <= 0 || s.sh <= 0) return;
    const uint8_t *const blend = g_blend_table();
    const uint32_t shade_base = (uint32_t)s.shade & 0xffffff00u;
    const bool shade_ok = shade_base <= MC_TABLES_SIZE - 0x100;
    const uint8_t *const sh = g_tables_image + (shade_ok ? shade_base : 0);
    switch (s.mode) {
    case 0: sprite_rows(band, sky, s, [](uint8_t t, uint8_t) { return t; }); break;
    case 1: if (shade_ok) sprite_rows(band, sky, s, [sh](uint8_t t, uint8_t) { return sh[t]; }); break;
    case 2: sprite_rows(band, sky, s, [blend](uint8_t t, uint8_t d) { return blend[((unsigned)t << 8) | d]; }); break;
    case 3: sprite_rows(band, sky, s, [blend](uint8_t t, uint8_t d) { return blend[((unsigned)d << 8) | t]; }); break;
    case 4: {
        const unsigned k = s.upright ? 4u : 0u;
        sprite_rows(band, sky, s, [blend, k](uint8_t t, uint8_t) { return blend[(k << 8) | t]; });
        break;
    }
    case 5: {
        const unsigned k = s.upright ? 5u : 0u;
        sprite_rows(band, sky, s, [blend, k](uint8_t t, uint8_t) { return blend[((unsigned)t << 8) | k]; });
        break;
    }
    case 6: if (shade_ok) sprite_rows(band, sky, s, [blend, sh](uint8_t t, uint8_t d) { return sh[blend[((unsigned)t << 8) | d]]; }); break;
    case 7: if (shade_ok) sprite_rows(band, sky, s, [blend, sh](uint8_t t, uint8_t d) { return sh[blend[((unsigned)d << 8) | t]]; }); break;
    case 8: if (!s.upright && shade_ok) sprite_rows(band, sky, s, [sh](uint8_t, uint8_t d) { return sh[d]; }); break;
    case 9: {
        const uint8_t colour = (uint8_t)((uint32_t)s.shade >> 16);
        sprite_rows(band, sky, s, [colour](uint8_t, uint8_t) { return colour; });
        break;
    }
    default: break;
    }
}

void ext_draw_sky(const ExtBand &band, const ExtSky &sky) {
    for (int y = band.y0; y < band.y1; ++y) {
        uint8_t *row = band.dest + (ptrdiff_t)y * band.pitch;
        if (!sky.textured) { std::memset(row, sky.colour, (size_t)band.width); continue; }
        int32_t u = sky.u0 + y * sky.duy, v = sky.v0 + y * sky.dvy;
        for (int x = 0; x < band.width; ++x) {
            row[x] = g_sky[(((uint32_t)v >> 8) & 0xff00u) | (((uint32_t)u >> 16) & 0xffu)];
            u += sky.dux; v += sky.dvx;
        }
    }
}

// Nearest palette colour of an RGB triple (6-bit components), squared distance.
static uint8_t nearest6(const uint8_t *pal6, int r, int g, int b) {
    int best = 1 << 30, bi = 0;
    for (int i = 0; i < 256; ++i) {
        const int dr = pal6[i * 3] - r, dg = pal6[i * 3 + 1] - g, db = pal6[i * 3 + 2] - b;
        const int d = 2 * dr * dr + 3 * dg * dg + db * db;
        if (d < best) { best = d; bi = i; }
    }
    return (uint8_t)bi;
}

void ext_build_mix_tables(const uint8_t *pal6) {
    // Quantise the blend target through a 6-6-6 bit cube cache (64^3 = 262144 entries, built on demand).
    static uint8_t cube[1 << 18];
    static bool have[1 << 18];
    std::memset(have, 0, sizeof have);
    for (int k = 1; k <= 3; ++k) {
        uint8_t *out = g_ext_mix[k - 1];
        for (int p = 0; p < 256; ++p) {
            for (int q = 0; q < 256; ++q) {
                const int r = (pal6[p * 3] * (4 - k) + pal6[q * 3] * k + 2) / 4;
                const int g = (pal6[p * 3 + 1] * (4 - k) + pal6[q * 3 + 1] * k + 2) / 4;
                const int b = (pal6[p * 3 + 2] * (4 - k) + pal6[q * 3 + 2] * k + 2) / 4;
                const int key = ((r & 63) << 12) | ((g & 63) << 6) | (b & 63);
                if (!have[key]) { cube[key] = nearest6(pal6, r, g, b); have[key] = true; }
                out[(p << 8) | q] = cube[key];
            }
        }
    }
}
