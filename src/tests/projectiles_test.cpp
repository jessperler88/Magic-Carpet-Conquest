// Unit test for projectiles.cpp (class 9: flight, steering, impact, target selection).
//  1. pure pieces by construction: scores, steering limits, hit statistics, acceleration;
//  2. every handler on hand-built situations inside the engine's own snapshot of level 38
//     (movie/gam00000.dat): straight flight and lifetime, ground / water, collision, rebound,
//     lightning, castle seed, arrows, mana seekers;
//  3. the four arrows that are in flight in the snapshot: their fields against what the constructor,
//     skeleton_attack_fire_19550 and projectile_type13_s13_update_45b60 produce, then stepped to
//     their end;
//  4. automatic aim against a brute-force search over the pool;
//  5. smoke runs: the movie from the snapshot and generated levels, with projectiles of every type
//     launched from the wizards the way the spell handlers set them up.
// argv[1] = game dir.
#define _CRT_SECURE_NO_WARNINGS
#include "sim.h"
#include "projectiles.h"
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

// ---- helpers -------------------------------------------------------------------------------------

struct SoundCall { int thing, player, sound; };
static std::vector<SoundCall> g_sounds;
static void record_sound(int thing, int player, int sound) { g_sounds.push_back({thing, player, sound}); }

static bool load_snapshot() {
    if (!sim_load_snapshot("movie/gam00000.dat", "movie/map00000.dat")) return false;
    g_cfg->paused = 0;
    g_cfg->substeps = 0;
    g_cfg->flags = 0;
    std::memset(g_cfg->creature_lists, 0, sizeof g_cfg->creature_lists);
    g_cfg->player_list = g_cfg->mana_ball_list = g_cfg->wizard_list = g_cfg->projectile_list = 0;
    g_projectile_null_hit_index = 0;
    return true;
}

static Thing *wizard(int player) { return thing_at(g_state->players[player].thing); }

static void run_handler(Thing *t) {
    ThingUpdateFn fn = thing_update_fn(t->cls, t->state);
    CHECK(fn != nullptr);
    if (fn) fn(t);
}

static std::vector<uint8_t> pool_classes() {
    std::vector<uint8_t> v(MC_THING_SLOTS);
    for (int i = 0; i < MC_THING_SLOTS; i++) v[i] = thing_at(i)->cls;
    return v;
}
// Things allocated since `before` was taken (slots that were free then).
static std::vector<Thing *> created_since(const std::vector<uint8_t> &before) {
    std::vector<Thing *> v;
    for (int i = 1; i < MC_THING_SLOTS; i++)
        if (before[i] == 0 && thing_at(i)->cls != 0) v.push_back(thing_at(i));
    return v;
}
static int live_count() {
    int n = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) n += thing_at(i)->cls != 0;
    return n;
}
static bool pos_eq(const Pos &a, const Pos &b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

// A projectile as a caster would leave it: created by the Table B constructor, then owner / angles.
// `aimed` = the first-tick automatic aim is already done (flag 2).
static Thing *launch(int type, uint16_t x, uint16_t y, int z, int yaw, int pitch, int owner, bool aimed = true) {
    Pos p{x, y, (int16_t)z};
    Thing *t = thing_create(&p, 9, type);
    CHECK(t != nullptr);
    if (!t) return nullptr;
    t->owner = (uint16_t)owner;
    t->yaw = (uint16_t)yaw;
    t->pitch = (uint16_t)pitch;
    if (aimed) t->flags |= 2;
    return t;
}

// The pool, the free stack and the cell lists agree with each other.
static void check_pool_consistent(const char *what) {
    std::vector<uint8_t> is_free(MC_THING_SLOTS);
    int bad = 0;
    for (int i = 0; i <= g_state->free_top; i++) {
        int idx = g_state->free_list[i];
        if (idx <= 0 || idx >= MC_THING_SLOTS || is_free[idx]) { bad++; continue; }
        is_free[idx] = 1;
    }
    for (int i = 1; i < MC_THING_SLOTS; i++)
        if ((thing_at(i)->cls == 0) != (is_free[i] != 0)) bad++;
    // every linked Thing is found in the list of the cell it stands in, and nothing else is
    std::vector<uint8_t> seen(MC_THING_SLOTS);
    for (int c = 0; c < MC_MAP_CELLS; c++) {
        int guard = 0;
        for (unsigned i = g_cell_things[c]; i != 0 && guard < MC_THING_SLOTS; i = thing_at(i)->cell_next, guard++) {
            if (i >= (unsigned)MC_THING_SLOTS) { bad++; break; }
            Thing *t = thing_at(i);
            if (t->cls == 0 || !(t->flags & 4) || mc_cell_of(t->x, t->y) != c || seen[i]) bad++;
            seen[i] = 1;
        }
        if (guard >= MC_THING_SLOTS) bad++;
    }
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        Thing *t = thing_at(i);
        if (t->cls != 0 && (t->flags & 4) && !seen[i]) bad++;
    }
    if (bad) { std::printf("FAIL pool / cell lists inconsistent (%s): %d problem(s)\n", what, bad); g_fail++; }
}

// ---- 1. pure pieces ------------------------------------------------------------------------------

static void test_registration() {
    for (int s = 0; s <= 20; s++) CHECK(thing_update_fn(9, s) != nullptr);
    CHECK(thing_update_fn(9, 0) == projectile_type0_s0_update);
    CHECK(thing_update_fn(9, 1) == projectile_homing_update);
    static const int shared[] = {2, 4, 5, 6, 11, 15, 16, 17, 20};
    for (int s : shared) CHECK(thing_update_fn(9, s) == projectile_update_shared);
    CHECK(thing_update_fn(9, 3) == projectile_type3_s3_update);
    CHECK(thing_update_fn(9, 7) == projectile_type7_s7_update);
    CHECK(thing_update_fn(9, 8) == projectile_type8_s8_update);
    CHECK(thing_update_fn(9, 9) == projectile_lightning_update);
    CHECK(thing_update_fn(9, 10) == projectile_type10_s10_update);
    CHECK(thing_update_fn(9, 12) == projectile_type12_s12_update);
    CHECK(thing_update_fn(9, 13) == projectile_type13_s13_update);
    CHECK(thing_update_fn(9, 14) == projectile_type14_s14_update);
    CHECK(thing_update_fn(9, 18) == projectile_type18_s18_update);
    CHECK(thing_update_fn(9, 19) == projectile_type19_s19_update);
    // the type -> state mapping of the constructors (the handlers are indexed by state)
    static const int state_of[20] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 17, 18, 19, 20};
    for (int ty = 0; ty < 20; ty++) {
        Pos p{0x4000, 0x4000, 0x3000};
        Thing *t = thing_create(&p, 9, ty);
        CHECK(t != nullptr);
        if (!t) continue;
        CHECK_EQ(t->type, ty);
        CHECK_EQ(t->state, state_of[ty]);
        CHECK_EQ(t->health, t->max_health);
        CHECK_EQ(t->flags & 0xa, 0);            // not collidable, first tick not done
        CHECK_EQ(t->impact_cls, 10);
        thing_free(t);
    }
}

static void test_scores() {
    // Thing-shaped probes on the stack: the score functions only read positions, angles, extents.
    Thing a{}, b{};
    a.x = 0x8000; a.y = 0x8000; a.z = 0x1000;
    b.x = 0x8000; b.y = 0x8000 - 0x400; b.z = 0x1000;
    // straight ahead (yaw 0 = -y), level: both differences 0 -> (d cos 0)^2 twice = 2 d^2
    CHECK_EQ(pos_angle_to(thing_pos(&a), thing_pos(&b)), 0);
    CHECK_EQ(target_aim_score(&a, &b, 0x71, 0x71), 2 * 0x400 * 0x400);
    CHECK_EQ(projectile_target_score(&a, &b, 0x71, 0x71), 2 * 0x400 * 0x400);
    CHECK_EQ(target_aim_score(&a, &b, 0, 0), 2 * 0x400 * 0x400);            // the cone test is "above"
    // nearer is better
    b.y = 0x8000 - 0x200;
    CHECK_EQ(target_aim_score(&a, &b, 0x71, 0x71), 2 * 0x200 * 0x200);
    // the horizontal distance limit is 0x1400 inclusive
    b.y = 0x8000 - 0x1400;
    CHECK_EQ(target_aim_score(&a, &b, 0x71, 0x71), 2 * 0x1400 * 0x1400);
    b.y = 0x8000 - 0x1401;
    CHECK_EQ(target_aim_score(&a, &b, 0x71, 0x71), 0xffffffffu);
    CHECK_EQ(projectile_target_score(&a, &b, 0x71, 0x71), 0xffffffffu);
    // a quarter turn to the side: outside a 0x71 cone; inside a 0x200 cone it costs (4 d)^2 + d^2
    b.x = 0x8000 + 0x400; b.y = 0x8000;
    CHECK_EQ(pos_angle_to(thing_pos(&a), thing_pos(&b)), 0x200);
    CHECK_EQ(target_aim_score(&a, &b, 0x71, 0x71), 0xffffffffu);
    CHECK_EQ(target_aim_score(&a, &b, 0x1ff, 0x71), 0xffffffffu);
    CHECK_EQ(target_aim_score(&a, &b, 0x200, 0x71), 16 * 0x400 * 0x400 + 0x400 * 0x400);
    a.yaw = 0x200;
    CHECK_EQ(target_aim_score(&a, &b, 0x71, 0x71), 2 * 0x400 * 0x400);
    // off-axis by a small angle: the formula with the engine's own sine table
    a.yaw = 0x200 - 0x40;
    {
        int32_t d = 0x400;
        int32_t cy = (mc_cos(0x40) * d) >> 16, sy = (mc_sin(0x40) * d * 4) >> 16;
        CHECK_EQ(target_aim_score(&a, &b, 0x71, 0x71), cy * cy + sy * sy + d * d);
        CHECK(target_aim_score(&a, &b, 0x71, 0x71) > 2u * 0x400 * 0x400);   // worse than dead ahead
    }
    CHECK_EQ(target_aim_score(&a, &b, 0x3f, 0x71), 0xffffffffu);
    CHECK(target_aim_score(&a, &b, 0x40, 0x71) != 0xffffffffu);
    // 46470 aims at the middle of the box (z + ext_z0 unless the target is a castle, type 2) and
    // puts z back; 465b0 aims at the base
    a.yaw = 0x200;
    b.ext_z0 = 0x100;
    a.z = 0x1100;
    CHECK_EQ(projectile_target_score(&a, &b, 0x71, 0x71), 2 * 0x400 * 0x400);
    CHECK_EQ(b.z, 0x1000);
    uint32_t base = target_aim_score(&a, &b, 0x71, 0x71);
    CHECK(base != 0xffffffffu && base > 2u * 0x400 * 0x400);
    CHECK_EQ(projectile_target_score(&a, &b, 0x71, 0), 2 * 0x400 * 0x400);
    CHECK_EQ(target_aim_score(&a, &b, 0x71, 0), 0xffffffffu);
    b.type = 2;
    CHECK_EQ(projectile_target_score(&a, &b, 0x71, 0x71), base);
    CHECK_EQ(b.z, 0x1000);
    // every early exit of 46470 restores z as well
    b.type = 0;
    a.yaw = 0;
    CHECK_EQ(projectile_target_score(&a, &b, 0x71, 0x71), 0xffffffffu);     // yaw
    CHECK_EQ(b.z, 0x1000);
    a.yaw = 0x200; a.z = 0x2000;
    CHECK_EQ(projectile_target_score(&a, &b, 0x71, 0x71), 0xffffffffu);     // pitch
    CHECK_EQ(b.z, 0x1000);
    a.z = 0x1100; b.x = 0x8000 + 0x1500;
    CHECK_EQ(projectile_target_score(&a, &b, 0x71, 0x71), 0xffffffffu);     // distance
    CHECK_EQ(b.z, 0x1000);
}

