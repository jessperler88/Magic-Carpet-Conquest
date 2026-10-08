// Pixel comparison of the port's renderer against the original game (task F of round 5).
//
// The reference is extracted/reference/movie0_fb/tick%05d.fb: what the retail carpet.exe (patched copy,
// tools/reference/fb/patch_carpet.py) drew while it played movie 0 in DOSBox - the 320x200 back buffer
// right before vga_present_frame_2f480, the DAC palette, Config[0..0xff], the sprite animation table and
// the renderer globals 0x93f40.. / 0xb5800.. after the render - every tick 413..1013 and every 10th tick
// up to 8960. The same run's state dumps (tick%05d.gam) are byte-identical to extracted/reference/movie0
// (fb_info.py --same-run), so the frames belong to the per-tick reference the port already matches.
// A second set, movie0_fbhud (patch_carpet.py --hud), has the flight HUD drawn during the movie as well
// (MC_RFB_HUD=1; every tick up to 2500).
//
// The test plays movie 0 in the port as reference_test does, plus what the reference run did before the
// first tick: `carpet -roll 1 -level 38` generates level 38 before the snapshot replaces it, which leaves
// the terrain RNG g_rng16 (DAT_0012dfb0, in neither the GameState nor the map dump) at 0x2fea; in-game
// retexturing (terrain_paint retexture_rect: rotation bits of the map flags) draws from it. Per tick it
// does what game_tick_update_32e80 does around the simulation: texture_anim_update before it, render_frame
// after it (render_frame_draw for the pixels, hud_tick_state for its game-state writes). At every tick
// with a frame dump it compares the pixels: count, first / last differing row, and a split by region taken
// from extra port renders of the same tick: "thing" = pixels the port's things cover (render without the
// cell-things hooks), "sky" = pixels no terrain triangle covers (render with the textured sky toggled),
// "hud" = pixels outside the 3D view window or (HUD set) changed by the flight HUD, "terrain" = the rest.
// Diff images (reference | port | diff, PPM) for the ticks listed in MC_RFB_TICKS go to the output dir.
// It also checks the state dumps, the terrain dumps (every 100th tick) and the carried-over renderer
// state below.
//
// Renderer state carried between frames, and how it is handled:
//   * render_view's slope low-pass (DAT_00093f7c / 80): starts from the data-segment defaults in both
//     (the original renders nothing before the first movie tick); compared after every render with the
//     dump's post-render value. MC_RFB_SEED=1 instead seeds it once from the first dump.
//   * the sprite animation table (texture_anim_update; frames advance only for sprites drawn in the
//     previous frame): compared per tick (active records, next frame number) with the dump.
//   * the previous frame in the back buffer (motion-blur path) - never used in the movie
//     (Config.pentium == 0); the frame buffer is still kept across ticks as in the original.
//   * render_frame's screen-clear memory (hud.cpp s_last_clear): the frame of the tick is drawn first.
//   * the HUD's credits roll (Config.credits_state) - 0 throughout movie 0; compared with the dump.
//   * the render options GameState+0x2195..+0x21b8 are the running game's settings, not snapshot data:
//     copied from the reference run's first state dump.
//
// argv: [1] game dir  [2] frame dump dir (default MC_REFERENCE_FB_DIR)  [3] last tick  [4] output dir
//       for the diff images (default: current directory)  [5] state dump dir (default movie0, or the
//       frame dump dir with MC_RFB_HUD)
// Env: MC_RFB_TICKS=t,t,.. (diff images of these ticks), MC_RFB_IMAGES=n (how many worst ticks to list),
//      MC_RFB_SEED=1, MC_RFB_VERBOSE=1 (a line per compared tick), MC_RFB_NOGENLEVEL=1 (skip the
//      level-38 generation), MC_RFB_HUD=1, MC_RFB_MAPDIFF=1 (list differing map-flag cells),
//      MC_RFB_LENIENT=1 (differing frames do not fail the test).
// Exit: 0 when the dumps are missing (SKIP) or every frame is identical; 1 when the simulation diverged
// from the state dumps or (unless MC_RFB_LENIENT) a frame differs. Since round 5 every dumped frame of
// both sets is pixel-identical, so this is a regression gate like reference_test.
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "engine.h"
#include "sim.h"
#include "thing.h"
#include "player.h"
#include "demo.h"
#include "hud.h"
#include "render.h"
#include "sprites.h"
#include "raster.h"
#include "ui_draw.h"
#include "projectiles.h"
#include "mc_globals.h"
#include "mc_math.h"
#include "mcfile.h"
#include "crash_handler.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef MC_REFERENCE_FB_DIR
#define MC_REFERENCE_FB_DIR MC_REPO_DIR "/extracted/reference/movie0_fb"
#endif
#ifndef MC_REFERENCE_DIR
#define MC_REFERENCE_DIR MC_REPO_DIR "/extracted/reference/movie0"
#endif

