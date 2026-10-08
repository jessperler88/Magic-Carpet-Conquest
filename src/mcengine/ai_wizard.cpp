// Computer-controlled wizard AI of carpet.exe: 0x11de0..0x15590, translated from the disassembly
// (tools/analysis/img.py dis 0x11de0 0x15590). See ai_wizard.h for the overview and
// docs/analysis/port_ai_wizard.md for the verification.
//
// Conventions kept from the original: every Thing reached through an index field is `things[idx]`
// with "idx == 0 means none" where the original compared the pointer with &things[0]; divisions are
// truncating (Watcom emits idiv or the sar/sbb sequence for signed division by a power of two);
// the threat table is indexed raw with player_threat() because its 8th entry overlaps
// PlayerBlock.ai_aggression.
#include "ai_wizard.h"
#include "player.h"
#include "level_features.h"
#include "world_set.h"
#include "gen/ai_wizard_tables.h"
#include <cstring>

uint16_t g_ai_human_wizard = 0;     // DAT_000acac0
uint32_t g_ai_rand_seed = 1;        // DAT_0009e5c8

namespace {

inline int iabs(int v) { return v < 0 ? -v : v; }
inline int isign(int v) { return v == 0 ? 0 : v / iabs(v); }          // the cdq / xor / sub / idiv idiom

// things[idx] as the original addresses it (`idx * 0xa4 + &things[0]`): never null; indices outside
// the pool (impossible for valid state) read slot 0 instead of foreign memory.
inline Thing *thing_ref(unsigned idx) { return thing_at(idx < (unsigned)thing_pool_slots() ? idx : 0); }
// The same with the original's "pointer <= &things[0]" test: null for index 0.
inline Thing *thing_or_null(unsigned idx) { return (idx != 0 && idx < (unsigned)thing_pool_slots()) ? thing_at(idx) : nullptr; }

inline uint16_t &threat_of(PlayerBlock *P, int player) { return *player_threat(P, player & 7); }
inline uint16_t &grudge_of(PlayerBlock *P, int player) { return *(player_threat(P, player & 7) + 1); }

// 50000 - (other.mana_total / 10 * aggression) / 255: the hostility threshold a threat value must
// exceed (13000, 13210, 13440, 13ce0, 150f0).
inline int hostility_threshold(const Thing *other, const PlayerBlock *P) {
    return 50000 - (other->mana_total / 10 * P->ai_aggression) / 255;
}

// Thing.target / Thing.unk94 := the chosen target and its signature.
inline void set_target(Thing *t, const Thing *target) {
    t->target = thing_index(target);
    t->unk94 = thing_signature(target);
}

// The hover tail shared by the mode handlers: stop and move z toward target_z + 0x200 by the
// descriptor's z step.
void hover_toward(Thing *t, int target_z) {
    if (!engine1995()) player_block(t)->target_speed = 0;      // 1995 (CD 0x1388a etc.): the speed is left alone
    int d = (int16_t)t->z - (target_z + 0x200);
    const MoveDesc *desc = mc_move_desc(t->desc);
    t->z = (int16_t)((int16_t)t->z + (int16_t)desc->z_step * isign(d));
}

// pos_dist_sq_xy as the nearest searches compare it: unsigned. With 256 cells the original's uint32 of
// the int32 sum (0x80000000 at exactly 128 cells on both axes); a wider world's int64 square as is.
inline uint64_t dist_sq_u(const Pos *a, const Pos *b) {
    const int64_t d = pos_dist_sq_xy(a, b);
    return d < 0 ? (uint64_t)(uint32_t)d : (uint64_t)d;
}
// The original's 0xffffffff "none yet" (no 256-world distance reaches it); 64-bit for a wider world.
constexpr uint64_t kFarSq = ~(uint64_t)0;

// Walks a Config list (player_list, mana_ball_list, projectile_list, creature_lists[]).
struct ListWalk {
    uint32_t idx;
    explicit ListWalk(uint32_t head) : idx(head) {}
    bool ok() const { return idx != 0 && idx < (uint32_t)thing_pool_slots(); }
    Thing *get() const { return thing_at(idx); }
    void next() { idx = thing_at(idx)->next; }
};

} // namespace

// ---- crt_rand_5aff8 -------------------------------------------------------------------------------
int ai_rand() {
    g_ai_rand_seed = g_ai_rand_seed * 0x41c64e6du + 0x3039u;
    return (int)((g_ai_rand_seed >> 16) & 0x7fff);
}

// ---- thing_signature_14080 / ai_target_valid_140a0 ------------------------------------------------
uint16_t thing_signature(const Thing *t) {
    int v = ((int8_t)t->cls << 7) + (int8_t)t->type;
    return (uint16_t)(v + t->owner);
}

int ai_target_valid(const Thing *t, const Thing *target) {
    return thing_signature(target) == t->unk94 ? 1 : 0;
}

// ---- ai_get_spell_thing_13ac0 ---------------------------------------------------------------------
Thing *ai_get_spell_thing(const Thing *t, int spell) {
    spell &= 0xff;
    if (spell >= 24) return nullptr;                                   // (the original indexes P+0x2a4 unchecked)
    int idx = (int16_t)player_block(t)->spell_thing[spell];
    return (idx > 0 && idx < thing_pool_slots()) ? thing_at((unsigned)idx) : nullptr;
}

// ---- finders 13ec0 / 13fa0 / 14010 ----------------------------------------------------------------
// player_find_nearest_by_type_13ec0(thing, type): nearest other-owner player Thing of `type` (0, 2, 3)
// or of any type (0xff); type 1 and anything else -> null.
Thing *player_find_nearest_by_type(const Thing *t, int type) {
    type &= 0xff;
    Thing *best = nullptr;
    uint64_t best_d = kFarSq;
    if (type == 0xff) {
        for (ListWalk w(g_cfg->player_list); w.ok(); w.next()) {
            Thing *p = w.get();
            if (p->owner == t->owner) continue;
            uint64_t d = dist_sq_u(thing_pos(t), thing_pos(p));
            if (d < best_d) { best = p; best_d = d; }
        }
    } else if (type == 0 || type == 2 || type == 3) {
        for (ListWalk w(g_cfg->player_list); w.ok(); w.next()) {
            Thing *p = w.get();
            if (p->owner == t->owner) continue;
            if ((int8_t)p->type != type) continue;
            uint64_t d = dist_sq_u(thing_pos(t), thing_pos(p));
            if (d < best_d) { best = p; best_d = d; }
        }
    }
    return best;
}

// player_find_nearest_wizard_excl_13fa0(thing, excl): nearest wizard (type 0 / 1) whose owner is
// neither thing's nor excl's.
Thing *player_find_nearest_wizard_excl(const Thing *t, const Thing *excl) {
    Thing *best = nullptr;
    uint64_t best_d = kFarSq;
    for (ListWalk w(g_cfg->player_list); w.ok(); w.next()) {
        Thing *p = w.get();
        if (p->owner == t->owner || p->owner == excl->owner) continue;
        if (p->type != 0 && p->type != 1) continue;
        uint64_t d = dist_sq_u(thing_pos(t), thing_pos(p));
        if (d < best_d) { best = p; best_d = d; }
    }
    return best;
}

