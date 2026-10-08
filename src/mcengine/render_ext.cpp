// Extended renderer (round 7, task A; port-only): render_view_ext draws the 3D view into a frame of
// any size with a configurable draw distance (g_settings section A), in the original's 8-bit look:
// the same textures, UV table, shade / blend tables, sky, fill modes, sprite selection and pixel
// modes, shadows and reflections. docs/analysis/port_render_ext.md describes the design; in short:
//
// - Terrain is a world-aligned grid around the camera cell (not the original's 40 x 21 view-aligned
//   rectangle). Every cell inside the draw circle and the horizontal view frustum becomes one quad
//   with the original's corner / UV / diagonal assignment (the view-quadrant-0 case of
//   render_landscape_29050, which is world-consistent). Painter's order as the original: rows along
//   the view quadrant's axis far to near, inside a row the columns from the outside in; behind the
//   camera row the same mirrored. Things of a cell are drawn right after the quad one row nearer
//   (the original's thing-cell offset in DAT_00093b20).
// - Level of detail: texture mip levels (renderer-only, palette-quantised box filter) once a texel
//   is smaller than a pixel, the per-texture average colour (flat shaded) at 1 texel per cell, and
//   beyond D0 = focal / 3 px (lod 1) 2x2, 4x4, 8x8 cell quads on world-aligned nested squares
//   (clipmap style: each level's region is a union of the next level's cells), with skirts hanging
//   from both sides of every level boundary so no crack shows.
// - Fog: the original's constants scaled to the draw distance (full light below fog_start_pct, shade 0
//   at 15/16 of the distance, culled at the distance - with 20 cells and 75 % exactly the original's
//   0xe10000 / 0x1690000 / 0x1900000). With the textured sky the far terrain and things instead haze
//   into the sky texel behind them (four mix steps, 4x4 ordered dither) so the horizon has no edge.
// - The frame is described once as a display list (triangles and sprites in painter's order) and
//   drawn by a small thread pool in horizontal bands (render_ext_raster.cpp).
#include "constructors.h"   // mana_ball_sprite
#include "render.h"
#include "render_ext.h"
#include "raster.h"
#include "settings.h"
#include "mc_globals.h"
#include "mc_math.h"
#include "sprites.h"
#include "terrain.h"
#include "thing.h"
#include "world.h"
#include "gen/core_tables.h"
#include "gen/render_tables.h"
#include "gen/sprites_tables.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

RenderExtStats g_render_ext_stats{};
uint8_t *g_render_ext_cell_count = nullptr;

namespace {

// ---------------------------------------------------------------------------------------------
// Thread pool (persistent, detached workers; never destroyed so process exit cannot deadlock)
// ---------------------------------------------------------------------------------------------

class BandPool {
public:
    void ensure(int workers) {
        std::lock_guard<std::mutex> lk(m_);
        while ((int)count_ < workers) {
            std::thread([this] { worker(); }).detach();
            count_++;
        }
    }
    int workers() const { return (int)count_; }
    // Runs f(0..n-1) on the workers and the calling thread; returns when all are done.
    void run(int n, const std::function<void(int)> &f) {
        if (count_ == 0 || n <= 1) { for (int i = 0; i < n; ++i) f(i); return; }
        {
            std::lock_guard<std::mutex> lk(m_);
            job_ = &f; njobs_ = n; next_ = 0; remaining_ = n; gen_++;
        }
        cv_.notify_all();
        {
            std::lock_guard<std::mutex> lk(m_);
            active_++;
        }
        work(f, n);
        std::unique_lock<std::mutex> lk(m_);
        active_--;
        done_.wait(lk, [&] { return remaining_ == 0 && active_ == 0; });
        job_ = nullptr;
    }

private:
    void work(const std::function<void(int)> &f, int n) {
        for (;;) {
            const int i = next_.fetch_add(1);
            if (i >= n) break;
            f(i);
            if (remaining_.fetch_sub(1) == 1) {
                std::lock_guard<std::mutex> lk(m_);
                done_.notify_all();
            }
        }
    }
    void worker() {
        uint64_t seen = 0;
        for (;;) {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return gen_ != seen && job_ != nullptr; });
            seen = gen_;
            const std::function<void(int)> *f = job_;
            const int n = njobs_;
            active_++;
            lk.unlock();
            work(*f, n);
            lk.lock();
            active_--;
            if (remaining_ == 0 && active_ == 0) done_.notify_all();
        }
    }
    std::mutex m_;
    std::condition_variable cv_, done_;
    const std::function<void(int)> *job_ = nullptr;
    int njobs_ = 0;
    std::atomic<int> next_{0}, remaining_{0};
    int active_ = 0;
    uint64_t gen_ = 0;
    std::atomic<int> count_{0};
};

BandPool &pool() {
    static BandPool *p = new BandPool();    // intentionally never deleted (detached workers)
    return *p;
}

// ---------------------------------------------------------------------------------------------
// Renderer-only resources: texture mip atlases, per-texture average colours, colour cube, mix tables
// ---------------------------------------------------------------------------------------------

constexpr int kMaxMip = 5;                  // 32 -> 16 -> 8 -> 4 -> 2 -> 1
struct Resources {
    const uint8_t *atlas = nullptr;
    int B = 0;
    uint8_t pal[768] = {};
    bool valid = false;
    int mips = 0;                           // log2(B): mip `mips` is 1 texel = the average colour
    std::vector<uint8_t> mip_atlas[kMaxMip + 1];
    const uint8_t *mip_tex[kMaxMip + 1][256] = {};
    uint8_t avg[256] = {};                  // exact B x B average colour per texture (nearest palette entry)
    uint8_t avg_rgb[256][3] = {};
    uint8_t cube[1 << 18] = {};             // 6-6-6 RGB -> nearest palette index (lazy)
    bool cube_have[1 << 18] = {};
};
Resources &res() { static Resources *r = new Resources(); return *r; }

uint8_t nearest_colour(int r, int g, int b) {
    Resources &R = res();
    r = std::clamp(r, 0, 63); g = std::clamp(g, 0, 63); b = std::clamp(b, 0, 63);
    const int key = (r << 12) | (g << 6) | b;
    if (!R.cube_have[key]) {
        int best = 1 << 30, bi = 0;
        for (int i = 0; i < 256; ++i) {
            const int dr = R.pal[i * 3] - r, dg = R.pal[i * 3 + 1] - g, db = R.pal[i * 3 + 2] - b;
            const int d = 2 * dr * dr + 3 * dg * dg + db * db;
            if (d < best) { best = d; bi = i; }
        }
        R.cube[key] = (uint8_t)bi;
        R.cube_have[key] = true;
    }
    return R.cube[key];
}

