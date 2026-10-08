// Round 11 task B: the Thing core, the spatial index, effects and castles in a wide world (world.h).
// ${MC_SIM_ALL}, no renderer. Level 38 is loaded and repeated as a 1024-cell world (world_widen_tiled):
//  1. terrain probes of thing.cpp / spatial.cpp / effects.cpp at (x + k*65536, y + m*65536) give what the
//     256 world gives at (x, y) (samples include both seams: x = 65535 -> 65536 and 0x3ffff -> 0);
//  2. collision / mana searches, area damage (slot 0 and slot 1), thing_exists_near_pos and cell_kill_things
//     across both seams hit the Thing next to the searcher and not its 256-world alias 256 cells away;
//  3. distances / angles 300..600 cells apart (int64 squares, no torus alias), castle_find_free_mana_ball;
//  4. a castle raise (effect 0x2a) and a ground levelling (effect 0x29) straddling x = 1023 -> 0 / y = 1023 -> 0
//     and the old 255 -> 256 boundary change exactly the cells (and to exactly the values) the same effect
//     changes in the 256 world around cell (0, 0);
//  5. mana balls rolling east / west and an earthquake crack crossing both seams keep a continuous track,
//     identical step by step to the same Thing at another tile boundary;
//  6. 3000 ticks of the tiled world, twice in one process: identical net_state_checksum sequences; the cell
//     lists stay consistent.
// argv[1] = game dir. Exit 0 = pass.
#include "sim.h"
#include "thing.h"
#include "spatial.h"
#include "effects.h"
#include "castle.h"
#include "player.h"
#include "net.h"
#include "mc_math.h"
#include "mc_globals.h"
#include "crash_handler.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_fail++; } } while (0)

static constexpr int N = 1024;
static constexpr int32_t U = N * 256;            // world units per side
static constexpr int32_t ROW2 = 2 * 65536;       // a tile row without Things (they stay in the first copy)

// ---- 1. terrain probes ---------------------------------------------------------------------------------

struct Probe {
    int32_t x, y;
    int h, corner, drop, slope0, slope1;
    uint32_t type_mask, flag_bit;
    unsigned ring_cx, ring_cy;
};

static Probe probe_at(int32_t x, int32_t y) {
    Probe p{};
    p.x = x; p.y = y;
    Pos pos;
    pos.x = x; pos.y = y; pos.z = 0;
    p.h = terrain_height_at(&pos);
    p.corner = terrain_max_corner_level(&pos);
    p.drop = terrain_max_drop_around(&pos);
    int16_t s[2];
    terrain_slope_vector(&pos, s);
    p.slope0 = s[0]; p.slope1 = s[1];
    p.type_mask = terrain_type_mask_at(&pos);
    p.flag_bit = terrain_cell_flag_bit(&pos);
    const unsigned c = terrain_ring_find_height_ne8(world_cell_of(x, y), 1);
    p.ring_cx = (unsigned)world_cell_x(c) & 0xff;
    p.ring_cy = (unsigned)world_cell_y(c) & 0xff;
    return p;
}

static std::vector<Probe> probes_256() {
    std::vector<Probe> v;
    const int32_t edge[] = {0, 1, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xfe80, 0xff00, 0xff7f, 0xff80, 0xffff};
    for (int32_t x : edge)
        for (int32_t y : edge) v.push_back(probe_at(x, y));
    uint32_t r = 12345;
    for (int i = 0; i < 400; i++) {
        r = mc_lcg(r);
        const int32_t x = (int32_t)(r & 0xffff);
        r = mc_lcg(r);
        v.push_back(probe_at(x, (int32_t)(r & 0xffff)));
    }
    return v;
}

static void check_probes(const std::vector<Probe> &ref) {
    int bad = 0, n = 0;
    for (int k = 0; k < N / 256; k++)
        for (int m = 0; m < N / 256; m++)
            for (const Probe &p : ref) {
                const Probe q = probe_at(p.x + k * 65536, p.y + m * 65536);
                n++;
                if (q.h != p.h || q.corner != p.corner || q.drop != p.drop || q.slope0 != p.slope0 || q.slope1 != p.slope1 ||
                    q.type_mask != p.type_mask || q.flag_bit != p.flag_bit || q.ring_cx != p.ring_cx || q.ring_cy != p.ring_cy) {
                    if (bad++ < 5)
                        std::printf("  probe (%x, %x) tile (%d, %d): h %d/%d corner %d/%d drop %d/%d ring %u,%u/%u,%u\n", p.x, p.y, k, m,
                                    q.h, p.h, q.corner, p.corner, q.drop, p.drop, q.ring_cx, q.ring_cy, p.ring_cx, p.ring_cy);
                }
            }
    std::printf("1. terrain probes: %d samples x %d tiles = %d, %d differ\n", (int)ref.size(), (N / 256) * (N / 256), n, bad);
    CHECK_EQ(bad, 0);
}