// player_find_nearest_castle_excl_14010(thing, excl)
Thing *player_find_nearest_castle_excl(const Thing *t, const Thing *excl) {
    Thing *best = nullptr;
    uint64_t best_d = kFarSq;
    for (ListWalk w(g_cfg->player_list); w.ok(); w.next()) {
        Thing *p = w.get();
        if (p->owner == t->owner || p->owner == excl->owner) continue;
        if (p->type != 2) continue;
        uint64_t d = dist_sq_u(thing_pos(t), thing_pos(p));
        if (d < best_d) { best = p; best_d = d; }
    }
    return best;
}

// ---- ai_cache_human_wizard_15540 ------------------------------------------------------------------
int ai_cache_human_wizard(const Thing * /*t*/) {
    uint16_t found = g_ai_human_wizard;
    for (ListWalk w(g_cfg->player_list); w.ok(); w.next())
        if (w.get()->type == 0) found = (uint16_t)w.idx;
    g_ai_human_wizard = found;
    return 1;
}

// ---- ai_set_mode (13880 .. 13a00) -----------------------------------------------------------------
int ai_set_mode(Thing *t, int mode) {
    player_block(t)->ai_mode = (uint8_t)mode;
    return 1;
}

// ---- spell state ----------------------------------------------------------------------------------
// ai_spell_active_14aa0
int ai_spell_active(Thing *t, int spell) {
    if ((spell & 0xff) >= 0x18) return 0;
    Thing *s = ai_get_spell_thing(t, spell);
    return (s && s->cast_ticks > 0) ? 1 : 0;
}

// ai_spell_in_progress_14c40
Thing *ai_spell_in_progress(const Thing *t, int spell) {
    Thing *s = ai_get_spell_thing(t, spell);
    return (s && s->cast_ticks > 0) ? s : nullptr;
}

// ai_can_afford_spell_14ad0 (the original reads the spell Thing without a null test; every caller
// has tested it before)
int ai_can_afford_spell(Thing *t, int spell) {
    Thing *s = ai_get_spell_thing(t, spell);
    if (!s) return 0;
    return s->mana_total <= t->mana_total ? 1 : 0;
}

// ai_has_any_attack_spell_154e0
int ai_has_any_attack_spell(const Thing *t) {
    return (ai_get_spell_thing(t, 0) || ai_get_spell_thing(t, 0xf) || ai_get_spell_thing(t, 8) ||
            ai_get_spell_thing(t, 0x11) ||
            (engine1995() && ai_get_spell_thing(t, 0x14)) ||           // 1995 (CD 0x16961): Fire Wall too
            ai_get_spell_thing(t, 7)) ? 1 : 0;
}

// ai_spell_ready_14640(thing, spell): jump table 0x145f4 for spells 0..0x11, generic test above.
int ai_spell_ready(Thing *t, int spell) {
    spell &= 0xff;
    PlayerBlock *P = player_block(t);
    Thing *s;
    // 1995 (CD 0x15a00, jump table 0x159ac for 0..0x14): Fire Wall (0x14) is tested as an aimed
    // spell like 3 / 7 / 8 / 0x11; 1996 sends it to the generic test.
    int kind = (spell == 0x14 && engine1995()) ? 3 : spell;
    switch (kind) {
    case 2:                                                         // 0x1465f: speed-up, no cooldown test
        s = ai_get_spell_thing(t, spell);
        if (!s) return 0;
        return t->mana >= s->mana_total ? 1 : 0;
    case 4: case 0xc: case 0xe:                                     // 0x14691: not casting, cooldown, mana
        s = ai_get_spell_thing(t, spell);
        if (!s || s->cast_ticks != 0 || P->spell_cooldown[spell] != 0) return 0;
        return t->mana >= s->mana_total ? 1 : 0;
    case 3: case 7: case 8: case 0x11:                              // 0x146e1: as above plus the aim test
        s = ai_get_spell_thing(t, spell);
        if (!s || s->cast_ticks != 0 || P->spell_cooldown[spell] != 0) return 0;
        if (t->mana < s->mana_total) return 0;
        return angle_diff(t->yaw, t->target_yaw) < ai_aim_tolerance(P->ai_accuracy) ? 1 : 0;
    case 0: case 0xb: case 0xd: case 0xf:                           // 0x148a3: cooldown, mana, aim (may be mid-cast)
        s = ai_get_spell_thing(t, spell);
        if (!s || P->spell_cooldown[spell] != 0) return 0;
        if (t->mana < s->mana_total) return 0;
        return angle_diff(t->yaw, t->target_yaw) < ai_aim_tolerance(P->ai_accuracy) ? 1 : 0;
    case 0x10: {                                                    // 0x14783: the castle spell
        s = ai_get_spell_thing(t, spell);
        if (!s) return 0;
        if (P->castle == 0) {
            if (P->spell_cooldown[spell] != 0) return 0;
            return t->mana >= s->mana_total ? 1 : 0;
        }
        if (s->cast_ticks != 0 || P->spell_cooldown[spell] != 0) return 0;
        if ((uint8_t)castle_footprint_clear(thing_ref(P->castle)) == 0) return 0;
        if (t->mana < s->mana_total) return 0;
        return angle_diff(t->yaw, t->target_yaw) < ai_aim_tolerance(P->ai_accuracy) ? 1 : 0;
    }
    default:                                                        // 1, 5, 6, 9, 0xa and 0x12..0x17 (0x14936)
        if (spell >= 0x18) return 0;
        s = ai_get_spell_thing(t, spell);
        if (!s || P->spell_cooldown[spell] != 0) return 0;
        return t->mana >= s->mana_total ? 1 : 0;
    }
}

// ai_castle_spell_ready_14980: the upgrade test of ai_goal_upgrade_castle (mana_total instead of mana).
int ai_castle_spell_ready(Thing *t) {
    Thing *s = ai_get_spell_thing(t, 0x10);
    if (!s) return 0;
    PlayerBlock *P = player_block(t);
    if (P->castle == 0) {
        if (P->spell_cooldown[0x10] != 0) return 0;
        return t->mana_total >= s->mana_total ? 1 : 0;
    }
    if (s->cast_ticks != 0 || P->spell_cooldown[0x10] != 0) return 0;
    if ((uint8_t)castle_footprint_clear(thing_ref(P->castle)) == 0) return 0;
    if (t->mana_total < s->mana_total) return 0;
    return angle_diff(t->yaw, t->target_yaw) < ai_aim_tolerance(P->ai_accuracy) ? 1 : 0;
}

