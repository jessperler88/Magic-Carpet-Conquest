// Spatial queries and damage delivery (0x102b0..0x117c0), cell_kill_things_3da30,
// castle_spell_reset_charge_41310, thing_aim_at_43ee0. Translated from the disassembly of carpet.exe.
#include "spatial.h"
#include "terrain_paint.h"
#include "mode_rules.h"     // same_side (round 12): exactly `a == b` outside a mode run with teams
#include <cstring>

void (*g_hook_thing_drop_mana_ball)(Thing *) = nullptr;

namespace {

// idiv by 256 (truncates toward zero) as the area functions compute their cell coordinates.
inline int div256(int v) { return v / 256; }

// t's class / type filter against a candidate.
inline bool filter_pass(const Thing *t, const Thing *o) {
    if (t->filter_cls == 0xff) return true;
    if (t->filter_cls != o->cls) return false;
    return t->filter_type == 0xff || t->filter_type == o->type;
}

// Slot write of the area functions: accumulate while an attacker is recorded, else start over.
inline void slot_hit(Thing *o, int slot, uint32_t amount, uint16_t attacker) {
    DamageSlot &s = o->damage_slots[slot];
    if (s.attacker != 0) s.amount += (int32_t)amount;
    else                 s.amount = (int32_t)amount;
    s.attacker = attacker;
}

// Walk of the things in one cell; `fn` returns true to stop. The chain is at most the pool long.
template <typename F> inline Thing *walk_cell(unsigned cell, F fn) {
    int guard = 0;
    for (unsigned i = g_cell_things[cell & (g_world.cells - 1)]; i != 0 && i < (uint32_t)thing_pool_slots() && guard < thing_pool_slots(); guard++) {
        Thing *o = thing_at(i);
        unsigned next = o->cell_next;
        if (fn(o)) return o;
        i = next;
    }
    return nullptr;
}

// The shared frame of the four spiral searches: rings 0..(ext_x + 0xff) / 256 around the cell of
// (x + 0x80, y + 0x80).
template <typename F> Thing *spiral_find(const Thing *t, F match) {
    int cx = (world_signed(t->x) + 0x80) >> 8;
    int cy = (world_signed(t->y) + 0x80) >> 8;
    SpiralSearch s;
    if (!spiral_search_begin(&s, 0, div256((int16_t)t->ext_x + 0xff))) return nullptr;
    int dx, dy;
    while (spiral_search_next(&s, &dx, &dy) == 1) {
        unsigned cell = world_cell(cx + dx, cy + dy);
        if (Thing *hit = walk_cell(cell, match)) return hit;
    }
    return nullptr;
}

// Castles (player type 2) through the per-tick player list.
template <typename F> void for_each_castle(F fn) {
    int guard = 0;
    for (uint32_t i = g_cfg->player_list; i != 0 && i < (uint32_t)thing_pool_slots() && guard < thing_pool_slots(); guard++) {
        Thing *p = thing_at(i);
        uint32_t next = p->next;
        if (p->type == 2) fn(p);
        i = next;
    }
}

// The slot-0 cell walk shared by the three area functions (cells of (x - 0x80) / 256 +- radius).
// The idiv truncates toward zero on the signed coordinate: in the upper half of the world (a negative
// world_signed value) the window centre is one cell further east / south than the floor would give - the
// original's quirk, kept; with N > 256 it applies to the upper half of the wider world.
template <typename F> void area_cells_slot0(Thing *t, F hit) {
    int cx = div256(world_signed(t->x) - 0x80);
    int cy = div256(world_signed(t->y) - 0x80);
    int r  = div256((int16_t)t->ext_x + 0xff);
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            unsigned cell = world_cell(cx + dx, cy + dy);
            walk_cell(cell, [&](Thing *o) {
                if (same_side(o->owner, t->owner)) return false;
                if (!thing_collide(t, o)) return false;
                if (!(o->prop_flags & 1)) return false;
                if (!(o->flags & 8)) return false;
                if (o->cls == 3 && o->type == 2) return false;
                if (filter_pass(t, o)) hit(o);
                return false;
            });
        }
}

}  // namespace

// ---- collision searches ----------------------------------------------------------------------------

// thing_find_collision_105f0
Thing *thing_find_collision(Thing *t) {
    return spiral_find(t, [&](Thing *o) {
        return (o->flags & 8) && filter_pass(t, o) && !same_side(t->owner, o->owner) && thing_collide(t, o);
    });
}

// thing_find_collision_other_owner_10980 (only caller: the mana ball merge - balls own themselves, kept as is)
Thing *thing_find_collision_other_owner(Thing *t) {
    return spiral_find(t, [&](Thing *o) {
        return filter_pass(t, o) && t->owner != o->owner && thing_collide(t, o);
    });
}

