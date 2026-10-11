// Creatures (class 5): the per-type Table A handlers. Translated from the disassembly of carpet.exe.
//
// Ported here (address order): dragon / vulture / bee / worm / archer (0x19b70..0x1a830), skeleton
// (0x1bb00..0x1c8e0), builder incl. its castle placement state (0x1d420..0x1e130), townie
// (0x1e130..0x1e5b0), trader (0x1e5c0..0x1ea50). The shared bodies they call are in
// creature_common.cpp; see creatures.h for the state layout (base = type * 6).
#include "creatures.h"
#include "level_features.h"   // effect_wizard_init, terrain_rect_height_range, castle_footprint
#include "mc_math.h"
#include "mode_rules.h"     // same_side (round 12)
#include "mode.h"

namespace {

inline const MoveDesc *desc_of(const Thing *t) { return mc_move_desc(t->desc); }

inline PlayerBlock *owner_block(const Thing *t) {     // Thing+0xa0 (the dummy block for 0)
    return reinterpret_cast<PlayerBlock *>(thing_player_block(t));
}

// tick % desc.think_period == 0 (the original faults on a zero period)
inline bool think_now(const Thing *t) {
    int period = (int16_t)desc_of(t)->think_period;
    return period != 0 && (int)t->tick % period == 0;
}

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

// The original's 0xffffffff "no limit / none yet" (no 256-world distance reaches it); 64-bit maximum so
// that a wider world's far candidates (beyond 256 cells: > 2^32) still count.
constexpr uint64_t kFarSq = ~(uint64_t)0;

// sound_request_49720((int16)thing index, -1, id)
inline void creature_sound(const Thing *t, int id) { sound_request((int16_t)thing_index(t), -1, id); }

// Nearest thing of a Config list within `limit` (squared xy distance, unsigned compares; the first
// of equally near ones wins). `accept` filters candidates before the distance test.
template <typename F>
Thing *nearest_in_list(uint32_t head, const Thing *t, uint64_t limit, F accept) {
    uint64_t best_d = kFarSq;
    Thing *best = nullptr;
    for (uint32_t i = head; i != 0; i = thing_at(i)->next) {
        Thing *o = thing_at(i);
        if (!accept(o)) continue;
        uint64_t d = dist_sq_xy16(o, t);
        if (d > limit) continue;
        if (d >= best_d) continue;
        best = o;
        best_d = d;
    }
    return best;
}

// ---- dragon (type 0, base 0) ---------------------------------------------------------------------

// creature_generic_s0_update_19b70 (state 0)
void creature_dragon_s0_update(Thing *t) { creature_idle_seek_leader(t, 0); }

// creature_apply_zvel_19c30: the dragon's bobbing flight; aux is the vertical speed, -5 per tick and
// reset to 0x96 below ground + 0x100.
void creature_apply_zvel(Thing *t) {
    t->z = (int16_t)(t->z + t->aux);
    int ground = (int16_t)terrain_height_at(thing_pos(t));
    t->aux = (int16_t)(t->aux - 5);
    if ((int)t->z < ground + 0x100) t->aux = 0x96;
}

// creature_dragon_s1_19b80
void creature_dragon_s1_update(Thing *t) {
    creature_ai_step(t, 0);
    creature_apply_zvel(t);
}

// creature_dragon_s2_update_19ba0
void creature_dragon_s2_update(Thing *t) {
    if (creature_attack_target(t, 0, creature_attack_fire)) creature_sound(t, 8);
    creature_apply_zvel(t);
}

// creature_dragon_s3_update_19bf0
void creature_dragon_s3_update(Thing *t) {
    creature_follow_leader(t, 0);
    creature_apply_zvel(t);
}

void creature_dragon_s4_update(Thing *t) { creature_die(t, 0); }           // creature_dragon_s4_update_19c10
void creature_dragon_s5_update(Thing *t) { creature_dead_drop_mana(t); }   // creature_dragon_s5_update_19c20

// ---- vulture (type 1, base 6) --------------------------------------------------------------------

// creature_dragon_s6_update_19c70 (state 6 = vulture base + 0): fly to the target (a type-0x28 mana
// thing picked in state 7) while it exists.
void creature_vulture_s6_update(Thing *t) {
    creature_idle_seek_leader(t, 6);
    creature_move_step(t);
    if (t->state != 6) return;
    if (!think_now(t)) return;
    Thing *target = thing_at(t->target);
    if (target->cls != 0) {
        t->target_yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(target));
        return;
    }
    t->target = 0;
    thing_set_state(t, 7);
}

// creature_vulture_s7_update_19d10
void creature_vulture_s7_update(Thing *t) {
    creature_ai_step(t, 6);
    if (!think_now(t)) return;
    Thing *best = nearest_in_list(g_cfg->mana_ball_list, t, sight_sq(t),
                                  [](const Thing *o) { return o->type == 0x28; });
    if (!best) return;
    t->target = thing_index(best);
    thing_set_state(t, 6);
}

// creature_vulture_s8_update_19de0
void creature_vulture_s8_update(Thing *t) {
    if (creature_attack_target(t, 6, creature_attack_melee)) creature_sound(t, 7);
}

void creature_vulture_s9_update(Thing *t)  { creature_follow_leader(t, 6); }   // creature_vulture_s9_update_19e30
void creature_vulture_s10_update(Thing *t) { creature_die(t, 6); }             // creature_vulture_s10_update_19e40
void creature_vulture_s11_update(Thing *t) { creature_dead_drop_mana(t); }     // creature_vulture_s11_update_19e50

// ---- bee (type 2, base 0xc) ----------------------------------------------------------------------