// ai_cast_spell_14240(thing, spell): jump table 0x141ec for spells 0..0x11.
int ai_cast_spell(Thing *t, int spell) {
    spell &= 0xff;
    if (!ai_spell_ready(t, spell)) return 0;
    t->flags &= ~0x100u;                                            // left-hand cast flag
    // 1995 (CD 0x155f0, jump table 0x15590 for 0..0x14): 0x12 / 0x13 return 0 and Fire Wall (0x14)
    // is an aimed single cast; speed-up (2) is refused while it runs (CD 0x158c7).
    const bool e95 = engine1995();
    if (spell > (e95 ? 0x14 : 0x11)) return 0;
    if (e95 && (spell == 0x12 || spell == 0x13)) return 0;
    PlayerBlock *P = player_block(t);
    Thing *s = ai_get_spell_thing(t, spell);
    if (!s) return 0;
    if (e95 && spell == 2 && s->cast_ticks != 0) return 0;
    int kind = (e95 && spell == 0x14) ? 3 : spell;
    switch (kind) {
    case 0: case 0xf: {                                             // 0x14287: fireball / lightning burst
        if (P->ai_burst < 0) return 0;
        if (t->mana < s->mana_total) return 0;
        if ((uint16_t)angle_diff(t->yaw, t->target_yaw) >= 0xaa) return 0;
        P->spell_cooldown[spell] = g_ai_spell_cooldown_reload[spell];
        t->pitch = (uint16_t)pos_pitch_to(thing_pos(t), thing_pos(thing_ref(t->target)));
        s->cast_ticks = s->duration;
        P->ai_burst = (int16_t)(P->ai_burst + 1);
        if (P->ai_burst >= 8) P->ai_burst = (int16_t)((P->ai_reaction - 255) / 8 - 1);
        return 1;
    }
    case 3: case 7: case 8: case 0xb: case 0xd: case 0x11:          // 0x14398: aimed single casts
        if (t->mana < s->mana_total) return 0;
        if ((uint16_t)angle_diff(t->yaw, t->target_yaw) >= 0xe3) return 0;
        // The 1995 reload table (CD 0x90034) differs from 1996's (0x938c4) only in entry 0x14
        // (Fire Wall): 4 instead of 1. Only the 1995 code casts 0x14.
        P->spell_cooldown[spell] = (e95 && spell == 0x14) ? 4 : g_ai_spell_cooldown_reload[spell];
        t->pitch = (uint16_t)pos_pitch_to(thing_pos(t), thing_pos(thing_ref(t->target)));
        s->cast_ticks = s->duration;
        return 1;
    case 0x10: {                                                    // 0x14455: castle spell
        if (s->cast_ticks != 0) return 0;
        if (t->mana < s->mana_total) return 0;
        if (P->castle != 0) {                                       // upgrade: a normal cast
            s->cast_ticks = s->duration;
            P->spell_cooldown[spell] = g_ai_spell_cooldown_reload[spell];
            return 1;
        }
        Thing *c = thing_create(&t->home, 3, 2);                    // found the castle at the chosen site
        if (c) {
            c->owner = t->owner;
            P->castle = thing_index(c);
        }
        return 1;
    }
    default:                                                        // 1, 4, 5, 0xe (0x14551) and 2, 6, 9, 0xa, 0xc (0x14505 / 0x145a0)
        if (t->mana < s->mana_total) return 0;
        s->cast_ticks = s->duration;
        P->spell_cooldown[spell] = g_ai_spell_cooldown_reload[spell];
        return 1;
    }
}

// ai_spawn_spells_14b00: for every human flyer in the player list (normally one) the 24 acquisition
// countdowns of the AI run; at 0 the spell is created through its Table B constructor at the
// wizard's position, gets flag 1 and caster = the wizard, and takes the first free book slot.
void ai_spawn_spells(Thing *t) {
    PlayerBlock *P = player_block(t);
    for (ListWalk w(g_cfg->player_list); w.ok(); w.next()) {
        if (w.get()->type != 0) continue;
        for (int s = 0; s < 24; s++) {
            if (P->spell_thing[s] != 0) continue;
            int16_t want = (int16_t)P->ai_want_spell[s];
            if (want <= 0) continue;
            want = (int16_t)(want - 1);
            P->ai_want_spell[s] = (uint16_t)want;
            if (want != 0) continue;
            ThingCreateFn fn = thing_create_fn(12, s);              // `call [DAT_000962b6 + s * 0xe]`
            Thing *n = fn ? fn(thing_pos(t)) : nullptr;
            if (!n) continue;
            for (int k = 0; k < 24; k++) {
                if (P->spell_slot[k] != 0) continue;
                n->flags |= 1;
                n->caster = thing_index(t);
                P->spell_slot[k] = (int32_t)thing_index(n);
                break;
            }
        }
    }
}

namespace {

// Shared by 14c70 / 14f00: the conserve-mana flag P+0x196. Set below a quarter of the total, cleared
// again above quarter + 6000 (or above half when that is less than the total).
void update_conserve_flag(Thing *t) {
    PlayerBlock *P = player_block(t);
    int quarter = t->mana_total / 4;
    if (quarter > t->mana) { P->ai_conserve = 1; return; }
    if (P->ai_conserve == 0) return;
    int q = quarter + 6000;
    if (q < t->mana_total) {
        if (q > t->mana) return;
    } else {
        if (t->mana_total / 2 > t->mana) return;
    }
    P->ai_conserve = 0;
}

// One rung of the attack-spell ladder. Returns 1 when the caller must return `*out`: the spell is
// ready (out = spell) or it is affordable and off cooldown but not castable yet (out = 0xff: wait
// for it instead of using a lesser spell). 0 = the spell is not owned / not affordable: try the next.
int attack_spell_rung(Thing *t, int spell, int *out) {
    if (!ai_get_spell_thing(t, spell)) return 0;
    if (ai_spell_ready(t, spell)) { *out = spell; return 1; }
    if (ai_can_afford_spell(t, spell) && player_block(t)->spell_cooldown[spell] == 0) { *out = 0xff; return 1; }
    return 0;
}

// The last rung (lightning): ready -> 0xf, anything else -> 0xff.
int lightning_rung(Thing *t) {
    if (!ai_get_spell_thing(t, 0xf)) return 0xff;
    return ai_spell_ready(t, 0xf) ? 0xf : 0xff;
}

} // namespace

// ai_choose_attack_spell_14c70: skeleton army > volcano > [target casting rebound: accuracy chance of lightning only] > meteor >
// fireball > lightning. 0xff = nothing to cast now.
int ai_choose_attack_spell(Thing *t) {
    update_conserve_flag(t);
    PlayerBlock *P = player_block(t);
    if (P->ai_conserve != 0) return 0xff;
    int out;
    if (attack_spell_rung(t, 0x11, &out)) return out;
    if (attack_spell_rung(t, 8, &out)) return out;
    if (ai_spell_in_progress(thing_ref(t->target), 0xe)) {         // target casting rebound (0x14dc0: je 0x14e33 when not)
        if (ai_rand() % 255 < P->ai_accuracy) return lightning_rung(t);
    }
    if (attack_spell_rung(t, 7, &out)) return out;
    if (engine1995() && attack_spell_rung(t, 0x14, &out)) return out;   // 1995 (CD 0x1623b): Fire Wall
    if (attack_spell_rung(t, 0, &out)) return out;
    return lightning_rung(t);
}

