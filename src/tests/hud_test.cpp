#define _CRT_SECURE_NO_WARNINGS
// Test of the HUD / frame composition (hud.cpp) and the 2D layer (ui_draw.cpp).
//  1. 2D layer by construction: span blitter (copy, solid, mirror of the halving in 320x200),
//     text width / advance of known strings against the .tab entries, colour cube, rect fills,
//     clipping of every primitive against a guarded buffer;
//  2. the radar by construction: rectangular radar at yaw 0 puts the camera cell's colour at the
//     centre pixel, the circle profile limits the circular radar;
//  3. frames of the level 38 snapshot (engine_load_snapshot) through render_frame at 640x480
//     (g_video_mode_flags 8) and 320x200 (1): flight HUD, spell book, map screen, help screen,
//     written as hud_test_frame_*.ppm into the current directory; HUD regions differ from the
//     plain view, guard bands intact, Config.spell_slot agrees with input_book_update_selection,
//     message ticks count down.
// argv[1] = game dir. Exit code 0 = pass.
#include "engine.h"
#include "hud.h"
#include "ui_draw.h"
#include "mc_globals.h"
#include "player.h"
#include "input.h"
#include "mcfile.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static const int GUARD = 16384;

struct Canvas {
    std::vector<uint8_t> buf;
    FrameBuffer fb;
    int w, h;
    Canvas(int w_, int h_) : buf((size_t)w_ * h_ + 2 * GUARD, 0xAB), fb{nullptr, w_, h_}, w(w_), h(h_) {
        fb.pixels = buf.data() + GUARD;
        std::memset(fb.pixels, 0, (size_t)w * h);
    }
    bool guards_ok() const {
        for (int i = 0; i < GUARD; i++)
            if (buf[i] != 0xAB || buf[(size_t)GUARD + (size_t)w * h + i] != 0xAB) return false;
        return true;
    }
    uint8_t px(int x, int y) const { return fb.pixels[y * w + x]; }
};

static bool write_ppm(const std::string &path, const FrameBuffer &fb) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    uint8_t rgb[768];
    mc_palette_to_rgb(g_palette6, rgb);
    std::fprintf(f, "P6\n%d %d\n255\n", fb.width, fb.height);
    for (int i = 0; i < fb.width * fb.height; i++) std::fwrite(rgb + fb.pixels[i] * 3, 1, 3, f);
    std::fclose(f);
    return true;
}

static void set_mode(const char *game, int flags) {
    g_video_mode_flags = (uint16_t)flags;
    CHECK(ui_draw_set_video_mode(game));
}

