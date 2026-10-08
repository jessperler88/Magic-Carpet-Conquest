// Projectiles of carpet.exe (class 9, 0x43f30..0x468e0): flight, steering, impact, target selection,
// line of fire. Translated from the disassembly (addresses in the comments); see
// docs/analysis/port_projectiles.md.
#include "settings.h"
#include "projectiles.h"
#include "world_set.h"
#include "level_features.h"   // castle_site_clear_at_pos_11be0
#include "player.h"           // player_block, player_set_combat_music_timer_40b50
#include "mc_math.h"

uint16_t g_projectile_null_hit_index = 0;

namespace {

// The original computes v / |v| (cdq / xor / sub / idiv) with 0 for 0.
inline int isign(int v) { return v == 0 ? 0 : v / (v < 0 ? -v : v); }

// &things[index] as the original forms it (base + index * 0xa4, no range check). An index outside
// the pool is memory the port does not have: it reads as the scratch Thing 0.
inline Thing *thing_ref(int index) {
    return thing_at((unsigned)index < (unsigned)thing_pool_slots() ? (unsigned)index : 0u);
}
// "pointer > &things[0]" as the handlers test a Thing reference for "there is one".
inline bool thing_is_set(const Thing *t) { return t != nullptr && thing_index(t) != 0; }

// (hit - things) / 0xa4 stored into a 16-bit field; see g_projectile_null_hit_index for NULL.
inline uint16_t hit_index(const Thing *hit) {
    return hit ? thing_index(hit) : g_projectile_null_hit_index;
}

// The two angle_turn_step calls that end 43f30 / 43ff0 and sit in the middle of 45530: yaw toward
// target_yaw by at most MoveDesc+2, pitch toward target_pitch by at most MoveDesc+6 (the third
// argument, MoveDesc+4 / +8, is pushed but never read by 3e800).
void turn_to_target_angles(Thing *t) {
    const MoveDesc *d = mc_move_desc(t->desc);
    t->yaw = (uint16_t)((t->yaw + angle_turn_step(t->yaw, t->target_yaw, d->unk4, d->turn_min)) & 0x7ff);
    d = mc_move_desc(t->desc);
    t->pitch = (uint16_t)((t->pitch + angle_turn_step(t->pitch, t->target_pitch, d->unk8, d->turn_max)) & 0x7ff);
}

// speed_cur moves toward speed_base by 2 per tick (sign(base - cur) * 2, so it never settles when
// the difference is odd).
void accelerate(Thing *t) {
    int d = (int)t->speed_base - (int)t->speed_cur;
    t->speed_cur = (int16_t)(t->speed_cur + isign(d) * 2);
}

// The first-tick automatic aim most handlers share: snap to the chosen target, or record the
// launch direction as the target direction when there is none.
void first_tick_aim(Thing *t) {
    if (t->flags & 2) return;
    t->flags |= 2;
    if (projectile_pick_target(t)) {
        t->yaw = t->target_yaw;
        t->pitch = t->target_pitch;
    } else {
        t->target_yaw = t->yaw;
        t->target_pitch = t->pitch;
    }
}

// Rebound (caster flag 0x8000): the projectile is thrown back at its owner. Identical in 44150,
// 44510, 44aa0 and 457a0 except for the random yaw spread (rng % spread - spread / 2).
void reflect(Thing *t, Thing *hit, unsigned spread) {
    sound_request((int16_t)thing_index(hit), -1, 0x1c);
    uint16_t old_owner = t->owner;                  // edi = &things[(i16)owner], turned back into an index below
    hit->mana -= t->mana / 4;
    t->target_yaw = (uint16_t)((t->yaw + 0x400) & 0x7ff);
    int dir = angle_turn_dir(0, t->pitch);
    int diff = angle_diff(0, t->pitch);
    uint16_t pitch = (uint16_t)((-(diff * dir)) & 0x7ff);
    t->target_pitch = pitch;
    t->pitch = pitch;
    t->rng = mc_lcg(t->rng);
    t->yaw = (uint16_t)(t->target_yaw + t->rng % spread - spread / 2);   // not masked
    t->target = old_owner;
    t->owner = hit->owner;
    t->health = t->max_health;
    g_pos_scratch = *thing_pos(hit);
    g_pos_scratch.z = (int16_t)(g_pos_scratch.z + hit->ext_h);
    thing_move_to(t, &g_pos_scratch);
}

// Ground contact of the flying kinds: below the terrain a projectile (except type 4) that came down
// on water leaves a splash (effect 5) and no impact effect; elsewhere it explodes. Above the ground
// its lifetime runs down. Returns true when it must explode.
bool ground_or_expire(Thing *t) {
    int h = terrain_height_at(thing_pos(t));
    g_pos_scratch.z = (int16_t)h;
    if ((int16_t)h > t->z) {
        if (t->type == 4) return true;
        if (terrain_type_mask_at(thing_pos(t)) != 1) return true;
        Thing *splash = thing_create(thing_pos(t), 10, 5);
        if (splash) splash->owner = t->owner;
        thing_mark_delete(t);
        return false;
    }
    return --t->health < 0;
}

// The part 44150, 44aa0 and 457a0 have in common, instruction for instruction: steer / first-tick
// aim, accelerate, move, collide (rebound or stop in the middle of the Thing hit), ground, lifetime.
// Returns true when the caller's impact tail must run; `hit` is the Thing collided with (null for
// ground / lifetime), `target` is &things[t->target] as it was on entry.
bool fly(Thing *t, Thing *&hit, Thing *&target) {
    target = thing_ref(t->target);
    hit = nullptr;
    if (thing_is_set(target)) projectile_steer_to_target(t, target);
    else                      first_tick_aim(t);
    accelerate(t);
    g_pos_scratch = *thing_pos(t);
    math_rotate_offset(&g_pos_scratch, t->yaw, t->pitch, t->speed_cur);
    thing_move_to(t, &g_pos_scratch);
    hit = thing_find_collision(t);
    if (hit) {
        if ((hit->flags & 0x8000) && t->mana / 4 <= hit->mana && t->impact_cls == 10 &&
            (t->impact_type == 1 || t->impact_type == 0x11 || (t->impact_type == 0x35 && world_hidden()))) {   // HW: + 0x35
            reflect(t, hit, 0x2d);
            return false;
        }
        thing_z_add_half_height(hit);
        thing_move_to(t, thing_pos(hit));
        thing_z_sub_half_height(hit);
        return true;
    }
    return ground_or_expire(t);
}

// cos / sin in the score functions: DAT_00098fec[i] and DAT_000987ec[i] indexed with an angle
// difference (0..0x400).
uint32_t aim_score(unsigned dyaw, unsigned dpitch, int32_t dist) {
    int32_t cy = mc_trig_raw(dyaw + 0x200) * dist;
    int32_t sy = mc_trig_raw(dyaw) * dist;
    int32_t cp = mc_trig_raw(dpitch + 0x200) * dist;
    int32_t sp = dist * mc_trig_raw(dpitch);
    cy >>= 16;
    sy = (int32_t)((uint32_t)sy << 2) >> 16;
    cp >>= 16;
    sp = (int32_t)((uint32_t)sp << 2) >> 16;
    return (uint32_t)(sp * sp) + (uint32_t)(cy * cy) + (uint32_t)(sy * sy) + (uint32_t)(cp * cp);
}

}  // namespace

