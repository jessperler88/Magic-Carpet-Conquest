// Spells of carpet.exe (class 12, 0x46ae0..0x494b0): dropped spell pickups, the cast helpers and
// every class-12 Table A handler. Translated from the disassembly (addresses in the comments); see
// docs/analysis/port_spells.md for what is verified.
//
// Reading aid for the cast handlers: `t` is the spell Thing, `c` its caster (Thing.caster, a wizard),
// `p` a projectile the spell launches. The original tests "caster pointer above the pool base", i.e.
// caster index != 0; things[0] is the scratch sentinel.
#include "spells.h"
#include "world_set.h"
#include "mc_math.h"
#include "mode.h"

namespace {

// The caster's player block (PlayerRec+0x44f; the dummy block for a Thing nobody owns).
inline PlayerBlock *block(const Thing *t) {
    return reinterpret_cast<PlayerBlock *>(thing_player_block(t));
}
// Thing from an index the original multiplies by 0xa4 unchecked; anything outside the pool reads as
// "none" (things[0]) here.
inline Thing *thing_ref(unsigned idx) { return thing_at(idx < (unsigned)thing_pool_slots() ? idx : 0u); }
inline Thing *spell_caster(const Thing *t) { return thing_ref(t->caster); }
inline bool   is_none(const Thing *t) { return t == thing_at(0); }
// (thing - base) / 0xa4, cwde: the index as a sound request takes it.
inline int idx16(const Thing *t) { return (int16_t)thing_index(t); }

// DAT_000943c4: spell_phase2_pickup_46dd0 clears it and tests it again after the pickup; nothing
// else in the image references it (two references, both in that function), so the flag-copy branch
// it guards never runs in the retail build.
uint8_t g_spell_pickup_keep_flags = 0;

// ---- pieces every launch shares ----------------------------------------------------------------

inline void shot_add_caster_speed(Thing *p, const Thing *c) { p->speed_cur = (int16_t)(p->speed_cur + c->speed_cur); }
inline void shot_set_impact(Thing *p, int cls, int type) { p->impact_cls = (uint8_t)cls; p->impact_type = (uint8_t)type; }
inline void shot_raise(Thing *p, const Thing *c) { p->z = (int16_t)(p->z + c->ext_h); }
inline void shot_copy_angles(Thing *p, const Thing *c) { p->yaw = c->yaw; p->pitch = c->pitch; }
// Thing.aux of the projectile = the caster's aim charge (P+0x146, 0..200), which starts again at 0.
inline void shot_take_aim(Thing *p, Thing *c) {
    p->aux = (int16_t)block(c)->aim_charge;
    block(c)->aim_charge = 0;
}
// Thing.home of the projectile = the point `dist` ahead of the caster (its target / end point).
inline void shot_target_ahead(Thing *p, const Thing *c, int pitch, int dist) {
    p->home = *thing_pos(c);
    math_rotate_offset(&p->home, c->yaw, pitch, dist);
}
inline void shot_target_on_ground(Thing *p) { p->home.z = (int16_t)terrain_height_at(&p->home); }

using LaunchFn = void (*)(Thing *t, Thing *c, const Pos *pos);

// The frame of spell_fireball_update_47130, shared instruction for instruction by 475b0, 48c20,
// 48de0, 49140 and 492e0: the launch tick fires burst + 1 times (Thing.burst, queued by
// player_cast_spell_410f0 for multi-shot spells; 0 for every spell the constructors make).
void spell_cast_burst(Thing *t, LaunchFn launch) {
    if (t->cast_ticks <= 0) return;
    Thing *c = spell_caster(t);
    if (!is_none(c)) {
        if (spell_can_cast(t, c)) {
            const Pos *pos = thing_pos(c);
            while ((int8_t)t->burst >= 0) {
                if (t->cast_ticks == t->duration) launch(t, c, pos);
                spell_charge_mana(t, c);
                t->burst--;
            }
            t->burst = 0;
            t->cast_ticks--;
            return;
        }
        t->cast_ticks = 1;
    }
    t->cast_ticks--;
}

// The frame of the one-shot projectile spells (479f0, 47b90, 47d40, 480e0, 482f0, 48510, 488a0,
// 48a70): launch on the first tick, spell_charge_mana on every tick. spell_earthquake_update_47840
// differs in one jump: its later ticks skip spell_charge_mana (`charge_every_tick` false).
void spell_cast_single(Thing *t, LaunchFn launch, bool charge_every_tick) {
    if (t->cast_ticks <= 0) return;
    Thing *c = spell_caster(t);
    if (!is_none(c)) {
        if (!spell_can_cast(t, c)) {
            t->cast_ticks = 1;
        } else if (t->cast_ticks == t->duration) {
            launch(t, c, thing_pos(c));
            spell_charge_mana(t, c);
            t->cast_ticks--;
            return;
        } else if (charge_every_tick) {
            spell_charge_mana(t, c);
            t->cast_ticks--;
            return;
        }
    }
    t->cast_ticks--;
}

// ---- the projectile each spell launches (first cast tick) --------------------------------------

// spell_fireball_update_47130 / spell_mini_fireball_update_492e0 (identical bodies): projectile 0.
void launch_fireball(Thing *t, Thing *c, const Pos *pos) {
    Thing *p = thing_create(pos, 9, 0);
    if (!p) return;
    shot_add_caster_speed(p, c);
    spell_projectile_origin(c, p);
    shot_set_impact(p, 10, 0);
    p->owner = c->owner;
    p->damage = t->damage;
    p->mana = t->mana;
    shot_raise(p, c);
    shot_copy_angles(p, c);
    shot_take_aim(p, c);
    shot_target_ahead(p, c, c->pitch, 0x4000);
    sound_request(idx16(p), -1, 9);
}

// spell_possession_s9_update_475b0: projectile 1 (homing), fixed aux 0xc8, no damage copy.
void launch_possession(Thing *t, Thing *c, const Pos *pos) {
    Thing *p = thing_create(pos, 9, 1);
    if (!p) return;
    shot_add_caster_speed(p, c);
    spell_projectile_origin(c, p);
    shot_set_impact(p, 10, 0xc);
    p->owner = c->owner;
    shot_raise(p, c);
    p->aux = 0xc8;
    p->mana = t->mana;
    block(c)->aim_charge = 0;
    shot_target_ahead(p, c, c->pitch, 0x2800);
    shot_copy_angles(p, c);
    sound_request(idx16(p), -1, 0x28);
}

// spell_earthquake_update_47840: projectile 2, target 0x1000 ahead on the ground.
void launch_earthquake(Thing *t, Thing *c, const Pos *pos) {
    Thing *p = thing_create(pos, 9, 2);
    if (!p) return;
    shot_add_caster_speed(p, c);
    spell_projectile_origin(c, p);
    shot_set_impact(p, 10, 0xf);
    p->owner = c->owner;
    shot_raise(p, c);
    shot_copy_angles(p, c);
    p->mana = t->mana;
    p->aux = (int16_t)block(c)->aim_charge;
    p->damage = t->damage;
    block(c)->aim_charge = 0;
    shot_target_ahead(p, c, 0, 0x1000);
    shot_target_on_ground(p);
    sound_request(idx16(p), -1, 9);
}

// spell_meteor_update_479f0: projectile 3.
void launch_meteor(Thing *t, Thing *c, const Pos *pos) {
    Thing *p = thing_create(pos, 9, 3);
    if (!p) return;
    shot_add_caster_speed(p, c);
    spell_projectile_origin(c, p);
    shot_set_impact(p, 10, 0x11);
    p->owner = c->owner;
    shot_raise(p, c);
    p->mana = t->mana;
    p->damage = t->damage;
    shot_take_aim(p, c);
    shot_target_ahead(p, c, c->pitch, 0x2800);
    shot_copy_angles(p, c);
    sound_request(idx16(p), -1, 0xf);
}

// spell_volcano_s24_update_47b90: projectile 4, target on the ground.
void launch_volcano(Thing *t, Thing *c, const Pos *pos) {
    Thing *p = thing_create(pos, 9, 4);
    if (!p) return;
    shot_add_caster_speed(p, c);
    spell_projectile_origin(c, p);
    shot_set_impact(p, 10, 9);
    p->owner = c->owner;
    shot_raise(p, c);
    p->mana = t->mana;
    p->damage = t->damage;
    shot_take_aim(p, c);
    shot_target_ahead(p, c, 0, 0x1000);
    shot_target_on_ground(p);
    shot_copy_angles(p, c);
    sound_request(idx16(p), -1, 0xf);
}

// spell_crater_update_47d40: projectile 5, target on the ground.
void launch_crater(Thing *t, Thing *c, const Pos *pos) {
    Thing *p = thing_create(pos, 9, 5);
    if (!p) return;
    shot_add_caster_speed(p, c);
    spell_projectile_origin(c, p);
    shot_set_impact(p, 10, 0xb);
    p->damage = t->damage;
    p->owner = c->owner;
    shot_raise(p, c);
    shot_copy_angles(p, c);
    p->mana = t->mana;
    shot_take_aim(p, c);
    shot_target_ahead(p, c, 0, 0x1000);
    shot_target_on_ground(p);
    sound_request(idx16(p), -1, 0xf);
}

// spell_rubber_band_s33_update_480e0: projectile 7; the only launch that neither adds the caster's
// speed nor touches the aim charge.
void launch_rubber_band(Thing *t, Thing *c, const Pos *pos) {
    Thing *p = thing_create(pos, 9, 7);
    if (!p) return;
    spell_projectile_origin(c, p);
    shot_set_impact(p, 10, 0x1a);
    p->damage = t->damage;
    p->owner = c->owner;
    shot_raise(p, c);
    p->mana = t->mana;
    shot_target_ahead(p, c, c->pitch, 0x2800);
    shot_copy_angles(p, c);
    sound_request(idx16(p), -1, 9);
}

// spell_steal_mana_s39_update_482f0: projectile 8; the spell's damage is copied and then replaced
// by the constant 0x7d0.
void launch_steal_mana(Thing *t, Thing *c, const Pos *pos) {
    Thing *p = thing_create(pos, 9, 8);
    if (!p) return;
    shot_add_caster_speed(p, c);
    spell_projectile_origin(c, p);
    shot_set_impact(p, 10, 0x19);
    p->damage = t->damage;
    p->owner = c->owner;
    shot_raise(p, c);
    p->damage = 0x7d0;
    p->mana = t->mana;
    shot_take_aim(p, c);
    shot_target_ahead(p, c, c->pitch, 0x4000);
    shot_copy_angles(p, c);
    sound_request(idx16(p), -1, 9);
}

// spell_lightning_update_48510: projectile 9.
void launch_lightning(Thing *t, Thing *c, const Pos *pos) {
    Thing *p = thing_create(pos, 9, 9);
    if (!p) return;
    shot_add_caster_speed(p, c);
    spell_projectile_origin(c, p);
    shot_set_impact(p, 10, 0x17);
    p->owner = c->owner;
    p->mana = t->mana;
    shot_raise(p, c);
    shot_copy_angles(p, c);
    p->aux = (int16_t)block(c)->aim_charge;
    p->damage = t->damage;
    block(c)->aim_charge = 0;
    shot_target_ahead(p, c, c->pitch, 0x4000);
    sound_request(idx16(p), -1, 0x17);
}

// spell_skeleton_s51_update_488a0: projectile 0xb. Its damage field is the mana the skeleton army
// may take from the caster's castle: the spell's mana_total when the castle can hold that much,
// else 1.
void launch_skeleton(Thing *t, Thing *c, const Pos *pos) {
    Thing *p = thing_create(pos, 9, 0xb);
    if (!p) return;
    shot_add_caster_speed(p, c);
    spell_projectile_origin(c, p);
    shot_set_impact(p, 10, 0x24);
    p->damage = t->damage;
    p->owner = c->owner;
    shot_raise(p, c);
    p->damage = 1;
    p->mana = t->mana;
    uint16_t castle = block(c)->castle;
    if (castle != 0 && thing_ref(castle)->mana_total >= t->mana_total) p->damage = (uint16_t)t->mana_total;
    shot_take_aim(p, c);
    shot_target_ahead(p, c, c->pitch, 0x4000);
    shot_copy_angles(p, c);
    sound_request(idx16(p), -1, 9);
}

// spell_thunderbolt_s54_update_48a70: projectile 0xc; its impact "effect" is class 9 type 9, i.e.
// a lightning projectile.
void launch_thunderbolt(Thing *t, Thing *c, const Pos *pos) {
    Thing *p = thing_create(pos, 9, 0xc);
    if (!p) return;
    shot_add_caster_speed(p, c);
    spell_projectile_origin(c, p);
    shot_set_impact(p, 9, 9);
    p->damage = t->damage;
    p->owner = c->owner;
    shot_raise(p, c);
    p->mana = t->mana;
    shot_take_aim(p, c);
    shot_target_ahead(p, c, c->pitch, 0x4000);
    shot_copy_angles(p, c);
    sound_request(idx16(p), -1, 9);
}

// The body shared by 48c20 / 48de0 / 49140: the fireball launch with another projectile type,
// impact effect and sound (0 = none).
void launch_fireball_like(Thing *t, Thing *c, const Pos *pos, int proj_type, int impact_type, int sound) {
    Thing *p = thing_create(pos, 9, proj_type);
    if (!p) return;
    shot_add_caster_speed(p, c);
    spell_projectile_origin(c, p);
    shot_set_impact(p, 10, impact_type);
    p->owner = c->owner;
    p->damage = t->damage;
    p->mana = t->mana;
    shot_raise(p, c);
    shot_copy_angles(p, c);
    shot_take_aim(p, c);
    shot_target_ahead(p, c, c->pitch, 0x4000);
    if (sound) sound_request(idx16(p), -1, sound);
}
// spell_mana_magnet_s57_update_48c20: projectile 0x11 (the large homing one), impact 10 / 0x36.
void launch_mana_magnet(Thing *t, Thing *c, const Pos *pos) { launch_fireball_like(t, c, pos, 0x11, 0x36, 0x28); }
// spell_fire_wall_s60_update_48de0: projectile 0x10, impact 10 / 0x35.
void launch_fire_wall(Thing *t, Thing *c, const Pos *pos)   { launch_fireball_like(t, c, pos, 0x10, 0x35, 9); }
// spell_update_shared_49140: projectile 0x12, impact 10 / 0x37, no sound request.
void launch_smart_bomb(Thing *t, Thing *c, const Pos *pos)  { launch_fireball_like(t, c, pos, 0x12, 0x37, 0); }

// The frame shared by spell_speedup_update_47420 (sign +1) and spell_reverse_speed_s63_update_48fa0
// (sign -1): while the cast lasts and no speed key is held, the flyer is driven at 3x (first tick)
// / 2x its base speed and leaves a smoke puff every 4th tick.
void spell_cast_speed(Thing *t, int sign) {
    if (t->cast_ticks <= 0) return;
    Thing *c = spell_caster(t);
    if (is_none(c)) return;
    PlayerBlock *P = block(c);
    if (spell_can_cast(t, c) && P->accelerating == 0) {
        if (t->cast_ticks == t->duration && !(t->flags & 0x80)) {
            t->flags |= 0x80;
            sound_request(idx16(c), -1, 0x13);
        }
        if ((int)t->cast_ticks == (int)t->duration - 2 && (t->flags & 0x80)) t->flags &= ~0x80u;
        // FUN_0003fbf0(c): an empty function (a lone `ret`) in the retail build.
        if (t->cast_ticks == t->duration) P->target_speed = (int16_t)(sign * (int)c->speed_base * 3);
        else                              P->target_speed = (int16_t)(sign * (int)c->speed_base * 2);
        c->speed_cur = P->target_speed;
        if (!(t->tick & 3)) {
            Thing *e = thing_create(thing_pos(c), 10, 2);
            if (e) {
                e->owner = c->owner;
                e->health = (int32_t)((uint32_t)e->health << 2);
            }
        }
        spell_charge_mana(t, c);
    } else if (P->accelerating != 0) {
        t->cast_ticks = 1;
    }
    t->cast_ticks--;
    if (t->cast_ticks == 0) {
        P->target_speed = (int16_t)(sign * (int)c->speed_base);
        c->speed_cur = P->target_speed;
        t->flags &= ~0x80u;
    }
}

} // namespace

