#define _CRT_SECURE_NO_WARNINGS
// Test of the sprite cache (sprite_cache.cpp) and the Thing renderer (render_things.cpp).
//  1. cache bookkeeping: resident groups, group load / unload, LRU eviction, level priorities, the
//     demo recorder's "needed" table, FLIC animation of the animated sprites;
//  2. render_sprite_scaled with a synthetic sprite: exact comparison with a reference scaler at
//     roll 0 (plain, mirrored, upside down, upright path), position / orientation of the four sprite
//     quadrants for roll angles in all 8 octants, and a random stress test inside a guarded canvas;
//  3. frames of the engine's own snapshot of level 38 (movie/gam00000.dat + map00000.dat) with the
//     things drawn by render_cell_things / render_cell_things_mirrored, written as
//     sprites_test_*.ppm into the current directory (convert with tools/port/ppm2png.py).
// argv[1] = game dir. Exit code 0 = pass.
#include "sprites.h"
#include "raster.h"
#include "render.h"
#include "tables.h"
#include "terrain.h"
#include "mc_math.h"
#include "mcfile.h"
#include "crash_handler.h"
#include "gen/core_tables.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static uint32_t s_rng = 0x1234567;
static uint32_t rnd() { s_rng = s_rng * 1664525u + 1013904223u; return s_rng >> 8; }
static int rnd_range(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }

// ---------------------------------------------------------------------------------------------
// 1. cache
// ---------------------------------------------------------------------------------------------

static int count_loaded() {
    int n = 0;
    for (int i = 0; i < MC_SPRITE_COUNT; i++) n += g_sprite_ptr[i] != nullptr;
    return n;
}

static unsigned find_unloaded_group(unsigned after) {
    for (unsigned i = after; i < MC_SPRITE_COUNT; i++)
        if (sprite_group_of(i) == i && !g_sprite_ptr[i] && !g_sprite_locked[i]) return i;
    return 0xffff;
}

static void test_cache() {
    g_cfg->tick = 100;
    // directory: group id = index of the group's first sprite, groups are contiguous
    int groups = 0;
    for (unsigned i = 0; i < MC_SPRITE_COUNT; i++) {
        const unsigned g = sprite_group_of(i);
        CHECK(g <= i && sprite_group_of(g) == g);
        if (g == i) groups++;
        CHECK(sprite_chunk_size(i) > 6);
    }
    CHECK(sprite_group_of(MC_SPRITE_SLOTS) == 0);
    CHECK(sprite_group_of(MC_SPRITE_OAK) == MC_SPRITE_OAK);    // port sprites (world_art): their own group
    CHECK(sprite_group_of(MC_SPRITE_OAK_BURNT) == MC_SPRITE_OAK_BURNT);
    // resident groups are loaded by sprite_cache_init and nothing else is
    int locked = 0, resident_sprites = 0;
    for (unsigned i = 0; i < MC_SPRITE_COUNT; i++) {
        if (g_sprite_locked[i]) { locked++; CHECK(g_sprite_ptr[i] != nullptr); }
        if (g_sprite_ptr[i]) { resident_sprites++; CHECK(g_sprite_locked[sprite_group_of(i)] != 0 || g_sprite_locked[i] != 0); }
    }
    std::printf("cache: %d groups, %d resident (locked) sprite descriptors, %d sprites loaded at init\n",
                groups, locked, resident_sprites);
    CHECK(locked > 0 && resident_sprites >= locked);

    // on-demand load of a whole group
    unsigned g1 = 0xffff;
    for (unsigned i = 0; i < MC_SPRITE_COUNT; i++)      // a group with several sprites
        if (sprite_group_of(i) == i && !g_sprite_ptr[i] && !g_sprite_locked[i] && sprite_group_of(i + 2) == i) { g1 = i; break; }
    CHECK(g1 != 0xffff);
    if (g1 == 0xffff) return;
    uint32_t sum = 0;
    unsigned members = 0;
    for (unsigned i = g1; i < MC_SPRITE_COUNT && sprite_group_of(i) == g1; i++) { sum += sprite_chunk_size(i) + 10; members++; }
    CHECK(sprite_group_size(g1 + 1) == sum);
    reinterpret_cast<uint8_t *>(g_cfg)[0x95] = 0;
    CHECK(sprite_ensure_loaded(g1 + 1) == 1);
    CHECK(reinterpret_cast<uint8_t *>(g_cfg)[0x95] == 5);
    for (unsigned i = g1; i < g1 + members; i++) {
        CHECK(g_sprite_ptr[i] != nullptr);
        CHECK(g_sprite_group_stamp[i] == 100);
        CHECK(sprite_loaded_bytes(i) == sprite_chunk_size(i));
        const uint8_t *s = g_sprite_ptr[i];
        const unsigned w = s[2] | (s[3] << 8), h = s[4] | (s[5] << 8);
        CHECK(w > 0 && h > 0 && (size_t)w * h + 6 <= sprite_loaded_bytes(i));
    }
    CHECK(g_sprite_ptr[g1 + members] == nullptr || sprite_group_of(g1 + members) != g1);

    // locked groups refuse the polite unload
    unsigned lk = 0;
    while (lk < MC_SPRITE_COUNT && !g_sprite_locked[lk]) lk++;
    CHECK(!sprite_group_unload_if_unlocked(lk) && g_sprite_ptr[lk] != nullptr);
    CHECK(sprite_group_unload_if_unlocked(g1) && g_sprite_ptr[g1] == nullptr && g_sprite_ptr[g1 + members - 1] == nullptr);
    CHECK(g_sprite_group_stamp[g1] == 0);
    CHECK(!sprite_group_unload_if_unlocked(g1));         // not loaded any more

    // LRU: three groups with stamps 30, 10, 20 -> the one stamped 10 goes first
    unsigned ga = find_unloaded_group(0), gb = find_unloaded_group(ga + 1), gc = find_unloaded_group(gb + 1);
    CHECK(ga != 0xffff && gb != 0xffff && gc != 0xffff);
    g_cfg->tick = 30; sprite_ensure_loaded(ga);
    g_cfg->tick = 10; sprite_ensure_loaded(gb);
    g_cfg->tick = 20; sprite_ensure_loaded(gc);
    const uint32_t size_b = sprite_group_size(gb);
    CHECK(sprite_cache_evict_lru(1) == size_b);
    CHECK(g_sprite_ptr[ga] && !g_sprite_ptr[gb] && g_sprite_ptr[gc]);
    CHECK(sprite_cache_evict_lru(0) == 0);              // nothing needed, nothing freed
    const uint32_t freed = sprite_cache_evict_lru(0x7fffffff);   // at most 5 groups per call, residents stay
    CHECK(freed == sprite_group_size(ga) + sprite_group_size(gc));
    CHECK(!g_sprite_ptr[ga] && !g_sprite_ptr[gc]);
    CHECK(count_loaded() == resident_sprites);
    // with only residents loaded they become candidates
    CHECK(sprite_cache_evict_lru(1) > 0);
    CHECK(count_loaded() < resident_sprites);
    texture_load_resident();
    CHECK(count_loaded() == resident_sprites);

    // level priorities: class 5 model 1 needs SpriteDesc 86 (first record of DAT_0009649e with one entry)
    sprite_group_priorities_clear();
    sprite_mark_needed_for_model(5, 1, -1);
    const unsigned grp86 = sprite_group_of(g_sprite_desc[86].base_sprite);
    CHECK(g_sprite_group_priority[grp86] == g_sprite_desc[86].load_priority && g_sprite_desc[86].load_priority != 0);
    int marked = 0;
    for (int i = 0; i < MC_SPRITE_COUNT; i++) marked += g_sprite_group_priority[i] != 0;
    CHECK(marked == 1);
    sprite_mark_needed_for_model(77, 77, 3);            // unknown model: the fallback descriptor
    CHECK(g_sprite_group_priority[sprite_group_of(g_sprite_desc[3].base_sprite)] == g_sprite_desc[3].load_priority);
    sprite_mark_needed_for_model(77, 77, -1);           // no fallback: nothing
    // class 5 model 4: the list has no terminator and continues through the next record (5, 5, ...)
    sprite_group_priorities_clear();
    sprite_mark_needed_for_model(5, 4, -1);
    CHECK(g_sprite_group_priority[sprite_group_of(g_sprite_desc[206].base_sprite)] == g_sprite_desc[206].load_priority);
    CHECK(g_sprite_group_priority[sprite_group_of(g_sprite_desc[244].base_sprite)] == g_sprite_desc[244].load_priority);
    sprite_group_priorities_clear();
    sprite_mark_needed_for_model(5, 1, -1);
    g_cfg->tick = 55;
    sprite_ensure_loaded(ga);                           // not needed by the "level": must be dropped
    sprite_groups_reload_by_priority();
    CHECK(!g_sprite_ptr[ga]);
    CHECK(g_sprite_ptr[grp86] != nullptr);
    CHECK(count_loaded() >= resident_sprites);
    for (unsigned i = 0; i < MC_SPRITE_COUNT; i++)
        if (g_sprite_ptr[i]) CHECK(g_sprite_locked[sprite_group_of(i)] || g_sprite_group_priority[sprite_group_of(i)]);

    // the demo recorder's table: 0 = not loaded, 1 = loaded, 2 = loaded resident group head
    texture_mark_needed();
    std::vector<uint8_t> was(MC_SPRITE_COUNT);
    for (unsigned i = 0; i < MC_SPRITE_COUNT; i++) {
        was[i] = g_sprite_ptr[i] != nullptr;
        CHECK(g_state->texture_needed[i] == (g_sprite_ptr[i] ? (g_sprite_locked[i] ? 2 : 1) : 0));
    }
    sprite_group_unload(grp86);
    sprite_ensure_loaded(gc);
    texture_load_needed();
    for (unsigned i = 0; i < MC_SPRITE_COUNT; i++) CHECK((g_sprite_ptr[i] != nullptr) == (was[i] != 0));

    // hooks of switch_activate (thing.h)
    CHECK(g_hook_sprite_group_priorities_clear == sprite_group_priorities_clear);
    CHECK(g_hook_sprite_groups_reload_by_priority == sprite_groups_reload_by_priority);
    ThingInit rec{};
    rec.cls = 5; rec.model = 1;
    sprite_group_priorities_clear();
    g_hook_sprite_mark_needed_for_model(&rec);
    CHECK(g_sprite_group_priority[grp86] != 0);
}