// ---- 2. spatial queries ----------------------------------------------------------------------------------

static Thing *make_thing(int32_t x, int32_t y, int cls, int type, int ext) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->cls = (uint8_t)cls;
    t->type = (uint8_t)type;
    t->state = 0;
    t->prop_flags = 3;
    thing_set_extents(t, ext, 0x100);
    Pos p;
    p.x = x; p.y = y; p.z = 0;
    p.z = (int16_t)terrain_height_at(&p);
    thing_link_cell(t, &p);
    return t;
}
static void kill_thing(Thing *t) { if (t) thing_free(t); }

static bool in_cell_list(const Thing *t) {
    const uint32_t c = world_cell_of(t->x, t->y);
    int guard = 0;
    for (unsigned i = g_cell_things[c]; i != 0 && guard < thing_pool_slots(); i = thing_at(i)->cell_next, guard++)
        if (thing_at(i) == t) return true;
    return false;
}

// One searcher A at (ax, y), a victim V 0x80 east of it across the seam, and the decoy D at V's 256-world
// alias (V + 65536: the same place in a 256 world, 256 cells away here).
static void spatial_case(const char *what, int32_t ax, int32_t y) {
    const int32_t vx = world_wrap(ax + 0x80);
    Thing *A = make_thing(ax, y, 10, 0x30, 0x80);
    Thing *V = make_thing(vx, y, 5, 1, 0x80);
    Thing *D = make_thing(world_wrap(vx + 65536), y, 5, 1, 0x80);
    CHECK(A && V && D);
    if (!A || !V || !D) return;
    CHECK(in_cell_list(V) && in_cell_list(D));
    CHECK_EQ(world_cx(V->x), world_cx(vx));
    CHECK(thing_collide(A, V) != 0);
    CHECK(thing_collide(A, D) == 0);
    CHECK(thing_find_collision(A) == V);
    CHECK(thing_find_collision_other_owner(A) == V);
    // area damage slot 0 and slot 1
    V->damage_slots[0] = DamageSlot{0, 0};
    D->damage_slots[0] = DamageSlot{0, 0};
    thing_area_damage(A, 0, 77);
    CHECK_EQ(V->damage_slots[0].amount, 77);
    CHECK_EQ(V->damage_slots[0].attacker, A->owner);
    CHECK_EQ(D->damage_slots[0].attacker, 0);
    V->damage_slots[1] = DamageSlot{0, 0};
    thing_area_damage(A, 1, 55);
    CHECK_EQ(V->damage_slots[1].amount, 55);
    CHECK_EQ(D->damage_slots[1].attacker, 0);
    thing_area_damage_quake(A, 0, 11);
    CHECK_EQ(V->damage_slots[0].amount, 77 + 11);
    CHECK_EQ(D->damage_slots[0].attacker, 0);
    // a mana ball (effect 0x27) the searcher may take, across the seam
    Thing *M = make_thing(world_wrap(ax - 0x60), y, 10, 0x27, 0x40);
    CHECK(M != nullptr);
    if (M) {
        CHECK(thing_find_mana_ball_touching(A) == M);
        Pos mp = *thing_pos(M);
        CHECK_EQ(thing_exists_near_pos(&mp, 10, 0x27), 1);
        Pos alias = mp;
        alias.x = mp.x + 65536;
        CHECK_EQ(thing_exists_near_pos(&alias, 10, 0x27), 0);
        kill_thing(M);
    }
    // cell_kill_things on V's cell (a creature of another owner: health -1); D untouched
    V->health = 100; D->health = 100;
    cell_kill_things(world_cell_of(V->x, V->y), A->owner);
    CHECK_EQ(V->health, -1);
    CHECK_EQ(D->health, 100);
    std::printf("2. spatial %-24s A x=%06x V x=%06x: collision / area damage / mana / cell kill ok\n", what, ax, vx);
    kill_thing(A); kill_thing(V); kill_thing(D);
}

// ---- 3. distances and angles ---------------------------------------------------------------------------