// render_landscape.cpp (port accessors, requested for render.h)

static constexpr int W = 320, H = 200, NPIX = W * H;

struct FbDump {
    std::vector<uint8_t> fb, pal, cfg;
    int anim_count = 0;
    std::vector<uint8_t> anim;
    uint8_t r93f40[0x60] = {};
    uint8_t rb5800[0xc0] = {};
    uint32_t screen[3] = {};
};

static bool load_fb(const std::string &dir, int tick, FbDump *d) {
    char name[64];
    std::snprintf(name, sizeof name, "tick%05d.fb", tick);
    mc_blob b;
    if (!mc_read_file((dir + "/" + name).c_str(), &b)) return false;
    bool ok = b.len >= (size_t)NPIX + 768 + 256 + 6;
    if (ok) {
        const uint8_t *p = b.data;
        d->fb.assign(p, p + NPIX);
        d->pal.assign(p + NPIX, p + NPIX + 768);
        d->cfg.assign(p + NPIX + 768, p + NPIX + 1024);
        size_t o = NPIX + 1024;
        uint16_t cnt; std::memcpy(&cnt, p + o, 2);
        o += 6;
        d->anim_count = cnt;
        size_t an = (size_t)cnt * 0x1c;
        if (o + an + 0x60 + 0xc0 + 12 <= b.len) {
            d->anim.assign(p + o, p + o + an); o += an;
            std::memcpy(d->r93f40, p + o, 0x60); o += 0x60;
            std::memcpy(d->rb5800, p + o, 0xc0); o += 0xc0;
            std::memcpy(d->screen, p + o, 12);
        } else {
            d->anim_count = 0;
        }
    }
    mc_blob_free(&b);
    return ok;
}

static bool load_state(const std::string &dir, int tick, GameState *out) {
    char name[64];
    std::snprintf(name, sizeof name, "tick%05d.gam", tick);
    mc_blob b;
    if (!mc_read_file((dir + "/" + name).c_str(), &b)) return false;
    bool ok = b.len == MC_STATE_IMAGE_SIZE;
    if (ok) ok = thing_load_snapshot_image(b.data, out);
    mc_blob_free(&b);
    return ok;
}

// Things (without the list links), players and the RNG equal?
static bool state_equal(const GameState &ref) {
    if (ref.rng != g_state->rng) return false;
    if (std::memcmp(ref.players, g_state->players, sizeof ref.players) != 0) return false;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        if (ref.things[i].cls == 0 && g_state->things[i].cls == 0) continue;   // free in both (as reference_test)
        const uint8_t *a = reinterpret_cast<const uint8_t *>(&ref.things[i]);
        const uint8_t *b = reinterpret_cast<const uint8_t *>(&g_state->things[i]);
        if (std::memcmp(a + 4, b + 4, 0x10) != 0 || std::memcmp(a + 0x18, b + 0x18, sizeof(Thing) - 0x18) != 0)
            return false;
    }
    return true;
}

