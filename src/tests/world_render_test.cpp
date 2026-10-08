#define _CRT_SECURE_NO_WARNINGS
// world_render_test (Phase 4 round 11, task E): the renderers, the radar / map screen and the debug views in a
// world wider than 256 cells (docs/analysis/port_world_render.md).
//
// Level 38 is loaded and widened with world_widen_tiled(1024) (the 256 level repeated 4 x 4). The tiled world
// is the oracle: a camera at (x + k*65536, y + m*65536) must see exactly what the 256 world shows at (x, y).
//  1. Terrain only (the cell lists emptied): the extended renderer (lod 0 / 1, draw distance 100), the faithful
//     renderer (320x200) and the radar (round flight radar and the rectangular map screen) are pixel-identical
//     to the 256 frames for cameras inside tiles, straddling the old 255 -> 256 boundary and straddling the new
//     seam 1023 -> 0, in tiles (0,0) .. (3,3).
//  2. With the Things (which stay in tile (0,0)): the extended and faithful frames and the map screen's blips
//     at a camera in the middle of tile (0,0) equal the 256 ones.
//  3. The wide window: draw distance 200 (kWin = 256) straddling the seam - no map cell drawn twice, cells more
//     than 128 cells away drawn.
//  4. Projection export: the view's camera is the full position (tile 3); a point across the seam projects as
//     in the 256 world; the ground pick returns the cell of the right tile; a point 400 cells away is not in
//     front of the faithful view (no int32 overflow).
//  5. Interpolation (the low-16-bit prev_pos of pacing.cpp) across the seam: the follow camera of a Thing at
//     x = N*256 - 40 coming from x = 80.
//  6. Drawing changes no game state (net_state_checksum before / after).
// PPMs into argv[2] / MC_WRENDER_OUT (default: current directory). Exit 0 = pass; SKIP without game data.
#include "engine.h"
#include "sim.h"
#include "mc_globals.h"
#include "render.h"
#include "render_ext.h"
#include "raster.h"
#include "settings.h"
#include "thing.h"
#include "terrain.h"
#include "player.h"
#include "hud.h"
#include "ui_draw.h"
#include "net.h"
#include "world.h"
#include "debug_overlay.h"
#include "mcfile.h"
#include "crash_handler.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); g_fail++; } } while (0)

static std::string g_out = ".";

static void write_ppm(const std::string &name, const uint8_t *px, int w, int h) {
    const std::string path = g_out + "/" + name;
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    uint8_t rgb[768];
    mc_palette_to_rgb(g_palette6, rgb);
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::vector<uint8_t> row((size_t)w * 3);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) std::memcpy(&row[(size_t)x * 3], rgb + px[(size_t)y * w + x] * 3, 3);
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
}

using Img = std::vector<uint8_t>;

constexpr int EXT_W = 480, EXT_H = 300;

static Img render_ext_frame(const Camera &cam, int dd, int lod) {
    g_settings.render_extended = true;
    g_settings.draw_distance = dd;
    g_settings.lod = lod;
    g_settings.fog_mode = 0;
    Img px((size_t)EXT_W * EXT_H, 0);
    render_view_ext(FrameBuffer{px.data(), EXT_W, EXT_H}, cam);
    return px;
}

static Img render_faithful_frame(const Camera &cam, bool capture = false) {
    Img px((size_t)320 * 200, 0);
    FrameBuffer fb{px.data(), 320, 200};
    render_set_slope_state(0, 0);                    // the carried low-pass: the same start for every frame
    render_set_view_window(fb, 0x28);
    if (capture) render_capture_begin(fb, false);
    render_view_frame(fb, cam);
    if (capture) render_capture_end();
    return px;
}

// The radar into a 640x480 frame: the round flight radar (scale 0x100) or the map screen (0xaa, rectangular),
// optionally with the blips.
static Img render_radar(int32_t cam_x, int32_t cam_y, int yaw, bool map, bool blips) {
    Img px((size_t)640 * 480, 0x10);
    FrameBuffer fb{px.data(), 640, 480};
    ui_set_target(fb);
    const int w = map ? 0x17e : 0x80, h = map ? 0x19e : 0x80, scale = map ? 0xaa : 0x100, rect = map ? 1 : 0;
    ui_draw_radar(0, 0, cam_x, cam_y, w, h, yaw, scale, 0, rect);
    if (blips) ui_draw_radar_blips(0, 0, cam_x, cam_y, w, h, yaw, scale, 0, rect);
    return px;
}

