// Unit / integration test for creatures2.cpp (crab, kraken, troll, griffon; class 5 states 31..53).
// Built with ${MC_SIM_ALL} so the creatures meet the real projectiles, effects and castles.
// argv[1] = game dir.
//
//  1. construction tests of every handler (hand-computed cases from the disassembly) on a fresh
//     level 38 after one tick, creatures created with thing_create next to the local player;
//  2. the snapshot's krakens (level 38, 413 ticks): states / fields of my types, and the 412-tick
//     replay of sim_test with the crab / kraken / troll / griffon handlers unbound and bound;
//  3. smoke: the shipped movie from the snapshot (kraken) and levels 12 (griffon), 24 (troll), 44
//     (crab, kraken) for 3000 ticks: pool consistency, census per state, handlers still missing.
#include "sim.h"
#include "creatures.h"
#include "creatures2.h"
#include "level_features.h"
#include "player.h"
#include "demo.h"
#include "mc_math.h"
#include "crash_handler.h"
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_fail++; } } while (0)

static const char *g_game_dir = nullptr;

static bool is_creature(const Thing *t) { return t->cls == 5; }
static bool is_mine(const Thing *t) { return is_creature(t) && t->type >= 5 && t->type <= 8; }
static bool live(const Thing &t) { return t.cls != 0 && t.cls < 14; }

// Census of my four types by state (segments of a kraken are state 0x78 and counted under "seg").
static void census(const char *what) {
    int by_state[128] = {}, n = 0, seg = 0, all = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing *t = thing_at(i);
        if (!is_creature(t)) continue;
        all++;
        if (!is_mine(t)) continue;
        if (t->state == 0x78) { seg++; continue; }
        n++;
        by_state[t->state & 127]++;
    }
    std::printf("%-26s %3d creatures, crab/kraken/troll/griffon %2d (+%d segments) state:count", what, all, n, seg);
    for (int s = 0; s < 128; s++) if (by_state[s]) std::printf(" %d:%d", s, by_state[s]);
    std::printf("\n");
}

// Every live creature is in a state of its type and is linked into the cell list of its position;
// every cell chain is consistent (as sim_test's check_pool).
static void check_pool(const char *what) {
    int bad_state = 0, bad_link = 0, n = 0;
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
    int bad = 0;
    for (int c = 0; c < MC_MAP_CELLS; c++) {
        int guard = 0; unsigned prev = 0;
        for (unsigned i = g_cell_things[c]; i != 0; i = thing_at(i)->cell_next) {
            if (i >= MC_THING_SLOTS || ++guard > MC_THING_SLOTS) { bad++; break; }
            const Thing *t = thing_at(i);
            if (t->cls == 0 || !(t->flags & 4) || t->cell_prev != prev) bad++;
            seen[i]++;
            prev = i;
        }
    }
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing *t = thing_at(i);
        if (t->cls != 0 && (t->flags & 4) && seen[i] != 1) bad++;
        if (seen[i] > 1) bad++;
    }
    if (bad) std::printf("  %s: %d cell chain problems\n", what, bad);
    CHECK_EQ(bad_state, 0);
    CHECK_EQ(bad_link, 0);
    CHECK_EQ(bad, 0);
    CHECK(n > 0);
}

static ThingUpdateFn handler(int state) {
    ThingUpdateFn fn = thing_update_fn(5, state);
    CHECK(fn != nullptr);
    return fn;
}

static int count_class(int cls) {
    int n = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) if (thing_at(i)->cls == cls) n++;
    return n;
}

static Thing *newest_of_class(const std::vector<uint8_t> &before, int cls) {
    for (int i = 1; i < MC_THING_SLOTS; i++)
        if (thing_at(i)->cls == cls && before[(size_t)i] != cls) return thing_at(i);
    return nullptr;
}

static std::vector<uint8_t> class_map() {
    std::vector<uint8_t> m((size_t)MC_THING_SLOTS);
    for (int i = 0; i < MC_THING_SLOTS; i++) m[(size_t)i] = thing_at(i)->cls;
    return m;
}

// Place `o` `dist` units in front of `t` (yaw 0 looks toward -y, math_atan2) on the same height.
static void put_in_front(Thing *t, Thing *o, int dist) {
    t->yaw = 0;
    Pos p{ t->x, (uint16_t)(t->y - dist), t->z };
    thing_move_to(o, &p);
}

static Thing *make_creature(int type, const Pos *pos) {
    Thing *t = thing_create(pos, 5, type);
    CHECK(t != nullptr);
    return t;
}

static int think_period(const Thing *t) { return (int16_t)mc_move_desc(t->desc)->think_period; }

// Recorded sound requests (the sound system is not linked: the hook is ours).
static int s_sounds[64];
static int s_sound_thing = -1;
static void sound_hook(int thing, int, int sound) { if (sound >= 0 && sound < 64) s_sounds[sound]++; s_sound_thing = thing; }
static void clear_sounds() { std::memset(s_sounds, 0, sizeof s_sounds); s_sound_thing = -1; }

// ---- 1. construction tests ------------------------------------------------------------------------