// The four maps against the state run's terrain dump (tick%05d.map, every 100th tick): cells that differ
// per map (type, height, light, flags); -1 when there is no dump.
static bool compare_maps(const std::string &dir, int tick, int out[4]) {
    char name[64];
    std::snprintf(name, sizeof name, "tick%05d.map", tick);
    mc_blob b;
    if (!mc_read_file((dir + "/" + name).c_str(), &b)) return false;
    out[0] = out[1] = out[2] = out[3] = 0;
    if (b.len >= 0x40000) {
        const uint8_t *maps[4] = { g_map_type, g_map_height, g_map_light, g_map_flags };
        for (int m = 0; m < 4; m++)
            for (int i = 0; i < MC_MAP_CELLS; i++) out[m] += b.data[m * 0x10000 + i] != maps[m][i];
        if (std::getenv("MC_RFB_MAPDIFF") && out[3]) {
            int shown = 0;
            for (int i = 0; i < MC_MAP_CELLS && shown < 12; i++)
                if (b.data[0x30000 + i] != g_map_flags[i]) {
                    std::printf("    flags cell (%d,%d): ref %02x port %02x type %d height %d\n", i & 255, i >> 8,
                                b.data[0x30000 + i], g_map_flags[i], g_map_type[i], g_map_height[i]);
                    shown++;
                }
        }
    }
    mc_blob_free(&b);
    return true;
}

