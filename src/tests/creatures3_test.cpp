// Unit / integration test for creatures3.cpp (class 5: emu, genie, type 15, wyvern). Built against the
// sim core plus the round-3 gameplay subsystems (see creatures3_test.cmake) so the creatures meet real
// projectiles, effects and castles; register_gameplay() below mirrors sim_all.cpp without the other
// round-4 subsystems. argv[1] = game dir; argv[2] = "quick" skips the long runs (movie, levels, genie
// scan); "probe <level>" prints what a level holds.
//
//  1. construction tests of the helpers and state bodies (hand-computed from the disassembly).
//  2. the snapshot (none of the four types is in it) and the sim_test replay with my handlers
//     registered (slot counts).
//  3. the shipped movie from the snapshot: per-tick invariants, census per state over time, pool
//     checks, dispatch report.
//  4. campaign levels 24 / 44 for 3000 ticks and the first level that spawns a genie (found by scanning
//     levels 0..69).
#include "sim.h"
#include "creatures3.h"
#include "level_features.h"
#include "player.h"
#include "demo.h"
#include "effects.h"
#include "projectiles.h"
#include "spells.h"
#include "castle.h"
#include "scenery.h"
#include "input.h"
#include "mc_math.h"
#include "crash_handler.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

// The whole simulation (sim_all.cpp): every gameplay subsystem incl. creatures3, as the game runs.
static void register_gameplay() { sim_register_gameplay(); }
static void prepare_movie() { sim_prepare_movie(); }

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_fail++; } } while (0)

static const char *g_game_dir = nullptr;

static bool is_creature(const Thing *t) { return t->cls == 5; }
static bool live(const Thing &t) { return t.cls != 0 && t.cls < 14; }

// The states this file owns (plus the +0 states bound in creatures.cpp, which the census shows too).
static bool my_type(int type) { return type == 10 || type == 11 || type == 15 || type == 16; }

static void census(const char *what) {
    int by_state[128] = {}, n = 0, mine = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing *t = thing_at(i);
        if (!is_creature(t)) continue;
        n++;
        if (!my_type(t->type)) continue;
        mine++;
        by_state[t->state & 127]++;
    }
    std::printf("%-26s %3d creatures, %3d of types 10/11/15/16, state:count", what, n, mine);
    for (int s = 0; s < 128; s++) if (by_state[s]) std::printf(" %d:%d", s, by_state[s]);
    std::printf("\n");
}

// Every live creature is in a state of its type and is linked into the cell list of its position;
// every cell chain is consistent.
static void check_pool(const char *what) {
    int bad_state = 0, bad_link = 0, bad_chain = 0, n = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing *t = thing_at(i);
        if (!is_creature(t)) continue;
        n++;
        int base = t->type * 6;
        bool ok = (t->state >= base && t->state <= base + 5) || t->state == 0x78 ||
                  (t->type == 0xc && t->state == 0x4f);
        if (!ok) { if (bad_state++ < 5) std::printf("  %s: thing %d type %d in state %d\n", what, i, t->type, t->state); }
        if (t->flags & 4) {
            unsigned cell = ((unsigned)(t->y >> 8) << 8) | (t->x >> 8);
            bool found = false;
            int guard = 0;
            for (unsigned k = g_cell_things[cell]; k != 0 && guard < MC_THING_SLOTS; k = thing_at(k)->cell_next, guard++)
                if (k == (unsigned)i) { found = true; break; }
            if (!found) { if (bad_link++ < 5) std::printf("  %s: thing %d not in the list of cell %04x\n", what, i, cell); }
        }
    }
    std::vector<int> seen(MC_THING_SLOTS, 0);
    for (int c = 0; c < MC_MAP_CELLS; c++) {
        int guard = 0; unsigned prev = 0;
        for (unsigned i = g_cell_things[c]; i != 0; i = thing_at(i)->cell_next) {
            if (i >= MC_THING_SLOTS || ++guard > MC_THING_SLOTS) { bad_chain++; break; }
            const Thing *t = thing_at(i);
            if (t->cls == 0 || !(t->flags & 4) || t->cell_prev != prev) bad_chain++;
            if (seen[i]++) bad_chain++;
            prev = i;
        }
    }
    if (bad_state || bad_link || bad_chain) {
        std::printf("FAIL pool (%s): %d bad states, %d bad links, %d chain problems\n", what, bad_state, bad_link, bad_chain);
        g_fail++;
    }
    CHECK(n > 0);
}

// Handlers of my states must never show up in the dispatch report.
static void check_dispatch(const char *what) {
    int missing = thing_dispatch_report(nullptr);
    std::printf("%s: handlers dispatched without a port: %d\n", what, missing);
    if (missing) thing_dispatch_report(stdout);
    // the report is by (class, state); re-run the walk for class 5 states 61..119
    for (int s = 61; s <= 119; s++) {
        if (s >= 72 && s <= 90) continue;         // builder / townie / trader / type 15 +0: creatures.cpp
        if (s == 60) continue;
        if (thing_update_fn(5, s) == nullptr && thing_table_a_enabled(5, s)) {
            std::printf("FAIL %s: class 5 state %d has no handler\n", what, s);
            g_fail++;
        }
    }
}

static int count_things(int cls, int type) {
    int n = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++)
        if (thing_at(i)->cls == cls && (type < 0 || thing_at(i)->type == type)) n++;
    return n;
}

static Thing *first_thing(int cls, int type) {
    for (int i = 1; i < MC_THING_SLOTS; i++)
        if (thing_at(i)->cls == cls && thing_at(i)->type == type) return thing_at(i);
    return nullptr;
}

static void delete_all(int cls, int type) {
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        Thing *t = thing_at(i);
        if (t->cls == cls && (type < 0 || t->type == type)) { thing_unlink_cell(t); thing_free(t); }
    }
}

static Pos pos_of(unsigned x, unsigned y) {
    Pos p; p.x = (uint16_t)x; p.y = (uint16_t)y; p.z = (int16_t)terrain_height_at(&p); return p;
}