static void test_crab() {
    Thing *player = thing_at(g_state->players[g_state->local_player].thing);
    Pos here = *thing_pos(player);
    here.x = (uint16_t)(here.x + 0x1000);              // away from the player: no enemy in sight
    Thing *crab = make_creature(5, &here);
    if (!crab) return;
    const int period = think_period(crab);
    std::printf("crab: desc think %d sight %d fov %d, speed %d, mana %d / %d, health %d\n", period,
                (int16_t)mc_move_desc(crab->desc)->sight_radius, (int16_t)mc_move_desc(crab->desc)->fov,
                crab->speed_base, (int)crab->mana, (int)crab->mana_total, (int)crab->health);
    CHECK_EQ(crab->state, 0x1f);
    CHECK(period > 0);
    crab->timer_a = 0x10;

    // state 30 -> 31
    handler(30)(crab);
    CHECK_EQ(crab->state, 0x1f);

    // regeneration: max_health >> 7 per tick, not above max
    crab->health = crab->max_health - 10;
    crab->tick = 1;                                     // not a think tick
    uint16_t x0 = crab->x, y0 = crab->y;
    handler(31)(crab);
    CHECK_EQ(crab->health, crab->max_health - 10 + (crab->max_health >> 7));
    CHECK(crab->x != x0 || crab->y != y0);              // it walked
    crab->health = crab->max_health - 100;
    handler(32)(crab);                                  // the attack state regenerates as well
    CHECK_EQ(crab->health, crab->max_health - 100 + (crab->max_health >> 7));
    crab->health = crab->max_health;
    handler(31)(crab);
    CHECK_EQ(crab->health, crab->max_health);           // no regeneration at (or above) max_health

    // a mana ball: picked from the list on a think tick, walked to, state 33 within speed_base * 128
    Pos bp = here; bp.y = (uint16_t)(bp.y - 0x2000);
    Thing *ball = thing_create(&bp, 10, 0x27);
    CHECK(ball != nullptr);
    if (!ball) return;
    std::printf("crab: mana ball %d with %d mana\n", thing_index(ball), (int)ball->mana);
    Pos crab_pos = here;
    thing_move_to(crab, &crab_pos);
    crab->target = 0;
    g_cfg->mana_ball_list = thing_index(ball);          // the list pass of thing_update_all has not run
    g_cfg->player_list = 0;                             // no enemy in sight while it collects (set again below)
    ball->next = 0;
    crab->tick = 0;
    crab->yaw = 0x400;                                  // looking away from the player
    handler(31)(crab);
    CHECK_EQ(crab->target, thing_index(ball));
    CHECK_EQ(crab->state, 0x1f);
    handler(31)(crab);                                  // next think tick: aim at it
    CHECK_EQ(crab->target_yaw, (uint16_t)pos_angle_to(thing_pos(crab), thing_pos(ball)));
    CHECK_EQ(crab->state, 0x1f);
    crab->yaw = crab->target_yaw;                       // facing the ball: the step brings it closer
    bp = *thing_pos(crab); bp.y = (uint16_t)(bp.y - (crab->speed_base << 7) + 0x80);
    thing_move_to(ball, &bp);
    crab->aux = 0;
    handler(31)(crab);                                  // within speed_base * 128 after this tick's step
    CHECK_EQ(crab->state, 0x21);
    CHECK_EQ(crab->aux, 0xf);
    // a non-think tick does nothing but move / regenerate
    crab->state = 0x1f;
    crab->tick = 1;
    crab->target_yaw = 0x123;
    handler(31)(crab);
    CHECK_EQ(crab->target_yaw, 0x123);
    CHECK_EQ(crab->state, 0x1f);

    // state 33: tick % aux; far -> aim; within speed_base * 20 -> aux = 3; within speed_base * 5 -> eat
    crab->state = 0x21; crab->aux = 0xf; crab->tick = 0;
    bp = *thing_pos(crab); bp.y = (uint16_t)(bp.y - crab->speed_base * 40);
    thing_move_to(ball, &bp);
    handler(33)(crab);
    CHECK_EQ(crab->state, 0x21);
    CHECK_EQ(crab->aux, 0xf);
    CHECK_EQ(crab->target_yaw, (uint16_t)pos_angle_to(thing_pos(crab), thing_pos(ball)));
    crab->tick = 1;                                     // 1 % 15 != 0: nothing
    crab->target_yaw = 0x321;
    handler(33)(crab);
    CHECK_EQ(crab->target_yaw, 0x321);
    crab->tick = 0;
    crab->yaw = crab->target_yaw = 0;                   // facing the ball
    bp = *thing_pos(crab); bp.y = (uint16_t)(bp.y - crab->speed_base * 20 + 0x40);   // within 20 x, beyond 5 x after the step
    thing_move_to(ball, &bp);
    handler(33)(crab);
    CHECK_EQ(crab->aux, 3);
    CHECK_EQ(crab->state, 0x21);
    crab->tick = 3;
    bp = *thing_pos(crab); bp.y = (uint16_t)(bp.y - crab->speed_base * 3);
    thing_move_to(ball, &bp);
    int32_t mana = crab->mana, ball_mana = ball->mana;
    uint16_t sprite = crab->sprite;
    int32_t maxh = crab->max_health;
    handler(33)(crab);
    CHECK_EQ(crab->state, 0x1f);
    CHECK_EQ(crab->target, 0);
    CHECK_EQ(crab->mana, mana + ball_mana);
    CHECK(ball->flags & 0x400);
    CHECK_EQ(ball->mana_owner, 0);
    // crab_update_mana_sprite: level = mana / (mana_total / 8), clamped 0..7
    int level = (int)(crab->mana / (crab->mana_total / 8));
    if (level > 7) level = 7;
    CHECK_EQ(crab->sprite, 0xb9 + level);
    if (level > sprite - 0xb9) CHECK_EQ(crab->max_health, maxh + 5000); else CHECK_EQ(crab->max_health, maxh);
    // a vanished target sends it back to 31
    crab->state = 0x21; crab->aux = 0xf; crab->tick = 0;
    crab->target = thing_index(player);                 // not a mana ball
    handler(33)(crab);
    CHECK_EQ(crab->state, 0x1f);
    CHECK_EQ(crab->target, 0);
    // in state 31 a target that is no mana ball is dropped
    crab->target = thing_index(player);
    crab->tick = 0;
    g_cfg->mana_ball_list = 0;
    handler(31)(crab);
    CHECK_EQ(crab->target, 0);
    CHECK_EQ(crab->state, 0x1f);

    // the egg: mana > mana_total + 500 on a think tick without a target -> class 10 type 0x34 with
    // aux = 100 + 10 * (rng % 10), 500 mana paid, one RNG draw
    crab->mana = crab->mana_total + 501;
    crab->rng = 12345;
    crab->tick = 0;
    std::vector<uint8_t> before = class_map();
    int effects = count_class(10);
    handler(31)(crab);
    Thing *egg = newest_of_class(before, 10);
    CHECK(egg != nullptr);
    if (egg) {
        CHECK_EQ(egg->type, 0x34);
        CHECK_EQ(egg->state, 0x38);
        uint32_t r = mc_lcg(12345);
        CHECK_EQ(crab->rng, r);
        CHECK_EQ(egg->aux, (int16_t)((r % 10u) * 10 + 100));
        CHECK_EQ(crab->mana, crab->mana_total + 1);
        CHECK_EQ(count_class(10), effects + 1);
        thing_mark_delete(egg);
    }
    before = class_map();
    crab->mana = crab->mana_total + 500;                // not enough: no egg
    handler(31)(crab);
    CHECK(newest_of_class(before, 10) == nullptr);
    CHECK_EQ(crab->mana, crab->mana_total + 500);

    // an enemy in sight / fov on a think tick: attack (32), even while asleep
    crab->timer_a = 0;
    crab->tick = 0;
    put_in_front(crab, player, 0x300);
    g_cfg->player_list = thing_index(player);
    player->next = 0;
    handler(31)(crab);
    CHECK_EQ(crab->state, 0x20);
    CHECK_EQ(crab->target, thing_index(player));
    // state 32: the volley on a think tick in range (creature_attack_volley makes the projectiles)
    clear_sounds();
    crab->timer_a = 0x10;
    crab->tick = 0;
    before = class_map();
    int projectiles = count_class(9);
    handler(32)(crab);
    std::printf("crab: volley created %d projectile(s), sound 0x20 x%d\n", count_class(9) - projectiles, s_sounds[0x20]);
    CHECK(count_class(9) > projectiles);
    CHECK_EQ(s_sounds[0x20], 1);
    CHECK_EQ(s_sound_thing, thing_index(crab));
    // hit by a class-3 thing in state 31 / 33: target it; killed: dying state 34 then 35
    crab->state = 0x1f; crab->target = 0; crab->tick = 1;
    crab->damage_slots[0] = { 10, thing_index(player) };
    handler(31)(crab);
    CHECK_EQ(crab->state, 0x20);
    CHECK_EQ(crab->target, thing_index(player));
    crab->state = 0x21;
    crab->target = 0;
    crab->damage_slots[0] = { 10, thing_index(player) };
    handler(33)(crab);
    CHECK_EQ(crab->state, 0x20);
    crab->state = 0x1f;
    crab->damage_slots[0] = { 100000, thing_index(player) };
    handler(31)(crab);
    CHECK_EQ(crab->state, 0x22);
    CHECK_EQ(crab->killer, thing_index(player));
    handler(34)(crab);
    CHECK_EQ(crab->state, 0x23);
    crab->tick = 8;
    int effects2 = count_class(10);
    handler(35)(crab);
    CHECK(crab->flags & 0x400);
    CHECK(count_class(10) >= effects2 + 1);            // death effect (+ the dropped mana ball)

    // crab_target_nearest_mana_ball_1aef0 over the whole pool (the balls deleted above are still in
    // the pool until the next thing_update_all: make them invisible first)
    for (int i = 1; i < MC_THING_SLOTS; i++) if (thing_at(i)->cls == 10 && thing_at(i)->type == 0x27) thing_at(i)->cls = 0;
    {
        Pos a = here; a.x = (uint16_t)(a.x + 0x300);
        Pos b = here; b.x = (uint16_t)(b.x + 0x600);
        Thing *near_ball = thing_create(&a, 10, 0x27);
        Thing *far_ball = thing_create(&b, 10, 0x27);
        Thing probe{};
        probe.x = here.x; probe.y = here.y; probe.z = here.z;
        if (near_ball && far_ball) {
            crab_target_nearest_mana_ball(&probe);
            CHECK_EQ(probe.target, thing_index(near_ball));
            thing_mark_delete(near_ball);
            near_ball->cls = 0;
            crab_target_nearest_mana_ball(&probe);
            CHECK_EQ(probe.target, thing_index(far_ball));
            far_ball->cls = 0;
            crab_target_nearest_mana_ball(&probe);
            CHECK_EQ(probe.target, 0);
        }
    }
    for (int i = 1; i < MC_THING_SLOTS; i++) if (thing_at(i)->cls == 10 && thing_at(i)->type == 0x27) thing_mark_delete(thing_at(i));
}