// ai_choose_castle_attack_spell_14f00: the same ladder without the random lightning branch.
int ai_choose_castle_attack_spell(Thing *t) {
    update_conserve_flag(t);
    if (player_block(t)->ai_conserve != 0) return 0xff;
    int out;
    if (attack_spell_rung(t, 0x11, &out)) return out;
    if (attack_spell_rung(t, 8, &out)) return out;
    if (attack_spell_rung(t, 7, &out)) return out;
    if (engine1995() && attack_spell_rung(t, 0x14, &out)) return out;   // 1995 (CD 0x1646d): Fire Wall
    if (attack_spell_rung(t, 0, &out)) return out;
    return lightning_rung(t);
}

// ---- movement -------------------------------------------------------------------------------------
// ai_approach_target_140d0(thing, target, near, far): distance = 3D to the target, or (target null)
// 2D to Thing.home. Within `near`: stop, return 1. Beyond `far`: cast speed-up when it is ready and
// not already running, else cruise. Between: cruise. Returns 0 unless stopped.
int ai_approach_target(Thing *t, const Thing *target, int near_dist, int far_dist) {
    PlayerBlock *P = player_block(t);
    P->accelerating = 0;
    int d = target ? pos_dist_xyz(thing_pos(t), thing_pos(target)) : pos_dist_xy(thing_pos(t), &t->home);
    if (d <= near_dist) {
        P->target_speed = 0;
        P->accelerating = 1;
        return 1;
    }
    if (engine1995()) {
        // 1995 (CD 0x15470): while speed-up runs nothing is set (the spell's speed stays); beyond
        // `far` it is cast when ready; otherwise cruise.
        if (ai_spell_active(t, 2)) return 0;
        if (d > far_dist && ai_spell_ready(t, 2)) {
            ai_cast_spell(t, 2);
            return 0;
        }
        P->target_speed = t->speed_base;
        P->accelerating = 1;
        return 0;
    }
    if (d > far_dist && ai_spell_ready(t, 2)) {
        if (!ai_spell_active(t, 2)) ai_cast_spell(t, 2);
        return 0;
    }
    P->target_speed = t->speed_base;
    P->accelerating = 1;
    return 0;
}

// ai_wizard_move_13b10: the AI flight model.
int ai_wizard_move(Thing *t) {
    PlayerBlock *P = player_block(t);
    const MoveDesc *desc = mc_move_desc(t->desc);
    Pos *pos = &g_pos_scratch;                                      // DAT_000adfc4
    *pos = *thing_pos(t);
    int ground = (int16_t)terrain_height_at(pos);
    pos_follow_ground(pos, ground, (int16_t)desc->clear_hi, (int16_t)desc->clear_lo, (int16_t)desc->z_step);   // desc +0xc, +0xa, +0xe
    math_rotate_offset(pos, t->yaw, 0, (int16_t)t->speed_cur);
    math_rotate_offset(pos, (uint16_t)(t->yaw + 0x200), 0, P->strafe_speed);
    int strafe_sign = isign(P->strafe_speed);
    P->strafe_speed = (int16_t)(P->strafe_speed - 4 * strafe_sign);
    thing_move_to(t, pos);

    int accel = isign(P->target_speed - t->speed_cur);
    t->speed_cur = (int16_t)(t->speed_cur + 16 * accel);

    // Turn toward target_yaw: step = difference / ((255 - reaction) / 16 + 8), clamped to the
    // descriptor's [min, max] yaw step, and snap when the turn crosses the target.
    int diff = angle_diff(t->yaw, t->target_yaw & 0x7ff);
    int rate = (255 - P->ai_reaction) / 16 + 8;
    int step = (uint16_t)diff / (uint16_t)rate;
    if ((int16_t)step > (int16_t)desc->turn_min) step = (int16_t)desc->turn_min;       // +2: maximum
    else if ((int16_t)step < (int16_t)desc->unk4) step = (int16_t)desc->unk4;          // +4: minimum
    int dir = angle_turn_dir(t->yaw, t->target_yaw);
    uint16_t old_yaw = t->yaw, target = t->target_yaw;
    uint16_t new_yaw = (uint16_t)((old_yaw + step * dir) & 0x7ff);
    t->yaw = new_yaw;
    if (old_yaw < target) {
        if (new_yaw > target) t->yaw = target;
    } else if (old_yaw > target) {
        if (target > new_yaw) t->yaw = target;
    }
    return 1;
}

// ---- threat tracking ------------------------------------------------------------------------------
// ai_record_threat_from_projectiles_150f0: every projectile fired by a player Thing scores once
// (flag 0x2000) against its target's owner: a castle +5000 / +1000 (types 3, 4, 0xb / others) and a
// grudge when the owner's threat passes the hostility threshold, a wizard or balloon +3000 / +500,
// a player-owned mana ball hit by a possession shot + mana / 4.
void ai_record_threat_from_projectiles() {
    for (ListWalk w(g_cfg->projectile_list); w.ok(); w.next()) {
        Thing *proj = w.get();
        if (proj->flags & 0x2000) continue;
        int si = (int16_t)proj->owner;
        if (si <= 0 || si >= thing_pool_slots()) continue;
        Thing *shooter = thing_at((unsigned)si);
        if (shooter->cls != 3) continue;
        Thing *target = thing_or_null(proj->target);
        if (!target) continue;
        proj->flags |= 0x2000;
        int shooter_no = player_block(shooter)->player_no;
        bool heavy = (proj->type >= 3 && proj->type <= 4) || proj->type == 0xb ||
                     (proj->type == 0x10 && engine1995());          // 1995 (CD 0x165f5 / 0x16704): Fire Wall too
        bool seed = proj->type == 0xa;                              // castle seed: no threat added (0x1519f / 0x152aa jbe)
        if (target->cls == 3) {
            PlayerBlock *O = player_block(thing_ref(target->owner));
            if (target->type == 2) {
                int v = threat_of(O, shooter_no) + (seed ? 0 : heavy ? 5000 : 1000);
                if (v < 0) v = 0;
                if (v > 0xffff) v = 0xffff;
                threat_of(O, shooter_no) = (uint16_t)v;
                // (the aggression is read from the castle's own player block, i.e. the dummy block)
                if (v > hostility_threshold(shooter, player_block(target))) grudge_of(O, shooter_no) = 1;
            } else {
                int v = threat_of(O, shooter_no) + (seed ? 0 : heavy ? 3000 : 500);
                if (v < 0) v = 0;
                if (v > 0xffff) v = 0xffff;
                threat_of(O, shooter_no) = (uint16_t)v;
            }
        } else if (target->cls == 10) {
            if (proj->type != 1 || target->type != 0x27) continue;
            Thing *owner = thing_or_null(target->mana_owner);
            if (!owner || owner->cls != 3) continue;
            PlayerBlock *O = player_block(owner);
            int v = threat_of(O, shooter_no) + target->mana / 4;
            if (v < 0) v = 0;
            if (v > 0xffff) v = 0xffff;
            threat_of(O, shooter_no) = (uint16_t)v;
        }
    }
}

