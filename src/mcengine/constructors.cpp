// Table B constructors (thing "create" handlers) of carpet.exe, 0x359c0..0x3a7e0.
// Translated from the disassembly (python tools/analysis/img.py dis 0x359c0 0x3a840); the original
// name and address of every function is in the comment on its definition.
//
// Common shape: `push ebx; call thing_alloc_35560; mov ebx,eax; test eax,eax; je fail`, then field
// writes, thing_link_cell_3e250(t, pos), thing_restore_health_35080(t), thing_set_sprite_35240.
// The order of the calls, of the per-thing RNG steps (t->rng = t->rng * 0x24a1 + 0x24df) and of the
// per-type serial counter is the original's; plain field stores between two calls are grouped.
//
// Pointer fields: Thing+0x9c (descriptor pointer into the table at 0x96a10) is stored as the
// MoveDesc index (addr - 0x96a10) / 0x20; no constructor writes Thing+0xa0 (thing_alloc leaves the
// dummy player block there; player_spawn_3f360 sets the real one).
#include "settings.h"
#include "constructors.h"
#include "world_set.h"
#include "world.h"
#include "mc_math.h"
#include "gen/constructors_tables.h"
#include <cstring>
#include <utility>

namespace {

constexpr uint32_t desc_index(uint32_t addr) { return (addr - 0x96a10u) / 0x20u; }

// t->rng = t->rng * 0x24a1 + 0x24df; returns the new value.
inline uint32_t rng_next(Thing *t) { t->rng = mc_lcg(t->rng); return t->rng; }

// movsd / movsw into Thing+0x48 without touching the cell lists.
inline void set_pos_raw(Thing *t, const Pos *pos) { Pos p = *pos; t->x = p.x; t->y = p.y; t->z = p.z; }

// `flags &= 0xfffdfff7` followed by `or byte [t+0x12], 2`: not collidable, recyclable (0x20000).
inline void fx_flags(Thing *t) { t->flags = (t->flags & 0xfffdfff7u) | 0x20000u; }

// ---- shared creature fragments (inlined in every creature constructor) ---------------------------

// rng step; target_yaw = yaw = (rng & 0x7ff) - 1; pitch = target_yaw.
inline void creature_random_heading(Thing *t) {
    uint16_t a = (uint16_t)((rng_next(t) & 0x7ff) - 1);
    t->target_yaw = a;
    t->yaw = a;
    t->pitch = t->target_yaw;
}

// Per-type serial: `dl = [g_state + 0xc + type]; [..] = dl + 1; t->tick = dl`. The 17 counter bytes
// live at GameState+0xc (zeroed per level by level_load_file_3d160).
inline void creature_take_serial(Thing *t) {
    uint8_t *counter = &g_state->padc[(int8_t)t->type];
    uint8_t serial = *counter;
    *counter = (uint8_t)(serial + 1);
    t->tick = serial;
}

// timer_a = think_period - tick % think_period + 4 (staggers the first "awake" check per serial).
inline void creature_stagger_awake(Thing *t) {
    int period = (int16_t)mc_move_desc(t->desc)->think_period;
    int rem = period ? (int)t->tick % period : 0;      // the original idiv would fault on 0
    t->timer_a = (uint8_t)(period - rem + 4);
}

} // namespace

// mana_ball_update_sprite_25e20 (outside this file's address range, effects code; lives here because
// effect_create_mana_ball_39840 needs it and is exported through constructors.h): size level = first
// threshold >= mana (7 when none), sprite base by the owner's player number (0x69 + player * 8;
// 0x34 unowned / player -1). Jump table 0x25df8: [0] (player -1) = 0x34, [1..8] = 0x69 + 8 * player;
// above 8 the original leaves ECX as it is, the port keeps 0x34.
int mana_ball_sprite(const Thing *t, int *size_level) {
    int16_t level = 0;
    while (level < 7 && t->mana > g_mana_ball_thresholds[level]) level++;
    int16_t base = 0x34;
    if (t->mana_owner != 0 && t->mana_owner < thing_pool_slots() && thing_at(t->mana_owner)->cls == 3) {
        int16_t player;
        std::memcpy(&player, thing_player_block(thing_at(t->mana_owner)) + 0x30, 2);
        uint16_t sel = (uint16_t)(player + 1);
        // sel > 8: the original jumps past the jump table with ECX uninitialised.
        if (sel >= 1 && sel <= 8) base = (int16_t)(0x69 + (sel - 1) * 8);
    }
    if (size_level) *size_level = level;
    return base + level;
}

void mana_ball_update_sprite(Thing *t) {
    int level;
    const int sprite = mana_ball_sprite(t, &level);
    if ((int16_t)t->sprite != sprite) {
        thing_set_sprite(t, sprite);
        if (level != 0) thing_set_sprite_halved(t, sprite);
    }
}

namespace {

// ---- class 3: players --------------------------------------------------------------------------

// player_create_flyer1_359c0, player_create_flyer2_359e0, player_create_flyer3_35a00,
// player_create_flyer4_35a20, player_create_flyer5_35a40, player_create_flyer6_7_35a60,
// player_create_flyer8_35a80, player_create_flyer6_7_35aa0 (level models "Flyer1".."Flyer8", types
// 4..11): no thing at all, the position is stored as player N's start position
// (GameState+0x23d9 + N*6) and null is returned.
template <int N> Thing *player_create_flyer_start(const Pos *pos) {
    world_set_start_pos(N, *pos);
    return nullptr;
}

// Shared body of player_create_flyer_35ac0 / player_create_type1_35b40.
Thing *player_create_wizard(const Pos *pos, int type, uint32_t desc_addr) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = (uint8_t)type;
    t->cls = 3;
    t->type = (uint8_t)type;
    t->max_health = 10000;
    t->speed_base = 0x50;
    t->desc = desc_index(desc_addr);
    t->prop_flags = 0x1d;
    t->owner = thing_index(t);
    thing_link_cell(t, pos);
    thing_set_sprite(t, 0x2c);
    thing_restore_health(t);
    return t;
}
// player_create_flyer_35ac0 (class 3 type 0: the human-controlled wizard)
Thing *player_create_flyer(const Pos *pos) { return player_create_wizard(pos, 0, 0x96af0); }
// player_create_type1_35b40 (class 3 type 1: computer wizard)
Thing *player_create_type1(const Pos *pos) { return player_create_wizard(pos, 1, 0x96b10); }

// player_create_type2_35bc0 (class 3 type 2: castle). The position is snapped to a cell corner:
// (x_cell, y_cell) with x_cell + 1 when x_cell + y_cell is odd; z = terrain height at the
// requested position. Also leaves the requested position in the scratch DAT_000adfc4.
Thing *player_create_type2(const Pos *pos) {
    g_pos_scratch = *pos;
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 5;
    t->cls = 3;
    t->type = 2;
    t->max_health = 40000;
    t->aux = 0;
    t->prop_flags = 0x21;
    Pos p = g_pos_scratch;
    p.x = (uint16_t)(p.x >> 8);
    p.y = (uint16_t)(p.y >> 8);
    p.z = (int16_t)terrain_height_at(&g_pos_scratch);
    if (((int16_t)p.y + (int16_t)p.x) % 2 != 0) p.x++;
    p.x = (uint16_t)(p.x << 8);
    p.y = (uint16_t)(p.y << 8);
    t->home = p;
    thing_link_cell(t, &p);
    thing_restore_health(t);
    thing_set_sprite(t, 0xb1);
    return t;
}

// player_create_type3_35ca0 (class 3 type 3)
Thing *player_create_type3(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 7;
    t->cls = 3;
    t->type = 3;
    t->max_health = 10000;
    t->speed_cur = 0x30;
    t->mana_total = 10000;
    t->mana = 0;
    t->prop_flags = 1;
    t->desc = desc_index(0x96b30);
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite(t, 0xa9);
    return t;
}

// ---- classes 1, 6, 7 (types 0..3), 8: empty classes ---------------------------------------------

// class1_create_type1_35d20 .. class1_create_type10_35e40, class6_create_type0_37aa0,
// class6_create_type1_37ac0, weather_create_type0_37ae0 .. weather_create_type3_37b40,
// class8_create_type0_37be0 .. class8_create_type5_37c80: `t = thing_alloc(); if (t)
// thing_restore_health(t); return 0`. The slot stays class 0 and is off the free stack until the
// next models_initialise_354c0 rebuilds it - so each such level record shifts the index of every
// thing spawned after it, which the port has to reproduce.
Thing *create_alloc_discard(const Pos *) {
    Thing *t = thing_alloc();
    if (t) thing_restore_health(t);
    return nullptr;
}