// The kraken lives in water: the nearest cell to `from` whose 5 x 5 neighbourhood passes
// creature_check_terrain for t's descriptor (creature_move_step kills a creature that cannot step
// anywhere). Returns false when there is none within 64 cells.
static bool find_allowed_area(const Thing *t, const Pos *from, Pos *out) {
    for (int r = 0; r < 64; r++) {
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                if (dy != -r && dy != r && dx != -r && dx != r) continue;
                uint8_t cx = (uint8_t)((from->x >> 8) + dx), cy = (uint8_t)((from->y >> 8) + dy);
                bool ok = true;
                for (int ny = -2; ny <= 2 && ok; ny++)
                    for (int nx = -2; nx <= 2 && ok; nx++) {
                        Pos p{ (uint16_t)(((uint8_t)(cx + nx) << 8) | 0x80), (uint16_t)(((uint8_t)(cy + ny) << 8) | 0x80), 0 };
                        p.z = (int16_t)terrain_height_at(&p);
                        if (creature_check_terrain(t, &p, 1) != 0) ok = false;
                    }
                if (!ok) continue;
                out->x = (uint16_t)((cx << 8) | 0x80);
                out->y = (uint16_t)((cy << 8) | 0x80);
                out->z = (int16_t)terrain_height_at(out);
                return true;
            }
        }
    }
    return false;
}

