// Creature shared code of carpet.exe (0x18050..0x19b70) and the wake timers (0x468e0, 0x46960).
// Translated from the disassembly; see creatures.h for the state layout and the field notes.
//
// Conventions: every function takes its arguments on the stack in the order of the C++ signature.
// Thing pointers of the original are thing indices here, so "ptr > &things[0]" list walks become
// "index != 0" and "(ptr - things) / 0xa4" becomes thing_index().
#include "creatures.h"
#include "mc_math.h"

namespace {

constexpr uint32_t desc_index(uint32_t addr) { return (addr - 0x96a10u) / 0x20u; }   // Thing+0x9c values

inline const MoveDesc *desc_of(const Thing *t) { return mc_move_desc(t->desc); }

inline PlayerBlock *owner_block(const Thing *t) {     // Thing+0xa0 (PlayerRec+0x44f; the dummy block for 0)
    return reinterpret_cast<PlayerBlock *>(thing_player_block(t));
}

// "tick % desc.think_period == 0" (xor edx,edx; mov dl,[t+0x3f]; movsx ecx,[desc+0x1a]; idiv). The
// original faults on a zero period; no descriptor a creature uses has one.
inline bool think_now(const Thing *t) {
    int period = (int16_t)desc_of(t)->think_period;
    return period != 0 && (int)t->tick % period == 0;
}

// (dx^2 + dy^2) of the torus-wrapped coordinate differences, unsigned. The original's unsigned 32-bit
// sum of the 16-bit wrapped differences (at most 2^31, so the same value in 64 bits with 256 cells);
// round 11: 64-bit for a wider world (world.h).
inline uint64_t dist_sq_xy16(const Thing *a, const Thing *b) {
    int64_t dx = world_signed(a->x - b->x);
    int64_t dy = world_signed(a->y - b->y);
    return (uint64_t)(dx * dx) + (uint64_t)(dy * dy);
}

// (int16)desc.sight_radius squared, as the 32-bit imul leaves it.
inline uint64_t sight_sq(const Thing *t) {
    int32_t s = (int16_t)desc_of(t)->sight_radius;
    return (uint32_t)(s * s);
}

// "no limit" / "nothing found yet" of the nearest searches: the original's 0xffffffff, which no 256-world
// distance reaches; a wider world needs the 64-bit maximum (distances beyond 256 cells exceed 2^32).
constexpr uint64_t kFarSq = ~(uint64_t)0;

// angle_diff(t.yaw, pos_angle_to(t, o)) < (int16)desc.fov
inline bool in_fov(const Thing *t, const Thing *o) {
    int fov = (int16_t)desc_of(t)->fov;
    int ang = pos_angle_to(thing_pos(t), thing_pos(o)) & 0xffff;
    return (angle_diff(t->yaw, ang) & 0xffff) < fov;
}

// The flock-leader search shared by 18610 and 18870: nearest creature of t's type that has no leader
// itself, inside the sight radius and the field of view.
Thing *find_leader(Thing *t) {
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

// One attempt of creature_move_step: scratch := position, ground following, step along the yaw.
void move_probe(Thing *t) {
    const MoveDesc *d = desc_of(t);
    g_pos_scratch = *thing_pos(t);
    int ground = (int16_t)terrain_height_at(&g_pos_scratch);
    // pushes: desc+0xe, desc+0xa, desc+0xc, ground, pos
    pos_follow_ground(&g_pos_scratch, ground, (int16_t)d->clear_hi /* +0xc */, (int16_t)d->clear_lo /* +0xa */,
                      (int16_t)d->z_step);
    math_rotate_offset(&g_pos_scratch, t->yaw, 0, (int16_t)t->speed_cur);
}

// Commit the probed position and turn toward target_yaw (the tail every successful attempt shares).
void move_commit(Thing *t) {
    const MoveDesc *d = desc_of(t);
    thing_move_to(t, &g_pos_scratch);
    int step = angle_turn_step(t->yaw, t->target_yaw, d->unk4 /* +4 */, d->turn_min /* +2 */);
    t->yaw = (uint16_t)((t->yaw + step) & 0x7ff);
}

bool move_probe_ok(Thing *t) {
    if (creature_check_terrain(t, &g_pos_scratch, 1) != 0) return false;
    return terrain_slope_at(&g_pos_scratch) < (int16_t)desc_of(t)->unk10;   // desc+0x10: steepest slope walked
}

// The part every projectile attack callback shares: owner, aim, raise by the shooter's height, target.
void projectile_aim(Thing *p, const Thing *t, const Thing *target) {
    p->owner = t->owner;
    p->yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(target));
    p->pitch = (uint16_t)pos_pitch_to(thing_pos(t), thing_pos(target));
    p->z = (int16_t)(p->z + t->ext_h);
    p->target = t->target;
}

} // namespace