// ---- class 2: scenery ---------------------------------------------------------------------------

// scenery_create_tree_35e60: jittered by -32..+31 inside the cell; sprite 0x53 / 0x54 at random.
Thing *scenery_create_tree(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 0;
    t->cls = 2;
    t->type = 0;
    t->aux = (int16_t)(thing_index(t) % 11);
    t->health = (int32_t)(rng_next(t) % 5000u + 2500u);     // overwritten by thing_restore_health below
    t->prop_flags = 1;
    Pos p = *pos;
    p.x = (uint16_t)((int16_t)p.x + (int32_t)(rng_next(t) & 0x3f) - 0x20);
    p.y = (uint16_t)((int16_t)p.y + (int32_t)(rng_next(t) & 0x3f) - 0x20);
    thing_link_cell(t, &p);
    thing_restore_health(t);
    // thing_set_sprite_small_352d0; Hidden Worlds (HIDDEN.EXE 0x37f80): always 0x53, no RNG draw
    thing_set_sprite(t, world_hidden() ? 0x53 : ((rng_next(t) & 1) ? 0x54 : 0x53));
    return t;
}

// scenery_create_standing_stone_35f90
Thing *scenery_create_standing_stone(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->flags &= ~8u;
    t->aux = (int16_t)(thing_index(t) % 11);
    t->state = 3;
    t->cls = 2;
    t->type = 1;
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite(t, 0x4f);          // thing_set_sprite_small_352d0
    return t;
}

// scenery_create_dolmen_36010
Thing *scenery_create_dolmen(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->flags &= ~8u;
    t->aux = (int16_t)(thing_index(t) % 11);
    t->state = 6;
    t->cls = 2;
    t->type = 2;
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite(t, 0x27);          // thing_set_sprite_small_352d0
    thing_set_extents(t, 0x400, 0x400);
    return t;
}

// scenery_create_bad_stone_360a0
Thing *scenery_create_bad_stone(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->flags &= ~8u;
    t->aux = (int16_t)(thing_index(t) % 11);
    t->state = 9;
    t->cls = 2;
    t->type = 3;
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite(t, 0x10e);         // thing_set_sprite_small_352d0
    return t;
}

// Shared body of scenery_create_2d_dome_36120 / scenery_create_2d_dome_36190.
Thing *scenery_create_dome(const Pos *pos, int type, int state) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = (uint8_t)state;
    t->cls = 2;
    t->type = (uint8_t)type;
    t->aux = (int16_t)(thing_index(t) % 11);
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite(t, 0x30);          // thing_set_sprite_small_352d0
    return t;
}
// scenery_create_2d_dome_36120 (type 4, state 0xc)
Thing *scenery_create_2d_dome_a(const Pos *pos) { return scenery_create_dome(pos, 4, 0xc); }
// scenery_create_2d_dome_36190 (type 5, state 0xf)
Thing *scenery_create_2d_dome_b(const Pos *pos) { return scenery_create_dome(pos, 5, 0xf); }

// ---- class 5: creatures -------------------------------------------------------------------------

// The 16 body segments of the dragon / worm: a copy of the head (taken before the head is linked),
// chained through parent (+0x34) / child (+0x36), state 0x78, sprite base + i, tick = i.
// `mana_on_head` reproduces the one difference between the two originals: the dragon writes
// mana_total / 32 into the *head's* mana on every iteration, the worm into the segment's.
void creature_create_tail16(Thing *head, const Pos *pos, int sprite_base, bool mana_on_head) {
    Thing *prev = head;
    for (int i = 0; i <= 0xf; i++) {
        Thing *seg = thing_alloc();
        if (seg) {
            *seg = *head;
            // prev is only null after a failed allocation (pool exhausted without recyclables); the
            // original then computes a wild index and writes through a null pointer.
            seg->parent = prev ? thing_index(prev) : 0;
            if (prev) prev->child = thing_index(seg);
            seg->child = 0;
            seg->state = 0x78;
            (mana_on_head ? head : seg)->mana = head->mana_total / 32;
            seg->tick = (uint8_t)i;
            thing_set_sprite(seg, (int16_t)(sprite_base + i));
            seg->speed = (uint16_t)seg->ext_x;
            thing_link_cell(seg, pos);
            thing_restore_health(seg);
        }
        prev = seg;
    }
}

// creature_create_dragon_362d0 (type 0, state 1): head + 16 segments (sprites 0x13..0x22); needs 16
// free slots.
Thing *creature_create_dragon(const Pos *pos) {
    if (thing_free_count() < 0x10) return nullptr;
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 1;
    t->cls = 5;
    t->type = 0;
    t->speed_base = 0x50;
    t->turn_rate = 0x10;
    t->speed_cur = 0x1e;
    t->max_health = 9000;
    thing_set_mana_from_health(t);
    t->mana_total = t->mana;
    t->mana = t->mana / 2;
    creature_random_heading(t);
    t->aux = (int16_t)(thing_index(t) % 100);
    t->target_pitch = 0;
    t->speed = 0x60;
    t->prop_flags = 1;
    creature_take_serial(t);
    t->desc = desc_index(0x96b90);
    creature_stagger_awake(t);
    t->filter_cls = 3;
    creature_create_tail16(t, pos, 0x13, true);
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite(t, 0x28);
    return t;
}

// creature_create_vulture_36510 (type 1, state 7)
Thing *creature_create_vulture(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 7;
    t->cls = 5;
    t->type = 1;
    t->speed_base = 0x64;
    t->turn_rate = 0x10;
    t->max_health = 2000;
    t->speed_cur = (int16_t)(t->speed_base / 2);
    thing_set_mana_from_health(t);
    t->target_yaw = 0;
    t->yaw = t->target_yaw;
    t->pitch = t->target_yaw;
    t->aux = (int16_t)(thing_index(t) % 100);
    t->target_pitch = 0;
    t->prop_flags = 1;
    creature_take_serial(t);
    t->desc = desc_index(0x96bb0);
    t->timer_a = (uint8_t)(mc_move_desc(t->desc)->think_period + 1);
    t->filter_cls = 3;
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite(t, 0x56);
    return t;
}

// creature_create_bee_36610 (type 2, state 0xd)
Thing *creature_create_bee(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 0xd;
    t->cls = 5;
    t->type = 2;
    t->speed_base = 0x46;
    t->turn_rate = 0x1e;
    t->max_health = 3000;
    t->speed_cur = (int16_t)(t->speed_base / 2);
    thing_set_mana_from_health(t);
    creature_random_heading(t);
    t->aux = (int16_t)(thing_index(t) % 100);
    t->target_pitch = 0;
    t->damage = 0x15e;
    t->filter_cls = 3;
    t->filter_type = 0;
    t->prop_flags = 1;
    creature_take_serial(t);
    t->desc = desc_index(0x96bd0);
    creature_stagger_awake(t);
    t->filter_cls = 3;
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite(t, 3);
    thing_set_extents(t, 0x80, 0x80);
    return t;
}

// creature_create_worm_36750 (type 3, state 0x13): head + 16 segments (sprites 0x59..0x68). Unlike
// the dragon there is no free-slot check.
Thing *creature_create_worm(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 0x13;
    t->cls = 5;
    t->type = 3;
    t->speed_base = 0x40;
    t->turn_rate = 0x10;
    t->speed_cur = 0x1e;
    t->max_health = 9000;
    thing_set_mana_from_health(t);
    t->mana_total = t->mana;
    t->mana = t->mana / 2;
    creature_random_heading(t);
    t->aux = (int16_t)(thing_index(t) % 100);
    t->target_pitch = 0;
    t->speed = 0x60;
    t->prop_flags = 1;
    creature_take_serial(t);
    t->desc = desc_index(0x96bf0);
    creature_stagger_awake(t);
    t->filter_cls = 3;
    creature_create_tail16(t, pos, 0x59, false);
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite(t, 0x58);
    return t;
}

// creature_create_archer_36980 (type 4, state 0x19)
Thing *creature_create_archer(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 0x19;
    t->cls = 5;
    t->type = 4;
    t->speed_base = 0x1e;
    t->turn_rate = 0;
    t->max_health = 1000;
    t->speed_cur = t->speed_base;
    thing_set_mana_from_health(t);
    creature_random_heading(t);
    t->aux = (int16_t)(thing_index(t) % 100);
    t->target_pitch = 0;
    t->damage = 500;
    t->prop_flags = 1;
    creature_take_serial(t);
    t->desc = desc_index(0x96c10);
    creature_stagger_awake(t);
    t->filter_cls = 3;
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite(t, 0);
    thing_set_extents(t, 0x80, 0x80);
    return t;
}