// ---- pickups -----------------------------------------------------------------------------------

// spell_dropped_update_46ae0
int spell_dropped_update(Thing *t, int type, int state) {
    if (t->health != 0) {
        t->health--;
        if (t->health == 0) {
            thing_mark_delete(t);
            return 0;
        }
    }
    Pos *pos = thing_pos(t);
    if (pos_sink_or_follow_ground(pos, (int16_t)terrain_height_at(pos), 0, 0, -0x80) == -1) {
        thing_mark_delete(t);                               // sunk in the water
        return 0;
    }
    if (t->tick & 3) return 0;
    const int id = (int8_t)t->type;
    if (id < 0 || id >= 24) return 0;                       // (the original indexes P+0x2a4 unchecked)

    for (uint32_t pi = g_cfg->player_list; pi != 0 && pi < (uint32_t)thing_pool_slots(); pi = thing_at(pi)->next) {
        Thing *p = thing_at(pi);
        // port (mode runs; user, 2026-10-10: the computer wizards mostly used fireball): a computer wizard at the
        // pickup - touching it, or within 2 cells of the one it flew to (ai_goal_fetch_spell; it stops a cell short) -
        // learns the spell a second later through its own countdown. The pickup stays as it is.
        if (p->type == 1 && mode_active() && p->health >= 0) {
            PlayerBlock *Q = block(p);
            if (Q->spell_thing[id] == 0 && Q->ai_want_spell[id] == 0 && Q->ai_allowed[id] == 1 &&
                (thing_collide(p, t) || (Q->ai_mode == 4 && p->target == thing_index(t) && pos_dist_xy(thing_pos(p), pos) <= 0x200)))
                Q->ai_want_spell[id] = 25;
        }
        if (p->type != 0) continue;                         // flyers only
        if (p->health < 0) continue;
        PlayerBlock *P = block(p);
        // The local player already owns this spell: flag 1.
        if (g_state->local_player == P->player_no && !(t->flags & 1) && P->spell_thing[id] != 0) t->flags |= 1;
        if (!thing_collide(p, t)) continue;

        // A flyer reached the spell: every AI wizard that may use it and lacks it starts wanting it.
        for (uint32_t qi = g_cfg->player_list; qi != 0 && qi < (uint32_t)thing_pool_slots(); qi = thing_at(qi)->next) {
            Thing *q = thing_at(qi);
            if (q->type != 1) continue;
            PlayerBlock *Q = block(q);
            if (Q->spell_thing[id] == 0 && Q->ai_want_spell[id] == 0 && Q->ai_allowed[id] == 1) Q->ai_want_spell[id] = 200;
        }

        // First empty book slot, unless the book already holds a spell of this type.
        int8_t first_empty = -1;
        bool owned = false;
        for (int i = 0; i < 24; i++) {
            int32_t idx = P->spell_slot[i];
            if (idx > 0) {                                  // pointer above the pool base
                if (idx >= thing_pool_slots()) continue;        // (unchecked in the original)
                const Thing *s = thing_at((unsigned)idx);
                if (s->cls == 0xc && s->type == (uint8_t)type) {
                    owned = true;
                    break;
                }
            } else if (first_empty == -1) {
                first_empty = (int8_t)i;
            }
        }
        if (owned) continue;
        if (first_empty == -1) continue;

        if (t->flags & 0x40000) t->mana_cost = 0;           // a sealed spell needs no castle mana
        sound_request(idx16(p), -1, 0x12);
        t->flags |= 1;
        t->caster = thing_index(p);
        t->state = (uint8_t)state;
        P->spell_slot[first_empty] = thing_index(t);
        P->slot_left = (int16_t)first_empty;                // the new spell goes into the left hand
        for (int k = 0; k < 10; k++) {
            if ((int8_t)P->hotkey_slot[k] == -1) {
                P->hotkey_slot[k] = (uint8_t)first_empty;
                break;
            }
        }
        return 1;
    }
    return 0;
}

