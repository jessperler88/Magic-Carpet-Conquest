// Creatures (class 5): emu (0x1c8f0..0x1c950), genie (0x1c950..0x1d420), type 15 (0x1ea60..0x1f200),
// wyvern (0x1f200..0x1f690) and the disabled records (0x1f690..0x1f6b0). Translated from the
// disassembly of carpet.exe; the shared bodies are in creature_common.cpp (creatures.h, base = type * 6).
#include "creatures3.h"
#include "mc_math.h"
#include "mode_rules.h"     // same_side (round 12)
#include "world_set.h"
#include "gen/creatures3_tables.h"

namespace {

inline const MoveDesc *desc_of(const Thing *t) { return mc_move_desc(t->desc); }

constexpr uint32_t desc_index(uint32_t addr) { return (addr - 0x96a10u) / 0x20u; }

// tick % desc.think_period == 0 (the original faults on a zero period)
inline bool think_now(const Thing *t) {
    int period = (int16_t)desc_of(t)->think_period;
    return period != 0 && (int)t->tick % period == 0;
}

// tick % n == 0 for a derived period (think_period + 1, * 2, * 8, ...)
inline bool every(const Thing *t, int n) { return n != 0 && (int)t->tick % n == 0; }

// Unsigned squared xy distance of the torus differences (the original's uint32 sum of the 16-bit
// wrapped differences, at most 2^31: the same value with 256 cells; 64-bit for a wider world).
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

inline bool target_gone(const Thing *target) { return target->health < 0 || (target->flags & 0x400); }

// sound_request_49720((int16)thing index, -1, id)
inline void creature_sound(const Thing *t, int id) { sound_request((int16_t)thing_index(t), -1, id); }

// The enemy search of the main states: nearest player-list thing within the sight radius (squared xy
// distance, unsigned), without flag 0x20, inside the field of view; `accept` filters first.
template <typename F>
Thing *nearest_enemy_in_fov(const Thing *t, F accept) {
    uint64_t limit = sight_sq(t);
    int fov = (int16_t)desc_of(t)->fov;
    uint64_t best_d = kFarSq;
    Thing *best = nullptr;
    for (uint32_t i = g_cfg->player_list; i != 0; i = thing_at(i)->next) {
        Thing *p = thing_at(i);
        if (!accept(p)) continue;
        uint64_t d = dist_sq_xy16(p, t);
        if (d > limit) continue;
        if (p->flags & 0x20) continue;
        int ang = pos_angle_to(thing_pos(t), thing_pos(p)) & 0xffff;
        if ((angle_diff(t->yaw, ang) & 0xffff) >= fov) continue;
        if (d >= best_d) continue;
        best = p;
        best_d = d;
    }
    return best;
}

// Aim a freshly created projectile from t at target (the yaw / pitch pair every attack here computes).
inline void aim_projectile(Thing *p, const Thing *t, const Thing *target) {
    p->yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(target));
    p->pitch = (uint16_t)pos_pitch_to(thing_pos(t), thing_pos(target));
}

// ---- emu (type 10, base 0x3c): a plain creature_ai_step type with the archer's arrows -------------

void creature_emu_s61_update(Thing *t) { creature_ai_step(t, 0x3c); }             // creature_emu_s61_update_1c8f0
void creature_emu_s62_update(Thing *t) { creature_attack_target(t, 0x3c, creature_attack_arrow); }  // creature_emu_s62_update_1c900 (result unused)
void creature_emu_s63_update(Thing *t) { creature_follow_leader(t, 0x3c); }       // creature_emu_s63_update_1c920
void creature_emu_s64_update(Thing *t) { creature_die(t, 0x3c); }                 // creature_emu_s64_update_1c930
void creature_emu_s65_update(Thing *t) { creature_dead_drop_mana(t); }            // creature_emu_s65_update_1c940

// ---- genie (type 11, base 0x42) --------------------------------------------------------------------
// The genie is hidden (flags bit 0) while it wanders in state 67 and shows itself (state 66 toggles the
// bit) in front of a target to fight it in state 68; it steals mana balls and regenerates.

