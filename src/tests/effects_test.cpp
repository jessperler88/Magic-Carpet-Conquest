// Unit test for effects.cpp (class-10 effect handlers 0x2376f..0x284c0). argv[1] = game dir.
//
//  1. helpers and every handler by construction on level 38 (state transitions, timers, RNG draws
//     replayed with the LCG, damage slots, things spawned), expectations taken from the disassembly;
//  2. level 38 from its start, 412 ticks, against the engine's own snapshot (movie/gam00000.dat was
//     taken inside tick 413): the mana balls that are in both must agree field by field, and the
//     erupting crater (effect 0x12) must have made the same RNG draws;
//  3. the snapshot's 74 effects stepped with thing_update_all (invariants);
//  4. smoke runs of game_tick_sim on several levels with the dispatch report.
#include "sim.h"
#include "effects.h"
#include "level_features.h"
#include "terrain_paint.h"
#include "player.h"
#include "mc_math.h"
#include "mcfile.h"
#include "gen/dispatch_tables.h"
#include "gen/effects_tables.h"
#include "gen/constructors_tables.h"   // g_mana_ball_thresholds
#include "crash_handler.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_fail++; } } while (0)

// ---- small utilities -----------------------------------------------------------------------------

static bool deleted(const Thing *t) { return (t->flags & 0x400) != 0; }
static unsigned cell_xy(unsigned x, unsigned y) { return ((y & 0xff) << 8) | (x & 0xff); }

// One call of the thing's Table A handler, as thing_update_all makes it.
static void step(Thing *t) {
    ThingUpdateFn fn = thing_update_fn((int8_t)t->cls, (int8_t)t->state);
    CHECK(fn != nullptr);
    if (fn) fn(t);
    t->tick++;
}
// Steps until the thing is marked for deletion; returns the number of calls (0 = limit reached).
static int run_out(Thing *t, int limit = 100000) {
    for (int n = 1; n <= limit; n++) {
        step(t);
        if (deleted(t)) return n;
    }
    return 0;
}
// Frees everything that is marked for deletion (what thing_update_all does at the start of a tick).
static void sweep() {
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        Thing *t = thing_at(i);
        if (t->cls != 0 && deleted(t)) thing_free(t);
    }
}

// The set of live pool slots, to find the things a call created.
struct PoolMark {
    uint8_t cls[MC_THING_SLOTS];
    void take() { for (int i = 0; i < MC_THING_SLOTS; i++) cls[i] = thing_at(i)->cls; }
    std::vector<Thing *> fresh(int want_cls = -1, int want_type = -1) const {
        std::vector<Thing *> out;
        for (int i = 1; i < MC_THING_SLOTS; i++) {
            Thing *t = thing_at(i);
            if (cls[i] != 0 || t->cls == 0) continue;
            if (want_cls >= 0 && t->cls != want_cls) continue;
            if (want_type >= 0 && t->type != want_type) continue;
            out.push_back(t);
        }
        return out;
    }
};

// Number of draws that lead from seed `from` to `to` (-1 when more than `limit`).
static int lcg_steps(uint32_t from, uint32_t to, int limit = 100000) {
    for (int n = 0; n <= limit; n++) {
        if (from == to) return n;
        from = mc_lcg(from);
    }
    return -1;
}

static int spiral_cells(int r0, int r1) {
    SpiralSearch s;
    int dx, dy, n = 0;
    if (!spiral_search_begin(&s, r0, r1)) return 0;
    while (spiral_search_next(&s, &dx, &dy) == 1) n++;
    return n;
}

// A test site: an 11 x 11 block of cells without things or buildings, flattened to the height of
// its centre (so slope vectors are zero). Returns the centre of the centre cell on the ground.
static bool site_free(int cx, int cy, int r, bool want_water) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            unsigned c = cell_xy((unsigned)(cx + dx), (unsigned)(cy + dy));
            if (g_cell_things[c] != 0 || (g_map_flags[c] & 0x80)) return false;
            if (want_water ? g_map_height[c] != 0 : g_map_height[c] < 8) return false;
            uint8_t tex = g_map_type[c];
            if (tex >= 6 && tex <= 0x22) return false;
        }
    return true;
}
static int s_site_cursor[2] = {0, 0};      // [0] land, [1] water: index into a 13-cell grid (the map wraps)
static Pos make_site(bool want_water = false) {
    // land: a 13-cell grid; water (scarce on some levels): every 7th cell, a 7 x 7 block is enough
    const int stride = want_water ? 7 : 13, per_row = 256 / stride;
    for (int &n = s_site_cursor[want_water ? 1 : 0]; n < per_row * per_row; n++) {
        int cx = 6 + (n % per_row) * stride, cy = 6 + (n / per_row) * stride;
        if (!site_free(cx, cy, want_water ? 3 : 5, want_water)) continue;
        uint8_t h = g_map_height[cell_xy((unsigned)cx, (unsigned)cy)];
        for (int dy = -5; dy <= 5; dy++)
            for (int dx = -5; dx <= 5; dx++) g_map_height[cell_xy((unsigned)(cx + dx), (unsigned)(cy + dy))] = h;
        Pos p;
        p.x = (uint16_t)((cx << 8) | 0x80);
        p.y = (uint16_t)((cy << 8) | 0x80);
        p.z = 0;
        p.z = (int16_t)terrain_height_at(&p);
        n++;
        return p;
    }
    std::printf("FAIL: no %s test site left\n", want_water ? "water" : "land");
    g_fail++;
    return Pos{0x8080, 0x8080, 0};
}
// The cell an effect handler works on: (coordinate + 0x80) >> 8.
static unsigned fx_cell(const Pos *p) { return cell_xy((unsigned)(((int16_t)p->x + 0x80) >> 8), (unsigned)(((int16_t)p->y + 0x80) >> 8)); }

// Recorded sound requests.
struct SoundReq { int thing, player, sound; };
static std::vector<SoundReq> s_sounds;
static void record_sound(int thing, int player, int sound) { s_sounds.push_back({thing, player, sound}); }
static int sounds_of(int id) {
    int n = 0;
    for (const SoundReq &r : s_sounds) if (r.sound == id) n++;
    return n;
}

static Thing *local_player_thing() { return thing_at(g_state->players[g_state->local_player & 7].thing); }

// A creature that takes every damage type, put on a site (used as the victim of area damage).
static Thing *make_victim(const Pos *p) {
    Thing *v = thing_create(p, 5, 9);
    CHECK(v != nullptr);
    if (!v) return nullptr;
    v->prop_flags = 0x3f;
    v->flags |= 8;
    std::memset(v->damage_slots, 0, sizeof v->damage_slots);
    return v;
}

static void histogram(const char *what) {
    int by_type[64] = {}, n = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing *t = thing_at(i);
        if (t->cls != 10) continue;
        n++;
        by_type[t->type & 63]++;
    }
    std::printf("%s: %d effects; by type:", what, n);
    for (int i = 0; i < 64; i++) if (by_type[i]) std::printf(" 0x%02x x%d", i, by_type[i]);
    std::printf("\n");
}

// ---- 1. helpers ------------------------------------------------------------------------------------

static void test_helpers() {
    // terrain_max_drop_around against the definition
    uint32_t r = 12345;
    int nonzero = 0;
    for (int n = 0; n < 4000; n++) {
        r = mc_lcg(r);
        Pos p;
        p.x = (uint16_t)(r >> 8);
        p.y = (uint16_t)(r >> 20 ^ r);
        p.z = 0;
        int cx = ((int16_t)p.x - 0x80) >> 8, cy = ((int16_t)p.y - 0x80) >> 8;
        int centre = g_map_height[cell_xy((unsigned)cx, (unsigned)cy)], want = 0;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int d = g_map_height[cell_xy((unsigned)(cx + dx), (unsigned)(cy + dy))] - centre;
                if (d < want) want = d;
            }
        int got = terrain_max_drop_around(&p);
        if (got != want) { CHECK_EQ(got, want); break; }
        if (got) nonzero++;
    }
    std::printf("terrain_max_drop_around: 4000 positions agree with the 8-neighbour minimum (%d non-zero)\n", nonzero);

    // terrain_ring_find_height_ne8 on a prepared block
    std::vector<uint8_t> saved(g_map_height, g_map_height + MC_MAP_CELLS);
    const unsigned cx = 100, cy = 100, start = cell_xy(cx, cy);
    for (int dy = -40; dy <= 40; dy++)
        for (int dx = -40; dx <= 40; dx++) g_map_height[cell_xy(cx + dx, cy + dy)] = 8;
    CHECK_EQ(terrain_ring_find_height_ne8(start, 0), start);          // nothing but 8
    CHECK_EQ(terrain_ring_find_height_ne8(start, 0x1e), start);       // radius out of range
    g_map_height[cell_xy(cx - 3, cy - 3)] = 9;                        // first cell of ring 3
    CHECK_EQ(terrain_ring_find_height_ne8(start, 0), cell_xy(cx - 3, cy - 3));
    CHECK_EQ(terrain_ring_find_height_ne8(start, 4), start);          // rings 4.. do not reach it
    g_map_height[cell_xy(cx - 3, cy - 3)] = 8;
    g_map_height[cell_xy(cx, cy - 2)] = 7;                            // ring 2: +x side ends at (cx, cy - 2), the +y side starts there
    CHECK_EQ(terrain_ring_find_height_ne8(start, 1), cell_xy(cx, cy - 2));
    g_map_height[cell_xy(cx, cy - 2)] = 8;
    g_map_height[start] = 0;                                          // the centre itself is on ring r's -x side start
    CHECK_EQ(terrain_ring_find_height_ne8(start, 1), start);
    std::memcpy(g_map_height, saved.data(), MC_MAP_CELLS);

    // effect_age_tick
    Thing t{};
    t.health = 1;
    effect_age_tick(&t);
    CHECK_EQ(t.aux, 1); CHECK_EQ(t.health, 0); CHECK(!deleted(&t));
    effect_age_tick(&t);
    CHECK_EQ(t.health, -1); CHECK(!deleted(&t));
    effect_age_tick(&t);
    CHECK_EQ(t.aux, 3); CHECK_EQ(t.health, -2); CHECK(deleted(&t));
    CHECK(g_hook_thing_drop_mana_ball == thing_drop_mana_ball);
}

// ---- 2. handlers by construction -------------------------------------------------------------------