// ---- steering ------------------------------------------------------------------------------------

// projectile_steer_to_target_43f30
void projectile_steer_to_target(Thing *t, Thing *target) {
    thing_z_add_half_height(target);
    t->target_yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(target));
    t->target_pitch = (uint16_t)pos_pitch_to(thing_pos(t), thing_pos(target));
    turn_to_target_angles(t);
    thing_z_sub_half_height(target);
}

// thing_turn_toward_43ff0
void thing_turn_toward(Thing *a, Thing *b) {
    a->target_yaw = (uint16_t)pos_angle_to(thing_pos(a), thing_pos(b));
    a->target_pitch = (uint16_t)pos_pitch_to(thing_pos(a), thing_pos(b));
    turn_to_target_angles(a);
}

// projectile_record_hit_stats_440a0
void projectile_record_hit_stats(Thing *t, Thing *hit, Thing *target) {
    switch (t->type) {
    case 0: case 1: case 3: case 7: case 8: case 9: case 0x13: break;
    case 0x10: if (world_hidden()) break; return;           // Hidden Worlds (HIDDEN.EXE 0x52a00) counts Fire Wall shots
    default: return;
    }
    Thing *owner = thing_ref((int16_t)t->owner);
    if (owner->cls != 3 || owner->type != 0) return;
    player_block(owner)->shots++;
    if (!thing_is_set(hit)) return;
    if (!thing_is_set(target)) return;
    if (hit->owner != target->owner) return;
    // (the original tests the owner's P pointer for NULL here; it never is: thing_alloc points it at
    // the dummy block)
    player_block(owner)->hits++;
}

// ---- flight --------------------------------------------------------------------------------------

// projectile_fly_and_impact_44150
void projectile_fly_and_impact(Thing *t) {
    Thing *hit, *target;
    if (!fly(t, hit, target)) return;
    Thing *e = thing_create(thing_pos(t), (int8_t)t->impact_cls, (int8_t)t->impact_type);
    if (!e) return;
    projectile_record_hit_stats(t, hit, target);
    if (t->impact_type == 0x22) e->health = t->damage;        // teleport: lifetime = damage
    e->owner = t->owner;
    e->yaw = t->yaw;
    e->pitch = t->pitch;
    e->target = hit_index(hit);
    e->damage = t->damage;
    thing_mark_delete(t);
}