// ai_find_incoming_projectile_153b0: nearest projectile homing on this wizard, within 20 cells.
Thing *ai_find_incoming_projectile(const Thing *t) {
    Thing *best = nullptr;
    uint64_t best_d = kFarSq;
    for (ListWalk w(g_cfg->projectile_list); w.ok(); w.next()) {
        Thing *proj = w.get();
        if ((int)proj->target != (int)(int16_t)t->owner) continue;
        uint64_t d = dist_sq_u(thing_pos(t), thing_pos(proj));
        if (d < best_d) { best_d = d; best = proj; }
    }
    return best_d < 0x1900000u ? best : nullptr;
}

// ai_set_dodge_steer_15420
void ai_set_dodge_steer(Thing *t, const Thing * /*proj*/) {
    player_block(t)->strafe_speed = 0x50;
}

// ai_counter_projectile_15460: within 4 cells, by projectile type (jump table 0x15434): fireball /
// meteor -> rebound else shield; shield-piercing types 4 / 9 -> shield.
void ai_counter_projectile(Thing *t, const Thing *proj) {
    if (pos_dist_sq_xy(thing_pos(t), thing_pos(proj)) >= 0x100000) return;
    int type = proj->type;
    if (type == 0x10 && engine1995()) type = 0;                     // 1995 (CD 0x168c7): the Fire Wall projectile too
    switch (type) {
    case 0: case 3:
        if (ai_spell_ready(t, 0xe)) { ai_cast_spell(t, 0xe); break; }
        if (ai_spell_ready(t, 4)) ai_cast_spell(t, 4);
        break;
    case 4: case 9:
        if (ai_spell_ready(t, 4)) ai_cast_spell(t, 4);
        break;
    default:
        break;
    }
}

// ---- mana ball selection --------------------------------------------------------------------------
// ai_find_mana_ball_target_13ce0: the nearest mana ball that is unowned, owned by a hostile player
// (distance measured from the own castle), or owned by a friend with no foreign wizard within 20
// cells and no castle touching it.
Thing *ai_find_mana_ball_target(Thing *t) {
    PlayerBlock *P = player_block(t);
    Thing *castle = thing_ref(P->castle);                            // things[0] when none
    const Pos *castle_pos = thing_pos(castle);
    const Pos *self_pos = thing_pos(t);
    Thing *best = nullptr;
    uint64_t best_d = kFarSq;
    for (ListWalk w(g_cfg->mana_ball_list); w.ok(); w.next()) {
        Thing *ball = w.get();
        Thing *owner = thing_ref(ball->mana_owner);
        uint64_t d;
        if (owner->cls != 3) {
            d = dist_sq_u(self_pos, thing_pos(ball));
        } else {
            if (ball->mana_owner == (uint16_t)(int16_t)t->owner) continue;
            int threat = threat_of(P, player_block(owner)->player_no);
            if (hostility_threshold(owner, P) < threat) {
                d = dist_sq_u(castle_pos, thing_pos(ball));
            } else {
                Thing *wiz = player_find_nearest_wizard_excl(ball, t);
                if (!wiz) continue;
                Thing *near_castle = P->castle == 0 ? player_find_nearest_by_type(ball, 2)
                                                    : player_find_nearest_castle_excl(ball, castle);
                if (t->owner != wiz->owner && pos_dist_sq_xy(thing_pos(ball), thing_pos(wiz)) <= 0x1900000) continue;
                if (near_castle && thing_collide(ball, near_castle)) continue;
                d = dist_sq_u(self_pos, thing_pos(ball));
            }
        }
        if (d < best_d) { best = ball; best_d = d; }
    }
    return best;
}

// ---- goals ----------------------------------------------------------------------------------------
// ai_find_castle_site_12bd0: without a castle, with the castle spell owned and affordable, walk a 4 x 4
// grid of map quadrants starting at the own quadrant and take the first quadrant corner or centre that
// has no castle within 0x3000 (Chebyshev). Thing 0 is the position scratch; the site goes to Thing.home.
int ai_find_castle_site(Thing *t) {
    PlayerBlock *P = player_block(t);
    if (P->castle != 0) return 0;
    if (!ai_get_spell_thing(t, 0x10)) return 0;
    if (!ai_can_afford_spell(t, 0x10)) return 0;
    Thing *scratch = thing_at(0);
    scratch->owner = t->owner;
    // Round 11: the quadrants are a quarter of the world per axis (0x4000 units = 64 cells with 256
    // cells). The original's signed (int16_t)x / 0x4000 is world_signed(x) / quadrant (truncating, so
    // the start quadrant of the upper half is -2 / -1 / 0 as in the original); the corner is
    // (q & 3) * quadrant, the "centre" cell (q & 3) * (size / 4) + size / 8 - 1 (0x1f with 256 cells).
    // The 0x3000 Chebyshev clearance is a game range and stays.
    const int32_t quad = g_world.units / 4;
    const int quad_cells = g_world.size / 4, centre_cell = g_world.size / 8 - 1;
    int qx0 = world_signed(t->x) / quad, qy0 = world_signed(t->y) / quad;
    for (int qy = qy0; (int16_t)qy < (int16_t)(qy0 + 4); qy++) {
        for (int qx = qx0; (int16_t)qx < (int16_t)(qx0 + 4); qx++) {
            scratch->x = (qx & 3) * quad;                           // quadrant corner
            scratch->y = (qy & 3) * quad;
            Thing *c = player_find_nearest_by_type(scratch, 2);
            if (!c || pos_dist_chebyshev_xy(thing_pos(c), thing_pos(scratch)) > 0x3000) {
                t->home = *thing_pos(scratch);
                return 1;
            }
            scratch->x = ((qx & 3) * quad_cells + centre_cell) << 8;   // quadrant centre
            scratch->y = ((qy & 3) * quad_cells + centre_cell) << 8;
            c = player_find_nearest_by_type(scratch, 2);
            if (!c || pos_dist_chebyshev_xy(thing_pos(c), thing_pos(scratch)) > 0x3000) {
                t->home = *thing_pos(scratch);
                return 1;
            }
        }
    }
    return 0;
}

// ai_goal_repair_castle_12d70 (no caller in retail)
int ai_goal_repair_castle(Thing *t) {
    Thing *castle = thing_or_null(player_block(t)->castle);
    if (!castle || castle->health >= castle->max_health) return 0;
    set_target(t, castle);
    return 1;
}