static int diff_count(const Img &a, const Img &b) {
    if (a.size() != b.size()) return -1;
    int n = 0;
    for (size_t i = 0; i < a.size(); i++) n += a[i] != b[i];
    return n;
}

// The cell lists rebuilt in slot order (what world_widen_tiled does), so the 256 and the tiled world list the
// Things of a cell in the same order.
static void relink_in_slot_order() {
    std::memset(g_cell_things, 0, sizeof(uint16_t) * g_world.cells);
    for (int i = 1; i < thing_pool_slots(); i++) {
        Thing *t = thing_at((unsigned)i);
        if (!(t->flags & 4)) continue;
        t->flags &= ~4u;
        const Pos p = *thing_pos(t);
        thing_link_cell(t, &p);
    }
}

struct Case { const char *name; int32_t x, y; int yaw; };
static const Case kCases[] = {
    {"inside", 0x8037, 0x7a91, 0x123},
    {"seam_x", 0xfe80, 0x4000, 0x200},             // looking +x across x = 65535 -> 0 / 65536
    {"seam_y", 0x3000, 0x0100, 0x000},             // looking -y across y = 0
    {"corner", 0xff40, 0xff20, 0x500},             // both seams
};
constexpr int kNumCases = (int)(sizeof kCases / sizeof kCases[0]);
static const int kTiles[][2] = {{0, 0}, {1, 0}, {0, 1}, {2, 3}, {3, 3}, {3, 0}};
constexpr int kNumTiles = (int)(sizeof kTiles / sizeof kTiles[0]);

