// Port-only settings (Phase 3). Nothing here exists in carpet.exe.
//
// The defaults are the original's behaviour ("faithful"): every test and every reference runs with a
// default-constructed PortSettings, so the per-tick and pixel references stay byte / pixel identical.
// mcport fills g_settings from its config file / command line (src/mcport/config.*) before the game
// starts. Engine code reads g_settings; it never writes it (the platform does).
//
// Round 7 (docs/port/BRIEFING_round7.md): each task owns one section below and may add fields only
// there. Every field gets a comment with its range and what the original does.
#pragma once
#include <cstdint>

struct PortSettings {
    // ---- task A: extended renderer (render_landscape.cpp, render_things.cpp, sprite_cache.cpp) ----
    bool render_extended = false;   // false = render_landscape_29050 exactly (40x21 grid, 20-cell cull)
    int  draw_distance   = 20;      // extended: cull radius in cells (original 20; 8..255, render_ext uses at most 127 in a 256-cell world)
    int  fog_start_pct   = 75;      // extended: full light below this % of draw_distance (original 15/20 = 75)
    int  lod             = 1;       // extended: 0 = every cell a full-texture quad; 1 = quality: texture mip levels
                                    //   once a texel is smaller than a pixel, 2x2 / 4x4 / 8x8 cell quads below
                                    //   3 px per cell; 2 = fast: the same at twice the distance-to-size ratio
    int  fog_mode        = 0;       // extended: 0 = auto (haze into the sky texture when the textured sky is on,
                                    //   else darken as the original), 1 = darken (original), 2 = haze to the sky
    int  render_threads  = 0;       // extended: rasteriser threads, 0 = auto (hardware threads, max 16), 1 = none
    int  thing_min_px    = 1;       // extended: things smaller than this on screen (px) are skipped (1..16)

    // ---- task B: frame composition and presentation (compose.*, src/mcport/platform*) ----
    // compose: 0 = the original presentation (the 320x200 / 640x480 game frame scaled to the window);
    //          1 = native: the 3D view rendered at the window's resolution (render_view_ext) under the
    //          game's 2D layer (compose.h). mcport installs the compositor when this is 1.
    int  compose     = 0;
    int  view_width  = 0;           // native: 3D view render resolution; 0 = the window's (else 64..7680,
    int  view_height = 0;           //   scaled to the window - a performance knob); both or neither
    int  hud_scale_mode = 0;        // native: 0 = integer nearest (largest whole scale that fits, 4:3),
                                    //   1 = fit nearest (exact 4:3 fit), 2 = fit filtered (linear)
    int  radar_zoom_pct = 100;      // flight radar zoomed out by this % (100 = the original: 1 cell per
                                    //   radar pixel, 64 cells each way; 50..200)
    bool radar_round = false;       // 320x200: draw the flight radar 64 x 54 so it is a circle on a 4:3
                                    //   display (the original's 64 x 64 shows as a tall oval)
    bool hud_corners = true;       // native, widescreen flight: move the HUD's top corner blocks (radar /
                                    //   status bars, spell labels) to the display corners

    // ---- task C: Thing pool (thing.cpp and the game logic) ----
    // Pool size incl. slot 0; 1000 = original (MC_THING_SLOTS). Clamped to 1000..32768
    // (MC_THING_SLOTS_MAX: indices must stay positive as int16, the original sign-extends them). Read at
    // level start (switch_activate(0), after the map is generated with 1000 slots so that it stays the
    // original's) and when a snapshot / save is loaded (thing_pool_ext_reset); a network
    // session or a movie being played overrides it (thing_pool_force_slots). Slots >= 1000 live outside
    // GameState (thing.h); until the original's 1000 slots would be exhausted the game runs byte for
    // byte as with 1000.
    int  thing_slots = 1000;
    // Extended pool only: towns send out villagers (effect_ridge_node_s52_update_27710) only while
    // fewer than 999 slots are in use, as the original's pool limited them; false = no limit (town
    // populations then grow for as long as the level runs). No effect with 1000 slots.
    bool thing_cap_villagers = true;