static void test_kraken() {
    Thing *player = thing_at(g_state->players[g_state->local_player].thing);
    PlayerBlock *P = player_block(player);
    Pos here = *thing_pos(player);
    Thing *k = make_creature(6, &here);
    if (!k) return;
    Pos water;
    bool found = find_allowed_area(k, &here, &water);
    CHECK(found);
    if (!found) return;
    std::printf("kraken: water at cell %d,%d (player at %d,%d)\n", water.x >> 8, water.y >> 8, here.x >> 8, here.y >> 8);
    thing_move_to(k, &water);
    for (Thing *s = thing_at(k->child); s != thing_at(0); s = thing_at(s->child)) thing_move_to(s, &water);
    here = water;
    const int period = think_period(k);
    const int sight = (int16_t)mc_move_desc(k->desc)->sight_radius;
    int segs = 0;
    for (Thing *s = thing_at(k->child); s != thing_at(0); s = thing_at(s->child)) segs++;
    std::printf("kraken: desc think %d sight %d, %d segments, health %d, mana %d / %d\n", period, sight, segs,
                (int)k->health, (int)k->mana, (int)k->mana_total);
    CHECK_EQ(k->state, 0x25);
    CHECK_EQ(segs, 2);
    k->timer_a = 0x10;

    // 36 / 37: the shared bodies; 37 announces the attack state with sound 0x25
    handler(36)(k);
    CHECK_EQ(k->state, 0x25);
    clear_sounds();
    k->tick = 1;
    handler(37)(k);
    CHECK_EQ(k->state, 0x25);
    CHECK_EQ(s_sounds[0x25], 0);
    k->tick = 0;
    put_in_front(k, player, 0x400);
    g_cfg->player_list = thing_index(player);
    player->next = 0;
    handler(37)(k);
    CHECK_EQ(k->state, 0x26);
    CHECK_EQ(k->target, thing_index(player));
    CHECK_EQ(s_sounds[0x25], 1);

    // 38: the grip. aux counts up from the constructor's value; while positive the player is dragged
    k->aux = 0;
    k->tick = 1;                                        // no aim (tick & 3), no think
    k->castle_size = 0;
    P->knock_speed = 0; P->knock_yaw = 0; P->knock_pitch = 0;
    clear_sounds();
    handler(38)(k);
    CHECK_EQ(k->state, 0x26);
    CHECK_EQ(k->aux, 1);
    CHECK_EQ(P->knock_speed, 0x50);
    CHECK_EQ(P->knock_pitch, 0x100);
    CHECK_EQ(P->knock_yaw, (pos_angle_to(thing_pos(k), thing_pos(player)) + 0x400) & 0x7ff);
    CHECK_EQ(s_sounds[0x2a], 1);
    CHECK_EQ(k->castle_size, 0);                        // no burst without a think tick
    // aux 40 -> 41 still grips; aux 41 -> -90: rest
    k->aux = 40;
    handler(38)(k);
    CHECK_EQ(k->aux, 41);
    CHECK_EQ(s_sounds[0x2a], 2);
    handler(38)(k);
    CHECK_EQ(k->aux, -90);
    CHECK_EQ(s_sounds[0x2a], 2);
    P->knock_speed = 0;
    k->aux = -5;
    handler(38)(k);
    CHECK_EQ(k->aux, -4);
    CHECK_EQ(P->knock_speed, 0);                        // no grip while resting
    // every 4th tick: aim at the target
    k->tick = 4;
    k->target_yaw = 0x7ff;
    handler(38)(k);
    CHECK_EQ(k->target_yaw, (uint16_t)pos_angle_to(thing_pos(k), thing_pos(player)));
    // a think tick in range: sound 0x25, burst of 5, first shot right away
    put_in_front(k, player, 0x400);
    k->tick = 0;
    k->aux = -50;
    clear_sounds();
    std::vector<uint8_t> before = class_map();
    int projectiles = count_class(9);
    handler(38)(k);
    CHECK_EQ(s_sounds[0x25], 1);
    CHECK_EQ(k->castle_size, 4);
    CHECK_EQ(count_class(9), projectiles + 1);
    Thing *shot = newest_of_class(before, 9);
    CHECK(shot != nullptr);
    if (shot) {
        CHECK_EQ(shot->cls, 9);
        CHECK_EQ(shot->type, 9);
        CHECK_EQ(shot->impact_cls, 10);
        CHECK_EQ(shot->impact_type, 0x17);
        CHECK_EQ(shot->damage, 0x320);
        CHECK_EQ(shot->owner, k->owner);
        CHECK_EQ(shot->target, thing_index(player));
        CHECK_EQ(shot->desc, (0x96ad0u - 0x96a10u) / 0x20u);
        CHECK_EQ(shot->filter_cls, player->filter_cls);
        CHECK_EQ(shot->filter_type, player->filter_type);
        CHECK_EQ(shot->yaw, (uint16_t)pos_angle_to(thing_pos(k), thing_pos(player)));
        CHECK_EQ(shot->pitch, (uint16_t)pos_pitch_to(thing_pos(k), thing_pos(player)));
        CHECK_EQ(shot->x, k->x);
        CHECK_EQ(shot->y, k->y);
        thing_mark_delete(shot);
    }
    // the burst continues one shot per tick
    k->tick = 1;
    handler(38)(k);
    CHECK_EQ(k->castle_size, 3);
    CHECK_EQ(count_class(9), projectiles + 2);
    // kraken_fire by construction
    before = class_map();
    Thing *p2 = kraken_fire(k, player);
    CHECK(p2 != nullptr);
    Thing *raw = thing_create(thing_pos(k), 9, 9);      // the constructor's z, to measure the raise
    if (p2 && raw) {
        CHECK_EQ(p2->z, (int16_t)(raw->z + k->ext_h));
        CHECK_EQ(p2->damage, 0x320);
        thing_mark_delete(p2);
        thing_mark_delete(raw);
    }
    // out of sight on a think tick: back to 37 (no shot)
    k->castle_size = 0;
    k->tick = 0;
    Pos away = *thing_pos(k); away.y = (uint16_t)(away.y - sight - 0x100);   // ("far" is a Windows macro)
    thing_move_to(player, &away);
    handler(38)(k);
    CHECK_EQ(k->state, 0x25);
    // target gone: back to 37
    k->state = 0x26;
    k->tick = 1;
    put_in_front(k, player, 0x400);
    int32_t ph = player->health;
    player->health = -1;
    handler(38)(k);
    CHECK_EQ(k->state, 0x25);
    player->health = ph;
    // hit by a class-3 thing: retarget with aux = -10, no move
    k->state = 0x26;
    k->aux = 7;
    k->damage_slots[0] = { 10, thing_index(player) };
    uint16_t kx = k->x, ky = k->y;
    handler(38)(k);
    CHECK_EQ(k->aux, -10);
    CHECK_EQ(k->target, thing_index(player));
    CHECK_EQ(k->state, 0x26);
    CHECK_EQ(k->x, kx);
    CHECK_EQ(k->y, ky);
    // 39 announces an attack too; 40 / 41 die with the segments
    k->state = 0x27;
    k->parent = 0;
    clear_sounds();
    handler(39)(k);
    CHECK_EQ(k->state, 0x25);                           // no leader -> main state
    CHECK_EQ(s_sounds[0x25], 0);
    k->damage_slots[0] = { 100000, thing_index(player) };
    k->state = 0x26;
    handler(38)(k);
    CHECK_EQ(k->state, 0x28);
    handler(40)(k);
    CHECK_EQ(k->state, 0x29);
    for (Thing *s = thing_at(k->child); s != thing_at(0); s = thing_at(s->child)) CHECK_EQ(s->state, 0x29);
    k->tick = 8;
    handler(41)(k);
    CHECK(k->flags & 0x400);
    for (Thing *s = thing_at(k->child); s != thing_at(0); s = thing_at(s->child)) thing_mark_delete(s);
    for (int i = 1; i < MC_THING_SLOTS; i++) if (thing_at(i)->cls == 9) thing_mark_delete(thing_at(i));
}