static Camera case_camera(const Case &c) {
    Camera cam{};
    cam.cam_x = c.x; cam.cam_y = c.y; cam.yaw = c.yaw;
    cam.cam_z = terrain_sample_height(c.x, c.y) + 0x300;
    cam.pitch = -24; cam.roll = 0; cam.zoom = 0x80;
    return cam;
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char *game_dir = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    if (argc > 2) g_out = argv[2];
    else if (const char *o = std::getenv("MC_WRENDER_OUT")) g_out = o;
    if (!engine_init(game_dir)) { std::printf("SKIP: engine_init failed (game data missing)\n"); return 0; }
    if (!ui_draw_init(game_dir)) { std::printf("SKIP: HUD data missing\n"); return 0; }
    if (!engine_load_level(38)) { std::printf("SKIP: level 38 missing\n"); return 0; }
    g_cfg->flags = 0; g_cfg->paused = 0;
    for (int t = 0; t < 50; t++) engine_tick();
    g_state->opt_motion_blur = 0;
    g_render_interp = RenderInterp{};
    const uint32_t anim_tick = g_anim_tick;          // constant: the wave phase and render_ext's slope low-pass
    const PortSettings defaults = g_settings;
    relink_in_slot_order();

    // ---- the 256 world -------------------------------------------------------------------------------------
    Camera cams[kNumCases];
    Img ext0[kNumCases], ext1[kNumCases], faith[kNumCases], radar[kNumCases], mapscr[kNumCases];
    for (int i = 0; i < kNumCases; i++) cams[i] = case_camera(kCases[i]);
    // with Things, camera in the middle of the level (tile (0,0) of the tiled world)
    const Img things_ext = render_ext_frame(cams[0], 100, 1);
    const Img things_faith = render_faithful_frame(cams[0]);
    // the flight radar's blips (64 cells around the camera). Not the map screen's: it reaches 127 cells, and a
    // Thing about 128 cells away is the one copy on the far side in the 256 world but has no copy there in the
    // tiled world (the Things live in tile (0,0) only) - a true difference, not an error.
    const Img things_map = render_radar(cams[0].cam_x, cams[0].cam_y, cams[0].yaw, false, true);
    write_ppm("w256_things_ext.ppm", things_ext.data(), EXT_W, EXT_H);
    // terrain only
    std::vector<uint16_t> lists(g_cell_things, g_cell_things + MC_MAP_CELLS);
    std::memset(g_cell_things, 0, sizeof(uint16_t) * MC_MAP_CELLS);
    for (int i = 0; i < kNumCases; i++) {
        ext0[i] = render_ext_frame(cams[i], 100, 0);
        ext1[i] = render_ext_frame(cams[i], 100, 1);
        faith[i] = render_faithful_frame(cams[i]);
        radar[i] = render_radar(cams[i].cam_x, cams[i].cam_y, cams[i].yaw, false, false);
        mapscr[i] = render_radar(cams[i].cam_x, cams[i].cam_y, cams[i].yaw, true, false);
    }
    write_ppm("w256_seam_x_ext.ppm", ext1[1].data(), EXT_W, EXT_H);
    // projection export across the seam (camera seam_x looking +x)
    int p256_sx = 0, p256_sy = 0, p256_d = 0, f256_sx = 0, f256_sy = 0, f256_d = 0, pick256_x = -1, pick256_y = -1;
    const int32_t probe_x = 3 * 256 + 0x40, probe_y = cams[1].cam_y;     // 4.5 cells past the seam
    const int32_t probe_z = terrain_sample_height(probe_x, probe_y);
    (void)render_ext_frame(cams[1], 100, 1);
    const bool p256 = render_project_world(probe_x, probe_y, probe_z, &p256_sx, &p256_sy, &p256_d);
    const bool pick256 = render_pick_ground(EXT_W / 2, EXT_H * 2 / 3, &pick256_x, &pick256_y);
    (void)render_faithful_frame(cams[1], true);
    const bool f256 = render_project_world(probe_x, probe_y, probe_z, &f256_sx, &f256_sy, &f256_d);
    CHECK(p256 && f256 && pick256, "256 world: probe across the seam not projected / no ground pick");
    std::memcpy(g_cell_things, lists.data(), sizeof(uint16_t) * MC_MAP_CELLS);

    // ---- the tiled 1024 world ------------------------------------------------------------------------------
    const int N = 1024;
    if (!world_widen_tiled(N)) { std::printf("FAIL: world_widen_tiled(%d)\n", N); return 1; }
    CHECK(g_world.size == N, "world size %d", g_world.size);
    g_anim_tick = anim_tick;
    const uint32_t sum0 = net_state_checksum();

    // 2. with the Things, tile (0,0)
    {
        const Img e = render_ext_frame(cams[0], 100, 1);
        const Img f = render_faithful_frame(cams[0]);
        const Img m = render_radar(cams[0].cam_x, cams[0].cam_y, cams[0].yaw, false, true);
        const Img mt = render_radar(cams[0].cam_x, cams[0].cam_y, cams[0].yaw, false, false);
        const int de = diff_count(e, things_ext), df = diff_count(f, things_faith), dm = diff_count(m, things_map);
        const int blip_px = diff_count(m, mt);
        std::printf("with Things, tile (0,0): ext %d / faithful %d / radar + blips %d pixels differ (%d blip pixels)\n", de, df,
                    dm, blip_px);
        CHECK(de == 0 && df == 0 && dm == 0, "frames with Things differ from the 256 world");
        CHECK(blip_px > 0, "no blips drawn");
    }

    // 1. terrain only, every case in every tile
    std::vector<uint16_t> wide_lists(g_cell_things, g_cell_things + g_world.cells);
    std::memset(g_cell_things, 0, sizeof(uint16_t) * g_world.cells);
    int frames = 0, bad = 0;
    for (int i = 0; i < kNumCases; i++)
        for (int k = 0; k < kNumTiles; k++) {
            Camera c = cams[i];
            c.cam_x += kTiles[k][0] * 0x10000;
            c.cam_y += kTiles[k][1] * 0x10000;
            const Img e0 = render_ext_frame(c, 100, 0), e1 = render_ext_frame(c, 100, 1);
            const Img f = render_faithful_frame(c);
            const Img r = render_radar(c.cam_x, c.cam_y, c.yaw, false, false);
            const Img m = render_radar(c.cam_x, c.cam_y, c.yaw, true, false);
            const int d[5] = {diff_count(e0, ext0[i]), diff_count(e1, ext1[i]), diff_count(f, faith[i]),
                              diff_count(r, radar[i]), diff_count(m, mapscr[i])};
            frames += 5;
            for (int q = 0; q < 5; q++) if (d[q] != 0) bad++;
            if (d[0] || d[1] || d[2] || d[3] || d[4])
                std::printf("FAIL: %s tile (%d,%d): ext lod0 %d, ext lod1 %d, faithful %d, radar %d, map %d pixels differ\n",
                            kCases[i].name, kTiles[k][0], kTiles[k][1], d[0], d[1], d[2], d[3], d[4]);
            if (i == 1 && kTiles[k][0] == 3 && kTiles[k][1] == 0) {
                write_ppm("w1024_seam_x_tile30_ext.ppm", e1.data(), EXT_W, EXT_H);
                write_ppm("w1024_seam_x_tile30_faithful.ppm", f.data(), 320, 200);
                write_ppm("w1024_seam_x_tile30_map.ppm", m.data(), 640, 480);
            }
        }
    std::printf("terrain only: %d frames (%d cameras x %d tiles x ext lod 0 / 1, faithful, radar, map), %d differ\n",
                frames, kNumCases, kNumTiles, bad);
    g_fail += bad;

    // 3. the wide window: draw distance 200 across the seam
    {
        std::vector<uint8_t> count(g_world.cells, 0);
        g_render_ext_cell_count = count.data();
        Camera c = cams[1];
        c.cam_x += 3 * 0x10000;
        c.cam_z += 0x800;
        const Img e = render_ext_frame(c, 200, 2);
        g_render_ext_cell_count = nullptr;
        int twice = 0, drawn = 0, beyond = 0;
        const int ccx = world_cx(c.cam_x), ccy = world_cx(c.cam_y);
        for (uint32_t cell = 0; cell < g_world.cells; cell++) {
            if (count[cell] == 0) continue;
            drawn++;
            if (count[cell] > 1) twice++;
            const int dx = std::abs(world_signed((world_cell_x(cell) - ccx) * 256) / 256);
            const int dy = std::abs(world_signed((world_cell_y(cell) - ccy) * 256) / 256);
            if (dx > 128 || dy > 128) beyond++;
        }
        std::printf("draw distance 200 across the seam: radius %.0f, %d cells drawn, %d beyond 128 cells, %d twice\n",
                    g_render_ext_stats.draw_radius, drawn, beyond, twice);
        CHECK(g_render_ext_stats.draw_radius == 200.0, "draw radius %.1f (want 200)", g_render_ext_stats.draw_radius);
        CHECK(twice == 0 && beyond > 1000, "wide window: %d cells twice, %d beyond 128 cells", twice, beyond);
        write_ppm("w1024_dd200_seam.ppm", e.data(), EXT_W, EXT_H);
    }

    // 4. projection export / picks in tile (3,0), across the seam N*256 -> 0
    {
        Camera c = cams[1];
        c.cam_x += 3 * 0x10000;
        (void)render_ext_frame(c, 100, 1);
        const RenderViewInfo &v = render_view_info();
        CHECK(v.cam_x >= 3 * 0x10000, "ext view camera %d not the full position", v.cam_x);
        int sx = 0, sy = 0, d = 0, px = -1, py = -1;
        const bool ok = render_project_world(probe_x, probe_y, probe_z, &sx, &sy, &d);
        CHECK(ok == p256 && sx == p256_sx && sy == p256_sy && d == p256_d, "ext projection across the seam: %d,%d,%d vs %d,%d,%d",
              sx, sy, d, p256_sx, p256_sy, p256_d);
        const bool pk = render_pick_ground(EXT_W / 2, EXT_H * 2 / 3, &px, &py);
        CHECK(pk && (px & 255) == pick256_x && py == pick256_y && std::abs(world_signed((px - world_cx(c.cam_x)) * 256)) < 64 * 256,
              "ext ground pick %d,%d (256 world %d,%d)", px, py, pick256_x, pick256_y);
        std::printf("ext view: camera %d (tile %d), probe across the seam at %d,%d, ground pick cell %d,%d\n", v.cam_x,
                    v.cam_x >> 16, sx, sy, px, py);
        (void)render_faithful_frame(c, true);
        const RenderViewInfo &vf = render_view_info();
        CHECK(vf.kind == RENDER_VIEW_FAITHFUL && vf.cam_x >= 3 * 0x10000, "faithful view camera %d not the full position", vf.cam_x);
        const bool okf = render_project_world(probe_x, probe_y, probe_z, &sx, &sy, &d);
        CHECK(okf == f256 && sx == f256_sx && sy == f256_sy && d == f256_d, "faithful projection across the seam");
        const bool far_ok = render_project_world(world_wrap(c.cam_x + 400 * 256), c.cam_y, 0, &sx, &sy, &d);
        CHECK(!far_ok, "a point 400 cells away projected by the faithful view");
        // ext: a point 400 cells ahead along +x is in front (no torus alias) but outside the draw radius
        (void)render_ext_frame(c, 100, 1);
        const bool far_ext = render_project_world(world_wrap(c.cam_x + 400 * 256), c.cam_y, 0, &sx, &sy, &d);
        CHECK(far_ext && d > 390 * 256, "ext: a point 400 cells ahead has depth %d", d);
    }

    // 5. interpolation across the seam (pacing.cpp's prev_pos holds the low 16 bits)
    {
        int slot = -1;
        for (int i = 1; i < thing_pool_slots() && slot < 0; i++)
            if (thing_at((unsigned)i)->cls == 5) slot = i;
        CHECK(slot > 0, "no creature");
        if (slot > 0) {
            Thing *t = thing_at((unsigned)slot);
            const int32_t sx = t->x, sy = t->y;
            debug_inspect_set(slot);
            std::vector<int16_t> prev((size_t)thing_pool_slots() * 3, 0);
            auto follow_at = [&](int32_t x, bool interp, uint32_t alpha, Camera *out) {
                t->x = x;
                prev[(size_t)slot * 3 + 0] = (int16_t)(uint16_t)80;
                prev[(size_t)slot * 3 + 1] = (int16_t)(uint16_t)(int32_t)t->y;
                prev[(size_t)slot * 3 + 2] = t->z;
                g_render_interp.active = interp;
                g_render_interp.alpha = alpha;
                g_render_interp.prev_pos = reinterpret_cast<const int16_t (*)[3]>(prev.data());
                g_render_interp.prev_count = thing_pool_slots();
                const bool r = debug_inspect_follow_camera(out);
                g_render_interp = RenderInterp{};
                return r;
            };
            Camera a{}, b{}, h{}, hb{};
            const int32_t at = N * 256 - 40;
            follow_at(at, true, 0, &a);               // alpha 0: the previous position, x = 80
            follow_at(80, false, 0, &b);
            follow_at(at, true, 0x8000, &h);          // halfway: x = 20 (40 units back to the seam, 80 past it)
            follow_at(20, false, 0, &hb);
            CHECK(a.cam_x == b.cam_x && a.cam_y == b.cam_y && a.cam_z == b.cam_z, "follow camera at alpha 0: %d vs %d", a.cam_x, b.cam_x);
            CHECK(h.cam_x == hb.cam_x && h.cam_z == hb.cam_z, "follow camera halfway: %d vs %d", h.cam_x, hb.cam_x);
            std::printf("interpolation across the seam: alpha 0 camera x %d (= %d), halfway %d (= %d)\n", a.cam_x, b.cam_x, h.cam_x, hb.cam_x);
            t->x = sx; t->y = sy;
            debug_inspect_set(-1);
        }
    }

    std::memcpy(g_cell_things, wide_lists.data(), sizeof(uint16_t) * g_world.cells);
    const uint32_t sum1 = net_state_checksum();
    CHECK(sum0 == sum1, "drawing changed the game state (%08x -> %08x)", sum0, sum1);
    g_settings = defaults;
    world_set_size(256);
    std::printf("%s\n", g_fail ? "FAILED" : "PASS");
    return g_fail ? 1 : 0;
}
