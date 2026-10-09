// Local input of carpet.exe (0x15590..0x17a80): device state -> the local player's command packet.
// Translated from the disassembly (addresses in the comments); see docs/analysis/port_input.md.
#include "input.h"
#include "settings.h"
#include "player.h"
#include "mode.h"
#include "mcfile.h"
#include "gen/input_tables.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---- device state ------------------------------------------------------------------------------
uint8_t  g_key_down[128];
uint8_t  g_key_last = 0;
uint8_t  g_key_prev_raw = 0;
uint8_t  g_key_first = 0;
int16_t  g_mouse_x = g_input_mouse_start[2];
int16_t  g_mouse_y = g_input_mouse_start[3];
int16_t  g_mouse_click_x = g_input_mouse_start[0];
int16_t  g_mouse_click_y = g_input_mouse_start[1];
uint16_t g_mouse_click_left = 0;
uint16_t g_mouse_click_right = 0;
uint16_t g_mouse_click_middle = 0;
uint16_t g_mouse_held_left = 0;
uint16_t g_mouse_held_right = 0;
uint16_t g_mouse_held_middle = 0;
int16_t  g_mouse_dbl_timer = 0;
int16_t  g_mouse_dbl_click = 0;
uint32_t g_mouse_event_mask = 0;
uint8_t  g_mouse_moved = 0;
uint32_t g_mouse_present = 1;

uint8_t  g_sound_available = 0;
uint8_t  g_sound_on = 0;
uint8_t  g_music_available = 0;
uint8_t  g_music_on = 0;
uint8_t  g_sky_available = 1;
uint8_t  g_book_cell_w = 0;
uint8_t  g_book_cell_h = 0;

// Sprite 3 of the HUD table as the game sees it after loading (see input.h).
int input_book_cell_w() { return g_book_cell_w ? g_book_cell_w : 64; }
int input_book_cell_h() { return g_book_cell_h ? g_book_cell_h : (g_video_mode_flags & 1) ? 36 : 37; }

void (*g_hook_input_mouse_warp)(int, int) = nullptr;
void (*g_hook_input_platform)(InputPlatformRequest, int) = nullptr;
void (*g_hook_input_joystick_poll)(int, int, int, int, int, int, int) = nullptr;

// DAT_000b6e84 / 86 / 88 and DAT_000adfa8 of input_snapshot_34060 / input_changed_34090.
static int16_t  s_snap_x = 0, s_snap_y = 0;
static uint16_t s_snap_key = 0;
static uint32_t s_changed_latch = 0;

static inline void platform(InputPlatformRequest what, int arg = 0) {
    if (g_hook_input_platform) g_hook_input_platform(what, arg);
}
// input_joystick_poll_5a4e0: the joystick (and the VFX1 head tracker) is the platform's business.
static inline void joystick_poll(int cursor_mode, int x0, int y0, int x1, int y1, int step_x, int step_y) {
    // TODO(port): input_joystick_poll_5a4e0(cursor_mode, x0, y0, x1, y1, step_x, step_y)
    if (g_hook_input_joystick_poll) g_hook_input_joystick_poll(cursor_mode, x0, y0, x1, y1, step_x, step_y);
}

// ---- feeding API -------------------------------------------------------------------------------

// input_keyboard_install_4fb46 clears the key table; the mouse variables get their start values.
void input_reset() {
    std::memset(g_key_down, 0, sizeof g_key_down);
    g_key_last = g_key_prev_raw = g_key_first = 0;
    g_mouse_click_x = g_input_mouse_start[0];
    g_mouse_click_y = g_input_mouse_start[1];
    g_mouse_x = g_input_mouse_start[2];
    g_mouse_y = g_input_mouse_start[3];
    g_mouse_click_left = g_mouse_click_right = g_mouse_click_middle = 0;
    g_mouse_held_left = g_mouse_held_right = g_mouse_held_middle = 0;
    g_mouse_dbl_timer = g_mouse_dbl_click = 0;
    g_mouse_event_mask = 0;
    g_mouse_moved = 0;
    g_mouse_present = 1;
    s_snap_x = s_snap_y = 0;
    s_snap_key = 0;
    s_changed_latch = 0;
}

// input_keyboard_isr_4fa28: one byte from port 0x60.
void input_key_raw(uint8_t byte) {
    g_key_last = byte;
    if (g_key_prev_raw == 0xe0 && (byte == 0x2a || byte == 0xaa)) {
        // the fake shift an extended key is wrapped in: swallowed
        g_key_prev_raw = byte;
        g_key_last = 0x80;
    } else {
        g_key_prev_raw = byte;
        g_key_down[byte & 0x7f] = byte > 0x7f ? 0 : 1;      // (0xe0 itself clears entry 0x60)
    }
    // (keyboard acknowledge through port 0x61)
    if (g_key_last < 0x80 && g_key_first == 0) g_key_first = g_key_last;
    // TODO(port): Tab (0x0f) in a network game (Config.flags & 0x80): FUN_0004cdf0(0, 0x3f, 0, 0) and
    //             DAT_000adfca = 1, which chains every later interrupt to the previous int 9 handler.
}