// Animated sprites: every FLIC loop must decode inside its chunk and end exactly at the chunk end.
static void test_anim() {
    int animated = 0, ring = 0, frames_total = 0, still = 0;
    for (unsigned n = 0; n < MC_SPRITE_COUNT; n++) {
        const bool was_loaded = g_sprite_ptr[n] != nullptr;
        if (!sprite_ensure_loaded(n)) { CHECK(!"sprite does not load"); continue; }
        uint8_t *s = g_sprite_ptr[n];
        if (!(s[0] & 1)) { if (!was_loaded) sprite_group_unload_if_unlocked(n); continue; }
        animated++;
        unsigned frame = 0, count = 0;
        uint32_t off = 0;
        CHECK(sprite_anim_info(n, &frame, &count, &off));
        const unsigned w = s[2] | (s[3] << 8), h = s[4] | (s[5] << 8);
        if (!(frame == 1 && count >= 2 && off == w * h + 6)) std::printf("  anim sprite %u: frame %u count %u off %u (w %u h %u)\n", n, frame, count, off, w, h);
        CHECK(frame == 1 && count >= 1 && off == w * h + 6);
        std::vector<uint8_t> first(s + 6, s + 6 + w * h);
        // not drawn -> not advanced
        texture_anim_update();
        sprite_anim_info(n, &frame, nullptr, nullptr);
        CHECK(frame == 1);
        bool changed = false;
        for (unsigned k = 0; k < count; k++) {
            s[0] |= 8;
            texture_anim_update();
            CHECK((s[0] & 8) == 0);
            unsigned f2 = 0;
            CHECK(sprite_anim_info(n, &f2, nullptr, &off));     // still active = the frame decoded
            CHECK(f2 == k + 2);
            if (std::memcmp(first.data(), s + 6, w * h) != 0) changed = true;
            frames_total++;
        }
        if (!changed) { std::printf("  anim sprite %u: %u frames, image never changes\n", n, count); still++; }
        CHECK(off == sprite_loaded_bytes(n) - 6);               // the loop ends at the end of the chunk
        if (std::memcmp(first.data(), s + 6, w * h) == 0) ring++;
        // the next update wraps to the first frame chunk
        s[0] |= 8;
        texture_anim_update();
        CHECK(sprite_anim_info(n, &frame, nullptr, nullptr) && frame == 2);
        if (!was_loaded) sprite_group_unload_if_unlocked(n);
    }
    std::printf("anim: %d animated sprites, %d frames decoded, %d loops return to the stored image, %d never change\n",
                animated, frames_total, ring, still);
    CHECK(animated == 213 && ring == animated);
}

// ---------------------------------------------------------------------------------------------
// 2. blitter
// ---------------------------------------------------------------------------------------------

static const int VW = 320, VH = 200, MARGIN = 64;
static const int CW = VW + 2 * MARGIN, CH = VH + 2 * MARGIN;
static std::vector<uint8_t> s_canvas(CW * CH);
static const int SW = 16, SH = 12;                      // synthetic sprite: quadrants coloured 1..4
static uint8_t s_sprite[SW * SH];

static uint8_t *view_px() { return s_canvas.data() + MARGIN * CW + MARGIN; }