// thing_find_mana_near_10730
Thing *thing_find_mana_near(Thing *t) {
    return spiral_find(t, [&](Thing *o) {
        if (!(o->flags & 8) || o->cls != 10) return false;
        if (o->type != 0x27 && o->type != 0x28 && o->type != 0x2d) return false;
        if (same_side(t->owner, o->owner)) return false;
        if (same_side((int)(int16_t)t->owner, (int)o->mana_owner)) return false;     // round 12: no ally's mana
        return thing_collide(t, o) != 0;
    });
}

// thing_find_mana_ball_touching_10870
Thing *thing_find_mana_ball_touching(Thing *t) {
    return spiral_find(t, [&](Thing *o) {
        return (o->flags & 8) && o->cls == 10 && o->type == 0x27 && thing_collide(t, o);
    });
}

// thing_exists_near_pos_10ac0
int thing_exists_near_pos(const Pos *pos, int cls, int type) {
    int cx = (world_signed(pos->x) - 0x80) >> 8;
    int cy = (world_signed(pos->y) - 0x80) >> 8;
    for (int dy = 0; dy < 2; dy++)
        for (int dx = 0; dx < 2; dx++) {
            unsigned cell = world_cell(cx + dx, cy + dy);
            Thing *hit = walk_cell(cell, [&](Thing *o) {
                return o->cls == (uint8_t)cls && o->type == (uint8_t)type
                    && (uint32_t)pos_dist_xyz(pos, thing_pos(o)) <= 0x80;
            });
            if (hit) return 1;
        }
    return 0;
}

// ---- damage ------------------------------------------------------------------------------------------

// thing_area_damage_10d20
void thing_area_damage(Thing *t, int slot_in, unsigned amount_in) {
    unsigned slot = slot_in & 0xff;
    uint32_t amount = amount_in & 0xffff;
    if (slot == 0) {
        for_each_castle([&](Thing *p) {
            if (!same_side(p->owner, t->owner) && thing_collide(t, p)) slot_hit(p, 0, amount, t->owner);
        });
        area_cells_slot0(t, [&](Thing *o) { slot_hit(o, 0, amount, t->owner); });
        return;
    }
    // Other damage types: cells of (x + 0x80) >> 8 +- radius, any class but 0, type bit in prop_flags.
    if (slot >= 6) return;                    // the original would write past the six slots
    uint16_t bit = (uint16_t)(1u << (slot & 0x1f));
    int cx = (world_signed(t->x) + 0x80) >> 8;
    int cy = (world_signed(t->y) + 0x80) >> 8;
    int r  = div256((int16_t)t->ext_x + 0xff);
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            unsigned cell = world_cell(cx + dx, cy + dy);
            walk_cell(cell, [&](Thing *o) {
                if (same_side(o->owner, t->owner)) return false;
                if (o->cls == 0) return false;
                if (!(o->flags & 8)) return false;
                if (!(o->prop_flags & bit)) return false;
                if (!filter_pass(t, o)) return false;
                if (!thing_collide(t, o)) return false;
                slot_hit(o, (int)slot, amount, t->owner);
                return false;
            });
        }
}

// thing_area_damage_11160
void thing_area_damage_fire(Thing *t, int slot, unsigned amount_in) {
    if ((slot & 0xff) != 0) return;
    uint32_t amount = amount_in & 0xffff;
    for_each_castle([&](Thing *p) {
        if (!same_side(p->owner, t->owner) && thing_collide(t, p)) slot_hit(p, 0, amount, t->owner);
    });
    area_cells_slot0(t, [&](Thing *o) {
        slot_hit(o, 0, (o->cls == 2 && o->type == 0) ? amount / 10 : amount, t->owner);
    });
}

// thing_area_damage_11450
void thing_area_damage_quake(Thing *t, int slot, unsigned amount_in) {
    if ((slot & 0xff) != 0) return;
    uint32_t amount = amount_in & 0xffff;
    for_each_castle([&](Thing *p) {
        if (!thing_collide(t, p)) return;
        p->duration = 0x1e;
        if (!same_side(p->owner, t->owner)) slot_hit(p, 0, amount, t->owner);
    });
    area_cells_slot0(t, [&](Thing *o) { slot_hit(o, 0, amount, t->owner); });
}

// thing_add_pending_damage_117c0
void thing_add_pending_damage(const Thing *attacker, Thing *victim, int slot_in, unsigned amount) {
    unsigned slot = slot_in & 0xff;
    if (slot >= 6) return;
    DamageSlot &s = victim->damage_slots[slot];
    if (s.attacker == 0) s.amount += (int32_t)(amount & 0xffff);
    else                 s.amount = (int32_t)(amount & 0xffff);
    s.attacker = attacker->owner;
}