static void test_explosions() {
    Pos p = make_site();
    // explosion (state 0): two waiting ticks while aux & 3, then the live tick
    Thing *e = thing_create(&p, 10, 0);
    CHECK(e != nullptr);
    if (!e) return;
    CHECK_EQ(e->state, 0);
    e->flags |= 0x10000;                              // no area damage in this test
    e->aux = 2;
    int h0 = e->health;
    step(e); CHECK_EQ(e->aux, 1); CHECK_EQ(e->health, h0);
    step(e); CHECK_EQ(e->aux, 0); CHECK_EQ(e->health, h0);
    uint32_t r0 = e->rng;
    std::vector<uint8_t> height0(g_map_height, g_map_height + MC_MAP_CELLS);
    s_sounds.clear();
    e->flags |= 1;
    step(e);
    CHECK(e->flags & 2);
    CHECK(!(e->flags & 1));
    CHECK_EQ(e->health, h0 - 1);
    CHECK_EQ(sounds_of(3), 1);
    // plain land: one draw for the dent (rnd % 7 cells lower), one for the vertical speed
    uint32_t r1 = mc_lcg(r0), r2 = mc_lcg(r1);
    CHECK_EQ(e->rng, r2);
    CHECK_EQ(e->z_vel, (int16_t)(r2 % 0x41u - 0x20));
    CHECK(e->z_vel >= -0x20 && e->z_vel <= 0x20);
    int dented = 0, dent_bad = 0;
    for (int c = 0; c < MC_MAP_CELLS; c++) {
        int d = height0[c] - g_map_height[c];
        if (d == 0) continue;
        dented++;
        if (d != (int)(r1 % 7u)) dent_bad++;
    }
    CHECK_EQ(dent_bad, 0);
    CHECK_EQ(dented, r1 % 7u ? spiral_cells(0, 0) : 0);      // ring 0 of the spiral walk
    CHECK(r1 % 7u == 0 || g_map_height[fx_cell(&p)] == height0[fx_cell(&p)] - (int)(r1 % 7u));
    CHECK_EQ(e->frame, 1);
    int calls = 1 + run_out(e);
    CHECK_EQ(calls, h0 + 2);                          // health h0 .. -1, deleted by the call that reads -1
    CHECK_EQ(e->rng, r2);                             // no further draws
    CHECK_EQ(sounds_of(3), 1);
    std::printf("explosion: life %d, %d handler calls, dent %d, z_vel %d\n", h0, calls + 2, (int)(r1 % 7u), e->z_vel);

    // an explosion over a castle texture leaves the ground alone (texture 9 is not 0x1a / 0xa / 0xb)
    Pos q = make_site();
    unsigned qc = fx_cell(&q);
    uint8_t tex0 = g_map_type[qc];
    g_map_type[qc] = 9;
    Thing *e2 = thing_create(&q, 10, 0);
    e2->flags |= 0x10000;
    uint32_t s0 = e2->rng;
    uint8_t qh = g_map_height[qc];
    step(e2);
    CHECK_EQ(e2->rng, mc_lcg(s0));                    // only the z_vel draw
    CHECK_EQ(g_map_height[qc], qh);
    g_map_type[qc] = tex0;
    thing_mark_delete(e2);

    // area damage of the live tick (type 0, once)
    Pos v_pos = make_site();
    Thing *v = make_victim(&v_pos);
    Thing *e3 = thing_create(&v_pos, 10, 0);
    if (v && e3) {
        e3->z = v->z;
        step(e3);
        CHECK_EQ(v->damage_slots[0].amount, e3->damage);
        CHECK_EQ(v->damage_slots[0].attacker, e3->owner);
        step(e3);
        CHECK_EQ(v->damage_slots[0].amount, e3->damage);
        thing_mark_delete(e3);
        thing_mark_delete(v);
    }

    // big explosion (state 1): explosions on about half of the cells of ring aux
    Pos b_pos = make_site();
    Thing *b = thing_create(&b_pos, 10, 1);
    CHECK(b != nullptr);
    if (!b) return;
    b->aux = 2;
    b->health = 3;
    b->flags |= 0x10000;
    int ring = spiral_cells(2, 2), total = 0;
    s_sounds.clear();
    for (int tick = 0; tick < 3; tick++) {
        PoolMark m; m.take();
        uint32_t seed = b->rng;
        step(b);
        int want = 0;
        for (int c = 0; c < ring; c++) {
            seed = mc_lcg(seed);
            if ((int)((seed % 0x9du) / 0x4fu) * 2 - 1 > 0) { seed = mc_lcg(mc_lcg(seed)); want++; }
        }
        CHECK_EQ(b->rng, seed);
        std::vector<Thing *> made = m.fresh(10, 0);
        CHECK_EQ((int)made.size(), want);
        total += want;
        for (Thing *x : made) {
            CHECK_EQ(x->owner, b->owner);
            CHECK((x->flags & 0x10080) == 0x10080);
            CHECK(std::abs((int16_t)x->x - (int16_t)b->x) <= 2 * 0xc0 + 0xa0 + 0x60);
            thing_mark_delete(x);
        }
        sweep();
    }
    CHECK_EQ(sounds_of(3), 1);
    CHECK(!deleted(b));
    step(b); CHECK(!deleted(b));                      // health 0 -> -1
    step(b); CHECK(deleted(b));
    std::printf("big explosion: ring 2 has %d cells, %d explosions in 3 ticks\n", ring, total);

    // states 2 / 3 (lifetime only), 4 (nothing), splash (state 5)
    Thing *t2 = thing_create(&b_pos, 10, 2), *t3 = thing_create(&b_pos, 10, 3), *t4 = thing_create(&b_pos, 10, 4);
    if (t2 && t3 && t4) {
        CHECK_EQ(run_out(t2), t2->max_health + 2);
        CHECK_EQ(run_out(t3), t3->max_health + 2);
        int h4 = t4->health;
        for (int i = 0; i < 300; i++) step(t4);
        CHECK(!deleted(t4)); CHECK_EQ(t4->health, h4);
        thing_mark_delete(t4);
    }
    Thing *sp = thing_create(&b_pos, 10, 5);
    if (sp) {
        s_sounds.clear();
        int life = sp->health;
        step(sp);
        CHECK(sp->flags & 2); CHECK_EQ(sp->frame, 1); CHECK_EQ(sounds_of(0x1b), 1);
        CHECK_EQ(1 + run_out(sp), life + 2);
        CHECK_EQ(sounds_of(0x1b), 1);
    }
    sweep();
}

static void test_fire_and_smoke() {
    Pos p = make_site();
    Thing *f = thing_create(&p, 10, 6);
    CHECK(f != nullptr);
    if (!f) return;
    f->flags |= 0x10080;                              // no smoke, no damage
    int h0 = f->health, sprite0 = f->sprite;
    uint32_t r0 = f->rng;
    for (int i = 0; i < 7; i++) { step(f); CHECK_EQ(f->sprite, sprite0 + i + 1); CHECK_EQ(f->aux, i + 1); }
    step(f); CHECK_EQ(f->sprite, sprite0 + 7); CHECK_EQ(f->aux, 7);
    int calls = 8;
    while (f->health > 12) { step(f); calls++; CHECK_EQ(f->sprite, sprite0 + 7); }
    step(f); calls++;
    // the call that takes the health to 11 already shrinks
    CHECK_EQ(f->health, 11);
    CHECK_EQ(f->sprite, sprite0 + 6);
    int n = run_out(f);
    CHECK_EQ(calls + n, h0 + 2);
    CHECK_EQ(f->sprite, sprite0);
    CHECK_EQ(f->aux, 0);
    CHECK_EQ(f->rng, r0);
    CHECK_EQ(f->z, terrain_height_at(thing_pos(f)) + f->z_vel);
    std::printf("fire: life %d, sprite %d -> %d -> %d, %d handler calls\n", h0, sprite0, sprite0 + 7, f->sprite, calls + n);

    // with smoke: one draw per shrinking tick, a white smoke (aux 100, life 15, sprite + 2) on rnd % 7 == 0
    Thing *g = thing_create(&p, 10, 6);
    if (g) {
        g->flags |= 0x10000;
        g->health = 13;
        g->aux = 7;
        g->sprite = (uint16_t)(g->sprite + 7);
        g->z_vel = 0x23;
        step(g); CHECK_EQ(g->health, 12);
        CHECK_EQ(g->z, terrain_height_at(thing_pos(g)) + 0x23);
        uint32_t seed = g->rng;
        int smokes = 0, want = 0;
        for (int i = 0; i < 7; i++) {
            PoolMark m; m.take();
            step(g);
            seed = mc_lcg(seed);
            CHECK_EQ(g->rng, seed);
            std::vector<Thing *> made = m.fresh(10, 0xd);
            if (seed % 7u == 0) want++;
            CHECK_EQ((int)made.size(), seed % 7u == 0 ? 1 : 0);
            for (Thing *s : made) {
                smokes++;
                CHECK_EQ(s->aux, 100); CHECK_EQ(s->health, 15); CHECK_EQ(s->owner, g->owner); CHECK_EQ(s->sprite, 0x43 + 2);
                thing_mark_delete(s);
            }
        }
        CHECK_EQ(g->aux, 0);
        step(g);
        CHECK_EQ(g->rng, seed);                       // aux 0: no more draws
        std::printf("fire with smoke: 7 shrinking ticks, %d smoke puffs (expected %d)\n", smokes, want);
        thing_mark_delete(g);
    }
    // fire burns a victim: thing_area_damage_fire, type 0, every tick, the dying tick included
    Pos v_pos = make_site();
    Thing *v = make_victim(&v_pos);
    Thing *h = thing_create(&v_pos, 10, 6);
    if (v && h) {
        v->z = h->z;
        h->health = 1;
        step(h); CHECK_EQ(v->damage_slots[0].amount, 0x32);
        step(h); CHECK_EQ(v->damage_slots[0].amount, 0x64);
        step(h); CHECK(deleted(h)); CHECK_EQ(v->damage_slots[0].amount, 0x96);
        thing_mark_delete(v);
    }
    // fire on water dies at once
    Pos w = make_site(true);
    CHECK_EQ(terrain_type_mask_at(&w), 1);
    Thing *fw = thing_create(&w, 10, 6);
    if (fw) {
        fw->flags |= 0x10000;
        step(fw);
        CHECK(deleted(fw));
    }
    sweep();

    // smoke: white (state 0xd) and black (state 0xe)
    for (int type = 0xd; type <= 0xe; type++) {
        Thing *s = thing_create(&p, 10, type);
        CHECK(s != nullptr);
        if (!s) continue;
        s->yaw = 0x200;
        int life = s->health, sprite = s->sprite, speed = s->speed_cur, n_calls = 0;
        Pos start = *thing_pos(s);
        bool ok = true;
        while (!deleted(s) && n_calls < 1000) {
            Pos before = *thing_pos(s);
            int hb = s->health;
            step(s);
            n_calls++;
            if (deleted(s)) break;
            speed -= 4;
            if (speed < 0x40) speed = 0x40;
            if (speed > 0x80) speed = 0x80;
            int z = (int16_t)(before.z + speed), ground = (int16_t)terrain_height_at(&before);
            if (z < ground) z = ground;
            if (s->speed_cur != speed || s->z != z) ok = false;
            if (n_calls < 16) { if (!(n_calls & 1)) sprite++; }
            else if (s->x != before.x || s->y != before.y) ok = false;
            if (hb - 1 < 6 && (type == 0xe || sprite > 0x43)) sprite--;
            if (s->sprite != sprite) ok = false;
        }
        CHECK(ok);
        CHECK_EQ(n_calls, life + 2);
        int moved = pos_dist_xy(&start, thing_pos(s));
        CHECK(std::abs(moved - 15 * 0x1e) <= 15);     // 15 drift steps of 0x1e along the yaw
        std::printf("%s smoke: life %d, rose %d, drifted %d, sprite %d -> %d\n", type == 0xd ? "white" : "black", life,
                    s->z - start.z, moved, type == 0xd ? 0x43 : 9, s->sprite);
    }
    sweep();
}

static void test_damage_effects() {
    Pos p = make_site();
    Thing *v = make_victim(&p);
    if (!v) return;
    struct Case { int type, state, slot; bool every_tick, anim; int sound; } cases[] = {
        {0x0c, 0x0c, 1, true,  true,  -1},     // 240b0
        {0x17, 0x17, 0, false, false, 0x18},   // 24c20 lightning strike
        {0x19, 0x19, 3, false, true,  -1},     // 24cb0 steal mana
        {0x1a, 0x1a, 4, true,  true,  -1},     // 24d10
    };
    for (const Case &c : cases) {
        std::memset(v->damage_slots, 0, sizeof v->damage_slots);
        Thing *e = thing_create(&p, 10, c.type);
        CHECK(e != nullptr);
        if (!e) continue;
        CHECK_EQ(e->state, c.state);
        e->z = v->z;
        e->flags |= 8;
        thing_set_extents(e, 0x200, 0x400);
        int life = e->health, dmg = e->damage;
        s_sounds.clear();
        step(e);
        CHECK_EQ(e->aux, 1);
        CHECK_EQ(v->damage_slots[c.slot].amount, dmg);
        CHECK_EQ(v->damage_slots[c.slot].attacker, e->owner);
        if (c.anim) CHECK_EQ(e->frame, 1);
        if (c.sound >= 0) CHECK_EQ(sounds_of(c.sound), 1);
        step(e);
        CHECK_EQ(v->damage_slots[c.slot].amount, c.every_tick ? 2 * dmg : dmg);
        for (int s = 0; s < 6; s++) if (s != c.slot) CHECK_EQ(v->damage_slots[s].amount, 0);
        int calls = 2 + (deleted(e) ? 0 : run_out(e));
        // the lightning strike cuts its life to one more tick: calls = 1 (burst), health 1 -> 0 -> -1, delete
        CHECK_EQ(calls, c.type == 0x17 ? 4 : life + 2);
        std::printf("effect type 0x%02x: damage slot %d, amount %d %s, %d handler calls\n", c.type, c.slot, dmg,
                    c.every_tick ? "every tick" : "once", calls);
        sweep();
    }
    // the states that are a lone ret
    for (int state : {0x04, 0x14, 0x15, 0x16, 0x18, 0x3d}) {
        Thing t{};
        t.cls = 10; t.state = (uint8_t)state; t.health = 5; t.rng = 77;
        Thing before = t;
        ThingUpdateFn fn = thing_update_fn(10, state);
        CHECK(fn != nullptr);
        if (fn) fn(&t);
        CHECK(std::memcmp(&t, &before, sizeof t) == 0);
    }
    // state 0x23 (type 0x21): ages and sets the first-tick flag
    Thing *a = thing_create(&p, 10, 0x21);
    if (a) {
        int life = a->health;
        step(a); CHECK(a->flags & 2); CHECK_EQ(a->aux, 1);
        CHECK_EQ(1 + run_out(a), life + 2);
    }
    thing_mark_delete(v);
    sweep();
}