// creature_genie_s66_update_1c950 (state 66 = base + 0, the state the constructor sets): the vanish /
// appear transition. First tick (aux == 0): 12 class-10 type-1 puffs on a 3 x 4 grid of 0x28 from the
// genie (its owner, flag 1), aux = 1, visibility toggled. Second tick (aux == 1): sound
// 0x15; hidden -> forget the target and the pending attacker, teleport 0x3200 + (rng % 60) * 0x100 in
// x and y, state 67; visible -> state 68.
void creature_genie_s66_update(Thing *t) {
    int16_t aux = t->aux;
    if (aux == 0) {
        t->aux = 0xc;
        for (;;) {
            int16_t n = t->aux;
            t->aux = (int16_t)(n - 1);
            if (n == 0) break;
            g_pos_scratch = *thing_pos(t);
            int k = (int16_t)t->aux;
            g_pos_scratch.x = g_pos_scratch.x + (k % 3) * 0x28;        // (16-bit sums; WCoord wraps)
            g_pos_scratch.y = g_pos_scratch.y + (k / 3) * 0x28;
            Thing *e = thing_create(&g_pos_scratch, 10, 1);
            if (e) {
                e->owner = t->owner;
                e->flags |= 0x10000u;           // or byte [eax+0x12], 1
            }
        }
        t->aux = 1;
        t->flags ^= 1u;
        return;
    }
    t->aux = (int16_t)(aux - 1);
    if (aux != 1) return;
    creature_sound(t, 0x15);
    if (t->flags & 1) {
        t->target = 0;
        t->damage_slots[0].attacker = 0;
        g_pos_scratch = *thing_pos(t);
        t->rng = mc_lcg(t->rng);
        // (a jump of 50..109 cells, wrapped at the world's width: a game range, the same in any world)
        g_pos_scratch.x = g_pos_scratch.x + (int32_t)(((t->rng % 0x3cu) << 8) + 0x3200);
        t->rng = mc_lcg(t->rng);
        g_pos_scratch.y = g_pos_scratch.y + (int32_t)(((t->rng % 0x3cu) << 8) + 0x3200);
        thing_move_to(t, &g_pos_scratch);
        thing_set_state(t, 0x43);
    } else {
        thing_set_state(t, 0x44);
    }
}

// creature_genie_s67_update_1caf0 (state 67 = base + 1): hidden wandering. Damage: dead -> state 70;
// hit by a class-3 thing -> appear in front of it. Else move; every think tick: regenerate
// max_health / 64 (clamped to -1 .. max_health), while awake and above a quarter of max_health look
// for an enemy in sight / fov (appear in front of it) or steal a mana ball; random heading change;
// above three quarters of max_health the first player-list thing that holds mana becomes the target.
void creature_genie_s67_update(Thing *t) {
    const int base = 0x42;
    switch (creature_apply_damage(t)) {
    case 2:
        thing_set_state(t, (uint8_t)(base + 4));
        return;
    case 1:
        if (thing_at(t->last_attacker)->cls != 3) return;
        t->target = t->last_attacker;
        genie_appear_at_target(t);
        return;
    default:
        break;
    }
    creature_move_step(t);
    if (!think_now(t)) return;
    int32_t h = t->health + (t->max_health >> 6);
    t->health = h;
    if (h < -1) t->health = -1;
    if (t->health > t->max_health) t->health = t->max_health;
    if (t->timer_a != 0 && (t->max_health >> 2) < t->health) {
        // 1995 (CD 0x1e1ab): only wizards (player types 0 / 1) are candidates
        const bool e95 = engine1995();
        Thing *enemy = nearest_enemy_in_fov(t, [e95](const Thing *p) { return !e95 || p->type == 0 || p->type == 1; });
        if (enemy) {
            t->target = thing_index(enemy);
            genie_appear_at_target(t);
        } else {
            genie_steal_mana(t);
        }
    }
    // (the remainder test of the inlined wander turn is `(tick % period) * 4 != 0`: already false here)
    creature_wander_turn(t);
    if (t->max_health - (t->max_health >> 2) >= t->health) return;
    // the first player-list thing with mana and without flag 0x20 (the "score" is 1 or 0, so a later
    // candidate never beats the first)
    Thing *best = nullptr;
    int best_score = 0;
    for (uint32_t i = g_cfg->player_list; i != 0; i = thing_at(i)->next) {
        Thing *p = thing_at(i);
        if (engine1995() && p->type != 0 && p->type != 1) continue;   // 1995 (CD 0x1e311): wizards only
        int score = (p->mana != 0 && !(p->flags & 0x20)) ? 1 : 0;
        if (score != 0 && score > best_score) {
            best = p;
            best_score = score;
        }
    }
    if (!best) return;
    t->target = thing_index(best);
    genie_appear_at_target(t);
}