// Spawn a creature of `type` at cell (cx, cy) with a fixed rng, awake, out of the dispatch loop.
static Thing *spawn_creature(int type, unsigned cx, unsigned cy, uint32_t rng = 12345) {
    Pos p = pos_of(cx * 0x100 + 0x80, cy * 0x100 + 0x80);
    Thing *t = thing_create(&p, 5, type);
    CHECK(t != nullptr);
    if (!t) return nullptr;
    t->rng = rng;
    t->timer_a = 0x10;
    t->tick = 0;
    return t;
}

static const Thing *player_thing(int p) { return thing_at(g_state->players[p].thing % MC_THING_SLOTS); }

// Load a level and run two ticks: the player things are spawned by the first tick and the per-class
// lists (g_cfg->player_list, wizard_list, ...) are rebuilt by thing_update_all.
static bool load_level_started(int lv) {
    void (*saved_input)() = g_hook_player_local_input;
    g_hook_player_local_input = nullptr;
    g_cfg->flags = 0; g_cfg->paused = 0;
    bool ok = sim_load_level(lv);
    if (ok) { game_tick_sim(); game_tick_sim(); }
    g_hook_player_local_input = saved_input;
    CHECK(ok);
    CHECK(g_state->players[0].thing != 0);
    return ok;
}

// First cell whose texture is one of the castle textures (terrain mask 0x20000: ids 0x15, 0x16, 0x18).
static unsigned find_castle_cell() {
    for (unsigned c = 0; c < MC_MAP_CELLS; c++) {
        uint8_t tex = g_map_type[c];
        if (tex == 0x15 || tex == 0x16 || tex == 0x18) return c;
    }
    return 0;
}

// ---- 1. construction tests ----------------------------------------------------------------------------

static void test_sprites_and_helpers() {
    if (!load_level_started(44)) return;
    Thing *g = spawn_creature(0xf, 100, 100, 0);
    if (!g) return;
    // type15_set_attack_sprite: rng 0 -> 0x24df = 9439, % 20 = 19 > 10 -> sprite 1
    type15_set_attack_sprite(g);
    CHECK_EQ(g->rng, 0x24dfu);
    CHECK_EQ(g->speed_cur, 0);
    CHECK_EQ(g->sprite, 1);
    // next draw: 0x24df * 0x24a1 + 0x24df = 0x546b11e = 88518942, % 20 = 2 -> 0xce
    type15_set_attack_sprite(g);
    CHECK_EQ(g->rng, 0x546b11eu);
    CHECK_EQ(g->sprite, 0xce);
    g->rng = 3;      // 3 -> 0x92c2 = 37570, % 20 = 10 (the boundary: not > 10) -> 0xce
    type15_set_attack_sprite(g);
    CHECK_EQ(g->rng, 0x92c2u);
    CHECK_EQ(g->sprite, 0xce);
    g->rng = 7;      // 7 -> 0x12546 = 75078, % 20 = 18 -> 1
    type15_set_attack_sprite(g);
    CHECK_EQ(g->sprite, 1);
    type15_set_move_sprite(g);
    CHECK_EQ(g->speed_cur, g->speed_base);
    CHECK_EQ(g->speed_cur, 0x1e);
    CHECK_EQ(g->sprite, 0);
    std::printf("type 15 desc: think %d sight %d fov %d terrain mask %08x\n", mc_move_desc(g->desc)->think_period,
                mc_move_desc(g->desc)->sight_radius, mc_move_desc(g->desc)->fov, mc_move_desc(g->desc)->terrain_mask);
    CHECK_EQ(mc_move_desc(g->desc)->terrain_mask, 0x20000u);
    thing_unlink_cell(g); thing_free(g);

    // genie_vanish / genie_appear_at_target
    Thing *n = spawn_creature(0xb, 100, 100, 7);
    Thing *w = spawn_creature(0x10, 110, 100, 8);          // stands in for a target with a yaw
    if (!n || !w) return;
    n->state = 0x44; n->aux = 9; n->target = thing_index(w);
    genie_vanish(n);
    CHECK_EQ(n->state, 0x42);
    CHECK_EQ(n->aux, 0);
    CHECK_EQ(n->target, 0);
    genie_appear_at_target(n);                              // no target: nothing happens
    CHECK_EQ(n->x, 100 * 0x100 + 0x80);
    n->state = 0x43; n->aux = 5; n->target = thing_index(w);
    w->yaw = 0x200;                                         // +x
    n->speed_cur = 0x3c;
    genie_appear_at_target(n);
    CHECK_EQ(n->state, 0x42);
    CHECK_EQ(n->aux, 0);
    // 0x3c << 6 = 0xf00 ahead of the target along +x
    CHECK_EQ((int16_t)(n->x - w->x), 0xf00);
    CHECK_EQ(n->y, w->y);
    // in the cell list of its new cell
    {
        unsigned cell = ((unsigned)(n->y >> 8) << 8) | (n->x >> 8);
        bool found = false;
        for (unsigned k = g_cell_things[cell]; k != 0; k = thing_at(k)->cell_next) if (k == thing_index(n)) found = true;
        CHECK(found);
    }
    thing_unlink_cell(n); thing_free(n);
    thing_unlink_cell(w); thing_free(w);
}