static void distances() {
    Pos a;
    a.x = 0x180; a.y = ROW2 + 0x80; a.z = 0;
    struct { int cells; int32_t dist; int angle; } cases[] = {
        {300, 300 * 256, 0x200}, {500, 500 * 256, 0x200}, {511, 511 * 256, 0x200}, {513, 511 * 256, 0x600},
        {600, 424 * 256, 0x600}, {-300, 300 * 256, 0x600}, {-500, 500 * 256, 0x600},
    };
    for (const auto &c : cases) {
        Pos b = a;
        b.x = a.x + c.cells * 256;
        CHECK_EQ(pos_dist_xy(&a, &b), c.dist);
        CHECK_EQ(pos_dist_sq_xy(&a, &b), (int64_t)c.dist * c.dist);
        CHECK_EQ(pos_dist_xyz(&a, &b), c.dist);
        CHECK_EQ(pos_angle_to(&a, &b), c.angle);
        CHECK_EQ(pos_dist_chebyshev_xy(&a, &b), c.dist);
        Pos by = a;                                       // the same along y
        by.y = a.y + c.cells * 256;
        CHECK_EQ(pos_dist_xy(&a, &by), c.dist);
        CHECK_EQ(pos_angle_to(&a, &by), (c.angle == 0x200) ? 0x400 : 0);
    }
    // 500 cells diagonally: sqrt(2) * 128000 = 181019
    Pos d = a;
    d.x = a.x + 500 * 256; d.y = a.y + 500 * 256;
    CHECK_EQ(pos_dist_xy(&a, &d), 181019);
    CHECK_EQ(pos_angle_to(&a, &d), 0x300);
    // nearest own mana ball: 300 cells beats 450, a far alias does not win
    Thing *from = make_thing(a.x, a.y, 3, 3, 0x80);
    Thing *b450 = make_thing(world_wrap(a.x + 450 * 256), a.y, 10, 0x27, 0x40);
    Thing *b300 = make_thing(world_wrap(a.x - 300 * 256), a.y, 10, 0x27, 0x40);
    CHECK(from && b450 && b300);
    if (from && b450 && b300) {
        b450->mana_owner = from->owner;
        b300->mana_owner = from->owner;
        const uint32_t saved = g_cfg->mana_ball_list;
        g_cfg->mana_ball_list = thing_index(b450);
        b450->next = thing_index(b300);
        b300->next = 0;
        CHECK(castle_find_free_mana_ball(from, nullptr, nullptr) == b300);
        g_cfg->mana_ball_list = saved;
    }
    kill_thing(from); kill_thing(b450); kill_thing(b300);
    std::printf("3. distances / angles 300..600 cells: ok\n");
}

// ---- 4. castle raise / ground levelling across the seams ---------------------------------------------------

struct Window { std::vector<uint8_t> h, t, f; };
static constexpr int W = 24;

static Window window_at(int cx, int cy) {
    Window w;
    for (int dy = -W; dy <= W; dy++)
        for (int dx = -W; dx <= W; dx++) {
            const uint32_t c = world_cell(cx + dx, cy + dy);
            w.h.push_back(g_map_height[c]);
            w.t.push_back(g_map_type[c]);
            w.f.push_back(g_map_flags[c]);
        }
    return w;
}

// Runs the castle-building effect `type` (0x2a raise, 0x29 level) of castle size 3 centred on cell (cx, cy)
// to completion; returns the window around it.
static Window run_castle_effect(int type, int cx, int cy) {
    g_rng16 = 0x1234;
    Thing *caster = thing_alloc();                        // a stand-in castle: duration 0
    Pos p;
    p.x = cx * 256 + 0x10; p.y = cy * 256 + 0x10; p.z = 0;
    p.z = (int16_t)(terrain_height_at(&p) & ~0x1f);
    ThingCreateFn create = thing_create_fn(10, type);
    Thing *e = create ? create(&p) : nullptr;
    CHECK(caster && e);
    if (e && caster) {
        e->castle_size = 3;
        e->caster = thing_index(caster);
        e->owner = thing_index(caster);
        ThingUpdateFn fn = thing_update_fn(10, e->state);
        CHECK(fn != nullptr);
        for (int guard = 0; fn && !(e->flags & 0x400) && guard < 200; guard++) fn(e);
        CHECK(e->flags & 0x400);
    }
    if (e) thing_free(e);
    if (caster) thing_free(caster);
    return window_at(cx, cy);
}