void ensure_resources() {
    Resources &R = res();
    const int B = (int8_t)g_state->texture_block_size;
    if (R.valid && R.atlas == g_texture_atlas && R.B == B && std::memcmp(R.pal, g_palette6, 768) == 0) return;
    const bool pal_changed = std::memcmp(R.pal, g_palette6, 768) != 0 || !R.valid;
    std::memcpy(R.pal, g_palette6, 768);
    if (pal_changed) {
        std::memset(R.cube_have, 0, sizeof R.cube_have);
        ext_build_mix_tables(g_palette6);
    }
    R.atlas = g_texture_atlas; R.B = B; R.valid = true;
    R.mips = 0;
    while ((1 << R.mips) < B && R.mips < kMaxMip) R.mips++;
    // Mip m: each texture box-filtered to (B >> m) texels, packed like the original atlas
    // (256 / Bm textures per 256-byte row) so the rasteriser's texel fetch works unchanged.
    for (int t = 0; t < 256; ++t) R.mip_tex[0][t] = g_texture_table[t];
    for (int m = 1; m <= R.mips; ++m) {
        const int Bm = B >> m, cols = 256 / Bm, rows = (256 + cols - 1) / cols;
        R.mip_atlas[m].assign((size_t)rows * Bm * 256, 0);
        for (int t = 0; t < 256; ++t) {
            uint8_t *dst = R.mip_atlas[m].data() + (size_t)(t / cols) * Bm * 256 + (size_t)(t % cols) * Bm;
            R.mip_tex[m][t] = dst;
            const uint8_t *src = g_texture_table[t];
            if (!src) continue;
            const int f = 1 << m;
            for (int y = 0; y < Bm; ++y)
                for (int x = 0; x < Bm; ++x) {
                    int sr = 0, sg = 0, sb = 0;
                    for (int yy = 0; yy < f; ++yy)
                        for (int xx = 0; xx < f; ++xx) {
                            const uint8_t c = src[(size_t)(y * f + yy) * 256 + x * f + xx];
                            sr += R.pal[c * 3]; sg += R.pal[c * 3 + 1]; sb += R.pal[c * 3 + 2];
                        }
                    const int n = f * f;
                    dst[(size_t)y * 256 + x] = nearest_colour((sr + n / 2) / n, (sg + n / 2) / n, (sb + n / 2) / n);
                }
        }
    }
    for (int t = 0; t < 256; ++t) {
        const uint8_t *src = g_texture_table[t];
        int sr = 0, sg = 0, sb = 0;
        if (src)
            for (int y = 0; y < B; ++y)
                for (int x = 0; x < B; ++x) {
                    const uint8_t c = src[(size_t)y * 256 + x];
                    sr += R.pal[c * 3]; sg += R.pal[c * 3 + 1]; sb += R.pal[c * 3 + 2];
                }
        const int n = B * B;
        R.avg_rgb[t][0] = (uint8_t)((sr + n / 2) / n);
        R.avg_rgb[t][1] = (uint8_t)((sg + n / 2) / n);
        R.avg_rgb[t][2] = (uint8_t)((sb + n / 2) / n);
        R.avg[t] = nearest_colour(R.avg_rgb[t][0], R.avg_rgb[t][1], R.avg_rgb[t][2]);
    }
}

inline uint8_t tex_prop(const uint8_t *table, unsigned t) { return t < 164 ? table[t] : 0; }

// ---------------------------------------------------------------------------------------------
// Frame state
// ---------------------------------------------------------------------------------------------

constexpr double kNear = 32.0;              // camera-space near plane (world units; 1/8 cell)
// The window of cell offsets around the camera: -kWin..kWin-1, at most one copy of the N-cell torus
// (kWin = min(N / 2, 256), set per frame from g_world: 128 in the original's 256 world, 256 from N = 512 on).
constexpr int    kWinMax = 256;
int              kWin = 128;
int              kVN = 2 * 128 + 1;         // vertex offsets -kWin..kWin

struct Frame {
    int W, H;
    double focal, horizon, scx, scy, cr, sr;     // projection, screen centre, roll cos / sin
    double cy, sy;                               // yaw cos / sin
    int cam_x, cam_y, cam_z, cam_cx, cam_cy, fx, fy;
    uint16_t yaw16;
    int q;                                       // view quadrant (0 looks -y, 1 +x, 2 +y, 3 -x)
    double cull2, near2, far2;                   // squared distances (world units)
    double radius;                               // draw radius (world units)
    bool darken, haze, second, shadows;
    int lod, levels;                             // levels = finest..coarsest LOD level count - 1
    int K;                                       // Chebyshev radius of each level's region (next-level cells)
    double tan_h, sec_h;                         // horizontal frustum (half-diagonal incl. roll)
    int32_t phase;
    int min_px;
};
Frame F;

struct EV {                                  // vertex cache entry
    uint32_t stamp;
    float xc, zc;
    int32_t h, hm;                           // height - cam_z (incl. wave), mirrored height
    int32_t shade, hz;
    int32_t sx, sy;                          // projection of (xc, h, zc) when zc >= kNear
    uint8_t proj;
};
std::vector<EV> g_vc;
uint32_t g_stamp = 0;

std::vector<ExtTri> g_tris;
std::vector<ExtSprite> g_sprites;
std::vector<uint32_t> g_order;               // bit 31 = sprite
constexpr uint32_t kSpriteBit = 0x80000000u;

inline void project(double xc, double h, double zc, int32_t &sx, int32_t &sy) {
    const double X = xc * F.focal / zc, Y = h * F.focal / zc + F.horizon;
    const double px = F.scx + (F.cr * X - F.sr * Y), py = F.scy - (Y * F.cr + X * F.sr);
    sx = (int32_t)std::floor(std::clamp(px, -1.0e8, 1.0e8) + 0.5);
    sy = (int32_t)std::floor(std::clamp(py, -1.0e8, 1.0e8) + 0.5);
}

double fog_haze(double d2) {
    if (d2 <= F.near2) return 0.0;
    if (d2 >= F.far2) return 1.0;
    return (d2 - F.near2) / (F.far2 - F.near2);
}

// Vertex at cell-corner offset (i, j) from the camera cell (-kWin..kWin).
const EV &vertex(int i, int j) {
    EV &e = g_vc[(size_t)(j + kWin) * kVN + (i + kWin)];
    if (e.stamp == g_stamp) return e;
    e.stamp = g_stamp;
    // cx * 0x80 below only matters mod 16 cells, so the wave is the same in every 256-cell tile
    const int cx = world_wrap_cell(F.cam_cx + i), cyc = world_wrap_cell(F.cam_cy + j);
    const uint32_t cell = world_cell(cx, cyc);
    const double dx = (double)(i * 256 - F.fx), dy = (double)(F.fy - j * 256);
    const double xc = F.cy * dx - F.sy * dy, zc = F.cy * dy + F.sy * dx;
    e.xc = (float)xc; e.zc = (float)zc;
    const int32_t height = (int32_t)g_map_height[cell];
    int32_t wave = (mc_sin((unsigned)(F.phase + cx * 0x80) & 0x7ff) >> 8) *
                   (mc_sin((unsigned)(cyc * 0x80 + F.phase) & 0x7ff) >> 8);
    e.hm = -F.cam_z - ((((wave >> 4) + 0x8000) * height) >> 10);
    e.h = height * 0x20 - F.cam_z;
    if (g_map_flags[cell] & 8) e.h -= wave >> 10; else wave = 0;
    int32_t shade = wave * 8 + (((int32_t)g_map_light[cell] * 0x100 + 0x80) << 8);
    const double d2 = xc * xc + zc * zc;
    if (F.darken) {
        if (d2 > F.near2) shade = d2 < F.far2 ? (int32_t)((F.far2 - d2) * (double)shade / (F.far2 - F.near2)) : 0;
        e.hz = 0;
    } else {
        e.hz = (int32_t)(fog_haze(d2) * 65536.0);
    }
    e.shade = shade;
    e.proj = zc >= kNear;
    if (e.proj) project(xc, e.h, zc, e.sx, e.sy);
    return e;
}

// ---------------------------------------------------------------------------------------------
// Triangle emission with near-plane clipping
// ---------------------------------------------------------------------------------------------

struct CV {                                   // camera-space vertex with attributes
    double xc, zc, h;
    int32_t u, v, s, hz;
    int32_t sx, sy;
    bool proj;
};

inline CV cv_from(const EV &e, int32_t u, int32_t v, bool mirrored) {
    CV c;
    c.xc = e.xc; c.zc = e.zc; c.h = mirrored ? e.hm : e.h;
    c.u = u; c.v = v; c.s = e.shade; c.hz = e.hz;
    c.proj = !mirrored && e.proj;
    c.sx = e.sx; c.sy = e.sy;
    return c;
}

