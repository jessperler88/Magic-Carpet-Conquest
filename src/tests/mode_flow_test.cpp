// mode_flow_test (Phase 4 round 12, task D; docs/analysis/port_r12_flow.md): the urn-to-result gate, headless.
//
// The same sequence mcport runs (mode_flow.h + rts_run.cpp's rts_begin, linked here), driven through fe_frame:
//  1. main menu with PortSettings.mode_menu on -> the urn (found as the pixels that differ from the menu with
//     mode_menu off) -> the setup screen (state 11) -> Enter = Start -> FE_START_MODE with fe_mode_params();
//  2. the match: 1024 cells, 1 human (god mode, idle) + 3 bots, MATCH_WIN_TIMER with a 1-minute limit; the level ends
//     by the match rules; mode_flow_finish -> state 12 with the final block (match over, end reason timer); the
//     campaign's progress (Config.level, the .gam's spells / options) is back; the result screen's Enter -> main menu;
//  3. the same with the pause menu's forfeit (mode_flow_forfeit after 200 ticks): state 12, the local record
//     eliminated, the match over without a winner;
//  4. the match-rule options of `rts` (rts_parse_args) and of the scenario header.
// Pending parts (printed as PENDING, not failures) while the other round-12 tasks have not landed: no urn drawn
// (task A) -> the setup screen is skipped (fe_mode_set_params + FE_START_MODE directly); the match rules never end
// the level (task B: mode_time_left_seconds() stays -1) -> the match is forfeited after 300 ticks instead.
// argv[1] = game dir. Screens: mode_flow_*.ppm next to the executable.
#define _CRT_SECURE_NO_WARNINGS
#include "frontend.h"
#include "fe_mode.h"
#include "mode.h"
#include "mode_level.h"
#include "mode_flow.h"
#include "mode_rules.h"
#include "game.h"
#include "sim.h"
#include "input.h"
#include "palette_fx.h"
#include "debug_cmd.h"
#include "scenario.h"
#include "settings.h"
#include "mc_globals.h"
#include "rts_run.h"
#include "world.h"
#include "crash_handler.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); if (_a != _b) { \
    std::printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); g_fail++; } } while (0)

static std::string g_game, g_out;
static uint8_t g_fb[320 * 200];
static uint32_t g_tick = 1000;
static FeResult g_last = FE_CONTINUE;
static int g_level = -1;
static bool g_pending_a = false, g_pending_b = false;

static FeResult frame() {
    g_tick += 2;
    g_last = fe_frame(FrameBuffer{g_fb, 320, 200}, g_tick, &g_level);
    return g_last;
}
static void run(int n) { for (int i = 0; i < n; i++) if (frame() != FE_CONTINUE) return; }
static bool wait_state(int s, int max = 600) {
    for (int i = 0; i < max && fe_state() != s; i++) if (frame() != FE_CONTINUE) break;
    if (fe_state() != s) return false;
    int quiet = 0;
    for (int k = 0; k < 400 && quiet < 6; k++) { if (frame() != FE_CONTINUE) break; quiet = palette_fade_active() ? 0 : quiet + 1; }
    return fe_state() == s;
}
static void click(int x, int y) {          // 320x200 pixels -> the 640x400 virtual mouse
    input_mouse_move(x * 2, y * 2);
    frame();
    input_mouse_button(0, true);
    frame();
    input_mouse_button(0, false);
    run(2);
}
static void key(int sc) { input_key_event(sc, true); frame(); input_key_event(sc, false); }