// creature_create_crab_36b30 (type 5, state 0x1f)
Thing *creature_create_crab(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->speed_base = 0x1e;
    t->speed_cur = t->speed_base;
    t->state = 0x1f;
    t->cls = 5;
    t->type = 5;
    t->turn_rate = 3;
    t->max_health = 5000;
    t->mana = 500;
    creature_random_heading(t);
    t->mana_total = 12000;
    t->target_pitch = 0;
    t->aux = (int16_t)(thing_index(t) % 100);
    t->damage = 500;
    t->prop_flags = 1;
    creature_take_serial(t);
    t->desc = desc_index(0x96c30);
    creature_stagger_awake(t);
    t->filter_cls = 3;
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite(t, 0xb9);
    thing_set_extents(t, 0x80, 0x80);
    t->health = 5000;
    return t;
}

// creature_create_kraken_36c80 (type 6, state 0x25): head (sprite 0x31) + 2 segments (sprites 0x32,
// 0xc1); needs 16 free slots.
Thing *creature_create_kraken(const Pos *pos) {
    if (thing_free_count() < 0x10) return nullptr;
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 0x25;
    t->cls = 5;
    t->type = 6;
    t->speed_base = 0x50;
    t->turn_rate = 0x10;
    t->speed_cur = 0x1e;
    t->max_health = 9000;
    thing_set_mana_from_health(t);
    t->mana_total = t->mana;
    t->mana = t->mana / 3;
    creature_random_heading(t);
    t->aux = (int16_t)(thing_index(t) % 100);
    t->target_pitch = 0;
    t->speed = 0x60;
    t->prop_flags = 1;
    creature_take_serial(t);
    t->desc = desc_index(0x96c50);
    t->timer_a = 0x40;
    t->filter_cls = 3;
    Thing *prev = t;
    for (int i = 0; i <= 1; i++) {
        Thing *seg = thing_alloc();
        if (seg) {
            *seg = *t;
            seg->parent = prev ? thing_index(prev) : 0;     // see creature_create_tail16
            if (prev) prev->child = thing_index(seg);
            seg->child = 0;
            seg->state = 0x78;
            seg->mana = t->mana_total / 3;
            seg->tick = (uint8_t)i;
            thing_set_sprite(seg, i != 0 ? 0xc1 : 0x32);
            seg->speed = (uint16_t)(seg->ext_x << 2);
            thing_link_cell(seg, pos);
            thing_restore_health(seg);
        }
        prev = seg;
    }
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite(t, 0x31);
    return t;
}

// creature_create_troll_36f00 (type 7, state 0x2b)
Thing *creature_create_troll(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 0x2b;
    t->cls = 5;
    t->type = 7;
    t->speed_base = 0x14;
    t->turn_rate = 3;
    t->speed_cur = t->speed_base;
    creature_random_heading(t);
    t->aux = (int16_t)(thing_index(t) % 100);
    t->target_pitch = 0;
    t->damage = 500;
    t->prop_flags = 1;
    creature_take_serial(t);
    t->desc = desc_index(0x96c70);
    t->timer_a = 0x40;
    t->filter_cls = 3;
    thing_link_cell(t, pos);
    creature_troll_init_variant(t);
    thing_set_extents(t, 0x80, 0x80);
    return t;
}

// creature_create_griffon_37000 (type 8, state 0x31)
Thing *creature_create_griffon(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 0x31;
    t->cls = 5;
    t->type = 8;
    t->speed_base = 0x28;
    t->turn_rate = 0x14;
    t->max_health = 10000;
    t->speed_cur = t->speed_base;
    thing_set_mana_from_health(t);
    creature_random_heading(t);
    t->aux = (int16_t)(thing_index(t) % 100);
    t->target_pitch = 0;
    t->damage = 1000;
    t->prop_flags = 1;
    creature_take_serial(t);
    t->desc = desc_index(0x96c90);
    t->timer_a = 0x40;
    t->filter_cls = 3;
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite(t, 0x2f);
    thing_set_extents(t, 0x80, 0x80);
    return t;
}

// creature_create_skeleton_37110 (type 9, state 0x36). The heading is rng % 0x832 - 1 (not masked
// to 0x7ff like the others).
Thing *creature_create_skeleton(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 0x36;
    t->cls = 5;
    t->type = 9;
    t->speed_base = 0x14;
    t->turn_rate = 0;
    t->max_health = 1000;
    t->speed_cur = t->speed_base;
    thing_set_mana_from_health(t);
    uint16_t a = (uint16_t)(rng_next(t) % 0x832u - 1);
    t->target_pitch = 0;
    t->damage = 500;
    t->prop_flags = 1;
    t->target_yaw = a;
    t->yaw = a;
    t->pitch = t->target_yaw;
    creature_take_serial(t);
    t->desc = desc_index(0x96cb0);
    creature_stagger_awake(t);
    t->filter_cls = 3;
    t->aux = (int16_t)(thing_index(t) % 10 + 0x1d);
    thing_link_cell(t, pos);
    t->z = (int16_t)terrain_height_at(thing_pos(t));
    thing_restore_health(t);
    thing_set_sprite(t, 0xdc);
    thing_set_extents(t, 0x80, 0x80);
    return t;
}

// creature_create_emu_37260 (type 0xa, state 0x3d)
Thing *creature_create_emu(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 0x3d;
    t->cls = 5;
    t->type = 0xa;
    t->speed_base = 0x3c;
    t->turn_rate = 0x14;
    t->max_health = 2000;
    t->speed_cur = t->speed_base;
    thing_set_mana_from_health(t);
    creature_random_heading(t);
    t->aux = (int16_t)(thing_index(t) % 100);
    t->target_pitch = 0;
    t->damage = 500;
    t->prop_flags = 1;
    creature_take_serial(t);
    t->desc = desc_index(0x96cd0);
    t->timer_a = 0x40;
    t->filter_cls = 3;
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite(t, 0xd0);
    thing_set_extents(t, 0x80, 0x80);
    return t;
}

// creature_create_genie_37370 (type 0xb, state 0x42)
Thing *creature_create_genie(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 0x42;
    t->cls = 5;
    t->type = 0xb;
    t->speed_base = 0x3c;
    t->turn_rate = 0x14;
    t->max_health = 20000;
    t->speed_cur = t->speed_base;
    thing_set_mana_from_health(t);
    t->mana_total = t->mana + t->mana;
    creature_random_heading(t);
    t->target_pitch = 0;
    t->damage = 500;
    t->prop_flags = 1;
    creature_take_serial(t);
    t->desc = desc_index(0x96cf0);
    t->timer_a = 0x40;
    t->filter_cls = 3;
    t->aux = 0;                          // (index % 100 is stored first, then overwritten)
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite(t, 0xc8);
    thing_set_extents(t, 0x80, 0x80);
    return t;
}

// Shared head of the three villagers (builder, townie, trader): everything up to and including
// thing_restore_health.
Thing *creature_create_villager(const Pos *pos, int type, int state) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = (uint8_t)state;
    t->cls = 5;
    t->type = (uint8_t)type;
    t->speed_base = 0x28;
    t->turn_rate = 0x14;
    t->speed_cur = t->speed_base;
    creature_random_heading(t);
    t->max_health = 1000;
    t->mana = 0;
    t->target_pitch = 0;
    t->damage = 500;
    t->prop_flags = 1;
    creature_take_serial(t);
    t->desc = desc_index(0x96b50);
    t->timer_a = 0x40;
    t->filter_cls = 3;
    t->aux = 2;                          // (index % 100 is stored first, then overwritten)
    thing_link_cell(t, pos);
    thing_restore_health(t);
    return t;
}

// creature_create_builder_374a0 (type 0xc, state 0x49)
Thing *creature_create_builder(const Pos *pos) {
    Thing *t = creature_create_villager(pos, 0xc, 0x49);
    if (!t) return nullptr;
    thing_set_sprite(t, 0xdd);
    thing_set_extents(t, 0x80, 0x80);
    return t;
}

// creature_create_townie_375e0 (type 0xd, state 0x4f): sprite by rng % 7 through the jump table at
// 0x375b8 (0..3 -> 0xd9, 4..6 -> 0xda).
Thing *creature_create_townie(const Pos *pos) {
    Thing *t = creature_create_villager(pos, 0xd, 0x4f);
    if (!t) return nullptr;
    thing_set_sprite(t, rng_next(t) % 7u < 4 ? 0xd9 : 0xda);
    thing_set_extents(t, 0x80, 0x80);
    return t;
}