void push_tri(const CV &a, const CV &b, const CV &c, uint8_t mode, uint8_t colour, const uint8_t *tex, int32_t uv_max) {
    ExtTri t;
    const CV *p[3] = { &a, &b, &c };
    int ymin = 1 << 30, ymax = -(1 << 30);
    uint8_t hz = 0;
    for (int k = 0; k < 3; ++k) {
        ExtVtx &v = t.v[k];
        if (p[k]->proj) { v.x = p[k]->sx; v.y = p[k]->sy; }
        else project(p[k]->xc, p[k]->h, p[k]->zc, v.x, v.y);
        v.u = p[k]->u; v.v = p[k]->v; v.s = p[k]->s; v.hz = p[k]->hz;
        if (v.hz > 0) hz = 1;
        ymin = std::min(ymin, v.y); ymax = std::max(ymax, v.y);
    }
    if (ymax <= 0 || ymin >= F.H) return;
    // cheap reject: entirely left / right of the target, or back facing
    const int64_t area = (int64_t)(t.v[1].x - t.v[0].x) * (t.v[2].y - t.v[0].y) -
                         (int64_t)(t.v[2].x - t.v[0].x) * (t.v[1].y - t.v[0].y);
    if (area <= 0) return;
    if (std::max({t.v[0].x, t.v[1].x, t.v[2].x}) < 0 || std::min({t.v[0].x, t.v[1].x, t.v[2].x}) >= F.W) return;
    t.tex = tex; t.uv_max = uv_max; t.mode = mode; t.colour = colour; t.haze = hz;
    t.ymin = (int16_t)std::clamp(ymin, 0, F.H);
    t.ymax = (int16_t)std::clamp(ymax, 0, F.H);
    g_order.push_back((uint32_t)g_tris.size());
    g_tris.push_back(t);
    g_render_ext_stats.triangles++;
}

CV lerp_cv(const CV &a, const CV &b, double t) {
    CV c;
    c.xc = a.xc + (b.xc - a.xc) * t; c.zc = a.zc + (b.zc - a.zc) * t; c.h = a.h + (b.h - a.h) * t;
    c.u = (int32_t)(a.u + (b.u - a.u) * t); c.v = (int32_t)(a.v + (b.v - a.v) * t);
    c.s = (int32_t)(a.s + (double)(b.s - a.s) * t); c.hz = (int32_t)(a.hz + (double)(b.hz - a.hz) * t);
    c.proj = false; c.sx = c.sy = 0;
    return c;
}

void emit_tri(const CV &a, const CV &b, const CV &c, uint8_t mode, uint8_t colour, const uint8_t *tex, int32_t uv_max) {
    const bool ia = a.zc >= kNear, ib = b.zc >= kNear, ic = c.zc >= kNear;
    if (ia && ib && ic) { push_tri(a, b, c, mode, colour, tex, uv_max); return; }
    if (!ia && !ib && !ic) return;
    // Sutherland-Hodgman against z >= kNear (keeps the winding)
    const CV in[3] = { a, b, c };
    CV out[4];
    int n = 0;
    for (int k = 0; k < 3; ++k) {
        const CV &P = in[k], &Q = in[(k + 1) % 3];
        const bool pin = P.zc >= kNear, qin = Q.zc >= kNear;
        if (pin) out[n++] = P;
        if (pin != qin) out[n++] = lerp_cv(P, Q, (kNear - P.zc) / (Q.zc - P.zc));
    }
    for (int k = 1; k + 1 < n; ++k) push_tri(out[0], out[k], out[k + 1], mode, colour, tex, uv_max);
}

// ---------------------------------------------------------------------------------------------
// Terrain cells
// ---------------------------------------------------------------------------------------------

// floor division for possibly negative values
inline int fdiv(int a, int s) { return a >= 0 ? a / s : -((-a + s - 1) / s); }

// Is the fine cell at offset (i, j) covered by the levels <= L (L < F.levels: the Chebyshev square of
// level-(L+1) cells around the camera's level-(L+1) cell; L >= F.levels: everything)?
inline bool covered(int L, int i, int j) {
    if (L >= F.levels) return true;
    if (L < 0) return false;
    const int s1 = 1 << (L + 1);
    const int I = fdiv(F.cam_cx + i, s1) - fdiv(F.cam_cx, s1);
    const int J = fdiv(F.cam_cy + j, s1) - fdiv(F.cam_cy, s1);
    return std::abs(I) <= F.K && std::abs(J) <= F.K;
}

// Horizontal frustum + circle test of the square [i0, i0 + s] x [j0, j0 + s] (cell offsets), with an
// extra margin in world units.
bool cell_visible(int i0, int j0, int s, double margin) {
    const double half = s * 128.0;
    const double dx = (i0 * 256 + half) - F.fx, dy = F.fy - (j0 * 256 + half);
    const double xc = F.cy * dx - F.sy * dy, zc = F.cy * dy + F.sy * dx;
    const double rad = half * 1.41422 + margin;
    if (zc + rad < kNear) return false;
    if ((xc - zc * F.tan_h) > rad * F.sec_h) return false;
    if ((-xc - zc * F.tan_h) > rad * F.sec_h) return false;
    return true;
}

// Nearest point of the cell square to the camera inside the draw circle?
bool cell_in_circle(int i0, int j0, int s) {
    const double x0 = i0 * 256.0 - F.fx, x1 = x0 + s * 256.0;
    const double y0 = j0 * 256.0 - F.fy, y1 = y0 + s * 256.0;
    const double nx = x0 > 0 ? x0 : (x1 < 0 ? x1 : 0.0), ny = y0 > 0 ? y0 : (y1 < 0 ? y1 : 0.0);
    return nx * nx + ny * ny < F.cull2;
}

int mip_for(double dist, int s) {
    if (F.lod == 0) return 0;
    const Resources &R = res();
    if (s > 1) return R.mips;                                    // merged quads: flat colour
    if (dist < 20 * 256.0) return 0;                             // the original's range keeps the full texture
    // pixels per cell; a mip level is used once more than 2 texels (lod 1) / 1 texel (lod 2) fall on a pixel
    const double ppc = F.focal * 256.0 / dist * (F.lod >= 2 ? 1.0 : 2.0);
    int m = 0;
    while (m < R.mips && (double)(R.B >> m) > ppc) m++;
    return m;
}

void count_cells(int i0, int j0, int s) {
    if (!g_render_ext_cell_count) return;
    for (int j = 0; j < s; ++j)
        for (int i = 0; i < s; ++i) {
            uint8_t &c = g_render_ext_cell_count[world_cell(F.cam_cx + i0 + i, F.cam_cy + j0 + j)];
            if (c < 255) c++;
        }
}

