// Pixel comparison of the port's renderer against the original, round 6 (task D): 640x480, the render
// options, the help screen and the credits roll.
//
// The references are made like the round-5 ones (render_reference_test.cpp, docs/analysis/
// port_render_reference.md): a patched carpet.exe in DOSBox plays movie 0 and dumps the GameState and
// what it drew every tick (tools/reference/fb/patch_carpet.py, run_reference.py). New in round 6:
//   * `--hires`: config_parse stores video mode 8, so the movie plays in 640x480 from its first tick
//     (frames of width * height bytes: the dump's screen variables say which);
//   * `--schedule`: a poke schedule applied by the dump cave right after the render of a tick (before the
//     dump policy), e.g. "textured sky on from tick 700". The run's directory holds it as schedule.txt
//     ("raw_tick kind offset value" per byte; kind 0 GameState, 1 Config, 2 local PlayerRec; the last line
//     "# hires H hud H"). The test applies the same bytes at the same point: after render_frame's state
//     writes (hud_tick_state) of the tick, before the next tick's simulation.
//   * the flight HUD drawn during the movie (`--hud`) as in render_reference_hud.
//
// Per compared tick: differing pixels and the split by region (port classification as in the round-5 test),
// the state dump (Things without links, players, RNG), the render options of the dump against the port's,
// the slope low-pass, the sprite animation records, the DAC. At the end: the match per option segment
// (a segment = a run of ticks with the same render options / input mode / credits state / Config.pentium).
//
// argv: [1] game dir  [2] dump dir (default movie0_fb640)  [3] last tick  [4] output dir for diff images
// Env: MC_RFB_TICKS=t,.. (reference | port | diff PPM + raw index frames), MC_RFB_VERBOSE=1,
//      MC_RFB_LENIENT=1 (differing frames do not fail), MC_RFB_HUD=0/1 (override the schedule's hud flag).
// Exit: 0 = SKIP (no dumps) or every frame identical and the simulation equal to the state dumps.
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
#include "input.h"
#include "mc_globals.h"
#include "mc_math.h"
#include "mcfile.h"
#include "crash_handler.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#ifndef MC_REFERENCE_FB2_DIR
#define MC_REFERENCE_FB2_DIR MC_REPO_DIR "/extracted/reference/movie0_fb640"
#endif