static void test_quake_lava_meteor() {
    // earthquake (state 0xf): one wandering step per tick, a 10-tick crater with its extents behind it
    Pos p = make_site();
    Thing *q = thing_create(&p, 10, 0xf);
    CHECK(q != nullptr);
    if (q) {
        q->health = 4;
        thing_set_extents(q, 0x300, 0x1234);
        for (int i = 0; i < 4; i++) {
            PoolMark m; m.take();
            Pos before = *thing_pos(q);
            uint16_t yaw0 = q->yaw;
            uint32_t seed = q->rng;
            step(q);
            seed = mc_lcg(seed);
            CHECK_EQ(q->rng, seed);
            CHECK_EQ(q->yaw, (yaw0 + seed % 0x5bu - 0x2d) & 0x7ff);
            CHECK(std::abs(pos_dist_xy(&before, thing_pos(q)) - 0x100) <= 2);
            std::vector<Thing *> made = m.fresh(10, 0xb);
            CHECK_EQ((int)made.size(), 1);
            for (Thing *c : made) {
                CHECK_EQ(c->health, 10); CHECK_EQ(c->ext_x, 0x300); CHECK_EQ(c->ext_h, 0x1234); CHECK_EQ(c->owner, q->owner);
                CHECK(c->x == q->x && c->y == q->y);
                thing_mark_delete(c);
            }
        }
        step(q); CHECK(!deleted(q));                  // health 0 -> -1 still acts
        step(q); CHECK(deleted(q));
        sweep();
    }
    // on cells whose flag nibble is 0 (terrain_cell_flag_bit & 1: class-0 water that is not animated
    // open sea) it counts up and gives up at 9; elsewhere it counts back down
    Pos w = make_site();
    uint8_t saved_flags[11][11];
    for (int dy = -5; dy <= 5; dy++)
        for (int dx = -5; dx <= 5; dx++) {
            unsigned c = cell_xy((unsigned)((w.x >> 8) + dx), (unsigned)((w.y >> 8) + dy));
            saved_flags[dy + 5][dx + 5] = g_map_flags[c];
            g_map_flags[c] &= 0xf0;
        }
    Thing *qw = thing_create(&w, 10, 0xf);
    if (qw) {
        CHECK(terrain_cell_flag_bit(&w) & 1);
        int n = 0;
        // (the crack moves a cell per tick: the 11 x 11 block keeps it on such cells for 4 steps at least)
        while (!deleted(qw) && n < 4) { CHECK(terrain_cell_flag_bit(thing_pos(qw)) & 1); step(qw); n++; }
        CHECK_EQ(qw->aux, n);
        qw->x = w.x; qw->y = w.y;
        qw->aux = 8;
        step(qw);
        CHECK(deleted(qw));
        std::printf("earthquake on nibble-0 cells: aux %d after %d ticks, gone at 9\n", n, n);
    }
    for (int dy = -5; dy <= 5; dy++)
        for (int dx = -5; dx <= 5; dx++)
            g_map_flags[cell_xy((unsigned)((w.x >> 8) + dx), (unsigned)((w.y >> 8) + dy))] = saved_flags[dy + 5][dx + 5];
    Thing *qd = thing_create(&w, 10, 0xf);
    if (qd) {
        CHECK(!(terrain_cell_flag_bit(&w) & 1));
        qd->aux = 3;
        step(qd);
        CHECK_EQ(qd->aux, 2);
        thing_mark_delete(qd);
    }
    for (int i = 1; i < MC_THING_SLOTS; i++) if (thing_at(i)->cls == 10 && thing_at(i)->type == 0xb) thing_mark_delete(thing_at(i));
    sweep();

    // lava blob (state 0x10)
    Pos l = make_site();
    {
        Thing *b = thing_create(&l, 10, 0x10);
        CHECK(b != nullptr);
        if (b) {
            int life = b->health, fires = 0, calls = 0, bounces = 0;
            bool ok = true;
            PoolMark m; m.take();
            while (!deleted(b) && calls < 1000) {
                int16_t zv = b->z_vel;
                step(b);
                calls++;
                if (std::abs((int16_t)b->home.x) > 0x50 || std::abs((int16_t)b->home.y) > 0x50) ok = false;
                if (b->z < (int16_t)terrain_height_at(thing_pos(b))) ok = false;
                if (b->z_vel < -0x180 || b->z_vel > 0x100) ok = false;
                if (zv < 0 && b->z_vel >= 0) bounces++;
            }
            CHECK(ok);
            CHECK_EQ(calls, life + 2);
            for (Thing *f : m.fresh(10, 6)) {
                fires++;
                CHECK_EQ(f->health, 0x1e); CHECK_EQ(f->damage, 0x32 * 3); CHECK_EQ(f->owner, b->owner);
                thing_mark_delete(f);
            }
            CHECK(fires >= 1);
            std::printf("lava blob: life %d, %d landings, %d fires lit, travelled %d\n", life, bounces, fires, pos_dist_xy(&l, thing_pos(b)));
            sweep();
        }
        // over water: a splash, and the blob is gone
        s_site_cursor[1] = 0;                       // the water site of the fire test again
        Pos w2 = make_site(true);
        Thing *bw = thing_create(&w2, 10, 0x10);
        if (bw) {
            bw->home.x = bw->home.y = 0;
            PoolMark m; m.take();
            int calls = run_out(bw, 200);
            std::vector<Thing *> made = m.fresh(10, 5);
            CHECK(calls > 0);
            CHECK_EQ((int)made.size(), 1);
            CHECK_EQ((int)m.fresh(10, 6).size(), 0);
            for (Thing *s : made) { CHECK_EQ(s->owner, bw->owner); thing_mark_delete(s); }
            std::printf("lava blob over water: splash after %d ticks\n", calls);
            sweep();
        }
    }

    // meteor impact (state 0x11)
    Pos mp = make_site();
    Thing *me = thing_create(&mp, 10, 0x11);
    CHECK(me != nullptr);
    if (me) {
        Pos vp = mp;
        vp.x = (uint16_t)(vp.x + 0x100);
        Thing *v = make_victim(&vp);
        int life = me->health, calls = 0, explosions = 0;
        s_sounds.clear();
        while (!deleted(me) && calls < 100) {
            PoolMark m; m.take();
            int16_t aux = me->aux;
            uint32_t seed = me->rng;
            step(me);
            calls++;
            if (deleted(me)) break;
            CHECK((me->flags & 0x10002) == 0x10002);
            CHECK_EQ(me->ext_x, aux * 0xc0);
            CHECK_EQ(me->ext_h, 0x200);
            CHECK_EQ(me->aux, (aux + 2) % 11);
            int cells = spiral_cells(aux, aux);
            CHECK_EQ(lcg_steps(seed, me->rng), 1 + 2 * cells);
            std::vector<Thing *> made = m.fresh(10, 0);
            CHECK_EQ((int)made.size(), cells);
            for (Thing *x : made) {
                explosions++;
                CHECK((x->flags & 0x10080) == 0x10080); CHECK_EQ(x->ext_x, 0x200); CHECK_EQ(x->aux, 0); CHECK_EQ(x->owner, me->owner);
                thing_mark_delete(x);
            }
            sweep();
        }
        CHECK_EQ(calls, life + 2);
        CHECK_EQ(sounds_of(0x1e), 1);
        // damage / max_health per tick while the victim is inside the growing box (aux * 0xc0 >= 0x100 - ext)
        if (v) {
            std::printf("meteor: life %d, %d explosions, victim at 0x100 took %d (%d per tick)\n", life, explosions,
                        v->damage_slots[0].amount, me->damage / me->max_health);
            CHECK(v->damage_slots[0].amount > 0);
            CHECK_EQ(v->damage_slots[0].amount % (me->damage / me->max_health), 0);
            thing_mark_delete(v);
        }
        sweep();
    }
}

static uint16_t state_word(unsigned off) {
    uint16_t v;
    std::memcpy(&v, reinterpret_cast<const uint8_t *>(g_state) + off, 2);
    return v;
}

static void test_eruption() {
    Pos p = make_site();
    std::memset(reinterpret_cast<uint8_t *>(g_state) + 0x24, 0, 4);
    Thing *a = thing_create(&p, 10, 0x12);
    CHECK(a != nullptr);
    if (!a) return;
    CHECK_EQ(a->aux, 0);
    uint32_t r0 = a->rng;
    uint16_t yaw0 = a->yaw;
    PoolMark m; m.take();
    step(a);
    CHECK_EQ(a->aux, 1);
    CHECK_EQ(state_word(0x24), thing_index(a));
    std::vector<Thing *> smoke = m.fresh(10, 0x13), blob = m.fresh(10, 0x10), shot = m.fresh(9, 0);
    CHECK_EQ((int)smoke.size(), 1); CHECK_EQ((int)blob.size(), 1); CHECK_EQ((int)shot.size(), 1);
    CHECK_EQ(a->rng, mc_lcg(r0));                      // one draw: the blob's seed
    CHECK_EQ(a->yaw, (uint16_t)(yaw0 + 0x500));
    if (!smoke.empty()) { CHECK_EQ(state_word(0x26), thing_index(smoke[0])); CHECK_EQ(smoke[0]->owner, a->owner); }
    if (!blob.empty()) { CHECK_EQ(blob[0]->rng, a->rng); CHECK_EQ(blob[0]->owner, a->owner); }
    if (!shot.empty()) {
        Thing *s = shot[0];
        CHECK_EQ(s->impact_cls, 10); CHECK_EQ(s->impact_type, 0x11); CHECK_EQ(s->health, 1); CHECK_EQ(s->pitch, 0xfe7e);
        CHECK_EQ(s->yaw, a->yaw & 0x7ff); CHECK_EQ(s->owner, a->owner);
        CHECK(std::abs(pos_dist_xy(thing_pos(a), &s->home) - 0x600) <= 4);
        CHECK_EQ(s->home.z, terrain_height_at(&s->home));
    }
    // a second crater takes over: the first one is sent to sleep, its smoke column goes
    Pos p2 = make_site();
    Thing *b = thing_create(&p2, 10, 0x12);
    if (b) {
        step(b);
        CHECK_EQ(a->aux, 0xfa);
        CHECK_EQ(state_word(0x24), thing_index(b));
        if (!smoke.empty()) CHECK(deleted(smoke[0]));
        CHECK(state_word(0x26) != 0 && (smoke.empty() || state_word(0x26) != thing_index(smoke[0])));
    }
    // the active phase: a blob on every tick whose roll succeeds (aux & 0xf != 0, rnd % 5 == 0)
    if (b) {
        int blobs = 0, rolls = 0;
        bool ended = false;
        for (int tick = 0; tick < 0x7f && !deleted(b); tick++) {
            int16_t aux = b->aux;
            uint32_t seed = b->rng;
            PoolMark mk; mk.take();
            step(b);
            int made = (int)mk.fresh(10, 0x10).size();
            if ((aux & 0xf) != 0) {
                rolls++;
                uint32_t r = mc_lcg(seed);
                bool act = r % 5u == 0;
                CHECK_EQ(made, act ? 1 : 0);
                CHECK_EQ(b->rng, act ? mc_lcg(r) : r);
                if (aux == 0x7f && act) { CHECK(deleted(b)); CHECK_EQ(state_word(0x24), 0); ended = true; }
            } else {
                CHECK_EQ(made, 0);
                CHECK_EQ(b->rng, seed);
            }
            blobs += made;
            if (!deleted(b)) CHECK_EQ(b->aux, aux + 1);
            for (Thing *x : mk.fresh()) thing_mark_delete(x);
            sweep();
        }
        std::printf("eruption: %d rolls, %d lava blobs in 127 ticks, %s at aux 0x7f\n", rolls, blobs, ended ? "ended" : "went dormant");
        if (!deleted(b)) {
            // dormant: nothing happens, not even a draw, until aux is above 2500
            uint32_t seed = b->rng;
            for (int i = 0; i < 50; i++) step(b);
            CHECK_EQ(b->rng, seed);
            CHECK_EQ(b->aux, 0x80 + 50);
            // above 2500 with a free slot: one draw per tick, restart on rnd % 100 == 0
            std::memset(reinterpret_cast<uint8_t *>(g_state) + 0x24, 0, 2);
            b->aux = 0x9c5;
            int waited = 0;
            while (b->aux != 1 && waited < 5000 && !deleted(b)) {
                uint32_t r = mc_lcg(b->rng);
                int16_t aux = b->aux;
                step(b);
                waited++;
                if (r % 0x64u == 0) CHECK_EQ(b->aux, 1); else CHECK_EQ(b->aux, aux + 1);
            }
            CHECK_EQ(state_word(0x24), thing_index(b));
            std::printf("eruption: dormant crater restarted after %d ticks above 2500\n", waited);
        }
    }
    // the ground moved under it: gone
    Thing *c = thing_create(&p, 10, 0x12);
    if (c) {
        c->z = (int16_t)(c->z + 1);
        step(c);
        CHECK(deleted(c));
    }
    // the smoke column (state 0x13): 4 white clouds per chosen centre cell on odd health
    Thing *col = thing_create(&p, 10, 0x13);
    if (col) {
        Thing *v = make_victim(&p);
        int cells = spiral_cells(0, 0), clouds = 0;
        for (int tick = 0; tick < 6; tick++) {
            PoolMark mk; mk.take();
            uint32_t seed = col->rng;
            step(col);
            int chosen = 0;
            for (int i = 0; i < cells; i++) {
                seed = mc_lcg(seed);
                if ((int)((seed % 0x9du) / 0x4fu) * 2 - 1 > 0) { seed = mc_lcg(mc_lcg(seed)); chosen++; }
            }
            CHECK_EQ(col->rng, seed);
            std::vector<Thing *> made = mk.fresh(10, 0xd);
            bool odd = (col->health & 1) != 0;
            CHECK_EQ((int)made.size(), odd ? 4 * chosen : 0);
            // (pool slots are not handed out in index order: compare the yaws as a set)
            unsigned want_first = (unsigned)((col->health / 2) & 1) << 8;
            int per_yaw[4] = {};
            for (Thing *cloud : made) {
                unsigned k = (cloud->yaw - want_first) / 0x200;
                CHECK(k < 4 && cloud->yaw == want_first + k * 0x200);
                if (k < 4) per_yaw[k]++;
                CHECK_EQ(cloud->owner, col->owner);
                thing_mark_delete(cloud);
            }
            for (int k = 0; k < 4 && odd; k++) CHECK_EQ(per_yaw[k], chosen);
            clouds += (int)made.size();
            sweep();
        }
        if (v) { CHECK_EQ(v->damage_slots[0].amount, 6 * col->damage); thing_mark_delete(v); }
        col->health = -1;
        step(col);
        CHECK(deleted(col));
        if (v) CHECK_EQ(v->damage_slots[0].amount, 7 * col->damage);   // the dying tick deals damage too
        std::printf("volcano smoke: centre quad has %d cells, %d clouds in 6 ticks\n", cells, clouds);
    }
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        Thing *t = thing_at(i);
        if (t->cls == 10 && (t->type == 0x12 || t->type == 0x13 || t->type == 0x10)) thing_mark_delete(t);
        if (t->cls == 9) thing_mark_delete(t);
    }
    std::memset(reinterpret_cast<uint8_t *>(g_state) + 0x24, 0, 4);
    sweep();
}

