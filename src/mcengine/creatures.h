// Creatures (class 5): shared movement / AI / attack / death code (0x18050..0x19b70), the wake
// timers (0x468e0, 0x46960) and the per-type state handlers.
// Owner: creature_common.cpp (shared code) + creatures.cpp (per-type Table A handlers).
// Report: docs/analysis/port_creatures.md.
//
// State layout (confirmed from every caller's `push base`): a creature type owns the six Table A
// states base .. base + 5 with **base = type * 6**:
//
//   base + 0   type-specific "extra" state (dragon / worm / emu / type 15: creature_idle_seek_leader;
//              skeleton 54: rising from the ground; builder 72: place a castle; townie 78 / trader 84: empty)
//   base + 1   main state, the one most constructors start in (creature_ai_step, or the type's own wander)
//   base + 2   attack (creature_attack_target + a per-type attack callback)
//   base + 3   follow the flock leader (creature_follow_leader)
//   base + 4   dying (creature_die)
//   base + 5   dead (creature_dead_drop_mana)
//
// The shared functions take `base` as their second stack argument and derive the target states from
// it; the names in gen/dispatch_tables.h count the +0 state to the previous type (e.g. "dragon_s6" is
// the vulture's base + 0, "genie_s72" the builder's) - the binding is by address, so it does not matter.
//
// Thing fields as creatures use them (in addition to mc_types.h):
//   timer_a  (+0x3a) awake gate: counted down by creature_proximity_wake_timer, reloaded to 0x10 while
//            the creature is within 0x1800 (xy) of the local player; pending damage is only consumed
//            and enemies are only looked for while it is non-zero
//   timer_b  (+0x3b) delay before the next proximity test (only ever cleared by the wake timer)
//   cast_ticks (+0x30) |dy| to the local player at the last wake reload (see creature_proximity_wake_timer)
//   parent   (+0x34) flock leader (segments: the segment in front), child (+0x36) first / next segment
//   turn_rate (+0x82) is a speed for creatures: a follower moves at leader.speed_cur + leader.turn_rate,
//            townies / traders walk at it while they have no destination
//   aux      (+0x1a) per-type countdown; castle_size (+0x47) per-type flag (skeleton: 1 = summoning pose)
#pragma once
#include "thing.h"
#include "spatial.h"

// Binds the class-5 Table A handlers (by original address) and installs g_hook_creature_wake_tick.
void creatures_register_handlers();

// ---- shared creature code (creature_common.cpp) --------------------------------------------------

// Attack callback handed to creature_attack_target: stack args (creature, target), 16-bit result in
// AX (non-zero = an attack was made).
using CreatureAttackFn = int (*)(Thing *t, Thing *target);

// The damage intake every creature state body starts with (inlined in the original at each site, e.g.
// 0x1861b..0x186d4): while awake (timer_a != 0) consume damage slot 0 (health -= amount, attacker ->
// last_attacker, slot attacker cleared, the amount is left behind) and take over the health /
// last_attacker of the first segment of the child chain that has less health; health < 0 -> killer :=
// last_attacker. Returns 0 = nothing, 1 = hit, 2 = dead.
int  creature_apply_damage(Thing *t);
// The random heading change of the wander states (inlined at 0x189c1, 0x1be06, 0x1db81, 0x1e31f,
// 0x1e798): two draws from t->rng, target_yaw += (+1 or -1) * (0x55 + (rng & 0xff)), masked to 0x7ff.
void creature_wander_turn(Thing *t);

