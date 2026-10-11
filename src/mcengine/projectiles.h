// Projectiles (class 9, 0x43f30..0x468e0): flight, steering, impact, target selection, line of fire.
// Owner: projectiles.cpp (round 3). Report: docs/analysis/port_projectiles.md.
//
// A projectile is created by its constructor (constructors.cpp, Table B) and then completed by the
// caster: owner (+0x18), yaw / pitch, damage (+0x2c), mana (+0x8c, 0x32 by default), the class / type
// of the effect it leaves on impact (impact_cls / impact_type, +0x44 / +0x45) and, for the castle
// seed, the destination (home, +0x96). Type and Table A state are not the same thing:
//
//   type  0..13  -> state = type          type 14 -> state 15      type 17 -> state 18
//   type  9 also -> state 14 (the bolt     type 15 -> state 16      type 18 -> state 19
//                  segments of lightning)  type 16 -> state 17      type 19 -> state 20
//
// Thing.health is the remaining lifetime in ticks (max_health = range / speed), flag 2 (+0x10) means
// "first tick done" (the automatic aim ran), Thing.target (+0x92) the Thing being homed on.
#pragma once
#include "thing.h"
#include "spatial.h"

// Binds this subsystem's handlers to the class tables (by original address) and installs its hooks.
void projectiles_register_handlers();

// ---- steering ------------------------------------------------------------------------------------
// projectile_steer_to_target_43f30(t, target): target_yaw / target_pitch := direction to the middle
// of `target`, then yaw / pitch turn toward them by at most MoveDesc+2 / MoveDesc+6 per call.
void projectile_steer_to_target(Thing *t, Thing *target);
// thing_turn_toward_43ff0(a, b): the same without raising b to the middle of its box.
void thing_turn_toward(Thing *a, Thing *b);

// ---- target selection ----------------------------------------------------------------------------
// projectile_pick_target_45f00(t): automatic aim of a freshly launched projectile, by projectile
// type. On success Thing.target is the chosen Thing, target_yaw / target_pitch point at it
// (thing_aim_at) and 1 is returned; the caller decides whether yaw / pitch snap to it. Also clamps
// Thing.aux (+0x1a) to 0x10.
//   types 0, 3, 4, 0x10, 0x12, 0x13: enemy players / castles within the *owner's* sight radius and
//                                    awake enemy creatures, cone 0x71 / 0x71
//   type 1:                          mana balls and wizard castles (effect 0x27 / 0x28 / 0x2d) whose
//                                    mana owner is not the projectile's owner
//   types 7, 8, 0xb, 0xc:            enemy players / castles only
//   type 9 (lightning):              players within max_health * speed_base, creatures with a pitch
//                                    cone of 0x200 (any elevation)
//   type 0x11:                       any awake mana ball
//   everything else:                 no automatic aim (returns 0)
int projectile_pick_target(Thing *t);
// projectile_target_score_46470(shooter, target, max_yaw, max_pitch): 0xffffffff when the direction
// to the middle of `target` is more than max_yaw / max_pitch off the shooter's yaw / pitch or the
// horizontal distance is above 0x1400, else a "smaller is better" score: with d = distance,
// (d cos dy)^2 + (4 d sin dy)^2 + (d cos dp)^2 + (4 d sin dp)^2.
uint32_t projectile_target_score(const Thing *shooter, Thing *target, unsigned max_yaw, unsigned max_pitch,
                                 int32_t max_dist = 0x1400);   // port: max_dist (the original: 0x1400)
// target_aim_score_465b0: the same toward the target's base position (used for castles).
uint32_t target_aim_score(const Thing *a, const Thing *b, unsigned max_yaw, unsigned max_pitch);
// projectile_line_of_fire_clear_466b0(shooter, target) (listed as 466af, one pad byte early). Dead
// code in retail (no call and no pointer reference anywhere in the image), translated as it is,
// including its bug: the probe marched from the shooter is tested against the *shooter's* box, not
// the target's. Returns 1 on that "hit", 0 when out of range (MoveDesc.sight_radius), outside the
// 0x100 / 0x38 cone, when the terrain rises above the ray, or when the distance is used up.
int projectile_line_of_fire_clear(Thing *shooter, Thing *target);

