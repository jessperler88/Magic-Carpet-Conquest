// Frame composition and HUD of carpet.exe: render_frame_1fab0 and the HUD functions it calls.
// See hud.h. Translated from the disassembly (argument orders from the pushes before every call).
//
// Original addresses: render_frame_1fab0, render_set_view_window_top_2f320, ui_draw_radar_43610,
// ui_draw_radar_blips_42a20, ui_draw_map_43910, ui_draw_status_bars_219f0, ui_fill_bar_212f0,
// ui_draw_player_list_21370, ui_draw_thing_label_22870, ui_draw_spell_panel_icon_22d80,
// ui_draw_spell_icon_22820, ui_draw_debug_overlay_4ad80.
#define _CRT_SECURE_NO_WARNINGS
#include "hud.h"
#include "settings.h"
#include "debug_camera.h"
#include <algorithm>
#include "ui_draw.h"
#include "mc_globals.h"
#include "mc_math.h"
#include "raster.h"
#include "player.h"
#include "input.h"
#include "text.h"
#include "gen/hud_tables.h"
#include <cstdio>
#include <cstring>

const char *g_hud_screenshot_dir = nullptr;
uint8_t g_hud_joystick_present = 0;     // DAT_0012ed40

// DAT_00093900 (last full-screen clear: 0 flight / help, 2 map) and DAT_00093902 (view size at the
// last clear); start values from the data segment: 0x63, 0x28.
static uint8_t s_last_clear = 0x63;
static uint8_t s_last_view_size = 0x28;
static int s_local = 0;
// Pass flags of the current render_frame pass (see hud.h): s_writes = perform the game-state writes
// of render_frame_1fab0, s_drawing = a frame buffer is attached (pixels are produced).
static bool s_writes = true;
static bool s_drawing = true;

// DAT_000b7480: radar row extents {right (exclusive), left} per screen row, written by the radar and
// read by the blips.
static int16_t s_radar_rows[512][2];

static inline int32_t mul32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }
static inline bool lo1() { return g_video_mode_flags == 1; }          // `cmp word [0x12edae], 1`
static inline uint8_t tick_bit(int k) { return reinterpret_cast<uint8_t *>(g_cfg)[0x5d + k]; }   // Config+0x5d+k
static inline uint8_t blink_white() { return g_colour_cube[tick_bit(2) ? 0xfff : 0]; }           // cube[(bit*0xff0>>8) * 0x111]
static inline uint8_t blink_red() { return g_colour_cube[tick_bit(2) ? 0xf00 : 0]; }             // cube[(bit*0xff0>>8) << 8]

static Thing *thing_or_null(int idx) {         // `cmp eax, &things[0]; jbe` (pointer above slot 0)
    return (idx > 0 && idx < thing_pool_slots()) ? thing_at((unsigned)idx) : nullptr;
}
static PlayerBlock *pblock(const Thing *t) { return reinterpret_cast<PlayerBlock *>(thing_player_block(t)); }
// P.spell_slot[slot] with the original's unchecked index (slot 0xff reads the next player record):
// read as an int32 from the GameState when it lies inside, else 0.
static int32_t spell_slot_raw(const PlayerBlock *P, int slot) {
    if (reinterpret_cast<const uint8_t *>(P) == g_dummy_player_block) {
        return (slot >= 0 && slot < 24) ? P->spell_slot[slot] : 0;
    }
    const intptr_t off = reinterpret_cast<const uint8_t *>(P) - reinterpret_cast<const uint8_t *>(g_state)
                         + (intptr_t)offsetof(PlayerBlock, spell_slot) + (intptr_t)slot * 4;
    if (off < 0 || off + 4 > (intptr_t)MC_STATE_IMAGE_SIZE) return 0;
    int32_t v;
    state_image_read(*g_state, off, &v, 4);         // round 11: the original's layout (mc_image.h)
    return v;
}

// ---------------------------------------------------------------------------------------------
// render_set_view_window_top_2f320(size)
// ---------------------------------------------------------------------------------------------
void render_set_view_window_top(const FrameBuffer &fb, int size) {
    int off, w, h;
    if (ui_lo_res()) { off = fb.width - size * 8; w = (size * 8) & 0xffff; h = (size * 5) & 0xffff; }
    else             { off = fb.width - size * 16; w = (size * 16) & 0xffff; h = (size * 0xc) & 0xffff; }
    render_set_viewport(fb.pixels + off, nullptr, fb.width, w, h);
}

