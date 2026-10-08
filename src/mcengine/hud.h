// Frame composition and HUD: render_frame_1fab0 (view window, render_view, radar, status bars,
// player list, thing labels, spell panel / spell book screen, messages, help screen, credits) and
// the ui_draw_* HUD functions it calls. Owner: hud.cpp (2D primitives / fonts in ui_draw.h).
// Report: docs/analysis/port_hud.md.
//
// Use: engine_init(), ui_draw_init(game_dir) once (after the palette is loaded), then per frame
// render_frame(fb, g_state->local_player). All UI coordinates below are in the 640-wide virtual
// screen (halved by the blitters when g_video_mode_flags & 1).
#pragma once
#include "render.h"
#include "mc_types.h"

// render_frame_1fab0 for `player` (the local player; the original reads g_state->local_player):
// everything the original draws into the back buffer for one frame. By PlayerRec.input_mode:
// 0 / 3 flight view + HUD, 1 help screen, 2 spell book (+ the map screen), 4 map screen.
// In mode 2 it writes Config.spell_slot (the book cell under the pointer, 0xff = none) and
// it counts the PlayerMsg display ticks down, both as the original does.
void render_frame(const FrameBuffer &fb, int player);

// The same function split in its two halves (port only), for a tick driver that simulates at a fixed
// rate and draws at frame rate. render_frame(fb, p) == hud_tick_state(p) + render_frame_draw(fb, p)
// with the draw taken from the state *before* the writes (render_frame draws and writes in one pass;
// see the order notes in docs/analysis/port_hud.md "Game-state writes").
//  - hud_tick_state(p): exactly the game-state writes render_frame_1fab0 and its callees make for this
//    player, in the same order, with no frame buffer: P.spell_flash[type]-- for every owned spell
//    icon drawn (hand labels in flight, every owned book cell in mode 2), P.castle_hit_flash /
//    damage_flash / hit_flash-- (status panels), PlayerMsg.ticks-- (message lines), Config.spell_slot
//    (mode 2: the book cell under g_mouse_x / g_mouse_y), Config.credits_state (attract movie).
//    Call it once per game tick where the original renders (after the simulation of the tick). It
//    reads the pointer position (g_mouse_x / y), Config.tick_bits, g_video_mode_flags and the HUD
//    sprite 3 size (falls back to input_book_cell_w / h() when ui_draw_init was not called).
//  - render_frame_draw(fb, p): only the pixels; writes nothing to GameState / Config.
void hud_tick_state(int player);
void render_frame_draw(const FrameBuffer &fb, int player);

// render_set_view_window_top_2f320(size): the reduced view window at the top right of the screen
// (map screen). Port note: DAT_000b5810 (the window offset render_view's motion blur uses) is
// static in render_landscape.cpp and is not updated by this function.
void render_set_view_window_top(const FrameBuffer &fb, int size);

// ---- HUD pieces (all arguments as the original pushes them) -------------------------------------
// ui_draw_radar_43610(x, y, cam_x, cam_y, w, h, yaw, scale, unused, rect): circular (rect == 0,
// translucent) or rectangular (rect != 0, opaque) terrain map centred on (cam_x, cam_y).
void ui_draw_radar(int x, int y, int cam_x, int cam_y, int w, int h, int yaw, int scale, int unused, int rect);
// ui_draw_radar_blips_42a20: the same arguments; things, castle line, centre cross.
void ui_draw_radar_blips(int x, int y, int cam_x, int cam_y, int w, int h, int yaw, int scale, int unused, int rect);
// Port: zoom of the spell book's map screen (display only, never read by the simulation). 100 = the original
// 0xaa units per pixel (one 256-cell world across); 25 .. hud_map_zoom_max_pct() (the whole world of a wider one).
extern int g_hud_map_zoom_pct;
int  hud_map_zoom_max_pct();
void hud_map_zoom_step(int steps);     // mouse wheel steps: + = closer, - = further out
// ui_draw_map_43910(x, y, w, h, cell_x, cell_y): rectangular overview (no caller in the exe).
void ui_draw_map(int x, int y, int w, int h, int cell_x, int cell_y);
void ui_draw_status_bars(Thing *player_thing);                         // ui_draw_status_bars_219f0
void ui_draw_player_list();                                            // ui_draw_player_list_21370
void ui_draw_thing_label(int x, int y, Thing *spell, int mode);        // ui_draw_thing_label_22870 (mode 0 translucent, 1 opaque)
void ui_draw_spell_panel_icon(int x, int y, Thing *spell, int mode);   // ui_draw_spell_panel_icon_22d80
void ui_draw_spell_icon(int x, int y, int spell_id);                   // ui_draw_spell_icon_22820 (a spell the player does not own)
void ui_fill_bar(int x, int y, int max_w, int h, int value, int colour);   // ui_fill_bar_212f0
// ui_draw_debug_overlay_4ad80: called by game_tick_update_32e80 after render_frame; draws only when
// PlayerRec.flags & 8 (Alt+V). Sets Config.debug_bits bit 0 (bit 1 for the password 0xf851b9).
void ui_draw_debug_overlay();

// Port only: where the Alt+X screenshot (debug_screenshot_3ca00) is written; null = current directory.
extern const char *g_hud_screenshot_dir;
// Joystick present (DAT_0012ed40): draws the book cell outline under the pointer. 0 in the port.
extern uint8_t g_hud_joystick_present;
