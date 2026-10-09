// Round 12 (task A): the Conquest mode's front-end screens (fe_mode.h) and the setup screen's minimap preview.
//
//  1. worldgen_preview leaves the simulation alone: with a mode level loaded, a preview of another world changes no map
//     byte, no GameState byte, no RNG and not the world size; a level loaded after a preview is byte-identical to the
//     same level loaded before it; the preview's seats are the cells of the real build's WFEAT_SPAWN features;
//  2. the main menu with mode_menu off has no urn (the urn rectangle is the original's picture); with it on only the
//     urn's rectangle changes, the urn lights up under the pointer and is reachable with Tab;
//  3. urn -> setup screen; seed typed, a new seed rolled, world size / bots stepped with both buttons, Start returns
//     FE_START_MODE with exactly those params; Esc returns to the main menu; the preview follows seed / size changes;
//  4. the lobby's urn: the setup from the lobby, OK selects Conquest (mode_proposal = the setup), a second click on
//     the urn deselects it (no proposal);
//  5. the result screen of a synthetic ModeState (victory and defeat), then the main menu again.
// Screenshots (640x400 PNG, pixel-doubled) go to %TEMP%/mc_fe_mode_test (or MC_FE_MODE_OUT): look at them.
#define _CRT_SECURE_NO_WARNINGS
#include "frontend.h"
#include "fe_mode.h"
#include "savegame.h"
#include "palette_fx.h"
#include "input.h"
#include "text.h"
#include "sound.h"
#include "settings.h"
#include "mc_globals.h"
#include "mc_math.h"
#include "mcfile.h"
#include "thing.h"
#include "sim.h"
#include "mode.h"
#include "mode_level.h"
#include "worldgen.h"
#include "world.h"
#include "player.h"
#include "../mcport/screenshot.h"
#include "crash_handler.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); if (_a != _b) { \
    std::printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); g_fail++; } } while (0)

static std::string g_game;
static fs::path g_out;

// ---------------------------------------------------------------------------------------------------------------
// part 1: the preview and the simulation
// ---------------------------------------------------------------------------------------------------------------

static uint32_t fnv(const void *p, size_t n, uint32_t h = 2166136261u) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}
static uint32_t sim_hash() {
    uint32_t h = fnv(g_map_height, g_world.cells);
    h = fnv(g_map_type, g_world.cells, h);
    h = fnv(g_map_flags, g_world.cells, h);
    h = fnv(g_map_light, g_world.cells, h);
    h = fnv(g_cell_things, sizeof(uint16_t) * g_world.cells, h);
    h = fnv(g_state, sizeof *g_state, h);
    h = fnv(&g_world, sizeof g_world, h);
    h = fnv(&g_rng16, sizeof g_rng16, h);
    h = fnv(&g_mode, sizeof g_mode, h);
    const uint32_t mask = g_world_unit_mask;
    h = fnv(&mask, sizeof mask, h);
    return h;
}
static bool load_run(const ModeParams &mp) {
    if (mode_run_active()) mode_stop_run();
    g_cfg->flags = 0;
    g_cfg->paused = 0;
    if (!mode_start_run(g_game.c_str(), GAME_MODE_CONQUEST, mp)) return false;
    return sim_load_level(MODE_LEVEL_INDEX);
}