static void test_genie_states() {
    if (!load_level_started(44)) return;
    delete_all(10, 0); delete_all(10, 1);
    Thing *n = spawn_creature(0xb, 120, 120, 100);
    if (!n) return;
    CHECK_EQ(n->state, 0x42);
    CHECK_EQ(n->aux, 0);
    CHECK_EQ(n->flags & 1, 0);
    uint16_t x0 = n->x, y0 = n->y;
    int effects_before = count_things(10, 1);
    ThingUpdateFn s66 = thing_update_fn(5, 0x42);
    CHECK(s66 != nullptr);
    if (!s66) return;
    // first tick: 12 puffs on the 3 x 4 grid, aux = 1, hidden
    s66(n);
    CHECK_EQ(n->aux, 1);
    CHECK_EQ(n->flags & 1, 1);
    CHECK_EQ(n->state, 0x42);
    CHECK_EQ(count_things(10, 1) - effects_before, 12);
    int grid_hits = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing *e = thing_at(i);
        if (e->cls != 10 || e->type != 1) continue;
        int dx = (int16_t)(e->x - x0), dy = (int16_t)(e->y - y0);
        if (dx % 0x28 == 0 && dy % 0x28 == 0 && dx / 0x28 >= 0 && dx / 0x28 <= 2 && dy / 0x28 >= 0 && dy / 0x28 <= 3) grid_hits++;
        CHECK_EQ(e->owner, n->owner);
        CHECK_EQ(e->flags & 0x10000u, 0x10000u);
    }
    CHECK_EQ(grid_hits, 12);
    CHECK_EQ(n->rng, 100u);                                 // no rng draw on the first tick
    // second tick, hidden: random teleport by 0x3200 + (rng % 60) * 0x100 in x and y, state 67
    n->target = 5; n->damage_slots[0].attacker = 9;
    uint32_t r1 = mc_lcg(100), r2 = mc_lcg(r1);
    s66(n);
    CHECK_EQ(n->aux, 0);
    CHECK_EQ(n->state, 0x43);
    CHECK_EQ(n->target, 0);
    CHECK_EQ(n->damage_slots[0].attacker, 0);
    CHECK_EQ(n->rng, r2);
    CHECK_EQ((uint16_t)(n->x - x0), (uint16_t)(0x3200 + (r1 % 60) * 0x100));
    CHECK_EQ((uint16_t)(n->y - y0), (uint16_t)(0x3200 + (r2 % 60) * 0x100));
    // visible second tick -> state 68
    n->flags &= ~1u; n->aux = 1; n->state = 0x42;
    s66(n);
    CHECK_EQ(n->state, 0x44);
    // aux 5: just counts down
    n->aux = 5; n->state = 0x42;
    s66(n);
    CHECK_EQ(n->aux, 4);
    CHECK_EQ(n->state, 0x42);

    // state 67: regeneration on a think tick (period 10), damage by a class-3 thing -> appear at it
    ThingUpdateFn s67 = thing_update_fn(5, 0x43);
    CHECK(s67 != nullptr);
    if (!s67) return;
    n->state = 0x43; n->tick = 10; n->health = 1000; n->timer_a = 0;         // asleep: no enemy search
    n->rng = 100;
    uint16_t yaw_before = n->target_yaw;
    s67(n);
    CHECK_EQ(n->health, 1000 + (20000 >> 6));
    CHECK_EQ(n->rng, mc_lcg(mc_lcg(100)));                 // the wander turn's two draws
    CHECK(n->target_yaw != yaw_before);
    CHECK_EQ(n->state, 0x43);                               // health below 3/4: no mana-holder target
    n->tick = 11; n->health = 19000;
    s67(n);
    CHECK_EQ(n->health, 19000);                             // not a think tick
    n->health = 20000; n->tick = 10;
    s67(n);
    CHECK_EQ(n->health, 20000);                             // clamped to max_health
    // above 3/4 health on a think tick: the first player-list thing with mana becomes the target
    CHECK_EQ(n->state, 0x42);
    CHECK(n->target != 0);
    CHECK(thing_at(n->target)->mana != 0);
    // damage by a class-3 thing
    n->state = 0x43; n->timer_a = 0x10; n->health = 20000;
    const Thing *pt = player_thing(0);
    n->damage_slots[0].amount = 100; n->damage_slots[0].attacker = thing_index(pt);
    n->target = 0;
    s67(n);
    CHECK_EQ(n->health, 19900);
    CHECK_EQ(n->target, thing_index(pt));
    CHECK_EQ(n->state, 0x42);
    CHECK_EQ(n->aux, 0);
    // lethal damage -> state 70
    n->state = 0x43; n->health = 10; n->damage_slots[0].amount = 100; n->damage_slots[0].attacker = thing_index(pt);
    s67(n);
    CHECK_EQ(n->state, 0x46);
    CHECK_EQ(n->killer, thing_index(pt));

    // state 68: attack. Needs a target with a position: the local player's thing moved next to it.
    ThingUpdateFn s68 = thing_update_fn(5, 0x44);
    CHECK(s68 != nullptr);
    if (!s68) return;
    Thing *target = thing_at(g_state->players[0].thing % MC_THING_SLOTS);
    Pos tp = pos_of(n->x + 0x400, n->y);
    tp.z = (int16_t)(n->z + 0x100);
    thing_move_to(target, &tp);
    delete_all(9, -1);
    n->state = 0x44; n->health = 20000; n->target = thing_index(target); n->aux = 0; n->tick = 10; n->timer_a = 0x10;
    n->damage_slots[0].attacker = 0;
    s68(n);
    CHECK_EQ(n->state, 0x44);
    CHECK_EQ(n->aux, 1);
    CHECK_EQ(count_things(9, 8), 1);
    const Thing *p = first_thing(9, 8);
    if (p) {
        CHECK_EQ(p->impact_cls, 10);
        CHECK_EQ(p->impact_type, 0x19);
        CHECK_EQ(p->owner, n->owner);
        CHECK_EQ(p->damage, 0xbb8);
        CHECK_EQ(p->aux, 0x14);
        CHECK_EQ(p->target, thing_index(target));
        CHECK_EQ(p->yaw, (uint16_t)pos_angle_to(thing_pos(n), thing_pos(target)));
        CHECK_EQ(p->pitch, (uint16_t)pos_pitch_to(thing_pos(n), thing_pos(target)));
        CHECK_EQ(p->z, (int16_t)(n->z + n->ext_h));
    }
    // below half health -> vanish (and still a shot on the think tick, as the original does)
    n->health = 9000; n->tick = 10;
    s68(n);
    CHECK_EQ(n->state, 0x42);
    CHECK_EQ(n->aux, 1);                                    // aux reset to 0 by the vanish, then ++
    CHECK_EQ(count_things(9, 8), 2);
    // target out of sight on a think tick -> vanish, no shot
    n->state = 0x44; n->health = 20000; n->target = thing_index(target); n->tick = 10;
    Pos far_pos = pos_of(n->x + 0x2000, n->y);
    thing_move_to(target, &far_pos);
    s68(n);
    CHECK_EQ(n->state, 0x42);
    CHECK_EQ(count_things(9, 8), 2);
    // a hit by a class-3 thing retargets without moving
    n->state = 0x44; n->target = 0; n->tick = 3;
    n->damage_slots[0].amount = 1; n->damage_slots[0].attacker = thing_index(target);
    uint16_t x1 = n->x;
    s68(n);
    CHECK_EQ(n->target, thing_index(target));
    CHECK_EQ(n->x, x1);
    CHECK_EQ(n->state, 0x44);

    // genie_steal_mana: the nearest type-0x27 mana ball in sight
    delete_all(10, 0x27); delete_all(10, 0);
    n->mana = 100; n->mana_total = 100000;
    Pos bp = pos_of(n->x + 0x300, n->y);
    Thing *ball = thing_create(&bp, 10, 0x27);
    CHECK(ball != nullptr);
    if (ball) {
        ball->mana = 777; ball->mana_owner = 3;
        genie_steal_mana(n);
        CHECK_EQ(n->mana, 877);
        CHECK_EQ(ball->mana_owner, 0);
        CHECK(ball->flags & 0x400);
        const Thing *e = first_thing(10, 0);
        CHECK(e != nullptr);
        if (e) { CHECK_EQ(e->owner, n->owner); CHECK_EQ(e->flags & 0x10000u, 0x10000u); CHECK_EQ(e->x, ball->x); }
        // full genie: nothing
        n->mana = n->mana_total;
        ball->flags &= ~0x400u; ball->mana = 5;
        genie_steal_mana(n);
        CHECK_EQ(n->mana, n->mana_total);
        CHECK_EQ(ball->flags & 0x400, 0);
    }
}