static int32_t rd32(const uint8_t *p) { int32_t v; std::memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

// Diff image: reference | port | diff (differing pixels red over a dimmed reference) through the
// reference's DAC palette.
static void write_diff_ppm(const std::string &path, const uint8_t *ref, const uint8_t *port, const uint8_t *pal) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", W * 3 + 8, H);
    std::vector<uint8_t> row((size_t)(W * 3 + 8) * 3);
    auto put = [&](int x, int c, uint8_t *o) {
        o[x * 3 + 0] = (uint8_t)((pal[c * 3 + 0] & 63) * 255 / 63);
        o[x * 3 + 1] = (uint8_t)((pal[c * 3 + 1] & 63) * 255 / 63);
        o[x * 3 + 2] = (uint8_t)((pal[c * 3 + 2] & 63) * 255 / 63);
    };
    for (int y = 0; y < H; y++) {
        std::fill(row.begin(), row.end(), (uint8_t)0x40);
        for (int x = 0; x < W; x++) {
            const int i = y * W + x;
            put(x, ref[i], row.data());
            put(W + 4 + x, port[i], row.data());
            uint8_t *o = row.data() + (2 * W + 8 + x) * 3;
            if (ref[i] != port[i]) { o[0] = 255; o[1] = 0; o[2] = 0; }
            else { put(0, ref[i], o); o[0] /= 3; o[1] /= 3; o[2] /= 3; }
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
}

struct TickResult {
    int tick = 0, diff = 0, sky = 0, terrain = 0, thing = 0, hud = 0, sky_n = 0, terrain_n = 0, thing_n = 0, hud_n = 0;
    int first_row = -1, last_row = -1;
};

int main(int argc, char **argv) {
    mc_install_crash_handler();
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char *game_dir = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    const std::string fb_dir = argc > 2 ? argv[2] : MC_REFERENCE_FB_DIR;
    const int last_tick = argc > 3 ? std::atoi(argv[3]) : 1 << 30;
    const std::string out_dir = argc > 4 ? argv[4] : ".";
    // MC_RFB_HUD=1: the reference run was made with fb/patch_carpet.py --hud (flight HUD drawn during the
    // movie); the port draws it too (Config.flags bit 2 hidden from render_frame) and its state is compared
    // with that run's own state dumps (the HUD's state writes make them differ from movie0).
    const bool hud_mode = std::getenv("MC_RFB_HUD") != nullptr;
    const std::string state_dir = argc > 5 ? argv[5] : hud_mode ? fb_dir : std::string(MC_REFERENCE_DIR);
    const int n_images = std::getenv("MC_RFB_IMAGES") ? std::atoi(std::getenv("MC_RFB_IMAGES")) : 4;
    const bool seed = std::getenv("MC_RFB_SEED") != nullptr;
    const bool verbose = std::getenv("MC_RFB_VERBOSE") != nullptr;
    const bool strict = std::getenv("MC_RFB_LENIENT") == nullptr;
    std::vector<int> want_ticks;
    if (const char *t = std::getenv("MC_RFB_TICKS"))
        for (const char *p = t; *p;) { want_ticks.push_back(std::atoi(p)); while (*p && *p != ',') p++; if (*p) p++; }

    static GameState ref;
    static FbDump first;
    if (!load_state(state_dir, 413, &ref) || !load_fb(fb_dir, 413, &first)) {
        std::printf("SKIP: reference %s/tick00413.fb or %s/tick00413.gam missing; run tools/reference/fb/run_reference.py\n",
                    fb_dir.c_str(), state_dir.c_str());
        return 0;
    }
    if (!engine_init(game_dir)) { std::printf("engine_init failed\n"); return 2; }
    g_hook_frame_state = nullptr;          // render_frame's state writes are done here, after the comparison
    g_projectile_null_hit_index = (uint16_t)((int32_t)(0u - g_snapshot_things_base) / (int32_t)sizeof(ThingImage));
    g_cfg->flags = 0; g_cfg->paused = 0;
    sim_prepare_movie();
    if (!std::getenv("MC_RFB_NOGENLEVEL")) {
        // The reference run (`carpet -roll 1 -level 38`) generates level 38 before the first tick loads the
        // snapshot: the terrain RNG g_rng16 (DAT_0012dfb0, not part of the GameState / map dump) keeps the
        // value the generator left, and in-game retexturing draws from it.
        sim_load_level(38);
        std::printf("g_rng16 after generating level 38: 0x%04x\n", (unsigned)g_rng16);
    }
    ui_draw_set_video_mode(game_dir);      // 320x200: the mspr0-0 HUD set (as mcport does after setting mode 1)
    if (!demo_open(game_dir, 0)) { std::printf("movie 0 missing\n"); return 2; }

    std::vector<uint8_t> pix(NPIX, 0), pix_nothing(NPIX), pix_sky(NPIX), pix_nohud(NPIX);
    const FrameBuffer fb{pix.data(), W, H};
    const FrameBuffer fb_b{pix_nothing.data(), W, H}, fb_c{pix_sky.data(), W, H}, fb_d{pix_nohud.data(), W, H};

    auto with_hud = [&](auto fn) {
        const uint16_t saved = g_cfg->flags;
        if (hud_mode) g_cfg->flags = (uint16_t)(saved & ~4);
        fn();
        g_cfg->flags = saved;
    };
    std::vector<TickResult> results;
    int compared = 0, identical = 0, state_bad = 0, first_state_bad = 0, maps_bad = 0, first_maps_bad = 0;
    int slope_bad = 0, first_slope_bad = 0, anim_bad = 0, first_anim_bad = 0, pal_same = 0, cfg_credit_bad = 0;
    int first_diff_tick = 0;
    long long sum_diff = 0, sum_sky = 0, sum_terrain = 0, sum_thing = 0, sum_hud = 0, n_sky = 0, n_terrain = 0, n_thing = 0, n_hud = 0;
    bool seeded = false, more = true;
    FbDump d;
    for (int steps = 0; more && steps < 20000; steps++) {
        if (steps > 0) with_hud([&] { hud_tick_state(g_state->local_player); });   // render_frame's writes of the previous tick
        if (!(g_cfg->paused & 1)) texture_anim_update();         // game_tick_update_32e80 order
        more = demo_step();
        if (steps == 0) {
            // The render options +0x2195..+0x21b8 are the running game's settings, not part of the
            // snapshot (demo_load_state_3c200 keeps them): take the reference run's (DOSBox, Config.pentium
            // 0: no textured sky, no second surface; shadows and both HUD parts on).
            std::memcpy(&g_state->opt_second_surface, &ref.opt_second_surface, 0x21b9 - 0x2195);
        }
        const int local = g_state->local_player & 7;
        const int tick = (int)g_state->players[local].tick;
        g_anim_tick = (uint32_t)tick;
        if (tick > last_tick) break;
        const bool have = load_fb(fb_dir, tick, &d);
        int mapd[4];
        if (tick % 100 == 0 && compare_maps(state_dir, tick, mapd) && (mapd[0] | mapd[1] | mapd[2] | mapd[3])) {
            maps_bad++;
            if (!first_maps_bad) {
                first_maps_bad = tick;
                std::printf("  maps %d: cells differ: type %d height %d light %d flags %d\n", tick, mapd[0], mapd[1], mapd[2], mapd[3]);
            }
        }
        if (have && load_state(state_dir, tick, &ref) && !state_equal(ref)) {
            state_bad++;
            if (!first_state_bad) {
                first_state_bad = tick;
                if (ref.rng != g_state->rng) std::printf("  state %d: rng differs\n", tick);
                for (int p = 0; p < 8; p++) {
                    const uint8_t *a = reinterpret_cast<const uint8_t *>(&ref.players[p]);
                    const uint8_t *b = reinterpret_cast<const uint8_t *>(&g_state->players[p]);
                    for (int k = 0; k < (int)sizeof(PlayerRec); k++)
                        if (a[k] != b[k]) { std::printf("  state %d: player %d +%#x %02x/%02x\n", tick, p, k, a[k], b[k]); break; }
                }
                for (int i = 1; i < MC_THING_SLOTS; i++) {
                    if (ref.things[i].cls == 0 && g_state->things[i].cls == 0) continue;
                    const uint8_t *a = reinterpret_cast<const uint8_t *>(&ref.things[i]);
                    const uint8_t *b = reinterpret_cast<const uint8_t *>(&g_state->things[i]);
                    for (int k = 4; k < (int)sizeof(Thing); k++)
                        if ((k < 0x14 || k >= 0x18) && a[k] != b[k]) {
                            std::printf("  state %d: thing %d (cls %d type %d) +%#x %02x/%02x\n", tick, i, a[0x40], a[0x41], k, a[k], b[k]);
                            break;
                        }
                }
            }
        }

        // The frame of this tick first (render_frame keeps screen-clear state between calls), then two
        // extra renders for the region masks (things hooks off; textured sky toggled), with the slope
        // low-pass state put back so they use the same camera and leave the carried state alone.
        int32_t sx0, sy0, sx1, sy1;
        render_get_slope_state(&sx0, &sy0);
        with_hud([&] { render_frame_draw(fb, local); });
        render_get_slope_state(&sx1, &sy1);
        const int view_off = (int)(g_rt_dest - pix.data());
        const int vx0 = view_off % W, vy0 = view_off / W, vx1 = vx0 + g_rt_width, vy1 = vy0 + g_rt_height;
        if (!have) continue;
        {
            CellThingsFn h1 = g_render_cell_things, h2 = g_render_cell_things_mirrored;
            g_render_cell_things = nullptr; g_render_cell_things_mirrored = nullptr;
            render_set_slope_state(sx0, sy0);
            with_hud([&] { render_frame_draw(fb_b, local); });
            const uint8_t sky_opt = g_state->opt_textured_sky;
            g_state->opt_textured_sky = sky_opt ? 0 : 1;
            render_set_slope_state(sx0, sy0);
            with_hud([&] { render_frame_draw(fb_c, local); });
            g_state->opt_textured_sky = sky_opt;
            g_render_cell_things = h1; g_render_cell_things_mirrored = h2;
            if (hud_mode) {                                      // the same frame without the flight HUD
                render_set_slope_state(sx0, sy0);
                render_frame_draw(fb_d, local);
            }
            render_set_slope_state(sx1, sy1);
        }
        compared++;

        TickResult r;
        r.tick = tick;
        for (int i = 0; i < NPIX; i++) {
            const int px = i % W, py = i / W;
            const bool is_hud = px < vx0 || px >= vx1 || py < vy0 || py >= vy1 || (hud_mode && pix[i] != pix_nohud[i]);
            const bool is_thing = !is_hud && pix[i] != pix_nothing[i];
            const bool is_sky = !is_thing && (pix_nothing[i] != pix_sky[i] || pix_nothing[i] == 0xff);
            const bool dif = pix[i] != d.fb[i];
            if (is_hud) { r.hud_n++; r.hud += dif; }
            else if (is_thing) { r.thing_n++; r.thing += dif; }
            else if (is_sky) { r.sky_n++; r.sky += dif; }
            else { r.terrain_n++; r.terrain += dif; }
            if (dif) {
                r.diff++;
                if (r.first_row < 0) r.first_row = i / W;
                r.last_row = i / W;
            }
        }
        if (r.diff == 0) identical++;
        else if (!first_diff_tick) first_diff_tick = tick;
        sum_diff += r.diff; sum_sky += r.sky; sum_terrain += r.terrain; sum_thing += r.thing; sum_hud += r.hud;
        n_sky += r.sky_n; n_terrain += r.terrain_n; n_thing += r.thing_n; n_hud += r.hud_n;
        results.push_back(r);

        // Carried-over renderer state after this render vs the dump (post-render values).
        int32_t sx, sy;
        render_get_slope_state(&sx, &sy);
        const int32_t rsx = rd32(d.r93f40 + 0x3c), rsy = rd32(d.r93f40 + 0x40);
        const bool slope_ok = sx == rsx && sy == rsy;
        if (!slope_ok) { slope_bad++; if (!first_slope_bad) first_slope_bad = tick; }
        if (seed && !seeded) {
            render_set_slope_state(rsx, rsy);                    // state of the next frame
            seeded = true;
        }
        int anim_diff = 0;
        for (int k = 0; k < d.anim_count; k++) {
            const uint8_t *a = d.anim.data() + k * 0x1c;
            if (rd32(a + 4) == 0) continue;                      // free record
            const unsigned spr = rd16(a + 0x1a);
            unsigned frame = 0;
            if (!sprite_anim_info(spr, &frame, nullptr, nullptr) || frame != rd16(a + 0x16)) {
                if (anim_diff == 0 && verbose)
                    std::printf("  anim %d: sprite %u ref frame %u port %u\n", tick, spr, rd16(a + 0x16), frame);
                anim_diff++;
            }
        }
        if (anim_diff) { anim_bad++; if (!first_anim_bad) first_anim_bad = tick; }
        bool pal_eq = true;
        for (int k = 0; k < 768; k++) if ((d.pal[k] & 63) != (g_palette6[k] & 63)) { pal_eq = false; break; }
        pal_same += pal_eq;
        if (std::memcmp(d.cfg.data() + 0xa1, g_cfg->credits_state, 7) != 0) cfg_credit_bad++;

        if (verbose || compared <= 5 || tick % 500 == 0)
            std::printf("%5d diff %5d (sky %d/%d terrain %d/%d thing %d/%d hud %d/%d) rows %d..%d slope %s anim %d pal %s\n", tick,
                        r.diff, r.sky, r.sky_n, r.terrain, r.terrain_n, r.thing, r.thing_n, r.hud, r.hud_n, r.first_row, r.last_row,
                        slope_ok ? "=" : "x", anim_diff, pal_eq ? "=" : "x");
        if (std::find(want_ticks.begin(), want_ticks.end(), tick) != want_ticks.end()) {
            char name[64];
            std::snprintf(name, sizeof name, "/rfb_%05d.ppm", tick);
            write_diff_ppm(out_dir + name, d.fb.data(), pix.data(), d.pal.data());
            std::snprintf(name, sizeof name, "/rfb_%05d.raw", tick);   // reference then port, palette indices
            if (FILE *f = std::fopen((out_dir + name).c_str(), "wb")) {
                std::fwrite(d.fb.data(), 1, NPIX, f);
                std::fwrite(pix.data(), 1, NPIX, f);
                std::fclose(f);
            }
        }
    }
    demo_close();

    std::printf("\ncompared %d frames: %d identical, first differing tick %d, mean %.1f differing pixels of %d\n",
                compared, identical, first_diff_tick, compared ? (double)sum_diff / compared : 0.0, NPIX);
    auto pct = [](long long bad, long long n) { return n ? 100.0 * (double)(n - bad) / (double)n : 100.0; };
    std::printf("match by region (port classification): sky %.3f%% (%lld px), terrain %.3f%% (%lld px), things %.3f%% (%lld px), hud %.3f%% (%lld px), all %.3f%%\n",
                pct(sum_sky, n_sky), n_sky, pct(sum_terrain, n_terrain), n_terrain, pct(sum_thing, n_thing), n_thing, pct(sum_hud, n_hud), n_hud,
                pct(sum_diff, (long long)compared * NPIX));
    std::printf("state dumps: %d ticks differ from the port's simulation (first %d)\n", state_bad, first_state_bad);
    std::printf("terrain dumps: %d differ from the port's maps (first %d)\n", maps_bad, first_maps_bad);
    std::printf("slope low-pass after render: %d ticks differ (first %d)%s\n", slope_bad, first_slope_bad,
                seed ? "" : " (not seeded)");
    std::printf("sprite animation records: %d ticks differ (first %d)\n", anim_bad, first_anim_bad);
    std::printf("DAC palette = data/palette.dat in %d of %d frames; credits state differs in %d frames\n", pal_same,
                compared, cfg_credit_bad);
    // Worst ticks.
    std::vector<TickResult> worst = results;
    std::sort(worst.begin(), worst.end(), [](const TickResult &a, const TickResult &b) { return a.diff > b.diff; });
    std::printf("worst ticks:");
    for (size_t i = 0; i < worst.size() && i < 10; i++) std::printf(" %d(%d)", worst[i].tick, worst[i].diff);
    std::printf("\n");
    // Histogram.
    int h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;
    for (const TickResult &r : results) {
        if (r.diff == 0) h0++; else if (r.diff < 10) h1++; else if (r.diff < 100) h2++; else if (r.diff < 1000) h3++; else h4++;
    }
    std::printf("frames by differing pixels: 0: %d, 1-9: %d, 10-99: %d, 100-999: %d, >=1000: %d\n", h0, h1, h2, h3, h4);
    // Re-render the worst ticks as images: replay is cheap, but the frames are gone - write them on a
    // second pass over the movie only when asked (MC_RFB_TICKS); here list the ticks to ask for.
    if (n_images > 0 && !worst.empty() && want_ticks.empty()) {
        std::printf("diff images: MC_RFB_TICKS=");
        for (int i = 0; i < n_images && i < (int)worst.size(); i++) std::printf("%s%d", i ? "," : "", worst[i].tick);
        std::printf("\n");
    }
    if (state_bad) { std::printf("FAIL: the simulation diverged from the state dumps\n"); return 1; }
    if (strict && identical != compared) { std::printf("FAIL: %d frames differ\n", compared - identical); return 1; }
    std::printf("%s: %d / %d frames pixel-identical\n", identical == compared ? "OK" : "DONE", identical, compared);
    return 0;
}