// creature_genie_s68_update_1ce80 (state 68 = base + 2): visible attack. Damage: dead -> state 70;
// hit by a class-3 thing -> retarget. Else move; below half health -> vanish; target dead / deleted /
// gone -> steal mana, vanish; else aim every 8 ticks. Every think tick: out of sight -> vanish; else
// sound 0xb every 8th think tick, aux++, and a class-9 type-8 projectile (impact 0x19, damage 3000,
// aux 0x14) at the target.
void creature_genie_s68_update(Thing *t) {
    const int base = 0x42;
    switch (creature_apply_damage(t)) {
    case 2:
        thing_set_state(t, (uint8_t)(base + 4));
        return;
    case 1:
        if (thing_at(t->last_attacker)->cls == 3) t->target = t->last_attacker;
        return;
    default:
        break;
    }
    creature_move_step(t);
    Thing *target = thing_at(t->target);
    if ((t->max_health >> 1) > t->health) {
        genie_vanish(t);
    } else if (target_gone(target) || target->cls == 0) {
        genie_steal_mana(t);
        genie_vanish(t);
    } else if ((t->tick & 7) == 0) {
        t->target_yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(target));
    }
    if (!think_now(t)) return;
    int sight = (int16_t)desc_of(t)->sight_radius;
    if ((uint32_t)pos_dist_xyz(thing_pos(t), thing_pos(target)) >= (uint32_t)sight) {
        genie_vanish(t);
        return;
    }
    if (every(t, (int16_t)desc_of(t)->think_period * 8)) creature_sound(t, 0xb);
    t->aux++;
    // (the original has two identical thing_create branches for "target type 0 and aux odd" and the
    // rest; both set impact type 0x19)
    Thing *p = thing_create(thing_pos(t), 9, 8);
    if (!p) return;
    p->impact_type = 0x19;
    p->impact_cls = 10;
    p->owner = t->owner;
    p->damage = 0xbb8;
    p->z = (int16_t)(p->z + t->ext_h);
    p->aux = 0x14;
    p->target = t->target;
    aim_projectile(p, t, target);
    creature_sound(t, 9);
}

void creature_genie_s69_update(Thing *t) { creature_follow_leader(t, 0x42); }     // creature_genie_s69_update_1d1f0
void creature_genie_s70_update(Thing *t) { creature_die(t, 0x42); }               // creature_genie_s70_update_1d200
void creature_genie_s71_update(Thing *t) { creature_dead_drop_mana(t); }          // creature_genie_s71_update_1d210

// ---- type 15 (base 0x5a): the guard archer of the wizard castles --------------------------------------

// creature_type15_s91_update_1ea60 (state 91 = base + 1): grid walk; damage: dead -> 94, hit by a
// class-3 thing of another owner -> attack it (92). Every think tick while awake: the nearest
// player-list thing of another owner in sight / fov -> 92. Entering 92 sets the shooting sprite.
void creature_type15_s91_update(Thing *t) {
    switch (creature_apply_damage(t)) {
    case 2:
        thing_set_state(t, 0x5e);
        break;
    case 1: {
        const Thing *a = thing_at(t->last_attacker);
        if (a->cls == 3 && !same_side(a->owner, t->owner)) {
            t->target = t->last_attacker;
            thing_set_state(t, 0x5c);
        }
        break;
    }
    default: {
        type15_move(t);
        if (!think_now(t)) break;
        if (t->timer_a == 0) break;
        Thing *enemy = nearest_enemy_in_fov(t, [t](const Thing *p) { return !same_side(p->owner, t->owner); });
        if (!enemy) break;
        t->target = thing_index(enemy);
        thing_set_state(t, 0x5c);
        break;
    }
    }
    if (t->state == 0x5c) type15_set_attack_sprite(t);
}