static void test_type15_states() {
    if (!load_level_started(24)) return;
    delete_all(9, -1);
    // a castle guard: it may only stand on castle cells (terrain mask 0x20000). Find one.
    unsigned cell = find_castle_cell();
    std::printf("first castle cell of level 24: %04x (texture %02x)\n", cell, g_map_type[cell]);
    CHECK(cell != 0);
    Thing *g = spawn_creature(0xf, cell & 0xff, cell >> 8, 1000);
    if (!g) return;
    CHECK_EQ(g->state, 0x5b);
    CHECK_EQ(terrain_type_mask_at(thing_pos(g)), 0x20000u);
    ThingUpdateFn s91 = thing_update_fn(5, 0x5b), s92 = thing_update_fn(5, 0x5c);
    CHECK(s91 && s92);
    if (!s91 || !s92) return;

    // type15_move on a tick % 8 != 0 and % 16 != 0: no heading choice, no snap; same yaw -> moves
    g->tick = 1; g->target_yaw = g->yaw = 0x200; g->rng = 1000;
    uint16_t x0 = g->x;
    type15_move(g);
    CHECK_EQ(g->rng, 1000u);                                // no draw
    CHECK((int16_t)(g->x - x0) == 0x1e || (int16_t)(g->x - x0) == 0x1d);   // one step of speed_cur along +x
    // turning (target_yaw != yaw): one draw, moves only when rng % 20 <= 10
    g->target_yaw = 0x400; g->rng = 3;                     // 3 -> 0x92c2 % 20 = 10 -> moves
    x0 = g->x;
    type15_move(g);
    CHECK_EQ(g->rng, 0x92c2u);
    CHECK(g->x != x0);
    g->rng = 0;                                             // 0 -> 0x24df % 20 = 19 -> no move
    x0 = g->x;
    type15_move(g);
    CHECK_EQ(g->rng, 0x24dfu);
    CHECK_EQ(g->x, x0);
    // tick % 8 == 0: standing on a forbidden cell -> dying state at once
    Pos grass = pos_of(0x8080, 0x8080);
    for (unsigned c = 0; c < MC_MAP_CELLS; c++) {
        if (g_map_type[c] == 0) continue;
        Pos p = pos_of((c & 0xff) * 0x100 + 0x80, (c >> 8) * 0x100 + 0x80);
        if (terrain_type_mask_at(&p) != 0x20000u) { grass = p; break; }
    }
    thing_move_to(g, &grass);
    g->tick = 8; g->state = 0x5b; g->rng = 5;
    type15_move(g);
    CHECK_EQ(g->state, 0x5e);
    CHECK_EQ(g->rng, 5u);
    // tick % 8 == 0 on the castle: four draws, the heading goes to the best passable direction
    // (expected_heading simulates the choice); tick % 16 == 0 also snaps the coordinate across the
    // chosen heading to the cell centre before the step
    Pos back = pos_of((cell & 0xff) * 0x100 + 0x80, (cell >> 8) * 0x100 + 0x80);
    auto expected_heading = [&](uint32_t seed, uint16_t yaw0, uint32_t *rng_after, uint32_t *best_out) {
        uint32_t r = seed, best = 1; uint16_t want = yaw0, yaw = yaw0;
        static const uint16_t kW[4] = { 7000, 7000, 10, 7000 };
        for (int i = 0; i < 4; i++) {
            Pos p = *thing_pos(g);
            math_rotate_offset(&p, yaw, 0, 0x100);
            r = mc_lcg(r);
            uint32_t score = creature_check_terrain(g, &p, 1) == 0 ? r % kW[i] + 2 : 0;
            if ((uint16_t)score > (uint16_t)best) { best = score; want = yaw; }
            yaw = (uint16_t)((yaw + 0x200) & 0x7ff);
        }
        *rng_after = r; *best_out = best;
        return want;
    };
    thing_move_to(g, &back);
    g->state = 0x5b; g->tick = 8; g->yaw = g->target_yaw = 0x200; g->rng = 42;
    {
        uint32_t expect, best;
        uint16_t want = expected_heading(42, 0x200, &expect, &best);
        type15_move(g);
        if (want != 0x200) expect = mc_lcg(expect);            // 4 draws, plus 1 when it has to turn
        CHECK_EQ(g->rng, expect);
        CHECK_EQ(g->yaw, want);
        std::printf("type 15 at cell %04x: chosen heading %03x (best score %u)\n", cell, want, best);
    }
    thing_move_to(g, &back);
    g->tick = 16; g->yaw = 0x200; g->rng = 42;
    {
        uint32_t expect, best;
        uint16_t want = expected_heading(42, 0x200, &expect, &best);
        g->target_yaw = want;                                   // no turning draw, the step is made
        g->x = (uint16_t)((g->x & 0xff00) + 0x21);
        g->y = (uint16_t)((g->y & 0xff00) + 0x37);
        type15_move(g);
        CHECK_EQ(g->rng, expect);
        CHECK_EQ(g->yaw, want);
        bool along_x = want == 0x200 || want == 0x600;
        if (along_x) { CHECK_EQ(g->y & 0xff, 0x80); CHECK_EQ(g->x & 0xff, want == 0x200 ? 0x21 + 0x1e : 0x21 - 0x1e); }
        else         { CHECK_EQ(g->x & 0xff, 0x80); CHECK_EQ(g->y & 0xff, want == 0x400 ? 0x37 + 0x1e : 0x37 - 0x1e); }
    }

    // state 91 -> 92 when a player-list thing of another owner is in sight / fov on a think tick
    Thing *wiz = thing_at(g_state->players[0].thing % MC_THING_SLOTS);
    g->state = 0x5b; g->tick = 15; g->timer_a = 0x10; g->yaw = g->target_yaw = 0x200; g->owner = 0;
    g->damage_slots[0].attacker = 0;
    Pos wp = pos_of(g->x + 0x600, g->y);
    wp.z = (int16_t)(g->z + 0x80);
    thing_move_to(wiz, &wp);
    wiz->flags &= ~0x20u;
    g->rng = 3;
    s91(g);
    CHECK_EQ(g->state, 0x5c);
    CHECK_EQ(g->target, thing_index(wiz));
    CHECK_EQ(g->speed_cur, 0);
    CHECK(g->sprite == 0xce || g->sprite == 1);
    // same owner: ignored
    g->state = 0x5b; g->owner = wiz->owner; g->tick = 15; g->target = 0;
    s91(g);
    CHECK_EQ(g->state, 0x5b);
    g->owner = 0;
    // a hit by a class-3 thing of another owner -> attack it; by the own owner -> nothing
    g->state = 0x5b; g->tick = 1; g->damage_slots[0].amount = 1; g->damage_slots[0].attacker = thing_index(wiz);
    s91(g);
    CHECK_EQ(g->state, 0x5c);
    CHECK_EQ(g->health, 999);
    g->state = 0x5b; g->owner = wiz->owner; g->damage_slots[0].amount = 1; g->damage_slots[0].attacker = thing_index(wiz);
    s91(g);
    CHECK_EQ(g->state, 0x5b);
    g->owner = 0;

    // state 92: an arrow on the think tick, aim every 4 ticks, back to 91 when out of sight / dead
    g->state = 0x5c; g->tick = 15; g->target = thing_index(wiz); g->target_yaw = 0; g->damage_slots[0].attacker = 0;
    s92(g);
    CHECK_EQ(g->state, 0x5c);
    CHECK_EQ(count_things(9, 0xd), 1);
    const Thing *a = first_thing(9, 0xd);
    if (a) {
        CHECK_EQ(a->owner, g->owner);
        CHECK_EQ(a->target, thing_index(wiz));
        CHECK_EQ(a->filter_cls, g->filter_cls);
        CHECK_EQ(a->filter_type, g->filter_type);
        CHECK_EQ(a->yaw, (uint16_t)pos_angle_to(thing_pos(g), thing_pos(wiz)));
        CHECK_EQ(a->z, (int16_t)(g->z + g->ext_h));
    }
    g->tick = 4; g->target_yaw = 0;
    s92(g);
    CHECK_EQ(g->target_yaw, (uint16_t)pos_angle_to(thing_pos(g), thing_pos(wiz)));
    CHECK_EQ(count_things(9, 0xd), 1);                      // not a think tick
    g->tick = 15;
    thing_move_to(wiz, &grass);                             // far away
    s92(g);
    CHECK_EQ(g->state, 0x5b);
    CHECK_EQ(g->sprite, 0);
    CHECK_EQ(g->speed_cur, 0x1e);
    g->state = 0x5c; wiz->health = -1;
    s92(g);
    CHECK_EQ(g->state, 0x5b);
    wiz->health = wiz->max_health;
    // lethal damage in 92 -> 94
    g->state = 0x5c; g->health = 0; g->damage_slots[0].amount = 1; g->damage_slots[0].attacker = thing_index(wiz);
    s92(g);
    CHECK_EQ(g->state, 0x5e);
}