void input_key_event(int scancode, bool down) {
    if (scancode & MC_SC_EXT) input_key_raw(0xe0);
    uint8_t make = (uint8_t)(scancode & 0x7f);
    input_key_raw(down ? make : (uint8_t)(make | 0x80));
}

// mouse_event_callback_5b86c (EAX = condition mask, ECX / EDX = position).
void input_mouse_event(unsigned mask, int x, int y) {
    if (g_mouse_present == 0) return;
    g_mouse_moved = 1;
    g_mouse_event_mask = mask;
    // (640x480: the driver reports x8 coordinates, `sar 3` here: done by the caller in the port)
    g_mouse_x = (int16_t)x;
    g_mouse_y = (int16_t)y;
    if (g_mouse_x > 0x27e) g_mouse_x = 0x27e;
    if (g_mouse_y > 0x1de) g_mouse_y = 0x1de;
    if (mask & 2) {
        if (g_mouse_held_left == 0) {
            if (g_mouse_dbl_timer > 0) g_mouse_dbl_click = 1;
            else                       g_mouse_dbl_timer = g_input_dblclick_window[0];
        }
        if (g_mouse_held_left == 0 && g_mouse_click_left == 0) {
            g_mouse_click_left = 1;
            g_mouse_click_x = (int16_t)x;
            g_mouse_click_y = (int16_t)y;
        }
        g_mouse_held_left = 1;
    }
    if (mask & 4) g_mouse_held_left = 0;
    if (mask & 8) {
        if (g_mouse_held_right == 0 && g_mouse_click_right == 0) {
            g_mouse_click_right = 1;
            g_mouse_click_x = (int16_t)x;
            g_mouse_click_y = (int16_t)y;
        }
        g_mouse_held_right = 1;
    }
    if (mask & 0x10) g_mouse_held_right = 0;
    if (mask & 0x20) {
        if (g_mouse_held_middle == 0 && g_mouse_click_middle == 0) {
            g_mouse_click_middle = 1;
            g_mouse_click_x = (int16_t)x;
            g_mouse_click_y = (int16_t)y;
        }
        g_mouse_held_middle = 1;
    }
    if (mask & 0x40) g_mouse_held_middle = 0;
    // TODO(port): pointer redraw in video memory (mouse_cursor_restore_vram_5b560 / mouse_cursor_draw_vram_5b050)
}

void input_mouse_move(int x, int y) {
    // the driver's range (int 33h functions 7 / 8 in mouse_set_range_5be68): 0..0x280, 0..0x190 / 0..0x1e0
    const int max_y = g_video_mode_flags == 1 ? 0x190 : 0x1e0;
    if (x < 0) x = 0;
    if (x > 0x280) x = 0x280;
    if (y < 0) y = 0;
    if (y > max_y) y = max_y;
    input_mouse_event(1, x, y);
}

void input_mouse_button(int button, bool down) {
    unsigned mask = 0;
    switch (button) {
    case 0: mask = down ? 2u : 4u; break;
    case 1: mask = down ? 8u : 0x10u; break;
    case 2: mask = down ? 0x20u : 0x40u; break;
    default: return;
    }
    input_mouse_event(mask, g_mouse_x, g_mouse_y);
}

// input_mouse_set_pos_4a030(x, y)
void input_mouse_set_pos(int x, int y) {
    if ((int32_t)g_mouse_present <= 0) return;
    if ((int16_t)x == -1) return;
    int warp_x = (int16_t)x;
    if ((int16_t)x > 0) {
        if ((int16_t)x > 0x27e) x = 0x27e;
        g_mouse_x = (int16_t)x;
        warp_x = g_mouse_x;
    }
    if ((int16_t)y > 0) {
        const int max_y = (g_video_mode_flags & 1) ? 0x18e : 0x1de;
        if ((int16_t)y > max_y) y = max_y;
        g_mouse_y = (int16_t)y;
    }
    // int 33h function 4 (x8 coordinates in the 640x480 mode)
    if (g_hook_input_mouse_warp) g_hook_input_mouse_warp(warp_x, g_mouse_y);
}

// input_mouse_center_4a000
void input_mouse_center() {
    input_mouse_set_pos(0x140, g_video_mode_flags == 1 ? 0xc8 : 0xf0);
}

