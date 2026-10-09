// Round 12 (task A): the Conquest match HUD (hud.h hud_draw_mode) over a real mode level, 640x480 and 320x200:
// match timer, respawn countdown, "press space", eliminated, desecration bars (both directions), victory / defeat
// banner. Draw only: g_mode and GameState are unchanged by drawing; without a mode nothing is drawn.
// Screenshots (PNG) in %TEMP%/mc_hud_mode_test (or MC_HUD_MODE_OUT): look at them.
#define _CRT_SECURE_NO_WARNINGS
#include "engine.h"
#include "hud.h"
#include "ui_draw.h"
#include "mc_globals.h"
#include "player.h"
#include "mode.h"
#include "mode_level.h"
#include "mcfile.h"
#include "../mcport/screenshot.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static fs::path g_out;

static uint32_t fnv(const void *p, size_t n, uint32_t h = 2166136261u) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

struct Frame {
    int w, h;
    std::vector<uint8_t> px;
    Frame(int w_, int h_) : w(w_), h(h_), px((size_t)w_ * h_, 0) {}
    FrameBuffer fb() { return FrameBuffer{px.data(), w, h}; }
};

static void write_png(const Frame &f, const char *name) {
    uint8_t pal[768];
    mc_palette_to_rgb(g_palette6, pal);
    const int s = f.w < 640 ? 2 : 1;
    std::vector<uint8_t> rgb((size_t)f.w * s * f.h * s * 3);
    for (int y = 0; y < f.h * s; y++)
        for (int x = 0; x < f.w * s; x++)
            std::memcpy(&rgb[((size_t)y * f.w * s + x) * 3], pal + f.px[(size_t)(y / s) * f.w + x / s] * 3, 3);
    const std::string path = (g_out / (std::string("hud_mode_") + name + ".png")).string();
    CHECK(shot_write_png(path.c_str(), f.w * s, f.h * s, rgb.data()));
    std::printf("  wrote %s\n", path.c_str());
}

// the frame of the local player with the current g_mode
static Frame draw(int w, int h) {
    Frame f(w, h);
    const uint32_t before = fnv(g_state, sizeof *g_state, fnv(&g_mode, sizeof g_mode));
    render_frame_draw(f.fb(), 0);
    CHECK(fnv(g_state, sizeof *g_state, fnv(&g_mode, sizeof g_mode)) == before);   // draw only
    return f;
}
static int diff(const Frame &a, const Frame &b) {
    int n = 0;
    for (size_t i = 0; i < a.px.size(); i++) n += a.px[i] != b.px[i];
    return n;
}