// ai_goal_upgrade_castle_12df0
int ai_goal_upgrade_castle(Thing *t) {
    Thing *castle = thing_or_null(player_block(t)->castle);
    if (engine1995()) {
        // 1995 (CD 0x14120): castle spell owned, not running, off cooldown, footprint clear,
        // mana_total enough, castle in state 4. No castle-duration and no aim test.
        if (!castle) return 0;
        Thing *s = ai_get_spell_thing(t, 0x10);
        if (!s || s->cast_ticks != 0 || player_block(t)->spell_cooldown[0x10] != 0) return 0;
        if ((uint8_t)castle_footprint_clear(castle) == 0) return 0;
        if (t->mana_total < s->mana_total) return 0;
        if (castle->state != 4) return 0;
        set_target(t, castle);
        return 1;
    }
    if (!castle || castle->state != 4 || castle->duration != 0) return 0;
    if (!ai_castle_spell_ready(t)) return 0;
    set_target(t, castle);
    return 1;
}

// ai_goal_collect_mana_12e90: needs possession (3); with the castle spell owned only while the
// castle spell is not yet affordable.
int ai_goal_collect_mana(Thing *t) {
    if (!ai_get_spell_thing(t, 3)) return 0;
    Thing *castle_spell = ai_get_spell_thing(t, 0x10);
    if (castle_spell && t->mana_total > castle_spell->mana_total) return 0;
    Thing *ball = ai_find_mana_ball_target(t);
    if (!ball) return 0;
    set_target(t, ball);
    return 1;
}

// ai_goal_retreat_12f70
int ai_goal_retreat(Thing *t) {
    if (t->max_health / 2 <= t->health) return 0;
    Thing *castle = thing_or_null(player_block(t)->castle);
    if (!castle) return 0;
    set_target(t, castle);
    return 1;
}

// ai_goal_attack_castle_13000
int ai_goal_attack_castle(Thing *t) {
    if (!ai_has_any_attack_spell(t)) return 0;
    PlayerBlock *P = player_block(t);
    if (P->castle == 0 && ai_get_spell_thing(t, 0x10)) return 0;
    Thing *best = nullptr;
    uint64_t best_d = kFarSq;
    for (ListWalk w(g_cfg->player_list); w.ok(); w.next()) {
        Thing *p = w.get();
        if (p->owner == t->owner || p->type != 2) continue;
        Thing *owner = thing_ref(p->owner);
        int threat = threat_of(P, player_block(owner)->player_no);
        bool candidate = false;
        if (hostility_threshold(owner, P) < threat) {
            if (pos_dist_sq_xy(thing_pos(owner), thing_pos(p)) > 0x3840000 && !thing_collide(owner, p)) candidate = true;   // owner away from his castle (0x130e2..0x130ec)
        }
        if (!candidate) {
            if ((255 - P->ai_aggression) * 0x280 + p->mana >= thing_ref(P->castle)->mana) continue;
        }
        uint64_t d = dist_sq_u(thing_pos(t), thing_pos(p));
        if (d < best_d) { best = p; best_d = d; }
    }
    if (!best) return 0;
    int r = (int16_t)mc_move_desc(t->desc)->sight_radius;
    if (pos_dist_sq_xy(thing_pos(best), thing_pos(t)) >= r * r) return 0;
    set_target(t, best);
    return 1;
}

// ai_goal_attack_wizard_13210
int ai_goal_attack_wizard(Thing *t) {
    if (!ai_has_any_attack_spell(t)) return 0;
    PlayerBlock *P = player_block(t);
    if (P->castle == 0 && ai_get_spell_thing(t, 0x10)) return 0;
    Thing *best = nullptr;
    uint64_t best_d = kFarSq;
    for (ListWalk w(g_cfg->player_list); w.ok(); w.next()) {
        Thing *p = w.get();
        if (p->owner == t->owner) continue;
        if (p->type != 0 && p->type != 1) continue;
        if (ai_spell_in_progress(p, 0xc)) continue;                 // invisible
        PlayerBlock *Q = player_block(p);
        if (grudge_of(P, Q->player_no) == 1) {
            set_target(t, p);
            return 1;
        }
        int threat = threat_of(P, Q->player_no);
        if (hostility_threshold(p, P) > threat) {
            if (Q->castle != 0) continue;
            if (!ai_get_spell_thing(p, 0x10)) continue;
            if (((255 - P->ai_aggression) << 5) + p->mana >= t->mana) continue;
        }
        uint64_t d = dist_sq_u(thing_pos(t), thing_pos(p));
        if (d < best_d) { best = p; best_d = d; }
    }
    if (!best) return 0;
    int r = (int16_t)mc_move_desc(t->desc)->sight_radius + 10;
    if (pos_dist_sq_xy(thing_pos(best), thing_pos(t)) >= r * r) return 0;
    set_target(t, best);
    return 1;
}

// ai_goal_attack_type3_13440
int ai_goal_attack_type3(Thing *t) {
    if (!ai_has_any_attack_spell(t)) return 0;
    PlayerBlock *P = player_block(t);
    Thing *best = nullptr;
    uint64_t best_d = kFarSq;
    for (ListWalk w(g_cfg->player_list); w.ok(); w.next()) {
        Thing *p = w.get();
        if (p->owner == t->owner || p->type != 3) continue;
        Thing *owner = thing_ref(p->owner);
        PlayerBlock *Q = player_block(owner);
        int threat = threat_of(P, Q->player_no);
        if (hostility_threshold(owner, P) >= threat) continue;
        if ((0x113 - P->ai_aggression) * 10 >= p->mana) continue;
        if (thing_collide(p, thing_ref(Q->castle))) continue;
        uint64_t d = dist_sq_u(thing_pos(t), thing_pos(p));
        if (d < best_d) { best = p; best_d = d; }
    }
    if (!best) return 0;
    int r = (int16_t)mc_move_desc(t->desc)->sight_radius;
    if (pos_dist_sq_xy(thing_pos(best), thing_pos(t)) >= r * r) return 0;
    set_target(t, best);
    return 1;
}

// ai_goal_creature_near_rival_13600 (no caller in retail): a creature of another owner for which some
// other AI wizard's mode byte equals the 0 / 1 result of the idle handler and whose target is within
// 20 cells of this wizard.
int ai_goal_creature_near_rival(Thing *t) {
    for (int list = 0; list < 20; list++) {
        for (ListWalk w(g_cfg->creature_lists[list]); w.ok(); w.next()) {
            Thing *c = w.get();
            if (c->owner == t->owner) continue;
            for (int p = 0; p < g_state->player_count && p < 8; p++) {
                Thing *wiz = thing_ref(g_state->players[p].thing);
                if (wiz->type != 1) continue;
                if (p == player_block(t)->player_no) continue;
                int mode = (int8_t)player_block(wiz)->ai_mode;
                if (ai_mode12_idle(t) != mode) continue;
                if (pos_dist_sq_xy(thing_pos(thing_ref(wiz->target)), thing_pos(t)) >= 0x1900000) continue;
                set_target(t, c);
                return 1;
            }
        }
    }
    return 0;
}