static void test_steering() {
    // descriptor 1 (0x96a30, most projectiles): turn limits 0x16 / 0x16 per tick
    Thing *t = launch(2, 0x4000, 0x4000, 0x3000, 0, 0, 0);
    if (!t) return;
    const MoveDesc *d = mc_move_desc(t->desc);
    CHECK_EQ(d->id, 1);
    CHECK_EQ(d->turn_min, 0x16);
    CHECK_EQ(d->turn_max, 0x16);
    Thing b{};
    b.x = 0x4000 + 0x800; b.y = 0x4000; b.z = 0x3000; b.ext_z0 = 0x40;
    projectile_steer_to_target(t, &b);
    CHECK_EQ(t->target_yaw, 0x200);
    CHECK_EQ(t->yaw, 0x16);
    CHECK_EQ(b.z, 0x3000);                                   // the half-height bracket is closed
    // the target direction is toward the middle of the box, 0x40 above -> pitch turns the "up" way
    int want_pitch = pos_pitch_to(thing_pos(t), thing_pos(&b));   // level: 0
    CHECK_EQ(want_pitch, 0);
    CHECK(t->target_pitch != 0);
    b.z = 0x3040;
    CHECK_EQ(t->target_pitch, pos_pitch_to(thing_pos(t), thing_pos(&b)));
    b.z = 0x3000;
    uint16_t tp = t->target_pitch;
    int dp = angle_diff(0, tp);
    CHECK(dp > 0 && dp < 0x16);
    CHECK_EQ(t->pitch, tp);                                  // closer than the limit: reached at once
    projectile_steer_to_target(t, &b);
    CHECK_EQ(t->yaw, 0x2c);
    for (int i = 0; i < 30; i++) projectile_steer_to_target(t, &b);
    CHECK_EQ(t->yaw, 0x200);
    // the other way round the circle, with the 11-bit wrap
    t->yaw = 0x7f0;
    b.x = 0x4000 - 0x800;                                    // target_yaw 0x600
    projectile_steer_to_target(t, &b);
    CHECK_EQ(t->target_yaw, 0x600);
    CHECK_EQ(t->yaw, 0x7f0 - 0x16);
    t->yaw = 0x7f8;
    b.x = 0x4000 + 0x10; b.y = 0x4000 - 0x800;              // just right of north: through 0
    projectile_steer_to_target(t, &b);
    CHECK_EQ(t->yaw, (0x7f8 + angle_diff(0x7f8, t->target_yaw)) & 0x7ff);
    CHECK(t->yaw < 0x10);
    // 43ff0 aims at the base position
    t->yaw = 0; t->pitch = 0;
    b.x = 0x4000 + 0x800; b.y = 0x4000;
    thing_turn_toward(t, &b);
    CHECK_EQ(t->target_pitch, 0);
    CHECK_EQ(t->pitch, 0);
    CHECK_EQ(t->yaw, 0x16);
    // descriptor of the fireball (0x96ab0) and of lightning (0x96a90): whatever the limits are, a
    // call never turns further than them
    for (int ty : {0, 8, 1}) {
        Thing *u = launch(ty, 0x4000, 0x4000, 0x3000, 0, 0, 0);
        if (!u) continue;
        const MoveDesc *ud = mc_move_desc(u->desc);
        projectile_steer_to_target(u, &b);
        CHECK_EQ(u->yaw, ud->turn_min < 0x200 ? ud->turn_min : 0x200);
        std::printf("steering: type %d descriptor %u turn limits %u / %u, sight %u\n", ty, ud->id, ud->turn_min, ud->turn_max, ud->sight_radius);
        thing_free(u);
    }
    thing_free(t);
}

static void test_hit_stats() {
    Thing *w0 = wizard(0);
    CHECK(w0->cls == 3 && w0->type == 0);
    PlayerBlock *P = player_block(w0);
    Thing *enemy = wizard(1);
    Thing *enemy_castle = nullptr;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        Thing *o = thing_at(i);
        if (o->cls == 3 && o->type == 2 && o->owner == enemy->owner) enemy_castle = o;
    }
    CHECK(enemy_castle != nullptr);
    if (!enemy_castle) return;
    for (int ty = 0; ty < 20; ty++) {
        bool counted = ty == 0 || ty == 1 || ty == 3 || ty == 7 || ty == 8 || ty == 9 || ty == 0x13;
        Thing *t = launch(ty, 0x4000, 0x4000, 0x3000, 0, 0, w0->owner);
        if (!t) continue;
        int32_t shots = P->shots, hits = P->hits;
        projectile_record_hit_stats(t, nullptr, thing_at(0));                 // a miss
        CHECK_EQ(P->shots, shots + (counted ? 1 : 0));
        CHECK_EQ(P->hits, hits);
        projectile_record_hit_stats(t, enemy, enemy);                         // hit what was aimed at
        CHECK_EQ(P->shots, shots + (counted ? 2 : 0));
        CHECK_EQ(P->hits, hits + (counted ? 1 : 0));
        projectile_record_hit_stats(t, enemy_castle, enemy);                  // ... or something of its owner
        CHECK_EQ(P->hits, hits + (counted ? 2 : 0));
        projectile_record_hit_stats(t, enemy, thing_at(0));                   // hit without a target
        projectile_record_hit_stats(t, enemy, w0);                            // hit somebody else's
        CHECK_EQ(P->shots, shots + (counted ? 5 : 0));
        CHECK_EQ(P->hits, hits + (counted ? 2 : 0));
        // not fired by a flyer: nothing is counted anywhere
        t->owner = enemy->owner;                                              // AI wizard: class 3 type 1
        PlayerBlock *PE = player_block(enemy);
        int32_t es = PE->shots, eh = PE->hits;
        projectile_record_hit_stats(t, w0, w0);
        CHECK_EQ(PE->shots, es);
        CHECK_EQ(PE->hits, eh);
        CHECK_EQ(P->shots, shots + (counted ? 5 : 0));
        P->shots = shots; P->hits = hits;
        thing_free(t);
    }
}

// ---- 2. handlers on hand-built situations --------------------------------------------------------

// Flat-out flight in empty sky: one math_rotate_offset step of speed_cur per tick, lifetime
// max_health = range / speed ticks, then the impact effect at the place where it ran out.
static void test_straight_flight() {
    static const int types[] = {2, 4, 5, 6, 11, 14, 15, 16, 19, 3, 0, 8, 12};
    for (int ty : types) {
        auto before = pool_classes();
        Thing *t = launch(ty, 0x4000, 0x4000, 0x3000, 0x200, 0, 0);
        if (!t) continue;
        t->owner = thing_index(t);
        t->impact_type = 0;                       // explosion
        t->damage = 0x7d;
        int life = t->max_health;
        int speed = t->speed_cur;
        CHECK_EQ(life, (ty == 15 ? 0x50 : ty == 12 ? 0x800 / 0x180 : ty == 14 ? 0x1000 / 0x80 : 0x2000 / 0x180));
        CHECK_EQ(speed, (ty == 14 || ty == 15) ? 0x80 : 0x180);
        int ticks = 0;
        Pos expect = *thing_pos(t);
        while (!(t->flags & 0x400) && ticks < 200) {
            run_handler(t);
            ticks++;
            math_rotate_offset(&expect, 0x200, 0, speed);
            CHECK(pos_eq(*thing_pos(t), expect));
            if (!(t->flags & 0x400)) CHECK_EQ(t->health, life - ticks);
            CHECK_EQ(t->speed_cur, speed);
            CHECK_EQ(t->yaw, 0x200);
            CHECK_EQ(t->target_yaw, 0);           // flag 2 was set: no automatic aim, no copy
        }
        CHECK_EQ(ticks, life + 1);
        CHECK_EQ(t->x, 0x4000 + (life + 1) * speed);              // sin(0x200) = 1.0: exactly `speed` per tick
        CHECK_EQ(t->y, 0x4000);
        CHECK_EQ(t->z, 0x3000);
        auto made = created_since(before);
        // type 3 leaves a trail effect (10 / 1) per tick, including the last one; type 8 makes no
        // effect unless it hit a player
        std::vector<Thing *> effects, trail;
        for (Thing *e : made) {
            if (e == t) continue;
            if (ty == 3 && e->cls == 10 && e->type == 1) trail.push_back(e); else effects.push_back(e);
        }
        if (ty == 3) {
            CHECK_EQ((int)trail.size(), life + 1);
            for (Thing *e : trail) { CHECK_EQ(e->owner, t->owner); CHECK_EQ(e->flags & 0x10080, 0x10080); }
        }
        if (ty == 8) {
            CHECK_EQ((int)effects.size(), 0);
        } else {
            CHECK_EQ((int)effects.size(), 1);
            if (effects.size() == 1) {
                Thing *e = effects[0];
                CHECK_EQ(e->cls, 10);
                CHECK_EQ(e->type, ty == 12 ? 0x26 : 0);
                CHECK(pos_eq(*thing_pos(e), *thing_pos(t)));
                CHECK_EQ(e->owner, t->owner);
                CHECK_EQ(e->yaw, 0x200);
                CHECK_EQ(e->pitch, 0);
                if (ty != 0) {                    // 44510 passes neither damage nor target on
                    CHECK_EQ(e->damage, 0x7d);
                    CHECK_EQ(e->target, 0);       // nothing was hit: g_projectile_null_hit_index
                }
                if (ty == 12) { CHECK_EQ(e->impact_cls, 10); CHECK_EQ(e->impact_type, 0); }
            }
        }
        for (Thing *e : made) thing_free(e);
    }
    // the null-hit index is what the port writes when nothing was hit
    {
        auto before = pool_classes();
        Thing *t = launch(2, 0x4000, 0x4000, 0x3000, 0x200, 0, 0);
        if (t) {
            t->owner = thing_index(t);
            t->health = 0;
            g_projectile_null_hit_index = 0xd6b5;
            run_handler(t);
            g_projectile_null_hit_index = 0;
            auto made = created_since(before);
            CHECK_EQ((int)made.size(), 2);
            for (Thing *e : made) { if (e != t) CHECK_EQ(e->target, 0xd6b5); thing_free(e); }
        }
    }
    // teleport (impact type 0x22): the effect's lifetime is the projectile's damage word
    {
        auto before = pool_classes();
        Thing *t = launch(11, 0x4000, 0x4000, 0x3000, 0x200, 0, 0);
        if (t) {
            t->owner = thing_index(t);
            t->health = 0;
            t->impact_type = 0x22;
            t->damage = 777;
            run_handler(t);
            auto made = created_since(before);
            CHECK_EQ((int)made.size(), 2);
            for (Thing *e : made) {
                if (e != t) { CHECK_EQ(e->type, 0x22); CHECK_EQ(e->health, 777); CHECK_EQ(e->damage, 777); }
                thing_free(e);
            }
        }
    }
}

