// Castles and balloons of carpet.exe (class 3 states 4..10, 0x41290..0x42960). Translated from the
// disassembly (python tools/analysis/img.py dis 41290 42a20); the original name and address of every
// function is in the comment on its definition. Report: docs/analysis/port_castle.md.
//
// Already ported elsewhere and called from here: castle_spell_reset_charge_41310 (spatial.cpp),
// castle_apply_level_stats_42170 / castle_set_level_stats_42200 / castle_spell_set_capacity_42370 /
// player_take_damage_42770 (player.cpp), thing_set_castle_extents_353f0 / castle_crush_wizards_118c0 /
// castle_footprint_clear_11980 (level_features.cpp).
#include "castle.h"
#include "scenery.h"
#include "player.h"
#include "level_features.h"
#include "mc_math.h"
#include "world_set.h"
#include "mode.h"
#include "mode_rules.h"
#include <cstring>

// The original reaches the owner's player block as things[(i16)castle.owner].player (`movsx` of
// +0x18, * 0xa4, + 0x7503), never through the castle's own +0xa0 (which stays the dummy block).
static inline Thing *owner_thing(const Thing *t) {
    return thing_at(thing_wrap((unsigned)(int16_t)t->owner));
}
static inline PlayerBlock *owner_block(const Thing *t) { return player_block(owner_thing(t)); }
// A thing index read from a u16 field and multiplied by 0xa4 unchecked in the original.
static inline Thing *thing_u16(uint16_t idx) { return thing_at(thing_wrap(idx)); }

// The jump table at 0x41978 (castle_manage_balloons_and_guards_419a0): balloons / guards by level.
int castle_level_balloons(unsigned level) {
    static const uint8_t n[8] = {0, 1, 1, 1, 2, 2, 3, 3};
    return level < 8 ? n[level] : 0;
}
int castle_level_guards(unsigned level) {
    static const uint8_t n[8] = {0, 0, 0, 4, 6, 0xe, 0x12, 0x22};
    return level < 8 ? n[level] : 0;
}

// player_update_shared_42520 / player_flyer8_s10_update_42760 / class8_update_shared_42960: `ret`.
void castle_update_none(Thing *) {}

// castle_find_free_mana_ball_41290(from, excl_a, excl_b)
Thing *castle_find_free_mana_ball(const Thing *from, const Thing *excl_a, const Thing *excl_b) {
    // Round 11: the original compares the int32 square unsigned (a wrapped negative square at 128 cells is
    // "far"); in a wider world the int64 square is exact and compared as it is.
    const bool wide = world_wide();
    uint64_t best = wide ? UINT64_MAX : 0xffffffffu;
    Thing *found = nullptr;
    for (uint32_t i = g_cfg->mana_ball_list; i != 0 && i < (uint32_t)thing_pool_slots(); i = thing_at(i)->next) {
        Thing *b = thing_at(i);
        if (b->type != 0x27) continue;
        if ((int32_t)b->mana_owner != (int32_t)(int16_t)from->owner) continue;   // zero- vs sign-extended
        if (b == excl_a || b == excl_b) continue;
        const int64_t sq = pos_dist_sq_xyz(thing_pos(from), thing_pos(b));
        const uint64_t d = wide ? (uint64_t)sq : (uint64_t)(uint32_t)sq;
        if (d < best) {                                  // jae: unsigned
            found = b;
            best = d;
        }
    }
    return found;
}

// castle_take_damage_42460: consumes damage slot 0 and the upgrade request in slot 5.
int castle_take_damage(Thing *t) {
    int result = 0;
    if (t->health < 0) return 2;
    if (t->damage_slots[0].attacker != 0) {
        t->health -= t->damage_slots[0].amount;
        if (t->health < 0) {
            t->killer = t->damage_slots[0].attacker;
            t->damage_slots[0].attacker = 0;
            return 2;
        }
        // (the original pushes the amount and the thing here for a call that was compiled out)
        t->damage_slots[0].attacker = 0;
        t->damage_slots[0].amount = 0;
        result = 1;
        owner_block(t)->castle_hit_flash = 4;            // P+0x187
    }
    // Slot 5 (+0x7c): the castle spell's effect (27d20) writes the owner's index when the owner
    // casts Castle on his own castle.
    if ((int32_t)t->damage_slots[5].attacker == (int32_t)(int16_t)t->owner) {
        if (t->aux < 7) t->flags |= 0x40;
        t->damage_slots[5].attacker = 0;
    }
    return result;
}