// creature_vulture_s12_update_19e60 (state 12 = bee base + 0)
void creature_bee_s12_update(Thing *t) {
    creature_idle_seek_leader(t, 0xc);
    if (t->state == 0xe) t->aux = 1;
}

// creature_bee_s13_update_19e80
void creature_bee_s13_update(Thing *t) {
    creature_ai_step(t, 0xc);
    if (t->state == 0xe) {
        creature_sound(t, 0xd);
        t->aux = 1;
    }
}

// creature_bee_s14_update_19ed0: dive at the target (three times the base speed once the aux delay
// ran out), level with it by desc.z_step per tick, back off at -turn_rate after a sting.
void creature_bee_s14_update(Thing *t) {
    int16_t delay = t->aux;
    if (delay != 0) {
        t->aux = (int16_t)(delay - 1);
        if (delay == 1) t->speed_cur = (int16_t)(t->speed_base * 3);
    }
    const Thing *target = thing_at(t->target);
    int diff = (int)t->z - (int)target->z;
    int sign = diff == 0 ? 0 : diff / (diff < 0 ? -diff : diff);
    t->z = (int16_t)(t->z + (int16_t)desc_of(t)->z_step * sign);
    if (creature_attack_target(t, 0xc, creature_attack_melee)) {
        creature_sound(t, 0xd);
        t->speed_cur = (int16_t)(0 - t->turn_rate);
        t->aux = (int16_t)(desc_of(t)->think_period * 3);
    }
    if (t->state != 0xe) t->speed_cur = t->speed_base;
}

// creature_bee_s15_update_19fd0
void creature_bee_s15_update(Thing *t) {
    creature_follow_leader(t, 0xc);
    if (t->state == 0xe) t->aux = 1;
}

void creature_bee_s16_update(Thing *t) { creature_die(t, 0xc); }           // creature_bee_s16_update_19ff0
void creature_bee_s17_update(Thing *t) { creature_dead_drop_mana(t); }     // creature_bee_s17_update_1a000

// ---- worm (type 3, base 0x12) --------------------------------------------------------------------

void creature_worm_s18_update(Thing *t) { creature_idle_seek_leader(t, 0x12); }   // creature_bee_s18_update_1a010 (state 18)
void creature_worm_s19_update(Thing *t) { creature_ai_step(t, 0x12); }            // creature_worm_s19_update_1a020

// creature_worm_s20_update_1a030
void creature_worm_s20_update(Thing *t) {
    if (creature_attack_target(t, 0x12, creature_attack_fire)) creature_sound(t, 8);
}

void creature_worm_s21_update(Thing *t) { creature_follow_leader(t, 0x12); }      // creature_worm_s21_update_1a080
void creature_worm_s22_update(Thing *t) { creature_die(t, 0x12); }                // creature_worm_s22_update_1a090
void creature_worm_s23_update(Thing *t) { creature_dead_drop_mana(t); }           // creature_worm_s23_update_1a0a0

// ---- archer (type 4, base 0x18) ------------------------------------------------------------------
// The archers are the villagers' guards: they only go for wizards who are "wanted" (P+0x210 != 0, set
// to 200 by an attack on a villager, a wizard castle or an archer) and for skeletons.

void villager_blame(uint16_t thing);

// creature_set_attack_sprite_1a760: stand still, one of two shooting sprites by rng % 20, collision
// filter = the target's class / type.
void creature_set_attack_sprite(Thing *t) {
    t->rng = mc_lcg(t->rng);
    uint32_t r = t->rng % 0x14u;
    t->speed_cur = 0;
    thing_set_sprite(t, r > 10 ? 1 : 0xce);
    const Thing *target = thing_at(t->target);
    t->filter_cls = target->cls;
    t->filter_type = target->type;
}

// creature_set_move_sprite_1a7f0
void creature_set_move_sprite(Thing *t) {
    t->speed_cur = t->speed_base;
    thing_set_sprite(t, 0);
    t->filter_cls = 3;
    t->filter_type = 0xff;
}

// creature_worm_s24_update_1a0b0 (state 24 = archer base + 0)
void creature_archer_s24_update(Thing *t) {
    creature_idle_seek_leader(t, 0x18);
    if (t->state == 0x1a) creature_set_attack_sprite(t);
}