static void test_wyvern_states() {
    if (!load_level_started(24)) return;
    delete_all(9, -1);
    // a wizard castle (village, class 10 type 0x2d) to attack
    Thing *castle = nullptr;
    for (int i = 1; i < MC_THING_SLOTS; i++) if (thing_at(i)->cls == 10 && thing_at(i)->type == 0x2d) { castle = thing_at(i); break; }
    CHECK(castle != nullptr);
    if (!castle) return;
    unsigned cx = castle->x >> 8, cy = castle->y >> 8;
    Thing *w = spawn_creature(0x10, cx + 6, cy, 77);
    if (!w) return;
    CHECK_EQ(w->state, 0x61);
    ThingUpdateFn s97 = thing_update_fn(5, 0x61), s98 = thing_update_fn(5, 0x62);
    CHECK(s97 && s98);
    if (!s97 || !s98) return;
    int period = mc_move_desc(w->desc)->think_period;
    CHECK_EQ(period, 40);
    // 97: the castle search runs on tick % 41 == 0 only and picks the nearest village (xy)
    Thing *nearest = nullptr;
    {
        uint32_t best = 0xffffffffu;
        for (int i = 1; i < MC_THING_SLOTS; i++) {
            const Thing *v = thing_at(i);
            if (v->cls != 10 || v->type != 0x2d) continue;
            int32_t dx = (int16_t)(v->x - w->x), dy = (int16_t)(v->y - w->y);
            uint32_t d = (uint32_t)(dx * dx + dy * dy);
            if (d < best) { best = d; nearest = thing_at(i); }
        }
    }
    w->tick = 41; w->timer_a = 0; w->yaw = w->target_yaw = 0x600;   // asleep: creature_ai_step looks for no enemy
    s97(w);
    CHECK_EQ(w->state, 0x62);
    CHECK_EQ(w->target, thing_index(nearest));
    w->state = 0x61; w->target = 0; w->tick = 40;             // a think tick of creature_ai_step, not of the search
    s97(w);
    CHECK_EQ(w->state, 0x61);
    CHECK_EQ(w->target, 0);
    w->timer_a = 0x10;
    // 98: facing the castle on a think tick arms a burst of 15; one fireball per tick after that
    w->state = 0x62; w->target = thing_index(castle); w->tick = 40; w->aux = 0;
    w->yaw = (uint16_t)pos_angle_to(thing_pos(w), thing_pos(castle));
    s98(w);
    CHECK_EQ(w->aux, 0xf);
    CHECK_EQ(count_things(9, 0), 0);
    w->tick = 41;
    s98(w);
    CHECK_EQ(w->aux, 0xe);
    CHECK_EQ(count_things(9, 0), 1);
    const Thing *f = first_thing(9, 0);
    if (f) {
        CHECK_EQ(f->impact_cls, 10);
        CHECK_EQ(f->impact_type, 0);
        CHECK_EQ(f->desc, (0x96a50u - 0x96a10u) / 0x20u);
        CHECK_EQ(f->owner, w->owner);
        CHECK_EQ(f->damage, 0xbb8);
        CHECK_EQ(f->mana, 0xea60);
        CHECK_EQ(f->target, thing_index(castle));
        CHECK_EQ(f->filter_cls, w->filter_cls);
        CHECK_EQ(f->z, (int16_t)(w->z + (w->ext_h << 2)));
        CHECK_EQ(f->yaw, (uint16_t)pos_angle_to(thing_pos(w), thing_pos(castle)));
    }
    for (int k = 0; k < 14; k++) { w->tick = (uint8_t)(42 + k); if (w->tick % 40 == 0) w->tick++; s98(w); }
    CHECK_EQ(w->aux, 0);
    CHECK_EQ(count_things(9, 0), 15);
    // not facing: no burst
    w->tick = 40; w->yaw = (uint16_t)((pos_angle_to(thing_pos(w), thing_pos(castle)) + 0x400) & 0x7ff);
    s98(w);
    CHECK_EQ(w->aux, 0);
    // aim every 8 ticks (a non-player target only from 0x200 away)
    w->tick = 8; w->target_yaw = 0;
    s98(w);
    CHECK_EQ(w->target_yaw, (uint16_t)pos_angle_to(thing_pos(w), thing_pos(castle)));
    // target gone -> 97
    castle->flags |= 0x400;
    s98(w);
    CHECK_EQ(w->state, 0x61);
    castle->flags &= ~0x400u;
    // out of sight on a think tick -> 97
    w->state = 0x62; w->tick = 40;
    Pos far_pos = pos_of(castle->x + 0x1300, castle->y);
    thing_move_to(w, &far_pos);
    s98(w);
    CHECK_EQ(w->state, 0x61);
    // lethal damage -> 100, hit by a class-3 thing -> retarget
    const Thing *pt = player_thing(0);
    w->state = 0x62; w->health = 0; w->damage_slots[0].amount = 1; w->damage_slots[0].attacker = thing_index(pt);
    s98(w);
    CHECK_EQ(w->state, 0x64);
    w->state = 0x62; w->health = 1000; w->target = 0; w->damage_slots[0].amount = 1; w->damage_slots[0].attacker = thing_index(pt);
    s98(w);
    CHECK_EQ(w->target, thing_index(pt));
    CHECK_EQ(w->state, 0x62);
}