static int window_diff(const Window &a, const Window &b) {
    int n = 0;
    for (size_t i = 0; i < a.h.size(); i++) n += a.h[i] != b.h[i] || a.t[i] != b.t[i] || a.f[i] != b.f[i];
    return n;
}
static int window_changed(const Window &before, const Window &after) { return window_diff(before, after); }

struct Maps { std::vector<uint8_t> t, h, l, f; };
static Maps save_maps() {
    const size_t n = g_world.cells;
    return Maps{std::vector<uint8_t>(g_map_type, g_map_type + n), std::vector<uint8_t>(g_map_height, g_map_height + n),
                std::vector<uint8_t>(g_map_light, g_map_light + n), std::vector<uint8_t>(g_map_flags, g_map_flags + n)};
}
static void restore_maps(const Maps &m) {
    std::memcpy(g_map_type, m.t.data(), m.t.size());
    std::memcpy(g_map_height, m.h.data(), m.h.size());
    std::memcpy(g_map_light, m.l.data(), m.l.size());
    std::memcpy(g_map_flags, m.f.data(), m.f.size());
}

struct CastleRef { Window before, raise, level; };
static CastleRef castle_ref_256() {
    CastleRef r;
    const Maps m = save_maps();
    r.before = window_at(0, 0);
    r.raise = run_castle_effect(0x2a, 0, 0);
    restore_maps(m);
    r.level = run_castle_effect(0x29, 0, 0);
    restore_maps(m);
    std::printf("4. 256 world, castle at cell (0, 0): raise changes %d cells, levelling %d cells (window %dx%d)\n",
                window_changed(r.before, r.raise), window_changed(r.before, r.level), 2 * W + 1, 2 * W + 1);
    CHECK(window_changed(r.before, r.raise) > 20);
    return r;
}

static void castle_wide(const CastleRef &ref) {
    const Maps m = save_maps();
    const int centres[][2] = {{0, 0}, {256, 256}, {512, 0}, {768, 512}, {0, 768}};
    // The levelling ends in terrain_smooth_castle_border, whose terrain_smooth_cell reads its west / north-west
    // neighbours at a *linear* index (level_features.cpp, the original's quirk): at the world's x = 0 column
    // "west" is the previous row's last cell. So the 256 result at cell (0, 0) is the oracle for the centres on
    // the x seam, and the centres on an old 255 -> 256 boundary (no quirk there) must agree with each other.
    Window level_inner;
    bool have_inner = false;
    for (const auto &c : centres) {
        const Window before = window_at(c[0], c[1]);
        CHECK_EQ(window_diff(before, ref.before), 0);
        const Window raise = run_castle_effect(0x2a, c[0], c[1]);
        const int dr = window_diff(raise, ref.raise);
        // nothing outside the window changed: compare the whole map with the saved one minus the window
        int outside = 0;
        for (uint32_t cell = 0; cell < g_world.cells; cell++) {
            const int dx = world_signed((world_cell_x(cell) - c[0]) * 256) / 256, dy = world_signed((world_cell_y(cell) - c[1]) * 256) / 256;
            if (dx >= -W && dx <= W && dy >= -W && dy <= W) continue;
            outside += g_map_height[cell] != m.h[cell] || g_map_type[cell] != m.t[cell] || g_map_flags[cell] != m.f[cell];
        }
        restore_maps(m);
        const Window level = run_castle_effect(0x29, c[0], c[1]);
        int dl = 0;
        const char *vs = "the 256 result";
        if (c[0] == 0) {
            dl = window_diff(level, ref.level);
        } else {
            vs = "the other 255 -> 256 centres";
            if (!have_inner) { level_inner = level; have_inner = true; }
            dl = window_diff(level, level_inner);
            CHECK(window_changed(before, level) > 20);
        }
        restore_maps(m);
        std::printf("4. 1024 world, castle at cell (%3d, %3d): raise %d cells differ from the 256 result (%d changed outside), levelling %d differ from %s\n",
                    c[0], c[1], dr, outside, dl, vs);
        CHECK_EQ(dr, 0);
        CHECK_EQ(outside, 0);
        CHECK_EQ(dl, 0);
    }
}

// ---- 5. continuous tracks across the seams ----------------------------------------------------------------

struct Step { int32_t dx, dy; int z; };