// creature_archer_s25_1a0e0 (state 25 = base + 1)
void creature_archer_s25_update(Thing *t) {
    const int base = 0x18;
    t->aux = 0;
    switch (creature_apply_damage(t)) {
    case 2:
        thing_set_state(t, (uint8_t)(base + 4));
        break;
    case 1:
        if (thing_at(t->last_attacker)->cls == 3) {
            t->target = t->last_attacker;
            thing_set_state(t, (uint8_t)(base + 2));
        }
        break;
    default: {
        creature_move_step(t);
        if (!think_now(t)) break;
        if (t->target != 0) {
            // on its way into a wizard castle (the target is given from outside)
            Thing *w = thing_at(t->target);
            if (w->cls == 10 && w->type == 0x2d) {
                if ((uint32_t)pos_dist_xyz(thing_pos(t), thing_pos(w)) > 0x1000u) {
                    t->target_yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(w));
                } else {
                    thing_set_state(t, (uint8_t)(base + 4));
                    t->aux = 1;                 // "arrived": state 28 deletes it
                    w->aux++;
                }
            } else {
                t->target = 0;
            }
            break;
        }
        creature_wander_turn(t);
        int period4 = (int16_t)desc_of(t)->think_period * 4;
        if (period4 == 0 || (int)t->tick % period4 != 0) break;
        // nearest player-list thing in sight and in the field of view; it only counts when it is a
        // wanted wizard
        uint64_t limit = sight_sq(t);
        int fov = (int16_t)desc_of(t)->fov;
        uint64_t best_d = kFarSq;
        Thing *enemy = nullptr;
        for (uint32_t i = g_cfg->player_list; i != 0; i = thing_at(i)->next) {
            Thing *p = thing_at(i);
            uint64_t d = dist_sq_xy16(p, t);
            if (d > limit) continue;
            if (p->flags & 0x20) continue;
            int ang = pos_angle_to(thing_pos(t), thing_pos(p)) & 0xffff;
            if ((angle_diff(t->yaw, ang) & 0xffff) >= fov) continue;
            if (d >= best_d) continue;
            enemy = p;
            best_d = d;
        }
        if (enemy && !((enemy->type == 0 || enemy->type == 1) && owner_block(enemy)->timer210 != 0)) enemy = nullptr;
        if (enemy && mode_target_sheltered(enemy)) enemy = nullptr;     // port (mode runs): not a wizard in its castle
        if (!enemy)         // the nearest skeleton in sight (cfg+0x8e42 = list 9)
            enemy = nearest_in_list(g_cfg->creature_lists[9], t, limit, [](const Thing *) { return true; });
        if (enemy) {
            if (enemy->cls == 10 && enemy->type == 0x2d) break;
            t->target = thing_index(enemy);
            thing_set_state(t, (uint8_t)(base + 2));
            break;
        }
        // a flock leader, as creature_ai_step does
        best_d = kFarSq;
        Thing *leader = nullptr;
        for (uint32_t i = g_cfg->creature_lists[(int8_t)t->type]; i != 0; i = thing_at(i)->next) {
            Thing *o = thing_at(i);
            if (o->parent != 0 || o == t) continue;
            uint64_t d = dist_sq_xy16(o, t);
            if (d > limit) continue;
            int ang = pos_angle_to(thing_pos(t), thing_pos(o)) & 0xffff;
            if ((angle_diff(t->yaw, ang) & 0xffff) >= fov) continue;
            if (d >= best_d) continue;
            leader = o;
            best_d = d;
        }
        if (leader) {
            t->parent = thing_index(leader);
            thing_set_state(t, (uint8_t)(base + 3));
        }
        break;
    }
    }
    if (t->state == 0x1a) creature_set_attack_sprite(t);
}

// creature_archer_s26_update_1a630 (state 26 = base + 2): shoot arrows; every think tick the target
// wizard is marked wanted again.
void creature_archer_s26_update(Thing *t) {
    if (mode_target_sheltered(thing_at(t->target))) {       // port (mode runs): lets go of a wizard in its castle
        thing_set_state(t, 0x18 + 1);
        creature_set_move_sprite(t);
        return;
    }
    creature_attack_target(t, 0x18, creature_attack_arrow);
    if (t->state != 0x1a) {
        creature_set_move_sprite(t);
        return;
    }
    if (!think_now(t)) return;
    villager_blame(t->target);
}

// creature_archer_s27_update_1a6f0 (state 27 = base + 3)
void creature_archer_s27_update(Thing *t) {
    creature_follow_leader(t, 0x18);
    if (t->state == 0x1a) creature_set_attack_sprite(t);
}

// creature_archer_s28_update_1a720 (state 28 = base + 4)
void creature_archer_s28_update(Thing *t) {
    if (t->aux != 0) {
        thing_mark_delete(t);
        return;
    }
    creature_die(t, 0x18);
}

void creature_archer_s29_update(Thing *t) { creature_dead_drop_mana(t); }      // creature_archer_s29_update_1a750
// creature_archer_s30_update_1a820 (state 30 = crab base + 0): straight on to state 31
void creature_crab_s30_update(Thing *t)   { thing_set_state(t, 0x1f); }

// ---- skeleton (type 9, base 0x36) ----------------------------------------------------------------
// aux (+0x1a): state 54 rise countdown; state 55 countdown to the summoning pose (400, held at 400
// while awake; negative = pose cool-down counting up). castle_size (+0x47): 1 = summoning pose.

// skeleton_set_attack_sprite_1c7e0: entering the attack state; a target of the own side sends it
// back to state 55.
void skeleton_set_attack_sprite(Thing *t) {
    const Thing *target = thing_at(t->target);
    if (same_side(t->owner, target->owner)) {
        thing_set_state(t, 0x37);
        return;
    }
    t->speed_cur = 0;
    thing_set_sprite(t, 0xca);
    t->filter_cls = target->cls;
    t->filter_type = target->type;
}

// skeleton_set_idle_1c860
void skeleton_set_idle(Thing *t) {
    t->speed_cur = t->speed_base;
    thing_set_sprite(t, 0xc9);
    t->filter_cls = 3;
    t->filter_type = 0xff;
    t->aux = 0x32;
    t->castle_size = 0;
}

// skeleton_start_convert_pose_1c8a0
void skeleton_start_convert_pose(Thing *t) {
    thing_set_sprite(t, 0xf5);
    t->castle_size = 1;
}

// skeleton_reset_convert_timer_1c8c0
void skeleton_reset_convert_timer(Thing *t) {
    t->castle_size = 0;
    t->aux = 0x190;
    thing_set_sprite(t, 0xc9);
}

// The victim search 1bb70 and 1c1e0 share: every think tick one of the lists Archer (4), Builder
// (0xc), Townie (0xd) in turn, nearest within the sight radius (any owner, no field of view).
Thing *skeleton_find_villager(const Thing *t) {
    int period = (int16_t)desc_of(t)->think_period;     // callers checked tick % period == 0
    static const uint8_t kLists[3] = { 4, 0xc, 0xd };   // cfg+0x8e2e, +0x8e4e, +0x8e52
    int k = ((int)t->tick / period) % 3;
    return nearest_in_list(g_cfg->creature_lists[kLists[k]], t, sight_sq(t), [](const Thing *) { return true; });
}