static void test_acceleration() {
    struct Case { int cur, base, after1, after2; };
    // speed_cur += 2 * sign(speed_base - speed_cur), before the move of the same tick
    static const Case cases[] = {{0x100, 0x180, 0x102, 0x104}, {0x200, 0x180, 0x1fe, 0x1fc}, {0x17f, 0x180, 0x181, 0x17f},
                                 {0x180, 0x180, 0x180, 0x180}, {0x1d0, 0x180, 0x1ce, 0x1cc}};
    for (const Case &c : cases) {
        Thing *t = launch(2, 0x4000, 0x4000, 0x3000, 0x200, 0, 0);
        if (!t) continue;
        t->owner = thing_index(t);
        t->speed_cur = (int16_t)c.cur;
        t->speed_base = (int16_t)c.base;
        run_handler(t);
        CHECK_EQ(t->speed_cur, c.after1);
        CHECK_EQ(t->x, 0x4000 + c.after1);
        run_handler(t);
        CHECK_EQ(t->speed_cur, c.after2);
        CHECK_EQ(t->x, 0x4000 + c.after1 + c.after2);
        thing_free(t);
    }
    // the fireball (44510) and the ground huggers (448b0, 45c90) and arrows (45b60) do not accelerate
    for (int ty : {0, 1, 17, 13}) {
        Thing *t = launch(ty, 0x4000, 0x4000, 0x3000, 0x200, 0, 0);
        if (!t) continue;
        t->owner = thing_index(t);
        t->speed_cur = 0x1d0;                       // launched from a moving wizard: 0x180 + 0x50
        run_handler(t);
        CHECK_EQ(t->speed_cur, 0x1d0);
        CHECK_EQ(t->x, 0x4000 + 0x1d0);
        thing_free(t);
    }
}

// A cell whose terrain type mask is exactly `water` ? 1 : anything else, away from every Thing.
static bool find_cell(bool water, Pos *out) {
    for (int y = 4; y < 252; y += 3)
        for (int x = 4; x < 252; x += 3) {
            Pos p{(uint16_t)(x * 256 + 0x80), (uint16_t)(y * 256 + 0x80), 0};
            bool is_water = terrain_type_mask_at(&p) == 1;
            if (is_water != water) continue;
            bool empty = true;
            for (int dy = -2; dy <= 2 && empty; dy++)
                for (int dx = -2; dx <= 2 && empty; dx++)
                    if (g_cell_things[mc_cell((unsigned)(x + dx), (unsigned)(y + dy))] != 0) empty = false;
            if (!empty) continue;
            p.z = (int16_t)terrain_height_at(&p);
            if (!water && p.z < 0x100) continue;
            *out = p;
            return true;
        }
    return false;
}

static void test_ground_and_water() {
    Pos land{}, sea{};
    CHECK(find_cell(false, &land));
    CHECK(find_cell(true, &sea));
    std::printf("ground test: land cell %02x,%02x height %d, water cell %02x,%02x height %d\n",
                land.x >> 8, land.y >> 8, land.z, sea.x >> 8, sea.y >> 8, sea.z);
    struct Case { int type; bool water; int expect_type; bool steps_back; };
    // pitch 0x200 = straight down (z -= sin(pitch) * speed): one tick takes it 0x100 under the ground
    static const Case cases[] = {
        {2, false, 0xf, false}, {2, true, 5, false},     // water: a splash instead of the impact effect
        {4, false, 9, false},   {4, true, 9, false},     // type 4 ignores water
        {0, false, 0, true},    {0, true, 5, true},      // 44510 backs out of the ground first
        {12, false, 0x26, false}, {12, true, 5, false},
        {8, false, -1, false},  {8, true, 5, false},     // 44aa0: no effect on the ground, a splash on water
        {1, false, 0xc, false}, {1, true, 0xc, false},   // 448b0 hugs the ground: see below
    };
    for (const Case &c : cases) {
        const Pos &at = c.water ? sea : land;
        auto before = pool_classes();
        Thing *t = launch(c.type, at.x, at.y, at.z + 0x80, 0x100, 0x200, 0);
        if (!t) continue;
        t->owner = thing_index(t);
        t->impact_type = (uint8_t)(c.type == 2 ? 0xf : c.type == 4 ? 9 : c.type == 1 ? 0xc : 0);
        Pos start = *thing_pos(t);
        if (c.type == 1) {
            // the ground hugger is lifted onto the terrain by its own move and only ends when its
            // lifetime does (or when the ground is above it after the move, which the lift prevents)
            run_handler(t);
            CHECK(!(t->flags & 0x400));
            CHECK_EQ(t->z, terrain_height_at(thing_pos(t)));
            CHECK_EQ(t->health, t->max_health - 1);
            t->health = 0;
            run_handler(t);
        } else {
            run_handler(t);
        }
        CHECK(t->flags & 0x400);
        if (c.type != 1) {
            if (c.steps_back) CHECK(pos_eq(*thing_pos(t), start));
            else              CHECK_EQ(t->z, start.z - 0x180);
        }
        auto made = created_since(before);
        int effects = 0;
        for (Thing *e : made) {
            if (e == t) continue;
            effects++;
            CHECK_EQ(e->cls, 10);
            CHECK_EQ(e->type, c.expect_type);
            CHECK_EQ(e->owner, t->owner);
            // created at the projectile's position; the splash constructor (38950) then puts
            // itself on the water surface
            CHECK_EQ(e->x, t->x);
            CHECK_EQ(e->y, t->y);
            if (e->type == 5) CHECK_EQ(e->z, terrain_height_at(thing_pos(e)));
            else              CHECK_EQ(e->z, t->z);
        }
        CHECK_EQ(effects, c.expect_type < 0 ? 0 : 1);
        for (Thing *e : made) thing_free(e);
    }
}

// Collision with a Thing: the projectile stops in the middle of its box and leaves the impact effect
// with the Thing's index; a rebounding target (flag 0x8000) throws fireball / meteor impacts back.
static void test_collision_and_rebound() {
    Thing *w0 = wizard(0), *w1 = wizard(1);
    CHECK(w1->cls == 3 && w1->type == 1 && (w1->flags & 8));
    PlayerBlock *P = player_block(w0);
    g_sounds.clear();
    // start one step short of the wizard's middle, flying at it; the filter keeps the wizard's own
    // things (spells, castle) out of the collision search
    auto aim_at_w1 = [&](int type) -> Thing * {
        Pos p = *thing_pos(w1);
        p.z = (int16_t)(p.z + w1->ext_z0);
        math_rotate_offset(&p, 0x600, 0, 0x180);
        Thing *t = launch(type, p.x, p.y, p.z, 0x200, 0, w0->owner);
        if (t) { t->filter_cls = 3; t->filter_type = 1; }
        return t;
    };
    const Pos w1_pos = *thing_pos(w1);
    const Pos w1_mid{w1->x, w1->y, (int16_t)(w1->z + w1->ext_z0)};
    const int32_t w1_mana = w1->mana;
    const uint32_t w1_flags = w1->flags;

    // --- plain hit, every flying kind -----------------------------------------------------------
    static const int types[] = {2, 3, 8, 7, 12, 0};
    for (int ty : types) {
        auto before = pool_classes();
        Thing *t = aim_at_w1(ty);
        if (!t) continue;
        t->impact_type = 0x11;
        t->damage = 321;
        int32_t shots = P->shots, hits = P->hits;
        run_handler(t);
        CHECK(t->flags & 0x400);
        CHECK(pos_eq(*thing_pos(t), w1_mid));
        CHECK(pos_eq(*thing_pos(w1), w1_pos));
        auto made = created_since(before);
        int effects = 0;
        for (Thing *e : made) {
            if (e == t) continue;
            if (ty == 3 && e->type == 1) continue;                    // the trail
            effects++;
            CHECK_EQ(e->cls, 10);
            CHECK_EQ(e->type, ty == 12 ? 0x26 : 0x11);
            CHECK_EQ(e->owner, w0->owner);
            CHECK_EQ(e->yaw, 0x200);
            if (ty != 0) { CHECK_EQ(e->target, thing_index(w1)); CHECK_EQ(e->damage, 321); }
            else         { CHECK_EQ(e->target, 0); CHECK(e->damage != 321); }    // 44510: the effect constructor's values stay
            CHECK(pos_eq(*thing_pos(e), w1_mid));
        }
        CHECK_EQ(effects, 1);
        // statistics: types 0, 3, 7, 8 count a shot; a hit needs a target of the same owner, and
        // these were launched without one (Thing.target 0)
        bool counted = ty == 0 || ty == 3 || ty == 7 || ty == 8;
        CHECK_EQ(P->shots, shots + (counted ? 1 : 0));
        CHECK_EQ(P->hits, hits);
        for (Thing *e : made) thing_free(e);
    }
    // --- with a target: steered, and the hit is counted --------------------------------------------
    {
        auto before = pool_classes();
        Thing *t = aim_at_w1(8);
        if (t) {
            t->target = thing_index(w1);
            t->impact_type = 0x19;
            int32_t shots = P->shots, hits = P->hits;
            run_handler(t);
            CHECK(t->flags & 0x400);
            CHECK_EQ(P->shots, shots + 1);
            CHECK_EQ(P->hits, hits + 1);
            CHECK_EQ(t->target_yaw, 0x200);
            auto made = created_since(before);
            CHECK_EQ((int)made.size(), 2);
            for (Thing *e : made) { if (e != t) { CHECK_EQ(e->type, 0x19); CHECK_EQ(e->target, thing_index(w1)); } thing_free(e); }
            P->shots = shots; P->hits = hits;
        }
    }
    // --- 44aa0 hitting something that is not a player: no effect -----------------------------------
    {
        Thing *victim = nullptr;
        for (int i = 1; i < MC_THING_SLOTS && !victim; i++) {
            Thing *o = thing_at(i);
            if (o->cls == 5 && (o->flags & 0xc) == 0xc && o->health >= 0) victim = o;
        }
        CHECK(victim != nullptr);
        if (victim) {
            auto before = pool_classes();
            Pos p = *thing_pos(victim);
            p.z = (int16_t)(p.z + victim->ext_z0);
            math_rotate_offset(&p, 0x600, 0, 0x180);
            Thing *t = launch(8, p.x, p.y, p.z, 0x200, 0, w0->owner);
            if (t) {
                t->filter_cls = 5; t->filter_type = victim->type;
                int32_t shots = P->shots;
                run_handler(t);
                CHECK(t->flags & 0x400);
                CHECK_EQ(t->x, victim->x);
                CHECK_EQ(P->shots, shots + 1);
                auto made = created_since(before);
                CHECK_EQ((int)made.size(), 1);
                for (Thing *e : made) thing_free(e);
                P->shots = shots;
            }
        }
    }
    // --- rebound -----------------------------------------------------------------------------------
    struct Case { int type; int impact; int32_t target_mana; int expect; };   // expect: 0 reflect, 1 explode, 2 pass through
    static const Case cases[] = {
        {2, 1, 1000, 0}, {3, 0x11, 1000, 0}, {8, 1, 12, 0}, {12, 0x11, 12, 0},
        {2, 0, 1000, 1},      // not a fireball / meteor impact: explodes
        {2, 1, 11, 1},        // target mana below a quarter of the projectile's 0x32: explodes
        {0, 0, 1000, 0},      // 44510 reflects whatever the impact type is
        {0, 0, 11, 2},        // ... and a target too weak lets it pass
    };
    for (const Case &c : cases) {
        auto before = pool_classes();
        Thing *t = aim_at_w1(c.type);
        if (!t) continue;
        CHECK_EQ(t->mana, 0x32);
        t->impact_type = (uint8_t)c.impact;
        t->pitch = 0x7f0;                                   // slightly up; the path is short enough to still hit
        t->health = 5;
        w1->flags |= 0x8000;
        w1->mana = c.target_mana;
        uint32_t rng = t->rng;
        g_sounds.clear();
        run_handler(t);
        Pos moved = *thing_pos(t);
        if (c.expect == 0) {
            unsigned spread = c.type == 0 ? 0x5b : 0x2d;
            uint32_t r = mc_lcg(rng);
            CHECK(!(t->flags & 0x400));
            CHECK_EQ(w1->mana, c.target_mana - 12);                       // 0x32 / 4
            CHECK_EQ(t->target_yaw, 0x600);
            CHECK_EQ(t->rng, r);
            CHECK_EQ(t->yaw, (uint16_t)(0x600 + r % spread - spread / 2));
            CHECK_EQ(t->pitch, 0x10);                                     // mirrored: -0x7f0 & 0x7ff
            CHECK_EQ(t->target_pitch, 0x10);
            CHECK_EQ(t->owner, w1->owner);
            CHECK_EQ(t->target, w0->owner);                               // back at whoever fired it
            CHECK_EQ(t->health, t->max_health);
            CHECK_EQ(t->x, w1->x); CHECK_EQ(t->y, w1->y);
            CHECK_EQ(t->z, w1->z + w1->ext_h);
            CHECK_EQ((int)g_sounds.size(), 1);
            if (g_sounds.size() == 1) { CHECK_EQ(g_sounds[0].thing, thing_index(w1)); CHECK_EQ(g_sounds[0].player, -1); CHECK_EQ(g_sounds[0].sound, 0x1c); }
            CHECK_EQ((int)created_since(before).size(), c.type == 3 ? 2 : 1);
            // next tick it homes on its former owner
            run_handler(t);
            CHECK_EQ(t->target_yaw, pos_angle_to(&moved, thing_pos(w0)) & 0xffff);
        } else if (c.expect == 1) {
            CHECK(t->flags & 0x400);
            CHECK_EQ(w1->mana, c.target_mana);
            CHECK(pos_eq(*thing_pos(t), w1_mid));
            CHECK_EQ((int)created_since(before).size(), 2);
            CHECK_EQ((int)g_sounds.size(), 0);
        } else {
            CHECK(!(t->flags & 0x400));
            CHECK_EQ(w1->mana, c.target_mana);
            CHECK_EQ(t->health, 5);                                       // not even the lifetime runs
            CHECK_EQ(t->owner, w0->owner);
            CHECK_EQ((int)created_since(before).size(), 1);
            CHECK_EQ((int)g_sounds.size(), 0);
        }
        CHECK(pos_eq(*thing_pos(w1), w1_pos));
        for (Thing *e : created_since(before)) thing_free(e);
        w1->flags = w1_flags;
        w1->mana = w1_mana;
    }
}