// input_snapshot_34060
void input_snapshot() {
    s_snap_x = g_mouse_x;
    s_snap_y = g_mouse_y;
    s_snap_key = g_key_last;
    s_changed_latch = 0;
}

// input_changed_34090
int input_changed() {
    if (s_changed_latch == 0) {
        if (g_mouse_x == s_snap_x && g_mouse_y == s_snap_y && g_mouse_held_left == 0 &&
            g_mouse_held_right == 0 && (uint16_t)g_key_last == s_snap_key)
            return (int)s_changed_latch;
        s_snap_x = g_mouse_x;
        s_snap_y = g_mouse_y;
        s_snap_key = g_key_last;
        s_changed_latch = 1;
    }
    return (int)s_changed_latch;
}

// (language strings: text.cpp since round 4)

// ---- helpers -----------------------------------------------------------------------------------
static inline int local_index() { return g_state->local_player & 7; }   // (the original does not mask)
static inline bool cheat_gate() { return (g_cfg->flags & 0x8000) != 0; } // Config+1 & 0x80
static inline bool net_or_custom() { return (g_cfg->flags & 0x10) != 0; }

// The on-screen notice of the function keys: strcpy into players[local].messages[local], 0x32 ticks.
static void notice_text(int text_index) {
    PlayerMsg *m = &g_state->players[local_index()].messages[local_index()];
    const char *src = input_text(text_index);
    size_t i = 0;
    for (; i < sizeof m->text - 1 && src[i] != 0; i++) m->text[i] = src[i];   // (unbounded strcpy in the original)
    m->text[i] = 0;
}
static void notice_show() {
    PlayerMsg *m = &g_state->players[local_index()].messages[local_index()];
    m->ticks = 0x32;
    m->arg = 2;
}

// The level's music track, i16 at GameState+0x240.
static int level_music_track() {
    int16_t v;
    std::memcpy(&v, reinterpret_cast<const uint8_t *>(g_state) + 0x240, 2);
    return v;
}

// The spell Thing of book slot `slot` the way the original reaches it: things[P+0x214[slot]] with no
// range check on either index. slot is 0xff until a spell is chosen, which reads 0x3fc bytes further
// on, inside the next player's record (its unused camera log, zero), i.e. things[0]; emulated by
// reading the GameState at the same offset. Out-of-pool values fall back to things[0].
static Thing *spell_of_slot(const Thing *t, int slot) {
    const uint8_t *P = thing_player_block(t);
    int32_t idx = 0;
    if (slot >= 0 && slot < 24) {
        std::memcpy(&idx, P + offsetof(PlayerBlock, spell_slot) + slot * 4, 4);
    } else {
        const uint8_t *lo = reinterpret_cast<const uint8_t *>(g_state);
        const uint8_t *hi = lo + sizeof(GameState);
        if (P >= lo && P < hi) {
            // round 11: the address in the original's layout (mc_image.h)
            const intptr_t off = (P - lo) + (intptr_t)offsetof(PlayerBlock, spell_slot) + (intptr_t)slot * 4;
            if (off >= 0 && off + 4 <= (intptr_t)MC_STATE_IMAGE_SIZE) state_image_read(*g_state, off, &idx, 4);
        }
    }
    return thing_at((idx > 0 && idx < thing_pool_slots()) ? (unsigned)idx : 0u);
}

// ---- player_mouse_steer_15590 ------------------------------------------------------------------
void player_mouse_steer() {
    if (g_cfg->substeps != 0) return;
    int32_t x, y;
    if (g_video_mode_flags == 1) {
        x = ((int32_t)g_mouse_x * 128 - 0xa000) / 0x140;
        y = -((int32_t)g_mouse_y * 128 - 0x6400) / 0xc8;
    } else {
        x = ((int32_t)g_mouse_x * 128 - 0xa000) / 0x140;
        y = -(((int32_t)g_mouse_y * 128 - 0x7800) / 0xf0);
    }
    if (x < -0x7f) x = -0x7f;
    if (x > 0x7f) x = 0x7f;
    if (y < -0x7f) y = -0x7f;
    if (y > 0x7f) y = 0x7f;
    CmdPacket *pk = &g_state->commands[local_index()];
    pk->steer_x = (int8_t)x;
    pk->steer_y = (int8_t)y;
}