// ---------------------------------------------------------------------------------------------
// 1. 2D layer
// ---------------------------------------------------------------------------------------------
static void test_2d(const char *game) {
    for (int flags : {8, 1}) {
        set_mode(game, flags);
        const bool lo = flags == 1;
        const int W = lo ? 320 : 640, H = lo ? 200 : 480;
        Canvas c(W, H);
        ui_set_target(c.fb);

        // sprite table sizes: the 320 tables are doubled (mspr0-0 sprite 3 is 32x18 -> 64x36)
        const UiSprite *s3 = hud_sprite(3);
        CHECK(s3->w == 64 && s3->h == (lo ? 36 : 37));

        // text: width = sum of the glyph widths of font 1 (entries c + 1), drawn advance identical
        ui_set_font(1);
        const char *msg = "PAUSED!";
        int expect = 0;
        for (const char *p = msg; *p; p++) expect += g_ui_font_tables[1].set.entries[(uint8_t)*p + 1].width * (lo ? 2 : 1);
        const int tw = ui_text_width(msg);
        CHECK(tw == expect && tw > 20);
        const int end = ui_draw_text(msg, 100, 100, ui_col_white());
        CHECK(end == 100 + tw);
        int lit = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) lit += c.px(x, y) == ui_col_white();
        CHECK(lit > 30);
        // every lit pixel of the text lies inside its box
        int outside = 0;
        const int bx0 = lo ? 50 : 100, by0 = lo ? 50 : 100, bx1 = bx0 + (lo ? tw / 2 : tw), by1 = by0 + (lo ? ui_font_line_height() / 2 : ui_font_line_height());
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (c.px(x, y) == ui_col_white() && (x < bx0 || x >= bx1 || y < by0 || y >= by1)) outside++;
        CHECK(outside == 0);
        std::printf("2d %dx%d: \"%s\" width %d (font 1 line height %d), %d pixels lit\n", W, H, msg, tw, ui_font_line_height(), lit);
        ui_set_font(0);
        std::printf("2d: font 0 \"Magic Carpet\" width %d, line height %d\n", ui_text_width("Magic Carpet"), ui_font_line_height());

        // colour cube (components c*4+3, 0x334f3): the corners are the nearest entries to black / white / red
        CHECK(ui_col_black() == palette_find_nearest(g_palette6, 3, 3, 3));
        CHECK(ui_col_white() == palette_find_nearest(g_palette6, 63, 63, 63));
        CHECK(ui_col_red() == palette_find_nearest(g_palette6, 63, 3, 3));

        // rect fill halving
        std::memset(c.fb.pixels, 0, (size_t)W * H);
        gfx_fill_rect(10, 20, 30, 40, 7);
        int filled = 0;
        for (int i = 0; i < W * H; i++) filled += c.fb.pixels[i] == 7;
        CHECK(filled == (lo ? 15 * 20 : 30 * 40));
        CHECK(c.px(lo ? 5 : 10, lo ? 10 : 20) == 7);

        // clipping: everything far outside / across the edges, the guard bands stay intact
        const int coords[] = {-2000, -300, -40, -1, 0, 1, 300, 600, 639, 640, 1000, 32000};
        for (int x : coords)
            for (int y : coords) {
                ui_draw_sprite(x, y, hud_sprite(41));
                ui_draw_sprite_blend(x, y, hud_sprite(40));
                ui_draw_sprite_tint(x, y, hud_sprite(7), 3);
                ui_draw_sprite_shaded(x, y, hud_sprite(8), 0x10);
                ui_draw_text("Clip test 123", x, y, 5);
                ui_draw_text_background("Clip", x, y, 5);
                gfx_fill_rect(x, y, 100, 50, 9);
                gfx_put_pixel(x, y, 9);
                ui_shade_rect(x, y, 80, 60, 0x18);
                vga_draw_box(x, y, 64, 37, 4);
                ui_copy_block(x, y, y, x);
                ui_draw_radar(x, y, 0x8000, 0x8000, 0x80, 0x80, x & 0x7ff, 0x100, 0, (x ^ y) & 1);
            }
        CHECK(c.guards_ok());
        ui_draw_map(10, 10, 64, 48, 0x4000, 0x4000);
        CHECK(c.guards_ok());
    }
}