static void test_teleport_orbiter_storm_army() {
    Thing *pl = local_player_thing();
    CHECK(pl->cls == 3);
    Pos home = *thing_pos(pl);
    uint16_t yaw_saved = pl->yaw;
    // teleport gate on top of the player
    Pos dest = make_site();
    Thing *g = thing_create(&home, 10, 0x22);
    CHECK(g != nullptr);
    if (g) {
        CHECK_EQ(g->state, 0x24);
        g->z = pl->z;
        g->home = dest;
        thing_set_extents(g, 0x100, 0x100);
        // looking away: nothing happens
        pl->yaw = (uint16_t)((pos_angle_to(thing_pos(pl), thing_pos(g)) + 0x400) & 0x7ff);
        s_sounds.clear();
        g->z = pl->z;
        step(g);
        CHECK(pl->x == home.x && pl->y == home.y);
        CHECK_EQ(sounds_of(0x15), 1);
        CHECK_EQ(g->z, terrain_height_at(thing_pos(g)));
        // looking at it
        g->z = pl->z;
        pl->yaw = (uint16_t)(pos_angle_to(thing_pos(pl), thing_pos(g)) & 0x7ff);
        step(g);
        CHECK(pl->x == dest.x && pl->y == dest.y);
        CHECK_EQ(pl->z, terrain_height_at(&dest) + mc_move_desc(g->desc)->clear_hi);
        CHECK_EQ(sounds_of(0x16), 1);
        CHECK_EQ(sounds_of(0x15), 1);
        CHECK_EQ(g_cfg->palette_effect, 6);
        g_cfg->palette_effect = 0;
        // a gate with a lifetime closes when it reaches 0 (permanent gates have health 0)
        for (int i = 0; i < 20; i++) step(g);
        CHECK(!deleted(g));
        g->health = 2;
        step(g); CHECK(!deleted(g)); CHECK_EQ(g->health, 1);
        step(g); CHECK(deleted(g)); CHECK_EQ(sounds_of(0x14), 1);
        thing_move_to(pl, &home);
        pl->yaw = yaw_saved;
    }
    sweep();

    // orbiter (state 0x25; its constructor hands back a raw allocation, so build one by hand)
    Pos c_pos = make_site();
    Thing *centre = make_victim(&c_pos);
    Thing *o = thing_alloc();
    if (centre && o) {
        o->cls = 10; o->type = 0x23; o->state = 0x25;
        o->caster = thing_index(centre);
        o->speed_cur = 0x180;
        o->yaw = 0x100;
        thing_link_cell(o, &c_pos);
        for (int i = 0; i < 5; i++) {
            uint16_t yaw = o->yaw;
            step(o);
            CHECK_EQ(o->yaw, (yaw + 0x2d) & 0x7ff);
            CHECK(std::abs(pos_dist_xy(thing_pos(o), thing_pos(centre)) - 0x180) <= 3);
            CHECK_EQ(o->z, terrain_height_at(thing_pos(o)) + 0x100);
        }
        centre->health = 1;
        step(o);
        CHECK(deleted(o));
        Thing lone{};
        lone.cls = 10; lone.state = 0x25;
        Thing before = lone;
        thing_update_fn(10, 0x25)(&lone);              // no caster: nothing
        CHECK(std::memcmp(&lone, &before, sizeof lone) == 0);
        thing_mark_delete(centre);
    }
    sweep();

    // storm cloud (state 0x28): climbs to 0x400 above the ground, then two shots per tick
    Pos s_pos = make_site();
    Thing *st = thing_create(&s_pos, 10, 0x26);
    CHECK(st != nullptr);
    if (st) {
        CHECK_EQ(st->state, 0x28);
        st->impact_cls = 9; st->impact_type = 0;
        st->damage = 777;
        int life = st->health, climb = 0, ground = terrain_height_at(thing_pos(st));
        uint32_t seed = st->rng;
        while (st->z != ground + 0x400 && climb < 100) { step(st); climb++; }
        CHECK_EQ(climb, (ground + 0x400 - s_pos.z + 0x3f) / 0x40);
        CHECK_EQ(st->health, life);
        CHECK_EQ(st->rng, seed);
        PoolMark m; m.take();
        s_sounds.clear();
        step(st);
        seed = mc_lcg(seed);
        CHECK_EQ(st->rng, seed);
        CHECK_EQ(st->pitch, 0x38);
        CHECK_EQ(st->yaw, seed & 0x7ff);               // two half turns bring it back
        std::vector<Thing *> shots = m.fresh(9, 0);
        CHECK_EQ((int)shots.size(), 2);
        if (shots.size() == 2) {
            // first shot: yaw + 0x400, second: back at yaw (the sound is requested on the second)
            if (shots[0]->yaw != (seed & 0x7ff)) std::swap(shots[0], shots[1]);
            CHECK_EQ(shots[1]->yaw, ((seed & 0x7ff) + 0x400) & 0x7ff);
            CHECK_EQ(shots[0]->yaw, seed & 0x7ff);
            std::swap(shots[0], shots[1]);
            for (Thing *s : shots) {
                CHECK_EQ(s->pitch, 0x38); CHECK_EQ(s->impact_cls, 10); CHECK_EQ(s->impact_type, 0x17);
                CHECK_EQ(s->damage, 777); CHECK_EQ(s->owner, st->owner);
                CHECK_EQ(s->health, s->max_health / 3);
            }
            CHECK_EQ(sounds_of(0x17), 1);
            CHECK_EQ(s_sounds.back().thing, thing_index(shots[1]));
        }
        CHECK_EQ(1 + run_out(st), life + 2);
        std::printf("storm cloud: %d climbing ticks, life %d, %d shots\n", climb, life, (int)m.fresh(9, 0).size());
        for (Thing *s : m.fresh()) thing_mark_delete(s);
    }
    sweep();

    // skeleton army (state 0x26)
    Pos a_pos = make_site();
    Thing *army = thing_create(&a_pos, 10, 0x24);
    CHECK(army != nullptr);
    if (army) {
        CHECK_EQ(army->state, 0x26);
        g_cfg->creature_lists[9] = 0;
        PoolMark m; m.take();
        step(army);
        CHECK(deleted(army));
        CHECK_EQ(army->damage, 10000);
        std::vector<Thing *> sk = m.fresh(5, 9);
        CHECK_EQ((int)sk.size(), 8);
        unsigned yaw_seen = 0;
        for (size_t i = 0; i < sk.size(); i++) {
            // facing outwards: yaw = angle of its place on the circle + 0x400, one per 0x100 step
            CHECK_EQ(sk[i]->yaw & 0xff, 0);
            yaw_seen |= 1u << (sk[i]->yaw >> 8);
            CHECK(angle_diff(sk[i]->yaw, (pos_angle_to(&a_pos, thing_pos(sk[i])) + 0x400) & 0x7ff) <= 2);
            CHECK_EQ(sk[i]->target_yaw, sk[i]->yaw);
            CHECK_EQ(sk[i]->mana, 10000 % 1250);
            CHECK_EQ(sk[i]->mana_owner, army->owner);
            CHECK(std::abs(pos_dist_xy(&a_pos, thing_pos(sk[i])) - 0x200) <= 3);
        }
        CHECK_EQ(yaw_seen, 0xff);
        // 61 skeletons of that owner in the per-type list already: only 0x40 - 61 = 3 more, a third of a
        // turn apart, each with 10000 % 3333 = 1 mana
        int second_count = -1;
        if (sk.size() == 8) {
            std::vector<Thing *> owned(sk);
            while (owned.size() < 61) {
                Thing *x = thing_create(&a_pos, 5, 9);
                if (!x) break;
                x->mana_owner = army->owner;
                owned.push_back(x);
            }
            CHECK_EQ((int)owned.size(), 61);
            for (size_t i = 0; i + 1 < owned.size(); i++) owned[i]->next = thing_index(owned[i + 1]);
            owned.back()->next = 0;
            g_cfg->creature_lists[9] = thing_index(owned[0]);
            Thing *army2 = thing_create(&a_pos, 10, 0x24);
            if (army2) {
                army2->owner = army->owner;
                PoolMark m2; m2.take();
                step(army2);
                std::vector<Thing *> second = m2.fresh(5, 9);
                second_count = (int)second.size();
                CHECK_EQ(second_count, 3);
                for (Thing *s : second) {
                    CHECK_EQ(s->mana, 1);
                    CHECK(s->yaw == 0x400 || s->yaw == ((0x2aa + 0x400) & 0x7ff) || s->yaw == ((0x554 + 0x400) & 0x7ff));
                    thing_mark_delete(s);
                }
            }
            for (Thing *s : owned) thing_mark_delete(s);
        } else {
            for (Thing *s : sk) thing_mark_delete(s);
        }
        g_cfg->creature_lists[9] = 0;
        std::printf("skeleton army: %d skeletons in a circle of 0x200; with 61 owned already: %d\n", (int)sk.size(), second_count);
    }
    sweep();
}

static int expected_ball_sprite(const Thing *b, const GameState *state) {
    int level = 0;
    while (level < 7 && b->mana > g_mana_ball_thresholds[level]) level++;
    int base = 0x34;
    if (b->mana_owner != 0) {
        const Thing *o = &state->things[b->mana_owner % MC_THING_SLOTS];
        if (o->cls == 3) {
            // the owner's player number: which player record has this thing?
            for (int p = 0; p < 8; p++)
                if (state->players[p].thing == b->mana_owner) base = 0x69 + 8 * p;
        }
    }
    return base + level;
}