// projectile_type0_s0_update_44510
void projectile_type0_s0_update(Thing *t) {
    bool explode = false;
    Thing *target = thing_ref(t->target);
    if (thing_is_set(target)) {
        projectile_steer_to_target(t, target);
    } else if (!(t->flags & 2)) {
        t->flags |= 2;
        if (projectile_pick_target(t)) {
            // the yaw turns toward the target by at most 0x22; the pitch snaps
            int d = angle_diff(t->yaw, t->target_yaw);
            if ((int16_t)d < 0) d = 0;
            if ((int16_t)d > 0x22) d = 0x22;
            int dir = angle_turn_dir(t->yaw, t->target_yaw);
            t->yaw = (uint16_t)(t->yaw + dir * d);              // not masked
            t->pitch = t->target_pitch;
        } else {
            t->target_yaw = t->yaw;
            t->target_pitch = t->pitch;
        }
    }
    Pos before = *thing_pos(t);
    g_pos_scratch = before;
    math_rotate_offset(&g_pos_scratch, t->yaw, t->pitch, t->speed_cur);
    thing_move_to(t, &g_pos_scratch);
    Thing *hit = thing_find_collision(t);
    if (hit) {
        if (hit->flags & 0x8000) {
            // unlike 44150: a rebounding target too weak to reflect lets the fireball pass
            if (t->mana / 4 > hit->mana) return;
            reflect(t, hit, 0x5b);
            return;
        }
        thing_z_add_half_height(hit);
        thing_move_to(t, thing_pos(hit));
        thing_z_sub_half_height(hit);
        explode = true;
    } else {
        int h = terrain_height_at(thing_pos(t));
        g_pos_scratch.z = (int16_t)h;
        if ((int16_t)h > t->z) {
            thing_move_to(t, &before);                          // back out of the ground
            if (t->type == 4 || terrain_type_mask_at(thing_pos(t)) != 1) {
                explode = true;
            } else {
                Thing *splash = thing_create(thing_pos(t), 10, 5);
                if (splash) splash->owner = t->owner;
                thing_mark_delete(t);
            }
        } else if (--t->health < 0) {
            explode = true;
        }
    }
    if (!explode) return;
    Thing *e = thing_create(thing_pos(t), (int8_t)t->impact_cls, (int8_t)t->impact_type);
    if (!e) return;
    projectile_record_hit_stats(t, hit, target);
    e->owner = t->owner;
    e->yaw = t->yaw;
    e->pitch = t->pitch;
    thing_mark_delete(t);
}

// projectile_homing_update_448b0
void projectile_homing_update(Thing *t) {
    bool explode = false;
    Thing *target = thing_ref(t->target);
    if (thing_is_set(target)) {
        projectile_steer_to_target(t, target);
    } else if (!(t->flags & 2)) {
        t->flags |= 2;
        if (projectile_pick_target(t)) {
            t->yaw = t->target_yaw;
            t->pitch = t->target_pitch;
        }
    }
    g_pos_scratch = *thing_pos(t);
    math_rotate_offset(&g_pos_scratch, t->yaw, t->pitch, t->speed_cur);
    int h = terrain_height_at(&g_pos_scratch);
    if ((int16_t)h > g_pos_scratch.z) g_pos_scratch.z = (int16_t)h;
    thing_move_to(t, &g_pos_scratch);
    Thing *hit = thing_find_mana_near(t);
    if (hit) {
        thing_z_add_half_height(hit);
        thing_move_to(t, thing_pos(hit));
        thing_z_sub_half_height(hit);
        explode = true;
    } else {
        h = terrain_height_at(thing_pos(t));
        g_pos_scratch.z = (int16_t)h;
        if ((int16_t)h > t->z) explode = true;
        else if (--t->health < 0) explode = true;
    }
    if (!explode) return;
    Thing *e = thing_create(thing_pos(t), (int8_t)t->impact_cls, (int8_t)t->impact_type);
    if (!e) return;
    projectile_record_hit_stats(t, hit, target);
    e->owner = t->owner;
    e->yaw = t->yaw;
    e->pitch = t->pitch;
    thing_mark_delete(t);
}

// projectile_update_shared_44a40. Hidden Worlds points the class-9 record 0x11 (the Fire Wall shot,
// type 0x10) at a new handler (HIDDEN.EXE 0x54600): the same flight, then one explosion per tick at the
// projectile (flags 0x10080: no area damage of its own), including the impact tick.
void projectile_update_shared(Thing *t) {
    const bool fire_wall_trail = t->state == 0x11 && world_hidden();
    projectile_fly_and_impact(t);
    if (!fire_wall_trail || t->cls == 0) return;
    Thing *e = thing_create(thing_pos(t), 10, 0);
    if (!e) return;
    e->flags |= 0x10080u;
    e->owner = t->owner;
}

// projectile_type3_s3_update_44a50
void projectile_type3_s3_update(Thing *t) {
    projectile_fly_and_impact(t);
    if (t->cls == 0) return;
    Thing *e = thing_create(thing_pos(t), 10, 1);
    if (!e) return;
    e->flags |= 0x10080;
    e->owner = t->owner;
}