// castle_spill_mana_41720: mana above the capacity (everything at level 0) leaves as up to 32 mana
// balls thrown 0xf00..0x22ff units away.
void castle_spill_mana(Thing *t) {
    int32_t excess = 0;
    PlayerBlock *P = owner_block(t);
    if (t->mana + P->mana_in_transit > t->mana_total) excess = t->mana - t->mana_total;
    if (t->aux == 0) excess = t->mana;
    if (excess <= 0) return;
    int32_t n = excess / 1000;
    int32_t avail = thing_free_count();
    if ((int16_t)avail == 0) {
        models_initialise();
        avail = thing_free_count();
        n = 8;
        g_state->active_top = -1;
        if ((int16_t)avail == 0) return;
    }
    if (n < 1) n = 1;
    if (n > 0x20) n = 0x20;
    if ((int16_t)avail < 0) avail = 0;
    if ((int32_t)(int16_t)avail > n) avail = n;
    const int16_t count = (int16_t)avail;
    if (count == 0) return;                              // (idiv by zero in the original; unreachable)
    int32_t per = excess / count;
    for (int32_t i = 0; (int32_t)count > i; i++) {
        g_pos_scratch = *thing_pos(t);
        Thing *ball = thing_create(&g_pos_scratch, 10, 0x27);
        if (!ball) continue;
        ball->mana = per;
        ball->mana_owner = t->owner;
        ball->rng = mc_lcg(ball->rng);
        ball->home.x = 0;
        ball->home.y = 0;
        ball->speed_cur = (int16_t)(ball->rng % 0x30u + 0x10);
        int32_t above = (int32_t)t->z - (int32_t)(int16_t)terrain_height_at(thing_pos(t));
        ball->z_vel = (int16_t)((0x400 - above) / 8);    // signed divide, toward zero
        t->rng = mc_lcg(t->rng);
        int dist = (int16_t)(t->rng % 0x1400u + 0xf00);
        t->rng = mc_lcg(t->rng);
        math_rotate_offset(&g_pos_scratch, (int)(t->rng & 0x7ff), 0, dist);
        thing_move_to(ball, &g_pos_scratch);
        t->mana -= ball->mana;
        excess -= ball->mana;
        if (excess < per) per = excess;
    }
    if (!engine1995()) return;
    // 1995 (CD 0x4735e..0x473ca): four effects 0x36 (state 0x3b) around the castle, each at a random
    // yaw and distance 0x1900, owned by the castle's owner.
    for (int k = 0; k < 4; k++) {
        g_pos_scratch = *thing_pos(t);
        Thing *e = thing_create(&g_pos_scratch, 10, 0x36);
        if (!e) continue;
        e->owner = t->owner;
        t->rng = mc_lcg(t->rng);
        math_rotate_offset(&g_pos_scratch, (int)(t->rng & 0x7ff), 0, 0x1900);
        thing_move_to(e, &g_pos_scratch);
    }
}