static void test_emu_states() {
    if (!load_level_started(44)) return;
    delete_all(9, -1);
    Thing *e = spawn_creature(0xa, 100, 100, 5);
    if (!e) return;
    CHECK_EQ(e->state, 0x3d);
    ThingUpdateFn s62 = thing_update_fn(5, 0x3e);
    CHECK(s62 != nullptr);
    if (!s62) return;
    Thing *wiz = thing_at(g_state->players[0].thing % MC_THING_SLOTS);
    Pos wp = pos_of(e->x + 0x400, e->y);
    thing_move_to(wiz, &wp);
    e->state = 0x3e; e->target = thing_index(wiz); e->tick = 40;
    s62(e);
    CHECK_EQ(count_things(9, 0xd), 1);                      // creature_attack_arrow
    const Thing *a = first_thing(9, 0xd);
    if (a) CHECK_EQ(a->damage, 250);
    CHECK_EQ(e->state, 0x3e);
    wiz->health = -1;
    s62(e);
    CHECK_EQ(e->state, 0x3d);
    wiz->health = wiz->max_health;
}

// ---- 2. the snapshot --------------------------------------------------------------------------------

static std::vector<Thing> load_snapshot_things() {
    std::vector<Thing> snap;
    if (!sim_load_snapshot("movie/gam00000.dat", "movie/map00000.dat")) return snap;
    snap.assign(g_state->things, g_state->things + MC_THING_SLOTS);
    return snap;
}

