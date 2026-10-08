// Creatures (class 5), part 2: crab, kraken, troll, griffon (0x1a830..0x1bb00). Translated from
// the disassembly of carpet.exe; the shared bodies they call are in creature_common.cpp, the state
// layout (base = type * 6) is described in creatures.h, the field use of these four types in
// creatures2.h.
#include "creatures2.h"
#include "constructors.h"     // crab_update_mana_sprite_36ac0
#include "mc_math.h"
#include "world_set.h"

namespace {

constexpr uint32_t desc_index(uint32_t addr) { return (addr - 0x96a10u) / 0x20u; }   // Thing+0x9c values

inline const MoveDesc *desc_of(const Thing *t) { return mc_move_desc(t->desc); }

inline PlayerBlock *owner_block(const Thing *t) {     // Thing+0xa0 (the dummy block for 0)
    return reinterpret_cast<PlayerBlock *>(thing_player_block(t));
}

// tick % desc.think_period == 0 (xor edx,edx; mov dl,[t+0x3f]; movsx,[desc+0x1a]; idiv). The original
// faults on a zero period; no descriptor of these types has one.
inline bool think_now(const Thing *t) {
    int period = (int16_t)desc_of(t)->think_period;
    return period != 0 && (int)t->tick % period == 0;
}

// (dx^2 + dy^2) of the torus-wrapped coordinate differences, unsigned (the original's uint32 sum of the
// 16-bit wrapped differences, at most 2^31: the same value with 256 cells; 64-bit for a wider world).
inline uint64_t dist_sq_xy16(const Thing *a, const Thing *b) {
    int64_t dx = world_signed(a->x - b->x);
    int64_t dy = world_signed(a->y - b->y);
    return (uint64_t)(dx * dx) + (uint64_t)(dy * dy);
}

inline uint64_t sight_sq(const Thing *t) {
    int32_t s = (int16_t)desc_of(t)->sight_radius;
    return (uint32_t)(s * s);
}

// The original's 0xffffffff "none yet" (unreachable with 256 cells); 64-bit for a wider world.
constexpr uint64_t kFarSq = ~(uint64_t)0;

// angle_diff(t.yaw, pos_angle_to(t, o) & 0xffff) & 0xffff < (int16)desc.fov
inline bool in_fov(const Thing *t, const Thing *o) {
    int fov = (int16_t)desc_of(t)->fov;
    int ang = pos_angle_to(thing_pos(t), thing_pos(o)) & 0xffff;
    return (angle_diff(t->yaw, ang) & 0xffff) < fov;
}

// sound_request_49720((int16)thing index, -1, id)
inline void creature_sound(const Thing *t, int id) { sound_request((int16_t)thing_index(t), -1, id); }

inline bool is_mana_ball(const Thing *o) { return o->cls == 10 && o->type == 0x27; }

// The enemy search of creature_ai_step as the crab (0x1a9b9) and the griffon (0x1b748) inline it:
// nearest player-list thing inside the sight radius and the field of view. The crab skips things
// with flag 0x20 (0x1a9df); the griffon's copy has no such test.
Thing *nearest_player_thing(const Thing *t, bool skip_flag20) {
    uint64_t limit = sight_sq(t);
    uint64_t best_d = kFarSq;
    Thing *best = nullptr;
    for (uint32_t i = g_cfg->player_list; i != 0; i = thing_at(i)->next) {
        Thing *p = thing_at(i);
        uint64_t d = dist_sq_xy16(p, t);
        if (d > limit) continue;
        if (skip_flag20 && (p->flags & 0x20)) continue;
        if (!in_fov(t, p)) continue;
        if (d >= best_d) continue;
        best = p;
        best_d = d;
    }
    return best;
}

// The flock-leader search as the griffon inlines it (0x1b871): nearest leaderless creature of the
// same type inside the sight radius and the field of view.
Thing *nearest_leader(const Thing *t) {
    uint64_t limit = sight_sq(t);
    uint64_t best_d = kFarSq;
    Thing *best = nullptr;
    for (uint32_t i = g_cfg->creature_lists[(int8_t)t->type]; i != 0; i = thing_at(i)->next) {
        Thing *o = thing_at(i);
        if (o->parent != 0 || o == t) continue;
        uint64_t d = dist_sq_xy16(o, t);
        if (d > limit) continue;
        if (!in_fov(t, o)) continue;
        if (d >= best_d) continue;
        best = o;
        best_d = d;
    }
    return best;
}

// The villagers' "who did that" (0x1b7d7, 0x1b9d7, 0x1baa4): a thing of type 0 or 1 (a wizard; the
// class is not tested) has its player block marked wanted, P+0x210 = 200.
inline bool is_wizard_type(const Thing *who) { return who->type == 0 || who->type == 1; }
void blame_wizard(uint16_t thing) {
    const Thing *who = thing_at(thing);
    if (is_wizard_type(who)) owner_block(who)->timer210 = 200;
}

// The tail of crab states 31 / 32 (0x1abfc, 0x1ac64): health regenerates by max_health >> 7 per tick.
void crab_regenerate(Thing *t) {
    if (t->health < t->max_health) t->health += t->max_health >> 7;
}

// ---- crab (type 5, base 0x1e) --------------------------------------------------------------------

// creature_crab_s31_1a830 (state 31 = base + 1): the crab's main state. Damage intake (dead -> 34, hit
// by a class-3 thing -> target it, 32); move; every think tick: the nearest player-list thing in
// sight / fov becomes the target (32) - awake or not; otherwise walk to the chosen mana ball (within
// speed_base * 128 -> 33 with aux = 15), or pick the nearest mana ball of the list and, when the
// carried mana exceeds mana_total + 500, lay a crab egg (class 10 type 0x34, aux = 100 + 10 * (rng %
// 10)) for 500 mana. Health regenerates every tick.
void creature_crab_s31_update(Thing *t) {
    const int base = 0x1e;
    switch (creature_apply_damage(t)) {
    case 2:
        thing_set_state(t, base + 4);
        break;
    case 1:
        if (thing_at(t->last_attacker)->cls == 3) {
            t->target = t->last_attacker;
            thing_set_state(t, base + 2);
        }
        break;
    default: {
        creature_move_step(t);
        if (!think_now(t)) break;
        Thing *enemy = nearest_player_thing(t, true);
        if (enemy) {
            t->target = thing_index(enemy);
            thing_set_state(t, base + 2);
            break;
        }
        if (t->target != 0) {
            Thing *ball = thing_at(t->target);
            if (!is_mana_ball(ball)) {
                t->target = 0;
                break;
            }
            int reach = (int16_t)t->speed_base << 7;
            if ((uint32_t)pos_dist_xy(thing_pos(t), thing_pos(ball)) <= (uint32_t)reach) {
                t->aux = 0xf;
                thing_set_state(t, base + 3);
            } else {
                t->target_yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(ball));
            }
            break;
        }
        // (tick % think_period) * 2 != 0 -> done: never, the think test above already passed
        uint64_t best_d = kFarSq;
        Thing *best = nullptr;
        for (uint32_t i = g_cfg->mana_ball_list; i != 0; i = thing_at(i)->next) {
            Thing *o = thing_at(i);
            if (o->type != 0x27) continue;
            uint64_t d = dist_sq_xy16(o, t);
            if (d >= best_d) continue;
            best = o;
            best_d = d;
        }
        if (best) t->target = thing_index(best);
        if (t->mana_total + 0x1f4 < t->mana) {
            Thing *egg = thing_create(thing_pos(t), 10, 0x34);
            if (egg) {
                t->rng = mc_lcg(t->rng);
                egg->aux = (int16_t)((t->rng % 10u) * 10 + 100);
                t->mana -= 0x1f4;
            }
        }
        break;
    }
    }
    crab_regenerate(t);
}