// castle_manage_balloons_and_guards_419a0
void castle_manage_balloons_and_guards(Thing *t) {
    Thing *owner = owner_thing(t);
    const unsigned level = (uint16_t)t->aux;
    const int16_t balloons = (int16_t)castle_level_balloons(level);
    const int16_t guards = (int16_t)castle_level_guards(level);

    int16_t i = 0;
    for (; i < balloons; i++) {
        PlayerBlock *P = player_block(owner);
        P->balloon_mana = 0;                             // P+0x126, cleared on every iteration
        Thing *b = thing_at(thing_wrap(P->balloons[i]));
        if (b == thing_at(0)) {
            // empty slot: a new balloon in the player's colour
            b = thing_create(thing_pos(t), 3, 3);
            if (b) {
                b->owner = t->owner;
                b->sprite = (uint16_t)(b->sprite + (uint16_t)player_block(owner)->player_no);
                b->mana_owner = t->owner;
                player_block(owner)->balloons[i] = thing_index(b);
                b->state = 9;
            }
            continue;
        }
        if (b->health < 0) {
            if (g_hook_thing_drop_mana_ball) g_hook_thing_drop_mana_ball(b);   // thing_drop_mana_ball_25fe0
            thing_mark_delete(b);
            player_block(owner)->balloons[i] = 0;
            continue;
        }
        const int32_t stored = t->mana + P->mana_in_transit;
        const uint16_t castle_idx = thing_index(t);
        if (stored >= t->mana_total) {
            b->target = castle_idx;                      // castle full: come home
        } else if ((int)t->tick % (int)balloons == 0 && b->state == 9) {
            b->target = castle_idx;
            if (b->mana_total > b->mana) {
                // Room in the hold: the nearest own mana ball the other two balloons are not after.
                // An empty slot reads the scratch Thing's target, zeroed first.
                thing_at(0)->target = 0;
                PlayerBlock *Pb = player_block(owner);
                Thing *excl_b = thing_u16(thing_u16(Pb->balloons[(i + 2) % 3])->target);
                Thing *excl_a = thing_u16(thing_u16(Pb->balloons[(i + 1) % 3])->target);
                Thing *ball = castle_find_free_mana_ball(b, excl_a, excl_b);
                if (ball) b->target = thing_index(ball);
            }
        }
        player_block(owner)->balloon_mana += b->mana;            // P+0x126
        player_block(owner)->balloon_total += b->mana_total;     // P+0x122 (never cleared here)
    }
    // Balloons beyond the level's allowance drop their cargo and go.
    for (; i < 3; i++) {
        PlayerBlock *P = player_block(owner);
        Thing *b = thing_at(thing_wrap(P->balloons[i]));
        if (b == thing_at(0)) continue;
        if (g_hook_thing_drop_mana_ball) g_hook_thing_drop_mana_ball(b);
        thing_mark_delete(b);
        player_block(owner)->balloons[i] = 0;
    }

    // Guards (creature type 0xf): one new guard per 0x10 castle updates into the first free slot.
    if (t->z_vel > 0) t->z_vel--;
    for (int16_t g = 0; g < guards; g++) {
        PlayerBlock *P = player_block(owner);
        Thing *gt = thing_at(thing_wrap(P->guards[g]));
        if (gt != thing_at(0)) {
            if (gt->cls == 5 && gt->type == 0xf && gt->state != 0x5f) continue;
            P->guards[g] = 0;                            // dead (state 0x5f) or the slot was reused
            t->z_vel = 0x10;
            continue;
        }
        if (t->z_vel != 0) continue;
        gt = thing_create(thing_pos(t), 5, 0xf);
        if (!gt) continue;
        t->z_vel = 0x10;
        gt->owner = t->owner;
        gt->mana_owner = t->owner;
        player_block(owner)->guards[g] = thing_index(gt);
        gt->yaw = 0x200;
        gt->target_yaw = gt->yaw;
        g_pos_scratch = *thing_pos(gt);
        g_pos_scratch.x = g_pos_scratch.x + 0x80;              // WCoord wraps (the original's u16 store)
        g_pos_scratch.y = g_pos_scratch.y + 0x280;
        g_pos_scratch.z = (int16_t)terrain_height_at(&g_pos_scratch);
        thing_move_to(gt, &g_pos_scratch);
    }
}

// Shared body of castle_spawn_build_effect_2a_41610 / castle_spawn_build_effect_29_41670.
static void castle_spawn_build_effect(Thing *t, int type, int next_step) {
    Thing *e = thing_create(&t->home, 10, type);
    if (!e) return;
    e->castle_size = (uint8_t)t->aux;
    e->owner = t->owner;
    e->caster = thing_index(t);
    t->cast_ticks = (int16_t)next_step;
}
// castle_spawn_build_effect_2a_41610: the terrain raiser (effect 0x2a, state 0x2c) for the current level.
void castle_spawn_build_effect_2a(Thing *t) { castle_spawn_build_effect(t, 0x2a, 4); }
// castle_spawn_build_effect_29_41670: effect 0x29 (state 0x2b).
void castle_spawn_build_effect_29(Thing *t) { castle_spawn_build_effect(t, 0x29, 6); }

// castle_begin_build_stage_41f00: the castle grows one level; the terrain raiser (created through
// its constructor effect_create_type42_39a50 directly, without the Table B checks) builds it.
void castle_begin_build_stage(Thing *t) {
    ThingCreateFn create = thing_create_fn(10, 0x2a);
    Thing *e = create ? create(&t->home) : nullptr;
    if (!e) return;
    sound_request((int16_t)thing_index(t), -1, 0xa);
    t->flags &= ~0x40u;
    t->aux++;
    t->state = 5;
    t->cast_ticks = 4;
    thing_set_castle_extents(t, t->aux);
    castle_set_level_stats(t);
    PlayerBlock *P = owner_block(t);
    const uint16_t idx = thing_index(t);
    P->castle = idx;
    P->castle_level = (uint16_t)t->aux;   // 16-bit store into P+0x1a0
    e->caster = idx;
    e->owner = t->owner;
    e->spell_flags = 0;
    e->castle_size = (uint8_t)t->aux;
    e->flags |= 0x10000;                                 // or byte [e+0x12], 1
}