// Lightning: the ray is traced in the first tick, 8 bolt segments per step + 1 are laid along it and
// the impact effect appears at its end. The segments live until the next tick.
static void test_lightning() {
    g_cfg->player_list = 0;                         // nobody to aim at
    std::memset(g_cfg->creature_lists, 0, sizeof g_cfg->creature_lists);
    auto before = pool_classes();
    Thing *t = launch(9, 0x4000, 0x4000, 0x3000, 0x200, 0x7c0, 0, false);
    if (!t) return;
    t->owner = thing_index(t);
    t->impact_type = 0x17;
    t->damage = 0x1f4;
    t->speed_cur = 0x1d0;                           // as launched from a moving wizard: reset to speed_base
    CHECK_EQ(t->max_health, 0xe00 / 0x180);         // 9
    const Pos start = *thing_pos(t);
    int free_before = thing_free_count();
    uint32_t rng0 = t->rng;
    run_handler(t);
    CHECK(t->flags & 0x400);
    CHECK(t->flags & 2);
    CHECK(!(t->flags & 4));                         // taken out of its cell
    CHECK_EQ(t->speed_cur, 0x180);
    CHECK_EQ(t->health, -1);
    CHECK_EQ(t->yaw, 0x200);
    CHECK_EQ(t->target_yaw, 0x200);                 // no target found: launch direction recorded
    CHECK_EQ(t->target_pitch, 0x7c0);
    // the ray: 10 steps of 0x180
    Pos end = start;
    for (int i = 0; i < 10; i++) math_rotate_offset(&end, 0x200, 0x7c0, 0x180);
    CHECK(pos_eq(*thing_pos(t), end));
    auto made = created_since(before);
    std::vector<Thing *> segs;
    Thing *effect = nullptr;
    for (Thing *e : made) {
        if (e == t) continue;
        if (e->cls == 9) segs.push_back(e); else effect = e;
    }
    CHECK_EQ((int)segs.size(), 81);                 // count = 10 steps * 8, loop runs count .. 0
    CHECK_EQ(free_before - thing_free_count(), 82);
    // Segments are allocated in order from the top of the free stack; recover the order by walking
    // along the ray: segment k sits at start + k * delta displaced sideways (yaw + 0x200) and
    // vertically by the same `wander` = 12 * off_b, |off_b| <= 9, changing by exactly 1 per segment.
    Pos delta{0, 0, 0};
    math_rotate_offset(&delta, 0x200, 0x7c0, 0x180 / 8);
    std::vector<int> wander_of(82, 1 << 20);
    int matched = 0;
    for (Thing *s : segs) {
        CHECK_EQ(s->type, 9);
        CHECK_EQ(s->state, 0xe);
        CHECK_EQ(s->sprite, 0xd8);
        CHECK_EQ(s->owner, t->owner);
        CHECK(s->flags & 4);
        CHECK_EQ(s->max_health, s > t ? 0 : -1);
        CHECK_EQ(s->health, s->max_health);
        bool found = false;
        for (int k = 0; k <= 80 && !found; k++) {
            if (wander_of[k] != (1 << 20)) continue;
            Pos axis{(uint16_t)(start.x + k * delta.x), (uint16_t)(start.y + k * delta.y), (int16_t)(start.z + k * delta.z)};
            int w = s->z - axis.z;
            if (w % 12 != 0 || w < -108 || w > 108) continue;
            Pos want = axis;
            want.z = (int16_t)(axis.z + w);
            math_rotate_offset(&want, 0x400, 0, w);
            if (!pos_eq(want, *thing_pos(s))) continue;
            wander_of[k] = w;
            found = true;
        }
        if (found) matched++;
    }
    CHECK_EQ(matched, 81);
    CHECK_EQ(wander_of[0], 0);                      // the first segment is the launch position itself
    int max_w = 0;
    for (int k = 1; k <= 80; k++) {
        int dw = wander_of[k] - wander_of[k - 1];
        CHECK(dw == 12 || dw == -12);
        if (wander_of[k] > max_w) max_w = wander_of[k];
        if (-wander_of[k] > max_w) max_w = -wander_of[k];
    }
    // near the far end the walk is squeezed back to 0: limit = min(8, remaining / 2)
    CHECK(wander_of[80] >= -24 && wander_of[80] <= 24);
    CHECK(wander_of[79] >= -36 && wander_of[79] <= 36);
    CHECK(t->rng != rng0);
    std::printf("lightning: 81 segments, widest wander %d units, end at %04x %04x %d\n", max_w, (unsigned)end.x, (unsigned)end.y, end.z);
    // the impact effect: one segment step past the last segment, with the last wander
    CHECK(effect != nullptr);
    if (effect) {
        CHECK_EQ(effect->cls, 10);
        CHECK_EQ(effect->type, 0x17);
        CHECK_EQ(effect->owner, t->owner);
        CHECK_EQ(effect->yaw, 0x200);
        CHECK_EQ(effect->pitch, 0x7c0);
        CHECK_EQ(effect->damage, 0x1f4);
        CHECK_EQ(effect->target, 0);
        Pos axis{(uint16_t)(start.x + 81 * delta.x), (uint16_t)(start.y + 81 * delta.y), (int16_t)(start.z + 81 * delta.z)};
        int w = effect->z - axis.z;
        CHECK(w == wander_of[80] + 12 || w == wander_of[80] - 12);
        CHECK(pos_dist_xyz(thing_pos(effect), &axis) <= 200);
    }
    // segment lifetime (45c70): gone at the tick after the bolt, whatever side of the bolt's slot
    for (Thing *s : segs) {
        if (s > t) { run_handler(s); CHECK(!(s->flags & 0x400)); CHECK_EQ(s->health, -1); }   // rest of the pass
    }
    for (Thing *s : segs) { run_handler(s); CHECK(s->flags & 0x400); }
    for (Thing *e : made) thing_free(e);

    // a rebounding player in the ray gets a quarter of the damage; the hit is counted
    Thing *w0 = wizard(0), *w1 = wizard(1);
    const uint32_t w1_flags = w1->flags;
    for (int pass = 0; pass < 3; pass++) {
        auto b2 = pool_classes();
        Pos p = *thing_pos(w1);
        p.z = (int16_t)(p.z + w1->ext_z0);
        math_rotate_offset(&p, 0x600, 0, 3 * 0x180);
        Thing *u = launch(9, p.x, p.y, p.z, 0x200, 0, w0->owner, true);
        if (!u) continue;
        u->filter_cls = 3; u->filter_type = 1;
        u->impact_type = 0x17;
        u->damage = 0x1f4;
        u->target = thing_index(w1);
        if (pass >= 1) { w1->flags |= 0x8000; }
        int32_t mana = w1->mana;
        if (pass == 2) w1->mana = 11;
        int32_t shots = player_block(w0)->shots, hits = player_block(w0)->hits;
        run_handler(u);
        CHECK(pos_eq(*thing_pos(u), *thing_pos(w1)));              // 44ea0 stops at the base position
        CHECK_EQ(u->health, u->max_health - 2);                      // two free steps, the third hit
        auto m2 = created_since(b2);
        int nseg = 0;
        for (Thing *e : m2) {
            if (e == u) continue;
            if (e->cls == 9) { nseg++; continue; }
            CHECK_EQ(e->type, 0x17);
            CHECK_EQ(e->target, thing_index(w1));
            CHECK_EQ(e->damage, pass == 1 ? 0x1f4 / 4 : 0x1f4);
        }
        CHECK_EQ(nseg, 3 * 8 + 1);
        CHECK_EQ(player_block(w0)->shots, shots + 1);
        CHECK_EQ(player_block(w0)->hits, hits + 1);
        player_block(w0)->shots = shots; player_block(w0)->hits = hits;
        for (Thing *e : m2) thing_free(e);
        w1->flags = w1_flags;
        w1->mana = mana;
    }
}

