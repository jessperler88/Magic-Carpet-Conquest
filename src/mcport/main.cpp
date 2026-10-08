// mcport - native Magic Carpet port. Phase 2 state (round 6): the whole game as the original runs it -
// front end, campaign levels, result screen, save games, sound and music - plus a free-fly viewer.
//
//   mcport [game_dir]               the game: language / logos / main menu, levels, results (as carpet.exe)
//   mcport [game_dir] play [level]  straight into campaign level 0..69, then on through the front end
//   mcport [game_dir] [level]       free-fly viewer over level 0..69 (the simulation runs, nobody steers)
//   mcport [game_dir] demo [n]      play movie/mviNNNNN.dat from its state snapshot (default 0; recordings
//                                   made with Alt+R are found in the save directory first)
//   mcport [game_dir] network       the game with the multiplayer lobby (main-menu item 3); MC_NET_HOST /
//                                   MC_NET_PORT select the name server (the first instance on a machine
//                                   becomes it), MC_NET_SYNC=n compares the game state every n ticks (0 off)
//   mcport [game_dir] rts [options] a Conquest mode run (rts_run.h); `rts --net N ...` = N humans over the
//                                   network without the lobby (every instance: the same command, MC_NET=1)
//
// In a level: the mouse steers (pointer offset from the screen centre), Up / Down fly faster / slower,
// Left / Right slide, left / right mouse button cast the spell in that hand, 1..0 (Ctrl+1..0) put a
// quick-select spell into the left (right) hand, Enter opens / closes the spell book, Space respawns /
// leaves a won level, P pauses, R toggles 320x200 / 640x480, F1 / F2 sound / music, F3 game speed,
// F4..F9 render options, [ ] view size, Esc opens the pause menu (keys.menu = none: the original Esc, leave the level), Shift+Q quits. Full table:
// docs/analysis/port_input.md. F12 quits the port at once.
//
// viewer: W/S forward/back, A/D strafe, Q/E or mouse (right button held) yaw, R/F up/down,
// Up/Down pitch, Z/X roll, +/- zoom, L/K next/previous level, Home = reset camera,
// F2 shadows, F4 second surface, F5 textured sky, F8 smoothing, Esc quit.
// Space pauses the game ticks. Demo mode: Tab toggles between the player's camera and the free
// camera, [ / ] halve / double the playback rate.
//
// Pacing: the original has none - one simulation tick per rendered frame, as fast as the machine
// draws (docs/analysis/port_game.md "Pacing"). The port ticks at a fixed rate (MC_TICK_HZ, default 25)
// and draws every frame; while a palette fade runs (the original blocks inside it) nothing ticks and
// the fade steps at the VGA's 70 Hz. The front end runs at 70 frames per second.
// Phase 3 (pacing.h, docs/analysis/port_pacing.md): g_settings.interpolate (env MC_INTERPOLATE=1) draws
// the 3D view between the last two ticks (camera + Things, render-time only); g_settings.fps_cap (env
// MC_FPS_CAP=n, 0 = uncapped / vsync) limits the frame rate. The title bar shows fps, the average /
// worst frame time and the tick rate. Port-only test switches: MC_TICK_LOG=<file> appends
// "<tick> <net_state_checksum>" after every simulation tick (and the frame counts at exit),
// MC_QUIT_AFTER_TICKS=n quits after n ticks.
// Phase 4 round 10 (debug suite, docs/analysis/port_timectl.md): time control in every mode (not a network game)
// - Pause pause / resume, End one tick (Shift+End ten), PageUp / PageDown faster / slower (x1/16 .. x64, then
// as fast as possible), Insert normal speed; in a level Delete = free debug camera (viewer keys, mouse looks;
// the game's keys are not fed), Shift+Delete = the wizard to the camera (debug packet); Ctrl+F11 = screenshot
// (PNG in <save dir>/screenshots or MC_SHOT_DIR); F11 cycles the frame-time line / + the tick profile / off.
// The log: <save dir>/mcport.log (previous run mcport.1.log), MC_LOG_LEVEL=error|warn|info|debug.
// MC_TIME_SPEED=max|4|1/2 (starting speed), MC_TICK_PROFILE=n (profile ticks, a log line every n).
// Environment: MC_MOVIE_DIR (full movies, e.g. the CD's CARPET/INTRO), MC_TICK_HZ, MC_SOUND=0, MC_SOUND_VOLUME, MC_MUSIC=opl (default: FM bank on the emulated
// OPL2) | midi (GM bank via the Windows MIDI mapper) | square | 0, MC_MUSIC_VOLUME (OPL gain, 256 = default).
#include "platform.h"
#include "audio_sdl.h"
#include "engine.h"
#include "mc_globals.h"
#include "mc_math.h"
#include "render.h"
#include "player.h"
#include "demo.h"
#include "sim.h"
#include "input.h"
#include "hud.h"
#include "ui_draw.h"
#include "sound.h"
#include "game.h"
#include "frontend.h"
#include "palette_fx.h"
#include "fli.h"
#include "mcfile.h"
#include "net.h"
#include "net_tcp.h"
#include "pacing.h"
#include "compose.h"
#include "settings.h"
#include "config.h"
#include "gamepad.h"
#include "savegame.h"
#include "game_menu.h"
#include "rts_run.h"
#include "replay_run.h"
#include "mode_level.h"
#include "mclog.h"
#include "timectl.h"
#include "screenshot.h"
#include "tick_profile.h"
#include "debug_camera.h"
#include "world.h"
#include "debug_cmd.h"
#include "console.h"
#include "debug_overlay.h"
#include "inspect_tool.h"
#include "world_set.h"
#include <SDL.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

static const double TIMER_HZ = 1193182.0 / 0x2726;  // the original PIT rate: 119.06 Hz (g_timer_ticks)
static const double CAMERA_HZ = TIMER_HZ;            // free camera steps
static const double VGA_HZ = 70.0;                   // palette fade steps (vsync) and front-end frames
static const double DEMO_TICK_HZ = 20.0;             // movie playback (recorded on mid-90s hardware)

// SDL scancode -> the DOS set-1 make code the game's input code tests (input.h), 0 = not used.
static int set1_scancode(int sdl) {
    if (sdl >= SDL_SCANCODE_1 && sdl <= SDL_SCANCODE_9) return MC_SC_1 + (sdl - SDL_SCANCODE_1);
    if (sdl >= SDL_SCANCODE_F1 && sdl <= SDL_SCANCODE_F10) return MC_SC_F1 + (sdl - SDL_SCANCODE_F1);
    switch (sdl) {
    case SDL_SCANCODE_0: return MC_SC_0;
    case SDL_SCANCODE_ESCAPE: return MC_SC_ESC;
    case SDL_SCANCODE_BACKSPACE: return MC_SC_BACKSPACE;
    case SDL_SCANCODE_TAB: return MC_SC_TAB;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return MC_SC_ENTER;
    case SDL_SCANCODE_LCTRL: return MC_SC_CTRL;
    case SDL_SCANCODE_RCTRL: return MC_SC_CTRL | MC_SC_EXT;
    case SDL_SCANCODE_LALT: return MC_SC_ALT;
    case SDL_SCANCODE_RALT: return MC_SC_ALT | MC_SC_EXT;
    case SDL_SCANCODE_LSHIFT: return MC_SC_LSHIFT;
    case SDL_SCANCODE_RSHIFT: return MC_SC_RSHIFT;
    case SDL_SCANCODE_SPACE: return MC_SC_SPACE;
    case SDL_SCANCODE_LEFTBRACKET: return MC_SC_LBRACKET;
    case SDL_SCANCODE_RIGHTBRACKET: return MC_SC_RBRACKET;
    case SDL_SCANCODE_UP: return MC_SC_UP | MC_SC_EXT;
    case SDL_SCANCODE_DOWN: return MC_SC_DOWN | MC_SC_EXT;
    case SDL_SCANCODE_LEFT: return MC_SC_LEFT | MC_SC_EXT;
    case SDL_SCANCODE_RIGHT: return MC_SC_RIGHT | MC_SC_EXT;
    case SDL_SCANCODE_Q: return MC_SC_Q; case SDL_SCANCODE_W: return MC_SC_W; case SDL_SCANCODE_E: return MC_SC_E;
    case SDL_SCANCODE_R: return MC_SC_R; case SDL_SCANCODE_T: return MC_SC_T; case SDL_SCANCODE_Y: return MC_SC_Y;
    case SDL_SCANCODE_U: return MC_SC_U; case SDL_SCANCODE_I: return MC_SC_I; case SDL_SCANCODE_O: return MC_SC_O;
    case SDL_SCANCODE_P: return MC_SC_P; case SDL_SCANCODE_A: return MC_SC_A; case SDL_SCANCODE_S: return MC_SC_S;
    case SDL_SCANCODE_D: return MC_SC_D; case SDL_SCANCODE_F: return MC_SC_F; case SDL_SCANCODE_G: return MC_SC_G;
    case SDL_SCANCODE_H: return MC_SC_H; case SDL_SCANCODE_J: return MC_SC_J; case SDL_SCANCODE_K: return MC_SC_K;
    case SDL_SCANCODE_L: return MC_SC_L; case SDL_SCANCODE_Z: return MC_SC_Z; case SDL_SCANCODE_X: return MC_SC_X;
    case SDL_SCANCODE_C: return MC_SC_C; case SDL_SCANCODE_V: return MC_SC_V; case SDL_SCANCODE_B: return MC_SC_B;
    case SDL_SCANCODE_N: return MC_SC_N; case SDL_SCANCODE_M: return MC_SC_M;
    default: return 0;
    }
}

static mc::Platform *s_plat = nullptr;
static std::string s_game;

// Height of the game's 640-wide virtual screen: 400 in the 320x200 mode, 480 otherwise (input.h).
static int virtual_h() { return g_video_mode_flags == 1 ? 400 : 480; }
static FrameBuffer frame() { return FrameBuffer{s_plat->fb, s_plat->fb_w, s_plat->fb_h}; }

// Round 7 (compose.h, task B): present the frame - composed (native view + HUD layer) or the original way.
static void present_frame() {
    if (compose_installed()) {
        int dw, dh;
        s_plat->output_size(&dw, &dh);
        s_plat->present_composed(compose_frame_for_present(dw, dh, frame()));
    } else {
        s_plat->present();
    }
}
// The game's 2D pass over the view: composed, it runs inside compose_draw_frame (twice when a view was
// placed - it must not write game state; render_frame_draw / overlay / pointer do not).
template <class F> static void draw_view_frame(F &&draw) {
    if (compose_installed()) {
        int dw, dh;
        s_plat->output_size(&dw, &dh);
        compose_draw_frame(dw, dh, frame(), draw);
    } else {
        draw();
    }
}

// g_hook_input_mouse_warp: the game moved its pointer (spell book closed): move the OS pointer too.
// In a level the OS mouse is relative (hidden, unbounded like the DOS mouse) and the pointer position
// lives here in the game's 640-wide virtual coordinates.
static int s_level_mx = 320, s_level_my = 200;
static bool s_level_mouse = false;
static void warp_pointer(int x, int y) {
    if (s_level_mouse) { s_level_mx = x; s_level_my = y; return; }
    if (s_plat) s_plat->warp_mouse(x * s_plat->fb_w / 640, y * s_plat->fb_h / virtual_h());
}

// The platform half of video_toggle_resolution_33600 (g_hook_game_video_mode_changed): frame buffer
// size, sprite / font tables of the new mode.
static void video_mode_changed() {
    const bool lo = (g_video_mode_flags & 1) != 0;
    s_plat->resize(lo ? 320 : 640, lo ? 200 : 480);
    ui_draw_set_video_mode(s_game.c_str());
}

// g_hook_input_platform: requests of the game's input code that the platform carries out.
static void platform_request(InputPlatformRequest what, int arg) {
    if (sound_handle_input_request(what, arg)) return;
    if (what == INPUT_REQ_TOGGLE_RESOLUTION) game_toggle_resolution();
}

static void upload_palette() {
    if (!palette_display_dirty()) return;
    uint8_t rgb[768];
    mc_palette_to_rgb(g_display_palette6, rgb);
    s_plat->set_palette(rgb);
    palette_display_clear_dirty();
}

