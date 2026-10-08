// Scenery (class 2, 0x43ba0..0x43e80) and switches (class 11, 0x4a2a0..0x4a940) of carpet.exe.
// Translated from the disassembly (python tools/analysis/img.py dis 43ba0 43ea0 / dis 4a2a0 4a9a0);
// the original name and address of every function is in the comment on its definition.
// Report: docs/analysis/port_castle.md.
#include "scenery.h"
#include "mc_math.h"
#include "mode.h"
#include <cstring>

// ---- class 2: scenery ------------------------------------------------------------------------------

// The tail the three tree handlers share: follow the ground; a tree whose cell became water
// (terrain_type_mask_at == 1) leaves a splash (effect 5) and is deleted.
static void scenery_tree_ground_check(Thing *t) {
    Pos *pos = thing_pos(t);
    t->z = (int16_t)terrain_height_at(pos);
    if (terrain_type_mask_at(pos) != 1) return;
    Thing *splash = thing_create(pos, 10, 5);
    if (splash) splash->owner = t->owner;
    thing_mark_delete(t);
}

// scenery_tree_update_43ba0 (state 0): a living tree. Damage slot 0 burns it down: at health < 0 a
// fire (effect 6) owned by the attacker's owner is lit and the tree burns (state 1) for 130..189
// ticks.
void scenery_tree_update(Thing *t) {
    t->flags |= 0x20000;                                 // or byte [t+0x12], 2: recyclable, every tick
    if (t->damage_slots[0].attacker != 0) {
        t->health -= t->damage_slots[0].amount;
        if (t->health < 0) {
            Thing *fire = thing_create(thing_pos(t), 10, 6);
            if (fire) {
                fire->owner = thing_at(thing_wrap(t->damage_slots[0].attacker))->owner;
                fire->z_vel = (int16_t)(((int32_t)t->ext_h * 3) / 4);        // flame height: 3/4 of the tree
                t->rng = mc_lcg(t->rng);
                const int32_t life = (int32_t)(t->rng % 0x3cu + 0x82);
                fire->health = life;
                t->health = life;
                t->flags &= 0xfffdfff7u;                 // not collidable ...
                t->state = 1;
                t->flags |= 0x20000;                     // ... and recyclable again
                thing_relink_cell(t, thing_pos(t));
            }
        }
        t->damage_slots[0].attacker = 0;
    }
    scenery_tree_ground_check(t);
}

// scenery_tree_s1_update_43cd0 (state 1): burning. With 0x3c ticks left the sprite becomes the
// burnt tree (0x53 -> 0xe2, 0x54 -> 0xe3) and the tree stays in state 2.
void scenery_tree_s1_update(Thing *t) {
    t->health--;
    if (t->health < 0x3c) {
        const uint16_t sprite = t->sprite;
        t->state = 2;
        if (sprite == 0x53)      thing_set_sprite(t, 0xe2);      // thing_set_sprite_small_352d0
        else if (sprite == 0x54) thing_set_sprite(t, 0xe3);
    }
    scenery_tree_ground_check(t);
}

// scenery_tree_s2_update_43d60 (state 2): the burnt tree (it never grows back).
void scenery_tree_s2_update(Thing *t) {
    scenery_tree_ground_check(t);
}

// scenery_standing_stone_update_43db0 (state 3) and scenery_badstone_update_43e60 (state 9, byte
// for byte the same code).
void scenery_standing_stone_update(Thing *t) {
    t->flags |= 0x20000;
    t->z = (int16_t)terrain_height_at(thing_pos(t));
}

// scenery_dolmen_update_43de0 (state 6): every living player Thing whose box overlaps the dolmen
// gets flag 0x1000 (the flyer treats it like being inside the own castle).
void scenery_dolmen_update(Thing *t) {
    for (unsigned p = 0; (uint16_t)p < (uint16_t)g_state->player_count && p < 8; p++) {
        Thing *pt = thing_at(thing_wrap(g_state->players[p].thing));
        if (pt->health < 0) continue;
        if (thing_collide(pt, t)) pt->flags |= 0x1000;
    }
    t->z = (int16_t)terrain_height_at(thing_pos(t));
}

// scenery_update_shared_43dd0 (states 4, 5), scenery_update_shared_43e50 (7, 8),
// scenery_update_shared_43e80 (10..17), switch_type31_s31_update_4a8a0: `ret`.
void scenery_update_none(Thing *) {}