// creature_crab_s32_update_1ac20 (state 32 = base + 2)
void creature_crab_s32_update(Thing *t) {
    if (creature_attack_target(t, 0x1e, creature_attack_volley)) creature_sound(t, 0x20);
    crab_regenerate(t);
}

// creature_crab_s33_collect_mana_1ac80 (state 33 = base + 3): walk to the target mana ball; every aux
// ticks: a vanished ball -> 31; within speed_base * 5 eat it (mana added, ball deleted, sprite /
// max health by crab_update_mana_sprite) -> 31; within speed_base * 20 think every 3 ticks.
void creature_crab_s33_update(Thing *t) {
    const int base = 0x1e;
    switch (creature_apply_damage(t)) {
    case 2:
        thing_set_state(t, base + 4);
        return;
    case 1:
        if (thing_at(t->last_attacker)->cls == 3) {
            t->target = t->last_attacker;
            thing_set_state(t, base + 2);
        }
        return;
    default:
        break;
    }
    creature_move_step(t);
    int period = (int16_t)t->aux;                       // tick % aux (the original faults on 0)
    if (period == 0 || (int)t->tick % period != 0) return;
    Thing *ball = thing_at(t->target);
    if (!is_mana_ball(ball)) {
        t->target = 0;
        thing_set_state(t, base + 1);
        return;
    }
    uint32_t d = (uint32_t)pos_dist_xy(thing_pos(t), thing_pos(ball));
    int speed = (int16_t)t->speed_base;
    if (d <= (uint32_t)(speed * 5)) {
        t->target = 0;
        t->mana += ball->mana;
        ball->mana_owner = 0;
        thing_mark_delete(ball);
        thing_set_state(t, base + 1);
        crab_update_mana_sprite(t);
        return;
    }
    if (d <= (uint32_t)(speed * 20)) t->aux = 3;
    t->target_yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(ball));
}