// The presented frame as RGB: the composed image at the drawable size when composing, else the game frame.
static void capture_rgb(int *w, int *h, std::vector<uint8_t> *rgb) {
    *w = s_plat->fb_w; *h = s_plat->fb_h;
    if (compose_installed()) {
        const ComposeOutput &o = compose_output();
        *w = o.display_w; *h = o.display_h;
        rgb->resize((size_t)*w * *h * 3);
        compose_to_rgb(o, g_display_palette6, rgb->data());
    } else {
        rgb->resize((size_t)*w * *h * 3);
        for (int i = 0; i < *w * *h; i++) {
            const uint8_t *c = g_display_palette6 + s_plat->fb[i] * 3;
            for (int k = 0; k < 3; k++) (*rgb)[(size_t)i * 3 + k] = (uint8_t)(c[k] << 2 | c[k] >> 4);
        }
    }
}

// MC_SHOT=n[,m...]: write frame n (counted from start-up) as mcport_shot_<n>.ppm (headless checks).
static void maybe_screenshot(long frame_no) {
    // (SDL_getenv returns a buffer the next call overwrites on Windows: keep a copy)
    static const std::string spec_s = SDL_getenv("MC_SHOT") ? SDL_getenv("MC_SHOT") : "";
    const char *spec = spec_s.c_str();
    if (!*spec) return;
    for (const char *p = spec; *p; ) {
        char *end;
        long n = std::strtol(p, &end, 10);
        if (end == p) break;
        if (n == frame_no) {
            char name[64];
            std::snprintf(name, sizeof name, "mcport_shot_%ld.ppm", n);
            int w = 0, h = 0;
            std::vector<uint8_t> rgb;
            capture_rgb(&w, &h, &rgb);
            if (!shot_write_ppm(name, w, h, rgb.data())) mclog(MCLOG_WARN, "could not write %s", name);
        }
        p = *end == ',' ? end + 1 : end;
    }
}

// Round 10: `shot [name]` (console) / the screenshot key: the next presented frame as PNG (PPM for a .ppm
// name) in MC_SHOT_DIR, else <save dir>/screenshots (screenshot.h shot_resolve_path).
static std::vector<std::string> s_shot_requests;
static std::string s_shot_dir;
static int s_shot_counter = 0;
static void request_screenshot(const std::string &name) { s_shot_requests.push_back(name); }
static void write_requested_screenshots() {
    if (s_shot_requests.empty()) return;
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;
    capture_rgb(&w, &h, &rgb);
    for (const std::string &name : s_shot_requests) {
        const std::string path = shot_resolve_path(s_shot_dir, name, ++s_shot_counter);
        std::error_code ec;
        const std::filesystem::path parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
        const bool ppm = std::filesystem::path(path).extension() == ".ppm";
        if (ppm ? shot_write_ppm(path.c_str(), w, h, rgb.data()) : shot_write_png(path.c_str(), w, h, rgb.data()))
            mclog(MCLOG_INFO, "screenshot %dx%d: %s", w, h, path.c_str());
        else
            mclog(MCLOG_WARN, "screenshot: could not write %s", path.c_str());
    }
    s_shot_requests.clear();
}

// MC_TEST_INPUT="frame@x,y[c]+frame@kN+..." injects mouse events (window pixels; c = left click
// pressed this frame, released the next) through the SDL queue - scripted headless checks.
static void maybe_inject_input(long frame_no) {
    static const std::string spec_s = SDL_getenv("MC_TEST_INPUT") ? SDL_getenv("MC_TEST_INPUT") : "";
    static long release_at = -1;
    const char *spec = spec_s.c_str();
    if (!*spec) return;
    SDL_Window *win = SDL_GetKeyboardFocus() ? SDL_GetKeyboardFocus() : SDL_GetMouseFocus();
    if (!win) win = SDL_GetWindowFromID(1);
    auto push_button = [](bool down) {
        SDL_Event e{};
        e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
        SDL_PushEvent(&e);
    };
    if (frame_no == release_at) push_button(false);
    for (const char *p = spec; *p; ) {
        char *end;
        long n = std::strtol(p, &end, 10);
        if (end == p || *end != '@') break;
        if (end[1] == 'k') {                     // frame@kN: SDL scancode N pressed this frame, released the next
            const int sc = (int)std::strtol(end + 2, &end, 10);
            if (n == frame_no || n + 1 == frame_no) {
                SDL_Event e{};
                e.type = n == frame_no ? SDL_KEYDOWN : SDL_KEYUP;
                e.key.state = n == frame_no ? SDL_PRESSED : SDL_RELEASED;
                e.key.keysym.scancode = (SDL_Scancode)sc;
                SDL_PushEvent(&e);
            }
            p = *end == '+' ? end + 1 : end;
            continue;
        }
        int x = (int)std::strtol(end + 1, &end, 10);
        int y = *end == ',' ? (int)std::strtol(end + 1, &end, 10) : 0;
        bool click = false;
        if (*end == 'c') { click = true; end++; }
        if (n == frame_no) {
            SDL_Event e{};
            e.type = SDL_MOUSEMOTION;
            e.motion.windowID = win ? SDL_GetWindowID(win) : 0;
            e.motion.x = x;
            e.motion.y = y;
            SDL_PushEvent(&e);
            if (click) { push_button(true); release_at = frame_no + 1; }
        }
        p = *end == '+' ? end + 1 : end;
    }
}

static void free_camera_step(Camera &cam, const mc::Input &in) {
    int speed = in.keys[SDL_SCANCODE_LSHIFT] ? 48 : 12;
    if (in.keys[SDL_SCANCODE_LCTRL] || in.keys[SDL_SCANCODE_RCTRL]) speed *= 4;   // round 11: crossing a 1024+ world
    int fwd = (in.keys[SDL_SCANCODE_W] ? 1 : 0) - (in.keys[SDL_SCANCODE_S] ? 1 : 0);
    int side = (in.keys[SDL_SCANCODE_D] ? 1 : 0) - (in.keys[SDL_SCANCODE_A] ? 1 : 0);
    // yaw 0 looks towards -y, 0x200 towards +x (render.h); sin/cos are 16.16
    int dx = (int)(((int64_t)mc_sin(cam.yaw) * fwd + (int64_t)mc_cos(cam.yaw) * side) * speed >> 16);
    int dy = (int)(((int64_t)-mc_cos(cam.yaw) * fwd + (int64_t)mc_sin(cam.yaw) * side) * speed >> 16);
    cam.cam_x = world_wrap(cam.cam_x + dx);       // round 11: wraps at g_world.units (0xffff at 256 cells)
    cam.cam_y = world_wrap(cam.cam_y + dy);
    if (in.keys[SDL_SCANCODE_Q]) cam.yaw = (cam.yaw - 8) & MC_ANGLE_MASK;
    if (in.keys[SDL_SCANCODE_E]) cam.yaw = (cam.yaw + 8) & MC_ANGLE_MASK;
    if (in.keys[SDL_SCANCODE_R]) cam.cam_z += speed;
    if (in.keys[SDL_SCANCODE_F]) cam.cam_z -= speed;
    if (in.keys[SDL_SCANCODE_UP]) cam.pitch += 2;
    if (in.keys[SDL_SCANCODE_DOWN]) cam.pitch -= 2;
    if (in.keys[SDL_SCANCODE_Z]) cam.roll = (cam.roll - 4) & MC_ANGLE_MASK;
    if (in.keys[SDL_SCANCODE_X]) cam.roll = (cam.roll + 4) & MC_ANGLE_MASK;
    if (in.keys[SDL_SCANCODE_EQUALS]) cam.zoom += 2;
    if (in.keys[SDL_SCANCODE_MINUS] && cam.zoom > 0x40) cam.zoom -= 2;
    int ground = g_map_height[world_cell_of(cam.cam_x, cam.cam_y)] * 0x20;
    if (cam.cam_z < ground + 0x80) cam.cam_z = ground + 0x80;
}

// Movie playback from its snapshot (the viewer's `demo` mode and the front end's attract demo).
static bool start_movie(int movie) {
    sim_prepare_movie();                      // game logic as the original plays the movie (sim.h)
    // The original generates the level before the snapshot loads (carpet -roll 1 -level 38); that leaves
    // the terrain RNG g_rng16, which neither snapshot file holds, at the value the movie continues from.
    if (movie == 0) engine_load_level(38);
    if (!demo_open(s_game.c_str(), movie)) {
        mclog(MCLOG_ERROR, "could not open movie %d", movie);
        return false;
    }
    input_snapshot();
    if (!engine_tick()) {                     // loads the snapshot and plays the first tick
        mclog(MCLOG_ERROR, "movie %d has no state snapshot", movie);
        return false;
    }
    palette_display_set(g_palette6);
    return true;
}

// ---- frame pacing (pacing.h) -----------------------------------------------------------------
static TickInterp s_interp;                   // the game view between the last two ticks
static bool s_rts = false;                    // `rts`: a game-mode run (rts_run.h, Phase 4)
static long s_ticks_run = 0;                  // simulation ticks since start-up (MC_TICK_LOG / MC_QUIT_AFTER_TICKS)
static FILE *s_tick_log = nullptr;
static FILE *s_frame_log = nullptr;           // MC_FRAME_LOG: per drawn game-view frame: tick, alpha, camera
static FILE *open_log(const char *path) {
    FILE *f = nullptr;
#ifdef _WIN32
    if (fopen_s(&f, path, "w") != 0) f = nullptr;
#else
    f = std::fopen(path, "w");
#endif
    return f;
}
static long s_quit_after_ticks = 0;
// MC_QUIT_AFTER_TICKS reached: no further tick runs (checked per tick: a fast-forward frame runs many).
static bool quit_ticks_reached() { return s_quit_after_ticks > 0 && s_ticks_run >= s_quit_after_ticks; }
// ---- round 10 (task D): time control, tick profiler, debug camera (docs/analysis/port_timectl.md) ----------
// Time control (timectl.h): pause / step / fast-forward / slow motion of the main loop's tick clock. Outside
// the simulation: the same ticks run in the same order. Not in a network game.
static TimeControl s_time;
static TimeControl s_time_neutral;            // the attract demo and network games tick as before
// Round 10 (task B, docs/analysis/port_console.md): the debug console (` Backquote), --console-stdin / --scenario.
static Console s_console;
static ConsoleOptions s_con_opts;
// Tick profiler (tick_profile.h): on while F11 shows its line or with MC_TICK_PROFILE=n (a log line every n
// ticks; 0 = on without lines). s_prof_win feeds the F11 line, s_prof_log the log, g_tick_profile_dump A's dumps.
static TickProfile s_prof;
static TickProfileWindow s_prof_win, s_prof_log;
static long s_prof_log_every = -1;            // -1: MC_TICK_PROFILE not set
static std::string s_prof_text;
static void update_tick_profiler(bool f11_wants) {
    const bool on = f11_wants || s_prof_log_every >= 0;
    if (on && !g_tick_profile) { g_tick_profile = &s_prof; s_prof_win.clear(); }
    if (!on && g_tick_profile) { g_tick_profile = nullptr; s_prof_text.clear(); }
}
// Debug camera (debug_camera.h): the free camera over a running level. The game's keys are not fed while it
// is on; the viewer's free-camera keys move it, the mouse turns it; `cam follow <slot>` keeps it behind a Thing.
static bool s_dcam_on = false;
static Camera s_dcam{}, s_dcam_prev{};
static int s_dcam_follow = -1;                // followed Thing slot, -1 = none
static uint32_t s_dcam_follow_gen = 0;        // its thing_slot_generation when the follow started
static void dcam_set(const Camera &c) {
    s_dcam = s_dcam_prev = c;
    s_dcam.cam_x = world_wrap(s_dcam.cam_x); s_dcam.cam_y = world_wrap(s_dcam.cam_y);   // round 11: g_world.units
    s_dcam_prev = s_dcam;
}
// One free-camera step in follow mode: 3 cells behind the Thing along the camera's yaw, 2 cells above it.
static void dcam_follow_step() {
    if (s_dcam_follow < 0) return;
    if (s_dcam_follow >= thing_pool_slots() || thing_at((unsigned)s_dcam_follow)->cls == 0 ||
        thing_slot_generation((unsigned)s_dcam_follow) != s_dcam_follow_gen) {
        mclog(MCLOG_INFO, "camera: Thing %d is gone, follow ends", s_dcam_follow);
        s_dcam_follow = -1;
        return;
    }
    const Thing *t = thing_at((unsigned)s_dcam_follow);
    s_dcam.cam_x = world_wrap(t->x - (int)((int64_t)mc_sin(s_dcam.yaw) * 0x300 >> 16));
    s_dcam.cam_y = world_wrap(t->y + (int)((int64_t)mc_cos(s_dcam.yaw) * 0x300 >> 16));
    s_dcam.cam_z = t->z + 0x200;
    const int ground = g_map_height[world_cell_of(s_dcam.cam_x, s_dcam.cam_y)] * 0x20;
    if (s_dcam.cam_z < ground + 0x80) s_dcam.cam_z = ground + 0x80;
}