// creature_griffon_s54_update_1bb00 (state 54 = skeleton base + 0, the state the constructor sets):
// rising out of the ground. aux counts down; sprite 0xed at 0x10, one animation frame every second
// tick below that, then state 55.
void creature_skeleton_s54_update(Thing *t) {
    int16_t before = t->aux;
    int16_t now = (int16_t)(before - 1);
    t->aux = now;
    if (before == 0) {
        skeleton_set_idle(t);
        thing_set_state(t, 0x37);
        t->aux = 0x190;
        t->castle_size = 0;
        // port (mode runs): twice the health once it has risen (2000) - with arrows that find their mark the
        // archers of a world's towns shot every skeleton dead in a few minutes (user, 2026-10-10: the undead should
        // be a lasting threat)
        if (mode_active()) { t->max_health *= 2; t->health = t->max_health; }
        return;
    }
    if (now == 0x10) {
        thing_set_sprite(t, 0xed);
        return;
    }
    if (now < 0x10 && now % 2 == 0) thing_anim_advance(t);
}

// The body of 1bb70 for castle_size == 0 (0x1bbc1..0x1c1b2): walk, hunt enemy castles and wizards,
// convert villagers.
void skeleton_wander(Thing *t, int base) {
    if (t->timer_a != 0) t->aux = 0x190;
    switch (creature_apply_damage(t)) {
    case 2:
        thing_set_state(t, (uint8_t)(base + 4));
        return;
    case 1:
        t->target = t->last_attacker;           // whatever hit it (no class test here)
        thing_set_state(t, (uint8_t)(base + 2));
        return;
    default:
        break;
    }
    creature_move_step(t);
    if (!think_now(t)) return;

    // nearest castle of another owner anywhere on the map: walk toward it, attack inside
    // sight radius + castle half width
    Thing *castle = nearest_in_list(g_cfg->player_list, t, kFarSq,
                                    [t](const Thing *p) { return p->type == 2 && !same_side(p->owner, t->owner); });
    if (castle) {
        t->target_yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(castle));
        int range = (int16_t)desc_of(t)->sight_radius + (int)castle->ext_x;
        if ((uint32_t)pos_dist_xyz(thing_pos(t), thing_pos(castle)) <= (uint32_t)range) {
            t->target = thing_index(castle);
            thing_set_state(t, (uint8_t)(base + 2));
            return;
        }
    } else {
        creature_wander_turn(t);
    }

    if (t->timer_a != 0) {
        // any player-list thing of another owner in sight and in the field of view
        uint64_t limit = sight_sq(t);
        int fov = (int16_t)desc_of(t)->fov;
        uint64_t best_d = kFarSq;
        Thing *best = nullptr;
        for (uint32_t i = g_cfg->player_list; i != 0; i = thing_at(i)->next) {
            Thing *p = thing_at(i);
            if (same_side(p->owner, t->owner)) continue;
            uint64_t d = dist_sq_xy16(p, t);
            if (d > limit) continue;
            if (p->flags & 0x20) continue;
            int ang = pos_angle_to(thing_pos(t), thing_pos(p)) & 0xffff;
            if ((angle_diff(t->yaw, ang) & 0xffff) >= fov) continue;
            if (d >= best_d) continue;
            best = p;
            best_d = d;
        }
        if (best) {
            t->target = thing_index(best);
            thing_set_state(t, (uint8_t)(base + 2));
            return;
        }
    }

    Thing *victim = skeleton_find_villager(t);
    if (!victim) return;
    if ((uint32_t)pos_dist_xyz(thing_pos(t), thing_pos(victim)) > 0x600u) return;
    g_pos_scratch = *thing_pos(victim);
    thing_mark_delete(victim);
    Thing *n = thing_create(&g_pos_scratch, 5, 9);
    // the new skeleton joins the owner only when that is a player thing (1c1e0 copies it always)
    if (n && thing_at((uint16_t)(int16_t)t->owner)->cls == 3) n->owner = t->owner;
}

// creature_skeleton_s55_update_1bb70 (state 55 = base + 1)
void creature_skeleton_s55_update(Thing *t) {
    const int base = 0x36;
    if (t->aux > 0) {
        t->aux--;
        if (t->aux == 0) skeleton_start_convert_pose(t);
    }
    if (t->castle_size == 0)      skeleton_wander(t, base);
    else if (t->castle_size == 1) skeleton_convert_villager(t);
    if (t->state == 0x38) skeleton_set_attack_sprite(t);
}

// creature_skeleton_s56_attack_1c570 (state 56 = base + 2)
void creature_skeleton_s56_update(Thing *t) {
    switch (creature_apply_damage(t)) {
    case 2:
        thing_set_state(t, 0x3a);
        break;
    case 1:
        if (thing_at(t->last_attacker)->cls == 3) t->target = t->last_attacker;
        break;
    default: {
        creature_move_step(t);
        Thing *target = thing_at(t->target);
        if (target->health < 0 || (target->flags & 0x400)) {
            thing_set_state(t, 0x37);
            break;
        }
        if (t->tick % 10 == 0) t->target_yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(target));
        if (!think_now(t)) break;
        int range = (int16_t)desc_of(t)->sight_radius;
        if (target->cls == 3 && target->type == 2) range += target->ext_x;     // castles: from the wall
        if ((uint32_t)pos_dist_xyz(thing_pos(t), thing_pos(target)) >= (uint32_t)range) {
            // port (mode runs): out of range of a creature that hit it (an archer shoots from 20 cells, a skeleton
            // from 8) it walks up to it for as long as it is within 24 cells, instead of going back to wandering
            // under the arrows - the archers shot every skeleton of a world dead without loss (user, 2026-10-10:
            // the undead should be a lasting threat)
            if (mode_active() && target->cls == 5 && (uint32_t)pos_dist_xyz(thing_pos(t), thing_pos(target)) < 0x1800u) {
                t->target_yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(target));
                t->speed_cur = t->speed_base;
                return;                                 // (stays in the attack state, keeps its marching speed)
            }
            thing_set_state(t, 0x37);
        } else {
            if (mode_active()) t->speed_cur = 0;        // (it stood still to shoot before the port's approach)
            skeleton_attack_fire(t, target);
        }
        break;
    }
    }
    if (t->state != 0x38) skeleton_set_idle(t);
}