static void canvas_reset(uint8_t fill) {
    std::memset(s_canvas.data(), 0xEE, s_canvas.size());
    for (int y = 0; y < VH; y++) std::memset(view_px() + y * CW, fill, VW);
    render_set_viewport(view_px(), nullptr, CW, VW, VH);
}

static bool canvas_margin_intact() {
    for (int y = 0; y < CH; y++)
        for (int x = 0; x < CW; x++) {
            const bool inside = x >= MARGIN && x < MARGIN + VW && y >= MARGIN && y < MARGIN + VH;
            if (!inside && s_canvas[y * CW + x] != 0xEE) return false;
        }
    return true;
}

// The camera part of render_landscape that the blitter depends on.
static void set_roll(int roll) {
    render_build_roll_table((-roll) & 0x7ff);
    g_rcam.sin_roll = mc_sin((unsigned)roll & 0x7ff);
    g_rcam.cos_roll = mc_cos((unsigned)roll & 0x7ff);
}

static void blit(int x, int y, int w, int h, int mode, int shade, bool mirror, bool upright, unsigned anchor) {
    SpriteBlit &b = g_sprite_blit;
    b = SpriteBlit{};
    b.mode = mode; b.shade = shade;
    b.dst_w = w; b.dst_h = h;
    b.pixels = s_sprite; b.pixels_size = sizeof s_sprite;
    b.x = x; b.y = y;
    b.src_w = mirror ? -SW : SW; b.src_h = SH; b.src_stride = SW;
    b.upright = upright ? 1 : 0;
    render_sprite_scaled(anchor);
}

// Reference for roll 0: top-left (x0, y0), w x h pixels, source row / column by 16.16 steps.
static void reference_blit(std::vector<uint8_t> &img, int x0, int y0, int w, int h, bool mirror, bool flip) {
    const int32_t xstep = (int32_t)((uint32_t)SW << 16) / w, ystep = (int32_t)((uint32_t)SH << 16) / h;
    for (int r = 0; r < h; r++) {
        const int sr = flip ? ((h - 1 - r) * ystep) >> 16 : (r * ystep) >> 16;
        for (int i = 0; i < w; i++) {
            const int sc = mirror ? ((w - 1 - i) * xstep) >> 16 : (i * xstep) >> 16;
            const int x = x0 + i, y = y0 + r;
            if (x < 0 || x >= VW || y < 0 || y >= VH) continue;
            img[y * VW + x] = s_sprite[sr * SW + sc];
        }
    }
}

static bool view_equals(const std::vector<uint8_t> &img) {
    for (int y = 0; y < VH; y++)
        if (std::memcmp(view_px() + y * CW, img.data() + y * VW, VW) != 0) return false;
    return true;
}