// The quad of the cell square at offset (i0, j0), size s.
void emit_quad(int i0, int j0, int s, bool mirrored) {
    const Resources &R = res();
    const EV &e00 = vertex(i0, j0), &e10 = vertex(i0 + s, j0), &e11 = vertex(i0 + s, j0 + s), &e01 = vertex(i0, j0 + s);
    const int cx = world_wrap_cell(F.cam_cx + i0), cyc = world_wrap_cell(F.cam_cy + j0);
    const uint32_t cell = world_cell(cx, cyc);
    const uint8_t texture = g_map_type[cell];
    if (mirrored && texture == 0) return;
    const double dmx = (i0 * 256.0 + s * 128.0) - F.fx, dmy = (j0 * 256.0 + s * 128.0) - F.fy;
    const int m = mip_for(std::sqrt(dmx * dmx + dmy * dmy), s);
    uint8_t mode = EXT_FILL_TEX_GOURAUD, colour = 0;
    const uint8_t *tex = R.mip_tex[m][texture];
    int32_t uvmax = (int32_t)(((uint32_t)(R.B >> m) << 16) - 1u);
    if (m >= R.mips) {
        mode = EXT_FILL_FLAT_SHADED;
        if (s == 1) colour = R.avg[texture];
        else {
            int sr = 0, sg = 0, sb = 0;
            for (int j = 0; j < s; ++j)
                for (int i = 0; i < s; ++i) {
                    const uint8_t t = g_map_type[world_cell(cx + i, cyc + j)];
                    sr += R.avg_rgb[t][0]; sg += R.avg_rgb[t][1]; sb += R.avg_rgb[t][2];
                }
            const int n = s * s;
            colour = nearest_colour((sr + n / 2) / n, (sg + n / 2) / n, (sb + n / 2) / n);
        }
    }
    if (!mirrored && F.second && mode != EXT_FILL_FLAT_SHADED) {
        if (tex_prop(g_tex_prop_water, texture) && m == 0) mode = EXT_FILL_WATER;
        else if (tex_prop(g_tex_prop_flat, texture)) {
            mode = EXT_FILL_TEX_FLATLIT;
            colour = (uint8_t)(((int64_t)e00.shade + e10.shade + e11.shade + e01.shade) >> 18);
        }
    }
    // UV corners of view quadrant 0: c00 -> uv[0..1], c10 -> [2..3], c11 -> [4..5], c01 -> [6..7]
    const unsigned sel = ((g_map_flags[cell] >> 2) & 0x1c);
    const int32_t *uv = g_uv_table + sel * 8;
    auto uvs = [&](int k) -> int32_t { return uv[k] ? uvmax : 0; };
    const CV a = cv_from(e00, uvs(0), uvs(1), mirrored), b = cv_from(e10, uvs(2), uvs(3), mirrored);
    const CV c = cv_from(e11, uvs(4), uvs(5), mirrored), d = cv_from(e01, uvs(6), uvs(7), mirrored);
    const bool par = s == 1 ? (((cx + cyc) & 1) != 0) : (((fdiv(F.cam_cx + i0, s) + fdiv(F.cam_cy + j0, s)) & 1) != 0);
    if (!mirrored) {
        if (!par) { emit_tri(a, b, c, mode, colour, tex, uvmax); emit_tri(a, c, d, mode, colour, tex, uvmax); }
        else      { emit_tri(a, b, d, mode, colour, tex, uvmax); emit_tri(d, b, c, mode, colour, tex, uvmax); }
    } else {                                   // reflection: opposite winding, mode 5 (as the exe)
        if (mode != EXT_FILL_FLAT_SHADED) mode = EXT_FILL_TEX_GOURAUD;
        if (!par) { emit_tri(a, c, b, mode, colour, tex, uvmax); emit_tri(a, d, c, mode, colour, tex, uvmax); }
        else      { emit_tri(a, d, b, mode, colour, tex, uvmax); emit_tri(d, c, b, mode, colour, tex, uvmax); }
    }
    g_render_ext_stats.quads++;
    if (!mirrored) count_cells(i0, j0, s);
}

// A skirt below the edge (ia, ja) - (ib, jb) (vertex offsets) down to `bottom` (height - cam_z),
// both windings, flat colour of the cell.
void emit_skirt(int ia, int ja, int ib, int jb, int32_t bottom, uint8_t colour) {
    const EV &ea = vertex(ia, ja), &eb = vertex(ib, jb);
    if (std::min(ea.h, eb.h) <= bottom) {
        if (ea.h <= bottom && eb.h <= bottom) return;
    }
    CV a = cv_from(ea, 0, 0, false), b = cv_from(eb, 0, 0, false);
    CV a2 = a, b2 = b;
    a2.h = bottom; b2.h = bottom; a2.proj = b2.proj = false;
    emit_tri(a, b, b2, EXT_FILL_FLAT_SHADED, colour, nullptr, 0);
    emit_tri(a, b2, a2, EXT_FILL_FLAT_SHADED, colour, nullptr, 0);
    emit_tri(a, b2, b, EXT_FILL_FLAT_SHADED, colour, nullptr, 0);
    emit_tri(a, a2, b2, EXT_FILL_FLAT_SHADED, colour, nullptr, 0);
    g_render_ext_stats.skirts++;
}

// ---------------------------------------------------------------------------------------------
// Things
// ---------------------------------------------------------------------------------------------

struct SpritePick {
    const uint8_t *pix;
    int32_t w, h;
    bool mirror, upright;
    int32_t mode_lit, mode_fog;
};

// thing_select_sprite of render_things.cpp (render_cell_things_2c600's draw-type switch).
bool pick_sprite(const Thing *t, const SpriteDesc *d, bool shadow, SpritePick &out) {
    const unsigned base = d->base_sprite;
    const unsigned dir = (unsigned)((((int32_t)t->yaw - (int32_t)F.yaw16) >> 3) & 0xf0) >> 4;
    unsigned n;
    bool mirror = false, upright = false;
    const unsigned dt = d->draw_type;
    if (dt > 0x24) return false;
    switch (dt) {
    case 0: case 1: n = base; break;
    case 0x15: n = base; upright = !shadow; break;
    case 0x11: if (dir < 8) n = base + dir; else { n = base + 0xf - dir; mirror = true; } break;
    case 0x12: n = base + dir; break;
    case 0x13: n = base + g_sprite_dir_remap_13[dir]; mirror = dir >= 8; break;
    case 0x14: n = base + g_sprite_dir_remap_14[dir]; mirror = dir >= 8; break;
    default:
        if (dt >= 0x16) upright = true;
        n = base + t->frame;
        break;
    }
    if (n >= MC_SPRITE_COUNT) return false;
    if (!g_sprite_ptr[n] && !sprite_ensure_loaded(n)) return false;
    g_sprite_group_stamp[sprite_group_of(n)] = g_cfg->tick;
    uint8_t *s = g_sprite_ptr[n];
    const int32_t w = (int32_t)((unsigned)s[2] | ((unsigned)s[3] << 8));
    const int32_t h = (int32_t)((unsigned)s[4] | ((unsigned)s[5] << 8));
    if (h == 0 || w == 0) return false;
    const size_t bytes = sprite_loaded_bytes(n);
    if (bytes < 6 + (size_t)w * (size_t)h) return false;
    s[0] |= 8;                                               // "drawn" (texture_anim_update animates it)
    out.pix = s + 6; out.w = w; out.h = h; out.mirror = mirror; out.upright = upright;
    const unsigned sg = d->shade_group < 6 ? d->shade_group : 0;
    out.mode_lit = g_sprite_mode_lit[sg];
    out.mode_fog = g_sprite_mode_fogged[sg];
    return true;
}

// Builds the sprite command. (X, Y) = projected anchor; anchor 1 = bottom centre, 0 / 2 = top centre
// upside down (shadow, reflection); w / h on-screen size.
void push_sprite(const SpritePick &p, double X, double Y, double w, double h, unsigned anchor, int32_t mode,
                 int32_t shade, uint16_t hz) {
    ExtSprite s;
    s.pix = p.pix; s.sw = p.w; s.sh = p.h; s.mode = mode; s.shade = shade; s.hz = hz;
    s.upright = p.upright ? 1 : 0;
    double rx, ry, dx, dy, ox, oy;                    // right / down unit vectors on screen, top-left
    if (p.upright) {
        rx = 1; ry = 0; dx = 0; dy = 1;
        const double q = (w + h) / 4.0;
        if (anchor == 1) { ox = X - F.sr * q - q; oy = Y - F.cr * q - q; }
        else if (anchor == 2) { ox = X + F.sr * q - q; oy = Y + F.cr * q - q; }
        else { ox = X; oy = Y; }
    } else {
        rx = F.cr; ry = -F.sr; dx = F.sr; dy = F.cr;
        if (anchor == 1) { ox = X - h * dx - w * 0.5 * rx; oy = Y - h * dy - w * 0.5 * ry; }
        else             { ox = X - w * 0.5 * rx;         oy = Y - w * 0.5 * ry; }
    }
    double ax = rx * w / p.w, ay = ry * w / p.w, bx = dx * h / p.h, by = dy * h / p.h;
    if (p.mirror) { ox += rx * w; oy += ry * w; ax = -ax; ay = -ay; }
    if (anchor != 1 && !p.upright) { ox += dx * h; oy += dy * h; bx = -bx; by = -by; }
    s.ox = (float)ox; s.oy = (float)oy; s.ax = (float)ax; s.ay = (float)ay; s.bx = (float)bx; s.by = (float)by;
    // bounding box
    const double xs[4] = { ox, ox + ax * p.w, ox + bx * p.h, ox + ax * p.w + bx * p.h };
    const double ys[4] = { oy, oy + ay * p.w, oy + by * p.h, oy + ay * p.w + by * p.h };
    const double x0 = std::min({xs[0], xs[1], xs[2], xs[3]}), x1 = std::max({xs[0], xs[1], xs[2], xs[3]});
    const double y0 = std::min({ys[0], ys[1], ys[2], ys[3]}), y1 = std::max({ys[0], ys[1], ys[2], ys[3]});
    if (x1 < 0 || x0 >= F.W || y1 < 0 || y0 >= F.H) return;
    s.ymin = (int16_t)std::clamp((int)std::floor(y0), 0, F.H);
    s.ymax = (int16_t)std::clamp((int)std::ceil(y1) + 1, 0, F.H);
    if (s.ymin >= s.ymax) return;
    g_order.push_back(kSpriteBit | (uint32_t)g_sprites.size());
    g_sprites.push_back(s);
    g_render_ext_stats.sprites++;
}