// ai_goal_hunt_creature_13770: the creature nearest to the own castle (or the wizard) that carries
// mana and is not the wizard's own.
int ai_goal_hunt_creature(Thing *t) {
    if (!ai_has_any_attack_spell(t)) return 0;
    Thing *castle = thing_or_null(player_block(t)->castle);
    const Pos *origin = thing_pos(castle ? castle : t);
    Thing *best = nullptr;
    uint64_t best_d = kFarSq;
    for (int list = 0; list < 20; list++) {
        for (ListWalk w(g_cfg->creature_lists[list]); w.ok(); w.next()) {
            Thing *c = w.get();
            if (c->owner == t->owner || c->mana <= 0) continue;
            uint64_t d = dist_sq_u(origin, thing_pos(c));
            if (d < best_d) { best = c; best_d = d; }
        }
    }
    if (!best) return 0;
    set_target(t, best);
    return 1;
}

// ai_goal_default_13a20
int ai_goal_default(Thing *t) {
    PlayerBlock *P = player_block(t);
    Thing *castle = thing_or_null(P->castle);
    if (t->health < t->max_health && castle) {
        set_target(t, castle);
        P->ai_mode = 0xb;
    } else {
        P->ai_mode = 0xc;
    }
    return 1;
}

// ai_choose_goal_12330
int ai_choose_goal(Thing *t) {
    if (ai_find_castle_site(t)) return ai_set_mode(t, 3);
    if (ai_goal_retreat(t)) return ai_set_mode(t, 0xb);
    int period = ai_think_period(player_block(t)->ai_reaction);
    if (period != 0 && t->tick % period != 0) return 1;              // (period 0 would divide by zero in the original)
    if (ai_goal_upgrade_castle(t)) return ai_set_mode(t, 1);
    if (ai_goal_attack_castle(t)) return ai_set_mode(t, 7);
    if (ai_goal_attack_wizard(t)) return ai_set_mode(t, 8);
    if (ai_goal_attack_type3(t)) return ai_set_mode(t, 9);
    if (ai_goal_collect_mana(t)) return ai_set_mode(t, 6);
    if (ai_goal_hunt_creature(t)) return ai_set_mode(t, 0xd);
    ai_goal_default(t);
    return 1;
}

// ---- mode handlers --------------------------------------------------------------------------------
// ai_mode1_upgrade_castle_12470: fly to the own castle (target) and cast the castle spell there.
int ai_mode1_upgrade_castle(Thing *t) {
    Thing *target = thing_ref(t->target);
    if (!ai_target_valid(t, target)) return 0;
    t->target_yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(target));
    if (!ai_approach_target(t, target, 0x200, 0x800)) return 1;
    if (ai_cast_spell(t, 0x10)) return 0;
    hover_toward(t, target->z);
    return 1;
}

// ai_mode3_fly_to_castle_site_12560: fly to Thing.home and found the castle.
int ai_mode3_fly_to_castle_site(Thing *t) {
    t->target_yaw = (uint16_t)pos_angle_to(thing_pos(t), &t->home);
    if (!ai_approach_target(t, nullptr, 0x800, engine1995() ? 0xc00 : 0x1000)) return 1;   // 1995: CD 0x1390a
    if (ai_cast_spell(t, 0x10)) return 0;
    hover_toward(t, t->home.z);
    return 1;
}

// ai_mode4_approach_target_12600
int ai_mode4_approach_target(Thing *t) {
    Thing *target = thing_ref(t->target);
    if (!ai_target_valid(t, target)) return 0;
    t->target_yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(target));
    return ai_approach_target(t, target, 0x100, 0x800) ? 0 : 1;
}

// ai_mode12_idle_12680: cast speed-up when ready, wait while it runs, else cruise.
// The 1995 version (CD 0x13a10, also the no-castle tail of mode 11 at CD 0x13b45): nothing while
// speed-up runs, else cast it when ready, else cruise; always returns 0.
static int ai_idle_1995(Thing *t) {
    if (ai_spell_active(t, 2)) return 0;
    if (ai_spell_ready(t, 2)) {
        ai_cast_spell(t, 2);
        return 0;
    }
    PlayerBlock *P = player_block(t);
    P->target_speed = t->speed_base;
    P->accelerating = 1;
    return 0;
}

int ai_mode12_idle(Thing *t) {
    if (engine1995()) return ai_idle_1995(t);
    if (ai_spell_ready(t, 2)) {
        ai_cast_spell(t, 2);
        return 1;
    }
    Thing *s = ai_get_spell_thing(t, 2);
    if (s && s->cast_ticks > 0) return 1;
    PlayerBlock *P = player_block(t);
    P->target_speed = t->speed_base;
    P->accelerating = 1;
    return 0;
}

// ai_mode11_return_home_126f0
int ai_mode11_return_home(Thing *t) {
    PlayerBlock *P = player_block(t);
    Thing *castle = thing_or_null(P->castle);
    if (castle) {
        if (pos_dist_sq_xy(thing_pos(t), thing_pos(castle)) > 0x6400000) {
            if (ai_cast_spell(t, 0x13)) return 1;                   // (never succeeds: ai_cast_spell refuses spells above 0x11)
        }
        ai_cast_spell(t, 0xc);                                      // invisible
        if (!ai_target_valid(t, castle)) return 0;
        t->target_yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(castle));
        return ai_approach_target(t, castle, 0x100, 0x800) ? 0 : 1;
    }
    if (ai_spell_ready(t, 0xc)) ai_cast_spell(t, 0xc);
    if (engine1995()) return ai_idle_1995(t);
    if (ai_spell_ready(t, 2)) {
        ai_cast_spell(t, 2);
        return 1;
    }
    Thing *s = ai_get_spell_thing(t, 2);
    if (s && s->cast_ticks > 0) return 1;
    P->target_speed = t->speed_base;
    P->accelerating = 1;
    return 0;
}

// ai_mode6_collect_mana_12830: approach the mana ball, cast possession at it and, when facing it
// within 0x1c, claim it (mana_owner = self).
int ai_mode6_collect_mana(Thing *t) {
    Thing *target = thing_ref(t->target);
    if (!ai_target_valid(t, target)) return 0;
    t->target_yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(target));
    if (!ai_approach_target(t, target, 0x400, 0xc00)) return 1;
    if (ai_cast_spell(t, 3)) {
        int a = pos_angle_to(thing_pos(t), thing_pos(target));
        if ((uint16_t)angle_diff(t->yaw, a & 0xffff) < 0x1c) target->mana_owner = t->owner;
        if (!engine1995()) return 0;                                // 1995 (CD 0x13c57): hover after the cast too
    }
    hover_toward(t, target->z);
    return 1;
}