// projectile_type7_s7_update_44a90
void projectile_type7_s7_update(Thing *t) {
    projectile_type8_s8_update(t);
}

// projectile_type8_s8_update_44aa0
void projectile_type8_s8_update(Thing *t) {
    Thing *hit, *target;
    if (!fly(t, hit, target)) return;
    if (hit && hit->cls == 3 && (hit->type == 0 || hit->type == 1)) {
        Thing *e = thing_create(thing_pos(t), (int8_t)t->impact_cls, (int8_t)t->impact_type);
        if (!e) return;
        projectile_record_hit_stats(t, hit, target);
        if (t->impact_type == 0x22) e->health = t->damage;
        e->owner = t->owner;
        e->yaw = t->yaw;
        e->pitch = t->pitch;
        e->target = hit_index(hit);
        e->damage = t->damage;
        thing_mark_delete(t);
        return;
    }
    projectile_record_hit_stats(t, nullptr, target);
    thing_mark_delete(t);
}

// projectile_step_44ea0
void projectile_step(Thing *t) {
    bool done = false;
    if (!thing_is_set(thing_ref(t->target))) first_tick_aim(t);
    g_pos_scratch = *thing_pos(t);
    math_rotate_offset(&g_pos_scratch, t->yaw, t->pitch, t->speed_cur);
    *thing_pos(t) = g_pos_scratch;                              // plain copy: the Thing is not linked
    Thing *hit = thing_find_collision(t);
    if (hit) {
        *thing_pos(t) = *thing_pos(hit);
        done = true;
    } else {
        int h = terrain_height_at(thing_pos(t));
        g_pos_scratch.z = (int16_t)h;
        if ((int16_t)h > t->z) done = true;
        else if (--t->health < 0) done = true;
    }
    if (done) thing_mark_delete(t);
}

// projectile_lightning_update_44fc0: the whole ray is traced in one tick (projectile_step until the
// Thing is marked for deletion), then 8 bolt segments per step are laid along it, wandering sideways
// and up / down by a bounded random walk, and the impact effect is created at the end.
void projectile_lightning_update(Thing *t) {
    t->speed_cur = t->speed_base;
    Pos start = *thing_pos(t);
    thing_unlink_cell(t);
    projectile_step(t);
    int32_t count = 1;
    uint16_t yaw = t->yaw, pitch = t->pitch;
    while (!(t->flags & 0x400)) {
        projectile_step(t);
        count++;
    }
    t->yaw = yaw;
    t->pitch = pitch;
    Pos delta = {0, 0, 0};
    count <<= 3;
    int32_t step = t->speed_cur / 8;
    math_rotate_offset(&delta, t->yaw, t->pitch, (int16_t)step);
    int32_t off_a = 0, off_b = 0;     // [esp+0x20] / [esp+0x24]; only off_b reaches the positions
    g_pos_scratch = start;
    Pos seg = start;
    while ((int16_t)count >= 0) {
        Thing *s = thing_alloc();
        if (s) {
            s->state = 0xe;
            s->cls = 9;
            s->type = 9;
            s->owner = t->owner;
            // a segment behind `t` in the pool is updated once more in this pass: one tick more
            s->max_health = (s >= t ? 1 : 0) - 1;
            thing_set_sprite(s, 0xd8);
            thing_link_cell(s, &seg);
            thing_restore_health(s);
        }
        int32_t lim = (int16_t)count / 2;
        if ((int16_t)lim < 0) lim = 0;
        if ((int16_t)lim > 8) lim = 8;
        if ((int16_t)lim < (int16_t)off_b) {
            off_b = off_b - 1;
        } else if ((int16_t)off_b < -(int16_t)lim) {
            off_b = off_b + 1;
        } else {
            t->rng = mc_lcg(t->rng);
            off_b = (int16_t)off_b + ((int32_t)(t->rng % 0x9d) / 0x4f * 2 - 1);
        }
        if ((int16_t)lim < (int16_t)off_a) {
            off_a = off_a - 1;
        } else if ((int16_t)off_a < -(int16_t)lim) {
            off_a = off_a + 1;
        } else {
            t->rng = mc_lcg(t->rng);
            off_a = (int16_t)off_a + ((int32_t)(t->rng % 0x9d) / 0x4f * 2 - 1);
        }
        // (delta.x / .y are wrapped offsets: the 16-bit sum of the original, WCoord wraps at the width)
        g_pos_scratch.x = g_pos_scratch.x + delta.x;
        g_pos_scratch.y = g_pos_scratch.y + delta.y;
        g_pos_scratch.z = (int16_t)(g_pos_scratch.z + delta.z);
        int32_t wander = (int16_t)step / 4 * (int16_t)off_b;
        seg = g_pos_scratch;
        seg.z = (int16_t)(g_pos_scratch.z + wander);
        math_rotate_offset(&seg, (t->yaw + 0x200) & 0x7ff, 0, (int16_t)wander);
        count--;
    }
    Thing *hit = thing_find_collision(t);
    Thing *target = thing_ref(t->target);
    Thing *e = thing_create(&seg, (int8_t)t->impact_cls, (int8_t)t->impact_type);
    if (!e) return;
    projectile_record_hit_stats(t, hit, target);
    e->owner = t->owner;
    e->yaw = t->yaw;
    e->pitch = t->pitch;
    e->target = hit_index(hit);
    if (hit && (hit->flags & 0x8000) && hit->cls == 3 && t->mana / 4 <= hit->mana)
        e->damage = (uint16_t)(t->damage / 4);                  // rebound quarters lightning
    else
        e->damage = t->damage;
}