// spell_phase2_pickup_46dd0
void spell_phase2_pickup(Thing *t) {
    g_spell_pickup_keep_flags = 0;
    if (!spell_dropped_update(t, (int8_t)t->type, (int8_t)(t->state - 2))) return;
    // call [0x962b6 + type * 14]: the class-12 Table B handler, without the enabled / index tests.
    ThingCreateFn fn = thing_create_fn(0xc, (int8_t)t->type);
    Thing *n = fn ? fn(thing_pos(t)) : nullptr;
    if (!n) return;
    n->state = (uint8_t)(n->state + 2);
    if (g_spell_pickup_keep_flags != 0) n->flags |= (t->flags & 0x40000) | 1;
    if (n->flags & 0x40000) n->sprite = 0x118;
}

// spell_dropped_dispatch_46e50
int spell_dropped_dispatch(Thing *t) {
    return spell_dropped_update(t, (int8_t)t->type, (int8_t)(t->state - 1));
}

// ---- cast helpers ------------------------------------------------------------------------------

// What a wizard's castle holds for the spells' requirement (Thing.mana_cost): the castle's own mana, 0 without a
// castle. port (mode runs): plus the mana of the town buildings the wizard has claimed. That mana counts in his
// total but never reaches the castle (the balloons fetch balls only), so a wizard living off claimed towns had a
// level-7 castle holding nothing and could cast none of the big spells (user, 2026-10-11).
int32_t spell_castle_store(const Thing *caster) {
    const PlayerBlock *P = block(caster);
    if (P->castle == 0) return 0;
    int64_t store = thing_ref(P->castle)->mana;
    if (mode_active())
        for (uint32_t i = g_cfg->wizard_list; i != 0 && i < (uint32_t)thing_pool_slots(); i = thing_at(i)->next)
            if (thing_at(i)->mana_owner == thing_index(caster)) store += thing_at(i)->mana;
    return store > 0x7fffffff ? 0x7fffffff : (int32_t)store;
}