// The snapshot itself holds none of the four types (they are spawned later by switches); report it.
static void report_snapshot(const std::vector<Thing> &snap) {
    int n[20] = {};
    for (int i = 1; i < MC_THING_SLOTS; i++)
        if (snap[i].cls == 5 && snap[i].type < 20) n[snap[i].type]++;
    std::printf("snapshot creatures by type:");
    for (int k = 0; k < 20; k++) if (n[k]) std::printf(" %d:%d", k, n[k]);
    std::printf("  (emu %d, genie %d, type 15 %d, wyvern %d)\n", n[10], n[11], n[15], n[16]);
}

// Per-tick invariants of the live creatures of my types during a run, plus a running census: which
// states were seen at all, the peak simultaneous count per state.
struct RunStats {
    int seen[128] = {};         // ticks in which at least one creature was in the state
    int peak[128] = {};
    int bad = 0;
    int off_castle = 0;         // guard-ticks spent walking on a non-castle cell (the cell changed under it)
    int first_tick[20] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    std::vector<int> off_since = std::vector<int>(MC_THING_SLOTS, -1);
    void sample(int tick) {
        int now[128] = {};
        for (int i = 1; i < MC_THING_SLOTS; i++) {
            const Thing *t = thing_at(i);
            if (t->cls != 5 || !my_type(t->type)) { off_since[i] = -1; continue; }
            now[t->state & 127]++;
            if (first_tick[t->type] < 0) first_tick[t->type] = tick;
            int base = t->type * 6;
            bool ok = t->state >= base && t->state <= base + 5;
            if (t->type == 0xf) {
                ok = ok && (t->sprite == 0 || t->sprite == 0xce || t->sprite == 1);
                ok = ok && (t->state != 0x5c || t->speed_cur == 0);
                // walks on castle cells only; when the cell changes under it (the castle is hit or
                // rebuilt) type15_move kills it at the next 8th tick, so at most 8 ticks off the castle
                if (t->state == 0x5b && terrain_type_mask_at(thing_pos(t)) != 0x20000u) {
                    off_castle++;
                    if (off_since[i] < 0) off_since[i] = tick;
                    ok = ok && tick - off_since[i] <= 8;
                } else {
                    off_since[i] = -1;
                }
            } else if (t->type == 0x10) {
                ok = ok && t->aux >= 0 && t->aux <= 0xf;
            } else if (t->type == 0xb) {
                ok = ok && (t->state != 0x43 || (t->flags & 1));          // wandering genies are hidden
                ok = ok && (t->state != 0x44 || !(t->flags & 1));         // attacking genies are visible
            }
            if (!ok && bad++ < 5)
                std::printf("  tick %d: thing %d type %d state %d sprite %d speed %d aux %d flags %x at %04x,%04x (tex %02x) breaks an invariant\n",
                            tick, i, t->type, t->state, t->sprite, t->speed_cur, t->aux, t->flags, (unsigned)t->x, (unsigned)t->y,
                            g_map_type[((unsigned)(t->y >> 8) << 8) | (t->x >> 8)]);
        }
        for (int s = 0; s < 128; s++) { if (now[s]) seen[s]++; if (now[s] > peak[s]) peak[s] = now[s]; }
    }
    void report(const char *what) const {
        std::printf("%s: states seen (state: ticks present / peak count):", what);
        for (int s = 0; s < 128; s++) if (seen[s]) std::printf(" %d: %d / %d", s, seen[s], peak[s]);
        std::printf("\n%s: first appearance tick: emu %d, genie %d, type 15 %d, wyvern %d; guard-ticks off the castle %d; invariant violations %d\n",
                    what, first_tick[10], first_tick[11], first_tick[15], first_tick[16], off_castle, bad);
        if (bad) { std::printf("FAIL %s: invariants\n", what); g_fail++; }
    }
};

// sim_test's replay, with every subsystem (incl. mine) registered: slot counts per class.
static bool same_thing(const Thing &a, const Thing &b) {
    const uint8_t *pa = reinterpret_cast<const uint8_t *>(&a), *pb = reinterpret_cast<const uint8_t *>(&b);
    return std::memcmp(pa + 4, pb + 4, 0x10) == 0 && std::memcmp(pa + 0x18, pb + 0x18, sizeof(Thing) - 0x18) == 0;
}

static void test_replay_to_snapshot(const std::vector<Thing> &snap) {
    void (*saved_input)() = g_hook_player_local_input;
    g_hook_player_local_input = nullptr;
    g_video_mode_flags = 1;                         // the recording ran in 320x200 (level_features.h castle_footprint)
    g_cfg->flags = 0; g_cfg->paused = 0;
    CHECK(sim_load_level(38));
    thing_dispatch_reset_stats();
    for (int guard = 0; g_state->players[0].tick < 389 && guard < 1000; guard++) game_tick_sim();
    g_state->commands[0].cmd = 0x1e; g_state->commands[0].arg = 1;
    game_tick_sim();
    for (int guard = 0; g_state->players[0].tick < 412 && guard < 1000; guard++) game_tick_sim();
    check_pool("replay to snapshot");
    int same[14] = {}, total[14] = {}, same15 = 0, total15 = 0, same16 = 0, total16 = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing &s = snap[i], &o = g_state->things[i];
        if (!live(s)) continue;
        total[s.cls]++;
        bool eq = same_thing(s, o);
        if (eq) same[s.cls]++;
        if (s.cls == 5 && s.type == 0xf) { total15++; same15 += eq; }
        if (s.cls == 5 && s.type == 0x10) { total16++; same16 += eq; }
    }
    std::printf("replay to the snapshot, identical slots: creatures %d / %d (type 15 %d / %d, wyvern %d / %d), effects %d / %d, projectiles %d / %d, spells %d / %d\n",
                same[5], total[5], same15, total15, same16, total16, same[10], total[10], same[9], total[9], same[12], total[12]);
    census("port after 412 ticks");
    check_dispatch("replay");
    g_video_mode_flags = 8;
    g_hook_player_local_input = saved_input;
}

// ---- 3. the movie ---------------------------------------------------------------------------------------