// projectile_type10_s10_update_45360: the castle seed. With a target Thing (an existing castle) it
// homes on it; without one it flies to Thing.home (45530).
void projectile_type10_s10_update(Thing *t) {
    bool done = false;
    Thing *target = thing_ref(t->target);
    if (!thing_is_set(target)) {
        projectile_castle_seed_update(t);
        return;
    }
    thing_turn_toward(t, target);
    accelerate(t);
    g_pos_scratch = *thing_pos(t);
    math_rotate_offset(&g_pos_scratch, t->yaw, t->pitch, t->speed_cur);
    thing_move_to(t, &g_pos_scratch);
    if (thing_collide(t, target)) {
        thing_move_to(t, thing_pos(target));
        done = true;
    } else {
        int h = terrain_height_at(thing_pos(t));
        g_pos_scratch.z = (int16_t)h;
        if ((int16_t)h > t->z) done = true;
        else if (--t->health < 0) done = true;
    }
    if (!done) return;
    if (t->impact_cls == 3) {
        // a new castle (class 3) is refused while the owner has one (P+0x32)
        if (player_block(thing_ref((int16_t)t->owner))->castle != 0) {
            thing_mark_delete(t);
            return;
        }
    }
    Thing *e = thing_create(thing_pos(t), (int8_t)t->impact_cls, (int8_t)t->impact_type);
    if (e) {
        e->owner = t->owner;
        thing_mark_delete(t);
        return;
    }
    // creation failed: the Castle spell is released (the seed itself stays and tries again)
    castle_spell_reset_charge(thing_ref((int16_t)t->owner), 0);
}

// projectile_castle_seed_update_45530
void projectile_castle_seed_update(Thing *t) {
    bool done = false, blocked = false;
    if (!(t->flags & 2)) {
        // first tick: only the site test at the launch position
        t->flags |= 2;
        if ((uint8_t)castle_site_clear_at_pos(thing_pos(t)) != 0) return;
        castle_spell_reset_charge(t, 0);
        thing_mark_delete(t);
        return;
    }
    t->target_yaw = (uint16_t)pos_angle_to(thing_pos(t), &t->home);
    t->target_pitch = (uint16_t)pos_pitch_to(thing_pos(t), &t->home);
    turn_to_target_angles(t);
    accelerate(t);
    g_pos_scratch = *thing_pos(t);
    math_rotate_offset(&g_pos_scratch, t->yaw, t->pitch, t->speed_cur);
    thing_move_to(t, &g_pos_scratch);
    Thing *target = thing_ref(t->target);          // Thing 0 here (45360 only comes without a target)
    if (thing_collide(t, target)) {
        thing_z_add_half_height(target);           // (never taken back in the original)
        thing_move_to(t, thing_pos(target));
        done = true;
    } else {
        int h = terrain_height_at(thing_pos(t));
        g_pos_scratch.z = (int16_t)h;
        if ((int16_t)h > t->z) {
            done = true;
        } else if (--t->health < 0) {
            done = true;
        } else {
            blocked = (uint8_t)castle_site_clear_at_pos(thing_pos(t)) == 0;
            if (blocked) done = true;
        }
    }
    if (!done) return;
    if (blocked) {
        // over a site that is taken: one step back, build there
        g_pos_scratch = *thing_pos(t);
        math_rotate_offset(&g_pos_scratch, (t->yaw + 0x400) & 0x7ff, t->pitch, t->speed_cur);
        thing_move_to(t, &g_pos_scratch);
    }
    Thing *e = thing_create(thing_pos(t), (int8_t)t->impact_cls, (int8_t)t->impact_type);
    if (!e) return;
    e->owner = t->owner;
    thing_mark_delete(t);
}

// projectile_type12_s12_update_457a0
void projectile_type12_s12_update(Thing *t) {
    Thing *hit, *target;
    if (!fly(t, hit, target)) return;
    Thing *e = thing_create(thing_pos(t), 10, 0x26);
    if (!e) return;
    projectile_record_hit_stats(t, hit, target);
    if (t->impact_type == 0x22) e->health = t->damage;
    e->owner = t->owner;
    e->yaw = t->yaw;
    e->pitch = t->pitch;
    e->target = hit_index(hit);
    e->damage = t->damage;
    e->impact_cls = t->impact_cls;
    e->impact_type = t->impact_type;
    thing_mark_delete(t);
}