// spell_can_cast_46e70
int spell_can_cast(Thing *spell, Thing *caster) {
    PlayerBlock *P = block(caster);
    bool ok = caster->mana >= 0 && caster->health >= 0;
    if (ok && spell->mana_cost != 0) {
        // The spell draws on the castle: the caster needs one that holds mana_cost.
        if (P->castle == 0 || spell->mana_cost > spell_castle_store(caster)) ok = false;
    }
    if (ok) {
        // Only the start of a cast (cast_ticks == duration) needs the mana.
        if (caster->mana >= spell->mana_total || spell->cast_ticks != spell->duration) return 1;
    }
    sound_request(0, P->player_no, 0x1d);
    return 0;
}

// spell_charge_mana_46f20
int spell_charge_mana(Thing *spell, Thing *caster) {
    if (spell->cast_ticks == spell->duration) {
        if (caster->mana_cost < 0) caster->mana_cost -= spell->mana_total;
        else                       caster->mana_cost = -spell->mana_total;
        return 1;
    }
    if (spell->cast_ticks != 0 && caster->mana_cost > 0) caster->mana_cost = 0;
    return 0;
}

// spell_projectile_origin_46f90
void spell_projectile_origin(Thing *caster, Thing *proj) {
    Pos pos = *thing_pos(proj);
    if (caster->flags & 0x300) {
        // 0x100 = left hand: 0x200 to the left of the heading; 0x200 = right hand (tested second).
        unsigned yaw = (caster->flags & 0x100) ? ((unsigned)caster->yaw - 0x200u) & 0x7ffu
                                               : ((unsigned)caster->yaw + 0x200u) & 0x7ffu;
        math_rotate_offset(&pos, (int)yaw, 0, 0x100);
        if ((int16_t)terrain_height_at(&pos) > pos.z) pos = *thing_pos(proj);
        // the projectile's segments, if it has any, follow
        for (Thing *s = thing_ref(proj->child); !is_none(s); s = thing_ref(s->child)) thing_move_to(s, &pos);
    }
    thing_move_to(proj, &pos);
}