// ---- inlined building blocks ---------------------------------------------------------------------

int creature_apply_damage(Thing *t) {
    int r = 0;
    if (t->timer_a != 0) {
        uint16_t attacker = t->damage_slots[0].attacker;
        if (attacker != 0) {
            t->health -= t->damage_slots[0].amount;
            t->damage_slots[0].attacker = 0;
            r = 1;
        }
        t->last_attacker = attacker;
        if (t->child != 0) {
            for (Thing *seg = thing_at(t->child); seg != thing_at(0); seg = thing_at(seg->child)) {
                if (seg->health < t->health) {
                    t->health = seg->health;
                    t->last_attacker = seg->last_attacker;
                    r = 1;
                    break;
                }
            }
        }
    }
    if (t->health < 0) {
        t->killer = t->last_attacker;
        r = 2;
    }
    return r;
}

void creature_wander_turn(Thing *t) {
    t->rng = mc_lcg(t->rng);
    int dir = (int)((t->rng % 0x9du) / 0x4fu) * 2 - 1;        // -1 for 0..78, +1 for 79..156
    t->rng = mc_lcg(t->rng);
    int amount = (int)(t->rng & 0xff) + 0x55;
    t->target_yaw = (uint16_t)((t->target_yaw + dir * amount) & 0x7ff);
}

// ---- 0x18050..0x18610 ----------------------------------------------------------------------------

// creature_segment_update_18050
void creature_segment_update(Thing *t) {
    Thing *parent = thing_at(t->parent);
    if (parent->cls != 5) thing_mark_delete(t);
    if (t->timer_a != 0) {
        t->yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(parent));
        t->pitch = (uint16_t)pos_pitch_to(thing_pos(t), thing_pos(parent));
        g_pos_scratch = *thing_pos(parent);
        math_rotate_offset(&g_pos_scratch, t->yaw, t->pitch, (int16_t)(0 - t->speed));
        thing_move_to(t, &g_pos_scratch);
        uint16_t attacker = t->damage_slots[0].attacker;
        if (attacker != 0) {
            t->health -= t->damage_slots[0].amount;
            t->damage_slots[0].attacker = 0;
        }
        t->last_attacker = attacker;
    } else if ((t->tick & 3) == 0) {
        thing_move_to(t, thing_pos(parent));
        t->yaw = parent->yaw;
    }
}

// terrain_slope_at_18150
int terrain_slope_at(const Pos *pos) {
    const int x = world_cx(pos->x), y = world_cx(pos->y);
    int h00 = g_map_height[world_cell(x, y)];
    int h10 = g_map_height[world_cell(x + 1, y)];
    int h11 = g_map_height[world_cell(x + 1, y + 1)];
    int h01 = g_map_height[world_cell(x, y + 1)];
    int a = h00 + h01 - h10 - h11;
    int b = h10 + h00 - h01 - h11;
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    return a > b ? a : b;
}

// creature_move_step_181e0
int creature_move_step(Thing *t) {
    move_probe(t);
    // same cell: no terrain test at all
    if ((t->x >> 8) == (g_pos_scratch.x >> 8) && (t->y >> 8) == (g_pos_scratch.y >> 8)) {
        move_commit(t);
        return 1;
    }
    if (move_probe_ok(t)) {
        move_commit(t);
        return 1;
    }
    uint16_t yaw = t->yaw;
    t->yaw = (uint16_t)((yaw + 0x155) & 0x7ff);
    move_probe(t);
    if (move_probe_ok(t)) {
        move_commit(t);
        return 1;
    }
    t->yaw = (uint16_t)((yaw - 0x155) & 0x7ff);
    move_probe(t);
    if (move_probe_ok(t)) {
        move_commit(t);
        return 1;
    }
    t->yaw = (uint16_t)((yaw + 0x400) & 0x7ff);
    move_probe(t);
    if (move_probe_ok(t)) {
        move_commit(t);
        return 1;
    }
    t->health = -1;         // boxed in: dies (no killer)
    return 1;
}