// ---- class 11: switches ----------------------------------------------------------------------------

// The SwiId as the handlers push it for switch_activate_356e0: `movsx eax, word [t+0x18]` (an id
// >= 0x8000 therefore never matches a level record).
static inline unsigned switch_id(const Thing *t) { return (unsigned)(int32_t)(int16_t)t->owner; }

// switch_test_player_4a8b0
int switch_test_player(Thing *sw, int want) {
    if (sw->tick & 7) return 0;
    // port (Phase 4 round 11): in a mode run computer wizards (type 1) fire switches too - the generated world's
    // trigger areas are for every wizard, bots included. The original: human flyers (type 0) only.
    const int max_type = mode_active() ? 1 : 0;
    for (uint32_t i = g_cfg->player_list; i != 0 && i < (uint32_t)thing_pool_slots(); i = thing_at(i)->next) {
        Thing *p = thing_at(i);
        if (p->type > max_type) continue;
        if (thing_collide(sw, p) != want) continue;
        sound_request((int16_t)i, -1, 0x29);
        return 1;
    }
    sw->z = (int16_t)terrain_height_at(thing_pos(sw));
    return 0;
}

// switch_test_any_player_4a940
int switch_test_any_player(Thing *sw, int want) {
    for (unsigned p = 0; (uint16_t)p < (uint16_t)g_state->player_count && p < 8; p++) {
        Thing *pt = thing_at(thing_wrap(g_state->players[p].thing));
        if (thing_collide(sw, pt) == want) return 1;
    }
    return 0;
}

// Fires once: the level records with DisId == SwiId are spawned and cleared, the switch is deleted.
static void switch_once(Thing *t, int want) {
    if (!switch_test_player(t, want)) return;
    switch_activate(switch_id(t), true);
    thing_mark_delete(t);
}
// Fires repeatedly: the records are kept; after firing the switch waits until 10 of its updates
// found no player Thing in the triggering position (aux counts down only then).
static void switch_repeat(Thing *t, int want) {
    if (t->aux == 0) {
        if (!switch_test_player(t, want)) return;
        switch_activate(switch_id(t), false);
        t->aux = 10;
        return;
    }
    if (!switch_test_any_player(t, want)) t->aux--;
}

// switch_hidden_inside_s0_update_4a2a0 (state 0), switch_update_once_4a460 (state 5),
// switch_obvious_inside_s9_update_4a560 (state 9): a wizard inside the box, once.
static void switch_inside_once_update(Thing *t) { switch_once(t, 1); }
// switch_hidden_outside_s1_update_4a2d0 (state 1), switch_death_outside_s6_update_4a490 (state 6),
// switch_obvious_outside_s10_update_4a590 (state 10): a wizard outside the box, once.
static void switch_outside_once_update(Thing *t) { switch_once(t, 0); }
// switch_hidden_inside_re_s2_update_4a300 (state 2), switch_death_inside_re_s7_update_4a4c0 (state 7),
// switch_type11_s11_update_4a5c0 (state 11): inside, repeating.
static void switch_inside_repeat_update(Thing *t) { switch_repeat(t, 1); }
// switch_update_repeat_4a350 (state 3), switch_type8_s8_update_4a510 (state 8),
// switch_type12_s12_update_4a610 (state 12): outside, repeating.
static void switch_outside_repeat_update(Thing *t) { switch_repeat(t, 0); }

// switch_on_victory_s4_update_4a3a0 (state 4): when player 0 (always record 0, not the local
// player) owns a castle and his "level won" status bit is set, the switch fires (records kept),
// takes the bit back - the level goes on - and is deleted.
static void switch_on_victory_update(Thing *t) {
    Thing *pt = thing_at(thing_wrap(g_state->players[0].thing));
    const uint8_t *P = thing_player_block(pt);
    uint16_t castle;
    int16_t player_no;
    std::memcpy(&castle, P + 0x32, 2);
    if (castle == 0) return;
    std::memcpy(&player_no, P + 0x30, 2);
    if (!(g_state->players[player_no & 7].status & 2)) return;
    switch_activate(switch_id(t), false);
    thing_mark_delete(t);
    std::memcpy(&player_no, thing_player_block(pt) + 0x30, 2);
    g_state->players[player_no & 7].status &= 0xfffd;                // and byte [rec+2], 0xfd
    sound_request((int16_t)thing_index(t), -1, 0x29);
}