static void test_troll() {
    Thing *player = thing_at(g_state->players[g_state->local_player].thing);
    Pos here = *thing_pos(player);
    here.x = (uint16_t)(here.x + 0x1800);
    Thing *t = make_creature(7, &here);
    if (!t) return;
    const int period = think_period(t);
    std::printf("troll: desc think %d sight %d, sprite 0x%x, health %d, speed %d turn_rate %d\n", period,
                (int16_t)mc_move_desc(t->desc)->sight_radius, t->sprite, (int)t->health, t->speed_base, t->turn_rate);
    CHECK_EQ(t->state, 0x2b);
    CHECK(t->sprite == 0x55 || t->sprite == 0xc7);
    t->timer_a = 0x10;

    // 42: idle
    handler(42)(t);
    CHECK_EQ(t->state, 0x2b);
    // 43: the health top-up on think ticks only; a zero health stays zero
    t->health = 100;
    t->tick = 1;
    handler(43)(t);
    CHECK_EQ(t->health, 100);
    t->tick = 0;
    handler(43)(t);
    CHECK_EQ(t->health, t->max_health);
    t->health = 0;
    handler(43)(t);
    CHECK_EQ(t->health, 0);
    t->health = t->max_health;
    // damage arrives after the top-up: a hit of max_health + 1 in one think period kills
    t->damage_slots[0] = { t->max_health + 1, thing_index(player) };
    t->health = 5;
    handler(43)(t);
    CHECK_EQ(t->state, 0x2e);
    CHECK_EQ(t->health, -1);
    t->state = 0x2b;
    t->health = t->max_health;
    // entering the attack state sets aux = 1
    t->aux = 55;
    t->tick = 0;
    put_in_front(t, player, 0x400);
    g_cfg->player_list = thing_index(player);
    player->next = 0;
    handler(43)(t);
    CHECK_EQ(t->state, 0x2c);
    CHECK_EQ(t->target, thing_index(player));
    CHECK_EQ(t->aux, 1);

    // 44: a shot on a think tick in range puts the walking sprite 0x55 into the pose 0xc6 for 30 ticks
    // at turn_rate speed; the pose ends when aux runs out or the state is left
    thing_set_sprite(t, 0x55);
    t->speed_cur = t->speed_base;
    t->aux = 0;
    t->tick = 0;
    std::vector<uint8_t> before = class_map();
    int projectiles = count_class(9);
    handler(44)(t);
    CHECK_EQ(count_class(9), projectiles + 1);
    Thing *shot = newest_of_class(before, 9);
    if (shot) { CHECK_EQ(shot->type, 0xe); CHECK_EQ(shot->damage, 780); thing_mark_delete(shot); }
    CHECK_EQ(t->sprite, 0xc6);
    CHECK_EQ(t->aux, 0x1e);
    CHECK_EQ(t->speed_cur, (int16_t)t->turn_rate);
    t->tick = 1;
    handler(44)(t);                                     // aux 30 -> 29, pose kept
    CHECK_EQ(t->aux, 0x1d);
    CHECK_EQ(t->sprite, 0xc6);
    t->aux = 1;
    handler(44)(t);                                     // the last pose tick: back to walking
    CHECK_EQ(t->aux, 0);
    CHECK_EQ(t->sprite, 0x55);
    CHECK_EQ(t->speed_cur, t->speed_base);
    // the pose is dropped when the target is lost
    thing_set_sprite(t, 0xc6);
    t->speed_cur = (int16_t)t->turn_rate;
    t->aux = 20;
    int32_t ph = player->health;
    player->health = -1;
    handler(44)(t);
    CHECK_EQ(t->state, 0x2b);
    CHECK_EQ(t->sprite, 0x55);
    CHECK_EQ(t->speed_cur, t->speed_base);
    player->health = ph;
    // the 0xc7 variant never changes its sprite
    thing_set_sprite(t, 0xc7);
    t->state = 0x2c;
    t->aux = 0;
    t->tick = 0;
    before = class_map();
    handler(44)(t);
    CHECK_EQ(t->sprite, 0xc7);
    shot = newest_of_class(before, 9);
    if (shot) thing_mark_delete(shot);
    // 45: follow; entering the attack state sets aux = 1
    t->state = 0x2d;
    t->parent = 0;
    handler(45)(t);
    CHECK_EQ(t->state, 0x2b);
    // 46 / 47
    t->killer = thing_index(player);
    handler(46)(t);
    CHECK_EQ(t->state, 0x2f);
    t->tick = 8;
    handler(47)(t);
    CHECK(t->flags & 0x400);
}