struct FbDump {
    int w = 0, h = 0;
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
    bool ok = b.len >= 64000 + 1024 + 6 + 12;
    if (ok) {
        const uint8_t *p = b.data;
        std::memcpy(d->screen, p + b.len - 12, 12);     // width, back buffer pointer, height
        d->w = (int)d->screen[0];
        d->h = (int)d->screen[2];
        const size_t n = (size_t)d->w * (size_t)d->h;
        ok = (d->w == 320 || d->w == 640) && n + 1024 + 6 + 12 <= b.len;
        if (ok) {
            d->fb.assign(p, p + n);
            d->pal.assign(p + n, p + n + 768);
            d->cfg.assign(p + n + 768, p + n + 1024);
            size_t o = n + 1024;
            uint16_t cnt; std::memcpy(&cnt, p + o, 2);
            o += 6;
            d->anim_count = cnt;
            const size_t an = (size_t)cnt * 0x1c;
            if (o + an + 0x60 + 0xc0 + 12 == b.len) {
                d->anim.assign(p + o, p + o + an); o += an;
                std::memcpy(d->r93f40, p + o, 0x60); o += 0x60;
                std::memcpy(d->rb5800, p + o, 0xc0);
            } else {
                ok = false;
            }
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

static bool state_equal(const GameState &ref) {
    if (ref.rng != g_state->rng) return false;
    if (std::memcmp(ref.players, g_state->players, sizeof ref.players) != 0) return false;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        if (ref.things[i].cls == 0 && g_state->things[i].cls == 0) continue;
        const uint8_t *a = reinterpret_cast<const uint8_t *>(&ref.things[i]);
        const uint8_t *b = reinterpret_cast<const uint8_t *>(&g_state->things[i]);
        if (std::memcmp(a + 4, b + 4, 0x10) != 0 || std::memcmp(a + 0x18, b + 0x18, sizeof(Thing) - 0x18) != 0)
            return false;
    }
    return true;
}

static void report_state_diff(const GameState &ref, int tick) {
    if (ref.rng != g_state->rng) std::printf("  state %d: rng differs\n", tick);
    for (int p = 0; p < 8; p++) {
        const uint8_t *a = reinterpret_cast<const uint8_t *>(&ref.players[p]);
        const uint8_t *b = reinterpret_cast<const uint8_t *>(&g_state->players[p]);
        for (int k = 0; k < (int)sizeof(PlayerRec); k++)
            if (a[k] != b[k]) { std::printf("  state %d: player %d +%#x %02x/%02x\n", tick, p, k, a[k], b[k]); break; }
    }
    int shown = 0;
    for (int i = 1; i < MC_THING_SLOTS && shown < 6; i++) {
        if (ref.things[i].cls == 0 && g_state->things[i].cls == 0) continue;
        const uint8_t *a = reinterpret_cast<const uint8_t *>(&ref.things[i]);
        const uint8_t *b = reinterpret_cast<const uint8_t *>(&g_state->things[i]);
        for (int k = 4; k < (int)sizeof(Thing); k++)
            if ((k < 0x14 || k >= 0x18) && a[k] != b[k]) {
                std::printf("  state %d: thing %d (cls %d type %d) +%#x %02x/%02x\n", tick, i, a[0x40], a[0x41], k, a[k], b[k]);
                shown++;
                break;
            }
    }
}

static int32_t rd32(const uint8_t *p) { int32_t v; std::memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

static void write_diff_ppm(const std::string &path, const uint8_t *ref, const uint8_t *port, const uint8_t *pal, int W, int H) {
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

struct Poke { uint32_t raw_tick; int kind, off, value; };

static bool load_schedule(const std::string &dir, std::vector<Poke> *out, int *hires, int *hud) {
    mc_blob b;
    if (!mc_read_file((dir + "/schedule.txt").c_str(), &b)) return false;
    std::string s(reinterpret_cast<const char *>(b.data), b.len);
    mc_blob_free(&b);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t e = s.find('\n', pos);
        if (e == std::string::npos) e = s.size();
        const std::string line = s.substr(pos, e - pos);
        pos = e + 1;
        if (line.empty()) continue;
        if (line[0] == '#') {
            int h = 0, u = 0;
            if (std::sscanf(line.c_str(), "# hires %d hud %d", &h, &u) == 2) { *hires = h; *hud = u; }
            continue;
        }
        Poke p{};
        unsigned t = 0;
        if (std::sscanf(line.c_str(), "%u %d %d %d", &t, &p.kind, &p.off, &p.value) == 4) {
            p.raw_tick = t;
            out->push_back(p);
        }
    }
    return true;
}

static void apply_pokes(const std::vector<Poke> &pokes, uint32_t raw_tick) {
    for (const Poke &p : pokes) {
        if (p.raw_tick != raw_tick) continue;
        uint8_t *base = p.kind == 0 ? reinterpret_cast<uint8_t *>(g_state)
                      : p.kind == 1 ? reinterpret_cast<uint8_t *>(g_cfg)
                      : reinterpret_cast<uint8_t *>(&g_state->players[g_state->local_player & 7]);
        base[p.off] = (uint8_t)p.value;
    }
}

struct TickResult {
    int tick = 0, diff = 0, sky = 0, terrain = 0, thing = 0, hud = 0, sky_n = 0, terrain_n = 0, thing_n = 0, hud_n = 0;
    std::string segment;
};

// The render-relevant settings of the tick, as a segment key.
static std::string segment_key() {
    const GameState *st = g_state;
    const PlayerRec &rec = st->players[st->local_player & 7];
    char buf[160];
    std::snprintf(buf, sizeof buf, "refl %d shad %d sky %d view %d hud %d%d blur %d smooth %d mode %d pent %d cred %d",
                  st->opt_second_surface, st->opt_shadows, st->opt_textured_sky, st->view_size, st->opt_hud_a,
                  st->opt_hud_b, st->opt_motion_blur, st->opt_smooth, rec.input_mode, g_cfg->pentium,
                  g_cfg->credits_state[0] != 0);
    return buf;
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char *game_dir = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    const std::string dir = argc > 2 ? argv[2] : MC_REFERENCE_FB2_DIR;
    const int last_tick = argc > 3 ? std::atoi(argv[3]) : 1 << 30;
    const std::string out_dir = argc > 4 ? argv[4] : ".";
    const bool verbose = std::getenv("MC_RFB_VERBOSE") != nullptr;
    const bool strict = std::getenv("MC_RFB_LENIENT") == nullptr;
    std::vector<int> want_ticks;
    if (const char *t = std::getenv("MC_RFB_TICKS"))
        for (const char *p = t; *p;) { want_ticks.push_back(std::atoi(p)); while (*p && *p != ',') p++; if (*p) p++; }

    static GameState ref;
    static FbDump first;
    if (!load_state(dir, 413, &ref) || !load_fb(dir, 413, &first)) {
        std::printf("SKIP: reference %s/tick00413.fb / .gam missing; run tools/reference/fb/run_reference.py "
                    "(see docs/analysis/port_render_reference2.md)\n", dir.c_str());
        return 0;
    }
    std::vector<Poke> pokes;
    int hires = first.w == 640, hud_flag = 0;
    if (!load_schedule(dir, &pokes, &hires, &hud_flag))
        std::printf("no schedule.txt: plain playback\n");
    if (const char *h = std::getenv("MC_RFB_HUD")) hud_flag = std::atoi(h);
    const bool hud_mode = hud_flag != 0;
    const int W = first.w, H = first.h, NPIX = W * H;
    std::printf("reference %s: %dx%d, %zu scheduled byte pokes, flight HUD %s\n", dir.c_str(), W, H, pokes.size(),
                hud_mode ? "forced on" : "as in the movie");

    if (!engine_init(game_dir)) { std::printf("engine_init failed\n"); return 2; }
    g_hook_frame_state = nullptr;
    g_projectile_null_hit_index = (uint16_t)((int32_t)(0u - g_snapshot_things_base) / (int32_t)sizeof(ThingImage));
    g_cfg->flags = 0; g_cfg->paused = 0;
    sim_prepare_movie();
    // The mode of the run (config_parse_33750 sets DAT_0012edae before anything is generated).
    g_video_mode_flags = (uint16_t)(W == 640 ? 8 : 1);
    sim_load_level(38);                    // `carpet -roll 1 -level 38`: g_rng16 as the reference run left it
    std::printf("g_rng16 after generating level 38: 0x%04x\n", (unsigned)g_rng16);
    ui_draw_set_video_mode(game_dir);
    // game_main_32a00 0x32c35: the second screen buffer (motion blur) only in 320x200.
    static std::vector<uint8_t> frame2;
    if (g_video_mode_flags == 1) { frame2.assign(64000, 0); g_frame2 = frame2.data(); }
    else g_frame2 = nullptr;
    g_cfg->pentium = first.cfg[8];          // DOSBox: cpu_detect_5ac80 says 0
    if (!demo_open(game_dir, 0)) { std::printf("movie 0 missing\n"); return 2; }

    std::vector<uint8_t> pix((size_t)NPIX, 0), pix_nothing((size_t)NPIX), pix_sky((size_t)NPIX), pix_nohud((size_t)NPIX);
    const FrameBuffer fb{pix.data(), W, H};
    const FrameBuffer fb_b{pix_nothing.data(), W, H}, fb_c{pix_sky.data(), W, H}, fb_d{pix_nohud.data(), W, H};
    // The flight HUD during playback (patch --hud): hud.cpp's port-only switch, so the credits roll of
    // render_frame's tail still sees the playback bit (Config.flags & 4) as in the patched original.
    auto with_hud = [&](auto fn) {
        g_hud_force_flight_hud = hud_mode;
        fn();
        g_hud_force_flight_hud = false;
    };

    std::vector<TickResult> results;
    int compared = 0, identical = 0, state_bad = 0, first_state_bad = 0, opt_bad = 0, first_opt_bad = 0;
    int slope_bad = 0, first_slope_bad = 0, anim_bad = 0, first_anim_bad = 0, pal_same = 0, cred_bad = 0, first_cred_bad = 0;
    int first_diff_tick = 0, auto_blur = 0;
    bool more = true;
    int prev_tick = -1, pending_cred_tick = -1;
    uint8_t pending_cred[7] = {};
    FbDump d;
    for (int steps = 0; more && steps < 20000; steps++) {
        if (steps > 0) {
            with_hud([&] { hud_tick_state(g_state->local_player); });   // render_frame's writes of the previous tick
            // vga_present_frame_2f480 of the previous tick: the pointer goes into the back buffer (book: entry 1).
            const int im = g_state->players[g_state->local_player & 7].input_mode;
            ui_draw_mouse_pointer(fb, ui_sprite(g_ui_pointers, im == 2 ? 1 : 0), g_mouse_x, g_mouse_y);
            apply_pokes(pokes, (uint32_t)prev_tick);                     // cave B, after that render
            if (pending_cred_tick == prev_tick && std::memcmp(pending_cred, g_cfg->credits_state, 7) != 0) {
                cred_bad++;
                if (!first_cred_bad) first_cred_bad = prev_tick;
            }
        }
        if (!(g_cfg->paused & 1)) texture_anim_update();
        more = demo_step();
        if (steps == 0) std::memcpy(&g_state->opt_second_surface, &ref.opt_second_surface, 0x21b9 - 0x2195);
        const int local = g_state->local_player & 7;
        const int tick = (int)g_state->players[local].tick;
        prev_tick = tick;
        g_anim_tick = (uint32_t)tick;
        if (tick > last_tick) break;
        const bool have = load_fb(dir, tick, &d) && d.w == W;
        if (have && load_state(dir, tick, &ref)) {
            if (!state_equal(ref)) {
                state_bad++;
                if (!first_state_bad) { first_state_bad = tick; report_state_diff(ref, tick); }
            }
            if (std::memcmp(&ref.opt_second_surface, &g_state->opt_second_surface, 0x21b9 - 0x2195) != 0) {
                opt_bad++;
                if (!first_opt_bad) first_opt_bad = tick;
            }
        }

        int32_t sx0, sy0, sx1, sy1;
        render_get_slope_state(&sx0, &sy0);
        with_hud([&] { render_frame_draw(fb, local); });
        render_get_slope_state(&sx1, &sy1);
        const int view_off = (int)(g_rt_dest - pix.data());
        const int vx0 = view_off % W, vy0 = view_off / W, vx1 = vx0 + g_rt_width, vy1 = vy0 + g_rt_height;
        if (!have) continue;
        std::string seg = segment_key();
        {   // render_view_2f6e0's automatic blur (320x200, flight, Config.pentium, full view, |speed| > 0x50)
            const PlayerRec &pl = g_state->players[local];
            int sp = g_state->things[pl.thing % MC_THING_SLOTS].speed_cur;
            if (sp < 0) sp = -sp;
            if (W == 320 && pl.input_mode == 0 && g_cfg->pentium && g_frame2 && g_state->view_size == 0x28 && sp > 0x50) {
                seg += " autoblur";
                auto_blur++;
            }
        }
        {
            // Region masks from extra renders (things hooks off; textured sky toggled; no flight HUD), with
            // the carried state (slope low-pass, the previous frame for motion blur) put back.
            std::vector<uint8_t> f2;
            if (g_frame2) f2.assign(g_frame2, g_frame2 + 64000);
            CellThingsFn h1 = g_render_cell_things, h2 = g_render_cell_things_mirrored;
            g_render_cell_things = nullptr; g_render_cell_things_mirrored = nullptr;
            std::memcpy(pix_nothing.data(), pix.data(), (size_t)NPIX);
            render_set_slope_state(sx0, sy0);
            with_hud([&] { render_frame_draw(fb_b, local); });
            const uint8_t sky_opt = g_state->opt_textured_sky;
            g_state->opt_textured_sky = sky_opt ? 0 : 1;
            std::memcpy(pix_sky.data(), pix.data(), (size_t)NPIX);
            render_set_slope_state(sx0, sy0);
            with_hud([&] { render_frame_draw(fb_c, local); });
            g_state->opt_textured_sky = sky_opt;
            g_render_cell_things = h1; g_render_cell_things_mirrored = h2;
            if (hud_mode) {
                std::memcpy(pix_nohud.data(), pix.data(), (size_t)NPIX);
                render_set_slope_state(sx0, sy0);
                render_frame_draw(fb_d, local);
            }
            render_set_slope_state(sx1, sy1);
            if (g_frame2) std::memcpy(g_frame2, f2.data(), 64000);
            ui_set_target(fb);
        }
        compared++;

        TickResult r;
        r.tick = tick;
        r.segment = seg;
        for (int i = 0; i < NPIX; i++) {
            const int px = i % W, py = i / W;
            const bool is_hud = px < vx0 || px >= vx1 || py < vy0 || py >= vy1 || (hud_mode && pix[i] != pix_nohud[i]);
            const bool is_thing = !is_hud && pix[i] != pix_nothing[i];
            const bool is_sky = !is_thing && (pix_nothing[i] != pix_sky[i] || pix_nothing[i] == 0xff);
            const bool dif = pix[(size_t)i] != d.fb[(size_t)i];
            if (is_hud) { r.hud_n++; r.hud += dif; }
            else if (is_thing) { r.thing_n++; r.thing += dif; }
            else if (is_sky) { r.sky_n++; r.sky += dif; }
            else { r.terrain_n++; r.terrain += dif; }
            r.diff += dif;
        }
        if (r.diff == 0) identical++;
        else if (!first_diff_tick) first_diff_tick = tick;
        results.push_back(r);

        int32_t sx, sy;
        render_get_slope_state(&sx, &sy);
        if (sx != rd32(d.r93f40 + 0x3c) || sy != rd32(d.r93f40 + 0x40)) { slope_bad++; if (!first_slope_bad) first_slope_bad = tick; }
        int anim_diff = 0;
        for (int k = 0; k < d.anim_count; k++) {
            const uint8_t *a = d.anim.data() + k * 0x1c;
            if (rd32(a + 4) == 0) continue;
            unsigned frame = 0;
            if (!sprite_anim_info(rd16(a + 0x1a), &frame, nullptr, nullptr) || frame != rd16(a + 0x16)) anim_diff++;
        }
        if (anim_diff) { anim_bad++; if (!first_anim_bad) first_anim_bad = tick; }
        bool pal_eq = true;
        for (int k = 0; k < 768; k++) if ((d.pal[k] & 63) != (g_palette6[k] & 63)) { pal_eq = false; break; }
        pal_same += pal_eq;
        // credits_state: the dump's Config is taken after render N's writes and cave B's pokes of raw tick N;
        // compared at the start of the next step (after hud_tick_state + apply_pokes).
        std::memcpy(pending_cred, d.cfg.data() + 0xa1, 7);
        pending_cred_tick = tick;

        if (verbose || (r.diff && results.size() < 2000 && (first_diff_tick == tick)) || tick % 500 == 0)
            std::printf("%5d diff %6d (sky %d/%d terrain %d/%d thing %d/%d hud %d/%d) [%s]\n", tick, r.diff, r.sky, r.sky_n,
                        r.terrain, r.terrain_n, r.thing, r.thing_n, r.hud, r.hud_n, seg.c_str());
        if (std::find(want_ticks.begin(), want_ticks.end(), tick) != want_ticks.end()) {
            char name[64];
            std::snprintf(name, sizeof name, "/rfb2_%05d.ppm", tick);
            write_diff_ppm(out_dir + name, d.fb.data(), pix.data(), d.pal.data(), W, H);
            std::snprintf(name, sizeof name, "/rfb2_%05d.raw", tick);
            if (FILE *f = std::fopen((out_dir + name).c_str(), "wb")) {
                std::fwrite(d.fb.data(), 1, (size_t)NPIX, f);
                std::fwrite(pix.data(), 1, (size_t)NPIX, f);
                std::fclose(f);
            }
        }
    }
    demo_close();

    std::printf("\ncompared %d frames (%dx%d): %d identical, first differing tick %d\n", compared, W, H, identical, first_diff_tick);
    // Per segment.
    struct Seg { int first = 0, last = 0, frames = 0, ident = 0; long long diff = 0, sky = 0, terr = 0, thing = 0, hud = 0; };
    std::vector<std::pair<std::string, Seg>> segs;
    for (const TickResult &r : results) {
        if (segs.empty() || segs.back().first != r.segment) segs.push_back({ r.segment, Seg{} });
        Seg &s = segs.back().second;
        if (!s.frames) s.first = r.tick;
        s.last = r.tick; s.frames++; s.ident += r.diff == 0;
        s.diff += r.diff; s.sky += r.sky; s.terr += r.terrain; s.thing += r.thing; s.hud += r.hud;
    }
    std::printf("per segment (ticks, frames identical, differing pixels sky / terrain / thing / hud):\n");
    for (const auto &[key, s] : segs)
        std::printf("  %5d..%5d  %4d / %4d  %9lld = %lld / %lld / %lld / %lld   [%s]\n", s.first, s.last, s.ident, s.frames, s.diff,
                    s.sky, s.terr, s.thing, s.hud, key.c_str());
    std::printf("frames drawn with the automatic motion blur: %d\n", auto_blur);
    std::printf("state dumps: %d ticks differ from the port's simulation (first %d)\n", state_bad, first_state_bad);
    std::printf("render options of the dumps differ from the port's in %d ticks (first %d)\n", opt_bad, first_opt_bad);
    std::printf("slope low-pass: %d ticks differ (first %d); sprite animation: %d ticks differ (first %d)\n", slope_bad,
                first_slope_bad, anim_bad, first_anim_bad);
    std::printf("DAC = data/palette.dat in %d of %d frames; credits state differs in %d frames (first %d)\n", pal_same,
                compared, cred_bad, first_cred_bad);
    std::vector<TickResult> worst = results;
    std::sort(worst.begin(), worst.end(), [](const TickResult &a, const TickResult &b) { return a.diff > b.diff; });
    std::printf("worst ticks:");
    for (size_t i = 0; i < worst.size() && i < 10; i++) std::printf(" %d(%d)", worst[i].tick, worst[i].diff);
    std::printf("\n");
    if (state_bad) { std::printf("FAIL: the simulation diverged from the state dumps\n"); return 1; }
    if (strict && identical != compared) { std::printf("FAIL: %d frames differ\n", compared - identical); return 1; }
    std::printf("%s: %d / %d frames pixel-identical\n", identical == compared ? "OK" : "DONE", identical, compared);
    return 0;
}