// ---- 0x18610..0x19360: the state bodies ----------------------------------------------------------

// creature_idle_seek_leader_18610
void creature_idle_seek_leader(Thing *t, int base) {
    switch (creature_apply_damage(t)) {
    case 2:
        thing_set_state(t, (uint8_t)(base + 4));
        return;
    case 1:
        if (thing_at(t->last_attacker)->cls != 3) return;
        t->target = t->last_attacker;
        thing_set_state(t, (uint8_t)(base + 2));
        return;
    default:
        break;
    }
    if (!think_now(t)) return;
    Thing *leader = find_leader(t);
    if (!leader) return;
    t->parent = thing_index(leader);
    thing_set_state(t, (uint8_t)(base + 3));
}

// creature_ai_step_18870
void creature_ai_step(Thing *t, int base) {
    switch (creature_apply_damage(t)) {
    case 2:
        thing_set_state(t, (uint8_t)(base + 4));
        return;
    case 1:
        if (thing_at(t->last_attacker)->cls != 3) return;
        t->target = t->last_attacker;
        thing_set_state(t, (uint8_t)(base + 2));
        return;
    default:
        break;
    }
    creature_move_step(t);
    if (!think_now(t)) return;
    creature_wander_turn(t);
    if (t->timer_a == 0) return;

    // nearest thing of the player list (wizards, castles, balloons) in sight and in the field of view
    uint64_t limit = sight_sq(t);
    uint64_t best_d = kFarSq;
    Thing *best = nullptr;
    for (uint32_t i = g_cfg->player_list; i != 0; i = thing_at(i)->next) {
        Thing *p = thing_at(i);
        uint64_t d = dist_sq_xy16(p, t);
        if (d > limit) continue;
        if (p->flags & 0x20) continue;
        if (!in_fov(t, p)) continue;
        if (d >= best_d) continue;
        best = p;
        best_d = d;
    }
    if (best) {
        t->target = thing_index(best);
        thing_set_state(t, (uint8_t)(base + 2));
        return;
    }
    Thing *leader = find_leader(t);
    if (!leader) return;
    t->parent = thing_index(leader);
    thing_set_state(t, (uint8_t)(base + 3));
}

// creature_attack_target_18c20
int creature_attack_target(Thing *t, int base, CreatureAttackFn attack) {
    switch (creature_apply_damage(t)) {
    case 2:
        thing_set_state(t, (uint8_t)(base + 4));
        return 0;
    case 1:
        if (thing_at(t->last_attacker)->cls == 3) t->target = t->last_attacker;   // no state change
        return 0;
    default:
        break;
    }
    creature_move_step(t);
    Thing *target = thing_at(t->target);
    if ((t->tick & 3) == 0) t->target_yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(target));
    if (target->health < 0 || (target->flags & 0x400)) {
        thing_set_state(t, (uint8_t)(base + 1));
        return 0;
    }
    if (!think_now(t)) return 0;
    int sight = (int16_t)desc_of(t)->sight_radius;
    if ((uint32_t)pos_dist_xyz(thing_pos(t), thing_pos(target)) >= (uint32_t)sight) {
        thing_set_state(t, (uint8_t)(base + 1));
        return 0;
    }
    return (int16_t)attack(t, target) != 0 ? 1 : 0;
}