// castle_build_seq_set_done_42000 (no caller in the retail image; the build effects write step 2 themselves)
void castle_build_seq_set_done(Thing *t) { t->cast_ticks = 2; }

// castle_collapse_level_42010: a destroyed castle loses one level. A tenth of the capacity is thrown
// out as mana balls, the footprint of the lost level collapses (the class-10 state 0x35 handler
// effect_type51_s53_update_27930 run directly on the scratch Thing things[0]); at level 0 the castle
// is gone.
void castle_collapse_level(Thing *t) {
    if (t->aux > 0) {
        const int32_t tenth = (t->mana_total * 10) / 100;
        t->mana_total -= tenth;
        castle_spill_mana(t);
        t->mana_total += tenth;
        const uint16_t idx = thing_index(t);
        sound_request((int16_t)idx, -1, 0x1e);
        Thing *s0 = thing_at(0);
        s0->x = t->home.x; s0->y = t->home.y; s0->z = t->home.z;
        s0->castle_size = (uint8_t)t->aux;
        s0->owner = t->owner;
        s0->type = 0;
        s0->aux = 0;
        s0->caster = idx;
        ThingUpdateFn collapse = thing_update_fn(10, 0x35);
        if (collapse) collapse(s0);
        t->aux--;
        thing_set_castle_extents(t, t->aux);
        castle_set_level_stats(t);
        castle_spell_reset_charge(t, 1);
    }
    if (t->aux == 0) {
        castle_spell_reset_charge(t, 0);
        if (mode_active() && g_hook_mode_castle_gone) g_hook_mode_castle_gone(t);   // port (round 12): match stats
        owner_block(t)->castle = 0;
        thing_mark_delete(t);
    }
}

// player_flyer1_s4_update_413a0 (class 3 state 4): the active castle.
void castle_active_update(Thing *t) {
    const uint16_t timer = (uint16_t)t->duration;        // +0x32: set to 0x1e by a quake hit (11450), 5 after a collapse
    if (timer != 0) {
        if (timer == 1) {                                // timer ran out: rebuild the terrain (sequencer step 3)
            t->state = 5;
            t->cast_ticks = 3;
            t->duration = 0;
            return;
        }
        t->duration = (int16_t)(timer - 1);
        castle_spell_reset_charge(t, 1);
        t->z = (int16_t)terrain_height_at(thing_pos(t));
        return;
    }
    const int hit = castle_take_damage(t);
    if (hit == 2) {
        t->state = 6;
    } else if (t->flags & 0x40) {                        // upgrade requested
        t->cast_ticks = 0;
        t->state = 5;
    }
    t->z = (int16_t)terrain_height_at(thing_pos(t));
    t->mana_owner = t->owner;
    if (t->tick & 1) return;
    castle_spill_mana(t);
    thing_set_castle_extents(t, t->aux);
    castle_manage_balloons_and_guards(t);
    if (t->mana >= t->mana_total) return;
    // Take in one own mana ball that touches the castle.
    for (uint32_t i = g_cfg->mana_ball_list; i != 0 && i < (uint32_t)thing_pool_slots(); i = thing_at(i)->next) {
        Thing *b = thing_at(i);
        if (b->type != 0x27) continue;
        if ((int32_t)b->mana_owner != (int32_t)(int16_t)t->owner) continue;
        if (!thing_collide(t, b)) continue;
        t->mana += b->mana;
        thing_mark_delete(b);
        return;
    }
}

// player_flyer2_s5_update_41500 (class 3 state 5): the build / upgrade sequencer on cast_ticks
// (+0x30); jump table at 0x414e0.
void castle_build_update(Thing *t) {
    switch ((uint16_t)t->cast_ticks) {
    case 0:                                              // start: site check, then grow one level
        castle_crush_wizards(t);
        if (t->aux != 0 && (uint8_t)castle_footprint_clear(t) == 0) {
            t->cast_ticks = 2;                           // no room: back to the active state
            t->flags &= ~0x40u;
            return;
        }
        if (!(t->flags & 2)) {                           // once: the owner's colour
            const uint16_t player_no = (uint16_t)owner_block(t)->player_no;
            t->flags |= 2;
            t->sprite = (uint16_t)(t->sprite + player_no);
        }
        castle_begin_build_stage(t);
        return;
    case 2:                                              // done
        t->state = 4;
        castle_spell_reset_charge(t, 0);
        t->cast_ticks = 0;
        return;
    case 3:                                              // rebuild after a quake / collapse
        castle_spell_reset_charge(t, 1);
        castle_spawn_build_effect_2a(t);
        return;
    case 5:
        castle_spell_reset_charge(t, 1);
        t->z = (int16_t)terrain_height_at(thing_pos(t));
        castle_spawn_build_effect_29(t);
        return;
    case 1: case 4: case 6:                              // waiting for the build effect
        t->z = (int16_t)terrain_height_at(thing_pos(t));
        return;
    default:
        return;
    }
}