// creature_create_trader_37730 (type 0xe, state 0x55)
Thing *creature_create_trader(const Pos *pos) {
    Thing *t = creature_create_villager(pos, 0xe, 0x55);
    if (!t) return nullptr;
    thing_set_sprite(t, 0xdb);
    thing_set_extents(t, 0x80, 0x80);
    return t;
}

// creature_create_type15_37850 (type 0xf, state 0x5b): no RNG use, recyclable (flag 0x20000).
Thing *creature_create_type15(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 0x5b;
    t->cls = 5;
    t->type = 0xf;
    t->speed_base = 0x1e;
    t->turn_rate = 0;
    t->max_health = 1000;
    t->target_yaw = 0;
    t->speed_cur = t->speed_base;
    t->yaw = t->target_yaw;
    t->pitch = t->target_yaw;
    t->mana = 0;
    t->target_pitch = 0;
    t->aux = (int16_t)(thing_index(t) % 100);
    t->damage = 500;
    t->prop_flags = 1;
    creature_take_serial(t);
    t->desc = desc_index(0x96d10);
    creature_stagger_awake(t);
    t->filter_cls = 3;
    t->flags |= 0x20000u;
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite(t, 0);
    thing_set_extents(t, 0x80, 0x80);
    return t;
}

// creature_create_wyvern_37980 (type 0x10, state 0x61)
Thing *creature_create_wyvern(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 0x61;
    t->cls = 5;
    t->type = 0x10;
    t->speed_base = 0x3c;
    t->turn_rate = 0x14;
    t->max_health = 100000;
    t->speed_cur = t->speed_base;
    thing_set_mana_from_health(t);
    creature_random_heading(t);
    t->target_pitch = 0;
    t->damage = 500;
    t->prop_flags = 1;
    creature_take_serial(t);
    t->desc = desc_index(0x96d30);
    t->timer_a = 0x40;
    t->filter_cls = 3;
    t->aux = 0;                          // (index % 100 is stored first, then overwritten)
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite(t, 0xcf);
    thing_set_extents(t, 0x80, 0x80);
    return t;
}

// ---- class 7: weather ---------------------------------------------------------------------------

// weather_create_wind_37b60 (type 4, state 4): never linked into a cell (the position argument is
// not read).
Thing *weather_create_wind(const Pos *) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 4;
    t->cls = 7;
    t->type = 4;
    t->speed_base = 0x50;
    t->turn_rate = 0x10;
    t->speed_cur = (int16_t)(t->speed_base / 2);
    uint16_t a = (uint16_t)((rng_next(t) & 0x7ff) - 1);
    t->target_yaw = a;
    t->yaw = a;
    t->flags &= ~8u;
    thing_restore_health(t);
    return t;
}

// ---- class 9: projectiles -----------------------------------------------------------------------

// Shared body of the projectile constructors: speed_cur = speed_base = speed, lifetime (max_health)
// = range / speed, optional mana 0x32 and descriptor, flag 8 cleared, link, restore, sprite.
// desc_addr 0 = the constructor leaves the default descriptor (and does not set mana).
Thing *projectile_create(const Pos *pos, int type, int state, int speed, int range, uint32_t desc_addr, int sprite) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = (uint8_t)state;
    t->cls = 9;
    t->type = (uint8_t)type;
    t->speed_cur = (int16_t)speed;
    t->speed_base = (int16_t)speed;
    t->max_health = range / (int32_t)t->speed_cur;
    if (desc_addr) {
        t->mana = 0x32;
        t->desc = desc_index(desc_addr);
    }
    t->flags &= ~8u;
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite(t, sprite);
    return t;
}
// The homing pair (types 1 and 0x11) doubles the sprite extents: FUN_353d0(t, ext_x * 2, ext_h * 2).
Thing *projectile_double_extents(Thing *t) {
    if (t) thing_set_extents(t, (int16_t)(t->ext_x * 2), (int16_t)(t->ext_h * 2));
    return t;
}

// projectile_create_type0_37cb0
Thing *projectile_create_type0(const Pos *pos)  { return projectile_create(pos, 0, 0, 0x180, 0x2000, 0x96ab0, 0x2a); }
// projectile_create_type1_37d30 (homing): collision filter class 10. The filter is written before
// the link in the original; nothing in between reads it.
// Port (settings.h possession_range_pct): the lifetime - 11 ticks of flight, health 10 - scaled to the
// nearest whole tick; 100% gives the original's range 0x1000 exactly.
Thing *projectile_create_type1(const Pos *pos) {
    const int pct = gameplay_rules().possession_range_pct;
    const int range = pct == 100 ? 0x1000 : ((11 * pct + 50) / 100 - 1) * 0x180;
    Thing *t = projectile_create(pos, 1, 1, 0x180, range, 0x96a50, 0xd1);
    if (t) t->filter_cls = 10;
    return projectile_double_extents(t);
}
// projectile_create_type2_37de0
Thing *projectile_create_type2(const Pos *pos)  { return projectile_create(pos, 2, 2, 0x180, 0x2000, 0x96a30, 0xd3); }
// projectile_create_type3_37e60
Thing *projectile_create_type3(const Pos *pos)  { return projectile_create(pos, 3, 3, 0x180, 0x2000, 0x96a30, 0x4c); }
// projectile_create_type4_37ee0
Thing *projectile_create_type4(const Pos *pos)  { return projectile_create(pos, 4, 4, 0x180, 0x2000, 0x96a30, 0xd2); }
// projectile_create_type5_37f60
Thing *projectile_create_type5(const Pos *pos)  { return projectile_create(pos, 5, 5, 0x180, 0x2000, 0x96a30, 0xd3); }
// projectile_create_type6_37fe0
Thing *projectile_create_type6(const Pos *pos)  { return projectile_create(pos, 6, 6, 0x180, 0x2000, 0x96a30, 0xd4); }
// projectile_create_type7_38060
Thing *projectile_create_type7(const Pos *pos)  { return projectile_create(pos, 7, 7, 0x180, 0x2000, 0x96a30, 0xd5); }
// projectile_create_type8_380e0
Thing *projectile_create_type8(const Pos *pos)  { return projectile_create(pos, 8, 8, 0x180, 0x2000, 0x96a90, 0xd6); }
// projectile_create_type9_38160 (lightning)
Thing *projectile_create_type9(const Pos *pos)  { return projectile_create(pos, 9, 9, 0x180, 0xe00, 0x96a90, 0xd8); }
// projectile_create_type10_381e0
Thing *projectile_create_type10(const Pos *pos) { return projectile_create(pos, 0xa, 0xa, 0x180, 0x2000, 0x96a30, 0x12); }
// projectile_create_type11_38260
Thing *projectile_create_type11(const Pos *pos) { return projectile_create(pos, 0xb, 0xb, 0x180, 0x2000, 0x96a30, 0x119); }
// projectile_create_type12_382e0
Thing *projectile_create_type12(const Pos *pos) { return projectile_create(pos, 0xc, 0xc, 0x180, 0x800, 0x96a30, 0xd8); }

// Shared body of projectile_create_type13_38360 / projectile_create_type13_long_383cc: no mana, no
// descriptor, thing_set_sprite_double_35340.
Thing *projectile_create_13(const Pos *pos, int range, int sprite) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 0xd;
    t->cls = 9;
    t->type = 0xd;
    t->speed_cur = 0x180;
    t->speed_base = 0x180;
    t->max_health = range / (int32_t)t->speed_cur;
    t->flags &= ~8u;
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite_double(t, sprite);
    return t;
}
// projectile_create_type13_38360
Thing *projectile_create_type13(const Pos *pos) { return projectile_create_13(pos, 0x1400, 0xc3); }
// projectile_create_type14_38440 (type 0xe -> state 0xf, speed 0x80)
Thing *projectile_create_type14(const Pos *pos) { return projectile_create(pos, 0xe, 0xf, 0x80, 0x1000, 0, 0xc4); }
// projectile_create_type15_384b0 (type 0xf -> state 0x10, speed 0x80, fixed lifetime 0x50)
Thing *projectile_create_type15(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->max_health = 0x50;
    t->state = 0x10;
    t->cls = 9;
    t->type = 0xf;
    t->speed_cur = 0x80;
    t->speed_base = 0x80;
    t->flags &= ~8u;
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite(t, 0xd7);
    return t;
}
// projectile_create_type16_38510 (type 0x10 -> state 0x11)
// Hidden Worlds' Fire Wall shot (HIDDEN.EXE 0x3a5f0): the homing MoveDesc 0x96a50 (index 2) and sprite 0x4c.
Thing *projectile_create_type16(const Pos *pos) {
    if (world_hidden()) return projectile_create(pos, 0x10, 0x11, 0x180, 0x2000, 0x96a50, 0x4c);
    return projectile_create(pos, 0x10, 0x11, 0x180, 0x2000, 0x96ab0, 0x2a);
}
// projectile_create_type17_38590 (type 0x11 -> state 0x12)
Thing *projectile_create_type17(const Pos *pos) {
    return projectile_double_extents(projectile_create(pos, 0x11, 0x12, 0x180, 0x1000, 0x96a50, 0xd1));
}
// projectile_create_type18_38630 (type 0x12 -> state 0x13)
Thing *projectile_create_type18(const Pos *pos) { return projectile_create(pos, 0x12, 0x13, 0x180, 0x2000, 0x96ab0, 0x2a); }
// projectile_create_type19_386b0 (type 0x13 -> state 0x14)
Thing *projectile_create_type19(const Pos *pos) { return projectile_create(pos, 0x13, 0x14, 0x180, 0x2000, 0x96ab0, 0x2a); }