// projectile_type13_s13_update_45b60
void projectile_type13_s13_update(Thing *t) {
    if (!(t->flags & 2)) {
        t->rng = mc_lcg(t->rng);
        sound_request((int16_t)thing_index(t), -1, (int16_t)((t->rng & 3) + 0x21));
        t->flags |= 2;
    }
    g_pos_scratch = *thing_pos(t);
    math_rotate_offset(&g_pos_scratch, t->yaw, t->pitch, t->speed_cur);
    Thing *hit = thing_find_collision(t);          // at the position before the move
    int h = terrain_height_at(&g_pos_scratch);
    if ((int16_t)h <= g_pos_scratch.z) {
        int32_t life = t->health;
        t->health = life - 1;
        if (life != 0 && !hit) {
            thing_move_to(t, &g_pos_scratch);
            return;
        }
    }
    if (hit) {
        thing_z_add_half_height(hit);
        thing_move_to(t, thing_pos(hit));
        thing_z_sub_half_height(hit);
    }
    thing_area_damage(t, 0, t->damage);
    thing_mark_delete(t);
}

// projectile_type14_s14_update_45c70
void projectile_type14_s14_update(Thing *t) {
    int32_t life = t->health;
    t->health = life - 1;
    if (life < 0) thing_mark_delete(t);
}

// projectile_type18_s18_update_45c90
void projectile_type18_s18_update(Thing *t) {
    bool explode = false;
    Thing *target = thing_ref(t->target);
    if (thing_is_set(target)) {
        projectile_steer_to_target(t, target);
    } else if (!(t->flags & 2)) {
        t->flags |= 2;
        if (projectile_pick_target(t)) {
            t->yaw = t->target_yaw;
            t->pitch = t->target_pitch;
        }
    }
    g_pos_scratch = *thing_pos(t);
    math_rotate_offset(&g_pos_scratch, t->yaw, t->pitch, t->speed_cur);
    int h = terrain_height_at(&g_pos_scratch);
    if ((int16_t)h > g_pos_scratch.z) g_pos_scratch.z = (int16_t)h;
    thing_move_to(t, &g_pos_scratch);
    Thing *hit = thing_find_mana_ball_touching(t);
    if (hit) {
        thing_z_add_half_height(hit);
        thing_move_to(t, thing_pos(hit));
        thing_z_sub_half_height(hit);
        explode = true;
    } else {
        h = terrain_height_at(thing_pos(t));
        g_pos_scratch.z = (int16_t)h;
        if ((int16_t)h > t->z) explode = true;
        else if (--t->health < 0) explode = true;
    }
    if (!explode) return;
    Thing *e = thing_create(thing_pos(t), 10, 0xc);
    if (e) {
        projectile_record_hit_stats(t, hit, target);
        e->owner = t->owner;
        e->yaw = t->yaw;
        e->pitch = t->pitch;
    }
    e = thing_create(thing_pos(t), (int8_t)t->impact_cls, (int8_t)t->impact_type);
    if (!e) return;
    projectile_record_hit_stats(t, hit, target);
    e->owner = t->owner;
    e->yaw = t->yaw;
    e->pitch = t->pitch;
    thing_mark_delete(t);
}

// projectile_type19_s19_update_45e60
void projectile_type19_s19_update(Thing *t) {
    Thing *e = thing_create(thing_pos(t), (int8_t)t->impact_cls, (int8_t)t->impact_type);
    if (!e) return;
    e->owner = t->owner;
    e->yaw = t->yaw;
    e->pitch = t->pitch;
    e->damage = t->damage;
    thing_mark_delete(t);
}

// ---- target selection ----------------------------------------------------------------------------