// ---- player_queue_command_17270(cmd, arg) ------------------------------------------------------
// Jump table 0x171f0 (31 entries). Most commands only go into a free packet (or one that already
// holds the same command): the first command of a tick wins.
void player_queue_command(int cmd, int arg) {
    const uint16_t c = (uint16_t)cmd;
    const uint8_t a = (uint8_t)arg;
    const int lp = local_index();
    CmdPacket *pk = &g_state->commands[lp];
    PlayerRec *rec = &g_state->players[lp];
    if (c > 0x1e) return;
    // movsx of the queued command byte against the 16-bit argument
    const bool free_or_same = (int16_t)(int8_t)pk->cmd == (int16_t)c || pk->cmd == 0;
    switch (c) {
    case 0: case 1: case 2: case 0x1a:              // 0x17298: unconditional
        pk->cmd = (uint8_t)c;
        break;
    case 4:                                         // 0x172b8
        if (free_or_same) { pk->cmd = (uint8_t)c; pk->arg |= a; }
        break;
    case 5: case 6:                                 // 0x17328: key bits accumulate in +5
        if (free_or_same) { pk->cmd = (uint8_t)c; pk->bits |= a; }
        break;
    case 0xa: case 0xb: case 0xc: case 0xe:         // 0x17388, 0x173e4, 0x1743e, 0x17498
    case 0x10: case 0x12: case 0x13: case 0x1d:     // 0x176a3, 0x1776b, 0x177c5, 0x17657
        if (free_or_same) pk->cmd = (uint8_t)c;
        break;
    case 0xf:                                       // 0x174f2: only while the wizard lies dead
        if (free_or_same) {
            const Thing *t = thing_at(thing_wrap(rec->thing));
            if (t->health < 0 && t->state == 3) pk->cmd = (uint8_t)c;
        }
        break;
    case 0x11:                                      // 0x176fd
        if (free_or_same) { pk->arg = a; pk->cmd = (uint8_t)c; }
        break;
    case 0x14: case 0x15: case 0x16: case 0x17: case 0x18: case 0x19:   // 0x1781f.. -> 0x17a0a
        if (free_or_same) { pk->cmd = (uint8_t)c; pk->arg = a; }
        break;
    case 0x1b:                                      // 0x17596: level won
        if (free_or_same && (rec->status & 2)) pk->cmd = (uint8_t)c;
        break;
    case 0x1c:                                      // 0x175f6: level lost
        if (free_or_same && (rec->status & 4)) pk->cmd = (uint8_t)c;
        break;
    case 0x1e:                                      // 0x1795c: cheats need the name or the gate
        if (!cheat_gate() && std::memcmp(rec->name, "chronicle", 9) != 0) break;
        if (free_or_same) { pk->cmd = (uint8_t)c; pk->arg = a; }
        break;
    default:                                        // 3, 7, 8, 9, 0xd: 0x17a34, nothing
        break;
    }
}