// ai_mode7_attack_castle_12950
int ai_mode7_attack_castle(Thing *t) {
    Thing *target = thing_ref(t->target);
    if (!ai_target_valid(t, target)) return 0;
    t->target_yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(target));
    if (!ai_approach_target(t, target, 0x800, 0xe00)) return 1;
    int period = ai_think_period(player_block(t)->ai_reaction);
    if (period != 0 && t->tick % period != 0) return 1;             // not a think tick: no hover (0x129f8 jne 0x12a70)
    int spell = ai_choose_castle_attack_spell(t);
    if ((int8_t)spell != -1 && ai_cast_spell(t, spell)) return 0;
    hover_toward(t, target->z);
    return 1;
}

// ai_mode8_attack_wizard_12a90 (modes 8, 9, 0xd)
int ai_mode8_attack_wizard(Thing *t) {
    Thing *target = thing_ref(t->target);
    if (!ai_target_valid(t, target)) return 0;
    t->target_yaw = (uint16_t)pos_angle_to(thing_pos(t), thing_pos(target));
    const bool e95 = engine1995();                                  // 1995 (CD 0x13e27): 0xc00 / 0x1000
    if (!ai_approach_target(t, target, e95 ? 0xc00 : 0xd00, e95 ? 0x1000 : 0x1200)) return 1;
    PlayerBlock *P = player_block(t);
    if (P->ai_burst < 0) return 1;                                  // recovering: no hover (0x12b15 jl 0x12bc6)
    int spell = ai_choose_attack_spell(t);
    if ((int8_t)spell != -1 && ai_spell_ready(t, spell) && ai_cast_spell(t, spell)) {
        if (target->type == 0 || target->type == 1)
            grudge_of(P, player_block(target)->player_no) = 0;
        return 0;
    }
    hover_toward(t, target->z);
    return 1;
}

// ---- the tick -------------------------------------------------------------------------------------
// player_ai_wizard_tick_11f20
int player_ai_wizard_tick(Thing *t) {
    PlayerBlock *P = player_block(t);
    bool in_castle = false;
    ai_cache_human_wizard(t);
    if (P->ai_burst < 0) P->ai_burst = (int16_t)(P->ai_burst + 1);
    for (int s = 0; s < 24; s++)
        if ((int16_t)P->spell_cooldown[s] > 0) P->spell_cooldown[s] = (uint16_t)(P->spell_cooldown[s] - 1);
    // Threat drift toward the neutral value 0x601f: up by aggression + 1, down by 0x100 - aggression
    // unless a grudge holds it.
    for (int p = 0; p < 8; p++) {
        uint16_t &threat = threat_of(P, p);
        if (threat < 0x601f) {
            threat = (uint16_t)(threat + P->ai_aggression + 1);
            if (threat > 0x601f) threat = 0x601f;
        }
        if (threat > 0x601f) {
            if (grudge_of(P, p) == 0) threat = (uint16_t)(threat - (0x100 - P->ai_aggression));
            if (threat < 0x601f) threat = 0x601f;
        }
    }
    player_rebuild_spell_index(t);
    if (P->castle != 0 && thing_collide(t, thing_ref(P->castle))) in_castle = true;
    if (in_castle) P->invuln_timer = 2;
    if (P->invuln_timer == 0) {
        if (player_apply_hits(t) == 2) {
            t->state = 2;
            return 0;
        }
    } else {
        std::memset(t->damage_slots, 0, sizeof t->damage_slots);
        P->invuln_timer = (int16_t)(P->invuln_timer - 1);
    }
    ai_wizard_move(t);
    if (P->aim_charge < 200) P->aim_charge++;
    t->mana += t->mana_cost;
    t->health += P->health_regen;
    if (t->health < -1) t->health = -1;
    if (t->health > t->max_health) t->health = t->max_health;
    if (P->countdown15f != 0) P->countdown15f--;
    if (in_castle || (t->flags & 0x1000)) {                         // at home (or on a dolmen): fast regeneration
        t->mana_cost = t->mana_total / 200;
        P->health_regen = (int16_t)(t->max_health / 200);
        if (t->mana_cost < 1000) t->mana_cost = 1000;
        t->flags &= ~0x1000u;
    } else {
        t->mana_cost = t->mana_total / 2000;
        P->health_regen = (int16_t)(t->max_health / 500);
        if (t->mana_cost < 100) t->mana_cost = 100;
    }
    if (t->mana < 0) t->mana = 0;
    if (t->mana > t->mana_total) t->mana = t->mana_total;
    ai_spawn_spells(t);
    int period = ai_think_period(P->ai_reaction);
    if (period != 0 && t->tick % period == 0) {
        Thing *proj = ai_find_incoming_projectile(t);
        if (proj) {
            ai_set_dodge_steer(t, proj);
            ai_counter_projectile(t, proj);
        }
        if (t->health < t->max_health) ai_cast_spell(t, 1);         // heal
    }
    const MoveDesc *desc = mc_move_desc(t->desc);
    int ground = (int16_t)terrain_height_at(thing_pos(t));
    if ((int16_t)t->z > ground + (int16_t)desc->clear_lo) t->z = (int16_t)(ground + (int16_t)desc->clear_lo);
    if ((int16_t)t->z < ground + (int16_t)desc->clear_hi) t->z = (int16_t)(ground + (int16_t)desc->clear_hi);
    return 1;
}

// player_type1_s1_update_11de0: tick, mode dispatch (jump table 0x11da0), goal selection. The return
// value of the tick (death) and of the mode handler are ignored, as in the original.
void player_type1_s1_update(Thing *t) {
    player_ai_wizard_tick(t);
    switch (player_block(t)->ai_mode) {
    case 0:    ai_choose_goal(t); break;                            // mode 0 runs the goal selection twice
    case 1:    ai_mode1_upgrade_castle(t); break;
    case 2:    break;                                               // ret_zero_12550
    case 3:    ai_mode3_fly_to_castle_site(t); break;
    case 4:    ai_mode4_approach_target(t); break;
    case 5:    break;                                               // ret_zero_12550
    case 6:    ai_mode6_collect_mana(t); break;
    case 7:    ai_mode7_attack_castle(t); break;
    case 8:    ai_mode8_attack_wizard(t); break;
    case 9:    ai_mode8_attack_wizard(t); break;
    case 0xb:  ai_mode11_return_home(t); break;
    case 0xc:  ai_mode12_idle(t); break;
    case 0xd:  ai_mode8_attack_wizard(t); break;
    default:   break;                                               // 0xa and anything above 0xd
    }
    ai_choose_goal(t);
}

void ai_wizard_register_handlers() {
    thing_register_update(0x11de0, player_type1_s1_update);
    g_hook_ai_record_threat = ai_record_threat_from_projectiles;
}