static void write_ppm(const char *name) {
    const std::string path = g_out + "/mode_flow_" + name + ".ppm";
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n320 200\n255\n");
    const uint8_t *px = fe_screen_pixels(), *pal = fe_palette6();
    for (int i = 0; i < 64000; i++) {
        const uint8_t *c = pal + px[i] * 3;
        const uint8_t rgb[3] = {(uint8_t)(c[0] << 2 | c[0] >> 4), (uint8_t)(c[1] << 2 | c[1] >> 4), (uint8_t)(c[2] << 2 | c[2] >> 4)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
    std::printf("  wrote %s (state %d)\n", path.c_str(), fe_state());
}

// The main menu after a fresh front-end start (state 2), the mouse in the top-left corner.
static std::vector<uint8_t> menu_image(bool mode_menu) {
    g_settings.mode_menu = mode_menu;
    fe_shutdown();
    g_tick = 1000;                                      // the same frames and ticks for both images
    input_reset();
    input_mouse_move(0, 0);
    if (!fe_init(g_game.c_str(), 2)) return {};
    wait_state(2);
    run(20);
    return std::vector<uint8_t>(fe_screen_pixels(), fe_screen_pixels() + 64000);
}

// What a match should be played with in this test.
static ModeParams wanted_params() {
    ModeParams p;
    p.seed = 4242;
    p.world_size = 1024;
    p.bots = 3;
    p.humans = 1;
    p.flags = MODE_PARAM_DEBUG;
    p.match.win = MATCH_WIN_TIMER;
    p.match.time_limit_min = 1;
    return p;
}

// 1: main menu -> urn -> setup -> Start. True when FE_START_MODE was returned (or the pending fallback).
static bool menu_to_start() {
    std::printf("== 1: main menu -> urn -> setup screen -> Start\n");
    const std::vector<uint8_t> off = menu_image(false);
    const std::vector<uint8_t> off2 = menu_image(false);
    const std::vector<uint8_t> on = menu_image(true);
    if (off.empty() || on.empty()) { std::printf("FAIL: the front end did not start\n"); g_fail++; return false; }
    CHECK(off == off2);                                 // the menu is reproducible: what differs below is the urn
    write_ppm("menu_urn");
    int x0 = 320, y0 = 200, x1 = -1, y1 = -1, n = 0;
    for (int i = 0; i < 64000; i++)
        if (off[(size_t)i] != on[(size_t)i]) {
            const int x = i % 320, y = i / 320;
            n++;
            if (x < x0) x0 = x;
            if (y < y0) y0 = y;
            if (x > x1) x1 = x;
            if (y > y1) y1 = y;
        }
    fe_mode_set_params(wanted_params());
    if (n == 0) {
        g_pending_a = true;
        std::printf("  PENDING (task A): no urn in the main menu - fe_mode_set_params + FE_START_MODE directly\n");
        return true;
    }
    std::printf("  the urn: %d pixels in %d,%d..%d,%d\n", n, x0, y0, x1, y1);
    CHECK(x0 >= 160 && y0 >= 100);                      // bottom right
    click((x0 + x1) / 2, (y0 + y1) / 2);
    if (!wait_state(FE_STATE_MODE_SETUP)) { std::printf("FAIL: the urn's click did not open the setup screen (state %d)\n", fe_state()); g_fail++; return false; }
    write_ppm("setup");
    key(MC_SC_ENTER);                                   // Start
    for (int i = 0; i < 600 && g_last != FE_START_MODE; i++) frame();
    if (g_last != FE_START_MODE) { std::printf("FAIL: Enter on the setup screen did not return FE_START_MODE\n"); g_fail++; return false; }
    const ModeParams p = fe_mode_params(), w = wanted_params();
    CHECK(p.seed == w.seed && p.world_size == w.world_size && p.bots == w.bots);
    CHECK(p.match.win == w.match.win && p.match.time_limit_min == w.match.time_limit_min);
    for (int i = 0; i < 30; i++) frame();               // the exit fade (as mcport presents it)
    return true;
}

// The campaign values a match must not change. The options as a front-end visit leaves them (game_before_frontend
// resets the sound / music toggles among them, as the original does before every menu).
static uint8_t g_options[0xc];
static void set_campaign() {
    g_cfg->level = 7;
    uint8_t *gs = reinterpret_cast<uint8_t *>(g_state);
    for (int i = 0; i < 0x18; i++) gs[0x3bd6 + i] = (uint8_t)(i & 1);
    for (int i = 0; i < 0xc; i++) gs[0x2195 + i] = (uint8_t)(i * 3 + 1);
    game_before_frontend();
    std::memcpy(g_options, gs + 0x2195, sizeof g_options);
}
static bool campaign_intact() {
    const uint8_t *gs = reinterpret_cast<const uint8_t *>(g_state);
    bool ok = true;
    if (g_cfg->level != 7) { std::printf("  Config.level %d, not 7\n", (int)g_cfg->level); ok = false; }
    for (int i = 0; i < 0x18; i++)
        if (gs[0x3bd6 + i] != (uint8_t)(i & 1)) { std::printf("  spell byte %d is %d\n", i, gs[0x3bd6 + i]); ok = false; }
    for (int i = 0; i < 0xc; i++)
        if (gs[0x2195 + i] != g_options[i]) { std::printf("  option byte %d is %d\n", i, gs[0x2195 + i]); ok = false; }
    return ok;
}

// 2 / 3: the match from mcport's FE_START_MODE branch to the result screen. forfeit_tick > 0: forfeit then.
static void play_match(int forfeit_tick) {
    set_campaign();
    g_cfg->flags &= ~0x10;
    const ModeParams mp = mode_flow_menu_params(fe_mode_params(), 0x1234);
    CHECK(mp.humans == 1 && (mp.flags & MODE_PARAM_DEBUG) && (mp.match.win & MATCH_WIN_TIMER));
    const int level = mode_flow_enter();
    CHECK_EQ(level, MODE_LEVEL_INDEX);
    if (!rts_begin(g_game.c_str(), mp, RTS_START_OFFLINE)) { std::printf("FAIL: the run did not start\n"); g_fail++; mode_flow_abort(); return; }
    CHECK(mode_active() && g_world.size == 1024 && g_state->player_count == 4);
    CHECK(mode_campaign_saved());
    std::string why;
    CHECK(debug_cmd_queue(debug_cmd_god(1), &why));     // the idle human survives the minute
    const auto t0 = std::chrono::steady_clock::now();
    GameStatus s = GAME_RUNNING;
    int t = 0;
    bool forfeited = false;
    const int cap = MODE_TICKS_PER_SECOND * 60 + 1500;  // the limit + the end pause + slack
    for (; t < cap; t++) {
        debug_cmd_pump();
        s = game_level_tick();
        (void)debug_cmd_take_messages();
        if (t == 2 && forfeit_tick <= 0 && mode_time_left_seconds() < 0) {
            g_pending_b = true;
            std::printf("  PENDING (task B): no time limit in the match rules - forfeiting at tick 300 instead\n");
        }
        if (s != GAME_RUNNING) break;
        if ((forfeit_tick > 0 && t + 1 == forfeit_tick) || (g_pending_b && forfeit_tick <= 0 && t + 1 == 300)) {
            mode_flow_forfeit();                        // the pause menu's "Forfeit the match" (offline)
            forfeited = true;
            t++;
            break;
        }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const ModeState &r = fe_mode_result();
    if (!forfeited) {
        CHECK(s == GAME_LEVEL_WON || s == GAME_LEVEL_LOST);
        const uint32_t end_tick = g_mode.match_end_tick;
        game_level_end();
        mode_flow_finish(false);
        std::printf("  the level ended after %d ticks (%.1f s, %.0f ticks/s): %s, match over at tick %u, end reason %u, "
                    "winning side %u\n", t + 1, secs, (t + 1) / (secs > 0 ? secs : 1), s == GAME_LEVEL_WON ? "won" : "lost",
                    (unsigned)end_tick, (unsigned)r.end_reason, (unsigned)r.winner_side);
        CHECK_EQ(r.match_state, MATCH_OVER);
        CHECK_EQ(r.end_reason, MATCH_END_TIMER);
        CHECK(r.match_end_tick >= (uint32_t)(MODE_TICKS_PER_SECOND * 60) && r.match_end_tick <= (uint32_t)(MODE_TICKS_PER_SECOND * 60 + 2));
        CHECK(r.tick > r.match_end_tick);               // the end pause ran before the level ended
        int placed = 0;
        for (int p = 0; p < 4; p++) placed += r.players[p].place != 0;
        CHECK_EQ(placed, 4);
        CHECK((s == GAME_LEVEL_WON) == (r.players[0].side == r.winner_side));
    } else {
        std::printf("  forfeited at tick %d (%.1f s)\n", t, secs);
        CHECK(r.players[0].flags & MODE_PLAYER_ELIMINATED);
        CHECK_EQ(r.match_state, MATCH_OVER);
        CHECK_EQ(r.end_reason, MATCH_END_NONE);
        CHECK(!game_in_level());
    }
    CHECK(r.version == MODE_STATE_VERSION && r.mode == GAME_MODE_CONQUEST && r.params.world_size == 1024);
    CHECK_EQ(fe_mode_result_local(), 0);
    CHECK(!mode_active() && !mode_run_active() && gameplay_rules().mode == 0);
    CHECK(!mode_campaign_saved());
    CHECK(campaign_intact());
    CHECK_EQ(fe_state(), FE_STATE_MODE_RESULT);
    // the result screen (task A), then Continue (Enter) -> the main menu
    run(60);
    if (fe_state() == FE_STATE_MODE_RESULT) {
        key(MC_SC_ENTER);                               // the first key shows every line at once
        run(5);
        write_ppm(forfeited ? "result_forfeit" : "result");
        key(MC_SC_ENTER);                               // Continue
        CHECK(wait_state(2));
    } else {
        if (!g_pending_a) std::printf("  note: the result screen was left at once (state %d)\n", fe_state());
        g_pending_a = true;
        CHECK(wait_state(2));
    }
    CHECK_EQ(fe_state(), 2);
}

// 4: the match-rule options.
static void options() {
    std::printf("== 4: match-rule options (rts command line, scenario header)\n");
    auto parse = [](std::vector<const char *> args, ModeParams *p, std::string *err) {
        std::vector<char *> v;
        v.push_back(const_cast<char *>("mcport"));
        v.push_back(const_cast<char *>("dir"));
        v.push_back(const_cast<char *>("rts"));
        for (const char *a : args) v.push_back(const_cast<char *>(a));
        return rts_parse_args((int)v.size(), v.data(), 3, p, err);
    };
    ModeParams p;
    std::string err;
    CHECK(parse({"--teams", "2v2", "--win", "elim,timer", "--time", "2", "--desecrate", "off", "--bot-skill", "3",
                 "--respawn", "4", "--respawn-level", "2", "--mana-pct", "60", "--hold", "20", "--result"}, &p, &err));
    const uint8_t teams[8] = {1, 1, 2, 2, 0, 0, 0, 0};
    CHECK(std::memcmp(p.match.team, teams, 8) == 0);
    CHECK_EQ(p.match.win, MATCH_WIN_ELIMINATION | MATCH_WIN_TIMER);
    CHECK_EQ(p.match.time_limit_min, 2);
    CHECK_EQ(p.match.desecrate_s, 0xff);
    CHECK_EQ(p.match.bot_skill, 3);
    CHECK(p.match.respawn_s == 4 && p.match.respawn_level_s == 2 && p.match.mana_pct == 60 && p.match.mana_hold_s == 20);
    CHECK(rts_show_result());
    CHECK(parse({"--teams", "1,0,2,2,1", "--time", "5"}, &p, &err));
    const uint8_t teams2[8] = {1, 0, 2, 2, 1, 0, 0, 0};
    CHECK(std::memcmp(p.match.team, teams2, 8) == 0);
    CHECK_EQ(p.match.win, MATCH_WIN_ELIMINATION | MATCH_WIN_MANA | MATCH_WIN_TIMER);   // the default + the timer
    CHECK(!rts_show_result());
    CHECK(parse({"--win", "none", "--time", "3", "--teams", "ffa"}, &p, &err));
    CHECK_EQ(p.match.win, MATCH_WIN_NONE);
    CHECK(parse({"--teams", "1v3"}, &p, &err));
    const uint8_t teams3[8] = {1, 2, 2, 2, 0, 0, 0, 0};
    CHECK(std::memcmp(p.match.team, teams3, 8) == 0 && p.match.win == 0);
    CHECK(!parse({"--win", "timer"}, &p, &err));        // no time limit
    std::printf("  --win timer alone: %s\n", err.c_str());
    CHECK(!parse({"--teams", "9"}, &p, &err));
    CHECK(!parse({"--teams", "4v5"}, &p, &err));
    CHECK(!parse({"--desecrate", "255"}, &p, &err));
    CHECK(!parse({"--bot-skill", "5"}, &p, &err));
    CHECK(!parse({"--win", "gold"}, &p, &err));
    CHECK(!parse({"--time"}, &p, &err));
    Scenario sc;
    CHECK(scenario_parse("rts seed 3 size 1024 bots 3 teams 2v2 time 1 desecrate off result\nstop 10\n", "t.scn", &sc));
    CHECK(sc.rts && sc.rts_result && sc.rts_params.world_size == 1024 && sc.rts_params.match.time_limit_min == 1);
    CHECK(std::memcmp(sc.rts_params.match.team, teams, 8) == 0 && sc.rts_params.match.desecrate_s == 0xff);
    CHECK(sc.rts_params.match.win & MATCH_WIN_TIMER);
    Scenario bad;
    CHECK(!scenario_parse("rts seed 3 win timer\n", "t.scn", &bad));
    std::printf("  %s\n", mode_match_rules_describe(sc.rts_params.match).c_str());
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_game = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    std::string exe = argv[0];
    g_out = exe.substr(0, exe.find_last_of("/\\"));
    if (g_out == exe) g_out = ".";

    options();

    if (!sim_init(g_game.c_str())) { std::printf("SKIP: game data not found in %s\n", g_game.c_str()); return g_fail ? 1 : 0; }
    sim_register_gameplay();
    g_video_mode_flags = 1;
    const std::string save = g_out + "/mode_flow_save";
    std::error_code ec;
    std::filesystem::create_directories(save, ec);
    fe_set_save_dir(save.c_str(), nullptr);
    input_reset();

    // 1 + 2: the urn to the result screen
    if (menu_to_start()) {
        std::printf("== 2: the match (1024 cells, 3 bots, timer 1 minute) to the result screen\n");
        play_match(0);
    }
    // 3: the forfeit
    std::printf("== 3: the pause menu's forfeit\n");
    fe_mode_set_params(wanted_params());
    play_match(200);
    fe_shutdown();
    fe_set_save_dir(nullptr, nullptr);

    if (g_pending_a || g_pending_b)
        std::printf("mode_flow_test: PENDING parts: %s%s\n", g_pending_a ? "task A (urn / setup / result screens) " : "",
                    g_pending_b ? "task B (match end by the rules)" : "");
    if (g_fail) { std::printf("mode_flow_test: %d failure(s)\n", g_fail); return 1; }
    std::printf("mode_flow_test: OK\n");
    return 0;
}
