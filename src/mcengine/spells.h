// Spells (class 12, 0x46ae0..0x494b0): dropped spell pickups, the cast helpers and every spell Table A handler.
// Owner: spells.cpp (round 3). Report: docs/analysis/port_spells.md.
//
// A spell is a Thing of class 12, type = spell id (0..23), state = id * 3 + phase:
//   phase 0  owned by a wizard (Thing.caster = the wizard, listed in its book P+0x214): the cast handler.
//            player_cast_spell_410f0 arms it with cast_ticks = duration; the handler runs while
//            cast_ticks > 0 and counts it down (the tick with cast_ticks == duration is the launch).
//   phase 1  lying on the ground after its owner died (spell_phase1_common_472f0), Thing.health = ticks left.
//   phase 2  level pickup (spell_phase2_common_47300): picked up like phase 1, and a fresh phase-2 copy
//            stays behind for the other wizards.
// Spell fields: duration (+0x32) = cast length in ticks, mana_total (+0x88) = mana needed / charged per
// launch, mana (+0x8c) = mana_total / duration (copied into the projectile), mana_cost (+0x84) = castle
// mana the caster's castle must hold (0 = no castle needed), damage (+0x2c) copied into the projectile.
#pragma once
#include "thing.h"
#include "spatial.h"

// Binds this subsystem's handlers to the class tables (by original address) and installs its hooks.
void spells_register_handlers();

// ---- pickups (phases 1 and 2) ------------------------------------------------------------------
// spell_dropped_update_46ae0(spell, spell id, new state): life countdown (Thing.health, 0 = forever),
// sink / follow the ground, and every 4th tick the pickup test against every live flyer. Returns 1
// when a flyer took the spell (the Thing is then that flyer's spell in `state`), else 0.
int  spell_dropped_update(Thing *t, int type, int state);
void spell_phase2_pickup(Thing *t);                         // spell_phase2_pickup_46dd0
int  spell_dropped_dispatch(Thing *t);                      // spell_dropped_dispatch_46e50

// ---- cast helpers ------------------------------------------------------------------------------
// spell_can_cast_46e70(spell, caster): 0 (and "cannot" sound 0x1d for the caster's player) when the
// caster is dead / out of mana, the spell needs more castle mana than the caster's castle holds, or
// the cast is about to start (cast_ticks == duration) without mana_total mana; else 1.
int  spell_can_cast(Thing *spell, Thing *caster);
// The mana that requirement is tested against: the castle's, plus (mode runs) the caster's claimed town buildings'.
int32_t spell_castle_store(const Thing *caster);
// spell_charge_mana_46f20(spell, caster): on the launch tick the caster's mana rate (Thing.mana_cost,
// added to its mana by the flyer update) is charged with -mana_total, returns 1; on the other cast
// ticks a positive rate is zeroed (no regeneration while casting), returns 0.
int  spell_charge_mana(Thing *spell, Thing *caster);
// spell_projectile_origin_46f90(caster, projectile): moves the new projectile (and its child chain)
// 0x100 to the side of the hand that cast it (caster flags 0x100 left / 0x200 right) unless that
// point is under ground.
void spell_projectile_origin(Thing *caster, Thing *proj);

// ---- Table A handlers --------------------------------------------------------------------------
void spell_phase1_common(Thing *t);             // spell_phase1_common_472f0   (states 3n + 1)
void spell_phase2_common(Thing *t);             // spell_phase2_common_47300   (states 3n + 2, except 68)
void spell_fireball_update(Thing *t);           // spell_fireball_update_47130            state 0
void spell_heal_s3_update(Thing *t);            // spell_heal_s3_update_47310             state 3
void spell_speedup_update(Thing *t);            // spell_speedup_update_47420             state 6
void spell_possession_s9_update(Thing *t);      // spell_possession_s9_update_475b0       state 9
void spell_shield_update(Thing *t);             // spell_shield_update_47760              state 12
void spell_beyond_sight_s15_update(Thing *t);   // spell_beyond_sight_s15_update_477d0    state 15
void spell_earthquake_update(Thing *t);         // spell_earthquake_update_47840          state 18
void spell_meteor_update(Thing *t);             // spell_meteor_update_479f0              state 21
void spell_volcano_s24_update(Thing *t);        // spell_volcano_s24_update_47b90         state 24
void spell_crater_update(Thing *t);             // spell_crater_update_47d40              state 27
void spell_teleport_s30_update(Thing *t);       // spell_teleport_s30_update_47ef0        state 30
void spell_rubber_band_s33_update(Thing *t);    // spell_rubber_band_s33_update_480e0     state 33
void spell_invisible_s36_update(Thing *t);      // spell_invisible_s36_update_48250       state 36
void spell_steal_mana_s39_update(Thing *t);     // spell_steal_mana_s39_update_482f0      state 39
void spell_rebound_update(Thing *t);            // spell_rebound_update_48490             state 42
void spell_lightning_update(Thing *t);          // spell_lightning_update_48510           state 45
void spell_castle_s48_update(Thing *t);         // spell_castle_s48_update_486b0          state 48
void spell_skeleton_s51_update(Thing *t);       // spell_skeleton_s51_update_488a0        state 51
void spell_thunderbolt_s54_update(Thing *t);    // spell_thunderbolt_s54_update_48a70     state 54
void spell_mana_magnet_s57_update(Thing *t);    // spell_mana_magnet_s57_update_48c20     state 57
void spell_fire_wall_s60_update(Thing *t);      // spell_fire_wall_s60_update_48de0       state 60
void spell_reverse_speed_s63_update(Thing *t);  // spell_reverse_speed_s63_update_48fa0   state 63
void spell_update_shared(Thing *t);             // spell_update_shared_49140              states 66 and 68
void spell_mini_fireball_update(Thing *t);      // spell_mini_fireball_update_492e0       state 69