// creature_skeleton_s57_update_1c790 (state 57 = base + 3)
void creature_skeleton_s57_update(Thing *t) {
    creature_follow_leader(t, 0x36);
    if (t->state == 0x38) skeleton_set_attack_sprite(t);
}

void creature_skeleton_s58_update(Thing *t) { creature_die(t, 0x36); }          // creature_skeleton_s58_update_1c7c0
void creature_skeleton_s59_update(Thing *t) { creature_dead_drop_mana(t); }     // creature_skeleton_s59_update_1c7d0
// creature_skeleton_s60_update_1c8e0 (state 60 = emu base + 0)
void creature_emu_s60_update(Thing *t)      { creature_idle_seek_leader(t, 0x3c); }

// ---- villagers: builder (type 0xc, base 0x48), townie (0xd, 0x4e), trader (0xe, 0x54) -------------

// The inlined "who did that" of the villagers (0x1dacb, 0x1dd23, 0x1df8d, 0x1e0b5, 0x1e252, 0x1e517,
// 0x1e6d2, 0x1e9c5): when the attacker / killer thing has type 0 or 1 (a wizard) its player block
// gets P+0x210 = 200. The class is not tested.
void villager_blame(uint16_t thing) {
    const Thing *who = thing_at(thing);
    if (who->type == 0 || who->type == 1) owner_block(who)->timer210 = 200;
}

// creature_builder_s73_update_1d9d0 (state 73 = base + 1): wander for aux think ticks, then look for
// a wizard (state 75).
void creature_builder_s73_update(Thing *t) {
    const int base = 0x48;
    switch (creature_apply_damage(t)) {
    case 2:
        thing_set_state(t, (uint8_t)(base + 4));
        return;
    case 1:
        villager_blame(t->last_attacker);
        return;
    default:
        break;
    }
    creature_move_step(t);
    if (!think_now(t)) return;
    creature_wander_turn(t);
    int16_t left = t->aux;
    t->aux = (int16_t)(left - 1);
    if (left != 0) return;
    t->aux = 1;
    thing_set_state(t, (uint8_t)(base + 3));
}

// creature_builder_s74_update_1dc20 (state 74 = base + 2): walk to the chosen wizard; within 0xa00 ->
// state 72 (place a castle); gives up after aux think ticks.
void creature_builder_s74_update(Thing *t) {
    const int base = 0x48;
    switch (creature_apply_damage(t)) {
    case 2:
        thing_set_state(t, (uint8_t)(base + 4));
        return;
    case 1:
        villager_blame(t->last_attacker);
        return;
    default:
        break;
    }
    creature_move_step(t);
    int period = (int16_t)desc_of(t)->think_period;
    if (period == 0 || ((int)t->tick % period) / 2 != 0) return;     // remainder 0 or 1: two ticks in a row
    Thing *target = thing_at(t->target);
    int16_t left = t->aux;
    t->aux = (int16_t)(left - 1);
    if (left == 0 || target->cls == 0) {
        t->aux = 5;
        thing_set_state(t, (uint8_t)(base + 1));
        // falls through to the aim / arrival test
    }
    t->target_yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(target));
    if ((uint32_t)pos_dist_xyz(thing_pos(t), thing_pos(target)) < 0xa00u) {
        t->aux = 0;
        thing_set_state(t, (uint8_t)base);
    }
}

// creature_builder_s75_update_1de90 (state 75 = base + 3): pick the nearest wizard (effect 0x2d, 3D
// distance, not one it stands on) -> state 74 with 10 think ticks, else back to wandering.
void creature_builder_s75_update(Thing *t) {
    const int base = 0x48;
    switch (creature_apply_damage(t)) {
    case 2:
        thing_set_state(t, (uint8_t)(base + 4));
        return;
    case 1:
        villager_blame(t->last_attacker);
        return;
    default:
        break;
    }
    uint32_t best_d = 0xffffffffu;
    Thing *best = nullptr;
    for (uint32_t i = g_cfg->wizard_list; i != 0; i = thing_at(i)->next) {
        Thing *w = thing_at(i);
        uint32_t d = (uint32_t)pos_dist_xyz(thing_pos(t), thing_pos(w));
        if (d == 0 || d >= best_d) continue;
        best = w;
        best_d = d;
    }
    if (best) {
        t->target = thing_index(best);
        t->aux = 10;
        thing_set_state(t, (uint8_t)(base + 2));
    } else {
        t->aux = 5;
        thing_set_state(t, (uint8_t)(base + 1));
    }
}

void creature_builder_s76_update(Thing *t) { creature_die(t, 0x48); }      // creature_builder_s76_update_1e0a0

// creature_builder_s77_update_1e0b0
void creature_builder_s77_update(Thing *t) {
    villager_blame(t->killer);
    creature_dead_drop_mana(t);
}