// player_respawn_start_416d0 (class 3 state 6): the castle was destroyed (health < 0). Despite the
// name this is not about the wizard: the castle loses a level and returns to state 4 with a
// 5-update rebuild timer; with an exhausted pool it only returns to state 4 and comes back here.
void castle_destroyed_update(Thing *t) {
    if (thing_free_count() == 0) {
        t->state = 4;
        return;
    }
    castle_collapse_level(t);
    t->state = 4;
    castle_spill_mana(t);
    castle_manage_balloons_and_guards(t);
    t->cast_ticks = 0;
    t->duration = 5;
}

// balloon_update_42530 (class 3 state 9): fly to the target (+0x92) the castle assigned - a mana
// ball to swallow or the castle to unload at.
void balloon_update(Thing *t) {
    Thing *tgt = thing_u16(t->target);
    if (tgt != thing_at(0)) {
        g_pos_scratch = *thing_pos(t);
        t->yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(tgt));
        bool fly = true;                                 // math_rotate_offset by speed_cur along yaw / pitch
        if (tgt->cls == 10) {
            if ((int32_t)tgt->mana_owner != (int32_t)(int16_t)t->owner) {
                fly = false;
            } else {
                const int32_t dist = pos_dist_xy(&g_pos_scratch, thing_pos(tgt));
                if (dist <= 0x400) {
                    tgt->flags |= 0x40;                  // the ball comes to meet the balloon
                    tgt->target = thing_index(t);
                    if (thing_collide(tgt, t)) {
                        t->mana += tgt->mana;
                        t->target = 0;
                        t->mana_owner = tgt->mana_owner;
                        t->health = t->max_health;
                        thing_mark_delete(tgt);
                    }
                } else {
                    tgt->flags &= ~0x40u;
                }
                if (dist <= (int32_t)t->speed_cur) {     // closer than one step: sit on it
                    g_pos_scratch.x = tgt->x;
                    g_pos_scratch.y = tgt->y;
                    fly = false;
                }
            }
        } else if (tgt->cls == 3) {
            const int32_t reach = (int32_t)t->speed_cur * (int32_t)tgt->aux;
            const int32_t dist = pos_dist_xy(&g_pos_scratch, thing_pos(tgt));
            if ((uint32_t)dist <= (uint32_t)reach) {     // ja: unsigned
                fly = false;
                const int32_t clear = mc_move_desc(t->desc)->clear_hi;          // desc+0xc
                const int32_t ground = (int16_t)terrain_height_at(&g_pos_scratch);
                if ((int32_t)g_pos_scratch.z <= ground + clear && tgt->aux > 0) {
                    // low over a standing castle: unload
                    g_pos_scratch.x = tgt->x;
                    g_pos_scratch.y = tgt->y;
                    tgt->mana += t->mana;
                    t->mana = 0;
                    t->mana_owner = t->owner;
                    t->health = t->max_health;
                }
            }
        }
        if (fly) math_rotate_offset(&g_pos_scratch, t->yaw, t->pitch, t->speed_cur);
        const MoveDesc *d = mc_move_desc(t->desc);
        const int ground = (int16_t)terrain_height_at(&g_pos_scratch);
        pos_follow_ground(&g_pos_scratch, ground, d->clear_hi, d->clear_lo, d->z_step);   // desc +0xc, +0xa, +0xe
        thing_move_to(t, &g_pos_scratch);
    }
    player_take_damage(t);
}

void castle_register_handlers() {
    thing_register_update(0x413a0, castle_active_update);
    thing_register_update(0x41500, castle_build_update);
    thing_register_update(0x416d0, castle_destroyed_update);
    thing_register_update(0x42520, castle_update_none);
    thing_register_update(0x42530, balloon_update);
    thing_register_update(0x42760, castle_update_none);
    thing_register_update(0x42960, castle_update_none);
    // engine_init (engine.cpp, round-3 state) calls castle_register_handlers() but has no call for
    // the scenery / switch half of this task; until it has one, bind those here as well (binding
    // twice is harmless).
}
