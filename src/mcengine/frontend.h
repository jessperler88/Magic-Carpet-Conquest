// Front end (round 5, task B): frontend_menu_loop_52070 and its screens (fe_*), mouse pointer
// sprites (sptrs.dat), the save-slot dialogs. Frame-stepped: one fe_frame() call per displayed frame,
// no blocking loops (the original's palette fades and FLI loops become steps, see palette_fx.h / fli.h).
// Input comes from the device state of input.h (the same globals mcport feeds for the game).
//
// CONTRACT between task B (owner), task D (game flow) and the integrator (mcport): the declarations
// below are fixed for round 5; task B implements them and may add more.
#pragma once
#include <cstdint>
#include "render.h"

enum FeResult {
    FE_CONTINUE = 0,     // keep calling fe_frame
    FE_START_LEVEL = 1,  // leave the front end (DAT_0009e504) and play *level_out
    FE_QUIT = 2,         // quit to the OS
    // ---- additions by task B ----
    FE_START_DEMO = 3,   // the main menu's attract "demo level": Config.flags |= 0x24, play movie *level_out
                         // (Config.movie = 0) from its snapshot; afterwards come back with fe_frame (state 2)
    // ---- round 12 (fe_mode.h) ----
    FE_START_MODE = 4,   // the Conquest setup screen's "Start": an offline mode run with fe_mode_params()
};

// fe_init_state_51ed0 + the front-end resource loading. `first_state` = the DAT_0012ed2e screen to
// start in (6 language as the original, 2 to go straight to the main menu).
bool fe_init(const char *game_dir, int first_state);
// Enter a screen from outside, e.g. 5 (level result) after a level has ended.
void fe_enter(int state);
int  fe_state();
// One frame of the front end at timer tick `now_ticks` (119.06 Hz PIT ticks).
FeResult fe_frame(const FrameBuffer &fb, uint32_t now_ticks, int *level_out);
void fe_shutdown();

// ==== additions by task B (round 5) ====================================================================
// Use of fe_frame (frontend_menu_loop_52070 inside game_main_32a00, see game.h):
//   game_before_frontend();  do { r = fe_frame(fb, ticks, &lvl); present fb + g_display_palette6 } while (r == FE_CONTINUE);
// FE_START_LEVEL: *level_out = Config.level (game_after_frontend applies the level skip and fades out;
// the front end does not fade itself, as in the original). The screen to show when the front end is
// entered again is already selected (5 = level result for every level start); fe_enter(5) is harmless.
// FE_QUIT: PlayerRec.quit of the local player is 1 (Shift+Q on a logo, "Quit to DOS", end of the outro).
// The next fe_frame after FE_START_LEVEL / FE_START_DEMO resets DAT_0009e504 and reloads sptrs.dat
// (DAT_0012ebdc), as frontend_menu_loop_52070 does on its first call after a level.
//
// The front end draws in the 320x200 mode (game_main switches a 640x480 game to 320x200 before the
// menu loop): mouse coordinates in g_mouse_x / g_mouse_y are 640x400 virtual (input.h). fb may be
// 320x200 (copied) or at least 640x400 (pixel-doubled into the top-left 640x400). The original never
// shows the front end at 640x480 (settled in round 6, port_render_reference2.md / port_net.md: every
// DAT_0012edae != 1 branch in the fe_* code is dead at run time). The mouse pointer (sptrs.dat entry per screen,
// mouse_cursor_set_sprite_5ba5c) is drawn into fb at the end of fe_frame, hot spot top-left.

// Save directory for save\carpet%02X.gam and the two marker files the original keeps next to it
// (c:\carpet.cd\intro.pld = the intro was watched once, c:\carpet.cd\language.inf = chosen language).
// null / "" = saving disabled and no markers (every start shows the language screen and an
// unskippable first intro, as on a fresh DOS install). `dos_game_dir` (optional): the original game
// directory; its save\ slots are read (never written) when a slot is missing in save_dir.
void fe_set_save_dir(const char *save_dir, const char *dos_game_dir = nullptr);