// creature_segment_update_18050(thing): Table A state 0x78, body segments of dragon / worm / kraken.
// Deletes itself when the parent is no creature; awake: face the parent and sit `speed` behind it,
// consume damage slot 0; asleep: every 4 ticks jump onto the parent.
void creature_segment_update(Thing *t);
// terrain_slope_at_18150(pos): max(|h00 + h01 - h10 - h11|, |h00 + h10 - h01 - h11|) of the four height
// bytes around pos's cell (steepness along x / y).
int  terrain_slope_at(const Pos *pos);
// creature_move_step_181e0(thing): follow the ground, step speed_cur along yaw; a step into another
// cell must pass creature_check_terrain(.., 1) and terrain_slope_at < desc+0x10, otherwise yaw + 0x155,
// yaw - 0x155 and yaw + 0x400 are tried and when all fail the creature dies (health = -1). On success
// the yaw turns toward target_yaw by at most desc+2. Always returns 1.
int  creature_move_step(Thing *t);
// creature_idle_seek_leader_18610(thing, base): damage intake (dead -> base + 4; hit by a class-3
// thing -> target it, base + 2); every desc.think_period ticks adopt the nearest leaderless creature
// of the same type inside sight radius and fov as leader -> base + 3. Does not move.
void creature_idle_seek_leader(Thing *t, int base);
// creature_ai_step_18870(thing, base): the main-state body. Damage intake as above; move; every
// think_period ticks: random heading change, then (awake only) nearest player-list thing in sight /
// fov without flag 0x20 -> target, base + 2; else a flock leader as in 18610 -> base + 3.
void creature_ai_step(Thing *t, int base);
// creature_attack_target_18c20(thing, base, attack): damage intake (dead -> base + 4; hit by a class-3
// thing -> retarget); move; aim at the target every 4 ticks; target dead / deleted -> base + 1; every
// think_period ticks: 3D distance >= sight radius -> base + 1, else attack(thing, target). Returns 1
// when the callback reported an attack (callers request the attack sound on it).
int  creature_attack_target(Thing *t, int base, CreatureAttackFn attack);
// creature_follow_leader_18e90(thing, base): no leader -> base + 1. Dead -> the leader attacks the
// killer, base + 4; hit by a class-3 thing -> leader and thing attack it (base + 2). Else move and
// every think_period ticks act on the leader's state: main / idle -> aim at it (away from a
// foreign-owned creature of the same type within 0x100), speed = leader.speed_cur + leader.turn_rate;
// attack -> same target, base + 2; follow -> adopt the leader's leader; anything else -> base + 1.
void creature_follow_leader(Thing *t, int base);
// creature_die_191d0(thing, base): every segment of the child chain -> base + 5 (their killer is
// taken over), kill credit (P+0x167) for a human wizard killing an independent creature that is not a
// skeleton / builder / townie / trader / type 15, then thing -> base + 5.
void creature_die(Thing *t, int base);
// creature_dead_drop_mana_19310(thing): every 8th tick: drop the carried mana as a mana ball
// (thing_drop_mana_ball_25fe0), create the death effect (class 10 type 1) and delete the creature.
// (Callers push the base as a second argument; the function does not read it.)
void creature_dead_drop_mana(Thing *t);
// creature_adopt_leader_19360(thing, other, state): parent := other's leader (when it exists, is not
// `thing` and is alive) or `other` itself when it has none; then Thing.state := state (a direct
// write). No callers in the retail image.
void creature_adopt_leader(Thing *t, Thing *other, int state);

// Attack callbacks (CreatureAttackFn). The projectile ones create a class-9 thing at the creature's
// position, aimed at the target (yaw / pitch), raised by the creature's ext_h, owner / target /
// collision filter copied from the creature. Result: 1 when the projectile was created.
int creature_attack_fire(Thing *t, Thing *target);          // creature_attack_fire_193f0 (dragon, worm): type 0, desc 6, damage 500
int creature_attack_arrow(Thing *t, Thing *target);         // creature_attack_arrow_194a0 (archer, emu): type 0xd, damage 250, sprite 0xc3 doubled
int skeleton_attack_fire(Thing *t, Thing *target);          // skeleton_attack_fire_19550: type 0xd, damage 400 (600 with a mana owner), sprite 0xcb doubled
int creature_attack_melee(Thing *t, Thing *target);         // creature_attack_melee_19620 (vulture, bee): within 0x400 -> pending damage t->damage
int creature_attack_volley(Thing *t, Thing *target);        // creature_attack_volley_19680 (crab): 1..5 shots by carried mana, three kinds by rng
int creature_attack_fire_troll(Thing *t, Thing *target);    // creature_attack_fire_troll_19940: type 0xe, desc 6, damage 780
int creature_attack_fire_griffon(Thing *t, Thing *target);  // creature_attack_fire_griffon_199f0: type 9, impact 0x17, damage 4000, filter from the target
int creature_attack_fire_homing_desc(Thing *t, Thing *target);  // creature_attack_fire_homing_desc_19a90 (dead code in retail): type 0, desc 2

// ---- wake timers (0x468e0, 0x46960) ---------------------------------------------------------------
// creature_wake_tick_468e0: once per thing_update_all (through g_hook_creature_wake_tick): dead
// creatures get timer_a = 0xfa / timer_b = 0, every other creature and every mana ball runs
// creature_proximity_wake_timer.
void creature_wake_tick();
// creature_proximity_wake_timer_46960(thing): timer_a counts down (copied to every segment); at 0,
// timer_b counts down; at 0 the squared xy distance to the local player's thing is tested: below
// 0x2400000 -> timer_a = 0x10 (segments 0x12) and cast_ticks := |dy| (the original pushes the dy^2
// left in EDX by pos_dist_sq_xy as the argument of math_isqrt). Returns 0.
// Port (round 11 B6): in a mode run the distance is to the nearest wizard of any player record in play
// (human or computer; deterministic, the same on every peer), and the wake tick also runs in a network
// mode run (thing.cpp; the original skips it in a network game, which leaves every creature awake).
int  creature_proximity_wake_timer(Thing *t);

// ---- per-type helpers other subsystems may want (creatures.cpp) -----------------------------------
// skeleton_convert_villager_1c1e0(thing): body of skeleton state 55 while the summoning pose is held.
void skeleton_convert_villager(Thing *t);
// terrain_rect_is_flat_1d420(pos, w, h, max_diff): height range of the w x h cell block centred on pos
// (start cell made "even": x + 1 when x + y is odd) below max_diff.
int  terrain_rect_is_flat(const Pos *pos, unsigned w, unsigned h, unsigned max_diff);
// castle_size_half_extents_1d4b0(size, &ext_x, &ext_y): building.tab width / height (halved in 320x200)
// * 0x80 + 0x300.
void castle_size_half_extents(unsigned size, uint16_t *ext_x, uint16_t *ext_y);