// creature_genie_s72_place_castle_1d540 (state 72 = builder base + 0): found a new wizard castle next
// to the target wizard. aux counts the attempts: 0..3 = east / west / south / north of the wizard
// (by the half extents of both castles plus 1..3 cells, and -5..-3 cells along the other axis); the
// site must not be water (type mask 1), must be flat and must not touch a wizard or a castle. A
// site that is not flat or is occupied is retried on the next tick with the next direction; water
// and the fifth attempt send the builder back to wandering. After founding the castle the builder
// continues as a townie (state 0x4f) and walks into a wizard castle.
void creature_builder_s72_update(Thing *t) {
    const int base = 0x48;
    Thing *wizard = thing_at(t->target);
    bool give_up = false;
    if (wizard->cls == 0 || wizard->type != 0x2d) {
        t->aux = 5;
        give_up = true;
    } else {
        g_pos_scratch = *thing_pos(wizard);
        int16_t attempt = t->aux;
        t->aux = (int16_t)(attempt + 1);
        if (attempt >= 4) {
            t->aux = 1;
            give_up = true;
        } else {
            t->rng = mc_lcg(t->rng);
            uint8_t size = (uint8_t)((t->rng & 7) + 0x19);
            uint16_t ext_x, ext_y;
            castle_size_half_extents(size, &ext_x, &ext_y);
            auto rnd3 = [t]() { t->rng = mc_lcg(t->rng); return (int)(t->rng % 3u); };
            // (the original's 16-bit sums; the WCoord stores wrap them at the world's width)
            switch ((uint16_t)attempt) {        // jump table 0x1d524
            case 0: {
                int a = rnd3();
                g_pos_scratch.x = g_pos_scratch.x + ((int)ext_x + wizard->ext_x + (a << 8) + 0x100);
                int b = rnd3();
                g_pos_scratch.y = g_pos_scratch.y + ((b << 8) - 0x500);
                break;
            }
            case 1: {
                int a = rnd3();
                g_pos_scratch.x = g_pos_scratch.x - ((int)ext_x + wizard->ext_x + (a << 8) + 0x100);
                int b = rnd3();
                g_pos_scratch.y = g_pos_scratch.y + ((b << 8) - 0x500);
                break;
            }
            case 2: {
                int a = rnd3();
                g_pos_scratch.x = g_pos_scratch.x + ((a << 8) - 0x500);
                int b = rnd3();
                g_pos_scratch.y = g_pos_scratch.y + ((int)ext_y + wizard->ext_y + (b << 8) + 0x100);
                break;
            }
            case 3: {
                int a = rnd3();
                g_pos_scratch.x = g_pos_scratch.x + ((a << 8) - 0x500);
                int b = rnd3();
                g_pos_scratch.y = g_pos_scratch.y - ((int)wizard->ext_y + ext_y + (b << 8) + 0x100);
                break;
            }
            default:
                break;                          // negative aux: the wizard's own position
            }
            const int32_t sx = g_pos_scratch.x, sy = g_pos_scratch.y;
            if (terrain_type_mask_at(&g_pos_scratch) == 1) {
                t->aux = 2;
                give_up = true;
            } else {
                unsigned max_diff = ((ext_x >> 7) + (ext_y >> 7) > 4 ? 1u : 0u) + 0xf;
                // port (mode runs): three times the slope - a generated world has little ground as flat as the
                // original's hand-made town sites, and the builders gave up (user, 2026-10-10: the towns do not
                // spread); the new building levels its own footprint
                if (mode_active()) max_diff *= 3;
                if (!terrain_rect_is_flat(&g_pos_scratch, ext_x >> 8, ext_y >> 8, max_diff)) return;
                auto touches = [&](const Thing *o) {
                    int dx = world_signed(o->x - sx);
                    if (dx < 0) dx = -dx;
                    if (dx > (int)ext_x + o->ext_x) return false;
                    int dy = world_signed(o->y - sy);
                    if (dy < 0) dy = -dy;
                    return dy <= (int)o->ext_y + ext_y;
                };
                for (uint32_t i = g_cfg->wizard_list; i != 0; i = thing_at(i)->next)
                    if (touches(thing_at(i))) return;
                for (uint32_t i = g_cfg->player_list; i != 0; i = thing_at(i)->next)
                    if (thing_at(i)->type == 2 && touches(thing_at(i))) return;
                // port (mode runs): the towns spread by one building every 30 s over the four a seat they start
                // with, up to one per 64 x 64 cells of world and 96 at most (the original's stop at its 1000
                // Things); a builder with no room wanders on and tries again
                bool room = true;
                if (mode_active()) {
                    int buildings = 0;
                    for (uint32_t i = g_cfg->wizard_list; i != 0; i = thing_at(i)->next) buildings++;
                    const int cells = g_world.size / 64, most = cells * cells < 96 ? cells * cells : 96;
                    const int now = 4 * (int)(g_mode.params.bots + g_mode.params.humans) + (int)(g_mode.tick / (30 * MODE_TICKS_PER_SECOND));
                    room = buildings < (now < most ? now : most);
                }
                if (!room) {
                    t->aux = 5;
                    t->target = 0;
                    thing_set_state(t, (uint8_t)(base + 1));
                    return;
                }
                Thing *n = thing_create(&g_pos_scratch, 10, 0x2d);
                if (n) {
                    creature_sound(t, 10);
                    effect_wizard_init(n, size);
                    n->state = 0x33;
                }
                t->target = 0;
                thing_set_state(t, 0x4f);
                return;
            }
        }
    }
    if (give_up) {
        t->target = 0;
        thing_set_state(t, (uint8_t)(base + 1));
    }
}

// creature_builder_s78_update_1e130 (state 78 = townie base + 0), creature_update_shared_1e4f0
// (80, 81), creature_townie_s84_update_1e5b0 (84 = trader base + 0), creature_update_shared_1e980
// (86, 87): a lone `ret`.
void creature_noop_update(Thing *) {}