static void test_preview() {
    std::printf("preview vs the simulation\n");
    ModeParams a;
    a.seed = 7; a.world_size = 512; a.bots = 1; a.humans = 1;
    ModeParams b;
    b.seed = 9; b.world_size = 1024; b.bots = 3; b.humans = 1; b.world.water = 45;
    static LevelData tmpl_b;
    CHECK(mode_build_level(g_game.c_str(), b, &tmpl_b));

    CHECK(load_run(a));
    const uint32_t h1 = sim_hash();
    CHECK(load_run(a));
    const uint32_t h2 = sim_hash();
    std::printf("  level A loaded twice: %08x %08x\n", h1, h2);
    // a preview while the level is loaded changes nothing
    WorldPreview wp;
    CHECK(worldgen_preview(b, tmpl_b, &wp));
    CHECK_EQ(sim_hash(), h2);
    CHECK_EQ(wp.size, 1024);
    CHECK_EQ((long long)wp.height.size(), 1024ll * 1024);
    CHECK_EQ(wp.players, 4);
    int water = 0;
    for (uint8_t h : wp.height) water += h == 0;
    std::printf("  preview of B: %d seats, symmetry %s, water %d %%\n", (int)wp.seats.size(), worldgen_symmetry_name(wp.symmetry),
                (int)(water * 100ll / (long long)wp.height.size()));
    CHECK(water > (int)(wp.height.size() * 35 / 100) && water < (int)(wp.height.size() * 60 / 100));
    // the biggest world the setup offers: how long the setup screen waits for its minimap
    {
        ModeParams c;
        c.seed = 11; c.world_size = 2048; c.bots = 7; c.humans = 1;
        static LevelData tmpl_c;
        CHECK(mode_build_level(g_game.c_str(), c, &tmpl_c));
        WorldPreview wc;
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(worldgen_preview(c, tmpl_c, &wc));
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  preview of a 2048 world, 8 players: %.0f ms, %d seats\n", ms, (int)wc.seats.size());
        CHECK_EQ(wc.seats.size(), 8);
        CHECK_EQ(sim_hash(), h2);
    }
    // the same level loaded after the preview
    CHECK(load_run(a));
    CHECK_EQ(sim_hash(), h2);
    // the preview's seats are the real build's seats
    CHECK(load_run(b));
    std::vector<WorldFeature> spawns;
    for (const WorldFeature &f : worldgen_last_layout().features) if (f.kind == WFEAT_SPAWN) spawns.push_back(f);
    CHECK_EQ(spawns.size(), wp.seats.size());
    for (size_t i = 0; i < spawns.size() && i < wp.seats.size(); i++) {
        CHECK_EQ(spawns[i].x, wp.seats[i].x);
        CHECK_EQ(spawns[i].y, wp.seats[i].y);
    }
    // ... and the preview's water is the built world's water before the texture passes (the passes only flatten
    // a few shore quads): compare the sea / land split of the real maps
    int agree = 0;
    for (uint32_t c = 0; c < g_world.cells; c++) agree += ((g_map_flags[c] & 7) == 0) == (wp.height[c] == 0);
    std::printf("  water / land agreement with the built world: %.2f %%\n", agree * 100.0 / g_world.cells);
    CHECK(agree > (int)(g_world.cells / 100 * 97));
    mode_stop_run();
}

// ---------------------------------------------------------------------------------------------------------------
// part 2..5: the front end
// ---------------------------------------------------------------------------------------------------------------

static uint8_t g_fb[320 * 200];
static uint32_t g_tick = 1000;
static FeResult g_any = FE_CONTINUE;
static int g_level = -1;

static FeResult frame(int ticks = 2) {
    g_tick += (uint32_t)ticks;
    const FeResult r = fe_frame(FrameBuffer{g_fb, 320, 200}, g_tick, &g_level);
    if (r != FE_CONTINUE) g_any = r;
    return r;
}
static void run(int n) { for (int i = 0; i < n; i++) frame(); }
static bool wait_state(int s, int max = 4000) {
    for (int i = 0; i < max && fe_state() != s; i++) frame();
    if (fe_state() != s) return false;
    int quiet = 0;
    for (int k = 0; k < 400 && quiet < 6; k++) { frame(); quiet = palette_fade_active() ? 0 : quiet + 1; }
    return fe_state() == s;
}
static void move(int x, int y) { input_mouse_move(x * 2, y * 2); }
static void click(int x, int y, int button = 0, int settle = 3) {
    move(x, y);
    frame();
    input_mouse_button(button, true);
    frame();
    input_mouse_button(button, false);
    run(settle);
}
static void key(int sc, int settle = 2) { input_key_event(sc, true); frame(); input_key_event(sc, false); run(settle); }

static void write_png(const char *name) {
    std::vector<uint8_t> rgb(640 * 400 * 3);
    const uint8_t *pal = g_display_palette6;
    for (int y = 0; y < 400; y++)
        for (int x = 0; x < 640; x++) {
            const uint8_t c = g_fb[(y / 2) * 320 + x / 2];
            for (int k = 0; k < 3; k++) rgb[((size_t)y * 640 + x) * 3 + k] = (uint8_t)(pal[c * 3 + k] << 2 | pal[c * 3 + k] >> 4);
        }
    const std::string path = (g_out / (std::string("fe_mode_") + name + ".png")).string();
    CHECK(shot_write_png(path.c_str(), 640, 400, rgb.data()));
    std::printf("  wrote %s\n", path.c_str());
}

// the setup screen's rows (fe_mode.cpp: left page x 52, rows of 8 pixels from y 41)
static void click_row(int row, int button = 0, int x_off = 20) { click(52 + x_off, 41 + row * 8 + 3, button); }
constexpr int R_SEED = 0, R_SIZE = 1, R_BOTS = 2;