    // ---- task D: frame pacing (src/mcport/main.cpp) ----
    // (src/mcport/pacing.h; the original draws exactly one frame per tick and has no limiter)
    bool interpolate = false;       // draw the 3D view between the last two ticks (camera + Things; render-time
                                    //   only, never the simulation); false = every frame shows the last tick
    int  fps_cap     = 0;           // frames per second limit, 0 = uncapped (vsync decides), 1..1000

    // ---- task E: everything else the config file carries goes in src/mcport/config.* ----
    // (the config file mcport.ini, docs/analysis/port_settings.md, carries every field above as well)
    // Quick save / quick load (Alt+S = command 10, command 0xb; player.cpp -> demo_save_state /
    // demo_load_state(10000)). false = the original: movie/gam10000.dat holds the GameState only, the
    // load keeps the current cell heads, class-list heads (Config) and terrain, so cell lists can turn
    // into cycles (docs/analysis/port_reference3.md). true = the port's complete state snapshot
    // (savegame.h savestate_*: GameState, Config, maps, cell heads, pool extension) through
    // g_hook_demo_quick_save / g_hook_demo_quick_load (demo.h), consistent after the load.
    bool quicksave_full = false;

    // ---- round 8: gameplay rules (they change the simulation: see GameplayRules below) ----
    // Possession (mana claim, projectile class 9 type 1) range in % of the original's (100..200). The
    // original's shot lives 11 ticks (projectile_create_type1_37d30: range 0x1000 / speed 0x180 = health 10)
    // and picks its target on the first tick among mana within 0x1400 (20 cells, projectile_target_score_46470);
    // both scale: the lifetime to the nearest whole tick (130 -> 14 ticks, +27%), the pick radius exactly.
    int  possession_range_pct = 100;

    // ---- round 8: flight keys (input.cpp local_input_flight; input only, the commands are the original's) ----
    // In flight the original reads no plain letter key except I, P and R, and not Tab.
    bool keys_wasd = false;         // W / S / A / D act as Up / Down / Left / Right (the arrows keep working)
    bool keys_book_tab = false;     // Tab opens and closes the spell book like Enter (Enter keeps working)

    // ---- round 10 task E: desync tooling (net.cpp parts exchange, replay_check.h) ----
    // On the first checksum mismatch of a network game (MC_NET_SYNC side channel) write the complete state to
    // <save dir>/desync_<exchange>_p<player>.mcs (savestate format, ~850 KB, once per level). Local only: the
    // parts exchange itself happens regardless (every peer must send the same messages). No effect on the game.
    bool desync_dump = true;

    // ---- round 12: launching the Conquest mode (frontend.cpp / fe_mode.cpp; front end only, no simulation) ----
    // The red urn bottom right of the main menu (-> the Conquest setup screen) and in the multiplayer lobby (the
    // session plays Conquest). false = the original's menus pixel for pixel (faithful, the front-end references).
    bool mode_menu = false;
};

extern PortSettings g_settings;     // defined in mc_globals.cpp (default = faithful)

// The settings that change the simulation. A movie (demo_open: the recording's rules; the original's
// mvi movies: faithful) or a network session (the host's) overrides g_settings for as long as it runs,
// so the game logic reads them through gameplay_rules(), never g_settings directly. Movies recorded with
// non-faithful rules go to the port's mvx files (demo.h DemoExtHeader).
struct GameplayRules {
    int possession_range_pct = 100;
    // Phase 4 (round 10): the game mode (mode.h GAME_MODE_*; 0 = the original). Never from the config file:
    // mode_begin forces it for a mode run, movies / network sessions / save states carry it.
    int mode = 0;
};
GameplayRules gameplay_rules();                         // forced rules if any, else g_settings (clamped)
void gameplay_force_rules(const GameplayRules *rules);  // null = back to g_settings
bool gameplay_rules_forced();
inline bool gameplay_rules_faithful(const GameplayRules &r) { return r.possession_range_pct == 100 && r.mode == 0; }