// ---- front-end globals shared with task D / the integrator (original names in the comments) ----
extern uint8_t g_fe_leave;             // DAT_0009e504: leave the front end (start a level / quit)
extern uint8_t g_fe_reload;            // DAT_0012ebdc: reload the front-end resources on the next frame
extern uint8_t g_fe_game_in_progress;  // DAT_0009e500: 1 at program start, cleared by the first level start
                                       // or a load; while 1, menu item 1 starts Config.level without the
                                       // "New Game?" question and item 11 (start level) is disabled. No code
                                       // of the original ever sets it again.
extern uint8_t g_fe_session;           // DAT_0012ed30: lobby session counter ("CARPET%d"), saved in .gam
extern uint8_t g_fe_lobby_players;     // DAT_0012ed31: lobby player count (2), saved in .gam
extern uint8_t g_fe_network;           // DAT_0009e3c8: network driver present (enables menu item 3, the
                                       // multiplayer lobby). The port has no network: 0; tests set it.
extern uint8_t g_fe_input_flags;       // DAT_0009e583: 1 analog joystick, 2 digital, 8 VFX1, 0x20 4-button.
                                       // Always 0 in the port (no joystick / VFX1 drivers): see fe_input_page.
extern uint8_t g_fe_input_page;        // DAT_0012ed33: input device chosen on the config screen (0 mouse,
                                       // 1 digital joystick, 2 analog, 4 VFX1 analog, 6 VFX1 digital)
// Level names of the result screen and the lobby (pointer table 0x97490: 0 = "", 1..50 = "1. Al Jahan" ..
// "50. Volcania", 51..70 multiplayer maps). "" when out of range.
const char *fe_level_name(int index);
// sound_initialise_34140, called once by the config screen when it is left (null = skipped; the
// platform initialises its audio at start-up instead).
extern void (*g_fe_hook_sound_initialise)();
// Raw screen state for tests: the last image "blitted" to the VGA (320x200) and the front-end palette.
const uint8_t *fe_screen_pixels();
const uint8_t *fe_palette6();

// ==== additions by task B (round 6) ====================================================================
// The multiplayer lobby (state 4) runs the join through net.h (net_session_join_begin / _step, one step
// per fe_frame) and installs the NetBIOS progress callbacks (net_set_lobby_hooks; an idle hook the
// platform installed before is kept). It needs g_fe_network = 1 (set it from net_init() == 1). On a
// successful join it sets Config.flags |= 0x10, GameState.local_player, exchanges the level choice
// (player 0's wins) and returns FE_START_LEVEL; the screen stays 4, so the front end shows the lobby
// again when it is entered after the network game (the original never sets state 5 on that path).
//
// The config screen's summary shows the sound card's I/O / IRQ / DMA and the music port (texts 23..26)
// when <save_dir>/sndsetup.inf and sndsetup.dat exist (fe_sndsetup_read_57af0's c:\carpet.cd files).
//
// In-game mouse pointer: mouse_cursor_set_sprite_5ba5c with the data/pointers table (g_ui_pointers,
// DAT_000adfc0). game_main sets entry 0 after the front end and after a resolution toggle,
// player_set_input_mode_3bb50 sets entry 1 when the local player opens the spell book (mode 2) and
// entry 0 otherwise. Entry 0 is an empty sprite: flight shows no pointer. The platform draws the
// current one after render_frame_draw with mouse_cursor_draw (hot spot top-left, at g_mouse_x / y).
void mouse_cursor_set_pointer(int entry);     // -1 = none
int  mouse_cursor_pointer();
void mouse_cursor_draw(const FrameBuffer &fb);
// Port (round 10): pointers entry `entry` (1 = the spell book / map arrow) at (x, y) in the 640-wide virtual
// screen, hot spot top-left - the inspector's cursor mode.
void mouse_cursor_draw_entry(const FrameBuffer &fb, int entry, int x, int y);