// The castle seed (state 10).
static void test_castle_seed() {
    Thing *w0 = wizard(0);
    PlayerBlock *P = player_block(w0);
    // a free site, searched like the spell does: 0x1000 ahead of a launch point, on the ground
    Pos site{};
    bool have = false;
    for (int y = 8; y < 248 && !have; y += 4)
        for (int x = 8; x < 248 && !have; x += 4) {
            Pos p{(uint16_t)(x << 8), (uint16_t)(y << 8), 0};
            Pos q = p;
            math_rotate_offset(&q, 0x200, 0, 0x1000);
            if (castle_site_clear_at_pos(&p) && castle_site_clear_at_pos(&q) && terrain_type_mask_at(&q) != 1) { site = p; have = true; }
        }
    CHECK(have);
    if (!have) return;
    const uint16_t had_castle = P->castle;
    // --- no castle yet: fly to Thing.home, become a class 3 type 2 Thing there ---------------------
    {
        auto before = pool_classes();
        P->castle = 0;
        site.z = (int16_t)(terrain_height_at(&site) + 0x200);
        Thing *t = launch(10, site.x, site.y, site.z, 0x200, 0, w0->owner, false);
        if (!t) return;
        t->impact_cls = 3; t->impact_type = 2;
        t->home = site;
        math_rotate_offset(&t->home, 0x200, 0, 0x1000);
        t->home.z = (int16_t)terrain_height_at(&t->home);
        // first tick: only the site test at the launch point
        Pos p0 = *thing_pos(t);
        run_handler(t);
        CHECK(t->flags & 2);
        CHECK(!(t->flags & 0x400));
        CHECK(pos_eq(*thing_pos(t), p0));
        CHECK_EQ(t->health, t->max_health);
        int ticks = 0, max_turn = 0;
        while (!(t->flags & 0x400) && ticks < 100) {
            uint16_t yaw = t->yaw, pitch = t->pitch;
            Pos prev = *thing_pos(t);
            run_handler(t);
            ticks++;
            int dy = angle_diff(yaw, t->yaw), dp = angle_diff(pitch, t->pitch);
            if (dy > max_turn) max_turn = dy;
            if (dp > max_turn) max_turn = dp;
            CHECK_EQ(t->target_yaw, pos_angle_to(&prev, &t->home) & 0xffff);
        }
        CHECK(t->flags & 0x400);
        CHECK(max_turn <= 0x16);
        auto made = created_since(before);
        Thing *castle = nullptr;
        for (Thing *e : made) if (e != t) castle = e;
        CHECK_EQ((int)made.size(), 2);
        CHECK(castle != nullptr);
        if (castle) {
            CHECK_EQ(castle->cls, 3);
            CHECK_EQ(castle->type, 2);
            CHECK_EQ(castle->owner, w0->owner);
            // the castle constructor snaps to a cell corner (x + 1 cell when x + y cells is odd)
            CHECK_EQ(castle->y, t->y & 0xff00);
            CHECK(castle->x == (t->x & 0xff00) || castle->x == (uint16_t)((t->x & 0xff00) + 0x100));
            int miss = pos_dist_xy(thing_pos(t), &t->home);
            std::printf("castle seed: landed after %d ticks, %d units from the destination, ground %d, z %d\n",
                        ticks, miss, terrain_height_at(thing_pos(t)), t->z);
            CHECK(miss < 0x180);
        }
        for (Thing *e : made) thing_free(e);
    }
    // --- launch point on a taken site: the Castle spell is released and the seed removed -----------
    {
        Thing *enemy_castle = nullptr;
        for (int i = 1; i < MC_THING_SLOTS; i++) {
            Thing *o = thing_at(i);
            if (o->cls == 3 && o->type == 2) enemy_castle = o;
        }
        CHECK(enemy_castle != nullptr);
        uint16_t spell_idx = P->spell_thing[16];
        if (enemy_castle && spell_idx) {
            Thing *spell = thing_at(spell_idx);
            int16_t saved = spell->cast_ticks;
            spell->cast_ticks = 7;
            CHECK_EQ(castle_site_clear_at_pos(thing_pos(enemy_castle)), 0);
            Thing *t = launch(10, enemy_castle->x, enemy_castle->y, enemy_castle->z + 0x400, 0, 0, w0->owner, false);
            if (t) {
                t->impact_cls = 3; t->impact_type = 2;
                run_handler(t);
                CHECK(t->flags & 0x400);
                CHECK_EQ(spell->cast_ticks, 0);
                thing_free(t);
            }
            spell->cast_ticks = saved;
        } else {
            std::printf("castle seed: no Castle spell Thing for player 0 in the snapshot, release not checked\n");
        }
    }
    // --- flying over a taken site: one step back, built there ------------------------------------
    {
        Thing *enemy_castle = nullptr;
        for (int i = 1; i < MC_THING_SLOTS; i++) {
            Thing *o = thing_at(i);
            if (o->cls == 3 && o->type == 2) enemy_castle = o;
        }
        if (enemy_castle) {
            // approach the castle from a clear cell far out, high above its box
            Pos from{};
            bool ok = false;
            for (int d = 0x1400; d < 0x6000 && !ok; d += 0x100) {
                from = *thing_pos(enemy_castle);
                math_rotate_offset(&from, 0x600, 0, d);
                ok = castle_site_clear_at_pos(&from) != 0;
            }
            CHECK(ok);
            auto before = pool_classes();
            from.z = 0x6000;
            Thing *t = launch(10, from.x, from.y, from.z, 0x200, 0, w0->owner, true);
            if (t && ok) {
                t->impact_cls = 3; t->impact_type = 2;
                t->home = *thing_pos(enemy_castle);
                t->home.z = 0x6000;
                t->health = t->max_health = 1000;
                int ticks = 0;
                Pos prev = *thing_pos(t);
                while (!(t->flags & 0x400) && ticks < 200) { prev = *thing_pos(t); run_handler(t); ticks++; }
                CHECK(t->flags & 0x400);
                CHECK(ticks < 200);
                // the end position is the last step undone (to rounding): a clear site again
                CHECK(pos_dist_xyz(thing_pos(t), &prev) <= 4);
                CHECK_EQ(castle_site_clear_at_pos(thing_pos(t)), 1);
                auto made = created_since(before);
                CHECK_EQ((int)made.size(), 2);
                for (Thing *e : made) { if (e != t) { CHECK_EQ(e->cls, 3); CHECK_EQ(e->type, 2); CHECK_EQ(e->y, t->y & 0xff00); } }
                std::printf("castle seed: stopped %d units short of the enemy castle after %d ticks\n",
                            pos_dist_xy(thing_pos(t), thing_pos(enemy_castle)), ticks);
                for (Thing *e : made) thing_free(e);
            }
        }
    }
    // --- with a target Thing (the own castle): homes on it, leaves the upgrade effect -------------
    {
        Thing *own = nullptr;
        for (int i = 1; i < MC_THING_SLOTS; i++) {
            Thing *o = thing_at(i);
            if (o->cls == 3 && o->type == 2) own = o;       // any castle will do as the Thing to home on
        }
        if (own) {
            auto before = pool_classes();
            Pos p = *thing_pos(own);
            p.z = (int16_t)(own->z + 0x200);
            math_rotate_offset(&p, 0x600, 0, 0x2400);
            Thing *t = launch(10, p.x, p.y, p.z, 0x200, 0, w0->owner, false);
            if (t) {
                t->impact_cls = 10; t->impact_type = 0x2b;
                t->target = thing_index(own);
                int ticks = 0;
                while (!(t->flags & 0x400) && ticks < 100) { run_handler(t); ticks++; }
                CHECK(t->flags & 0x400);
                CHECK(pos_eq(*thing_pos(t), *thing_pos(own)));      // stops at the Thing's base position
                auto made = created_since(before);
                CHECK_EQ((int)made.size(), 2);
                for (Thing *e : made) { if (e != t) { CHECK_EQ(e->cls, 10); CHECK_EQ(e->type, 0x2b); CHECK_EQ(e->owner, w0->owner); } }
                std::printf("castle seed: reached the castle box after %d ticks\n", ticks);
                for (Thing *e : made) thing_free(e);
                // a "new castle" seed that arrives while the owner has a castle is dropped
                auto b3 = pool_classes();
                P->castle = thing_index(own);
                Thing *u = launch(10, own->x, own->y, own->z, 0x200, 0, w0->owner, true);
                if (u) {
                    u->impact_cls = 3; u->impact_type = 2;
                    u->target = thing_index(own);
                    run_handler(u);
                    CHECK(u->flags & 0x400);
                    CHECK_EQ((int)created_since(b3).size(), 1);
                    thing_free(u);
                }
            }
        }
    }
    P->castle = had_castle;
}

// A live, linked, collidable, damageable creature narrower than a projectile step with no other
// creature within 0x400 (so that "the nearest thing ahead" is unambiguous).
static Thing *find_isolated_creature() {
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        Thing *o = thing_at(i);
        if (o->cls != 5 || (o->flags & 0xc) != 0xc || o->health < 0 || !(o->prop_flags & 1)) continue;
        if (o->state == 0x78 || o->type >= 20 || o->ext_x >= 0x100) continue;
        if (o->z + o->ext_z0 < terrain_height_at(thing_pos(o))) continue;
        bool alone = true;
        for (int j = 1; j < MC_THING_SLOTS && alone; j++) {
            Thing *q = thing_at(j);
            if (j != i && q->cls == 5 && pos_dist_xy(thing_pos(o), thing_pos(q)) < 0x400) alone = false;
        }
        if (alone) return o;
    }
    return nullptr;
}

// Arrows (state 13): fly while lifetime is left and nothing is in the way, then area damage.
static void test_arrow() {
    g_sounds.clear();
    // lifetime in empty sky: max_health moves, the update after that ends it without moving
    {
        Thing *t = launch(13, 0x4000, 0x4000, 0x3000, 0x200, 0, 0, false);
        if (!t) return;
        t->owner = thing_index(t);
        CHECK_EQ(t->max_health, 13);
        uint32_t rng = t->rng;
        int ticks = 0;
        while (!(t->flags & 0x400) && ticks < 100) { run_handler(t); ticks++; }
        CHECK_EQ(ticks, 14);
        CHECK_EQ(t->x, 0x4000 + 13 * 0x180);
        CHECK_EQ(t->health, -1);
        // one "whoosh" request on the first tick: sound 0x21 + (rng & 3), one RNG draw
        CHECK_EQ(t->rng, mc_lcg(rng));
        CHECK_EQ((int)g_sounds.size(), 1);
        if (g_sounds.size() == 1) {
            CHECK_EQ(g_sounds[0].thing, thing_index(t));
            CHECK_EQ(g_sounds[0].player, -1);
            CHECK_EQ(g_sounds[0].sound, 0x21 + (int)(mc_lcg(rng) & 3));
        }
        thing_free(t);
    }
    // into a creature: the collision is searched *before* the move, so the arrow first moves onto
    // the creature and deals its damage on the following tick, from the middle of the creature's box
    Thing *victim = find_isolated_creature();
    CHECK(victim != nullptr);
    if (!victim) return;
    DamageSlot saved = victim->damage_slots[0];
    victim->damage_slots[0] = DamageSlot{0, 0};
    Pos p = *thing_pos(victim);
    p.z = (int16_t)(p.z + victim->ext_z0 + 0x40);
    math_rotate_offset(&p, 0x600, 0, 0x180);
    Thing *t = launch(13, p.x, p.y, p.z, 0x200, 0, 0);
    if (!t) return;
    t->owner = 999;
    t->damage = 0x190;
    t->filter_cls = 5; t->filter_type = victim->type;
    run_handler(t);
    CHECK(!(t->flags & 0x400));
    CHECK_EQ(t->x, victim->x);
    CHECK_EQ(victim->damage_slots[0].amount, 0);
    run_handler(t);
    CHECK(t->flags & 0x400);
    CHECK_EQ(t->z, victim->z + victim->ext_z0);
    CHECK_EQ(victim->damage_slots[0].amount, 0x190);
    CHECK_EQ(victim->damage_slots[0].attacker, 999);
    victim->damage_slots[0] = saved;
    thing_free(t);
}