static void test_griffon() {
    Thing *player = thing_at(g_state->players[g_state->local_player].thing);
    PlayerBlock *P = player_block(player);
    Pos here = *thing_pos(player);
    here.x = (uint16_t)(here.x + 0x1800);
    Thing *g = make_creature(8, &here);
    if (!g) return;
    const int period = think_period(g);
    std::printf("griffon: desc think %d sight %d fov %d, health %d, speed %d\n", period,
                (int16_t)mc_move_desc(g->desc)->sight_radius, (int16_t)mc_move_desc(g->desc)->fov, (int)g->health, g->speed_base);
    CHECK_EQ(g->state, 0x31);

    handler(48)(g);
    CHECK_EQ(g->state, 0x31);
    // 49: a think tick draws the random turn (two LCG steps) even asleep; nothing else while asleep
    g->timer_a = 0;
    g->tick = 0;
    g->rng = 77;
    g->target_yaw = 0x100;
    put_in_front(g, player, 0x300);
    g_cfg->player_list = thing_index(player);
    player->next = 0;
    P->timer210 = 200;
    handler(49)(g);
    CHECK_EQ(g->rng, mc_lcg(mc_lcg(77)));
    CHECK(g->target_yaw != 0x100);
    CHECK_EQ(g->state, 0x31);
    g->tick = 1;
    g->rng = 77;
    handler(49)(g);
    CHECK_EQ(g->rng, 77);                               // no draw outside a think tick
    // awake, the wizard in front is wanted: attack and re-arm the flag
    g->timer_a = 0x10;
    g->tick = 0;
    P->timer210 = 1;
    handler(49)(g);
    CHECK_EQ(g->state, 0x32);
    CHECK_EQ(g->target, thing_index(player));
    CHECK_EQ(P->timer210, 200);
    // passive until hurt: an innocent wizard that damages it becomes its target, state 50 (whose flag
    // 0x8000 then reflects that wizard's projectiles, projectiles.cpp reflect())
    g->state = 0x31;
    g->target = 0;
    P->timer210 = 0;
    g->damage_slots[0].amount = 100;
    g->damage_slots[0].attacker = (uint16_t)thing_index(player);
    const int32_t hp = g->health;
    handler(49)(g);
    CHECK_EQ(g->state, 0x32);
    CHECK_EQ(g->target, thing_index(player));
    CHECK_EQ(g->health, hp - 100);
    CHECK_EQ(P->timer210, 0);                           // being hurt does not make the wizard "wanted"
    g->damage_slots[0].amount = 0;
    g->damage_slots[0].attacker = 0;
    // an innocent wizard is ignored, and without another griffon nothing else happens
    g->state = 0x31;
    g->target = 0;
    P->timer210 = 0;
    g_cfg->creature_lists[8] = thing_index(g);
    g->next = 0;
    handler(49)(g);
    CHECK_EQ(g->state, 0x31);
    CHECK_EQ(g->target, 0);
    // a leaderless griffon in sight / fov becomes the leader (51)
    Pos lp = *thing_pos(g); lp.y = (uint16_t)(lp.y - 0x500);
    Thing *leader = make_creature(8, &lp);
    if (leader) {
        g_cfg->creature_lists[8] = thing_index(g);
        g->next = thing_index(leader);
        leader->next = 0;
        leader->parent = 0;
        g->yaw = 0;
        g->tick = 0;
        handler(49)(g);
        CHECK_EQ(g->state, 0x33);
        CHECK_EQ(g->parent, thing_index(leader));
        // 51: follow; the leader is in its main state -> aim at it
        g->tick = 0;
        handler(51)(g);
        CHECK_EQ(g->state, 0x33);
        CHECK_EQ(g->target_yaw, (uint16_t)pos_angle_to(thing_pos(g), thing_pos(leader)));
        thing_mark_delete(leader);
        leader->cls = 0;
    }
    // 50: flag 0x8000, full speed while aux != 0, a shot gives sound 0x26 and marks the wizard wanted
    g->state = 0x32;
    g->parent = 0;
    g->target = thing_index(player);
    g->flags &= ~0x8000u;
    g->aux = 3;
    g->speed_cur = 1;
    g->tick = 0;
    put_in_front(g, player, 0x400);
    P->timer210 = 0;
    clear_sounds();
    std::vector<uint8_t> before = class_map();
    int projectiles = count_class(9);
    handler(50)(g);
    CHECK(g->flags & 0x8000u);
    CHECK_EQ(g->speed_cur, g->speed_base);
    CHECK_EQ(count_class(9), projectiles + 1);
    Thing *shot = newest_of_class(before, 9);
    if (shot) { CHECK_EQ(shot->type, 9); CHECK_EQ(shot->damage, 4000); thing_mark_delete(shot); }
    CHECK_EQ(s_sounds[0x26], 2);                        // the shot and the think tick
    CHECK_EQ(P->timer210, 200);
    g->tick = 1;
    clear_sounds();
    P->timer210 = 0;
    handler(50)(g);
    CHECK_EQ(s_sounds[0x26], 0);
    CHECK_EQ(P->timer210, 0);
    // 52: the killer wizard is marked wanted, then death
    g->killer = thing_index(player);
    P->timer210 = 0;
    handler(52)(g);
    CHECK_EQ(P->timer210, 200);
    CHECK_EQ(g->state, 0x35);
    g->tick = 8;
    handler(53)(g);
    CHECK(g->flags & 0x400);
    for (int i = 1; i < MC_THING_SLOTS; i++) if (thing_at(i)->cls == 9) thing_mark_delete(thing_at(i));
}

