// Front end of carpet.exe: frontend_menu_loop_52070 and its screens, frame-stepped.
//
// Original functions (all in 0x51ed0..0x59500 unless noted): fe_init_state_51ed0, frontend_menu_loop_52070,
// fe_screen_config_521c0, fe_check_shift_q_quit_52980, fe_wait_ticks_or_input_529d0, fe_config_draw_52ab0,
// fe_config_draw_summary_52e20, fe_config_screen_init_53070, fe_config_screen_exit_531e0,
// fe_screen_main_menu_532b0, fe_input_idle_check_53ad0, fe_menu_select_prev_53b90, fe_menu_select_next_53bc0,
// fe_menu_item_enabled_53bf0, fe_main_menu_animate_53c40, fe_main_menu_init_53d30, fe_main_menu_exit_54010,
// fe_main_menu_draw_overlay_540c0, fe_load_confirm_draw_54150, fe_save_slot_dialog_541f0,
// fe_text_entry_begin_54640, fe_dialog_draw_buttons_546e0, fe_draw_save_slot_names_54850,
// fe_screen_intro_movie_54900, fe_screen_outro_movie_54ab0, fe_screen_multiplayer_54bd0,
// fe_multiplayer_draw_slots_55210, fe_multiplayer_init_55630, fe_multiplayer_exit_557c0,
// fe_multiplayer_refresh_55870, fe_screen_level_result_55b00, fe_screen_bullfrog_logo_563c0,
// fe_screen_intel_logo_56510, fe_flic_loop_frame_56670, fe_screen_title_56730, fe_screen_language_56940,
// fe_confirm_dialog_56e20, fe_menu_quit_57270, fe_draw_centred_icon_57350, fe_menu_multiplayer_57400,
// fe_menu_start_level_57450, fe_menu_new_or_resume_game_57480, fe_draw_centered_text_57530,
// fe_menu_text_dialog_57580, fe_dialog_draw_title_a_578a0, fe_dialog_draw_title_b_57930,
// fe_fli_composite_bg_579c0, fe_build_bright_table_579f0, fe_sndsetup_read_57af0 (replaced),
// fe_input_poll_57cc0, ui_text_edit_field_58290, ui_str_prepend_58820, ui_str_delete_at_58880,
// fe_config_apply_input_device_59330, fe_highlight_masked_item_593c4, gfx_fill_rect_clipped_5940c, and the
// helpers flic_set_dest_50de0 / flic_play_chunk_50dfd / flic_decode_frame_50e88 / fli_decode_ss2_50f71 /
// fli_decode_brun_51024 (the in-memory FLIC player of the main-menu animations), vga_draw_sprite_spans_6070d
// with its sprite window (gfx_set_clip_window_65c10), mouse_cursor_set_sprite_5ba5c, music_update_65b20.
//
// Every blocking loop of the original (palette fades, FLI playback, the waits on the logo screens, the
// dialogs and text fields, the level-result reveal) is a phase of a small per-screen state machine: a
// "blocker" (fade / FLI / wait / delay) runs one step per fe_frame, the screen code continues where it
// stopped when the blocker is done. Report: docs/analysis/port_frontend.md.
#define _CRT_SECURE_NO_WARNINGS
#include "frontend.h"
#include "savegame.h"
#include "fli.h"
#include "palette_fx.h"
#include "ui_draw.h"
#include "input.h"
#include "text.h"
#include "sound.h"
#include "sndbank.h"
#include "mc_globals.h"
#include "thing.h"
#include "mcfile.h"
#include "gen/frontend_tables.h"
#include "net.h"
#include "world_set.h"
#include "settings.h"
#include "mode.h"
#include "tmaps.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// =============================================================================================
// globals
// =============================================================================================

uint8_t g_fe_leave = 0;               // DAT_0009e504
uint8_t g_fe_reload = 1;              // DAT_0012ebdc
uint8_t g_fe_game_in_progress = 1;    // DAT_0009e500 (initialised data: 1)
uint8_t g_fe_session = 0;             // DAT_0012ed30
uint8_t g_fe_lobby_players = 2;       // DAT_0012ed31
uint8_t g_fe_network = 0;             // DAT_0009e3c8
uint8_t g_fe_input_flags = 0;         // DAT_0009e583
uint8_t g_fe_input_page = 0;          // DAT_0012ed33
void (*g_fe_hook_sound_initialise)() = nullptr;

static std::string s_game_dir;
static std::string s_save_dir;        // c:\carpet.cd (markers) and its save\ subdirectory

static uint8_t  s_state = 6;          // DAT_0012ed2e
static uint8_t  s_snd_step = 7;       // DAT_0012ed29 sound-setup wizard step (7 = done: the port skips it)
static uint8_t  s_opt_sel = 0;        // DAT_0012ed2a
static uint8_t  s_opt_hover = 0xff;   // DAT_0012ed2b
static uint8_t  s_slot_mode = 0;      // DAT_0012ed2d 0 menu, 1 load, 2 save
static uint8_t  s_item = 0;           // DAT_0012ed26 hovered / selected main-menu item 1..11
static uint8_t  s_lobby_anim = 6;     // DAT_0012ed32 pointer animation frame while joining
static uint8_t  s_attract = 0;        // DAT_0012ed34 attract phase 0..2
static uint8_t  s_flags = 0;          // DAT_0012ed35: 1 screen initialised, 2 intro.pld existed, 4 intro shown,
                                      // 8 handler leaves the menu, 0x10 lobby joining, 0x20 key filter, 0x40 text entry,
                                      // 0x80 sound initialised
static uint8_t  s_attract_flags = 0;  // DAT_0012ed36: 1 attract intro, 2 attract title, 4 attract demo
static uint32_t s_frame = 0;          // DAT_0012ed10
static uint32_t s_idle_tick = 0;      // DAT_0012ed14
static uint16_t s_saved_level = 0;    // DAT_0012ed18 (attract demo)
static uint8_t  s_saved_spells[0x18]; // DAT_0012ebc0 (attract demo)
static int16_t  s_lobby_scroll = 0;   // DAT_0012ed1e
static int16_t  s_lobby_hover = 0;    // DAT_0012ed20
static int16_t  s_lobby_sel = 0;      // DAT_0012ed22
static uint16_t s_lobby_saved_level = 0;  // DAT_0012ed24
static int16_t  s_last_x = 0, s_last_y = 0;   // DAT_0012ed3a / ed38
static uint8_t  s_idle_key = 0;       // DAT_0012ed3c
static int32_t  s_idle_snap[5];       // DAT_0009e548..558
static int16_t  s_cur_x, s_cur_y, s_click_x, s_click_y;   // DAT_0012ec3c / 3e / 40 / 42
static uint8_t  s_ev0 = 0, s_ev1 = 0; // DAT_0012ec44 / ec45 edge flags (1 / 2 / 4 left, 8 / 0x10 / 0x20 middle, 0x40 / 0x80 right; ec45 bit0 right held)
static uint8_t  s_flic_transparent = 0;   // DAT_0009e468
static uint16_t s_fli_aborted = 0;    // DAT_0012eabc
static char     s_slot_names[SAVE_SLOTS][SAVE_NAME_LEN + 1];   // the buffers DAT_0009e4e8 points at (0x9e469..)
static uint8_t  s_lobby[8][3];        // DAT_0009e520 {joined, ready, anim}
static char     s_snd_name[0x20], s_mus_name[0x20], s_snd_id[0x20], s_mus_id[0x20];   // DAT_0012eb00 / eb20 / eb80 / eba0
// the ten-byte settings of sndsetup.dat in file order: DAT_0012ebfe, ebe0 (sound I/O), ec12, ebea (IRQ),
// ec26, ec30 (DMA), ec08, ebf4 (music port)
static char     s_snd_fields[8][11];

// buffers (320x200)
static uint8_t s_buf_a[64000], s_buf_b[64000];
static uint8_t *s_ed74 = s_buf_a;     // DAT_0012ed74 back buffer
static uint8_t *s_adf68 = s_buf_b;    // DAT_000adf68 background
static uint8_t s_backup1[64000];      // DAT_0012ed04 *SCREEN BACKUP
static uint8_t s_backup2[64000];      // DAT_0012ed08 *SCREEN BACKUP
static uint8_t s_mask[64000];         // DAT_0012ed00 mmmask.dat
static uint8_t s_vga[64000];          // what the last vga_blit_backbuffer_320_610f0 showed
static uint8_t s_present[64000];
static uint8_t s_pal[768];            // DAT_0012ecfc *PALETTE
static uint8_t s_bright[256];         // DAT_0012ed0c *BRIGHT TABLE

// resources
static UiSpriteTable s_sptrs, s_gcspr, s_mmspr, s_pmultspr, s_langspr, s_sfont0, s_sfont1, s_sfont2;
static FontDesc s_font0, s_font1, s_font2;   // DAT_0009e508 / 9e510 / 9e518
static mc_blob  s_globe{nullptr, 0}, s_timer{nullptr, 0}, s_title2{nullptr, 0};
static int      s_cursor = -1;        // sptrs entry of the mouse pointer, -1 hidden

// main-menu animation pointers (offsets into the blobs, 0 = none)
static long s_globe_loop = 0, s_globe_pos = 0; static int16_t s_globe_n = 0;   // DAT_0012ecd6 / ecda / ecde
static long s_timer_loop = 0, s_timer_pos = 0; static int16_t s_timer_n = 0;   // DAT_0012ece4 / ece8 / ecec
static long s_title_loop = 0, s_title_pos = 0; static int16_t s_title_n = 0;   // DAT_0012ecc8 / eccc / ecd0
static uint32_t s_title_last = 0; static bool s_title_started = false;

static uint32_t s_now = 0;            // DAT_0012eab4
static bool     s_busy = false;       // a screen is in the middle of a blocking sequence
static bool     s_left = false;       // the last fe_frame returned START_LEVEL / START_DEMO

// port (round 12): the Conquest screens (fe_mode.cpp, registered through fe_set_mode_screens) and the urn
static const FeModeScreens *s_mode_screens = nullptr;
static bool s_leave_mode = false;     // the setup screen's "Start": fe_frame returns FE_START_MODE
static bool s_lobby_conquest = false; // the lobby's urn is selected
static bool s_mp_to_setup = false;    // the lobby is left for the setup screen (and comes back)
constexpr uint8_t kUrnItem = 12;      // main-menu item id of the urn (no mmmask.dat pixel has it)
constexpr int kUrnMenuX = 0x11a, kUrnMenuY = 0xa4;     // on the grey book bottom right (320x200 pixels)
constexpr int kUrnLobbyX = 0x0c, kUrnLobbyY = 0xa6;    // on the map bottom left of the lobby

static uint8_t *cfg_raw(size_t off) { return reinterpret_cast<uint8_t *>(g_cfg) + off; }
static PlayerRec &local_rec() { return g_state->players[g_state->local_player & 7]; }

// =============================================================================================
// resources and low-level drawing
// =============================================================================================

static void fe_relocate(UiSpriteTable &t) {     // sprite_table_relocate_x2_62a80 / sprite_table_relocate_62ad0
    t.entries.clear();
    if (!t.loaded) return;
    const bool x2 = ui_lo_res();
    t.entries.resize(t.set.count);
    for (size_t i = 0; i < t.set.count; i++) {
        const mc_tab_entry &e = t.set.entries[i];
        UiSprite &s = t.entries[i];
        if (e.offset < t.set.dat.len) { s.data = t.set.dat.data + e.offset; s.size = (uint32_t)(t.set.dat.len - e.offset); }
        else { s.data = nullptr; s.size = 0; }
        s.w = (uint8_t)(x2 ? e.width << 1 : e.width);
        s.h = (uint8_t)(x2 ? e.height << 1 : e.height);
    }
}
// Fonts are only relocated (ui_font_relocate_glyphs_58f30), never doubled.
static void fe_relocate_plain(UiSpriteTable &t) {
    const uint16_t keep = g_video_mode_flags;
    g_video_mode_flags = 8;
    fe_relocate(t);
    g_video_mode_flags = keep;
}
static bool fe_load_table(UiSpriteTable &t, const char *name) {
    if (t.loaded) { mc_sprite_set_free(&t.set); t.loaded = false; t.entries.clear(); }
    if (!mc_sprite_set_load(s_game_dir.c_str(), name, &t.set)) return false;
    t.loaded = true;
    return true;
}
static void fe_free_table(UiSpriteTable &t) {
    if (t.loaded) mc_sprite_set_free(&t.set);
    t.loaded = false;
    t.entries.clear();
}
static const UiSprite *spr(const UiSpriteTable &t, unsigned i) { return ui_sprite(t, i); }
static bool fe_load_file(const char *rel, uint8_t *dst, size_t cap) {   // file_load_rnc_3cbe0
    char path[1024];
    mc_path_join(path, sizeof path, s_game_dir.c_str(), rel);
    return mc_load_rnc_into(path, dst, cap) > 0;
}
static void fe_load_blob(mc_blob &b, const char *rel) {
    if (b.data) mc_blob_free(&b);
    b.data = nullptr; b.len = 0;
    char path[1024];
    mc_path_join(path, sizeof path, s_game_dir.c_str(), rel);
    mc_read_unpacked(path, &b);
}
static void fe_free_blob(mc_blob &b) { if (b.data) mc_blob_free(&b); b.data = nullptr; b.len = 0; }

// ui_font_init_589d0(font, table, palette)
static void fe_font_init(FontDesc *f, const UiSpriteTable *t) {
    ui_font_init(f, t);
    f->colour1 = (uint8_t)palette_find_nearest(s_pal, 0xff, 0xff, 0xff);
    f->colour2 = (uint8_t)palette_find_nearest(s_pal, 0, 0, 0);
    f->flags = 3;
}
static int font_space_h(const FontDesc *f) { return (f->glyphs && f->count) ? f->glyphs[0].h : 0; }   // [font] + 5

static void fe_copy(const uint8_t *src, uint8_t *dst) { if (src != dst) std::memcpy(dst, src, 64000); }   // gfx_copy_rows_320_65b90(src, dst, 200)
static void fe_clear(uint8_t *dst) { std::memset(dst, 0, 64000); }                                        // gfx_fill_rows_320_4ce83(dst, 200, 0)
static void fe_blit() { std::memcpy(s_vga, s_ed74, 64000); }                                               // vga_blit_backbuffer_320_610f0
static void fe_swap() { uint8_t *t = s_ed74; s_ed74 = s_adf68; s_adf68 = t; }
static void fe_target() { ui_set_target(FrameBuffer{s_ed74, 320, 200}); }

// sprite window DAT_0012ed88 / ed98 / ed80 / eda8 (gfx_set_clip_window_65c10)
static int s_win_x = 0, s_win_y = 0, s_win_w = 320, s_win_h = 200;
static void fe_set_window(int x, int y, int w, int h) { s_win_x = (int16_t)x; s_win_y = (int16_t)y; s_win_w = (int16_t)w; s_win_h = (int16_t)h; }