int32_t thing_shade_level(double d2) {             // DAT_000b5818 with the extended fog
    if (!F.darken || d2 <= F.near2) return 0x2000;
    if (d2 >= F.far2) return 0;
    return ((int32_t)((F.far2 - d2) * 32.0 / (F.far2 - F.near2))) << 8;
}

// Thing position with the render-time interpolation (render.h g_render_interp). prev_pos holds the low 16
// bits of the previous tick's x / y (pacing.cpp): the 16-bit difference to the current position is the
// tick's movement in any world (a Thing moves far less than 128 cells per tick), so the previous position is
// the current one minus that difference; with 256 cells this is exactly prev + d * alpha in 16 bits.
void thing_pos_lerp(unsigned idx, const Thing *t, int32_t &x, int32_t &y, int32_t &z) {
    x = t->x; y = t->y; z = t->z;
    const RenderInterp &ri = g_render_interp;
    if (ri.active && ri.prev_pos && (int)idx < ri.prev_count) {
        const int16_t *p = ri.prev_pos[idx];
        const int64_t a = ri.alpha;
        const int32_t dx = (int16_t)(uint16_t)(t->x - (uint16_t)p[0]), dy = (int16_t)(uint16_t)(t->y - (uint16_t)p[1]);
        x = world_wrap(t->x - dx + (int32_t)(((int64_t)dx * a) >> 16));
        y = world_wrap(t->y - dy + (int32_t)(((int64_t)dy * a) >> 16));
        z = (int32_t)p[2] + (int32_t)(((int64_t)((int32_t)t->z - p[2]) * a) >> 16);
    }
}

// Body segments of dragons / worms (render-only, not in the original). The original wakes a creature only
// within 24 cells of the local player (creature_proximity_wake_timer_46960); asleep, its segments (state
// 0x78) do not trail the head but snap onto their parent every 4th tick (creature_segment_update_18050), so
// the body collapses into one jumping stack - invisible behind the original's 20-cell fog, plain to see with
// the far view. Awake, the head's wake timer runs out every 17th tick: for that one tick every segment counts
// as asleep and a quarter of them snap onto their parent (the original's game state really has that one-tick
// kink; it showed as a flicker). The extended renderer therefore draws every segment follow-the-leader,
// awake or asleep: `speed` units from its (drawn) parent, in the direction of where it was drawn last frame -
// the rule the awake simulation itself applies once per tick. A new chain starts from the interpolated
// simulation position when that sits at a plausible distance, else straight behind the parent's heading.
// The simulation is untouched.
struct SegView { uint32_t frame; uint16_t parent; int32_t x, y, z; };
std::vector<SegView> g_seg_view;
uint32_t g_seg_frame = 0;                                  // ++ per described frame (render_view_ext)

inline bool is_segment(const Thing *t) { return t->cls == 5 && t->state == 0x78; }