// ---- class 10: effects --------------------------------------------------------------------------

// effect_create_shared_37ca0 (types 0x14, 0x15, 0x16, 0x18): `xor eax,eax; ret`.
Thing *effect_create_shared_none(const Pos *) { return nullptr; }

// effect_create_explosion_38730 (type 0)
Thing *effect_create_explosion(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->max_health = 8;
    t->state = 0;
    t->cls = 10;
    t->type = 0;
    t->damage = 400;
    t->prop_flags = 0;
    fx_flags(t);
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite(t, 7);
    thing_set_extents(t, 0x80, 0x80);
    return t;
}

// effect_create_big_explosion_387b0 (type 1)
Thing *effect_create_big_explosion(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 1;
    t->cls = 10;
    t->type = 1;
    t->max_health = 1;
    t->damage = 400;
    fx_flags(t);
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite(t, 0x29);
    return t;
}

// effect_create_type2_38810 (type 2): positioned but not linked into a cell.
Thing *effect_create_type2(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->max_health = 8;
    t->state = 2;
    t->cls = 10;
    t->type = 2;
    set_pos_raw(t, pos);
    t->aux = 0;
    t->flags = (t->flags & 0xfffdfff6u) | 0x20001u;
    thing_restore_health(t);
    return t;
}

// effect_create_type3_38870 (type 3)
Thing *effect_create_type3(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->max_health = 7;
    t->state = 3;
    t->cls = 10;
    t->type = 3;
    t->damage = 0;
    fx_flags(t);
    t->aux = 0;
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite(t, 0x24);
    return t;
}

// effect_create_type4_388e0 (type 4): not linked; z = terrain height.
Thing *effect_create_type4(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->max_health = 100;
    t->state = 4;
    t->cls = 10;
    t->type = 4;
    t->damage = 0;
    fx_flags(t);
    t->aux = 0;
    set_pos_raw(t, pos);
    t->z = (int16_t)terrain_height_at(thing_pos(t));
    thing_restore_health(t);
    return t;
}

// effect_create_splash_38950 (type 5)
Thing *effect_create_splash(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->max_health = 8;
    t->state = 5;
    t->cls = 10;
    t->type = 5;
    t->damage = 0;
    fx_flags(t);
    t->aux = 0;
    thing_link_cell(t, pos);
    t->z = (int16_t)terrain_height_at(thing_pos(t));
    thing_restore_health(t);
    thing_set_sprite(t, 0xf4);
    return t;
}

// effect_create_fire_389d0 (type 6)
Thing *effect_create_fire(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 6;
    t->cls = 10;
    t->type = 6;
    t->damage = 0x32;
    t->max_health = 0xf0;
    t->z_vel = 0;
    fx_flags(t);
    Pos p = *pos;                         // pos may alias the thing after the link
    thing_link_cell(t, &p);
    t->z = (int16_t)terrain_height_at(&p);
    thing_restore_health(t);
    thing_set_sprite(t, 0xe4);
    thing_set_extents(t, 0x110, 0x600);
    t->aux = 0;
    return t;
}

// effect_create_type7_38a70 (type 7)
Thing *effect_create_type7(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 7;
    t->cls = 10;
    t->type = 7;
    t->max_health = 0xc;
    fx_flags(t);
    t->speed_cur = (int16_t)(rng_next(t) % 0x14u + 0x14);
    t->filter_cls = 10;
    t->filter_type = 7;
    Pos p = *pos;
    thing_link_cell(t, &p);
    t->z = (int16_t)terrain_height_at(&p);
    thing_set_sprite(t, 0x4e);            // thing_set_sprite_small_352d0
    thing_restore_health(t);
    t->flags |= 1;
    return t;
}

// effect_create_mini_volcano_38b10 (type 8): not linked.
Thing *effect_create_mini_volcano(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 8;
    t->cls = 10;
    t->type = 8;
    t->max_health = 8;
    set_pos_raw(t, pos);
    t->flags &= ~8u;
    t->damage = 100;
    thing_restore_health(t);
    thing_set_extents(t, 0x200, 0x200);
    return t;
}

// effect_create_type12_38cb0 (type 0xc)
Thing *effect_create_type12(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 0xc;
    t->cls = 10;
    t->type = 0xc;
    t->max_health = 8;
    Pos p = *pos;
    set_pos_raw(t, &p);
    t->damage = 0xfa00;
    t->flags = (t->flags & ~9u) | 1;
    thing_link_cell(t, &p);
    thing_restore_health(t);
    thing_set_sprite(t, 0x29);
    t->flags |= 1;
    thing_set_extents(t, 0x200, 0x200);
    return t;
}

// Shared body of effect_create_white_smoke_38d40 / effect_create_black_smoke_38de0.
Thing *effect_create_smoke(const Pos *pos, int type, unsigned life_mod, int life_min, int sprite) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = (uint8_t)type;
    t->cls = 10;
    t->type = (uint8_t)type;
    t->max_health = (int32_t)(rng_next(t) % life_mod + life_min);
    t->speed_cur = (int16_t)(rng_next(t) % 0x35u + 0x33);
    t->filter_cls = 10;
    t->filter_type = (uint8_t)type;
    fx_flags(t);
    thing_link_cell(t, pos);
    thing_set_sprite(t, sprite);          // thing_set_sprite_small_352d0
    thing_restore_health(t);
    return t;
}
// effect_create_white_smoke_38d40 (type 0xd): life 17..39, rise speed 0x33..0x67
Thing *effect_create_white_smoke(const Pos *pos) { return effect_create_smoke(pos, 0xd, 0x17, 0x11, 0x43); }
// effect_create_black_smoke_38de0 (type 0xe): life 28..60
Thing *effect_create_black_smoke(const Pos *pos) { return effect_create_smoke(pos, 0xe, 0x21, 0x1c, 9); }

// Shared body of effect_create_earthquake_38e80 and effect_create_type53/54/55/56 (0x39b80, 0x39c10,
// 0x39ca0, 0x39d30): an area effect that is positioned but not linked, random yaw, extents
// 0x400 x 0x4000. The types 0x35..0x38 additionally set flag 1.
Thing *effect_create_area(const Pos *pos, int type, int state, int life, int aux, bool flag1) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = (uint8_t)state;
    t->cls = 10;
    t->type = (uint8_t)type;
    t->max_health = life;
    t->speed_cur = 0x100;
    t->flags &= ~8u;
    t->damage = 100;
    t->aux = (int16_t)aux;
    t->yaw = (uint16_t)(rng_next(t) & 0x7ff);
    set_pos_raw(t, pos);
    if (flag1) t->flags |= 1;
    thing_restore_health(t);
    thing_set_extents(t, 0x400, 0x4000);
    return t;
}
// effect_create_earthquake_38e80 (type 0xf)
Thing *effect_create_earthquake(const Pos *pos) { return effect_create_area(pos, 0xf, 0xf, 0x80, 0, false); }
// effect_create_type53_39b80 (type 0x35 -> state 0x3a)
// Hidden Worlds (HIDDEN.EXE 0x3bc60): the Fire Wall impact = effect_create_meteor_38f10 with life 6
// (no yaw draw, no flag 1, no extents; projectile_fly_and_impact sets the damage afterwards).
Thing *effect_create_type53(const Pos *pos) {
    if (world_hidden()) {
        Thing *t = thing_alloc();
        if (!t) return nullptr;
        t->state = 0x3a;
        t->cls = 10;
        t->type = 0x35;
        set_pos_raw(t, pos);
        t->max_health = 6;
        t->damage = 3000;
        t->flags &= ~8u;
        thing_restore_health(t);
        return t;
    }
    return effect_create_area(pos, 0x35, 0x3a, 0x80, 0, true);
}
// effect_create_type54_39c10 (type 0x36 -> state 0x3b)
Thing *effect_create_type54(const Pos *pos) { return effect_create_area(pos, 0x36, 0x3b, 0x80, 0, true); }
// effect_create_type55_39ca0 (type 0x37 -> state 0x3c)
Thing *effect_create_type55(const Pos *pos) { return effect_create_area(pos, 0x37, 0x3c, 0x13, 0x20, true); }
// effect_create_type56_39d30 (type 0x38 -> state 0x3d)
Thing *effect_create_type56(const Pos *pos) { return effect_create_area(pos, 0x38, 0x3d, 0x80, 0, true); }