// creature_type15_s92_update_1ecd0 (state 92 = base + 2): stand and shoot. Damage: dead -> 94 (a hit
// changes nothing). Aim every 4 ticks; target dead / deleted -> 91; every think tick: out of sight ->
// 91, else a class-9 type-0xd arrow (the creature's filter, raised by ext_h) at the target. Leaving
// 92 restores the walking sprite.
void creature_type15_s92_update(Thing *t) {
    if (creature_apply_damage(t) == 2) {
        t->state = 0x5e;
    } else {
        Thing *target = thing_at(t->target);
        if ((t->tick & 3) == 0) t->target_yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(target));
        if (target_gone(target)) {
            thing_set_state(t, 0x5b);
        } else if (think_now(t)) {
            int sight = (int16_t)desc_of(t)->sight_radius;
            if ((uint32_t)pos_dist_xyz(thing_pos(t), thing_pos(target)) >= (uint32_t)sight) {
                thing_set_state(t, 0x5b);
            } else {
                Thing *p = thing_create(thing_pos(t), 9, 0xd);
                if (p) {
                    p->owner = t->owner;
                    aim_projectile(p, t, target);
                    p->z = (int16_t)(p->z + t->ext_h);
                    p->target = t->target;
                    p->filter_type = t->filter_type;
                    p->filter_cls = t->filter_cls;
                }
            }
        }
    }
    if (t->state != 0x5c) type15_set_move_sprite(t);
}

void creature_type15_s93_update(Thing *t) { creature_follow_leader(t, 0x5a); }    // creature_type15_s93_update_1eee0
void creature_type15_s94_update(Thing *t) { creature_die(t, 0x5a); }              // creature_type15_s94_update_1eef0
void creature_type15_s95_update(Thing *t) { creature_dead_drop_mana(t); }         // creature_type15_s95_update_1ef00

// ---- wyvern (type 16, base 0x60) ---------------------------------------------------------------------

// creature_type15_s96_update_1f200 (state 96 = wyvern base + 0)
void creature_wyvern_s96_update(Thing *t) { creature_idle_seek_leader(t, 0x60); }

// creature_wyvern_s97_update_1f210 (state 97 = base + 1): creature_ai_step, and every
// (think_period + 1) ticks the nearest wizard castle (wizard_list) in sight becomes the target (98).
void creature_wyvern_s97_update(Thing *t) {
    creature_ai_step(t, 0x60);
    if (t->state != 0x61) return;
    if (!every(t, (int16_t)desc_of(t)->think_period + 1)) return;
    uint64_t limit = sight_sq(t);
    uint64_t best_d = kFarSq;
    Thing *best = nullptr;
    for (uint32_t i = g_cfg->wizard_list; i != 0; i = thing_at(i)->next) {
        Thing *w = thing_at(i);
        uint64_t d = dist_sq_xy16(w, t);
        if (d > limit) continue;
        if (d >= best_d) continue;
        best = w;
        best_d = d;
    }
    if (!best) return;
    t->target = thing_index(best);
    thing_set_state(t, 0x62);
}

// creature_wyvern_s98_update_1f2e0 (state 98 = base + 2): attack. Damage: dead -> 100; hit by a
// class-3 thing -> retarget (a hit ends the tick). Else move; every 8 ticks aim at the target (a
// non-player target only from 0x200 away); target dead / deleted -> 97; while aux > 0 one class-9
// type-0 fireball per tick (desc 0x96a50, damage 3000, mana 60000, raised by 4 * ext_h). Every think
// tick: target beyond the sight radius (xy) -> 97; sound 0x27 every second think tick; facing the
// target within 0xe3 -> aux = 15.
void creature_wyvern_s98_update(Thing *t) {
    switch (creature_apply_damage(t)) {
    case 2:
        thing_set_state(t, 0x64);
        return;
    case 1:
        if (thing_at(t->last_attacker)->cls == 3) t->target = t->last_attacker;
        return;
    default:
        break;
    }
    creature_move_step(t);
    Thing *target = thing_at(t->target);
    if ((t->tick & 7) == 0) {
        if (target->cls == 3 || (uint32_t)pos_dist_xyz(thing_pos(t), thing_pos(target)) >= 0x200u)
            t->target_yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(target));
    }
    if (target_gone(target)) {
        thing_set_state(t, 0x61);
        return;
    }
    if (t->aux != 0) {
        t->aux--;
        Thing *p = thing_create(thing_pos(t), 9, 0);
        if (p) {
            p->impact_cls = 10;
            p->impact_type = 0;
            p->desc = desc_index(0x96a50);
            p->filter_type = t->filter_type;
            p->filter_cls = t->filter_cls;
            p->owner = t->owner;
            aim_projectile(p, t, target);
            p->z = (int16_t)(p->z + (int16_t)(t->ext_h << 2));
            p->damage = 0xbb8;
            p->mana = 0xea60;
            p->target = t->target;
        }
    }
    if (!think_now(t)) return;
    if (dist_sq_xy16(target, t) >= sight_sq(t)) {
        thing_set_state(t, 0x61);
        return;
    }
    if (every(t, (int16_t)desc_of(t)->think_period * 2)) creature_sound(t, 0x27);
    int ang = pos_angle_to(thing_pos(t), thing_pos(target)) & 0xffff;
    if ((angle_diff(t->yaw, ang) & 0xffff) < 0xe3) t->aux = 0xf;
}