// ---- Table A handlers --------------------------------------------------------------------------

// spell_phase1_common_472f0
void spell_phase1_common(Thing *t) { spell_dropped_dispatch(t); }

// spell_phase2_common_47300
void spell_phase2_common(Thing *t) { spell_phase2_pickup(t); }

// spell_fireball_update_47130
void spell_fireball_update(Thing *t) { spell_cast_burst(t, launch_fireball); }

// spell_heal_s3_update_47310: 5 % of the maximum health per tick, mana_total charged on every tick.
void spell_heal_s3_update(Thing *t) {
    if (t->cast_ticks <= 0) return;
    Thing *c = spell_caster(t);
    if (!is_none(c)) {
        if (!spell_can_cast(t, c) || c->health >= c->max_health || c->mana < t->mana_total) {
            t->cast_ticks = 1;
        } else {
            if (t->cast_ticks == t->duration) sound_request(idx16(c), -1, 0x19);
            c->health += (int32_t)((uint32_t)c->max_health * 5u) / 100;
            if (c->health > c->max_health) c->health = c->max_health;
            if (c->mana_cost < 0) c->mana_cost -= t->mana_total;
            else                  c->mana_cost = -t->mana_total;
        }
    }
    t->cast_ticks--;
}

// spell_speedup_update_47420
void spell_speedup_update(Thing *t) { spell_cast_speed(t, 1); }