static void view_diff_report(const std::vector<uint8_t> &img) {
    int n = 0, x0 = VW, x1 = -1, y0 = VH, y1 = -1, rx0 = VW, rx1 = -1, ry0 = VH, ry1 = -1, gx0 = VW, gx1 = -1, gy0 = VH, gy1 = -1;
    for (int y = 0; y < VH; y++)
        for (int x = 0; x < VW; x++) {
            const uint8_t g = view_px()[y * CW + x], r = img[y * VW + x];
            if (g != r) { n++; if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
            if (r != 9) { if (x < rx0) rx0 = x; if (x > rx1) rx1 = x; if (y < ry0) ry0 = y; if (y > ry1) ry1 = y; }
            if (g != 9) { if (x < gx0) gx0 = x; if (x > gx1) gx1 = x; if (y < gy0) gy0 = y; if (y > gy1) gy1 = y; }
        }
    std::printf("    %d pixels differ in x %d..%d y %d..%d; reference box x %d..%d y %d..%d, drawn box x %d..%d y %d..%d\n",
                n, x0, x1, y0, y1, rx0, rx1, ry0, ry1, gx0, gx1, gy0, gy1);
}

static void test_blit_exact() {
    canvas_reset(9);                                    // sets the render target the roll table is built for
    set_roll(0);
    int cases = 0, bad = 0;
    for (int it = 0; it < 400; it++) {
        const int w = rnd_range(1, 150), h = rnd_range(1, 120);
        const int x = rnd_range(-60, VW + 60), y = rnd_range(-60, VH + 60);
        const bool mirror = (it & 1) != 0;
        const unsigned anchor = it % 3;                 // 0 shadow, 1 thing, 2 reflection
        std::vector<uint8_t> ref(VW * VH, 9);
        // anchor 1: (x, y) = bottom centre; 0 / 2: top centre, upside down
        if (anchor == 1) reference_blit(ref, x - (w >> 1), y - h, w, h, mirror, false);
        else             reference_blit(ref, x - (w >> 1), y, w, h, mirror, true);
        canvas_reset(9);
        blit(x, y, w, h, 0, 0x2000, mirror, false, anchor);
        cases++;
        if (!view_equals(ref)) {
            if (bad < 5) { std::printf("  exact mismatch: x=%d y=%d w=%d h=%d mirror=%d anchor=%u\n", x, y, w, h, mirror, anchor); view_diff_report(ref); }
            bad++;
        }
        CHECK(canvas_margin_intact());
    }
    // upright path at roll 0: anchor 1 centres the box half its size above the point
    for (int it = 0; it < 200; it++) {
        const int w = rnd_range(1, 150), h = rnd_range(1, 120);
        const int x = rnd_range(-60, VW + 60), y = rnd_range(-60, VH + 60);
        const unsigned anchor = it % 3;
        const int q = (w + h) >> 2;
        std::vector<uint8_t> ref(VW * VH, 9);
        if (anchor == 1)      reference_blit(ref, x - q, y - 2 * q, w, h, false, false);
        else if (anchor == 2) reference_blit(ref, x - q, y, w, h, false, false);
        else                  reference_blit(ref, x, y, w, h, false, false);
        canvas_reset(9);
        blit(x, y, w, h, 0, 0x2000, false, true, anchor);
        cases++;
        if (!view_equals(ref)) {
            if (bad < 8) { std::printf("  upright mismatch: x=%d y=%d w=%d h=%d anchor=%u\n", x, y, w, h, anchor); view_diff_report(ref); }
            bad++;
        }
        CHECK(canvas_margin_intact());
    }
    std::printf("blit exact (roll 0): %d cases, %d mismatches\n", cases, bad);
    CHECK(bad == 0);

    // pixel modes on one opaque 40x30 blit over a background of colour 77
    const uint8_t *shade = g_shade_table(), *blend = g_blend_table();
    struct ModeCase { int mode, shade; bool upright; };
    const ModeCase mc[] = {
        {1, 0x1200, false}, {2, 0x2000, false}, {3, 0x2000, false}, {4, 0x2000, false}, {5, 0x2000, false},
        {6, 0x0900, false}, {7, 0x0900, false}, {8, 0x2800, false}, {9, 0x5a0000, false},
        {1, 0x1200, true},  {2, 0x2000, true},  {3, 0x2000, true},  {4, 0x2000, true},  {5, 0x2000, true},
        {6, 0x0900, true},  {7, 0x0900, true},  {8, 0x2800, true},  {9, 0x5a0000, true}, {10, 0x2000, false},
    };
    for (const ModeCase &m : mc) {
        canvas_reset(77);
        blit(100, 100, 40, 30, m.mode, m.shade, false, m.upright, 0);
        // texel of the top-left pixel: upside down in the rolled path (anchor 0) -> colour 3, else 1
        const unsigned t = m.upright ? 1 : 3, d = 77, lv = (unsigned)m.shade >> 8;
        unsigned want = d;
        switch (m.mode) {
        case 1: want = shade[(lv << 8) | t]; break;
        case 2: want = blend[(t << 8) | d]; break;
        case 3: want = blend[(d << 8) | t]; break;
        case 4: want = blend[((m.upright ? 4u : 0u) << 8) | t]; break;
        case 5: want = blend[(t << 8) | (m.upright ? 5u : 0u)]; break;
        case 6: want = shade[(lv << 8) | blend[(t << 8) | d]]; break;
        case 7: want = shade[(lv << 8) | blend[(d << 8) | t]]; break;
        case 8: want = m.upright ? d : shade[(lv << 8) | d]; break;
        case 9: want = 0x5a; break;
        default: break;
        }
        const int px = m.upright ? 100 : 100 - 20;
        const unsigned got = view_px()[100 * CW + px + 1];
        if (got != want) std::printf("  mode %d upright %d: got %u want %u\n", m.mode, m.upright, got, want);
        CHECK(got == want);
    }
}

// Quadrant centroids of a 64x48 blit for a roll angle: the sprite must be the rectangle rotated
// about the anchor like the terrain is rolled ("right" = (cos, -sin), "down" = (sin, cos)).
static void test_blit_rolled() {
    static const int rolls[] = {0, 0x10, 0x60, 0xc0, 0xff, 0x100, 0x101, 0x140, 0x1c0, 0x200, 0x230, 0x2d0, 0x300,
                                0x340, 0x3c0, 0x400, 0x440, 0x4c0, 0x500, 0x540, 0x5c0, 0x600, 0x640, 0x6c0,
                                0x700, 0x740, 0x7c0, 0x7ff};
    const int W = 64, H = 48, PX = 160, PY = 100;
    double worst = 0;
    int worst_roll = 0, worst_cfg = 0;
    for (int roll : rolls) {
        const double a = roll * 6.283185307179586 / 2048.0, s = std::sin(a), c = std::cos(a);
        for (int cfg = 0; cfg < 4; cfg++) {             // bit0 mirror, bit1 anchor 2 (upside down below the point)
            const bool mirror = (cfg & 1) != 0, hang = (cfg & 2) != 0;
            set_roll(roll);
            canvas_reset(0);
            blit(PX, PY, W, H, 0, 0x2000, mirror, false, hang ? 2u : 1u);
            CHECK(canvas_margin_intact());
            double sx[5] = {}, sy[5] = {};
            int cnt[5] = {};
            for (int y = 0; y < VH; y++)
                for (int x = 0; x < VW; x++) {
                    const unsigned v = view_px()[y * CW + x];
                    if (v >= 1 && v <= 4) { sx[v] += x + 0.5; sy[v] += y + 0.5; cnt[v]++; }
                }
            const int total = cnt[1] + cnt[2] + cnt[3] + cnt[4];
            if (std::abs(total - W * H) > W * H / 8) {
                std::printf("  roll 0x%x cfg %d: %d pixels drawn, expected about %d\n", roll, cfg, total, W * H);
                CHECK(!"pixel count");
            }
            // top-left corner of the rectangle and the quadrant centres
            const double tlx = hang ? PX - W / 2.0 * c : PX - H * s - W / 2.0 * c;
            const double tly = hang ? PY + W / 2.0 * s : PY - H * c + W / 2.0 * s;
            if (std::getenv("SPRITES_TEST_VERBOSE")) {
                double mx = 0, my = 0;
                for (int v = 1; v <= 4; v++) { mx += sx[v] / (cnt[v] ? cnt[v] : 1) / 4; my += sy[v] / (cnt[v] ? cnt[v] : 1) / 4; }
                const double cx = tlx + W / 2.0 * c + H / 2.0 * s, cy = tly - W / 2.0 * s + H / 2.0 * c;
                std::printf("  roll 0x%03x octant %d cfg %d: %5d px, centre error (%+.2f, %+.2f)\n", roll, g_roll.octant, cfg, total, mx - cx, my - cy);
            }
            for (int v = 1; v <= 4; v++) {
                int col = (v - 1) & 1, row = (v - 1) >> 1;      // source quadrant of colour v
                if (mirror) col ^= 1;
                if (hang) row ^= 1;
                const double u = (col ? 0.75 : 0.25) * W, w2 = (row ? 0.75 : 0.25) * H;
                const double ex = tlx + u * c + w2 * s, ey = tly - u * s + w2 * c;
                if (cnt[v] == 0) { std::printf("  roll 0x%x cfg %d: colour %d missing\n", roll, cfg, v); CHECK(!"colour missing"); continue; }
                const double dx = sx[v] / cnt[v] - ex, dy = sy[v] / cnt[v] - ey;
                const double err = std::sqrt(dx * dx + dy * dy);
                if (err > worst) { worst = err; worst_roll = roll; worst_cfg = cfg; }
                if (err > 4.0) {                        // quadrant centres are 32 / 24 px apart
                    std::printf("  roll 0x%x (octant %d) cfg %d colour %d: centroid (%.1f, %.1f), expected (%.1f, %.1f)\n",
                                roll, g_roll.octant, cfg, v, sx[v] / cnt[v], sy[v] / cnt[v], ex, ey);
                    CHECK(!"quadrant position");
                }
            }
        }
    }
    std::printf("blit rolled: %d angles x 4 configurations, worst quadrant centroid error %.2f px (roll 0x%x cfg %d)\n",
                (int)(sizeof rolls / sizeof rolls[0]), worst, worst_roll, worst_cfg);
}

// Clipping consistency: a blit into the 320x200 view must equal the same blit into a larger view
// (padded along the roll table's minor axis on both sides and along its major axis at the far end,
// which keeps the Bresenham phase of the table) cropped to the 320x200 window.
static void test_blit_crop() {
    static std::vector<uint8_t> a(VW * VH), big(CW * CH);
    int cases = 0, bad = 0, edge_quirk = 0;
    int per_octant[8] = {};
    for (int it = 0; it < 4000; it++) {
        const int roll = (it % 5 == 0) ? (int)((rnd() % 8) * 0x100 + rnd_range(-2, 2)) & 0x7ff : (int)(rnd() & 0x7ff);
        const int w = rnd_range(1, 220), h = rnd_range(1, 220);
        const int x = rnd_range(-120, VW + 120), y = rnd_range(-120, VH + 120);
        const bool mirror = (rnd() & 1) != 0;
        const unsigned anchor = (unsigned)rnd_range(0, 2);
        // small view
        std::memset(s_canvas.data(), 9, s_canvas.size());
        render_set_viewport(view_px(), nullptr, CW, VW, VH);
        set_roll(roll);
        const int octant = g_roll.octant;
        blit(x, y, w, h, 0, 0x2000, mirror, false, anchor);
        for (int r = 0; r < VH; r++) std::memcpy(&a[r * VW], view_px() + r * CW, VW);
        // padded view
        const bool major_is_width = octant == 0 || octant == 3 || octant == 4 || octant == 7;
        const bool major_forward = octant == 0 || octant == 1 || octant == 2 || octant == 7;
        const int P = 48;
        int pl = 0, pr = 0, pt = 0, pb = 0;
        if (major_is_width) { pt = pb = P; if (major_forward) pr = P; else pl = P; }
        else                { pl = pr = P; if (major_forward) pb = P; else pt = P; }
        std::memset(s_canvas.data(), 9, s_canvas.size());
        render_set_viewport(view_px() - pt * CW - pl, nullptr, CW, VW + pl + pr, VH + pt + pb);
        set_roll(roll);
        blit(x + pl, y + pt, w, h, 0, 0x2000, mirror, false, anchor);
        cases++;
        per_octant[octant]++;
        // Odd octants: the exe's line numbering along the minor axis is off by one (the first line it
        // keeps at the far edge is invisible and the last visible one at the near edge is dropped),
        // so a sprite crossing the near minor edge loses its pixels on that one screen row / column
        // (octant 1: x = 0, 3: y = 0, 5: x = width - 1, 7: y = height - 1). Everything else must match.
        int diff = 0;
        bool quirk_only = (octant & 1) != 0;
        for (int r = 0; r < VH; r++)
            for (int c = 0; c < VW; c++) {
                const uint8_t s1 = a[r * VW + c], s2 = view_px()[r * CW + c];
                if (s1 == s2) continue;
                diff++;
                const bool edge = octant == 1 ? c == 0 : octant == 3 ? r == 0 : octant == 5 ? c == VW - 1 : r == VH - 1;
                if (!(edge && s1 == 9)) quirk_only = false;
            }
        if (diff && quirk_only) { edge_quirk++; diff = 0; }
        if (diff) {
            if (bad < 12) {
                int n = 0, x0 = VW, x1 = -1, y0 = VH, y1 = -1, only_small = 0, only_big = 0;
                for (int r = 0; r < VH; r++)
                    for (int c = 0; c < VW; c++) {
                        const uint8_t s1 = a[r * VW + c], s2 = view_px()[r * CW + c];
                        if (s1 != s2) {
                            n++; if (c < x0) x0 = c; if (c > x1) x1 = c; if (r < y0) y0 = r; if (r > y1) y1 = r;
                            if (s2 == 9) only_small++; else if (s1 == 9) only_big++;
                        }
                    }
                std::printf("  crop mismatch: roll 0x%03x (octant %d) x=%d y=%d w=%d h=%d mirror=%d anchor=%u: %d px in x %d..%d y %d..%d (%d only in the small view, %d only in the padded view)\n",
                            roll, octant, x, y, w, h, mirror, anchor, n, x0, x1, y0, y1, only_small, only_big);
            }
            bad++;
        }
    }
    std::printf("blit crop consistency: %d cases (per octant %d %d %d %d %d %d %d %d), %d mismatches, %d odd-octant cases with the edge-line quirk\n", cases,
                per_octant[0], per_octant[1], per_octant[2], per_octant[3], per_octant[4], per_octant[5], per_octant[6], per_octant[7], bad, edge_quirk);
    CHECK(bad == 0);
    canvas_reset(9);
}

static void test_blit_stress() {
    g_sprite_oob_reads = g_sprite_oob_writes = 0;
    int margin_bad = 0;
    for (int it = 0; it < 60000; it++) {
        if ((it & 63) == 0) set_roll((int)(rnd() & 0x7ff));
        if ((it & 1023) == 0) canvas_reset(5);
        const int w = rnd_range(1, 400), h = rnd_range(1, 400);
        const int x = rnd_range(-250, VW + 250), y = rnd_range(-250, VH + 250);
        const int mode = rnd_range(0, 9);
        const int shade = mode == 8 ? rnd_range(0x2000, 0x2800) : mode == 9 ? rnd_range(0, 0xff) << 16 : rnd_range(0, 0x20) << 8;
        const bool upright = (rnd() & 7) == 0;
        blit(x, y, w, h, mode, shade, !upright && (rnd() & 1), upright, (unsigned)rnd_range(0, 2));
        if ((it & 255) == 255 && !canvas_margin_intact()) margin_bad++;
    }
    std::printf("blit stress: 60000 random blits (all octants / modes / anchors), refused reads %u writes %u, margin violations %d\n",
                g_sprite_oob_reads, g_sprite_oob_writes, margin_bad);
    CHECK(margin_bad == 0);
    CHECK(g_sprite_oob_reads == 0 && g_sprite_oob_writes == 0);

    // sprites far larger than the screen (a thing right in front of the camera): the exe's tables
    // overflow here, the port must stay inside the viewport
    uint32_t before_r = g_sprite_oob_reads, before_w = g_sprite_oob_writes;
    canvas_reset(5);
    for (int it = 0; it < 6000; it++) {
        if ((it & 15) == 0) set_roll((int)(rnd() & 0x7ff));
        const int w = rnd_range(300, 6000), h = rnd_range(300, 6000);
        blit(rnd_range(-3000, 3000), rnd_range(-3000, 3000), w, h, rnd_range(0, 9), 0x2000, (rnd() & 1) != 0, false, (unsigned)rnd_range(0, 2));
    }
    CHECK(canvas_margin_intact());
    std::printf("blit stress (huge sprites): 6000 blits, refused reads %u writes %u\n",
                g_sprite_oob_reads - before_r, g_sprite_oob_writes - before_w);
    g_sprite_oob_reads = g_sprite_oob_writes = 0;
}

// ---------------------------------------------------------------------------------------------
// 3. frames of the level 38 snapshot
// ---------------------------------------------------------------------------------------------

static bool load_snapshot(const char *game_dir) {
    char path[1024];
    mc_blob gam, map;
    mc_path_join(path, sizeof path, game_dir, "movie/gam00000.dat");
    if (!mc_read_file(path, &gam)) return false;
    mc_path_join(path, sizeof path, game_dir, "movie/map00000.dat");
    if (!mc_read_file(path, &map)) { mc_blob_free(&gam); return false; }
    bool ok = gam.len == MC_STATE_IMAGE_SIZE && map.len >= 0x60000;
    if (ok) {
        std::memcpy(g_map_type, map.data, 0x10000);
        std::memcpy(g_map_height, map.data + 0x10000, 0x10000);
        std::memcpy(g_map_light, map.data + 0x20000, 0x10000);
        std::memcpy(g_map_flags, map.data + 0x30000, 0x10000);
        std::memcpy(g_cell_things, map.data + 0x40000, 0x20000);
        ok = thing_load_snapshot_image(gam.data, g_state);
    }
    mc_blob_free(&gam);
    mc_blob_free(&map);
    return ok;
}

static const int GUARD = 8192;
static int s_hook_calls = 0, s_hook_calls_mirror = 0;
static void counting_hook(int first, const VertexRec *cell) { s_hook_calls++; render_cell_things(first, cell); }
static void counting_hook_mirror(int first, const VertexRec *cell) { s_hook_calls_mirror++; render_cell_things_mirrored(first, cell); }

static bool write_ppm(const std::string &path, const uint8_t *px, int w, int h) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    uint8_t rgb[768];
    mc_palette_to_rgb(g_palette6, rgb);
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::vector<uint8_t> row((size_t)w * 3);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) std::memcpy(&row[(size_t)x * 3], rgb + px[y * w + x] * 3, 3);
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return true;
}