int main(int argc, char **argv) {
    const char *game = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    if (!engine_init(game)) { std::printf("SKIP: engine_init failed (no game data?)\n"); return 0; }
    mode_level_register();
    const char *out = std::getenv("MC_HUD_MODE_OUT");
    g_out = out ? fs::path(out) : fs::temp_directory_path() / "mc_hud_mode_test";
    std::error_code ec;
    fs::create_directories(g_out, ec);

    ModeParams mp;
    mp.seed = 5; mp.world_size = 512; mp.bots = 1; mp.humans = 1;
    mp.match.time_limit_min = 10;
    mp.match.win = MATCH_WIN_ELIMINATION | MATCH_WIN_TIMER;
    g_cfg->flags = 0;
    CHECK(mode_start_run(game, GAME_MODE_CONQUEST, mp));
    CHECK(engine_load_level(MODE_LEVEL_INDEX));
    g_hook_player_local_input = nullptr;
    for (int i = 0; i < 40; i++) engine_tick();      // the wizards spawn
    g_state->opt_hud_a = 1;
    g_state->opt_hud_b = 1;
    g_video_mode_flags = 8;
    CHECK(ui_draw_init(game));

    const ModeState saved = g_mode;
    // no mode: nothing drawn by hud_draw_mode
    {
        Frame f(640, 480);
        ui_set_target(f.fb());
        g_mode.mode = GAME_MODE_ORIGINAL;
        hud_draw_mode(0);
        int nz = 0;
        for (uint8_t v : f.px) nz += v != 0;
        CHECK(nz == 0);
        g_mode = saved;
    }
    for (int flags : {8, 1}) {
        g_video_mode_flags = (uint16_t)flags;
        CHECK(ui_draw_set_video_mode(game));
        const bool lo = flags == 1;
        const int W = lo ? 320 : 640, H = lo ? 200 : 480;
        const char *tag = lo ? "320" : "640";
        char name[64];
        for (int font = 0; font < 2; font++) {
            ui_set_font(font);
            std::printf("%s font %d: line height %d, \"VICTORY\" %d wide\n", tag, font, ui_font_line_height(), ui_text_width("VICTORY"));
        }
        // the plain frame: no limit, nothing special -> identical to the frame without a mode
        g_mode = saved;
        g_mode.params.match.time_limit_min = 0;
        const Frame plain = draw(W, H);
        g_mode.mode = GAME_MODE_ORIGINAL;
        const Frame nomode = draw(W, H);
        CHECK(diff(plain, nomode) == 0);
        // the timer: 10 minutes, 8:20 played -> 1:40 left
        g_mode = saved;
        g_mode.tick = 25 * 500;
        const Frame timer = draw(W, H);
        std::printf("%s timer: %d pixels\n", tag, diff(plain, timer));
        CHECK(diff(plain, timer) > 50);
        std::snprintf(name, sizeof name, "timer_%s", tag); write_png(timer, name);
        // dead, 7.2 s to the respawn
        g_mode = saved;
        g_mode.players[0].respawn_ticks = 180;
        g_mode.players[0].deaths = 1;
        const Frame dead = draw(W, H);
        CHECK(diff(plain, dead) > 200);
        std::snprintf(name, sizeof name, "respawn_%s", tag); write_png(dead, name);
        // the castle being desecrated by record 1 (40 %) while the local wizard desecrates record 1's (75 %)
        g_mode = saved;
        g_mode.players[1].desecrate_rec1 = 1;
        g_mode.players[1].desecrate_ticks = 250;
        g_mode.players[1].flags |= MODE_PLAYER_DESECRATING;
        g_mode.players[0].desecrate_rec1 = 2;
        g_mode.players[0].desecrate_ticks = 470;
        g_mode.players[0].flags |= MODE_PLAYER_DESECRATING;
        const Frame des = draw(W, H);
        CHECK(diff(plain, des) > 400);
        std::snprintf(name, sizeof name, "desecrate_%s", tag); write_png(des, name);
        // eliminated
        g_mode = saved;
        g_mode.players[0].flags |= MODE_PLAYER_ELIMINATED;
        const Frame elim = draw(W, H);
        CHECK(diff(plain, elim) > 200);
        std::snprintf(name, sizeof name, "eliminated_%s", tag); write_png(elim, name);
        // the match is over: victory for side 0
        g_mode = saved;
        for (int p = 0; p < 2; p++) { g_mode.players[p].flags |= MODE_PLAYER_IN_MATCH; g_mode.players[p].side = (uint8_t)p; }
        g_mode.match_state = MATCH_OVER;
        g_mode.end_reason = MATCH_END_ELIMINATION;
        g_mode.winner_side = 0;
        const Frame win = draw(W, H);
        CHECK(diff(plain, win) > 200);
        std::snprintf(name, sizeof name, "victory_%s", tag); write_png(win, name);
        g_mode.winner_side = 1;
        g_mode.end_reason = MATCH_END_MANA;
        const Frame lose = draw(W, H);
        CHECK(diff(win, lose) > 50);
        std::snprintf(name, sizeof name, "defeat_%s", tag); write_png(lose, name);
    }
    g_mode = saved;
    mode_stop_run();
    ui_draw_shutdown();
    engine_shutdown();
    std::printf(g_fail ? "hud_mode_test: %d FAILURES\n" : "hud_mode_test: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