// projectile_pick_target_45f00 (jump table at 0x45eac, indexed by Thing.type)
int projectile_pick_target(Thing *t) {
    if (t->aux > 0x10) t->aux = 0x10;
    uint32_t best = 0xffffffffu;
    Thing *found = nullptr;
    switch (t->type) {
    case 9: {                                                   // 0x45f30
        for (uint32_t i = g_cfg->player_list; i != 0 && i < (uint32_t)thing_pool_slots(); i = thing_at(i)->next) {
            Thing *o = thing_at(i);
            if (o->owner == t->owner) continue;
            if (o->flags & 0x20) continue;
            uint32_t range = (uint32_t)(t->max_health * (int32_t)t->speed_base);
            if ((uint32_t)pos_dist_xyz(thing_pos(o), thing_pos(t)) > range) continue;
            uint32_t s = o->type == 2 ? target_aim_score(t, o, 0x71, 0x71) : projectile_target_score(t, o, 0x71, 0x71);
            if (s >= best) continue;
            found = o;
            best = s;
        }
        for (int list = 0; list < 20; list++) {
            for (uint32_t i = g_cfg->creature_lists[list]; i != 0 && i < (uint32_t)thing_pool_slots(); i = thing_at(i)->next) {
                Thing *o = thing_at(i);
                if (o->owner == t->owner) continue;
                if (o->timer_a == 0) continue;
                uint32_t s = projectile_target_score(t, o, 0x71, 0x200);
                if (s >= best) continue;
                found = o;
                best = s;
            }
        }
        if (!found) return 0;
        t->target = thing_index(found);
        thing_aim_at(t, found);
        return 1;
    }
    case 0: case 3: case 4: case 0x10: case 0x12: case 0x13: {  // 0x46062
        // Hidden Worlds (HIDDEN.EXE 0x54bb0): type 0x10 has its own copy of this case with cone 0x100
        const unsigned cone = (t->type == 0x10 && world_hidden()) ? 0x100 : 0x71;
        const Thing *owner = thing_ref((int16_t)t->owner);
        for (uint32_t i = g_cfg->player_list; i != 0 && i < (uint32_t)thing_pool_slots(); i = thing_at(i)->next) {
            Thing *o = thing_at(i);
            if (o->owner == t->owner) continue;
            if (o->flags & 0x20) continue;
            uint32_t range = (uint32_t)(int32_t)(int16_t)mc_move_desc(owner->desc)->sight_radius;
            if ((uint32_t)pos_dist_xyz(thing_pos(o), thing_pos(t)) > range) continue;
            uint32_t s = o->type == 2 ? target_aim_score(t, o, cone, 0x71) : projectile_target_score(t, o, cone, 0x71);
            if (s >= best) continue;
            found = o;
            best = s;
        }
        for (int list = 0; list < 20; list++) {
            for (uint32_t i = g_cfg->creature_lists[list]; i != 0 && i < (uint32_t)thing_pool_slots(); i = thing_at(i)->next) {
                Thing *o = thing_at(i);
                if (o->owner == t->owner) continue;
                if (o->timer_a == 0) continue;
                uint32_t s = projectile_target_score(t, o, cone, 0x71);
                if (s >= best) continue;
                found = o;
                best = s;
            }
        }
        if (!found) return 0;
        t->target = thing_index(found);
        thing_aim_at(t, found);
        if (found->cls == 3 && found->type == 0) player_set_combat_music_timer(found);
        return 1;
    }
    case 1: {                                                   // 0x461fc
        // port: Possession's pick radius (0x1400) scaled with settings.h possession_range_pct
        const int32_t reach = (int32_t)(0x1400 * gameplay_rules().possession_range_pct / 100);
        for (uint32_t i = g_cfg->mana_ball_list; i != 0 && i < (uint32_t)thing_pool_slots(); i = thing_at(i)->next) {
            Thing *o = thing_at(i);
            if ((int32_t)o->mana_owner == (int32_t)(int16_t)t->owner) continue;
            if (o->timer_a == 0) continue;
            uint32_t s = projectile_target_score(t, o, 0x71, 0x71, reach);
            if (s >= best) continue;
            found = o;
            best = s;
        }
        for (uint32_t i = g_cfg->wizard_list; i != 0 && i < (uint32_t)thing_pool_slots(); i = thing_at(i)->next) {
            Thing *o = thing_at(i);
            if ((int32_t)o->mana_owner == (int32_t)(int16_t)t->owner) continue;
            if (o->timer_a == 0) continue;
            uint32_t s = projectile_target_score(t, o, 0x71, 0x71, reach);
            if (s >= best) continue;
            found = o;
            best = s;
        }
        if (!found) return 0;
        t->target = thing_index(found);
        thing_aim_at(t, found);
        return 1;
    }
    case 0x11: {                                                // 0x462df
        for (uint32_t i = g_cfg->mana_ball_list; i != 0 && i < (uint32_t)thing_pool_slots(); i = thing_at(i)->next) {
            Thing *o = thing_at(i);
            if (o->timer_a == 0) continue;
            uint32_t s = projectile_target_score(t, o, 0x71, 0x71);
            if (s >= best) continue;
            found = o;
            best = s;
        }
        if (!found) return 0;
        t->target = thing_index(found);
        thing_aim_at(t, found);
        return 1;
    }
    case 7: case 8: case 0xb: case 0xc: {                       // 0x46356
        const Thing *owner = thing_ref((int16_t)t->owner);
        for (uint32_t i = g_cfg->player_list; i != 0 && i < (uint32_t)thing_pool_slots(); i = thing_at(i)->next) {
            Thing *o = thing_at(i);
            if (o->owner == t->owner) continue;
            if (o->flags & 0x20) continue;
            uint32_t range = (uint32_t)(int32_t)(int16_t)mc_move_desc(owner->desc)->sight_radius;
            if ((uint32_t)pos_dist_xyz(thing_pos(o), thing_pos(t)) > range) continue;
            // (the original has the "castle" branch here too, but both call 46470)
            uint32_t s = projectile_target_score(t, o, 0x71, 0x71);
            if (s >= best) continue;
            found = o;
            best = s;
        }
        if (!found) return 0;
        t->target = thing_index(found);
        thing_aim_at(t, found);
        if (found->cls == 3 && found->type == 0) player_set_combat_music_timer(found);
        return 1;
    }
    default:                                                    // 0x46462
        return 0;
    }
}