struct FrameStats { int sprite_pixels, calls, calls_mirror; bool ok; };

// Renders the view twice (without and with the thing hooks) and reports how many pixels the things
// changed. Guard bands around the frame buffer must stay untouched.
static FrameStats render_frame(const char *name, int w, int h, const Camera &cam, bool mirrored_too = true) {
    FrameStats st{};
    std::vector<uint8_t> buf((size_t)w * h + 2 * GUARD), plain((size_t)w * h);
    FrameBuffer fb{buf.data() + GUARD, w, h};
    auto guards_ok = [&]() {
        for (int i = 0; i < GUARD; i++)
            if (buf[i] != 0xAB || buf[(size_t)GUARD + (size_t)w * h + i] != 0xAB) return false;
        return true;
    };
    st.ok = true;
    for (int pass = 0; pass < 2; pass++) {
        std::memset(buf.data(), 0xAB, buf.size());
        g_render_cell_things = pass ? counting_hook : nullptr;
        g_render_cell_things_mirrored = pass && mirrored_too ? counting_hook_mirror : nullptr;
        s_hook_calls = s_hook_calls_mirror = 0;
        render_set_view_window(fb, 0x28);
        render_landscape(cam);
        if (!guards_ok()) { std::printf("FAIL: %s pass %d wrote into the guard band\n", name, pass); st.ok = false; g_fail++; }
        if (pass == 0) std::memcpy(plain.data(), fb.pixels, plain.size());
    }
    for (size_t i = 0; i < plain.size(); i++) st.sprite_pixels += plain[i] != fb.pixels[i];
    st.calls = s_hook_calls;
    st.calls_mirror = s_hook_calls_mirror;
    const std::string path = std::string("sprites_test_") + name + ".ppm";
    write_ppm(path, fb.pixels, w, h);
    std::printf("frame %-22s %dx%d cam (%5d,%5d,z %4d) yaw 0x%03x pitch %3d roll 0x%03x: %4d cell lists (+%d mirrored), %6d pixels changed by things\n",
                name, w, h, cam.cam_x & 0xffff, cam.cam_y & 0xffff, cam.cam_z, cam.yaw & 0x7ff, cam.pitch, cam.roll & 0x7ff,
                st.calls, st.calls_mirror, st.sprite_pixels);
    render_things_install();
    return st;
}