// creature_follow_leader_18e90
void creature_follow_leader(Thing *t, int base) {
    if (t->parent == 0) {
        // TODO(port): vga_set_dac_entry_4cdf0(0, 0x3f, 0, 0) - leftover debug aid (border colour red)
        thing_set_state(t, (uint8_t)(base + 1));
        return;
    }
    Thing *leader = thing_at(t->parent);
    int r = creature_apply_damage(t);
    uint8_t attack_state = (uint8_t)(base + 2);
    if (r == 2) {
        // dying: the leader goes for the killer
        leader->target = t->last_attacker;
        leader->parent = 0;
        thing_set_state(leader, attack_state);
        thing_set_state(t, (uint8_t)(base + 4));
        return;
    }
    if (r == 1) {
        if (thing_at(t->last_attacker)->cls != 3) return;
        leader->parent = 0;
        leader->target = t->last_attacker;
        thing_set_state(leader, attack_state);
        t->parent = 0;
        t->target = t->last_attacker;
        thing_set_state(t, attack_state);
        return;
    }
    creature_move_step(t);
    if (!think_now(t)) return;
    // jump table 0x18e74 on (int8)leader.state - base: 0, 1 follow; 2 attack; 3 chain; 4, 5 and
    // everything else: drop the leader
    unsigned rel = (unsigned)((int8_t)leader->state - (int)(uint16_t)base);
    switch (rel) {
    case 3:
        t->parent = leader->parent;
        [[fallthrough]];
    case 0:
    case 1: {
        t->target_yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(thing_at(t->parent)));
        // separation: turn away from the first creature of the type with another owner within 0x100
        for (uint32_t i = g_cfg->creature_lists[(int8_t)t->type]; i != 0; i = thing_at(i)->next) {
            Thing *o = thing_at(i);
            if (o->owner == t->owner) continue;
            // (operands sign-extended before the subtraction, no wrap: the original's quirk; with N > 256
            // world_signed centres the coordinates on 0 the same way, so the test misses across the
            // half-world line exactly as the original misses across x = 0x8000)
            int dx = world_signed(t->x) - world_signed(o->x);
            if (dx < 0) dx = -dx;
            if (dx >= 0x100) continue;
            int dy = world_signed(t->y) - world_signed(o->y);
            if (dy < 0) dy = -dy;
            if (dy >= 0x100) continue;
            t->target_yaw = (uint16_t)pos_angle_to(thing_pos(o), thing_pos(t));
            break;
        }
        t->speed_cur = (int16_t)(leader->speed_cur + leader->turn_rate);
        return;
    }
    case 2:
        t->target = leader->target;
        t->parent = 0;
        thing_set_state(t, attack_state);
        return;
    default:
        t->parent = 0;
        thing_set_state(t, (uint8_t)(base + 1));
        return;
    }
}

// creature_die_191d0
void creature_die(Thing *t, int base) {
    uint8_t dead_state = (uint8_t)(base + 5);
    for (Thing *seg = thing_at(t->child); seg != thing_at(0); seg = thing_at(seg->child)) {
        thing_set_state(seg, dead_state);
        if (seg->killer != 0) t->killer = seg->killer;
    }
    Thing *killer = thing_at(t->killer);
    if (killer != thing_at(0) && killer->cls == 3 && killer->type == 0 &&
        !(t->type == 9 && t->mana_owner > 0) &&
        (int16_t)t->owner == (int)thing_index(t)) {
        uint8_t ty = t->type;
        if (ty != 0xc && ty != 0xd && ty != 0xe && ty != 0xf && ty != 9) owner_block(killer)->kills++;
    }
    thing_set_state(t, dead_state);
}

// creature_dead_drop_mana_19310
void creature_dead_drop_mana(Thing *t) {
    if (t->tick & 7) return;
    if (g_cfg->flags & 0x200) t->mana = 5000;      // test byte [cfg+1], 2
    if (g_hook_thing_drop_mana_ball) g_hook_thing_drop_mana_ball(t);   // thing_drop_mana_ball_25fe0
    Thing *fx = thing_create(thing_pos(t), 10, 1);
    if (fx) fx->owner = t->owner;
    thing_mark_delete(t);
}

// creature_adopt_leader_19360
void creature_adopt_leader(Thing *t, Thing *other, int state) {
    uint16_t leader;
    if (other->parent != 0) {
        if (other->parent == thing_index(t)) return;
        if (thing_at(other->parent)->cls == 0) return;
        leader = other->parent;
    } else {
        leader = thing_index(other);
    }
    t->parent = leader;
    t->state = (uint8_t)state;
}