// ---- player_function_keys_156b0 ----------------------------------------------------------------
void player_function_keys() {
    GameState *st = g_state;
    const int lp = local_index();
    if (st->commands[lp].cmd != 0) return;
    PlayerRec *rec = &st->players[lp];
    Thing *t = thing_at(thing_wrap(rec->thing));

    if (g_key_down[MC_SC_ALT]) {
        switch (g_key_last) {
        case 0x2f:      // V
            if (cheat_gate()) player_queue_command(4, 8);
            break;
        case 0x3b: case 0x3c: case 0x3d: case 0x3e: case 0x3f: case 0x40: case 0x41:   // F1..F7
            player_queue_command(0x1e, g_key_last - 0x3a);
            break;
        default:
            return;     // the key stays pending
        }
        g_key_last = 0;
        return;
    }

    if (g_key_down[MC_SC_LSHIFT] || g_key_down[MC_SC_RSHIFT]) {
        switch (g_key_last) {
        case 0x10:      // Q
            player_queue_command(2, 0);
            break;
        case 0x12:      // E
            if (cheat_gate()) player_queue_command(0x1a, 0);
            break;
        case 0x13:      // R: restart the level (round 12: not in a mode run - it would restart the match)
            if (!net_or_custom() && !mode_active()) {
                rec->status |= 0xc;
                rec->blk.castle = 0;                    // GameState+0x388c + player * 0x801
            }
            break;
        case 0x21:      // F
            if (cheat_gate()) rec->status |= 4;
            break;
        case 0x25:      // K
            if (!net_or_custom()) t->health = -1;
            break;
        case 0x26:      // L
            if (!net_or_custom()) {
                unsigned castle = player_block(t)->castle;
                if (castle != 0 && castle < (uint32_t)thing_pool_slots()) thing_at(castle)->health = -1;
            }
            break;
        case 0x2e:      // C
            if (cheat_gate()) rec->status |= 2;
            break;
        default:
            return;     // the key stays pending
        }
        g_key_last = 0;
        return;
    }

    if (!net_or_custom() && g_key_last == 0x19) {       // P: pause
        g_cfg->paused ^= 1;
        if (g_cfg->paused & 1) {
            if (g_sound_on) platform(INPUT_REQ_SOUND_STOP_ALL);          // TODO(port): sound_stop_all_5c040()
            if (g_music_on) platform(INPUT_REQ_MUSIC_STOP);              // TODO(port): music_stop_1f960()
        } else if (g_music_on) {
            platform(INPUT_REQ_MUSIC_PLAY, level_music_track());         // TODO(port): music_play_track_5c0a0(track)
        }
        g_key_last = 0;
    }
    if (g_key_last == 0x13) {                           // R
        if (st->opt_allowed[10] != 0 && st->mode_3d == 0)
            platform(INPUT_REQ_TOGGLE_RESOLUTION);                       // TODO(port): video_toggle_resolution_33600()
        g_key_last = 0;
    }
    if (g_key_last == 0x39) {                           // Space
        player_queue_command(0xf, 0);
        player_queue_command(0x1b, 0);
        g_key_last = 0;
    }

    if ((uint8_t)(g_key_last - 0x3b) > 9) return;
    switch (g_key_last) {                               // jump table 0x15688
    case 0x3b:      // F1: sound effects
        if (g_sound_available) {
            g_sound_on ^= 1;
            notice_text(g_sound_on ? MC_TEXT_SOUND_ON : MC_TEXT_SOUND_OFF);
            platform(INPUT_REQ_SOUND_STOP_ALL);                          // TODO(port): sound_stop_all_5c040()
            notice_show();
        }
        break;
    case 0x3c:      // F2: music
        if (g_music_available) {
            if (g_music_on == 0) {
                g_music_on = 1;
                platform(INPUT_REQ_MUSIC_PLAY, level_music_track());     // TODO(port): music_play_track_5c0a0(track)
                notice_text(MC_TEXT_MUSIC_ON);
            } else {
                platform(INPUT_REQ_MUSIC_STOP);                          // TODO(port): music_stop_1f960()
                notice_text(MC_TEXT_MUSIC_OFF);
                g_music_on = 0;
            }
            notice_show();
        }
        break;
    case 0x3d:      // F3: thing updates per tick
        if (!net_or_custom()) {
            g_cfg->substeps = (uint8_t)((g_cfg->substeps + 1) % 3);
            switch (g_cfg->substeps) {
            case 0: notice_text(MC_TEXT_SPEED_NORMAL); break;
            case 1: notice_text(MC_TEXT_SPEED_FAST); break;
            case 2: notice_text(MC_TEXT_SPEED_SUPER_FAST); break;
            default: break;
            }
            notice_show();
        }
        break;
    case 0x3e:      // F4: soften
        if (st->opt_allowed[8] != 0) {
            st->opt_smooth ^= 1;
            notice_text(st->opt_smooth ? MC_TEXT_SOFTEN_ON : MC_TEXT_SOFTEN_OFF);
            notice_show();
        }
        break;
    case 0x3f:      // F5: reflections
        if (st->opt_allowed[0] != 0) {
            st->opt_second_surface ^= 1;
            notice_text(st->opt_second_surface ? MC_TEXT_REFLECTIONS_ON : MC_TEXT_REFLECTIONS_OFF);
            notice_show();
        }
        break;
    case 0x40:      // F6: sky
        if (st->opt_allowed[2] != 0 && g_sky_available) {
            st->opt_textured_sky ^= 1;
            notice_text(st->opt_textured_sky ? MC_TEXT_SKY_ON : MC_TEXT_SKY_OFF);
            notice_show();
        }
        break;
    case 0x41:      // F7: shadows
        if (st->opt_allowed[1] != 0) {
            st->opt_shadows ^= 1;
            notice_text(st->opt_shadows ? MC_TEXT_SHADOWS_ON : MC_TEXT_SHADOWS_OFF);
            notice_show();
        }
        break;
    case 0x42:      // F8: icons and map
        st->opt_hud_a ^= 1;
        st->opt_hud_b ^= 1;
        notice_text(st->opt_hud_a ? MC_TEXT_ICONS_ON : MC_TEXT_ICONS_OFF);
        notice_show();
        break;
    case 0x43:      // F9: speed blur off -> light -> heavy -> off
        if (st->opt_allowed[7] != 0) {
            switch (st->opt_motion_blur) {
            case 0:
                if (g_frame2 != nullptr) {              // DAT_000adf70: needs the second screen buffer
                    st->opt_motion_blur = 1;
                    notice_text(MC_TEXT_BLUR_LIGHT);
                    notice_show();
                }
                break;
            case 1:
                st->opt_motion_blur = 2;
                notice_text(MC_TEXT_BLUR_HEAVY);
                notice_show();
                break;
            case 2:
                notice_text(MC_TEXT_BLUR_OFF);
                notice_show();
                st->opt_motion_blur = 0;
                break;
            default:
                break;
            }
        }
        break;
    case 0x44:      // F10: 3D mode, flight only
        if (rec->input_mode == 0 && st->opt_allowed[6] != 0) {
            switch (st->mode_3d) {
            case 0:
                if (g_frame2 != nullptr) {
                    platform(INPUT_REQ_3D_MODE_ON);                      // TODO(port): render_mapmode_palette_2ff50()
                    st->mode_3d = 1;
                    st->view_size = 0x28;
                }
                break;
            case 1:
                platform(INPUT_REQ_3D_MODE_RESTORE);                     // TODO(port): video_restore_mode_2ff10()
                st->mode_3d = 2;
                break;
            case 2:
                st->mode_3d = 0;
                break;
            default:
                break;
            }
        }
        break;
    default:
        break;
    }
    g_key_last = 0;
}