// spell_possession_s9_update_475b0
void spell_possession_s9_update(Thing *t) { spell_cast_burst(t, launch_possession); }

// spell_shield_update_47760: caster flag 0x4000 (consumed by player_apply_hits_40b70).
void spell_shield_update(Thing *t) {
    Thing *c = spell_caster(t);
    if (is_none(c)) return;
    if (t->cast_ticks <= 0) return;
    if (spell_can_cast(t, c)) {
        c->flags |= 0x4000;
        spell_charge_mana(t, c);
    } else {
        t->cast_ticks = 1;
    }
    t->cast_ticks--;
}

// spell_beyond_sight_s15_update_477d0: only the mana; the effect is read off cast_ticks elsewhere.
void spell_beyond_sight_s15_update(Thing *t) {
    if (t->cast_ticks <= 0) return;
    Thing *c = spell_caster(t);
    if (!is_none(c)) {
        if (spell_can_cast(t, c)) spell_charge_mana(t, c);
        else                      t->cast_ticks = 1;
    }
    t->cast_ticks--;
}

// spell_earthquake_update_47840
void spell_earthquake_update(Thing *t) { spell_cast_single(t, launch_earthquake, false); }

// spell_meteor_update_479f0
void spell_meteor_update(Thing *t) { spell_cast_single(t, launch_meteor, true); }

// spell_volcano_s24_update_47b90
void spell_volcano_s24_update(Thing *t) { spell_cast_single(t, launch_volcano, true); }

// spell_crater_update_47d40
void spell_crater_update(Thing *t) { spell_cast_single(t, launch_crater, true); }