// F11 (port-only) cycles: off -> the frame-time line of the title bar -> + the tick profile -> off, drawn into
// the game view's bottom-left corner (visible in borderless fullscreen; part of the HUD layer when composed).
// The time-control / debug-camera state shows above them whenever it is not the default.
static int s_show_pace = 0;
static char s_pace_text[96] = "";
static std::string status_text() {
    std::string s;
    const std::string t = s_time.describe();
    if (!t.empty()) s += "TIME " + t;
    if (s_dcam_on) {
        s += s.empty() ? "" : "   ";
        s += "CAMERA free";
        if (s_dcam_follow >= 0) s += " (follow " + std::to_string(s_dcam_follow) + ")";
    }
    return s;
}
static void draw_pace_overlay() {
    const std::string status = status_text();
    if (!(s_show_pace && s_pace_text[0]) && status.empty()) return;
    ui_set_target(frame());
    ui_set_font(1);
    const int lh = ui_font_line_height(), x = ui_font_space_width();   // 640-wide HUD space
    int y = virtual_h() - 2 * lh;
    if (s_show_pace && s_pace_text[0]) { ui_draw_text(s_pace_text, x, y, ui_col_white()); y -= lh; }
    if (s_show_pace == 2 && !s_prof_text.empty()) { ui_draw_text(s_prof_text.c_str(), x, y, ui_col_white()); y -= lh; }
    if (!status.empty()) ui_draw_text(status.c_str(), x, y, ui_col_white());
}

// One simulation tick through `fn` (game_level_tick / engine_tick), bracketed by the interpolation
// snapshots (read-only) and logged for the determinism check.
template <class F> static auto sim_tick(F fn) -> decltype(fn()) {
    const bool lerp = g_settings.interpolate;
    if (lerp) s_interp.before_tick(g_state->local_player & 7);
    debug_cmd_pump();                         // round 10 (task B): the next queued debug packet into the local slot
    TickProfile *const tp = g_tick_profile;
    if (tp) tick_profile_begin(tp);
    auto r = fn();
    if (tp) {
        tick_profile_end(tp);
        s_prof_win.add(*tp);
        if (g_tick_profile_dump) g_tick_profile_dump->add(*tp);
        if (s_prof_log_every > 0) {
            s_prof_log.add(*tp);
            if (s_prof_log.ticks >= s_prof_log_every) {
                mclog(MCLOG_INFO, "profile ticks %ld-%ld: %s", s_ticks_run + 2 - s_prof_log.ticks, s_ticks_run + 1,
                      s_prof_log.format(TP_CLASSES).c_str());
                s_prof_log.clear();
            }
        }
    }
    if (lerp) s_interp.after_tick(g_state->local_player & 7);
    for (const std::string &m : debug_cmd_take_messages()) mclog(MCLOG_INFO, "%s", m.c_str());   // applied debug packets
    s_console.after_tick();                   // round 10 (task B): the running scenario's step (assertions, host commands)
    s_ticks_run++;
    if (s_rts) rts_after_tick(s_ticks_run); else dump_after_tick(s_ticks_run);   // JSON dumps (task A, rts_run.h)
    if (s_tick_log) tick_log_write(s_tick_log, s_ticks_run);   // + checksum parts with MC_TICK_LOG_PARTS=1 (task E)
    return r;
}

// ---- settings, controller, save anywhere (task E: config.h, gamepad.h, savegame.h) ----------------
static PlatformOptions s_opts;                // mcport.ini / environment / command line (config_load)
static GamepadMapper   s_pad_map;
static std::string     s_save_dir;            // SDL_GetPrefPath("Bullfrog", "MagicCarpet")

// A short message for the player: stdout and the window title (the next title refresh replaces it).
static void notice(const char *fmt, ...) {
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    mclog(MCLOG_INFO, "%s", msg);
    if (s_plat) {
        char title[260];
        std::snprintf(title, sizeof title, "Magic Carpet (native port) - %s", msg);
        s_plat->set_title(title);
    }
}

// ---- the in-level pause menu (game_menu.h, round 9) ----------------------------------------------------
static GameMenu s_menu;
static int s_menu_mx = 320, s_menu_my = 200;   // its pointer in the 640-wide virtual screen
static int s_menu_acc_x = 0, s_menu_acc_y = 0;
static float s_menu_pad_x = 0, s_menu_pad_y = 0;
static bool s_menu_prev_l = true, s_menu_prev_r = true;

// Lets go of every key and mouse button the game holds (opening the menu: nothing stays pressed).
static void release_game_input() {
    for (int sc = 1; sc < 128; sc++) if (g_key_down[sc]) input_key_event(sc, false);
    input_mouse_button(0, false);
    input_mouse_button(1, false);
}

// Closes the menu and writes the options it changed into mcport.ini.
static void menu_close() {
    const auto changed = s_menu.close();
    if (changed.empty()) return;
    std::string ini = s_opts.ini_path;
    if (ini.empty() && !s_save_dir.empty()) ini = s_save_dir + "mcport.ini";
    if (!ini.empty() && !config_set_keys(ini.c_str(), changed)) mclog(MCLOG_WARN, "could not write %s", ini.c_str());
    for (const auto &kv : changed) mclog(MCLOG_INFO, "menu: %s = %s", kv.first.c_str(), kv.second.c_str());
}

// The level a save slot belongs to (-1: empty / invalid), and its 640x480 flag.
static int state_slot_level(int slot, bool *hires) {
    char path[1024];
    SaveStateHeader h{};
    if (!savestate_slot_path(slot, path, sizeof path) || !savestate_read_header(path, &h)) return -1;
    if (hires) *hires = (h.flags & 1) != 0;
    return h.level;
}

// Load save slot `slot`: the state's level is started first when another level (or none) runs, the
// game's resolution is switched when the state was saved in the other one (castle sizes depend on it).
// `in_level` = a level is running. Returns false when the slot is empty / invalid / refused.
static bool load_state_slot(int slot, bool in_level, int *level) {
    bool hires = false;
    const int lv = state_slot_level(slot, &hires);
    if (lv < 0) { notice("slot %d: no saved state", slot); return false; }
    bool force_begin = false;
    if (lv == MODE_LEVEL_INDEX) {                               // task A: a game-mode state (rts): start its run first
        char path[1024];
        if (!savestate_slot_path(slot, path, sizeof path) || !mode_start_run_for_state(s_game.c_str(), path)) {
            notice("slot %d: the game mode of this state cannot be started", slot);
            return false;
        }
        s_rts = true;
        force_begin = true;                                     // the mode run's parameters changed: begin its level again
    } else if (s_rts) {                                         // a campaign state ends the mode run
        rts_end();
        s_rts = false;
    }
    const bool started = !in_level || lv != *level || force_begin;
    if (!in_level) {
        *level = game_after_frontend(lv);                       // fade, title, sound / music banks
        if (!game_level_begin(*level)) { notice("could not load level %d", lv); return false; }
    } else if (lv != *level || force_begin) {
        *level = lv;
        if (!game_level_begin(lv)) { notice("could not load level %d", lv); return false; }
    }
    if (hires != (g_video_mode_flags != 1)) game_toggle_resolution();
    if (!savestate_load(slot)) { notice("slot %d: %s", slot, savestate_error()); return false; }
    debug_overlay_reset();                                      // round 10 (task C): inspector / damage history
    // A level was started for the state: its palette fade-in was cut short by the load (the state's
    // Config says "faded in"); run it again as at a level start (palette only).
    if (started) g_cfg->fade_stage = 0;
    notice("state loaded from slot %d (level %d)%s%s", slot, lv, *savestate_error() ? " - " : "", savestate_error());
    return true;
}

// ---- round 10 (task D) port keys: time control, debug camera, screenshot ----------------------------------
// None is a key the game reads (input.h g_input_bindings; set1_scancode maps none of Pause / End / PageUp /
// PageDown / Insert / Delete, and F11 is not a game key). The integrator moves these chords into
// PlatformOptions [keys] (config.h / config.cpp k_desc, docs/analysis/port_timectl.md); until then they are
// the defaults below.
struct DebugKeys {
    KeyChord time_pause{SDL_SCANCODE_PAUSE, 0};                  // pause / resume
    KeyChord time_step{SDL_SCANCODE_END, 0};                     // one tick (pauses first)
    KeyChord time_step10{SDL_SCANCODE_END, KEYMOD_SHIFT};        // ten ticks
    KeyChord time_faster{SDL_SCANCODE_PAGEUP, 0};                // x2 .. x64, then as fast as possible
    KeyChord time_slower{SDL_SCANCODE_PAGEDOWN, 0};              // down to x1/16
    KeyChord time_normal{SDL_SCANCODE_INSERT, 0};                // x1
    KeyChord debug_camera{SDL_SCANCODE_DELETE, 0};               // free camera over the level on / off
    KeyChord camera_teleport{SDL_SCANCODE_DELETE, KEYMOD_SHIFT}; // the wizard to the free camera (debug packet)
    KeyChord screenshot{SDL_SCANCODE_F11, KEYMOD_CTRL};          // the next frame as PNG
};
static const DebugKeys s_dkeys;
enum DebugAction { DA_NONE, DA_PACE, DA_PAUSE, DA_STEP, DA_STEP10, DA_FASTER, DA_SLOWER, DA_NORMAL, DA_CAMERA,
                   DA_TELEPORT, DA_SHOT };
static DebugAction debug_key_action(int sc, uint8_t mods) {
    if (!sc) return DA_NONE;
    const KeyChord k{sc, (uint8_t)(mods & (KEYMOD_CTRL | KEYMOD_SHIFT | KEYMOD_ALT))};
    if (k == KeyChord{SDL_SCANCODE_F11, 0}) return DA_PACE;
    const struct { const KeyChord &key; DebugAction a; } map[] = {
        {s_dkeys.time_pause, DA_PAUSE}, {s_dkeys.time_step, DA_STEP}, {s_dkeys.time_step10, DA_STEP10},
        {s_dkeys.time_faster, DA_FASTER}, {s_dkeys.time_slower, DA_SLOWER}, {s_dkeys.time_normal, DA_NORMAL},
        {s_dkeys.debug_camera, DA_CAMERA}, {s_dkeys.camera_teleport, DA_TELEPORT}, {s_dkeys.screenshot, DA_SHOT}};
    for (const auto &m : map)
        if (m.key.scancode != 0 && m.key == k) return m.a;
    return DA_NONE;
}

// Time control commands (keys, console `time ...`). Refused in a network game. Logs the new state.
static void time_command(DebugAction a, int n = 1) {
    if (g_cfg->flags & 0x10) { mclog(MCLOG_WARN, "time control: not in a network game"); return; }
    switch (a) {
    case DA_PAUSE: s_time.toggle_pause(); break;
    case DA_STEP: case DA_STEP10: s_time.step(a == DA_STEP10 ? 10 : n); break;
    case DA_FASTER: s_time.faster(); break;
    case DA_SLOWER: s_time.slower(); break;
    case DA_NORMAL: s_time.normal(); break;
    default: return;
    }
    const std::string d = s_time.describe();
    mclog(MCLOG_INFO, "time: %s (speed %s)", d.empty() ? "running" : d.c_str(), TimeControl::speed_name(s_time.speed16()).c_str());
}