static void test_mana() {
    Thing *p0 = local_player_thing();
    uint16_t p0i = thing_index(p0);
    CHECK_EQ(player_block(p0)->player_no, g_state->local_player);

    // --- thing_drop_mana_ball
    Pos site = make_site();
    Thing *carrier = make_victim(&site);
    if (!carrier) return;
    carrier->z = (int16_t)(carrier->z + 0x123);
    carrier->mana = 0;
    carrier->mana_owner = p0i;
    uint32_t seed = carrier->rng;
    PoolMark m; m.take();
    thing_drop_mana_ball(carrier);
    CHECK_EQ(carrier->rng, seed);                      // no mana: nothing at all
    CHECK_EQ(carrier->mana_owner, p0i);
    CHECK_EQ((int)m.fresh().size(), 0);
    carrier->mana = 700;
    carrier->yaw = 0x7f0;
    g_hook_thing_drop_mana_ball(carrier);
    CHECK_EQ(carrier->rng, mc_lcg(seed));
    CHECK_EQ(carrier->mana_owner, 0);
    CHECK_EQ(carrier->mana, 700);                      // the mana field is the caller's business
    std::vector<Thing *> made = m.fresh(10, 0x27);
    CHECK_EQ((int)made.size(), 1);
    if (made.size() == 1) {
        Thing *b = made[0];
        uint32_t bs = (uint32_t)thing_index(b) + g_state->rng;      // thing_alloc's seed; the constructor draws nothing
        uint32_t r1 = mc_lcg(bs), r2 = mc_lcg(r1);
        CHECK_EQ(b->rng, r2);
        CHECK_EQ(b->mana, 700);
        CHECK_EQ(b->mana_owner, p0i);
        CHECK_EQ(b->yaw, (0x7f0 + r1 % 0x71u - 0x38) & 0x7ff);
        CHECK_EQ(b->speed_cur, (int)(r2 % 0x30u) + 0x10);
        CHECK_EQ(b->z_vel, (0x400 - 0x123) / 8);
        Pos v{0, 0, 0};
        math_rotate_offset(&v, b->yaw, 0, b->speed_cur);
        CHECK(b->home.x == v.x && b->home.y == v.y);
        CHECK_EQ(b->sprite, 0x35);                     // still the constructor's (0x200 mana, no owner) ...
        // flight: thrown up, lands, comes to rest on the flat site
        int ticks = 0, max_rise = 0;
        bool ok = true;
        Pos start = *thing_pos(b);
        for (; ticks < 400; ticks++) {
            step(b);
            if (ticks == 0) CHECK_EQ(b->sprite, 0x69 + 8 * g_state->local_player + 2);   // ... until its first update: 700 <= 1024 is size 2, owner colour
            int ground = (int16_t)terrain_height_at(thing_pos(b));
            if (b->z < ground) ok = false;
            if (std::abs((int16_t)b->home.x) > 0x40 || std::abs((int16_t)b->home.y) > 0x40) ok = false;
            if (b->z_vel < -0x80) ok = false;
            if (b->z - start.z > max_rise) max_rise = b->z - start.z;
        }
        CHECK(ok);
        CHECK_EQ(b->z, terrain_height_at(thing_pos(b)));
        CHECK(b->home.x == 0 && b->home.y == 0);
        CHECK(b->z_vel == 0 || b->z_vel == -0x10);     // the resting hop: 0, -0x10, 0, ...
        std::printf("dropped mana ball: yaw %03x speed %d z_vel %d, rose %d, rolled %d, at rest after 400 ticks\n",
                    b->yaw, b->speed_cur, (0x400 - 0x123) / 8, max_rise, pos_dist_xy(&start, thing_pos(b)));
        // asleep (timer_a == 0): the handler leaves it alone
        Thing before = *b;
        b->timer_a = 0;
        before.timer_a = 0;
        thing_update_fn(10, 0x29)(b);
        CHECK(std::memcmp(b, &before, sizeof before) == 0);
        b->timer_a = 0x80;

        // --- a claim (damage slot 1) changes the owner, a pull (slot 4) adds velocity
        Thing *p1 = thing_at(g_state->players[(g_state->local_player + 1) & 3].thing);
        b->damage_slots[1].attacker = thing_index(p1);
        b->damage_slots[1].amount = 5;
        b->flags |= 0x40;
        b->target = 0;                                 // not a balloon: the collect flag is dropped anyway
        s_sounds.clear();
        step(b);
        CHECK_EQ(b->mana_owner, thing_index(p1));
        CHECK_EQ(b->damage_slots[1].attacker, 0); CHECK_EQ(b->damage_slots[1].amount, 0);
        CHECK(!(b->flags & 0x40));
        CHECK_EQ(sounds_of(4), 1);
        if (!s_sounds.empty()) CHECK_EQ(s_sounds[0].thing, thing_index(p1));
        step(b);
        CHECK_EQ(b->sprite, 0x69 + 8 * player_block(p1)->player_no + 2);
        s_sounds.clear();
        b->damage_slots[1].attacker = thing_index(p1);  // the same owner again: no sound
        step(b);
        CHECK_EQ(sounds_of(4), 0);
        CHECK_EQ(b->damage_slots[1].attacker, 0);

        Thing *puller = make_victim(&site);
        if (puller) {
            Pos pp = *thing_pos(b);
            pp.x = (uint16_t)(pp.x + 0x300);
            thing_move_to(puller, &pp);
            Pos at = *thing_pos(b);
            b->damage_slots[4].attacker = thing_index(puller);
            b->damage_slots[4].amount = 100;
            step(b);
            int yaw = pos_angle_to(&at, thing_pos(puller));
            Pos pv{0, 0, 0};
            math_rotate_offset(&pv, yaw, 0, 4);
            CHECK_EQ(b->yaw, yaw);
            CHECK_EQ(b->damage_slots[4].attacker, 0);
            CHECK_EQ(b->x, (uint16_t)(at.x + pv.x));
            CHECK_EQ(b->y, (uint16_t)(at.y + pv.y));
            CHECK_EQ((int16_t)b->home.x, (int16_t)pv.x * 250 / 256);   // friction of the same tick (flat ground)
            std::printf("mana ball pulled: velocity %d,%d -> %d,%d after friction\n", (int16_t)pv.x, (int16_t)pv.y,
                        (int16_t)b->home.x, (int16_t)b->home.y);
            thing_mark_delete(puller);
        }

        // --- being collected (flag 0x40): homes on a balloon (class 3 type 3)
        Thing *balloon = thing_alloc();
        if (balloon) {
            balloon->cls = 3; balloon->type = 3;
            Pos bp = *thing_pos(b);
            bp.x = (uint16_t)(bp.x + 0x200);
            bp.z = (int16_t)(bp.z + 0x300);
            thing_link_cell(balloon, &bp);
            b->home.x = b->home.y = 0;
            b->flags |= 0x40;
            b->target = thing_index(balloon);
            int n = 0;
            Pos last = *thing_pos(b);
            while ((b->x != bp.x || b->y != bp.y) && n < 100) {
                step(b);
                n++;
                CHECK(b->flags & 0x40);
                CHECK_EQ(b->z_vel, 0x80);
                int moved = pos_dist_xy(&last, thing_pos(b));
                CHECK(moved <= 0x11);
                last = *thing_pos(b);
            }
            CHECK(n >= 0x200 / 0x10 && n <= 0x200 / 0x10 + 2);
            int16_t z = b->z;
            step(b);
            CHECK_EQ(b->z, z + 0x20);                  // climbs to the balloon 0x20 per tick
            for (int i = 0; i < 40; i++) step(b);
            CHECK(b->z >= balloon->z && b->z <= balloon->z + 0x200);
            // out of reach: the flag is dropped
            Pos away = bp;
            away.x = (uint16_t)(away.x + 0x500);
            thing_move_to(balloon, &away);
            step(b);
            CHECK(!(b->flags & 0x40));
            std::printf("mana ball collected: reached the balloon in %d ticks\n", n);
            thing_free(balloon);
        }
        thing_mark_delete(b);
    }
    thing_mark_delete(carrier);
    sweep();

    // --- mana_ball_merge: who owns the merged ball
    Thing *p1 = thing_at(g_state->players[(g_state->local_player + 1) & 3].thing);
    Thing *hoard1 = thing_create(&site, 10, 0x28), *hoard2 = thing_create(&site, 10, 0x28);
    CHECK(hoard1 && hoard2 && p1->cls == 3 && p1 != p0);
    if (hoard1 && hoard2) {
        uint16_t P0 = p0i, P1 = thing_index(p1), H1 = thing_index(hoard1), H2 = thing_index(hoard2);
        int32_t total0 = p0->mana_total, total1 = p1->mana_total;
        struct Case { uint16_t a, b; int32_t rich0, rich1; uint16_t want; const char *what; } cases[] = {
            {0,  0,  0, 0, 0,  "both free"},
            {0,  P1, 0, 0, P1, "a free"},
            {P0, 0,  0, 0, P0, "b free"},
            {H1, H2, 0, 0, H1, "two hoards"},
            {H1, P1, 0, 0, P1, "hoard + player"},
            {P0, H2, 0, 0, P0, "player + hoard"},
            {P0, P0, 0, 0, P0, "same player"},
            {P0, P1, 500, 400, P0, "richer a"},
            {P0, P1, 400, 500, P1, "richer b"},
            {P0, P1, 400, 400, P1, "equal: b"},
        };
        for (const Case &c : cases) {
            Thing *a = thing_create(&site, 10, 0x27), *b = thing_create(&site, 10, 0x27);
            if (!a || !b) { CHECK(a && b); break; }
            a->mana = 300; b->mana = 500;
            a->mana_owner = c.a; b->mana_owner = c.b;
            p0->mana_total = c.rich0; p1->mana_total = c.rich1;
            int free0 = thing_free_count();
            mana_ball_merge(a, b);
            CHECK_EQ(a->mana, 800);
            if (a->mana_owner != c.want) std::printf("FAIL merge case '%s': owner %d, expected %d\n", c.what, a->mana_owner, c.want), g_fail++;
            CHECK_EQ(b->cls, 0);                        // freed at once
            CHECK_EQ(thing_free_count(), free0 + 1);
            thing_free(a);
        }
        p0->mana_total = total0; p1->mana_total = total1;
        std::printf("mana_ball_merge: %d ownership cases\n", (int)(sizeof cases / sizeof cases[0]));

        // --- the hoard marker (state 0x2a): a claim by a player passes its balls on
        Thing *b1 = thing_create(&site, 10, 0x27), *b2 = thing_create(&site, 10, 0x27);
        if (b1 && b2) {
            b1->mana_owner = H1; b2->mana_owner = H2;
            step(hoard1);
            CHECK(!deleted(hoard1));
            CHECK_EQ(hoard1->z, terrain_height_at(thing_pos(hoard1)));
            hoard1->damage_slots[1].attacker = P1;
            step(hoard1);
            CHECK(deleted(hoard1));
            CHECK_EQ(hoard1->damage_slots[1].attacker, 0);
            CHECK_EQ(b1->mana_owner, P1);
            CHECK_EQ(b2->mana_owner, H2);
            // claimed by something that is not a player: deleted, nothing passed on
            hoard2->damage_slots[1].attacker = H1;
            step(hoard2);
            CHECK(deleted(hoard2));
            CHECK_EQ(b2->mana_owner, H2);
            thing_free(b1); thing_free(b2);
        }
    }
    sweep();

    // --- two resting balls on the same spot merge through the handler; the sprite follows the mana
    Pos spot = make_site();
    Thing *a = thing_create(&spot, 10, 0x27), *b = thing_create(&spot, 10, 0x27);
    if (a && b) {
        CHECK_EQ(a->sprite, 0x35);                      // 0x200 mana: size 1
        a->z_vel = 0; b->z_vel = 0;
        step(a);
        CHECK_EQ(a->mana, 0x400);
        CHECK_EQ(b->cls, 0);
        CHECK_EQ(a->sprite, 0x36);
        static const struct { int mana, sprite; } sizes[] = {
            {1, 0x34}, {256, 0x34}, {257, 0x35}, {512, 0x35}, {1024, 0x36}, {2048, 0x37}, {4096, 0x38}, {9192, 0x39},
            {18384, 0x3a}, {18385, 0x3b}, {3100000, 0x3b},
        };
        for (const auto &s : sizes) {
            a->mana = s.mana;
            step(a);
            CHECK_EQ(a->sprite, s.sprite);
        }
        thing_free(a);
    }

    // --- the mana magnet (state 0x3b): a pull request on every ball within 0xe00
    Pos mg = make_site();
    Thing *magnet = thing_create(&mg, 10, 0x36);
    Pos near_pos = mg, far_pos = mg;
    near_pos.x = (uint16_t)(near_pos.x + 0xd00);
    far_pos.y = (uint16_t)(far_pos.y + 0xe10);
    Thing *nb = thing_create(&near_pos, 10, 0x27), *fb = thing_create(&far_pos, 10, 0x27);
    if (magnet && nb && fb) {
        CHECK_EQ(magnet->state, 0x3b);
        nb->next = thing_index(fb); fb->next = 0;
        g_cfg->mana_ball_list = thing_index(nb);
        int life = magnet->health;
        step(magnet);
        CHECK_EQ(nb->damage_slots[4].amount, 100);
        CHECK_EQ(nb->damage_slots[4].attacker, thing_index(magnet));
        CHECK_EQ(fb->damage_slots[4].attacker, 0);
        CHECK_EQ(1 + run_out(magnet), life + 2);
        g_cfg->mana_ball_list = 0;
        thing_free(nb); thing_free(fb);
    }
    sweep();
}