// ---- 0x193f0..0x19b70: attack callbacks ----------------------------------------------------------

// creature_attack_fire_193f0
int creature_attack_fire(Thing *t, Thing *target) {
    Thing *p = thing_create(thing_pos(t), 9, 0);
    if (!p) return 0;
    p->impact_cls = 10;
    p->impact_type = 0;
    projectile_aim(p, t, target);
    p->desc = desc_index(0x96ad0);
    p->filter_type = t->filter_type;
    p->damage = 500;
    p->filter_cls = t->filter_cls;
    return 1;
}

// creature_attack_arrow_194a0
int creature_attack_arrow(Thing *t, Thing *target) {
    Thing *p = thing_create(thing_pos(t), 9, 0xd);
    if (!p) return 0;
    projectile_aim(p, t, target);
    p->filter_type = t->filter_type;
    p->damage = 0xfa;
    p->filter_cls = t->filter_cls;
    thing_set_sprite_double(p, 0xc3);
    return 1;
}

// skeleton_attack_fire_19550
int skeleton_attack_fire(Thing *t, Thing *target) {
    Thing *p = thing_create(thing_pos(t), 9, 0xd);
    if (!p) return 0;
    projectile_aim(p, t, target);
    p->filter_type = t->filter_type;
    p->filter_cls = t->filter_cls;
    p->damage = t->mana_owner != 0 ? 600 : 400;
    thing_set_sprite_double(p, 0xcb);
    return 1;
}

// creature_attack_melee_19620
int creature_attack_melee(Thing *t, Thing *target) {
    if (pos_dist_xyz(thing_pos(t), thing_pos(target)) >= 0x400) return 0;
    thing_add_pending_damage(t, target, 0, t->damage);
    return 1;
}

// creature_attack_volley_19680
int creature_attack_volley(Thing *t, Thing *target) {
    uint16_t owner = t->owner;
    uint16_t yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(target));
    uint16_t pitch = (uint16_t)pos_pitch_to(thing_pos(t), thing_pos(target));
    int16_t  raise = t->ext_h;
    uint16_t tgt = t->target;
    uint8_t  ftype = t->filter_type, fcls = t->filter_cls;
    // shots = carried mana * 7 / total mana (unsigned 32-bit; the original faults on mana_total == 0)
    uint32_t n32 = t->mana_total != 0 ? ((uint32_t)t->mana * 7u) / (uint32_t)t->mana_total : 0;
    uint16_t n = (uint16_t)n32;
    uint16_t kind = 0;
    if (n != 0) {
        t->rng = mc_lcg(t->rng);
        kind = (uint16_t)((t->rng % ((uint32_t)n * 100u)) / 100u);
    }
    if (n < 1) n = 1;
    if (n > 5) n = 5;

    auto fill = [&](Thing *p, int impact_type, uint32_t desc, int damage) {
        p->impact_type = (uint8_t)impact_type;
        p->desc = desc;
        p->filter_type = ftype;
        p->filter_cls = fcls;
        p->impact_cls = 10;
        p->owner = owner;
        p->yaw = yaw;
        p->pitch = pitch;
        p->damage = (uint16_t)damage;
        p->z = (int16_t)(p->z + raise);
        p->target = tgt;
    };

    Thing *last = nullptr;          // edi: the result of the last thing_create
    if (kind == 0) {                // jump table 0x19664: 0 -> spread of small shots
        for (uint16_t i = 0; i < n; i++) {
            last = thing_create(thing_pos(t), 9, 0);
            if (last) fill(last, 0, 6u - i, 400);                  // 0x96a10 + (6 - i) * 0x20
        }
    } else if (kind <= 2) {         // 1, 2 -> n - 1 heavier shots
        for (uint16_t i = 1; i < n; i++) {
            last = thing_create(thing_pos(t), 9, 9);
            if (last) fill(last, 0x17, 6u - i, 800);
        }
    } else if (kind <= 6) {         // 3..6 -> one big shot
        last = thing_create(thing_pos(t), 9, 3);
        if (last) fill(last, 0x11, desc_index(0x96a70), 8000);
    }
    return last ? 1 : 0;
}