// effect_create_meteor_38f10 (type 0x11): not linked.
Thing *effect_create_meteor(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 0x11;
    t->cls = 10;
    t->type = 0x11;
    set_pos_raw(t, pos);
    t->max_health = 10;
    t->damage = 3000;
    t->flags &= ~8u;
    thing_restore_health(t);
    return t;
}

// effect_create_type16_38f60 (type 0x10): random life 100..199, speed 0x34..0x65, random yaw; home
// (+0x96, still zero) is pushed out by speed along the yaw.
Thing *effect_create_type16(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 0x10;
    t->cls = 10;
    t->type = 0x10;
    t->damage = 200;
    t->max_health = (int32_t)(rng_next(t) % 100u + 100);
    uint32_t speed = rng_next(t) % 0x32u;
    t->z_vel = 0x100;
    t->yaw = (uint16_t)(rng_next(t) & 0x7ff);
    t->speed_cur = (int16_t)(speed + 0x34);
    fx_flags(t);
    Pos p = *pos;
    thing_link_cell(t, &p);
    t->z = (int16_t)(terrain_height_at(&p) + 0x40);
    math_rotate_offset(&t->home, t->yaw, 0, t->speed_cur);
    thing_restore_health(t);
    thing_set_sprite(t, 0xd2);
    return t;
}

// effect_create_type18_39050 (type 0x12): not linked.
Thing *effect_create_type18(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 0x12;
    t->cls = 10;
    t->type = 0x12;
    t->damage = 200;
    t->aux = 0;
    t->max_health = 10000;
    t->flags &= ~8u;
    set_pos_raw(t, pos);
    thing_restore_health(t);
    return t;
}

// effect_create_type19_390a0 (type 0x13)
Thing *effect_create_type19(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 0x13;
    t->cls = 10;
    t->type = 0x13;
    t->damage = 200;
    t->max_health = 0xf0;
    fx_flags(t);
    thing_link_cell(t, pos);
    t->flags |= 1;
    thing_restore_health(t);
    thing_set_sprite(t, 0xe4);
    thing_set_extents(t, 0x200, 0x200);
    return t;
}

// effect_create_lightning_39120 (type 0x17)
Thing *effect_create_lightning(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->max_health = 8;
    t->state = 0x17;
    t->cls = 10;
    t->type = 0x17;
    t->damage = 0x19;
    fx_flags(t);
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite(t, 7);
    thing_set_extents(t, 0xc8, 0xc8);
    t->flags |= 1;
    return t;
}

// Shared body of the "8 ticks, linked, sprite, extents 0x200" effects: effect_create_steal_mana_391a0,
// effect_create_type26_39220, effect_create_type36_39680, effect_create_type37_397c0,
// effect_create_type43_39990.
Thing *effect_create_burst(const Pos *pos, int type, int state, int damage, int sprite) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = (uint8_t)state;
    t->cls = 10;
    t->type = (uint8_t)type;
    t->max_health = 8;
    Pos p = *pos;
    set_pos_raw(t, &p);                   // (type 0x2b does not pre-copy; the link stores the same)
    t->damage = (uint16_t)damage;
    t->flags &= ~8u;
    thing_link_cell(t, &p);
    thing_restore_health(t);
    thing_set_sprite(t, sprite);
    thing_set_extents(t, 0x200, 0x200);
    return t;
}
// effect_create_steal_mana_391a0 (type 0x19)
Thing *effect_create_steal_mana(const Pos *pos) { return effect_create_burst(pos, 0x19, 0x19, 2000, 0x11b); }
// effect_create_type26_39220 (type 0x1a)
Thing *effect_create_type26(const Pos *pos)     { return effect_create_burst(pos, 0x1a, 0x1a, 200, 0x11c); }
// effect_create_type36_39680 (type 0x24 -> state 0x26)
Thing *effect_create_type36(const Pos *pos)     { return effect_create_burst(pos, 0x24, 0x26, 0xfa00, 0x29); }
// effect_create_type43_39990 (type 0x2b -> state 0x2d)
Thing *effect_create_type43(const Pos *pos)     { return effect_create_burst(pos, 0x2b, 0x2d, 0xfa00, 0x29); }

// effect_create_type33_39770 (type 0x21 -> state 0x23): not linked.
Thing *effect_create_type33(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 0x23;
    t->cls = 10;
    t->type = 0x21;
    t->flags &= ~8u;
    set_pos_raw(t, pos);
    thing_restore_health(t);
    thing_set_extents(t, 0x200, 0x200);
    return t;
}

// effect_create_teleport_395a0 (type 0x22 -> state 0x24): hovers 0x280 above the ground; the
// destination (+0x96) defaults to 0x8000 units away in a random direction
// (level_spawn_thing_record_35800 overwrites x / y from the level record).
Thing *effect_create_teleport(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 0x24;
    t->cls = 10;
    t->type = 0x22;
    t->max_health = 0;
    t->filter_cls = 3;
    t->filter_type = 0xff;
    t->flags &= ~8u;
    Pos p = *pos;
    set_pos_raw(t, &p);
    thing_set_sprite(t, 0xdf);
    thing_set_extents(t, 0x100, 0x100);
    thing_restore_health(t);
    thing_link_cell(t, &p);
    t->z = (int16_t)(terrain_height_at(thing_pos(t)) + 0x280);
    t->home = *thing_pos(t);
    math_rotate_offset(&t->home, (int)(rng_next(t) & 0x7ff), 0, (int)0xffff8000);
    return t;
}

// effect_create_type35_39670 (type 0x23): `call thing_alloc; test eax,eax; ret` - hands back the
// raw allocation (class 0, default fields).
Thing *effect_create_type35(const Pos *) { return thing_alloc(); }

// effect_create_type38_39700 (type 0x26 -> state 0x28)
Thing *effect_create_type38(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 0x28;
    t->cls = 10;
    t->type = 0x26;
    t->flags &= ~8u;
    t->max_health = 0x20;
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite(t, 0x110);
    thing_set_extents(t, 0x200, 0x200);
    return t;
}

// effect_create_mana_ball_39840 (type 0x27 -> state 0x29): 0x200 mana, 0x9c4 when Config.flags has
// bit 0x200.
Thing *effect_create_mana_ball(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 0x29;
    t->cls = 10;
    t->type = 0x27;
    t->filter_cls = 10;
    t->filter_type = 0x27;
    t->z_vel = 0x80;
    t->mana = 0x200;
    if (g_cfg->flags & 0x200) t->mana = 0x9c4;
    t->speed_cur = 0x20;
    t->prop_flags = 3;
    t->timer_a = 0x80;
    t->timer_b = 0;
    thing_link_cell(t, pos);
    thing_restore_health(t);
    mana_ball_update_sprite(t);
    return t;
}

// effect_create_type40_398c0 (type 0x28 -> state 0x2a)
Thing *effect_create_type40(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 0x2a;
    t->cls = 10;
    t->type = 0x28;
    t->aux = (int16_t)(thing_index(t) % 11);
    t->prop_flags = 2;
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite(t, 0x41);            // thing_set_sprite_small_352d0
    return t;
}

// Shared body of effect_create_type41_39a00 / effect_create_type42_39a50: castle-building helpers,
// zero lifetime, not linked.
Thing *effect_create_castle_step(const Pos *pos, int type, int state, int spell_flags) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = (uint8_t)state;
    t->cls = 10;
    t->type = (uint8_t)type;
    t->max_health = 0;
    t->flags &= ~8u;
    set_pos_raw(t, pos);
    if (spell_flags) t->spell_flags = (uint8_t)spell_flags;
    thing_restore_health(t);
    return t;
}
// effect_create_type41_39a00 (type 0x29 -> state 0x2b)
Thing *effect_create_type41(const Pos *pos) { return effect_create_castle_step(pos, 0x29, 0x2b, 0); }
// effect_create_type42_39a50 (type 0x2a -> state 0x2c, +0x3c = 1)
Thing *effect_create_type42(const Pos *pos) { return effect_create_castle_step(pos, 0x2a, 0x2c, 1); }