// The target height of a footprint instruction byte as 26f10 reads it (differs from 26320 for the
// terrace bytes); false = the byte leaves the height alone.
static bool raise_target(uint8_t b, int base, int *out) {
    if (b >= 0xf) {
        unsigned low = b % 16u;
        if (low == 0) return false;
        *out = base + (int)(low - 1) * 4;
        return true;
    }
    if (b <= 6) return false;
    *out = base;
    return true;
}

static void test_castle_effects() {
    // footprint table: the raise effect centres the sub-footprints with w / h swapped
    int not_square = 0;
    for (unsigned s = 0; s < 8; s++) {
        const CastleFootprint *fp = castle_footprint(s);
        if (fp->w != fp->h) not_square++;
        std::printf("%sfootprint %u: %ux%u", s ? ", " : "castle ", s, fp->w, fp->h);
    }
    std::printf("\n");
    CHECK_EQ(not_square, 0);

    Thing *castle = thing_alloc();
    CHECK(castle != nullptr);
    if (!castle) return;
    castle->cls = 3; castle->type = 2; castle->state = 4;

    // --- 26b50: level the footprint rectangle to the average of its surroundings
    {
        Pos p = make_site();
        p.x &= 0xff00; p.y &= 0xff00;                 // on the cell corner: (x + 0x80) >> 8 is this cell
        unsigned cx = p.x >> 8, cy = p.y >> 8;
        Thing *e = thing_create(&p, 10, 0x29);
        CHECK(e != nullptr);
        if (e) {
            CHECK_EQ(e->state, 0x2b);
            e->caster = thing_index(castle);
            e->castle_size = 1;
            const CastleFootprint *fp = castle_footprint(1);
            unsigned w = fp->w, h = fp->h, x0 = cx - w / 2, y0 = cy - h / 2;
            // the site is flat at `flat`; the effect starts from a base 12 height bytes lower
            int flat = g_map_height[cell_xy(cx, cy)];
            e->z = (int16_t)((flat - 12) << 5);
            g_map_flags[cell_xy(x0 + 1, y0 + 1)] |= 0x80;
            std::vector<uint8_t> before(g_map_height, g_map_height + MC_MAP_CELLS);
            step(e);
            CHECK_EQ(e->aux, 10);
            CHECK_EQ(e->cast_ticks, flat - 12);
            CHECK_EQ(e->damage, flat);                 // terrain_height_avg of the (flat) surroundings
            int steps = 0;
            while (e->aux > 0 && steps < 50) { step(e); steps++; }
            CHECK_EQ(steps, 10);
            CHECK_EQ(e->aux, -10);
            CHECK_EQ(e->cast_ticks, flat);
            bool ok = true;
            for (unsigned r = 0; r < h; r++)
                for (unsigned c = 0; c < w; c++) {
                    unsigned cell = cell_xy(x0 + c, y0 + r);
                    if (g_map_height[cell] != (uint8_t)(before[cell] + 12)) ok = false;
                }
            CHECK(ok);
            CHECK_EQ(g_map_height[cell_xy(x0 - 1, y0)], before[cell_xy(x0 - 1, y0)]);
            CHECK_EQ(g_map_height[cell_xy(x0 + w, y0)], before[cell_xy(x0 + w, y0)]);
            CHECK_EQ(g_map_flags[cell_xy(x0 + 1, y0 + 1)] & 0x88, 8);      // parked for the wait
            int wait = 0;
            while (e->aux != 0 && wait < 50) { step(e); wait++; }
            CHECK_EQ(wait, 10);
            CHECK_EQ(g_map_flags[cell_xy(x0 + 1, y0 + 1)] & 0x88, 0x80);   // and back
            CHECK(!deleted(e));
            castle->cast_ticks = 0;
            step(e);
            CHECK(deleted(e));
            CHECK_EQ(castle->cast_ticks, 2);
            CHECK_EQ(castle->home.z, flat << 5);
            g_map_flags[cell_xy(x0 + 1, y0 + 1)] &= 0x7f;
            std::printf("castle ground levelling (26b50): %ux%u cells raised by 12 in 10 steps, 10 ticks wait, 22 handler calls\n", w, h);
        }
        // already level: hands back on the second call; a hit castle: at once
        Pos p2 = make_site();
        p2.x &= 0xff00; p2.y &= 0xff00;
        Thing *e2 = thing_create(&p2, 10, 0x29);
        if (e2) {
            e2->caster = thing_index(castle);
            e2->castle_size = 1;
            castle->cast_ticks = 0;
            step(e2);
            CHECK_EQ(e2->aux, 0);
            step(e2);
            CHECK(deleted(e2));
            CHECK_EQ(castle->cast_ticks, 2);
        }
        sweep();
    }

    // --- 26f10: raise the castle of size N out of the ground
    for (int size = 1; size <= 3; size++) {
        Pos p = make_site();
        p.x &= 0xff00; p.y &= 0xff00;
        unsigned cx = p.x >> 8, cy = p.y >> 8;
        Thing *e = thing_create(&p, 10, 0x2a);
        CHECK(e != nullptr);
        if (!e) continue;
        CHECK_EQ(e->state, 0x2c);
        CHECK_EQ(e->spell_flags, 1);
        e->caster = thing_index(castle);
        e->castle_size = (uint8_t)size;
        int base = g_map_height[cell_xy(cx, cy)];
        e->z = (int16_t)(base << 5);
        // expected heights: the footprints 1..size laid over each other
        std::vector<int> want(MC_MAP_CELLS, -1);
        int painted = 0;
        for (int s = 1; s <= size; s++) {
            const CastleFootprint *fp = castle_footprint((unsigned)s);
            unsigned x0 = cx - fp->w / 2, y = cy - fp->h / 2, x = x0, rows = fp->h;
            for (uint32_t i = 0; rows != 0 && i < fp->map_len;) {
                int8_t c = (int8_t)fp->map[i++];
                if (c == 0) { y++; x = x0; rows--; }
                else if (c < 0) x -= c;
                else for (int n = c; n != 0; n--, x++) {
                    int target;
                    if (raise_target(fp->map[i++], base, &target)) { if (want[cell_xy(x, y)] < 0) painted++; want[cell_xy(x, y)] = target; }
                }
            }
        }
        std::vector<uint8_t> height_before(g_map_height, g_map_height + MC_MAP_CELLS);
        std::vector<uint8_t> flags_before(g_map_flags, g_map_flags + MC_MAP_CELLS), type_before(g_map_type, g_map_type + MC_MAP_CELLS);
        castle->cast_ticks = 0;
        castle->duration = 0;
        int calls = 0, paused = 0;
        // a hit castle pauses the work (the step counter still runs)
        step(e); calls++;
        CHECK_EQ(e->aux, 0x12);
        castle->duration = 5;
        std::vector<uint8_t> snap_h(g_map_height, g_map_height + MC_MAP_CELLS);
        step(e); calls++; paused++;
        CHECK_EQ(e->aux, 0x11);
        CHECK(std::memcmp(snap_h.data(), g_map_height, MC_MAP_CELLS) == 0);
        castle->duration = 0;
        while (e->aux > 0 && calls < 100) { step(e); calls++; }
        CHECK_EQ(calls, 0x13);
        CHECK_EQ(e->aux, -0x19);
        const CastleFootprint *top = castle_footprint((unsigned)size);
        auto rect_count = [&](uint8_t mask, uint8_t value) {
            int n = 0;
            for (unsigned r = 0; r < top->h; r++)
                for (unsigned c = 0; c < top->w; c++)
                    if ((g_map_flags[cell_xy(cx - top->w / 2 + c, cy - top->h / 2 + r)] & mask) == value) n++;
            return n;
        };
        int wrong = 0;
        for (int c = 0; c < MC_MAP_CELLS; c++)
            if (want[c] >= 0 && g_map_height[c] != (uint8_t)want[c]) wrong++;
        CHECK_EQ(wrong, 0);
        // after the last step the built-on cells whose height moved are parked: 0x80 off, bit 3 on
        // (cells painted without a height difference keep 0x80)
        int parked = rect_count(0x88, 0x08), kept = rect_count(0x88, 0x80);
        CHECK(parked > 0);
        int wait = 0;
        while (!deleted(e) && wait < 100) { step(e); wait++; }
        CHECK_EQ(wait, 0x19);
        CHECK_EQ(castle->cast_ticks, 2);
        int built = rect_count(0x88, 0x80);
        CHECK_EQ(built, parked + kept);
        CHECK_EQ(rect_count(0x08, 0x08), 0);
        // The same castle stamped at once, one footprint per level as player_spawn_3f360 does for a
        // level-start castle (castle_stamp_footprint_26320). The two functions read the terrace bytes
        // (high nibble 3) differently, so this is a comparison, not an identity.
        std::vector<uint8_t> raised(g_map_height, g_map_height + MC_MAP_CELLS);
        std::vector<uint8_t> raised_flags(g_map_flags, g_map_flags + MC_MAP_CELLS), raised_type(g_map_type, g_map_type + MC_MAP_CELLS);
        std::memcpy(g_map_height, height_before.data(), MC_MAP_CELLS);
        std::memcpy(g_map_flags, flags_before.data(), MC_MAP_CELLS);
        std::memcpy(g_map_type, type_before.data(), MC_MAP_CELLS);
        Thing stamp{};
        stamp.x = p.x; stamp.y = p.y; stamp.z = (int16_t)(base << 5);
        for (int s = 1; s <= size; s++) {
            stamp.castle_size = (uint8_t)s;
            castle_stamp_footprint(&stamp);
        }
        int same_h = 0, diff_h = 0, same_tex = 0, diff_tex = 0;
        for (int c = 0; c < MC_MAP_CELLS; c++) {
            if (want[c] < 0) continue;
            if (raised[c] == g_map_height[c]) same_h++; else diff_h++;
            if (raised_type[c] == g_map_type[c]) same_tex++; else diff_tex++;
        }
        CHECK_EQ(diff_h, 0);                           // the grown castle has the heights of the stamped one
        std::memcpy(g_map_height, raised.data(), MC_MAP_CELLS);
        std::memcpy(g_map_flags, raised_flags.data(), MC_MAP_CELLS);
        std::memcpy(g_map_type, raised_type.data(), MC_MAP_CELLS);
        std::printf("castle raise (26f10) size %d: %d cells with a target height all reached in 18 steps (1 paused), %d built-on cells, "
                    "%d handler calls; against castle_stamp_footprint of sizes 1..N: height equal on %d cells, different on %d; "
                    "texture equal on %d, different on %d\n", size, painted, built, calls + wait, same_h, diff_h, same_tex, diff_tex);
        sweep();
    }

    // --- 27d20: the castle seed
    {
        Thing *owner = local_player_thing();
        PlayerBlock *P = player_block(owner);
        uint16_t castle_saved = P->castle;
        Pos p = make_site();
        castle->x = p.x; castle->y = p.y; castle->z = p.z;
        castle->ext_x = castle->ext_y = 0x400; castle->ext_h = 0x4000; castle->ext_z0 = -0x2000;
        std::memset(castle->damage_slots, 0, sizeof castle->damage_slots);
        P->castle = thing_index(castle);
        Thing *seed = thing_create(&p, 10, 0x2b);
        CHECK(seed != nullptr);
        if (seed) {
            CHECK_EQ(seed->state, 0x2d);
            seed->owner = thing_index(owner);
            step(seed);
            CHECK(deleted(seed));
            CHECK_EQ(castle->damage_slots[5].amount, 10);
            CHECK_EQ(castle->damage_slots[5].attacker, thing_index(owner));
        }
        // far from the castle: the Castle spell (P+0x2c4 = spell_thing[16]) is reset
        uint16_t spell_saved = P->spell_thing[16];
        Thing *spell = thing_alloc();                   // stands in for the owner's Castle spell thing
        CHECK(spell != nullptr);
        if (!spell) return;
        spell->cls = 12; spell->type = 16; spell->state = 4 * 0;
        P->spell_thing[16] = thing_index(spell);
        spell->cast_ticks = 7;
        std::memset(castle->damage_slots, 0, sizeof castle->damage_slots);
        Pos away = make_site();
        Thing *seed2 = thing_create(&away, 10, 0x2b);
        if (seed2) {
            seed2->owner = thing_index(owner);
            step(seed2);
            CHECK(deleted(seed2));
            CHECK_EQ(castle->damage_slots[5].attacker, 0);
            CHECK_EQ(spell->cast_ticks, 0);
            std::printf("castle seed: upgrade request filed on contact; off the castle the owner's Castle spell is reset\n");
        }
        P->spell_thing[16] = spell_saved;
        thing_free(spell);
        P->castle = castle_saved;
    }
    thing_free(castle);
    sweep();

    // --- 27e90 (disabled record): the boulder rolls downhill and damages
    {
        Pos p = make_site();
        Thing *v = make_victim(&p);
        Thing *b = thing_create(&p, 10, 0x2c);
        CHECK(b != nullptr);
        if (b && v) {
            CHECK_EQ(b->state, 0x2e);
            CHECK(!thing_table_a_enabled(10, 0x2e));
            b->flags |= 8;
            thing_set_extents(b, 0x100, 0x400);
            b->home.x = 0x90; b->home.y = (uint16_t)-0x200;
            Pos at = *thing_pos(b);
            step(b);
            CHECK_EQ((int16_t)b->home.x, 0x80); CHECK_EQ((int16_t)b->home.y, -0x80);    // clamped
            CHECK_EQ(b->x, (uint16_t)(at.x + 0x80)); CHECK_EQ(b->y, (uint16_t)(at.y - 0x80));
            CHECK_EQ(b->z, terrain_height_at(thing_pos(b)));
            CHECK_EQ(v->damage_slots[0].amount, b->damage);
            thing_mark_delete(b);
        }
        if (v) thing_mark_delete(v);
        sweep();
    }
}