// thing_try_damage_116e0
void thing_try_damage(const Thing *attacker, Thing *victim, int slot_in, unsigned amount) {
    unsigned slot = slot_in & 0xff;
    if (victim->cls == 0 || !(victim->flags & 8)) return;
    if (!(victim->prop_flags & (uint16_t)(1u << (slot & 0x1f)))) return;
    if (!filter_pass(attacker, victim)) return;
    if (same_side(attacker->owner, victim->owner)) return;
    if (!thing_collide(attacker, victim)) return;
    thing_add_pending_damage(attacker, victim, (int)slot, amount);
}

// cell_kill_things_3da30
void cell_kill_things(unsigned cell, unsigned owner_in) {
    uint16_t owner = (uint16_t)owner_in;
    walk_cell(cell & (g_world.cells - 1), [&](Thing *o) {
        if (same_side((int)(int16_t)o->owner, (int)owner)) return false;
        if (o->cls == 2) {
            thing_mark_delete(o);
        } else if (o->cls == 5) {
            if (o->type != 0x10 && o->type != 6 && o->type != 8) {
                o->health = -1;
                o->killer = owner;
                o->last_attacker = owner;
            }
        }
        return false;
    });
}

// ---- terrain checks ------------------------------------------------------------------------------------

// creature_check_terrain_102b0
uint32_t creature_check_terrain(const Thing *t, const Pos *pos, unsigned flags) {
    const MoveDesc *d = mc_move_desc(t->desc);
    if (flags & 2) {
        if ((int)pos->z < (int)(int16_t)terrain_height_at(pos) + d->clear_hi) return 1;
        if ((int)pos->z > (int)(int16_t)terrain_height_at(pos) + d->clear_lo) return 1;
    }
    if (flags & 1) {
        uint32_t bad = terrain_type_mask_at(pos) & ~d->terrain_mask;
        if (bad) return bad;
    }
    if (flags & 4) {
        unsigned pitch = (unsigned)pos_pitch_to(thing_pos(t), pos) & 0xffff;
        int dir = (int16_t)angle_turn_dir(0, (int)pitch);
        if (dir == 1) {
            if ((int)((unsigned)angle_diff(0, (int)pitch) & 0xffff) > (int)(int16_t)d->unk12) return 1;
        } else if (dir == -1) {
            if ((int)((unsigned)angle_diff(0, (int)pitch) & 0xffff) > (int)(int16_t)d->unk10) return 1;
        }
    }
    return 0;
}

// terrain_max_corner_level_10c30
int terrain_max_corner_level(const Pos *pos) {
    const int cx = world_cx(pos->x), cy = world_cx(pos->y);
    unsigned m = g_map_height[world_cell(cx, cy)];
    unsigned v = g_map_height[world_cell(cx + 1, cy)];
    if (m < v) m = v;
    v = g_map_height[world_cell(cx, cy + 1)];
    if (m < v) m = v;
    v = g_map_height[world_cell(cx + 1, cy + 1)];
    if (m < v) m = v;
    return (int)m >> 5;
}

// terrain_minmax_along_path_10cb0
int terrain_minmax_along_path(Pos *pos, unsigned yaw, int step, int count, int32_t out[3]) {
    out[2] = 0x40000000;
    out[0] = 0;
    for (int16_t n = (int16_t)count; n >= 0; n--) {
        int h = (int16_t)terrain_height_at(pos);
        if (h > out[0]) out[0] = h;
        if (h < out[2]) out[2] = h;
        math_rotate_offset(pos, (int)(yaw & 0xffff), 0, (int16_t)step);
    }
    return 1;
}

// ---- small shared helpers ------------------------------------------------------------------------------

// thing_aim_at_43ee0
void thing_aim_at(Thing *a, Thing *b) {
    thing_z_add_half_height(b);
    a->target_yaw = (uint16_t)pos_angle_to(thing_pos(a), thing_pos(b));
    a->target_pitch = (uint16_t)pos_pitch_to(thing_pos(a), thing_pos(b));
    thing_z_sub_half_height(b);
}

// castle_spell_reset_charge_41310
void castle_spell_reset_charge(const Thing *castle, int mode) {
    const Thing *owner = thing_at(thing_wrap((unsigned)(int16_t)castle->owner));
    int16_t spell;
    std::memcpy(&spell, thing_player_block(owner) + 0x2c4, 2);
    if (spell == 0) return;
    Thing *s = thing_at(thing_wrap((unsigned)spell));
    if ((int16_t)mode == 0) s->cast_ticks = 0;
    else                    s->cast_ticks = (int16_t)(s->duration - 1);
}