void creature_crab_s34_update(Thing *t) { creature_die(t, 0x1e); }            // creature_crab_s34_update_1aed0
void creature_crab_s35_update(Thing *t) { creature_dead_drop_mana(t); }      // creature_crab_s35_update_1aee0

// ---- kraken (type 6, base 0x24) ------------------------------------------------------------------

void creature_kraken_s36_update(Thing *t) { creature_idle_seek_leader(t, 0x24); }   // creature_crab_s36_update_1afa0

// creature_kraken_s37_update_1afb0
void creature_kraken_s37_update(Thing *t) {
    creature_ai_step(t, 0x24);
    if (t->state == 0x26) creature_sound(t, 0x25);
}

// creature_kraken_s38_update_1b000 (state 38 = base + 2): damage intake (dead -> 40; hit by a class-3
// thing -> retarget with aux = -10, no move); move; aim every 4th tick; target dead / deleted -> 37.
// aux counts up: while positive the target's player is dragged toward the kraken (knock-back speed
// 0x50, pitch 0x100) with sound 0x2a; above 40 it restarts at -90. Every think tick: target farther
// than the sight radius -> 37, else sound 0x25 and a burst of 5 shots (castle_size), one per tick.
void creature_kraken_s38_update(Thing *t) {
    const int base = 0x24;
    if (engine1995()) t->speed_cur = 0x1e;                         // 1995 (CD 0x1c501): speed 30 on entry
    switch (creature_apply_damage(t)) {
    case 2:
        thing_set_state(t, base + 4);
        return;
    case 1:
        if (thing_at(t->last_attacker)->cls == 3) {
            t->aux = -10;
            t->target = t->last_attacker;
        }
        return;
    default:
        break;
    }
    creature_move_step(t);
    Thing *target = thing_at(t->target);
    if ((t->tick & 3) == 0) t->target_yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(target));
    if (target->health < 0 || (target->flags & 0x400)) {
        thing_set_state(t, base + 1);
        return;
    }
    int16_t before = t->aux;
    t->aux = (int16_t)(before + 1);
    if (before > 0x28) t->aux = -90;
    if (t->aux > 0) {
        PlayerBlock *p = owner_block(target);
        p->knock_yaw = (uint16_t)((pos_angle_to(thing_pos(t), thing_pos(target)) + 0x400) & 0x7ff);
        p->knock_pitch = 0x100;
        p->knock_speed = 0x50;
        creature_sound(t, 0x2a);
    }
    if (think_now(t)) {
        int sight = (int16_t)desc_of(t)->sight_radius;
        if ((uint32_t)pos_dist_xyz(thing_pos(t), thing_pos(target)) >= (uint32_t)sight) {
            thing_set_state(t, base + 1);
            return;
        }
        creature_sound(t, 0x25);
        t->castle_size = 5;
    }
    if (t->castle_size != 0) {
        t->castle_size--;
        kraken_fire(t, target);
    }
}

// creature_kraken_s39_update_1b390
void creature_kraken_s39_update(Thing *t) {
    creature_follow_leader(t, 0x24);
    if (engine1995()) t->speed_cur = 0x1e;                         // 1995 (CD 0x1c893)
    if (t->state == 0x26) creature_sound(t, 0x25);
}