static void test_spell_effects() {
    // crab egg (state 0x38 -> 0x39)
    Pos p = make_site();
    Thing *egg = thing_create(&p, 10, 0x34);
    CHECK(egg != nullptr);
    if (egg) {
        CHECK_EQ(egg->state, 0x38);
        int wait = egg->aux, n = 0;
        while (egg->state == 0x38 && n < 5000) { step(egg); n++; }
        CHECK_EQ(n, wait + 1);
        CHECK_EQ(egg->state, 0x39);
        CHECK_EQ(egg->max_health, 0x1388);
        uint32_t count0 = g_state->creature_count;
        PoolMark m; m.take();
        step(egg);
        CHECK(deleted(egg));
        std::vector<Thing *> crab = m.fresh(5, 5), bang = m.fresh(10, 1);
        CHECK_EQ((int)crab.size(), 1); CHECK_EQ((int)bang.size(), 1);
        CHECK_EQ(g_state->creature_count, count0 + 1);
        if (!crab.empty() && !bang.empty()) CHECK_EQ(bang[0]->owner, crab[0]->owner);
        std::printf("crab egg: hatched after %d ticks into a crab and a big explosion\n", n);
        g_state->creature_count = count0;
        for (Thing *t : m.fresh()) thing_mark_delete(t);
    }
    sweep();

    // fire pillar (state 0x3a)
    Pos fp = make_site();
    Thing *pillar = thing_create(&fp, 10, 0x35);
    CHECK(pillar != nullptr);
    if (pillar) {
        CHECK_EQ(pillar->state, 0x3a);
        pillar->damage = 0x1000;
        Thing *v = make_victim(&fp);
        int cells = spiral_cells(0, 1), fires = 0, calls = 0;
        while (!deleted(pillar) && calls < 100) {
            PoolMark m; m.take();
            int16_t aux = pillar->aux;
            uint32_t seed = pillar->rng;
            step(pillar);
            calls++;
            CHECK_EQ(lcg_steps(seed, pillar->rng), 1 + 2 * cells);
            CHECK_EQ(pillar->ext_x, 0x200); CHECK_EQ(pillar->ext_h, 0x800);
            std::vector<Thing *> made = m.fresh(10, 6);
            CHECK_EQ((int)made.size(), cells);
            for (Thing *f : made) {
                fires++;
                CHECK_EQ(f->health, aux == 0 ? 0xe : 1);
                CHECK_EQ(f->z_vel, aux * 0x80);
                CHECK_EQ(f->aux, 7);
                CHECK_EQ(f->damage, 0x1000);
                CHECK_EQ(f->sprite, 0xe4 + 7);
                CHECK((f->flags & 0x10080) == 0x10080);
                thing_mark_delete(f);
            }
            sweep();
        }
        CHECK_EQ(calls, 15);
        if (v) {
            CHECK_EQ(v->damage_slots[0].amount, 15 * (0x1000 / pillar->max_health));
            thing_mark_delete(v);
        }
        std::printf("fire pillar: 15 ticks, %d fires (%d cells per tick), %d damage per tick\n", fires, cells, 0x1000 / pillar->max_health);
    }
    sweep();

    // the delayed blast (state 0x3c)
    Pos bp = make_site();
    Thing *blast = thing_create(&bp, 10, 0x37);
    CHECK(blast != nullptr);
    if (blast) {
        CHECK_EQ(blast->state, 0x3c);
        Thing *owner = local_player_thing();
        blast->owner = thing_index(owner);
        Pos near_pos = bp, far_pos = bp;
        near_pos.x = (uint16_t)(near_pos.x + 0x400);
        far_pos.x = (uint16_t)(far_pos.x + 0x480);
        far_pos.y = (uint16_t)(far_pos.y + 0xa00);
        Thing *near_creature = make_victim(&near_pos), *far_creature = make_victim(&far_pos), *own = make_victim(&near_pos);
        Thing *enemy = thing_at(g_state->players[(g_state->local_player + 1) & 3].thing);
        Pos enemy_home = *thing_pos(enemy);
        DamageSlot enemy_slot = enemy->damage_slots[0];
        Pos ep = bp;
        ep.y = (uint16_t)(ep.y + 0x300);
        thing_move_to(enemy, &ep);
        std::memset(&enemy->damage_slots[0], 0, sizeof(DamageSlot));
        if (near_creature && far_creature && own) {
            own->owner = blast->owner;
            int fuse = blast->aux, n = 0;
            s_sounds.clear();
            while (blast->aux != 0 && n < 1000) { step(blast); n++; }
            CHECK_EQ(n, fuse);
            CHECK_EQ(sounds_of(0x2b), fuse);
            CHECK(near_creature->health >= 0);
            step(blast);
            CHECK(deleted(blast));
            CHECK_EQ(near_creature->health, -1);
            CHECK(far_creature->health >= 0);
            CHECK(own->health >= 0);
            CHECK_EQ(enemy->damage_slots[0].amount, blast->damage);
            CHECK_EQ(enemy->damage_slots[0].attacker, blast->owner);
            CHECK_EQ(sounds_of(0x2c), 2);
            CHECK_EQ(g_cfg->palette_effect, 3);
            g_cfg->palette_effect = 0;
            std::printf("blast: fuse %d ticks, kills within 0xa00, %d damage to the enemy wizard\n", fuse, blast->damage);
            thing_mark_delete(near_creature); thing_mark_delete(far_creature); thing_mark_delete(own);
        }
        thing_move_to(enemy, &enemy_home);
        enemy->damage_slots[0] = enemy_slot;
    }
    sweep();
}

// ---- 3. level start versus the engine's snapshot ---------------------------------------------------

static std::vector<uint8_t> g_snap_gam;
static const GameStateImage *snap() { return reinterpret_cast<const GameStateImage *>(g_snap_gam.data()); }
static bool load_raw_snapshot(const char *game_dir) {
    char path[1024];
    mc_blob b;
    mc_path_join(path, sizeof path, game_dir, "movie/gam00000.dat");
    if (!mc_read_file(path, &b)) return false;
    g_snap_gam.assign(b.data, b.data + b.len);
    mc_blob_free(&b);
    return g_snap_gam.size() == MC_STATE_IMAGE_SIZE;
}

// Test-only stand-in for creature_wake_tick_468e0 / creature_proximity_wake_timer_46960 (task E),
// restricted to the mana-ball list: the awake gate timer_a counts down; at 0 a ball within 0x1800 of
// the local player's thing is woken for 16 ticks and notes the distance in cast_ticks.
static int s_wake_mode = 0;          // 0 = as the original, 2 = always awake
static void test_wake_tick() {
    const Thing *lp = local_player_thing();
    int guard = 0;
    for (uint32_t i = g_cfg->mana_ball_list; i != 0 && i < MC_THING_SLOTS && guard < MC_THING_SLOTS; guard++) {
        Thing *b = thing_at(i);
        i = b->next;
        if (s_wake_mode == 2) { b->timer_a = 0x80; continue; }
        if (b->timer_a != 0) { b->timer_a--; continue; }
        if (b->timer_b != 0) { b->timer_b--; continue; }
        int d2 = pos_dist_sq_xy(thing_pos(b), thing_pos(lp));
        if (d2 < 0x2400000) {
            b->cast_ticks = (int16_t)mc_isqrt((uint32_t)d2);
            b->timer_a = 0x10;
        }
        b->timer_b = 0;
    }
}