// projectile_target_score_46470
uint32_t projectile_target_score(const Thing *shooter, Thing *target, unsigned max_yaw, unsigned max_pitch,
                                 int32_t max_dist) {
    thing_z_add_half_height(target);
    unsigned dyaw = (unsigned)angle_diff(shooter->yaw, pos_angle_to(thing_pos(shooter), thing_pos(target)) & 0xffff);
    if ((uint16_t)dyaw > (uint16_t)max_yaw) {
        thing_z_sub_half_height(target);
        return 0xffffffffu;
    }
    unsigned dpitch = (unsigned)angle_diff(shooter->pitch, pos_pitch_to(thing_pos(shooter), thing_pos(target)) & 0xffff);
    if ((uint16_t)dpitch > (uint16_t)max_pitch) {
        thing_z_sub_half_height(target);
        return 0xffffffffu;
    }
    int32_t dist = pos_dist_xy(thing_pos(shooter), thing_pos(target));
    if (dist > max_dist) {
        thing_z_sub_half_height(target);
        return 0xffffffffu;
    }
    thing_z_sub_half_height(target);
    return aim_score(dyaw & 0xffff, dpitch & 0xffff, dist);
}

// target_aim_score_465b0
uint32_t target_aim_score(const Thing *a, const Thing *b, unsigned max_yaw, unsigned max_pitch) {
    unsigned dyaw = (unsigned)angle_diff(a->yaw, pos_angle_to(thing_pos(a), thing_pos(b)) & 0xffff);
    if ((uint16_t)dyaw > (uint16_t)max_yaw) return 0xffffffffu;
    unsigned dpitch = (unsigned)angle_diff(a->pitch, pos_pitch_to(thing_pos(a), thing_pos(b)) & 0xffff);
    if ((uint16_t)dpitch > (uint16_t)max_pitch) return 0xffffffffu;
    int32_t dist = pos_dist_xy(thing_pos(a), thing_pos(b));
    if (dist > 0x1400) return 0xffffffffu;
    return aim_score(dyaw & 0xffff, dpitch & 0xffff, dist);
}

// projectile_line_of_fire_clear_466af (real entry 0x466b0)
int projectile_line_of_fire_clear(Thing *shooter, Thing *target) {
    const Pos *tp = thing_pos(target);
    int32_t range = (int16_t)mc_move_desc(shooter->desc)->sight_radius;
    int32_t dist = pos_dist_xyz(thing_pos(shooter), tp);
    if (dist > range) return 0;
    if ((uint16_t)angle_diff(shooter->yaw, pos_angle_to(thing_pos(shooter), tp) & 0xffff) > 0x100) return 0;
    if ((uint16_t)angle_diff(shooter->pitch, pos_pitch_to(thing_pos(shooter), tp) & 0xffff) > 0x38) return 0;
    Thing probe = *shooter;                                     // a copy on the stack, never linked
    probe.z = (int16_t)(probe.z + probe.ext_z0);
    probe.yaw = (uint16_t)pos_angle_to(thing_pos(&probe), tp);
    probe.pitch = (uint16_t)pos_pitch_to(thing_pos(&probe), tp);
    probe.speed_cur = 0x180;
    probe.filter_cls = 0xff;
    probe.filter_type = 0xff;
    target->z = (int16_t)(target->z + target->ext_z0);
    for (int32_t i = 0; i < dist / (int32_t)probe.speed_cur; i++) {
        math_rotate_offset(thing_pos(&probe), probe.yaw, probe.pitch, probe.speed_cur);
        if (thing_collide(&probe, shooter)) {                   // sic: the shooter, not the target
            target->z = (int16_t)(target->z - target->ext_z0);
            return 1;
        }
        if ((int16_t)terrain_height_at(thing_pos(&probe)) > probe.z) {
            target->z = (int16_t)(target->z - target->ext_z0);
            return 0;
        }
    }
    target->z = (int16_t)(target->z - target->ext_z0);
    return 0;
}

// ---- registration --------------------------------------------------------------------------------

void projectiles_register_handlers() {
    thing_register_update(0x44510, projectile_type0_s0_update);
    thing_register_update(0x448b0, projectile_homing_update);
    thing_register_update(0x44a40, projectile_update_shared);
    thing_register_update(0x44a50, projectile_type3_s3_update);
    thing_register_update(0x44a90, projectile_type7_s7_update);
    thing_register_update(0x44aa0, projectile_type8_s8_update);
    thing_register_update(0x44fc0, projectile_lightning_update);
    thing_register_update(0x45360, projectile_type10_s10_update);
    thing_register_update(0x457a0, projectile_type12_s12_update);
    thing_register_update(0x45b60, projectile_type13_s13_update);
    thing_register_update(0x45c70, projectile_type14_s14_update);
    thing_register_update(0x45c90, projectile_type18_s18_update);
    thing_register_update(0x45e60, projectile_type19_s19_update);
}