static void test_movie() {
    g_cfg->flags = 0; g_cfg->paused = 0;
    uint16_t saved_mode = g_video_mode_flags;
    prepare_movie();
    if (!demo_open(g_game_dir, 0)) { std::printf("movie 0 missing, playback skipped\n"); g_video_mode_flags = saved_mode; return; }
    thing_dispatch_reset_stats();
    census("movie start (snapshot)");
    int ticks = 0;
    bool more = true;
    int max_arrows = 0, max_fire = 0, max_genie_shots = 0;
    RunStats stats;
    while (more && ticks < 20000) {
        more = demo_step();
        ticks++;
        stats.sample(ticks);
        int arrows = count_things(9, 0xd), fire = count_things(9, 0), gs = count_things(9, 8);
        if (arrows > max_arrows) max_arrows = arrows;
        if (fire > max_fire) max_fire = fire;
        if (gs > max_genie_shots) max_genie_shots = gs;
        if (ticks % 1000 == 0 || !more) {
            char label[64];
            std::snprintf(label, sizeof label, "movie tick %d", ticks);
            census(label);
            check_pool(label);
        }
    }
    g_video_mode_flags = saved_mode;
    std::printf("movie 0: %d ticks, %ld / %ld packets; peak class-9 counts: type 0xd %d, type 0 %d, type 8 %d\n",
                ticks, demo_packets_read(), demo_packets_total(), max_arrows, max_fire, max_genie_shots);
    CHECK(demo_packets_read() == demo_packets_total());
    stats.report("movie");
    check_dispatch("movie");
    demo_close();
}

// ---- 4. levels -------------------------------------------------------------------------------------------

static void run_level(int lv, int ticks) {
    void (*saved_input)() = g_hook_player_local_input;
    g_hook_player_local_input = nullptr;
    g_cfg->flags = 0; g_cfg->paused = 0;
    if (!sim_load_level(lv)) { std::printf("FAIL: level %d did not load\n", lv); g_fail++; return; }
    thing_dispatch_reset_stats();
    char label[64];
    std::snprintf(label, sizeof label, "level %d start", lv);
    census(label);
    int peak[4] = {};
    RunStats stats;
    for (int t = 1; t <= ticks; t++) {
        game_tick_sim();
        stats.sample(t);
        if (t % 100 == 0) {
            int c[4] = { count_things(9, 0xd), count_things(9, 0), count_things(9, 8), count_things(10, 1) };
            for (int k = 0; k < 4; k++) if (c[k] > peak[k]) peak[k] = c[k];
        }
        if (t % 1000 == 0 || t == ticks) {
            std::snprintf(label, sizeof label, "level %d tick %d", lv, t);
            census(label);
            check_pool(label);
        }
    }
    std::printf("level %d: peaks sampled every 100 ticks: arrows (9/0xd) %d, fireballs (9/0) %d, genie shots (9/8) %d, type-1 effects %d\n",
                lv, peak[0], peak[1], peak[2], peak[3]);
    std::snprintf(label, sizeof label, "level %d", lv);
    stats.report(label);
    check_dispatch(label);
    g_hook_player_local_input = saved_input;
}

static int find_genie_level() {
    int found = -1;
    for (int lv = 0; lv < 70; lv++) {
        if (!sim_load_level(lv)) continue;
        int in_records = 0;
        for (const ThingInit &r : g_state->level.things) {
            if (r.cls == 0 && r.model == 0 && r.x == 0 && r.y == 0) continue;
            if (r.cls == 5 && r.model == 11) in_records++;
        }
        int alive = count_things(5, 11);
        if (in_records || alive) {
            std::printf("level %d: %d genies at level start, %d genie records waiting for a switch\n", lv, alive, in_records);
            if (found < 0 && alive) found = lv;
        }
    }
    return found;
}

// "probe <level>": what a level holds at start and after two ticks (class / type census, castle cells).
static void probe_level(int lv) {
    CHECK(sim_load_level(lv));
    for (int pass = 0; pass < 2; pass++) {
        int by_ct[14][64] = {};
        for (int i = 1; i < MC_THING_SLOTS; i++) {
            const Thing *t = thing_at(i);
            if (live(*t) && t->type < 64) by_ct[t->cls][t->type]++;
        }
        std::printf("level %d after %d ticks:", lv, pass * 2);
        for (int c = 1; c < 14; c++) for (int ty = 0; ty < 64; ty++) if (by_ct[c][ty]) std::printf(" %d/%d:%d", c, ty, by_ct[c][ty]);
        int tex[0x24] = {};
        for (int c = 0; c < MC_MAP_CELLS; c++) tex[g_map_type[c] > 0x22 ? 0x23 : g_map_type[c]]++;
        std::printf("\n  textures:");
        for (int k = 0; k < 0x24; k++) if (tex[k]) std::printf(" %02x:%d", k, tex[k]);
        std::printf("\n  players %d, player 0 thing %d\n", g_state->player_count, g_state->players[0].thing);
        game_tick_sim(); game_tick_sim();
    }
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_game_dir = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    bool quick = argc > 2 && std::strcmp(argv[2], "quick") == 0;
    if (!sim_init(g_game_dir)) { std::printf("sim_init failed\n"); return 2; }
    register_gameplay();
    if (argc > 3 && std::strcmp(argv[2], "probe") == 0) { probe_level(std::atoi(argv[3])); return 0; }

    std::printf("== construction tests\n");
    test_sprites_and_helpers();
    test_genie_states();
    test_type15_states();
    test_wyvern_states();
    test_emu_states();

    std::printf("== snapshot\n");
    std::vector<Thing> snap = load_snapshot_things();
    if (snap.empty()) { std::printf("snapshot failed to load\n"); return 2; }
    report_snapshot(snap);
    test_replay_to_snapshot(snap);

    if (!quick) {
        std::printf("== movie\n");
        test_movie();
        std::printf("== levels\n");
        run_level(24, 3000);
        run_level(44, 3000);
        int genie_level = find_genie_level();
        std::printf("first level with a genie at level start: %d\n", genie_level);
        CHECK(genie_level >= 0);
        if (genie_level >= 0) run_level(genie_level, 3000);
    }

    std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