// The ground huggers: state 1 (stops at foreign mana) and state 18 (stops at any mana ball).
static void test_mana_seekers() {
    Thing *w0 = wizard(0);
    Pos land{};
    CHECK(find_cell(false, &land));
    // a mana ball three steps east of the launch point, on the ground
    Pos bp = land;
    math_rotate_offset(&bp, 0x200, 0, 3 * 0x180);
    bp.z = (int16_t)terrain_height_at(&bp);
    for (int ty : {1, 17}) {
        for (int own = 0; own < 2; own++) {
            auto before = pool_classes();
            Thing *ball = thing_create(&bp, 10, 0x27);
            CHECK(ball != nullptr);
            if (!ball) continue;
            ball->mana_owner = own ? w0->owner : 0;
            ball->flags |= 8;
            ball->timer_a = 0xfa;
            g_cfg->mana_ball_list = thing_index(ball);
            ball->next = 0;
            g_cfg->wizard_list = 0;
            Thing *t = launch(ty, land.x, land.y, land.z - 0x40, 0x1c0, 0, w0->owner, false);
            if (!t) continue;
            t->impact_type = (uint8_t)(ty == 1 ? 0xc : 0x36);
            // first tick: automatic aim. Type 1 only takes mana that is not its owner's; type 0x11
            // takes any ball.
            bool expect_aim = ty == 17 || !own;
            int ticks = 0;
            run_handler(t);
            ticks++;
            CHECK(t->flags & 2);
            CHECK_EQ(t->target, expect_aim ? thing_index(ball) : 0);
            if (expect_aim) {
                // the aim is taken from under the ground at launch (z - 0x40) to the middle of the ball
                CHECK_EQ(t->yaw, t->target_yaw);
                CHECK(angle_diff(t->yaw, 0x200) < 0x40);
            } else {
                CHECK_EQ(t->yaw, 0x1c0);          // unlike the flying kinds: no copy when nothing is found
                CHECK_EQ(t->target_yaw, 0);
            }
            CHECK(t->z >= terrain_height_at(thing_pos(t)));       // lifted onto the ground
            while (!(t->flags & 0x400) && ticks < 60) {
                run_handler(t);
                ticks++;
                if (!(t->flags & 0x400)) CHECK(t->z >= terrain_height_at(thing_pos(t)));
            }
            CHECK(t->flags & 0x400);
            auto made = created_since(before);
            int effects = 0, burst = 0;
            for (Thing *e : made) {
                if (e == t || e == ball) continue;
                CHECK_EQ(e->cls, 10);
                CHECK_EQ(e->owner, w0->owner);
                CHECK(pos_eq(*thing_pos(e), *thing_pos(t)));
                if (ty == 17 && e->type == 0xc) burst++; else { effects++; CHECK_EQ(e->type, ty == 1 ? 0xc : 0x36); }
            }
            CHECK_EQ(effects, 1);
            CHECK_EQ(burst, ty == 17 ? 1 : 0);
            if (expect_aim) {
                CHECK(ticks <= 6);
                CHECK_EQ(t->x, ball->x); CHECK_EQ(t->y, ball->y);
                CHECK_EQ(t->z, ball->z + ball->ext_z0);
            } else if (ticks != t->max_health + 1) {
                // it did not run its range out along the ground: then it stopped in the middle of
                // some other foreign mana (a ball or wizard castle of the level) lying in its way
                Thing *other = nullptr;
                for (int i = 1; i < MC_THING_SLOTS && !other; i++) {
                    Thing *o = thing_at(i);
                    if (o == ball || o->cls != 10 || (o->type != 0x27 && o->type != 0x28 && o->type != 0x2d)) continue;
                    if (o->mana_owner == w0->owner || !(o->flags & 8)) continue;
                    if (o->x == t->x && o->y == t->y && t->z == o->z + (o->type == 2 ? 0 : o->ext_z0)) other = o;
                }
                CHECK(other != nullptr);
                if (other) std::printf("   (stopped by effect #%d type %x, mana owner %d, lying in its way)\n",
                                       (int)thing_index(other), other->type, other->mana_owner);
            }
            std::printf("mana seeker type %d, ball owned by %s: ended after %d ticks %s\n", ty, own ? "the caster" : "nobody",
                        ticks, expect_aim ? "on the ball" : "without touching it");
            for (Thing *e : made) thing_free(e);
            g_cfg->mana_ball_list = 0;
        }
    }
    // state 19 (type 18) turns into its impact effect at once
    {
        auto before = pool_classes();
        Thing *t = launch(18, 0x4000, 0x4000, 0x3000, 0x123, 0x45, w0->owner, false);
        if (t) {
            t->impact_type = 0x37;
            t->damage = 55;
            run_handler(t);
            CHECK(t->flags & 0x400);
            CHECK(!(t->flags & 2));
            auto made = created_since(before);
            CHECK_EQ((int)made.size(), 2);
            for (Thing *e : made) {
                if (e != t) { CHECK_EQ(e->type, 0x37); CHECK_EQ(e->owner, w0->owner); CHECK_EQ(e->yaw, 0x123); CHECK_EQ(e->pitch, 0x45); CHECK_EQ(e->damage, 55); }
                thing_free(e);
            }
        }
    }
}

// The dead line-of-fire function: translated as it is, so test what it does.
static void test_line_of_fire() {
    Thing *w0 = wizard(0), *w1 = wizard(1);
    // out of sight range
    CHECK_EQ(projectile_line_of_fire_clear(w0, w1), 0);
    Thing a = *w1, b = *w1;
    a.z = b.z = 0x3000;
    a.yaw = 0x200; a.pitch = 0;
    b.x = (uint16_t)(a.x + 0x600);
    int16_t bz = b.z;
    int sight = (int16_t)mc_move_desc(a.desc)->sight_radius;
    CHECK(sight >= 0x600);
    // three 0x180 steps toward b; the probe leaves the shooter's own box (2 * 125 wide) on the
    // first step, nothing else is tested -> 0, and b.z is restored
    CHECK_EQ(projectile_line_of_fire_clear(&a, &b), 0);
    CHECK_EQ(b.z, bz);
    // a shooter wider than half a step "hits" itself on the first step -> 1
    a.ext_x = a.ext_y = 0x100;
    CHECK_EQ(projectile_line_of_fire_clear(&a, &b), 1);
    CHECK_EQ(b.z, bz);
    // outside the 0x100 yaw cone / the 0x38 pitch cone
    a.yaw = 0x200 + 0x101;
    CHECK_EQ(projectile_line_of_fire_clear(&a, &b), 0);
    a.yaw = 0x200; a.pitch = 0x39;
    CHECK_EQ(projectile_line_of_fire_clear(&a, &b), 0);
    a.pitch = 0x38;
    CHECK_EQ(projectile_line_of_fire_clear(&a, &b), 1);
    // terrain above the ray ends it with 0 before the self-hit can happen only when the first step
    // is already underground
    a.pitch = 0;
    a.ext_x = a.ext_y = 0x10;
    a.z = b.z = (int16_t)(terrain_height_at(thing_pos(&a)) - 0x400);
    bz = b.z;
    CHECK_EQ(projectile_line_of_fire_clear(&a, &b), 0);
    CHECK_EQ(b.z, bz);
}

// ---- 3. the arrows of the snapshot ---------------------------------------------------------------

static void test_snapshot_arrows() {
    Thing *castle = nullptr;
    std::vector<Thing *> arrows;
    for (int i = 1; i < MC_THING_SLOTS; i++) if (thing_at(i)->cls == 9) arrows.push_back(thing_at(i));
    CHECK_EQ((int)arrows.size(), 4);
    // what the retail constructor gives a type 13 projectile
    Pos p{0x4000, 0x4000, 0x3000};
    Thing *ref = thing_create(&p, 9, 13);
    CHECK(ref != nullptr);
    if (!ref) return;
    Thing ctor = *ref;
    thing_free(ref);
    // skeleton_attack_fire_19550 then sets the sprite to 0xcb (doubled extents) - compute what that gives
    Thing fired = ctor;
    thing_set_sprite_double(&fired, 0xcb);
    int exact_launch = 0;
    for (Thing *t : arrows) {
        int idx = thing_index(t);
        Thing *owner = thing_at(t->owner);
        Thing *target = thing_at(t->target);
        castle = target;
        std::printf("arrow #%d: owner #%d (class %d type %d state %d), target #%d (class %d type %d), health %d/%d, tick %d, yaw %03x pitch %03x\n",
                    idx, t->owner, owner->cls, owner->type, owner->state, t->target, target->cls, target->type,
                    t->health, t->max_health, t->tick, t->yaw, t->pitch);
        // constructor fields nobody changes afterwards
        CHECK_EQ(t->type, 13);
        CHECK_EQ(t->state, ctor.state);
        CHECK_EQ(t->max_health, ctor.max_health);          // 0x1400 / 0x180 = 13
        CHECK_EQ(t->speed_cur, ctor.speed_cur);
        CHECK_EQ(t->speed_base, ctor.speed_base);
        CHECK_EQ(t->mana, ctor.mana);
        CHECK_EQ(t->desc, ctor.desc);
        CHECK_EQ(t->impact_cls, ctor.impact_cls);
        CHECK_EQ(t->impact_type, ctor.impact_type);
        CHECK_EQ(t->prop_flags, ctor.prop_flags);
        // set by the skeleton's attack: sprite 0xcb doubled, damage 400, the skeleton's target and filter
        CHECK_EQ(t->sprite, fired.sprite);
        CHECK_EQ(t->ext_z0, fired.ext_z0);
        CHECK_EQ(t->ext_x, fired.ext_x);
        CHECK_EQ(t->ext_y, fired.ext_y);
        CHECK_EQ(t->ext_h, fired.ext_h);
        CHECK_EQ(t->damage, 0x190);
        CHECK_EQ(owner->cls, 5);
        CHECK_EQ(t->filter_cls, owner->filter_cls);
        CHECK_EQ(t->filter_type, owner->filter_type);
        CHECK_EQ(t->target, owner->target);
        // what 45b60 does to it: flag 2 on the first tick, nothing to the target angles, lifetime
        // minus one per update. Thing.tick starts at the low byte of the index and counts updates.
        CHECK_EQ(t->flags, (ctor.flags | 2));
        CHECK_EQ(t->target_yaw, 0);
        CHECK_EQ(t->target_pitch, 0);
        int updates = (uint8_t)(t->tick - (uint8_t)idx);
        CHECK(updates >= 1 && updates <= 13);
        CHECK_EQ(t->health, t->max_health - updates);
        // the flight: launched at the skeleton's position raised by its ext_h, aimed from its base
        // position at the target's, then `updates` straight steps of 0x180. Exact when the skeleton
        // has not moved since.
        Pos launch_pos = *thing_pos(owner);
        int yaw = pos_angle_to(&launch_pos, thing_pos(target)) & 0xffff;
        int pitch = pos_pitch_to(&launch_pos, thing_pos(target)) & 0xffff;
        launch_pos.z = (int16_t)(launch_pos.z + owner->ext_h);
        Pos q = launch_pos;
        for (int k = 0; k < updates; k++) math_rotate_offset(&q, t->yaw, t->pitch, 0x180);
        bool exact = pos_eq(q, *thing_pos(t)) && yaw == t->yaw && pitch == t->pitch;
        std::printf("   %d update(s); from the owner's present position: yaw %03x pitch %03x, predicted %04x %04x %d, actual %04x %04x %d%s\n",
                    updates, yaw, pitch, (unsigned)q.x, (unsigned)q.y, q.z, (unsigned)t->x, (unsigned)t->y, t->z, exact ? "  (exact)" : "");
        if (exact) exact_launch++;
        // in any case the arrow is `updates` steps from somewhere close to its owner
        Pos back = *thing_pos(t);
        for (int k = 0; k < updates; k++) math_rotate_offset(&back, t->yaw + 0x400, (0x800 - t->pitch) & 0x7ff, 0x180);
        CHECK(pos_dist_xyz(&back, &launch_pos) < 0x200);
    }
    std::printf("snapshot arrows reproduced exactly from their owners: %d / 4\n", exact_launch);
    CHECK(exact_launch >= 1);
    if (!castle) return;

    // Step them to their end with the real update loop. Each flies straight until the ground is
    // above the next position or its lifetime is used up, then deals 400 to whatever its box
    // touches - the castle through the player list.
    CHECK(castle->cls == 3 && castle->type == 2);
    int32_t dmg0 = castle->damage_slots[0].amount;
    struct Track { Thing *t; Pos pos; int32_t health; bool done; int end_tick; bool touches; };
    std::vector<Track> tr;
    for (Thing *t : arrows) tr.push_back({t, *thing_pos(t), t->health, false, 0, false});
    int expected_dmg = 0;
    for (int tick = 1; tick <= 20; tick++) {
        thing_update_all();
        for (Track &k : tr) {
            if (k.done) continue;
            Thing *t = k.t;
            if (t->flags & 0x400) {
                k.done = true;
                k.end_tick = tick;
                // Either it ended where it was (ground ahead or lifetime used up; the castle's box
                // is then found by the area damage through the player list), or the collision
                // search around it (filter: castles only) found the castle's own cell and it was
                // moved onto the castle's position first.
                Pos next = k.pos;
                math_rotate_offset(&next, t->yaw, t->pitch, 0x180);
                bool ground = (int16_t)terrain_height_at(&next) > next.z;
                bool on_castle = pos_eq(*thing_pos(t), *thing_pos(castle));
                if (!on_castle) {
                    CHECK(pos_eq(*thing_pos(t), k.pos));
                    CHECK(ground || k.health == 0);
                }
                k.touches = thing_collide(t, castle) != 0;
                if (k.touches) expected_dmg += 0x190;
                std::printf("arrow #%d ended at tick %d (%s), %s the castle box\n", (int)thing_index(t), tick,
                            on_castle ? "castle found by the collision search" : ground ? "ground" : "lifetime",
                            k.touches ? "inside" : "outside");
            } else {
                Pos next = k.pos;
                math_rotate_offset(&next, t->yaw, t->pitch, 0x180);
                CHECK(pos_eq(*thing_pos(t), next));
                CHECK_EQ(t->health, k.health - 1);
                k.pos = next;
                k.health = t->health;
            }
        }
    }
    for (const Track &k : tr) CHECK(k.done);
    CHECK_EQ(castle->damage_slots[0].amount - dmg0, expected_dmg);
    std::printf("castle #%d pending damage: %d -> %d (expected +%d)\n", (int)thing_index(castle), dmg0,
                castle->damage_slots[0].amount, expected_dmg);
    CHECK(expected_dmg > 0);
    int left = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) left += thing_at(i)->cls == 9;
    CHECK_EQ(left, 0);
    check_pool_consistent("snapshot arrows");
}