// The debug camera on / off (a level only). On: starts at the player's camera, the game's keys and buttons are
// let go and the pointer centred (no steering while the game is not fed).
static void dcam_enable(bool on) {
    if (on == s_dcam_on) return;
    s_dcam_on = on;
    s_dcam_follow = -1;
    g_debug_camera.active = false;
    if (on) {
        dcam_set(player_camera(g_state->local_player));
        release_game_input();
        s_level_mx = 320; s_level_my = virtual_h() / 2;
        input_mouse_move(s_level_mx, s_level_my);
    }
    mclog(MCLOG_INFO, "camera: %s", on ? "free (the game's keys are not fed; Delete again returns)" : "the player's");
}
// `cam follow <slot>` (C's inspector follow mode): -1 stops following.
static void dcam_follow(int slot) {
    if (slot < 0) { s_dcam_follow = -1; return; }
    if (slot >= thing_pool_slots() || thing_at((unsigned)slot)->cls == 0) { mclog(MCLOG_WARN, "camera: no Thing in slot %d", slot); return; }
    if (!s_dcam_on) dcam_enable(true);
    s_dcam_follow = slot;
    s_dcam_follow_gen = thing_slot_generation((unsigned)slot);
    dcam_follow_step();
    s_dcam_prev = s_dcam;
}
// `cam to x y` (cells): above the cell centre, 4 cells over the ground.
static void dcam_to_cell(int cx, int cy) {
    if (!s_dcam_on) dcam_enable(true);
    s_dcam_follow = -1;
    cx = world_wrap_cell(cx); cy = world_wrap_cell(cy);  // round 11: cells wrap at g_world.size
    s_dcam.cam_x = (cx << 8) | 0x80;
    s_dcam.cam_y = (cy << 8) | 0x80;
    s_dcam.cam_z = g_map_height[world_cell_of(s_dcam.cam_x, s_dcam.cam_y)] * 0x20 + 0x400;
    s_dcam_prev = s_dcam;
    mclog(MCLOG_INFO, "camera: cell %d,%d (%d,%d z %d)", cx, cy, s_dcam.cam_x, s_dcam.cam_y, s_dcam.cam_z);
}

// Shift+Delete: the local wizard to the free camera's position (task B's teleport packet, debug_cmd.h:
// recorded / replayed like any command; refused where debug packets are not allowed).
static void teleport_to_camera() {
    if (!s_dcam_on) { mclog(MCLOG_WARN, "teleport: the free camera is off (Delete turns it on)"); return; }
    std::string why;
    // round 11: the camera is wrapped at g_world.units (0..0xffff in the 256 world); what of a wider position
    // the teleport packet carries is debug_cmd's business (debug_cmd.h)
    if (!debug_cmd_queue(debug_cmd_teleport(s_dcam.cam_x, s_dcam.cam_y, s_dcam.cam_z), &why))
        mclog(MCLOG_WARN, "teleport: %s", why.c_str());
    else
        mclog(MCLOG_INFO, "teleport: wizard to %d,%d z %d (cell %d,%d)", s_dcam.cam_x, s_dcam.cam_y, s_dcam.cam_z,
              world_cx(s_dcam.cam_x), world_cx(s_dcam.cam_y));
}