// ui_draw_sprite_60688 -> vga_draw_sprite_spans_6070d: position relative to the sprite window, halved
// with the size in the 320x200 mode, pixels outside the window dropped.
static void fe_draw_sprite_to(uint8_t *dst, int x, int y, const UiSprite *s) {
    if (!s || !s->data || s->h == 0) return;
    int h = s->h;
    if (ui_lo_res()) { h >>= 1; x >>= 1; y >>= 1; }
    const uint8_t *p = s->data, *end = s->data + s->size;
    for (int row = 0; row < h; row++) {
        const int py = y + row;
        int px = x;
        for (;;) {
            if (p >= end) return;
            const int8_t n = (int8_t)*p++;
            if (n == 0) break;
            if (n < 0) { px += -n; continue; }
            if (p + n > end) return;
            if (py >= 0 && py < s_win_h && s_win_y + py < 200) {
                uint8_t *line = dst + (s_win_y + py) * 320;
                for (int i = 0; i < n; i++) {
                    const int dx = px + i;
                    if (dx >= 0 && dx < s_win_w && s_win_x + dx < 320 && s_win_x + dx >= 0) line[s_win_x + dx] = p[i];
                }
            }
            p += n; px += n;
        }
    }
}
static void fe_draw_sprite(int x, int y, const UiSprite *s) { fe_draw_sprite_to(s_ed74, x, y, s); }

// ui_draw_text_58ab0 into the back buffer
static void fe_text(int x, int y, FontDesc *f, const char *s) { fe_target(); ui_fe_draw_text(x, y, f, s); }
static int fe_width(const FontDesc *f, const char *s) { return ui_fe_text_width(f, s); }

// gfx_fill_rect_clipped_5940c(x, y, w, h, colour, mode): window-relative, clipped to the window; mode 0
// (fill) is the only one implemented in the original.
static void fe_fill_rect_clipped(int x, int y, int w, int h, int colour, int mode) {
    if (y >= s_win_y + s_win_h) return;      // compared with DAT_0012ed90 (window y + h) before the origin is added
    if (y < 0) { h += y; y = 0; }
    if (y + h > s_win_h) h = s_win_h - y;
    if (h <= 0) return;
    if (x >= s_win_x + s_win_w) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > s_win_w) w = s_win_w - x;
    if (w <= 0) return;
    if ((mode & 3) != 0) return;
    for (int r = 0; r < h; r++) {
        const int py = s_win_y + y + r, px = s_win_x + x;
        if (py < 0 || py >= 200) continue;
        for (int c = 0; c < w; c++) if (px + c >= 0 && px + c < 320) s_ed74[py * 320 + px + c] = (uint8_t)colour;
    }
}

// fe_highlight_masked_item_593c4(src, dst, mask, table, item)
static void fe_highlight(const uint8_t *src, uint8_t *dst, const uint8_t *mask, const uint8_t *table, uint8_t item) {
    for (int i = 0; i < 64000; i++) dst[i] = (mask[i] == item) ? table[src[i]] : src[i];
}

// mouse_cursor_set_sprite_5ba5c(DAT_0012ec4c + entry * 6) / (0)
static void fe_cursor(int entry) { s_cursor = entry; }
static void fe_cursor_state() { if (g_fe_input_flags == 0) fe_cursor(s_state); }
static void fe_cursor_hide() { if (g_fe_input_flags == 0) fe_cursor(-1); }

// fe_build_bright_table_579f0: every colour 30 % brighter (clamped at 63), nearest palette entry.
static void fe_build_bright_table() {
    for (int i = 0; i < 256; i++) {
        unsigned r = s_pal[i * 3], g = s_pal[i * 3 + 1], b = s_pal[i * 3 + 2];
        uint16_t rr = (uint16_t)((int)(r * 0x1e) / 100 + (int)r); if (rr > 0x3f) rr = 0x3f;
        uint16_t gg = (uint16_t)((int)(g * 0x1e) / 100 + (int)g); if (gg > 0x3f) gg = 0x3f;
        uint16_t bb = (uint16_t)((int)(b * 0x1e) / 100 + (int)b); if (bb > 0x3f) bb = 0x3f;
        s_bright[i] = (uint8_t)palette_find_nearest(s_pal, rr & 0xff, gg & 0xff, bb & 0xff);
    }
}

// music_update_65b20: restart the current song when it has ended (the front end's music loop).
static void fe_music_update() {
    if (!g_music_available || !g_music_on || g_music_track == 0) return;
    if (!music_song_done()) return;
    const mc_sndbank *bank = music_bank();
    uint32_t len = 0;
    const uint8_t *song = bank ? mc_sndbank_sample(bank, g_music_track, &len) : nullptr;
    sound_backend()->music_play(g_music_track, song, len);    // snd_midi_song_setup_5ec41 + snd_midi_start_song_5eab0
}
static void fe_play_sample(int sample) {   // sound_play_sample_65c70(0, sample): stops the same sample, starts it at full volume
    sound_stop_sample(0, sample);
    sound_play_sample_loud(0, sample);
}

// marker files c:\carpet.cd\intro.pld / language.inf
static bool marker_path(const char *name, char *buf, size_t cap) {
    if (s_save_dir.empty()) return false;
    std::snprintf(buf, cap, "%s/%s", s_save_dir.c_str(), name);
    return true;
}

// =============================================================================================
// port (round 12): the urn of the Conquest mode (tmaps chunk 116 = SpriteDesc 0x4d's base sprite, the spell urn
// lying in the game world), remapped from the game palette (data/palette.dat) to the screen's palette
// =============================================================================================

static struct {
    bool tried = false, ok = false;
    int w = 0, h = 0;
    std::vector<uint8_t> px;          // game palette indices, 0 = transparent
    uint8_t game_pal[768] = {};
    uint8_t remap_for[768] = {};      // the screen palette the remap tables were built for
    bool remapped = false;
    uint8_t normal[256] = {}, bright[256] = {}, dim[256] = {};
} s_urn;

static bool urn_load() {
    if (s_urn.tried) return s_urn.ok;
    s_urn.tried = true;
    char path[1024];
    mc_path_join(path, sizeof path, s_game_dir.c_str(), "data/palette.dat");
    if (mc_load_rnc_into(path, s_urn.game_pal, sizeof s_urn.game_pal) != (long)sizeof s_urn.game_pal) return false;
    mc_tmap_set set;
    if (!mc_tmap_set_load(s_game_dir.c_str(), &set)) return false;
    mc_tmap tm;
    if (mc_tmap_get(&set, 116, &tm)) {
        s_urn.w = tm.width; s_urn.h = tm.height;
        s_urn.px.assign(tm.pixels, tm.pixels + (size_t)tm.width * tm.height);
        s_urn.ok = s_urn.w > 0 && s_urn.h > 0;
        mc_tmap_free(&tm);
    }
    mc_tmap_set_free(&set);
    return s_urn.ok;
}

// nearest screen colours of every game colour: as it is, 30 % brighter (the menu's highlight) and 40 % darker
static void urn_remap() {
    if (s_urn.remapped && std::memcmp(s_urn.remap_for, s_pal, sizeof s_pal) == 0) return;
    std::memcpy(s_urn.remap_for, s_pal, sizeof s_pal);
    s_urn.remapped = true;
    bool used[256] = {};
    for (uint8_t c : s_urn.px) used[c] = true;
    for (int c = 1; c < 256; c++) {
        if (!used[c]) continue;
        const int r = s_urn.game_pal[c * 3], g = s_urn.game_pal[c * 3 + 1], b = s_urn.game_pal[c * 3 + 2];
        auto lift = [](int v) { v = v * 130 / 100; return v > 0x3f ? 0x3f : v; };
        s_urn.normal[c] = (uint8_t)palette_find_nearest(s_pal, r, g, b);
        s_urn.bright[c] = (uint8_t)palette_find_nearest(s_pal, lift(r), lift(g), lift(b));
        s_urn.dim[c] = (uint8_t)palette_find_nearest(s_pal, r * 55 / 100, g * 55 / 100, b * 55 / 100);
    }
}

// 0 = normal, 1 = bright (hover), 2 = dim (the lobby's urn when the session plays the original game)
static void urn_draw(uint8_t *dst, int x, int y, int look) {
    if (!urn_load()) return;
    urn_remap();
    const uint8_t *table = look == 1 ? s_urn.bright : look == 2 ? s_urn.dim : s_urn.normal;
    for (int r = 0; r < s_urn.h; r++) {
        const int py = y + r;
        if (py < 0 || py >= 200) continue;
        for (int c = 0; c < s_urn.w; c++) {
            const int px = x + c;
            const uint8_t v = s_urn.px[(size_t)r * s_urn.w + c];
            if (v && px >= 0 && px < 320) dst[py * 320 + px] = table[v];
        }
    }
}

bool fe_mode_menu_shown() { return g_settings.mode_menu && s_mode_screens != nullptr && urn_load(); }
static bool urn_hit(int x, int y, int ux, int uy) { return x >= ux && x < ux + s_urn.w && y >= uy && y < uy + s_urn.h; }

void fe_set_mode_screens(const FeModeScreens *screens) { s_mode_screens = screens; }
void fe_urn_rect_menu(int *x, int *y, int *w, int *h) { urn_load(); *x = kUrnMenuX; *y = kUrnMenuY; *w = s_urn.w; *h = s_urn.h; }
void fe_urn_rect_lobby(int *x, int *y, int *w, int *h) { urn_load(); *x = kUrnLobbyX; *y = kUrnLobbyY; *w = s_urn.w; *h = s_urn.h; }
void fe_tk_draw_urn(uint8_t *dst, int x, int y, bool bright) { urn_draw(dst, x, y, bright ? 1 : 0); }
void fe_tk_urn_size(int *w, int *h) { urn_load(); *w = s_urn.w; *h = s_urn.h; }

bool fe_lobby_conquest() { return s_lobby_conquest; }
void fe_lobby_set_conquest(bool on) {
    s_lobby_conquest = on && s_mode_screens != nullptr;
    if (s_lobby_conquest) mode_set_proposal(GAME_MODE_CONQUEST, *s_mode_screens->params());
    else mode_set_proposal(GAME_MODE_ORIGINAL, ModeParams{});
}

// =============================================================================================
// in-memory FLIC player: flic_play_chunk_50dfd is task C's fli_chunk_play (fli.h)
// =============================================================================================

static FliChunkState s_chunk;                   // DAT_0009e45e / 9e460 / 9e468
static uint16_t s_flic_w = 320, s_flic_h = 0;   // flic_set_dest_50de0 values for the next header-less call

static long flic_play_chunk(const mc_blob &b, long pos, uint8_t *dest, size_t cap) {
    if (!b.data || pos < 0) return 0;
    if (s_chunk.width != s_flic_w || s_chunk.height != s_flic_h) fli_chunk_set_dest(&s_chunk, s_flic_w, s_flic_h);
    s_chunk.transparent = s_flic_transparent;
    const size_t np = fli_chunk_play(&s_chunk, b.data, b.len, (size_t)pos, dest, cap);
    s_flic_w = s_chunk.width; s_flic_h = s_chunk.height;   // a 0xaf12 header changes them
    return (long)np;
}

// =============================================================================================
// input
// =============================================================================================

// fe_input_poll_57cc0, input mode DAT_0012ed2f = 2 (mouse; modes 0 and 1 are unreachable: the only write
// of DAT_0012ed2f is the 2 of fe_init_state_51ed0).
static void fe_input_poll() {
    // (DAT_0009e583 & 0x23: input_joystick_poll_5a4e0(2, 0, 0, 0x280, 0x190, 4, 4) - no joystick in the port)
    s_cur_x = g_mouse_x; s_cur_y = g_mouse_y;
    s_click_x = g_mouse_click_x; s_click_y = g_mouse_click_y;
    if (g_mouse_held_left) {
        s_ev0 &= 0xfd;
        if (s_ev0 & 4) s_ev0 &= 0xfe; else s_ev0 |= 1;
        s_ev0 |= 4;
    } else {
        s_ev0 &= 0xfe;
        if (s_ev0 & 4) s_ev0 |= 2; else s_ev0 &= 0xfd;
        s_ev0 &= 0xfb;
    }
    if (g_mouse_held_right) {
        s_ev0 &= 0x7f;
        if (s_ev1 & 1) s_ev0 &= 0xbf; else s_ev0 |= 0x40;
        s_ev1 |= 1;
    } else {
        s_ev0 &= 0xbf;
        if (s_ev1 & 1) s_ev0 |= 0x80; else s_ev0 &= 0x7f;
        s_ev1 &= 0xfe;
    }
    if (g_mouse_held_middle) {
        s_ev0 &= 0xef;
        if (s_ev0 & 0x20) s_ev0 &= 0xf7; else s_ev0 |= 8;
        s_ev0 |= 0x20;
    } else {
        s_ev0 &= 0xf7;
        if (s_ev0 & 0x20) s_ev0 |= 0x10; else s_ev0 &= 0xef;
        s_ev0 &= 0xdf;
    }
    g_mouse_click_right = 0;
    g_mouse_click_left = 0;
}