// ---- 2. the snapshot -----------------------------------------------------------------------------

static bool same_thing(const Thing &a, const Thing &b) {
    const uint8_t *pa = reinterpret_cast<const uint8_t *>(&a), *pb = reinterpret_cast<const uint8_t *>(&b);
    return std::memcmp(pa + 4, pb + 4, 0x10) == 0 && std::memcmp(pa + 0x18, pb + 0x18, sizeof(Thing) - 0x18) == 0;
}

static void test_snapshot_fields(const std::vector<Thing> &snap) {
    int n = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing &t = snap[(size_t)i];
        if (!live(t) || !is_mine(&t)) continue;
        n++;
        int base = t.type * 6;
        CHECK((t.state >= base && t.state <= base + 5) || t.state == 0x78);
        if (t.state == 0x78) continue;
        std::printf("  snapshot %s %d: state %d at %04x,%04x z %d yaw %03x health %d / %d mana %d / %d aux %d castle_size %d "
                    "timer_a %d target %d parent %d child %d sprite 0x%x speed %d / %d\n",
                    t.type == 6 ? "kraken" : "type", i, t.state, (unsigned)t.x, (unsigned)t.y, t.z, t.yaw, (int)t.health, (int)t.max_health,
                    (int)t.mana, (int)t.mana_total, t.aux, t.castle_size, t.timer_a, t.target, t.parent, t.child, t.sprite,
                    t.speed_cur, t.speed_base);
        if (t.type == 6) {
            CHECK_EQ(t.max_health, 9000);
            CHECK_EQ(t.mana_total, 4500);
            CHECK(t.state == 0x25 || t.state == 0x26 || t.state == 0x27);
            CHECK(t.aux >= -90 && t.aux <= 99);
            CHECK(t.castle_size <= 5);
            int segs = 0;
            for (const Thing *s = &snap[t.child]; s != &snap[0]; s = &snap[s->child]) { CHECK_EQ(s->state, 0x78); segs++; }
            CHECK_EQ(segs, 2);
        }
    }
    std::printf("snapshot: %d things of my types\n", n);
}

static void replay_to_snapshot(const std::vector<Thing> &snap, const char *what) {
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
    check_pool(what);
    int same = 0, same_mine = 0, total = 0, total_mine = 0, creatures = 0, creatures_same = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        const Thing &s = snap[(size_t)i], &o = g_state->things[i];
        if (!live(s)) continue;
        total++;
        bool eq = same_thing(s, o);
        if (eq) same++;
        if (s.cls == 5) { creatures++; if (eq) creatures_same++; }
        if (is_mine(&s)) { total_mine++; if (eq) same_mine++; }
    }
    std::printf("replay (%s): %d / %d slots identical, creatures %d / %d, crab/kraken/troll/griffon (+segments) %d / %d; "
                "handlers dispatched without a port: %d\n", what, same, total, creatures_same, creatures, same_mine, total_mine,
                thing_dispatch_report(nullptr));
    thing_dispatch_report(stdout);
    g_video_mode_flags = 8;
    g_hook_player_local_input = saved_input;
}

// ---- 3. smoke runs --------------------------------------------------------------------------------