// effect_create_type44_39aa0 (type 0x2c -> state 0x2e)
Thing *effect_create_type44(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 0x2e;
    t->cls = 10;
    t->type = 0x2c;
    t->max_health = 500;
    t->damage = 500;
    t->flags &= ~8u;
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite(t, 8);
    return t;
}

// effect_create_crab_egg_39b00 (type 0x34 -> state 0x38)
Thing *effect_create_crab_egg(const Pos *pos) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = 0x38;
    t->cls = 10;
    t->type = 0x34;
    t->max_health = 100000;
    t->damage = 500;
    t->aux = 600;
    t->mana = 500;
    t->mana_total = 2000;
    t->flags &= ~8u;
    thing_link_cell(t, pos);
    thing_restore_health(t);
    thing_set_sprite(t, 0xcd);
    return t;
}

// ---- class 11: switches -------------------------------------------------------------------------

// switch_create_hidden_inside_39e10 .. switch_create_type31_3a1f0 (0x20 apart): type N, state N.
template <int N> Thing *switch_create_stub(const Pos *pos) { return switch_create_common(pos, N, N); }

// ---- class 12: spells ---------------------------------------------------------------------------

// The nine stack arguments of spell_create_common_3a210 as each 0x30-byte stub pushes them.
struct SpellParams { uint32_t addr; int type, state, total_mana, levels, flags, unk3e, cost, damage; };
constexpr SpellParams kSpells[24] = {
    {0x3a330, 0x00, 0x00, 0xc8,    5,    1, 0, 0,       0x7d},      // spell_create_fireball_3a330
    {0x3a390, 0x01, 0x03, 0x3e8,   0x15, 1, 0, 0,       0x64},      // spell_create_heal_3a390
    {0x3a360, 0x02, 0x06, 0x3e8,   0xfb, 0, 0, 0,       0x64},      // spell_create_spell2_3a360
    {0x3a2e0, 0x03, 0x09, 0x32,    3,    1, 0, 0,       0x64},      // spell_create_possession_3a2e0
    {0x3a450, 0x04, 0x0c, 0x7d0,   0xfb, 1, 0, 0,       0x64},      // spell_create_shield_3a450
    {0x3a5d0, 0x05, 0x0f, 0xbb8,   0x65, 1, 0, 0,       0x64},      // spell_create_beyond_sight_3a5d0
    {0x3a3f0, 0x06, 0x12, 0x1770,  0x33, 1, 0, 0x1d4c0, 0x1770},    // spell_create_earthquake_3a3f0
    {0x3a480, 0x07, 0x15, 0x2710,  0x0b, 1, 0, 0x186a0, 0x2710},    // spell_create_meteor_3a480
    {0x3a630, 0x08, 0x18, 0x7530,  0x41, 1, 0, 0x2bf20, 0x3e8},     // spell_create_volcano_3a630
    {0x3a5a0, 0x09, 0x1b, 0x2ee0,  0x1f, 1, 0, 0x186a0, 0x1770},    // spell_create_crater_3a5a0
    {0x3a3c0, 0x0a, 0x1e, 0x1388,  0x33, 1, 0, 0x2710,  0x64},      // spell_create_teleport_3a3c0
    {0x3a510, 0x0b, 0x21, 0x9c4,   0x11, 1, 0, 0x3e80,  0x64},      // spell_create_rubber_band_3a510
    {0x3a570, 0x0c, 0x24, 0x1388,  0xfb, 1, 0, 0xc350,  0x64},      // spell_create_invisible_3a570
    {0x3a540, 0x0d, 0x27, 0x1f4,   0x0b, 1, 0, 0x4e20,  0x64},      // spell_create_steal_mana_3a540
    {0x3a4b0, 0x0e, 0x2a, 0x3e8,   0x65, 1, 0, 0x1f40,  0x64},      // spell_create_rebound_3a4b0
    {0x3a4e0, 0x0f, 0x2d, 0x3e8,   2,    0, 0, 0x61a8,  0x1f4},     // spell_create_lightning_3a4e0
    {0x3a300, 0x10, 0x30, 0x3e8,   0x65, 1, 0, 0,       0x2710},    // spell_create_castle_3a300
    {0x3a660, 0x11, 0x33, 0x32c8,  0x0d, 1, 0, 0x249f0, 0x64},      // spell_create_skeleton_3a660
    {0x3a600, 0x12, 0x36, 0x4e20,  0x21, 1, 0, 0x15f90, 0x7d0},     // spell_create_thunderbolt_3a600
    {0x3a420, 0x13, 0x39, 0xfa0,   0x11, 1, 0, 0x2710,  0x64},      // spell_create_mana_magnet_3a420
    {0x3a690, 0x14, 0x3c, 0x1388,  0x33, 1, 0, 0x2ee0,  0x15f90},   // spell_create_fire_wall_3a690
    {0x3a6c0, 0x15, 0x3f, 0x3e8,   0xfb, 0, 0, 0,       0x64},      // spell_create_reverse_speed_3a6c0
    {0x3a6f0, 0x16, 0x42, 0x124f8, 0x65, 1, 0, 0x30d40, 0x1b58},    // spell_create_smart_bomb_3a6f0
    {0x3a720, 0x17, 0x45, 0x258,   3,    0, 0, 0xc350,  0x32},      // spell_create_mini_fireball_3a720
};
// Hidden Worlds' Fire Wall (HIDDEN.EXE 0x3c730): 26 ticks, cost 0xea60, damage 0x1388.
constexpr SpellParams kFireWallHidden = {0x3a690, 0x14, 0x3c, 0x1388, 0x1a, 1, 0, 0xea60, 0x1388};
template <int N> Thing *spell_create_stub(const Pos *pos) {
    const SpellParams &s = (N == 20 && world_hidden()) ? kFireWallHidden : kSpells[N];
    return spell_create_common(pos, s.type, s.state, s.total_mana, s.levels, s.flags, s.unk3e, s.cost, s.damage);
}

// ---- class 13 -----------------------------------------------------------------------------------

// class13_create_type0_3a750 .. class13_create_type3_3a7e0 (0x30 apart): type N, state N,
// flags &= 1, no position, not linked.
template <int N> Thing *class13_create(const Pos *) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->state = N;
    t->cls = 0xd;
    t->type = N;
    t->flags &= 1;
    thing_restore_health(t);
    return t;
}

template <int... N> void register_switches(std::integer_sequence<int, N...>) {
    (thing_register_create(0x39e10u + (uint32_t)N * 0x20u, switch_create_stub<N>), ...);
}
template <int... N> void register_spells(std::integer_sequence<int, N...>) {
    (thing_register_create(kSpells[N].addr, spell_create_stub<N>), ...);
}
template <int... N> void register_class13(std::integer_sequence<int, N...>) {
    (thing_register_create(0x3a750u + (uint32_t)N * 0x30u, class13_create<N>), ...);
}

} // namespace

// ---- public helpers -----------------------------------------------------------------------------

// crab_update_mana_sprite_36ac0
void crab_update_mana_sprite(Thing *t) {
    int32_t step = t->mana_total / 8;
    int16_t level = (int16_t)(step ? t->mana / step : 0);   // the original idiv would fault on 0
    if (level < 0) level = 0;
    if (level > 7) level = 7;
    if (level > (int16_t)t->sprite - 0xb9) t->max_health += 5000;
    thing_set_sprite(t, (int16_t)(level + 0xb9));
}

// creature_troll_init_variant_36ea0
void creature_troll_init_variant(Thing *t) {
    if (t->tick % 2 != 0) {
        thing_set_sprite(t, 0x55);
        t->castle_size = 1;
        t->max_health = 4000;
    } else {
        thing_set_sprite(t, 0xc7);
        t->castle_size = 2;
        t->max_health = 2000;
    }
    thing_set_mana_from_health(t);
    t->health = t->max_health;
}

// switch_create_common_39dc0: positioned but not linked into a cell;
// level_spawn_thing_record_35800 then sets the switch id (+0x18) and the trigger extents.
Thing *switch_create_common(const Pos *pos, int type, int state) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->cls = 0xb;
    t->type = (uint8_t)type;
    t->state = (uint8_t)state;
    t->flags = (t->flags & ~9u) | 1;
    t->aux = 0;
    set_pos_raw(t, pos);
    thing_restore_health(t);
    return t;
}