// gate = false: the same run with the balls always awake (no CHECKs; shows that the awake gate matters).
static void test_level_start_vs_snapshot(bool gate) {
    const GameStateImage *s = snap();
    if (!sim_load_level(38)) { std::printf("level 38 failed to load\n"); g_fail++; return; }
    // The crater of the second volcano is thing 503 in the snapshot and would be 501 here (the
    // original run had two more things alive by tick 18): two placeholders line the indices up, so
    // that thing_alloc seeds its RNG with the same value (index + GameState.rng).
    for (int k = 0; k < 2; k++) {
        Thing *d = thing_alloc();
        if (d) { d->cls = 10; d->type = 4; d->state = 4; }
    }
    s_wake_mode = gate ? 0 : 2;
    g_hook_creature_wake_tick = test_wake_tick;
    bool start_ball[MC_THING_SLOTS] = {};
    int start_balls = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++)
        if (thing_at(i)->cls == 10 && thing_at(i)->type == 0x27) { start_ball[i] = true; start_balls++; }
    CHECK_EQ(start_balls, 80);
    for (int tick = 0; tick < 412; tick++) game_tick_sim();
    g_hook_creature_wake_tick = nullptr;

    int both = 0, pos_eq = 0, vel_eq = 0, all_eq = 0, timer_eq = 0, merged_same = 0, sim_only = 0, snap_only_original = 0;
    int sim_left = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing *a = thing_at(i);
        const ThingImage *b = &s->things[i];
        bool sa = a->cls == 10 && a->type == 0x27, sb = b->cls == 10 && b->type == 0x27;
        if (sa) sim_left++;
        if (!start_ball[i]) continue;
        // a start ball that is unowned in the snapshot and sits where it was dropped at level start is the same ball
        if (sa && !sb) sim_only++;
        if (!sa && sb && b->mana_owner == 0) snap_only_original++;
        if (!sa || !sb) continue;
        both++;
        bool p = a->x == b->x && a->y == b->y && a->z == b->z;
        bool v = a->home.x == b->home.x && a->home.y == b->home.y && a->z_vel == b->z_vel;
        bool rest = a->mana == b->mana && a->sprite == b->sprite && a->flags == b->flags && a->mana_owner == b->mana_owner
                 && a->tick == b->tick && a->yaw == b->yaw && a->state == b->state && a->ext_x == b->ext_x && a->ext_h == b->ext_h;
        if (p) pos_eq++;
        if (v) vel_eq++;
        if (p && v && rest) all_eq++;
        if (a->timer_a == b->timer_a) timer_eq++;
        if (p && a->mana == b->mana && a->mana > 0x200) merged_same++;
        if (gate && !(p && v && rest))
            std::printf("  ball #%d differs: sim pos %04x %04x %d v %d %d zv %d mana %d owner %d sprite %x | snapshot pos %04x %04x %d v %d %d zv %d mana %d owner %d sprite %x\n",
                        i, (unsigned)a->x, (unsigned)a->y, a->z, (int16_t)a->home.x, (int16_t)a->home.y, a->z_vel, a->mana, a->mana_owner, a->sprite,
                        (unsigned)b->x, (unsigned)b->y, b->z, (int16_t)b->home.x, (int16_t)b->home.y, b->z_vel, b->mana, b->mana_owner, b->sprite);
    }
    std::printf("level 38 start + 412 ticks vs snapshot, mana balls: %d at start, %d left in the port's run, %d slots hold a ball in both; "
                "position equal %d, velocity equal %d, every compared field equal %d, awake timer equal %d, merged balls with equal mana %d; "
                "start balls only in the port %d, unowned start balls only in the snapshot %d\n",
                start_balls, sim_left, both, pos_eq, vel_eq, all_eq, timer_eq, merged_same, sim_only, snap_only_original);
    if (!gate) {
        std::printf("  (that was the run without the awake gate: every ball updated on every tick)");
        std::putchar(10);
        return;
    }
    // 39 slots; #356 was reused by a later ball in the original run, #385 / #386 were claimed by an AI wizard there
    CHECK_EQ(both, 39);
    CHECK_EQ(pos_eq, 38);
    CHECK_EQ(vel_eq, 38);
    CHECK_EQ(all_eq, 36);
    CHECK_EQ(timer_eq, 39);
    CHECK(merged_same >= 5);
    CHECK_EQ(snap_only_original, 0);                   // the port never merges a ball the original kept

    // the erupting crater
    const Thing *a = thing_at(503);
    const ThingImage *b = &s->things[503];
    CHECK(a->cls == 10 && a->type == 0x12 && b->cls == 10 && b->type == 0x12);
    std::printf("crater #503: port aux %d rng %08x yaw %04x tick %d health %d pos %04x %04x %d | snapshot aux %d rng %08x yaw %04x tick %d health %d pos %04x %04x %d\n",
                a->aux, a->rng, a->yaw, a->tick, a->health, (unsigned)a->x, (unsigned)a->y, a->z, b->aux, b->rng, b->yaw, b->tick,
                b->health, (unsigned)b->x, (unsigned)b->y, b->z);
    CHECK_EQ(a->aux, b->aux);
    CHECK_EQ(a->rng, b->rng);
    CHECK_EQ(a->yaw, b->yaw);
    CHECK_EQ(a->tick, b->tick);
    CHECK_EQ(a->health, b->health);
    CHECK(a->x == b->x && a->y == b->y && a->z == b->z);
    CHECK_EQ(a->flags, b->flags);
    CHECK_EQ(a->owner, b->owner);
    uint16_t sv;
    std::memcpy(&sv, g_snap_gam.data() + 0x24, 2);
    CHECK_EQ(state_word(0x24), sv);                    // GameState+0x24: the erupting volcano
    std::printf("GameState+0x24 (erupting volcano): port %d, snapshot %d\n", state_word(0x24), sv);
    histogram("  port after 412 ticks");
    {
        int n = 0;
        for (int i = 1; i < MC_THING_SLOTS; i++) if (s->things[i].cls == 10 && s->things[i].type == 0x2d) n++;
        std::printf("  (wizard castles in the snapshot: %d; the port's lava blobs get other pool slots, hence other seeds and landing places)\n", n);
    }
}

// ---- 4. the snapshot's effects stepped ---------------------------------------------------------------

static void test_snapshot_run() {
    if (!sim_load_snapshot("movie/gam00000.dat", "movie/map00000.dat")) { std::printf("snapshot failed to load\n"); g_fail++; return; }
    histogram("snapshot");
    int balls = 0, sprite_ok = 0, at_rest = 0, on_ground = 0, awake = 0, thrown = 0;
    long long mana0 = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing *t = thing_at(i);
        if (t->cls != 10 || t->type != 0x27) continue;
        balls++;
        mana0 += t->mana;
        if (t->sprite == expected_ball_sprite(t, g_state)) sprite_ok++;
        else std::printf("  ball #%d: sprite %x, expected %x (mana %d owner %d)\n", i, t->sprite, expected_ball_sprite(t, g_state), t->mana, t->mana_owner);
        if (t->home.x == 0 && t->home.y == 0) at_rest++;
        if (t->z == (int16_t)terrain_height_at(thing_pos(t))) on_ground++;
        if (t->timer_a) awake++;
        CHECK(std::abs((int16_t)t->home.x) <= 0x40 && std::abs((int16_t)t->home.y) <= 0x40);
        CHECK(t->z_vel >= -0x80);
        CHECK_EQ(t->state, 0x29);
        // speed_cur: 0x20 from the constructor, 0x10 + rnd % 0x30 when thing_drop_mana_ball threw it
        CHECK(t->speed_cur >= 0x10 && t->speed_cur <= 0x3f);
        CHECK(t->yaw < 0x800);
        if (t->speed_cur != 0x20 || t->yaw != 0) thrown++;
    }
    std::printf("snapshot mana balls: %d, sprite = base(owner) + size(mana) for %d, on the ground %d, zero velocity %d, awake %d, "
                "with a thrown ball's yaw / speed %d (speed 0x10..0x3f for all)\n", balls, sprite_ok, on_ground, at_rest, awake, thrown);
    CHECK_EQ(balls, 54);
    CHECK_EQ(sprite_ok, balls);
    CHECK(on_ground >= balls - 5);                      // a few are in flight (dropped moments before)

    // a ball that rests (zero velocity) on the ground is a fixed point of the handler apart from the hop of z_vel
    int fixed = 0, resting = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        Thing *t = thing_at(i);
        if (t->cls != 10 || t->type != 0x27 || t->home.x != 0 || t->home.y != 0) continue;
        int16_t slope[2];
        terrain_slope_vector(thing_pos(t), slope);
        if (slope[0] != 0 || slope[1] != 0) continue;
        resting++;
        Thing copy = *t;
        uint8_t timer = t->timer_a;
        t->timer_a = 1;
        // (no neighbour to merge with: checked through the mana)
        thing_update_fn(10, 0x29)(t);
        bool same = t->x == copy.x && t->y == copy.y && t->z == copy.z && t->home.x == 0 && t->home.y == 0 && t->sprite == copy.sprite
                 && (t->mana != copy.mana || (t->z_vel == 0 || t->z_vel == -0x10));
        if (same) fixed++;
        if (t->mana == copy.mana) { t->z_vel = copy.z_vel; }
        t->timer_a = timer;
    }
    std::printf("snapshot mana balls at rest on level ground: %d, unchanged by one handler call: %d\n", resting, fixed);
    CHECK_EQ(fixed, resting);

    // reload (the calls above may have merged balls) and run the whole pool
    if (!sim_load_snapshot("movie/gam00000.dat", "movie/map00000.dat")) { g_fail++; return; }
    s_wake_mode = 0;
    g_hook_creature_wake_tick = test_wake_tick;
    thing_dispatch_reset_stats();
    int merges = 0;
    for (int tick = 0; tick < 600; tick++) {
        int before = 0, after = 0;
        for (int i = 1; i < MC_THING_SLOTS; i++) if (thing_at(i)->cls == 10 && thing_at(i)->type == 0x27) before++;
        thing_update_all();
        for (int i = 1; i < MC_THING_SLOTS; i++) if (thing_at(i)->cls == 10 && thing_at(i)->type == 0x27) after++;
        merges += before - after;
    }
    g_hook_creature_wake_tick = nullptr;
    long long mana1 = 0;
    int balls1 = 0, live = 0, linked_ok = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing *t = thing_at(i);
        if (t->cls == 0) continue;
        live++;
        if (t->cls != 10 || t->type != 0x27) continue;
        balls1++;
        mana1 += t->mana;
        CHECK(t->z >= (int16_t)terrain_height_at(thing_pos(t)));
        CHECK_EQ(t->sprite, expected_ball_sprite(t, g_state));
        // the ball is in the cell list of its cell
        bool found = false;
        int guard = 0;
        for (uint16_t j = g_cell_things[mc_cell_of(t->x, t->y)]; j != 0 && guard < MC_THING_SLOTS; j = thing_at(j)->cell_next, guard++)
            if (j == i) found = true;
        if (found) linked_ok++;
    }
    CHECK_EQ(linked_ok, balls1);
    CHECK_EQ(mana1, mana0);                             // merging conserves mana; nothing else is ported here
    CHECK_EQ(balls - balls1, merges);
    CHECK_EQ(live + thing_free_count(), MC_THING_SLOTS - 1);
    std::printf("snapshot + 600 thing_update_all: %d balls (%d merges), total ball mana %lld -> %lld, %d live things + %d free = %d\n",
                balls1, merges, mana0, mana1, live, thing_free_count(), live + thing_free_count());
    histogram("  after 600 ticks");
}

// ---- 5. smoke runs ---------------------------------------------------------------------------------

static void smoke_run(int level, int ticks) {
    if (!sim_load_level(level)) { std::printf("level %d failed to load\n", level); g_fail++; return; }
    s_wake_mode = 0;
    g_hook_creature_wake_tick = test_wake_tick;
    thing_dispatch_reset_stats();
    int seen[64] = {}, peak = 0;
    int start_effects = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) if (thing_at(i)->cls == 10) start_effects++;
    for (int tick = 0; tick < ticks; tick++) {
        game_tick_sim();
        int n = 0;
        bool now[64] = {};
        for (int i = 1; i < MC_THING_SLOTS; i++) {
            const Thing *t = thing_at(i);
            if (t->cls != 10) continue;
            n++;
            now[t->type & 63] = true;
        }
        for (int k = 0; k < 64; k++) if (now[k]) seen[k]++;
        if (n > peak) peak = n;
    }
    g_hook_creature_wake_tick = nullptr;
    int live = 0, effects = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing *t = thing_at(i);
        if (t->cls == 0) continue;
        live++;
        if (t->cls == 10) effects++;
    }
    CHECK_EQ(live + thing_free_count(), MC_THING_SLOTS - 1);
    std::printf("level %d, %d ticks: effects %d -> %d (peak %d); ticks an effect type was present:", level, ticks, start_effects, effects, peak);
    for (int k = 0; k < 64; k++) if (seen[k]) std::printf(" 0x%02x:%d", k, seen[k]);
    std::printf("\n");
    // every class-10 handler this run dispatched must be ported
    int missing_fx = 0;
    for (int state = 0; state < 62; state++) {
        const DispatchRec &r = g_dispatch_a_cls10[state];
        if (r.handler != 0 && thing_update_fn(10, state) == nullptr) missing_fx++;
    }
    CHECK_EQ(missing_fx, 0);
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    const char *game_dir = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    if (!sim_init(game_dir)) { std::printf("sim_init failed\n"); return 2; }
    effects_register_handlers();
    g_hook_sound_request = record_sound;
    if (!load_raw_snapshot(game_dir)) { std::printf("movie/gam00000.dat missing\n"); return 2; }

    // every Table A record of class 10 has a port now
    int bound = 0, records = 0;
    for (int state = 0; state < 62; state++) {
        if (g_dispatch_a_cls10[state].handler == 0) continue;
        records++;
        if (thing_update_fn(10, state)) bound++;
        else std::printf("class 10 state %d (0x%05x %s) is not bound\n", state, g_dispatch_a_cls10[state].handler, g_dispatch_a_cls10[state].name);
    }
    std::printf("class 10 Table A: %d of %d records bound\n", bound, records);
    CHECK_EQ(bound, records);

    if (!sim_load_level(38)) { std::printf("level 38 failed to load\n"); return 2; }
    game_tick_sim();                                    // spawns the players
    histogram("level 38");
    test_helpers();
    test_explosions();
    test_fire_and_smoke();
    test_damage_effects();
    test_quake_lava_meteor();
    test_eruption();
    test_teleport_orbiter_storm_army();
    test_mana();
    test_castle_effects();
    test_spell_effects();

    test_level_start_vs_snapshot(true);
    test_level_start_vs_snapshot(false);
    test_snapshot_run();

    smoke_run(38, 2500);
    thing_dispatch_report(stdout);
    for (int level : {0, 5, 17, 30}) smoke_run(level, 2000);

    std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