// switch_creature_dead_trigger_4a660
void switch_creature_dead_trigger(Thing *t, int creature_type) {
    if (creature_type != -1) {
        if ((unsigned)creature_type < 20 && g_cfg->creature_lists[creature_type] != 0) return;
    } else {
        for (int k = 0; k <= 0x10; k++) {
            if (k > 0xb && k != 0x10) continue;          // the villagers (0xc..0xf) do not count
            if (g_cfg->creature_lists[k] != 0) return;
        }
    }
    if (t->aux == 0) {
        t->aux = 0x10;
        return;
    }
    if (t->aux == 1) {
        sound_request((int16_t)thing_index(t), -1, 0x29);
        switch_activate(switch_id(t), true);
        thing_mark_delete(t);
        return;
    }
    t->aux--;
}

// switch_dragon_trigger_s13_update_4a780 (creature type 0) .. switch_type29_s29_update_4a880 (type
// 0x10), 0x10 bytes apart: `push type; push thing; call switch_creature_dead_trigger_4a660`;
// switch_creature_all_s30_update_4a890 pushes -1.
template <int Type> static void switch_creature_trigger_update(Thing *t) {
    switch_creature_dead_trigger(t, Type);
}

void scenery_register_handlers() {
    thing_register_update(0x43ba0, scenery_tree_update);
    thing_register_update(0x43cd0, scenery_tree_s1_update);
    thing_register_update(0x43d60, scenery_tree_s2_update);
    thing_register_update(0x43db0, scenery_standing_stone_update);
    thing_register_update(0x43dd0, scenery_update_none);
    thing_register_update(0x43de0, scenery_dolmen_update);
    thing_register_update(0x43e50, scenery_update_none);
    thing_register_update(0x43e60, scenery_standing_stone_update);
    thing_register_update(0x43e80, scenery_update_none);

    thing_register_update(0x4a2a0, switch_inside_once_update);
    thing_register_update(0x4a2d0, switch_outside_once_update);
    thing_register_update(0x4a300, switch_inside_repeat_update);
    thing_register_update(0x4a350, switch_outside_repeat_update);
    thing_register_update(0x4a3a0, switch_on_victory_update);
    thing_register_update(0x4a460, switch_inside_once_update);
    thing_register_update(0x4a490, switch_outside_once_update);
    thing_register_update(0x4a4c0, switch_inside_repeat_update);
    thing_register_update(0x4a510, switch_outside_repeat_update);
    thing_register_update(0x4a560, switch_inside_once_update);
    thing_register_update(0x4a590, switch_outside_once_update);
    thing_register_update(0x4a5c0, switch_inside_repeat_update);
    thing_register_update(0x4a610, switch_outside_repeat_update);
    thing_register_update(0x4a780, switch_creature_trigger_update<0>);
    thing_register_update(0x4a790, switch_creature_trigger_update<1>);
    thing_register_update(0x4a7a0, switch_creature_trigger_update<2>);
    thing_register_update(0x4a7b0, switch_creature_trigger_update<3>);
    thing_register_update(0x4a7c0, switch_creature_trigger_update<4>);
    thing_register_update(0x4a7d0, switch_creature_trigger_update<5>);
    thing_register_update(0x4a7e0, switch_creature_trigger_update<6>);
    thing_register_update(0x4a7f0, switch_creature_trigger_update<7>);
    thing_register_update(0x4a800, switch_creature_trigger_update<8>);
    thing_register_update(0x4a810, switch_creature_trigger_update<9>);
    thing_register_update(0x4a820, switch_creature_trigger_update<0xa>);
    thing_register_update(0x4a830, switch_creature_trigger_update<0xb>);
    thing_register_update(0x4a840, switch_creature_trigger_update<0xc>);
    thing_register_update(0x4a850, switch_creature_trigger_update<0xd>);
    thing_register_update(0x4a860, switch_creature_trigger_update<0xe>);
    thing_register_update(0x4a870, switch_creature_trigger_update<0xf>);
    thing_register_update(0x4a880, switch_creature_trigger_update<0x10>);
    thing_register_update(0x4a890, switch_creature_trigger_update<-1>);
    thing_register_update(0x4a8a0, scenery_update_none);
}