// What the original stored into an impact effect's Thing.target when nothing was hit, in the run that
// produced the snapshot: (0 - &things[0]) / 0xa4, low 16 bits. Informational (the snapshot holds no
// effect that shows it); see g_projectile_null_hit_index.
static void report_null_hit_index(const char *game_dir) {
    char path[1024];
    std::snprintf(path, sizeof path, "%s/movie/gam00000.dat", game_dir);
    std::FILE *f = std::fopen(path, "rb");
    if (!f) return;
    std::vector<uint8_t> raw(MC_STATE_IMAGE_SIZE);
    size_t got = std::fread(raw.data(), 1, raw.size(), f);
    std::fclose(f);
    if (got != raw.size()) return;
    const GameStateImage *s = reinterpret_cast<const GameStateImage *>(raw.data());
    // the largest pointer on the free stack belongs to the highest free slot (thing_relink_snapshot)
    uint32_t max_ptr = 0;
    for (int i = 0; i <= s->free_top && i < MC_THING_SLOTS; i++)
        if ((uint32_t)s->free_list[i] > max_ptr) max_ptr = (uint32_t)s->free_list[i];
    int max_idx = 0;
    for (int i = MC_THING_SLOTS - 1; i >= 1; i--) if (s->things[i].cls == 0) { max_idx = i; break; }
    if (!max_idx || !max_ptr) return;
    uint32_t things_base = max_ptr - (uint32_t)max_idx * (uint32_t)sizeof(ThingImage);
    int32_t q = (int32_t)(0u - things_base) / (int32_t)sizeof(ThingImage);    // sar / idiv as in 0x444d1
    std::printf("null-hit index: the snapshot's Thing pool was at 0x%08x, so a projectile that hit nothing stored "
                "target = 0x%04x (%u) in its impact effect; the port stores %u\n",
                things_base, (unsigned)(uint16_t)q, (unsigned)(uint16_t)q, (unsigned)g_projectile_null_hit_index);
}

// ---- 4. automatic aim against a brute-force search ------------------------------------------------

// One sweep of launches (every wizard, 32 headings, projectile types 0 / 7 / 9 / 3 / 12) compared
// with a brute-force search over the whole pool that applies the list membership rules of
// thing_update_all and the per-type rules of 45f00.
static void pick_target_sweep(const char *what) {
    static const int types[5] = {0, 7, 9, 3, 12};
    int found_n[5] = {0, 0, 0, 0, 0}, tried = 0, mismatch = 0, music = 0, castles = 0, creatures = 0, players = 0;
    Thing *human = wizard(0);
    for (int pl = 0; pl < 4; pl++) {
        Thing *w = wizard(pl);
        if (w->cls != 3) continue;
        for (int yaw = 0; yaw < 0x800; yaw += 0x40) {
            for (int k = 0; k < 5; k++) {
                int ty = types[k];
                bool players_only = ty == 7 || ty == 12;
                Thing *t = launch(ty, w->x, w->y, w->z + w->ext_h, yaw, 0, w->owner, false);
                if (!t) continue;
                t->aux = 0x40;
                tried++;
                uint32_t best = 0xffffffffu;
                int best_idx = 0;
                uint32_t range = ty == 9 ? (uint32_t)(t->max_health * t->speed_base)
                                         : (uint32_t)(int32_t)(int16_t)mc_move_desc(w->desc)->sight_radius;
                for (int i = 1; i < MC_THING_SLOTS; i++) {
                    Thing *o = thing_at(i);
                    if (o->cls != 3 || o->health < 0 || (o->flags & 0x10)) continue;
                    if (o->owner == t->owner || (o->flags & 0x20)) continue;
                    if ((uint32_t)pos_dist_xyz(thing_pos(o), thing_pos(t)) > range) continue;
                    uint32_t s = (o->type == 2 && !players_only) ? target_aim_score(t, o, 0x71, 0x71) : projectile_target_score(t, o, 0x71, 0x71);
                    if (s < best) { best = s; best_idx = i; }
                }
                if (!players_only) {
                    for (int list = 0; list < 20; list++)
                        for (int i = 1; i < MC_THING_SLOTS; i++) {
                            Thing *o = thing_at(i);
                            if (o->cls != 5 || o->type != list || o->health < 0 || o->state == 0x78) continue;
                            if (o->owner == t->owner || o->timer_a == 0) continue;
                            uint32_t s = projectile_target_score(t, o, 0x71, ty == 9 ? 0x200 : 0x71);
                            if (s < best) { best = s; best_idx = i; }
                        }
                }
                int16_t music_before = player_block(human)->combat_music;
                player_block(human)->combat_music = 0;
                int r = projectile_pick_target(t);
                CHECK_EQ(t->aux, 0x10);                               // clamped
                if (r != (best_idx != 0) || t->target != best_idx) mismatch++;
                if (r) {
                    found_n[k]++;
                    Thing *o = thing_at(t->target);
                    if (o->cls == 5) creatures++; else if (o->type == 2) castles++; else players++;
                    // target angles = direction to the middle of the target
                    Thing probe = *t;
                    thing_aim_at(&probe, o);
                    CHECK_EQ(t->target_yaw, probe.target_yaw);
                    CHECK_EQ(t->target_pitch, probe.target_pitch);
                    CHECK(angle_diff(yaw, t->target_yaw) <= 0x71);
                    CHECK_EQ(t->yaw, yaw);                            // the caller decides about yaw / pitch
                    // locking onto the human flyer keeps the combat music going (not for lightning)
                    bool is_human = o == human;
                    CHECK_EQ(player_block(human)->combat_music, (is_human && ty != 9) ? 100 : 0);
                    if (is_human && ty != 9) music++;
                } else {
                    CHECK_EQ(t->target, 0);
                    CHECK_EQ(player_block(human)->combat_music, 0);
                }
                player_block(human)->combat_music = music_before;
                thing_free(t);
            }
        }
    }
    std::printf("automatic aim, %s: %d launches (4 wizards x 32 headings), targets found by type 0 / 7 / 9 / 3 / 12: "
                "%d / %d / %d / %d / %d (%d creatures, %d castles, %d players or balloons), %d mismatch(es) against brute force, "
                "%d lock(s) on the human player\n",
                what, tried, found_n[0], found_n[1], found_n[2], found_n[3], found_n[4], creatures, castles, players, mismatch, music);
    CHECK_EQ(mismatch, 0);
}

static void test_pick_target() {
    thing_update_all();                 // builds the per-class lists the search walks
    int awake = 0, total = 0;
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        Thing *o = thing_at(i);
        if (o->cls == 5) { total++; awake += o->timer_a != 0; }
    }
    std::printf("automatic aim: %d of %d creatures of the snapshot are awake (timer_a != 0)\n", awake, total);
    pick_target_sweep("snapshot as it is");
    // the same with every creature awake, and the wizards moved next to each other's castles so
    // that players and castles are in range as well
    std::vector<uint8_t> saved_timer(MC_THING_SLOTS);
    for (int i = 1; i < MC_THING_SLOTS; i++) {
        Thing *o = thing_at(i);
        saved_timer[i] = o->timer_a;
        if (o->cls == 5) o->timer_a = 1;
    }
    pick_target_sweep("every creature awake");
    {
        std::vector<Thing *> castles;
        for (int i = 1; i < MC_THING_SLOTS; i++) if (thing_at(i)->cls == 3 && thing_at(i)->type == 2) castles.push_back(thing_at(i));
        Pos saved_pos[4];
        for (int pl = 0; pl < 4; pl++) {
            Thing *w = wizard(pl);
            saved_pos[pl] = *thing_pos(w);
            if (castles.empty()) continue;
            Thing *c = castles[(size_t)pl % castles.size()];
            Pos p = *thing_pos(c);
            math_rotate_offset(&p, pl * 0x200, 0, 0xc00);
            p.z = (int16_t)(terrain_height_at(&p) + 0x200);
            thing_move_to(w, &p);
        }
        pick_target_sweep("wizards at the castles");
        // ... and all four wizards in a ring of radius 0x500 at one height: players in range of
        // each other (the human flyer among them: combat music)
        Pos centre = saved_pos[0];
        centre.z = (int16_t)(terrain_height_at(&centre) + 0x600);
        for (int pl = 0; pl < 4; pl++) {
            Pos p = centre;
            math_rotate_offset(&p, pl * 0x200, 0, 0x500);
            thing_move_to(wizard(pl), &p);
        }
        pick_target_sweep("wizards in a ring");
        for (int pl = 0; pl < 4; pl++) thing_move_to(wizard(pl), &saved_pos[pl]);
    }
    for (int i = 1; i < MC_THING_SLOTS; i++) thing_at(i)->timer_a = saved_timer[i];
    // types without automatic aim
    for (int ty : {2, 5, 6, 10, 13, 14, 15}) {
        Thing *w = wizard(1);
        Thing *t = launch(ty, w->x, w->y, w->z + w->ext_h, w->yaw, 0, w->owner, false);
        if (!t) continue;
        CHECK_EQ(projectile_pick_target(t), 0);
        CHECK_EQ(t->target, 0);
        thing_free(t);
    }
    // a sleeping creature (timer_a == 0) is never chosen; the same creature awake is
    {
        Thing *victim = find_isolated_creature();
        CHECK(victim != nullptr);
        if (victim) {
            auto before = pool_classes();
            uint8_t saved = victim->timer_a;
            uint32_t saved_players = g_cfg->player_list;
            g_cfg->player_list = 0;
            Pos p = *thing_pos(victim);
            p.z = (int16_t)(p.z + victim->ext_z0);
            math_rotate_offset(&p, 0x400, 0, 0x100);         // 0x100 south of it, looking north
            Thing *t = launch(0, p.x, p.y, p.z, 0, 0, 999, false);
            if (t) {
                victim->timer_a = 1;
                CHECK_EQ(projectile_pick_target(t), 1);
                CHECK_EQ(t->target, thing_index(victim));    // nothing scores better than 0x100 dead ahead
                CHECK_EQ(t->target_yaw, 0);
                t->target = 0;
                victim->timer_a = 0;
                int r = projectile_pick_target(t);
                CHECK(r == 0 || t->target != thing_index(victim));
                // the fireball's first tick turns at most 0x22 toward the target and snaps the pitch
                victim->timer_a = 1;
                t->target = 0;
                t->yaw = 0x60; t->pitch = 0x33;
                t->health = 50;
                Thing copy = *t;
                CHECK_EQ(projectile_pick_target(&copy), 1);
                run_handler(t);
                CHECK_EQ(t->target, thing_index(victim));
                CHECK_EQ(t->yaw, 0x60 - 0x22);
                CHECK_EQ(t->pitch, copy.target_pitch);
                // from then on it steers by its descriptor
                const MoveDesc *d = mc_move_desc(t->desc);
                uint16_t y1 = t->yaw;
                if (!(t->flags & 0x400)) {
                    run_handler(t);
                    CHECK(angle_diff(y1, t->yaw) <= d->turn_min);
                }
            }
            for (Thing *e : created_since(before)) thing_free(e);
            victim->timer_a = saved;
            g_cfg->player_list = saved_players;
        }
    }
}