// A mana ball at (x, y) with velocity (vx, 0) rolls for `ticks` updates; returns the per-tick steps.
static std::vector<Step> roll_ball(int32_t x, int32_t y, int vx, int ticks, int32_t *crossed) {
    std::vector<Step> v;
    Pos p;
    p.x = x; p.y = y; p.z = 0;
    p.z = (int16_t)(terrain_height_at(&p) + 0x100);
    ThingCreateFn create = thing_create_fn(10, 0x27);
    Thing *b = create ? create(&p) : nullptr;
    CHECK(b != nullptr);
    if (!b) return v;
    b->rng = 0x5555;
    b->home.x = (uint16_t)vx;
    b->home.y = 0;
    b->timer_a = 0x80;
    ThingUpdateFn fn = thing_update_fn(10, b->state);
    *crossed = 0;
    for (int i = 0; i < ticks && fn; i++) {
        const int32_t ox = b->x, oy = b->y;
        fn(b);
        const Step s{world_signed(b->x - ox), world_signed(b->y - oy), b->z};
        v.push_back(s);
        if ((ox > b->x && s.dx > 0) || (ox < b->x && s.dx < 0)) (*crossed)++;   // wrapped (seam crossing)
        if (!in_cell_list(b)) { std::printf("  ball not in its cell list at tick %d\n", i); g_fail++; break; }
    }
    thing_free(b);
    return v;
}

static void tracks() {
    // east: start 0x180 west of the seam / the old boundary / another tile boundary, velocity +0x30
    const int32_t ys = ROW2 + 0x5080;
    const int32_t starts[] = {U - 0x180, 65536 - 0x180, 3 * 65536 - 0x180};
    const int32_t wstarts[] = {0x180, 65536 + 0x180, 2 * 65536 + 0x180};
    std::vector<Step> ref_e, ref_w;
    for (int i = 0; i < 3; i++) {
        int32_t ce = 0, cw = 0;
        std::vector<Step> e = roll_ball(starts[i], ys, 0x30, 40, &ce);
        std::vector<Step> w = roll_ball(wstarts[i], ys, -0x30, 40, &cw);
        int32_t sum_e = 0, sum_w = 0, max_e = 0, max_w = 0;
        for (const Step &s : e) { sum_e += s.dx; max_e = s.dx > max_e ? s.dx : max_e; }
        for (const Step &s : w) { sum_w += s.dx; max_w = -s.dx > max_w ? -s.dx : max_w; }
        std::printf("5. mana ball from x=%06x east: %d steps, moved %+d (max step %d, %d wraps); from x=%06x west: moved %+d (max step %d, %d wraps)\n",
                    starts[i], (int)e.size(), sum_e, max_e, ce, wstarts[i], sum_w, max_w, cw);
        CHECK(sum_e > 0x180 && max_e <= 0x40);
        CHECK(sum_w < -0x180 && max_w <= 0x40);
        if (i == 0) { CHECK_EQ(ce, 1); CHECK_EQ(cw, 1); ref_e = e; ref_w = w; }
        else {
            bool same = e.size() == ref_e.size() && w.size() == ref_w.size();
            for (size_t k = 0; same && k < e.size(); k++) same = e[k].dx == ref_e[k].dx && e[k].dy == ref_e[k].dy && e[k].z == ref_e[k].z;
            for (size_t k = 0; same && k < w.size(); k++) same = w[k].dx == ref_w[k].dx && w[k].dy == ref_w[k].dy && w[k].z == ref_w[k].z;
            CHECK(same);
        }
    }
    // earthquake crack heading east across the seam (yaw 0x200), the same RNG at three tile boundaries
    std::vector<Step> ref;
    for (int i = 0; i < 3; i++) {
        const uint32_t saved_rng = g_state->rng;
        Pos p;
        p.x = starts[i] - 0x400; p.y = ys; p.z = 0;
        ThingCreateFn create = thing_create_fn(10, 0xf);
        Thing *q = create ? create(&p) : nullptr;
        CHECK(q != nullptr);
        if (!q) continue;
        q->rng = 0x777;
        q->yaw = 0x200;
        std::vector<char> live((size_t)thing_pool_slots());
        for (int k = 1; k < thing_pool_slots(); k++) live[(size_t)k] = thing_at(k)->cls != 0;
        ThingUpdateFn fn = thing_update_fn(10, q->state);
        std::vector<Step> track;
        int wraps = 0, craters = 0;
        for (int k = 0; k < 24 && fn && !(q->flags & 0x400); k++) {
            const int32_t ox = q->x, oy = q->y;
            fn(q);
            const Step s{world_signed(q->x - ox), world_signed(q->y - oy), q->yaw};
            track.push_back(s);
            if (ox > q->x && s.dx > 0) wraps++;
            CHECK(s.dx * s.dx + s.dy * s.dy <= 0x101 * 0x101 && s.dx * s.dx + s.dy * s.dy >= 0xfe * 0xfe);
            for (int j = thing_pool_slots() - 1; j >= 1; j--) {        // the craters it leaves: one per step, on the crack
                Thing *c = thing_at(j);
                if (c->cls == 0 || live[(size_t)j] || c == q) continue;
                craters++;
                CHECK_EQ(c->x, q->x);
                CHECK_EQ(c->y, q->y);
                thing_free(c);
            }
        }
        std::printf("5. earthquake from x=%06x: %d steps, %d craters, %d wraps, ends at x=%06x\n", (int32_t)p.x, (int)track.size(), craters, wraps, (int32_t)q->x);
        CHECK(track.size() >= 8);
        if (i == 0) { CHECK_EQ(wraps, 1); ref = track; }
        else {
            bool same = track.size() == ref.size();
            for (size_t k = 0; same && k < track.size(); k++) same = track[k].dx == ref[k].dx && track[k].dy == ref[k].dy && track[k].z == ref[k].z;
            CHECK(same);
        }
        thing_free(q);
        g_state->rng = saved_rng;
    }
}