// spell_teleport_s30_update_47ef0: to the own castle and, on the next cast, back to where the wizard
// came from (Thing.home of the spell, z == 0 = no return point); without a castle 0x4000 units in a
// random direction.
void spell_teleport_s30_update(Thing *t) {
    if (t->cast_ticks <= 0) return;
    Thing *c = spell_caster(t);
    if (is_none(c)) return;
    PlayerBlock *P = block(c);
    if (spell_can_cast(t, c)) {
        if (t->cast_ticks == t->duration) {
            Thing *castle = thing_ref(P->castle);
            if (!is_none(castle)) {
                if (t->home.z == 0) {
                    t->home = *thing_pos(c);
                    thing_move_to(c, thing_pos(castle));
                    P->target_speed = 0;
                } else {
                    thing_move_to(c, &t->home);
                    t->home.z = 0;
                    P->target_speed = 0;
                }
            } else {
                t->home = *thing_pos(c);
                t->rng = mc_lcg(t->rng);
                math_rotate_offset(&t->home, (int)(t->rng & 0x7ff), 0, 0x4000);
                thing_move_to(c, &t->home);
                P->target_speed = 0;
                t->home.z = 0;
            }
            sound_request(idx16(c), -1, 0x16);
        }
        spell_charge_mana(t, c);
    } else {
        t->cast_ticks = 1;
    }
    t->cast_ticks--;
    if (t->cast_ticks == 0) P->target_speed = 0;
}

// spell_rubber_band_s33_update_480e0
void spell_rubber_band_s33_update(Thing *t) { spell_cast_single(t, launch_rubber_band, true); }

// spell_invisible_s36_update_48250: caster flag 0x20 for the length of the cast. The first tick
// zeroes P+0x14b (invulnerability timer) through the *spell's* player pointer, which is the dummy
// block for every spell Thing, so that write does nothing in practice. The final flag clear also
// runs when the spell has no caster (on the scratch Thing).
void spell_invisible_s36_update(Thing *t) {
    if (t->cast_ticks <= 0) return;
    Thing *c = spell_caster(t);
    if (!is_none(c)) {
        if (spell_can_cast(t, c)) {
            if (t->cast_ticks == t->duration) {
                block(t)->invuln_timer = 0;
                c->flags |= 0x20;
            } else if (!(c->flags & 0x20)) {
                t->cast_ticks = 1;
            }
            spell_charge_mana(t, c);
        } else {
            t->cast_ticks = 1;
        }
    }
    t->cast_ticks--;
    if (t->cast_ticks == 0) c->flags &= ~0x20u;
}

// spell_steal_mana_s39_update_482f0
void spell_steal_mana_s39_update(Thing *t) { spell_cast_single(t, launch_steal_mana, true); }

// spell_rebound_update_48490: caster flag 0x8000 while casting, cleared on every idle tick.
void spell_rebound_update(Thing *t) {
    Thing *c = spell_caster(t);
    if (is_none(c)) return;
    if (t->cast_ticks <= 0) {
        c->flags &= ~0x8000u;
        return;
    }
    if (spell_can_cast(t, c)) {
        c->flags |= 0x8000;
        spell_charge_mana(t, c);
    } else {
        t->cast_ticks = 1;
    }
    t->cast_ticks--;
}

// spell_lightning_update_48510
void spell_lightning_update(Thing *t) { spell_cast_single(t, launch_lightning, true); }

// spell_castle_s48_update_486b0: launches the castle seed (projectile 0xa) and then stays at
// cast_ticks = duration - 1 ("busy") until castle_spell_reset_charge_41310 releases it. With a
// castle the seed flies to it (impact effect 10 / 0x2b = upgrade); without one it lands 0x1000
// ahead and becomes a class 3 type 2 Thing (a new castle).
void spell_castle_s48_update(Thing *t) {
    if (t->cast_ticks <= 0) return;
    Thing *c = spell_caster(t);
    if (is_none(c)) return;
    if (!spell_can_cast(t, c)) {
        t->cast_ticks = 0;
        return;
    }
    if (t->cast_ticks != t->duration) return;
    spell_charge_mana(t, c);
    Thing *p = thing_create(thing_pos(c), 9, 0xa);
    if (!p) return;
    t->cast_ticks = (int16_t)(t->duration - 1);
    shot_add_caster_speed(p, c);
    spell_projectile_origin(c, p);
    p->damage = t->damage;
    p->owner = c->owner;
    shot_raise(p, c);
    p->mana = t->mana;
    Thing *castle = thing_ref(block(c)->castle);
    if (!is_none(castle)) {
        shot_set_impact(p, 10, 0x2b);
        p->target = thing_index(castle);
    } else {
        shot_target_ahead(p, c, 0, 0x1000);
        int ground = terrain_height_at(&p->home);
        shot_set_impact(p, 3, 2);
        p->home.z = (int16_t)ground;
    }
    shot_take_aim(p, c);
    shot_copy_angles(p, c);
    sound_request(idx16(p), -1, 0xf);
}