// ---- 5. smoke runs -------------------------------------------------------------------------------

// The projectile type -> impact effect pairs the spell handlers use (47130 .. 492e0), the castle seed
// excluded (covered above; it would found castles all over the map).
struct Shot { int type, impact_cls, impact_type; };
static const Shot k_shots[] = {
    {0, 10, 0}, {1, 10, 0xc}, {2, 10, 0xf}, {3, 10, 0x11}, {4, 10, 9}, {5, 10, 0xb}, {7, 10, 0x1a}, {8, 10, 0x19},
    {9, 10, 0x17}, {11, 10, 0x24}, {12, 9, 9}, {17, 10, 0x36}, {16, 10, 0x35}, {18, 10, 0x37}, {13, 10, 0}, {14, 10, 0},
};

// What spell_fireball_update_47130 does around thing_create (without spell_projectile_origin's hand
// offset and the mana bookkeeping).
static Thing *spell_style_launch(Thing *caster, const Shot &s) {
    Thing *t = thing_create(thing_pos(caster), 9, s.type);
    if (!t) return nullptr;
    t->speed_cur = (int16_t)(t->speed_cur + caster->speed_cur);
    t->impact_cls = (uint8_t)s.impact_cls;
    t->impact_type = (uint8_t)s.impact_type;
    t->owner = caster->owner;
    t->damage = 0x7d;
    Pos p = *thing_pos(t);
    p.z = (int16_t)(p.z + caster->ext_h);
    thing_move_to(t, &p);
    t->yaw = caster->yaw;
    t->pitch = caster->pitch;
    PlayerBlock *P = player_block(caster);
    t->aux = P->aim_charge;
    t->home = *thing_pos(caster);
    math_rotate_offset(&t->home, caster->yaw, caster->pitch, 0x4000);
    return t;
}

struct SmokeStats {
    int launched[20] = {}, ended[20] = {}, life_sum[20] = {}, targeted[20] = {};
    int effects_by_type[0x40] = {}, effects_other = 0, segments = 0, alloc_fail = 0;
    int max_projectiles = 0, max_live = 0, off_ground = 0, reaped = 0;
};

// Runs `ticks` ticks of game_tick_sim(), launching one projectile per wizard every `period` ticks.
// Effects and whatever else has no ported update handler in this test build would pile up, so
// class-10 Things created during the run are removed 12 ticks after they appear.
static void smoke_run(const char *what, int ticks, int period, bool movie) {
    SmokeStats st;
    std::vector<int> born(MC_THING_SLOTS, -1), age10(MC_THING_SLOTS, -1), ptype(MC_THING_SLOTS, -1);
    std::vector<int> orig10(MC_THING_SLOTS, -1), seen10(MC_THING_SLOTS, -1);
    std::vector<uint8_t> seg_seen(MC_THING_SLOTS);
    auto note_originals = [&]() {
        for (int i = 1; i < MC_THING_SLOTS; i++) orig10[i] = thing_at(i)->cls == 10 ? thing_at(i)->type : -1;
    };
    note_originals();
    thing_dispatch_reset_stats();
    const int n_shots = (int)(sizeof k_shots / sizeof k_shots[0]);
    for (int tick = 0; tick < ticks; tick++) {
        if (movie) { if (!demo_step()) { std::printf("%s: recording ended at tick %d\n", what, tick); break; } }
        else game_tick_sim();
        if (movie && tick == 0) note_originals();        // the first step replaced the state by the snapshot
        // bookkeeping over the pool
        int nproj = 0, nlive = 0;
        for (int i = 1; i < MC_THING_SLOTS; i++) {
            Thing *t = thing_at(i);
            bool live = t->cls != 0;
            if (live) nlive++;
            if (live && t->cls == 9 && t->state != 0xe) nproj++;
            // a tracked projectile whose slot is free again or re-used has ended
            if (ptype[i] >= 0 && (!live || t->cls != 9 || t->type != ptype[i] || (t->flags & 0x400))) {
                st.ended[ptype[i]]++;
                st.life_sum[ptype[i]] += tick - born[i];
                ptype[i] = -1;
            }
            if (live && t->cls == 9 && t->state != 0xe && ptype[i] >= 0) {
                if (t->target != 0 && born[i] == tick - 1) st.targeted[t->type]++;
                // nothing leaves the map vertically in a way the original could not: z stays in range
                if (t->z < -0x400) st.off_ground++;
            }
            // Effects: a slot is "original" while it still holds the class-10 type it held at the
            // start (a freed slot is usually re-used within the same tick, so "was free" cannot
            // be observed); anything else of class 10 was created during the run.
            if (live && t->cls == 10 && orig10[i] != t->type) {
                orig10[i] = -1;
                if (seen10[i] != t->type) {
                    seen10[i] = t->type;
                    age10[i] = 0;
                    if (t->type < 0x40) st.effects_by_type[t->type]++; else st.effects_other++;
                } else if (++age10[i] > 12 && thing_update_fn(10, t->state) == nullptr && !(t->flags & 0x400)) {
                    thing_mark_delete(t);
                    st.reaped++;
                }
            } else if (!live || t->cls != 10) {
                orig10[i] = -1;
                seen10[i] = -1;
            }
            bool is_seg = live && t->cls == 9 && t->state == 0xe;
            if (is_seg && !seg_seen[i]) st.segments++;
            seg_seen[i] = is_seg;
        }
        if (nproj > st.max_projectiles) st.max_projectiles = nproj;
        if (nlive > st.max_live) st.max_live = nlive;
        if (tick % period == 0) {
            for (int pl = 0; pl < g_state->player_count && pl < 8; pl++) {
                Thing *w = wizard(pl);
                if (w->cls != 3 || w->health < 0 || (w->flags & 0x20) || w->state > 1) continue;
                const Shot &s = k_shots[(tick / period + pl * 5) % n_shots];   // every wizard goes through every type
                Thing *t = spell_style_launch(w, s);
                if (!t) { st.alloc_fail++; continue; }
                int i = thing_index(t);
                st.launched[s.type]++;
                ptype[i] = s.type;
                born[i] = tick;
            }
        }
        if ((tick + 1) % 500 == 0) {
            int cls_n[13] = {};
            for (int i = 1; i < MC_THING_SLOTS; i++) if (thing_at(i)->cls < 13) cls_n[thing_at(i)->cls]++;
            std::printf("%s tick %4d: live %d (creatures %d, projectiles %d, effects %d, spells %d), free %d\n", what, tick + 1,
                        MC_THING_SLOTS - 1 - cls_n[0], cls_n[5], cls_n[9], cls_n[10], cls_n[12], thing_free_count());
            check_pool_consistent(what);
        }
    }
    std::printf("%s: projectiles by type (launched / ended / mean lifetime in ticks / locked on a target at launch):\n", what);
    int total = 0, total_ended = 0;
    for (int ty = 0; ty < 20; ty++) {
        if (!st.launched[ty]) continue;
        total += st.launched[ty];
        total_ended += st.ended[ty];
        std::printf("   type %2d: %4d / %4d / %5.1f / %d\n", ty, st.launched[ty], st.ended[ty],
                    st.ended[ty] ? (double)st.life_sum[ty] / st.ended[ty] : 0.0, st.targeted[ty]);
        // every projectile ends: its range is at most 0x50 ticks, rebounds aside
        CHECK(st.launched[ty] - st.ended[ty] <= 8);
    }
    std::printf("%s: effects created by type:", what);
    for (int ty = 0; ty < 0x40; ty++) if (st.effects_by_type[ty]) std::printf(" %x:%d", ty, st.effects_by_type[ty]);
    std::printf("\n%s: %d launched, %d ended, %d lightning segments, most projectiles at once %d, most Things %d, "
                "%d failed allocations, %d effect(s) removed by the test, %d below -0x400\n",
                what, total, total_ended, st.segments, st.max_projectiles, st.max_live, st.alloc_fail, st.reaped, st.off_ground);
    CHECK(total > 0);
    CHECK_EQ(st.off_ground, 0);
    check_pool_consistent(what);
    // no class-9 handler may be missing from the dispatch
    std::printf("%s: handlers dispatched without a port (other subsystems are not linked into this test):\n", what);
    thing_dispatch_report(stdout);
    for (int s = 0; s <= 20; s++) CHECK(thing_update_fn(9, s) != nullptr);
}

int main(int argc, char **argv) {
    mc_install_crash_handler();
    const char *game_dir = argc > 1 ? argv[1] : MC_DEFAULT_GAME_DIR;
    if (!sim_init(game_dir)) { std::printf("sim_init failed\n"); return 2; }
    projectiles_register_handlers();
    g_hook_sound_request = record_sound;

    if (!load_snapshot()) { std::printf("snapshot failed to load\n"); return 2; }
    int live0 = live_count();
    test_registration();
    test_scores();
    test_steering();
    test_hit_stats();
    test_straight_flight();
    test_acceleration();
    test_ground_and_water();
    test_collision_and_rebound();
    test_lightning();
    test_castle_seed();
    test_arrow();
    test_mana_seekers();
    test_line_of_fire();
    CHECK_EQ(live_count(), live0);              // every test cleaned up after itself
    check_pool_consistent("constructed cases");

    if (!load_snapshot()) return 2;
    report_null_hit_index(game_dir);
    test_snapshot_arrows();
    if (!load_snapshot()) return 2;
    test_pick_target();

    // smoke: the movie from the snapshot (players fly the recorded inputs) ...
    if (!load_snapshot()) return 2;
    if (demo_open(game_dir, 0)) {
        smoke_run("movie", 2500, 3, true);
        demo_close();
    } else {
        std::printf("FAIL: movie/mvi00000.dat missing\n");
        g_fail++;
    }
    // ... and freshly generated levels
    for (int level : {38, 0, 12}) {
        g_cfg->flags = 0;
        if (!sim_load_level(level)) { std::printf("level %d failed to load\n", level); g_fail++; continue; }
        g_cfg->paused = 0;
        g_cfg->substeps = 0;
        char name[32];
        std::snprintf(name, sizeof name, "level %d", level);
        smoke_run(name, level == 38 ? 2000 : 600, 5, false);
    }

    std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