// ---------------------------------------------------------------------------------------------
// 2. radar by construction
// ---------------------------------------------------------------------------------------------
static void test_radar(const char *game) {
    set_mode(game, 8);
    Canvas c(640, 480);
    ui_set_target(c.fb);
    const int cam_x = 0x5380, cam_y = 0x9a80;          // any cell centre
    const uint16_t cell = mc_cell_of((uint16_t)cam_x, (uint16_t)cam_y);
    const uint8_t want = g_shade_table()[(g_map_light[cell] << 8) | g_tex_avg_colour()[g_map_type[cell]]];
    ui_draw_radar(0, 0, cam_x, cam_y, 0x80, 0x80, 0, 0x100, 0, 1);
    CHECK(c.px(0x40, 0x40) == want);
    // scale 0x100: one cell per pixel; the pixel 3 to the right is the cell 3 cells along +x
    {
        const uint16_t cell3 = mc_cell_of((uint16_t)(cam_x + 0x300), (uint16_t)cam_y);
        CHECK(c.px(0x43, 0x40) == g_shade_table()[(g_map_light[cell3] << 8) | g_tex_avg_colour()[g_map_type[cell3]]]);
    }
    // circular radar: corners untouched, centre written
    std::memset(c.fb.pixels, 0, 640 * 480);
    ui_draw_radar(0, 0, cam_x, cam_y, 0x80, 0x80, 0x123, 0x100, 0, 0);
    CHECK(c.px(0, 0) == 0 && c.px(0x7f, 0) == 0 && c.px(0, 0x7f) == 0);
    int written = 0;
    for (int y = 0; y < 0x80; y++)
        for (int x = 0; x < 0x80; x++) written += c.px(x, y) != 0;
    std::printf("radar: centre colour %d (cell 0x%04x), circular radar covers %d of %d pixels (pi/4 = %d)\n",
                want, cell, written, 0x80 * 0x80, 0x80 * 0x80 * 785 / 1000);
    CHECK(written > 0x80 * 0x80 * 70 / 100 && written < 0x80 * 0x80 * 85 / 100);
    CHECK(c.guards_ok());
}

// ---------------------------------------------------------------------------------------------
// 3. snapshot frames
// ---------------------------------------------------------------------------------------------
static int diff_count(const Canvas &a, const std::vector<uint8_t> &plain, int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) n += a.px(x, y) != plain[(size_t)y * a.w + x];
    return n;
}