// pixels that differ inside the urn's rectangle; *outside = those outside it in the right half of the screen (the left
// half has the globe's animation and the parked pointer)
static int urn_diff(const uint8_t *a, const uint8_t *b, int ux, int uy, int uw, int uh, int *outside) {
    int in = 0, out = 0;
    for (int y = 0; y < 200; y++)
        for (int x = 160; x < 320; x++) {
            if (a[y * 320 + x] == b[y * 320 + x]) continue;
            if (x >= ux && x < ux + uw && y >= uy && y < uy + uh) in++; else out++;
        }
    *outside = out;
    return in;
}

static void test_front_end(const std::string &save) {
    mc_globals_init();
    g_video_mode_flags = 1;
    input_reset();
    text_load(g_game.c_str(), 0);
    sound_register_handlers();
    g_state->local_player = 0;
    std::snprintf(g_state->players[0].name, sizeof g_state->players[0].name, "Jessica");
    fe_mode_register();
    fe_set_save_dir(save.c_str(), nullptr);
    CHECK(fe_init(g_game.c_str(), 2));

    // ---- 2: the main menu, mode_menu off, then on ----
    std::printf("main menu\n");
    g_settings.mode_menu = false;
    CHECK(wait_state(2, 600));
    move(10, 10);
    run(10);
    static uint8_t off[64000], on[64000], hover[64000];
    std::memcpy(off, g_fb, sizeof off);
    write_png("menu_faithful");
    g_settings.mode_menu = true;
    run(4);
    std::memcpy(on, g_fb, sizeof on);
    int ux, uy, uw, uh;
    fe_urn_rect_menu(&ux, &uy, &uw, &uh);
    std::printf("  urn %dx%d at (%d, %d)\n", uw, uh, ux, uy);
    CHECK(uw == 32 && uh == 26);
    int outside = 0;
    const int inside = urn_diff(off, on, ux, uy, uw, uh, &outside);
    std::printf("  mode_menu on: %d pixels changed inside the urn's rectangle, %d outside\n", inside, outside);
    CHECK(inside > 300);
    CHECK_EQ(outside, 0);
    write_png("menu_urn");
    move(ux + uw / 2, uy + uh / 2);
    run(4);
    std::memcpy(hover, g_fb, sizeof hover);
    const int lit = urn_diff(on, hover, ux, uy, uw, uh, &outside);
    std::printf("  hover: %d urn pixels lit, %d other pixels changed (the pointer)\n", lit, outside);
    CHECK(lit > 200);
    write_png("menu_urn_hover");
    // Tab reaches the urn: from no item, Shift+Tab selects the last item = the urn
    move(10, 10);
    run(3);
    input_key_event(MC_SC_LSHIFT, true);
    key(MC_SC_TAB);
    input_key_event(MC_SC_LSHIFT, false);
    run(3);
    int tab_lit = urn_diff(on, g_fb, ux, uy, uw, uh, &outside);
    std::printf("  Shift+Tab: %d urn pixels lit\n", tab_lit);
    CHECK(tab_lit > 200);
    key(MC_SC_TAB);                                   // on to item 1: the urn goes dark again
    run(3);
    tab_lit = urn_diff(on, g_fb, ux, uy, uw, uh, &outside);
    CHECK_EQ(tab_lit, 0);
    input_key_event(MC_SC_LSHIFT, true);
    key(MC_SC_TAB);                                   // back to the urn, Enter opens the setup
    input_key_event(MC_SC_LSHIFT, false);
    key(MC_SC_ENTER);
    CHECK(wait_state(FE_STATE_MODE_SETUP, 600));

    // ---- 11: the setup screen ----
    std::printf("setup screen\n");
    run(20);
    write_png("setup_first");
    const uint32_t first_seed = fe_mode_params().seed;
    click_row(R_SEED);                                // a new seed
    CHECK(fe_mode_params().seed != first_seed);
    // type 12345: Backspace until 0, then the digits (the pointer stays on the seed row)
    for (int i = 0; i < 11; i++) key(MC_SC_BACKSPACE, 0);
    CHECK_EQ(fe_mode_params().seed, 0);
    run(2);
    write_png("setup_random");                        // seed 0 = random at the start: "RANDOM", the minimap a "?"
    for (int k : {MC_SC_1, MC_SC_2, MC_SC_3, MC_SC_4, MC_SC_5}) key(k, 0);
    CHECK_EQ(fe_mode_params().seed, 12345);
    // world size 1024 -> 2048 (left) -> 1024 -> 512 (right twice)
    CHECK_EQ(fe_mode_params().world_size, 1024);
    click_row(R_SIZE);
    CHECK_EQ(fe_mode_params().world_size, 2048);
    click_row(R_SIZE, 1);
    click_row(R_SIZE, 1);
    CHECK_EQ(fe_mode_params().world_size, 512);
    // bots 3 -> 4 -> 2
    click_row(R_BOTS);
    CHECK_EQ(fe_mode_params().bots, 4);
    click_row(R_BOTS, 1);
    click_row(R_BOTS, 1);
    CHECK_EQ(fe_mode_params().bots, 2);
    // the preview follows (6 quiet frames after the last change)
    const int gens = fe_mode_preview_generations();
    move(160, 170);
    run(12);
    CHECK(fe_mode_preview_generations() > gens);
    static uint8_t img_a[64 * 64], img_b[64 * 64];
    int seats = 0;
    CHECK(fe_mode_preview_image(img_a, &seats));
    CHECK_EQ(seats, 4);                                // 3 players on a rot4 world: 4 seats, one empty
    write_png("setup_seed12345");
    // another seed: another world
    move(52 + 20, 41 + 3);
    run(2);
    for (int i = 0; i < 6; i++) key(MC_SC_BACKSPACE, 0);
    for (int k : {MC_SC_7, MC_SC_7, MC_SC_7}) key(k, 0);
    CHECK_EQ(fe_mode_params().seed, 777);
    move(160, 170);
    run(12);
    CHECK(fe_mode_preview_image(img_b, &seats));
    int differ = 0;
    for (int i = 0; i < 64 * 64; i++) differ += img_a[i] != img_b[i];
    std::printf("  preview seed 12345 vs 777: %d of 4096 pixels differ\n", differ);
    CHECK(differ > 400);
    write_png("setup_seed777");
    // hover a world control (hint line) and the minimap
    move(166 + 10, 41 + 64 + 4 + 3);
    run(3);
    write_png("setup_hover_water");
    // Start (the ✓ button)
    g_any = FE_CONTINUE;
    click(0x11c + 9, 0x7e + 9);
    for (int i = 0; i < 400 && g_any == FE_CONTINUE; i++) frame();
    CHECK_EQ(g_any, FE_START_MODE);
    {
        const ModeParams &p = fe_mode_params();
        CHECK_EQ(p.seed, 777);
        CHECK_EQ(p.world_size, 512);
        CHECK_EQ(p.bots, 2);
        CHECK_EQ(p.humans, 1);
        CHECK_EQ(p.flags & (MODE_PARAM_TILED | MODE_PARAM_GENERATE), 0);
        CHECK(mode_params_generated(p));
        std::printf("  FE_START_MODE: seed %u, %u cells, %u bots, %u humans\n", (unsigned)p.seed, (unsigned)p.world_size,
                    (unsigned)p.bots, (unsigned)p.humans);
    }
    // the front end again (as mcport after a run without a result): the setup reappears, Esc goes back to the menu
    CHECK(wait_state(FE_STATE_MODE_SETUP, 600));
    run(10);
    key(MC_SC_ESC);
    CHECK(wait_state(2, 600));
    CHECK_EQ(fe_mode_params().seed, 777);             // the setup is remembered

    // ---- 4: the lobby with the urn ----
    std::printf("lobby\n");
    g_fe_network = 1;
    {
        // the multiplayer item: the centre of its mmmask.dat pixels
        static uint8_t mask[64000];
        char p[1024];
        mc_path_join(p, sizeof p, g_game.c_str(), "data/screens/mmmask.dat");
        CHECK(mc_load_rnc_into(p, mask, sizeof mask) == 64000);
        long sx = 0, sy = 0, n = 0;
        for (int i = 0; i < 64000; i++) if (mask[i] == 3) { sx += i % 320; sy += i / 320; n++; }
        int bx = 0, by = 0; long best = -1;
        for (int i = 0; i < 64000 && n; i++) if (mask[i] == 3) {
            const long dx = i % 320 - sx / n, dy = i / 320 - sy / n, d = dx * dx + dy * dy;
            if (best < 0 || d < best) { best = d; bx = i % 320; by = i / 320; }
        }
        click(bx, by);
    }
    CHECK(wait_state(4, 600));
    run(10);
    write_png("lobby_urn");
    CHECK(!fe_lobby_conquest());
    fe_urn_rect_lobby(&ux, &uy, &uw, &uh);
    click(ux + uw / 2, uy + uh / 2);                  // the urn: the setup screen for the session
    CHECK(wait_state(FE_STATE_MODE_SETUP, 600));
    run(10);
    write_png("setup_from_lobby");
    click(0x11c + 9, 0x7e + 9);                       // OK: Conquest for this session
    CHECK(wait_state(4, 600));
    run(10);
    CHECK(fe_lobby_conquest());
    {
        ModeParams prop;
        CHECK_EQ(mode_proposal(&prop), GAME_MODE_CONQUEST);
        CHECK_EQ(prop.seed, 777);
        CHECK_EQ(prop.world_size, 512);
        CHECK_EQ(prop.humans, g_fe_lobby_players);
    }
    write_png("lobby_conquest");
    click(ux + uw / 2, uy + uh / 2);                  // the urn again: the original game
    CHECK(!fe_lobby_conquest());
    CHECK_EQ(mode_proposal(nullptr), GAME_MODE_ORIGINAL);
    click(0xec + 5, 0x9c + 5);                        // back (the hand)
    CHECK(wait_state(2, 800));
    g_fe_network = 0;

    // ---- 12: the result screen ----
    std::printf("result screen\n");
    ModeState st;
    st.version = MODE_STATE_VERSION;
    st.mode = GAME_MODE_CONQUEST;
    st.params = fe_mode_params();
    st.params.bots = 3;
    st.params.match.team[0] = 1; st.params.match.team[1] = 1; st.params.match.team[2] = 2; st.params.match.team[3] = 2;
    st.match_state = MATCH_OVER;
    st.end_reason = MATCH_END_ELIMINATION;
    st.winner_side = 0;
    st.tick = st.match_end_tick = 25 * (12 * 60 + 34) + 7;
    const uint8_t side[4] = {0, 0, 1, 1};
    for (int i = 0; i < 4; i++) {
        ModePlayer &mp = st.players[i];
        mp.flags = MODE_PLAYER_IN_MATCH | (i ? MODE_PLAYER_BOT : 0u) | (i >= 2 ? MODE_PLAYER_ELIMINATED : 0u);
        mp.side = side[i];
        mp.place = (uint8_t)(i < 2 ? 1 : 2);
        mp.kills_wizards = (uint32_t)(3 - i);
        mp.kills_creatures = (uint32_t)(40 + 7 * i);
        mp.deaths = (uint32_t)i;
        mp.castles_razed = i == 0 ? 2u : 0u;
        mp.mana_peak = 12000 + 1500 * i;
    }
    fe_mode_set_result(st, 0);
    fe_enter(FE_STATE_MODE_RESULT);
    for (int i = 0; i < 400; i++) frame();
    write_png("result_victory");
    CHECK_EQ(fe_state(), FE_STATE_MODE_RESULT);
    click(160, 100);
    CHECK(wait_state(2, 800));
    // a defeat, revealed at once by the first click
    fe_mode_set_result(st, 2);
    fe_enter(FE_STATE_MODE_RESULT);
    for (int i = 0; i < 120; i++) frame();
    click(160, 100);
    run(10);
    write_png("result_defeat");
    CHECK_EQ(fe_state(), FE_STATE_MODE_RESULT);
    key(MC_SC_SPACE);
    CHECK(wait_state(2, 800));

    // ---- mode_menu off again: the menu is the original's picture ----
    g_settings.mode_menu = false;
    move(10, 10);
    run(6);
    fe_urn_rect_menu(&ux, &uy, &uw, &uh);
    CHECK_EQ(urn_diff(off, g_fb, ux, uy, uw, uh, &outside), 0);
    CHECK_EQ(outside, 0);
    fe_shutdown();
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    g_game = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    if (!sim_init(g_game.c_str())) { std::printf("SKIP: no game data in %s\n", g_game.c_str()); return 0; }
    sim_register_gameplay();
    mode_level_register();
    g_hook_player_local_input = nullptr;
    const char *out = std::getenv("MC_FE_MODE_OUT");
    g_out = out ? fs::path(out) : fs::temp_directory_path() / "mc_fe_mode_test";
    std::error_code ec;
    fs::create_directories(g_out, ec);
    fs::create_directories(g_out / "save", ec);
    for (const char *n : {"intro.pld", "language.inf"}) fs::remove(g_out / "save" / n, ec);

    test_preview();
    test_front_end((g_out / "save").string());
    std::printf("%s\n", g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