void creature_kraken_s40_update(Thing *t) { creature_die(t, 0x24); }          // creature_kraken_s40_update_1b3e0
void creature_kraken_s41_update(Thing *t) { creature_dead_drop_mana(t); }    // creature_kraken_s41_update_1b3f0

// ---- troll (type 7, base 0x2a) -------------------------------------------------------------------
// Two sprites: 0x55 walking (variant 1; variant 2 walks as 0xc7 and never changes), 0xc6 the firing
// pose, held for aux = 30 ticks at turn_rate speed.

void creature_troll_s42_update(Thing *t) { creature_idle_seek_leader(t, 0x2a); }   // creature_kraken_s42_update_1b400

// creature_troll_s43_update_1b410 (state 43 = base + 1): every think tick the health is topped up
// (the code adds ((max_health >> 6) > max_health), i.e. 0, and then sets health = max_health whenever
// the sum is non-zero: a full heal; kept as the CPU does it), then creature_ai_step; entering the
// attack state sets aux = 1.
void creature_troll_s43_update(Thing *t) {
    if (think_now(t)) {
        t->health += ((t->max_health >> 6) > t->max_health) ? 1 : 0;
        if (t->health != 0) t->health = t->max_health;
    }
    creature_ai_step(t, 0x2a);
    if (t->state == 0x2c) t->aux = 1;
}

// creature_troll_s44_update_1b470 (state 44 = base + 2)
void creature_troll_s44_update(Thing *t) {
    if (t->aux != 0) {
        int16_t before = t->aux;
        t->aux = (int16_t)(before - 1);
        if (before == 1 && t->sprite == 0xc6) {
            thing_set_sprite(t, 0x55);
            t->speed_cur = t->speed_base;
        }
    }
    if ((creature_attack_target(t, 0x2a, creature_attack_fire_troll) & 0xff) != 0 && t->sprite == 0x55) {
        thing_set_sprite(t, 0xc6);
        t->aux = 0x1e;
        t->speed_cur = (int16_t)t->turn_rate;
    }
    if (t->state != 0x2c && t->sprite == 0xc6) {
        thing_set_sprite(t, 0x55);
        t->speed_cur = t->speed_base;
    }
}

// creature_troll_s45_update_1b510
void creature_troll_s45_update(Thing *t) {
    creature_follow_leader(t, 0x2a);
    if (t->state == 0x2c) t->aux = 1;
}

void creature_troll_s46_update(Thing *t) { creature_die(t, 0x2a); }           // creature_troll_s46_update_1b530
void creature_troll_s47_update(Thing *t) { creature_dead_drop_mana(t); }     // creature_troll_s47_update_1b540

// ---- griffon (type 8, base 0x30) -----------------------------------------------------------------

void creature_griffon_s48_update(Thing *t) { creature_idle_seek_leader(t, 0x30); }   // creature_troll_s48_update_1b550

// creature_griffon_s49_update_1b560 (state 49 = base + 1): damage intake (dead -> 52; hit by a class-3
// thing -> target it, 50); move; every think tick a random turn, then (awake only) the nearest
// player-list thing in sight / fov is attacked when it is a wanted wizard (type 0 / 1 with P+0x210
// != 0; the flag is re-armed to 200), otherwise the nearest leaderless griffon in sight / fov becomes
// the leader (51).
void creature_griffon_s49_update(Thing *t) {
    const int base = 0x30;
    switch (creature_apply_damage(t)) {
    case 2:
        thing_set_state(t, base + 4);
        return;
    case 1:
        if (thing_at(t->last_attacker)->cls == 3) {
            t->target = t->last_attacker;
            thing_set_state(t, base + 2);
        }
        return;
    default:
        break;
    }
    creature_move_step(t);
    if (!think_now(t)) return;
    creature_wander_turn(t);
    if (t->timer_a == 0) return;
    Thing *enemy = nearest_player_thing(t, false);
    if (enemy && is_wizard_type(enemy) && owner_block(enemy)->timer210 != 0) {
        t->target = thing_index(enemy);
        owner_block(enemy)->timer210 = 200;
        thing_set_state(t, base + 2);
        return;
    }
    Thing *leader = nearest_leader(t);
    if (leader) {
        t->parent = thing_index(leader);
        thing_set_state(t, base + 3);
    }
}