// ---- player_local_input_16660 ------------------------------------------------------------------

// One hand of the flight mode (0x16c42 left / 0x16cfc right): a fresh click fires; a held button
// keeps firing while the spell is being cast or has shots of a burst queued.
static void flight_fire(const Thing *t, int slot, uint16_t *click, uint16_t held, int bit) {
    if (slot == -1) {
        *click = 0;
        return;
    }
    const Thing *spell = spell_of_slot(t, slot);
    if (spell->spell_flags == 1) {
        // port (user, 2026-10-09): in a mode run a held button repeats the basic fireball (spell 0) every
        // kFireballHoldTicks ticks (3.125 a second at 25 Hz; the mini fireball, the original's rapid fire, repeats every 3)
        constexpr int kFireballHoldTicks = 8;
        static int s_held_ticks[2];
        int &n = s_held_ticks[bit == 0x20];
        if (*click != 0) {
            player_queue_command(6, bit);
            *click = 0;
            n = 0;
        } else if (held != 0 && spell->type == 0 && mode_active()) {
            if (++n >= kFireballHoldTicks) {
                player_queue_command(6, bit);
                n = 0;
            }
        } else {
            n = 0;
        }
        return;
    }
    if (*click != 0 || (held != 0 && spell->cast_ticks > 0) ||
        (held != 0 && spell->unk3e != 0 && (int8_t)spell->burst > 0)) {
        player_queue_command(6, bit);
        *click = 0;
    }
}

