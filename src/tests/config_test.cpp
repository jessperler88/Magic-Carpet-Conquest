// Port round 7, task E (docs/analysis/port_settings.md): settings, config file, controller mapping, save
// anywhere. argv[1] = game dir (parts 5..7 SKIP without it).
//  1. key chords and pad actions: parse / name round trip;
//  2. the config file: default file -> read back = the playing defaults, no warnings; every key written with
//     other values and read back; unknown keys (kept, warned); malformed and out-of-range values (clamped);
//     keys missing from an old file are appended;
//  3. config_load precedence: file < environment < command line, --faithful (in command-line order), argv
//     compaction, the port key actions;
//  4. GamepadMapper: dead zones, steering, the left stick as held arrows, bindings, quick-select cycling,
//     context changes (no stuck keys), the cursor contexts, disconnect;
//  5. save anywhere: save at tick T, run N ticks, load, run N ticks: identical per-tick checksums; again after
//     another level was loaded in between (a restart); with a 4000-slot pool; the full quick save through
//     commands 10 / 0xb (PortSettings::quicksave_full);
//  6. demo_repair_cell_lists: consistent lists untouched, a cycle / a stale head repaired; the original's quick
//     load measured;
//  8. gameplay-rules movies (possession_range_pct): mvx version 2, played back under the recorded rules;
//     the original's movies under faithful rules.
//  7. extended-pool movies: recorded as mvx / gax / max, played back with the recorded pool size, a bad header
//     refused.
#define _CRT_SECURE_NO_WARNINGS
#include "config.h"
#include "gamepad.h"
#include "savegame.h"
#include "demo.h"
#include "sim.h"
#include "thing.h"
#include "player.h"
#include "mc_globals.h"
#include "settings.h"
#include "crash_handler.h"
#include <SDL_scancode.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); if (_a != _b) { \
    std::printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); g_fail++; } } while (0)