// Camera `dist` units behind (tx, ty) looking along `yaw` (0 = towards -y, 0x200 = towards +x).
static Camera look_at(int tx, int ty, int yaw, int dist, int height, int pitch, int roll) {
    Camera c{};
    const int fx = mc_sin((unsigned)yaw & 0x7ff), fy = -mc_cos((unsigned)yaw & 0x7ff);
    c.cam_x = (tx - (int)(((int64_t)fx * dist) >> 16)) & 0xffff;
    c.cam_y = (ty - (int)(((int64_t)fy * dist) >> 16)) & 0xffff;
    c.yaw = yaw;
    c.cam_z = terrain_sample_height((uint16_t)c.cam_x, (uint16_t)c.cam_y) + height;
    c.pitch = pitch; c.roll = roll; c.zoom = 0x80;
    return c;
}

static int cell_dist(const Thing &a, const Thing &b) {
    const int dx = std::abs(mc_wrap_diff(a.x >> 8, b.x >> 8)), dy = std::abs(mc_wrap_diff(a.y >> 8, b.y >> 8));
    return dx > dy ? dx : dy;
}

// A thing standing exactly on a terrain vertex must be anchored at the screen position the
// landscape renderer computed for that vertex (two independent code paths: the quadrant-rotated
// vertex grid of render_landscape and the per-thing transform of render_cell_things).
static int s_probe_x, s_probe_y, s_probe_hits;
static void anchor_probe(unsigned anchor) {
    if (anchor == 1) { s_probe_x = g_sprite_blit.x; s_probe_y = g_sprite_blit.y; s_probe_hits++; }
}