// The body creature_townie_s79_update_1e140 and creature_trader_s85_update_1e5c0 share: wander until
// a wizard castle is chosen (the nearest; the trader: the nearest farther than 0x3c00), walk to it
// and enter it (state base + 4 with aux = 1 = "arrived"; the wizard's head count aux goes up) while
// it has room (wizard.speed_base = capacity). A full or vanished castle is forgotten and the
// villager strolls on at turn_rate speed.
void villager_walk_to_wizard(Thing *t, int base, uint32_t min_dist_sq) {
    switch (creature_apply_damage(t)) {
    case 2:
        thing_set_state(t, (uint8_t)(base + 4));
        return;
    case 1:
        villager_blame(t->last_attacker);
        return;
    default:
        break;
    }
    creature_move_step(t);
    if (!think_now(t)) return;
    if (t->target == 0) {
        creature_wander_turn(t);
        if (mode_active() && min_dist_sq == 0) {
            // port (mode runs): a townie strolls for a few think ticks, then walks to one of the buildings within 56
            // cells picked at random. The original's always takes the nearest; with every building full (a
            // generated world's are) the whole town milled about the one castle (user, 2026-10-11).
            t->rng = mc_lcg(t->rng);
            if (t->rng % 4u != 0) return;
            constexpr uint64_t kNear = (uint64_t)0x3800 * 0x3800;
            int n = 0;
            for (uint32_t i = g_cfg->wizard_list; i != 0; i = thing_at(i)->next) n += dist_sq_xy16(thing_at(i), t) < kNear;
            if (n > 0) {
                t->rng = mc_lcg(t->rng);
                int pick = (int)(t->rng % (uint32_t)n);
                for (uint32_t i = g_cfg->wizard_list; i != 0; i = thing_at(i)->next) {
                    if (dist_sq_xy16(thing_at(i), t) >= kNear || pick-- != 0) continue;
                    t->target = (uint16_t)i;
                    t->speed_cur = t->speed_base;
                    return;
                }
            }
        }
        uint64_t best_d = kFarSq;
        Thing *best = nullptr;
        for (uint32_t i = g_cfg->wizard_list; i != 0; i = thing_at(i)->next) {
            Thing *w = thing_at(i);
            uint64_t d = dist_sq_xy16(w, t);
            if (d >= best_d) continue;
            if (min_dist_sq != 0 && d <= min_dist_sq) continue;
            best = w;
            best_d = d;
        }
        if (!best) return;
        t->target = thing_index(best);
        t->speed_cur = t->speed_base;
        return;
    }
    Thing *wizard = thing_at(t->target);
    if (wizard->cls == 10 && wizard->type == 0x2d) {
        if ((uint32_t)pos_dist_xyz(thing_pos(t), thing_pos(wizard)) > 0x800u) {
            t->target_yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(wizard));
            return;
        }
        if (wizard->speed_base > wizard->aux) {
            thing_set_state(t, (uint8_t)(base + 4));
            t->aux = 1;
            wizard->aux++;
            return;
        }
    }
    t->target = 0;
    t->speed_cur = (int16_t)t->turn_rate;
}

// creature_townie_s79_update_1e140 (state 79 = base + 1)
void creature_townie_s79_update(Thing *t) { villager_walk_to_wizard(t, 0x4e, 0); }

// creature_townie_s82_update_1e500 (state 82 = base + 4): arrived (aux != 0) -> just vanish; killed ->
// blame the killer and die.
void creature_townie_s82_update(Thing *t) {
    if (t->aux != 0) {
        thing_mark_delete(t);
        return;
    }
    villager_blame(t->killer);
    creature_die(t, 0x4e);
}

void creature_townie_s83_update(Thing *t) { creature_dead_drop_mana(t); }      // creature_townie_s83_update_1e5a0

// creature_trader_s85_update_1e5c0 (state 85 = base + 1)
void creature_trader_s85_update(Thing *t) { villager_walk_to_wizard(t, 0x54, 0xe100000u); }

// creature_trader_s88_update_1e990 (state 88 = base + 4)
void creature_trader_s88_update(Thing *t) {
    if (t->aux != 0) {
        thing_mark_delete(t);
        return;
    }
    creature_die(t, 0x54);
}

// creature_trader_s89_update_1e9c0 (state 89 = base + 5)
void creature_trader_s89_update(Thing *t) {
    villager_blame(t->killer);
    creature_dead_drop_mana(t);
}

// creature_trader_s90_update_1ea50 (state 90 = type 15 base + 0)
void creature_type15_s90_update(Thing *t) { creature_idle_seek_leader(t, 0x5a); }

} // namespace

// ---- exported helpers ----------------------------------------------------------------------------

// skeleton_convert_villager_1c1e0
void skeleton_convert_villager(Thing *t) {
    switch (creature_apply_damage(t)) {
    case 2:
        thing_set_state(t, 0x3a);
        return;
    case 1:
        t->target = t->last_attacker;
        thing_set_state(t, 0x38);
        return;
    default:
        break;
    }
    if (t->aux < 0) {                           // cool-down after an interrupted pose
        t->aux++;
        if (t->aux == 0) skeleton_reset_convert_timer(t);
        return;
    }
    if (t->timer_a != 0) {                      // somebody is near: break the pose off
        t->aux = -50;
        return;
    }
    if (!think_now(t)) return;
    Thing *victim = skeleton_find_villager(t);
    if (!victim) return;
    int32_t dx = world_signed(victim->x - t->x);
    int32_t dy = world_signed(victim->y - t->y);
    int32_t dz = (int16_t)(victim->z - t->z);
    if (world_isqrt(world_dist_sq3(dx, dy, dz)) > 0x600u) return;
    g_pos_scratch = *thing_pos(victim);
    thing_mark_delete(victim);
    Thing *n = thing_create(&g_pos_scratch, 5, 9);
    if (n) n->owner = t->owner;
}