// Input mode 0 (and 4): flying, 0x166b9.
static void local_input_flight() {
    GameState *st = g_state;
    const int lp = local_index();
    CmdPacket *pk = &st->commands[lp];
    if (pk->cmd != 0) {                                 // 0x16dcb: a command is still pending
        g_mouse_click_left = 0;
        g_mouse_click_right = 0;
        return;
    }
    player_function_keys();
    joystick_poll(0, 0x100, 0xa0, 0x280, 0x190, 0x10, 8);
    Thing *t = thing_at(thing_wrap(st->players[lp].thing));
    pk->bits = 0;

    if (g_key_down[MC_SC_CTRL]) {                       // Ctrl + 1..0: quick-select, right hand
        if ((uint8_t)(g_key_last - 2) <= 9) {
            player_queue_command(0x19, g_key_last - 2);
            g_key_last = 0;
        }
        player_mouse_steer();
        return;
    }

    if (g_key_down[MC_SC_ALT]) {
        switch (g_key_last) {
        case 0x13: player_queue_command(0xc, 0); break;         // R: record a movie
        case 0x1f: player_queue_command(0xa, 0); break;         // S: quick save
        case 0x21: player_queue_command(4, 0x20); break;        // F
        case 0x23: st->opt_interlaced ^= 1; break;              // H: VFX1 interlaced stereo
        case 0x2f: player_queue_command(4, 8); break;           // V
        case 0x32: player_queue_command(4, 0x10); break;        // M
        case 0x3b: case 0x3c: case 0x3d: case 0x3e: case 0x3f: case 0x40: case 0x41:   // F1..F7
            player_queue_command(0x1e, g_key_last - 0x3a);
            break;
        default:
            player_mouse_steer();
            return;
        }
        g_key_last = 0;
        player_mouse_steer();
        return;
    }

    if (g_key_down[MC_SC_LSHIFT] || g_key_down[MC_SC_RSHIFT]) {
        switch (g_key_last) {
        case 0x10: player_queue_command(2, 0); break;                           // Q
        case 0x12: if (cheat_gate()) player_queue_command(0x1a, 0); break;      // E
        case 0x2e: if (cheat_gate()) player_queue_command(0x1b, 0); break;      // C
        default:
            player_mouse_steer();
            return;
        }
        g_key_last = 0;
        player_mouse_steer();
        return;
    }

    if (g_key_last == 0x1a) {                           // [
        if (st->mode_3d == 0 && (int8_t)st->view_size < 0x28) st->view_size++;
        g_key_last = 0;
    }
    if (g_key_last == 0x1b) {                           // ]
        if (st->mode_3d == 0 && (int8_t)st->view_size > 0x11) st->view_size--;
        g_key_last = 0;
    }
    if (g_key_last == 0x17) {                           // I: chat line
        player_queue_command(0x10, 0);
        g_key_last = 0;
        return;
    }
    if (g_key_last == 1) {                              // Esc
        player_queue_command(0x1b, 0);
        player_queue_command(0x1d, 0);
        g_key_last = 0;
        return;
    }
    if (g_key_last == 0x13) {                           // R
        if (st->opt_allowed[10] != 0) platform(INPUT_REQ_TOGGLE_RESOLUTION);    // TODO(port): video_toggle_resolution_33600()
        g_key_last = 0;
    }
    if (g_key_last == 0x39) {                           // Space
        player_queue_command(0xf, 0);
        player_queue_command(0x1b, 0);
        player_queue_command(0x1c, 0);
        g_key_last = 0;
    }
    if (g_mouse_click_left != 0 && g_mouse_click_right != 0) {      // both buttons: the spell book
        if (!(g_cfg->flags & 0x200) && t->health >= 0) player_queue_command(0x14, 2);
        g_mouse_click_right = 0;
        g_mouse_click_left = 0;
    }
    if (g_key_last >= 2 && g_key_last <= 0xb) {         // 1..0: quick-select, left hand
        player_queue_command(0x18, g_key_last - 2);
        g_key_last = 0;
    } else if (g_key_last == 0x1c || (g_key_last == MC_SC_TAB && g_settings.keys_book_tab)) {   // Enter: the spell book
        if (t->health >= 0) player_queue_command(0x14, 2);
        g_key_last = 0;
    }
    const bool wasd = g_settings.keys_wasd;             // port: settings.h keys_wasd
    if (g_key_down[MC_SC_UP]    || (wasd && g_key_down[MC_SC_W])) player_queue_command(6, 1);
    if (g_key_down[MC_SC_DOWN]  || (wasd && g_key_down[MC_SC_S])) player_queue_command(6, 2);
    if (g_key_down[MC_SC_LEFT]  || (wasd && g_key_down[MC_SC_A])) player_queue_command(6, 4);
    if (g_key_down[MC_SC_RIGHT] || (wasd && g_key_down[MC_SC_D])) player_queue_command(6, 8);

    flight_fire(t, player_block(t)->slot_left, &g_mouse_click_left, g_mouse_held_left, 0x10);
    flight_fire(t, player_block(t)->slot_right, &g_mouse_click_right, g_mouse_held_right, 0x20);
    player_mouse_steer();
}

// Port (settings.h keys_book_tab): Tab closes the book / leaves mode 1 like Enter.
static bool book_tab_pressed() { return g_settings.keys_book_tab && g_key_last == MC_SC_TAB; }

// Input mode 1, 0x16de0.
static void local_input_mode1() {
    player_function_keys();
    if ((g_mouse_click_left != 0 && g_mouse_click_right != 0) || g_key_last == 0x1c || book_tab_pressed()) {
        g_key_last = 0;
        g_mouse_click_right = 0;
        g_mouse_click_left = 0;
        player_queue_command(0x14, 0);
    }
}

// The slot search of the three book commands: the first book slot that holds the Thing of the
// spell in book cell Config.spell_slot (0xff when there is none).
static uint8_t book_slot_of_selection(const Thing *t) {
    const PlayerBlock *P = player_block(t);
    for (int i = 0; i < 0x18; i++) {
        unsigned cell = (unsigned)(int)(int8_t)g_cfg->spell_slot;
        unsigned id = g_input_book_order[cell < 24 ? cell : 0];        // (unchecked in the original)
        if ((int32_t)(int16_t)P->spell_thing[id < 24 ? id : 0] == P->spell_slot[i]) return (uint8_t)i;
    }
    return 0xff;
}