// spell_skeleton_s51_update_488a0
void spell_skeleton_s51_update(Thing *t) { spell_cast_single(t, launch_skeleton, true); }

// spell_thunderbolt_s54_update_48a70
void spell_thunderbolt_s54_update(Thing *t) { spell_cast_single(t, launch_thunderbolt, true); }

// spell_mana_magnet_s57_update_48c20
void spell_mana_magnet_s57_update(Thing *t) { spell_cast_burst(t, launch_mana_magnet); }

// Hidden Worlds' Fire Wall launch (HIDDEN.EXE 0x58270): one homing shot aimed 0x2800 ahead, sound 0xf;
// the statements in HIDDEN's order.
void launch_fire_wall_hidden(Thing *t, Thing *c, const Pos *pos) {
    Thing *p = thing_create(pos, 9, 0x10);
    if (!p) return;
    shot_add_caster_speed(p, c);
    spell_projectile_origin(c, p);
    shot_set_impact(p, 10, 0x35);
    p->owner = c->owner;
    shot_raise(p, c);
    p->mana = t->mana;
    p->damage = t->damage;
    shot_take_aim(p, c);
    shot_target_ahead(p, c, c->pitch, 0x2800);
    shot_copy_angles(p, c);
    sound_request(idx16(p), -1, 0xf);
}

// spell_fire_wall_s60_update_48de0. Hidden Worlds: a single launch on the first cast tick and mana
// charged every tick (spell_cast_single), no burst loop.
void spell_fire_wall_s60_update(Thing *t) {
    if (world_hidden()) spell_cast_single(t, launch_fire_wall_hidden, true);
    else                spell_cast_burst(t, launch_fire_wall);
}

// spell_reverse_speed_s63_update_48fa0
void spell_reverse_speed_s63_update(Thing *t) { spell_cast_speed(t, -1); }

// spell_update_shared_49140: the cast handler of spell 22; Table A also binds it to state 68, the
// *phase 2* slot of that spell, so a level pickup of spell 22 can never be picked up.
// The 1995 Table A (class 12 record 68 at CD 0x98790, same in HIDDEN) binds state 68 to the phase-2 pickup handler
// (CD 0x56260 = spell_phase2_common), so there spell 22 can be picked up.
void spell_update_shared(Thing *t) {
    if (t->state == 0x44 && engine1995()) { spell_phase2_common(t); return; }
    spell_cast_burst(t, launch_smart_bomb);
}

// spell_mini_fireball_update_492e0
void spell_mini_fireball_update(Thing *t) { spell_cast_burst(t, launch_fireball); }

void spells_register_handlers() {
    thing_register_update(0x47130, spell_fireball_update);
    thing_register_update(0x472f0, spell_phase1_common);
    thing_register_update(0x47300, spell_phase2_common);
    thing_register_update(0x47310, spell_heal_s3_update);
    thing_register_update(0x47420, spell_speedup_update);
    thing_register_update(0x475b0, spell_possession_s9_update);
    thing_register_update(0x47760, spell_shield_update);
    thing_register_update(0x477d0, spell_beyond_sight_s15_update);
    thing_register_update(0x47840, spell_earthquake_update);
    thing_register_update(0x479f0, spell_meteor_update);
    thing_register_update(0x47b90, spell_volcano_s24_update);
    thing_register_update(0x47d40, spell_crater_update);
    thing_register_update(0x47ef0, spell_teleport_s30_update);
    thing_register_update(0x480e0, spell_rubber_band_s33_update);
    thing_register_update(0x48250, spell_invisible_s36_update);
    thing_register_update(0x482f0, spell_steal_mana_s39_update);
    thing_register_update(0x48490, spell_rebound_update);
    thing_register_update(0x48510, spell_lightning_update);
    thing_register_update(0x486b0, spell_castle_s48_update);
    thing_register_update(0x488a0, spell_skeleton_s51_update);
    thing_register_update(0x48a70, spell_thunderbolt_s54_update);
    thing_register_update(0x48c20, spell_mana_magnet_s57_update);
    thing_register_update(0x48de0, spell_fire_wall_s60_update);
    thing_register_update(0x48fa0, spell_reverse_speed_s63_update);
    thing_register_update(0x49140, spell_update_shared);
    thing_register_update(0x492e0, spell_mini_fireball_update);
}