// ---- flight --------------------------------------------------------------------------------------
// projectile_record_hit_stats_440a0(t, hit, target): for types 0, 1, 3, 7, 8, 9, 0x13 fired by a
// flyer (class 3 type 0): P.shots++, and P.hits++ when the Thing hit belongs to the owner of the
// Thing aimed at. `hit` may be null, `target` is &things[t->target] (thing 0 = none).
void projectile_record_hit_stats(Thing *t, Thing *hit, Thing *target);
// Port (debug, not game state: never hashed, saved or read by the simulation): the same count for the computer
// wizards, per player record, weapon ([0] fireball, [1] lightning, [2] the other counted projectiles) and what the
// wizard was targeting ([0] a creature, [1] a wizard, [2] a castle, [3] anything else / nothing).
// hit_any: the shot ended on a Thing; hit_target: on the Thing the wizard was targeting. Callers clear it.
struct AiShotStats { uint32_t shots, hit_any, hit_target; };
extern AiShotStats g_ai_shot_stats[8][3][4];
// projectile_fly_and_impact_44150: the shared flight of states 2..6, 11, 15, 16, 17, 20.
void projectile_fly_and_impact(Thing *t);
// projectile_step_44ea0: one collision-tested step of the lightning ray (the Thing is not in a cell
// list while it runs); marks the Thing for deletion when it hits something, the ground or runs out.
void projectile_step(Thing *t);

// ---- Table A handlers (class 9) ------------------------------------------------------------------
void projectile_type0_s0_update(Thing *t);       // 44510 state 0: fireball (limited first-tick turn, steps back out of the ground)
void projectile_homing_update(Thing *t);         // 448b0 state 1: ground-hugging, stops at foreign mana (10730)
void projectile_update_shared(Thing *t);         // 44a40 states 2, 4, 5, 6, 11, 15, 16, 17, 20
void projectile_type3_s3_update(Thing *t);       // 44a50 state 3: shared flight + a trail effect (10 / 1) per tick
void projectile_type7_s7_update(Thing *t);       // 44a90 state 7: calls 44aa0
void projectile_type8_s8_update(Thing *t);       // 44aa0 state 8: impact effect only when a player / wizard is hit
void projectile_lightning_update(Thing *t);      // 44fc0 state 9: instant ray + bolt segments
void projectile_type10_s10_update(Thing *t);     // 45360 state 10: castle seed homing on a Thing (45530 without one)
void projectile_castle_seed_update(Thing *t);    // 45530 castle seed flying to Thing.home
void projectile_type12_s12_update(Thing *t);     // 457a0 state 12: shared flight, leaves effect 0x26 carrying the impact class / type
void projectile_type13_s13_update(Thing *t);     // 45b60 state 13: arrow (area damage, no effect)
void projectile_type14_s14_update(Thing *t);     // 45c70 state 14: lightning bolt segment (dies when health < 0)
void projectile_type18_s18_update(Thing *t);     // 45c90 state 18: ground-hugging, stops at a mana ball (10870), effect 0xc + impact effect
void projectile_type19_s19_update(Thing *t);     // 45e60 state 19: turns into its impact effect at once

// The index several handlers store into the impact effect's Thing.target (+0x92) is computed as
// (hit - things) / 0xa4 even when nothing was hit (hit == NULL): the original then writes the low 16
// bits of -(address of the Thing pool) / 0xa4, a value that depends on where DOS/4GW put the game
// state. The port writes this variable instead (default 0 = "no Thing"); a harness that compares
// against a state dump of the original can set it to that run's value.
extern uint16_t g_projectile_null_hit_index;