static void test_anchor_vs_terrain(uint16_t scenery_sprite) {
    static const Camera cams[] = {
        {640, 30080, 0, 0x180, 0, 0, 0x80},          {700, 30000, 0x155, 0x300, 0x10, 0x60, 0x80},
        {43145, 48648, 0x2b0, 4700, 0x20, 0x7a0, 0x80}, {43145, 46216, 0x433, 4900, -0x10, 0x333, 0x100},
        {30000, 23000, 0x5e1, 5300, 0x18, 0x400, 0x80}, {31000, 23600, 0x7ff, 5200, 0, 0x5c0, 0xc0},
    };
    const Thing saved = g_state->things[0];
    g_sprite_blit_probe = anchor_probe;
    g_render_cell_things = nullptr;
    g_render_cell_things_mirrored = nullptr;
    int checked = 0, culled = 0, worst = 0, on_screen_count = 0;
    for (int ci = 0; ci < (int)(sizeof cams / sizeof cams[0]); ci++) {
        const Camera &cam = cams[ci];
        const int w = (ci & 1) ? 640 : 320, h = (ci & 1) ? 480 : 200;
        std::vector<uint8_t> buf((size_t)w * h);
        FrameBuffer fb{buf.data(), w, h};
        render_set_view_window(fb, 0x28);
        render_landscape(cam);
        std::vector<VertexRec> V(g_work_vertices(), g_work_vertices() + MC_GRID_CELLS);
        const unsigned yaw = (unsigned)cam.yaw & 0x7ff;
        const QuadStep &qs = mc_quad_steps()[((yaw + 0x100) >> 9) & 3];
        uint8_t cx = (uint8_t)((cam.cam_x >> 8) + qs.start_dx), cy = (uint8_t)((cam.cam_y >> 8) + qs.start_dy);
        for (int r = 0; r < MC_GRID_ROWS; r++) {
            for (int c = 0; c < MC_GRID_COLS; c++) {
                const VertexRec &rec = V[r * MC_GRID_COLS + c];
                if (!(rec.flags & 2) && rec.z_cam >= 0x200) {
                    Thing &t = g_state->things[0];
                    std::memset(&t, 0, sizeof t);
                    t.x = (uint16_t)(cx << 8);
                    t.y = (uint16_t)(cy << 8);
                    t.z = (int16_t)(rec.h_rel + cam.cam_z);
                    t.sprite = scenery_sprite;
                    VertexRec cell{};
                    cell.tex_prop = 1;                          // no shadow
                    s_probe_hits = 0;
                    render_cell_things(0, &cell);
                    if (s_probe_hits == 1) {
                        const int ex = std::abs(s_probe_x - rec.sx), ey = std::abs(s_probe_y - rec.sy);
                        const int e = ex > ey ? ex : ey;
                        // The two paths round the camera-space x / z differently by up to one unit
                        // (1/256 cell); on screen that is (focal + distance from the centre) / z pixels.
                        const int reach = std::abs(rec.sx - g_rcam.screen_cx) + std::abs(rec.sy - g_rcam.screen_cy) + g_rcam.focal;
                        const int tol = 2 + 2 * reach / rec.z_cam;
                        const bool on_screen = rec.sx >= 0 && rec.sx < w && rec.sy >= 0 && rec.sy < h;
                        if (on_screen && e > worst) worst = e;
                        if (e > tol && g_fail < 20)
                            std::printf("  camera %d vertex (%d,%d) cell (%d,%d): thing anchored at (%d,%d), vertex at (%d,%d), z %d\n",
                                        ci, r, c, cx, cy, s_probe_x, s_probe_y, rec.sx, rec.sy, rec.z_cam);
                        CHECK(e <= tol);
                        if (on_screen) on_screen_count++;
                        checked++;
                    } else {
                        culled++;
                    }
                }
                cx = (uint8_t)(cx + qs.col_dx);
                cy = (uint8_t)(cy + qs.col_dy);
            }
            cx = (uint8_t)(cx + qs.row_dx);
            cy = (uint8_t)(cy + qs.row_dy);
        }
    }
    g_state->things[0] = saved;
    g_sprite_blit_probe = nullptr;
    render_things_install();
    std::printf("anchor vs terrain vertex: %d vertices in 6 views (%d on screen, worst on-screen difference %d px), %d not drawn (cull limits)\n",
                checked, on_screen_count, worst, culled);
    CHECK(checked > 2000 && on_screen_count > 300 && worst <= 3);
}