// terrain_rect_is_flat_1d420
int terrain_rect_is_flat(const Pos *pos, unsigned w, unsigned h, unsigned max_diff) {
    // (cell coordinates wrap at the world's width; the original's uint8_t arithmetic with 256 cells)
    int x = world_cx(pos->x), y = world_cx(pos->y);
    x = world_wrap_cell(x - (int)((w & 0xffff) >> 1));
    y = world_wrap_cell(y - (int)((h & 0xffff) >> 1));
    if (((unsigned)x + (unsigned)y) % 2 != 0) x = world_wrap_cell(x + 1);
    int range = (int16_t)terrain_rect_height_range(x, y, (int)(h & 0xffff), (int)(w & 0xffff));
    return range < (int)(max_diff & 0xffff) ? 1 : 0;
}

// castle_size_half_extents_1d4b0
void castle_size_half_extents(unsigned size, uint16_t *ext_x, uint16_t *ext_y) {
    const CastleFootprint *fp = castle_footprint(size & 0xffff);
    uint16_t h = fp->h, w = fp->w;
    if (g_video_mode_flags == 1) { h >>= 1; w >>= 1; }
    *ext_y = (uint16_t)((((uint32_t)h << 8) >> 1) + 0x300);
    *ext_x = (uint16_t)((((uint32_t)w << 8) >> 1) + 0x300);
}

// ---- registration --------------------------------------------------------------------------------

void creatures_register_handlers() {
    g_hook_creature_wake_tick = creature_wake_tick;                 // creature_wake_tick_468e0

    thing_register_update(0x18050, creature_segment_update);        // state 120: body segments

    thing_register_update(0x19b70, creature_dragon_s0_update);      // dragon 0..5
    thing_register_update(0x19b80, creature_dragon_s1_update);
    thing_register_update(0x19ba0, creature_dragon_s2_update);
    thing_register_update(0x19bf0, creature_dragon_s3_update);
    thing_register_update(0x19c10, creature_dragon_s4_update);
    thing_register_update(0x19c20, creature_dragon_s5_update);
    thing_register_update(0x19c70, creature_vulture_s6_update);     // vulture 6..11
    thing_register_update(0x19d10, creature_vulture_s7_update);
    thing_register_update(0x19de0, creature_vulture_s8_update);
    thing_register_update(0x19e30, creature_vulture_s9_update);
    thing_register_update(0x19e40, creature_vulture_s10_update);
    thing_register_update(0x19e50, creature_vulture_s11_update);
    thing_register_update(0x19e60, creature_bee_s12_update);        // bee 12..17
    thing_register_update(0x19e80, creature_bee_s13_update);
    thing_register_update(0x19ed0, creature_bee_s14_update);
    thing_register_update(0x19fd0, creature_bee_s15_update);
    thing_register_update(0x19ff0, creature_bee_s16_update);
    thing_register_update(0x1a000, creature_bee_s17_update);
    thing_register_update(0x1a010, creature_worm_s18_update);       // worm 18..23
    thing_register_update(0x1a020, creature_worm_s19_update);
    thing_register_update(0x1a030, creature_worm_s20_update);
    thing_register_update(0x1a080, creature_worm_s21_update);
    thing_register_update(0x1a090, creature_worm_s22_update);
    thing_register_update(0x1a0a0, creature_worm_s23_update);
    thing_register_update(0x1a0b0, creature_archer_s24_update);     // archer 24..29
    thing_register_update(0x1a0e0, creature_archer_s25_update);
    thing_register_update(0x1a630, creature_archer_s26_update);
    thing_register_update(0x1a6f0, creature_archer_s27_update);
    thing_register_update(0x1a720, creature_archer_s28_update);
    thing_register_update(0x1a750, creature_archer_s29_update);
    thing_register_update(0x1a820, creature_crab_s30_update);       // crab 30

    thing_register_update(0x1bb00, creature_skeleton_s54_update);   // skeleton 54..59
    thing_register_update(0x1bb70, creature_skeleton_s55_update);
    thing_register_update(0x1c570, creature_skeleton_s56_update);
    thing_register_update(0x1c790, creature_skeleton_s57_update);
    thing_register_update(0x1c7c0, creature_skeleton_s58_update);
    thing_register_update(0x1c7d0, creature_skeleton_s59_update);
    thing_register_update(0x1c8e0, creature_emu_s60_update);        // emu 60

    thing_register_update(0x1d540, creature_builder_s72_update);    // builder 72..77
    thing_register_update(0x1d9d0, creature_builder_s73_update);
    thing_register_update(0x1dc20, creature_builder_s74_update);
    thing_register_update(0x1de90, creature_builder_s75_update);
    thing_register_update(0x1e0a0, creature_builder_s76_update);
    thing_register_update(0x1e0b0, creature_builder_s77_update);
    thing_register_update(0x1e130, creature_noop_update);           // townie 78..83
    thing_register_update(0x1e140, creature_townie_s79_update);
    thing_register_update(0x1e4f0, creature_noop_update);
    thing_register_update(0x1e500, creature_townie_s82_update);
    thing_register_update(0x1e5a0, creature_townie_s83_update);
    thing_register_update(0x1e5b0, creature_noop_update);           // trader 84..89
    thing_register_update(0x1e5c0, creature_trader_s85_update);
    thing_register_update(0x1e980, creature_noop_update);
    thing_register_update(0x1e990, creature_trader_s88_update);
    thing_register_update(0x1e9c0, creature_trader_s89_update);
    thing_register_update(0x1ea50, creature_type15_s90_update);     // type 15: 90
}