static void smoke_level(int level, int ticks) {
    void (*saved_input)() = g_hook_player_local_input;
    g_hook_player_local_input = nullptr;
    g_cfg->flags = 0; g_cfg->paused = 0;
    if (!sim_load_level(level)) { std::printf("level %d failed to load\n", level); g_fail++; g_hook_player_local_input = saved_input; return; }
    thing_dispatch_reset_stats();
    char what[64];
    std::snprintf(what, sizeof what, "level %d start", level);
    census(what);
    bool seen[128] = {};
    int eggs = 0;
    std::vector<uint8_t> was_egg((size_t)MC_THING_SLOTS, 0);
    for (int tick = 1; tick <= ticks; tick++) {
        game_tick_sim();
        for (int i = 1; i < MC_THING_SLOTS; i++) {
            const Thing *t = thing_at(i);
            if (is_mine(t)) seen[t->state & 127] = true;
            bool egg = t->cls == 10 && t->type == 0x34;
            if (egg && !was_egg[(size_t)i]) eggs++;
            was_egg[(size_t)i] = egg;
        }
        if (tick % 500 == 0 || tick == ticks) {
            std::snprintf(what, sizeof what, "level %d tick %d", level, tick);
            census(what);
            check_pool(what);
        }
    }
    std::printf("level %d: states of my types seen:", level);
    for (int st = 30; st <= 53; st++) if (seen[st]) std::printf(" %d", st);
    std::printf("; crab eggs laid %d\n", eggs);
    std::printf("level %d: handlers dispatched without a port: %d\n", level, thing_dispatch_report(nullptr));
    thing_dispatch_report(stdout);
    g_hook_player_local_input = saved_input;
}

static void smoke_movie() {
    g_cfg->flags = 0; g_cfg->paused = 0;
    uint16_t saved_mode = g_video_mode_flags;
    sim_prepare_movie();
    if (!demo_open(g_game_dir, 0)) { std::printf("movie/mvi00000.dat missing\n"); g_fail++; g_video_mode_flags = saved_mode; return; }
    thing_dispatch_reset_stats();
    int ticks = 0, first_seen = 0, max_mine = 0;
    bool more = true;
    while (more && ticks < 20000) {
        more = demo_step();
        ticks++;
        int mine = 0;
        for (int i = 1; i < MC_THING_SLOTS; i++) if (is_mine(thing_at(i)) && thing_at(i)->state != 0x78) mine++;
        if (mine && !first_seen) first_seen = ticks;
        if (mine > max_mine) max_mine = mine;
        if (ticks == 1 || ticks % 1000 == 0 || !more) {
            char what[64];
            std::snprintf(what, sizeof what, "movie tick %d", ticks);
            census(what);
            check_pool(what);
        }
    }
    g_video_mode_flags = saved_mode;
    std::printf("movie 0: %d ticks played, %ld / %ld packets; crab/kraken/troll/griffon first seen at tick %d, at most %d; "
                "handlers dispatched without a port: %d\n", ticks, demo_packets_read(), demo_packets_total(), first_seen, max_mine,
                thing_dispatch_report(nullptr));
    CHECK(demo_packets_read() == demo_packets_total());
    thing_dispatch_report(stdout);
    demo_close();
}

// argv[2] == "scan": which campaign levels start with crabs / krakens / trolls / griffons.
static void scan_levels() {
    for (int level = 0; level < 80; level++) {
        if (!sim_load_level(level)) break;
        int by_type[20] = {};
        for (int i = 1; i < MC_THING_SLOTS; i++)
            if (is_creature(thing_at(i)) && thing_at(i)->state != 0x78 && thing_at(i)->type < 20) by_type[thing_at(i)->type]++;
        if (!by_type[5] && !by_type[6] && !by_type[7] && !by_type[8]) continue;
        std::printf("level %2d: crab %d kraken %d troll %d griffon %d\n", level, by_type[5], by_type[6], by_type[7], by_type[8]);
    }
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_game_dir = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    if (!sim_init(g_game_dir)) { std::printf("sim_init failed\n"); return 2; }
    sim_register_gameplay();
    for (int state = 31; state <= 53; state++) CHECK(thing_update_fn(5, state) != nullptr);
    if (argc > 2 && std::strcmp(argv[2], "scan") == 0) scan_levels();
    g_hook_sound_request = sound_hook;

    // 2a. the snapshot's krakens
    if (!sim_load_snapshot("movie/gam00000.dat", "movie/map00000.dat")) { std::printf("snapshot failed to load\n"); return 2; }
    census("snapshot");
    check_pool("snapshot");
    std::vector<Thing> snap(g_state->things, g_state->things + MC_THING_SLOTS);
    CHECK_EQ(g_state->players[0].tick, 413u);
    test_snapshot_fields(snap);

    // 1. construction tests on a fresh level 38 (one tick: the players spawn on their first command)
    if (!sim_load_level(38)) { std::printf("level 38 failed to load\n"); return 2; }
    game_tick_sim();
    CHECK(g_state->players[g_state->local_player].thing != 0);
    test_crab();
    if (!sim_load_level(38)) return 2;
    game_tick_sim();
    test_kraken();
    if (!sim_load_level(38)) return 2;
    game_tick_sim();
    test_troll();
    if (!sim_load_level(38)) return 2;
    game_tick_sim();
    test_griffon();
    g_hook_sound_request = nullptr;

    // 2b. replay of the 412 ticks before the snapshot: my handlers unbound (as before this round) and bound
    static const uint32_t kMine[] = { 0x1a830, 0x1ac20, 0x1ac80, 0x1aed0, 0x1aee0, 0x1afa0, 0x1afb0, 0x1b000, 0x1b390,
                                      0x1b3e0, 0x1b3f0, 0x1b400, 0x1b410, 0x1b470, 0x1b510, 0x1b530, 0x1b540, 0x1b550,
                                      0x1b560, 0x1b940, 0x1ba60, 0x1ba70, 0x1baf0 };
    for (uint32_t a : kMine) thing_register_update(a, nullptr);
    replay_to_snapshot(snap, "creatures2 unbound");
    census("level 38 + 412 ticks, unbound");
    creatures2_register_handlers();
    replay_to_snapshot(snap, "creatures2 bound");
    census("level 38 + 412 ticks, bound");

    // 3. smoke
    smoke_movie();
    static const int kLevels[] = { 12, 24, 44, 25, 49 };
    for (int level : kLevels) smoke_level(level, 3000);

    std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