void thing_pos(unsigned idx, const Thing *t, int32_t &x, int32_t &y, int32_t &z, int depth = 0) {
    if (!is_segment(t) || depth > 40 || t->parent == 0 || (int)t->parent >= thing_pool_slots()) {
        thing_pos_lerp(idx, t, x, y, z);
        return;
    }
    if (g_seg_view.size() < (size_t)thing_pool_slots()) g_seg_view.resize((size_t)thing_pool_slots(), SegView{0, 0, 0, 0, 0});
    SegView &v = g_seg_view[idx];
    if (v.frame == g_seg_frame && v.parent == t->parent) { x = v.x; y = v.y; z = v.z; return; }
    const Thing *par = thing_at(t->parent);
    int32_t px, py, pz;
    thing_pos(t->parent, par, px, py, pz, depth + 1);
    const double spacing = (double)(uint16_t)t->speed;
    double dx = 0, dy = 0, dz = 0, len = 0;
    // continue from last frame's position unless this is a new chain / the gap is implausible
    const bool fresh = !(v.parent == t->parent && g_seg_frame - v.frame <= 2);
    if (!fresh) {
        dx = world_signed(v.x - px); dy = world_signed(v.y - py); dz = v.z - pz;
        len = std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    if (fresh || len < 1.0 || len > spacing * 8.0 + 512.0) {
        int32_t sx, sy, sz;                                 // the simulation's (interpolated) position
        thing_pos_lerp(idx, t, sx, sy, sz);
        dx = world_signed(sx - px); dy = world_signed(sy - py); dz = sz - pz;
        len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (len < spacing * 0.5 || len > spacing * 2.0 + 64.0) {
            // lay the segment out behind its parent's heading (yaw 0 = -y, 0x200 = +x)
            const double a = (double)(par->yaw & 0x7ff) * (6.283185307179586 / 2048.0);
            dx = -std::sin(a); dy = std::cos(a); dz = 0; len = 1.0;
        }
    }
    const double k = spacing / len;
    x = world_wrap(px + (int32_t)std::lround(dx * k));
    y = world_wrap(py + (int32_t)std::lround(dy * k));
    z = pz + (int32_t)std::lround(dz * k);
    v = SegView{g_seg_frame, t->parent, x, y, z};
}

bool g_ext_anchors = false;                                // render_anchors_wanted() for the frame being built

// The things of the cell at offset (i, j); `shadow_ok` = the host quad's texture allows shadows.
void emit_cell_things(int i, int j, bool mirrored) {
    const uint32_t cell = world_cell(F.cam_cx + i, F.cam_cy + j);
    unsigned idx = g_cell_things[cell];
    if (idx == 0) return;
    const bool shadow_ok = F.shadows && tex_prop(g_tex_prop_shadow, g_map_type[cell]) == 0;
    const int slots = thing_pool_slots();
    int budget = slots;                                          // bounded walk (cell-list cycles)
    while (idx != 0 && idx < (unsigned)slots && budget-- > 0) {
        const Thing *t = thing_at(idx);
        const unsigned this_idx = idx;
        idx = t->cell_next;
        if (t->flags & 0x21) continue;
        g_render_ext_stats.things_seen++;
        int32_t x, y, z;
        thing_pos(this_idx, t, x, y, z);
        const double dx = world_signed(x - F.cam_x), dy = world_signed(F.cam_y - y);
        const double xc = F.cy * dx - F.sy * dy, zc = F.cy * dy + F.sy * dx;
        const double d2 = xc * xc + zc * zc;
        if (!(zc > 0x40 && d2 < F.cull2)) continue;
        // A mana ball's sprite (owner colour, size) is only refreshed while it is awake, near the local
        // player; far away it would keep the colour of a dead wizard after its hoard was claimed.
        const int sprite_id = (t->cls == 10 && t->type == 0x27) ? mana_ball_sprite(t) : (int)(int16_t)t->sprite;
        const SpriteDesc *d = mc_sprite_desc((unsigned)sprite_id);
        const int32_t fog = thing_shade_level(d2);
        const uint16_t hz = F.haze ? (uint16_t)std::min(65535.0, fog_haze(d2) * 65536.0) : 0;
        if (hz >= 65535) continue;
        const double size = (double)(uint16_t)d->half_z * F.focal / zc;
        if (size + 1.0 < F.min_px) continue;
        if (!mirrored && shadow_ok && d->shade_group == 0 && size >= 4.0) {
            const int32_t ground = terrain_sample_height(x, y);
            SpritePick p;
            if (!pick_sprite(t, d, true, p)) continue;
            const double h = std::floor(size) / 4.0, w = std::floor(size) * p.w / p.h;
            double X, Y; int32_t ix, iy;
            project(xc, ground - F.cam_z, zc, ix, iy); X = ix; Y = iy;
            if (h >= 1.0 && w >= 1.0) push_sprite(p, X, Y, w, std::floor(h), 0, 8, (fog >> 2) + 0x2000, hz);
        }
        SpritePick p;
        if (!pick_sprite(t, d, false, p)) continue;
        const double h = std::floor(size) + 1.0, w = std::floor(std::floor(size) * p.w / p.h) + 1.0;
        int32_t ix, iy;
        project(xc, mirrored ? (double)(-z - F.cam_z) : (double)(z - F.cam_z), zc, ix, iy);
        const int32_t mode = fog == 0x2000 ? p.mode_lit : p.mode_fog;
        push_sprite(p, ix, iy, w, h, mirrored ? 2u : 1u, mode, fog, hz);
        g_render_ext_stats.things_drawn++;
        if (!mirrored && g_ext_anchors) {                   // round 10 (task C): projected anchor, render-only
            RenderAnchor a;
            a.slot = (uint16_t)this_idx;
            a.generation = thing_slot_generation(this_idx);
            a.sx = ix; a.sy = iy; a.w = (int32_t)w; a.h = (int32_t)h; a.depth = (int32_t)std::lround(zc);
            a.x = x; a.y = y; a.z = z;
            render_anchor_push(a);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Traversal
// ---------------------------------------------------------------------------------------------

// Row / column offsets -> (i, j) cell offsets for the view quadrant: rows run along the quadrant's
// view axis (y for 0 / 2, x for 1 / 3).
inline void rc_to_ij(int r, int c, int &i, int &j) {
    if (F.q == 0 || F.q == 2) { j = r; i = c; } else { i = r; j = c; }
}

// Visits the level-L cells of the region (offsets in level-L cells relative to the camera's level-L
// cell) in painter's order: rows |r| from far to near, columns |c| from the outside in.
template <class Fn> void for_each_level_cell(int L, Fn &&fn) {
    const int s = 1 << L;
    const int ci = fdiv(F.cam_cx, s), cj = fdiv(F.cam_cy, s);
    // level-L cell index range covering fine offsets [-kWin, kWin - 1]
    const int imin = fdiv(F.cam_cx - kWin, s) - ci, imax = fdiv(F.cam_cx + kWin - 1, s) - ci;
    const int jmin = fdiv(F.cam_cy - kWin, s) - cj, jmax = fdiv(F.cam_cy + kWin - 1, s) - cj;
    int rmin, rmax, cmin, cmax;
    if (F.q == 0 || F.q == 2) { rmin = jmin; rmax = jmax; cmin = imin; cmax = imax; }
    else                      { rmin = imin; rmax = imax; cmin = jmin; cmax = jmax; }
    const int rm = std::max(-rmin, rmax), cm = std::max(-cmin, cmax);
    for (int ar = rm; ar >= 0; --ar) {
        for (int sr = 0; sr < (ar ? 2 : 1); ++sr) {
            const int r = sr ? -ar : ar;
            if (r < rmin || r > rmax) continue;
            for (int ac = cm; ac >= 0; --ac) {
                for (int sc = 0; sc < (ac ? 2 : 1); ++sc) {
                    const int c = sc ? -ac : ac;
                    if (c < cmin || c > cmax) continue;
                    int I, J;
                    rc_to_ij(r, c, I, J);
                    // fine offset of the cell's min corner
                    const int i0 = (ci + I) * s - F.cam_cx, j0 = (cj + J) * s - F.cam_cy;
                    fn(i0, j0, r, c);
                }
            }
        }
    }
}

inline bool in_window(int i0, int j0, int s) {
    return i0 >= -kWin && j0 >= -kWin && i0 + s - 1 <= kWin - 1 && j0 + s - 1 <= kWin - 1;
}

// The cell is drawn at level L.
inline bool at_level(int L, int i0, int j0) {
    return covered(L, i0, j0) && !covered(L - 1, i0, j0);
}

void draw_level(int L, bool mirrored) {
    const int s = 1 << L;
    const double thing_margin = 3 * 256.0;
    if (L > 0) {
        for_each_level_cell(L, [&](int i0, int j0, int, int) {
            if (!in_window(i0, j0, s) || !at_level(L, i0, j0) || !cell_in_circle(i0, j0, s)) return;
            const bool vis = cell_visible(i0, j0, s, 0.0);
            if (vis) {
                emit_quad(i0, j0, s, mirrored);
                if (!mirrored) {
                    // skirts towards the finer region (inner side): hang down to the lowest fine vertex
                    const int dirs[4][2] = { {-1, 0}, {1, 0}, {0, -1}, {0, 1} };
                    for (const auto &dd : dirs) {
                        const int ni = i0 + dd[0] * s, nj = j0 + dd[1] * s;
                        if (!covered(L - 1, ni, nj)) continue;
                        int ia, ja, ib, jb;
                        if (dd[0] == -1)     { ia = i0; ja = j0 + s; ib = i0; jb = j0; }
                        else if (dd[0] == 1) { ia = i0 + s; ja = j0; ib = i0 + s; jb = j0 + s; }
                        else if (dd[1] == -1){ ia = i0; ja = j0; ib = i0 + s; jb = j0; }
                        else                 { ia = i0 + s; ja = j0 + s; ib = i0; jb = j0 + s; }
                        int32_t lo = std::min(vertex(ia, ja).h, vertex(ib, jb).h);
                        for (int k = 1; k < s; ++k) {
                            const int vi = ia + (ib - ia) * k / s, vj = ja + (jb - ja) * k / s;
                            lo = std::min(lo, vertex(vi, vj).h);
                        }
                        emit_skirt(ia, ja, ib, jb, lo - 0x10, res().avg[g_map_type[world_cell(F.cam_cx + i0, F.cam_cy + j0)]]);
                    }
                }
            }
            if (cell_visible(i0, j0, s, thing_margin))
                for (int j = 0; j < s; ++j)
                    for (int i = 0; i < s; ++i) emit_cell_things(i0 + i, j0 + j, mirrored);
        });
        return;
    }
    // Level 0: every cell of the region, things of the cell one row farther hosted after the quad.
    const bool outer = F.levels > 0;
    for_each_level_cell(0, [&](int i0, int j0, int r, int c) {
        if (!in_window(i0, j0, 1) || !covered(0, i0, j0)) return;
        const bool inside = cell_in_circle(i0, j0, 1);
        if (inside && cell_visible(i0, j0, 1, 0.0)) {
            if (outer && !mirrored) {
                // skirts on the outer boundary of the level-0 region (towards level 1): down to below
                // the coarse edge through this fine edge
                const int dirs[4][2] = { {-1, 0}, {1, 0}, {0, -1}, {0, 1} };
                for (const auto &dd : dirs) {
                    if (covered(0, i0 + dd[0], j0 + dd[1])) continue;
                    int ia, ja, ib, jb;
                    if (dd[0] == -1)     { ia = i0; ja = j0 + 1; ib = i0; jb = j0; }
                    else if (dd[0] == 1) { ia = i0 + 1; ja = j0; ib = i0 + 1; jb = j0 + 1; }
                    else if (dd[1] == -1){ ia = i0; ja = j0; ib = i0 + 1; jb = j0; }
                    else                 { ia = i0 + 1; ja = j0 + 1; ib = i0; jb = j0 + 1; }
                    int32_t lo = std::min(vertex(ia, ja).h, vertex(ib, jb).h);
                    // the level-1 edge containing it: endpoints at even offsets (world-aligned)
                    const int ea_i = (ia == ib) ? ia : (fdiv(F.cam_cx + std::min(ia, ib), 2) * 2 - F.cam_cx);
                    const int ea_j = (ja == jb) ? ja : (fdiv(F.cam_cy + std::min(ja, jb), 2) * 2 - F.cam_cy);
                    const int eb_i = (ia == ib) ? ia : ea_i + 2, eb_j = (ja == jb) ? ja : ea_j + 2;
                    if (ea_i >= -kWin && eb_i <= kWin && ea_j >= -kWin && eb_j <= kWin)
                        lo = std::min({lo, vertex(ea_i, ea_j).h, vertex(eb_i, eb_j).h});
                    emit_skirt(ia, ja, ib, jb, lo - 0x10, res().avg[g_map_type[world_cell(F.cam_cx + i0, F.cam_cy + j0)]]);
                }
            }
            emit_quad(i0, j0, 1, mirrored);
        }
        // hosted things: the cell one row farther from the camera (both neighbours in the camera row)
        auto host = [&](int dr) {
            int di, dj;
            rc_to_ij(dr, 0, di, dj);
            const int ti = i0 + di, tj = j0 + dj;
            if (!in_window(ti, tj, 1) || !covered(0, ti, tj)) return;
            if (!cell_visible(ti, tj, 1, thing_margin)) return;
            emit_cell_things(ti, tj, mirrored);
        };
        (void)c;
        if (r > 0) host(1);
        else if (r < 0) host(-1);
        else { host(1); host(-1); host(0); }
    });
}

// ---------------------------------------------------------------------------------------------
// Slope low-pass (render_view_2f6e0's camera nudge), own state, advanced once per game tick
// ---------------------------------------------------------------------------------------------

int32_t s_slope_x = g_slope_smooth_default[0], s_slope_y = g_slope_smooth_default[1];
int32_t s_slope_px = g_slope_smooth_default[0], s_slope_py = g_slope_smooth_default[1];
uint32_t s_slope_tick = 0xffffffffu;
std::vector<uint8_t> s_prev_frame;                      // motion blur: the previous extended frame
int s_prev_w = 0, s_prev_h = 0;

void slope_camera(Camera &cam) {
    int cx = world_cx(cam.cam_x), cy = world_cx(cam.cam_y);
    if ((cam.cam_x & 0xff) < 0x80) cx--;
    if ((cam.cam_y & 0xff) < 0x80) cy--;
    if (s_slope_tick != g_anim_tick) {
        s_slope_tick = g_anim_tick;
        s_slope_px = s_slope_x; s_slope_py = s_slope_y;  // previous tick's nudge (interpolation)
        const int h00 = g_map_height[world_cell(cx, cy)];
        const int h20 = g_map_height[world_cell(cx + 2, cy)];
        const int h22 = g_map_height[world_cell(cx + 2, cy + 2)];
        const int h02 = g_map_height[world_cell(cx, cy + 2)];
        const int slope_x = std::clamp((h00 - h20 - h22 + h02) * 2, -100, 100);
        const int slope_y = std::clamp((h00 + h20 - h22 - h02) * 2, -100, 100);
        s_slope_x += (slope_x - s_slope_x) >> 3;
        s_slope_y += (slope_y - s_slope_y) >> 3;
    }
    int32_t ox = s_slope_x, oy = s_slope_y;
    if (g_render_interp.active) {                       // drawn between ticks: lerp the per-tick nudge
        const int64_t a = g_render_interp.alpha;
        ox = s_slope_px + (int32_t)(((int64_t)(s_slope_x - s_slope_px) * a) >> 16);
        oy = s_slope_py + (int32_t)(((int64_t)(s_slope_y - s_slope_py) * a) >> 16);
    }
    cam.cam_x += ox;
    cam.cam_y += oy;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// render_view_ext
// ---------------------------------------------------------------------------------------------

static void render_ext_draw(uint8_t *pix, int pitch, int width, int height, const Camera &cam_in) {
    if (!pix || width <= 0 || height <= 0 || pitch < width || !g_state) return;
    g_render_ext_stats = RenderExtStats{};
    ensure_resources();
    Camera cam = cam_in;
    slope_camera(cam);
    const PortSettings &S = g_settings;

    F.W = width; F.H = height;
    const double wref = F.H * 4.0 / 3.0;                         // the 4:3 width with the same vertical view
    F.focal = F.H * (800.0 / 480.0) * cam.zoom / 256.0;          // = isqrt(640^2 + 480^2) * zoom >> 8 at 480 rows
    F.horizon = cam.pitch * wref / 256.0;
    F.scx = F.W / 2; F.scy = F.H / 2;
    const unsigned roll = (unsigned)cam.roll & 0x7ff;
    F.sr = mc_sin(roll) / 65536.0; F.cr = mc_cos(roll) / 65536.0;
    const unsigned yaw = (unsigned)cam.yaw & 0x7ff;
    F.yaw16 = (uint16_t)yaw;
    F.cy = mc_cos(yaw) / 65536.0; F.sy = mc_sin(yaw) / 65536.0;
    F.q = (int)((yaw + 0x100) >> 9) & 3;
    // the window: at most one copy of the torus around the camera (128 cells each way in the 256 world)
    kWin = std::min(g_world.size / 2, kWinMax);
    kVN = 2 * kWin + 1;
    F.cam_x = world_wrap(cam.cam_x); F.cam_y = world_wrap(cam.cam_y); F.cam_z = cam.cam_z;
    F.cam_cx = F.cam_x >> 8; F.cam_cy = F.cam_y >> 8; F.fx = F.cam_x & 0xff; F.fy = F.cam_y & 0xff;
    // 4..127 in the 256 world (the settings' range); a wider world's window allows up to 255 (kWin - 1)
    const int dd = std::clamp(S.draw_distance, 4, kWin - 1);
    F.lod = std::clamp(S.lod, 0, 2);
    // LOD regions: level 0 up to D0 cells, level L up to D0 * 2^L; at most 3 coarser levels (8x8).
    F.levels = 0; F.K = 1;
    if (F.lod > 0) {
        const double D0 = F.focal / (F.lod >= 2 ? 6.0 : 3.0);
        while (F.levels < 3 && D0 * (1 << F.levels) < dd) F.levels++;
        F.K = std::max(2, (int)std::ceil(D0 / 2.0));
    }
    // A coarse cell must stay inside the one-copy window: pull the fog end in by the coarsest size.
    const int smax = 1 << F.levels;
    const double R = std::min((double)dd, (double)(kWin - smax));
    F.radius = R * 256.0;
    F.cull2 = F.radius * F.radius;
    const double fs = std::clamp(S.fog_start_pct, 0, 100) / 100.0;
    F.near2 = (F.radius * fs) * (F.radius * fs);
    F.far2 = (F.radius * 0.9375) * (F.radius * 0.9375);           // 0x1690000 / 0x1900000 = 0.9375^2
    if (F.far2 <= F.near2) F.far2 = F.near2 + 1.0;
    const bool textured = g_state->opt_textured_sky != 0;
    const int fm = S.fog_mode;
    F.haze = fm == 2 || (fm == 0 && textured);
    F.darken = !F.haze;
    if (F.haze) F.far2 = F.cull2 * 0.97;                           // fully hazed just inside the cull radius
    F.second = g_state->opt_second_surface != 0;
    F.shadows = g_state->opt_shadows != 0;
    ++g_seg_frame;                                          // sleeping-segment layout (thing_pos)
    F.phase = g_render_interp.active ? (int32_t)(((g_anim_tick - 1) << 6) + (g_render_interp.alpha * 64 >> 16))
                                     : (int32_t)(g_anim_tick << 6);   // lerped between ticks
    F.min_px = std::clamp(S.thing_min_px, 1, 16);
    const double half_diag = std::sqrt((double)F.W * F.W + (double)F.H * F.H) / 2.0 + std::fabs(F.horizon);
    F.tan_h = half_diag / F.focal;
    F.sec_h = std::sqrt(1.0 + F.tan_h * F.tan_h);

    // round 10 (task C): the projection export (render.h) - this view's camera, and the anchors if wanted
    {
        RenderViewInfo &v = *render_view_info_edit();
        v.view_w = F.W; v.view_h = F.H;
        v.cam_x = F.cam_x; v.cam_y = F.cam_y; v.cam_z = F.cam_z; v.yaw = F.yaw16;
        v.focal = F.focal; v.horizon = F.horizon; v.scx = F.scx; v.scy = F.scy;
        v.cr = F.cr; v.sr = F.sr; v.cyaw = F.cy; v.syaw = F.sy; v.cull2 = F.cull2;
        render_view_info_ext_drawn(pix);
        g_ext_anchors = render_anchors_wanted();
        if (g_ext_anchors) render_anchors_clear();
    }

    // sky (render_sky_2f080's mapping with 256 texels across the 4:3 width)
    ExtSky sky{};
    sky.textured = textured;
    sky.colour = 0xff;
    {
        const int32_t d_sin = (int32_t)((double)mc_sin(roll) * 256.0 / wref);
        const int32_t d_cos = (int32_t)((double)mc_cos(roll) * 256.0 / wref);
        const double px = F.scx - F.horizon * F.sr, py = F.scy - F.horizon * F.cr;
        sky.dux = d_cos; sky.dvx = d_sin; sky.duy = -d_sin; sky.dvy = d_cos;
        sky.u0 = (int32_t)((int64_t)((uint32_t)F.yaw16 << 15) - (int64_t)((double)d_cos * px - (double)d_sin * py));
        sky.v0 = (int32_t)(-(int64_t)(py * d_cos + px * d_sin));
    }

    const auto t_build = std::chrono::steady_clock::now();
    // display list
    g_stamp++;
    if (g_stamp == 0) { g_stamp = 1; for (EV &e : g_vc) e.stamp = 0; }
    if (g_vc.size() != (size_t)kVN * kVN) g_vc.assign((size_t)kVN * kVN, EV{});
    g_tris.clear(); g_sprites.clear(); g_order.clear();
    if (F.second && F.cam_z < 0x1000)
        for (int L = F.levels; L >= 0; --L) draw_level(L, true);
    for (int L = F.levels; L >= 0; --L) draw_level(L, false);
    g_render_ext_stats.levels = F.levels + 1;

    const auto t_raster = std::chrono::steady_clock::now();
    // rasterise in bands
    int threads = S.render_threads > 0 ? S.render_threads : (int)std::thread::hardware_concurrency();
    threads = std::clamp(threads, 1, 16);
    if (threads > 1) pool().ensure(threads - 1);
    const int nbands = threads > 1 ? std::min(F.H, threads * 3) : 1;
    const int rows_per = (F.H + nbands - 1) / nbands;
    const ExtTri *tris = g_tris.data();
    const ExtSprite *sprs = g_sprites.data();
    const uint32_t *order = g_order.data();
    const size_t ncmd = g_order.size();
    auto band_of = [&](int k) {
        ExtBand b{pix, pitch, width, height, k * rows_per, std::min(F.H, (k + 1) * rows_per)};
        return b;
    };
    const std::function<void(int)> raster_job = [&](int k) {
        const ExtBand b = band_of(k);
        if (b.y0 >= b.y1) return;
        ext_draw_sky(b, sky);
        for (size_t i = 0; i < ncmd; ++i) {
            const uint32_t o = order[i];
            if (o & kSpriteBit) ext_draw_sprite(b, sky, sprs[o & ~kSpriteBit]);
            else ext_fill_triangle(b, sky, tris[o]);
        }
    };
    if (threads > 1) pool().run(nbands, raster_job); else raster_job(0);

    const auto t_post = std::chrono::steady_clock::now();
    // post filters: 2x2 smoothing (render_view_2f6e0) and the motion-blur blend with the previous frame
    const uint8_t *blend = g_blend_table();
    if (g_state->opt_smooth != 0 && F.W > 1 && F.H > 1) {
        std::vector<uint8_t> edge((size_t)nbands * F.W);
        for (int k = 1; k < nbands; ++k) {                       // first row of every band, unfiltered
            const int y = k * rows_per;
            if (y < F.H) std::memcpy(edge.data() + (size_t)k * F.W, pix + (size_t)y * pitch, (size_t)F.W);
        }
        const std::function<void(int)> smooth_job = [&](int k) {
            const ExtBand b = band_of(k);
            for (int y = b.y0; y < b.y1 && y < F.H - 1; ++y) {
                uint8_t *row = pix + (size_t)y * pitch;
                const uint8_t *below = (y + 1 == b.y1) ? edge.data() + (size_t)(k + 1) * F.W : row + pitch;
                for (int x = 0; x < F.W - 1; ++x) {
                    const uint8_t a = blend[((unsigned)below[x] << 8) | row[x]];
                    const uint8_t c = blend[((unsigned)below[x + 1] << 8) | row[x + 1]];
                    row[x] = blend[((unsigned)c << 8) | a];
                }
            }
        };
        if (threads > 1) pool().run(nbands, smooth_job); else smooth_job(0);
    }
    if (g_state->opt_motion_blur != 0) {
        const size_t n = (size_t)F.W * F.H;
        if (s_prev_w == F.W && s_prev_h == F.H && s_prev_frame.size() == n) {
            const bool mode1 = g_state->opt_motion_blur == 1;
            const std::function<void(int)> blur_job = [&](int k) {
                const ExtBand b = band_of(k);
                for (int y = b.y0; y < b.y1; ++y) {
                    uint8_t *d = pix + (size_t)y * pitch;
                    const uint8_t *p = s_prev_frame.data() + (size_t)y * F.W;
                    if (mode1) for (int x = 0; x < F.W; ++x) d[x] = blend[((unsigned)p[x] << 8) | d[x]];
                    else       for (int x = 0; x < F.W; ++x) d[x] = blend[((unsigned)d[x] << 8) | p[x]];
                }
            };
            if (threads > 1) pool().run(nbands, blur_job); else blur_job(0);
        }
        s_prev_frame.resize(n);
        for (int y = 0; y < F.H; ++y) std::memcpy(s_prev_frame.data() + (size_t)y * F.W, pix + (size_t)y * pitch, (size_t)F.W);
        s_prev_w = F.W; s_prev_h = F.H;
    } else {
        s_prev_w = s_prev_h = 0;
    }
    g_render_ext_stats.threads = threads;
    const auto t_end = std::chrono::steady_clock::now();
    g_render_ext_stats.build_ms = std::chrono::duration<double, std::milli>(t_raster - t_build).count();
    g_render_ext_stats.raster_ms = std::chrono::duration<double, std::milli>(t_post - t_raster).count();
    g_render_ext_stats.post_ms = std::chrono::duration<double, std::milli>(t_end - t_post).count();
    g_render_ext_stats.bands = nbands;
    g_render_ext_stats.draw_radius = R;
}

void render_view_ext(const FrameBuffer &fb, const Camera &cam) {
    render_ext_draw(fb.pixels, fb.width, fb.width, fb.height, cam);
}

// Drop-in for render_view at the game frame's size: draws into the current render target (the view
// window render_set_view_window chose inside `fb`), e.g. g_render_view_override = render_view_ext_target.
void render_view_ext_target(const FrameBuffer &, const Camera &cam) {
    render_ext_draw(g_rt_dest, g_rt_pitch, g_rt_width, g_rt_height, cam);
}

bool render_ext_segment_pos(unsigned idx, int32_t *x, int32_t *y, int32_t *z) {
    if (idx >= g_seg_view.size() || g_seg_view[idx].frame != g_seg_frame) return false;
    *x = g_seg_view[idx].x; *y = g_seg_view[idx].y; *z = g_seg_view[idx].z;
    return true;
}