int main(int argc, char **argv) {
    // Settings (config.h): playing defaults < mcport.ini in the save directory < environment < command line
    // (--set section.key=value, --faithful, --config file). Removes its options from argv.
    if (char *p = SDL_GetPrefPath("Bullfrog", "MagicCarpet")) { s_save_dir = p; SDL_free(p); }
    if (const char *e = SDL_getenv("MC_SAVE_DIR")) s_save_dir = e;   // portable install / headless tests
    // Round 10: the log (mclog.h): <save dir>/mcport.log, the previous run's kept as mcport.1.log;
    // MC_LOG_LEVEL=error|warn|info|debug (default info).
    {
        int lv = MCLOG_INFO;
        if (const char *e = SDL_getenv("MC_LOG_LEVEL")) {
            if (!mclog_parse_level(e, &lv)) std::fprintf(stderr, "MC_LOG_LEVEL=%s: not error / warn / info / debug\n", e);
        }
        mclog_set_level(lv);
        std::string args;
        for (int i = 1; i < argc; i++) { args += i > 1 ? " " : ""; args += argv[i]; }
        mclog_open(s_save_dir.c_str(), ("mcport " + args).c_str());
    }
    // round 10 task B: --console-stdin / MC_CONSOLE_STDIN=1, --scenario FILE / MC_SCENARIO=FILE (console.h)
    s_con_opts = console_parse_args(&argc, argv);
    // round 10 task E: --replay-check N | state:<file> [--replay-ticks T] [--replay-raw] (replay_run.h)
    ReplayRunArgs replay;
    {
        std::string err;
        if (!replay_run_parse(&argc, argv, &replay, &err)) { mclog(MCLOG_ERROR, "mcport: %s", err.c_str()); return 1; }
    }
    if (!config_load(s_save_dir.c_str(), &argc, argv, &g_settings, &s_opts)) {
        mclog(MCLOG_ERROR, "usage: mcport [game dir] [play N | load SLOT | demo N | network | LEVEL | rts [--seed S] [--bots B] [--size 256..4096] [--map 50..69] [--reseed] [--tiled | --generate] [--symmetry auto|none|point|mirror|rot4|dihedral] [--water %] [--rough %] [--continent %] [--springs N] [--wild %] [--lairs N] [--triggers N] [--portals N] [--no-debug] [--net N] [--ticks N] [--dump-every N] [--dump-dir D] [--dump-final]] "
                           "[--replay-check N|state:FILE [--replay-ticks T]] "
                           "[--faithful] [--set section.key=value ...] [--config file]");
        return 1;
    }
    mclog_set_console(false);                                  // config_load printed its warnings to stderr already
    for (const std::string &w : s_opts.warnings) mclog(MCLOG_WARN, "%s", w.c_str());
    mclog_set_console(true);
    mclog(MCLOG_INFO, "settings%s: %s", s_opts.faithful ? " (faithful)" : "", config_summary(g_settings).c_str());
    s_game = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    s_console.init(s_save_dir);                                 // history: <save dir>/console_history.txt
    if (s_con_opts.stdin_reader) s_console.start_stdin_reader();
    // A scenario file given at start: its header (level L / rts ...) chooses the run unless a mode is on the
    // command line; it starts with the level, mcport quits when it ends (exit code 1 = an assertion failed).
    Scenario scn_hdr;
    if (!s_con_opts.scenario.empty() && !scenario_load(s_con_opts.scenario, &scn_hdr)) {
        mclog(MCLOG_ERROR, "scenario: %s", scn_hdr.error.c_str());
        return 1;
    }
    const bool scn_mode = !s_con_opts.scenario.empty() && (argc <= 2);   // no mode on the command line
    if (scn_mode && !scn_hdr.rts && scn_hdr.level < 0) {
        mclog(MCLOG_ERROR, "scenario %s: no `level L` / `rts ...` header and no mode on the command line", scn_hdr.name.c_str());
        return 1;
    }
    // Pacing settings (task E's config file fills g_settings; these env switches override it).
    if (const char *e = SDL_getenv("MC_TICK_LOG")) s_tick_log = open_log(e);
    if (const char *e = SDL_getenv("MC_FRAME_LOG")) s_frame_log = open_log(e);
    if (const char *e = SDL_getenv("MC_QUIT_AFTER_TICKS")) s_quit_after_ticks = std::atol(e);
    // Round 10: MC_TIME_SPEED = the starting speed of the time control ("max" = as fast as possible: headless
    // runs; "4", "1/2", ...), MC_TICK_PROFILE=n = profile every tick, a log line every n ticks (0: dumps /
    // F11 only), MC_SHOT_DIR = where `shot` writes (default <save dir>/screenshots).
    if (const char *e = SDL_getenv("MC_TIME_SPEED")) {
        if (!s_time.set_speed_text(e)) mclog(MCLOG_WARN, "MC_TIME_SPEED=%s: not a speed (max, 4, 1/2, ...)", e);
    }
    static TickProfileWindow s_prof_dump;
    if (const char *e = SDL_getenv("MC_TICK_PROFILE")) {
        s_prof_log_every = std::atol(e) > 0 ? std::atol(e) : 0;
        g_tick_profile_dump = &s_prof_dump;
        update_tick_profiler(false);
    }
    dump_configure_from_env();                                  // task A: MC_DUMP_EVERY / MC_DUMP_DIR / MC_DUMP_FINAL
    dump_set_extra([] {                                         // task D's tick profile in every dump (MC_TICK_PROFILE)
        if (!g_tick_profile_dump || g_tick_profile_dump->ticks <= 0) return std::string();
        std::string j = "\"tick_profile\": " + g_tick_profile_dump->json();
        g_tick_profile_dump->clear();
        return j;
    });
    if (const char *e = SDL_getenv("MC_SHOT_DIR")) s_shot_dir = e;
    else s_shot_dir = (std::filesystem::path(s_save_dir) / "screenshots").string();
    // `network` / `-network` (config_parse_33750's option: net_init_4ee70 only, the join is the lobby's)
    const bool net_arg = argc > 2 && (std::strcmp(argv[2], "network") == 0 || std::strcmp(argv[2], "-network") == 0);
    const char *mode = (argc > 2 && !net_arg) ? argv[2] : "";
    const bool demo = std::strcmp(mode, "demo") == 0;
    const bool play = std::strcmp(mode, "play") == 0 || (scn_mode && !scn_hdr.rts);
    const bool load_mode = std::strcmp(mode, "load") == 0;   // `load SLOT`: straight into a saved state
    const bool rts = std::strcmp(mode, "rts") == 0 || (scn_mode && scn_hdr.rts);   // `rts [--seed S] [--size N] [--bots B]` (rts_run.h)
    const bool viewer = !demo && !play && !load_mode && !rts && mode[0] != 0;
    const bool game = !demo && !play && !load_mode && !rts && !viewer;
    int level = play ? (scn_mode ? scn_hdr.level : argc > 3 ? std::atoi(argv[3]) : 0) : viewer ? std::atoi(mode) : 0;
    int movie = (demo && argc > 3) ? std::atoi(argv[3]) : 0;
    double tick_hz = s_opts.tick_hz;                         // [game] tick_hz / MC_TICK_HZ / --set

    if (!engine_init(s_game.c_str())) {
        mclog(MCLOG_ERROR, "engine_init failed for %s", s_game.c_str());
        return 1;
    }
    desync_run_install();                               // round 10 task E: desync dumps + ai_seed part + MC_TICK_LOG_PARTS
    if (replay.active) return replay_run(s_game.c_str(), s_save_dir.c_str(), replay);
    // The original starts in its 320x200 mode (config_parse_33750: DAT_0012edae = 1); R switches to
    // 640x480 in a level (game_toggle_resolution). Set before a level is loaded: wizard castle
    // capacities depend on it (castle_footprint, level_features.h).
    g_video_mode_flags = 1;
    ui_draw_set_video_mode(s_game.c_str());
    palette_fx_set_game_dir(s_game.c_str());
    // Full-length movies (fli.h fli_set_movie_dir): MC_MOVIE_DIR, else the CD's INTRO folder extracted to
    // <game>/../../extracted/gog_cd/CARPET/INTRO (the GamesNostalgia package's intro / outro / level movies
    // are copies of intel.dat).
    {
        std::error_code ec;
        std::filesystem::path md = std::filesystem::path(s_game) / ".." / ".." / "extracted" / "gog_cd" / "CARPET" / "INTRO";
        if (const char *e = SDL_getenv("MC_MOVIE_DIR")) fli_set_movie_dir(e);
        else if (std::filesystem::is_directory(md, ec)) fli_set_movie_dir(md.lexically_normal().string().c_str());
        if (fli_movie_dir()[0]) mclog(MCLOG_INFO, "movies: %s", fli_movie_dir());
        // Hidden Worlds (world_set.h): MC_HIDDEN_DIR, else <game>/hidden (tools/port/install_hidden_worlds.py),
        // else the CD's CARPET folder extracted to <game>/../../extracted/gog_cd/CARPET.
        if (!SDL_getenv("MC_HIDDEN_DIR")) {
            std::filesystem::path hd = std::filesystem::path(s_game) / "hidden";
            std::filesystem::path cd = std::filesystem::path(s_game) / ".." / ".." / "extracted" / "gog_cd" / "CARPET";
            if (!std::filesystem::is_directory(hd, ec) && std::filesystem::is_directory(cd / "LEVELS", ec))
                world_set_init(s_game.c_str(), cd.lexically_normal().string().c_str());
        }
        if (world_set_hidden_available()) mclog(MCLOG_INFO, "hidden worlds: %s (campaign continues after level 50; play 100..124)", world_set_hidden_dir());
    }
    void (*game_input)() = g_hook_player_local_input;
    g_hook_player_local_input = nullptr;      // only a played level reads the devices through the game

    mc::Platform plat;
    if (!plat.init("Magic Carpet (native port)", 320, 200, 4)) {
        mclog(MCLOG_ERROR, "SDL init failed: %s", SDL_GetError());
        return 1;
    }
    s_plat = &plat;
    plat.set_window_size(s_opts.window_width, s_opts.window_height);   // [video] window_width / height
    if (!s_opts.vsync) plat.set_vsync(false);                    // [video] vsync (B's platform; on by default)
    if (s_opts.fullscreen) plat.set_fullscreen(true);            // [video] fullscreen (borderless)
    if (s_opts.pad.enabled && gamepad_init()) mclog(MCLOG_INFO, "controller support on (hot-plug)");
    // Native-resolution composition (task B): g_settings.compose from the config; MC_COMPOSE=1 overrides.
    if (const char *e = SDL_getenv("MC_COMPOSE")) g_settings.compose = std::atoi(e);
    if (g_settings.compose == 1) compose_install();
    // Extended renderer (task A) without the compositor: render_view_ext at the game frame's own size in
    // its view window. (Composed, B's override calls render_view_ext at the display size.) MC_RENDER_EXT
    // overrides g_settings.render_extended.
    if (const char *e = SDL_getenv("MC_RENDER_EXT")) g_settings.render_extended = std::atoi(e) != 0;
    if (g_settings.render_extended && !g_render_view_override)
        g_render_view_override = render_view_ext_target;
    palette_display_set(g_palette6);
    upload_palette();

    // Sound (SoundBackend, mixer + MIDI) and the in-game hooks of the game flow (game.h).
    audio_init(s_game.c_str());
    g_fli_subtitles_enabled = (g_cfg->language != 0 || !g_sound_available);
    g_hook_input_platform = platform_request;
    g_hook_input_mouse_warp = warp_pointer;
    g_hook_game_tick = engine_tick;
    g_hook_game_fade_out = [] { palette_fade_start(nullptr, 0x10); };
    g_hook_game_video_mode_changed = video_mode_changed;
    g_hook_game_title_screen = [] { title_screen_show(s_game.c_str(), frame()); };
    g_hook_game_mouse_cursor = mouse_cursor_set_pointer;         // mouse_cursor_set_sprite_5ba5c (game.h)
    g_hook_player_mouse_cursor = mouse_cursor_set_pointer;       // ... from player_set_input_mode_3bb50 (player.h)
    mouse_cursor_set_pointer(0);                                 // video_input_init_3ed60: pointers[0]

    // Network: `mcport <dir> network`, MC_NET=1 or MC_NET_HOST=<name server> (MC_NET_PORT, default 30600).
    // net_init_4ee70 makes the lobby (main-menu item 3) available; the join happens there.
    // `rts --net N` (round 11 session 3, rts_run.h): a network mode run without the lobby also opens the transport
    bool rts_net = false;
    if (rts && !scn_mode)
        for (int i = 3; i < argc; i++) if (std::strcmp(argv[i], "--net") == 0) rts_net = true;
    TcpTransport *net = net_tcp_from_env(net_arg || rts_net);
    if (net) {
        net_set_transport(net);
        NetLobbyHooks nh = net_lobby_hooks();
        nh.idle = [] { SDL_PumpEvents(); };                      // a waiting exchange keeps the window alive
        net_set_lobby_hooks(nh);
        if (const char *e = SDL_getenv("MC_NET_SYNC")) net_sync_set_interval(std::atoi(e));
        if (net_init() == 1) {
            g_fe_network = 1;                                    // DAT_0009e3c8
            mclog(MCLOG_INFO, "network: %s, sessions on port %u", net->is_name_server() ? "name server" : "connected to the name server",
                        (unsigned)net->session_port());
        } else {
            mclog(MCLOG_ERROR, "network: %s", net->last_error());
        }
    }

    enum Run { RUN_FE, RUN_LEVEL, RUN_DEMO, RUN_VIEWER } run = RUN_VIEWER;
    bool attract = false;                     // RUN_DEMO started by the front end (returns there)
    if (game || play || load_mode || rts) {
        const std::string &save_dir = s_save_dir;
        fe_set_save_dir(save_dir.c_str(), s_game.c_str());
        demo_set_record_dir(save_dir.c_str());            // Alt+R recordings / quick save: <save_dir>/movie
        savestate_install_quick_hooks();                  // Alt+S / quick load -> state slot 0 (game.quicksave_full)
        {   std::error_code ec; std::filesystem::create_directories(std::filesystem::path(save_dir) / "movie", ec); }
        input_reset();
        input_mouse_center();
        input_text_load(s_game.c_str(), g_cfg->language);
        if (!fe_init(s_game.c_str(), play ? 2 : 6)) {
            mclog(MCLOG_ERROR, "front end: resources missing in %s", s_game.c_str());
            return 1;
        }
        g_hook_player_local_input = game_input;
        if (load_mode) {
            const int slot = argc > 3 ? std::atoi(argv[3]) : 0;
            sound_load_bank(s_game.c_str(), 0);
            music_load_bank(s_game.c_str(), 0);
            if (!load_state_slot(slot, false, &level)) return 1;
            run = RUN_LEVEL;
        } else if (rts) {
            ModeParams mp;
            std::string err;
            if (scn_mode) mp = scn_hdr.rts_params;                // round 10: the scenario's `rts ...` header
            else if (!rts_parse_args(argc, argv, 3, &mp, &err)) { mclog(MCLOG_ERROR, "rts: %s", err.c_str()); return 1; }
            if (rts_quit_after_ticks() > 0) s_quit_after_ticks = rts_quit_after_ticks();   // rts --ticks N (task A)
            sound_load_bank(s_game.c_str(), 0);
            music_load_bank(s_game.c_str(), 0);
            if (!rts_begin(s_game.c_str(), mp)) { mclog(MCLOG_ERROR, "rts: the level could not be loaded"); return 1; }
            s_rts = true;
            level = MODE_LEVEL_INDEX;
            run = RUN_LEVEL;
        } else if (play) {
            sound_load_bank(s_game.c_str(), 0);
            music_load_bank(s_game.c_str(), 0);
            g_cfg->level = (uint16_t)level;
            if (!game_level_begin(level)) { mclog(MCLOG_ERROR, "could not load level %d", level); return 1; }
            run = RUN_LEVEL;
            mclog(MCLOG_INFO, "play: level %d - mouse steers, arrows speed / slide, mouse buttons cast, Esc menu (save / load / options / leave), Shift+Q quits, F12 quits at once", level);
        } else {
            game_before_frontend();
            run = RUN_FE;
        }
    } else if (demo) {
        sound_load_bank(s_game.c_str(), 0);
        music_load_bank(s_game.c_str(), 0);
        if (!start_movie(movie)) return 1;
        music_start_level(38);
        run = RUN_DEMO;
    } else {
        if (!engine_load_level(level)) { mclog(MCLOG_ERROR, "could not load level %d", level); return 1; }
        run = RUN_VIEWER;
    }

    // round 10 (task B): the --scenario file runs from the level's first tick
    if (!s_con_opts.scenario.empty()) {
        std::string err;
        if (run != RUN_LEVEL) { mclog(MCLOG_ERROR, "scenario: needs a level (play / rts / a level / rts header)"); return 1; }
        if (!s_console.run_file(s_con_opts.scenario, true, &err)) { mclog(MCLOG_ERROR, "scenario: %s", err.c_str()); return 1; }
    }
    Camera cam = demo ? player_camera(g_state->local_player) : engine_default_camera();
    mc::Input in;
    std::vector<int> deferred_releases;       // key releases held back until the game saw the press
    bool prev_l = false, prev_r = false, release_l = false, release_r = false;
    double demo_rate = DEMO_TICK_HZ;
    const uint64_t t0 = plat.ticks_us();
    uint64_t next_cam = t0, next_tick = t0, next_fade = t0, next_fe = t0;
    uint64_t fps_t0 = t0; int frames = 0; long frame_no = 0;
    bool relative = false, follow = demo, demo_running = demo;
    // Pacing: the period of the tick clock that drives the current mode (alpha = time since the last
    // due tick / period), the free camera's previous step, frame-time statistics.
    uint64_t tick_period = 0;
    Camera cam_prev = cam;
    int last_run = -1;
    uint64_t frame_us_sum = 0, frame_us_max = 0, ticks_at_t0 = 0;
    long frames_total = 0, frames_lerped = 0;
    int mouse_acc_x = 0, mouse_acc_y = 0;     // sub-pixel remainder of the relative mouse motion

    while (!in.quit) {
        const uint64_t frame_start = plat.ticks_us();
        if (s_quit_after_ticks > 0 && s_ticks_run >= s_quit_after_ticks) break;
        maybe_inject_input(frame_no);
        plat.poll(in);
        const uint64_t now = plat.ticks_us();
        std::vector<DebugAction> debug_acts;      // round 10: time control / camera / screenshot keys of this frame
        // Port keys (config.h [keys]: save / load slots, quit at once): swallowed, the game never sees them.
        PortKeyAction port_act = PORT_KEY_NONE;
        int port_slot = 0;
        bool open_menu = false;
        std::vector<GameMenuResult> menu_results;
        if (s_menu.is_open() && run != RUN_LEVEL) menu_close();
        {
            const uint8_t kmods = (uint8_t)(((in.keys[SDL_SCANCODE_LCTRL] || in.keys[SDL_SCANCODE_RCTRL]) ? KEYMOD_CTRL : 0) |
                                            ((in.keys[SDL_SCANCODE_LSHIFT] || in.keys[SDL_SCANCODE_RSHIFT]) ? KEYMOD_SHIFT : 0) |
                                            ((in.keys[SDL_SCANCODE_LALT] || in.keys[SDL_SCANCODE_RALT]) ? KEYMOD_ALT : 0));
            // The pause menu takes the keyboard while it is open (quit_now still works).
            const bool menu_ok = run == RUN_LEVEL && g_state->players[g_state->local_player & 7].input_mode != 3;
            for (int i = 0; i < in.key_event_count; i++) {
                // round 10 (task B): the open console takes every key (quit_now still works); its key closes it
                if (s_console.is_open()) {
                    const int sc = in.key_events[i].scancode;
                    in.key_events[i].scancode = 0;
                    if (!in.key_events[i].down) continue;
                    int slot = 0;
                    if (config_key_action(s_opts, sc, kmods, &slot) == PORT_KEY_QUIT) { port_act = PORT_KEY_QUIT; continue; }
                    if (s_console.is_toggle(sc, kmods)) s_console.close();
                    else s_console.key(sc, kmods);
                    continue;
                }
                if (s_menu.is_open()) {
                    const int sc = in.key_events[i].scancode;
                    in.key_events[i].scancode = 0;
                    if (!in.key_events[i].down) continue;
                    int slot = 0;
                    if (config_key_action(s_opts, sc, kmods, &slot) == PORT_KEY_QUIT) { port_act = PORT_KEY_QUIT; continue; }
                    menu_results.push_back(s_menu.key(sc));
                    continue;
                }
                if (!in.key_events[i].down) continue;
                // round 10 (task C): inspector / overlay / cursor-mode keys (Home, keypad; not game keys)
                if (inspect_tool_key(plat, in.key_events[i].scancode, kmods, run == RUN_LEVEL)) {
                    in.key_events[i].scancode = 0;
                    continue;
                }
                // round 10 (task B): the console key (Backquote; not a game key) opens the console in a level / the
                // viewer / a movie; the game's keys and buttons are let go (the level keeps running)
                if (s_console.is_toggle(in.key_events[i].scancode, kmods) && run != RUN_FE) {
                    in.key_events[i].scancode = 0;
                    s_console.open();
                    release_game_input();
                    prev_l = prev_r = release_l = release_r = false;
                    if (run == RUN_LEVEL) { s_level_mx = 320; s_level_my = virtual_h() / 2; input_mouse_move(s_level_mx, s_level_my); }
                    continue;
                }
                // round 10: time control, debug camera, screenshot, F11 (none of them a key the game reads)
                const DebugAction da = debug_key_action(in.key_events[i].scancode, kmods);
                if (da != DA_NONE) {
                    in.key_events[i].scancode = 0;
                    debug_acts.push_back(da);
                    continue;
                }
                int slot = 0;
                const PortKeyAction a = config_key_action(s_opts, in.key_events[i].scancode, kmods, &slot);
                if (a == PORT_KEY_NONE) continue;
                if (a == PORT_KEY_MENU) {
                    if (!menu_ok) continue;                 // outside a level / typing a chat line: the game's Esc
                    in.key_events[i].scancode = 0;
                    open_menu = true;
                    continue;
                }
                in.key_events[i].scancode = 0;            // set1_scancode(0) == 0: not fed to the game
                port_act = a;
                port_slot = slot;
            }
        }
        if (port_act == PORT_KEY_QUIT) break;
        // Round 10 (task B): lines from stdin, then the console's host commands (time, shot, save, load, cam, ...).
        s_console.poll_stdin();
        for (const HostCmd &h : s_console.take_host_commands()) {
            switch (h.op) {
            case HostOp::TIME:
                if (h.sub == "step") time_command(DA_STEP, h.n);
                else if (h.sub == "normal") time_command(DA_NORMAL);
                else if (h.sub == "speed") {
                    if (g_cfg->flags & 0x10) mclog(MCLOG_WARN, "time control: not in a network game");
                    else if (!s_time.set_speed_text(h.text.c_str())) mclog(MCLOG_WARN, "time speed %s: not a speed (max, 4, 1/2, ...)", h.text.c_str());
                    else mclog(MCLOG_INFO, "time: speed %s", TimeControl::speed_name(s_time.speed16()).c_str());
                }
                else if (h.sub == "toggle") time_command(DA_PAUSE);
                else if (h.sub == "pause") { if (!s_time.paused()) time_command(DA_PAUSE); }
                else if (h.sub == "resume" || h.sub == "run") { if (s_time.paused()) time_command(DA_PAUSE); }
                break;
            case HostOp::SHOT: request_screenshot(h.text); break;
            case HostOp::SAVE:
                if (run != RUN_LEVEL) { mclog(MCLOG_WARN, "save: no level"); break; }
                if (savestate_save(h.n, nullptr)) notice("state saved to slot %d (level %d)", h.n, level);
                else notice("slot %d not saved: %s", h.n, savestate_error());
                break;
            case HostOp::LOAD: port_act = PORT_KEY_LOAD; port_slot = h.n; break;   // the save / load block below
            case HostOp::SYNC: {
                const NetSyncStats st = net_sync_stats();
                mclog(MCLOG_INFO, "sync: %s, %u exchanges, %u checks, %u mismatches (first at exchange %d, first part %s)",
                      (g_cfg->flags & 0x10) ? "network game" : "not a network game", (unsigned)st.exchanges, (unsigned)st.checks,
                      (unsigned)st.mismatches, (int)st.first_mismatch, st.first_part >= 0 ? net_checksum_part_name(st.first_part) : "-");
                break;
            }
            case HostOp::DUMP: dump_now(s_ticks_run); break;                  // (task A) prints its own line
            case HostOp::CAM:
                if (run != RUN_LEVEL) { mclog(MCLOG_WARN, "cam: in a level only"); break; }
                if (h.sub == "free") { debug_inspect_set_follow(false); dcam_enable(true); }
                else if (h.sub == "off") { debug_inspect_set_follow(false); dcam_enable(false); }
                else if (h.sub == "follow") {
                    // the inspector's follow camera (the mouse orbits it, the wheel zooms; inspect_tool.h)
                    const int slot = h.n > 0 ? h.n : debug_inspect_slot();
                    if (slot > 0) { dcam_follow(-1); debug_inspect_set(slot); debug_inspect_set_follow(true); }
                    else mclog(MCLOG_WARN, "cam follow: which Thing? (cam follow #SLOT, or inspect one first)");
                }
                else if (h.sub == "to") dcam_to_cell(h.x >> 8, h.y >> 8);
                break;
            case HostOp::OVERLAY:                                         // (task C's code, port_inspect.md)
                if (h.sub.empty() || h.sub == "list") mclog(MCLOG_INFO, "%s", debug_overlay_list().c_str());
                else if (!(h.on < 0 ? debug_overlay_toggle(h.sub.c_str()) : debug_overlay_set(h.sub.c_str(), h.on != 0)))
                    mclog(MCLOG_WARN, "overlay: unknown '%s' (overlay list)", h.sub.c_str());
                break;
            case HostOp::INSPECT:                                         // (task C's code, port_inspect.md)
                if (h.sub == "off") { debug_inspect_set(-1); debug_inspect_set_cell(-1, -1); }
                else if (h.sub == "cursor") inspect_tool_set_cursor_mode(plat, true);
                else {
                    debug_inspect_set(h.n);
                    std::vector<std::string> lines;
                    debug_inspect_lines(h.n, &lines);
                    for (const std::string &l : lines) mclog(MCLOG_INFO, "%s", l.c_str());
                }
                break;
            case HostOp::QUIT: in.quit = true; break;
            default: break;
            }
        }
        // Round 10: time control, debug camera, screenshot (keys; the console's results join here).
        for (const DebugAction a : debug_acts) {
            switch (a) {
            case DA_PACE: s_show_pace = (s_show_pace + 1) % 3; break;
            case DA_SHOT: request_screenshot(""); break;
            case DA_CAMERA:
                if (run == RUN_LEVEL) dcam_enable(!s_dcam_on);
                break;
            case DA_TELEPORT: teleport_to_camera(); break;
            default: time_command(a); break;
            }
        }
        if ((g_cfg->flags & 0x10) && !s_time.neutral()) {          // a network game: the peers' ticks are locked
            s_time.reset();
            mclog(MCLOG_INFO, "time: network game - back to normal speed");
        }
        if (s_dcam_on && run != RUN_LEVEL) dcam_enable(false);
        update_tick_profiler(s_show_pace == 2);
        const bool dcam_input = s_dcam_on && run == RUN_LEVEL && !s_console.is_open();   // the free camera takes keys and mouse (not while typing)
        // Controller (gamepad.h): its key events join the keyboard's, its buttons the mouse's.
        static uint64_t pad_t = now;
        static float pad_acc_x = 0, pad_acc_y = 0;
        GamepadRaw pad_raw;
        gamepad_poll(&pad_raw);
        const bool pad_devices = run == RUN_FE || run == RUN_LEVEL || (run == RUN_DEMO && attract);
        const PadContext pad_ctx = !pad_devices ? PAD_CTX_NONE : (run != RUN_LEVEL || s_menu.is_open()) ? PAD_CTX_MENU
                                 : g_state->players[g_state->local_player & 7].input_mode == 2 ? PAD_CTX_BOOK : PAD_CTX_FLIGHT;
        GamepadFrame pad = s_pad_map.update(pad_raw, pad_ctx, (double)(now - pad_t) / 1e6, s_opts.pad);
        pad_t = now;
        if (pad.port_action == GamepadFrame::PORT_SAVE_QUICK) { port_act = PORT_KEY_SAVE; port_slot = 0; }
        if (pad.port_action == GamepadFrame::PORT_LOAD_QUICK) { port_act = PORT_KEY_LOAD; port_slot = 0; }
        g_timer_ticks = (uint32_t)((double)(now - t0) * TIMER_HZ / 1e6);
        const bool devices = run == RUN_FE || run == RUN_LEVEL || (run == RUN_DEMO && attract);
        // The pause menu: the controller's key events (B = Esc = back, the d-pad as arrows) and the pointer
        // (mouse motion / the controller's menu cursor, left button / A = click, right button = back, wheel).
        if (s_menu.is_open()) {
            for (int i = 0; i < pad.event_count; i++)
                if (pad.events[i].down) menu_results.push_back(s_menu.key(pad.events[i].scancode));
            s_menu_acc_x += in.mouse_dx * 640;
            s_menu_acc_y += in.mouse_dy * virtual_h();
            s_menu_mx += s_menu_acc_x / plat.fb_w; s_menu_acc_x %= plat.fb_w;
            s_menu_my += s_menu_acc_y / plat.fb_h; s_menu_acc_y %= plat.fb_h;
            s_menu_pad_x += pad.cursor_dx; s_menu_pad_y += pad.cursor_dy;
            s_menu_mx += (int)s_menu_pad_x; s_menu_my += (int)s_menu_pad_y;
            s_menu_pad_x -= (float)(int)s_menu_pad_x; s_menu_pad_y -= (float)(int)s_menu_pad_y;
            s_menu_mx = s_menu_mx < 0 ? 0 : s_menu_mx > 639 ? 639 : s_menu_mx;
            s_menu_my = s_menu_my < 0 ? 0 : s_menu_my > virtual_h() - 1 ? virtual_h() - 1 : s_menu_my;
            const bool ml = in.mouse_l || pad.mouse_l, mr = in.mouse_r || pad.mouse_r;
            const bool moved = in.mouse_dx || in.mouse_dy || pad.cursor_dx != 0 || pad.cursor_dy != 0;
            if (moved || (ml && !s_menu_prev_l) || (mr && !s_menu_prev_r))
                menu_results.push_back(s_menu.pointer(s_menu_mx, s_menu_my, ml && !s_menu_prev_l, mr && !s_menu_prev_r, virtual_h()));
            s_menu_prev_l = ml; s_menu_prev_r = mr;
            if (in.wheel) s_menu.wheel(in.wheel);
        } else if (run == RUN_LEVEL && g_state->players[g_state->local_player & 7].input_mode != 3 && s_opts.menu.mods == 0 &&
                   s_opts.menu.scancode != 0) {
            // the controller's Back (bound to Esc) opens the menu like the key
            for (int i = 0; i < pad.event_count; i++)
                if (pad.events[i].scancode == s_opts.menu.scancode) {
                    if (pad.events[i].down) open_menu = true;
                    pad.events[i].scancode = 0;
                }
        }
        if (open_menu && !s_menu.is_open()) {
            GameMenuEnv env;
            env.network = (g_cfg->flags & 0x10) != 0;
            env.can_save = savestate_allowed();
            env.level = level;
            env.fullscreen = plat.fullscreen();
            s_opts.fullscreen = env.fullscreen ? 1 : 0;           // Alt+Enter may have changed it
            s_menu.open(env, &g_settings, &s_opts);
            release_game_input();
            prev_l = prev_r = release_l = release_r = false;
            s_menu_mx = 320; s_menu_my = virtual_h() / 2;
            s_menu_prev_l = s_menu_prev_r = true;                 // a button held while opening is no click
        }
        // round 10 (task C): the cursor mode (absolute pointer, picking) owns the mouse while it is on; held
        // while the pause menu is open, switched off outside a level
        const bool cursor_mouse = s_menu.is_open() ? inspect_tool_cursor_mode()
                                                    : inspect_tool_mouse(plat, in, run == RUN_LEVEL);
        // port: the mouse wheel zooms the spell book's map screen (input modes 2 / 4; display only, hud.h)
        if (in.wheel && run == RUN_LEVEL && !s_menu.is_open() && !s_console.is_open()) {
            const uint8_t im = g_state->players[g_state->local_player & 7].input_mode;
            if (im == 2 || im == 4) hud_map_zoom_step(in.wheel);
        }
        if (devices && !s_menu.is_open() && !dcam_input && !s_console.is_open()) {   // round 10: the console takes the keys
            // Feed the device state the original's interrupt handlers kept. Releases (keys and mouse
            // buttons) are held back until the game / front end has run once, so a tap shorter than a
            // tick is still seen (fe_input_poll_57cc0 derives clicks from the held state).
            for (int i = 0; i < in.key_event_count; i++) {
                int sc = set1_scancode(in.key_events[i].scancode);
                if (!sc) continue;
                if (in.key_events[i].down) input_key_event(sc, true);
                else deferred_releases.push_back(sc);
            }
            for (int i = 0; i < pad.event_count; i++) {
                const int sc = set1_scancode(pad.events[i].scancode);
                if (!sc) continue;
                if (pad.events[i].down) input_key_event(sc, true);
                else deferred_releases.push_back(sc);
            }
            const bool level_mouse = run == RUN_LEVEL;
            if (level_mouse != s_level_mouse) {
                // Entering a level: start from where the game put its pointer (input_mouse_center).
                s_level_mouse = level_mouse;
                s_level_mx = g_mouse_x; s_level_my = g_mouse_y;
                plat.set_relative_mouse(level_mouse);
                in.mouse_dx = in.mouse_dy = 0;
                mouse_acc_x = mouse_acc_y = 0;
            }
            if (level_mouse && cursor_mouse) {
                // round 10 (task C): cursor mode - the game's pointer stays at the centre (no steering)
                s_level_mx = 320; s_level_my = virtual_h() / 2;
                mouse_acc_x = mouse_acc_y = 0;
                input_mouse_move(s_level_mx, s_level_my);
            } else if (level_mouse) {
                // relative motion (logical pixels) -> virtual 640-wide coordinates, clamped as the driver did;
                // the remainder is kept so many small per-frame motions (high frame rates) add up exactly
                mouse_acc_x += in.mouse_dx * 640;
                mouse_acc_y += in.mouse_dy * virtual_h();
                s_level_mx += mouse_acc_x / plat.fb_w; mouse_acc_x %= plat.fb_w;
                s_level_my += mouse_acc_y / plat.fb_h; mouse_acc_y %= plat.fb_h;
                // controller: the right stick places the pointer (offset from the centre = steering), the
                // book's cursor moves it
                if (pad.steer_active) {
                    s_level_mx = 320 + (int)(pad.steer_x * 319.0f);
                    s_level_my = virtual_h() / 2 + (int)(pad.steer_y * (float)(virtual_h() / 2 - 1));
                } else if (pad.steer_released) {
                    s_level_mx = 320;
                    s_level_my = virtual_h() / 2;
                }
                pad_acc_x += pad.cursor_dx; pad_acc_y += pad.cursor_dy;
                s_level_mx += (int)pad_acc_x; s_level_my += (int)pad_acc_y;
                pad_acc_x -= (float)(int)pad_acc_x; pad_acc_y -= (float)(int)pad_acc_y;
                if (s_level_mx < 0) s_level_mx = 0;
                if (s_level_mx > 639) s_level_mx = 639;
                if (s_level_my < 0) s_level_my = 0;
                if (s_level_my > virtual_h() - 1) s_level_my = virtual_h() - 1;
                input_mouse_move(s_level_mx, s_level_my);
            } else if (pad.cursor_dx != 0 || pad.cursor_dy != 0) {
                // controller in the menus: move the game's pointer and the OS pointer with it
                pad_acc_x += pad.cursor_dx; pad_acc_y += pad.cursor_dy;
                const int dx = (int)pad_acc_x, dy = (int)pad_acc_y;
                pad_acc_x -= (float)dx; pad_acc_y -= (float)dy;
                int vx = g_mouse_x + dx, vy = g_mouse_y + dy;
                vx = vx < 0 ? 0 : vx > 639 ? 639 : vx;
                vy = vy < 0 ? 0 : vy > virtual_h() - 1 ? virtual_h() - 1 : vy;
                input_mouse_move(vx, vy);
                warp_pointer(vx, vy);
            } else
                input_mouse_move(in.mouse_x * 640 / plat.fb_w, in.mouse_y * virtual_h() / plat.fb_h);
            // (cursor mode: the mouse buttons pick, the game does not see them)
            const bool ml = (in.mouse_l && !cursor_mouse) || pad.mouse_l, mr = (in.mouse_r && !cursor_mouse) || pad.mouse_r;
            if (ml && !prev_l) input_mouse_button(0, true);
            if (!ml && prev_l) release_l = true;
            if (mr && !prev_r) input_mouse_button(1, true);
            if (!mr && prev_r) release_r = true;
            prev_l = ml; prev_r = mr;
        } else if (!devices && in.key_pressed[SDL_SCANCODE_ESCAPE]) break;
        auto apply_releases = [&] {
            for (int sc : deferred_releases) input_key_event(sc, false);
            deferred_releases.clear();
            if (release_l) { input_mouse_button(0, false); release_l = false; }
            if (release_r) { input_mouse_button(1, false); release_r = false; }
        };

        // The pause menu's commands.
        bool menu_quit = false;
        for (const GameMenuResult &r : menu_results) {
            switch (r.cmd) {
            case GameMenuCmd::RESUME: menu_close(); break;
            case GameMenuCmd::SAVE: {
                char msg[160];
                if (savestate_save(r.slot, nullptr)) std::snprintf(msg, sizeof msg, "Saved to slot %d", r.slot);
                else std::snprintf(msg, sizeof msg, "Not saved: %s", savestate_error());
                s_menu.refresh_slots();
                s_menu.set_message(msg);
                break;
            }
            case GameMenuCmd::LOAD: menu_close(); port_act = PORT_KEY_LOAD; port_slot = r.slot; break;
            case GameMenuCmd::LEAVE_LEVEL:
                menu_close();
                input_key_event(1, true);                       // the original's Esc (commands 0x1b / 0x1d)
                deferred_releases.push_back(1);
                break;
            case GameMenuCmd::QUIT: menu_close(); menu_quit = true; break;
            case GameMenuCmd::FULLSCREEN: plat.set_fullscreen(r.on); break;
            default: break;
            }
            if (!s_menu.is_open()) { s_interp.reset(); next_tick = now; break; }
        }
        if (menu_quit) break;

        // Save anywhere (savegame.h): Ctrl+F1..F9 / Ctrl+F10 save slot 1..9 / 0, Shift+F.. loads (defaults).
        if (port_act == PORT_KEY_SAVE && run == RUN_LEVEL) {
            if (savestate_save(port_slot, nullptr)) notice("state saved to slot %d (level %d)", port_slot, level);
            else notice("slot %d not saved: %s", port_slot, savestate_error());
        } else if (port_act == PORT_KEY_LOAD && (run == RUN_LEVEL || run == RUN_FE) && !(g_cfg->flags & 0x10)) {
            if (load_state_slot(port_slot, run == RUN_LEVEL, &level)) {
                run = RUN_LEVEL;
                s_interp.reset();
                next_tick = now;
            }
        }
        if (run == RUN_VIEWER && (in.key_pressed[SDL_SCANCODE_L] || in.key_pressed[SDL_SCANCODE_K])) {
            level = (level + (in.key_pressed[SDL_SCANCODE_L] ? 1 : 69)) % 70;
            if (engine_load_level(level)) cam = cam_prev = engine_default_camera();
            s_interp.reset();
            mclog(MCLOG_INFO, "level %d: seed %d, %d free thing slots", level, g_state->level.gen.seed, thing_free_count());
        }
        if (run == RUN_VIEWER || (run == RUN_DEMO && !attract)) {
            if (in.key_pressed[SDL_SCANCODE_HOME]) cam = cam_prev = run == RUN_DEMO ? player_camera(g_state->local_player) : engine_default_camera();
            if (in.key_pressed[SDL_SCANCODE_F2]) g_state->opt_shadows ^= 1;
            if (in.key_pressed[SDL_SCANCODE_F4]) g_state->opt_second_surface ^= 1;
            if (in.key_pressed[SDL_SCANCODE_F5]) g_state->opt_textured_sky ^= 1;
            if (in.key_pressed[SDL_SCANCODE_F8]) g_state->opt_smooth ^= 1;
            if (in.key_pressed[SDL_SCANCODE_SPACE]) time_command(DA_PAUSE);   // the viewer's pause = the time control's
            if (in.mouse_r != relative) { relative = in.mouse_r; plat.set_relative_mouse(relative); }
            if (relative && !follow) {
                cam.yaw = (cam.yaw + in.mouse_dx * 2) & MC_ANGLE_MASK;
                cam_prev.yaw = (cam_prev.yaw + in.mouse_dx * 2) & MC_ANGLE_MASK;   // immediate, not lerped
            }
        }
        if (run == RUN_DEMO && !attract) {
            if (in.key_pressed[SDL_SCANCODE_TAB]) follow = !follow;
            if (in.key_pressed[SDL_SCANCODE_LEFTBRACKET] && demo_rate > 2.0) demo_rate /= 2;
            if (in.key_pressed[SDL_SCANCODE_RIGHTBRACKET] && demo_rate < 600.0) demo_rate *= 2;
        }
        const uint64_t cam_period = (uint64_t)(1e6 / CAMERA_HZ);
        int steps = 0;
        while (now >= next_cam && steps < 8) {
            next_cam += cam_period; steps++;
            cam_prev = cam;
            if (!follow && (run == RUN_VIEWER || run == RUN_DEMO)) free_camera_step(cam, in);
            if (dcam_input) {                              // round 10: the debug camera over the level
                s_dcam_prev = s_dcam;
                free_camera_step(s_dcam, in);
                dcam_follow_step();
            }
        }
        if (steps == 8) { next_cam = now; cam_prev = cam; s_dcam_prev = s_dcam; }
        // relative mouse: look around (immediate, not lerped). Not while the inspector owns the mouse (cursor
        // mode: the pointer picks Things; following: the mouse orbits the followed Thing).
        if (dcam_input && !cursor_mouse && (in.mouse_dx || in.mouse_dy)) {
            for (Camera *c : {&s_dcam, &s_dcam_prev}) {
                c->yaw = (c->yaw + in.mouse_dx * 2) & MC_ANGLE_MASK;
                c->pitch += in.mouse_dy;        // mouse up = look up (user, 2026-10-08: it was inverted)
            }
        }
        if (follow && run == RUN_DEMO) cam_prev = cam;

        // A palette fade blocks the original (vsync waits): step it at 70 Hz, nothing else runs and the
        // screen keeps what was drawn before it (e.g. the title picture before a level).
        if (palette_fade_active()) {
            if (now >= next_fade) {
                next_fade = now + (uint64_t)(1e6 / VGA_HZ);
                palette_fade_step();
            }
            next_tick = next_fe = now;
            apply_releases();
            upload_palette();
            audio_update();
            present_frame();
            continue;
        }

        // The front end and a level draw the game's own pointer; the OS one is hidden there.
        static int os_cursor = -1;
        const int want_cursor = (run == RUN_FE || run == RUN_LEVEL || (run == RUN_DEMO && attract)) ? 0 : 1;
        if (want_cursor != os_cursor) { SDL_ShowCursor(want_cursor ? SDL_ENABLE : SDL_DISABLE); os_cursor = want_cursor; }
        // A new mode (level start, movie start, back to the front end) starts without a previous tick.
        const int run_key = (int)run * 2 + (attract ? 1 : 0);
        if (run_key != last_run) { s_interp.reset(); last_run = run_key; }

        bool drew = false;
        if (run == RUN_FE) {
            if (now >= next_fe) {
                next_fe += (uint64_t)(1e6 / VGA_HZ);
                if (now > next_fe + 100000) next_fe = now;
                int lvl = 0;
                const FeResult r = fe_frame(frame(), g_timer_ticks, &lvl);
                apply_releases();
                if (r == FE_QUIT) break;
                if (r == FE_START_LEVEL) {
                    level = game_after_frontend(lvl);
                    if (!game_level_begin(level)) { mclog(MCLOG_ERROR, "could not load level %d", level); break; }
                    debug_overlay_reset();                     // round 10 (task C): inspector / damage history
                    run = RUN_LEVEL;
                    next_tick = now;
                } else if (r == FE_START_DEMO) {
                    g_hook_player_local_input = nullptr;
                    if (start_movie(lvl)) { run = RUN_DEMO; attract = true; demo_running = true; follow = true; next_tick = now; }
                    else { g_hook_player_local_input = game_input; game_before_frontend(); }
                }
            }
            drew = true;                      // fe_frame draws the whole screen
        } else if (run == RUN_LEVEL) {
            const uint64_t base_period = (uint64_t)(1e6 / tick_hz);
            tick_period = base_period;
            if (s_menu.is_open() && !(g_cfg->flags & 0x10)) {   // the pause menu holds the level (not a network game)
                next_tick = now;
                s_interp.hold();
            } else {
                // round 10: the time control (timectl.h) schedules the ticks; at x1, running, exactly as before:
                // up to 4 due ticks per frame, the clock restarted at `now` after the 4th
                TimeControl &tc = (g_cfg->flags & 0x10) ? s_time_neutral : s_time;
                int t = 0;
                tc.begin_frame(now, &next_tick);
                while (!quit_ticks_reached() && tc.want_tick(now, &next_tick, base_period, 4, t, plat.ticks_us() - frame_start)) {
                    t++;
                    const GameStatus s = sim_tick(game_level_tick);
                    tc.tick_ran();
                    apply_releases();
                    if (s != GAME_RUNNING) {
                        game_level_end();
                        s_interp.reset();
                        if (s_rts) { in.quit = true; break; }      // a mode run ends with its level (round 12: result screen)
                        if (s == GAME_QUIT) { in.quit = true; break; }
                        game_before_frontend();
                        if (!(g_cfg->flags & 0x10)) fe_enter(5);   // the level result screen (a network game: the lobby again)
                        run = RUN_FE;
                        break;
                    }
                    if (palette_fade_active()) break;
                }
                tc.end_frame(now, &next_tick);
                if (t == 0 && tc.holding()) s_interp.hold();
                tick_period = tc.period_us(base_period);
            }
        } else if (run == RUN_DEMO) {
            const double rate = attract ? DEMO_TICK_HZ : demo_rate;
            const uint64_t base_period = (uint64_t)(1e6 / rate);
            TimeControl &tc = attract ? s_time_neutral : s_time;     // the attract demo: as before
            int t = 0;
            tc.begin_frame(now, &next_tick);
            while (!quit_ticks_reached() && tc.want_tick(now, &next_tick, base_period, 32, t, plat.ticks_us() - frame_start)) {
                t++;
                tc.tick_ran();
                if (!demo_running) { s_interp.hold(); continue; }
                demo_running = sim_tick(engine_tick);
                if (!demo_running) mclog(MCLOG_INFO, "movie %d ended after %ld packets", movie, demo_packets_read());
            }
            tc.end_frame(now, &next_tick);
            if (t == 0 && tc.holding()) s_interp.hold();
            tick_period = tc.period_us(base_period);
            if (attract) {
                apply_releases();
                // the attract demo ends with the movie or any user input (input_changed_34090)
                if (!demo_running || input_changed()) {
                    demo_close();
                    g_hook_player_local_input = game_input;
                    game_before_frontend();
                    run = RUN_FE;
                    attract = false;
                }
            }
        } else {
            const uint64_t base_period = (uint64_t)(1e6 / DEMO_TICK_HZ);
            int t = 0;
            s_time.begin_frame(now, &next_tick);
            while (!quit_ticks_reached() && s_time.want_tick(now, &next_tick, base_period, 32, t, plat.ticks_us() - frame_start)) {
                t++;
                sim_tick(engine_tick);
                s_time.tick_ran();
            }
            s_time.end_frame(now, &next_tick);
            if (t == 0 && s_time.holding()) s_interp.hold();
            tick_period = s_time.period_us(base_period);
        }
        if (!drew && run != RUN_FE && !palette_fade_active()) {
            // Render-time interpolation (g_settings.interpolate): the frame between the last two ticks.
            const uint32_t alpha = pace_alpha(now, next_tick - tick_period, tick_period);
            const bool game_view = run == RUN_LEVEL || (run == RUN_DEMO && follow);
            if (g_settings.interpolate) {
                s_interp.fill(g_render_interp, alpha, game_view);
                if (g_render_interp.active && alpha > 0 && alpha < 0x10000) frames_lerped++;
            }
            if (s_frame_log && game_view) {
                const Camera c = g_render_interp.have_camera ? g_render_interp.camera : player_camera(g_state->local_player);
                std::fprintf(s_frame_log, "%ld %llu %u %d %d %d %d\n", s_ticks_run, (unsigned long long)(now - t0),
                             (unsigned)(g_render_interp.active ? g_render_interp.alpha : 0x10000), c.cam_x, c.cam_y, c.cam_z, c.yaw);
            }
            if (game_view) {
                // render_frame_1fab0's drawing (view, HUD, book, map, help); its game-state writes ran in
                // the tick (hud_tick_state through g_hook_frame_state), so drawing at frame rate is safe.
                // With interpolation the view uses g_render_interp.camera; the HUD draws the latest tick.
                if (run == RUN_DEMO) cam = player_camera(g_state->local_player);
                // round 10: the debug camera replaces the view (hud.cpp frame_pass, draw-only pass)
                g_debug_camera.active = s_dcam_on && run == RUN_LEVEL;
                if (g_debug_camera.active)
                    g_debug_camera.cam = g_settings.interpolate
                        ? pace_lerp_camera(s_dcam_prev, s_dcam, pace_alpha(now, next_cam - cam_period, cam_period), true) : s_dcam;
                // round 10 (task C): the inspector's follow mode drives the debug camera
                Camera follow_cam;
                if (run == RUN_LEVEL && inspect_tool_follow_camera(&follow_cam)) {
                    g_debug_camera.active = true;
                    g_debug_camera.cam = follow_cam;
                }
                debug_overlay_frame_begin(frame());           // round 10 (task C): projection capture when needed
                draw_view_frame([&] {
                    render_frame_draw(frame(), g_state->local_player);
                    ui_draw_debug_overlay();
                    debug_overlay_draw(frame(), g_state->local_player);   // round 10 (task C): overlays, inspector
                    // The mouse pointer (the DOS driver's cursor, mouse_cursor_set_sprite_5ba5c): player_set_input_mode_3bb50
                    // selects pointers entry 1 in the spell book / map screen (input mode 2) and entry 0 (the empty
                    // header sprite) otherwise, so flight shows no pointer. Drawn at the position, no hot spot.
                    draw_pace_overlay();
                    ui_set_target(frame());
                    s_console.draw(virtual_h());                  // round 10 (task B): the debug console
                    if (s_menu.is_open()) {
                        ui_set_target(frame());
                        s_menu.draw(virtual_h(), s_menu_mx, s_menu_my);
                    } else if (run == RUN_LEVEL) {
                        int pfx, pfy;           // cursor mode (inspect_tool): the spell book's pointer at the mouse
                        if (inspect_tool_pointer_frame(&pfx, &pfy))
                            mouse_cursor_draw_entry(frame(), 1, pfx * 640 / plat.fb_w, pfy * virtual_h() / plat.fb_h);
                        else mouse_cursor_draw(frame());
                    }
                });
                debug_overlay_frame_end();
            } else {
                const FrameBuffer fb = frame();
                // the free camera steps at 119 Hz: drawn between its last two steps when interpolating
                const Camera c = g_settings.interpolate
                    ? pace_lerp_camera(cam_prev, cam, pace_alpha(now, next_cam - cam_period, cam_period), true) : cam;
                draw_view_frame([&] {
                    render_set_view_window(fb, g_state->view_size);   // free camera: the view only
                    render_view_frame(fb, c);
                    draw_pace_overlay();
                    ui_set_target(frame());
                    s_console.draw(virtual_h());                  // round 10 (task B): the debug console
                });
            }
            g_render_interp = RenderInterp{};         // render-time only: nothing else sees it
        }
        upload_palette();
        audio_update();
        present_frame();
        maybe_screenshot(frame_no++);           // after the present: the composed output of this frame is final
        write_requested_screenshots();          // round 10: `shot` / the screenshot key

        // fps_cap (0 = uncapped: vsync decides): wait out the rest of the frame (SDL_Delay to ~1 ms, then spin).
        if (g_settings.fps_cap > 0) {
            for (;;) {
                const uint64_t w = pace_cap_wait_us(frame_start, plat.ticks_us(), g_settings.fps_cap);
                if (w == 0) break;
                if (w > 1500) plat.sleep_ms((uint32_t)((w - 1000) / 1000));
            }
        }
        const uint64_t frame_end = plat.ticks_us();
        const uint64_t frame_us = frame_end - frame_start;
        frame_us_sum += frame_us;
        if (frame_us > frame_us_max) frame_us_max = frame_us;
        frames_total++;

        // Title bar (port-only frame-time display): fps, average / worst frame time over the last 60
        // frames, simulation ticks per second, "lerp" while interpolating.
        if (++frames == 60) {
            uint64_t t = plat.ticks_us();
            const double secs = (double)(t - fps_t0) / 1e6;
            const double fps = 60.0 / secs;
            const double tps = (double)(s_ticks_run - (long)ticks_at_t0) / secs;
            char *pace = s_pace_text;
            std::snprintf(s_pace_text, sizeof s_pace_text, "%.1f fps  %.2f / %.2f ms  %.1f ticks/s%s", fps, (double)frame_us_sum / 60e3,
                          (double)frame_us_max / 1e3, tps, g_settings.interpolate ? "  lerp" : "");
            if (g_tick_profile) { s_prof_text = s_prof_win.format(3); s_prof_win.clear(); }
            const std::string tdesc = s_time.describe();      // round 10: the time control's state
            if (!tdesc.empty())
                std::snprintf(s_pace_text + std::strlen(s_pace_text), sizeof s_pace_text - std::strlen(s_pace_text), "  [%s]", tdesc.c_str());
            char title[320];
            if (run == RUN_DEMO)
                std::snprintf(title, sizeof title, "Magic Carpet (native port) - movie %d  tick %u  %.0f ticks/s%s  %s",
                              movie, (unsigned)g_state->players[g_state->local_player & 7].tick, attract ? DEMO_TICK_HZ : demo_rate,
                              !demo_running ? " (ended)" : s_time.paused() ? " (paused)" : "", pace);
            else if (run == RUN_LEVEL) {
                const PlayerRec &r = g_state->players[g_state->local_player & 7];
                const Thing *pt = thing_at(thing_wrap(r.thing));
                std::snprintf(title, sizeof title, "Magic Carpet (native port) - %s  health %d/%d  mana %d  %s%s",
                              level == MODE_LEVEL_INDEX ? "rts" : ("level " + std::to_string(level)).c_str(), (int)pt->health, (int)pt->max_health, (int)pt->mana,
                              r.input_mode == 2 ? "[spell book] " : (g_cfg->paused & 1) ? "[paused] " : "", pace);
            } else if (run == RUN_FE)
                std::snprintf(title, sizeof title, "Magic Carpet (native port)  %s", pace);
            else
                std::snprintf(title, sizeof title, "Magic Carpet (native port) - level %d  %s  cam %d,%d z=%d yaw=%d",
                              level, pace, cam.cam_x >> 8, cam.cam_y >> 8, cam.cam_z, cam.yaw);
            plat.set_title(title);
            fps_t0 = t; frames = 0;
            frame_us_sum = frame_us_max = 0;
            ticks_at_t0 = (uint64_t)s_ticks_run;
        }
    }
    if (s_tick_log) {
        std::fprintf(s_tick_log, "frames %ld lerped %ld ticks %ld\n", frames_total, frames_lerped, s_ticks_run);
        std::fclose(s_tick_log);
    }
    if (s_frame_log) std::fclose(s_frame_log);
    if (s_rts) rts_end(); else dump_final(s_ticks_run);         // task A: the final dump (MC_DUMP_FINAL)
    mclog(MCLOG_INFO, "mcport: %ld ticks, %ld frames", s_ticks_run, frames_total);
    net_shutdown();
    delete net;
    audio_shutdown();
    gamepad_shutdown();
    fe_shutdown();
    plat.shutdown();
    engine_shutdown();
    mclog_close();
    return s_console.exit_code();                               // round 10: 1 when a --scenario assertion failed
}