// Input mode 2: the spell book, 0x16e31.
static void local_input_book() {
    GameState *st = g_state;
    const int lp = local_index();
    CmdPacket *pk = &st->commands[lp];
    const int w = input_book_cell_w(), h = input_book_cell_h();
    joystick_poll(1, (int16_t)(0x280 - 4 * w), 0xa2, (int16_t)(0x280 - w), (int16_t)(h * 5 + 0xa2), w, h);
    player_function_keys();
    Thing *t = thing_at(thing_wrap(st->players[lp].thing));
    if (t->health < 0) player_queue_command(0x14, 0);   // dead: the book closes

    if (g_key_last >= 2 && g_key_last <= 0xb) {         // 1..0: bind the selected spell to the key
        if ((int8_t)g_cfg->spell_slot != -1) {
            pk->cmd = 0x17;
            pk->arg = (uint8_t)(g_key_last - 2);
            pk->pad2 = 0xff;
            pk->pad2 = book_slot_of_selection(t);
        }
        g_key_last = 0;
    }
    if ((g_mouse_click_left != 0 && g_mouse_click_right != 0) || g_key_last == 0x1c || book_tab_pressed()) {   // close
        g_key_last = 0;
        g_mouse_click_right = 0;
        g_mouse_click_left = 0;
        player_queue_command(0x14, 0);
    } else if (g_mouse_click_left != 0) {               // left click: spell into the left hand
        g_mouse_click_left = 0;
        if ((int8_t)g_cfg->spell_slot != -1) {
            pk->cmd = 0x15;
            pk->arg = 0xff;
            pk->arg = book_slot_of_selection(t);
        }
    } else if (g_mouse_click_right != 0) {              // right click: into the right hand
        g_mouse_click_right = 0;
        if ((int8_t)g_cfg->spell_slot != -1) {
            pk->cmd = 0x16;
            pk->arg = 0xff;
            pk->arg = book_slot_of_selection(t);
        }
    }
    g_key_last = 0;
}

// Input mode 3: typing a chat line, 0x17181.
static void local_input_text() {
    if (g_key_last != 0 && g_key_last <= 0x7f) {
        const uint8_t ch = g_input_scancode_ascii[g_key_last];
        if (g_key_last == 0x1c) {
            player_queue_command(0x13, 0);
        } else if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                   ch == 8 || ch == 0x20) {
            player_queue_command(0x11, ch);
        }
    }
    g_key_last = 0;
}

void player_local_input() {
    const PlayerRec *rec = &g_state->players[local_index()];
    if (rec->is_computer == 1) {                        // watching a computer player: F1 quits
        if (g_key_down[MC_SC_F1]) player_queue_command(2, 0);
        return;
    }
    switch (rec->input_mode) {                          // jump table 0x1661c
    case 0: case 4: local_input_flight(); break;
    case 1: local_input_mode1(); break;
    case 2: local_input_book(); break;
    case 3: local_input_text(); break;
    default: break;
    }
}

// ---- the book hit test of the HUD (render_frame_1fab0, 0x20b44..0x20d90) -------------------------
void input_book_cell_origin(int cell, int *x, int *y) {
    *x = 0x180 + (cell & 3) * 0x40;
    *y = (g_video_mode_flags == 1 ? 0xa2 : 0xc2) + (cell >> 2) * input_book_cell_h();
}

void input_book_update_selection() {
    const Thing *t = thing_at(thing_wrap(g_state->players[local_index()].thing));
    const int cell_h = input_book_cell_h();
    int x = 0x180;
    int y = g_video_mode_flags == 1 ? 0xa2 : 0xc2;
    g_cfg->spell_slot = 0xff;
    for (int cell = 0; cell < 0x18; cell++) {
        const PlayerBlock *P = player_block(t);
        int idx = (int16_t)P->spell_thing[g_input_book_order[cell]];
        if (idx > 0 && idx < thing_pool_slots()) {          // the player owns this spell
            const Thing *spell = thing_at((unsigned)idx);
            const int32_t cost = spell->mana_cost;
            bool usable = cost == 0;
            if (!usable && P->castle != 0)
                usable = cost <= thing_at(thing_wrap(P->castle))->mana;
            if (usable && g_mouse_x >= x && g_mouse_x < x + 0x40 && g_mouse_y >= y &&
                g_mouse_y < y + cell_h)
                g_cfg->spell_slot = (uint8_t)cell;
            // (otherwise the icon is drawn: ui_draw_spell_panel_icon_22d80; empty cells: ui_draw_spell_icon_22820)
        }
        x += 0x40;
        if (x >= 0x280) {
            x = 0x180;
            y += cell_h;
        }
    }
}

// ---- creature_kill_all_17ff0 -------------------------------------------------------------------
// Walks the 20 per-type creature lists of the last thing_update_all and kills everything on them.
void creature_kill_all() {
    for (int list = 0; list < 0x14; list++) {
        unsigned guard = 0;
        for (uint32_t i = g_cfg->creature_lists[list]; i != 0 && i < (uint32_t)thing_pool_slots() && guard < (uint32_t)thing_pool_slots();
             i = thing_at(i)->next, guard++)
            thing_at(i)->health = -1;
    }
}

void input_register_handlers() {
    g_hook_player_local_input = player_local_input;
}