void creature_wyvern_s99_update(Thing *t)  { creature_follow_leader(t, 0x60); }   // creature_wyvern_s99_update_1f660
void creature_wyvern_s100_update(Thing *t) { creature_die(t, 0x60); }             // creature_wyvern_s100_update_1f670
void creature_wyvern_s101_update(Thing *t) { creature_dead_drop_mana(t); }        // creature_wyvern_s101_update_1f680

// creature_update_shared_1f690 / 1f6a0 / 1f6b0 (states 102..119, disabled in the table): a lone `ret`.
void creature_noop_update(Thing *) {}

} // namespace

// ---- exported helpers ----------------------------------------------------------------------------------

// genie_vanish_1d220
void genie_vanish(Thing *t) {
    t->target = 0;
    t->aux = 0;
    thing_set_state(t, 0x42);
    creature_sound(t, 0xb);
}

// genie_appear_at_target_1d270
void genie_appear_at_target(Thing *t) {
    if (t->target == 0) return;
    t->aux = 0;
    thing_set_state(t, 0x42);
    const Thing *target = thing_at(t->target);
    g_pos_scratch = *thing_pos(target);
    math_rotate_offset(&g_pos_scratch, target->yaw, 0, (int16_t)(t->speed_cur << 6));
    thing_move_to(t, &g_pos_scratch);
}

// genie_steal_mana_1d310
void genie_steal_mana(Thing *t) {
    if (t->mana >= t->mana_total) return;
    uint64_t limit = sight_sq(t);
    uint64_t best_d = kFarSq;
    Thing *ball = nullptr;
    for (uint32_t i = g_cfg->mana_ball_list; i != 0; i = thing_at(i)->next) {
        Thing *o = thing_at(i);
        if (o->type != 0x27) continue;
        uint64_t d = dist_sq_xy16(o, t);
        if (d > limit) continue;
        if (d >= best_d) continue;
        ball = o;
        best_d = d;
    }
    if (!ball) return;
    t->mana += ball->mana;
    ball->mana_owner = 0;
    g_pos_scratch = *thing_pos(ball);
    thing_mark_delete(ball);
    Thing *e = thing_create(&g_pos_scratch, 10, 0);
    if (e) {
        e->owner = t->owner;
        e->flags |= 0x10000u;               // or byte [eax+0x12], 1
    }
    creature_sound(t, 0xb);
}

// type15_set_attack_sprite_1ef10
void type15_set_attack_sprite(Thing *t) {
    t->rng = mc_lcg(t->rng);
    uint32_t r = t->rng % 0x14u;
    t->speed_cur = 0;
    thing_set_sprite(t, r > 10 ? 1 : 0xce);
}

// type15_set_move_sprite_1ef50
void type15_set_move_sprite(Thing *t) {
    t->speed_cur = t->speed_base;
    thing_set_sprite(t, 0);
}