#define CHECK_STR(a, b) do { std::string _a = (a), _b = (b); if (_a != _b) { \
    std::printf("FAIL %s:%d: %s == %s ('%s' != '%s')\n", __FILE__, __LINE__, #a, #b, _a.c_str(), _b.c_str()); g_fail++; } } while (0)

namespace fs = std::filesystem;
static fs::path g_tmp;

static std::string slurp(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static void put_file(const fs::path &p, const std::string &s) { std::ofstream(p, std::ios::binary) << s; }
static void set_env(const char *n, const char *v) {
#ifdef _WIN32
    _putenv_s(n, v ? v : "");
#else
    if (v) setenv(n, v, 1); else unsetenv(n);
#endif
}

static bool settings_equal(const PortSettings &a, const PortSettings &b) {
    PlatformOptions p;
    for (const std::string &k : config_keys()) {
        const std::string sec = k.substr(0, k.find('.'));
        if (sec != "render" && sec != "display" && sec != "pacing" && k != "game.thing_slots" && k != "game.quicksave_full" &&
            k != "game.possession_range_pct" && k != "game.mode_menu" &&
            k != "keys.wasd" && k != "keys.book_tab") continue;
        if (config_get(k.c_str(), a, p) != config_get(k.c_str(), b, p)) return false;
    }
    return true;
}

// ---- 1 ----------------------------------------------------------------------------------------------------
static void test_keys() {
    const char *names[] = {"F11", "Ctrl+F1", "Shift+F11", "Ctrl+Shift+Alt+S", "Enter", "Space", "[", "]", "Esc", "KP5",
                           "0", "9", "PageDown", "none"};
    for (const char *n : names) {
        KeyChord k;
        CHECK(config_parse_key(n, &k));
        CHECK_STR(config_key_name(k), n);
    }
    KeyChord k;
    CHECK(config_parse_key("ctrl + f1", &k) && k.scancode == SDL_SCANCODE_F1 && k.mods == KEYMOD_CTRL);
    CHECK(config_parse_key("Shift+]", &k) && k.scancode == SDL_SCANCODE_RIGHTBRACKET && k.mods == KEYMOD_SHIFT);
    CHECK(!config_parse_key("Hyper+F1", &k));
    CHECK(!config_parse_key("F13", &k));
    CHECK(!config_parse_key("Ctrl+", &k));
    PadAction a;
    for (const char *n : {"cast_left", "cast_right", "spell_next", "spell_prev", "spell_next_right", "spell_prev_right",
                          "save_quick", "load_quick", "none", "Enter", "Ctrl+1"}) {
        CHECK(config_parse_pad_action(n, &a));
        CHECK_STR(config_pad_action_name(a), n);
    }
    CHECK(!config_parse_pad_action("jump", &a));
}

// ---- 2 ----------------------------------------------------------------------------------------------------
static void test_file() {
    const fs::path f = g_tmp / "default.ini";
    CHECK(config_write_default_file(f.string().c_str()));
    PortSettings s{}, want;
    PlatformOptions p;
    config_playing_defaults(&want);
    std::vector<std::string> warn, missing;
    CHECK(config_read_file(f.string().c_str(), &s, &p, &warn, &missing));
    CHECK(warn.empty());
    CHECK(missing.empty());                                    // commented-out keys count as present
    CHECK(settings_equal(s, PortSettings{}));                  // ... and change nothing
    for (const std::string &w : warn) std::printf("  unexpected warning: %s\n", w.c_str());
    // uncommenting every line gives the playing defaults
    {
        std::string text = slurp(f), out;
        std::istringstream in(text);
        for (std::string line; std::getline(in, line); ) {
            if (std::regex_match(line, std::regex("# [a-z0-9_.]+ = .*"))) line = line.substr(2);
            out += line + "\n";
        }
        put_file(g_tmp / "uncommented.ini", out);
        PortSettings u{};
        PlatformOptions up;
        warn.clear();
        CHECK(config_read_file((g_tmp / "uncommented.ini").string().c_str(), &u, &up, &warn, &missing));
        CHECK(settings_equal(u, want));
        for (const std::string &w : warn) std::printf("  unexpected warning: %s\n", w.c_str());
    }

    // every key with a non-default value, written and read back
    PortSettings s2{};
    PlatformOptions p2;
    s2.render_extended = true; s2.draw_distance = 97; s2.fog_start_pct = 60; s2.lod = 2; s2.compose = 1;
    s2.view_width = 1920; s2.view_height = 1080; s2.hud_scale_mode = 2; s2.hud_corners = false;
    s2.thing_slots = 12345; s2.interpolate = true; s2.fps_cap = 144; s2.quicksave_full = true;
    s2.possession_range_pct = 145; s2.keys_wasd = true; s2.keys_book_tab = true; s2.mode_menu = true;
    p2.window_width = 2560; p2.window_height = 1440; p2.fullscreen = 1; p2.display = 1; p2.vsync = false;
    p2.sound = false; p2.sound_volume = 200; p2.music = "midi"; p2.music_volume = 300; p2.tick_hz = 30.5;
    p2.movie_dir = "D:/CARPET/INTRO"; p2.save_slot[3] = {SDL_SCANCODE_F3, KEYMOD_ALT}; p2.quit_now = {};
    p2.pad.deadzone_left = 0.3f; p2.pad.invert_y = true; p2.pad.cursor_speed = 1234;
    p2.pad.bind[PAD_A] = PadAction{}; p2.pad.bind[PAD_A].kind = PadAction::SAVE_QUICK;
    CHECK(config_parse_pad_action("Ctrl+5", &p2.pad.bind[PAD_DPAD_LEFT]));
    const fs::path f2 = g_tmp / "values.ini";
    CHECK(config_write_file(f2.string().c_str(), s2, p2));
    PortSettings s3{};
    PlatformOptions p3;
    warn.clear();
    CHECK(config_read_file(f2.string().c_str(), &s3, &p3, &warn, &missing));
    CHECK(warn.empty() && missing.empty());
    int keys = 0;
    for (const std::string &k : config_keys()) {
        keys++;
        CHECK_STR(config_get(k.c_str(), s3, p3), config_get(k.c_str(), s2, p2));
    }
    std::printf("config file: %d keys round-trip\n", keys);

    // unknown keys, malformed values, clamping, a section-less "section.key"
    const fs::path f3 = g_tmp / "odd.ini";
    put_file(f3, "[render]\nfoo = 3\ndraw_distance = 500\nextended = maybe\n[game]\nthing_slots = 10\n"
                 "tick_hz = abc\nrender.lod = 0\n[nosuch]\nx=1\nno equals sign\n");
    PortSettings s4;
    PlatformOptions p4;
    warn.clear();
    CHECK(config_read_file(f3.string().c_str(), &s4, &p4, &warn, &missing));
    CHECK_EQ(s4.draw_distance, 255);       // 8..255 (render_ext uses at most 127 in a 256-cell world)
    CHECK_EQ(s4.thing_slots, 1000);
    CHECK_EQ(s4.lod, 0);
    CHECK_EQ(s4.render_extended, false);                       // malformed: unchanged
    CHECK(p4.tick_hz == 25.0);
    CHECK_EQ(warn.size(), 7u);
    for (const std::string &w : warn) std::printf("  warning (expected): %s\n", w.c_str());
    std::string w;
    CHECK(config_apply("pacing.fps_cap", "-5", &s4, &p4, &w) && s4.fps_cap == 0 && !w.empty());
    CHECK(config_apply("audio.music", "0", &s4, &p4, &w) && p4.music == "off");
    CHECK(!config_apply("audio.music", "trumpet", &s4, &p4, &w));
    CHECK(!config_apply("render.nothing", "1", &s4, &p4, &w));
}

// ---- 3 ----------------------------------------------------------------------------------------------------
static void test_load() {
    const fs::path dir = g_tmp / "save";
    fs::create_directories(dir);
    fs::remove(dir / "mcport.ini");
    set_env("MC_TICK_HZ", nullptr);
    set_env("MC_FAITHFUL", nullptr);
    set_env("MC_MUSIC", nullptr);
    {   // first run: the file is created, the playing defaults are in effect
        char a0[] = "mcport", a1[] = "C:/game", a2[] = "play", a3[] = "5";
        char *argv[] = {a0, a1, a2, a3, nullptr};
        int argc = 4;
        PortSettings s; PlatformOptions p;
        CHECK(config_load(dir.string().c_str(), &argc, argv, &s, &p));
        CHECK(fs::exists(dir / "mcport.ini"));
        CHECK_EQ(argc, 4);
        PortSettings want;
        config_playing_defaults(&want);
        CHECK(settings_equal(s, want));
        CHECK(!p.faithful);
        CHECK(s.mode_menu);                                     // round 12: the Conquest urn is on when playing
    }
    // an old file without most keys, plus an unknown one: the missing keys are appended, the unknown one kept
    put_file(dir / "mcport.ini", "# mine\n[game]\ntick_hz = 30\nmy_own_key = 7\n[render]\ndraw_distance = 50\n");
    set_env("MC_TICK_HZ", "40");
    {
        char a0[] = "mcport", a1[] = "C:/game", a2[] = "--set", a3[] = "game.tick_hz=50", a4[] = "play",
             a5[] = "--set=render.lod=0", a6[] = "3";
        char *argv[] = {a0, a1, a2, a3, a4, a5, a6, nullptr};
        int argc = 7;
        PortSettings s; PlatformOptions p;
        CHECK(config_load(dir.string().c_str(), &argc, argv, &s, &p));
        CHECK(p.tick_hz == 50.0);                               // command line > env > file
        CHECK_EQ(s.draw_distance, 50);                          // file > default
        CHECK_EQ(s.lod, 0);
        CHECK_EQ(argc, 4);
        CHECK(std::strcmp(argv[1], "C:/game") == 0 && std::strcmp(argv[2], "play") == 0 && std::strcmp(argv[3], "3") == 0);
        CHECK(std::strcmp(std::getenv("MC_TICK_HZ"), "50") == 0);   // exported for the old readers
        CHECK_EQ(p.warnings.size(), 1u);                        // the unknown key
        const std::string text = slurp(dir / "mcport.ini");
        CHECK(text.find("my_own_key = 7") != std::string::npos);
        CHECK(text.find("# game.thing_slots = 8192") != std::string::npos);   // appended, commented out
        CHECK(text.find("tick_hz = 30") != std::string::npos);
        PortSettings s2; PlatformOptions p2;                     // the appended file reads without "missing"
        std::vector<std::string> warn, missing;
        CHECK(config_read_file((dir / "mcport.ini").string().c_str(), &s2, &p2, &warn, &missing));
        CHECK(missing.empty());
    }
    {   // env < command line without --set: env wins over the file
        set_env("MC_TICK_HZ", "40");                            // (config_load exported 50 above)
        char a0[] = "mcport";
        char *argv[] = {a0, nullptr};
        int argc = 1;
        PortSettings s; PlatformOptions p;
        CHECK(config_load(dir.string().c_str(), &argc, argv, &s, &p));
        CHECK(p.tick_hz == 40.0);
    }
    set_env("MC_TICK_HZ", nullptr);
    {   // --faithful in command-line order
        char a0[] = "mcport", a1[] = "--set", a2[] = "render.draw_distance=99", a3[] = "--faithful", a4[] = "--set",
             a5[] = "pacing.fps_cap=60";
        char *argv[] = {a0, a1, a2, a3, a4, a5, nullptr};
        int argc = 6;
        PortSettings s; PlatformOptions p;
        CHECK(config_load(dir.string().c_str(), &argc, argv, &s, &p));
        PortSettings faithful;
        faithful.fps_cap = 60;
        CHECK(settings_equal(s, faithful));
        CHECK(p.faithful);
        CHECK(!s.mode_menu);                                    // --faithful: the original's menus
        CHECK_EQ(argc, 1);
        CHECK(p.tick_hz == 30.0);                               // platform options are not touched by --faithful
    }
    {   // MC_FAITHFUL=1, then a --set on top
        set_env("MC_FAITHFUL", "1");
        char a0[] = "mcport", a1[] = "--set", a2[] = "game.thing_slots=2000";
        char *argv[] = {a0, a1, a2, nullptr};
        int argc = 3;
        PortSettings s; PlatformOptions p;
        CHECK(config_load(dir.string().c_str(), &argc, argv, &s, &p));
        PortSettings want;
        want.thing_slots = 2000;
        CHECK(settings_equal(s, want));
        set_env("MC_FAITHFUL", nullptr);
    }
    {   // malformed command line
        char a0[] = "mcport", a1[] = "--bogus", a2[] = "--set", a3[] = "novalue";
        char *argv[] = {a0, a1, a2, a3, nullptr};
        int argc = 4;
        PortSettings s; PlatformOptions p;
        CHECK(!config_load(dir.string().c_str(), &argc, argv, &s, &p));
    }
    {   // --config: another file
        put_file(g_tmp / "other.ini", "[render]\ndraw_distance = 33\n");
        const std::string other = (g_tmp / "other.ini").string();
        char a0[] = "mcport", a1[] = "--config";
        std::vector<char> a2(other.begin(), other.end());
        a2.push_back(0);
        char *argv[] = {a0, a1, a2.data(), nullptr};
        int argc = 3;
        PortSettings s; PlatformOptions p;
        CHECK(config_load(dir.string().c_str(), &argc, argv, &s, &p));
        CHECK_EQ(s.draw_distance, 33);
        CHECK_EQ(argc, 1);
    }
    // the port's own keys
    PlatformOptions p;
    int slot = -1;
    CHECK(config_key_action(p, SDL_SCANCODE_F10, KEYMOD_CTRL, &slot) == PORT_KEY_SAVE && slot == 0);
    CHECK(config_key_action(p, SDL_SCANCODE_F10, KEYMOD_SHIFT, &slot) == PORT_KEY_LOAD && slot == 0);
    CHECK(config_key_action(p, SDL_SCANCODE_F10, 0, &slot) == PORT_KEY_NONE);         // the game's 3D-mode key
    CHECK(config_key_action(p, SDL_SCANCODE_F3, KEYMOD_CTRL, &slot) == PORT_KEY_SAVE && slot == 3);
    CHECK(config_key_action(p, SDL_SCANCODE_F9, KEYMOD_SHIFT, &slot) == PORT_KEY_LOAD && slot == 9);
    CHECK(config_key_action(p, SDL_SCANCODE_F12, 0, &slot) == PORT_KEY_QUIT);
    CHECK(config_key_action(p, SDL_SCANCODE_F5, 0, &slot) == PORT_KEY_NONE);          // the game's reflections key
    CHECK(config_key_action(p, SDL_SCANCODE_F1, KEYMOD_ALT, &slot) == PORT_KEY_NONE); // the game's cheat 1
    std::printf("config_load: precedence, --faithful, --config, argv, key actions checked\n");
}

// ---- 4 ----------------------------------------------------------------------------------------------------
static bool has_event(const GamepadFrame &f, int sc, bool down) {
    for (int i = 0; i < f.event_count; i++) if (f.events[i].scancode == sc && f.events[i].down == down) return true;
    return false;
}

static void test_pad() {
    GamepadConfig cfg;
    GamepadMapper m;
    GamepadRaw r;
    r.connected = true;
    // dead zone: a small deflection does nothing
    r.rx = 0.1f; r.lx = 0.2f;
    GamepadFrame f = m.update(r, PAD_CTX_FLIGHT, 0.016, cfg);
    CHECK(!f.steer_active && f.event_count == 0 && !f.any_input);
    // steering: full right = +1, the curve shapes the middle
    r.rx = 1.0f; r.ry = 0;
    f = m.update(r, PAD_CTX_FLIGHT, 0.016, cfg);
    CHECK(f.steer_active && f.steer_x > 0.99f && f.steer_y == 0.0f);
    r.rx = 0.56f;
    f = m.update(r, PAD_CTX_FLIGHT, 0.016, cfg);
    CHECK(f.steer_active && f.steer_x > 0.2f && f.steer_x < 0.45f);       // ((0.56-0.12)/0.88)^1.5 = 0.354
    r.rx = 0;
    f = m.update(r, PAD_CTX_FLIGHT, 0.016, cfg);
    CHECK(!f.steer_active && f.steer_released);
    // left stick: held arrows, one event per transition
    r.ly = -0.9f;
    f = m.update(r, PAD_CTX_FLIGHT, 0.016, cfg);
    CHECK(has_event(f, SDL_SCANCODE_UP, true) && f.event_count == 1);
    f = m.update(r, PAD_CTX_FLIGHT, 0.016, cfg);
    CHECK_EQ(f.event_count, 0);
    r.ly = 0; r.lx = 0.8f;
    f = m.update(r, PAD_CTX_FLIGHT, 0.016, cfg);
    CHECK(has_event(f, SDL_SCANCODE_UP, false) && has_event(f, SDL_SCANCODE_RIGHT, true));
    r.lx = 0;
    f = m.update(r, PAD_CTX_FLIGHT, 0.016, cfg);
    CHECK(has_event(f, SDL_SCANCODE_RIGHT, false));
    // A = Enter while held; the release follows the button even when the context changed meanwhile
    r.button[PAD_A] = true;
    f = m.update(r, PAD_CTX_FLIGHT, 0.016, cfg);
    CHECK(has_event(f, SDL_SCANCODE_RETURN, true));
    f = m.update(r, PAD_CTX_BOOK, 0.016, cfg);
    CHECK_EQ(f.event_count, 0);
    r.button[PAD_A] = false;
    f = m.update(r, PAD_CTX_BOOK, 0.016, cfg);
    CHECK(has_event(f, SDL_SCANCODE_RETURN, false) && !f.mouse_l);
    // book: A = left click, B = Enter, the stick moves the pointer
    r.button[PAD_A] = true;
    f = m.update(r, PAD_CTX_BOOK, 0.016, cfg);
    CHECK(f.mouse_l && f.event_count == 0);
    r.button[PAD_A] = false; r.button[PAD_B] = true;
    f = m.update(r, PAD_CTX_BOOK, 0.016, cfg);
    CHECK(!f.mouse_l && has_event(f, SDL_SCANCODE_RETURN, true));
    r.button[PAD_B] = false;
    f = m.update(r, PAD_CTX_BOOK, 0.016, cfg);
    CHECK(has_event(f, SDL_SCANCODE_RETURN, false));
    r.lx = 1.0f;
    f = m.update(r, PAD_CTX_BOOK, 0.5, cfg);
    CHECK(f.cursor_dx > 340 && f.cursor_dx < 360 && f.cursor_dy == 0 && f.event_count == 0);
    r.lx = 0; r.button[PAD_DPAD_UP] = true;
    f = m.update(r, PAD_CTX_MENU, 0.1, cfg);
    CHECK(f.cursor_dy < -69 && f.cursor_dy > -71);
    r.button[PAD_DPAD_UP] = false;
    r.button[PAD_B] = true;
    f = m.update(r, PAD_CTX_MENU, 0.016, cfg);
    CHECK(has_event(f, SDL_SCANCODE_ESCAPE, true));
    r.button[PAD_B] = false;
    m.update(r, PAD_CTX_MENU, 0.016, cfg);
    // triggers cast in every context
    r.rt = 0.8f;
    f = m.update(r, PAD_CTX_FLIGHT, 0.016, cfg);
    CHECK(f.mouse_r && !f.mouse_l);
    r.rt = 0.1f;
    f = m.update(r, PAD_CTX_FLIGHT, 0.016, cfg);
    CHECK(!f.mouse_r);
    // quick-select cycling: RB = next (1, 2, ...), LB = previous, X / Y with Ctrl (right hand)
    for (int i = 0; i < 2; i++) {
        r.button[PAD_RB] = true;
        f = m.update(r, PAD_CTX_FLIGHT, 0.016, cfg);
        CHECK(has_event(f, i == 0 ? SDL_SCANCODE_1 : SDL_SCANCODE_2, true));
        r.button[PAD_RB] = false;
        f = m.update(r, PAD_CTX_FLIGHT, 0.016, cfg);
        CHECK(has_event(f, i == 0 ? SDL_SCANCODE_1 : SDL_SCANCODE_2, false));
    }
    r.button[PAD_X] = true;                                     // previous in the right hand: from none -> key 0
    f = m.update(r, PAD_CTX_FLIGHT, 0.016, cfg);
    CHECK(f.event_count == 2 && f.events[0].scancode == SDL_SCANCODE_LCTRL && f.events[1].scancode == SDL_SCANCODE_0);
    r.button[PAD_X] = false;
    f = m.update(r, PAD_CTX_FLIGHT, 0.016, cfg);
    CHECK(f.event_count == 2 && has_event(f, SDL_SCANCODE_0, false) && has_event(f, SDL_SCANCODE_LCTRL, false));
    // a bound chord, a port action
    CHECK(config_parse_pad_action("Shift+R", &cfg.bind[PAD_Y]));
    cfg.bind[PAD_GUIDE].kind = PadAction::SAVE_QUICK;
    r.button[PAD_Y] = r.button[PAD_GUIDE] = true;
    f = m.update(r, PAD_CTX_FLIGHT, 0.016, cfg);
    CHECK(has_event(f, SDL_SCANCODE_LSHIFT, true) && has_event(f, SDL_SCANCODE_R, true));
    CHECK(f.port_action == GamepadFrame::PORT_SAVE_QUICK);
    // unplugged while held: everything is released
    r.ly = -1.0f;
    m.update(r, PAD_CTX_FLIGHT, 0.016, cfg);
    GamepadRaw gone;
    f = m.update(gone, PAD_CTX_FLIGHT, 0.016, cfg);
    CHECK(has_event(f, SDL_SCANCODE_R, false) && has_event(f, SDL_SCANCODE_LSHIFT, false) && has_event(f, SDL_SCANCODE_UP, false));
    f = m.update(gone, PAD_CTX_FLIGHT, 0.016, cfg);
    CHECK_EQ(f.event_count, 0);
    std::printf("gamepad mapping checked\n");
}

// ---- 5..7 (game data) ----------------------------------------------------------------------------------------
static uint32_t fnv(const void *p, size_t n, uint32_t h) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}
static uint32_t state_checksum() {
    uint32_t h = fnv(g_state, sizeof(GameState), 2166136261u);
    h = fnv(g_cfg, sizeof(Config), h);
    h = fnv(g_map_type, 0x10000, h);
    h = fnv(g_map_height, 0x10000, h);
    h = fnv(g_map_flags, 0x10000, h);
    h = fnv(g_cell_things, 0x20000, h);
    const int n = thing_pool_slots();
    if (n > MC_THING_SLOTS) {
        h = fnv(thing_at(MC_THING_SLOTS), sizeof(Thing) * (size_t)(n - MC_THING_SLOTS), h);
        h = fnv(thing_pool_ext_free_stack(), 4u * (size_t)(n - MC_THING_SLOTS), h);
        h = fnv(thing_pool_ext_active_stack(), 4u * (size_t)(n - MC_THING_SLOTS), h);
    }
    return h;
}
static std::vector<uint32_t> run_ticks(int n) {
    std::vector<uint32_t> r;
    for (int i = 0; i < n; i++) { game_tick_sim(); r.push_back(state_checksum()); }
    return r;
}
static bool load_level(int level, int slots) {
    g_settings.thing_slots = slots;
    g_cfg->flags = 0; g_cfg->paused = 0;
    return sim_load_level(level);
}
static int first_diff(const std::vector<uint32_t> &a, const std::vector<uint32_t> &b) {
    for (size_t i = 0; i < a.size() && i < b.size(); i++) if (a[i] != b[i]) return (int)i;
    return a.size() == b.size() ? -1 : (int)(std::min)(a.size(), b.size());
}

// Round 9: a save state records the gameplay rules; loading it with other settings plays on with the saved
// rules (identical ticks) until the next level releases them.
static void rules_state(int level) {
    g_settings.possession_range_pct = 150;
    CHECK(load_level(level, 1000));
    run_ticks(100);
    const uint32_t at_save = state_checksum();
    CHECK(savestate_save(5, nullptr));
    const std::vector<uint32_t> a = run_ticks(300);
    g_settings.possession_range_pct = 100;
    CHECK(load_level(level, 1000));
    CHECK(savestate_load(5));
    CHECK_EQ(state_checksum(), at_save);
    CHECK(gameplay_rules_forced());
    CHECK_EQ(gameplay_rules().possession_range_pct, 150);
    const std::vector<uint32_t> b = run_ticks(300);
    const int d = first_diff(a, b);
    std::printf("rules state: saved with possession 150 %%, loaded with the setting at 100: %s\n", d < 0 ? "identical" : "DIFFER");
    CHECK(d < 0);
    gameplay_force_rules(nullptr);                              // what game_after_frontend does at the next level
    CHECK_EQ(gameplay_rules().possession_range_pct, 100);
}

static void save_anywhere(int level, int slots, int before, int after) {
    if (!load_level(level, slots)) { std::printf("FAIL level %d did not load\n", level); g_fail++; return; }
    run_ticks(before);
    const uint32_t at_save = state_checksum();
    int live = 0, live_ext = 0;
    for (int i = 1; i < thing_pool_slots(); i++) {
        const bool on = thing_at((unsigned)i)->cls != 0;
        live += on;
        live_ext += on && i >= MC_THING_SLOTS;
    }
    if (slots > MC_THING_SLOTS) CHECK(live_ext > 0);
    CHECK(savestate_allowed());
    CHECK(savestate_save(4, nullptr));
    SaveStateHeader h{};
    char path[1024];
    CHECK(savestate_slot_path(4, path, sizeof path) && savestate_read_header(path, &h));
    CHECK_EQ(h.level, level);
    CHECK_EQ(h.thing_slots, slots);
    CHECK_EQ(h.tick, g_state->players[g_state->local_player & 7].tick);
    const std::vector<uint32_t> a = run_ticks(after);
    CHECK(savestate_load(4));
    CHECK_EQ(state_checksum(), at_save);
    const std::vector<uint32_t> b = run_ticks(after);
    // a restart: another level in between, then the level again and the state
    g_settings.thing_slots = 1000;
    CHECK(load_level(level == 3 ? 4 : 3, 1000));
    run_ticks(20);
    CHECK(load_level(level, 1000));                            // the save brings its own pool size back
    CHECK(savestate_load(4));
    CHECK_EQ(thing_pool_slots(), slots);
    CHECK_EQ(state_checksum(), at_save);
    const std::vector<uint32_t> c = run_ticks(after);
    const int d1 = first_diff(a, b), d2 = first_diff(a, c);
    std::printf("save anywhere, level %d, %d slots: saved at tick %d (%d live Things, %d in the extension), %d ticks after the load: %s / after a restart: %s\n",
                level, slots, before, live, live_ext, after, d1 < 0 ? "identical" : "DIFFER", d2 < 0 ? "identical" : "DIFFER");
    if (d1 >= 0) std::printf("  first difference after %d ticks\n", d1 + 1);
    if (d2 >= 0) std::printf("  first difference after %d ticks (restart)\n", d2 + 1);
    CHECK(d1 < 0);
    CHECK(d2 < 0);
    g_settings.thing_slots = 1000;
}

static int count_list_problems() {
    // demo_repair_cell_lists on a copy: count without changing anything
    std::vector<uint16_t> heads(g_cell_things, g_cell_things + MC_MAP_CELLS);
    std::vector<Thing> things;
    for (int i = 0; i < thing_pool_slots(); i++) things.push_back(*thing_at((unsigned)i));
    const int n = demo_repair_cell_lists();
    std::memcpy(g_cell_things, heads.data(), heads.size() * 2);
    for (int i = 0; i < thing_pool_slots(); i++) *thing_at((unsigned)i) = things[(size_t)i];
    return n;
}

static void quick_save_full(int level) {
    CHECK(load_level(level, 1000));
    run_ticks(150);
    savestate_install_quick_hooks();
    // the original's quick save / load (commands 10 / 0xb) for comparison: lists after a load 100 ticks later
    g_settings.quicksave_full = false;
    CHECK(demo_save_state(10000));
    run_ticks(100);
    CHECK(demo_load_state(10000));
    const int orig_problems = count_list_problems();
    std::printf("original quick load (gam10000.dat) on level %d: %d cell-list problems after the load\n", level, orig_problems);
    // full: commands 10 / 0xb go to save slot 0
    CHECK(load_level(level, 1000));
    run_ticks(150);
    g_settings.quicksave_full = true;
    const uint32_t at_save = state_checksum();
    char path[1024];
    CHECK(savestate_slot_path(0, path, sizeof path));
    std::remove(path);
    CHECK(demo_save_state(10000));
    SaveStateHeader h{};
    CHECK(savestate_read_header(path, &h));
    const std::vector<uint32_t> a = run_ticks(100);
    CHECK(demo_load_state(10000));
    CHECK_EQ(state_checksum(), at_save);
    CHECK_EQ(count_list_problems(), 0);
    const std::vector<uint32_t> b = run_ticks(100);
    CHECK(first_diff(a, b) < 0);
    g_settings.quicksave_full = false;
    g_hook_demo_quick_save = nullptr;
    g_hook_demo_quick_load = nullptr;
    std::printf("full quick save (commands 10 / 0xb -> slot 0): load identical, 0 list problems, 100 ticks identical\n");
}

static void cell_repair(int level) {
    CHECK(load_level(level, 1000));
    run_ticks(50);
    CHECK_EQ(demo_repair_cell_lists(), 0);
    // find a cell with two Things and make a cycle
    int cell = -1;
    for (int c = 0; c < MC_MAP_CELLS && cell < 0; c++) {
        const unsigned h = g_cell_things[c];
        if (h && thing_at(h)->cell_next) cell = c;
    }
    CHECK(cell >= 0);
    if (cell < 0) return;
    const unsigned first = g_cell_things[cell], second = thing_at(first)->cell_next;
    thing_at(second)->cell_next = (uint16_t)first;              // first -> second -> first ...
    CHECK(demo_repair_cell_lists() > 0);
    CHECK_EQ(demo_repair_cell_lists(), 0);
    int guard = 0;
    for (unsigned i = g_cell_things[cell]; i; i = thing_at(i)->cell_next) CHECK(++guard < 1000);
    // a stale head (what the original's quick load leaves): rebuilt from the Things' own links = unchanged lists
    CHECK(load_level(level, 1000));
    run_ticks(50);
    const uint32_t clean = state_checksum();
    std::vector<uint16_t> heads(g_cell_things, g_cell_things + MC_MAP_CELLS);
    g_cell_things[cell] = 0;
    for (int c = 0; c < MC_MAP_CELLS; c += 97) if (!g_cell_things[c]) g_cell_things[c] = (uint16_t)first;
    CHECK(demo_repair_cell_lists() > 0);
    CHECK(std::memcmp(heads.data(), g_cell_things, heads.size() * 2) == 0);
    CHECK_EQ(state_checksum(), clean);
    std::printf("cell-list repair: a cycle and stale heads repaired\n");
}

static void ext_movie(int level) {
    const fs::path rec = g_tmp / "rec";
    CHECK(load_level(level, 3000));
    g_cfg->movie = 7;
    g_cfg->flags |= 2;                                          // command 0xc: record from the next packet
    for (int i = 0; i < 120; i++) game_tick_sim();
    CHECK(demo_recording());
    CHECK(demo_extended());
    const long written = demo_packets_written();
    demo_close();
    CHECK(fs::exists(rec / "movie" / "mvx00007.dat") && fs::exists(rec / "movie" / "gax00007.dat") &&
          fs::exists(rec / "movie" / "max00007.dat"));
    CHECK(!fs::exists(rec / "movie" / "mvi00007.dat"));
    const std::string mvx = slurp(rec / "movie" / "mvx00007.dat");
    CHECK(mvx.size() == sizeof(DemoExtHeader) + (size_t)written * sizeof(CmdPacket));
    CHECK(mvx.compare(0, 4, "MCPX") == 0);
    // playback with the faithful setting: the recorded pool size is used
    CHECK(load_level(level, 1000));
    CHECK(demo_open(g_tmp.string().c_str(), 7));
    CHECK(demo_extended());
    int ticks = 0;
    while (demo_step() && ticks < 1000) ticks++;
    CHECK_EQ(thing_pool_slots(), 3000);
    CHECK(demo_packets_read() >= written - 8);
    std::printf("extended-pool movie: %ld packets recorded as mvx/gax/max (pool 3000), played back %d ticks with a 1000-slot setting\n",
                written, ticks);
    demo_close();
    CHECK_EQ(thing_pool_wanted_slots(), 1000);                  // the override is gone
    // a damaged header is refused
    std::string bad = mvx;
    bad[8] = '\xe7'; bad[9] = 3; bad[10] = 0; bad[11] = 0;  // 999 slots
    put_file(rec / "movie" / "mvx00008.dat", bad);
    CHECK(!demo_open(g_tmp.string().c_str(), 8));
    demo_set_record_dir("");
}

// A movie recorded with non-faithful gameplay rules (possession_range_pct 150): mvx version 2 with the rules
// in its header, played back with the setting at 100 under the recorded rules - the same GameState every
// tick; the original's mvi movie 0 plays with faithful rules whatever the setting.
static void rules_movie(int level, const char *game_dir) {
    const fs::path rec = g_tmp / "rec";
    demo_set_record_dir(rec.string().c_str());
    g_settings.possession_range_pct = 150;
    CHECK(load_level(level, 1000));
    g_cfg->movie = 9;
    g_cfg->flags |= 2;
    std::vector<uint32_t> recorded, played;
    for (int i = 0; i < 300; i++) {
        game_tick_sim();
        recorded.push_back(fnv(g_state, sizeof(GameState), 2166136261u));
    }
    CHECK(demo_recording());
    CHECK(demo_extended());
    const long written = demo_packets_written();
    demo_close();
    const std::string mvx = slurp(rec / "movie" / "mvx00009.dat");
    CHECK(mvx.size() == sizeof(DemoExtHeader) + sizeof(DemoExtRules) + (size_t)written * sizeof(CmdPacket));
    DemoExtHeader h{};
    DemoExtRules r{};
    if (mvx.size() >= sizeof h + sizeof r) {
        std::memcpy(&h, mvx.data(), sizeof h);
        std::memcpy(&r, mvx.data() + sizeof h, sizeof r);
    }
    CHECK(h.version == 2 && h.thing_slots == 1000 && r.possession_range_pct == 150);
    g_settings.possession_range_pct = 100;
    CHECK(load_level(level, 1000));
    CHECK(demo_open(g_tmp.string().c_str(), 9));
    CHECK_EQ(gameplay_rules().possession_range_pct, 150);
    while (demo_step() && played.size() < 1000) played.push_back(fnv(g_state, sizeof(GameState), 2166136261u));
    demo_close();
    CHECK_EQ(gameplay_rules().possession_range_pct, 100);       // the override is gone
    // the playback starts at the recording's first packet (tick 1 of the recording)
    int match = -2;
    for (int off = 0; off < 3 && match == -2; off++) {
        std::vector<uint32_t> tail(recorded.begin() + off, recorded.end());
        const size_t n = (std::min)(tail.size(), played.size());
        if (n > 200 && std::equal(tail.begin(), tail.begin() + (long)n, played.begin())) match = off;
    }
    std::printf("rules movie: %ld packets recorded as mvx v2 (possession 150 %%), played back %zu ticks with the "
                "setting at 100: %s\n", written, played.size(), match >= 0 ? "identical" : "DIVERGED");
    CHECK(match >= 0);
    // the original's movie: faithful rules
    g_settings.possession_range_pct = 130;
    if (demo_open(game_dir, 0)) {
        CHECK(!demo_extended());
        CHECK_EQ(gameplay_rules().possession_range_pct, 100);
        demo_close();
        CHECK_EQ(gameplay_rules().possession_range_pct, 130);
    }
    g_settings.possession_range_pct = 100;
    demo_set_record_dir("");
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    g_tmp = fs::temp_directory_path() / "mc_config_test";
    std::error_code ec;
    fs::remove_all(g_tmp, ec);
    fs::create_directories(g_tmp);
    test_keys();
    test_file();
    test_load();
    test_pad();

    const char *game_dir = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    if (!sim_init(game_dir)) {
        std::printf("SKIP save anywhere: no game data in %s\n", game_dir);
    } else {
        sim_register_gameplay();
        savegame_set_dirs(g_tmp.string().c_str(), nullptr);
        fs::create_directories(g_tmp / "rec" / "movie");
        demo_set_record_dir((g_tmp / "rec").string().c_str());
        save_anywhere(3, 1000, 300, 300);
        save_anywhere(44, 1000, 400, 300);
        save_anywhere(49, 1000, 100, 200);                      // a full pool (free stack empty)
        save_anywhere(49, 4000, 100, 200);                      // Things in the pool extension
        quick_save_full(1);
        cell_repair(44);
        ext_movie(2);
        rules_movie(2, game_dir);
        rules_state(2);
    }
    g_settings = PortSettings{};
    if (g_fail) { std::printf("config_test: %d FAILED\n", g_fail); return 1; }
    std::printf("config_test: all passed\n");
    return 0;
}