// creature_griffon_s50_update_1b940 (state 50 = base + 2): full speed while aux != 0, flag 0x8000
// set, creature_attack_target with the griffon's fireball; a shot gives sound 0x26 and keeps a wizard
// target wanted; every think tick sound 0x26 again.
void creature_griffon_s50_update(Thing *t) {
    if (t->aux != 0) t->speed_cur = t->speed_base;
    t->flags |= 0x8000u;
    if (creature_attack_target(t, 0x30, creature_attack_fire_griffon)) {
        creature_sound(t, 0x26);
        blame_wizard(t->target);
    }
    if (think_now(t)) creature_sound(t, 0x26);
}

void creature_griffon_s51_update(Thing *t) { creature_follow_leader(t, 0x30); }   // creature_griffon_s51_update_1ba60

// creature_griffon_s52_update_1ba70 (state 52 = base + 4): the killer wizard becomes wanted, then die.
void creature_griffon_s52_update(Thing *t) {
    blame_wizard(t->killer);
    creature_die(t, 0x30);
}

void creature_griffon_s53_update(Thing *t) { creature_dead_drop_mana(t); }   // creature_griffon_s53_update_1baf0

} // namespace

// ---- exported helpers ----------------------------------------------------------------------------

// crab_target_nearest_mana_ball_1aef0
void crab_target_nearest_mana_ball(Thing *t) {
    int best_d = 0x10000;
    Thing *best = nullptr;
    for (int i = 1; i < thing_pool_slots(); i++) {
        Thing *o = thing_at((unsigned)i);
        if (!is_mana_ball(o)) continue;
        int d = pos_dist_xyz(thing_pos(t), thing_pos(o));
        if (d >= best_d) continue;
        best = o;
        best_d = d;
    }
    t->target = best ? thing_index(best) : (uint16_t)0;
}

// The shot of creature_kraken_s38_update_1b000 (0x1b2e0..0x1b374)
Thing *kraken_fire(Thing *t, Thing *target) {
    Thing *p = thing_create(thing_pos(t), 9, 9);
    if (!p) return nullptr;
    p->impact_cls = 10;
    p->impact_type = 0x17;
    p->owner = t->owner;
    p->yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(target));
    p->pitch = (uint16_t)pos_pitch_to(thing_pos(t), thing_pos(target));
    p->z = (int16_t)(p->z + t->ext_h);
    p->desc = desc_index(0x96ad0);
    p->target = t->target;
    p->filter_type = target->filter_type;
    p->damage = 0x320;
    p->filter_cls = target->filter_cls;
    return p;
}

// ---- registration --------------------------------------------------------------------------------

void creatures2_register_handlers() {
    thing_register_update(0x1a830, creature_crab_s31_update);       // crab 31..35 (30 is in creatures.cpp)
    thing_register_update(0x1ac20, creature_crab_s32_update);
    thing_register_update(0x1ac80, creature_crab_s33_update);
    thing_register_update(0x1aed0, creature_crab_s34_update);
    thing_register_update(0x1aee0, creature_crab_s35_update);
    thing_register_update(0x1afa0, creature_kraken_s36_update);     // kraken 36..41
    thing_register_update(0x1afb0, creature_kraken_s37_update);
    thing_register_update(0x1b000, creature_kraken_s38_update);
    thing_register_update(0x1b390, creature_kraken_s39_update);
    thing_register_update(0x1b3e0, creature_kraken_s40_update);
    thing_register_update(0x1b3f0, creature_kraken_s41_update);
    thing_register_update(0x1b400, creature_troll_s42_update);      // troll 42..47
    thing_register_update(0x1b410, creature_troll_s43_update);
    thing_register_update(0x1b470, creature_troll_s44_update);
    thing_register_update(0x1b510, creature_troll_s45_update);
    thing_register_update(0x1b530, creature_troll_s46_update);
    thing_register_update(0x1b540, creature_troll_s47_update);
    thing_register_update(0x1b550, creature_griffon_s48_update);    // griffon 48..53
    thing_register_update(0x1b560, creature_griffon_s49_update);
    thing_register_update(0x1b940, creature_griffon_s50_update);
    thing_register_update(0x1ba60, creature_griffon_s51_update);
    thing_register_update(0x1ba70, creature_griffon_s52_update);
    thing_register_update(0x1baf0, creature_griffon_s53_update);
}