// type15_move_1ef80
void type15_move(Thing *t) {
    uint16_t weights[4];
    for (int i = 0; i < 4; i++) weights[i] = g_type15_dir_weights[i];
    if ((int)t->tick % 8 == 0) {
        uint32_t forbidden = ~desc_of(t)->terrain_mask;
        if (terrain_type_mask_at(thing_pos(t)) & forbidden) {
            t->state = 0x5e;
            return;
        }
        // score the four headings yaw, yaw + 0x200, + 0x400, + 0x600: a passable step of 0x100 scores
        // rng % weight + 2 (strictly better than the best so far wins; best starts at 1)
        uint16_t try_yaw = t->yaw;
        uint32_t best = 1;
        for (int i = 0; i < 4; i++) {
            g_pos_scratch = *thing_pos(t);
            math_rotate_offset(&g_pos_scratch, try_yaw, 0, 0x100);
            t->rng = mc_lcg(t->rng);
            uint32_t r = t->rng % (uint32_t)weights[i];
            uint32_t blocked = creature_check_terrain(t, &g_pos_scratch, 1);
            uint32_t score = blocked == 0 ? r + 2 : 0;
            if ((uint16_t)score > (uint16_t)best) {
                best = score;
                t->yaw = try_yaw;
            }
            try_yaw = (uint16_t)((try_yaw + 0x200) & 0x7ff);
        }
    }
    g_pos_scratch = *thing_pos(t);
    if ((int)t->tick % 0x10 == 0) {
        // snap the coordinate across the heading to the cell centre (jump table 0x1ef6c: 0 / 2 -> y,
        // 1 / 3 -> x)
        int k = (((int)t->yaw - 0x100) >> 9) & 3;
        // (the original's ((int16)v >> 8 << 8) + 0x80 in 16 bits: the cell's centre)
        if (k == 0 || k == 2)
            g_pos_scratch.y = (g_pos_scratch.y & ~0xff) + 0x80;
        else
            g_pos_scratch.x = (g_pos_scratch.x & ~0xff) + 0x80;
    }
    // turn away from the first creature of the same type and another owner within a cell in x and y
    for (uint32_t i = g_cfg->creature_lists[(int8_t)t->type]; i != 0; i = thing_at(i)->next) {
        const Thing *o = thing_at(i);
        if (same_side(o->owner, t->owner)) continue;
        // (sign-extended operands, no wrap: the original's quirk at 256 cells; a wider world takes the torus
        // difference - round 11 session 3)
        const bool wide = world_wide();
        int dx = wide ? world_signed(t->x - o->x) : world_signed(t->x) - world_signed(o->x);
        if (dx < 0) dx = -dx;
        if (dx >= 0x100) continue;
        int dy = wide ? world_signed(t->y - o->y) : world_signed(t->y) - world_signed(o->y);
        if (dy < 0) dy = -dy;
        if (dy >= 0x100) continue;
        t->target_yaw = (uint16_t)pos_angle_to(thing_pos(o), thing_pos(t));
        break;
    }
    if (t->target_yaw != t->yaw) {
        t->rng = mc_lcg(t->rng);
        if (t->rng % 0x14u > 10) return;        // half of the ticks are skipped while turning
    }
    math_rotate_offset(&g_pos_scratch, t->yaw, 0, (int16_t)t->speed_cur);
    const MoveDesc *d = desc_of(t);
    int ground = (int16_t)terrain_height_at(&g_pos_scratch);
    pos_follow_ground(&g_pos_scratch, ground, d->clear_hi, d->clear_lo, d->z_step);
    thing_move_to(t, &g_pos_scratch);
}

// ---- registration ------------------------------------------------------------------------------------

void creatures3_register_handlers() {
    thing_register_update(0x1c8f0, creature_emu_s61_update);        // emu 61..65 (60 is in creatures.cpp)
    thing_register_update(0x1c900, creature_emu_s62_update);
    thing_register_update(0x1c920, creature_emu_s63_update);
    thing_register_update(0x1c930, creature_emu_s64_update);
    thing_register_update(0x1c940, creature_emu_s65_update);

    thing_register_update(0x1c950, creature_genie_s66_update);      // genie 66..71
    thing_register_update(0x1caf0, creature_genie_s67_update);
    thing_register_update(0x1ce80, creature_genie_s68_update);
    thing_register_update(0x1d1f0, creature_genie_s69_update);
    thing_register_update(0x1d200, creature_genie_s70_update);
    thing_register_update(0x1d210, creature_genie_s71_update);

    thing_register_update(0x1ea60, creature_type15_s91_update);     // type 15: 91..95 (90 is in creatures.cpp)
    thing_register_update(0x1ecd0, creature_type15_s92_update);
    thing_register_update(0x1eee0, creature_type15_s93_update);
    thing_register_update(0x1eef0, creature_type15_s94_update);
    thing_register_update(0x1ef00, creature_type15_s95_update);

    thing_register_update(0x1f200, creature_wyvern_s96_update);     // wyvern 96..101
    thing_register_update(0x1f210, creature_wyvern_s97_update);
    thing_register_update(0x1f2e0, creature_wyvern_s98_update);
    thing_register_update(0x1f660, creature_wyvern_s99_update);
    thing_register_update(0x1f670, creature_wyvern_s100_update);
    thing_register_update(0x1f680, creature_wyvern_s101_update);

    thing_register_update(0x1f690, creature_noop_update);           // 102..119: disabled records
    thing_register_update(0x1f6a0, creature_noop_update);
    thing_register_update(0x1f6b0, creature_noop_update);
}