// spell_create_common_3a210: a spell thing as it lies in the world (sprite 0x4d, extents x4);
// max_health = health = 0. mana (+0x8c) = total mana / levels = mana per level.
Thing *spell_create_common(const Pos *pos, int type, int state, int total_mana, int levels,
                           int flags, int unk3e, int cost, int damage) {
    Thing *t = thing_alloc();
    if (!t) return nullptr;
    t->cls = 0xc;
    t->type = (uint8_t)type;
    t->state = (uint8_t)state;
    t->damage = (uint16_t)damage;
    t->duration = (int16_t)levels;
    t->unk3e = (uint8_t)unk3e;
    if ((uint8_t)unk3e != 0) flags = 0;
    t->spell_flags = (uint8_t)flags;
    t->mana = total_mana / (int32_t)t->duration;
    t->burst = 0;
    t->max_health = 0;
    t->health = 0;
    t->mana_cost = cost;
    t->mana_total = total_mana;
    t->flags &= ~8u;
    thing_link_cell(t, pos);
    thing_set_sprite(t, 0x4d);
    thing_set_extents(t, (int16_t)(t->ext_x << 2), (int16_t)(t->ext_h << 2));
    thing_restore_health(t);
    return t;
}

// projectile_create_type13_long_383cc (real entry 0x383d0; no Table B record)
Thing *projectile_create_type13_long(const Pos *pos) { return projectile_create_13(pos, 0x1e00, 0xcb); }

// effect_create_type37_397c0 (type 0x25 -> state 0x27; no Table B record)
Thing *effect_create_type37(const Pos *pos) { return effect_create_burst(pos, 0x25, 0x27, 0xfa00, 0x29); }

// ---- registration -------------------------------------------------------------------------------

void constructors_register_handlers() {
    // class 3
    thing_register_create(0x359c0, player_create_flyer_start<0>);
    thing_register_create(0x359e0, player_create_flyer_start<1>);
    thing_register_create(0x35a00, player_create_flyer_start<2>);
    thing_register_create(0x35a20, player_create_flyer_start<3>);
    thing_register_create(0x35a40, player_create_flyer_start<4>);
    thing_register_create(0x35a60, player_create_flyer_start<5>);
    thing_register_create(0x35a80, player_create_flyer_start<6>);
    thing_register_create(0x35aa0, player_create_flyer_start<7>);
    thing_register_create(0x35ac0, player_create_flyer);
    thing_register_create(0x35b40, player_create_type1);
    thing_register_create(0x35bc0, player_create_type2);
    thing_register_create(0x35ca0, player_create_type3);
    // classes 1, 6, 7 (types 0..3), 8: allocate-and-drop stubs
    for (uint32_t a = 0x35d20; a <= 0x35e40; a += 0x20) thing_register_create(a, create_alloc_discard);
    thing_register_create(0x37aa0, create_alloc_discard);
    thing_register_create(0x37ac0, create_alloc_discard);
    for (uint32_t a = 0x37ae0; a <= 0x37b40; a += 0x20) thing_register_create(a, create_alloc_discard);
    for (uint32_t a = 0x37be0; a <= 0x37c80; a += 0x20) thing_register_create(a, create_alloc_discard);
    // class 2
    thing_register_create(0x35e60, scenery_create_tree);
    thing_register_create(0x35f90, scenery_create_standing_stone);
    thing_register_create(0x36010, scenery_create_dolmen);
    thing_register_create(0x360a0, scenery_create_bad_stone);
    thing_register_create(0x36120, scenery_create_2d_dome_a);
    thing_register_create(0x36190, scenery_create_2d_dome_b);
    // class 5
    thing_register_create(0x362d0, creature_create_dragon);
    thing_register_create(0x36510, creature_create_vulture);
    thing_register_create(0x36610, creature_create_bee);
    thing_register_create(0x36750, creature_create_worm);
    thing_register_create(0x36980, creature_create_archer);
    thing_register_create(0x36b30, creature_create_crab);
    thing_register_create(0x36c80, creature_create_kraken);
    thing_register_create(0x36f00, creature_create_troll);
    thing_register_create(0x37000, creature_create_griffon);
    thing_register_create(0x37110, creature_create_skeleton);
    thing_register_create(0x37260, creature_create_emu);
    thing_register_create(0x37370, creature_create_genie);
    thing_register_create(0x374a0, creature_create_builder);
    thing_register_create(0x375e0, creature_create_townie);
    thing_register_create(0x37730, creature_create_trader);
    thing_register_create(0x37850, creature_create_type15);
    thing_register_create(0x37980, creature_create_wyvern);
    // class 7
    thing_register_create(0x37b60, weather_create_wind);
    // class 9
    thing_register_create(0x37cb0, projectile_create_type0);
    thing_register_create(0x37d30, projectile_create_type1);
    thing_register_create(0x37de0, projectile_create_type2);
    thing_register_create(0x37e60, projectile_create_type3);
    thing_register_create(0x37ee0, projectile_create_type4);
    thing_register_create(0x37f60, projectile_create_type5);
    thing_register_create(0x37fe0, projectile_create_type6);
    thing_register_create(0x38060, projectile_create_type7);
    thing_register_create(0x380e0, projectile_create_type8);
    thing_register_create(0x38160, projectile_create_type9);
    thing_register_create(0x381e0, projectile_create_type10);
    thing_register_create(0x38260, projectile_create_type11);
    thing_register_create(0x382e0, projectile_create_type12);
    thing_register_create(0x38360, projectile_create_type13);
    thing_register_create(0x38440, projectile_create_type14);
    thing_register_create(0x384b0, projectile_create_type15);
    thing_register_create(0x38510, projectile_create_type16);
    thing_register_create(0x38590, projectile_create_type17);
    thing_register_create(0x38630, projectile_create_type18);
    thing_register_create(0x386b0, projectile_create_type19);
    // class 10 (the terrain-effect types 9, 0xa, 0xb, 0x1b..0x20, 0x2d, 0x32, 0x33 are bound by
    // level_features.cpp)
    thing_register_create(0x37ca0, effect_create_shared_none);
    thing_register_create(0x38730, effect_create_explosion);
    thing_register_create(0x387b0, effect_create_big_explosion);
    thing_register_create(0x38810, effect_create_type2);
    thing_register_create(0x38870, effect_create_type3);
    thing_register_create(0x388e0, effect_create_type4);
    thing_register_create(0x38950, effect_create_splash);
    thing_register_create(0x389d0, effect_create_fire);
    thing_register_create(0x38a70, effect_create_type7);
    thing_register_create(0x38b10, effect_create_mini_volcano);
    thing_register_create(0x38cb0, effect_create_type12);
    thing_register_create(0x38d40, effect_create_white_smoke);
    thing_register_create(0x38de0, effect_create_black_smoke);
    thing_register_create(0x38e80, effect_create_earthquake);
    thing_register_create(0x38f60, effect_create_type16);
    thing_register_create(0x38f10, effect_create_meteor);
    thing_register_create(0x39050, effect_create_type18);
    thing_register_create(0x390a0, effect_create_type19);
    thing_register_create(0x39120, effect_create_lightning);
    thing_register_create(0x391a0, effect_create_steal_mana);
    thing_register_create(0x39220, effect_create_type26);
    thing_register_create(0x39770, effect_create_type33);
    thing_register_create(0x395a0, effect_create_teleport);
    thing_register_create(0x39670, effect_create_type35);
    thing_register_create(0x39680, effect_create_type36);
    thing_register_create(0x39700, effect_create_type38);
    thing_register_create(0x39840, effect_create_mana_ball);
    thing_register_create(0x398c0, effect_create_type40);
    thing_register_create(0x39a00, effect_create_type41);
    thing_register_create(0x39a50, effect_create_type42);
    thing_register_create(0x39990, effect_create_type43);
    thing_register_create(0x39aa0, effect_create_type44);
    thing_register_create(0x39b00, effect_create_crab_egg);
    thing_register_create(0x39b80, effect_create_type53);
    thing_register_create(0x39c10, effect_create_type54);
    thing_register_create(0x39ca0, effect_create_type55);
    thing_register_create(0x39d30, effect_create_type56);
    // classes 11, 12, 13
    register_switches(std::make_integer_sequence<int, 32>{});
    register_spells(std::make_integer_sequence<int, 24>{});
    register_class13(std::make_integer_sequence<int, 4>{});
}