// ---------------------------------------------------------------------------------------------
// ui_fill_bar_212f0(x, y, max_w, h, value, colour)
// ---------------------------------------------------------------------------------------------
void ui_fill_bar(int x, int y, int max_w, int h, int value, int colour) {
    int w = value;
    if (w >= max_w) w = max_w;
    if (w < 2) return;
    gfx_fill_rect((int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h, (uint8_t)colour);
}
static int ratio64(int32_t num, int32_t den) {       // num * 64 / den (idiv), port guard for den == 0
    if (den == 0) return 0;
    return (int)(mul32(num, 64) / den);
}

// ---------------------------------------------------------------------------------------------
// ui_draw_radar_43610
// ---------------------------------------------------------------------------------------------
void ui_draw_radar(int x, int y, int cam_x, int cam_y, int w, int h, int yaw, int scale, int unused, int rect) {
    (void)unused;
    if (!g_ui_fb.pixels) return;
    if (ui_lo_res()) { x >>= 1; y >>= 1; w >>= 1; h >>= 1; scale *= 2; }
    if (w <= 0 || h <= 0 || h > 512) return;          // port guard (division by w, row table size)
    const int pitch = g_ui_fb.width;
    if (rect) {
        for (int r = 0; r < h; r++) { s_radar_rows[r][0] = (int16_t)w; s_radar_rows[r][1] = 0; }
    } else {
        const int half = h / 2, r = w >> 1, hh = h >> 1;
        if (hh == 0) return;
        int32_t acc = 0;
        const int32_t step = 0x1000000 / hh;
        const uint8_t *circle = g_circle_profile();
        for (int i = 0; i < half; i++) {
            const int d = (circle[(acc >> 16) & 0xff] * r) >> 8;
            s_radar_rows[half + i][0] = s_radar_rows[half - 1 - i][0] = (int16_t)(r + d);
            s_radar_rows[half + i][1] = s_radar_rows[half - 1 - i][1] = (int16_t)(r - d);
            acc += step;
        }
    }
    yaw &= 0x7ff;
    const int32_t s = mul32(mc_sin((unsigned)yaw), scale) >> 16;
    const int32_t c = mul32(mc_cos((unsigned)yaw), scale) >> 16;
    const int32_t B = mul32(s, h) / w;           // [ebp-0x30]: v step per pixel
    const int32_t A = mul32(c, h) / w;           // [ebp-0x34]: u step per pixel
    int32_t u0 = cam_x - (mul32(A, w) - mul32(s, h)) / 2;
    int32_t v0 = cam_y - (mul32(B, w) + mul32(c, h)) / 2;
    const uint8_t *shade = g_shade_table(), *blend = g_blend_table(), *avg = g_tex_avg_colour();
    for (int r = 0; r < h; r++) {
        const int left = s_radar_rows[r][1], right = s_radar_rows[r][0];
        int32_t u = mul32(A, left) + u0, v = mul32(B, left) + v0;
        const int py = y + r;
        uint8_t *row = (py >= 0 && py < g_ui_fb.height) ? g_ui_fb.pixels + py * pitch : nullptr;
        for (int i = 0, n = right - left; i < n; i++) {        // the exe loops 2^32 times for n == 0
            const uint16_t cell = (uint16_t)((((uint32_t)v >> 8) & 0xff) << 8 | (((uint32_t)u >> 8) & 0xff));
            const uint8_t col = shade[(g_map_light[cell] << 8) | avg[g_map_type[cell]]];
            const int px = x + left + i;
            if (row && px >= 0 && px < g_ui_fb.width)
                row[px] = rect ? col : blend[(row[px] << 8) | col];
            u += A; v += B;
        }
        u0 -= s; v0 += c;
    }
}

// ---------------------------------------------------------------------------------------------
// ui_draw_radar_blips_42a20
// ---------------------------------------------------------------------------------------------
static bool blip_inside(int px, int py, int w, int h) {
    return px >= 0 && px < w && py >= 0 && py < h && py < 512 && px >= s_radar_rows[py][1] && px < s_radar_rows[py][0];
}

void ui_draw_radar_blips(int x, int y, int cam_x, int cam_y, int w, int h, int yaw, int scale, int unused, int rect) {
    (void)unused; (void)rect;
    if (!g_ui_fb.pixels) return;
    if (ui_lo_res()) { x >>= 1; y >>= 1; scale *= 2; w >>= 1; h >>= 1; }
    if (scale == 0) return;
    const int pitch = g_ui_fb.width;
    auto px_ok = [&](int px, int py) { const int ax = x + px, ay = y + py; return ax >= 0 && ay >= 0 && ax < g_ui_fb.width && ay < g_ui_fb.height; };
    auto at = [&](int px, int py) -> uint8_t & { return g_ui_fb.pixels[(y + py) * pitch + x + px]; };
    const int32_t inv = 0x10000 / scale;
    const int cx = w / 2, cy = h / 2;
    yaw &= 0x7ff;
    const int32_t C = mul32(mc_cos((unsigned)yaw), inv) >> 16;
    const int32_t S = (-mul32(mc_sin((unsigned)yaw), inv)) >> 16;
    ui_set_font(1);
    // port: a circular radar narrower than tall in cells (radar_round, w != h) steps h / w cells per column
    // (ui_draw_radar); the original always passes w == h here
    const bool squash = !rect && w != h;
    auto proj_x = [&](int dx, int dy) {
        const int ox = (mul32(C, dx) - mul32(dy, S)) >> 16;
        return (squash ? ox * w / h : ox) + cx;
    };
    auto proj_y = [&](int dx, int dy) { return ((mul32(dx, S) + mul32(C, dy)) >> 16) + cy; };

    PlayerRec &rec = g_state->players[s_local];
    Thing *T = thing_at(thing_wrap(rec.thing));
    PlayerBlock *P = pblock(T);
    int reveal = 0;                                     // spell 5 being cast: other wizards shown by name
    if (P->spell_thing[5] != 0) reveal = thing_at(thing_wrap((unsigned)(int16_t)P->spell_thing[5]))->cast_ticks;
    if (P->castle != 0) {                               // dotted line from the wizard to the castle
        const Thing *castle = thing_at(thing_wrap(P->castle));
        const int dx0 = (int16_t)(T->x - cam_x), dy0 = (int16_t)(T->y - cam_y);
        const int px0 = proj_x(dx0, dy0), py0 = proj_y(dx0, dy0);
        const int dx1 = (int16_t)(castle->x - cam_x), dy1 = (int16_t)(castle->y - cam_y);
        const int px1 = proj_x(dx1, dy1), py1 = proj_y(dx1, dy1);
        const int dist = (int)mc_isqrt((uint32_t)(mul32(px1 - px0, px1 - px0) + mul32(py1 - py0, py1 - py0)));
        const int ang = math_atan2((int16_t)(px1 - px0), (int16_t)(py1 - py0)) & 0xffff;
        for (int t = (int)(rec.tick & 3) + 4; t <= dist; t += 4) {
            const int bx = (mul32(mc_trig_raw((unsigned)ang & 0x7ff), t) >> 16) + px0;
            const int by = (mul32(mc_trig_raw(((unsigned)ang & 0x7ff) + 0x200), -t) >> 16) + py0;
            if (!blip_inside(bx, by, w, h)) break;
            if (px_ok(bx, by)) at(bx, by) = g_blend_table()[(at(bx, by) << 8) | ui_col_white()];
        }
    }
    uint8_t colour = 0;                                 // [esp+0x44]: keeps its value between things
    for (int i = 1; i < thing_pool_slots(); i++) {
        const Thing *t = thing_at((unsigned)i);
        unsigned sym = 0;
        int size = 1;
        switch (t->cls) {
        case 2:                                         // scenery
            if (t->type == 0) { colour = 0x1c; sym = t->state == 2 ? 0 : 1; }
            else if (t->type == 1 || t->type == 3) { if (tick_bit(2)) { colour = ui_col_black(); sym = 1; } }
            else { colour = 0x1c; sym = 1; }
            break;
        case 3:                                         // castle / balloon icons
            if (t->type == 2) sym = (uint8_t)(pblock(thing_at(thing_wrap((unsigned)(int16_t)t->owner)))->player_no + 0x3a);
            else if (t->type == 3 && (t->owner == T->owner || reveal != 0))
                sym = (uint8_t)(pblock(thing_at(thing_wrap((unsigned)(int16_t)t->owner)))->player_no + 0x42);
            break;
        case 5:                                         // creatures
            if (t->state == 0x78 || (t->flags & 1)) break;
            if ((int16_t)t->owner == i) colour = (t->type >= 0xc && t->type <= 0xe) ? ui_col_blue() : ui_col_black();
            // owned creature: the owner's B colour (0x42fa7: `mov al, [eax*2 + 0x97631]`)
            else colour = g_hud_player_colours[(pblock(thing_at(thing_wrap((unsigned)(int16_t)t->owner)))->player_no * 2 + 1) & 15];
            sym = 1;
            break;
        case 10:                                        // effects
            if (t->type == 0x12) break;
            if (t->type == 0x22) size = 2;
            if (t->type == 0x27) {                      // mana ball
                const Thing *mo = t->mana_owner ? thing_at(thing_wrap(t->mana_owner)) : nullptr;
                if (mo && mo->cls == 3) colour = g_hud_player_colours[(pblock(mo)->player_no * 2 + tick_bit(3)) & 15];
                else colour = 0xe8;
                sym = 1;
                break;
            }
            [[fallthrough]];
        case 9: {                                       // projectiles (and the other effects)
            const Thing *o = thing_at(thing_wrap((unsigned)(int16_t)t->owner));
            colour = o->cls == 3 ? g_hud_player_colours[(pblock(o)->player_no * 2) & 15] : ui_col_magenta();
            sym = 1;
            break;
        }
        case 11:                                        // switches
            if (t->type >= 9 && t->type <= 0xc) sym = 0x53;
            else if (t->type == 0x1f) sym = 0x54;
            break;
        case 12:                                        // spells lying around
            if (!(t->flags & 1)) { colour = ui_col_red(); sym = 1; }
            break;
        default: break;
        }
        if (!sym) continue;
        const int dx = (int16_t)(t->x - cam_x), dy = (int16_t)(t->y - cam_y);
        int px = proj_x(dx, dy), py = proj_y(dx, dy);
        if (!blip_inside(px, py, w, h)) continue;
        if (sym <= 1) {
            if (px_ok(px, py)) at(px, py) = colour;
            if (size > 1) {
                if (px_ok(px + 1, py)) at(px + 1, py) = colour;
                if (px_ok(px, py + 1)) at(px, py + 1) = colour;
                if (px_ok(px + 1, py + 1)) at(px + 1, py + 1) = colour;
            }
            continue;
        }
        const UiSprite *e = hud_sprite(sym);
        // sprite positions in 640-space: relative to the radar's own (unhalved) origin; the exe
        // adds nothing for x / y = 0, which is how it is called
        if (lo1()) {
            px *= 2; py *= 2;
            if (sym >= 0x42 && sym < 0x4a) ui_draw_sprite(px - e->w / 2, py - e->h, e);
            else if (sym >= 0x3a && sym < 0x42) ui_draw_sprite(px, py - e->h, e);
            else if (sym == 0x53 || sym == 0x54) ui_draw_sprite(px - e->w / 2, py - e->h / 2, e);
        } else {
            if (sym < 0x3c) ui_draw_sprite(px, py - e->h, e);
            else ui_draw_sprite(px - e->w / 2, py - e->h, e);
        }
    }
    if (reveal != 0) {
        for (int q = 0; q < (uint16_t)g_state->player_count && q < 8; q++) {
            if (q == g_state->local_player) continue;
            const Thing *t2 = thing_at(thing_wrap(g_state->players[q].thing));
            if (t2->health < 0) continue;
            const int dx = (int16_t)(t2->x - cam_x), dy = (int16_t)(t2->y - cam_y);
            const int px = proj_x(dx, dy), py = proj_y(dx, dy);
            if (!blip_inside(px, py, w, h)) continue;
            const uint8_t col = g_hud_player_colours[(pblock(t2)->player_no * 2 + 1) & 15];
            if (lo1()) ui_draw_text(g_state->players[q].name, px * 2 + 2, py * 2, col);
            else ui_draw_text(g_state->players[q].name, px + 2, py, col);
        }
    }
    // centre cross, darkening outward (shade levels 0x2c .. )
    const int n = w / 12;
    if (n <= 0) return;
    const int step = 0x800 / n;
    int sv = 0x2c00;
    const uint8_t *shade = g_shade_table();
    const int ccx = w / 2 - 1, ccy = h / 2;
    auto shade_px = [&](int px, int py, int lvl) { if (px_ok(px, py)) at(px, py) = shade[(lvl & 0xff00) + at(px, py)]; };
    shade_px(ccx, ccy, sv);
    for (int k = 1; k <= n; k++) {
        sv -= step;
        shade_px(ccx, ccy - k, sv);
        shade_px(ccx + k, ccy, sv);
        shade_px(ccx, ccy + k, sv);
        shade_px(ccx - k, ccy, sv);
    }
}

// ---------------------------------------------------------------------------------------------
// ui_draw_map_43910(x, y, w, h, cam_x, cam_y): no caller in the exe. x / y in 320 units.
// ---------------------------------------------------------------------------------------------
void ui_draw_map(int x, int y, int w, int h, int cam_x, int cam_y) {
    if (!g_ui_fb.pixels || w <= 0 || h <= 0) return;
    const uint8_t *shade = g_shade_table(), *blend = g_blend_table(), *avg = g_tex_avg_colour();
    const int pitch = g_ui_fb.width;
    uint8_t cx0 = (uint8_t)((cam_x >> 8) - w / 2), cy = (uint8_t)((cam_y >> 8) - h / 2);
    auto col = [&](uint8_t cxx, uint8_t cyy) { const uint16_t cell = mc_cell(cxx, cyy); return shade[(g_map_light[cell] << 8) | avg[g_map_type[cell]]]; };
    auto put = [&](int px, int py, uint8_t v) { if (px >= 0 && py >= 0 && px < g_ui_fb.width && py < g_ui_fb.height) g_ui_fb.pixels[py * pitch + px] = v; };
    auto get = [&](int px, int py) -> uint8_t { return (px >= 0 && py >= 0 && px < g_ui_fb.width && py < g_ui_fb.height) ? g_ui_fb.pixels[py * pitch + px] : 0; };
    if (ui_lo_res()) {
        for (int r = 0; r < h; r++, cy++)
            for (int k = 0; k < w; k++) put(x + k, y + r, col((uint8_t)(cx0 + k), cy));
        return;
    }
    const int X = x * 2, Y = y * 2;
    for (int r = 0; r < h; r++, cy++) {                 // even screen lines: 2 pixels per cell
        uint8_t prev = 0;
        for (int k = 0; k < w; k++) {
            const uint8_t c = col((uint8_t)(cx0 + k), cy);
            put(X + 2 * k + 1, Y + 2 * r, c);
            put(X + 2 * k, Y + 2 * r, (r & 1) ? blend[(prev << 8) | c] : blend[(c << 8) | prev]);
            prev = c;
        }
    }
    for (int r = 0; r < h - 1; r++)                     // odd lines: blend of the lines around
        for (int k = 0; k < 2 * w; k += 2) {
            const uint8_t a0 = get(X + k, Y + 2 * r), b0 = get(X + k, Y + 2 * r + 2);
            const uint8_t a1 = get(X + k + 1, Y + 2 * r), b1 = get(X + k + 1, Y + 2 * r + 2);
            put(X + k, Y + 2 * r + 1, blend[(b0 << 8) | a0]);
            put(X + k + 1, Y + 2 * r + 1, blend[(a1 << 8) | b1]);
        }
}

// ---------------------------------------------------------------------------------------------
// ui_draw_spell_icon_22820(x, y, spell id): a spell the player does not own
// ---------------------------------------------------------------------------------------------
void ui_draw_spell_icon(int x, int y, int spell_id) {
    ui_draw_sprite_blend((int16_t)x, (int16_t)y, hud_sprite(3));
    ui_draw_sprite_tint((int16_t)x, (int16_t)y, hud_sprite((unsigned)(spell_id + 6)), 0xa6);
}

// The tail shared by 22870 and 22d80: quick-select key highlight and the "not enough mana" dimming.
static void spell_icon_marks(int x, int y, Thing *spell, const Thing *caster, const UiSprite *dim) {
    PlayerBlock *P = pblock(caster);
    const int type = (int8_t)spell->type;
    if (type >= 0 && type < 24) {
        if (g_state->players[P->player_no & 7].input_mode != 0 || (int8_t)P->spell_flash[type] > 0) {
            if (s_writes) P->spell_flash[type]--;           // state write (hud_tick_state)
            for (int i = 0; i < 10; i++) {
                const int slot = (int8_t)P->hotkey_slot[i];
                if (slot == -1) continue;
                if (thing_or_null(spell_slot_raw(P, slot)) == spell) {
                    ui_draw_sprite_tint((int16_t)x, (int16_t)y, hud_sprite((unsigned)(i + 0x1e)), ui_col_black());
                    break;
                }
            }
        }
    }
    const int32_t cost = spell->mana_cost;
    if (cost != 0 && (P->castle == 0 || thing_at(thing_wrap(P->castle))->mana < cost))
        ui_shade_rect((int16_t)x, (int16_t)y, dim->w, dim->h, 0x30);
}

// cast_ticks in 1..0x3f with duration > 0x40 blinks the icon off on odd ticks
static bool spell_blinked_off(const Thing *spell) {
    const int16_t ct = spell->cast_ticks;
    return ct < 0x40 && ct != 0 && spell->duration > 0x40 && tick_bit(1) != 0;
}

// ---------------------------------------------------------------------------------------------
// ui_draw_thing_label_22870(x, y, spell, mode): the hand panels (left 0x1fe, right 0x23e) and the
// selected spell in the book: background (entry 1 idle / 2 casting), icon (type + 6), mana bar
// (remainder of the caster's mana), charges as 2-pixel dots, burst bar.
// ---------------------------------------------------------------------------------------------
void ui_draw_thing_label(int x, int y, Thing *spell, int mode) {
    if (!thing_in_pool(spell)) return;
    ui_set_font(1);
    const Thing *caster = thing_or_null(spell->caster);
    if (!caster) return;
    const PlayerBlock *P = pblock(caster);
    const uint8_t colA = g_hud_player_colours[(P->player_no * 2) & 15];
    const uint8_t colB = g_hud_player_colours[(P->player_no * 2 + 1) & 15];
    if (spell_blinked_off(spell)) return;
    const UiSprite *bg = hud_sprite(spell->cast_ticks != 0 ? 2 : 1);
    if (mode) ui_draw_sprite((int16_t)x, (int16_t)y, bg);
    else ui_draw_sprite_blend((int16_t)x, (int16_t)y, bg);
    ui_draw_sprite((int16_t)x, (int16_t)y, hud_sprite((unsigned)((int8_t)spell->type + 6)));
    const int32_t total = spell->mana_total;
    if (total != 0) {                                   // idiv by zero in the exe
        const int32_t rem = caster->mana % total;
        gfx_fill_rect((int16_t)(x + 4), (int16_t)(y + 0x24), (uint16_t)(mul32(rem, 56) / total), 4, colB);
        int32_t charges = caster->mana / total;
        for (int i = 0; i < 0x1b && charges != 0; i++)
            for (int k = 0; k < 2 && charges != 0; k++, charges--)
                gfx_put_pixel((int16_t)(x + 4 + 2 * i), (int16_t)(y + 0x24 + 2 * k), colA);
    }
    if (spell->unk3e != 0 && (int8_t)spell->burst > 0)
        gfx_fill_rect((int16_t)(x + 4), (int16_t)(y + 0x24), (uint16_t)(((int8_t)spell->burst * 55) / (int8_t)spell->unk3e), 4, ui_col_green());
    spell_icon_marks(x, y, spell, caster, hud_sprite(1));
}

// ---------------------------------------------------------------------------------------------
// ui_draw_spell_panel_icon_22d80(x, y, spell, mode): an owned spell in the book (entry 3 / 4).
// ---------------------------------------------------------------------------------------------
void ui_draw_spell_panel_icon(int x, int y, Thing *spell, int mode) {
    if (!thing_in_pool(spell)) return;
    ui_set_font(1);
    const Thing *caster = thing_or_null(spell->caster);
    if (!caster) return;
    if (spell_blinked_off(spell)) return;
    const UiSprite *bg = hud_sprite(spell->cast_ticks != 0 ? 4 : 3);
    if (mode) ui_draw_sprite((int16_t)x, (int16_t)y, bg);
    else ui_draw_sprite_blend((int16_t)x, (int16_t)y, bg);
    ui_draw_sprite((int16_t)x, (int16_t)y, hud_sprite((unsigned)((int8_t)spell->type + 6)));
    spell_icon_marks(x, y, spell, caster, hud_sprite(3));
}

// ---------------------------------------------------------------------------------------------
// ui_draw_status_bars_219f0(player thing): castle, balloons, wizard panels at the top left.
// ---------------------------------------------------------------------------------------------
void ui_draw_status_bars(Thing *T) {
    PlayerBlock *P = pblock(T);
    const int pn = P->player_no;
    const uint8_t colA = g_hud_player_colours[(pn * 2) & 15];
    const uint8_t colB = g_hud_player_colours[(pn * 2 + 1) & 15];
    const uint8_t colC = g_hud_player_colours[(pn * 2 + tick_bit(2)) & 15];
    const uint8_t bar = 0x7b;                           // [esp+0x30]
    const int mode = 0;                                 // [esp+0x20], always 0
    const int y = 2;
    int x = 2;
    const int32_t total_mana = (int32_t)g_cfg->total_mana;
    auto win_marks = [&](int bx) {
        if (g_cfg->flags & 0x10) return;
        const int wx = bx + (int)((g_state->level.win_percent * 64) / 100);
        gfx_fill_rect((int16_t)wx, (int16_t)(y + 0x18), 2, 2, colC);
        gfx_fill_rect((int16_t)wx, (int16_t)(y + 0x24), 2, 2, colC);
    };
    auto panel = [&](unsigned idx) {
        if (mode) ui_draw_sprite(x, y, hud_sprite(idx)); else ui_draw_sprite_blend(x, y, hud_sprite(idx));
    };
    ui_draw_sprite_blend(x, y, hud_sprite(40));
    x += hud_sprite(40)->w;
    // castle
    Thing *castle = thing_at(thing_wrap(P->castle));
    if (P->castle != 0 && castle->aux != 0) {
        if ((int8_t)P->castle_hit_flash > 0 && tick_bit(1)) { ui_draw_sprite_blend(x, y, hud_sprite(55)); if (s_writes) P->castle_hit_flash--; }
        else ui_draw_sprite_blend(x, y, hud_sprite(41));
        ui_draw_sprite(x + 2, y, hud_sprite((unsigned)(castle->aux + 0x2b)));
        ui_draw_sprite(x + 0x26, y, hud_sprite(42));
        const int bx = x + 0x3a;
        ui_fill_bar(bx, y + 8, 0x40, 10, ratio64(castle->health, castle->max_health), bar);
        const int32_t m = castle->mana + P->mana_in_transit;
        if (m == castle->mana_total) {
            ui_fill_bar(bx, y + 0x1a, 0x40, 10, ratio64(m, total_mana), tick_bit(2) == 1 ? colB : colA);
        } else {
            ui_fill_bar(bx, y + 0x1a, 0x40, 10, ratio64(castle->mana_total, total_mana), colB);
            ui_fill_bar(bx, y + 0x1a, 0x40, 10, ratio64(castle->mana + P->mana_in_transit, total_mana), colA);
        }
        win_marks(bx);
    } else {
        panel(54);
    }
    x += hud_sprite(41)->w;
    // balloons
    if (P->castle != 0) {
        if ((int8_t)P->damage_flash > 0 && tick_bit(1)) { ui_draw_sprite_blend(x, y, hud_sprite(55)); if (s_writes) P->damage_flash--; }
        else ui_draw_sprite_blend(x, y, hud_sprite(41));
        int n = 0;
        switch ((uint16_t)castle->aux) {
        case 1: case 2: case 3: n = 1; break;
        case 4: case 5: n = 2; break;
        case 6: case 7: n = 3; break;
        default: n = 0; break;
        }
        ui_draw_sprite(x + 2, y, hud_sprite((unsigned)(n + 0x32)));
        ui_draw_sprite(x + 0x26, y, hud_sprite(42));
        for (int i = 0; i < n; i++) {
            const Thing *b = thing_or_null(P->balloons[i]);
            if (!b) continue;
            if (b->health >= 0) ui_fill_bar(x + 0x3a, y + 0xa + 2 * i, 0x40, 2, ratio64(b->health, b->max_health), bar);
            ui_fill_bar(x + 0x3a, y + 0x1c + 2 * i, 0x40, 2, ratio64(b->mana, b->mana_total), colA);
        }
    } else {
        panel(54);
    }
    x += hud_sprite(41)->w;
    // wizard
    if ((int8_t)P->hit_flash > 0 && tick_bit(1)) { ui_draw_sprite_blend(x, y, hud_sprite(55)); if (s_writes) P->hit_flash--; }
    else panel(41);
    if (((T->flags & 0x30) || P->invuln_timer != 0) && tick_bit(1)) {
        ui_draw_sprite_blend(x + 2, y, hud_sprite(43));
        ui_draw_sprite_blend(x + 0x26, y, hud_sprite(42));
    } else {
        ui_draw_sprite(x + 2, y, hud_sprite(43));
        ui_draw_sprite(x + 0x26, y, hud_sprite(42));
    }
    const int bx = x + 0x3a;
    ui_fill_bar(bx, y + 8, 0x40, 10, ratio64(T->health, T->max_health), bar);
    ui_fill_bar(bx, y + 0x1a, 0x40, 10, ratio64(T->mana_total, total_mana), colB);
    ui_fill_bar(bx, y + 0x1a, 0x40, 10, ratio64(T->mana, total_mana), colA);
    win_marks(bx);
}

// ---------------------------------------------------------------------------------------------
// ui_draw_player_list_21370: map screen table of players (row: name + mana, columns: kills).
// ---------------------------------------------------------------------------------------------
void ui_draw_player_list() {
    ui_set_font(1);
    const int count = (uint16_t)g_state->player_count;
    int active = 0;
    for (int p = 0; p < count && p < 8; p++) active += g_state->players[p].active != 0;
    const UiSprite *e85 = hud_sprite(85), *e86 = hud_sprite(86);
    const int x0 = (0x280 - (e85->w + active * e86->w)) / 2;
    int y = ((lo1() ? 400 : 0x1e0) - e85->h * active) / 2;
    char buf[64];
    for (int p = 0; p < count && p < 8; p++) {
        const PlayerRec &rec = g_state->players[p];
        if (rec.active != 1) continue;
        const Thing *T = thing_at(thing_wrap(rec.thing));
        const uint8_t colA = g_hud_player_colours[p * 2], colB = g_hud_player_colours[p * 2 + 1];
        int x = x0;
        ui_draw_sprite(x, y, e85);
        gfx_fill_rect(x + 4, y + 4, (uint16_t)(e85->w - 8), (uint16_t)(e85->h - 8), colA);
        ui_draw_text(rec.name, x + 8, y + 6, colB);
        std::snprintf(buf, sizeof buf, "%d", T->mana_total);
        ui_draw_text(buf, x + 8, y + 0x14, colB);
        x += e85->w;
        for (int q = 0; q < 8; q++) {
            if (q != p && g_state->players[q].active == 1) {
                const uint8_t qa = g_hud_player_colours[q * 2], qb = g_hud_player_colours[q * 2 + 1];
                ui_draw_sprite(x, y, e86);
                gfx_fill_rect(x + 4, y + 4, (uint16_t)(e86->w - 8), (uint16_t)(e86->h - 8), qa);
                std::snprintf(buf, sizeof buf, "%03d", (int16_t)pblock(T)->kills_of_player[q]);
                ui_draw_text(buf, x + 8, y + 0xa, qb);
            } else if (q == p) {
                ui_draw_sprite(x, y, e86);
                gfx_fill_rect(x + 4, y + 4, (uint16_t)(e86->w - 8), (uint16_t)(e86->h - 8), ui_col_black());
            }
            x += e86->w;
        }
        y += e85->h;
    }
}

// ---------------------------------------------------------------------------------------------
// ui_draw_debug_overlay_4ad80
// ---------------------------------------------------------------------------------------------
void ui_draw_debug_overlay() {
    g_cfg->debug_bits |= 1;
    const PlayerRec &rec = g_state->players[g_state->local_player & 7];
    if (rec.flags & 8) {
        ui_set_font(1);
        const int x = 0x140;
        int y = 0;
        char buf[256];
        auto line = [&](const char *s, uint8_t col) { ui_draw_text(s, x, y, col); y += ui_font_line_height(); };
        auto pair = [&](const char *label, const char *value) { line(label, ui_col_red()); line(value, ui_col_blue()); };
        pair("Product name", "Magic Carpet");
        pair("Version number", "Beta v8.0");
        std::snprintf(buf, sizeof buf, "%s %s", "Oct 20 1994", "11:49:09");
        pair("Version date", buf);
        pair("Programmer", "Bullfrog, Sean Cooper.");
        pair("Supplied to", "EA, Webster, Matt");
        std::snprintf(buf, sizeof buf, "%d", g_cfg->level);
        pair("Level Number", buf);
        if (g_cfg->flags & 0x10) {
            int32_t rate; std::memcpy(&rate, reinterpret_cast<uint8_t *>(g_cfg) + 0x9d, 4);
            std::snprintf(buf, sizeof buf, "%d", rate);
            pair("Transfer rate:", buf);
        }
        int32_t turn; std::memcpy(&turn, reinterpret_cast<uint8_t *>(g_cfg) + 0x99, 4);
        std::snprintf(buf, sizeof buf, "%d", turn);
        pair("GameTurn:", buf);
        std::snprintf(buf, sizeof buf, "%d", 0);        // DAT_0009e328: last sound number (sound system)
        pair("Sound Number", buf);
        std::snprintf(buf, sizeof buf, "%d %d", (int)rec.tick, (int)g_timer_ticks);
        pair("Game turn", buf);
        std::snprintf(buf, sizeof buf, "Thing %d, Active %d", 0xa4, thing_pool_slots() - thing_free_count());
        pair("Thing", buf);
        std::snprintf(buf, sizeof buf, "Carpet %d", 0x38d03); line(buf, ui_col_blue());
        std::snprintf(buf, sizeof buf, "Tape %d", 0x8e7e); line(buf, ui_col_blue());
        std::snprintf(buf, sizeof buf, "Heap %d", (int)g_cfg->pool_size); line(buf, ui_col_blue());
        std::snprintf(buf, sizeof buf, "%d/%d", 0, 0);  // TODO(port): mem_stats_622a8() (DOS memory manager)
        pair("Memory (Used/Free)", buf);
        // TODO(port): the memory block list "s%7.7d,u%01d" of mem_alloc_59870's node table (DAT_00130120)
    }
    if (g_cfg->password == 0xf851b9) g_cfg->debug_bits |= 2;
}

// ---------------------------------------------------------------------------------------------
// render_frame_1fab0
// ---------------------------------------------------------------------------------------------

static void clear_screen(const FrameBuffer &fb) {      // mem_set_5afd0(DAT_0012ed74, 0, w * h)
    std::memset(fb.pixels, 0, (size_t)fb.width * (size_t)fb.height);
}

// Help screen (input mode 1): the 1994 key list, one line per line height (nullptr = an empty line; the
// 320x200 and 640x480 string sets of the exe, 0x90040.. / 0x90060.., have the same text).
static const char *const k_help_lines[] = {
    "Cursor up       Move forwards.", "Cursor down     Move backwards.", "Cursor left     Move left.",
    "Cursor right    Move right.", "Left button     Use spell to the left.", "Right button    Use spell to the right.",
    "1 - 0           Select assigned spells.", "Space           Rebirth.", "Shift Q         Exit to dos.",
    "F5              Reflections toggle.", "F6              Sky toggle.", "F7              Shadows toggle.",
    "F8              Icons toggle.", "F9              Speed blur toggle.", "F10             Stereo modes toggle.",
    "Return          ", "Left and Right  Spell selection.", nullptr, nullptr,       // two line heights each
    "If you are experiencing slowness, try Pressing F5,F6,F7.", nullptr, nullptr,       // (0x2093c / 0x20944)
    "Magic Carpet comes to you from Bullfrog Productions Ltd.", "And will be released on the 15th November 1994.",
    "Copyright 1994 Bullfrog Productions Ltd.", nullptr, "Good Luck!",
};

static const char *credit_line(unsigned i) {
    if (i >= sizeof g_hud_credit_ptrs / sizeof g_hud_credit_ptrs[0]) return "#";
    const uint32_t off = g_hud_credit_ptrs[i] - 0x92228;
    return off < sizeof g_hud_credit_text ? reinterpret_cast<const char *>(g_hud_credit_text) + off : "#";
}

static void draw_messages(int local) {
    int x = 0x84, y = 0x32;
    ui_set_font(1);
    PlayerRec &rec = g_state->players[local];
    if (rec.status & 2) {                               // level won
        const uint8_t col = g_state->view_size == 0x28 ? ui_col_black() : blink_white();
        ui_draw_text(text_get(60), x, y, col);          // "World restored." (*(char **)0xaddd8)
        y += ui_font_line_height();
        ui_draw_text(text_get(61), x, y, col);          // "Press the space bar to continue." (0xadddc)
        y += ui_font_line_height();
    }
    if (g_cfg->paused & 1) {
        ui_draw_text("PAUSED!", x, y, g_state->view_size == 0x28 ? ui_col_black() : blink_white());
        y += ui_font_line_height();
    }
    char buf[256];
    for (int q = 0; q < (uint16_t)g_state->player_count && q < 8; q++) {
        PlayerMsg &m = rec.messages[q];
        char text[0x41];
        std::memcpy(text, m.text, 0x40); text[0x40] = 0;
        const char *name = g_state->players[q].name;
        char nm[0x41]; std::memcpy(nm, name, 0x40); nm[0x40] = 0;
        switch (m.arg) {
        case 0:                                         // "<name> <text>" notices
            if ((int16_t)m.ticks <= 0) break;
            std::snprintf(buf, sizeof buf, "%s %s", nm, text);
            ui_draw_text(buf, x, y, ui_col_red());
            y += ui_font_line_height(); if (s_writes) m.ticks--;
            break;
        case 1:                                         // the chat line being typed
            std::snprintf(buf, sizeof buf, ">%s?", text);
            ui_draw_text(buf, x, y, ui_col_red());
            y += ui_font_line_height();
            break;
        case 2:
            if ((int16_t)m.ticks <= 0) break;
            std::snprintf(buf, sizeof buf, "%s", text);
            ui_draw_text(buf, x, y, ui_col_red());
            y += ui_font_line_height(); if (s_writes) m.ticks--;
            break;
        case 3:                                         // received chat, blinks for the first ticks
            if ((int16_t)m.ticks <= 0) break;
            std::snprintf(buf, sizeof buf, "[%s] %s", nm, text);
            ui_draw_text(buf, x, y, (int16_t)m.ticks > 100 ? blink_red() : ui_col_red());
            y += ui_font_line_height(); if (s_writes) m.ticks--;
            break;
        default: break;
        }
    }
}

static void spell_book(int local) {
    gfx_fill_rows(ui_lo_res() ? 0xc8 : 0x1e0, 0);
    Thing *T = thing_at(thing_wrap(g_state->players[local].thing));
    PlayerBlock *P = pblock(T);
    // HUD sprite 3 is the cell; without a loaded HUD table (state-only use) input.cpp's shipped sizes
    const int cell_h = hud_sprite(3)->h ? hud_sprite(3)->h : input_book_cell_h();
    const int cell_w = hud_sprite(3)->w ? hud_sprite(3)->w : input_book_cell_w();
    int x = 0x180, y = lo1() ? 0xa2 : 0xc2;
    int sel_x = 0, sel_y = 0;
    int sel = -1;                                       // Config.spell_slot (0xff = none)
    auto pointer_in = [&](int cx, int cy) {
        return g_mouse_x >= cx && g_mouse_x < cx + 0x40 && g_mouse_y >= cy && g_mouse_y < cy + cell_h;
    };
    for (int cell = 0; cell < 24; cell++) {
        const int id = g_hud_book_order[cell];
        Thing *spell = thing_or_null((int16_t)P->spell_thing[id]);
        if (spell) {
            const int32_t cost = spell->mana_cost;
            const bool affordable = cost == 0 || (P->castle != 0 && cost <= thing_at(thing_wrap(P->castle))->mana);
            if (affordable && pointer_in(x, y)) {
                sel_x = x; sel_y = y;
                sel = cell;                             // the selected one is drawn as a label below
            } else {
                ui_draw_spell_panel_icon(x, y, spell, 0);
                if (g_hud_joystick_present && pointer_in(x, y)) vga_draw_box(x, y, cell_w, cell_h, ui_col_white());
            }
        } else {
            ui_draw_spell_icon(x, y, id);
            if (g_hud_joystick_present && pointer_in(x, y)) vga_draw_box(x, y, cell_w, cell_h, ui_col_white());
        }
        x += 0x40;
        if (x >= 0x280) { x = 0x180; y += cell_h; }
    }
    if (s_writes) g_cfg->spell_slot = sel < 0 ? 0xff : (uint8_t)sel;   // state write (hud_tick_state)
    if (sel >= 0) {
        Thing *spell = thing_or_null((int16_t)P->spell_thing[g_hud_book_order[sel]]);
        if (spell) ui_draw_thing_label(sel_x, sel_y, spell, 1);
    }
}

static void map_screen(const FrameBuffer &fb, int local, const Camera &cam) {
    if (s_drawing) {
        if (s_last_clear != 2) { clear_screen(fb); s_last_clear = 2; }
        render_set_view_window_top(fb, 0x10);
        render_view_frame(fb, cam);
    }
    const int unused = fb.width - 0xc0;                 // DAT_0012ed80 - 0xc0
    const PosLogEntry &e = g_state->players[local].log[g_state->players[local].view_entry % 32];
    const int cx = (int16_t)e.x, cy = (int16_t)e.y, yaw = (int16_t)e.yaw;
    ui_draw_radar(0, 0, cx, cy, 0x17e, lo1() ? 0x17a : 0x19e, yaw, 0xaa, unused, 1);
    ui_draw_radar_blips(0, 0, cx, cy, 0x17e, lo1() ? 0x17e : 0x19e, yaw, 0xaa, unused, 1);
    if (g_mouse_y >= 0x17e) ui_draw_player_list();
}

static void frame_pass(const FrameBuffer &fb, int player);

// Port-only (render references): draw the flight HUD during movie playback as well - what
// tools/reference/fb/patch_carpet.py --hud does to the original (nops the `jne` after
// `test byte [cfg], 4` at 0x1fc85); the credits roll of the tail still sees Config bit 2.
bool g_hud_force_flight_hud = false;

// render_frame_1fab0: state writes and drawing in one pass (the original's single call).
void render_frame(const FrameBuffer &fb, int player) {
    s_writes = true; s_drawing = fb.pixels != nullptr;
    frame_pass(fb, player);
}
// Only the game-state writes of render_frame_1fab0, no frame buffer.
void hud_tick_state(int player) {
    s_writes = true; s_drawing = false;
    frame_pass(FrameBuffer{nullptr, 0, 0}, player);
    s_drawing = true;
}
// Only the drawing of render_frame_1fab0: no game-state writes.
void render_frame_draw(const FrameBuffer &fb, int player) {
    s_writes = false; s_drawing = fb.pixels != nullptr;
    frame_pass(fb, player);
    s_writes = true;
}

static void frame_pass(const FrameBuffer &fb, int player) {
    s_local = player & 7;
    const int local = s_local;
    ui_set_target(fb);
    g_book_cell_w = hud_sprite(3)->w;                   // the HUD table's sprite 3 (input.h)
    g_book_cell_h = hud_sprite(3)->h;
    PlayerRec &rec = g_state->players[local];
    // port (round 10): the debug camera (debug_camera.h) replaces the view in the draw-only pass
    const Camera cam = (g_debug_camera.active && !s_writes) ? g_debug_camera.cam
                     : (g_render_interp.have_camera && !s_writes) ? g_render_interp.camera : player_camera(local);
    const uint8_t mode = rec.input_mode;
    if (mode <= 4) {
        switch (mode) {
        case 0: case 3: {
            const int8_t vs = (int8_t)g_state->view_size;
            if (s_drawing) {
                if (s_last_clear != 0 || vs != (int)s_last_view_size) {
                    s_last_clear = 0;
                    s_last_view_size = (uint8_t)vs;
                    if (vs < 0x28) gfx_fill_rows(ui_lo_res() ? 0xc8 : 0x1e0, 0);
                }
                if (vs < 0x28) gfx_fill_rows(ui_lo_res() ? 0xc8 : 0x1e0, 0);
                render_set_view_window(fb, vs);
                render_view_frame(fb, cam);
            }
            Thing *T = thing_at(thing_wrap(rec.thing));
            if (T->health < 0 || ((g_cfg->flags & 4) && !g_hud_force_flight_hud)) break;   // 0x1fc82
            const PosLogEntry &e = rec.log[rec.view_entry % 32];
            if (g_state->opt_hud_a) {
                const int unused = fb.width - 2 * (fb.width / 5);   // DAT_0012ed80 - 2 * (DAT_0012ed80 / 5)
                // port: g_settings.radar_zoom_pct zooms the flight radar out (100 = the original 0x100 = 1 cell per pixel)
                const int radar_scale = 0x100 * std::clamp(g_settings.radar_zoom_pct, 50, 200) / 100;
                // port: g_settings.radar_round - 320x200 rows are 6/5 as tall as the columns are wide on a
                // 4:3 display, so 64 x 54 is a circle; ui_draw_radar steps the map by h / w per column already
                const int radar_h = (g_settings.radar_round && lo1()) ? 0x6c : 0x80;
                ui_draw_radar(0, 0, (int16_t)e.x, (int16_t)e.y, 0x80, radar_h, (int16_t)e.yaw, radar_scale, unused, 0);
                ui_draw_radar_blips(0, 0, (int16_t)e.x, (int16_t)e.y, 0x80, radar_h, (int16_t)e.yaw, radar_scale, unused, 0);
            }
            if (s_drawing && g_key_down[0x38] && g_key_down[0x2d]) debug_screenshot(g_hud_screenshot_dir);   // Alt+X
            ui_set_font(1);
            if (g_state->opt_hud_b) {
                PlayerBlock *P = pblock(T);
                ui_draw_thing_label(0x1fe, 2, thing_or_null(spell_slot_raw(P, P->slot_left)), 0);
                ui_draw_thing_label(0x23e, 2, thing_or_null(spell_slot_raw(P, P->slot_right)), 0);
                ui_draw_status_bars(T);
            }
            ui_set_font(1);
            if (!(g_cfg->flags & 0x200)) draw_messages(local);
            else ui_set_font(1);
            break;
        }
        case 1: {                                       // help screen
            const int8_t vs = (int8_t)g_state->view_size;
            if (!s_drawing) break;                      // the help screen writes no game state
            if (s_last_clear != 0 || vs != (int)s_last_view_size) {
                clear_screen(fb);
                s_last_view_size = (uint8_t)vs;
                s_last_clear = 0;
            }
            render_set_view_window(fb, vs);
            render_view_frame(fb, cam);
            ui_set_font(1);
            const int x = ui_font_space_width();
            int y = -ui_font_line_height() / 2;
            for (const char *line : k_help_lines) {
                y += ui_font_line_height();
                if (line) ui_draw_text(line, x, y, ui_col_white());
            }
            break;
        }
        case 2:
            spell_book(local);
            map_screen(fb, local, cam);                 // falls through to case 4 in the exe
            break;
        case 4:
            map_screen(fb, local, cam);
            break;
        default: break;
        }
    }
    // tail: recording indicator and the credits roll of the attract movie
    ui_set_font(1);
    char buf[64];
    if (!(g_cfg->flags & 0x20) && (rec.tick & 2) && (g_cfg->flags & 2)) {
        std::snprintf(buf, sizeof buf, "MOVIE: %d", (int16_t)g_cfg->movie);
        ui_draw_text(buf, 0, 0, ui_col_red());
    }
    if (!(g_cfg->flags & 4)) return;
    uint8_t *cs = g_cfg->credits_state;                 // +0xa1 state, +0xa2 i32 count, +0xa6 u16 line
    int32_t count; std::memcpy(&count, cs + 1, 4);
    uint16_t idx; std::memcpy(&idx, cs + 5, 2);
    uint8_t state = cs[0];
    // The state machine runs on locals in every pass (0x210ef..0x212cf: the 3 -> 2 -> 1 transitions come
    // before the drawing of the same call, a page that runs out is skipped without drawing); only the
    // passes with state writes store it back. Round 6 (render reference with the credits roll): the
    // draw-only pass used to skip the transitions and force a draw, which showed the previous page.
    if (state == 3) { if (count > 0) count--; else state = 2; }
    if (state == 2) { state = 1; idx = 0; count = 0x32; }
    if (state == 1) {
        if (count > 0) {
            int n = 0;
            int y = 0x17c;                              // computed and then discarded by the exe
            while (credit_line(idx + (unsigned)n)[0] != '!' && credit_line(idx + (unsigned)n)[0] != '#') { y -= ui_font_line_height(); n++; }
            (void)y;
            y = 8;
            for (int i = 0; i < n; i++) {
                ui_draw_text(credit_line(idx + (unsigned)i), 8, y, i == 0 ? ui_col_white() : ui_col_black());
                y += ui_font_line_height();
            }
            count--;
        } else {
            while (credit_line(idx)[0] != '!' && credit_line(idx)[0] != '#') idx++;
            idx++;
            if (credit_line(idx)[0] == '#') { state = 3; count = 200; }
            else count = 0x32;
        }
    }
    if (!s_writes) return;
    cs[0] = state;
    std::memcpy(cs + 1, &count, 4);
    std::memcpy(cs + 5, &idx, 2);
}