static void test_frames(const char *game) {
    const int local = g_state->local_player & 7;
    PlayerRec &rec = g_state->players[local];
    std::printf("snapshot: local player %d, input mode %d, view size 0x%x, hud options %d %d, thing %d health %d\n",
                local, rec.input_mode, g_state->view_size, g_state->opt_hud_a, g_state->opt_hud_b, rec.thing,
                g_state->things[rec.thing].health);
    {
        const PlayerBlock &P = rec.blk;
        for (int hand = 0; hand < 2; hand++) {
            const int slot = hand ? P.slot_right : P.slot_left;
            const int idx = (slot >= 0 && slot < 24) ? P.spell_slot[slot] : -1;
            const Thing *t = (idx > 0 && idx < 1000) ? &g_state->things[idx] : nullptr;
            std::printf("hand %d: book slot %d -> thing %d", hand, slot, idx);
            if (t) std::printf(" cls %d type %d caster %d cast_ticks %d duration %d mana_total %d", t->cls, t->type, t->caster, t->cast_ticks, t->duration, t->mana_total);
            std::printf("\n");
        }
    }
    // the snapshot's hands are empty (slot 0xff): put the first two owned spells into them so the
    // hand labels are exercised
    {
        int found = 0;
        for (int i = 0; i < 24 && found < 2; i++)
            if (rec.blk.spell_slot[i] > 0 && rec.blk.spell_slot[i] < 1000) {
                (found ? rec.blk.slot_right : rec.blk.slot_left) = (int16_t)i;
                found++;
            }
        std::printf("hands set to book slots %d / %d\n", rec.blk.slot_left, rec.blk.slot_right);
    }
    g_state->opt_hud_a = 1;
    g_state->opt_hud_b = 1;
    for (int flags : {8, 1}) {
        set_mode(game, flags);
        const bool lo = flags == 1;
        const int W = lo ? 320 : 640, H = lo ? 200 : 480;
        const char *tag = lo ? "320" : "640";
        // the plain 3D view, as mcport draws it today
        Canvas plain(W, H);
        render_set_view_window(plain.fb, g_state->view_size);
        render_view(plain.fb, player_camera(local));
        // the HUD frame; a notice in message slot 1 to see the message lines
        Canvas c(W, H);
        rec.input_mode = 0;
        std::snprintf(rec.messages[1].text, sizeof rec.messages[1].text, "%s", "has died.");
        rec.messages[1].arg = 0;
        rec.messages[1].ticks = 5;
        render_frame(c.fb, local);
        CHECK(c.guards_ok());
        CHECK(rec.messages[1].ticks == 4);
        write_ppm(std::string("hud_test_frame_") + tag + ".ppm", c.fb);
        const int s = lo ? 2 : 1;                       // 640-space -> screen
        std::vector<uint8_t> pv(plain.fb.pixels, plain.fb.pixels + W * H);
        const int radar = diff_count(c, pv, 0, 0, 0x80 / s, 0x80 / s);
        const int bars = diff_count(c, pv, 0x80 / s, 0, 0x1fe / s, 0x30 / s);
        const int labels = diff_count(c, pv, 0x1fe / s, 0, W, 0x30 / s);
        const int msgs = diff_count(c, pv, 0x84 / s, 0x32 / s, 0x200 / s, 0x60 / s);
        std::printf("frame %s: HUD changed %d radar, %d status-bar, %d hand-label, %d message pixels\n", tag, radar, bars, labels, msgs);
        CHECK(radar > 0x80 * 0x80 / (s * s) / 2);
        CHECK(bars > 1000 / (s * s));
        CHECK(labels > 1000 / (s * s));
        CHECK(msgs > 50 / (s * s));
        // unchanged outside the HUD: the bottom half of the view
        CHECK(diff_count(c, pv, 0, H / 2, W, H) == 0);

        // spell book: pointer on cell 0, the HUD's selection agrees with input.cpp's hit test
        for (int cell : {0, 5, 23}) {
            Canvas b(W, H);
            rec.input_mode = 2;
            int cx, cy;
            input_book_cell_origin(cell, &cx, &cy);
            g_mouse_x = (int16_t)(cx + 10);
            g_mouse_y = (int16_t)(cy + 10);
            input_book_update_selection();
            const uint8_t want = g_cfg->spell_slot;
            g_cfg->spell_slot = 0x77;
            render_frame(b.fb, local);
            CHECK(b.guards_ok());
            std::printf("book %s: pointer on cell %2d at (%d,%d): HUD selection %d, input.cpp %d\n", tag, cell, cx, cy,
                        g_cfg->spell_slot == 0xff ? -1 : g_cfg->spell_slot, want == 0xff ? -1 : want);
            CHECK(g_cfg->spell_slot == want);
            if (cell == 23) write_ppm(std::string("hud_test_frame_book_") + tag + ".ppm", b.fb);
        }
        // split: hud_tick_state makes exactly render_frame's writes, render_frame_draw makes none and
        // draws the same pixels (book mode with a selection, and the flight HUD with a message and a
        // flashing status panel)
        for (int m : {2, 0}) {
            rec.input_mode = (uint8_t)m;
            int cx, cy;
            input_book_cell_origin(5, &cx, &cy);
            g_mouse_x = (int16_t)(cx + 10); g_mouse_y = (int16_t)(cy + 10);
            rec.messages[2].arg = 2; rec.messages[2].ticks = 3;
            std::snprintf(rec.messages[2].text, sizeof rec.messages[2].text, "%s", "split test");
            rec.blk.hit_flash = 4;
            g_cfg->tick_bits[0] = 1;
            for (int i = 0; i < 24; i++) rec.blk.spell_flash[i] = 0;
            std::vector<uint8_t> st0((uint8_t *)g_state, (uint8_t *)g_state + sizeof(GameState));
            std::vector<uint8_t> cf0((uint8_t *)g_cfg, (uint8_t *)g_cfg + sizeof(Config));
            Canvas a(W, H), d(W, H);
            render_frame(a.fb, local);
            std::vector<uint8_t> st1((uint8_t *)g_state, (uint8_t *)g_state + sizeof(GameState));
            std::vector<uint8_t> cf1((uint8_t *)g_cfg, (uint8_t *)g_cfg + sizeof(Config));
            int changed = 0;
            for (size_t i = 0; i < st0.size(); i++) changed += st0[i] != st1[i];
            for (size_t i = 0; i < cf0.size(); i++) changed += cf0[i] != cf1[i];
            std::memcpy(g_state, st0.data(), st0.size()); std::memcpy(g_cfg, cf0.data(), cf0.size());
            render_frame_draw(d.fb, local);
            CHECK(std::memcmp(g_state, st0.data(), st0.size()) == 0 && std::memcmp(g_cfg, cf0.data(), cf0.size()) == 0);
            CHECK(std::memcmp(a.fb.pixels, d.fb.pixels, (size_t)W * H) == 0);
            hud_tick_state(local);
            CHECK(std::memcmp(g_state, st1.data(), st1.size()) == 0 && std::memcmp(g_cfg, cf1.data(), cf1.size()) == 0);
            std::printf("split %s mode %d: render_frame changed %d state bytes; tick_state reproduces them, draw-only writes none, pixels identical\n", tag, m, changed);
            CHECK(changed > 0);
            std::memcpy(g_state, st0.data(), st0.size()); std::memcpy(g_cfg, cf0.data(), cf0.size());
            rec.messages[2].ticks = 0;
        }
        // map screen with the player list (pointer low on the screen)
        {
            Canvas m(W, H);
            rec.input_mode = 4;
            g_mouse_y = 0x190;
            render_frame(m.fb, local);
            CHECK(m.guards_ok());
            write_ppm(std::string("hud_test_frame_map_") + tag + ".ppm", m.fb);
            // port: the wheel zoom (display only): closer in the 256 world; the original scale is the maximum there
            Canvas z(W, H);
            hud_map_zoom_step(3);
            CHECK(g_hud_map_zoom_pct == 51);
            render_frame(z.fb, local);
            CHECK(z.guards_ok());
            CHECK(std::memcmp(z.fb.pixels, m.fb.pixels, (size_t)W * H) != 0);
            write_ppm(std::string("hud_test_frame_map_zoom_") + tag + ".ppm", z.fb);
            hud_map_zoom_step(-10);
            CHECK(g_hud_map_zoom_pct == 100);
        }
        // help screen
        {
            Canvas hlp(W, H);
            rec.input_mode = 1;
            render_frame(hlp.fb, local);
            CHECK(hlp.guards_ok());
            write_ppm(std::string("hud_test_frame_help_") + tag + ".ppm", hlp.fb);
        }
        // debug overlay
        {
            Canvas d(W, H);
            rec.input_mode = 0;
            rec.flags |= 8;
            render_frame(d.fb, local);
            ui_draw_debug_overlay();
            rec.flags &= ~8;
            CHECK(d.guards_ok());
            CHECK(g_cfg->debug_bits & 1);
            write_ppm(std::string("hud_test_frame_debug_") + tag + ".ppm", d.fb);
        }
        rec.input_mode = 0;
    }
    set_mode(game, 8);
    // a 1024 world (the snapshot's level tiled): the wheel zooms out to the whole world (400 %)
    {
        CHECK(world_widen_tiled(1024));
        CHECK(hud_map_zoom_max_pct() == 400);
        hud_map_zoom_step(-20);
        CHECK(g_hud_map_zoom_pct == 400);
        Canvas w(640, 480);
        rec.input_mode = 4;
        render_frame(w.fb, local);
        CHECK(w.guards_ok());
        write_ppm("hud_test_frame_map_world1024.ppm", w.fb);
        rec.input_mode = 0;
        g_hud_map_zoom_pct = 100;
        world_set_size(256);
    }
}

int main(int argc, char **argv) {
    const char *game = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    if (!engine_init(game)) { std::printf("engine_init failed\n"); return 1; }
    CHECK(engine_load_snapshot("movie/gam00000.dat", "movie/map00000.dat"));
    g_video_mode_flags = 8;
    CHECK(ui_draw_init(game));
    test_2d(game);
    test_radar(game);
    test_frames(game);
    ui_draw_shutdown();
    engine_shutdown();
    std::printf(g_fail ? "hud_test: %d FAILURES\n" : "hud_test: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