static int hx(int16_t v) { return v >> 1; }    // movsx + sar 1: 640x400 virtual -> 320x200
static bool cur_in(int x0, int x1, int y0, int y1) { const int x = hx(s_cur_x), y = hx(s_cur_y); return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
static bool click_in(int x0, int x1, int y0, int y1) { const int x = hx(s_click_x), y = hx(s_click_y); return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
static bool shift_down() { return g_key_down[0x2a] || g_key_down[0x36]; }

// fe_check_shift_q_quit_52980
static bool fe_check_shift_q_quit() {
    if (!shift_down() || g_key_last != 0x10) return false;
    g_fe_leave = 1;
    local_rec().quit = 1;
    return true;
}

// fe_input_idle_check_53ad0: 1 when nothing changed since the last call.
static bool fe_input_idle_check() {
    if (g_mouse_x == s_idle_snap[0] && g_mouse_y == s_idle_snap[1] && g_key_last == s_idle_key &&
        g_mouse_held_left == s_idle_snap[2] && g_mouse_held_right == s_idle_snap[3] && g_mouse_held_middle == s_idle_snap[4])
        return true;
    s_idle_snap[0] = g_mouse_x; s_idle_snap[1] = g_mouse_y;
    s_idle_snap[2] = (int16_t)g_mouse_held_left; s_idle_snap[3] = (int16_t)g_mouse_held_right; s_idle_snap[4] = (int16_t)g_mouse_held_middle;
    s_idle_key = g_key_last;
    return false;
}

// =============================================================================================
// blockers: fades, FLI playback, waits
// =============================================================================================

enum BlockKind { BK_NONE, BK_FADE, BK_FLI, BK_WAIT, BK_DELAY };
static struct {
    BlockKind  kind = BK_NONE;
    FliPlayer *fli = nullptr;
    bool       abort_key = false;      // DAT_0012eabe
    bool       abort_change = false;   // DAT_0009e44e
    bool       palette = false;        // DAT_0009e44c
    bool       composite = false;      // DAT_0012ea9c = fe_fli_composite_bg_579c0
    uint32_t   until = 0;
    void     (*cb)() = nullptr;
} s_blk;

static bool blocked() { return s_blk.kind != BK_NONE; }

// vga_palette_fade_61510(target, steps, 0)
static void fe_fade(const uint8_t *target, int steps) {
    palette_fade_start(target, steps);
    if (palette_fade_active()) s_blk.kind = BK_FADE;
}
static void fe_fade_out() { fe_fade(nullptr, 0x10); }
static void fe_fade_in() { fe_fade(s_pal, 0x20); }

// fli_play_508f0(abort_on_key, palette, cue script) with DAT_0009e44e (abort on any input) and the
// frame callback DAT_0012ea9c. The cue script is task C's (fli.cpp attaches it by file name).
static void fe_fli_play(const char *rel, bool abort_key, bool palette, bool abort_change, bool composite) {
    s_fli_aborted = 0;
    g_mouse_click_right = 0; g_mouse_click_left = 0; g_key_last = 0;
    input_snapshot();
    FliPlayer *f = fli_open(s_game_dir.c_str(), rel);
    if (!f) return;                       // missing / not decodable: the screen goes on at once
    s_blk.kind = BK_FLI;
    s_blk.fli = f;
    s_blk.abort_key = abort_key;
    s_blk.abort_change = abort_change;
    s_blk.palette = palette;
    s_blk.composite = composite;
}

// fe_wait_ticks_or_input_529d0(callback, n)
static void fe_wait(int n, void (*cb)()) {
    s_blk.kind = BK_WAIT;
    s_blk.until = s_now + (uint32_t)(n * 0x78);
    s_blk.cb = cb;
}

static void fli_present(FliPlayer *f) {          // movie_frame_present_50600's blit (+ the composite callback)
    if (s_blk.composite) {
        const uint8_t *px = fli_pixels(f);
        const int w = fli_width(f), h = fli_height(f);
        if (px) {
            for (int y = 0; y < 200; y++)
                for (int x = 0; x < 320; x++) {
                    const uint8_t v = (x < w && y < h) ? px[y * w + x] : 0;
                    s_ed74[y * 320 + x] = v ? v : s_backup1[y * 320 + x];     // fe_fli_composite_bg_579c0
                }
        }
    } else {
        fli_blit(f, FrameBuffer{s_ed74, 320, 200}, 0, 0);
    }
    fe_blit();
}

static void blocker_step() {
    switch (s_blk.kind) {
    case BK_NONE: return;
    case BK_FADE:
        palette_fade_step();
        if (!palette_fade_active()) s_blk.kind = BK_NONE;
        return;
    case BK_FLI: {
        FliPlayer *f = s_blk.fli;
        bool done = false;
        // movie_wait_frame_50430: abort checks
        if (s_blk.abort_change && input_changed()) { s_fli_aborted = 1; done = true; }
        else if (s_blk.abort_key && (g_key_last != 0 || g_mouse_click_left != 0 || g_mouse_click_right != 0)) { s_fli_aborted = 1; done = true; }
        else if (fli_current_frame(f) >= fli_frame_count(f) - 2) done = true;   // the last shown frame (0 .. count - 2)
        else if (fli_frame_due(f, s_now)) {
            bool pal = false;
            if (!fli_next_frame(f, s_now, &pal)) done = true;
            else {
                if (s_blk.palette && pal && fli_palette6(f)) palette_display_set(fli_palette6(f));
                fli_present(f);
            }
        }
        if (done) { fli_close(f); s_blk.fli = nullptr; s_blk.kind = BK_NONE; s_blk.composite = false; }
        return;
    }
    case BK_WAIT: {
        bool done = false;
        if (fe_check_shift_q_quit()) done = true;
        if (g_mouse_click_left || g_mouse_click_right || g_mouse_click_middle || g_key_last) {
            g_key_last = 0; g_mouse_click_middle = 0; g_mouse_click_right = 0; g_mouse_click_left = 0;
            done = true;
        }
        if (s_blk.until < s_now) done = true;
        if (!done && s_blk.cb) s_blk.cb();
        if (done) s_blk.kind = BK_NONE;
        return;
    }
    case BK_DELAY:
        if (s_now - s_blk.until >= 0x1e0) s_blk.kind = BK_NONE;
        return;
    }
}

// =============================================================================================
// text entry field (ui_text_edit_field_58290), one loop iteration per frame
// =============================================================================================

static struct {
    int x, y, maxlen, colour, align, labelw;
    const char *label;
    char *str;                 // the edited buffer (maxlen + 1 bytes at least)
    char *cur, *end;
    void (*cb)();
    FontDesc *font;
    UiClipRect clip;
} s_edit;

static void str_prepend(char *at, const char *s) {            // ui_str_prepend_58820(at, s)
    const size_t n = std::strlen(s);
    std::memmove(at + n, at, std::strlen(at) + 1);
    std::memcpy(at, s, n);
}
static void str_delete(char *at, int n) { std::memmove(at, at + n, std::strlen(at + n) + 1); }   // ui_str_delete_at_58880

static void edit_begin(int x, int y, int maxlen, const char *label, char *str, int colour, void (*cb)(), int align, FontDesc *font) {
    s_edit.x = x; s_edit.y = y; s_edit.maxlen = maxlen; s_edit.label = label; s_edit.str = str;
    s_edit.colour = colour; s_edit.cb = cb; s_edit.align = align; s_edit.font = font;
    s_edit.cur = str;
    s_edit.end = str + std::strlen(str);
    g_mouse_click_right = 0; g_mouse_click_left = 0;
    ui_get_clip_rect(&s_edit.clip);
    s_edit.labelw = (label ? fe_width(font, label) : 0) + x;
}

// Returns 0 while editing, 1 accepted (Enter / click), 2 cancelled (Esc).
static int edit_step() {
    int result = 0;
    char *const limit = s_edit.str + s_edit.maxlen;
    if (g_mouse_click_left) result = 1;
    const uint8_t key = g_key_last;
    switch (key) {
    case 0x36: case 0x52: case 0x0f: case 0x2a: break;              // shifts, Insert, Tab: nothing
    case 0x01: result = 2; break;                                    // Esc
    case 0x1c: result = 1; break;                                    // Enter
    case 0x0e: if (s_edit.cur > s_edit.str) { s_edit.cur--; s_edit.end--; str_delete(s_edit.cur, 1); } break;   // Backspace
    case 0x53: if (s_edit.cur < s_edit.end) { s_edit.end--; str_delete(s_edit.cur, 1); } break;                 // Delete
    case 0x4b: if (s_edit.cur > s_edit.str) s_edit.cur--; break;    // Left
    case 0x4d: if (s_edit.cur < s_edit.end) s_edit.cur++; break;    // Right
    case 0x47: s_edit.cur = s_edit.str; break;                       // Home
    case 0x4f: s_edit.cur = s_edit.end; break;                       // End
    default: {
        if (s_flags & 0x20) {
            const uint8_t ok = key < 57 ? (shift_down() ? fe_scan_filter_shift[key] : fe_scan_filter[key]) : 0;
            if (!ok) break;
        }
        if (key == 0 || key > 0x39) break;
        if (!(s_edit.end < limit)) break;                            // insert mode (always on)
        const char c = (char)(shift_down() ? fe_scan_ascii_shift[key] : fe_scan_ascii[key]);
        if (c == 0) break;   // port: the original "inserts" an empty string and still moves the cursor past the end
        const char cs[2] = {c, 0};
        str_prepend(s_edit.cur, cs);
        if (s_edit.cur < limit) { s_edit.cur++; s_edit.end++; }
        const int lim = s_edit.align == 1 ? s_edit.clip.w - 2 * s_edit.x : s_edit.clip.w - s_edit.x;
        if (fe_width(s_edit.font, s_edit.str) >= lim && s_edit.cur > s_edit.str) {
            s_edit.cur--; s_edit.end--;
            str_delete(s_edit.cur, 1);
        }
        break;
    }
    }
    g_key_last = 0;
    if (s_edit.cb) s_edit.cb();
    s_edit.font->colour1 = (uint8_t)s_edit.colour;
    fe_text(s_edit.x, s_edit.y, s_edit.font, s_edit.label);
    int sx;
    if (s_edit.align == 0) sx = s_edit.labelw;
    else { const int w = fe_width(s_edit.font, s_edit.str); sx = (s_edit.clip.w - w) / 2; }
    if (s_edit.align == 0 || s_edit.align == 1) fe_text(sx, s_edit.y, s_edit.font, s_edit.str);
    // cursor: an underline as wide as the character under it
    char prefix[256];
    std::snprintf(prefix, sizeof prefix, "%s", s_edit.str);
    const size_t ci = (size_t)(s_edit.cur - s_edit.str);
    char under = ci < sizeof prefix ? prefix[ci] : 0;
    if (under == 0) under = ' ';
    if (ci < sizeof prefix) prefix[ci] = 0;
    const unsigned gi = (uint8_t)(under - 0x20);
    const UiSprite *g = (s_edit.font->glyphs && gi < s_edit.font->count) ? &s_edit.font->glyphs[gi] : nullptr;
    const int gw = g ? g->w : 0, gh = g ? g->h : 0;
    const int cy = s_edit.y + gh - 2 + s_edit.clip.y0;
    int cx;
    if (s_edit.align == 0) cx = fe_width(s_edit.font, prefix) + s_edit.labelw + s_edit.clip.x0;
    else cx = (s_edit.clip.w - fe_width(s_edit.font, s_edit.str)) / 2 + fe_width(s_edit.font, prefix) + s_edit.clip.x0;
    fe_fill_rect_clipped(cx, cy, gw, 2, 0xff, 0);
    fe_blit();
    if (result) {
        g_mouse_click_right = 0; g_mouse_click_left = 0; g_key_last = 0;
        g_key_down[1] = 0; g_key_down[0x1c] = 0;
    }
    return result;
}

// =============================================================================================
// shared main-menu drawing
// =============================================================================================

static int item_kind(int item) { return (item >= 0 && item < 13) ? fe_menu_items[item * 3 + 2] : 0; }
static bool item_has_handler(int item) { return (item >= 0 && item < 13) && (fe_menu_items[item * 3] | fe_menu_items[item * 3 + 1]) != 0; }
static char *slot_name(int kind) { static char dummy[3] = "  "; return (kind >= 1 && kind <= 6) ? s_slot_names[kind - 1] : dummy; }   // DAT_0009e4e4[kind]

// fe_draw_save_slot_names_54850
static void fe_draw_save_slot_names() {
    s_font1.colour1 = (uint8_t)palette_find_nearest(s_pal, 0x3f, 0x3f, 0);
    ui_push_clip_rect();
    for (int i = 0; i < 6; i++) {
        const int32_t *r = &fe_slot_rects[i * 4];
        ui_set_clip_rect(r[0], r[1], r[2], r[3]);
        const int y = (r[3] - font_space_h(&s_font1)) / 2;
        fe_text(4, y, &s_font1, s_slot_names[i]);
    }
    ui_pop_clip_rect();
}

// fe_main_menu_draw_overlay_540c0 (into the back buffer, which the callers swap with the background)
static void fe_main_menu_draw_overlay() {
    if (s_slot_mode == 1 || s_slot_mode == 2) { fe_draw_save_slot_names(); return; }
    if (s_slot_mode != 0) return;
    fe_draw_sprite(0x166, 0xa, spr(s_mmspr, 1));
    fe_draw_sprite(0x150, 0x56, spr(s_mmspr, 2));
    if (fe_mode_menu_shown()) urn_draw(s_ed74, kUrnMenuX, kUrnMenuY, 0);   // port (round 12): the Conquest urn
}
static void overlay_into_bg() { fe_swap(); fe_main_menu_draw_overlay(); fe_swap(); }

// fe_menu_item_enabled_53bf0
static bool fe_menu_item_enabled() {
    const uint8_t i = s_item;
    if (i == kUrnItem) return s_slot_mode == 0 && fe_mode_menu_shown();    // port (round 12)
    if (i > 7) {
        if (i == 0xb) return g_fe_game_in_progress == 0;
        return s_slot_mode != 0;
    }
    if (i == 3) return g_fe_network == 1;
    if (i == 7) return s_slot_mode != 0;
    return true;
}
// port (round 12): the Tab cycle includes the urn (item 12) when it is shown
static uint8_t fe_menu_last_item() { return fe_mode_menu_shown() ? kUrnItem : 0xb; }
static void fe_menu_select_prev() { do { s_item--; if (s_item < 1) s_item = fe_menu_last_item(); } while (!fe_menu_item_enabled()); }   // 53b90
static void fe_menu_select_next() { do { s_item++; if (s_item > fe_menu_last_item()) s_item = 1; } while (!fe_menu_item_enabled()); }   // 53bc0
// The item under the pointer: the urn's rectangle first (port, round 12), then mmmask.dat.
static uint8_t fe_menu_item_at(int x, int y) {
    if (s_slot_mode == 0 && fe_mode_menu_shown() && urn_hit(x, y, kUrnMenuX, kUrnMenuY)) return kUrnItem;
    return s_mask[y * 320 + x];
}

// dialog callbacks
static void fe_load_confirm_draw() {             // 0x54150
    fe_set_window(0x41, 0x4b, 0xbd, 0x2c);
    fe_draw_sprite(0x18, 8, spr(s_mmspr, 1));
    fe_text(0x1e, 0xc, &s_font1, slot_name(item_kind(s_item)));
    fe_set_window(0, 0, 0x140, 0xc8);
}
static void fe_dialog_draw_buttons() {           // 0x546e0
    fe_copy(s_adf68, s_ed74);
    fe_set_window(0x41, 0x4b, 0xbd, 0x2c);
    fe_draw_sprite(0x18, 8, spr(s_mmspr, 2));
    fe_set_window(0, 0, 0x140, 0xc8);
    if (s_flags & 0x40) return;
    fe_text(0x1e, 0xc, &s_font1, slot_name(item_kind(s_item)));
    fe_draw_sprite(0x88, 0xd2, spr(s_mmspr, 5));
    fe_draw_sprite(0x1e0, 0xd2, spr(s_mmspr, 6));
}
static void fe_draw_centred_icon() {             // 0x57350
    fe_set_window(0x41, 0x4b, 0xbd, 0x2c);
    const UiSprite *s = spr(s_mmspr, 7);
    fe_draw_sprite((int16_t)(0xbd - (s->w >> 1)), (int16_t)(0x2c - (s->h >> 1)), s);
    fe_set_window(0, 0, 0x140, 0xc8);
}
static void fe_draw_centered_text() {            // 0x57530: "New Game? Yes/No"
    UiClipRect r; ui_get_clip_rect(&r);
    const char *t = text_get(36);
    fe_text((r.w - fe_width(&s_font1, t)) / 2, 0xc, &s_font1, t);
}
static void fe_dialog_draw_title(int text) {     // 0x578a0 / 0x57930
    UiClipRect r; ui_get_clip_rect(&r);
    fe_copy(s_adf68, s_ed74);
    const char *t = text_get(text);
    fe_text((r.w - fe_width(&s_font1, t)) / 2, 0xa, &s_font1, t);
}
static void fe_dialog_draw_title_a() { fe_dialog_draw_title(34); }
static void fe_dialog_draw_title_b() { fe_dialog_draw_title(35); }

// ---- fe_confirm_dialog_56e20(callback): scroll animation, then Yes / No ----
static struct { int phase = 0; void (*cb)() = nullptr; int result = 0; } s_cd;

static void confirm_begin(void (*cb)()) { s_cd.phase = 0; s_cd.cb = cb; s_cd.result = 0; }
// Returns -1 while running, else 0 / 1.
static int confirm_step() {
    switch (s_cd.phase) {
    case 0:
        fe_copy(s_adf68, s_backup1);
        fe_cursor_hide();
        fe_blit();
        fe_fli_play("intro\\scroll.dat", false, false, false, true);
        s_cd.phase = 1;
        if (blocked()) return -1;
        [[fallthrough]];
    case 1:
        fe_copy(s_ed74, s_adf68);
        fe_cursor_state();
        ui_push_clip_rect();
        ui_set_clip_rect(0x50, 0x49, 0x9f, 0x30);
        s_cd.phase = 2;
        [[fallthrough]];
    case 2: {
        bool done = false;
        fe_copy(s_adf68, s_ed74);
        fe_input_poll();
        fe_draw_sprite(0x88, 0xd2, spr(s_mmspr, 5));
        fe_draw_sprite(0x1e0, 0xd2, spr(s_mmspr, 6));
        if (s_cd.cb) s_cd.cb();
        if ((s_ev0 & 1) || g_key_down[1] || g_key_down[0x1c]) {
            if (click_in(0x44, 0x51, 0x6a, 0x74) || g_key_down[0x1c]) { done = true; s_cd.result = 1; }
            else if (click_in(0xf0, 0xfa, 0x69, 0x73) || g_key_down[1]) { done = true; s_cd.result = 0; }
            g_mouse_click_left = 0;
            s_ev0 &= 0xfe;
        }
        fe_blit();
        if (!done) return -1;
        ui_pop_clip_rect();
        fe_copy(s_backup1, s_adf68);
        fe_copy(s_adf68, s_ed74);
        fe_blit();
        g_mouse_click_right = 0; g_mouse_click_left = 0;
        s_ev0 &= 0xbe;
        g_key_down[1] = 0; g_key_down[0x1c] = 0;
        s_cd.phase = 0;
        return s_cd.result;
    }
    }
    return -1;
}

// ---- fe_save_slot_dialog_541f0 ----
static struct { int phase = 0; int result = 0; bool done = false; } s_sd;

static void text_entry_begin() {                 // fe_text_entry_begin_54640
    s_flags |= 0x40;
    g_key_down[0x1c] = 0; g_mouse_click_right = 0; g_key_down[1] = 0; g_mouse_click_middle = 0; g_mouse_click_left = 0;
    edit_begin(0x1e, 0xc, 0x14, nullptr, slot_name(item_kind(s_item)), 0xff, fe_dialog_draw_buttons, 0, &s_font1);
}
static bool text_entry_step() {
    if (edit_step() == 0) return false;
    s_flags &= 0xbf;
    return true;
}

static int save_dialog_step() {
    for (;;) {
        switch (s_sd.phase) {
        case 0:
            s_flags &= 0xbf;
            s_sd.result = 0; s_sd.done = false;
            fe_copy(s_adf68, s_backup1);
            fe_cursor_hide();
            fe_blit();
            fe_fli_play("intro\\scroll.dat", false, false, false, true);
            s_sd.phase = 1;
            if (blocked()) return -1;
            break;
        case 1:
            fe_copy(s_ed74, s_adf68);
            fe_cursor_state();
            ui_push_clip_rect();
            ui_set_clip_rect(0x50, 0x49, 0x9f, 0x30);
            if (std::strcmp(slot_name(item_kind(s_item)), "--") == 0) { text_entry_begin(); s_sd.phase = 2; }
            else s_sd.phase = 3;
            break;
        case 2:                                    // the name of an empty slot is entered first
            if (!text_entry_step()) return -1;
            s_sd.phase = 3;
            return -1;
        case 3: {                                  // one iteration of the dialog loop
            fe_input_poll();
            if (g_key_down[0x1c]) {
                s_sd.done = true; s_sd.result = 1;
                g_mouse_click_left = 0; s_ev0 &= 0xfe; g_key_down[0x1c] = 0; g_key_down[1] = 0;
            }
            if (g_key_down[1]) {
                s_sd.done = true; s_sd.result = 0;
                g_mouse_click_left = 0; s_ev0 &= 0xfe; g_key_down[0x1c] = 0; g_key_down[1] = 0;
            }
            if (s_ev0 & 1) {
                if (click_in(0x44, 0x51, 0x6a, 0x74)) { s_sd.done = true; s_sd.result = 1; }
                if (click_in(0xf0, 0xfa, 0x69, 0x73)) { s_sd.done = true; s_sd.result = 0; }
                if (click_in(0x6e, 0xe6, 0x55, 0x5f)) { text_entry_begin(); s_sd.phase = 4; return -1; }
                g_mouse_click_left = 0; g_key_down[0x1c] = 0; g_key_down[1] = 0; s_ev0 &= 0xfe;
            }
            s_sd.phase = 5;
            break;
        }
        case 4:                                    // name field clicked inside the loop
            if (!text_entry_step()) return -1;
            g_key_down[0x1c] = 0; g_key_down[1] = 0;
            g_mouse_click_left = 0; s_ev0 &= 0xfe;
            s_sd.phase = 5;
            break;
        case 5:
            fe_dialog_draw_buttons();
            fe_blit();
            if (!s_sd.done) { s_sd.phase = 3; return -1; }
            ui_pop_clip_rect();
            fe_copy(s_backup1, s_adf68);
            fe_copy(s_adf68, s_ed74);
            fe_blit();
            g_mouse_click_right = 0; g_mouse_click_left = 0;
            s_ev0 &= 0xbe;
            g_key_down[1] = 0; g_key_down[0x1c] = 0;
            s_sd.phase = 0;
            return s_sd.result;
        }
    }
}

// ---- fe_menu_text_dialog_57580 (item 2: player name and call-name) ----
static int s_td_phase = 0;
static bool text_dialog_step() {
    switch (s_td_phase) {
    case 0:
        fe_copy(s_adf68, s_backup1);
        fe_cursor_hide();
        fe_blit();
        fe_fli_play("intro\\scroll.dat", false, false, false, true);
        s_td_phase = 1;
        if (blocked()) return false;
        [[fallthrough]];
    case 1:
        fe_copy(s_ed74, s_adf68);
        fe_cursor_state();
        ui_push_clip_rect();
        ui_set_clip_rect(0x46, 0x49, 0xb3, 0x30);
        edit_begin(8, 0x19, 0x1e, nullptr, reinterpret_cast<char *>(cfg_raw(0x1d)), 0xff, fe_dialog_draw_title_a, 1, &s_font1);
        s_td_phase = 2;
        [[fallthrough]];
    case 2:
        if (edit_step() == 0) return false;
        s_flags |= 0x20;
        g_key_down[0x1c] = 0;
        s_ev0 &= 0xfe;
        edit_begin(8, 0x19, 8, nullptr, reinterpret_cast<char *>(cfg_raw(0x3d)), 0xff, fe_dialog_draw_title_b, 1, &s_font1);
        s_td_phase = 3;
        return false;
    case 3:
        if (edit_step() == 0) return false;
        g_key_down[0x1c] = 0;
        s_flags &= 0xdf;
        s_ev0 &= 0xfe;
        fe_copy(s_backup1, s_adf68);
        s_state = 2;
        overlay_into_bg();
        fe_copy(s_backup1, s_adf68);
        fe_copy(s_adf68, s_ed74);
        fe_blit();
        ui_pop_clip_rect();
        s_td_phase = 0;
        return true;
    }
    return true;
}

// =============================================================================================
// state 2: main menu
// =============================================================================================

enum MmPhase { MM_IDLE, MM_INIT1, MM_INIT2, MM_MODAL, MM_QUIT_EXIT, MM_EXIT };
enum Modal { MOD_LOAD, MOD_SAVE, MOD_QUIT, MOD_NEWGAME, MOD_TEXT };
static MmPhase s_mm = MM_IDLE;
static Modal   s_modal = MOD_LOAD;
static int     s_mm_kind = 0;
static bool    s_mm_esi = false, s_mm_edi = false, s_mm_ebp = false;

// fe_main_menu_animate_53c40: the two embedded FLICs on every other frame.
static void fe_main_menu_animate() {
    if ((s_frame & 1) != 1 || s_state != 2) return;
    long np = flic_play_chunk(s_globe, s_globe_pos, s_backup2, sizeof s_backup2);
    s_globe_pos = np;
    s_globe_n++;
    if (s_globe_n == 0x1f) { s_globe_n = 1; s_globe_pos = s_globe_loop; }
    else if (s_globe_n == 1 && s_globe_loop == 0) s_globe_loop = np;
    if (g_fe_game_in_progress) return;
    np = flic_play_chunk(s_timer, s_timer_pos, s_backup2, sizeof s_backup2);
    s_timer_pos = np;
    s_timer_n++;
    if (s_timer_n == 4) { s_timer_n = 1; s_timer_pos = s_timer_loop; }
    else if (s_timer_n == 1 && s_timer_loop == 0) s_timer_loop = np;
}

// fe_main_menu_init_53d30, in three parts around its two fades.
static void mm_init_begin() {
    s_flic_w = 0x140; s_flic_h = 0;           // flic_set_dest_50de0(0x140, 0)
    fe_fade_out();
}
static void mm_init_mid() {
    sound_load_bank(s_game_dir.c_str(), 0xd);
    music_load_bank(s_game_dir.c_str(), 0);
    fe_load_table(s_mmspr, "data/screens/mmspr");                // resource list 0x512b4
    fe_load_file("data\\screens\\mmmask.dat", s_mask, sizeof s_mask);
    fe_load_table(s_sfont1, "data/screens/sfont1");
    fe_load_blob(s_globe, "data\\screens\\globe.dat");
    fe_load_blob(s_timer, "data\\screens\\timer.dat");
    fe_relocate(s_mmspr);
    fe_relocate_plain(s_sfont1);
    fe_font_init(&s_font1, &s_sfont1);
    s_font1.flags &= 0xfffd;
    fe_load_file("data\\screens\\mainmenu.dat", s_adf68, 64000);
    fe_copy(s_adf68, s_backup2);
    fe_load_file("data\\screens\\mainmenu.pal", s_pal, sizeof s_pal);
    fe_build_bright_table();
    savegame_read_names(s_slot_names);
    overlay_into_bg();
    fe_copy(s_adf68, s_ed74);
    fe_blit();
    fe_cursor_state();
    fe_fade_in();
}
static void mm_init_end() {
    s_globe_loop = 0; s_timer_loop = 0; s_timer_n = 0;
    s_flags |= 1;
    s_item = 0;
    s_globe_pos = 0; s_globe_n = 0;
    s_timer_pos = 0;
    s_flic_transparent = 1;
    s_ev0 &= 0xbe;
    s_idle_tick = s_now;
    input_snapshot();
    music_play_track(4);
    if (s_attract_flags & 4) {                  // back from the attract demo
        g_cfg->level = s_saved_level;
        std::memcpy(reinterpret_cast<uint8_t *>(g_state) + 0x3bd6, s_saved_spells, 0x18);
        s_attract_flags &= 0xfb;
        g_cfg->flags &= 0xffdb;
    }
}

// fe_main_menu_exit_54010
static void mm_exit_begin() { if (s_state != 5) fe_fade_out(); }
static void mm_exit_end() {
    if (s_state != 5) {
        fe_cursor_hide();
        fe_clear(s_ed74);
        fe_blit();
    }
    fe_free_table(s_mmspr); fe_free_table(s_sfont1);
    fe_free_blob(s_globe); fe_free_blob(s_timer);
    s_flic_transparent = 0;
    s_flags &= 0xfe;
}

static void mm_restore_bg_overlay() { fe_copy(s_backup2, s_adf68); overlay_into_bg(); }

// Part B of the frame (0x53866..): highlight, blit, attract timer, exit.
static void mm_part_b() {
    if ((s_item == 0xb && g_fe_game_in_progress) || s_item == 0) fe_highlight(s_adf68, s_ed74, s_mask, s_bright, 0xff);
    else fe_highlight(s_adf68, s_ed74, s_mask, s_bright, s_item);
    if (s_item == kUrnItem && fe_menu_item_enabled()) urn_draw(s_ed74, kUrnMenuX, kUrnMenuY, 1);   // port: urn hover
    // (DAT_0009e583 & 0x23: the pointer is drawn into the back buffer here; the port draws it in fe_frame)
    fe_blit();
    if (fe_input_idle_check()) {
        if (s_idle_tick + 0x12c0 <= s_now) {
            s_idle_tick = s_now;
            switch (s_attract) {
            case 0: s_state = 0; s_attract_flags |= 1; music_stop(); break;
            case 1: s_state = 8; s_attract_flags |= 2; music_stop(); break;
            case 2:
                s_attract_flags |= 4;
                s_mm_ebp = true;
                s_saved_level = g_cfg->level;
                g_fe_leave = 1;
                std::memcpy(s_saved_spells, reinterpret_cast<uint8_t *>(g_state) + 0x3bd6, 0x18);
                g_cfg->movie = 0;
                cfg_raw(0xa1)[0] = 3;
                { const uint32_t v = 200; std::memcpy(cfg_raw(0xa2), &v, 4); }
                g_cfg->flags |= 0x24;
                break;
            }
            s_attract++;
            if (s_attract == 3) s_attract = 0;
            s_mm_esi = true;
        }
    } else {
        s_idle_tick = s_now;
    }
    if (s_mm_esi) {
        mm_exit_begin();
        s_mm = MM_EXIT;
        if (blocked()) { s_busy = true; return; }
        mm_exit_end();
        if (s_mm_ebp) s_state = 2;
        s_mm = MM_IDLE;
    }
    s_busy = false;
}

// After a slot dialog (0x53735 / 0x53772).
static void mm_after_slot_action() {
    s_mm_edi = true;
    mm_restore_bg_overlay();
    s_slot_mode = 0;
}

static void mm_modal_done(int result) {
    switch (s_modal) {
    case MOD_LOAD: if (result) savegame_load(s_mm_kind - 1, s_slot_names[s_mm_kind - 1]); mm_after_slot_action(); break;
    case MOD_SAVE: if (result) savegame_save(s_mm_kind - 1, s_slot_names[s_mm_kind - 1]); mm_after_slot_action(); break;
    case MOD_NEWGAME:
        if (result) {                          // fe_menu_new_or_resume_game_57480 "yes"
            g_cfg->level = 0;
            s_state = 5;
            std::memset(local_rec().blk.spell_found, 0, 0x18);
            g_fe_leave = 1;
            s_flags |= 8;
        }
        if (s_flags & 8) { s_flags &= 0xf7; s_mm_esi = true; }
        break;
    case MOD_QUIT:                              // fe_menu_quit_57270 "no" (the "yes" branch is MM_QUIT_EXIT)
        fe_copy(s_backup2, s_adf68);
        s_slot_mode = 0;
        overlay_into_bg();
        break;
    case MOD_TEXT: break;
    }
}

static void mm_start_modal(Modal m) {
    s_modal = m;
    s_mm = MM_MODAL;
    s_busy = true;
}
// Runs the modal; true when it finished (result in *res).
static bool mm_modal_step(int *res) {
    switch (s_modal) {
    case MOD_LOAD: case MOD_QUIT: case MOD_NEWGAME: { const int r = confirm_step(); if (r < 0) return false; *res = r; return true; }
    case MOD_SAVE: { const int r = save_dialog_step(); if (r < 0) return false; *res = r; return true; }
    case MOD_TEXT: if (!text_dialog_step()) return false; *res = 0; return true;
    }
    return true;
}

// The frame body from the animation to the click handling (0x532d0..0x53866). Returns false when a
// modal was started (the rest of the frame runs when it is done).
static bool mm_part_a() {
    fe_main_menu_animate();
    fe_copy(s_backup2, s_adf68);
    overlay_into_bg();
    if (g_key_down[0x0f]) {                     // Tab / Shift+Tab
        g_key_down[0x0f] = 0;
        if (shift_down()) fe_menu_select_prev(); else fe_menu_select_next();
    }
    if ((hx(s_cur_x) != s_last_x || hx(s_cur_y) != s_last_y) && hx(s_cur_x) < 0x140 && hx(s_cur_y) < 0xc8) {
        s_last_x = (int16_t)hx(s_cur_x); s_last_y = (int16_t)hx(s_cur_y);
        s_item = fe_menu_item_at(s_last_x, s_last_y);
        if (!fe_menu_item_enabled()) s_item = 0;
    }
    if (g_key_down[0x1c]) { g_key_down[0x1c] = 0; s_ev0 |= 1; }    // Enter = click
    if ((s_ev0 & 0x40) || g_key_last == 1) {                          // right button / Esc: leave the slot mode
        g_key_last = 0;
        s_ev0 &= 0xbf;
        if (s_slot_mode) s_slot_mode = 0;
        s_item = fe_menu_item_at(s_last_x, s_last_y);
        if (!fe_menu_item_enabled()) s_item = 0;
    }
    if (!(s_ev0 & 1)) return true;
    s_ev0 &= 0xfe;
    if (s_item == 0xb && g_fe_game_in_progress) return true;
    if (s_item == kUrnItem) {                      // port (round 12): the urn opens the Conquest setup screen
        if (fe_menu_item_enabled()) {
            s_mode_screens->open_setup(2);
            s_state = 11;
            s_mm_esi = true;
        }
        return true;
    }
    if (item_has_handler(s_item)) {
        switch (s_item) {
        case 1:                                    // fe_menu_new_or_resume_game_57480
            if (g_fe_game_in_progress) {
                g_fe_game_in_progress = 0;
                s_flags |= 8;
                s_state = 5;
                g_fe_leave = 1;
            } else {
                confirm_begin(fe_draw_centered_text);
                mm_start_modal(MOD_NEWGAME);
                return false;
            }
            break;
        case 2:                                    // fe_menu_text_dialog_57580
            s_td_phase = 0;
            mm_start_modal(MOD_TEXT);
            return false;
        case 3:                                    // fe_menu_multiplayer_57400
            if (g_fe_network) {
                s_state = 4;
                std::memcpy(reinterpret_cast<uint8_t *>(g_state) + 0x2409, &g_state->players[0], 0x801);
                s_flags |= 8;
                g_cfg->saved_level = g_cfg->level;
            }
            break;
        case 4:                                    // fe_menu_quit_57270
            confirm_begin(fe_draw_centred_icon);
            mm_start_modal(MOD_QUIT);
            return false;
        case 0xb:                                  // fe_menu_start_level_57450
            s_flags |= 8;
            g_fe_leave = 1;
            g_fe_game_in_progress = 0;
            s_state = 5;
            break;
        default: break;
        }
        if (s_flags & 8) { s_flags &= 0xf7; s_mm_esi = true; }
        return true;
    }
    const int kind = item_kind(s_item);
    if (kind == 0) return true;
    s_mm_kind = kind;
    if (s_slot_mode == 1 && kind >= 1 && kind <= 6) {
        confirm_begin(fe_load_confirm_draw);
        mm_start_modal(MOD_LOAD);
        return false;
    }
    if (s_slot_mode == 2 && kind >= 1 && kind <= 6) {
        s_sd.phase = 0;
        mm_start_modal(MOD_SAVE);
        return false;
    }
    if (s_slot_mode == 0 && kind == 3) { s_state = 1; s_mm_esi = true; }
    // 0x53772
    if (!s_mm_edi && s_slot_mode == 0 && (kind == 1 || kind == 2)) {
        s_slot_mode = (uint8_t)kind;
        mm_restore_bg_overlay();
    }
    return true;
}

static void fe_screen_main_menu() {             // fe_screen_main_menu_532b0
    switch (s_mm) {
    case MM_IDLE:
        s_mm_esi = s_mm_edi = s_mm_ebp = false;
        if (!(s_flags & 1)) {
            mm_init_begin();
            s_mm = MM_INIT1;
            if (blocked()) { s_busy = true; return; }
        } else {
            if (mm_part_a()) mm_part_b();
            return;
        }
        [[fallthrough]];
    case MM_INIT1:
        mm_init_mid();
        s_mm = MM_INIT2;
        if (blocked()) { s_busy = true; return; }
        [[fallthrough]];
    case MM_INIT2:
        mm_init_end();
        s_mm = MM_IDLE;
        if (mm_part_a()) mm_part_b();
        return;
    case MM_MODAL: {
        int res = 0;
        if (!mm_modal_step(&res)) { s_busy = true; return; }
        s_mm = MM_IDLE;
        if (s_modal == MOD_QUIT && res) {         // "Quit to DOS": yes
            mm_exit_begin();
            s_mm = MM_QUIT_EXIT;
            if (blocked()) { s_busy = true; return; }
        } else {
            mm_modal_done(res);
            mm_part_b();
            return;
        }
    }
        [[fallthrough]];
    case MM_QUIT_EXIT:
        mm_exit_end();
        local_rec().quit = 1;
        g_fe_leave = 1;
        s_mm = MM_IDLE;
        mm_part_b();
        return;
    case MM_EXIT:
        mm_exit_end();
        if (s_mm_ebp) s_state = 2;
        s_mm = MM_IDLE;
        s_busy = false;
        return;
    }
}

// =============================================================================================
// state 1: game configuration (input device; the sound-setup wizard is skipped)
// =============================================================================================

static int s_cfg_phase = 0;
static bool s_cfg_confirm = false, s_cfg_esc = false;

static void fe_config_draw_summary(int y) {      // fe_config_draw_summary_52e20
    UiClipRect r; ui_get_clip_rect(&r);
    const int step = font_space_h(&s_font0) - 2;
    char buf[0x40];
    s_font0.colour1 = (uint8_t)palette_find_nearest(s_pal, 0x1e, 0x18, 0x11);
    std::snprintf(buf, sizeof buf, "%s :", text_get(17));
    fe_text(0, y, &s_font0, buf);
    y += step;
    std::snprintf(buf, sizeof buf, "%s", s_snd_name);
    fe_text(r.w - fe_width(&s_font0, buf), y, &s_font0, buf);
    y += step;
    std::snprintf(buf, sizeof buf, "%s :", text_get(18));
    fe_text(0, y, &s_font0, buf);
    y += step;
    std::snprintf(buf, sizeof buf, "%s", s_mus_name);
    fe_text(r.w - fe_width(&s_font0, buf), y, &s_font0, buf);
    y += step;
    // the I/O / IRQ / DMA lines and the music port (texts 23..26, format "%s : %s\n" at 0x931c0) for a card
    // id other than "NONE" (sndsetup.dat; the port's defaults have none)
    if (std::strncmp(s_snd_id, "NONE", 4) != 0) {
        std::snprintf(buf, sizeof buf, "%s : %s\n", text_get(23), s_snd_fields[1]);
        fe_text(0, y, &s_font0, buf);
        y += step;
        std::snprintf(buf, sizeof buf, "%s : %s\n", text_get(24), s_snd_fields[3]);
        fe_text(0, y, &s_font0, buf);
        y += step;
        std::snprintf(buf, sizeof buf, "%s : %s\n", text_get(25), s_snd_fields[5]);
        fe_text(0, y, &s_font0, buf);
        y += step;
    }
    if (std::strncmp(s_mus_id, "NONE", 4) != 0) {
        std::snprintf(buf, sizeof buf, "%s : %s\n", text_get(26), s_snd_fields[7]);
        fe_text(0, y, &s_font0, buf);
    }
}

static void fe_config_draw() {                   // fe_config_draw_52ab0 (step 7: summary, OK button)
    ui_push_clip_rect();
    ui_set_clip_rect(0x38, 0x40, 0x5a, 0x4e);
    fe_config_draw_summary(0);                   // fe_config_option_table_52bf0(7) = null
    if (s_snd_step == 7) fe_draw_sprite(0x22a, 0xe0, spr(s_gcspr, 11));
    ui_pop_clip_rect();
    const int16_t *p = &fe_device_pages[g_fe_input_page * 4];
    fe_draw_sprite(p[0] * 2, p[1] * 2, spr(s_gcspr, (unsigned)p[2]));
}

static void fe_config_apply_input_device(int flags) {   // fe_config_apply_input_device_59330
    g_fe_input_flags = 0;
    // TODO(port): joy_init_digital_5a010 (flags & 2), joy_init_analog_5a060 (flags & 0x21), vfx1_init_4fdc0
    // (flags & 8) - the port has neither joystick nor VFX1 drivers, so nothing is found and the flags stay 0.
    (void)flags;
}

static void fe_screen_config() {                 // fe_screen_config_521c0
    switch (s_cfg_phase) {
    case 0:
        s_cfg_confirm = false; s_cfg_esc = false;
        if (s_flags & 1) break;
        fe_fade_out();                           // fe_config_screen_init_53070
        s_cfg_phase = 1;
        if (blocked()) { s_busy = true; return; }
        [[fallthrough]];
    case 1:
        fe_load_table(s_sfont0, "data/screens/sfont0");     // resource list 0x511d8
        fe_load_table(s_gcspr, "data/screens/gcspr");
        fe_load_file("data/screens/gconfig.dat", s_adf68, 64000);
        fe_load_file("data\\screens\\gconfig.pal", s_pal, sizeof s_pal);
        fe_relocate(s_gcspr);
        fe_relocate_plain(s_sfont0);
        fe_font_init(&s_font0, &s_sfont0);
        s_font0.flags &= 0xfffd;
        fe_cursor_state();
        fe_copy(s_adf68, s_ed74);
        fe_blit();
        fe_fade_in();
        s_cfg_phase = 2;
        if (blocked()) { s_busy = true; return; }
        [[fallthrough]];
    case 2:
        s_flags |= 1;
        s_ev0 &= 0xbe;
        s_cfg_phase = 0;
        s_busy = false;
        break;
    case 3:
        goto exit_mid;
    }
    // ---- one frame of the screen ----
    if (g_key_down[1]) { g_key_down[1] = 0; s_cfg_esc = true; s_cfg_confirm = true; }
    if (((s_ev0 & 1) || (s_ev0 & 0x40)) && cur_in(0x118, 0x12e, 0x79, 0x93) && s_snd_step == 7) {
        g_key_down[1] = 0;
        s_cfg_confirm = true;
    }
    s_opt_hover = 0xff;
    if (cur_in(0x38, 0x92, 0x40, 0x8f)) {
        if (s_snd_step == 7) {
            if (s_ev0 & 1) {
                s_ev0 &= 0xfe;
                // The original restarts the sound-setup wizard here (DAT_0012ed29 = 0); the port skips it.
            }
        } else {
            const int h = font_space_h(&s_font0) - 2;
            if (h) s_opt_hover = (uint8_t)((hx(s_cur_y) - 0x40) / h);
        }
    }
    if (s_ev0 & 1) {
        g_mouse_click_left = 0;
        if (cur_in(0xab, 0x108, 0x2a, 0x93)) {
            g_fe_input_page++;
            if (g_fe_input_page == 7) g_fe_input_page = 0;
            if (g_fe_input_page == 3) g_fe_input_page = 4;
            if (g_fe_input_page == 5) g_fe_input_page = 6;
        }
        // (option area with DAT_0012ed29 <= 6: the wizard steps 0x523a0..0x52889, not ported)
    }
    fe_copy(s_adf68, s_ed74);
    fe_config_draw();
    fe_blit();
    if (!s_cfg_confirm) return;
    fe_config_apply_input_device((uint16_t)fe_device_pages[g_fe_input_page * 4 + 3]);
    fe_fade_out();                               // fe_config_screen_exit_531e0
    s_cfg_phase = 3;
    if (blocked()) { s_busy = true; return; }
exit_mid:
    fe_cursor_hide();
    fe_clear(s_ed74);
    fe_blit();
    fe_free_table(s_sfont0); fe_free_table(s_gcspr);
    s_flags &= 0xfe;
    if (!(s_flags & 0x80)) {
        if (g_fe_hook_sound_initialise) g_fe_hook_sound_initialise();   // sound_initialise_34140
        s_flags |= 0x80;
    }
    if ((s_flags & 2) && (s_flags & 4) && !(s_ev0 & 0x40)) s_state = 2;
    else s_state = s_cfg_esc ? 2 : 7;
    s_ev0 &= 0xbe;
    s_cfg_phase = 0;
    s_busy = false;
}

// =============================================================================================
// FLI screens: 0 intro, 7 Intel logo, 8 title, 9 Bullfrog logo, 10 outro
// =============================================================================================

static int s_seq = 0;          // phase of the running FLI / logo screen

static void clear_mouse_keys() { g_mouse_click_middle = 0; g_mouse_click_right = 0; g_mouse_click_left = 0; g_key_last = 0; }

// fe_screen_intro_movie_54900
static void fe_screen_intro() {
    switch (s_seq) {
    case 0:
        s_busy = true;
        fe_cursor_hide();
        fe_fade_out();
        s_seq = 1;
        if (blocked()) return;
        [[fallthrough]];
    case 1:
        fe_clear(s_ed74);
        fe_blit();
        g_mouse_click_right = 0; g_mouse_click_left = 0; g_key_down[1] = 0; g_key_last = 0;
        fe_fli_play("intro\\intro.dat", (s_flags & 2) != 0, true, (s_flags & 2) && (s_attract_flags & 1), false);
        s_seq = 2;
        if (blocked()) return;
        [[fallthrough]];
    case 2:
        fe_fade_out();
        s_seq = 3;
        if (blocked()) return;
        [[fallthrough]];
    case 3:
        fli_subtitle_free();                     // movie_free_23700
        sound_stop_all();
        music_stop();
        fe_clear(s_ed74);
        fe_blit();
        s_flags |= 6;
        if (!(s_attract_flags & 1)) s_state = 8;
        else { s_state = 2; s_attract_flags &= 0xfe; }
        s_flags &= 0xfe;
        s_seq = 0;
        s_busy = false;
    }
}

// fe_screen_outro_movie_54ab0
static void fe_screen_outro() {
    switch (s_seq) {
    case 0:
        s_busy = true;
        fe_cursor_hide();
        fe_fade_out();
        s_seq = 1;
        if (blocked()) return;
        [[fallthrough]];
    case 1:
        fe_clear(s_ed74);
        fe_blit();
        g_mouse_click_right = 0; g_mouse_click_left = 0; g_key_down[1] = 0; g_key_last = 0;
        fe_fli_play("intro\\outro.dat", false, true, false, false);
        s_seq = 2;
        if (blocked()) return;
        [[fallthrough]];
    case 2:
        fe_fade_out();
        s_seq = 3;
        if (blocked()) return;
        [[fallthrough]];
    case 3:
        sound_stop_all();
        music_stop();
        g_fe_leave = 1;
        local_rec().quit = 1;
        s_blk.kind = BK_DELAY;                   // busy wait of 0x1e0 ticks
        s_blk.until = s_now;
        s_seq = 4;
        return;
    case 4:
        s_seq = 0;
        s_busy = false;
    }
}

// fe_screen_bullfrog_logo_563c0 / fe_screen_intel_logo_56510: FLI, wait, fade.
static void logo_screen(const char *file, int wait_n, int next_state) {
    switch (s_seq) {
    case 0:
        s_busy = true;
        fe_cursor_hide();
        fe_fade_out();
        s_seq = 1;
        if (blocked()) return;
        [[fallthrough]];
    case 1:
        fe_clear(s_ed74);
        fe_blit();
        clear_mouse_keys();
        fe_fli_play(file, true, true, false, false);
        s_seq = 2;
        if (blocked()) return;
        [[fallthrough]];
    case 2:
        fe_wait(wait_n, nullptr);
        s_seq = 3;
        return;                                  // the wait always lasts at least one frame
    case 3:
        fe_fade_out();
        s_seq = 4;
        if (blocked()) return;
        [[fallthrough]];
    case 4:
        sound_stop_all();
        music_stop();
        fe_clear(s_ed74);
        fe_blit();
        s_state = (uint8_t)next_state;
        s_seq = 0;
        s_busy = false;
    }
}
static void fe_screen_bullfrog_logo() { logo_screen("intro\\logo.dat", 8, 0); }
static void fe_screen_intel_logo() {
    // cpu_detect_5ac80 refreshes Config.pentium; the port keeps the start-up value (mc_globals_init: 1)
    if (s_seq == 0 && g_cfg->pentium == 0) { s_state = 9; return; }
    logo_screen("intro\\intel.dat", 1, 9);
}

// fe_flic_loop_frame_56670: title-02.dat frames 1..4 in a loop, 16 ticks apart (the wait callback).
static void fe_flic_loop_frame() {
    const bool input = g_mouse_click_left || g_mouse_click_right || g_mouse_click_middle || g_key_last;
    if (s_title_started && !input && s_title_last + 0x10 >= s_now) return;
    s_title_started = true;
    s_title_last = s_now;
    const long np = flic_play_chunk(s_title2, s_title_pos, s_ed74, 64000);
    s_title_pos = np;
    s_title_n++;
    if (s_title_n == 5) { s_title_n = 1; s_title_pos = s_title_loop; }
    else if (s_title_n == 1 && s_title_loop == 0) s_title_loop = np;
    fe_blit();
}

// fe_screen_title_56730
static void fe_screen_title() {
    switch (s_seq) {
    case 0:
        s_busy = true;
        fe_cursor_hide();
        fe_fade_out();
        s_seq = 1;
        if (blocked()) return;
        [[fallthrough]];
    case 1:
        fe_clear(s_ed74);
        fe_blit();
        if ((uint8_t)g_state->texture_block_size == 0x20) {
            fe_load_blob(s_title2, "intro\\title-02.dat");
            s_title_pos = 0; s_title_loop = 0; s_title_n = 0; s_title_started = false;
        }
        clear_mouse_keys();
        fe_fli_play("intro\\title-01.dat", true, true, (s_attract_flags & 2) != 0, false);
        s_seq = 2;
        if (blocked()) return;
        [[fallthrough]];
    case 2:
        s_seq = 3;
        if (s_fli_aborted == 0) {
            fe_wait(6, (uint8_t)g_state->texture_block_size == 0x20 ? fe_flic_loop_frame : nullptr);
            return;
        }
        [[fallthrough]];
    case 3:
        s_frame = 0;
        fe_fade_out();
        s_seq = 4;
        if (blocked()) return;
        [[fallthrough]];
    case 4:
        sound_stop_all();
        music_stop();
        fe_clear(s_ed74);
        fe_blit();
        fe_free_blob(s_title2);
        s_attract_flags &= 0xfd;
        s_state = 2;
        s_seq = 0;
        s_busy = false;
    }
}

// =============================================================================================
// state 6: language selection
// =============================================================================================

static int s_lang_phase = 0;
static bool s_lang_done = false;

static void lang_finish() {
    text_load(s_game_dir.c_str(), g_cfg->language);     // data/{e,f,g,i}text.dat + text_split_lines_3ed30
    fe_free_table(s_langspr);
    s_state = 1;
    s_flags &= 0xfe;
    s_lang_phase = 0;
    s_busy = false;
}

static void fe_screen_language() {               // fe_screen_language_56940
    char path[1024];
    switch (s_lang_phase) {
    case 0: {
        if (marker_path("language.inf", path, sizeof path)) {
            FILE *f = std::fopen(path, "rb");
            if (f) {
                uint8_t lang = 0;
                if (std::fread(&lang, 1, 1, f) == 1) g_cfg->language = lang;
                std::fclose(f);
                lang_finish();
                return;
            }
        }
        s_busy = true;
        s_frame = 0;
        fe_load_file("data\\screens\\language.pal", s_pal, sizeof s_pal);
        fe_load_file("data\\screens\\language.dat", s_adf68, 64000);
        fe_load_table(s_langspr, "data/screens/langspr");
        fe_relocate(s_langspr);
        fe_copy(s_adf68, s_ed74);
        fe_blit();
        fe_cursor_state();
        fe_fade_in();
        s_lang_phase = 1;
        if (blocked()) return;
    }
        [[fallthrough]];
    case 1:
        s_ev0 &= 0xfe;
        g_key_down[1] = 0;
        s_flags |= 1;
        s_lang_done = false;
        s_lang_phase = 2;
        [[fallthrough]];
    case 2:
        fe_input_poll();
        if (s_ev0 & 1) {
            if (click_in(0x40, 0x8b, 0x2b, 0x52)) g_cfg->language = 0;
            else if (click_in(0x40, 0x8b, 0x5f, 0x86)) g_cfg->language = 1;
            else if (click_in(0xb3, 0xfe, 0x2b, 0x52)) g_cfg->language = 2;
            else if (click_in(0xb3, 0xfe, 0x5f, 0x86)) g_cfg->language = 3;
            else if (click_in(0x119, 0x130, 0x73, 0x8e)) s_lang_done = true;
            s_ev0 &= 0xfe;
        }
        fe_copy(s_adf68, s_ed74);
        {
            const int l = g_cfg->language & 3;
            fe_draw_sprite(fe_language_flags[l * 2] * 2, fe_language_flags[l * 2 + 1] * 2, spr(s_langspr, (unsigned)(g_cfg->language + 1)));
        }
        fe_blit();
        s_frame++;
        if (!s_lang_done) return;
        fe_fade_out();
        s_lang_phase = 3;
        if (blocked()) return;
        [[fallthrough]];
    case 3:
        fe_cursor_hide();
        s_ev0 &= 0xfe;
        g_key_down[1] = 0;
        if (marker_path("language.inf", path, sizeof path)) {
            FILE *f = std::fopen(path, "wb");
            if (f) { std::fwrite(&g_cfg->language, 1, 1, f); std::fclose(f); }
        }
        lang_finish();
    }
}

// =============================================================================================
// state 5: level result
// =============================================================================================

static struct { int phase = 0; bool exit = false; uint32_t t0 = 0; bool snd[6] = {}; } s_lr;

static void fe_screen_level_result() {           // fe_screen_level_result_55b00
    PlayerRec &rec = local_rec();
    switch (s_lr.phase) {
    case 0:
        s_lr.exit = false;
        for (bool &b : s_lr.snd) b = false;
        if (rec.status & 8) goto finish;
        s_busy = true;
        fe_cursor_hide();
        fe_fade_out();
        s_lr.phase = 1;
        if (blocked()) return;
        [[fallthrough]];
    case 1:
        fe_clear(s_ed74);
        fe_blit();
        // ((status & 2) | 4) is never 0: the FLI part always runs
        if (rec.status & 2) {
            g_mouse_click_middle = 0; g_mouse_click_right = 0; g_mouse_click_left = 0; g_key_last = 0;
            fe_fli_play((s_now & 1) ? "intro\\levelw2.dat" : "intro\\levelw1.dat", true, true, false, false);
        } else if (rec.status & 4) {
            g_mouse_click_middle = 0; g_mouse_click_right = 0; g_mouse_click_left = 0; g_key_last = 0;
            fe_fli_play("intro\\levelose.dat", true, true, false, false);
        }
        s_lr.phase = 2;
        if (blocked()) return;
        [[fallthrough]];
    case 2:
        fe_fade_out();
        s_lr.phase = 3;
        if (blocked()) return;
        [[fallthrough]];
    case 3:
        sound_stop_all();
        music_stop();
        fe_clear(s_ed74);
        fe_blit();
        sound_load_bank(s_game_dir.c_str(), 0xd);
        music_load_bank(s_game_dir.c_str(), 0);
        music_play_track(1);
        g_mouse_click_middle = 0; g_mouse_click_right = 0; g_mouse_click_left = 0; g_key_last = 0;
        s_state = 2;
        fe_load_table(s_sfont2, "data/screens/sfont2");    // resource list 0x5151c
        fe_relocate(s_sfont2);
        s_font2.glyphs = s_sfont2.entries.size() > 1 ? &s_sfont2.entries[1] : nullptr;
        s_font2.count = s_sfont2.entries.size() > 1 ? (unsigned)s_sfont2.entries.size() - 1 : 0;
        s_font2.flags = 0x10;                    // drawn through the sprite blitter, no colour remap
        fe_load_file("data/screens/pperf.pal", s_pal, sizeof s_pal);
        fe_load_file("data\\screens\\pperf.dat", s_adf68, 64000);
        fe_copy(s_adf68, s_ed74);
        fe_blit();
        fe_fade_in();
        s_lr.phase = 4;
        if (blocked()) return;
        [[fallthrough]];
    case 4:
        s_ev0 &= 0xfe;
        g_key_down[1] = 0;
        ui_set_clip_rect(0, 0, 0x140, 0xc8);
        s_lr.t0 = s_now;
        s_lr.phase = 5;
        [[fallthrough]];
    case 5: {
        if (g_key_last || g_mouse_click_left || s_now > s_lr.t0 + 0x528) {
            s_ev0 &= 0xb6;
            g_key_last = 0;
            g_mouse_click_left = 0;
            g_key_down[0x39] = 0; g_key_down[0x1c] = 0; g_key_down[1] = 0;
            s_lr.exit = true;
        }
        fe_copy(s_adf68, s_ed74);
        const Thing *t = thing_at(thing_wrap(rec.thing));
        const PlayerBlock *pb = reinterpret_cast<const PlayerBlock *>(thing_player_block(t));
        char buf[64];
        if (s_now > s_lr.t0 + 0x3c) fe_text(0x28, 8, &s_font2, fe_level_name(g_cfg->level));
        struct Line { uint32_t at; int text; int y; int32_t value; };
        const Line lines[5] = {
            {0x3c, 66, 0x50, pb->kills}, {0x78, 67, 0x78, pb->pct_accuracy}, {0xb4, 68, 0xa0, pb->pct_spells},
            {0xf0, 59, 0xc8, pb->pct_mana}, {0x12c, 69, 0xf0, pb->pct_overall},
        };
        for (int i = 0; i < 5; i++) {
            if (!(s_now > s_lr.t0 + lines[i].at)) continue;
            if (!s_lr.snd[i]) { fe_play_sample(3); s_lr.snd[i] = true; }
            fe_text(0x28, lines[i].y, &s_font2, text_get(lines[i].text));
            std::snprintf(buf, sizeof buf, "% 3d %%", (int)lines[i].value);
            fe_text((int16_t)(0x258 - fe_width(&s_font2, buf)), lines[i].y, &s_font2, buf);
        }
        if (s_now > s_lr.t0 + 0x168) {
            if (!s_lr.snd[5]) { fe_play_sample(3); s_lr.snd[5] = true; }
            fe_text(0x28, 0x118, &s_font2, text_get(79));
            const int32_t secs = (int32_t)pb->start_tick / 0x78;
            std::snprintf(buf, sizeof buf, "%dh% 02dm %02ds", (int)(secs / 0xe10), (int)((secs / 0x3c) % 0x3c), (int)(secs % 0x3c));
            fe_text((int16_t)(0x258 - fe_width(&s_font2, buf)), 0x118, &s_font2, buf);
        }
        fe_blit();
        if (!s_lr.exit) return;
        fe_free_table(s_sfont2);
        s_font2.glyphs = nullptr; s_font2.count = 0;
        fe_fade_out();
        s_lr.phase = 6;
        if (blocked()) return;
    }
        [[fallthrough]];
    case 6:
        music_stop();
        sound_stop_all();
    finish:
        s_state = world_campaign_complete(g_cfg->level) ? 10 : 2;   // Config.level == 0x32 (port: or past Hidden Worlds 25)
        s_lr.phase = 0;
        s_busy = false;
    }
}

// =============================================================================================
// state 4: multiplayer lobby (net.h: the join is stepped once per frame, the NetBIOS progress callbacks
// animate the slots)
// =============================================================================================

static int s_mp_phase = 0;
static bool s_mp_started = false;    // the join succeeded: the exit keeps the network player records

static void fe_multiplayer_draw_slots() {        // fe_multiplayer_draw_slots_55210
    if ((s_frame & 7) == 7) { s_lobby_anim++; if (s_lobby_anim == 0x15) s_lobby_anim = 6; }
    if (s_flags & 0x10) { if (g_fe_input_flags == 0) fe_cursor(s_lobby_anim); }
    else fe_cursor_state();
    fe_copy(s_adf68, s_ed74);
    ui_push_clip_rect();
    ui_set_clip_rect(0, 0, 0x140, 0xc8);
    for (int i = 0; i < 8; i++) {
        const int x = fe_lobby_slot_pos[i * 2], y = fe_lobby_slot_pos[i * 2 + 1];
        const uint8_t *r = s_lobby[i];
        if (r[0] == 0 && r[1] == 0) { fe_draw_sprite(x * 2, y * 2, spr(s_pmultspr, (unsigned)(i + 1))); continue; }
        if (r[1]) { fe_draw_sprite((x + 10) * 2, (y - 10) * 2, spr(s_pmultspr, 17)); continue; }
        if (!r[0] || r[2] > 3) continue;
        switch (r[2]) {
        case 0: case 3: fe_draw_sprite(x * 2, y * 2, spr(s_pmultspr, (unsigned)(i + 9))); break;
        case 1: if ((s_frame >> 4) & 1) fe_draw_sprite(x * 2, y * 2, spr(s_pmultspr, (unsigned)(i + 0x11))); break;
        case 2: fe_draw_sprite(x * 2, y * 2, spr(s_pmultspr, (unsigned)(i + 0x11))); break;
        }
    }
    ui_set_clip_rect(0xad, 0x48, 0x5a, 0x3d);
    const uint8_t white = (uint8_t)palette_find_nearest(s_pal, 0x3f, 0x3f, 0x3f);
    const uint8_t other = (uint8_t)palette_find_nearest(s_pal, 0x1a, 0x12, 0xd);
    int y = 0;
    if (fe_mode_menu_shown() && s_lobby_conquest) {
        // port (round 12): the session plays Conquest - the right page shows the setup instead of the level list
        const ModeParams &p = *s_mode_screens->params();
        s_font1.colour1 = white;
        fe_text((0x5a - fe_width(&s_font1, "Conquest")) / 2, y, &s_font1, "Conquest");
        y += font_space_h(&s_font1);
        char line[3][40];
        std::snprintf(line[0], sizeof line[0], "WORLD %u, BOTS %u", (unsigned)p.world_size, (unsigned)p.bots);
        std::snprintf(line[1], sizeof line[1], "SEED %u", (unsigned)p.seed);
        std::snprintf(line[2], sizeof line[2], "CLICK TO SET UP");
        s_font0.colour1 = other;
        for (int i = 0; i < 3; i++) {
            fe_text((0x5a - fe_width(&s_font0, line[i])) / 2, y, &s_font0, line[i]);
            y += font_space_h(&s_font0) + 1;
        }
    } else {
        for (int i = s_lobby_scroll; i < s_lobby_scroll + 5; i++) {
            s_font1.colour1 = (i == s_lobby_sel) ? white : other;
            const char *n = fe_level_name(0x33 + i);
            fe_text((0x5a - fe_width(&s_font1, n)) / 2, y, &s_font1, n);
            y += font_space_h(&s_font1) - 2;
        }
    }
    ui_pop_clip_rect();
    s_font1.colour1 = other;
    if (fe_mode_menu_shown()) {                   // port (round 12): the urn - dim = the original game, lit = Conquest
        const bool hover = urn_hit(hx(s_cur_x), hx(s_cur_y), kUrnLobbyX, kUrnLobbyY);
        urn_draw(s_ed74, kUrnLobbyX, kUrnLobbyY, hover ? 1 : (s_lobby_conquest ? 0 : 2));
    }
}
static void mp_draw_session() {
    ui_push_clip_rect();
    ui_set_clip_rect(0xad, 0x2a, 0x5a, 0xf);
    const char *s = g_cfg->session;
    fe_text((0x5a - fe_width(&s_font1, s)) / 2, 1, &s_font1, s);
    ui_pop_clip_rect();
}
static void fe_multiplayer_refresh() {           // fe_multiplayer_refresh_55870
    s_frame++;
    for (auto &r : s_lobby) if (r[0] == 0) r[2] = 0;
    fe_multiplayer_draw_slots();
    mp_draw_session();
    fe_blit();
}

// The NetBIOS layer's progress callbacks (net_add_name_4e530, net_call_4e600, net_session_join_4f030).
// The original redraws the slots and the session name and blits at once; the port redraws once at the
// end of the frame's join step (fe_screen_multiplayer, phase 5).
static void fe_lobby_set_anim(int p, uint8_t anim) {
    if (p < 0 || p >= 8) return;
    s_lobby[p][2] = anim;
    s_frame++;                                   // DAT_0012ed10
}
static void fe_lobby_slot_connecting(int p) { fe_lobby_set_anim(p, 1); }   // fe_lobby_slot_connecting_55920
static void fe_lobby_slot_connected(int p) { fe_lobby_set_anim(p, 2); }    // fe_multiplayer_slot_joined_559c0
static void fe_lobby_slot_clear(int p) { fe_lobby_set_anim(p, 0); }        // fe_lobby_slot_clear_55a60
// net_check_cancel_4e4b0's input test: Esc, or a left click on the lobby's top-right button (the abort
// button: 640-space 0x238..0x25e x 0x60..0x86 = 320-space 0x11c..0x12f x 0x30..0x43).
static bool fe_lobby_cancel_requested() {
    bool hit = g_key_last == 1;
    if (!hit && g_mouse_click_left != 0) {
        const int x = g_mouse_click_x, y = g_mouse_click_y;
        hit = x >= 0x238 && x <= 0x25e && y >= 0x60 && y <= 0x86;
    }
    if (!hit) { g_mouse_click_left = 0; return false; }
    g_key_last = 0;
    g_mouse_click_left = 0;
    fe_fade_out();                               // vga_palette_fade_61510(NULL, 0x10, 0)
    return true;
}
static void fe_lobby_install_hooks() {
    NetLobbyHooks h;
    h.slot_connecting = fe_lobby_slot_connecting;
    h.slot_connected = fe_lobby_slot_connected;
    h.slot_clear = fe_lobby_slot_clear;
    h.cancel_requested = fe_lobby_cancel_requested;
    h.idle = net_lobby_hooks().idle;             // keep the platform's idle hook
    net_set_lobby_hooks(h);
}

// The end of net_session_join_4f030 in fe_screen_multiplayer_54bd0 (0x54e90..0x54f22). true = the game
// starts (DAT_0009e504 = 1): every player takes the level player 0 chose.
static bool mp_join_done(int r) {
    g_state->local_player = (int16_t)r;
    if (r == -1) {
        if (blocked()) { s_mp_phase = 6; s_busy = true; return false; }   // the cancel's fade first
        fe_multiplayer_refresh();
        fe_fade_in();                            // vga_palette_fade_61510(palette, 0x20, 0)
        s_mp_phase = 4;
        if (blocked()) { s_busy = true; return false; }
        fe_multiplayer_refresh();
        s_mp_phase = 0;
        s_busy = false;
        return false;
    }
    g_cfg->flags |= 0x10;
    g_state->commands[r].arg = (uint8_t)g_cfg->level;
    net_exchange_frame(g_state->commands, 10);   // net_exchange_frame_4f530: the host's choice reaches everybody
    g_fe_leave = 1;
    g_cfg->level = (uint16_t)(int16_t)(int8_t)g_state->commands[0].arg;
    // port (round 8): every peer plays with the host's pool size and gameplay rules (net.h NetGameRules)
    {
        bool differs = false;
        const NetGameRules agreed = net_agree_rules(net_local_rules(), &differs);
        net_apply_rules(agreed);
        if (differs)
            std::fprintf(stderr, "network: playing with the host's settings (thing_slots %u, possession_range_pct %u)\n",
                         agreed.thing_slots, agreed.possession_range_pct);
    }
    s_mp_phase = 0;
    s_busy = false;
    return true;
}

static void fe_screen_multiplayer() {            // fe_screen_multiplayer_54bd0
    bool back = false, started = false;
    switch (s_mp_phase) {
    case 0:
        if (s_flags & 1) break;
        fe_fade_out();                           // fe_multiplayer_init_55630
        s_mp_phase = 1;
        if (blocked()) { s_busy = true; return; }
        [[fallthrough]];
    case 1:
        music_stop();
        fe_lobby_install_hooks();
        s_mp_started = false;
        fe_load_table(s_pmultspr, "data/screens/pmultspr");
        fe_load_table(s_sfont1, "data/screens/sfont1");
        fe_relocate(s_pmultspr);
        fe_relocate_plain(s_sfont1);
        fe_font_init(&s_font1, &s_sfont1);
        s_font1.flags &= 0xfffd;
        if (fe_mode_menu_shown()) {              // port (round 12): the Conquest summary's small font
            fe_load_table(s_sfont0, "data/screens/sfont0");
            fe_relocate_plain(s_sfont0);
        }
        fe_load_file("data\\screens\\pmulti.pal", s_pal, sizeof s_pal);
        fe_load_file("data\\screens\\pmulti.dat", s_adf68, 64000);
        fe_copy(s_adf68, s_ed74);
        fe_blit();
        fe_cursor_state();
        fe_fade_in();
        s_mp_phase = 2;
        if (blocked()) { s_busy = true; return; }
        [[fallthrough]];
    case 2:
        s_flags |= 1;
        s_font1.colour1 = (uint8_t)palette_find_nearest(s_pal, 0x1a, 0x12, 0xd);
        if (s_sfont0.loaded) { fe_font_init(&s_font0, &s_sfont0); s_font0.flags &= 0xfffd; }   // port (round 12)
        fe_multiplayer_refresh();
        s_mp_phase = 0;
        s_busy = false;
        break;
    case 3: goto exit_mid;
    case 4:
        fe_multiplayer_refresh();                // after the failed join: refresh, fade in
        s_mp_phase = 0;
        s_busy = false;
        return;
    case 5: {                                    // net_session_join_4f030, one step per frame
        const int r = net_session_join_step();
        if (r == NET_JOIN_PENDING) {
            fe_multiplayer_draw_slots();
            mp_draw_session();
            fe_blit();
            return;
        }
        if (mp_join_done(r)) { back = true; started = true; goto tail; }
        return;
    }
    case 6:                                      // the cancel's fade to black is over
        fe_multiplayer_refresh();
        fe_fade_in();
        s_mp_phase = 4;
        if (blocked()) return;
        fe_multiplayer_refresh();
        s_mp_phase = 0;
        s_busy = false;
        return;
    }
    if (g_key_down[1]) { g_key_down[1] = 0; back = true; }
    if (fe_mode_menu_shown() && (s_ev0 & 1)) {   // port (round 12): the urn and the Conquest summary
        const bool on_urn = click_in(kUrnLobbyX, kUrnLobbyX + s_urn.w - 1, kUrnLobbyY, kUrnLobbyY + s_urn.h - 1);
        const bool on_summary = s_lobby_conquest && cur_in(0xad, 0x107, 0x48, 0x85);
        if (on_urn && s_lobby_conquest) {
            s_ev0 &= 0xfe;
            fe_lobby_set_conquest(false);
        } else if (on_urn || on_summary) {         // the setup screen, then back to the lobby (its "OK" selects)
            s_ev0 &= 0xfe;
            s_mode_screens->open_setup(4);
            s_mp_to_setup = true;
            back = true;
        }
    }
    if (cur_in(0xad, 0x107, 0x48, 0x85)) {
        const int h = font_space_h(&s_font1) - 2;
        if (h) s_lobby_hover = (int16_t)((hx(s_cur_y) - 0x48) / h);
        if (s_ev0 & 1) s_lobby_sel = (int16_t)(s_lobby_scroll + s_lobby_hover);
    }
    if (s_ev0 & 1) {
        if (cur_in(0xad, 0x106, 0x39, 0x47)) { if (s_lobby_scroll) s_lobby_scroll--; }
        else if (cur_in(0xad, 0x106, 0x86, 0x94)) { if ((uint16_t)s_lobby_scroll + 5 < 0x14) s_lobby_scroll++; }
    }
    if ((s_ev0 & 1) || (s_ev0 & 0x40)) {
        if (click_in(0x11c, 0x12f, 0x30, 0x43)) {
            // a button without an action
        } else if (click_in(0x11c, 0x12f, 0x7e, 0x91)) {      // start
            s_lobby_saved_level = g_cfg->level;
            g_cfg->level = (uint16_t)(s_lobby_sel + 0x32);
            s_flags |= 0x10;
            s_lobby_anim = 6;
            fe_multiplayer_draw_slots();
            mp_draw_session();
            fe_blit();
            g_state->player_count = g_fe_lobby_players;
            s_flags &= 0xef;
            fe_cursor_state();
            s_ev0 &= 0xbe;
            if (fe_mode_menu_shown()) fe_lobby_set_conquest(s_lobby_conquest);   // port (round 12): the proposal
            const int r = net_session_join_begin(g_cfg->session, g_fe_lobby_players);
            if (r == NET_JOIN_PENDING) { s_mp_phase = 5; s_busy = true; return; }
            if (!mp_join_done(r)) return;
            back = true;
            started = true;
        } else if (click_in(0xec, 0x100, 0x9c, 0xb0)) {
            back = true;
        } else if (click_in(0xb0, 0xfd, 0x2e, 0x3a)) {         // session name: next CARPET<n>
            s_ev0 &= 0xfe;
            g_fe_session++;
            if (g_fe_session == 10) g_fe_session = 0;
            std::snprintf(g_cfg->session, sizeof g_cfg->session, "CARPET%d", g_fe_session);
        } else {
            for (int s = 0; s < 8; s++) {                         // player count by clicking a slot
                const UiSprite *sp = spr(s_pmultspr, (unsigned)(s + 1));
                const int x = fe_lobby_slot_pos[s * 2], y = fe_lobby_slot_pos[s * 2 + 1];
                const int cx = hx(s_click_x), cy = hx(s_click_y);
                if (cx < x || cx > x + (sp->w >> 1) || cy < y || cy > y + (sp->h >> 1) || !(s_ev0 & 1)) continue;
                g_fe_lobby_players = 0;
                for (int k = 0; k < 8; k++) {
                    if (k <= s || k < 2) { s_lobby[k][0] = 1; g_fe_lobby_players++; }
                    else s_lobby[k][0] = 0;
                    s_lobby[k][1] = 0;
                }
            }
        }
        s_ev0 &= 0xbe;
    }
tail:
    fe_multiplayer_draw_slots();
    mp_draw_session();
    fe_blit();
    if (!back) return;
    s_mp_started = started;
    fe_fade_out();                               // fe_multiplayer_exit_557c0
    s_mp_phase = 3;
    if (blocked()) { s_busy = true; return; }
exit_mid:
    fe_cursor_hide();
    fe_clear(s_ed74);
    fe_blit();
    fe_free_table(s_pmultspr); fe_free_table(s_sfont1);
    if (s_sfont0.loaded) fe_free_table(s_sfont0);    // port (round 12)
    s_flags &= 0xfe;
    if (s_state == 2) music_play_track(4);
    if (s_mp_to_setup) {
        // port (round 12): to the Conquest setup screen and back - the lobby keeps its player records
        s_mp_to_setup = false;
        s_state = 11;
    } else if (!s_mp_started) {
        // back without a game: restore the player record, the level and the single-player flags
        std::memcpy(&g_state->players[0], reinterpret_cast<uint8_t *>(g_state) + 0x2409, 0x801);
        s_state = 2;
        g_cfg->level = g_cfg->saved_level;
        g_cfg->flags &= 0xffef;
        g_state->local_player = 0;
    }
    // (after a network game started the screen stays 4: the original comes back to the lobby)
    s_mp_phase = 0;
    s_busy = false;
}

// =============================================================================================
// state machine, public API
// =============================================================================================

// fe_sndsetup_read_57af0: c:\carpet.cd\sndsetup.inf present -> sndsetup.dat holds the setup program's
// choice: card id (0x20), card name (0x20), music id (0x20), music name (0x20), then eight ten-byte
// settings. The port reads the pair from the save directory when both exist (the strings only feed the
// config screen's summary; the platform's audio does not use them). Returns true when read.
static bool fe_sndsetup_read() {
    char path[1024];
    if (!marker_path("sndsetup.inf", path, sizeof path)) return false;
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    std::fclose(f);
    if (!marker_path("sndsetup.dat", path, sizeof path)) return false;
    f = std::fopen(path, "rb");
    if (!f) return false;
    char id[0x20], name[0x20], mid[0x20], mname[0x20], fields[8][10];
    const bool ok = std::fread(id, 1, 0x20, f) == 0x20 && std::fread(name, 1, 0x20, f) == 0x20 &&
                    std::fread(mid, 1, 0x20, f) == 0x20 && std::fread(mname, 1, 0x20, f) == 0x20 &&
                    std::fread(fields, 1, sizeof fields, f) == sizeof fields;
    std::fclose(f);
    if (!ok) return false;
    auto copy = [](char *dst, size_t cap, const char *src, size_t n) {
        size_t k = 0;
        while (k < n && k + 1 < cap && src[k]) { dst[k] = src[k]; k++; }
        dst[k] = 0;
    };
    copy(s_snd_id, sizeof s_snd_id, id, 0x20);
    copy(s_snd_name, sizeof s_snd_name, name, 0x20);
    copy(s_mus_id, sizeof s_mus_id, mid, 0x20);
    copy(s_mus_name, sizeof s_mus_name, mname, 0x20);
    for (int i = 0; i < 8; i++) copy(s_snd_fields[i], sizeof s_snd_fields[i], fields[i], 10);
    return true;
}

static void fe_init_state() {                    // fe_init_state_51ed0
    s_snd_step = 7;                              // fe_sndsetup_read_57af0: the port always "has" sndsetup.inf
    s_attract = 0;
    s_lobby_hover = 0; s_lobby_sel = 0; s_lobby_scroll = 0;
    s_item = 0;
    g_fe_session = 0;
    g_fe_lobby_players = 2;
    s_flags = (uint8_t)((s_flags | 2) & 0x42);
    s_frame = 0;
    s_lobby_anim = 6;
    s_attract_flags &= 0xf8;
    s_state = 6;
    g_fe_input_page = 0;
    s_opt_sel = 0;
    s_slot_mode = 0;
    std::snprintf(g_cfg->session, sizeof g_cfg->session, "CARPET%d", 0);
    char path[1024];
    if (marker_path("intro.pld", path, sizeof path)) {
        FILE *f = std::fopen(path, "rb");
        if (f) std::fclose(f);
        else {
            s_flags &= 0xfd;
            f = std::fopen(path, "wb");
            if (f) { const uint32_t v = 0; std::fwrite(&v, 1, 4, f); std::fclose(f); }
        }
    } else {
        s_flags &= 0xfd;                         // no marker directory: the intro is "never seen"
    }
    g_cfg->language = 0;
    // sound summary of the config screen (sndsetup.inf strings), from what the platform has
    const uint32_t *cards = fe_sound_card_ptrs;
    auto card_text = [](uint32_t addr) -> const char * {
        if (addr < 0x92ed0 || addr >= 0x92ed0 + sizeof fe_sound_card_text) return "";
        return reinterpret_cast<const char *>(&fe_sound_card_text[addr - 0x92ed0]);
    };
    std::snprintf(s_snd_name, sizeof s_snd_name, "%s", card_text(g_sound_available ? cards[3 * 2] : cards[0]));       // "Soundblaster 16" / "No sound"
    std::snprintf(s_mus_name, sizeof s_mus_name, "%s", card_text(g_music_available ? cards[21 * 2] : cards[12 * 2])); // "General midi" / "No music"

    std::snprintf(s_snd_id, sizeof s_snd_id, "NONE");
    std::snprintf(s_mus_id, sizeof s_mus_id, "NONE");
    for (auto &f : s_snd_fields) std::snprintf(f, sizeof f, "0");
    fe_sndsetup_read();
}

void fe_set_save_dir(const char *save_dir, const char *dos_game_dir) {
    s_save_dir = save_dir ? save_dir : "";
    savegame_set_dirs(s_save_dir.c_str(), dos_game_dir);
}

const char *fe_level_name(int index) {
    if (world_is_hidden_level(index)) {                 // port: Hidden Worlds campaign (world_set.h)
        const char *n = world_hidden_level_name(index);
        return n ? n : "";
    }
    if (index < 0 || index >= 71) return "";
    const uint32_t a = fe_level_name_ptrs[index];
    if (a < 0x90b08 || a >= 0x90b08 + sizeof fe_level_name_text) return "";
    return reinterpret_cast<const char *>(&fe_level_name_text[a - 0x90b08]);
}

const uint8_t *fe_screen_pixels() { return s_vga; }

// ---- the in-game mouse pointer (mouse_cursor_set_sprite_5ba5c with the data/pointers table) ----------
static int s_game_pointer = -1;
void mouse_cursor_set_pointer(int entry) { s_game_pointer = entry; }
int  mouse_cursor_pointer() { return s_game_pointer; }
void mouse_cursor_draw(const FrameBuffer &fb) {
    if (s_game_pointer < 0 || !fb.pixels) return;
    const UiSprite *s = ui_sprite(g_ui_pointers, (unsigned)s_game_pointer);
    if (!s || !s->data || s->h == 0) return;
    ui_set_target(fb);
    ui_draw_sprite(g_mouse_x, g_mouse_y, s);   // hot spot top-left, as mouse_cursor_draw_5b35c
}
void mouse_cursor_draw_entry(const FrameBuffer &fb, int entry, int x, int y) {
    if (entry < 0 || !fb.pixels) return;
    const UiSprite *s = ui_sprite(g_ui_pointers, (unsigned)entry);
    if (!s || !s->data || s->h == 0) return;
    ui_set_target(fb);
    ui_draw_sprite(x, y, s);
}
const uint8_t *fe_palette6() { return s_pal; }

static void fe_load_pointers() {                 // resource list 0x510d0: sptrs.dat / .tab (+ palette, two backups)
    fe_load_table(s_sptrs, "data/screens/sptrs");
    fe_relocate(s_sptrs);
}

bool fe_init(const char *game_dir, int first_state) {
    s_game_dir = game_dir ? game_dir : "";
    for (int i = 0; i < SAVE_SLOTS; i++) {
        std::memcpy(s_slot_names[i], &fe_slot_name_defaults[i * 21], 21);
        s_slot_names[i][SAVE_NAME_LEN] = 0;
    }
    std::memcpy(s_lobby, fe_lobby_slots_init, sizeof s_lobby);
    fe_init_state();
    s_state = (uint8_t)first_state;
    s_busy = false; s_left = false;
    s_blk.kind = BK_NONE;
    s_mm = MM_IDLE; s_cfg_phase = 0; s_seq = 0; s_lang_phase = 0; s_lr.phase = 0; s_mp_phase = 0;
    s_ed74 = s_buf_a; s_adf68 = s_buf_b;
    s_leave_mode = false; s_mp_to_setup = false;
    fe_clear(s_vga);
    g_fe_reload = 1;
    g_fe_leave = 0;
    fe_set_window(0, 0, 0x140, 0xc8);
    fe_load_pointers();
    g_fe_reload = 0;
    return s_sptrs.loaded;
}

void fe_enter(int state) { s_state = (uint8_t)state; }
int fe_state() { return s_state; }

static uint8_t s_busy_screen = 0;   // the screen whose blocking sequence is running (its state may already be the next one)
static void dispatch() {                         // the jump table at 0x52038
    const uint8_t st = s_busy ? s_busy_screen : s_state;
    s_busy_screen = st;
    switch (st) {
    case 0: fe_screen_intro(); break;
    case 1: fe_screen_config(); break;
    case 2: fe_screen_main_menu(); break;
    case 4: fe_screen_multiplayer(); break;
    case 5: fe_screen_level_result(); break;
    case 6: fe_screen_language(); break;
    case 7: fe_screen_intel_logo(); break;
    case 8: fe_screen_title(); break;
    case 9: fe_screen_bullfrog_logo(); break;
    case 10: fe_screen_outro(); break;
    case 11: case 12:                            // port (round 12): the Conquest setup / result screens (fe_mode.cpp)
        if (s_mode_screens) s_mode_screens->frame(st);
        else s_state = 2;
        break;
    default: break;
    }
}

static void fe_present(const FrameBuffer &fb) {
    std::memcpy(s_present, s_vga, 64000);
    if (s_cursor >= 0) {
        const int wx = s_win_x, wy = s_win_y, ww = s_win_w, wh = s_win_h;
        fe_set_window(0, 0, 0x140, 0xc8);
        fe_draw_sprite_to(s_present, g_mouse_x, g_mouse_y, spr(s_sptrs, (unsigned)s_cursor));
        s_win_x = wx; s_win_y = wy; s_win_w = ww; s_win_h = wh;
    }
    if (!fb.pixels) return;
    if (fb.width >= 640 && fb.height >= 400) {
        for (int y = 0; y < 400; y++)
            for (int x = 0; x < 640; x++) fb.pixels[y * fb.width + x] = s_present[(y >> 1) * 320 + (x >> 1)];
    } else {
        const int w = fb.width < 320 ? fb.width : 320, h = fb.height < 200 ? fb.height : 200;
        for (int y = 0; y < h; y++) std::memcpy(fb.pixels + y * fb.width, s_present + y * 320, (size_t)w);
    }
}

FeResult fe_frame(const FrameBuffer &fb, uint32_t now_ticks, int *level_out) {
    s_now = now_ticks;
    const uint16_t keep_mode = g_video_mode_flags;
    if (!(g_video_mode_flags & 1)) g_video_mode_flags = 1;      // the front end always runs in 320x200
    if (s_left) { s_left = false; g_fe_leave = 0; }                 // game_main: DAT_0009e504 = 0, DAT_0012ebdc = 1
    FeResult result = FE_CONTINUE;
    bool run = true;
    if (blocked()) {
        blocker_step();
        run = !blocked();
    } else if (!s_busy) {
        if (g_fe_reload) {
            fe_load_pointers();
            std::memset(s_pal, 0, sizeof s_pal);   // *PALETTE is re-allocated with the resource list 0x510d0
            g_fe_reload = 0;
        }
        fe_input_poll();
    }
    if (run) {
        dispatch();
        if (!s_busy && !blocked()) {
            s_frame++;
            fe_music_update();
            if (g_fe_leave) {
                fe_clear(s_ed74);
                g_fe_reload = 1;
                music_stop();
                if (local_rec().quit) result = FE_QUIT;
                else if (s_leave_mode) result = FE_START_MODE;     // port (round 12): fe_mode_params()
                else if (g_cfg->flags & 4) { result = FE_START_DEMO; if (level_out) *level_out = g_cfg->movie; }
                else { result = FE_START_LEVEL; if (level_out) *level_out = g_cfg->level; }
                s_left = true;
                s_leave_mode = false;
                fe_cursor(-1);
            }
        }
    }
    fe_present(fb);
    g_video_mode_flags = keep_mode;
    return result;
}

// ---- port (round 12): the toolkit of fe_mode.cpp (frontend.h) -----------------------------------------------
uint8_t *fe_tk_back() { return s_ed74; }
uint8_t *fe_tk_background() { return s_adf68; }
uint8_t *fe_tk_palette() { return s_pal; }
const uint8_t *fe_tk_bright() { return s_bright; }
void fe_tk_present() { fe_blit(); }
bool fe_tk_load_screen(const char *dat_rel, const char *pal_rel) {
    char dat[256], pal[256];
    std::snprintf(dat, sizeof dat, "data/screens/%s", dat_rel);
    std::snprintf(pal, sizeof pal, "data/screens/%s", pal_rel);
    const bool ok = fe_load_file(dat, s_adf68, 64000) && fe_load_file(pal, s_pal, sizeof s_pal);
    fe_build_bright_table();
    return ok;
}
void fe_tk_fade_out() { fe_fade_out(); }
void fe_tk_fade_in() { fe_fade_in(); }
bool fe_tk_blocked() { return blocked(); }
void fe_tk_set_busy(bool busy) { s_busy = busy; }
bool fe_tk_load_fonts() {
    const bool ok = fe_load_table(s_sfont0, "data/screens/sfont0") && fe_load_table(s_sfont1, "data/screens/sfont1") &&
                    fe_load_table(s_sfont2, "data/screens/sfont2");
    fe_relocate_plain(s_sfont0);
    fe_font_init(&s_font0, &s_sfont0);
    s_font0.flags &= 0xfffd;
    fe_relocate_plain(s_sfont1);
    fe_font_init(&s_font1, &s_sfont1);
    s_font1.flags &= 0xfffd;
    fe_relocate(s_sfont2);                       // as the level result screen: drawn through the sprite blitter
    s_font2.glyphs = s_sfont2.entries.size() > 1 ? &s_sfont2.entries[1] : nullptr;
    s_font2.count = s_sfont2.entries.size() > 1 ? (unsigned)s_sfont2.entries.size() - 1 : 0;
    s_font2.flags = 0x10;
    return ok;
}
void fe_tk_free_fonts() {
    fe_free_table(s_sfont0); fe_free_table(s_sfont1); fe_free_table(s_sfont2);
    s_font0.glyphs = s_font1.glyphs = s_font2.glyphs = nullptr;
    s_font0.count = s_font1.count = s_font2.count = 0;
}
FontDesc *fe_tk_font(int n) { return n == 0 ? &s_font0 : n == 1 ? &s_font1 : &s_font2; }
void fe_tk_text(int x, int y, FontDesc *font, const char *s) { fe_text(x, y, font, s); }
int fe_tk_text_width(const FontDesc *font, const char *s) { return fe_width(font, s); }
void fe_tk_pointer(int *x, int *y) { *x = hx(s_cur_x); *y = hx(s_cur_y); }
uint8_t fe_tk_events() { return s_ev0; }
void fe_tk_clear_events(uint8_t mask) { s_ev0 &= (uint8_t)~mask; }
void fe_tk_cursor(int entry) { if (g_fe_input_flags == 0) fe_cursor(entry); }
// the next screen initialises itself (the Conquest screens never keep DAT_0012ed35 bit 0 set)
void fe_tk_set_state(int state) { s_state = (uint8_t)state; s_flags &= 0xfe; }
void fe_tk_start_mode() { s_leave_mode = true; g_fe_leave = 1; }
uint32_t fe_tk_now() { return s_now; }
uint32_t fe_tk_frame() { return s_frame; }
const char *fe_tk_game_dir() { return s_game_dir.c_str(); }

void fe_shutdown() {
    if (s_blk.fli) { fli_close(s_blk.fli); s_blk.fli = nullptr; }
    s_blk.kind = BK_NONE;
    UiSpriteTable *t[] = {&s_sptrs, &s_gcspr, &s_mmspr, &s_pmultspr, &s_langspr, &s_sfont0, &s_sfont1, &s_sfont2};
    for (UiSpriteTable *x : t) fe_free_table(*x);
    fe_free_blob(s_globe); fe_free_blob(s_timer); fe_free_blob(s_title2);
}