static void test_frames() {
    // inventory of what the snapshot asks the renderer to draw
    int live = 0, hidden = 0, by_type[0x25] = {}, shade_groups[6] = {};
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing &t = g_state->things[i];
        if (!t.cls || !(t.flags & 4)) continue;
        live++;
        if (t.flags & 0x21) { hidden++; continue; }
        const SpriteDesc &d = *mc_sprite_desc(t.sprite);
        if (d.draw_type < 0x25) by_type[d.draw_type]++;
        if (d.shade_group < 6) shade_groups[d.shade_group]++;
    }
    std::printf("snapshot: %d linked things, %d hidden (flags & 0x21); draw types:", live, hidden);
    for (int i = 0; i < 0x25; i++) if (by_type[i]) std::printf(" 0x%x:%d", i, by_type[i]);
    std::printf("; shade groups 0:%d 2:%d 3:%d\n", shade_groups[0], shade_groups[2], shade_groups[3]);
    std::printf("snapshot options: shadows %d second surface %d sky %d view size 0x%x block %d, local player tick %d\n",
                g_state->opt_shadows, g_state->opt_second_surface, g_state->opt_textured_sky, g_state->view_size,
                g_state->texture_block_size, g_state->players[g_state->local_player & 7].tick);
    CHECK(live > 400);

    // viewpoints from the data: the densest neighbourhood, a group of creatures, a thing near water
    int best_dense = 0, dense = 0, best_cre = -1, cre = 0, best_water = -1, water = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing &t = g_state->things[i];
        if (!t.cls || !(t.flags & 4) || (t.flags & 0x21)) continue;
        int near_all = 0, near_cre = 0;
        for (int j = 1; j < MC_THING_SLOTS; j++) {
            const Thing &u = g_state->things[j];
            if (!u.cls || !(u.flags & 4) || (u.flags & 0x21)) continue;
            const int dd = cell_dist(t, u);
            if (dd <= 5) near_all++;
            if (dd <= 4 && u.cls == 5) near_cre++;
        }
        if (near_all > best_dense) { best_dense = near_all; dense = i; }
        if (t.cls == 5 && near_cre > best_cre) { best_cre = near_cre; cre = i; }
        int wet = 0;
        for (int dy = -4; dy <= 4; dy++)
            for (int dx = -4; dx <= 4; dx++) {
                const unsigned tex = g_map_type[mc_cell((t.x >> 8) + dx, (t.y >> 8) + dy)];
                if (tex < 164 && g_tex_prop_water[tex]) wet++;
            }
        // near sea level (the reflection plane is z = 0 and the pass needs cam_z < 0x1000), some land around
        if (wet >= 12 && terrain_sample_height(t.x, t.y) < 0x100 && near_all > best_water) { best_water = near_all; water = i; }
    }
    const Thing &td = g_state->things[dense], &tc = g_state->things[cre], &tw = g_state->things[water];
    std::printf("viewpoints: dense thing %d at (%d,%d) with %d neighbours; creature %d at (%d,%d) with %d creatures near; water thing %d at (%d,%d)\n",
                dense, (int)td.x, (int)td.y, best_dense, cre, (int)tc.x, (int)tc.y, best_cre, water, (int)tw.x, (int)tw.y);
    CHECK(dense != 0 && cre != 0 && water != 0);

    g_state->view_size = 0x28;
    g_state->opt_textured_sky = 1;
    g_state->opt_shadows = 1;
    g_state->opt_second_surface = 1;
    g_cfg->tick = 413;
    g_anim_tick = 413;
    g_sprite_oob_reads = g_sprite_oob_writes = 0;

    test_anchor_vs_terrain(td.sprite);
    g_sprite_oob_reads = g_sprite_oob_writes = 0;

    // the demo's first camera (movie/gam00000.dat, see tests/engine_test.cpp)
    const Camera demo{640, 30080, 0, 0x100 + 0x80, 0, 0, 0x80};
    FrameStats st = render_frame("demo_320", 320, 200, demo);
    CHECK(st.ok && st.calls > 0 && st.sprite_pixels > 200);
    const int demo_pixels = st.sprite_pixels;
    st = render_frame("demo_640", 640, 480, demo);
    CHECK(st.ok && st.sprite_pixels > 2 * demo_pixels);     // about 4.8x the area

    // shadows off: fewer pixels change; LRU stamps were written with the frame counter
    g_state->opt_shadows = 0;
    st = render_frame("demo_320_noshadow", 320, 200, demo);
    CHECK(st.ok && st.sprite_pixels > 0 && st.sprite_pixels < demo_pixels);
    g_state->opt_shadows = 1;
    int stamped = 0, drawn_flag = 0;
    for (int i = 0; i < MC_SPRITE_COUNT; i++) {
        stamped += g_sprite_group_stamp[i] == 413 && sprite_group_of(i) == (unsigned)i && g_sprite_ptr[i];
        drawn_flag += g_sprite_ptr[i] && (g_sprite_ptr[i][0] & 8);
    }
    std::printf("after the demo frames: %d sprites loaded, %d groups stamped with tick 413, %d sprites flagged as drawn\n",
                count_loaded(), stamped, drawn_flag);
    CHECK(stamped > 0 && drawn_flag > 0);
    texture_anim_update();
    for (int i = 0; i < MC_SPRITE_COUNT; i++) CHECK(!g_sprite_ptr[i] || !(g_sprite_ptr[i][0] & 8));

    // the densest group of things from four sides, level and rolled
    st = render_frame("dense_yaw000", 320, 200, look_at(td.x, td.y, 0, 0x480, 0x140, 0x18, 0));
    CHECK(st.ok && st.sprite_pixels > 300);
    st = render_frame("dense_yaw200_roll", 320, 200, look_at(td.x, td.y, 0x200, 0x480, 0x140, 0x18, 0x50));
    CHECK(st.ok && st.sprite_pixels > 300);
    st = render_frame("dense_yaw400_640", 640, 480, look_at(td.x, td.y, 0x400, 0x500, 0x180, 0x20, 0x7c0));
    CHECK(st.ok && st.sprite_pixels > 1000);
    st = render_frame("dense_yaw600_roll180", 320, 200, look_at(td.x, td.y, 0x600, 0x480, 0x140, 0x18, 0x180));
    CHECK(st.ok && st.sprite_pixels > 300);
    st = render_frame("dense_close", 320, 200, look_at(td.x, td.y, 0x100, 0x180, 0x60, 0, 0));
    CHECK(st.ok && st.sprite_pixels > 300);

    // creatures (direction-dependent sprites)
    st = render_frame("creatures", 320, 200, look_at(tc.x, tc.y, 0x700, 0x300, tc.z - terrain_sample_height(tc.x, tc.y) + 0x60, 0x10, 0));
    CHECK(st.ok && st.sprite_pixels > 100);
    st = render_frame("creatures_640_roll", 640, 480, look_at(tc.x, tc.y, 0x300, 0x300, tc.z - terrain_sample_height(tc.x, tc.y) + 0x60, 0x10, 0x30));
    CHECK(st.ok && st.sprite_pixels > 300);

    // low over water: the mirrored pass draws the reflections first
    const Camera wcam = look_at(tw.x, tw.y, 0x80, 0x380, 0x90, 0x14, 0);
    st = render_frame("water_reflection", 320, 200, wcam);
    CHECK(st.ok && st.calls_mirror > 0 && st.sprite_pixels > 100);
    const int with_mirror = st.sprite_pixels;
    st = render_frame("water_no_reflection", 320, 200, wcam, false);
    CHECK(st.ok);
    std::printf("water view: %d pixels changed with reflections, %d without\n", with_mirror, st.sprite_pixels);
    st = render_frame("water_640_roll", 640, 480, look_at(tw.x, tw.y, 0x480, 0x380, 0x90, 0x14, 0x7d8));
    CHECK(st.ok && st.calls_mirror > 0);
    // the same things from 16 cells away across the water: fog band (15..19 cells), shaded pixel mode
    st = render_frame("water_far_fog", 640, 480, look_at(tw.x, tw.y, 0x700, 0x1000, 0x200, 0x10, 0));
    CHECK(st.ok && st.sprite_pixels > 1000);

    // every octant of the roll table through the real renderer, and a sweep around the dense spot
    for (int k = 0; k < 16; k++) {
        const Camera c = look_at(td.x, td.y, k * 0x80, 0x400, 0x120, 0x10, k * 0x80 + 0x23);
        std::vector<uint8_t> buf(320 * 200 + 2 * GUARD, 0xAB);
        FrameBuffer fb{buf.data() + GUARD, 320, 200};
        render_set_view_window(fb, 0x28);
        render_landscape(c);
        bool ok = true;
        for (int i = 0; i < GUARD; i++) ok = ok && buf[i] == 0xAB && buf[GUARD + 320 * 200 + i] == 0xAB;
        CHECK(ok);
        if (k == 5) write_ppm("sprites_test_sweep_roll2a3.ppm", fb.pixels, 320, 200);
    }
    // reduced view window: nothing outside the window may change
    {
        std::vector<uint8_t> buf(320 * 200, 0xCD);
        FrameBuffer fb{buf.data(), 320, 200};
        render_set_view_window(fb, 0x14);                    // 160 x 100 window at (80, 50)
        render_landscape(look_at(td.x, td.y, 0, 0x300, 0x100, 0x10, 0x40));
        int outside = 0;
        for (int y = 0; y < 200; y++)
            for (int x = 0; x < 320; x++)
                if (!(x >= 80 && x < 240 && y >= 50 && y < 150) && buf[y * 320 + x] != 0xCD) outside++;
        std::printf("reduced view window: %d pixels changed outside the window\n", outside);
        CHECK(outside == 0);
    }
    std::printf("frames: refused reads %u, refused writes %u\n", g_sprite_oob_reads, g_sprite_oob_writes);
    CHECK(g_sprite_oob_reads == 0 && g_sprite_oob_writes == 0);
}

int main(int argc, char **argv) {
    const char *game = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    setvbuf(stdout, nullptr, _IONBF, 0);
    mc_install_crash_handler();
    mc_globals_init();
    CHECK(tables_load_palette(game));
    CHECK(tables_load_textures(game));
    CHECK(tables_load_or_generate(game));
    CHECK(tables_load_sky(game));
    CHECK(sprite_table_init_sizes(game));
    CHECK(sprites_init(game));
    render_things_install();
    if (g_fail) { std::printf("sprites_test: init FAILED\n"); return 1; }

    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++) s_sprite[y * SW + x] = (uint8_t)(1 + (x >= SW / 2) + 2 * (y >= SH / 2));

    test_cache();
    test_anim();
    test_blit_exact();
    test_blit_rolled();
    test_blit_crop();
    test_blit_stress();

    CHECK(load_snapshot(game));
    if (!g_fail) test_frames();

    sprites_shutdown();
    if (g_fail) { std::printf("sprites_test: %d FAILED\n", g_fail); return 1; }
    std::printf("sprites_test: OK\n");
    return 0;
}