// ==== round 12 (port, task A): the Conquest mode's screens (fe_mode.h) =============================================
// The setup and result screens live in fe_mode.cpp and register themselves here (fe_mode_register(), called by a
// static initialiser of fe_mode.cpp and by mcport): frontend.cpp never calls fe_mode.cpp directly, so a program that
// links frontend.cpp without fe_mode.cpp (most tests) builds as before and never shows the urn.
// The urn (tmaps chunk 116, SpriteDesc 0x4d: the spell urn of the game world, remapped to the screen's palette) is
// shown bottom right of the main menu and in the multiplayer lobby only when g_settings.mode_menu is on and the
// screens are registered: with mode_menu off every front-end pixel is the original's.
struct ModeParams;
struct FontDesc;                            // ui_draw.h
struct FeModeScreens {
    void (*frame)(int state);               // one frame of FE_STATE_MODE_SETUP (11) / FE_STATE_MODE_RESULT (12)
    const ModeParams *(*params)();          // the setup screen's current choice (the lobby's proposal)
    void (*open_setup)(int return_state);   // the setup is entered from 2 (main menu: "Start" plays) or 4 (lobby)
};
void fe_set_mode_screens(const FeModeScreens *screens);
bool fe_mode_menu_shown();                  // g_settings.mode_menu and screens registered
// The lobby's urn: true = the session plays Conquest with the setup's params (mode_set_proposal before the lobby's
// rules exchange; only the host's proposal counts, net_agree_rules). fe_lobby_set_conquest also updates the proposal.
bool fe_lobby_conquest();
void fe_lobby_set_conquest(bool on);
// Screen rectangles of the urn (320x200 pixels) on the main menu and in the lobby (tests click them).
void fe_urn_rect_menu(int *x, int *y, int *w, int *h);
void fe_urn_rect_lobby(int *x, int *y, int *w, int *h);

// ---- the front-end toolkit fe_mode.cpp draws with (320x200 screen pixels, the original's buffers) ----------------
uint8_t *fe_tk_back();                      // the back buffer (DAT_0012ed74) the screen draws into
uint8_t *fe_tk_background();                // the background buffer (DAT_000adf68)
uint8_t *fe_tk_palette();                   // the screen palette (768 bytes, 6-bit)
const uint8_t *fe_tk_bright();              // its 30 % brighter remap (the main menu's highlight table)
void fe_tk_present();                       // vga_blit_backbuffer_320_610f0: show the back buffer
// data/screens/<dat> into the background and <pal> into the palette (+ the bright table); false when missing.
bool fe_tk_load_screen(const char *dat_rel, const char *pal_rel);
void fe_tk_fade_out();                      // palette fades as the original's screens do them (a blocker: the
void fe_tk_fade_in();                       // screen returns and is called again when fe_tk_blocked() is over)
bool fe_tk_blocked();
void fe_tk_set_busy(bool busy);             // a blocking sequence of the screen runs (dispatch keeps calling it)
bool fe_tk_load_fonts();                    // sfont0 / sfont1 / sfont2 (colour 1 remapped, the menu's flags)
void fe_tk_free_fonts();
FontDesc *fe_tk_font(int n);
void fe_tk_text(int x, int y, FontDesc *font, const char *s);    // into the back buffer, clipped to g_ui_clip
int  fe_tk_text_width(const FontDesc *font, const char *s);
void fe_tk_pointer(int *x, int *y);         // the pointer in 320x200 pixels
uint8_t fe_tk_events();                     // the edge flags of fe_input_poll_57cc0 (1 left click, 0x40 right click)
void fe_tk_clear_events(uint8_t mask);
void fe_tk_cursor(int entry);               // sptrs entry of the pointer (-1 hidden)
void fe_tk_set_state(int state);
void fe_tk_start_mode();                    // leave the front end: fe_frame returns FE_START_MODE
uint32_t fe_tk_now();                       // the timer tick of the current fe_frame
uint32_t fe_tk_frame();                     // the front end's frame counter (DAT_0012ed10)
const char *fe_tk_game_dir();
// The urn into `dst` (320x200) at (x, y), remapped to the current palette; bright = the hover highlight.
void fe_tk_draw_urn(uint8_t *dst, int x, int y, bool bright);
void fe_tk_urn_size(int *w, int *h);