// ---- 6. the tiled world for a few thousand ticks -------------------------------------------------------------

static int cell_list_errors() {
    int bad = 0;
    for (int i = 1; i < thing_pool_slots(); i++) {
        const Thing *t = thing_at(i);
        if (t->cls == 0 || !(t->flags & 4)) continue;
        if (t->cls == 10 && t->type == 0xf) continue;    // the earthquake moves without a relink (original)
        if (!in_cell_list(t)) bad++;
    }
    return bad;
}

static std::vector<uint32_t> run_ticks(int ticks, int *beyond, int *bad_links) {
    std::vector<uint32_t> sums;
    *beyond = 0;
    CHECK(sim_load_level(38));
    CHECK(world_widen_tiled(N));
    for (int i = 0; i < ticks; i++) {
        game_tick_sim();
        sums.push_back(net_state_checksum());
    }
    for (int i = 1; i < thing_pool_slots(); i++) {
        const Thing *t = thing_at(i);
        if (t->cls != 0 && (t->x >= 65536 || t->y >= 65536)) (*beyond)++;
    }
    *bad_links = cell_list_errors();
    return sums;
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    const char *game = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    if (!sim_init(game)) { std::printf("SKIP: no game data in %s\n", game); return 0; }
    sim_register_gameplay();
    if (!sim_load_level(38)) { std::printf("FAIL: level 38 did not load\n"); return 1; }
    CHECK_EQ(g_world.size, 256);

    const std::vector<Probe> ref = probes_256();
    const CastleRef cref = castle_ref_256();
    const int links_256 = cell_list_errors();

    CHECK(world_widen_tiled(N));
    CHECK_EQ(g_world.size, N);
    CHECK_EQ(cell_list_errors(), links_256);
    check_probes(ref);

    spatial_case("across x = 0x3ffff -> 0", U - 0x40, ROW2 + 0x4080);
    spatial_case("across x = 0xffff -> 0x10000", 65536 - 0x40, ROW2 + 0x4080);
    spatial_case("inside a tile", 2 * 65536 + 0x3000, ROW2 + 0x4080);
    distances();
    castle_wide(cref);
    tracks();

    const int ticks = 3000;
    int beyond1 = 0, beyond2 = 0, bad1 = 0, bad2 = 0;
    const std::vector<uint32_t> a = run_ticks(ticks, &beyond1, &bad1);
    const std::vector<uint32_t> b = run_ticks(ticks, &beyond2, &bad2);
    int first = -1;
    for (int i = 0; i < ticks && first < 0; i++) if (a[(size_t)i] != b[(size_t)i]) first = i;
    std::printf("6. %d ticks of the tiled world twice: checksums %s (last %08x / %08x), %d Things beyond the first tile, cell-list errors %d / %d (256 world at load: %d)\n",
                ticks, first < 0 ? "identical" : "DIFFER", a.back(), b.back(), beyond1, bad1, bad2, links_256);
    CHECK_EQ(first, -1);
    CHECK_EQ(beyond1, beyond2);
    CHECK_EQ(bad1, 0);

    std::printf("%s: %d failure(s)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