// creature_attack_fire_troll_19940
int creature_attack_fire_troll(Thing *t, Thing *target) {
    Thing *p = thing_create(thing_pos(t), 9, 0xe);
    if (!p) return 0;
    p->impact_cls = 10;
    p->impact_type = 0;
    projectile_aim(p, t, target);
    p->desc = desc_index(0x96ad0);
    p->filter_type = t->filter_type;
    p->damage = 0x30c;
    p->filter_cls = t->filter_cls;
    return 1;
}

// creature_attack_fire_griffon_199f0
int creature_attack_fire_griffon(Thing *t, Thing *target) {
    Thing *p = thing_create(thing_pos(t), 9, 9);
    if (!p) return 0;
    p->impact_cls = 10;
    p->impact_type = 0x17;
    projectile_aim(p, t, target);
    p->desc = desc_index(0x96ad0);
    p->filter_type = target->filter_type;       // from the target, unlike the others
    p->damage = 4000;
    p->filter_cls = target->filter_cls;
    return 1;
}

// creature_attack_fire_homing_desc_19a90 (no reference in the retail image)
int creature_attack_fire_homing_desc(Thing *t, Thing *target) {
    Thing *p = thing_create(thing_pos(t), 9, 0);
    if (!p) return 0;
    p->impact_cls = 10;
    p->impact_type = 0;
    p->desc = desc_index(0x96a50);
    p->filter_type = t->filter_type;
    p->filter_cls = t->filter_cls;
    projectile_aim(p, t, target);
    g_pos_scratch = *thing_pos(t);
    math_rotate_offset(&g_pos_scratch, p->yaw, p->pitch, 0);    // distance 0: returns at once
    thing_move_to(p, &g_pos_scratch);
    return 1;
}

// ---- 0x468e0, 0x46960: wake timers ---------------------------------------------------------------

// creature_wake_tick_468e0
void creature_wake_tick() {
    for (int type = 0; type < 0x14; type++) {
        for (uint32_t i = g_cfg->creature_lists[type]; i != 0; i = thing_at(i)->next) {
            Thing *t = thing_at(i);
            if (t->health < 0) {
                t->timer_a = 0xfa;
                t->timer_b = 0;
            } else {
                creature_proximity_wake_timer(t);
            }
        }
    }
    for (uint32_t i = g_cfg->mana_ball_list; i != 0; i = thing_at(i)->next)
        creature_proximity_wake_timer(thing_at(i));
}

// creature_proximity_wake_timer_46960
int creature_proximity_wake_timer(Thing *t) {
    if (t->timer_a != 0) {
        t->timer_a--;
        for (Thing *seg = thing_at(t->child); seg != thing_at(0); seg = thing_at(seg->child))
            seg->timer_a = t->timer_a;
        return 0;
    }
    if (t->timer_b != 0) {
        t->timer_b--;
        return 0;
    }
    const Thing *local = thing_at(g_state->players[g_state->local_player].thing);
    // pos_dist_sq_xy_3e970(thing pos, local pos): eax = dx^2 + dy^2 and edx is left holding dy^2
    // round 11: torus differences / int64 distance (world.h; the original's int32 sum with 256 cells,
    // which wraps negative - "near" - at exactly 128 cells on both axes). The wake-near-any-wizard rule
    // of a wide world is session 3.
    int32_t dy = world_signed(local->y - t->y);
    int32_t dx = world_signed(local->x - t->x);
    int64_t d2 = world_dist_sq(dx, dy);
    if (d2 < 0x2400000) {
        // "push edx; call math_isqrt_4cd7a": the root of dy^2, not of the distance (dy^2 <= 2^30 here
        // with 256 cells; below 0x2400000 otherwise)
        t->cast_ticks = (int16_t)mc_isqrt((uint32_t)((int64_t)dy * dy));
        t->timer_a = 0x10;
        for (Thing *seg = thing_at(t->child); seg != thing_at(0); seg = thing_at(seg->child))
            seg->timer_a = (uint8_t)(t->timer_a + 2);
    }
    t->timer_b = 0;
    return 0;
}
